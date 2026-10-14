#include "Engine.h"

namespace hydra
{
namespace
{
juce::NormalisableRange<float> faderRange()
{
    juce::NormalisableRange<float> r (-60.0f, 12.0f, 0.1f);
    r.setSkewForCentre (-15.0f);
    return r;
}

juce::NormalisableRange<float> logRange (float lo, float hi)
{
    return { lo, hi,
             [] (float s, float e, float n) { return s * std::pow (e / s, n); },
             [] (float s, float e, float v) { return std::log (v / s) / std::log (e / s); },
             [] (float, float, float v) { return v; } };
}

juce::NormalisableRange<float> linRange (float lo, float hi, float step = 0.0f)
{
    return { lo, hi, step };
}
} // namespace

//==============================================================================
dsp::GateSettings InputStrip::gateSettings() const
{
    dsp::GateSettings g;
    g.thresholdDb = gateThr->get();
    g.rangeDb = gateRange->get();
    g.attackMs = gateAttack->get();
    g.holdMs = gateHold->get();
    g.releaseMs = gateRelease->get();
    g.sidechain = gateScOn->isOn();
    g.sidechainHz = gateScFreq->get();
    g.voiceKey = gateVoice->isOn();
    g.voiceThreshold = gateVoiceThr->get() * 0.01f;
    g.voice = dnOn->isOn() && denoiser->isSupported() ? denoiser->getVad() : -1.0f;
    return g;
}

dsp::CompSettings InputStrip::compSettings() const
{
    dsp::CompSettings c;
    c.inputDb = compInput->get();
    c.thresholdDb = compThr->get();
    c.ratio = compRatio->get();
    c.kneeDb = compKnee->get();
    c.attackMs = compAttack->get();
    c.releaseMs = compRelease->get();
    c.makeupDb = compMakeup->get();
    c.autoMakeup = compAuto->isOn();
    return c;
}

dsp::DenoiserSettings InputStrip::denoiserSettings() const
{
    dsp::DenoiserSettings d;
    d.mode = juce::roundToInt (dnMode->get());
    d.amount = dnAmount->get() * 0.01f;
    return d;
}

//==============================================================================
Engine::Engine()
{
    ticksToSeconds = 1.0 / (double) juce::Time::getHighResolutionTicksPerSecond();
    silence.assign (kMaxBlock, 0.0f);

    createParams();

    types.add (juce::AudioIODeviceType::createAudioIODeviceType_ASIO());
    types.add (juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (juce::WASAPIDeviceMode::exclusive));
    types.add (juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (juce::WASAPIDeviceMode::sharedLowLatency));
    types.add (juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (juce::WASAPIDeviceMode::shared));
    types.removeObject (nullptr);
    rescanDevices();

    for (int i = 0; i < kNumInputs; ++i)
    {
        inputs[(size_t) i].index = i;
        inputs[(size_t) i].name = "Input " + juce::String (i + 1);
        inputs[(size_t) i].spectrum.ring.allocate (2, 16384);
        inputs[(size_t) i].l.assign (kMaxBlock, 0.0f);
        inputs[(size_t) i].r.assign (kMaxBlock, 0.0f);
    }
    for (int b = 0; b < kNumBuses; ++b)
    {
        buses[(size_t) b].index = b;
        buses[(size_t) b].name = busCode (b);
        buses[(size_t) b].spectrum.ring.allocate (2, 16384);
        buses[(size_t) b].l.assign (kMaxBlock, 0.0f);
        buses[(size_t) b].r.assign (kMaxBlock, 0.0f);
    }
    buses[0].output.kind = IoTarget::Kind::Master; // A1 = master device channels 1/2
    playL.assign (kMaxBlock, 0.0f);
    playR.assign (kMaxBlock, 0.0f);

    startTimerHz (10);
}

Engine::~Engine()
{
    stopTimer();
    closeMaster();
    for (auto& s : inputs) { s.plugins.clear(); }
    for (auto& b : buses)  { b.plugins.clear(); }
}

void Engine::createParams()
{
    for (int i = 0; i < kNumInputs; ++i)
    {
        auto& s = inputs[(size_t) i];
        const auto p = "in" + juce::String (i + 1) + ".";
        const auto n = "In " + juce::String (i + 1) + " ";

        s.gain       = &params.add (p + "gain", n + "Fader", ParamType::Continuous, faderRange(), 0.0f, " dB");
        s.trim       = &params.add (p + "trim", n + "Gain", ParamType::Continuous, linRange (-24, 24, 0.1f), 0.0f, " dB");
        s.pan        = &params.add (p + "pan", n + "Pan", ParamType::Continuous, linRange (-1, 1, 0.01f), 0.0f);
        s.mute       = &params.addToggle (p + "mute", n + "Mute");
        s.solo       = &params.addToggle (p + "solo", n + "Solo");
        s.mono       = &params.addToggle (p + "mono", n + "Mono");
        s.phase      = &params.addToggle (p + "phase", n + "Phase Invert");
        s.eq.create (params, p, n);
        s.gateOn       = &params.addToggle (p + "gate.on", n + "Gate On");
        s.gateThr      = &params.add (p + "gate.thr", n + "Gate Threshold", ParamType::Continuous, linRange (-80, 0, 0.1f), -50.0f, " dB");
        s.gateRange    = &params.add (p + "gate.range", n + "Gate Range", ParamType::Continuous, linRange (-80, 0, 0.1f), -80.0f, " dB");
        s.gateAttack   = &params.add (p + "gate.attack", n + "Gate Attack", ParamType::Continuous, logRange (0.1f, 100.0f), 1.0f, " ms");
        s.gateHold     = &params.add (p + "gate.hold", n + "Gate Hold", ParamType::Continuous, linRange (0, 1000, 1.0f), 80.0f, " ms");
        s.gateRelease  = &params.add (p + "gate.release", n + "Gate Release", ParamType::Continuous, logRange (5.0f, 2000.0f), 120.0f, " ms");
        s.gateScOn     = &params.addToggle (p + "gate.sc.on", n + "Gate Sidechain Filter");
        s.gateScFreq   = &params.add (p + "gate.sc.freq", n + "Gate Sidechain Freq", ParamType::Continuous, logRange (50.0f, 8000.0f), 1000.0f, " Hz");
        s.gateVoice    = &params.addToggle (p + "gate.voice", n + "Gate Voice Key");
        s.gateVoiceThr = &params.add (p + "gate.voice.thr", n + "Gate Voice Threshold", ParamType::Continuous, linRange (0, 100, 1.0f), 50.0f, " %");
        s.compOn       = &params.addToggle (p + "comp.on", n + "Comp On");
        s.compInput    = &params.add (p + "comp.input", n + "Comp Input", ParamType::Continuous, linRange (-24, 24, 0.1f), 0.0f, " dB");
        s.compThr      = &params.add (p + "comp.thr", n + "Comp Threshold", ParamType::Continuous, linRange (-60, 0, 0.1f), -20.0f, " dB");
        s.compRatio    = &params.add (p + "comp.ratio", n + "Comp Ratio", ParamType::Continuous, logRange (1.0f, 20.0f), 3.0f, ":1");
        s.compKnee     = &params.add (p + "comp.knee", n + "Comp Knee", ParamType::Continuous, linRange (0, 24, 0.1f), 6.0f, " dB");
        s.compAttack   = &params.add (p + "comp.attack", n + "Comp Attack", ParamType::Continuous, logRange (0.1f, 200.0f), 5.0f, " ms");
        s.compRelease  = &params.add (p + "comp.release", n + "Comp Release", ParamType::Continuous, logRange (5.0f, 3000.0f), 150.0f, " ms");
        s.compMakeup   = &params.add (p + "comp.makeup", n + "Comp Makeup", ParamType::Continuous, linRange (-12, 24, 0.1f), 0.0f, " dB");
        s.compAuto     = &params.addToggle (p + "comp.auto", n + "Comp Auto Makeup");
        s.dnOn         = &params.addToggle (p + "dn.on", n + "Denoiser On");
        s.dnAmount     = &params.add (p + "dn.amount", n + "Denoiser Amount", ParamType::Continuous, linRange (0, 100, 1.0f), 100.0f, " %");
        s.dnMode       = &params.addChoice (p + "dn.mode", n + "Denoiser Latency Mode", dsp::Denoiser::modeNames(), dsp::Denoiser::Low);
        s.fxBypass   = &params.addToggle (p + "fx.bypass", n + "FX Bypass");
        for (int b = 0; b < kNumBuses; ++b)
            s.route[(size_t) b] = &params.addToggle (p + "route." + busCode (b), n + "-> " + busCode (b), b == 0);
        for (int b = 0; b < kNumBuses; ++b)
            s.send[(size_t) b] = &params.add (p + "send." + busCode (b), n + "Level -> " + busCode (b), ParamType::Continuous, faderRange(), 0.0f, " dB");
    }

    for (int b = 0; b < kNumBuses; ++b)
    {
        auto& bus = buses[(size_t) b];
        const auto p = "bus." + busCode (b) + ".";
        const auto n = "Bus " + busCode (b) + " ";

        bus.gain     = &params.add (p + "gain", n + "Fader", ParamType::Continuous, faderRange(), 0.0f, " dB");
        bus.mute     = &params.addToggle (p + "mute", n + "Mute");
        bus.mono     = &params.addToggle (p + "mono", n + "Mono");
        bus.eq.create (params, p, n);
        bus.limOn    = &params.addToggle (p + "lim.on", n + "Limiter On", true);
        bus.limCeil  = &params.add (p + "lim.ceiling", n + "Limiter Ceiling", ParamType::Continuous, linRange (-30, 0, 0.1f), 0.0f, " dB");
        bus.fxBypass = &params.addToggle (p + "fx.bypass", n + "FX Bypass");
        bus.sel      = &params.addToggle (p + "sel", n + "Select (SEL)");
    }

    monOn  = &params.addToggle ("mon.on", "Monitor Selected Bus");
    monBus = &params.addChoice ("mon.bus", "Monitor Output", { "A1", "A2", "A3", "A4", "A5" }, 0);
    recorder.createParams (params);
}

//==============================================================================
juce::AudioIODeviceType* Engine::findType (const juce::String& name) const
{
    for (auto* t : types)
        if (t->getTypeName() == name)
            return t;
    return nullptr;
}

void Engine::rescanDevices()
{
    for (auto* t : types)
        t->scanForDevices();
}

juce::StringArray Engine::getMasterInputNames() const
{
    return masterDevice != nullptr ? masterDevice->getInputChannelNames() : juce::StringArray();
}

juce::StringArray Engine::getMasterOutputNames() const
{
    return masterDevice != nullptr ? masterDevice->getOutputChannelNames() : juce::StringArray();
}

double Engine::getMasterLatencyMs() const
{
    if (masterDevice == nullptr || sampleRate <= 0) return 0.0;
    const int frames = masterDevice->getCurrentBufferSizeSamples() * 2
                     + masterDevice->getInputLatencyInSamples() + masterDevice->getOutputLatencyInSamples();
    return 1000.0 * frames / sampleRate;
}

double Engine::getIoLatencyMs (const IoTarget& t, bool input) const
{
    if (t.kind == IoTarget::Kind::Master)
    {
        if (masterDevice == nullptr) return -1.0;
        const int frames = masterDevice->getCurrentBufferSizeSamples()
                         + (input ? masterDevice->getInputLatencyInSamples() : masterDevice->getOutputLatencyInSamples());
        return 1000.0 * frames / sampleRate;
    }
    if (current == nullptr) return -1.0;
    if (input) { for (auto& s : current->ins)  if (s->target.sameStream (t)) return 1000.0 * s->latencyFrames.load() / juce::jmax (1.0, s->getDeviceRate()); }
    else       { for (auto& s : current->outs) if (s->target.sameStream (t)) return 1000.0 * s->latencyFrames.load() / juce::jmax (1.0, s->getDeviceRate()); }
    return -1.0;
}

juce::String Engine::getStreamStatus (const IoTarget& t, bool input) const
{
    if (t.kind == IoTarget::Kind::Master)
        return masterDevice != nullptr ? juce::String (1000.0 * blockSize / sampleRate, 1) + " ms" : "offline";

    if (current == nullptr) return "offline";

    auto fmt = [] (const AsyncStream& s)
    {
        const double ms = 1000.0 * s.latencyFrames.load() / juce::jmax (1.0, s.getDeviceRate());
        if (s.idle.load()) return juce::String ("waiting for app");
        auto txt = "+" + juce::String (ms, 1) + " ms";
        if (const int u = s.underruns.load(); u > 0) txt << "  (" << u << " dropouts)";
        return txt;
    };

    if (input) { for (auto& s : current->ins)  if (s->target.sameStream (t)) return fmt (*s); }
    else       { for (auto& s : current->outs) if (s->target.sameStream (t)) return fmt (*s); }
    return "offline";
}

//==============================================================================
void Engine::closeMaster()
{
    if (masterDevice != nullptr)
    {
        masterDevice->stop();
        masterDevice->close();
        masterDevice.reset();
    }
    running = false;
    live.store (nullptr);
    graveyard.clear();
    if (current != nullptr)
    {
        for (auto& s : current->ins)  s->stop();
        for (auto& s : current->outs) s->stop();
        current.reset();
    }
}

juce::String Engine::setMasterConfig (const MasterConfig& cfg)
{
    closeMaster();
    master = cfg;
    lastError.clear();

    auto* type = findType (cfg.typeName);
    if (type == nullptr)
        return lastError = "Driver type not available: " + cfg.typeName;

    type->scanForDevices();
    const bool isAsio = type->getTypeName() == "ASIO";
    auto names = type->getDeviceNames (false);
    juce::String name = cfg.deviceName;
    if (name.isEmpty() || ! names.contains (name))
    {
        const int def = type->getDefaultDeviceIndex (false);
        name = juce::isPositiveAndBelow (def, names.size()) ? names[def] : names[0];
        master.deviceName = name;
    }
    if (name.isEmpty())
        return lastError = "No output device found for " + cfg.typeName;

    masterDevice.reset (type->createDevice (name, isAsio ? name : juce::String()));
    if (masterDevice == nullptr)
        return lastError = "Cannot create " + name;

    juce::BigInteger ins, outs;
    outs.setRange (0, juce::jmin (kMaxDeviceChannels, masterDevice->getOutputChannelNames().size()), true);
    if (isAsio)
        ins.setRange (0, juce::jmin (kMaxDeviceChannels, masterDevice->getInputChannelNames().size()), true);

    auto rates = masterDevice->getAvailableSampleRates();
    double rate = rates.contains (cfg.sampleRate) ? cfg.sampleRate : masterDevice->getCurrentSampleRate();
    if (rate <= 0 && ! rates.isEmpty()) rate = rates.contains (48000.0) ? 48000.0 : rates.getFirst();

    int bs = cfg.bufferSize;
    auto sizes = masterDevice->getAvailableBufferSizes();
    if (bs <= 0 || ! sizes.contains (bs))
        bs = masterDevice->getDefaultBufferSize();

    lastError = masterDevice->open (ins, outs, rate, bs);
    if (lastError.isNotEmpty())
    {
        masterDevice.reset();
        sendChangeMessage();
        return lastError;
    }

    master.sampleRate = masterDevice->getCurrentSampleRate();
    master.bufferSize = masterDevice->getCurrentBufferSizeSamples();
    sampleRate = master.sampleRate;
    blockSize = juce::jmin (kMaxBlock, master.bufferSize);

    masterDevice->start (this); // calls audioDeviceAboutToStart -> prepares DSP and plugins
    running = true;
    rebuildGraph();
    sendChangeMessage();
    return {};
}

void Engine::prepareChains()
{
    for (auto& s : inputs) s.plugins.prepare (sampleRate, blockSize);
    for (auto& b : buses)  b.plugins.prepare (sampleRate, blockSize);
}

//==============================================================================
void Engine::setInputSource (int strip, const IoTarget& t)
{
    inputs[(size_t) strip].source = t;
    rebuildGraph();
}

void Engine::setBusOutput (int bus, const IoTarget& t)
{
    buses[(size_t) bus].output = t;
    rebuildGraph();
}

void Engine::rebuildGraph()
{
    if (masterDevice == nullptr)
        return;

    auto g = std::make_unique<Graph>();
    juce::StringArray errors;

    auto findIn = [&] (const IoTarget& t) -> AsyncInput*
    {
        for (auto& s : g->ins) if (s->target.sameStream (t)) return s.get();
        if (current != nullptr)
            for (auto& s : current->ins)
                if (s->target.sameStream (t) && ! s->failed.load()) { g->ins.push_back (s); return s.get(); }
        juce::String err;
        if (auto s = openAsyncInput (t, types, sampleRate, blockSize, err)) { s->start(); g->ins.push_back (s); return s.get(); }
        errors.add (t.describe() + ": " + err);
        return nullptr;
    };

    auto findOut = [&] (const IoTarget& t) -> AsyncOutput*
    {
        for (auto& s : g->outs) if (s->target.sameStream (t)) return s.get();
        if (current != nullptr)
            for (auto& s : current->outs)
                if (s->target.sameStream (t) && ! s->failed.load()) { g->outs.push_back (s); return s.get(); }
        juce::String err;
        if (auto s = openAsyncOutput (t, types, sampleRate, blockSize, err)) { s->start(); g->outs.push_back (s); return s.get(); }
        errors.add (t.describe() + ": " + err);
        return nullptr;
    };

    for (int i = 0; i < kNumInputs; ++i)
    {
        const auto& t = inputs[(size_t) i].source;
        auto& m = g->in[(size_t) i];
        m.ch = t.channel; m.mono = t.mono; m.kind = t.kind;
        if (t.kind == IoTarget::Kind::Device || t.kind == IoTarget::Kind::Loopback || t.kind == IoTarget::Kind::App)
        {
            m.s = findIn (t);
            if (m.s == nullptr) m.kind = IoTarget::Kind::None;
        }
        else if (t.kind == IoTarget::Kind::Virtual)
            m.kind = IoTarget::Kind::None; // virtual driver: not installed yet
    }

    for (int b = 0; b < kNumBuses; ++b)
    {
        const auto& t = buses[(size_t) b].output;
        auto& m = g->out[(size_t) b];
        m.ch = t.channel; m.mono = t.mono; m.kind = t.kind;
        if (t.kind == IoTarget::Kind::Device)
        {
            m.s = findOut (t);
            if (m.s == nullptr) m.kind = IoTarget::Kind::None;
        }
        else if (t.kind == IoTarget::Kind::Virtual || t.kind == IoTarget::Kind::Loopback)
            m.kind = IoTarget::Kind::None;
    }

    publishGraph (std::move (g));
    lastError = errors.joinIntoString ("\n");
    sendChangeMessage();
}

void Engine::publishGraph (std::unique_ptr<Graph> g)
{
    live.store (g.get(), std::memory_order_release);
    if (current != nullptr)
    {
        // Streams that are no longer referenced stop when the old graph dies.
        graveyard.emplace_back (std::move (current), callbackCount.load() + 2);
    }
    current = std::move (g);
    if (! running) graveyard.clear();
}

int Engine::getSelectedBus() const noexcept
{
    for (int b = 0; b < kNumBuses; ++b)
        if (buses[(size_t) b].sel->isOn()) return b;
    return -1;
}

int Engine::getMonitorBus() const noexcept
{
    const int sel = getSelectedBus();
    if (sel < 0 || ! monOn->isOn()) return -1;
    const int mon = juce::jlimit (0, kNumHwBuses - 1, juce::roundToInt (monBus->get()));
    return mon == sel ? -1 : mon;
}

Param* Engine::faderTarget (Param* p) const noexcept
{
    const int sel = getSelectedBus();
    if (sel < 0) return p;
    for (auto& s : inputs)
        if (s.gain == p) return s.send[(size_t) sel];
    return p;
}

void Engine::enforceExclusiveSelect()
{
    // Only one bus can be selected: the most recently switched on one wins.
    int newest = -1, count = 0;
    for (int b = 0; b < kNumBuses; ++b)
        if (buses[(size_t) b].sel->isOn()) { ++count; if (b != selOwner || newest < 0) newest = b; }
    if (count > 1)
        for (int b = 0; b < kNumBuses; ++b)
            if (b != newest) buses[(size_t) b].sel->set (0.0f);
    selOwner = count > 0 ? newest : -1;
}

void Engine::timerCallback()
{
    enforceExclusiveSelect();

    const auto now = callbackCount.load();
    for (auto it = graveyard.begin(); it != graveyard.end();)
    {
        if (now >= it->second || ! running)
        {
            // Stop streams that did not survive into the current graph
            for (auto& s : it->first->ins)
                if (s.use_count() == 1) s->stop();
            for (auto& s : it->first->outs)
                if (s.use_count() == 1) s->stop();
            it = graveyard.erase (it);
        }
        else ++it;
    }

    // Auto-recover failed secondary devices (unplugged / re-plugged) every ~3 s.
    static int recoverTick = 0;
    if (current != nullptr && ++recoverTick >= 30)
    {
        recoverTick = 0;
        bool anyFailed = false;
        for (auto& s : current->ins)  anyFailed |= s->failed.load();
        for (auto& s : current->outs) anyFailed |= s->failed.load();
        if (anyFailed) { rescanDevices(); rebuildGraph(); }
    }
}

//==============================================================================
void Engine::audioDeviceAboutToStart (juce::AudioIODevice* d)
{
    sampleRate = d->getCurrentSampleRate();
    blockSize = juce::jmin (kMaxBlock, d->getCurrentBufferSizeSamples());

    for (auto& s : inputs)
    {
        s.eq.prepare();
        s.gate.prepare (sampleRate);
        s.comp.prepare (sampleRate);
        s.denoiser->prepare (sampleRate);
        s.dnWasOn = false;
        s.lastTrim = 1; s.lastL = s.lastR = 0;
        s.lastRoute.fill (0.0f);
    }
    lastPlayRoute.fill (0.0f);
    recorder.setSampleRate (sampleRate);
    for (auto& b : buses)
    {
        b.eq.prepare();
        b.limiter.prepare (sampleRate);
        b.lastGain = 0;
    }
    prepareChains();
}

void Engine::audioDeviceStopped() { recorder.setSampleRate (0.0); }

void Engine::audioDeviceError (const juce::String& e)
{
    lastError = e;
    juce::MessageManager::callAsync ([this] { sendChangeMessage(); });
}

void Engine::audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                               int n, const juce::AudioIODeviceCallbackContext&)
{
    const juce::ScopedNoDenormals noDenormals;
    const auto t0 = juce::Time::getHighResolutionTicks();

    const Graph* g = live.load (std::memory_order_acquire);
    if (g == nullptr)
    {
        for (int c = 0; c < numOut; ++c) if (out[c]) std::memset (out[c], 0, (size_t) n * sizeof (float));
    }
    else
    {
        for (int offset = 0; offset < n; offset += kMaxBlock)
            processChunk (*g, in, numIn, out, numOut, offset, juce::jmin (kMaxBlock, n - offset));
    }

    const double secs = (double) (juce::Time::getHighResolutionTicks() - t0) * ticksToSeconds;
    const float load = (float) (secs * sampleRate / juce::jmax (1, n));
    const float prev = cpuLoad.load (std::memory_order_relaxed);
    cpuLoad.store (load > prev ? load : prev + 0.05f * (load - prev), std::memory_order_relaxed);
    callbackCount.fetch_add (1, std::memory_order_release);
}

void Engine::processChunk (const Graph& g, const float* const* in, int numIn, float* const* out, int numOut,
                           int offset, int n) noexcept
{
    // 1. Pull asynchronous inputs (resampled + drift corrected to the master clock).
    for (auto& s : g.ins) s->pullBlock (n);

    for (auto& b : buses)
    {
        std::memset (b.l.data(), 0, (size_t) n * sizeof (float));
        std::memset (b.r.data(), 0, (size_t) n * sizeof (float));
    }

    bool anySolo = false;
    for (auto& s : inputs) anySolo |= s.solo->isOn();

    // 2. Input strips -> bus sums.
    for (int i = 0; i < kNumInputs; ++i)
    {
        auto& s = inputs[(size_t) i];
        const auto& m = g.in[(size_t) i];
        const float* L = nullptr;
        const float* R = nullptr;

        if (m.kind == IoTarget::Kind::Master)
        {
            if (m.ch < numIn && in[m.ch] != nullptr) L = in[m.ch] + offset;
            const int rc = m.mono ? m.ch : m.ch + 1;
            if (rc < numIn && in[rc] != nullptr) R = in[rc] + offset;
            if (R == nullptr) R = L;
        }
        else if (m.s != nullptr)
        {
            L = m.s->channel (m.ch);
            R = m.mono ? L : m.s->channel (m.ch + 1);
        }

        if (L == nullptr)
        {
            s.lastL = s.lastR = 0.0f;
            s.lastRoute.fill (0.0f);
            continue;
        }
        processStrip (s, L, R, n, anySolo, L == R);
    }

    // 3. Recorder playback into the chosen buses.
    if (recorder.pullPlayback (playL.data(), playR.data(), n))
    {
        const float lvl = dbToGain (recorder.level->get());
        for (int b = 0; b < kNumBuses; ++b)
        {
            const float target = recorder.out[(size_t) b]->isOn() ? lvl : 0.0f;
            const float prev = lastPlayRoute[(size_t) b];
            if (target == 0.0f && prev == 0.0f) continue;
            dsp::addRamp (buses[(size_t) b].l.data(), playL.data(), n, prev, target);
            dsp::addRamp (buses[(size_t) b].r.data(), playR.data(), n, prev, target);
            lastPlayRoute[(size_t) b] = target;
        }
    }
    else
        lastPlayRoute.fill (0.0f);

    // 4. Bus processing. Monitoring: the selected bus is processed first and replaces the
    //    monitor bus's own mix, so only the monitored signal is heard there (through its EQ / fader).
    const int selBus = getSelectedBus();
    int monBusIndex = -1; // derived from the same SEL snapshot (SEL may change concurrently)
    if (selBus >= 0 && monOn->isOn())
        if (const int m = juce::jlimit (0, kNumHwBuses - 1, juce::roundToInt (monBus->get())); m != selBus)
            monBusIndex = m;
    if (monBusIndex >= 0) processBus (buses[(size_t) selBus], n);
    for (int b = 0; b < kNumBuses; ++b)
    {
        if (monBusIndex >= 0 && b == selBus) continue;
        auto& bus = buses[(size_t) b];
        if (b == monBusIndex)
        {
            std::memcpy (bus.l.data(), buses[(size_t) selBus].l.data(), (size_t) n * sizeof (float));
            std::memcpy (bus.r.data(), buses[(size_t) selBus].r.data(), (size_t) n * sizeof (float));
        }
        processBus (bus, n);
    }

    if (recorder.wantsRecord())
    {
        const auto& rb = buses[(size_t) recorder.getRecordSourceBus()];
        recorder.pushRecord (rb.l.data(), rb.r.data(), n);
    }

    // 5. Buses -> outputs.
    for (int c = 0; c < numOut; ++c)
        if (out[c] != nullptr) std::memset (out[c] + offset, 0, (size_t) n * sizeof (float));
    for (auto& s : g.outs) s->clearMix (n);

    for (int b = 0; b < kNumBuses; ++b)
    {
        auto& bus = buses[(size_t) b];
        const auto& m = g.out[(size_t) b];
        float* oL = nullptr;
        float* oR = nullptr;
        if (m.kind == IoTarget::Kind::Master)
        {
            if (m.ch < numOut && out[m.ch] != nullptr) oL = out[m.ch] + offset;
            if (! m.mono && m.ch + 1 < numOut && out[m.ch + 1] != nullptr) oR = out[m.ch + 1] + offset;
        }
        else if (m.s != nullptr)
        {
            oL = m.s->mixChannel (m.ch);
            if (! m.mono && m.ch + 1 < m.s->getNumChannels()) oR = m.s->mixChannel (m.ch + 1);
        }

        if (oL == nullptr) continue;
        if (m.mono || oR == nullptr)
        {
            juce::FloatVectorOperations::addWithMultiply (oL, bus.l.data(), 0.5f, n);
            juce::FloatVectorOperations::addWithMultiply (oL, bus.r.data(), 0.5f, n);
        }
        else
        {
            juce::FloatVectorOperations::add (oL, bus.l.data(), n);
            juce::FloatVectorOperations::add (oR, bus.r.data(), n);
        }
    }

    // 6. Push asynchronous outputs.
    for (auto& s : g.outs) s->pushBlock (n);
}

void Engine::processStrip (InputStrip& s, const float* srcL, const float* srcR, int n, bool anySolo, bool monoSource) noexcept
{
    float* l = s.l.data();
    float* r = s.r.data();
    std::memcpy (l, srcL, (size_t) n * sizeof (float));
    std::memcpy (r, srcR, (size_t) n * sizeof (float));

    if (s.phase->isOn())
    {
        juce::FloatVectorOperations::negate (l, l, n);
        juce::FloatVectorOperations::negate (r, r, n);
    }

    const float trim = dbToGain (s.trim->get(), -100.0f);
    dsp::applyRamp (l, n, s.lastTrim, trim);
    dsp::applyRamp (r, n, s.lastTrim, trim);
    s.lastTrim = trim;
    s.preMeter.push (dsp::peak (l, n), dsp::peak (r, n));

    // Denoiser first, so EQ / gate / compressor work on the cleaned signal.
    const bool dn = s.dnOn->isOn();
    if (dn)
    {
        if (! s.dnWasOn) s.denoiser->reset();
        s.denoiser->process (l, r, n, s.denoiserSettings(), ! monoSource);
    }
    s.dnWasOn = dn;
    s.dspLatency.store (dn ? s.denoiser->getLatencySamples() : 0, std::memory_order_relaxed);

    s.eq.process (l, r, n, sampleRate);
    s.spectrum.push (l, r, n);

    if (s.gateOn->isOn())
        s.gate.process (l, r, n, s.gateSettings());
    else
        s.gate.shownGain.store (1.0f, std::memory_order_relaxed);

    if (s.compOn->isOn())
    {
        s.comp.process (l, r, n, s.compSettings());
        s.compGr.store (s.comp.lastGr, std::memory_order_relaxed);
    }
    else
        s.compGr.store (0.0f, std::memory_order_relaxed);

    if (! s.fxBypass->isOn())
        s.plugins.process (l, r, n);

    if (s.mono->isOn())
    {
        juce::FloatVectorOperations::add (l, r, n);
        juce::FloatVectorOperations::multiply (l, 0.5f, n);
        std::memcpy (r, l, (size_t) n * sizeof (float));
    }

    const bool silent = s.mute->isOn() || (anySolo && ! s.solo->isOn());
    const float pan = s.pan->get();
    const float fader = dbToGain (s.gain->get());
    const float panL = pan > 0.0f ? 1.0f - pan : 1.0f;
    const float panR = pan < 0.0f ? 1.0f + pan : 1.0f;
    if (silent) // meter shows the level the strip would have (drawn grey)
        s.meter.push (dsp::peak (l, n) * fader * panL, dsp::peak (r, n) * fader * panR);
    s.meter.muted.store (silent, std::memory_order_relaxed);

    const float g = silent ? 0.0f : fader;
    const float gl = g * panL;
    const float gr = g * panR;
    dsp::applyRamp (l, n, s.lastL, gl);
    dsp::applyRamp (r, n, s.lastR, gr);
    const bool wasSilent = s.lastL == 0.0f && s.lastR == 0.0f && gl == 0.0f && gr == 0.0f;
    s.lastL = gl; s.lastR = gr;

    if (! silent)
        s.meter.push (dsp::peak (l, n), dsp::peak (r, n));

    if (wasSilent)
    {
        s.lastRoute.fill (0.0f);
        return;
    }

    for (int b = 0; b < kNumBuses; ++b)
    {
        const float target = s.route[(size_t) b]->isOn() ? dbToGain (s.send[(size_t) b]->get()) : 0.0f;
        const float prev = s.lastRoute[(size_t) b];
        if (target == 0.0f && prev == 0.0f) continue;
        auto& bus = buses[(size_t) b];
        dsp::addRamp (bus.l.data(), l, n, prev, target);
        dsp::addRamp (bus.r.data(), r, n, prev, target);
        s.lastRoute[(size_t) b] = target;
    }
}

void Engine::processBus (Bus& b, int n) noexcept
{
    float* l = b.l.data();
    float* r = b.r.data();

    b.eq.process (l, r, n, sampleRate);
    b.spectrum.push (l, r, n);

    if (! b.fxBypass->isOn())
        b.plugins.process (l, r, n);

    if (b.mono->isOn())
    {
        juce::FloatVectorOperations::add (l, r, n);
        juce::FloatVectorOperations::multiply (l, 0.5f, n);
        std::memcpy (r, l, (size_t) n * sizeof (float));
    }

    const bool muted = b.mute->isOn();
    const float fader = dbToGain (b.gain->get());
    if (muted) // meter shows the level the bus would have (drawn grey), capped at the limiter ceiling
    {
        const float cap = b.limOn->isOn() ? dbToGain (b.limCeil->get()) : 1.0e9f;
        b.meter.push (juce::jmin (cap, dsp::peak (l, n) * fader), juce::jmin (cap, dsp::peak (r, n) * fader));
    }
    b.meter.muted.store (muted, std::memory_order_relaxed);

    const float g = muted ? 0.0f : fader;
    dsp::applyRamp (l, n, b.lastGain, g);
    dsp::applyRamp (r, n, b.lastGain, g);
    b.lastGain = g;

    if (b.limOn->isOn())
        b.limiter.process (l, r, n, b.limCeil->get());

    if (! muted)
        b.meter.push (dsp::peak (l, n), dsp::peak (r, n));
}
} // namespace hydra

#include "Engine.h"

#include <map>
#include <numeric>

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
    while (types.contains (nullptr)) types.removeObject (nullptr);
    rescanDevices();

    for (int i = 0; i < kMaxInputs; ++i)
    {
        inputs[(size_t) i].index = i;
        inputs[(size_t) i].name = "Input " + juce::String (i + 1);
        inputs[(size_t) i].spectrum.ring.allocate (2, 16384);
        inputs[(size_t) i].l.assign (kMaxBlock, 0.0f);
        inputs[(size_t) i].r.assign (kMaxBlock, 0.0f);
    }
    for (int b = 0; b < kMaxBuses; ++b)
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
    for (int i = 0; i < kMaxInputs; ++i)
    {
        auto& s = inputs[(size_t) i];
        const auto p = "in" + juce::String (i + 1) + ".";
        const auto n = "In " + juce::String (i + 1) + " ";

        s.active     = &params.addToggle (p + "active", n + "In Use", i < kDefaultInputs);
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
        s.syncOn     = &params.addToggle (p + "sync", n + "Sync Delay Auto");
        s.delayMs    = &params.add (p + "delay", n + "Sync Delay", ParamType::Continuous,
                                    linRange (0, (float) dsp::DelayLine::kMaxMs, 0.1f), 0.0f, " ms");
        for (int b = 0; b < kMaxBuses; ++b)
            s.route[(size_t) b] = &params.addToggle (p + "route." + busCode (b), n + "-> " + busCode (b), b == 0);
        for (int b = 0; b < kMaxBuses; ++b)
            s.send[(size_t) b] = &params.add (p + "send." + busCode (b), n + "Level -> " + busCode (b), ParamType::Continuous, faderRange(), 0.0f, " dB");
    }

    for (int b = 0; b < kMaxBuses; ++b)
    {
        auto& bus = buses[(size_t) b];
        const auto p = "bus." + busCode (b) + ".";
        const auto n = "Bus " + busCode (b) + " ";

        bus.active   = &params.addToggle (p + "active", n + "In Use",
                                          isHwBus (b) ? b < hwBusIndex (kDefaultHwBuses) : b < virtBusIndex (kDefaultVirtBuses));
        bus.gain     = &params.add (p + "gain", n + "Fader", ParamType::Continuous, faderRange(), 0.0f, " dB");
        bus.mute     = &params.addToggle (p + "mute", n + "Mute");
        bus.mono     = &params.addToggle (p + "mono", n + "Mono");
        bus.eq.create (params, p, n);
        bus.limOn    = &params.addToggle (p + "lim.on", n + "Limiter On", true);
        bus.limCeil  = &params.add (p + "lim.ceiling", n + "Limiter Ceiling", ParamType::Continuous, linRange (-30, 0, 0.1f), 0.0f, " dB");
        bus.fxBypass = &params.addToggle (p + "fx.bypass", n + "FX Bypass");
        bus.sel      = &params.addToggle (p + "sel", n + "Select (SEL)");
        bus.syncOn   = &params.addToggle (p + "sync", n + "Sync Delay Auto");
        bus.delayMs  = &params.add (p + "delay", n + "Sync Delay", ParamType::Continuous,
                                    linRange (0, (float) dsp::DelayLine::kMaxMs, 0.1f), 0.0f, " ms");
    }

    monOn  = &params.addToggle ("mon.on", "Monitor Selected Bus");
    juce::StringArray hwCodes;
    for (int a = 0; a < kMaxHwBuses; ++a) hwCodes.add (busCode (hwBusIndex (a)));
    monBus = &params.addChoice ("mon.bus", "Monitor Output", hwCodes, 0);
    recorder.createParams (params);

    for (int k = 0; k < kMaxScenes; ++k)
    {
        sceneRecall[(size_t) k] = &params.addToggle ("scene." + juce::String (k + 1), "Recall Scene " + juce::String (k + 1));
        sceneRecall[(size_t) k]->persistent = false;
    }
    sceneNext = &params.addToggle ("scene.next", "Next Scene");
    scenePrev = &params.addToggle ("scene.prev", "Previous Scene");
    sceneNext->persistent = false;
    scenePrev->persistent = false;
}

//==============================================================================
int Engine::numActiveInputs() const noexcept
{
    int n = 0;
    for (auto& s : inputs) n += s.active->isOn() ? 1 : 0;
    return n;
}

int Engine::numActiveBuses (bool virtualBuses) const noexcept
{
    int n = 0;
    for (int b = 0; b < kMaxBuses; ++b)
        if (isHwBus (b) != virtualBuses && buses[(size_t) b].active->isOn()) ++n;
    return n;
}

uint32_t Engine::getLayoutVersion() const noexcept
{
    uint32_t v = 0;
    for (auto& s : inputs) v += s.active->getVersion();
    for (auto& b : buses)  v += b.active->getVersion();
    return v;
}

int Engine::addInput()
{
    for (int i = 0; i < kMaxInputs; ++i)
        if (! inputs[(size_t) i].active->isOn())
        {
            inputs[(size_t) i].active->set (1.0f);
            sendChangeMessage();
            return i;
        }
    return -1;
}

int Engine::addBus (bool virtualBus)
{
    const int first = virtualBus ? virtBusIndex (0) : hwBusIndex (0);
    const int count = virtualBus ? kMaxVirtBuses : kMaxHwBuses;
    for (int b = first; b < first + count; ++b)
        if (! buses[(size_t) b].active->isOn())
        {
            buses[(size_t) b].active->set (1.0f);
            sendChangeMessage();
            return b;
        }
    return -1;
}

namespace
{
/** Resets every parameter whose id starts with prefix (except the slot's "active" switch). */
void resetWithPrefix (ParamRegistry& reg, const juce::String& prefix, std::vector<Param*>& out)
{
    for (auto& p : reg.all())
        if (p->id.startsWith (prefix) && ! p->id.endsWith (".active"))
        {
            p->reset();
            out.push_back (p.get());
        }
}
} // namespace

std::vector<Param*> Engine::removeInput (int i)
{
    std::vector<Param*> reset;
    if (! juce::isPositiveAndBelow (i, kMaxInputs) || ! isInputActive (i) || numActiveInputs() <= 1)
        return reset;

    auto& s = inputs[(size_t) i];
    s.active->set (0.0f);   // the audio thread skips the strip from the next block on
    resetWithPrefix (params, "in" + juce::String (i + 1) + ".", reset);
    s.name = "Input " + juce::String (i + 1);
    s.source = {};
    s.plugins.clear();
    rebuildGraph();
    sendChangeMessage();
    return reset;
}

std::vector<Param*> Engine::removeBus (int b)
{
    std::vector<Param*> reset;
    if (! juce::isPositiveAndBelow (b, kMaxBuses) || ! isBusActive (b)) return reset;
    if (isHwBus (b) && numActiveBuses (false) <= 1) return reset; // keep at least one A bus

    auto& bus = buses[(size_t) b];
    bus.active->set (0.0f);
    resetWithPrefix (params, "bus." + busCode (b) + ".", reset);
    for (auto& s : inputs)
        for (auto* p : { s.route[(size_t) b], s.send[(size_t) b] }) { p->reset(); reset.push_back (p); }
    if (auto* p = recorder.out[(size_t) b]) { p->set (0.0f); reset.push_back (p); }
    bus.name = busCode (b);
    bus.output = {};
    bus.plugins.clear();
    if (selOwner == b) selOwner = -1;
    rebuildGraph();
    sendChangeMessage();
    return reset;
}

//==============================================================================
namespace
{
/** Bus index of a code like "A3" / "B1", or -1. */
int busFromCode (const juce::String& code)
{
    if (code.length() < 2) return -1;
    const int n = code.substring (1).getIntValue() - 1;
    if (code[0] == 'A' && juce::isPositiveAndBelow (n, kMaxHwBuses))   return hwBusIndex (n);
    if (code[0] == 'B' && juce::isPositiveAndBelow (n, kMaxVirtBuses)) return virtBusIndex (n);
    return -1;
}

/** Id of the parameter that takes over p's value when the slots move as dest says (dest[slot] = new slot). */
juce::String movedId (const juce::String& id, bool buses, const std::vector<int>& dest)
{
    juce::StringArray t;
    t.addTokens (id, ".", {});
    if (! buses)
    {
        // inN.<anything>
        if (! t[0].startsWith ("in")) return id;
        const int i = t[0].substring (2).getIntValue() - 1;
        if (! juce::isPositiveAndBelow (i, (int) dest.size()) || dest[(size_t) i] == i) return id;
        return "in" + juce::String (dest[(size_t) i] + 1) + id.fromFirstOccurrenceOf (".", true, false);
    }
    // bus.X.<anything>, inN.route.X, inN.send.X, rec.out.X
    int part = -1;
    if (t[0] == "bus" && t.size() >= 3) part = 1;
    else if (t.size() == 3 && t[0].startsWith ("in") && (t[1] == "route" || t[1] == "send")) part = 2;
    else if (t.size() == 3 && t[0] == "rec" && t[1] == "out") part = 2;
    if (part < 0) return id;
    const int b = busFromCode (t[part]);
    if (b < 0 || dest[(size_t) b] == b) return id;
    t.set (part, busCode (dest[(size_t) b]));
    return t.joinIntoString (".");
}
} // namespace

std::vector<std::pair<Param*, Param*>> Engine::reorderPlan (Group group, const std::vector<int>& order) const
{
    std::vector<std::pair<Param*, Param*>> plan;
    const bool forBuses = group != Group::inputs;
    const int first = group == Group::virtBuses ? virtBusIndex (0) : 0;
    const int count = group == Group::inputs ? kMaxInputs : group == Group::hwBuses ? kMaxHwBuses : kMaxVirtBuses;

    // The slots in use keep being used: the k-th of them receives order[k].
    std::vector<int> used;
    for (int k = first; k < first + count; ++k)
        if (forBuses ? isBusActive (k) : isInputActive (k)) used.push_back (k);
    auto sorted = order;
    std::sort (sorted.begin(), sorted.end());
    if (sorted != used || order == used) return plan;

    std::vector<int> dest ((size_t) (forBuses ? kMaxBuses : kMaxInputs));
    std::iota (dest.begin(), dest.end(), 0);
    for (size_t k = 0; k < order.size(); ++k) dest[(size_t) order[k]] = used[k];

    for (auto& p : params.all())
    {
        const auto to = movedId (p->id, forBuses, dest);
        if (to == p->id) continue;
        if (auto* q = params.find (to)) plan.emplace_back (p.get(), q);
    }
    return plan;
}

bool Engine::reorderChannels (Group group, const std::vector<int>& order)
{
    const auto plan = reorderPlan (group, order);
    if (plan.empty()) return false;
    const bool forBuses = group != Group::inputs;
    const int first = group == Group::virtBuses ? virtBusIndex (0) : 0;
    std::vector<int> used (order);
    std::sort (used.begin(), used.end());
    std::vector<int> dest ((size_t) (forBuses ? kMaxBuses : kMaxInputs));
    std::iota (dest.begin(), dest.end(), 0);
    for (size_t k = 0; k < order.size(); ++k) dest[(size_t) order[k]] = used[k];

    // Parameter values: snapshot first, the moves form a permutation.
    std::vector<float> values;
    values.reserve (plan.size());
    for (auto& [from, to] : plan) values.push_back (from->get());
    for (size_t k = 0; k < plan.size(); ++k) plan[k].second->set (values[k]);

    if (! forBuses)
    {
        std::array<juce::String, kMaxInputs> names;
        std::array<IoTarget, kMaxInputs> sources;
        std::array<std::vector<std::unique_ptr<PluginSlot>>, kMaxInputs> chains;
        for (int i : used)
        {
            names[(size_t) i] = inputs[(size_t) i].name;
            sources[(size_t) i] = inputs[(size_t) i].source;
            chains[(size_t) i] = inputs[(size_t) i].plugins.takeAll();
        }
        for (int i : used)
        {
            auto& to = inputs[(size_t) dest[(size_t) i]];
            to.name = names[(size_t) i];
            to.source = sources[(size_t) i];
            to.plugins.putAll (std::move (chains[(size_t) i]));
        }
    }
    else
    {
        std::array<juce::String, kMaxBuses> names;
        std::array<IoTarget, kMaxBuses> outputs;
        std::array<std::vector<std::unique_ptr<PluginSlot>>, kMaxBuses> chains;
        for (int b : used)
        {
            names[(size_t) b] = buses[(size_t) b].name;
            outputs[(size_t) b] = buses[(size_t) b].output;
            chains[(size_t) b] = buses[(size_t) b].plugins.takeAll();
        }
        for (int b : used)
        {
            auto& to = buses[(size_t) dest[(size_t) b]];
            to.name = names[(size_t) b];
            to.output = outputs[(size_t) b];
            to.plugins.putAll (std::move (chains[(size_t) b]));
        }
        // Choices that name a bus follow it.
        if (group == Group::virtBuses)
            recorder.source->set ((float) (dest[(size_t) recorder.getRecordSourceBus()] - first));
        else
            monBus->set ((float) dest[(size_t) juce::jlimit (0, kMaxHwBuses - 1, juce::roundToInt (monBus->get()))]);
        if (selOwner >= 0) selOwner = dest[(size_t) selOwner];
    }

    rebuildGraph();
    sendChangeMessage();
    return true;
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

bool Engine::isMasterAsio (const IoTarget& t) const
{
    return t.kind == IoTarget::Kind::Device && t.typeName == kAsioTypeName && masterDevice != nullptr
        && masterDevice->getTypeName() == kAsioTypeName && masterDevice->getName() == t.deviceName;
}

juce::StringArray Engine::getSecondaryAsioDrivers() const
{
    juce::StringArray names;
    if (auto* type = findType (kAsioTypeName))
        names = type->getDeviceNames (false);
    if (masterDevice != nullptr && masterDevice->getTypeName() == kAsioTypeName)
        names.removeString (masterDevice->getName());
    return names;
}

juce::StringArray Engine::getAsioChannelNames (const juce::String& driver, bool inputs) const
{
    auto* type = findType (kAsioTypeName);
    return type != nullptr ? asioChannelNames (*type, driver, inputs) : juce::StringArray();
}

double Engine::getMasterLatencyMs() const
{
    if (masterDevice == nullptr || sampleRate <= 0) return 0.0;
    const int frames = masterDevice->getCurrentBufferSizeSamples() * 2
                     + masterDevice->getInputLatencyInSamples() + masterDevice->getOutputLatencyInSamples();
    return 1000.0 * frames / sampleRate;
}

Engine::IoLatency Engine::getIoLatency (const IoTarget& t, bool input) const
{
    IoLatency l;
    if (t.kind == IoTarget::Kind::Master || isMasterAsio (t))
    {
        if (masterDevice == nullptr || sampleRate <= 0) return l;
        l.online = l.masterClock = true;
        l.bufferFrames = masterDevice->getCurrentBufferSizeSamples();
        l.deviceRate = sampleRate;
        l.bufferMs = 1000.0 * l.bufferFrames / sampleRate;
        l.driverMs = 1000.0 * (input ? masterDevice->getInputLatencyInSamples() : masterDevice->getOutputLatencyInSamples()) / sampleRate;
        return l;
    }
    if (current == nullptr) return l;
    auto fill = [&l] (const AsyncStream& s)
    {
        if (s.failed.load()) return;
        const double rate = juce::jmax (1.0, s.getDeviceRate());
        l.online = true;
        l.bufferFrames = s.getDeviceBlock();
        l.deviceRate = rate;
        l.driverMs = 1000.0 * s.deviceLatencyFrames / rate;
        l.clockSyncMs = 1000.0 * s.latencyFrames.load() / rate;
    };
    if (input) { for (auto& s : current->ins)  if (s->target.sameStream (t)) { fill (*s); return l; } }
    else       { for (auto& s : current->outs) if (s->target.sameStream (t)) { fill (*s); return l; } }
    return l;
}

double Engine::getIoLatencyMs (const IoTarget& t, bool input) const
{
    const auto l = getIoLatency (t, input);
    return l.online ? l.total() : -1.0;
}

double Engine::pluginLatencyMs (const PluginChain& chain, const Param& bypassAll) const
{
    if (bypassAll.isOn() || sampleRate <= 0) return 0.0;
    int samples = 0;
    for (int i = 0; i < chain.size(); ++i)
        if (auto* s = chain.getSlot (i); s != nullptr && ! s->bypassed && s->instance != nullptr)
            samples += s->instance->getLatencySamples();
    return 1000.0 * samples / sampleRate;
}

std::vector<std::pair<juce::String, uint64_t>> Engine::getStreamDspTicks() const
{
    std::vector<std::pair<juce::String, uint64_t>> out;
    if (current == nullptr) return out;
    auto label = [] (const AsyncStream& s, bool input)
    {
        auto t = s.target;
        t.channel = 0;
        t.mono = false;
        auto name = t.kind == IoTarget::Kind::Device ? t.deviceName : t.describe();
        return juce::String (input ? "From  " : "To  ") + name;
    };
    for (auto& s : current->ins)  out.emplace_back (label (*s, true), s->dspTicks.load (std::memory_order_relaxed));
    for (auto& s : current->outs) out.emplace_back (label (*s, false), s->dspTicks.load (std::memory_order_relaxed));
    return out;
}

Engine::StreamHealth Engine::getStreamHealth (const IoTarget& t, bool input) const
{
    StreamHealth h;
    if (t.kind == IoTarget::Kind::None) return h;
    if (t.kind == IoTarget::Kind::Virtual) { h.state = StreamHealth::State::noDriver; return h; }

    const auto nowMs = juce::Time::currentTimeMillis();
    auto collect = [&h, nowMs] (const DropoutLog& log)
    {
        for (auto it = log.events.rbegin(); it != log.events.rend(); ++it)
            h.recent.push_back ({ it->second, (double) (nowMs - it->first) / 1000.0 });
    };
    auto errorFor = [this, &t]
    {
        juce::StringArray lines;
        lines.addLines (lastError);
        for (auto& line : lines)
            if (line.startsWith (t.describe() + ":")) return line.fromFirstOccurrenceOf (":", false, false).trim();
        return juce::String();
    };

    if (t.kind == IoTarget::Kind::Master || isMasterAsio (t))
    {
        if (masterDevice == nullptr) { h.state = StreamHealth::State::offline; h.error = lastError; return h; }
        h.state = StreamHealth::State::ok;
        collect (masterDropouts);
        return h;
    }

    const AsyncStream* found = nullptr;
    if (current != nullptr)
    {
        if (input) { for (auto& s : current->ins)  if (s->target.sameStream (t)) found = s.get(); }
        else       { for (auto& s : current->outs) if (s->target.sameStream (t)) found = s.get(); }
    }
    if (found == nullptr || found->failed.load())
    {
        h.state = StreamHealth::State::offline;
        h.error = found != nullptr && found->error.isNotEmpty() ? found->error : errorFor();
        return h;
    }
    h.state = found->idle.load() ? StreamHealth::State::waiting : StreamHealth::State::ok;
    if (auto it = dropoutLogs.find (found); it != dropoutLogs.end()) collect (it->second);
    return h;
}

void Engine::updateDropoutLogs()
{
    // Dropout counters only grow; remember when they grew, forget what is older than the window.
    const auto nowMs = juce::Time::currentTimeMillis();
    const auto oldest = nowMs - (juce::int64) (kDropoutWindowSeconds * 1000.0);
    auto track = [nowMs, oldest] (DropoutLog& log, int count)
    {
        if (count < log.seen) log.seen = count;   // counter restarted (device reopened)
        if (count > log.seen) log.events.emplace_back (nowMs, count - log.seen);
        log.seen = count;
        while (! log.events.empty() && log.events.front().first < oldest) log.events.erase (log.events.begin());
    };

    if (masterDevice != nullptr)
        if (const int x = masterDevice->getXRunCount(); x >= 0) track (masterDropouts, x);

    std::map<const AsyncStream*, DropoutLog> next;
    if (current != nullptr)
    {
        auto keep = [&] (const AsyncStream& s)
        {
            auto& log = next[&s];
            if (auto it = dropoutLogs.find (&s); it != dropoutLogs.end()) log = std::move (it->second);
            else log.seen = s.underruns.load();   // a new stream: count only what happens from now on
            track (log, s.underruns.load());
        };
        for (auto& s : current->ins)  keep (*s);
        for (auto& s : current->outs) keep (*s);
    }
    dropoutLogs = std::move (next);
}

//==============================================================================
double Engine::getChannelDelayMs (int ch) const noexcept
{
    const Param* manual = ch < kMaxInputs ? inputs[(size_t) ch].delayMs : buses[(size_t) (ch - kMaxInputs)].delayMs;
    return juce::jlimit (0.0, dsp::DelayLine::kMaxMs, (double) autoDelayMs[(size_t) ch].load (std::memory_order_relaxed) + manual->get());
}

int Engine::delaySamples (int ch) const noexcept
{
    return juce::roundToInt (getChannelDelayMs (ch) * sampleRate / 1000.0);
}

void Engine::updateSyncDelays()
{
    // 1. Latency of every channel without its own delay, smoothed (~1 s): the fill level of the drift buffers
    //    moves a little all the time. A big jump (other device, plugin added) is taken at once.
    for (int ch = 0; ch < kMaxInputs + kMaxBuses; ++ch)
    {
        auto& st = syncStates[(size_t) ch];
        double ms = -1.0;
        bool on = false;
        if (ch < kMaxInputs)
        {
            const auto& s = inputs[(size_t) ch];
            st.group = (int) Group::inputs;
            on = s.syncOn->isOn();
            if (s.active->isOn() && s.source.kind != IoTarget::Kind::None)
                if (const double dev = getIoLatencyMs (s.source, true); dev >= 0.0)
                    ms = dev + (s.dnOn->isOn() ? 1000.0 * s.dspLatency.load() / juce::jmax (1.0, sampleRate) : 0.0)
                       + pluginLatencyMs (s.plugins, *s.fxBypass);
        }
        else
        {
            const int b = ch - kMaxInputs;
            const auto& bus = buses[(size_t) b];
            st.group = (int) (isHwBus (b) ? Group::hwBuses : Group::virtBuses);
            on = bus.syncOn->isOn();
            if (bus.active->isOn() && bus.output.kind != IoTarget::Kind::None)
                if (const double dev = getIoLatencyMs (bus.output, false); dev >= 0.0)
                    ms = dev + pluginLatencyMs (bus.plugins, *bus.fxBypass);
        }
        if (ms < 0.0 || st.measuredMs < 0.0 || std::abs (ms - st.measuredMs) > 5.0) st.measuredMs = ms;
        else st.measuredMs += 0.1 * (ms - st.measuredMs);
        st.member = on && st.measuredMs >= 0.0;
        st.reference = false;
    }

    // 2. Per group: every member is delayed up to the slowest member. Changes below 0.5 ms are ignored,
    //    so the delay does not follow the jitter of the drift buffers.
    for (int group = 0; group < 3; ++group)
    {
        double slowest = -1.0;
        int ref = -1, members = 0;
        for (int ch = 0; ch < kMaxInputs + kMaxBuses; ++ch)
            if (const auto& st = syncStates[(size_t) ch]; st.group == group && st.member)
            {
                ++members;
                if (st.measuredMs > slowest) { slowest = st.measuredMs; ref = ch; }
            }
        for (int ch = 0; ch < kMaxInputs + kMaxBuses; ++ch)
        {
            auto& st = syncStates[(size_t) ch];
            if (st.group != group) continue;
            double wanted = 0.0;
            if (st.member && members > 1)
            {
                wanted = juce::jlimit (0.0, dsp::DelayLine::kMaxMs, slowest - st.measuredMs);
                st.reference = ch == ref;
            }
            if (std::abs (wanted - st.autoMs) >= 0.5 || (wanted == 0.0 && st.autoMs != 0.0))
                st.autoMs = wanted;
            autoDelayMs[(size_t) ch].store ((float) st.autoMs, std::memory_order_relaxed);
        }
    }
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
    dropoutLogs.clear();
    masterDropouts = {};
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

    // A secondary ASIO driver gets one stream per direction carrying every routed channel of it,
    // so all channels of that interface stay sample-aligned to each other.
    std::map<juce::String, std::vector<int>> asioIns, asioOuts;
    auto isSecondaryAsio = [this] (const IoTarget& t)
    {
        return t.kind == IoTarget::Kind::Device && t.typeName == kAsioTypeName && ! isMasterAsio (t);
    };
    auto addChannels = [] (std::vector<int>& v, const IoTarget& t)
    {
        v.push_back (t.channel);
        if (! t.mono) v.push_back (t.channel + 1);
    };
    for (auto& s : inputs) if (s.active->isOn() && isSecondaryAsio (s.source)) addChannels (asioIns[s.source.deviceName], s.source);
    for (auto& b : buses)  if (b.active->isOn() && isSecondaryAsio (b.output)) addChannels (asioOuts[b.output.deviceName], b.output);
    auto channelsFor = [&] (const IoTarget& t, std::map<juce::String, std::vector<int>>& m)
    {
        return isSecondaryAsio (t) ? m[t.deviceName] : std::vector<int>();
    };

    auto findIn = [&] (const IoTarget& t) -> AsyncInput*
    {
        const auto chans = channelsFor (t, asioIns);
        for (auto& s : g->ins) if (s->target.sameStream (t) && s->carries (chans)) return s.get();
        if (current != nullptr)
            for (auto& s : current->ins)
                if (s->target.sameStream (t) && s->carries (chans) && ! s->failed.load()) { g->ins.push_back (s); return s.get(); }
        juce::String err;
        if (auto s = openAsyncInput (t, types, sampleRate, blockSize, err, chans)) { s->start(); g->ins.push_back (s); return s.get(); }
        errors.add (t.describe() + ": " + err);
        return nullptr;
    };

    auto findOut = [&] (const IoTarget& t) -> AsyncOutput*
    {
        const auto chans = channelsFor (t, asioOuts);
        for (auto& s : g->outs) if (s->target.sameStream (t) && s->carries (chans)) return s.get();
        if (current != nullptr)
            for (auto& s : current->outs)
                if (s->target.sameStream (t) && s->carries (chans) && ! s->failed.load()) { g->outs.push_back (s); return s.get(); }
        juce::String err;
        if (auto s = openAsyncOutput (t, types, sampleRate, blockSize, err, chans)) { s->start(); g->outs.push_back (s); return s.get(); }
        errors.add (t.describe() + ": " + err);
        return nullptr;
    };

    for (int i = 0; i < kMaxInputs; ++i)
    {
        const auto& t = inputs[(size_t) i].source;
        auto& m = g->in[(size_t) i];
        m.ch = t.channel; m.chR = t.channel + 1; m.mono = t.mono; m.kind = t.kind;
        if (! inputs[(size_t) i].active->isOn())
            m.kind = IoTarget::Kind::None; // removed channel: never opens its device
        else if (isMasterAsio (t))
            m.kind = IoTarget::Kind::Master; // the master's own driver: use its channels directly
        else if (t.kind == IoTarget::Kind::Device || t.kind == IoTarget::Kind::Loopback || t.kind == IoTarget::Kind::App)
        {
            m.s = findIn (t);
            if (m.s == nullptr) m.kind = IoTarget::Kind::None;
            else if (! m.s->deviceChannels.empty())
            {
                m.ch = m.s->localChannel (t.channel);
                m.chR = t.mono ? m.ch : m.s->localChannel (t.channel + 1);
                if (m.ch < 0) m.kind = IoTarget::Kind::None;
                else if (m.chR < 0) m.chR = m.ch;
            }
        }
        else if (t.kind == IoTarget::Kind::Virtual)
            m.kind = IoTarget::Kind::None; // virtual driver: not installed yet
    }

    for (int b = 0; b < kMaxBuses; ++b)
    {
        const auto& t = buses[(size_t) b].output;
        auto& m = g->out[(size_t) b];
        m.ch = t.channel; m.chR = t.channel + 1; m.mono = t.mono; m.kind = t.kind;
        if (! buses[(size_t) b].active->isOn())
            m.kind = IoTarget::Kind::None;
        else if (isMasterAsio (t))
            m.kind = IoTarget::Kind::Master;
        else if (t.kind == IoTarget::Kind::Device)
        {
            m.s = findOut (t);
            if (m.s == nullptr) m.kind = IoTarget::Kind::None;
            else if (! m.s->deviceChannels.empty())
            {
                m.ch = m.s->localChannel (t.channel);
                m.chR = m.s->localChannel (t.channel + 1);
                if (m.ch < 0) m.kind = IoTarget::Kind::None;
            }
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
    for (int b = 0; b < kMaxBuses; ++b)
        if (buses[(size_t) b].sel->isOn() && buses[(size_t) b].active->isOn()) return b;
    return -1;
}

int Engine::getMonitorBus() const noexcept
{
    const int sel = getSelectedBus();
    if (sel < 0 || ! monOn->isOn()) return -1;
    const int mon = juce::jlimit (0, kMaxHwBuses - 1, juce::roundToInt (monBus->get()));
    return mon == sel || ! buses[(size_t) mon].active->isOn() ? -1 : mon;
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
    for (int b = 0; b < kMaxBuses; ++b)
        if (buses[(size_t) b].sel->isOn()) { ++count; if (b != selOwner || newest < 0) newest = b; }
    if (count > 1)
    {
        // Switched by MIDI: the buttons turned off here are part of that MIDI action (no undo step).
        const bool external = buses[(size_t) newest].sel->isExternalChange();
        for (int b = 0; b < kMaxBuses; ++b)
            if (b != newest) buses[(size_t) b].sel->set (0.0f, external);
    }
    selOwner = count > 0 ? newest : -1;
}

void Engine::timerCallback()
{
    enforceExclusiveSelect();
    updateDropoutLogs();
    updateSyncDelays();

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
    if (current != nullptr)
    {
        for (auto& s : current->ins)  s->poll();
        for (auto& s : current->outs) s->poll();
    }
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
        s.delay.prepare (sampleRate);
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
        b.delay.prepare (sampleRate);
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

    const auto elapsed = juce::Time::getHighResolutionTicks() - t0;
    const double secs = (double) elapsed * ticksToSeconds;
    const float load = (float) (secs * sampleRate / juce::jmax (1, n));
    const float prev = cpuLoad.load (std::memory_order_relaxed);
    cpuLoad.store (load > prev ? load : prev + 0.05f * (load - prev), std::memory_order_relaxed);
    if (profile.users.load (std::memory_order_relaxed) > 0)
    {
        auto add = [] (std::atomic<uint64_t>& a, uint64_t v) { a.store (a.load (std::memory_order_relaxed) + v, std::memory_order_relaxed); };
        add (profile.total, (uint64_t) juce::jmax ((juce::int64) 0, elapsed));
        add (profile.frames, (uint64_t) n);
        if (load > profile.peak.load (std::memory_order_relaxed)) profile.peak.store (load, std::memory_order_relaxed);
    }
    callbackCount.fetch_add (1, std::memory_order_release);
}

void Engine::processChunk (const Graph& g, const float* const* in, int numIn, float* const* out, int numOut,
                           int offset, int n) noexcept
{
    const bool timed = profile.users.load (std::memory_order_relaxed) > 0;
    auto now = [] { return juce::Time::getHighResolutionTicks(); };
    auto addTicks = [] (std::atomic<uint64_t>& a, juce::int64 t)
    {
        a.store (a.load (std::memory_order_relaxed) + (uint64_t) juce::jmax ((juce::int64) 0, t), std::memory_order_relaxed);
    };

    // 1. Pull asynchronous inputs (resampled + drift corrected to the master clock).
    for (auto& s : g.ins)
    {
        const auto t0 = timed ? now() : 0;
        s->pullBlock (n);
        if (timed) addTicks (s->dspTicks, now() - t0);
    }

    for (auto& b : buses)
    {
        std::memset (b.l.data(), 0, (size_t) n * sizeof (float));
        std::memset (b.r.data(), 0, (size_t) n * sizeof (float));
    }

    bool anySolo = false;
    for (auto& s : inputs) anySolo |= s.solo->isOn() && s.active->isOn();

    // 2. Input strips -> bus sums.
    for (int i = 0; i < kMaxInputs; ++i)
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
            R = m.mono ? L : m.s->channel (m.chR);
        }

        if (L == nullptr || ! s.active->isOn())
        {
            s.lastL = s.lastR = 0.0f;
            s.lastRoute.fill (0.0f);
            continue;
        }
        processStrip (s, L, R, n, anySolo, L == R, timed);
    }

    // 3. Recorder playback into the chosen buses.
    const auto recStart = timed ? now() : 0;
    if (recorder.pullPlayback (playL.data(), playR.data(), n))
    {
        const float lvl = dbToGain (recorder.level->get());
        for (int b = 0; b < kMaxBuses; ++b)
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
    if (timed) addTicks (profile.recorder, now() - recStart);

    // 4. Bus processing. Monitoring: the selected bus is processed first and replaces the
    //    monitor bus's own mix, so only the monitored signal is heard there (through its EQ / fader).
    const int selBus = getSelectedBus();
    int monBusIndex = -1; // derived from the same SEL snapshot (SEL may change concurrently)
    if (selBus >= 0 && monOn->isOn())
        if (const int m = juce::jlimit (0, kMaxHwBuses - 1, juce::roundToInt (monBus->get()));
            m != selBus && buses[(size_t) m].active->isOn())
            monBusIndex = m;
    if (monBusIndex >= 0) processBus (buses[(size_t) selBus], n, timed);
    for (int b = 0; b < kMaxBuses; ++b)
    {
        if (monBusIndex >= 0 && b == selBus) continue;
        auto& bus = buses[(size_t) b];
        if (! bus.active->isOn()) continue; // removed bus: nothing is routed to it
        if (b == monBusIndex)
        {
            std::memcpy (bus.l.data(), buses[(size_t) selBus].l.data(), (size_t) n * sizeof (float));
            std::memcpy (bus.r.data(), buses[(size_t) selBus].r.data(), (size_t) n * sizeof (float));
        }
        processBus (bus, n, timed);
    }

    if (recorder.wantsRecord())
    {
        const auto t0 = timed ? now() : 0;
        const auto& rb = buses[(size_t) recorder.getRecordSourceBus()];
        recorder.pushRecord (rb.l.data(), rb.r.data(), n);
        if (timed) addTicks (profile.recorder, now() - t0);
    }

    // 5. Buses -> outputs.
    for (int c = 0; c < numOut; ++c)
        if (out[c] != nullptr) std::memset (out[c] + offset, 0, (size_t) n * sizeof (float));
    for (auto& s : g.outs) s->clearMix (n);

    for (int b = 0; b < kMaxBuses; ++b)
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
            if (! m.mono && m.chR >= 0 && m.chR < m.s->getNumChannels()) oR = m.s->mixChannel (m.chR);
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
    for (auto& s : g.outs)
    {
        const auto t0 = timed ? now() : 0;
        s->pushBlock (n);
        if (timed) addTicks (s->dspTicks, now() - t0);
    }
}

void Engine::processStrip (InputStrip& s, const float* srcL, const float* srcR, int n, bool anySolo, bool monoSource, bool timed) noexcept
{
    // Profiling (DSP view open): each lap adds the time since the previous lap to a stage.
    auto mark = timed ? juce::Time::getHighResolutionTicks() : 0;
    auto lap = [&] (DspTimes::Stage stage)
    {
        if (! timed) return;
        const auto t = juce::Time::getHighResolutionTicks();
        s.dspTimes.add (stage, t - mark);
        mark = t;
    };
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

    lap (DspTimes::other);

    // Denoiser first, so EQ / gate / compressor work on the cleaned signal.
    const bool dn = s.dnOn->isOn();
    if (dn)
    {
        if (! s.dnWasOn) s.denoiser->reset();
        s.denoiser->process (l, r, n, s.denoiserSettings(), ! monoSource);
    }
    s.dnWasOn = dn;
    s.dspLatency.store (dn ? s.denoiser->getLatencySamples() : 0, std::memory_order_relaxed);
    lap (DspTimes::denoiser);

    s.eq.process (l, r, n, sampleRate);
    s.spectrum.push (l, r, n);
    lap (DspTimes::eq);

    if (s.gateOn->isOn())
        s.gate.process (l, r, n, s.gateSettings());
    else
        s.gate.shownGain.store (1.0f, std::memory_order_relaxed);
    lap (DspTimes::gate);

    if (s.compOn->isOn())
    {
        s.comp.process (l, r, n, s.compSettings());
        s.compGr.store (s.comp.lastGr, std::memory_order_relaxed);
    }
    else
        s.compGr.store (0.0f, std::memory_order_relaxed);
    lap (DspTimes::comp);

    if (! s.fxBypass->isOn())
        s.plugins.process (l, r, n, timed);
    lap (DspTimes::plugins);

    // Sync Delay (aligns this input with slower ones); nothing to do while it is 0.
    s.delay.process (l, r, n, delaySamples (s.index));

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
        lap (DspTimes::other);
        return;
    }

    for (int b = 0; b < kMaxBuses; ++b)
    {
        const float target = s.route[(size_t) b]->isOn() ? dbToGain (s.send[(size_t) b]->get()) : 0.0f;
        const float prev = s.lastRoute[(size_t) b];
        if (target == 0.0f && prev == 0.0f) continue;
        auto& bus = buses[(size_t) b];
        if (! bus.active->isOn()) { s.lastRoute[(size_t) b] = 0.0f; continue; }
        dsp::addRamp (bus.l.data(), l, n, prev, target);
        dsp::addRamp (bus.r.data(), r, n, prev, target);
        s.lastRoute[(size_t) b] = target;
    }
    lap (DspTimes::other);
}

void Engine::processBus (Bus& b, int n, bool timed) noexcept
{
    auto mark = timed ? juce::Time::getHighResolutionTicks() : 0;
    auto lap = [&] (DspTimes::Stage stage)
    {
        if (! timed) return;
        const auto t = juce::Time::getHighResolutionTicks();
        b.dspTimes.add (stage, t - mark);
        mark = t;
    };
    float* l = b.l.data();
    float* r = b.r.data();

    b.eq.process (l, r, n, sampleRate);
    b.spectrum.push (l, r, n);
    lap (DspTimes::eq);

    if (! b.fxBypass->isOn())
        b.plugins.process (l, r, n, timed);
    lap (DspTimes::plugins);

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

    lap (DspTimes::other);

    if (b.limOn->isOn())
        b.limiter.process (l, r, n, b.limCeil->get());
    lap (DspTimes::limiter);

    b.delay.process (l, r, n, delaySamples (busChannel (b.index)));

    if (! muted)
        b.meter.push (dsp::peak (l, n), dsp::peak (r, n));
    lap (DspTimes::other);
}
} // namespace hydra

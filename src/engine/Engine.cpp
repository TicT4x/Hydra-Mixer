#include "Engine.h"
#include "VacStream.h"
#include "state/VacControl.h"

#include <algorithm>
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

/** Level of one stem: the range of a channel fader (-60 dB = off, up to +12 dB), so its fader has the same scale. */
juce::NormalisableRange<float> stemGainRange() { return faderRange(); }

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
dsp::GateSettings GateFx::settings (float voiceProbability) const
{
    dsp::GateSettings g;
    g.thresholdDb = thr->get();
    g.rangeDb = range->get();
    g.attackMs = attack->get();
    g.holdMs = hold->get();
    g.releaseMs = release->get();
    g.sidechain = scOn->isOn();
    g.sidechainHz = scFreq->get();
    g.voiceKey = voice->isOn();
    g.voiceThreshold = voiceThr->get() * 0.01f;
    g.voice = voiceProbability;
    return g;
}

dsp::CompSettings CompFx::settings() const
{
    dsp::CompSettings c;
    c.inputDb = input->get();
    c.thresholdDb = thr->get();
    c.ratio = ratio->get();
    c.kneeDb = knee->get();
    c.attackMs = attack->get();
    c.releaseMs = release->get();
    c.makeupDb = makeup->get();
    c.autoMakeup = autoGain->isOn();
    return c;
}

void StemsFx::gains (float* out) const noexcept
{
    const bool guitar = stems::hasGuitar (juce::roundToInt (mode->get()));
    auto idx = [guitar] (int k) { return ! guitar && k == stems::guitar ? (int) stems::rest : k; };   // Live: guitar = rest
    bool anySolo = false;
    for (int k = 0; k < stems::numStems; ++k) anySolo = anySolo || (solo[(size_t) idx (k)]->isOn());
    for (int k = 0; k < stems::numStems; ++k)
    {
        const auto i = (size_t) idx (k);
        const bool audible = anySolo ? solo[i]->isOn() : ! mute[i]->isOn();   // a soloed stem plays even if muted
        out[k] = audible ? dbToGain (gain[i]->get()) : 0.0f;
    }
}

dsp::DenoiserSettings DenoiserFx::settings() const
{
    dsp::DenoiserSettings d;
    d.mode = juce::roundToInt (mode->get());
    d.amount = amount->get() * 0.01f;
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
    opener = std::make_unique<StreamOpener> (types);

    for (int i = 0; i < kMaxInputs; ++i)
    {
        inputs[(size_t) i].index = i;
        prepareDenoiser (inputs[(size_t) i].denoisers[0]);   // the first one always exists (UI shows its state)
        inputs[(size_t) i].name = "Input " + juce::String (i + 1);
        inputs[(size_t) i].spectrum.ring.allocate (2, 16384);
        inputs[(size_t) i].l.assign (kMaxBlock, 0.0f);
        inputs[(size_t) i].r.assign (kMaxBlock, 0.0f);
    }
    for (int b = 0; b < kMaxBuses; ++b)
    {
        buses[(size_t) b].index = b;
        buses[(size_t) b].name = busLabel (b);
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
    opener.reset();   // finishes closing what it was given (it uses the device types)
    for (auto& s : inputs) { s.plugins.clear(); releaseStems (s); }
    stemsGraveyard.clear();
    for (auto& b : buses)  { b.plugins.clear(); }
}

static void createCompParams (ParamRegistry& params, CompFx& c, const juce::String& idPrefix, const juce::String& namePrefix)
{
    c.on       = &params.addToggle (idPrefix + "on", namePrefix + "On");
    c.input    = &params.add (idPrefix + "input", namePrefix + "Input", ParamType::Continuous, linRange (-24, 24, 0.1f), 0.0f, " dB");
    c.thr      = &params.add (idPrefix + "thr", namePrefix + "Threshold", ParamType::Continuous, linRange (-60, 0, 0.1f), -20.0f, " dB");
    c.ratio    = &params.add (idPrefix + "ratio", namePrefix + "Ratio", ParamType::Continuous, logRange (1.0f, 20.0f), 3.0f, ":1");
    c.knee     = &params.add (idPrefix + "knee", namePrefix + "Knee", ParamType::Continuous, linRange (0, 24, 0.1f), 6.0f, " dB");
    c.attack   = &params.add (idPrefix + "attack", namePrefix + "Attack", ParamType::Continuous, logRange (0.1f, 200.0f), 5.0f, " ms");
    c.release  = &params.add (idPrefix + "release", namePrefix + "Release", ParamType::Continuous, logRange (5.0f, 3000.0f), 150.0f, " ms");
    c.makeup   = &params.add (idPrefix + "makeup", namePrefix + "Makeup", ParamType::Continuous, linRange (-12, 24, 0.1f), 0.0f, " dB");
    c.autoGain = &params.addToggle (idPrefix + "auto", namePrefix + "Auto Makeup");
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
        for (int k = 0; k < kFxInstances; ++k)
        {
            // Instance k: ids "in1.gate.*" (first), "in1.gate2.*" ...; names "In 1 Gate ...", "In 1 Gate 2 ...".
            const auto num = k == 0 ? juce::String() : juce::String (k + 1);
            const auto lbl = [&] (const char* what) { return n + what + (k == 0 ? juce::String() : " " + num) + " "; };
            s.phases[(size_t) k] = &params.addToggle (p + fxPart (Fx::phase, k), lbl ("Phase").trimEnd() + " Invert");
            s.eqs[(size_t) k].create (params, p, n, fxPart (Fx::eq, k), "EQ" + (k == 0 ? juce::String() : " " + num));
            auto& g = s.gates[(size_t) k];
            const auto gp = p + fxPart (Fx::gate, k) + ".";
            g.on       = &params.addToggle (gp + "on", lbl ("Gate") + "On");
            g.thr      = &params.add (gp + "thr", lbl ("Gate") + "Threshold", ParamType::Continuous, linRange (-80, 0, 0.1f), -50.0f, " dB");
            g.range    = &params.add (gp + "range", lbl ("Gate") + "Range", ParamType::Continuous, linRange (-80, 0, 0.1f), -80.0f, " dB");
            g.attack   = &params.add (gp + "attack", lbl ("Gate") + "Attack", ParamType::Continuous, logRange (0.1f, 100.0f), 1.0f, " ms");
            g.hold     = &params.add (gp + "hold", lbl ("Gate") + "Hold", ParamType::Continuous, linRange (0, 1000, 1.0f), 80.0f, " ms");
            g.release  = &params.add (gp + "release", lbl ("Gate") + "Release", ParamType::Continuous, logRange (5.0f, 2000.0f), 120.0f, " ms");
            g.scOn     = &params.addToggle (gp + "sc.on", lbl ("Gate") + "Sidechain Filter");
            g.scFreq   = &params.add (gp + "sc.freq", lbl ("Gate") + "Sidechain Freq", ParamType::Continuous, logRange (50.0f, 8000.0f), 1000.0f, " Hz");
            g.voice    = &params.addToggle (gp + "voice", lbl ("Gate") + "Voice Key");
            g.voiceThr = &params.add (gp + "voice.thr", lbl ("Gate") + "Voice Threshold", ParamType::Continuous, linRange (0, 100, 1.0f), 50.0f, " %");
            createCompParams (params, s.comps[(size_t) k], p + fxPart (Fx::comp, k) + ".", lbl ("Comp"));
            auto& d = s.denoisers[(size_t) k];
            const auto dp = p + fxPart (Fx::denoiser, k) + ".";
            d.on     = &params.addToggle (dp + "on", lbl ("Denoiser") + "On");
            d.amount = &params.add (dp + "amount", lbl ("Denoiser") + "Amount", ParamType::Continuous, linRange (0, 100, 1.0f), 100.0f, " %");
            d.mode   = &params.addChoice (dp + "mode", lbl ("Denoiser") + "Latency Mode", dsp::Denoiser::modeNames(), dsp::Denoiser::Low);
            auto& lim = s.limiters[(size_t) k];
            const auto lp = p + fxPart (Fx::limiter, k) + ".";
            lim.on      = &params.addToggle (lp + "on", lbl ("Limiter") + "On");
            lim.ceiling = &params.add (lp + "ceiling", lbl ("Limiter") + "Ceiling", ParamType::Continuous, linRange (-30, 0, 0.1f), 0.0f, " dB");
        }
        {
            // Stems: ids "in1.stems.on", "in1.stems.mode", "in1.stems.vocals" (gain), "in1.stems.vocals.mute" ...
            auto& st = s.stems;
            const auto sp = p + fxPart (Fx::stems, 0) + ".";
            st.on   = &params.addToggle (sp + "on", n + "Stem Separator On");
            st.mode = &params.addChoice (sp + "mode", n + "Stem Separator Latency Mode", stems::modeNames(), stems::kDefaultMode);
            for (int k = 0; k < stems::numStems; ++k)
            {
                const auto name = stems::stemName (k);
                st.gain[(size_t) k] = &params.add (sp + name.toLowerCase(), n + "Stem Separator " + name, ParamType::Continuous,
                                                   stemGainRange(), 0.0f, " dB");
                st.mute[(size_t) k] = &params.addToggle (sp + name.toLowerCase() + ".mute", n + "Stem Separator " + name + " Mute");
                st.solo[(size_t) k] = &params.addToggle (sp + name.toLowerCase() + ".solo", n + "Stem Separator " + name + " Solo");
            }
        }
        s.fxBypass   = &params.addToggle (p + "fx.bypass", n + "FX Bypass");
        s.syncOn     = &params.addToggle (p + "sync", n + "Sync Delay Auto");
        s.delayMs    = &params.add (p + "delay", n + "Sync Delay", ParamType::Continuous,
                                    linRange (0, (float) dsp::DelayLine::kMaxMs, 0.1f), 0.0f, " ms");
        for (int b = 0; b < kMaxBuses; ++b)
            s.route[(size_t) b] = &params.addToggle (p + "route." + busCode (b), n + "-> " + busLabel (b), b == 0);
        for (int b = 0; b < kMaxBuses; ++b)
            s.send[(size_t) b] = &params.add (p + "send." + busCode (b), n + "Level -> " + busLabel (b), ParamType::Continuous, faderRange(), 0.0f, " dB");
    }

    for (int b = 0; b < kMaxBuses; ++b)
    {
        auto& bus = buses[(size_t) b];
        const auto p = "bus." + busCode (b) + ".";
        const auto n = "Bus " + busLabel (b) + " ";

        bus.active   = &params.addToggle (p + "active", n + "In Use",
                                          isHwBus (b) ? b < hwBusIndex (kDefaultHwBuses) : b < virtBusIndex (kDefaultVirtBuses));
        bus.gain     = &params.add (p + "gain", n + "Fader", ParamType::Continuous, faderRange(), 0.0f, " dB");
        bus.mute     = &params.addToggle (p + "mute", n + "Mute");
        bus.mono     = &params.addToggle (p + "mono", n + "Mono");
        if (isHwBus (b)) bus.correction.on = &params.addToggle (p + "corr.on", n + "Correction On");
        for (int k = 0; k < kFxInstances; ++k)
        {
            const auto num = k == 0 ? juce::String() : " " + juce::String (k + 1);
            bus.eqs[(size_t) k].create (params, p, n, fxPart (Fx::eq, k), "EQ" + num);
            auto& l = bus.limiters[(size_t) k];
            // Off by default: since the LIM button is gone (9.11.) a bus gets a limiter only from the effect menu.
            // Sessions before version 3 had the first one on by default (Session::load keeps it on there).
            l.on      = &params.addToggle (p + fxPart (Fx::limiter, k) + ".on", n + "Limiter" + num + " On", false);
            l.ceiling = &params.add (p + fxPart (Fx::limiter, k) + ".ceiling", n + "Limiter" + num + " Ceiling", ParamType::Continuous,
                                     linRange (-30, 0, 0.1f), 0.0f, " dB");
            bus.phases[(size_t) k] = &params.addToggle (p + fxPart (Fx::phase, k), n + "Phase" + num + " Invert");
            createCompParams (params, bus.comps[(size_t) k], p + fxPart (Fx::comp, k) + ".", n + "Comp" + num + " ");
        }
        bus.fxBypass = &params.addToggle (p + "fx.bypass", n + "FX Bypass");
        bus.sel      = &params.addToggle (p + "sel", n + "Select (SEL)");
        bus.syncOn   = &params.addToggle (p + "sync", n + "Sync Delay Auto");
        bus.delayMs  = &params.add (p + "delay", n + "Sync Delay", ParamType::Continuous,
                                    linRange (0, (float) dsp::DelayLine::kMaxMs, 0.1f), 0.0f, " ms");
    }

    for (int m = 0; m < stems::kNumModels; ++m)
    {
        const juce::String id = m == (int) stems::Model::roformer ? "roformer" : "demucs";
        const juce::String name = m == (int) stems::Model::roformer ? "Stem Separator BS-RoFormer GPU" : "Stem Separator HT-Demucs GPU";
        stemsAuto[(size_t) m] = &params.addToggle ("stems." + id + ".auto", name + " Auto", true);
        stemsGpu[(size_t) m]  = &params.add ("stems." + id + ".gpu", name + " Share", ParamType::Continuous, linRange (10, 100, 1.0f), 50.0f, " %");
    }

    monOn  = &params.addToggle ("mon.on", "Monitor Selected Bus");
    juce::StringArray hwCodes;
    for (int a = 0; a < kMaxHwBuses; ++a) hwCodes.add (busLabel (hwBusIndex (a)));
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
            if (i == measureIn) releaseMeasureInput();   // a new channel starts without the measurement's device
            inputs[(size_t) i].source = {};
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
            if (b == measureBus) releaseMeasureBus();
            buses[(size_t) b].output = {};
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
    bus.name = busLabel (b);
    bus.icon = {};
    bus.output = {};
    bus.plugins.clear();
    setCorrection (kMaxInputs + b, {});
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
        std::array<juce::String, kMaxBuses> names, icons;
        std::array<IoTarget, kMaxBuses> outputs;
        std::array<std::vector<std::unique_ptr<PluginSlot>>, kMaxBuses> chains;
        std::array<CorrectionProfile, kMaxBuses> corrections;
        for (int b : used)
        {
            names[(size_t) b] = buses[(size_t) b].name;
            icons[(size_t) b] = buses[(size_t) b].icon;
            outputs[(size_t) b] = buses[(size_t) b].output;
            chains[(size_t) b] = buses[(size_t) b].plugins.takeAll();
            corrections[(size_t) b] = buses[(size_t) b].correction.getProfile();
        }
        for (int b : used)
        {
            auto& to = buses[(size_t) dest[(size_t) b]];
            to.name = names[(size_t) b];
            to.icon = icons[(size_t) b];
            to.output = outputs[(size_t) b];
            to.plugins.putAll (std::move (chains[(size_t) b]));
            setCorrection (kMaxInputs + dest[(size_t) b], corrections[(size_t) b]);
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
        l.transport = s.transport;
        l.steeredClock = s.isDirect();
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
    if (found == nullptr && opener->isPending (t, input) && openErrors.count ((input ? "in " : "out ") + t.typeName + "|" + t.deviceName) == 0)
        return h;   // being opened right now
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
        auto keep = [&] (const AsyncStream& s, bool input)
        {
            auto& log = next[&s];
            if (auto it = dropoutLogs.find (&s); it != dropoutLogs.end()) log = std::move (it->second);
            else log.seen = s.underruns.load();   // a new stream: count only what happens from now on
            const int before = log.seen;
            track (log, s.underruns.load());
            // hydra.log: one line per stream at most every 5 s (the rest is summed up in the next line).
            log.unlogged += juce::jmax (0, log.seen - before);
            if (log.unlogged > 0 && nowMs - log.lastLogged >= 5000)
            {
                const double ms = s.latencyFrames.load() * 1000.0 / juce::jmax (1.0, s.getDeviceRate());
                logLine ("dropout: " + juce::String (input ? "input " : "output ") + s.target.describe()
                         + (s.transport.isNotEmpty() ? " [" + s.transport + "]" : juce::String())
                         + ": " + juce::String (log.unlogged) + " (total " + juce::String (log.seen) + "), buffered "
                         + juce::String (ms, 1) + " ms");
                log.unlogged = 0;
                log.lastLogged = nowMs;
            }
        };
        for (auto& s : current->ins)  keep (*s, true);
        for (auto& s : current->outs) keep (*s, false);
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
                    ms = dev + (s.denoiserActive() ? 1000.0 * s.dspLatency.load() / juce::jmax (1.0, sampleRate) : 0.0)
                       + 1000.0 * s.stemsLatency.load() / juce::jmax (1.0, sampleRate)
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
std::vector<Fx> Engine::allowedFx (int ch) const
{
    // In their usual order (where ensureBuiltins inserts them).
    if (ch < kMaxInputs) return { Fx::stems, Fx::phase, Fx::denoiser, Fx::eq, Fx::gate, Fx::comp, Fx::limiter };
    if (isHwBus (ch - kMaxInputs)) return { Fx::correction, Fx::phase, Fx::eq, Fx::comp, Fx::limiter };
    return { Fx::phase, Fx::eq, Fx::comp, Fx::limiter };
}

juce::String Engine::fxPart (Fx f, int k)
{
    const juce::String base = f == Fx::denoiser ? "dn" : f == Fx::eq ? "eq" : f == Fx::gate ? "gate" : f == Fx::comp ? "comp"
                            : f == Fx::limiter ? "lim" : f == Fx::phase ? "phase" : f == Fx::stems ? "stems" : "corr";
    return k == 0 ? base : base + juce::String (k + 1);
}

Param* Engine::fxOnParam (int ch, Fx f, int k) const
{
    if (! juce::isPositiveAndBelow (k, maxInstances (f))) return nullptr;
    const auto i = (size_t) k;
    if (ch < kMaxInputs)
    {
        const auto& s = inputs[(size_t) ch];
        switch (f)
        {
            case Fx::denoiser: return s.denoisers[i].on;
            case Fx::eq:       return s.eqs[i].on;
            case Fx::gate:     return s.gates[i].on;
            case Fx::comp:     return s.comps[i].on;
            case Fx::phase:    return s.phases[i];
            case Fx::limiter:  return s.limiters[i].on;
            case Fx::stems:    return s.stems.on;
            default:           return nullptr;
        }
    }
    const auto& b = buses[(size_t) (ch - kMaxInputs)];
    switch (f)
    {
        case Fx::eq:         return b.eqs[i].on;
        case Fx::limiter:    return b.limiters[i].on;
        case Fx::comp:       return b.comps[i].on;
        case Fx::phase:      return b.phases[i];
        case Fx::correction: return b.correction.on;
        default:             return nullptr;
    }
}

std::vector<Param*> Engine::fxParams (int ch, Fx f, int k) const
{
    std::vector<Param*> out;
    auto* on = fxOnParam (ch, f, k);
    if (on == nullptr) return out;
    if (f == Fx::phase) return { on };
    const auto prefix = (ch < kMaxInputs ? "in" + juce::String (ch + 1) + "." : "bus." + busCode (ch - kMaxInputs) + ".") + fxPart (f, k) + ".";
    for (auto& p : params.all())
        if (p->id.startsWith (prefix)) out.push_back (p.get());
    return out;
}

namespace
{
/** Where a built-in effect goes when it is added without a position: right after the last one of the same kind,
    else after the last built-in effect that usually comes before it, otherwise at the start (a limiter: the end). */
int usualIndex (const PluginChain& chain, Fx f, const std::vector<Fx>& order)
{
    int same = -1;
    for (int i = 0; i < chain.size(); ++i)
        if (auto* s = chain.getSlot (i); s != nullptr && s->builtin == f) same = i;
    if (same >= 0) return same + 1;
    if (f == Fx::limiter) return -1;
    const auto rank = [&order] (Fx x) { return (int) (std::find (order.begin(), order.end(), x) - order.begin()); };
    int index = 0;
    for (int i = 0; i < chain.size(); ++i)
        if (auto* s = chain.getSlot (i); s != nullptr && ! s->isPlugin() && s->builtin != Fx::limiter && rank (s->builtin) < rank (f))
            index = i + 1;
    return index;
}
} // namespace

int Engine::addBuiltin (int ch, Fx f, int index)
{
    auto& chain = chainFor (ch);
    for (int k = 0; k < maxInstances (f); ++k)
    {
        auto* on = fxOnParam (ch, f, k);
        if (on == nullptr) return -1;
        if (chain.hasBuiltin (f, k)) continue;
        if (f == Fx::denoiser) prepareDenoiser (inputs[(size_t) ch].denoisers[(size_t) k]);
        for (auto* p : fxParams (ch, f, k)) p->reset();   // a fresh instance
        if (f == Fx::stems) prepareStems (inputs[(size_t) ch]);
        chain.addBuiltin (f, k, index >= 0 ? index : usualIndex (chain, f, allowedFx (ch)));
        on->set (1.0f);
        return k;
    }
    return -1;
}

void Engine::removeBuiltin (int ch, Fx f, int k)
{
    auto& chain = chainFor (ch);
    if (const int i = chain.indexOf (f, k); i >= 0) chain.remove (i);
    for (auto* p : fxParams (ch, f, k)) p->reset();
    if (auto* on = fxOnParam (ch, f, k)) on->set (0.0f);   // e.g. the first limiter is on by default
    if (f == Fx::correction) setCorrection (ch, {});
    if (f == Fx::stems && ch < kMaxInputs) releaseStems (inputs[(size_t) ch]);
}

void Engine::prepareStems (InputStrip& s)
{
    auto& st = s.stems;
    if (st.inst != nullptr) return;
    auto inst = std::make_unique<stems::Instance>();
    inst->prepare (sampleRate);
    stems::Service::get().add (*inst);
    st.inst = std::move (inst);
    st.live.store (st.inst.get(), std::memory_order_release);
}

void Engine::releaseStems (InputStrip& s)
{
    auto& st = s.stems;
    if (st.inst == nullptr) return;
    st.live.store (nullptr, std::memory_order_release);
    stems::Service::get().remove (*st.inst);
    s.stemsLatency.store (0);
    // ~20 MB of buffers: freed once the audio thread cannot be reading them any more (timerCallback).
    stemsGraveyard.emplace_back (std::move (st.inst), callbackCount.load() + 2);
}

void Engine::prepareDenoiser (DenoiserFx& d)
{
    // Further denoisers are created when they are first used (RNNoise state is large); never on the audio thread.
    if (d.dsp != nullptr) return;
    auto dn = std::make_unique<dsp::Denoiser>();
    dn->prepare (sampleRate);
    d.dsp = std::move (dn);
    d.live.store (d.dsp.get(), std::memory_order_release);
}

void Engine::ensureBuiltins()
{
    for (int ch = 0; ch < kMaxInputs + kMaxBuses; ++ch)
    {
        if (ch < kMaxInputs ? ! isInputActive (ch) : ! isBusActive (ch - kMaxInputs)) continue;
        auto& chain = chainFor (ch);
        const auto allowed = allowedFx (ch);
        for (int i = chain.size(); --i >= 0;)   // e.g. a gate pasted onto a bus
            if (auto* s = chain.getSlot (i); s != nullptr && ! s->isPlugin()
                && (std::find (allowed.begin(), allowed.end(), s->builtin) == allowed.end() || s->fxInstance >= maxInstances (s->builtin)))
                chain.remove (i);
        for (auto f : allowed)
            for (int k = 0; k < maxInstances (f); ++k)
                if (auto* on = fxOnParam (ch, f, k); on != nullptr && on->isOn() && ! chain.hasBuiltin (f, k))
                {
                    if (f == Fx::denoiser) prepareDenoiser (inputs[(size_t) ch].denoisers[(size_t) k]);
                    chain.addBuiltin (f, k, usualIndex (chain, f, allowed));
                }
        if (ch < kMaxInputs)   // denoisers / stems loaded from a session (or a chain pasted / removed)
        {
            auto& s = inputs[(size_t) ch];
            for (int k = 0; k < kFxInstances; ++k)
                if (chain.hasBuiltin (Fx::denoiser, k)) prepareDenoiser (s.denoisers[(size_t) k]);
            if (chain.hasBuiltin (Fx::stems)) prepareStems (s);
            else                              releaseStems (s);
        }
    }
    for (int i = 0; i < kMaxInputs; ++i)   // a removed input slot keeps no stems state
        if (! isInputActive (i)) releaseStems (inputs[(size_t) i]);
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
    std::vector<IoTarget> wantedIn, wantedOut;   // streams that are not open yet
    auto openError = [this, &errors] (const IoTarget& t, bool input)
    {
        if (auto it = openErrors.find ((input ? "in " : "out ") + t.typeName + "|" + t.deviceName); it != openErrors.end())
            errors.add (t.describe() + ": " + it->second);
    };

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
    for (int i = 0; i < kMaxInputs; ++i)
        if (const auto& s = inputs[(size_t) i]; (s.active->isOn() || i == measureIn) && isSecondaryAsio (s.source)) addChannels (asioIns[s.source.deviceName], s.source);
    for (int b = 0; b < kMaxBuses; ++b)
        if (const auto& bus = buses[(size_t) b]; (bus.active->isOn() || b == measureBus) && isSecondaryAsio (bus.output)) addChannels (asioOuts[bus.output.deviceName], bus.output);
    auto channelsFor = [&] (const IoTarget& t, std::map<juce::String, std::vector<int>>& m)
    {
        return isSecondaryAsio (t) ? m[t.deviceName] : std::vector<int>();
    };

    auto findIn = [&] (const IoTarget& t) -> AsyncInput*
    {
        if (holdDevice && holdDevice (t)) return nullptr;   // released for a moment (driver restart)
        const auto chans = channelsFor (t, asioIns);
        for (auto& s : g->ins) if (s->target.sameStream (t) && s->carries (chans)) return s.get();
        if (current != nullptr)
            for (auto& s : current->ins)
                if (s->target.sameStream (t) && s->carries (chans) && ! s->failed.load()) { g->ins.push_back (s); return s.get(); }
        juce::String err;
        if (isSecondaryAsio (t))
        {
            if (auto s = openAsyncInput (t, types, sampleRate, blockSize, err, chans)) { s->start(); g->ins.push_back (s); return s.get(); }
            errors.add (t.describe() + ": " + err);
            return nullptr;
        }
        // Windows streams open in the background: the graph is built again when this one is ready.
        wantedIn.push_back (t);
        std::shared_ptr<AsyncInput> s;
        const juce::String key = "in " + t.typeName + "|" + t.deviceName;
        if (! opener->takeInput (t, sampleRate, blockSize, s, err)) requestOpen (t, true);
        else if (s != nullptr) { openErrors.erase (key); s->start(); g->ins.push_back (s); return s.get(); }
        else openErrors[key] = err;
        openError (t, true);
        return nullptr;
    };

    auto findOut = [&] (const IoTarget& t) -> AsyncOutput*
    {
        if (holdDevice && holdDevice (t)) return nullptr;
        const auto chans = channelsFor (t, asioOuts);
        for (auto& s : g->outs) if (s->target.sameStream (t) && s->carries (chans)) return s.get();
        if (current != nullptr)
            for (auto& s : current->outs)
                if (s->target.sameStream (t) && s->carries (chans) && ! s->failed.load()) { g->outs.push_back (s); return s.get(); }
        juce::String err;
        if (isSecondaryAsio (t))
        {
            if (auto s = openAsyncOutput (t, types, sampleRate, blockSize, err, chans)) { s->start(); g->outs.push_back (s); return s.get(); }
            errors.add (t.describe() + ": " + err);
            return nullptr;
        }
        wantedOut.push_back (t);
        std::shared_ptr<AsyncOutput> s;
        const juce::String key = "out " + t.typeName + "|" + t.deviceName;
        if (! opener->takeOutput (t, sampleRate, blockSize, s, err)) requestOpen (t, false);
        else if (s != nullptr) { openErrors.erase (key); s->start(); g->outs.push_back (s); return s.get(); }
        else openErrors[key] = err;
        openError (t, false);
        return nullptr;
    };

    anyMissing = false;
    for (int i = 0; i < kMaxInputs; ++i)
    {
        const auto& t = inputs[(size_t) i].source;
        auto& m = g->in[(size_t) i];
        m.ch = t.channel; m.chR = t.channel + 1; m.mono = t.mono; m.kind = t.kind;
        if (! inputs[(size_t) i].active->isOn() && i != measureIn)
            m.kind = IoTarget::Kind::None; // removed channel: never opens its device (unless borrowed for a measurement)
        else if (isMasterAsio (t))
            m.kind = IoTarget::Kind::Master; // the master's own driver: use its channels directly
        else if (t.kind == IoTarget::Kind::Device || t.kind == IoTarget::Kind::Loopback || t.kind == IoTarget::Kind::App || t.kind == IoTarget::Kind::Network)
        {
            m.s = findIn (t);
            if (m.s == nullptr) { m.kind = IoTarget::Kind::None; anyMissing = true; }
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
        if (! buses[(size_t) b].active->isOn() && b != measureBus)
            m.kind = IoTarget::Kind::None;
        else if (isMasterAsio (t))
            m.kind = IoTarget::Kind::Master;
        else if (t.kind == IoTarget::Kind::Device || t.kind == IoTarget::Kind::Network)
        {
            m.s = findOut (t);
            if (m.s == nullptr) { m.kind = IoTarget::Kind::None; anyMissing = true; }
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
    opener->discardFinishedExcept (wantedIn, wantedOut, sampleRate, blockSize);
    lastError = errors.joinIntoString ("\n");
    sendChangeMessage();
}

void Engine::requestOpen (const IoTarget& t, bool input)
{
    if (t.kind == IoTarget::Kind::Device)
    {
        const juce::String key = (input ? "in " : "out ") + t.typeName + "|" + t.deviceName;
        auto* type = findType (t.typeName);
        if (type == nullptr) { openErrors[key] = "Unknown driver type " + t.typeName; return; }
        // A Virtual Audio Cable side hidden in Windows is not in the device lists, but opens over Kernel Streaming.
        const bool directCable = isVacDirectEnabled() && vac::isCableEndpoint (t.deviceName);
        if (! directCable && ! type->getDeviceNames (input).contains (t.deviceName))
        {
            type->scanForDevices();   // plugged in after the last scan? (only reads the names, no device is opened)
            if (! type->getDeviceNames (input).contains (t.deviceName)) { openErrors[key] = "Cannot find device " + t.deviceName; return; }
        }
    }
    opener->request (t, input, sampleRate, blockSize);
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

//==============================================================================
void Engine::setCorrection (int selectIndex, CorrectionProfile profile)
{
    if (auto* c = correctionFor (selectIndex)) c->setProfile (std::move (profile), sampleRate, callbackCount.load());
}

//==============================================================================
void Engine::startMeasurement (std::unique_ptr<MeasureRun> run)
{
    finishMeasurement();
    run->length = (int) juce::jmin (run->stimulus.size(), run->capture.size());
    measureOwned = std::move (run);
    measureLive.store (measureOwned.get(), std::memory_order_release);
}

void Engine::stopMeasurement()
{
    if (measureOwned != nullptr) measureOwned->stop.store (true);
}

std::vector<float> Engine::finishMeasurement()
{
    std::vector<float> rec;
    if (measureOwned == nullptr) return rec;
    measureLive.store (nullptr, std::memory_order_release);
    // A finished run is no longer touched by the audio thread (it only reads pos / length).
    if (measureOwned->finished() && ! measureOwned->stop.load())
        rec = std::move (measureOwned->capture);
    measureGraveyard.emplace_back (std::move (measureOwned), callbackCount.load() + 2);
    if (! running) measureGraveyard.clear();
    return rec;
}

int Engine::borrowMeasureInput()
{
    if (measureIn >= 0) return measureIn;
    for (int i = kMaxInputs - 1; i >= 0; --i)   // from the top: addInput takes the lowest free slot
        if (! inputs[(size_t) i].active->isOn())
        {
            measureIn = i;
            inputs[(size_t) i].source = {};
            return i;
        }
    return -1;
}

int Engine::borrowMeasureBus()
{
    if (measureBus >= 0) return measureBus;
    for (int b = kMaxBuses - 1; b >= 0; --b)
        if (! buses[(size_t) b].active->isOn())
        {
            measureBus = b;
            buses[(size_t) b].output = {};
            return b;
        }
    return -1;
}

void Engine::releaseMeasureInput()
{
    if (measureIn < 0) return;
    const int i = std::exchange (measureIn, -1);
    if (! inputs[(size_t) i].active->isOn()) inputs[(size_t) i].source = {};
    rebuildGraph();
}

void Engine::releaseMeasureBus()
{
    if (measureBus < 0) return;
    const int b = std::exchange (measureBus, -1);
    if (! buses[(size_t) b].active->isOn()) buses[(size_t) b].output = {};
    rebuildGraph();
}

void Engine::renderMeasurement (Bus& b, MeasureRun& run, int start, int count, int n) noexcept
{
    float* l = b.l.data();
    float* r = b.r.data();
    std::memset (l, 0, (size_t) n * sizeof (float));
    std::memset (r, 0, (size_t) n * sizeof (float));
    const float* src = run.stimulus.data() + start;
    if (run.outChannels & 1) std::memcpy (l, src, (size_t) count * sizeof (float));
    if (run.outChannels & 2) std::memcpy (r, src, (size_t) count * sizeof (float));
    if (run.stop.load (std::memory_order_relaxed))   // stopped: fade out within this chunk
    {
        dsp::applyRamp (l, count, 1.0f, 0.0f);
        dsp::applyRamp (r, count, 1.0f, 0.0f);
    }
    if (run.busEq || run.busCorrection)   // check a correction: the bus's Correction (and EQ) stay in
        b.correction.process (l, r, n, sampleRate);
    if (run.busEq)
        b.eq.process (l, r, n, sampleRate);
    b.lastGain = 0.0f;   // afterwards the bus fades back in
    b.meter.muted.store (false, std::memory_order_relaxed);
    b.meter.push (dsp::peak (l, n), dsp::peak (r, n));
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
    ensureBuiltins();
    updateDropoutLogs();
    updateSyncDelays();

    const auto now = callbackCount.load();
    for (auto& b : buses)  b.correction.update (sampleRate, now, running);
    measureGraveyard.erase (std::remove_if (measureGraveyard.begin(), measureGraveyard.end(),
                                            [&] (const auto& r) { return now >= r.second || ! running; }),
                            measureGraveyard.end());
    stemsGraveyard.erase (std::remove_if (stemsGraveyard.begin(), stemsGraveyard.end(),
                                          [&] (const auto& r) { return now >= r.second || ! running; }),
                          stemsGraveyard.end());
    for (auto it = graveyard.begin(); it != graveyard.end();)
    {
        if (now >= it->second || ! running)
        {
            // Streams that did not survive into the current graph: closed in the background. Secondary ASIO
            // streams stop here (the driver stays on the message thread, it closes with its last stream).
            auto retire = [this] (auto& s)
            {
                if (s.use_count() != 1) return;
                if (s->target.typeName == kAsioTypeName) s->stop();
                else                                     opener->dispose (std::move (s));
            };
            for (auto& s : it->first->ins)  retire (s);
            for (auto& s : it->first->outs) retire (s);
            it = graveyard.erase (it);
        }
        else ++it;
    }

    if (current != nullptr)
    {
        for (auto& s : current->ins)  s->poll();
        for (auto& s : current->outs) s->poll();
    }
    if (opener->hasFinished()) rebuildGraph();   // a stream opened in the background is ready

    // Master: a device that stops calling back (unplugged) is noticed after ~2 s.
    const auto calls = callbackCount.load();
    if (masterDevice != nullptr && running && calls == masterSeenCallbacks) ++masterQuietTicks;
    else masterQuietTicks = 0;
    masterSeenCallbacks = calls;

    // Driver error of the master: restart it after a short pause (longer when it keeps failing).
    if (masterErrorPending.exchange (false) && autoReconnect && master.deviceName.isNotEmpty() && masterRestartIn < 0)
    {
        const bool recent = juce::Time::getMillisecondCounterHiRes() - lastMasterRestartMs < 60000.0;
        masterRestartIn = recent ? juce::jmin (600, 5 << juce::jmin (masterRestartStreak, 7)) : 5;
    }
    if (masterRestartIn >= 0 && --masterRestartIn < 0)
    {
        if (autoReconnect) restartMaster ("driver error \"" + lastError + "\"");
        return;
    }

    // Reconnect every ~3 s: failed streams, devices that could not be opened, a missing / stalled master.
    if (! autoReconnect || ++recoverTicks < 30) return;
    recoverTicks = 0;
    const bool masterGone = (masterDevice == nullptr || masterQuietTicks >= 20) && master.deviceName.isNotEmpty();
    if (masterGone)
    {
        // Only the device the user chose: never fall back to another one behind their back. A stalled master
        // that is still listed is restarted like after an error (with the same growing pauses).
        if (auto* type = findType (master.typeName))
        {
            type->scanForDevices();
            const bool listed = type->getDeviceNames (false).contains (master.deviceName);
            const bool waitMore = masterDevice != nullptr && masterRestartStreak > 0
                               && juce::Time::getMillisecondCounterHiRes() - lastMasterRestartMs
                                      < 500.0 * (double) (1 << juce::jmin (masterRestartStreak, 7));
            if (listed && ! waitMore)
            {
                if (masterDevice != nullptr) restartMaster ("no audio callbacks for 2 s");
                else                         setMasterConfig (master);
            }
        }
        return;
    }
    bool anyFailed = anyMissing;
    if (current != nullptr)
    {
        for (auto& s : current->ins)  anyFailed |= s->failed.load();
        for (auto& s : current->outs) anyFailed |= s->failed.load();
    }
    if (anyFailed) rebuildGraph();   // tries again in the background; looks for a missing device itself
}

//==============================================================================
void Engine::audioDeviceAboutToStart (juce::AudioIODevice* d)
{
    sampleRate = d->getCurrentSampleRate();
    blockSize = juce::jmin (kMaxBlock, d->getCurrentBufferSizeSamples());

    for (auto& s : inputs)
    {
        for (int k = 0; k < kFxInstances; ++k)
        {
            s.eqs[(size_t) k].prepare();
            s.gates[(size_t) k].dsp.prepare (sampleRate);
            s.comps[(size_t) k].dsp.prepare (sampleRate);
            s.limiters[(size_t) k].dsp.prepare (sampleRate);
            auto& d = s.denoisers[(size_t) k];
            if (d.dsp != nullptr) d.dsp->prepare (sampleRate);
            d.wasOn = false;
        }
        if (s.stems.inst != nullptr) s.stems.inst->prepare (sampleRate);
        s.stems.wasOn = false;
        s.delay.prepare (sampleRate);
        s.lastTrim = 1; s.lastL = s.lastR = 0;
        s.lastRoute.fill (0.0f);
    }
    lastPlayRoute.fill (0.0f);
    recorder.setSampleRate (sampleRate);
    for (auto& b : buses)
    {
        for (auto& eq : b.eqs) eq.prepare();
        for (auto& l : b.limiters) l.dsp.prepare (sampleRate);
        for (auto& c : b.comps) c.dsp.prepare (sampleRate);
        b.correction.reset();
        b.delay.prepare (sampleRate);
        b.lastGain = 0;
    }
    prepareChains();
}

void Engine::audioDeviceStopped() { recorder.setSampleRate (0.0); }

void Engine::audioDeviceError (const juce::String& e)
{
    // May come from the driver's thread: only noted here, the timer restarts the master on the message thread.
    masterErrorPending.store (true);
    juce::MessageManager::callAsync ([this, e] { lastError = e; sendChangeMessage(); });
}

void Engine::restartMaster (const juce::String& reason)
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    masterRestartStreak = now - lastMasterRestartMs < 60000.0 ? masterRestartStreak + 1 : 1;
    lastMasterRestartMs = now;
    logLine ("master " + master.deviceName + ": " + reason + " - restarting (attempt " + juce::String (masterRestartStreak) + ")");
    const auto err = setMasterConfig (master);
    masterErrorPending.store (false);   // errors while closing the old device belong to this restart
    if (err.isNotEmpty()) logLine ("master restart failed: " + err);   // the 3-s check retries once the device is back
    masterQuietTicks = 0;
    sendChangeMessage();
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
    busyTicks.store (busyTicks.load (std::memory_order_relaxed) + (uint64_t) juce::jmax ((juce::int64) 0, elapsed), std::memory_order_relaxed);
    busyFrames.store (busyFrames.load (std::memory_order_relaxed) + (uint64_t) n, std::memory_order_relaxed);
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

    // Room measurement: the part of the sweep / recording that falls into this chunk.
    MeasureRun* run = measureLive.load (std::memory_order_acquire);
    int runStart = 0, runCount = 0;
    if (run != nullptr)
    {
        runStart = run->pos.load (std::memory_order_relaxed);
        runCount = juce::jlimit (0, n, run->length - runStart);
        if (runCount == 0) run = nullptr;
    }

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
    const int micTap = micTapInput.load (std::memory_order_relaxed);
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

        if (run != nullptr && i == run->input)   // the raw source, before any processing of the strip
        {
            if (const float* src = run->inputChannel == 0 ? L : R)
            {
                std::memcpy (run->capture.data() + runStart, src, (size_t) runCount * sizeof (float));
                const float pk = dsp::peak (src, runCount);
                if (pk > run->inputPeak.load (std::memory_order_relaxed)) run->inputPeak.store (pk, std::memory_order_relaxed);
            }
            else
                run->noInput.store (true, std::memory_order_relaxed);
        }
        if (i == micTap)
            if (const float* src = micTapChannel.load (std::memory_order_relaxed) == 0 ? L : R)
                if (const float pk = dsp::peak (src, n); pk > micTapPeak.load (std::memory_order_relaxed))
                    micTapPeak.store (pk, std::memory_order_relaxed);

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
        auto& bus = buses[(size_t) b];
        if (run != nullptr && b == run->bus)
        {
            renderMeasurement (bus, *run, runStart, runCount, n);   // the sweep replaces the bus's signal
            continue;
        }
        if (! bus.active->isOn()) continue; // removed bus: nothing is routed to it
        if (monBusIndex >= 0 && b == selBus) continue;
        if (b == monBusIndex)
        {
            std::memcpy (bus.l.data(), buses[(size_t) selBus].l.data(), (size_t) n * sizeof (float));
            std::memcpy (bus.r.data(), buses[(size_t) selBus].r.data(), (size_t) n * sizeof (float));
        }
        processBus (bus, n, timed);
        if (run != nullptr && run->muteOthers)
        {
            // Silent while measuring: fades out in the first chunk, the fader fades back in afterwards.
            if (runStart == 0)
            {
                dsp::applyRamp (bus.l.data(), n, 1.0f, 0.0f);
                dsp::applyRamp (bus.r.data(), n, 1.0f, 0.0f);
            }
            else
            {
                std::memset (bus.l.data(), 0, (size_t) n * sizeof (float));
                std::memset (bus.r.data(), 0, (size_t) n * sizeof (float));
            }
            bus.lastGain = 0.0f;
        }
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

    if (run != nullptr)
        run->pos.store (run->stop.load (std::memory_order_relaxed) ? run->length : runStart + runCount, std::memory_order_release);
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

    const float trim = dbToGain (s.trim->get(), -100.0f);
    dsp::applyRamp (l, n, s.lastTrim, trim);
    dsp::applyRamp (r, n, s.lastTrim, trim);
    s.lastTrim = trim;
    s.preMeter.push (dsp::peak (l, n), dsp::peak (r, n));

    lap (DspTimes::other);

    // Effect chain: plugins and built-in effects in the order the user set (pre-fader).
    uint32_t ran = 0;   // bit k: denoiser k / gate k / compressor k ran (shifted by 0 / 8 / 16)
    int latency = 0;
    bool tapped = false;
    const int tapEq = s.spectrum.eqInstance.load (std::memory_order_relaxed);
    bool chainRan = false, stemsRan = false;
    int stemsLatency = 0;
    if (! s.fxBypass->isOn())
        chainRan = s.plugins.process (l, r, n, timed, [&] (Fx f, int k)
        {
            lap (DspTimes::plugins);   // time since the previous stage: plugins (and the chain itself)
            const auto i = (size_t) k;
            switch (f)
            {
                case Fx::denoiser:
                {
                    auto& d = s.denoisers[i];
                    auto* dn = d.live.load (std::memory_order_acquire);
                    if (! d.on->isOn() || dn == nullptr) break;
                    if (! d.wasOn) dn->reset();
                    dn->process (l, r, n, d.settings(), ! monoSource);
                    latency += dn->getLatencySamples();
                    ran |= 1u << k;
                    lap (DspTimes::denoiser);
                    break;
                }
                case Fx::eq:
                    s.eqs[i].process (l, r, n, sampleRate);
                    if (k == tapEq) { s.spectrum.push (l, r, n); tapped = true; }
                    lap (DspTimes::eq);
                    break;
                case Fx::gate:
                    if (! s.gates[i].on->isOn()) break;
                    s.gates[i].dsp.process (l, r, n, s.gates[i].settings (s.voiceProbability()));
                    ran |= 1u << (8 + k);
                    lap (DspTimes::gate);
                    break;
                case Fx::comp:
                    if (! s.comps[i].on->isOn()) break;
                    s.comps[i].dsp.process (l, r, n, s.comps[i].settings());
                    s.comps[i].gr.store (s.comps[i].dsp.lastGr, std::memory_order_relaxed);
                    ran |= 1u << (16 + k);
                    lap (DspTimes::comp);
                    break;
                case Fx::limiter:
                    if (s.limiters[i].on->isOn()) s.limiters[i].dsp.process (l, r, n, s.limiters[i].ceiling->get());
                    lap (DspTimes::limiter);
                    break;
                case Fx::phase:
                    if (s.phases[i]->isOn())
                    {
                        juce::FloatVectorOperations::negate (l, l, n);
                        juce::FloatVectorOperations::negate (r, r, n);
                    }
                    break;
                case Fx::stems:
                {
                    auto& st = s.stems;
                    auto* inst = st.live.load (std::memory_order_acquire);
                    if (! st.on->isOn() || inst == nullptr) break;
                    float gains[stems::numStems];
                    st.gains (gains);
                    const int mode = juce::roundToInt (st.mode->get());
                    inst->process (l, r, n, gains, mode, ! st.wasOn, stemsTuning (stems::modelFor (mode)),
                                   st.meterUsers.load (std::memory_order_relaxed) > 0 ? st.peaks.data() : nullptr, StemsFx::numReaders);
                    stemsLatency = inst->getLatencySamples();
                    stemsRan = true;
                    lap (DspTimes::other);
                    break;
                }
                default: break;
            }
        });
    if (s.fxBypass->isOn() || chainRan)
    {
        s.stems.wasOn = stemsRan;
        s.stemsLatency.store (stemsRan ? stemsLatency : 0, std::memory_order_relaxed);
    }
    lap (DspTimes::plugins);
    for (int k = 0; k < kFxInstances; ++k)
    {
        const auto i = (size_t) k;
        s.denoisers[i].wasOn = (ran >> k) & 1u;
        if (! ((ran >> (8 + k)) & 1u)) s.gates[i].dsp.shownGain.store (1.0f, std::memory_order_relaxed);
        if (! ((ran >> (16 + k)) & 1u)) s.comps[i].gr.store (0.0f, std::memory_order_relaxed);
    }
    s.dspLatency.store (latency, std::memory_order_relaxed);
    if (! tapped) s.spectrum.push (l, r, n);   // the shown EQ is not in the chain: the analyzer shows the chain's output

    // Sync Delay (aligns this input with slower ones); nothing to do while it is 0.
    s.delay.process (l, r, n, delaySamples (s.index));

    if (s.mono->isOn())
    {
        juce::FloatVectorOperations::add (l, r, n);
        juce::FloatVectorOperations::multiply (l, 0.5f, n);
        std::memcpy (r, l, (size_t) n * sizeof (float));
    }

    const bool silent = anySolo ? ! s.solo->isOn() : s.mute->isOn();   // a soloed channel plays even if muted
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

    // Effect chain (pre-fader), in the order the user set: correction, phase, EQ, compressor, limiter and plugins.
    bool tapped = false;
    uint32_t compsRan = 0;
    const int tapEq = b.spectrum.eqInstance.load (std::memory_order_relaxed);
    if (! b.fxBypass->isOn())
        b.plugins.process (l, r, n, timed, [&] (Fx f, int k)
        {
            lap (DspTimes::plugins);
            const auto i = (size_t) k;
            switch (f)
            {
                case Fx::correction:
                    b.correction.process (l, r, n, sampleRate);
                    lap (DspTimes::eq);
                    break;
                case Fx::eq:
                    b.eqs[i].process (l, r, n, sampleRate);
                    if (k == tapEq) { b.spectrum.push (l, r, n); tapped = true; }
                    lap (DspTimes::eq);
                    break;
                case Fx::limiter:
                    if (b.limiters[i].on->isOn()) b.limiters[i].dsp.process (l, r, n, b.limiters[i].ceiling->get());
                    lap (DspTimes::limiter);
                    break;
                case Fx::comp:
                    if (! b.comps[i].on->isOn()) break;
                    b.comps[i].dsp.process (l, r, n, b.comps[i].settings());
                    b.comps[i].gr.store (b.comps[i].dsp.lastGr, std::memory_order_relaxed);
                    compsRan |= 1u << k;
                    lap (DspTimes::comp);
                    break;
                case Fx::phase:
                    if (b.phases[i]->isOn())
                    {
                        juce::FloatVectorOperations::negate (l, l, n);
                        juce::FloatVectorOperations::negate (r, r, n);
                    }
                    break;
                default: break;
            }
        });
    lap (DspTimes::plugins);
    for (int k = 0; k < kFxInstances; ++k)
        if (! ((compsRan >> k) & 1u)) b.comps[(size_t) k].gr.store (0.0f, std::memory_order_relaxed);
    if (! tapped) b.spectrum.push (l, r, n);

    if (b.mono->isOn())
    {
        juce::FloatVectorOperations::add (l, r, n);
        juce::FloatVectorOperations::multiply (l, 0.5f, n);
        std::memcpy (r, l, (size_t) n * sizeof (float));
    }

    const bool muted = b.mute->isOn();
    const float fader = dbToGain (b.gain->get());
    if (muted) // meter shows the level the bus would have (drawn grey)
        b.meter.push (dsp::peak (l, n) * fader, dsp::peak (r, n) * fader);
    b.meter.muted.store (muted, std::memory_order_relaxed);

    const float g = muted ? 0.0f : fader;
    dsp::applyRamp (l, n, b.lastGain, g);
    dsp::applyRamp (r, n, b.lastGain, g);
    b.lastGain = g;

    b.delay.process (l, r, n, delaySamples (busChannel (b.index)));

    if (! muted)
        b.meter.push (dsp::peak (l, n), dsp::peak (r, n));
    lap (DspTimes::other);
}
} // namespace hydra

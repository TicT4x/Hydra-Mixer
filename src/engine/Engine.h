#pragma once
#include "core/Params.h"
#include "core/Dsp.h"
#include "core/ParamEq.h"
#include "core/Denoiser.h"
#include "core/Correction.h"
#include "core/HeadphoneEq.h"
#include "core/Utility.h"
#include "core/VoiceFx.h"
#include "core/MatchEq.h"
#include "IoStreams.h"
#include "Stems.h"
#include "Transpose.h"
#include "PluginChain.h"
#include "Recorder.h"
#include "RoomMeasure.h"

#include <map>

namespace hydra
{
struct Meter
{
    std::atomic<float> l { 0 }, r { 0 };
    std::atomic<bool> muted { false }; // levels are what the channel would play if it were not muted (UI draws them grey)
    std::atomic<float> remoteL { 0 }, remoteR { 0 };   // second reader: phones / tablets (remote/RemoteControl.h)
    /** Remote meters are only collected while a phone or tablet shows them (one atomic load per push otherwise). */
    static inline std::atomic<int> remoteUsers { 0 };
    void push (float a, float b) noexcept
    {
        if (a > l.load (std::memory_order_relaxed)) l.store (a, std::memory_order_relaxed);
        if (b > r.load (std::memory_order_relaxed)) r.store (b, std::memory_order_relaxed);
        if (remoteUsers.load (std::memory_order_relaxed) > 0)
        {
            if (a > remoteL.load (std::memory_order_relaxed)) remoteL.store (a, std::memory_order_relaxed);
            if (b > remoteR.load (std::memory_order_relaxed)) remoteR.store (b, std::memory_order_relaxed);
        }
    }
    std::pair<float, float> take() noexcept { return { l.exchange (0.0f), r.exchange (0.0f) }; }
    std::pair<float, float> takeRemote() noexcept { return { remoteL.exchange (0.0f), remoteR.exchange (0.0f) }; }
};

/** Feeds a channel's signal (after its EQ) to the spectrum analyzer of the large EQ window.
    Inactive unless a view is open; then the audio thread writes into a wait-free ring. */
struct SpectrumTap
{
    std::atomic<int> users { 0 };   // open analyzer views
    std::atomic<int> eqInstance { 0 };   // the EQ (instance) whose output is analysed
    SpscAudioRing ring;
    void push (const float* l, const float* r, int n) noexcept
    {
        if (users.load (std::memory_order_relaxed) <= 0) return;
        const float* src[2] = { l, r };
        ring.write (src, 2, n);
    }
    /** The signal entering that EQ, for the grey "before" spectrum of the EQ window (only while it is shown). */
    std::atomic<int> preUsers { 0 };
    SpscAudioRing preRing;
    void pushPre (const float* l, const float* r, int n) noexcept
    {
        if (preUsers.load (std::memory_order_relaxed) <= 0 || users.load (std::memory_order_relaxed) <= 0) return;
        const float* src[2] = { l, r };
        preRing.write (src, 2, n);
    }
};

/** CPU time spent in each processing stage of a channel, for the DSP view. The audio thread only measures
    while a view is open (Engine::profileUsers > 0). Counters accumulate high-resolution ticks and are
    never reset: the view takes differences between two readings. Single writer (audio thread). */
struct DspTimes
{
    enum Stage { denoiser, eq, gate, comp, plugins, limiter, utility, transpose, stems, correction, voice, saturation, other, numStages };
    std::array<std::atomic<uint64_t>, numStages> ticks {};
    void add (Stage s, int64_t t) noexcept
    {
        auto& a = ticks[(size_t) s];
        a.store (a.load (std::memory_order_relaxed) + (uint64_t) juce::jmax ((int64_t) 0, t), std::memory_order_relaxed);
    }
    uint64_t get (Stage s) const noexcept { return ticks[(size_t) s].load (std::memory_order_relaxed); }
};

//==============================================================================
// One instance of a built-in effect: its parameters and its audio-thread state. A channel has kFxInstances of
// each (ids "in1.gate.*" for the first, "in1.gate2.*" ... for the others); the effect chain says which are used.
struct GateFx
{
    Param *on {}, *thr {}, *range {}, *attack {}, *hold {}, *release {}, *scOn {}, *scFreq {}, *voice {}, *voiceThr {};
    dsp::Gate dsp;
    /** voice: speech probability of the denoiser (-1 = none). */
    dsp::GateSettings settings (float voice) const;
};

struct CompFx
{
    Param *on {}, *input {}, *thr {}, *ratio {}, *knee {}, *attack {}, *release {}, *makeup {}, *autoGain {};
    dsp::Compressor dsp;
    std::atomic<float> gr { 0 };   // gain reduction shown in the UI
    /** Level entering the compressor (block peak, linear, before its input gain), for the live point on the curve.
        Measured only while views > 0 (the settings window is open). */
    std::atomic<float> inPeak { 0 };
    std::atomic<int> views { 0 };
    dsp::CompSettings settings() const;
    /** Audio thread, before process(). */
    void tapInput (const float* l, const float* r, int n) noexcept
    {
        if (views.load (std::memory_order_relaxed) > 0)
            inPeak.store (juce::jmax (dsp::peak (l, n), dsp::peak (r, n)), std::memory_order_relaxed);
    }
};

struct DenoiserFx
{
    Param *on {}, *amount {}, *mode {};
    std::unique_ptr<dsp::Denoiser> dsp;   // first instance always; others created when added (message thread)
    std::atomic<dsp::Denoiser*> live { nullptr };   // = dsp once it exists; what the audio thread reads
    bool wasOn = false;
    dsp::DenoiserSettings settings() const;
};

struct LimiterFx
{
    Param *on {}, *ceiling {};
    dsp::Limiter dsp;
};

/** Utility (core/Utility.h, like Ableton's): polarity, channel mode, width / mono, bass mono, balance, gain.
    Replaces the former phase block: legacyPhase is that block's switch ("in1.phase" / "in1.phase2" ...), still
    read from old sessions, scenes and MIDI; Engine::ensureBuiltins turns it into this Utility with the phase
    inverted. */
struct UtilityFx
{
    Param *on {}, *phase {}, *channel {}, *width {}, *mono {}, *bass {}, *bassFreq {}, *balance {}, *gain {}, *dc {};
    Param *legacyPhase {};
    dsp::Utility dsp;
    dsp::UtilitySettings settings() const noexcept
    {
        dsp::UtilitySettings st;
        st.invertL = st.invertR = phase->isOn();   // one switch flips both channels
        st.channel = juce::roundToInt (channel->get());
        st.width = width->get() * 0.01f;
        st.mono = mono->isOn();
        st.bassMono = bass->isOn(); st.bassHz = bassFreq->get();
        st.balance = balance->get() * 0.01f;
        st.gainDb = gain->get();
        st.dcFilter = dc->isOn();
        return st;
    }
};

/** Saturation (core/VoiceFx.h): soft harmonic saturation, level-matched. */
struct SaturationFx
{
    Param *on {}, *drive {}, *warmth {}, *mix {}, *output {};
    dsp::Saturator dsp;
    dsp::SaturatorSettings settings() const noexcept { return { drive->get(), warmth->get() * 0.01f, mix->get() * 0.01f, output->get() }; }
};

/** Studio Voice (core/VoiceFx.h): one knob (amount) and a profile run a whole vocal chain; Engine::convertVoice
    replaces it with the real blocks (EQ, Gate, Compressor(s), Saturation, Limiter) at their current settings.
    Once per channel. "Learn my mic" (Engine::startVoiceLearn): ~8 s of speech are analysed (match::Analyzer), the
    spectrum is kept in mic (params, so it is saved with the session), and the EQ is fitted from it toward the speech
    reference plus the profile's character (Engine::updateVoices, again when the profile changes). */
struct VoiceFx
{
    Param *on {}, *amount {}, *profile {};
    Param *learned {};                               // mic holds a learned spectrum
    std::array<Param*, match::kBands> mic {};        // the voice through this microphone (normalised dB)
    dsp::StudioVoice dsp;
    int profileIndex() const noexcept { return juce::roundToInt (profile->get()); }
    float amountNorm() const noexcept { return amount->get() * 0.01f; }

    // Learning (message thread, except the ring and the flag the audio thread writes / reads).
    enum class Learn { idle, listening, done, failed };
    std::atomic<bool> listening { false };           // audio thread pushes the channel's input into ring
    SpscAudioRing ring;                              // allocated when learning starts
    match::Analyzer analyzer;
    Learn learnState = Learn::idle;
    juce::String learnMessage;                       // "Speak normally ...", "No voice heard ..."
    float learnProgress = 0.0f;                      // 0..1 of the speech needed
    uint64_t fittedKey = ~(uint64_t) 0;              // what the current match was fitted for
};

/** Stems (engine/Stems.h): one per input. Its state exists only while the effect is in the chain. */
struct StemsFx
{
    Param *on {}, *mode {};
    std::array<Param*, stems::numStems> gain {}, mute {}, solo {};
    /** Advanced options (Stems window, "ADVANCED"; applied only while advanced is on): left / right side of a stem
        muted (e.g. a guitar solo only on the right), mono, pan, width (after mono: above 100 % it also makes a
        mono stem wide again, mono-compatible).*/
    std::array<Param*, stems::numStems> muteL {}, muteR {}, mono {}, pan {}, width {};
    Param* advanced {};
    /** The mix of each stem (gain() plus the advanced options). Any thread. */
    void mixes (stems::StemMix* out) const noexcept;
    /** Linear gain of each stem: mute = 0; while any stem is soloed, only soloed ones play (also a muted one). In Live mode (no
        guitar) the guitar follows the rest, which contains it. Any thread. */
    void gains (float* out) const noexcept;
    /** Levels of the stems (before their gains, peak since the last take) for two readers: the window (0) and the
        block's knobs (1). Measured only while meterUsers > 0 (one atomic read per block otherwise). */
    enum Reader { window = 0, block = 1, numReaders };
    std::atomic<int> meterUsers { 0 };
    std::array<std::atomic<float>, numReaders * stems::numStems> peaks {};
    float takePeak (int stem, Reader r = window) noexcept
    {
        return peaks[(size_t) (r * stems::numStems + stem)].exchange (0.0f, std::memory_order_relaxed);
    }
    std::unique_ptr<stems::Instance> inst;            // message thread
    std::atomic<stems::Instance*> live { nullptr };   // what the audio thread reads
    bool wasOn = false;                               // audio thread: processed in the previous block
};

//==============================================================================
/** Headphone Correction (once per channel): EQ that makes headphones 'from' sound like 'to' (hp::Library ids;
    'to' empty = neutral). The fitted profile (full strength) is kept with the channel, so it keeps working when an
    imported headphone file is gone. amount scales it, gain and auto makeup set the level (applied by the Engine timer). */
struct HeadphoneFx
{
    Param *on {}, *amount {};
    Param *gain {};       // output level (dB) on top of the preamp
    Param *autoGain {};   // auto makeup: gives back the loudness the correction takes (pink noise level), may clip
    /** What the dsp runs: the profile at amount, its preamp plus makeup and gain. */
    CorrectionProfile output() const;
    /** Makeup that auto gain adds (dB, >= 0): the loudness the correction at amount takes away. */
    double makeupDb() const;
    juce::String from, to;        // message thread
    CorrectionProfile full;       // at 100 %
    double rmsBeforeDb = 0.0, rmsAfterDb = 0.0;
    uint32_t version = 0;         // bumped with every change of from / to (views)
    std::array<float, 3> applied { -1.0f, 0.0f, 0.0f };   // amount, gain, auto the dsp profile was built for
    std::array<float, 3> settings() const { return { amount->get(), gain->get(), autoGain->get() }; }
    Correction dsp;               // runs the scaled profile (dsp.on == on)
};

//==============================================================================
/** The effect side of a channel (input or bus): its chain and every built-in effect with all instances.
    Since 12.11. every channel can have every effect (Fx); which ones it has says its chain. The state of the
    big ones (denoisers after the first input one, Stems, Transpose) is only created when they are added. */
struct ChannelFx
{
    PluginChain plugins;
    SpectrumTap spectrum;
    DspTimes dspTimes;
    std::array<ParamEq, kFxInstances> eqs;
    std::array<GateFx, kFxInstances> gates;
    std::array<CompFx, kFxInstances> comps;
    std::array<DenoiserFx, kFxInstances> denoisers;
    std::array<LimiterFx, kFxInstances> limiters;   // off by default, added from the effect menu (9.11.)
    std::array<UtilityFx, kFxInstances> utilities;
    StemsFx stems;                                  // source separation (once per channel)
    TransposeFx transpose;                          // pitch shift (once per channel)
    /** Speaker correction: the first instance is the one measured for a hardware output (Correction window, pool,
        wizard); further ones (since 12.11., up to kFxInstances) use saved corrections. */
    Correction correction;
    std::array<Correction, kFxInstances - 1> moreCorrections;
    Correction& correctionAt (int k) noexcept             { return k <= 0 ? correction : moreCorrections[(size_t) juce::jmin (k, kFxInstances - 1) - 1]; }
    const Correction& correctionAt (int k) const noexcept { return k <= 0 ? correction : moreCorrections[(size_t) juce::jmin (k, kFxInstances - 1) - 1]; }
    std::array<HeadphoneFx, kFxInstances> headphones;   // headphone correction (instances like the other effects)
    std::array<VoiceFx, kFxInstances> voices;           // Studio Voice
    std::array<SaturationFx, kFxInstances> saturations;
    /** A Match EQ learning its reference from this channel: its ring, written after this channel's chain. */
    std::atomic<SpscAudioRing*> refTap { nullptr };
    Param *fxBypass {};
    Param *fxPost {};   // effect chain after the fader (and pan) instead of before (Inspector: PRE / POST)

    /** Denoiser instance k is in the effect chain, switched on and the chain is not bypassed (any thread). */
    bool denoiserActive (int k) const noexcept
    {
        return denoisers[(size_t) k].on->isOn() && denoisers[(size_t) k].live.load (std::memory_order_acquire) != nullptr
            && plugins.hasBuiltin (Fx::denoiser, k) && ! fxBypass->isOn();
    }
    /** Any denoiser active. */
    bool denoiserActive() const noexcept
    {
        for (int k = 0; k < kFxInstances; ++k) if (denoiserActive (k)) return true;
        return false;
    }
    /** Speech probability of the first active denoiser (Voice Key of the gates), -1 if none. */
    float voiceProbability() const noexcept
    {
        for (int k = 0; k < kFxInstances; ++k)
            if (denoiserActive (k))
                if (auto* d = denoisers[(size_t) k].live.load (std::memory_order_acquire); d != nullptr) return d->getVad();
        return -1.0f;
    }

    /** Processing latency added inside this channel by its denoisers, in samples. */
    std::atomic<int> dspLatency { 0 };
    /** Latency of the Stems effect, in samples (0 while it is off or has no model yet). */
    std::atomic<int> stemsLatency { 0 };
    /** Latency of the Transpose effect, in samples (0 while it is off). */
    std::atomic<int> transposeLatency { 0 };
    /** True while nothing of this channel can be heard and its effects are skipped (Engine::processStrip / processBus). */
    std::atomic<bool> fxIdle { false };
};

//==============================================================================
struct InputStrip : ChannelFx
{
    int index = 0;
    juce::String name;
    IoTarget source;
    Meter meter, preMeter;

    // parameters
    Param *active {};   // slot in use (shown as a strip and processed); see Engine::addInput / removeInput
    Param *gain {}, *trim {}, *pan {}, *mute {}, *solo {}, *mono {};
    Param *syncOn {}, *delayMs {};   // Sync Delay: automatic alignment with the input sync group / manual delay
    std::array<Param*, kMaxBuses> route {};
    std::array<Param*, kMaxBuses> send {};   // per-bus fader level (edited while a bus is selected with SEL)

    // The first instance of each effect under the names it had before there were several.
    ParamEq& eq = eqs[0];
    Param*& gateOn = gates[0].on;   Param*& gateThr = gates[0].thr;   Param*& gateRange = gates[0].range;
    Param*& gateAttack = gates[0].attack;   Param*& gateHold = gates[0].hold;   Param*& gateRelease = gates[0].release;
    Param*& gateScOn = gates[0].scOn;   Param*& gateScFreq = gates[0].scFreq;   Param*& gateVoice = gates[0].voice;
    Param*& gateVoiceThr = gates[0].voiceThr;
    Param*& compOn = comps[0].on;   Param*& compInput = comps[0].input;   Param*& compThr = comps[0].thr;
    Param*& compRatio = comps[0].ratio;   Param*& compKnee = comps[0].knee;   Param*& compAttack = comps[0].attack;
    Param*& compRelease = comps[0].release;   Param*& compMakeup = comps[0].makeup;   Param*& compAuto = comps[0].autoGain;
    Param*& dnOn = denoisers[0].on;   Param*& dnAmount = denoisers[0].amount;   Param*& dnMode = denoisers[0].mode;
    dsp::Gate& gate = gates[0].dsp;
    dsp::Compressor& comp = comps[0].dsp;
    std::atomic<float>& compGr = comps[0].gr;
    std::unique_ptr<dsp::Denoiser>& denoiser = denoisers[0].dsp;

    dsp::GateSettings gateSettings() const { return gates[0].settings (voiceProbability()); }
    dsp::CompSettings compSettings() const { return comps[0].settings(); }
    dsp::DenoiserSettings denoiserSettings() const { return denoisers[0].settings(); }

    // audio-thread state
    dsp::DelayLine delay;
    float lastTrim = 1, lastL = 0, lastR = 0;
    std::array<float, kMaxBuses> lastRoute {};   // last applied send gain per bus (0 = not routed)
    std::vector<float> l, r;
    std::vector<float> recL, recR;   // what the recorder records of this input in this block (while it is armed)
};

struct Bus : ChannelFx
{
    int index = 0;
    juce::String name;
    juce::String icon;   // optional symbol (ui/ChannelIcons.h id) shown instead of "H1" / "V1"; empty = the code
    IoTarget output;
    Meter meter;

    Param *active {};   // slot in use (shown as a strip and processed); see Engine::addBus / removeBus
    Param *gain {}, *mute {}, *mono {};
    Param *sel {};   // SEL: input faders edit the sends to this bus (exclusive), optional monitoring
    Param *syncOn {}, *delayMs {};   // Sync Delay (see InputStrip)

    // The first instances under their old names.
    ParamEq& eq = eqs[0];
    Param*& limOn = limiters[0].on;
    Param*& limCeil = limiters[0].ceiling;
    dsp::Limiter& limiter = limiters[0].dsp;

    dsp::DelayLine delay;
    float lastGain = 0;
    std::vector<float> l, r;
};

/** One track of the multitrack recorder (a recorded input). Plays the track's clips of the loaded recorder project
    through its own strip: gain, effect chain (PRE / POST), mono, pan, fader, mute / solo (among the tracks).
    The parameters (ids "rt1.*") are project parameters: saved in the project file, not in the session. */
struct RecTrack : ChannelFx
{
    int index = 0;
    juce::String name;   // shown name = the recorded input's name (and the file name)
    Meter meter;

    Param *gain {}, *trim {}, *pan {}, *mute {}, *solo {}, *mono {};

    // audio-thread state
    float lastTrim = 1, lastL = 0, lastR = 0;
    std::vector<float> l, r;
};

struct MasterConfig
{
    juce::String typeName = "Windows Audio (Low Latency Mode)";
    juce::String deviceName;   // empty = system default output
    double sampleRate = 48000.0;
    int bufferSize = 0;        // 0 = device default
};

//==============================================================================
/** The mixer engine. Runs entirely inside the master device's audio callback. */
class Engine : private juce::AudioIODeviceCallback,
               private juce::Timer,
               public juce::ChangeBroadcaster
{
public:
    Engine();
    ~Engine() override;

    ParamRegistry params;
    PluginHost pluginHost;
    std::array<InputStrip, kMaxInputs> inputs;
    std::array<Bus, kMaxBuses> buses;
    /** Tracks of the multitrack recorder (channels kRecTrackBase + t). Used while a recorder project is loaded. */
    std::array<RecTrack, kMaxRecTracks> recTracks;
    Recorder recorder;

    /** Stems: GPU setting per model (stems.<model>.auto, stems.<model>.gpu in %), the same for every channel. */
    std::array<Param*, stems::kNumModels> stemsAuto {}, stemsGpu {};
    stems::Tuning stemsTuning (stems::Model m) const noexcept
    {
        return { stemsAuto[(size_t) m]->isOn(), stemsGpu[(size_t) m]->get() * 0.01f };
    }

    /** Monitoring: while a bus is selected (SEL), its signal replaces the normal mix of the monitor bus. */
    Param *monOn {}, *monBus {};
    /** Scene triggers (not stored): switching one on recalls that scene (SceneBank), so scenes can be
        called by MIDI and keyboard shortcuts like any button. Next / previous step through the list. */
    std::array<Param*, kMaxScenes> sceneRecall {};
    Param *sceneNext {}, *scenePrev {};
    /** Bus selected with SEL, or -1. */
    int getSelectedBus() const noexcept;
    /** Bus that currently plays the monitored signal, or -1. */
    int getMonitorBus() const noexcept;
    /** Parameter a control mapped to p really drives: while a bus is selected (SEL), an input's
        main fader drives its send to that bus. Any thread. */
    Param* faderTarget (Param* p) const noexcept;
    /** Keeps SEL exclusive (the most recently switched on bus wins). Message thread. */
    void enforceExclusiveSelect();

    //--------------------------------------------------------------------------
    // Channel layout (message thread). Slots are switched on / off, never created or renumbered.
    bool isInputActive (int i) const noexcept { return inputs[(size_t) i].active->isOn(); }
    bool isBusActive (int b) const noexcept   { return buses[(size_t) b].active->isOn(); }
    int numActiveInputs() const noexcept;
    int numActiveBuses (bool virtualBuses) const noexcept;
    /** Changes whenever a slot is added or removed. */
    uint32_t getLayoutVersion() const noexcept;
    /** Switches on the first free input slot (a fresh strip). Returns its index or -1 if all are in use. */
    int addInput();
    /** Switches on the first free A (virtualBus = false) or B bus. Returns the bus index or -1. */
    int addBus (bool virtualBus);
    /** Removes a channel: the slot is switched off and reset to defaults (parameters, name, source /
        output, plugins). For a bus, the routing / sends of all inputs and the recorder to it are reset too.
        At least one input and one A bus always remain. Returns every parameter that was reset, so the
        caller can drop MIDI mappings and shortcuts bound to them; empty if nothing was removed. */
    std::vector<Param*> removeInput (int i);
    std::vector<Param*> removeBus (int b);

    /** Channel groups that can be reordered among themselves. */
    enum class Group { inputs = 0, hwBuses, virtBuses };
    /** Pairs (parameter, parameter that holds its value after the reorder) of every parameter whose
        value moves when the channels of a group are put into the given order. order lists the group's
        active slots (input index / bus index) in the wanted order; empty if the order is invalid or
        unchanged. A MIDI mapping / shortcut that should follow its channel moves from first to second. */
    std::vector<std::pair<Param*, Param*>> reorderPlan (Group, const std::vector<int>& order) const;
    /** Puts the channels of a group into the given order (see reorderPlan): every slot takes over the
        settings, name, source / output and plugins of the channel that moves there. The set of slots in
        use stays the same, so a channel takes the number of the position it is moved to. For buses, the
        routing / sends of all inputs, the recorder outputs / source, the monitor bus and SEL follow too.
        Message thread. Returns false if nothing was changed. */
    bool reorderChannels (Group, const std::vector<int>& order);

    juce::OwnedArray<juce::AudioIODeviceType>& getDeviceTypes() noexcept { return types; }
    juce::AudioIODeviceType* findType (const juce::String& name) const;
    void rescanDevices();

    // Master clock device
    MasterConfig getMasterConfig() const { return master; }
    juce::String setMasterConfig (const MasterConfig& cfg);   // (re)opens the master
    juce::AudioIODevice* getMasterDevice() const noexcept { return masterDevice.get(); }
    juce::String getLastError() const { return lastError; }

    // Routing (message thread). Applied without stopping the engine.
    void setInputSource (int strip, const IoTarget& t);
    void setBusOutput (int bus, const IoTarget& t);
    void rebuildGraph();
    /** While set, streams on devices it returns true for are not opened, and rebuildGraph closes open ones
        (e.g. all Virtual Audio Cable endpoints while that driver restarts). Message thread. */
    std::function<bool (const IoTarget&)> holdDevice;

    // Status
    /** Recent peak: highest single-callback load, decaying over ~20 callbacks. */
    float getPeakLoad() const noexcept { return cpuLoad.load(); }
    /** Highest single-callback load of the last 0.5-1 s (1 = the block took as long as it plays: overload). */
    float getRecentPeak() const noexcept
    {
        return juce::jmax (peakLast.load (std::memory_order_relaxed), peakWindow.load (std::memory_order_relaxed));
    }
    /** Callbacks so far that took longer than real time (each one is usually an audible click). Readers diff it. */
    uint64_t getOverloads() const noexcept { return overloads.load (std::memory_order_relaxed); }

    /** Average DSP load (callback time / real time) like a DAW's CPU meter. Each reader owns one and
        calls update() regularly; the value covers the time since the previous reading (min. ~0.25 s). */
    struct LoadMeter
    {
        float update (const Engine& e) noexcept
        {
            const uint64_t t = e.busyTicks.load (std::memory_order_relaxed);
            const uint64_t f = e.busyFrames.load (std::memory_order_relaxed);
            const double budget = (double) (f - frames) / juce::jmax (1.0, e.sampleRate);   // seconds of audio
            if (f < frames || t < ticks) { ticks = t; frames = f; return value; }
            if (budget >= 0.25)
            {
                value = (float) ((double) (t - ticks) * e.ticksToSeconds / budget);
                ticks = t; frames = f;
            }
            return value;
        }
        uint64_t ticks = 0, frames = 0;
        float value = 0;
    };

    /** Reopen devices that went offline (unplugged, driver restarted, not there at startup) as soon as they are
        back; checked every ~3 s. Also restarts a master device that stopped calling back or reported an error
        (driver reset, format change; with growing pauses when it keeps failing). Off: only a new choice
        of the device or Tools > Restart Audio Engine reconnects. Message thread. */
    bool autoReconnect = true;

    //--------------------------------------------------------------------------
    // DSP profiling (DSP view): per channel / stage (DspTimes), per plugin (PluginSlot::dspTicks), per
    // asynchronous stream (AsyncStream::dspTicks) and these engine-wide counters, all in high-resolution ticks.
    // Measured only while profileUsers > 0, so a closed view costs nothing.
    struct Profile
    {
        std::atomic<int> users { 0 };
        std::atomic<uint64_t> total { 0 };      // whole audio callback
        std::atomic<uint64_t> frames { 0 };     // frames processed while measuring (the time budget)
        std::atomic<uint64_t> recorder { 0 };   // recorder playback mix + recording
        std::atomic<float> peak { 0 };          // highest single-callback load since the view last took it
    } profile;
    double getTicksPerSecond() const noexcept { return 1.0 / ticksToSeconds; }
    /** Engine time of the asynchronous streams (resampling, drift control): label + ticks. Message thread. */
    std::vector<std::pair<juce::String, uint64_t>> getStreamDspTicks() const;
    double getSampleRate() const noexcept { return sampleRate; }
    int getBlockSize() const noexcept { return blockSize; }
    double getMasterLatencyMs() const;

    /** State of a strip's source / a bus's output, for the status symbols in the strips. Message thread. */
    struct StreamHealth
    {
        enum class State { none, ok, offline, waiting, noDriver };
        State state = State::none;
        juce::String error;                        // why it is offline (if known)
        struct Dropouts { int count; double secondsAgo; };
        std::vector<Dropouts> recent;              // dropouts of the last kDropoutWindowSeconds, newest first
        int recentTotal() const { int n = 0; for (auto& d : recent) n += d.count; return n; }
    };
    static constexpr double kDropoutWindowSeconds = 120.0;   // older dropouts are no longer shown
    StreamHealth getStreamHealth (const IoTarget& t, bool input) const;

    /** Where the device-side latency of a strip's source / a bus's output comes from. Message thread. */
    struct IoLatency
    {
        bool online = false;
        bool masterClock = false;   // runs on the master clock: no clock sync buffer
        double bufferMs = 0.0;      // master: one buffer period
        double driverMs = 0.0;      // what the driver reports (converters, safety buffers; secondary devices: incl. their buffer)
        double clockSyncMs = 0.0;   // secondary devices: ring + resampler binding them to the master clock (moves slightly)
        int bufferFrames = 0;       // device period
        double deviceRate = 0.0;
        juce::String transport;     // special connection ("Kernel Streaming, cable 2, clock synced to the master")
        bool steeredClock = false;  // its clock follows the master (no resampler)
        double total() const noexcept { return bufferMs + driverMs + clockSyncMs; }
    };
    IoLatency getIoLatency (const IoTarget& t, bool input) const;
    /** Device-side latency of a strip's source / a bus's output in ms (-1 = offline). */
    double getIoLatencyMs (const IoTarget& t, bool input) const;
    /** Latency of the active (not bypassed) plugins of a chain in ms. Message thread. */
    double pluginLatencyMs (const PluginChain&, const Param& bypassAll) const;

    //--------------------------------------------------------------------------
    // Sync Delay: channels of one group (inputs, hardware outputs, virtual outputs) with sync on are delayed
    // so that they all have the latency of the slowest of them (e.g. two microphones on two interfaces).
    // On top of that every channel can have a manual delay (delayMs). Computed here at 10 Hz from the live,
    // smoothed latencies; the audio thread crossfades to a new delay.
    struct SyncState
    {
        double measuredMs = -1.0;   // latency of the channel without its delay (input / output side), -1 = offline
        double autoMs = 0.0;        // automatic part of the delay now applied
        bool member = false;        // sync on and online
        bool reference = false;     // the slowest member (gets no automatic delay)
        int group = 0;              // Group as int
    };
    /** Sync state of an input (0..kMaxInputs-1) or a bus (kMaxInputs + bus). Message thread. */
    const SyncState& getSyncState (int channel) const noexcept { return syncStates[(size_t) channel]; }
    /** Total delay (automatic + manual) the channel gets, in ms. */
    double getChannelDelayMs (int channel) const noexcept;
    static constexpr int busChannel (int bus) noexcept { return kMaxInputs + bus; }
    juce::StringArray getMasterInputNames() const;
    juce::StringArray getMasterOutputNames() const;

    /** ASIO drivers that can run next to the master (every ASIO driver except the master's own). */
    juce::StringArray getSecondaryAsioDrivers() const;
    /** Channel names of a secondary ASIO driver (probed once, cached). Message thread. */
    juce::StringArray getAsioChannelNames (const juce::String& driver, bool inputs) const;
    /** True if t names the ASIO driver that is the master (its channels are then used directly). */
    bool isMasterAsio (const IoTarget& t) const;

    void prepareChains();

    //--------------------------------------------------------------------------
    // Effect chain (message thread). Every channel has one chain of plugins and built-in effects (Fx) in any order.
    // A built-in effect whose on switch is turned on while it is not in the chain (old sessions, import, scenes,
    // undo) is inserted at its usual place (ensureBuiltins, 10 Hz).
    /** Built-in effects a channel can have (selection index scheme: inputs, then buses), in their usual order.
        Since 12.11. all of them on every channel. */
    std::vector<Fx> allowedFx (int channel) const;
    /** The effects made for this kind of channel (the "+" menu of the chain offers only these): inputs everything
        but the correction, hardware outputs correction, utility, EQ, compressor, limiter; virtual outputs without
        the correction. */
    std::vector<Fx> typicalFx (int channel) const;
    /** How many instances of f a channel can have (correction: 1). */
    static int maxInstances (Fx f) noexcept { return f == Fx::stems || f == Fx::transpose || f == Fx::voice ? 1 : kFxInstances; }
    PluginChain& chainFor (int channel) noexcept { return fxOf (channel).plugins; }
    const PluginChain& chainFor (int channel) const noexcept { return fxOf (channel).plugins; }

    /** Parameter id prefix of a channel: "in3.", "bus.A1.", "rt2." (recorder track). */
    static juce::String channelPrefix (int channel);
    /** Name of a channel (input, bus or recorder track). */
    juce::String channelName (int channel) const;
    /** The channel is in use: an active input / bus slot, or a track of the loaded recorder project. */
    bool isChannelActive (int channel) const noexcept;
    /** The on switch of instance k of a built-in effect of a channel (nullptr if the channel cannot have it). */
    Param* fxOnParam (int channel, Fx, int k = 0) const;
    /** Id part of instance k's parameters ("eq" / "eq2", "gate" ..., "util" / "util2", "tp"): ids are
        <channel prefix><part>.<name>. */
    static juce::String fxPart (Fx, int k);
    /** Every parameter of instance k of a built-in effect of a channel (its on switch included). */
    std::vector<Param*> fxParams (int channel, Fx, int k = 0) const;
    /** Adds a new instance of a built-in effect to the chain at index (-1 = its usual place) and switches it on.
        Returns the instance, or -1 if the channel cannot have more of them. */
    int addBuiltin (int channel, Fx, int index = -1);
    /** Removes instance k of a built-in effect from the chain; its settings go back to their defaults and it is
        switched off. */
    void removeBuiltin (int channel, Fx, int k = 0);
    /** Replaces Studio Voice instance k with the real effects it runs (EQ with high pass and cuts, Gate, Compressor, a second fast Compressor for Radio and Singing, EQ with the boosts, Saturation,
        Limiter) at its current settings, in its place in the chain, so it can be fine-tuned. The measured values
        (noise floor, leveling) are frozen at this moment. Stages that do nothing at the current amount are left out.
        Returns an empty string, or why it could not (e.g. no free instance of an effect: then nothing changes). */
    juce::String convertVoice (int channel, int k);
    /** Studio Voice "Learn my mic": listens to the channel until about 8 s of speech were heard (gives up after 40 s),
        then keeps the spectrum and fits the EQ. Message thread. cancel stops listening, forget drops what was learned. */
    void startVoiceLearn (int channel);
    void cancelVoiceLearn (int channel);
    void forgetVoiceLearn (int channel);
    /** Learning and fitting of all Studio Voices (Engine timer; tests call it directly). */
    void updateVoices();
    /** Match function of the EQs (ParamEq::matchData): designs a new filter when the curve changed, switches it off
        when the match is off or incomplete, frees old filters. Engine timer; the EQ window calls it more often while
        its knobs move. */
    void updateEqMatches();
    /** Session / clipboard: EQMATCH element with the captured spectra of EQ instance k (nullptr if there are none). */
    std::unique_ptr<juce::XmlElement> eqMatchToXml (int channel, int k) const;
    void eqMatchFromXml (int channel, int k, const juce::XmlElement* eqmatch);
    /** Inserts the built-in effects that are switched on but not in the chain, removes ones the channel cannot have. */
    void ensureBuiltins();
    /** Creates the RNNoise state of a denoiser instance that does not have one yet (message thread). */
    void prepareDenoiser (DenoiserFx&);
    /** Creates / frees the Stems state of an input when the effect enters / leaves its chain (message thread). */
    void prepareStems (ChannelFx&);
    /** Creates the pitch shifter of an input's Transpose effect (message thread, once). */
    void prepareTranspose (ChannelFx&);
    void releaseStems (ChannelFx&);

    //--------------------------------------------------------------------------
    // Correction (room + speaker / headphone) of a channel, selection index scheme (inputs, then buses).
    // Every channel has one (since 12.11.); measuring and the Correction window are for hardware outputs only
    // (isHwOutput), other channels can use a saved correction.
    Correction* correctionFor (int selectIndex, int k = 0) noexcept
    {
        return isFxChannel (selectIndex) && juce::isPositiveAndBelow (k, kFxInstances)
                   ? &fxOf (selectIndex).correctionAt (k) : nullptr;
    }
    static bool isHwOutput (int selectIndex) noexcept
    {
        const int b = selectIndex - kMaxInputs;
        return juce::isPositiveAndBelow (b, kMaxBuses) && isHwBus (b);
    }
    /** The effect side of a channel (selection index scheme). */
    ChannelFx& fxOf (int channel) noexcept
    {
        if (isRecTrackChannel (channel)) return recTracks[(size_t) (channel - kRecTrackBase)];
        return channel < kMaxInputs ? static_cast<ChannelFx&> (inputs[(size_t) channel]) : buses[(size_t) (channel - kMaxInputs)];
    }
    const ChannelFx& fxOf (int channel) const noexcept
    {
        if (isRecTrackChannel (channel)) return recTracks[(size_t) (channel - kRecTrackBase)];
        return channel < kMaxInputs ? static_cast<const ChannelFx&> (inputs[(size_t) channel]) : buses[(size_t) (channel - kMaxInputs)];
    }
    /** Replaces a channel's correction profile (message thread). Ignored for channels without a correction. */
    void setCorrection (int selectIndex, CorrectionProfile profile, int k = 0);

    /** Headphone Correction of a channel (message thread): makes headphones 'from' sound like 'to' (hp::Library ids,
        empty 'to' = neutral). Fits the EQ; an unknown 'from' leaves the channel without correction. */
    void setHeadphone (int channel, int k, const juce::String& from, const juce::String& to);
    /** Session / clipboard: HPCORR element with from, to and the fitted profile of instance k (nullptr if nothing is set). */
    std::unique_ptr<juce::XmlElement> headphoneToXml (int channel, int k) const;
    void headphoneFromXml (int channel, int k, const juce::XmlElement* hpcorr);
    /** Applies changed AMOUNT knobs and frees retired coefficient sets (Engine timer, 10 Hz). */
    void updateHeadphones();

    //--------------------------------------------------------------------------
    // Room measurement (Tools > Room Measurement), see MeasureRun. Message thread.
    /** Hands a prepared run to the audio thread (a running one is dropped). */
    void startMeasurement (std::unique_ptr<MeasureRun> run);
    /** The current run (playing or finished), or nullptr. */
    MeasureRun* getMeasurement() const noexcept { return measureOwned.get(); }
    /** Fades the sweep out and ends the run early (it then counts as finished, see finishMeasurement). */
    void stopMeasurement();
    /** Ends the run: returns its recording if it played to the end (empty if it was stopped or the engine
        never ran it). The run itself is freed once the audio thread has let go of it. */
    std::vector<float> finishMeasurement();

    /** Measuring with a device that is on no channel: a free (unused) input / bus slot is borrowed. It opens its
        device (choose it with setInputSource / setBusOutput) but is neither shown nor mixed; only a measurement
        records from / plays into it. Returns the slot (input index / bus index), -1 if every slot is in use.
        The same slot is returned again while it is borrowed. */
    int borrowMeasureInput();
    int borrowMeasureBus();
    /** Gives the slot back (its device is closed). */
    void releaseMeasureInput();
    void releaseMeasureBus();
    int getMeasureInput() const noexcept { return measureIn; }
    int getMeasureBus() const noexcept   { return measureBus; }

    /** Live level of a raw source (also a borrowed slot), before any processing: the wizard's microphone step.
        input -1 = off (then it costs one atomic read per block). */
    void setMicLevelTap (int input, int channel) noexcept
    {
        micTapChannel.store (channel, std::memory_order_relaxed);
        micTapInput.store (input, std::memory_order_relaxed);
    }
    /** Highest sample since the last call. */
    float takeMicLevel() noexcept { return micTapPeak.exchange (0.0f); }

private:
    struct Graph
    {
        std::vector<std::shared_ptr<AsyncInput>> ins;
        std::vector<std::shared_ptr<AsyncOutput>> outs;
        // ch / chR: left / right channel. For asynchronous streams they index the stream's channels
        // (chR = -1: the stream has no right channel).
        struct In  { IoTarget::Kind kind = IoTarget::Kind::None; int ch = 0, chR = 1; bool mono = false; AsyncInput* s = nullptr; };
        struct Out { IoTarget::Kind kind = IoTarget::Kind::None; int ch = 0, chR = 1; bool mono = false; AsyncOutput* s = nullptr; };
        std::array<In, kMaxInputs> in;
        std::array<Out, kMaxBuses> out;
    };

    void createParams();
    void closeMaster();
    void publishGraph (std::unique_ptr<Graph> g);
    void timerCallback() override;

    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                           int n, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;
    void audioDeviceError (const juce::String&) override;

    void processChunk (const Graph& g, const float* const* in, int numIn, float* const* out, int numOut, int offset, int n) noexcept;
    void createFxParams (ChannelFx&, const juce::String& idPrefix, const juce::String& namePrefix);
    /** Audio thread: a channel's effect chain in place (stereo: the denoiser treats both sides apart). */
    void runFxChain (ChannelFx&, float* l, float* r, int n, bool timed, bool stereo) noexcept;
    /** Audio thread: the chain is skipped (channel not heard). */
    void pauseFx (ChannelFx&) noexcept;
    bool fxWatched (const ChannelFx&) const noexcept;
    void processStrip (InputStrip& s, const float* srcL, const float* srcR, int n, bool anySolo, bool monoSource, bool timed,
                       const std::array<bool, kMaxBuses>& busOpen) noexcept;
    void processBus (Bus& b, int n, bool timed, bool used) noexcept;
    /** One recorder track through its strip, in place on t.l / t.r (playback and export). */
    void processRecTrack (RecTrack& t, int n, bool anySolo, bool timed) noexcept;
    /** Export thread: the tracks through their strips (Recorder::renderTracks). */
    void renderTracksOffline (float* const* l, float* const* r, int numTracks, int n, bool withMixer) noexcept;
    void resetTrackFx (RecTrack&) noexcept;
    std::atomic<bool> tracksBusy { false };   // the audio thread is processing the recorder tracks
    void updateDropoutLogs();
    void updateOverloadLog();
    void updateSyncDelays();
    void renderMeasurement (Bus& b, MeasureRun& run, int start, int count, int n) noexcept;   // audio thread
    int delaySamples (int channel) const noexcept;   // audio thread

    // Message thread: dropouts per stream as (time in ms, count), pruned after kDropoutWindowSeconds.
    struct DropoutLog { int seen = 0; std::vector<std::pair<juce::int64, int>> events; int unlogged = 0; juce::int64 lastLogged = 0; };
    std::map<const AsyncStream*, DropoutLog> dropoutLogs;
    DropoutLog masterDropouts;

    std::array<SyncState, kMaxInputs + kMaxBuses> syncStates {};
    std::array<std::atomic<float>, kMaxInputs + kMaxBuses> autoDelayMs {};   // read by the audio thread

    juce::OwnedArray<juce::AudioIODeviceType> types;
    std::unique_ptr<juce::AudioIODevice> masterDevice;
    MasterConfig master;
    juce::String lastError;
    double sampleRate = 48000.0;
    int blockSize = 256;
    bool running = false;

    std::unique_ptr<Graph> current;                 // owned; audio thread reads via 'live'
    std::atomic<Graph*> live { nullptr };
    std::vector<std::pair<std::unique_ptr<Graph>, uint64_t>> graveyard; // freed once the audio thread let go
    std::atomic<uint64_t> callbackCount { 0 };
    int selOwner = -1;     // SEL exclusivity (message thread)
    bool anyMissing = false;   // an active channel's device could not be opened (rebuildGraph)
    std::unique_ptr<StreamOpener> opener;           // opens Windows devices in the background
    std::map<juce::String, juce::String> openErrors;   // last failure per stream (it stays shown during the next try)
    /** Asks for a stream to be opened; a device plugged in after the last scan is looked for first. */
    void requestOpen (const IoTarget&, bool input);
    int recoverTicks = 0, masterQuietTicks = 0;
    // Automatic restart of the master after a driver error (audioDeviceError, any thread) or a stall. Restarts in
    // quick succession wait longer each time (0.5 s, 1, 2, 4 ... up to 60 s) so a broken driver cannot loop.
    std::atomic<bool> masterErrorPending { false };
    int masterRestartIn = -1;          // timer ticks (10 Hz) until the restart, -1 = none planned
    int masterRestartStreak = 0;       // restarts within a minute of each other
    double lastMasterRestartMs = -1.0e9;
    void restartMaster (const juce::String& reason);
    uint64_t masterSeenCallbacks = 0;
    std::vector<float> playL, playR;
    std::array<float*, kMaxRecTracks> trackL {}, trackR {};
    std::array<float, kMaxBuses> lastPlayRoute {};

    std::vector<std::pair<std::unique_ptr<stems::Instance>, uint64_t>> stemsGraveyard;   // freed once the audio thread let go

    std::unique_ptr<MeasureRun> measureOwned;     // message thread; the audio thread reads 'measureLive'
    std::atomic<MeasureRun*> measureLive { nullptr };
    std::vector<std::pair<std::unique_ptr<MeasureRun>, uint64_t>> measureGraveyard;
    int measureIn = -1, measureBus = -1;   // borrowed slots (see borrowMeasureInput), message thread
    std::atomic<int> micTapInput { -1 }, micTapChannel { 0 };
    std::atomic<float> micTapPeak { 0.0f };

    std::atomic<float> cpuLoad { 0 };
    std::atomic<float> peakWindow { 0 }, peakLast { 0 }, overloadPeak { 0 };   // audio thread writes (see getRecentPeak)
    std::atomic<uint64_t> overloads { 0 };
    int peakFrames = 0;                                                        // audio thread
    uint64_t overloadsLogged = 0;                                              // message thread
    double overloadLogTime = -1.0e9;
    std::atomic<uint64_t> busyTicks { 0 }, busyFrames { 0 };   // always counted (see LoadMeter), audio thread writes
    double ticksToSeconds = 1.0;
    std::vector<float> silence;
};
} // namespace hydra

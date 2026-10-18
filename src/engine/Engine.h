#pragma once
#include "core/Params.h"
#include "core/Dsp.h"
#include "core/ParamEq.h"
#include "core/Denoiser.h"
#include "IoStreams.h"
#include "PluginChain.h"
#include "Recorder.h"

#include <map>

namespace hydra
{
struct Meter
{
    std::atomic<float> l { 0 }, r { 0 };
    std::atomic<bool> muted { false }; // levels are what the channel would play if it were not muted (UI draws them grey)
    void push (float a, float b) noexcept
    {
        if (a > l.load (std::memory_order_relaxed)) l.store (a, std::memory_order_relaxed);
        if (b > r.load (std::memory_order_relaxed)) r.store (b, std::memory_order_relaxed);
    }
    std::pair<float, float> take() noexcept { return { l.exchange (0.0f), r.exchange (0.0f) }; }
};

/** Feeds a channel's signal (after its EQ) to the spectrum analyzer of the large EQ window.
    Inactive unless a view is open; then the audio thread writes into a wait-free ring. */
struct SpectrumTap
{
    std::atomic<int> users { 0 };   // open analyzer views
    SpscAudioRing ring;
    void push (const float* l, const float* r, int n) noexcept
    {
        if (users.load (std::memory_order_relaxed) <= 0) return;
        const float* src[2] = { l, r };
        ring.write (src, 2, n);
    }
};

/** CPU time spent in each processing stage of a channel, for the DSP view. The audio thread only measures
    while a view is open (Engine::profileUsers > 0). Counters accumulate high-resolution ticks and are
    never reset: the view takes differences between two readings. Single writer (audio thread). */
struct DspTimes
{
    enum Stage { denoiser, eq, gate, comp, plugins, limiter, other, numStages };
    std::array<std::atomic<uint64_t>, numStages> ticks {};
    void add (Stage s, int64_t t) noexcept
    {
        auto& a = ticks[(size_t) s];
        a.store (a.load (std::memory_order_relaxed) + (uint64_t) juce::jmax ((int64_t) 0, t), std::memory_order_relaxed);
    }
    uint64_t get (Stage s) const noexcept { return ticks[(size_t) s].load (std::memory_order_relaxed); }
};

//==============================================================================
struct InputStrip
{
    int index = 0;
    juce::String name;
    IoTarget source;
    PluginChain plugins;
    Meter meter, preMeter;
    SpectrumTap spectrum;
    std::atomic<float> compGr { 0 };

    // parameters
    Param *active {};   // slot in use (shown as a strip and processed); see Engine::addInput / removeInput
    Param *gain {}, *trim {}, *pan {}, *mute {}, *solo {}, *mono {}, *phase {};
    ParamEq eq;
    Param *gateOn {}, *gateThr {}, *gateRange {}, *gateAttack {}, *gateHold {}, *gateRelease {}, *gateScOn {}, *gateScFreq {}, *gateVoice {}, *gateVoiceThr {};
    Param *compOn {}, *compInput {}, *compThr {}, *compRatio {}, *compKnee {}, *compAttack {}, *compRelease {}, *compMakeup {}, *compAuto {};
    Param *dnOn {}, *dnAmount {}, *dnMode {};
    Param *fxBypass {};
    Param *syncOn {}, *delayMs {};   // Sync Delay: automatic alignment with the input sync group / manual delay
    std::array<Param*, kMaxBuses> route {};
    std::array<Param*, kMaxBuses> send {};   // per-bus fader level (edited while a bus is selected with SEL)

    dsp::GateSettings gateSettings() const;
    dsp::CompSettings compSettings() const;
    dsp::DenoiserSettings denoiserSettings() const;

    /** Processing latency added inside this strip (denoiser + plugins), in samples. */
    std::atomic<int> dspLatency { 0 };
    DspTimes dspTimes;

    // audio-thread state
    dsp::Gate gate;
    dsp::Compressor comp;
    dsp::DelayLine delay;
    std::unique_ptr<dsp::Denoiser> denoiser = std::make_unique<dsp::Denoiser>();
    bool dnWasOn = false;
    float lastTrim = 1, lastL = 0, lastR = 0;
    std::array<float, kMaxBuses> lastRoute {};   // last applied send gain per bus (0 = not routed)
    std::vector<float> l, r;
};

struct Bus
{
    int index = 0;
    juce::String name;
    IoTarget output;
    PluginChain plugins;
    Meter meter;
    SpectrumTap spectrum;
    DspTimes dspTimes;

    Param *active {};   // slot in use (shown as a strip and processed); see Engine::addBus / removeBus
    Param *gain {}, *mute {}, *mono {}, *limOn {}, *limCeil {}, *fxBypass {};
    Param *sel {};   // SEL: input faders edit the sends to this bus (exclusive), optional monitoring
    Param *syncOn {}, *delayMs {};   // Sync Delay (see InputStrip)
    ParamEq eq;

    dsp::Limiter limiter;
    dsp::DelayLine delay;
    float lastGain = 0;
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
    Recorder recorder;

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

    // Status
    float getCpuLoad() const noexcept { return cpuLoad.load(); }

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
    void processStrip (InputStrip& s, const float* srcL, const float* srcR, int n, bool anySolo, bool monoSource, bool timed) noexcept;
    void processBus (Bus& b, int n, bool timed) noexcept;
    void updateDropoutLogs();
    void updateSyncDelays();
    int delaySamples (int channel) const noexcept;   // audio thread

    // Message thread: dropouts per stream as (time in ms, count), pruned after kDropoutWindowSeconds.
    struct DropoutLog { int seen = 0; std::vector<std::pair<juce::int64, int>> events; };
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
    std::vector<float> playL, playR;
    std::array<float, kMaxBuses> lastPlayRoute {};

    std::atomic<float> cpuLoad { 0 };
    double ticksToSeconds = 1.0;
    std::vector<float> silence;
};
} // namespace hydra

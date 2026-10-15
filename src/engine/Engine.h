#pragma once
#include "core/Params.h"
#include "core/Dsp.h"
#include "core/ParamEq.h"
#include "core/Denoiser.h"
#include "IoStreams.h"
#include "PluginChain.h"
#include "Recorder.h"

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
    std::array<Param*, kMaxBuses> route {};
    std::array<Param*, kMaxBuses> send {};   // per-bus fader level (edited while a bus is selected with SEL)

    dsp::GateSettings gateSettings() const;
    dsp::CompSettings compSettings() const;
    dsp::DenoiserSettings denoiserSettings() const;

    /** Processing latency added inside this strip (denoiser + plugins), in samples. */
    std::atomic<int> dspLatency { 0 };

    // audio-thread state
    dsp::Gate gate;
    dsp::Compressor comp;
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

    Param *active {};   // slot in use (shown as a strip and processed); see Engine::addBus / removeBus
    Param *gain {}, *mute {}, *mono {}, *limOn {}, *limCeil {}, *fxBypass {};
    Param *sel {};   // SEL: input faders edit the sends to this bus (exclusive), optional monitoring
    ParamEq eq;

    dsp::Limiter limiter;
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
    double getSampleRate() const noexcept { return sampleRate; }
    int getBlockSize() const noexcept { return blockSize; }
    double getMasterLatencyMs() const;
    juce::String getStreamStatus (const IoTarget& t, bool input) const;
    /** Device-side latency of a strip's source / a bus's output in ms (-1 = offline). */
    double getIoLatencyMs (const IoTarget& t, bool input) const;
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
    void processStrip (InputStrip& s, const float* srcL, const float* srcR, int n, bool anySolo, bool monoSource) noexcept;
    void processBus (Bus& b, int n) noexcept;

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

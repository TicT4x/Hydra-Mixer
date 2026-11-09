#pragma once
#include <list>
#include "core/Resampler.h"

namespace hydra
{
/** Where a strip gets its audio from / where a bus sends its audio to. */
struct IoTarget
{
    enum class Kind { None = 0, Master, Device, Loopback, Virtual, App, Network };   // numbers are stored in session.xml

    Kind kind = Kind::None;
    juce::String typeName;   // JUCE device type ("Windows Audio (Low Latency Mode)", ...)
    juce::String deviceName; // device / endpoint friendly name, the .exe names for Kind::App ("a.exe|b.exe", see apps()),
                             // Kind::Network: "<stream>@<sender>" (input) or "hydralink://host:port/stream?..." (output, net::Destination)
    int channel = 0;         // first channel of the stereo pair (0-based)
    bool mono = false;       // use a single channel, duplicated to L/R
    int virtualIndex = 0;    // 0..7 (Virtual)

    bool operator== (const IoTarget& o) const
    {
        return kind == o.kind && typeName == o.typeName && deviceName == o.deviceName
            && channel == o.channel && mono == o.mono && virtualIndex == o.virtualIndex;
    }
    bool operator!= (const IoTarget& o) const { return ! operator== (o); }

    /** Same underlying stream (ignores channel selection). */
    bool sameStream (const IoTarget& o) const
    {
        return kind == o.kind && typeName == o.typeName && deviceName == o.deviceName && virtualIndex == o.virtualIndex;
    }

    juce::String describe() const;
    /** Kind::App: the captured programs (one or more, mixed on one channel). */
    juce::StringArray appNames() const;
    /** Kind::App target capturing these programs (sorted: the same set is the same stream); None if empty. */
    static IoTarget apps (juce::StringArray exes);
    std::unique_ptr<juce::XmlElement> toXml (const juce::String& tag) const;
    static IoTarget fromXml (const juce::XmlElement* e);
};

/** JUCE device type name of ASIO drivers. */
inline const char* const kAsioTypeName = "ASIO";

/** Shared state for asynchronous (non-master-clock) streams. */
class AsyncStream
{
public:
    virtual ~AsyncStream() = default;

    IoTarget target;               // identity (channel/mono ignored)
    std::atomic<int> underruns { 0 };
    std::atomic<int> latencyFrames { 0 }; // current buffered latency in device frames
    std::atomic<bool> failed { false };
    std::atomic<bool> idle { false };      // e.g. captured app is not running
    std::atomic<uint64_t> dspTicks { 0 };  // engine-thread time (resampling, drift control) while profiling
    std::atomic<uint32_t> deviceCallbacks { 0 };   // device-thread callbacks so far (watchdog: a stalled device has gone)
    juce::String error;

    /** Device channels this stream carries, ascending (secondary ASIO drivers carry only the
        routed channels). Empty = device channels 0..n-1. */
    std::vector<int> deviceChannels;
    int deviceChannelCount = 0;           // channels the device has (only set with deviceChannels)
    int deviceLatencyFrames = 0;          // fixed latency the driver reports (incl. its buffer), in device frames
    juce::String transport;               // how it is connected, if special ("Kernel Streaming, cable 2, clock synced ...")

    double getDeviceRate() const noexcept { return deviceRate; }
    int getDeviceBlock() const noexcept   { return deviceBlock; }
    int getNumChannels() const noexcept   { return channels; }

    /** Stream channel that carries a device channel, or -1. */
    int localChannel (int deviceChannel) const noexcept
    {
        if (deviceChannels.empty()) return deviceChannel < channels ? deviceChannel : -1;
        for (size_t i = 0; i < deviceChannels.size(); ++i)
            if (deviceChannels[i] == deviceChannel) return (int) i;
        return -1;
    }

    /** True if every wanted device channel (that the device has) is carried by this stream. */
    bool carries (const std::vector<int>& wanted) const noexcept
    {
        if (deviceChannels.empty()) return true;
        for (int c : wanted)
            if (c < deviceChannelCount && localChannel (c) < 0) return false;
        return true;
    }

    /** Message thread, ~10 Hz (watchdogs). */
    virtual void poll() {}

    /** Direct mode: the device runs at the engine rate and its clock can be steered (Virtual Audio Cable over
        Kernel Streaming). No resampler: the drift controller's correction goes to the device clock instead
        (clockRatio, read by the stream's poll()). */
    bool isDirect() const noexcept { return direct; }
    /** Direct mode: how much faster the device should run (1.0 = as is), written once per engine block. */
    std::atomic<double> clockRatio { 1.0 };

protected:
    SpscAudioRing ring;
    VarResampler resampler;
    DriftController drift;
    double deviceRate = 48000.0, engineRate = 48000.0;
    int deviceBlock = 480, engineBlock = 256, channels = 2;
    bool direct = false;   // set by the derived class before setup()
    bool preferStable = false;   // virtual cable / app capture: the buffer never shrinks back into a dropout (setup())
    double jitterFloor = 0.0;    // inputs: the buffer never tightens below one engine block + this (device frames; network jitter)
    double minWindowSeconds = 0.0;   // inputs: the drift controller's fill minimum spans at least this long (rare bursts, network)
    std::array<std::vector<float>, kMaxStreamChannels> buf;
    float* bufPtrs[kMaxStreamChannels] {};

    void allocBuffers()
    {
        for (int c = 0; c < kMaxStreamChannels; ++c)
        {
            buf[(size_t) c].assign (c < channels ? (size_t) kMaxBlock : 0, 0.0f);
            bufPtrs[c] = c < channels ? buf[(size_t) c].data() : nullptr;
        }
    }
};

//==============================================================================
/** Device -> engine. */
class AsyncInput : public AsyncStream
{
public:
    /** Engine thread, once per chunk: fills internal buffers with n engine-rate frames. */
    void pullBlock (int n) noexcept;
    const float* channel (int c) const noexcept { return bufPtrs[juce::jlimit (0, juce::jmax (0, channels - 1), c)]; }

    virtual void start() = 0;
    virtual void stop() = 0;

protected:
    /** Call from derived class once the device format is known. */
    void setup (double devRate, int devBlock, int numChannels, double engRate, int engBlock);
    void produce (const float* const* data, int numCh, int n) noexcept
    {
        ring.write (data, numCh, n);
        deviceCallbacks.fetch_add (1, std::memory_order_relaxed);
    }
    void produceInterleaved (const float* data, int numCh, int n) noexcept { ring.writeInterleaved (data, numCh, n); }

private:
    bool primed = false;
    double slipMargin = 0;  // device frames kept for clock slips (see setup)
};

/** Engine -> device. */
class AsyncOutput : public AsyncStream
{
public:
    /** Engine thread: buses add into these buffers, then pushBlock() resamples to the device ring. */
    float* mixChannel (int c) noexcept { return bufPtrs[juce::jlimit (0, juce::jmax (0, channels - 1), c)]; }
    void clearMix (int n) noexcept     { for (int c = 0; c < channels; ++c) std::memset (bufPtrs[c], 0, (size_t) n * sizeof (float)); }
    void pushBlock (int n) noexcept;

    virtual void start() = 0;
    virtual void stop() = 0;

protected:
    void setup (double devRate, int devBlock, int numChannels, double engRate, int engBlock);
    /** Device thread. */
    void consume (float* const* out, int numCh, int n) noexcept;

private:
    std::atomic<int> primeTarget { 0 };
    std::atomic<bool> devUnderrun { false };
    double minSafeFill = 0;  // device frames the ring must hold after an engine block
    bool primed = false; // device thread
};

//==============================================================================
/** Factory: opens the right kind of stream for an IoTarget. Returns nullptr + error on failure.
    asioChannels: for a secondary ASIO driver, the device channels the stream must carry. */
std::shared_ptr<AsyncInput>  openAsyncInput  (const IoTarget& t, juce::OwnedArray<juce::AudioIODeviceType>& types,
                                              double engineRate, int engineBlock, juce::String& error,
                                              const std::vector<int>& asioChannels = {});
std::shared_ptr<AsyncOutput> openAsyncOutput (const IoTarget& t, juce::OwnedArray<juce::AudioIODeviceType>& types,
                                              double engineRate, int engineBlock, juce::String& error,
                                              const std::vector<int>& asioChannels = {});

//==============================================================================
/** Secondary ASIO drivers (AsioSecondary.cpp). A driver is opened once with all of its channels;
    its input and output streams attach to that one device, which runs on its own clock and is
    bound to the master like any other secondary device (ring + resampler + drift control). */
std::shared_ptr<AsyncInput>  openAsioInput  (juce::AudioIODeviceType& type, const juce::String& driver, double engineRate,
                                             int engineBlock, const std::vector<int>& channels, juce::String& error);
std::shared_ptr<AsyncOutput> openAsioOutput (juce::AudioIODeviceType& type, const juce::String& driver, double engineRate,
                                             int engineBlock, const std::vector<int>& channels, juce::String& error);
/** Channel names of an ASIO driver: live names if it is open as a secondary, otherwise probed
    once (loads the driver briefly) and cached. Empty if the driver cannot be loaded. Message thread. */
juce::StringArray asioChannelNames (juce::AudioIODeviceType& type, const juce::String& driver, bool inputs);

//==============================================================================
/** Opens and closes Windows streams (WASAPI, loopback, per-app capture) on its own thread: this can take a long
    time (Bluetooth, a device that is busy) and must not freeze the UI. Secondary ASIO drivers stay on the message
    thread, they expect to be loaded there. The engine asks for a stream, keeps building its graph without it and
    takes it once it is finished (hasFinished, polled at 10 Hz). A finished stream is not started yet.

    It uses the engine's device types read-only. JUCE can rescan a type on the message thread when Windows reports a
    device change; that replaces the type's name lists, a race only with the few reads at the very start of opening. */
class StreamOpener final : private juce::Thread
{
public:
    explicit StreamOpener (juce::OwnedArray<juce::AudioIODeviceType>& types);
    ~StreamOpener() override;

    // All message thread.
    void request (const IoTarget&, bool input, double engineRate, int engineBlock);   // nothing if already requested
    bool isPending (const IoTarget&, bool input) const;   // requested, not finished yet
    bool hasFinished() const noexcept { return numFinished.load() > 0; }
    /** True if a finished result was there: the stream, or nullptr and the error. */
    bool takeInput (const IoTarget&, double engineRate, int engineBlock, std::shared_ptr<AsyncInput>&, juce::String& error);
    bool takeOutput (const IoTarget&, double engineRate, int engineBlock, std::shared_ptr<AsyncOutput>&, juce::String& error);
    /** Finished streams nobody wants any more (another device was chosen meanwhile) are closed. */
    void discardFinishedExcept (const std::vector<IoTarget>& wantedIn, const std::vector<IoTarget>& wantedOut, double engineRate, int engineBlock);
    /** Closes a stream that is no longer used, on the opener's thread. */
    void dispose (std::shared_ptr<void> stream);

private:
    struct Job
    {
        enum class State { queued, opening, done };
        IoTarget target;
        bool input = false;
        double rate = 0.0;
        int block = 0;
        State state = State::queued;
        std::shared_ptr<AsyncInput> in;
        std::shared_ptr<AsyncOutput> out;
        juce::String error;
        bool matches (const IoTarget& t, bool isInput, double r, int b) const { return input == isInput && rate == r && block == b && target.sameStream (t); }
    };

    void run() override;

    juce::OwnedArray<juce::AudioIODeviceType>& types;
    juce::CriticalSection lock;
    std::list<Job> jobs;
    std::vector<std::shared_ptr<void>> trash;
    std::atomic<int> numFinished { 0 };
};

/** Friendly names of active Windows render endpoints (for loopback capture). */
juce::StringArray listRenderEndpoints();

/** Executable names of processes that currently have an audio session (for per-app capture).
    The thread must have COM initialised. */
juce::StringArray listAudioApps();
/** The same in the background (it asks every playback device); done is called on the message thread. */
void listAudioAppsAsync (std::function<void (juce::StringArray)> done);

/** A virtual audio cable (VB-Cable, Virtual Audio Cable, Hi-Fi Cable ...): apps play into
    `playbackName`, Hydra captures `recordingName` (or the other way round for B buses). */
struct VirtualCable
{
    juce::String label, playbackName, recordingName;
};
std::vector<VirtualCable> findVirtualCables (juce::OwnedArray<juce::AudioIODeviceType>& types);
/** Same, from the friendly names of the playback endpoints (apps play into these) and the recording
    endpoints (Hydra captures these). */
std::vector<VirtualCable> findVirtualCables (const juce::StringArray& playback, const juce::StringArray& recording);
} // namespace hydra

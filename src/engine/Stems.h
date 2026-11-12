#pragma once
#include "Common.h"

#include <mutex>

/** Stems: live source separation of a channel into vocals, drums, bass, guitar and the rest (10.11.).

    Two models, both on the GPU through ONNX Runtime + DirectML, each on its own worker thread shared by all channels:
    - BS-RoFormer SW (6 stems; piano and "other" become part of the rest): 4-s windows at 44.1 kHz, modes Fast /
      Balanced / Best. The best separation, also of guitar; ~45 % of an RTX 5060 per channel.
    - HT-Demucs (4 stems, no guitar: it is part of the rest): 7.8-s windows, mode Live. Less latency (~1.3 s) and
      ~8 x cheaper per second than BS-RoFormer, a little less clean.
    The audio thread never waits for a model: it writes the channel into a ring, delays the dry signal by the effect's
    latency and mixes the stems the worker has finished for exactly that time. Rest = input - (vocals + drums + bass
    + guitar), so with every gain at 0 dB the output is the input (delayed): nothing of the signal is lost.

    Streaming: the worker computes one window per hop. Of each window only a part (hop + crossfade) is played, with a
    right-hand context of "look-ahead" seconds: the model sees what comes after the part that is played.
    Latency = hop + crossfade + look-ahead + compute budget. If a worker misses a deadline (GPU busy), the effect
    plays the dry signal for that part instead of a dropout. */
namespace hydra::stems
{
enum Stem { vocals = 0, drums, bass, guitar, rest, numStems };
juce::String stemName (int s);   // "Vocals" ...
constexpr int kModelStems = 4;   // vocals, drums, bass, guitar come from the model; the rest is what is left

enum class Model : int { roformer = 0, demucs, count };
constexpr int kNumModels = (int) Model::count;
struct ModelInfo
{
    const char* name;        // shown
    const char* file;        // in %APPDATA%\HydraMixer\Models (after the DirectML fixes)
    const char* url;         // download
    const char* sha256;      // of the download
    int64_t bytes;           // of the download
    int samples;             // fixed window at 44.1 kHz
    bool guitar;             // separates guitar
    const char* license;     // shown
    /** Folded models (foldModel, loads faster): the patched, unfolded file. Downloads pass through it; one left by an
        older Hydra is folded once when the model loads next. nullptr: the model is used as patched. */
    const char* unfolded = nullptr;
};
const ModelInfo& modelInfo (Model);

/** The release is one exe: onnxruntime.dll and DirectML.dll are embedded in it (RCDATA 201/202, CMakeLists.txt).
    Unpacks them once into base/ort-<size> (skipped when already there with the right size) and returns that
    folder; an empty File if this exe has none embedded (then error stays empty) or unpacking failed (error set).
    Only with HYDRA_STEMS. */
juce::File extractRuntime (const juce::File& base, juce::String& error);

/** Latency modes (choice parameter values; the Live mode was added last so older sessions keep their mode). */
juce::StringArray modeNames();   // Fast, Balanced, Best, Live
constexpr int kBestMode = 2, kLiveMode = 3, kNumModes = 4;
constexpr int kDefaultMode = kLiveMode;   // Live by default (lightest on the GPU; User 10.11.)
/** Modes in the order the UI shows them. */
inline std::vector<int> modeOrder() { return { kLiveMode, 0, 1, 2 }; }
Model modelFor (int mode) noexcept;
inline bool hasGuitar (int mode) noexcept { return modelInfo (modelFor (mode)).guitar; }
struct Timing
{
    double hop = 1.0, fade = 0.25, lookAhead = 1.375, budget = 1.0;   // seconds
    double latency() const noexcept { return hop + fade + lookAhead + budget; }
};
/** Auto: the fixed timing of each mode (Live 1.3 s ... Best 3.6 s). */
Timing timingFor (int mode) noexcept;

/** How much of the GPU one channel may use (a setting per model). More = a window more often = less latency.
    automatic: the fixed timing of the mode (timingFor). */
struct Tuning
{
    bool automatic = true;
    float share = 0.5f;   // 0.1 .. 1: GPU time / real time for one channel
    bool operator== (const Tuning& o) const noexcept { return automatic == o.automatic && (automatic || std::abs (share - o.share) < 0.005f); }
};
/** Timing for a mode with a tuning, from the model's measured time per window (modelMs; <= 0: a typical value). */
Timing timingFor (int mode, const Tuning&, double modelMs) noexcept;
/** Typical time per window when nothing was measured yet (RTX 5060, 10.11.). */
double typicalMs (Model) noexcept;

/** Separates the windows (one model run). Real: an ONNX model; tests can plug in a fake. Worker thread. */
struct Separator
{
    virtual ~Separator() = default;
    static constexpr int kRate = 44100;
    /** Window length at 44.1 kHz. */
    virtual int samples() const = 0;
    /** in: [2 channels][samples()]. out: [kModelStems][2][samples()] (vocals, drums, bass, guitar). False on failure. */
    virtual bool separate (const float* const* in, float* const* out) = 0;
    juce::String lastError;
};

class Service;

/** How one stem goes into the channel's mix: out L = ll * L + lr * R, out R = rl * L + rr * R
    (its gain, and with the advanced options: left / right side muted, mono, pan). */
struct StemMix
{
    float ll = 1.0f, lr = 0.0f, rl = 0.0f, rr = 1.0f;
    static StemMix gain (float g) noexcept { return { g, 0.0f, 0.0f, g }; }
};

/** The effect state of one channel. Created / prepared on the message thread, processed on the audio thread, filled by
    a worker. */
class Instance
{
public:
    Instance();
    ~Instance();

    /** Allocates for a sample rate and starts over (message thread; the audio thread must not process meanwhile). */
    void prepare (double sampleRate);

    /** Audio thread. In place. gains: linear gain per Stem (rest included). mode: latency mode. tuning: GPU setting of
        the mode's model (a change is taken once it has stayed for 0.4 s: it starts the stream over). restart: the
        effect was off (or the chain bypassed) since the last block, so the stream starts over. peaks (readers x
        numStems, or nullptr): raises each to the stem's peak (before its gain) while the stems are mixed in. */
    void process (float* l, float* r, int n, const float* gains, int mode, bool restart, const Tuning& tuning = {},
                  std::atomic<float>* peaks = nullptr, int readers = 1) noexcept;
    /** The same with a 2x2 mix per stem (side mutes, mono, pan: the advanced options; numStems entries). */
    void process (float* l, float* r, int n, const StemMix* mix, int mode, bool restart, const Tuning& tuning = {},
                  std::atomic<float>* peaks = nullptr, int readers = 1) noexcept;

    /** Latency of the output right now (0 while the separation does not run, e.g. no model yet). Any thread. */
    int getLatencySamples() const noexcept { return latency.load (std::memory_order_relaxed); }
    /** Samples played dry although the separation runs (the GPU was late), counted since the start. */
    uint64_t getMissedSamples() const noexcept { return missed.load (std::memory_order_relaxed); }
    /** Seconds between two windows of the running stream (0 before it starts): GPU share = time per window / this. */
    double getHopSeconds() const noexcept { return hopSeconds.load (std::memory_order_relaxed); }
    double getSampleRate() const noexcept { return sampleRate; }
    /** Timing of the running stream in samples (hop, crossfade, look-ahead, compute budget). */
    std::array<int64_t, 4> getFrames() const noexcept
    {
        return { hopF.load (std::memory_order_relaxed), fadeF.load (std::memory_order_relaxed),
                 lookF.load (std::memory_order_relaxed), budgetF.load (std::memory_order_relaxed) };
    }
    /** True while stems are mixed in (not only the delayed dry signal). */
    bool isWet() const noexcept { return wetShown.load (std::memory_order_relaxed); }
    /** True from a (re)start of the stream until stems are mixed in for the first time (the model hears the first
        window meanwhile; the channel plays the delayed dry signal). */
    bool isStarting() const noexcept { return ! everWet.load (std::memory_order_relaxed); }
    /** While starting: seconds until the separated sound begins (estimate). */
    double secondsUntilWet() const noexcept
    {
        const double left = (double) (latency.load (std::memory_order_relaxed) - writtenShared.load (std::memory_order_relaxed));
        return juce::jmax (0.0, left / sampleRate);
    }
    /** Mode of the running stream (-1 before the first block). */
    int getMode() const noexcept { return (int) (epochInfo.load (std::memory_order_relaxed) & 0xff) - 1; }
    /** Blocks processed since prepare: the service sees from it whether the effect still runs (off, bypassed or no
        audio = the count stands still, and the model is freed). */
    uint32_t getBlocks() const noexcept { return blocks.load (std::memory_order_relaxed); }

private:
    friend class Service;
    static constexpr int kStemChannels = 2 * kModelStems;   // stem ring: each stem L/R

    float* in (int ch) noexcept { return inBuf.data() + (size_t) ch * (size_t) inSize; }
    float* stem (int c) noexcept { return stemBuf.data() + (size_t) c * (size_t) stemSize; }
    void restartStream (int mode, const Tuning&) noexcept;   // audio thread

    // Set by prepare.
    double sampleRate = 48000.0;
    int hwIn = 0;   // input samples the resampler to 44.1 kHz needs beyond a window
    std::vector<float> inBuf, stemBuf;   // input ring (2 channels, also the dry delay line), stems ring
    int64_t inSize = 0, inMask = 0, stemSize = 0, stemMask = 0;

    // Audio thread.
    int64_t written = 0;             // stream samples written into the input ring since the epoch started
    uint32_t epoch = 0;
    int mode = -1, latencyNow = 0;
    float wetLast = 0.0f;
    std::array<StemMix, numStems> lastMix {};
    bool wasReady = false;
    Tuning tuningNow, tuningPending;
    int64_t pendingSince = 0;        // samples the pending tuning has stayed unchanged

    // Audio thread -> worker. epochInfo = epoch << 8 | (mode + 1), then written (see Stems.cpp: read order). The
    // timing of the epoch (frames) is written while epochInfo says "changing" (mode 0), like a sequence lock.
    std::atomic<uint32_t> epochInfo { 0 };
    std::atomic<int64_t> writtenShared { 0 };
    std::atomic<int64_t> hopF { 0 }, fadeF { 0 }, lookF { 0 }, budgetF { 0 };
    std::atomic<double> hopSeconds { 0.0 };
    // Worker -> audio thread: stems are valid in [validFrom, ready) of epoch readyEpoch.
    std::atomic<int64_t> ready { 0 }, validFrom { 0 };
    std::atomic<uint32_t> readyEpoch { ~0u };
    std::atomic<int> latency { 0 };
    std::atomic<uint64_t> missed { 0 };
    std::atomic<bool> wetShown { false }, everWet { false };
    std::atomic<uint32_t> blocks { 0 };

    // Service (under listLock): when the block count last moved.
    uint32_t seenBlocks = 0;
    double seenAt = -1.0e9;

    // Worker (under workLock).
    std::mutex workLock;
    uint32_t workEpoch = ~0u;
    int64_t nextJob = 0, lastDone = -2;
};

/** Models + their worker threads (one per process). */
class Service
{
public:
    static Service& get();
    /** Stops the workers and unloads the models (app shutdown, end of the tests). */
    static void shutdown();
    ~Service();

    enum class State { unavailable, noModel, downloading, preparing, loading, ready, failed };
    struct Status
    {
        State state = State::noModel;
        float progress = 0.0f;           // downloading / preparing: 0..1
        juce::String message;            // failed: why; otherwise details
        juce::String device;             // GPU the model runs on
        double inferenceMs = 0.0;        // average time of one model run (one window)
        int channels = 0;                // channels using this model now
    };
    Status getStatus (Model) const;
    /** GPU time / real time of all channels together, measured over the last seconds (1 = the GPU cannot keep up;
        0 while nothing separates, e.g. the effect is off or the input is silent). Any thread. */
    double getLoad() const;
    /** Share of the GPU one channel needs in a mode with a tuning (from the measured time per window). */
    double loadOf (int mode, const Tuning& = {}) const;
    /** Share of the GPU one channel's running stream needs (0 while it does not run). */
    double loadOf (const Instance&) const;
    /** State::unavailable message on PCs without a dedicated graphics card (Stems is not loaded there). */
    static constexpr const char* kNoGpuMessage = "No dedicated graphics card found. Stem separation needs one (NVIDIA, AMD or Intel Arc) and is not loaded";
    /** Message thread: no dedicated graphics card, nothing will load (set by the check at construction). */
    void markNoGpu();

    /** Seconds a model stays loaded after the last channel stopped using it. */
    static constexpr double kUnloadSeconds = 15.0;
    /** True while the model is loaded and separates (audio thread reads this every block). */
    static bool isReady (Model m) noexcept { return readyFlags[(size_t) m].load (std::memory_order_acquire); }
    /** Time per window measured when the model was loaded (0 = not loaded yet). Any thread. */
    static double referenceMs (Model m) noexcept { return refMs[(size_t) m].load (std::memory_order_relaxed); }

    /** The model file that is there (the folded one, else an unfolded one from an older Hydra); the folded name if
        none is there yet. */
    static juce::File modelFile (Model);
    /** Downloads, checks and prepares a model (its worker thread); then loads it if a channel uses it. */
    void startDownload (Model);
    void cancelDownload (Model);
    /** Deletes a model's files (unloads it first). */
    void removeModel (Model);

    // Channels (message thread).
    void add (Instance&);
    void remove (Instance&);

    /** Tests: use this separator instead of the ONNX model (nullptr = back to the model). */
    void setSeparatorForTests (Model, std::unique_ptr<Separator>);
    /** Tests: load the model from this file instead of modelFile(). */
    void setModelFileForTests (Model, const juce::File&);

private:
    Service();
    struct Slot;
    class Worker;
    void run (Slot&);   // worker loop of one model
    bool runJob (Slot&, Instance&, int64_t job, uint32_t epoch, int mode);
    void ensureModel (Slot&);
    void doDownload (Slot&);
    void setState (Slot&, State, const juce::String& message = {}, float progress = 0.0f);
    bool inUse (Model);
    /** Mode of a channel whose effect runs (its block count moved in the last 0.5 s), else -1. Caller holds listLock. */
    int activeMode (Instance&) const;
    void startWorkers();

    static inline std::array<std::atomic<bool>, kNumModels> readyFlags {};
    static inline std::array<std::atomic<double>, kNumModels> refMs {};
    mutable std::mutex statusLock;
    std::array<std::unique_ptr<Slot>, kNumModels> slots;
    std::mutex listLock;
    std::vector<Instance*> instances;
};

/** Tests: one run of an ONNX file on the calling thread (own session, DirectML or CPU). inputs / outputs: name, shape,
    data (outputs are resized). False (and error) if it cannot run. */
struct TensorData { const char* name; std::vector<int64_t> shape; std::vector<float> data; };
bool runOnnxForTests (const juce::File& model, bool cpu, const std::vector<TensorData>& inputs, std::vector<TensorData>& outputs,
                      juce::String& error);

/** Writes `in` with its constant parts computed once (ONNX Runtime's basic, provider-independent graph optimizations:
    constant folding, shape arithmetic, redundant nodes) to `out`. The model computes exactly the same; it only has far
    fewer nodes, so creating a session is faster (HT-Demucs: ~25 000 -> ~1 500 nodes, 7 s -> 2 s). The weights are
    stored as float32 afterwards (the file doubles). Worker thread (CPU, a few seconds). */
bool foldModel (const juce::File& in, const juce::File& out, juce::String& error);
} // namespace hydra::stems

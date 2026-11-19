#pragma once
#include "Common.h"
#include "Resampler.h"

struct DenoiseState;

namespace hydra::dsp
{
struct DenoiserSettings
{
    int mode = 1;              // see Denoiser::Mode
    float amount = 1.0f;       // 0..1 dry/wet (dry is latency aligned)
    /** On the audio thread: RNNoise's frame runs on the shared worker thread (see Denoiser). Offline (export,
        tests) it runs right where the frame is complete. */
    bool realtime = false;
};

/** RNNoise (xiph.org) neural noise suppression, stereo, block-size independent.

    RNNoise analyses fixed 10 ms frames at 48 kHz and uses one frame of lookahead, so its own
    synthesis ("Native") has 30 ms latency. The other modes only take RNNoise's 32 band gains and
    apply them with a zero-latency, perfectly reconstructing time-domain filterbank:
      UltraLow  0 ms  gains arrive ~20 ms late (word onsets slightly softened)
      Low      10 ms  gains ~10 ms late (recommended)
      Aligned  20 ms  gains exactly aligned with the audio
      Native   30 ms  original RNNoise synthesis incl. pitch filter (highest quality)
    Disabled = no processing and no latency.

    Worker thread (realtime, 18.11.): a whole frame of RNNoise (FFT, pitch search, network) is far more work than
    the audio between two frames; at small buffers all of it landed in every 7th-8th block (peaks of 15-40 % of a
    64-sample block, user 18.11.). On the audio thread the complete frame is handed to one shared worker thread
    (WorkerPool in Denoiser.cpp), the result is used one engine block later:
      UltraLow / Low   the gains simply start one block later (they are late by design; no extra latency)
      Aligned / Native one block of extra latency (`extraDelay` = the engine block), so the timing / sound stays
                       exactly as before.
    Is the worker not done when the result is due (Aligned / Native) or when the next frame is complete, the audio
    thread takes the job back and computes it itself (or waits the moment the worker needs to finish it): no frame
    is ever skipped. With prepare (rate, 0) or offline everything is computed in place, as before.

    Other sample rates: the model only knows 48 kHz, so it listens to a resampled copy of the input.
    In the gain modes the audio itself is never resampled (the filterbank runs at the real rate), so
    they keep their latency; only the gains come about 0.35 ms later (Aligned waits for that too).
    Native HQ has to go through 48 kHz and back, about 2-4 ms more.

    The voice probability (getVad) is exported for the strip's gate ("Voice Key"); the denoiser
    itself has no gate. */
class Denoiser
{
public:
    enum Mode { UltraLow = 0, Low, Aligned, Native, NumModes };
    static constexpr int kFrame = 480;
    static constexpr int kBands = 32;

    /** Latency of a mode at 48 kHz without the worker's extra block, in samples. */
    static int latencyForMode (int mode) noexcept { return mode >= Native ? 3 * kFrame : juce::jlimit (0, 2, mode) * kFrame; }
    static juce::StringArray modeNames() { return { "Ultra Low (0 ms)", "Low (10 ms)", "Aligned (20 ms)", "Native HQ (30 ms)" }; }

    Denoiser();
    ~Denoiser();

    /** Not on the audio thread (allocates when the sample rate is not 48 kHz). engineBlock: the engine's block size,
        the time the worker has for a frame (0 = no worker, every frame computed in place). */
    void prepare (double sampleRate, int engineBlock = 0);
    void reset() noexcept;

    /** stereo=false processes L only and copies it to R. */
    void process (float* l, float* r, int n, const DenoiserSettings& s, bool stereo) noexcept;

    int getLatencySamples() const noexcept       { return latency[mode]; }
    /** Latency of a mode at the prepared sample rate. */
    double latencyMsForMode (int m) const noexcept { return 1000.0 * latency[juce::jlimit (0, (int) NumModes - 1, m)] / rate; }
    /** RNNoise voice probability (0..1) of the most recent frame. */
    float getVad() const noexcept                { return vad.load (std::memory_order_relaxed); }

    /** Worker thread: computes the queued frame if it can claim it. */
    bool claimJob() noexcept;
    void runJob() noexcept;

private:
    static constexpr int kXover = kBands - 1;
    static constexpr int kPad = 32;      // kXover rounded up to whole SIMD vectors (the extra band stays silent)
    static constexpr int kChunk = 32;   // audio between two looks at the resampled copy (other sample rates)

    struct Channel
    {
        DenoiseState* st = nullptr;
        float fifo[kFrame] {};
        float jobIn[kFrame] {};          // the complete frame for RNNoise (16-bit scale)
        float jobGains[kBands] {};       // RNNoise's result: band gains ...
        float jobOut[kFrame] {};         // ... or (Native) its audio
        float nativeOut[kFrame] {};      // Native: the frame being played
        std::vector<float> delay;
        // The filterbank in double precision (its low crossovers are biquads with poles close to 1, see BiquadCoeffs).
        alignas (32) double z1[kPad] {}, z2[kPad] {};
        alignas (32) double w[kPad] {}, wStep[kPad] {};
        double wLast = 1.0, wLastStep = 0.0;
        int rampLeft = 0;
        void clear() noexcept;
    };

    enum JobState { idle = 0, queued, running, done };

    void configure (double sampleRate);
    /** A frame is complete (fifo of every channel): hands it to the worker or computes it. */
    void submitFrame (int chans, bool realtime) noexcept;
    /** The result is due: makes sure it is there (takes the job back / waits for the worker) and uses it. */
    void applyFrame (bool force) noexcept;
    /** Cancels / waits for a job in flight (reset, mode change). */
    void settleJob() noexcept;
    void process48k (float** io, int n, float wet, float dry, int chans, bool realtime) noexcept;
    void processResampled (float** io, int n, float wet, float dry, int chans, bool realtime) noexcept;
    /** The next sample of the gain filterbank (delayed input d). */
    float filterbank (Channel& c, float d) noexcept;

    Channel ch[2];
    alignas (32) double b0[kPad] {}, b1[kPad] {}, b2[kPad] {}, a1[kPad] {}, a2[kPad] {};
    double rate = 48000.0;
    int latency[NumModes] {};
    int pos = 0, writePos = 0, mode = Low, delayMask = 0, ramp = 96;
    double rampInv = 1.0 / 96.0;
    std::atomic<float> vad { 0.0f };

    // The frame in flight (one at a time).
    std::atomic<int> jobState { idle };
    int jobChans = 1, jobMode = Low;
    float jobVad = 0.0f;
    bool pending = false;        // audio thread: a submitted frame whose result has not been used yet
    int applyIn = 0;             // audio thread: samples until it is due (Aligned / Native: the extra block)
    int nativePos = kFrame;      // Native: read position in nativeOut (since its frame was applied)
    int engineBlock = 0, extraDelay = 0;   // extraDelay: added to Aligned / Native while the worker is used

    // Only used when the sample rate is not 48 kHz.
    bool resampled = false;
    int nativePreRoll = 0;
    VarResampler toModel, fromModel;
    SpscAudioRing modelIn, modelOut;
};
} // namespace hydra::dsp

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

    /** Latency of a mode at 48 kHz, in samples. */
    static int latencyForMode (int mode) noexcept { return mode >= Native ? 3 * kFrame : juce::jlimit (0, 2, mode) * kFrame; }
    static juce::StringArray modeNames() { return { "Ultra Low (0 ms)", "Low (10 ms)", "Aligned (20 ms)", "Native HQ (30 ms)" }; }

    Denoiser();
    ~Denoiser();

    /** Not on the audio thread (allocates when the sample rate is not 48 kHz). */
    void prepare (double sampleRate);
    void reset() noexcept;

    /** stereo=false processes L only and copies it to R. */
    void process (float* l, float* r, int n, const DenoiserSettings& s, bool stereo) noexcept;

    int getLatencySamples() const noexcept       { return latency[mode]; }
    /** Latency of a mode at the prepared sample rate. */
    double latencyMsForMode (int m) const noexcept { return 1000.0 * latency[juce::jlimit (0, (int) NumModes - 1, m)] / rate; }
    /** RNNoise voice probability (0..1) of the most recent frame. */
    float getVad() const noexcept                { return vad.load (std::memory_order_relaxed); }

private:
    static constexpr int kXover = kBands - 1;
    static constexpr int kChunk = 32;   // audio between two looks at the resampled copy (other sample rates)

    struct Channel
    {
        DenoiseState* st = nullptr;
        float fifo[kFrame] {};
        float nativeOut[kFrame] {};
        std::vector<float> delay;
        float z1[kXover] {}, z2[kXover] {};
        float w[kXover] {}, wStep[kXover] {};
        float wLast = 1.0f, wLastStep = 0.0f;
        int rampLeft = 0;
        void clear() noexcept;
    };

    void configure (double sampleRate);
    void startFrame (Channel& c) noexcept;
    void process48k (float** io, int n, float wet, float dry, int chans) noexcept;
    void processResampled (float** io, int n, float wet, float dry, int chans) noexcept;
    /** The next sample of the gain filterbank (delayed input d). */
    float filterbank (Channel& c, float d) noexcept;

    Channel ch[2];
    float b0[kXover] {}, b1[kXover] {}, b2[kXover] {}, a1[kXover] {}, a2[kXover] {};
    double rate = 48000.0;
    int latency[NumModes] {};
    int pos = 0, writePos = 0, mode = Low, delayMask = 0, ramp = 96;
    float rampInv = 1.0f / 96.0f;
    float frameVad = 0.0f;
    std::atomic<float> vad { 0.0f };

    // Only used when the sample rate is not 48 kHz.
    bool resampled = false;
    int nativePreRoll = 0;
    VarResampler toModel, fromModel;
    SpscAudioRing modelIn, modelOut;
};
} // namespace hydra::dsp

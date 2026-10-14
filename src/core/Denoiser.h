#pragma once
#include "Common.h"

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
    The voice probability (getVad) is exported for the strip's gate ("Voice Key"); the denoiser
    itself has no gate. */
class Denoiser
{
public:
    enum Mode { UltraLow = 0, Low, Aligned, Native, NumModes };
    static constexpr int kFrame = 480;
    static constexpr int kBands = 32;

    static int latencyForMode (int mode) noexcept { return mode >= Native ? 3 * kFrame : juce::jlimit (0, 2, mode) * kFrame; }
    static juce::StringArray modeNames() { return { "Ultra Low (0 ms)", "Low (10 ms)", "Aligned (20 ms)", "Native HQ (30 ms)" }; }

    Denoiser();
    ~Denoiser();

    void prepare (double sampleRate);
    void reset() noexcept;
    bool isSupported() const noexcept { return supported; }

    /** stereo=false processes L only and copies it to R. */
    void process (float* l, float* r, int n, const DenoiserSettings& s, bool stereo) noexcept;

    int getLatencySamples() const noexcept       { return supported ? latencyForMode (mode) : 0; }
    /** RNNoise voice probability (0..1) of the most recent frame. */
    float getVad() const noexcept                { return vad.load (std::memory_order_relaxed); }

private:
    static constexpr int kXover = kBands - 1;
    static constexpr int kDelaySize = 2048;
    static constexpr int kRamp = 96; // 2 ms gain interpolation

    struct Channel
    {
        DenoiseState* st = nullptr;
        float fifo[kFrame] {};
        float nativeOut[kFrame] {};
        float delay[kDelaySize] {};
        float z1[kXover] {}, z2[kXover] {};
        float w[kXover] {}, wStep[kXover] {};
        float wLast = 1.0f, wLastStep = 0.0f;
        int rampLeft = 0;
        void clear() noexcept;
    };

    void startFrame (Channel& c, bool isStereoPartner) noexcept;

    Channel ch[2];
    float b0[kXover] {}, b1[kXover] {}, b2[kXover] {}, a1[kXover] {}, a2[kXover] {};
    int pos = 0, writePos = 0, mode = Low;
    bool supported = true;
    float frameVad = 0.0f;
    std::atomic<float> vad { 0.0f };
};
} // namespace hydra::dsp

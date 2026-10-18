#include "Denoiser.h"
#include <rnnoise.h>

namespace hydra::dsp
{
void Denoiser::Channel::clear() noexcept
{
    if (st != nullptr) rnnoise_init (st, nullptr);
    std::fill (std::begin (fifo), std::end (fifo), 0.0f);
    std::fill (std::begin (nativeOut), std::end (nativeOut), 0.0f);
    std::fill (std::begin (delay), std::end (delay), 0.0f);
    std::fill (std::begin (z1), std::end (z1), 0.0f);
    std::fill (std::begin (z2), std::end (z2), 0.0f);
    std::fill (std::begin (w), std::end (w), 0.0f);
    std::fill (std::begin (wStep), std::end (wStep), 0.0f);
    wLast = 1.0f; wLastStep = 0.0f; rampLeft = 0;
}

Denoiser::Denoiser()
{
    jassert (rnnoise_get_frame_size() == kFrame);
    jassert (RNNOISE_NB_GAIN_BANDS == kBands);
    for (auto& c : ch) c.st = rnnoise_create (nullptr);

    // Crossovers halfway (geometrically) between RNNoise's band centres; 2nd-order Butterworth lowpasses.
    for (int j = 0; j < kXover; ++j)
    {
        const double fc = std::sqrt ((double) rnnoise_band_centre_hz (j) * rnnoise_band_centre_hz (j + 1));
        const double w0 = juce::MathConstants<double>::twoPi * fc / 48000.0;
        const double cw = std::cos (w0), alpha = std::sin (w0) / (2.0 * 0.70710678);
        const double a0 = 1.0 + alpha;
        b0[j] = (float) ((1.0 - cw) * 0.5 / a0);
        b1[j] = (float) ((1.0 - cw) / a0);
        b2[j] = b0[j];
        a1[j] = (float) (-2.0 * cw / a0);
        a2[j] = (float) ((1.0 - alpha) / a0);
    }
}

Denoiser::~Denoiser()
{
    for (auto& c : ch)
        if (c.st != nullptr) rnnoise_destroy (c.st);
}

void Denoiser::prepare (double sampleRate)
{
    supported = std::abs (sampleRate - 48000.0) < 1.0; // the RNNoise model is 48 kHz only
    reset();
}

void Denoiser::reset() noexcept
{
    for (auto& c : ch) c.clear();
    pos = 0;
    writePos = 0;
    frameVad = 0.0f;
    vad.store (0.0f);
}

void Denoiser::startFrame (Channel& c, bool) noexcept
{
    if (mode == Native)
    {
        const float v = rnnoise_process_frame (c.st, c.nativeOut, c.fifo);
        for (auto& s : c.nativeOut) s *= (1.0f / 32768.0f);
        frameVad = juce::jmax (frameVad, v);
        return;
    }

    float g[kBands];
    frameVad = juce::jmax (frameVad, rnnoise_process_frame_gains (c.st, g, c.fifo));

    // y = g[31] * x + sum_j (g[j] - g[j+1]) * LP_j(x)  -> telescoping, exact when all gains are equal
    constexpr float inv = 1.0f / (float) kRamp;
    for (int j = 0; j < kXover; ++j)
        c.wStep[j] = ((g[j] - g[j + 1]) - c.w[j]) * inv;
    c.wLastStep = (g[kBands - 1] - c.wLast) * inv;
    c.rampLeft = kRamp;
}

void Denoiser::process (float* l, float* r, int n, const DenoiserSettings& s, bool stereo) noexcept
{
    if (! supported || ch[0].st == nullptr || ch[1].st == nullptr)
        return;

    const int newMode = juce::jlimit (0, (int) NumModes - 1, s.mode);
    if (newMode != mode)
    {
        mode = newMode;
        reset();
    }

    const int delaySamples = latencyForMode (mode);
    const float wet = juce::jlimit (0.0f, 1.0f, s.amount);
    const float dry = 1.0f - wet;
    const int chans = stereo ? 2 : 1;
    float* io[2] = { l, r };

    int i = 0;
    while (i < n)
    {
        const int chunk = juce::jmin (n - i, kFrame - pos);
        const int wp = writePos;

        for (int c = 0; c < chans; ++c)
        {
            auto& C = ch[c];
            float* x = io[c] + i;
            int w = wp;
            for (int k = 0; k < chunk; ++k)
            {
                const float in = x[k];
                C.fifo[pos + k] = in * 32768.0f; // RNNoise expects 16-bit scale
                C.delay[w] = in;
                const float d = C.delay[(w - delaySamples) & (kDelaySize - 1)];
                w = (w + 1) & (kDelaySize - 1);

                float y;
                if (mode == Native)
                    y = C.nativeOut[pos + k];
                else
                {
                    if (C.rampLeft > 0)
                    {
                        for (int j = 0; j < kXover; ++j) C.w[j] += C.wStep[j];
                        C.wLast += C.wLastStep;
                        --C.rampLeft;
                    }
                    y = C.wLast * d;
                    for (int j = 0; j < kXover; ++j)
                    {
                        const float o = b0[j] * d + C.z1[j];
                        C.z1[j] = b1[j] * d - a1[j] * o + C.z2[j];
                        C.z2[j] = b2[j] * d - a2[j] * o;
                        y += C.w[j] * o;
                    }
                }
                x[k] = y * wet + d * dry;
            }
        }

        writePos = (wp + chunk) & (kDelaySize - 1);
        pos += chunk;
        i += chunk;

        if (pos == kFrame)
        {
            pos = 0;
            frameVad = 0.0f;
            for (int c = 0; c < chans; ++c)
                startFrame (ch[c], c == 1);
            vad.store (frameVad, std::memory_order_relaxed);
        }
    }

    if (! stereo)
        std::memcpy (r, l, (size_t) n * sizeof (float));
}
} // namespace hydra::dsp

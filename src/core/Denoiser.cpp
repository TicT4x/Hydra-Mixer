#include "Denoiser.h"
#include <rnnoise.h>

namespace hydra::dsp
{
void Denoiser::Channel::clear() noexcept
{
    if (st != nullptr) rnnoise_init (st, nullptr);
    std::fill (std::begin (fifo), std::end (fifo), 0.0f);
    std::fill (std::begin (nativeOut), std::end (nativeOut), 0.0f);
    std::fill (delay.begin(), delay.end(), 0.0f);
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
    prepare (48000.0);
}

Denoiser::~Denoiser()
{
    for (auto& c : ch)
        if (c.st != nullptr) rnnoise_destroy (c.st);
}

void Denoiser::configure (double sampleRate)
{
    rate = sampleRate >= 1000.0 ? sampleRate : 48000.0;
    resampled = std::abs (rate - 48000.0) >= 1.0;   // the RNNoise model is 48 kHz only
    const double ratio = resampled ? rate / 48000.0 : 1.0;   // audio samples per model sample

    // Crossovers halfway (geometrically) between RNNoise's band centres; 2nd-order Butterworth lowpasses.
    for (int j = 0; j < kXover; ++j)
    {
        const double fc = std::sqrt ((double) rnnoise_band_centre_hz (j) * rnnoise_band_centre_hz (j + 1));
        if (fc >= 0.45 * rate)   // above what this rate can carry: passes everything, the sum stays exact
        {
            b0[j] = 1.0f;
            b1[j] = b2[j] = a1[j] = a2[j] = 0.0f;
            continue;
        }
        const double w0 = juce::MathConstants<double>::twoPi * fc / rate;
        const double cw = std::cos (w0), alpha = std::sin (w0) / (2.0 * 0.70710678);
        const double a0 = 1.0 + alpha;
        b0[j] = (float) ((1.0 - cw) * 0.5 / a0);
        b1[j] = (float) ((1.0 - cw) / a0);
        b2[j] = b0[j];
        a1[j] = (float) (-2.0 * cw / a0);
        a2[j] = (float) ((1.0 - alpha) / a0);
    }

    ramp = juce::jmax (8, juce::roundToInt (0.002 * rate));   // 2 ms gain interpolation
    rampInv = 1.0f / (float) ramp;

    if (resampled)
    {
        // Filter length follows the lower of the two rates, so the anti-aliasing is equally good at every rate.
        toModel.prepare (2, ratio, kChunk * 2, (int) std::ceil (16.0 * juce::jmax (1.0, ratio)));
        fromModel.prepare (2, 1.0 / ratio, kFrame * 2, (int) std::ceil (16.0 * juce::jmax (1.0, 1.0 / ratio)));
        const int inDelay = toModel.getLatencyFrames() + 1;      // audio samples
        const int outDelay = fromModel.getLatencyFrames() + 1;   // model samples

        // Native HQ: silence ahead of the model's output, enough that it never runs dry between two frames.
        nativePreRoll = (int) std::ceil ((kFrame + outDelay) * ratio) + inDelay + kChunk + 2;
        modelIn.allocate (2, 4096);
        modelOut.allocate (2, nativePreRoll + 2 * (int) std::ceil (kFrame * ratio) + 64);

        latency[UltraLow] = 0;
        latency[Low] = juce::roundToInt (0.01 * rate);
        latency[Aligned] = juce::roundToInt (0.02 * rate) + inDelay;   // also waits for the resampled copy
        latency[Native] = juce::roundToInt ((2 * kFrame + outDelay) * ratio) + inDelay + nativePreRoll;
    }
    else
    {
        for (int m = 0; m < NumModes; ++m) latency[m] = latencyForMode (m);
    }

    const int delaySize = juce::nextPowerOfTwo (juce::jmax (2048, latency[Native] + 8));
    delayMask = delaySize - 1;
    for (auto& c : ch) c.delay.assign ((size_t) delaySize, 0.0f);
}

void Denoiser::prepare (double sampleRate)
{
    configure (sampleRate);
    reset();
}

void Denoiser::reset() noexcept
{
    for (auto& c : ch) c.clear();
    pos = 0;
    writePos = 0;
    frameVad = 0.0f;
    vad.store (0.0f);
    if (resampled)
    {
        toModel.reset();
        fromModel.reset();
        modelIn.skip (modelIn.available());
        modelOut.skip (modelOut.available());
        const float* silence[2] = { nullptr, nullptr };
        modelOut.write (silence, 2, nativePreRoll);
    }
}

void Denoiser::startFrame (Channel& c) noexcept
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
    for (int j = 0; j < kXover; ++j)
        c.wStep[j] = ((g[j] - g[j + 1]) - c.w[j]) * rampInv;
    c.wLastStep = (g[kBands - 1] - c.wLast) * rampInv;
    c.rampLeft = ramp;
}

inline float Denoiser::filterbank (Channel& c, float d) noexcept
{
    if (c.rampLeft > 0)
    {
        for (int j = 0; j < kXover; ++j) c.w[j] += c.wStep[j];
        c.wLast += c.wLastStep;
        --c.rampLeft;
    }
    float y = c.wLast * d;
    for (int j = 0; j < kXover; ++j)
    {
        const float o = b0[j] * d + c.z1[j];
        c.z1[j] = b1[j] * d - a1[j] * o + c.z2[j];
        c.z2[j] = b2[j] * d - a2[j] * o;
        y += c.w[j] * o;
    }
    return y;
}

void Denoiser::process (float* l, float* r, int n, const DenoiserSettings& s, bool stereo) noexcept
{
    if (ch[0].st == nullptr || ch[1].st == nullptr)
        return;

    const int newMode = juce::jlimit (0, (int) NumModes - 1, s.mode);
    if (newMode != mode)
    {
        mode = newMode;
        reset();
    }

    const float wet = juce::jlimit (0.0f, 1.0f, s.amount);
    float* io[2] = { l, r };
    if (resampled) processResampled (io, n, wet, 1.0f - wet, stereo ? 2 : 1);
    else           process48k (io, n, wet, 1.0f - wet, stereo ? 2 : 1);

    if (! stereo)
        std::memcpy (r, l, (size_t) n * sizeof (float));
}

void Denoiser::process48k (float** io, int n, float wet, float dry, int chans) noexcept
{
    const int delaySamples = latency[mode];
    int i = 0;
    while (i < n)
    {
        const int chunk = juce::jmin (n - i, kFrame - pos);
        const int wp = writePos;

        for (int c = 0; c < chans; ++c)
        {
            auto& C = ch[c];
            float* x = io[c] + i;
            float* delay = C.delay.data();
            int w = wp;
            for (int k = 0; k < chunk; ++k)
            {
                const float in = x[k];
                C.fifo[pos + k] = in * 32768.0f; // RNNoise expects 16-bit scale
                delay[w] = in;
                const float d = delay[(w - delaySamples) & delayMask];
                w = (w + 1) & delayMask;
                const float y = mode == Native ? C.nativeOut[pos + k] : filterbank (C, d);
                x[k] = y * wet + d * dry;
            }
        }

        writePos = (wp + chunk) & delayMask;
        pos += chunk;
        i += chunk;

        if (pos == kFrame)
        {
            pos = 0;
            frameVad = 0.0f;
            for (int c = 0; c < chans; ++c)
                startFrame (ch[c]);
            vad.store (frameVad, std::memory_order_relaxed);
        }
    }
}

void Denoiser::processResampled (float** io, int n, float wet, float dry, int chans) noexcept
{
    const int delaySamples = latency[mode];
    float nativeWet[2][kChunk];
    int i = 0;
    while (i < n)
    {
        const int chunk = juce::jmin (n - i, kChunk);

        // The model hears a 48 kHz copy of the input; a frame is ready every 10 ms.
        const float* in[2] = { io[0] + i, chans > 1 ? io[1] + i : nullptr };
        toModel.push (in, chunk, modelIn);
        while (modelIn.available() >= kFrame)
        {
            float* frame[2] = { ch[0].fifo, ch[1].fifo };
            modelIn.read (frame, 2, kFrame);
            frameVad = 0.0f;
            for (int c = 0; c < chans; ++c)
            {
                for (auto& v : ch[c].fifo) v *= 32768.0f; // RNNoise expects 16-bit scale
                startFrame (ch[c]);
            }
            vad.store (frameVad, std::memory_order_relaxed);
            if (mode == Native)
            {
                const float* out[2] = { ch[0].nativeOut, chans > 1 ? ch[1].nativeOut : nullptr };
                fromModel.push (out, kFrame, modelOut);
            }
        }

        if (mode == Native)
        {
            float* dst[2] = { nativeWet[0], nativeWet[1] };
            const int got = modelOut.read (dst, 2, chunk);
            for (int c = 0; c < 2; ++c)
                std::fill (nativeWet[c] + got, nativeWet[c] + chunk, 0.0f);
        }

        const int wp = writePos;
        for (int c = 0; c < chans; ++c)
        {
            auto& C = ch[c];
            float* x = io[c] + i;
            float* delay = C.delay.data();
            int w = wp;
            for (int k = 0; k < chunk; ++k)
            {
                delay[w] = x[k];
                const float d = delay[(w - delaySamples) & delayMask];
                w = (w + 1) & delayMask;
                const float y = mode == Native ? nativeWet[c][k] : filterbank (C, d);
                x[k] = y * wet + d * dry;
            }
        }
        writePos = (wp + chunk) & delayMask;
        i += chunk;
    }
}
} // namespace hydra::dsp

#pragma once
#include "RingBuffer.h"

namespace hydra
{
/** Polyphase windowed-sinc resampler with continuously variable ratio.
    Used for both sample-rate conversion (44.1k <-> 48k) and clock drift compensation
    between independent devices. Group delay is only halfTaps samples (8 or 16). */
class VarResampler
{
public:
    /** ratio = input frames consumed per output frame (nominal). */
    void prepare (int channels, double nominalRatio, int maxOutBlock)
    {
        numChannels = juce::jlimit (1, kMaxChannels, channels);
        nominal = nominalRatio;
        ratio = nominalRatio;
        halfTaps = std::abs (nominalRatio - 1.0) < 1.0e-3 ? 8 : 16;
        taps = halfTaps * 2;

        // Anti-aliasing: when consuming faster than producing, lower the cutoff.
        const double cutoff = 0.92 * juce::jmin (1.0, 1.0 / nominalRatio);
        buildTable (cutoff);

        histCapacity = (int) std::ceil (maxOutBlock * nominalRatio * 1.05) + taps * 4 + 64;
        for (int c = 0; c < kMaxChannels; ++c)
            hist[(size_t) c].assign (c < numChannels ? (size_t) histCapacity : 0, 0.0f);
        reset();
    }

    void reset()
    {
        for (auto& h : hist) std::fill (h.begin(), h.end(), 0.0f);
        histLen = taps;       // pre-roll of silence so the first output is valid
        pos = (double) (halfTaps - 1);
    }

    void setCorrection (double c) noexcept { ratio = nominal * c; }
    double getNominal() const noexcept     { return nominal; }
    int getLatencyFrames() const noexcept  { return halfTaps; }

    /** Frames of input still needed to produce n more outputs. */
    int inputNeededFor (int n) const noexcept
    {
        const double lastPos = pos + ratio * (double) (n - 1);
        return juce::jmax (0, (int) std::floor (lastPos) + halfTaps + 1 - histLen);
    }

    /** Pull mode: produce exactly n frames, pulling input from ring. Returns false on underrun
        (output is still fully written, missing input treated as silence). */
    bool pull (SpscAudioRing& ring, float* const* out, int n) noexcept
    {
        bool ok = true;
        int done = 0;
        while (done < n)
        {
            const int chunk = juce::jmin (n - done, 512);
            int need = inputNeededFor (chunk);
            if (need > 0)
            {
                compact();
                need = juce::jmin (need, histCapacity - histLen);
                float* dst[kMaxChannels];
                for (int c = 0; c < numChannels; ++c) dst[c] = hist[(size_t) c].data() + histLen;
                const int got = ring.read (dst, numChannels, need);
                if (got < need)
                {
                    ok = false;
                    for (int c = 0; c < numChannels; ++c)
                        std::fill (dst[c] + got, dst[c] + need, 0.0f);
                }
                histLen += need;
            }

            float* o[kMaxChannels];
            for (int c = 0; c < numChannels; ++c) o[c] = out[c] + done;
            produce (o, chunk);
            done += chunk;
        }
        return ok;
    }

    /** Push mode: consume n input frames, write all producible output to ring. */
    void push (const float* const* in, int n, SpscAudioRing& ring) noexcept
    {
        int consumed = 0;
        while (consumed < n)
        {
            compact();
            const int k = juce::jmin (n - consumed, histCapacity - histLen);
            for (int c = 0; c < numChannels; ++c)
            {
                if (in[c] != nullptr)
                    std::memcpy (hist[(size_t) c].data() + histLen, in[c] + consumed, (size_t) k * sizeof (float));
                else
                    std::fill (hist[(size_t) c].data() + histLen, hist[(size_t) c].data() + histLen + k, 0.0f);
            }
            histLen += k;
            consumed += k;

            // How many outputs are available with the current history?
            const double avail = ((double) (histLen - halfTaps - 1) - pos) / ratio;
            int outN = avail >= 0.0 ? (int) avail + 1 : 0;
            while (outN > 0)
            {
                const int chunk = juce::jmin (outN, (int) tmpSize);
                float* o[kMaxChannels];
                for (int c = 0; c < numChannels; ++c) o[c] = tmp[(size_t) c].data();
                produce (o, chunk);
                ring.write (o, numChannels, chunk);
                outN -= chunk;
            }
        }
    }

private:
    static constexpr int kMaxChannels = kMaxStreamChannels;
    static constexpr int kPhases = 512;
    static constexpr size_t tmpSize = 512;

    void buildTable (double cutoff)
    {
        table.assign ((size_t) (kPhases + 1) * (size_t) taps, 0.0f);
        for (int p = 0; p <= kPhases; ++p)
        {
            const double frac = (double) p / kPhases;
            double sum = 0.0;
            for (int k = 0; k < taps; ++k)
            {
                // tap k sits at offset (k - halfTaps + 1) relative to floor(pos)
                const double x = (double) (k - halfTaps + 1) - frac;
                const double sinc = std::abs (x) < 1.0e-9 ? 1.0
                                  : std::sin (juce::MathConstants<double>::pi * x * cutoff) / (juce::MathConstants<double>::pi * x * cutoff);
                // Kaiser-ish (Blackman-Harris) window over [-halfTaps, halfTaps]
                const double w = 0.5 * (1.0 + std::cos (juce::MathConstants<double>::pi * x / (double) halfTaps));
                const double v = sinc * w * w;
                table[(size_t) p * (size_t) taps + (size_t) k] = (float) v;
                sum += v;
            }
            for (int k = 0; k < taps; ++k)
                table[(size_t) p * (size_t) taps + (size_t) k] /= (float) sum;
        }
        for (int c = 0; c < kMaxChannels; ++c)
            tmp[(size_t) c].assign (c < numChannels ? tmpSize : 0, 0.0f);
    }

    void produce (float* const* out, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const int base = (int) pos;
            const double frac = pos - (double) base;
            const double fp = frac * kPhases;
            const int ph = (int) fp;
            const float t = (float) (fp - ph);
            const float* k0 = table.data() + (size_t) ph * (size_t) taps;
            const float* k1 = k0 + taps;
            const int start = base - halfTaps + 1;

            for (int c = 0; c < numChannels; ++c)
            {
                const float* h = hist[(size_t) c].data() + start;
                float a = 0.0f, b = 0.0f;
                for (int k = 0; k < taps; ++k)
                {
                    a += h[k] * k0[k];
                    b += h[k] * k1[k];
                }
                out[c][i] = a + (b - a) * t;
            }
            pos += ratio;
        }
    }

    /** Drop history that is no longer needed. */
    void compact() noexcept
    {
        const int keepFrom = (int) pos - halfTaps + 1;
        if (keepFrom <= 0) return;
        const int remain = histLen - keepFrom;
        if (remain > 0)
            for (int c = 0; c < numChannels; ++c)
                std::memmove (hist[(size_t) c].data(), hist[(size_t) c].data() + keepFrom, (size_t) remain * sizeof (float));
        histLen = juce::jmax (0, remain);
        pos -= keepFrom;
    }

    int numChannels = 2, halfTaps = 8, taps = 16, histCapacity = 0, histLen = 0;
    double nominal = 1.0, ratio = 1.0, pos = 0.0;
    std::vector<float> table;
    std::array<std::vector<float>, kMaxChannels> hist, tmp;
};

/** Keeps a ring buffer at a self-tuned target fill level by nudging the resampling ratio
    (PI controller). Lowers its target automatically when the stream is stable, raises it
    after an underrun — always the lowest latency that is still glitch-free.

    The regulated value is the *lowest* fill seen over a short window (a few device periods),
    not the mean: the fill at the engine's measuring point depends on the phase between device
    and engine callbacks, and with two clocks that phase rotates only slowly (100 ppm: one
    turn in ~30 s). Holding the window minimum at the target keeps the safety margin the
    same at every phase instead of only at the phases seen while the target was tightened. */
class DriftController
{
public:
    /** initialTarget/minTarget/maxTarget in frames (device frames), quantum = adaptation step.
        deviceRate: frames per second the ring moves at (sets the loop gain).
        windowBlocks: engine blocks the minimum is taken over (should span several device periods). */
    void prepare (double initialTarget, double minTarget, double maxTarget, double quantumFrames,
                  double engineRate, int engineBlock, double deviceRate, int windowBlocks)
    {
        lo = minTarget;
        hi = juce::jmax (minTarget, maxTarget);
        quantum = juce::jmax (8.0, quantumFrames);
        target = juce::jlimit (lo, hi, initialTarget);
        avg = target;
        integral = 0.0;
        blocksPerSecond = engineRate / juce::jmax (1, engineBlock);
        slackMin = 1.0e9;
        blockCounter = 0;
        window = juce::jlimit (1, 1024, windowBlocks);
        windowCounter = 0;
        curMin = prevMin = target;

        // PI loop on the fill error in frames. The ring moves by deviceRate * u frames/s for a
        // ratio correction u, so these gains give a critically damped loop (no overshoot into
        // an underrun after the target was lowered) with ~2 s time constant.
        constexpr double wn = 0.5, zeta = 1.0;
        const double rate = juce::jmax (1000.0, deviceRate);
        kp = 2.0 * zeta * wn / rate;
        ki = wn * wn / rate / blocksPerSecond; // per block
        alpha = 1.0 / juce::jmax (1.0, blocksPerSecond * 0.1); // ~0.1 s smoothing of the window minimum
    }

    /** Call once per engine block with the measured fill (frames) and the minimum fill that
        is still safe at that measuring point. Returns the ratio correction factor. */
    double update (double fill, double minSafeFill, bool hadUnderrun) noexcept
    {
        if (hadUnderrun)
        {
            // Keep the integral: it holds the learned clock drift.
            target = juce::jmin (hi, target + 2.0 * quantum);
            avg = fill;
            slackMin = 1.0e9;
            blockCounter = 0;
            curMin = prevMin = fill;
            windowCounter = 0;
        }

        slackMin = juce::jmin (slackMin, fill - minSafeFill);
        if (++blockCounter >= (int) (blocksPerSecond * 4.0)) // every ~4 s, try to tighten
        {
            if (slackMin > 2.0 * quantum && target > lo)
                target = juce::jmax (lo, target - quantum);
            slackMin = 1.0e9;
            blockCounter = 0;
        }

        // Sliding minimum over 1..2 windows (two buckets, O(1), no allocation).
        curMin = juce::jmin (curMin, fill);
        const double low = juce::jmin (curMin, prevMin);
        if (++windowCounter >= window) { prevMin = curMin; curMin = 1.0e9; windowCounter = 0; }

        avg += alpha * (low - avg);
        const double err = avg - target; // frames
        const double p = kp * err;
        // Anti-windup: integrate only while the output is not saturated.
        const double next = juce::jlimit (-kMaxDrift, kMaxDrift, integral + ki * err);
        if (std::abs (p + next) < kMaxCorr) integral = next;
        return 1.0 + juce::jlimit (-kMaxCorr, kMaxCorr, p + integral);
    }

    double getTarget() const noexcept { return target; }

private:
    double lo = 64, hi = 4096, quantum = 32, target = 128, avg = 128, integral = 0, blocksPerSecond = 375, slackMin = 1e9;
    double curMin = 128, prevMin = 128, kp = 0, ki = 0, alpha = 0.01;
    int blockCounter = 0, window = 1, windowCounter = 0;
    static constexpr double kMaxDrift = 0.001; // clock mismatch the integral may learn (1000 ppm)
    static constexpr double kMaxCorr = 0.003;  // total ratio correction (~5 cents at most)
};
} // namespace hydra

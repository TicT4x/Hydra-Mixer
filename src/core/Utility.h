#pragma once
#include "Dsp.h"

namespace hydra::dsp
{
/** Settings of the Utility effect (like Ableton's Utility). */
struct UtilitySettings
{
    bool invertL = false, invertR = false;   // polarity
    enum Channel { left = 0, stereo, right, swap };
    int channel = stereo;                    // left / right: that channel on both sides
    float width = 1.0f;                      // 0 = mono, 1 = unchanged, up to 4
    bool mono = false;
    bool bassMono = false;                   // low frequencies of the side signal removed (below bassHz)
    float bassHz = 120.0f;
    float balance = 0.0f;                    // -1 (left) .. +1 (right)
    float gainDb = 0.0f;
    bool dcFilter = false;
};

/** Utility: polarity, channel mode, width / mono, bass mono, balance, gain. No latency.
    Everything except bass mono and the DC filter is a 2x2 matrix on (L, R); a change of the settings ramps the
    matrix over one block, so switches (polarity, mono, channel) do not click either. Switched off, it fades to
    the identity and then costs nothing. */
struct Utility
{
    void prepare (double sampleRate) noexcept
    {
        sr = sampleRate;
        reset();
    }

    void reset() noexcept
    {
        m = { 1.0f, 0.0f, 0.0f, 1.0f };
        post = { 1.0f, 1.0f };
        bassMix = 0.0f;
        hp1.reset(); hp2.reset();
        dcL = dcR = dcXL = dcXR = 0.0f;
        bassHzSet = -1.0f;
    }

    /** False while switched off and fully faded out (nothing to do). */
    bool isActive() const noexcept
    {
        return ! (m[0] == 1.0f && m[1] == 0.0f && m[2] == 0.0f && m[3] == 1.0f && post[0] == 1.0f && post[1] == 1.0f && bassMix == 0.0f);
    }

    /** on = false: fades to the identity. */
    void process (float* l, float* r, int n, const UtilitySettings& s, bool on) noexcept
    {
        if (n <= 0 || (! on && ! isActive())) return;

        // Target matrix: width (mid / side) * channel mode * polarity.
        std::array<float, 4> target { 1.0f, 0.0f, 0.0f, 1.0f };
        std::array<float, 2> postTarget { 1.0f, 1.0f };
        float bassTarget = 0.0f;
        if (on)
        {
            const float pl = s.invertL ? -1.0f : 1.0f, pr = s.invertR ? -1.0f : 1.0f;
            // channel mode rows (in terms of the polarity-corrected inputs)
            float c00 = 1, c01 = 0, c10 = 0, c11 = 1;
            switch (s.channel)
            {
                case UtilitySettings::left:  c00 = 1; c01 = 0; c10 = 1; c11 = 0; break;
                case UtilitySettings::right: c00 = 0; c01 = 1; c10 = 0; c11 = 1; break;
                case UtilitySettings::swap:  c00 = 0; c01 = 1; c10 = 1; c11 = 0; break;
                default: break;
            }
            const float w = s.mono ? 0.0f : juce::jlimit (0.0f, 4.0f, s.width);
            const float a = 0.5f * (1.0f + w), b = 0.5f * (1.0f - w);
            // W * C * P
            const float wc00 = a * c00 + b * c10, wc01 = a * c01 + b * c11;
            const float wc10 = b * c00 + a * c10, wc11 = b * c01 + a * c11;
            target = { wc00 * pl, wc01 * pr, wc10 * pl, wc11 * pr };
            const float g = dbToGain (s.gainDb, -100.0f);
            const float bal = juce::jlimit (-1.0f, 1.0f, s.balance);
            postTarget = { g * (bal > 0.0f ? 1.0f - bal : 1.0f), g * (bal < 0.0f ? 1.0f + bal : 1.0f) };
            bassTarget = s.bassMono && ! s.mono ? 1.0f : 0.0f;
            if (s.bassMono && s.bassHz != bassHzSet)
            {
                bassHzSet = s.bassHz;
                hp1.c = hp2.c = BiquadCoeffs::make (BiquadCoeffs::Type::LowCut, sr, s.bassHz, 0.70710678, 0.0);   // LR4 = 2 x Butterworth
            }
        }

        if (on && s.dcFilter)
        {
            // One-pole high pass at ~5 Hz.
            const float k = 1.0f - (float) (juce::MathConstants<double>::twoPi * 5.0 / sr);
            for (int i = 0; i < n; ++i)
            {
                const float xl = l[i], xr = r[i];
                dcL = xl - dcXL + k * dcL; dcXL = xl; l[i] = dcL;
                dcR = xr - dcXR + k * dcR; dcXR = xr; r[i] = dcR;
            }
        }

        const float inv = 1.0f / (float) n;
        if (target == m)
        {
            const float m0 = m[0], m1 = m[1], m2 = m[2], m3 = m[3];
            if (! (m0 == 1.0f && m1 == 0.0f && m2 == 0.0f && m3 == 1.0f))
                for (int i = 0; i < n; ++i)
                {
                    const float xl = l[i], xr = r[i];
                    l[i] = m0 * xl + m1 * xr;
                    r[i] = m2 * xl + m3 * xr;
                }
        }
        else
        {
            std::array<float, 4> d;
            for (size_t j = 0; j < 4; ++j) d[j] = (target[j] - m[j]) * inv;
            for (int i = 0; i < n; ++i)
            {
                const float t = (float) (i + 1);
                const float xl = l[i], xr = r[i];
                l[i] = (m[0] + d[0] * t) * xl + (m[1] + d[1] * t) * xr;
                r[i] = (m[2] + d[2] * t) * xl + (m[3] + d[3] * t) * xr;
            }
            m = target;
        }

        // Bass mono: the side signal goes through an LR4 high pass (-6 dB at bassHz, no bump), faded in / out.
        if (bassTarget > 0.0f || bassMix > 0.0f)
        {
            const float step = (bassTarget - bassMix) * inv;
            float mix = bassMix;
            for (int i = 0; i < n; ++i)
            {
                const float mid = 0.5f * (l[i] + r[i]);
                const float side = 0.5f * (l[i] - r[i]);
                const float high = hp2.tick (hp1.tick (side));
                mix += step;
                const float s2 = side + mix * (high - side);
                l[i] = mid + s2;
                r[i] = mid - s2;
            }
            bassMix = bassTarget;
            if (bassMix == 0.0f) { hp1.reset(); hp2.reset(); }
        }

        applyRamp (l, n, post[0], postTarget[0]);
        applyRamp (r, n, post[1], postTarget[1]);
        post = postTarget;
    }

private:
    struct Mono
    {
        BiquadCoeffs c;
        float z1 = 0, z2 = 0;
        void reset() noexcept { z1 = z2 = 0.0f; }
        float tick (float in) noexcept
        {
            const float out = c.b0 * in + z1;
            z1 = c.b1 * in - c.a1 * out + z2;
            z2 = c.b2 * in - c.a2 * out;
            return out;
        }
    };
    double sr = 48000.0;
    std::array<float, 4> m { 1.0f, 0.0f, 0.0f, 1.0f };
    std::array<float, 2> post { 1.0f, 1.0f };
    float bassMix = 0.0f, bassHzSet = -1.0f;
    Mono hp1, hp2;
    float dcL = 0, dcR = 0, dcXL = 0, dcXR = 0;
};
} // namespace hydra::dsp

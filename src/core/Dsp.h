#pragma once
#include "Common.h"
#if defined (__AVX2__)
 #include <immintrin.h>
#endif

/** Lightweight, zero-latency, allocation-free DSP building blocks. */
namespace hydra::dsp
{
/** Biquad coefficients, double precision: at low frequencies (poles close to 1) float coefficients and states limit a
    bell at 30 Hz to about -75 dB below the signal; in double the rounding stays far below anything audible. */
struct BiquadCoeffs
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;

    enum class Type { Peak, LowShelf, HighShelf, LowCut, HighCut, Notch, BandPass };

    static BiquadCoeffs make (Type type, double sr, double freq, double q, double gainDb)
    {
        freq = juce::jlimit (10.0, sr * 0.49, freq);
        q = juce::jmax (0.05, q);
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w0 = juce::MathConstants<double>::twoPi * freq / sr;
        const double cw = std::cos (w0), sw = std::sin (w0);
        const double alpha = sw / (2.0 * q);
        double b0, b1, b2, a0, a1, a2;

        switch (type)
        {
            case Type::LowShelf:
            {
                const double s = 2.0 * std::sqrt (A) * alpha;
                b0 = A * ((A + 1) - (A - 1) * cw + s);
                b1 = 2 * A * ((A - 1) - (A + 1) * cw);
                b2 = A * ((A + 1) - (A - 1) * cw - s);
                a0 = (A + 1) + (A - 1) * cw + s;
                a1 = -2 * ((A - 1) + (A + 1) * cw);
                a2 = (A + 1) + (A - 1) * cw - s;
                break;
            }
            case Type::HighShelf:
            {
                const double s = 2.0 * std::sqrt (A) * alpha;
                b0 = A * ((A + 1) + (A - 1) * cw + s);
                b1 = -2 * A * ((A - 1) + (A + 1) * cw);
                b2 = A * ((A + 1) + (A - 1) * cw - s);
                a0 = (A + 1) - (A - 1) * cw + s;
                a1 = 2 * ((A - 1) - (A + 1) * cw);
                a2 = (A + 1) - (A - 1) * cw - s;
                break;
            }
            case Type::LowCut:
                b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2;
                a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
                break;
            case Type::HighCut:
                b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2;
                a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
                break;
            case Type::Notch:
                b0 = 1; b1 = -2 * cw; b2 = 1;
                a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
                break;
            case Type::BandPass:
                b0 = alpha; b1 = 0; b2 = -alpha;
                a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
                break;
            case Type::Peak:
            default:
                b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
                a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
                break;
        }

        BiquadCoeffs c;
        c.b0 = b0 / a0; c.b1 = b1 / a0; c.b2 = b2 / a0;
        c.a1 = a1 / a0; c.a2 = a2 / a0;
        return c;
    }

    /** Magnitude response in dB (for EQ curve drawing). */
    double magnitudeDb (double freq, double sr) const
    {
        const std::complex<double> z = std::polar (1.0, -juce::MathConstants<double>::twoPi * freq / sr);
        const auto num = b0 + b1 * z + b2 * z * z;
        const auto den = 1.0 + a1 * z + a2 * z * z;
        return 20.0 * std::log10 (std::abs (num / den) + 1.0e-12);
    }
};

/** Stereo transposed direct form II biquad: coefficients and the state of both channels (processed with runBiquads,
    which runs many sections of both channels in one pass). */
struct StereoBiquad
{
    BiquadCoeffs c;
    double z1[2] {}, z2[2] {};

    void reset() noexcept { z1[0] = z1[1] = z2[0] = z2[1] = 0.0; }
    void process (float* l, float* r, int n) noexcept;   // this section alone on both channels (after runBiquads)
    void processOne (int k, float* x, int n) noexcept;   // only channel k
};

/** One biquad section of one channel: its coefficients and its state (transposed direct form II). */
struct BiquadLane
{
    const BiquadCoeffs* c;
    double* z1;
    double* z2;
};

#if defined (__AVX2__)
namespace detail
{
/** N sections of each channel in one pass: lane 0 = the left channel's sections, lane 1 = the right one's (missing
    ones pass the signal unchanged). Why: a biquad is a chain of dependent multiply-adds (~8 cycles per sample);
    one loop per section and channel leaves the CPU waiting. Here both channels run in one SIMD register and the
    sections overlap (section 2 works on sample i while section 1 already starts on i + 1): about 6x faster than the
    float loops before, and in double precision (see BiquadCoeffs). */
template <int N>
inline void biquadGroup (float* l, float* r, int n, const BiquadLane* left, int numLeft, const BiquadLane* right, int numRight) noexcept
{
    __m128d b0[N], b1[N], b2[N], a1[N], a2[N], s1[N], s2[N];
    for (int j = 0; j < N; ++j)
    {
        const BiquadCoeffs one;   // identity: out = in, the state stays 0
        const auto& cl = j < numLeft ? *left[j].c : one;
        const auto& cr = j < numRight ? *right[j].c : one;
        b0[j] = _mm_setr_pd (cl.b0, cr.b0);
        b1[j] = _mm_setr_pd (cl.b1, cr.b1);
        b2[j] = _mm_setr_pd (cl.b2, cr.b2);
        a1[j] = _mm_setr_pd (cl.a1, cr.a1);
        a2[j] = _mm_setr_pd (cl.a2, cr.a2);
        s1[j] = _mm_setr_pd (j < numLeft ? *left[j].z1 : 0.0, j < numRight ? *right[j].z1 : 0.0);
        s2[j] = _mm_setr_pd (j < numLeft ? *left[j].z2 : 0.0, j < numRight ? *right[j].z2 : 0.0);
    }
    for (int i = 0; i < n; ++i)
    {
        __m128d x = _mm_cvtps_pd (_mm_unpacklo_ps (_mm_load_ss (l + i), _mm_load_ss (r + i)));
        for (int j = 0; j < N; ++j)
        {
            const __m128d o = _mm_fmadd_pd (b0[j], x, s1[j]);
            s1[j] = _mm_fmadd_pd (b1[j], x, _mm_fnmadd_pd (a1[j], o, s2[j]));
            s2[j] = _mm_fnmadd_pd (a2[j], o, _mm_mul_pd (b2[j], x));
            x = o;
        }
        const __m128 y = _mm_cvtpd_ps (x);
        _mm_store_ss (l + i, y);
        _mm_store_ss (r + i, _mm_movehdup_ps (y));
    }
    for (int j = 0; j < N; ++j)
    {
        alignas (16) double a[2], b[2];
        _mm_store_pd (a, s1[j]);
        _mm_store_pd (b, s2[j]);
        if (j < numLeft)  { *left[j].z1 = a[0];  *left[j].z2 = b[0]; }
        if (j < numRight) { *right[j].z1 = a[1]; *right[j].z2 = b[1]; }
    }
}
} // namespace detail
#endif

/** Runs the sections of each channel one after the other over l and r (exactly like processing them in turn).
    The two lists may differ (left / right bands, separate speaker corrections). */
inline void runBiquads (float* l, float* r, int n, const BiquadLane* left, int numLeft, const BiquadLane* right, int numRight) noexcept
{
    if (n <= 0) return;
#if defined (__AVX2__)
    for (int k = 0; k < numLeft || k < numRight; k += 4)
    {
        const int nl = juce::jlimit (0, 4, numLeft - k), nr = juce::jlimit (0, 4, numRight - k);
        const BiquadLane* L = left + juce::jmin (k, numLeft);
        const BiquadLane* R = right + juce::jmin (k, numRight);
        switch (juce::jmax (nl, nr))
        {
            case 1:  detail::biquadGroup<1> (l, r, n, L, nl, R, nr); break;
            case 2:  detail::biquadGroup<2> (l, r, n, L, nl, R, nr); break;
            case 3:  detail::biquadGroup<3> (l, r, n, L, nl, R, nr); break;
            default: detail::biquadGroup<4> (l, r, n, L, nl, R, nr); break;
        }
    }
#else
    auto run = [n] (float* x, const BiquadLane* s, int count)
    {
        for (int k = 0; k < count; ++k)
        {
            const auto& c = *s[k].c;
            double s1 = *s[k].z1, s2 = *s[k].z2;
            for (int i = 0; i < n; ++i)
            {
                const double in = x[i];
                const double out = c.b0 * in + s1;
                s1 = c.b1 * in - c.a1 * out + s2;
                s2 = c.b2 * in - c.a2 * out;
                x[i] = (float) out;
            }
            *s[k].z1 = s1; *s[k].z2 = s2;
        }
    };
    run (l, left, numLeft);
    run (r, right, numRight);
#endif
}

inline void StereoBiquad::process (float* l, float* r, int n) noexcept
{
    const BiquadLane left { &c, &z1[0], &z2[0] }, right { &c, &z1[1], &z2[1] };
    runBiquads (l, r, n, &left, 1, &right, 1);
}

inline void StereoBiquad::processOne (int k, float* x, int n) noexcept
{
    double s1 = z1[k], s2 = z2[k];
    for (int i = 0; i < n; ++i)
    {
        const double in = x[i];
        const double out = c.b0 * in + s1;
        s1 = c.b1 * in - c.a1 * out + s2;
        s2 = c.b2 * in - c.a2 * out;
        x[i] = (float) out;
    }
    z1[k] = s1; z2[k] = s2;
}

struct GateSettings
{
    float thresholdDb = -50.0f;
    float rangeDb = -80.0f;     // attenuation while closed ("damping")
    float attackMs = 1.0f;
    float holdMs = 80.0f;
    float releaseMs = 120.0f;
    bool sidechain = false;     // band-pass filtered detector (e.g. ignore rumble / hiss)
    float sidechainHz = 1000.0f;
    bool voiceKey = false;      // additionally require speech (RNNoise voice probability)
    float voiceThreshold = 0.5f;
    float voice = -1.0f;        // current voice probability 0..1, < 0 = unavailable (denoiser off)
};

/** Noise gate with hold, range, optional band-pass sidechain and optional voice key
    (opens only while the denoiser detects speech). Stereo-linked, zero latency. */
struct Gate
{
    double sr = 48000.0;
    float attackCoef = 0, releaseCoef = 0, envCoef = 0;
    float env = 0, gain = 1;
    int holdCounter = 0;
    float cache[3] { -1, -1, -1 };
    float scCache = -1.0f;
    BiquadCoeffs sc;
    double scZ1 = 0, scZ2 = 0;
    std::atomic<float> shownGain { 1.0f };

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        envCoef = (float) std::exp (-1.0 / (0.005 * sr));
        env = 0; gain = 1; holdCounter = 0; scZ1 = scZ2 = 0;
        cache[0] = cache[1] = cache[2] = -1.0f; scCache = -1.0f;
    }

    void process (float* l, float* r, int n, const GateSettings& s) noexcept
    {
        if (s.attackMs != cache[0])  { attackCoef  = (float) std::exp (-1.0 / (juce::jmax (0.05f, s.attackMs) * 0.001 * sr)); cache[0] = s.attackMs; }
        if (s.releaseMs != cache[1]) { releaseCoef = (float) std::exp (-1.0 / (juce::jmax (1.0f, s.releaseMs) * 0.001 * sr)); cache[1] = s.releaseMs; }
        if (s.sidechain && s.sidechainHz != scCache) { sc = BiquadCoeffs::make (BiquadCoeffs::Type::BandPass, sr, s.sidechainHz, 1.0, 0.0); scCache = s.sidechainHz; }

        const float thr = dbToGain (s.thresholdDb, -200.0f);
        const float floorGain = dbToGain (s.rangeDb, -200.0f);
        const int holdSamples = (int) (s.holdMs * 0.001 * sr);
        const bool voiceOk = ! s.voiceKey || s.voice < 0.0f || s.voice >= s.voiceThreshold;
        for (int i = 0; i < n; ++i)
        {
            float a;
            if (s.sidechain)
            {
                const double in = 0.5f * (l[i] + r[i]);
                const double o = sc.b0 * in + scZ1;
                scZ1 = sc.b1 * in - sc.a1 * o + scZ2;
                scZ2 = sc.b2 * in - sc.a2 * o;
                a = (float) std::abs (o);
            }
            else
                a = juce::jmax (std::abs (l[i]), std::abs (r[i]));
            env = a > env ? a : env * envCoef + a * (1.0f - envCoef);

            float targetGain;
            if (env >= thr && voiceOk) { holdCounter = holdSamples; targetGain = 1.0f; }
            else if (holdCounter > 0) { --holdCounter; targetGain = 1.0f; }
            else targetGain = floorGain;

            const float coef = targetGain > gain ? attackCoef : releaseCoef;
            gain = targetGain + coef * (gain - targetGain);
            l[i] *= gain; r[i] *= gain;
        }
        shownGain.store (gain, std::memory_order_relaxed);
    }
};

struct CompSettings
{
    float inputDb = 0.0f;
    float thresholdDb = -20.0f;
    float ratio = 3.0f;
    float kneeDb = 6.0f;
    float attackMs = 5.0f;
    float releaseMs = 150.0f;
    float makeupDb = 0.0f;
    bool autoMakeup = false;

    float staticGainDb (float inDb) const noexcept
    {
        const float slope = 1.0f - 1.0f / juce::jmax (1.0f, ratio);
        const float over = inDb - thresholdDb;
        const float k = juce::jmax (0.01f, kneeDb);
        if (over <= -k * 0.5f) return 0.0f;
        if (over >= k * 0.5f)  return -slope * over;
        const float t = over + k * 0.5f;
        return -slope * t * t / (2.0f * k);
    }

    float totalMakeupDb() const noexcept
    {
        // Auto gain: fully restore the level of a full-scale signal (0 dBFS in -> 0 dBFS out).
        return makeupDb + (autoMakeup ? -staticGainDb (0.0f) : 0.0f);
    }
};

/** Feed-forward compressor, soft knee, stereo-linked, zero latency. */
struct Compressor
{
    static constexpr float kDbToLog2 = 0.16609640474f;   // log2 (10) / 20
    static constexpr float kLog2ToDb = 6.0205999133f;    // 20 log10 (2)
    double sr = 48000.0;
    float attackCoef = 0, releaseCoef = 0;
    float envDb = 0.0f;   // current gain reduction (positive dB)
    float lastGr = 0;
    float cache[2] { -1, -1 };

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        envDb = 0.0f;
        cache[0] = cache[1] = -1.0f;
    }

    void process (float* l, float* r, int n, const CompSettings& s) noexcept
    {
        if (s.attackMs != cache[0])  { attackCoef  = (float) std::exp (-1.0 / (juce::jmax (0.05f, s.attackMs) * 0.001 * sr)); cache[0] = s.attackMs; }
        if (s.releaseMs != cache[1]) { releaseCoef = (float) std::exp (-1.0 / (juce::jmax (1.0f, s.releaseMs) * 0.001 * sr)); cache[1] = s.releaseMs; }

        const float inGain = dbToGain (s.inputDb, -200.0f);
        const float makeupDb = s.totalMakeupDb();
        const float makeupGain = std::exp2 (makeupDb * kDbToLog2);
        // Below the knee the curve does nothing: no logarithm needed (the level compared linearly).
        const float kneeStart = std::exp2 ((s.thresholdDb - 0.5f * juce::jmax (0.01f, s.kneeDb)) * kDbToLog2);
        float maxGr = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            l[i] *= inGain; r[i] *= inGain;
            const float a = juce::jmax (std::abs (l[i]), std::abs (r[i]));
            const float gr = a > kneeStart && a > 1.0e-6f ? -s.staticGainDb (std::log2 (a) * kLog2ToDb) : 0.0f;

            const float coef = gr > envDb ? attackCoef : releaseCoef;
            envDb = gr + coef * (envDb - gr);
            if (envDb < 1.0e-6f) envDb = 0.0f;   // released: below float resolution of the gain, exactly no reduction
            maxGr = juce::jmax (maxGr, envDb);
            const float g = envDb == 0.0f ? makeupGain : std::exp2 ((makeupDb - envDb) * kDbToLog2);
            l[i] *= g; r[i] *= g;
        }
        lastGr = maxGr;
    }
};

/** Zero-latency peak limiter: instant attack, smooth release, final safety clip. */
struct Limiter
{
    float releaseCoef = 0, gain = 1;

    void prepare (double sr) { releaseCoef = (float) std::exp (-1.0 / (0.08 * sr)); gain = 1; }

    void process (float* l, float* r, int n, float ceilingDb) noexcept
    {
        const float ceil = dbToGain (ceilingDb, -200.0f);
        for (int i = 0; i < n; ++i)
        {
            const float a = juce::jmax (std::abs (l[i]), std::abs (r[i]));
            const float need = a * gain > ceil ? ceil / a : 1.0f;
            gain = need < gain ? need : 1.0f + releaseCoef * (gain - 1.0f);
            if (a * gain > ceil) gain = ceil / a;
            l[i] = juce::jlimit (-ceil, ceil, l[i] * gain);
            r[i] = juce::jlimit (-ceil, ceil, r[i] * gain);
        }
    }
};

/** Linear per-block gain ramp (click-free fader moves without per-sample smoothing cost). */
inline void applyRamp (float* x, int n, float g0, float g1) noexcept
{
    if (g0 == g1)
    {
        if (g0 == 1.0f) return;
        juce::FloatVectorOperations::multiply (x, g0, n);
        return;
    }
    const float step = (g1 - g0) / (float) n;
    float g = g0;
    for (int i = 0; i < n; ++i) { x[i] *= g; g += step; }
}

/** Add src * (g0 -> g1 ramp) into dst. */
inline void addRamp (float* dst, const float* src, int n, float g0, float g1) noexcept
{
    if (g0 == g1)
    {
        if (g0 != 0.0f) juce::FloatVectorOperations::addWithMultiply (dst, src, g0, n);
        return;
    }
    const float step = (g1 - g0) / (float) n;
    float g = g0;
    for (int i = 0; i < n; ++i) { dst[i] += src[i] * g; g += step; }
}

/** Stereo delay for Sync Delay (aligning channels with different device latencies). Costs nothing while
    the delay is 0. A new delay is reached with a short crossfade between the old and the new read position,
    so changes (manual or the automatic sync following the drift buffers) do not click. */
struct DelayLine
{
    static constexpr double kMaxMs = 500.0;

    /** Allocates for kMaxMs at this rate (not on the audio thread). */
    void prepare (double sampleRate)
    {
        size = (int) std::ceil (sampleRate * kMaxMs / 1000.0) + 1;
        bufL.assign ((size_t) size, 0.0f);
        bufR.assign ((size_t) size, 0.0f);
        fadeLen = juce::jmax (16, (int) (sampleRate * 0.01));   // 10 ms
        write = written = current = previous = fadeLeft = 0;
        active = false;
    }

    int maxDelay() const noexcept { return size - 1; }
    int getDelay() const noexcept { return current; }

    void process (float* l, float* r, int n, int wanted) noexcept
    {
        if (size <= 1) return;
        wanted = juce::jlimit (0, size - 1, wanted);
        if (! active)
        {
            if (wanted == 0) return;
            // Nothing was recorded while the delay was 0: the history starts now. Reads from before that are
            // silent and the recorded signal fades in, so it does not start with a jump.
            write = current = written = 0;
            active = true;
        }
        if (fadeLeft == 0 && wanted != current)
        {
            previous = current;
            current = wanted;
            fadeLeft = fadeLen;
        }
        float* L = bufL.data();
        float* R = bufR.data();
        auto tap = [&] (int d, float& outL, float& outR)
        {
            if (d == 0) { outL = L[write]; outR = R[write]; return; }   // the direct signal
            const int age = written - d;   // samples recorded before the one d samples ago (>= 1: it exists)
            if (age <= 0) { outL = outR = 0.0f; return; }
            int rd = write - d;
            if (rd < 0) rd += size;
            const float g = age >= fadeLen ? 1.0f : (float) age / (float) fadeLen;
            outL = L[rd] * g;
            outR = R[rd] * g;
        };
        for (int i = 0; i < n; ++i)
        {
            L[write] = l[i];
            R[write] = r[i];
            if (written < size + fadeLen) ++written;
            float outL, outR;
            tap (current, outL, outR);
            if (fadeLeft > 0)
            {
                float pl, pr;
                tap (previous, pl, pr);
                const float t = (float) fadeLeft / (float) fadeLen;   // weight of the old position
                outL += t * (pl - outL);
                outR += t * (pr - outR);
                --fadeLeft;
            }
            l[i] = outL;
            r[i] = outR;
            if (++write == size) write = 0;
        }
        if (current == 0 && fadeLeft == 0) active = false;   // back to zero delay: stop recording
    }

private:
    std::vector<float> bufL, bufR;
    int size = 0, write = 0, written = 0, current = 0, previous = 0, fadeLeft = 0, fadeLen = 480;
    bool active = false;
};

inline float peak (const float* x, int n) noexcept
{
    auto r = juce::FloatVectorOperations::findMinAndMax (x, n);
    return juce::jmax (-r.getStart(), r.getEnd());
}
} // namespace hydra::dsp

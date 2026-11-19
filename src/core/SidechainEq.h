#pragma once
#include "Dsp.h"
#include <array>
#include <atomic>

/** Sidechain EQ (18.11., wish of the user: "a channel makes room for another, so that one can be heard better").
    A key signal (another channel, after its effects) turns an EQ on this channel down where and while the key has
    energy, e.g. the music dips around 1-4 kHz while the voice talks. Zero latency: the key is measured with band
    passes, the target runs through bell / shelf biquads whose gains follow the key per block.

    Two modes:
    - Auto (spectral, like Trackspacer): kAutoBands bells over the whole range; each one cuts as much as the key is
      loud in its band (above SENSITIVITY, full DEPTH 12 dB above it), within LOW..HIGH, scaled by AMOUNT.
    - Bands: up to kBands dynamic bands (bell / low shelf / high shelf) with their own frequency, Q, range (cut or
      boost at full key) and threshold; each listens to the key in its own region.
    Bands with no reduction cost nothing; with the key silent the whole effect only runs its key detector. */
namespace hydra::dsp
{
struct SidechainEqSettings
{
    static constexpr int kBands = 6;
    enum Mode { autoMode = 0, bandsMode = 1 };
    enum BandType { bell = 0, lowShelf = 1, highShelf = 2 };

    int mode = autoMode;
    float amount = 1.0f;          // 0..1, scales every reduction
    float depthDb = 6.0f;         // Auto: deepest cut
    float sensDb = -50.0f;        // Auto: key band level where cutting starts (dBFS)
    float loHz = 100.0f, hiHz = 10000.0f;   // Auto: region that may be cut
    float attackMs = 10.0f, releaseMs = 150.0f;
    struct Band
    {
        bool on = false;
        int type = bell;
        float freq = 1000.0f, q = 1.0f;
        float rangeDb = -6.0f;    // gain at full key (negative = cut)
        float thrDb = -40.0f;     // key level (in this band's region) where it starts
    };
    std::array<Band, kBands> bands {};
};

class SidechainEq
{
public:
    static constexpr int kAutoBands = 24;
    static constexpr int kBands = SidechainEqSettings::kBands;
    /** Key level above the threshold at which the full depth / range is reached. */
    static constexpr float kKneeDb = 12.0f;

    /** Centre frequency of an Auto band: 24 bands, log spaced 50 Hz .. 16 kHz (about 0.36 octaves apart). */
    static double autoFreq (int b) noexcept { return 50.0 * std::pow (16000.0 / 50.0, (double) b / (kAutoBands - 1)); }
    /** Q of the Auto bells: wider than the spacing so equal cuts give a smooth curve (ripple ~0.6 dB at 9 dB). */
    static constexpr double kAutoQ = 3.0;
    /** The bells overlap: equal cuts on all of them add up to about this much at a centre (computed for Q 3; see
        tests); each band cuts depth / kAutoOverlap so the total is about the depth. */
    static constexpr float kAutoOverlap = 1.95f;
    /** Q of each of the two band passes of an Auto detector (4th order: a loud key leaks less into far bands). */
    static constexpr double kDetQ = 2.5;

    void prepare (double sampleRate) noexcept
    {
        sr = sampleRate;
        reset();
        for (int b = 0; b < kAutoBands; ++b)
        {
            autoDet[(size_t) b].c = BiquadCoeffs::make (BiquadCoeffs::Type::BandPass, sr, autoFreq (b), kDetQ, 0.0);
            autoDet[(size_t) b].tau = detectorTau (autoFreq (b));
        }
        bandsKey = {};
    }

    void reset() noexcept
    {
        for (auto& d : autoDet) d.reset();
        for (auto& d : bandDet) d.reset();
        for (auto& f : autoFilt) { f.bq.reset(); f.gainDb = 0.0f; f.active = false; }
        for (auto& f : bandFilt) { f.bq.reset(); f.gainDb = 0.0f; f.active = false; }
        autoRed.fill (0.0f);
        bandGain.fill (0.0f);
        publish();
    }

    /** Measures the key (mono, any length; nullptr / 0 = silence for n samples of the target). Call once per target
        block before process(). */
    void feedKey (const float* key, int keyN, int targetN, const SidechainEqSettings& s) noexcept
    {
        const bool silent = key == nullptr || keyN <= 0;
        const int n = silent ? targetN : keyN;
        if (n <= 0) return;
        if (s.mode == SidechainEqSettings::autoMode)
        {
            for (auto& d : autoDet) d.run (key, n, sr, silent);
        }
        else
        {
            for (int b = 0; b < kBands; ++b)
            {
                const auto& band = s.bands[(size_t) b];
                auto& d = bandDet[(size_t) b];
                if (! band.on) { d.reset(); continue; }
                // Detector of the band's region: band pass for a bell, low / high pass for the shelves.
                const auto type = band.type == SidechainEqSettings::lowShelf ? BiquadCoeffs::Type::HighCut
                                : band.type == SidechainEqSettings::highShelf ? BiquadCoeffs::Type::LowCut : BiquadCoeffs::Type::BandPass;
                const BandsKey k { band.type, band.freq, band.q };
                if (! (k == bandsKey[(size_t) b]))
                {
                    bandsKey[(size_t) b] = k;
                    d.c = BiquadCoeffs::make (type, sr, band.freq, type == BiquadCoeffs::Type::BandPass ? juce::jmax (0.5f, band.q) : 0.707, 0.0);
                    d.tau = detectorTau (band.freq);
                }
                d.run (key, n, sr, silent);
            }
        }
        // Gain targets, then attack / release (per block, on the dB value).
        const double dt = (double) n / sr;
        const float att = (float) (1.0 - std::exp (-dt / juce::jmax (0.0005, s.attackMs * 0.001)));
        const float rel = (float) (1.0 - std::exp (-dt / juce::jmax (0.002, s.releaseMs * 0.001)));
        auto follow = [att, rel] (float& v, float target)
        {
            v += (target - v) * (std::abs (target) > std::abs (v) ? att : rel);
            if (std::abs (v) < 0.005f && std::abs (target) < 0.005f) v = 0.0f;
        };
        if (s.mode == SidechainEqSettings::autoMode)
        {
            const float lo = juce::jmax (10.0f, juce::jmin (s.loHz, s.hiHz)), hi = juce::jmax (s.loHz, s.hiHz);
            for (int b = 0; b < kAutoBands; ++b)
            {
                const auto f = (float) autoFreq (b);
                // Fades over a third of an octave outside LOW..HIGH.
                const float oct = f < lo ? std::log2 (lo / f) : f > hi ? std::log2 (f / hi) : 0.0f;
                const float w = juce::jlimit (0.0f, 1.0f, 1.0f - oct * 3.0f);
                const float over = juce::jlimit (0.0f, 1.0f, (autoDet[(size_t) b].levelDb() - s.sensDb) / kKneeDb);
                follow (autoRed[(size_t) b], -s.depthDb * s.amount * over * w);
            }
            for (auto& g : bandGain) follow (g, 0.0f);
        }
        else
        {
            for (int b = 0; b < kBands; ++b)
            {
                const auto& band = s.bands[(size_t) b];
                const float over = band.on ? juce::jlimit (0.0f, 1.0f, (bandDet[(size_t) b].levelDb() - band.thrDb) / kKneeDb) : 0.0f;
                follow (bandGain[(size_t) b], band.rangeDb * s.amount * over);
            }
            for (auto& g : autoRed) follow (g, 0.0f);
        }
        publish();
    }

    /** The EQ on the target (stereo, in place), with the gains of the last feedKey. */
    void process (float* l, float* r, int n, const SidechainEqSettings& s) noexcept
    {
        BiquadLane lanes[2][kAutoBands + kBands];
        int count = 0;
        auto add = [&] (Filter& f, float gainDb, BiquadCoeffs::Type type, double freq, double q)
        {
            if (gainDb == 0.0f) { f.active = false; return; }
            if (! f.active) { f.bq.reset(); f.active = true; f.gainDb = 1.0e9f; }
            if (std::abs (gainDb - f.gainDb) > 0.01f || f.freq != freq || f.q != q || f.type != type)
            {
                f.bq.c = BiquadCoeffs::make (type, sr, freq, q, gainDb);
                f.gainDb = gainDb; f.freq = freq; f.q = q; f.type = type;
            }
            lanes[0][count] = { &f.bq.c, &f.bq.z1[0], &f.bq.z2[0] };
            lanes[1][count] = { &f.bq.c, &f.bq.z1[1], &f.bq.z2[1] };
            ++count;
        };
        for (int b = 0; b < kAutoBands; ++b)
            add (autoFilt[(size_t) b], autoRed[(size_t) b] / kAutoOverlap, BiquadCoeffs::Type::Peak, autoFreq (b), kAutoQ);
        for (int b = 0; b < kBands; ++b)
        {
            const auto& band = s.bands[(size_t) b];
            const auto type = band.type == SidechainEqSettings::lowShelf ? BiquadCoeffs::Type::LowShelf
                            : band.type == SidechainEqSettings::highShelf ? BiquadCoeffs::Type::HighShelf : BiquadCoeffs::Type::Peak;
            add (bandFilt[(size_t) b], bandGain[(size_t) b], type, band.freq, filterQ (band));
        }
        if (count > 0) runBiquads (l, r, n, lanes[0], count, lanes[1], count);
    }

    /** Response (dB) at count frequencies. live: the EQ as it is now (published gains); otherwise what the bands do
        at full key (Bands mode, the range of each band at AMOUNT). Message thread. */
    void response (const float* freqs, float* outDb, int count, const SidechainEqSettings& s, bool live) const
    {
        for (int i = 0; i < count; ++i) outDb[i] = 0.0f;
        auto add = [&] (const BiquadCoeffs& c) { for (int i = 0; i < count; ++i) outDb[i] += (float) c.magnitudeDb (freqs[i], sr); };
        if (live)
            for (int b = 0; b < kAutoBands; ++b)
                if (const float g = shownAuto[(size_t) b].load (std::memory_order_relaxed); g != 0.0f)
                    add (BiquadCoeffs::make (BiquadCoeffs::Type::Peak, sr, autoFreq (b), kAutoQ, g / kAutoOverlap));
        for (int b = 0; b < kBands; ++b)
        {
            const auto& band = s.bands[(size_t) b];
            const float g = live ? shownBand[(size_t) b].load (std::memory_order_relaxed) : (band.on ? band.rangeDb * s.amount : 0.0f);
            if (g != 0.0f) add (bandCoeffs (band, g));
        }
    }

    BiquadCoeffs bandCoeffs (const SidechainEqSettings::Band& band, float gainDb) const
    {
        const auto type = band.type == SidechainEqSettings::lowShelf ? BiquadCoeffs::Type::LowShelf
                        : band.type == SidechainEqSettings::highShelf ? BiquadCoeffs::Type::HighShelf : BiquadCoeffs::Type::Peak;
        return BiquadCoeffs::make (type, sr, band.freq, filterQ (band), gainDb);
    }
    /** Q of a band's filter: a bell takes Q as it is; a shelf Q x 0.707 (Q 1 = the steepest slope without overshoot). */
    static double filterQ (const SidechainEqSettings::Band& band) noexcept
    {
        return band.type == SidechainEqSettings::bell ? juce::jmax (0.1, (double) band.q) : juce::jlimit (0.2, 1.4, band.q * 0.7071);
    }

    /** Live values for the UI: current gain per Auto band / per band (dB), key level per Auto band / band (dBFS). */
    std::array<std::atomic<float>, kAutoBands> shownAuto {}, shownAutoKey {};
    std::array<std::atomic<float>, kBands> shownBand {}, shownBandKey {};
    /** Deepest cut right now (dB, <= 0) / highest boost, for the block. */
    std::atomic<float> shownMin { 0.0f }, shownMax { 0.0f };
    double getSampleRate() const noexcept { return sr; }

private:
    /** Envelope of a band: the filter twice (4th order), mean square with a time constant of a few periods (min. 5 ms). */
    struct Detector
    {
        BiquadCoeffs c;
        double z1 = 0, z2 = 0, y1 = 0, y2 = 0, power = 0;
        double tau = 0.01;
        void reset() noexcept { z1 = z2 = y1 = y2 = power = 0.0; }
        void run (const float* x, int n, double sr, bool silent) noexcept
        {
            const double a = 1.0 - std::exp (-1.0 / (tau * sr));
            if (silent)
            {
                // Only the power decays (the filter state rings out with it: dropped, inaudible in the envelope).
                z1 = z2 = y1 = y2 = 0.0;
                power *= std::pow (1.0 - a, (double) n);
                if (power < 1.0e-14) power = 0.0;
                return;
            }
            double s1 = z1, s2 = z2, t1 = y1, t2 = y2, p = power;
            for (int i = 0; i < n; ++i)
            {
                const double in = x[i];
                const double mid = c.b0 * in + s1;
                s1 = c.b1 * in - c.a1 * mid + s2;
                s2 = c.b2 * in - c.a2 * mid;
                const double out = c.b0 * mid + t1;
                t1 = c.b1 * mid - c.a1 * out + t2;
                t2 = c.b2 * mid - c.a2 * out;
                p += a * (out * out - p);
            }
            z1 = s1; z2 = s2; y1 = t1; y2 = t2; power = p < 1.0e-14 ? 0.0 : p;
        }
        /** RMS in dBFS, +3 dB so a sine reads as its peak level. */
        float levelDb() const noexcept { return power > 0.0 ? (float) (10.0 * std::log10 (power) + 3.0103) : -150.0f; }
    };
    static double detectorTau (double freq) noexcept { return juce::jmax (0.005, 3.0 / freq); }

    struct Filter
    {
        StereoBiquad bq;
        float gainDb = 0.0f;
        double freq = 0.0, q = 0.0;
        BiquadCoeffs::Type type = BiquadCoeffs::Type::Peak;
        bool active = false;
    };
    struct BandsKey
    {
        int type = -1; float freq = 0, q = 0;
        bool operator== (const BandsKey& o) const noexcept { return type == o.type && freq == o.freq && q == o.q; }
    };

    void publish() noexcept
    {
        float mn = 0.0f, mx = 0.0f;
        for (int b = 0; b < kAutoBands; ++b)
        {
            shownAuto[(size_t) b].store (autoRed[(size_t) b], std::memory_order_relaxed);
            shownAutoKey[(size_t) b].store (autoDet[(size_t) b].levelDb(), std::memory_order_relaxed);
            mn = juce::jmin (mn, autoRed[(size_t) b]);
        }
        for (int b = 0; b < kBands; ++b)
        {
            shownBand[(size_t) b].store (bandGain[(size_t) b], std::memory_order_relaxed);
            shownBandKey[(size_t) b].store (bandDet[(size_t) b].levelDb(), std::memory_order_relaxed);
            mn = juce::jmin (mn, bandGain[(size_t) b]);
            mx = juce::jmax (mx, bandGain[(size_t) b]);
        }
        shownMin.store (mn, std::memory_order_relaxed);
        shownMax.store (mx, std::memory_order_relaxed);
    }

    double sr = 48000.0;
    std::array<Detector, kAutoBands> autoDet {};
    std::array<Detector, kBands> bandDet {};
    std::array<BandsKey, kBands> bandsKey {};
    std::array<Filter, kAutoBands> autoFilt {};
    std::array<Filter, kBands> bandFilt {};
    std::array<float, kAutoBands> autoRed {};
    std::array<float, kBands> bandGain {};
};
} // namespace hydra::dsp

#pragma once
#include "Dsp.h"
#include "ParamEq.h"
#include "MatchEq.h"

/** Voice effects: Saturation and Studio Voice (one knob that runs a whole vocal chain).
    All zero latency and allocation-free. Studio Voice is built from exactly the blocks a user gets when the effect
    is turned into a real chain (EQ, Gate, Compressor, EQ, Saturation, Limiter), so converting it changes
    nothing audible: VoiceDesign is the set of settings both use. */
namespace hydra::dsp
{
//==============================================================================
struct SaturatorSettings
{
    float driveDb = 6.0f;    // how hard the signal hits the curve
    float warmth = 0.3f;     // 0..1: asymmetry (even harmonics, like tape / tubes); 0 = symmetric (odd harmonics)
    float mix = 0.5f;        // 0..1 dry / saturated
    float outputDb = 0.0f;
};

/** Soft saturation: a tanh-like curve, level-matched for small signals (only peaks get denser), optional asymmetry
    with a DC blocker. No oversampling (zero latency); at voice levels and moderate drive the aliasing stays low. */
struct Saturator
{
    float dcCoef = 0.999f;
    float dcX[2] {}, dcY[2] {};

    void prepare (double sr)
    {
        dcCoef = (float) std::exp (-juce::MathConstants<double>::twoPi * 8.0 / sr);
        reset();
    }
    void reset() noexcept { dcX[0] = dcX[1] = dcY[0] = dcY[1] = 0.0f; }

    /** Padé approximation of tanh, exact at 0, saturates to +-1 (cheap, smooth). */
    static float softClip (float x) noexcept
    {
        if (x > 3.0f) return 1.0f;
        if (x < -3.0f) return -1.0f;
        const float x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }

    void process (float* l, float* r, int n, const SaturatorSettings& s) noexcept
    {
        const float drive = dbToGain (s.driveDb, -200.0f);
        const float bias = juce::jlimit (0.0f, 1.0f, s.warmth) * 0.35f;
        const float b0 = softClip (bias);
        // Small-signal gain of the curve at the bias: normalised away, so quiet parts keep their level.
        const float eps = 1.0e-3f;
        const float g0 = (softClip (bias + eps) - softClip (bias - eps)) / (2.0f * eps);
        const float norm = 1.0f / (drive * juce::jmax (0.05f, g0));
        const float mix = juce::jlimit (0.0f, 1.0f, s.mix);
        const float out = dbToGain (s.outputDb, -200.0f);
        float* ch[2] = { l, r };
        for (int c = 0; c < 2; ++c)
        {
            float* x = ch[c];
            float px = dcX[c], py = dcY[c];
            for (int i = 0; i < n; ++i)
            {
                const float dry = x[i];
                float w = (softClip (drive * dry + bias) - b0) * norm;
                if (bias > 0.0f)
                {
                    const float y = w - px + dcCoef * py;   // DC blocker (asymmetry shifts the average)
                    px = w; py = y; w = y;
                }
                x[i] = (dry + mix * (w - dry)) * out;
            }
            dcX[c] = px; dcY[c] = py;
        }
    }
};

//==============================================================================
/** One EQ band of a VoiceDesign (ParamEq band types). */
struct VoiceBand
{
    int type = ParamEq::Bell;
    float freq = 1000.0f, q = 1.0f, gainDb = 0.0f;
};

/** All settings of the Studio Voice chain at one moment: what it runs and what "Split into effects" writes.
    Order: EQ (high pass, then the profile's character or the learned match of the microphone) -> gate -> compressor
    -> second compressor (fast peak stage, Radio / Singing) -> saturation -> limiter. One EQ (user 13.11.). */
struct VoiceDesign
{
    static constexpr int kMaxMatch = 7;               // bands of a learned match
    static constexpr int kBands = 1 + kMaxMatch;      // high pass + character or match
    std::array<VoiceBand, kBands> bands {};
    int numBands = 0;
    GateSettings gate;
    CompSettings comp;
    bool comp2On = false;
    CompSettings comp2;
    SaturatorSettings sat;
    float ceilingDb = -1.0f;
};

/** A few biquads with cached coefficients (the same filters ParamEq runs for these bands). */
template <int N>
struct VoiceEq
{
    StereoBiquad filters[N], filters2[N];   // filters2: second section of a cut (24 dB/oct, like ParamEq)
    int numSections[N] {};
    float cache[N][4] {};
    void reset() noexcept { for (auto& f : filters) f.reset(); for (auto& f : filters2) f.reset(); for (auto& c : cache) c[0] = -1.0f; }
    void process (const std::array<VoiceBand, N>& bands, int num, double sr, float* l, float* r, int n) noexcept
    {
        for (int k = 0; k < num; ++k)
        {
            const auto& b = bands[(size_t) k];
            if (ParamEq::typeUsesGain (b.type) && b.gainDb == 0.0f) continue;
            auto& c = cache[k];
            if (c[0] != (float) b.type || c[1] != b.gainDb || c[2] != b.freq || c[3] != b.q)
            {
                const int was = numSections[k];
                numSections[k] = ParamEq::sections (b.type, sr, b.freq, b.q, b.gainDb, filters[k].c, filters2[k].c);
                if (numSections[k] > 1 && was < 2) filters2[k].reset();
                c[0] = (float) b.type; c[1] = b.gainDb; c[2] = b.freq; c[3] = b.q;
            }
            filters[k].process (l, r, n);
            if (numSections[k] > 1) filters2[k].process (l, r, n);
        }
    }
};

/** Studio Voice: one knob (amount 0..1) and a profile run a whole vocal chain (VoiceDesign). The gate threshold
    follows the measured noise floor; the compressor's input gain levels the voice to the profile's target loudness
    (measured only while someone speaks), so its threshold can be fixed relative to that target. At amount 0 the
    signal passes untouched; EQ gains, gate range, ratios, leveling and drive all scale with the amount.

    Profiles (sources: mixing / podcast / broadcast guides, see CLAUDE.md): high pass 75-100 Hz, mud cut 250-400 Hz,
    presence 2.5-4 kHz, air shelf 10-12 kHz; compression 2.5:1-4:1, radio with a second fast stage like a limiter. */
class StudioVoice
{
public:
    enum Profile { podcast = 0, radio, stream, singing, numProfiles };
    static juce::StringArray profileNames() { return { "Podcast", "Radio", "Stream", "Singing" }; }
    static juce::String profileHint (int p)
    {
        switch (p)
        {
            case radio:   return "Like a radio host: deep, full lows, dense and punchy, always up front";
            case stream:  return "Cuts through game audio: clear presence, no boom, strong gate against keyboard noise";
            case singing: return "Vocals over music: gentle compression that keeps the dynamics, air on top";
            default:      return "Clear and warm like a produced podcast: even level, a little presence";
        }
    }

    /** What the profile does at full amount. Compressor thresholds are relative to targetDb (the leveled voice).
        bands = the profile's character (used as it is without a learned microphone; with one it is part of the
        target the match aims for). */
    struct Recipe
    {
        float hpfHz;
        std::array<VoiceBand, 4> bands; int numBands;
        float gateRangeDb, gateReleaseMs;
        float compThrRel, compRatio, compKneeDb, compAttackMs, compReleaseMs;
        bool comp2On; float comp2ThrRel, comp2Ratio, comp2AttackMs, comp2ReleaseMs;
        float driveDb, warmth, satMix;
        float targetDb;        // speech level (RMS, dBFS) the leveling aims for
        float levelSeconds;    // how slowly the leveling follows
        float makeupAtDb;      // makeup = the reduction at targetDb + this (heavier compression needs a higher point)
    };
    static const Recipe& recipe (int profile) noexcept
    {
        using B = ParamEq::BandType;
        static const Recipe r[numProfiles] = {
            // podcast: HPF 80, -3 dB mud at 250, +1.5 warmth at 120, +2.5 presence at 4k, +1.5 air at 10k; 3:1, 10 / 150 ms
            { 80.0f, {{ { B::LowShelf, 120.0f, 0.7f, 1.5f }, { B::Bell, 250.0f, 1.2f, -3.0f }, { B::Bell, 4000.0f, 1.0f, 2.5f },
                        { B::HighShelf, 10000.0f, 0.7f, 1.5f } }}, 4,
              -20.0f, 170.0f,  -4.0f, 3.0f, 6.0f, 10.0f, 150.0f,  false, 0.0f, 1.0f, 1.0f, 50.0f,
              3.0f, 0.3f, 0.2f,  -18.0f, 3.0f, 3.0f },
            // radio: HPF 75 + lift 100-300 Hz like a broadcast mic (RE20), -3 dB boxiness at 450, broad +3 at 2.8k,
            // +2 air; 4:1 fast attack / slow release, then a fast 8:1 peak stage; denser saturation, louder target
            { 75.0f, {{ { B::LowShelf, 160.0f, 0.7f, 4.0f }, { B::Bell, 450.0f, 1.4f, -3.0f }, { B::Bell, 2800.0f, 0.7f, 3.0f },
                        { B::HighShelf, 9000.0f, 0.7f, 2.0f } }}, 4,
              -20.0f, 150.0f,  -8.0f, 4.0f, 4.0f, 5.0f, 250.0f,  true, 4.0f, 8.0f, 1.0f, 60.0f,
              6.0f, 0.5f, 0.35f,  -15.0f, 2.0f, 10.0f },
            // stream: HPF 100 (keyboard / desk thumps), -3 dB at 300, broad +3 presence at 3k, +1.5 at 8k; 4:1, 5 / 120 ms
            { 100.0f, {{ { B::Bell, 300.0f, 1.0f, -3.0f }, { B::Bell, 3000.0f, 0.8f, 3.0f }, { B::HighShelf, 8000.0f, 0.7f, 1.5f }, {} }}, 3,
              -26.0f, 150.0f,  -5.0f, 4.0f, 6.0f, 5.0f, 120.0f,  false, 0.0f, 1.0f, 1.0f, 50.0f,
              3.0f, 0.2f, 0.2f,  -18.0f, 2.0f, 3.0f },
            // singing: HPF 80, -2 dB at 300, +1.5 at 3.5k, +3 air shelf at 12k; 2.5:1 slow attack (consonants pass),
            // 200 ms release, a fast 4:1 stage only for peaks; light gate, slow leveling (keeps the dynamics)
            { 80.0f, {{ { B::Bell, 300.0f, 1.0f, -2.0f }, { B::Bell, 3500.0f, 1.0f, 1.5f }, { B::HighShelf, 12000.0f, 0.7f, 3.0f }, {} }}, 3,
              -10.0f, 300.0f,  -3.0f, 2.5f, 10.0f, 20.0f, 200.0f,  true, 6.0f, 4.0f, 2.0f, 80.0f,
              4.0f, 0.4f, 0.3f,  -18.0f, 5.0f, 3.0f },
        };
        return r[juce::jlimit (0, numProfiles - 1, profile)];
    }

    /** The profile's character as a curve (its bands without the high pass), on the match grid. */
    static match::Curve character (int profile, double sr = 48000.0)
    {
        const auto& rc = recipe (profile);
        match::Curve c {};
        for (int i = 0; i < match::kBands; ++i)
            for (int k = 0; k < rc.numBands; ++k)
            {
                const auto& b = rc.bands[(size_t) k];
                c[(size_t) i] += (float) ParamEq::bandDb (b.type, sr, b.freq, b.q, b.gainDb, match::bandFreq (i));
            }
        return c;
    }

    /** Bands of a learned match (full strength); n = 0: none. */
    struct Match
    {
        std::array<VoiceBand, VoiceDesign::kMaxMatch> bands {};
        int n = 0;
    };

    /** The chain at amount t (0..1) for a measured noise floor and leveling gain (dB, at full amount). match: the
        learned EQ, which replaces the profile's fixed bands (nullptr / empty: the fixed bands). */
    static VoiceDesign design (int profile, float t, float noiseFloorDb, float levelGainDb, const Match* match = nullptr) noexcept
    {
        const auto& rc = recipe (profile);
        t = juce::jlimit (0.0f, 1.0f, t);
        VoiceDesign d;
        d.bands[0] = { ParamEq::LowCut, 40.0f * std::pow (rc.hpfHz / 40.0f, t), 0.707f, 0.0f };
        d.numBands = 1;
        if (match != nullptr && match->n > 0)
            for (int k = 0; k < match->n && d.numBands < VoiceDesign::kBands; ++k) { auto b = match->bands[(size_t) k]; b.gainDb *= t; d.bands[(size_t) d.numBands++] = b; }
        else
            for (int k = 0; k < rc.numBands; ++k) { auto b = rc.bands[(size_t) k]; b.gainDb *= t; d.bands[(size_t) d.numBands++] = b; }
        d.gate.thresholdDb = juce::jlimit (-70.0f, -38.0f, noiseFloorDb + 15.0f);
        d.gate.rangeDb = rc.gateRangeDb * t;
        d.gate.attackMs = 2.0f;
        d.gate.holdMs = 150.0f;
        d.gate.releaseMs = rc.gateReleaseMs;
        d.comp.inputDb = levelGainDb * t;
        d.comp.thresholdDb = rc.targetDb + rc.compThrRel;
        d.comp.ratio = 1.0f + (rc.compRatio - 1.0f) * t;
        d.comp.kneeDb = rc.compKneeDb;
        d.comp.attackMs = rc.compAttackMs;
        d.comp.releaseMs = rc.compReleaseMs;
        d.comp.makeupDb = -d.comp.staticGainDb (rc.targetDb + rc.makeupAtDb);   // speech comes out about as loud as the target
        d.comp.autoMakeup = false;
        d.comp2On = rc.comp2On;
        d.comp2.inputDb = 0.0f;
        d.comp2.thresholdDb = rc.targetDb + rc.comp2ThrRel;
        d.comp2.ratio = 1.0f + (rc.comp2Ratio - 1.0f) * t;
        d.comp2.kneeDb = 3.0f;
        d.comp2.attackMs = rc.comp2AttackMs;
        d.comp2.releaseMs = rc.comp2ReleaseMs;
        d.comp2.makeupDb = 0.0f;
        d.comp2.autoMakeup = false;
        d.sat.driveDb = rc.driveDb * t;
        d.sat.warmth = rc.warmth;
        d.sat.mix = rc.satMix;
        d.sat.outputDb = 0.0f;
        d.ceilingDb = -1.0f;
        return d;
    }

    /** Message thread: the learned EQ to run (n = 0: the profile's fixed bands). Double-buffered: the audio thread
        reads the side published last. */
    void setMatch (const Match& m) noexcept
    {
        const int next = 1 - activeMatch.load (std::memory_order_acquire);
        matches[(size_t) next] = m;
        activeMatch.store (next, std::memory_order_release);
    }
    Match getMatch() const noexcept { return matches[(size_t) activeMatch.load (std::memory_order_acquire)]; }

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        reset();
    }

    /** Back to the start (also forgets the measured noise floor and level). */
    void reset() noexcept
    {
        eq.reset();
        gate.prepare (sr);
        comp.prepare (sr);
        comp2.prepare (sr);
        sat.prepare (sr);
        limiter.prepare (sr);
        noiseDb = -70.0f;
        levelPow = -1.0f;
        levelGain.store (0.0f, std::memory_order_relaxed);
        compGr.store (0.0f, std::memory_order_relaxed);
        gateOpen.store (1.0f, std::memory_order_relaxed);
    }

    /** Audio thread. amount 0..1; at 0 nothing happens (and the state restarts when it comes back). */
    void process (float* l, float* r, int n, int profile, float amount) noexcept
    {
        if (amount <= 0.0005f) { if (! idle) { reset(); idle = true; } return; }
        idle = false;
        if (profile != lastProfile) { lastProfile = profile; levelPow = -1.0f; }

        const auto& m = matches[(size_t) activeMatch.load (std::memory_order_acquire)];
        const auto d0 = design (profile, amount, noiseDb, levelGainDb(), &m);
        eq.process (d0.bands, d0.numBands, sr, l, r, n);
        // Measure after the EQ: noise floor (pauses) and speech level, once per block.
        measure (l, r, n, profile);
        const auto d = design (profile, amount, noiseDb, levelGainDb(), &m);

        gate.process (l, r, n, d.gate);
        comp.process (l, r, n, d.comp);
        float gr = comp.lastGr;
        if (d.comp2On) { comp2.process (l, r, n, d.comp2); gr += comp2.lastGr; }
        sat.process (l, r, n, d.sat);
        limiter.process (l, r, n, d.ceilingDb);

        compGr.store (gr, std::memory_order_relaxed);
        gateOpen.store (gate.shownGain.load (std::memory_order_relaxed), std::memory_order_relaxed);
        levelGain.store (d.comp.inputDb, std::memory_order_relaxed);
    }

    /** The settings right now (message thread: what "Split into effects" writes). */
    VoiceDesign current (int profile, float amount) const noexcept
    {
        const auto m = getMatch();
        return design (profile, amount, noiseFloorDb.load (std::memory_order_relaxed), fullLevelGain.load (std::memory_order_relaxed), &m);
    }

    // For the UI (any thread)
    std::atomic<float> levelGain { 0.0f };     // dB the leveling adds now (at the current amount)
    std::atomic<float> compGr { 0.0f };        // compression of both stages, dB
    std::atomic<float> gateOpen { 1.0f };      // gate gain 0..1
    std::atomic<float> noiseFloorDb { -70.0f };
    std::atomic<float> fullLevelGain { 0.0f }; // leveling gain at full amount

private:
    float levelGainDb() const noexcept
    {
        if (levelPow <= 0.0f) return 0.0f;
        const auto& rc = recipe (lastProfile);
        return juce::jlimit (-12.0f, 18.0f, rc.targetDb - 10.0f * std::log10 (levelPow + 1.0e-12f));
    }

    void measure (const float* l, const float* r, int n, int profile) noexcept
    {
        if (n <= 0) return;
        double sum = 0.0;
        for (int i = 0; i < n; ++i) sum += (double) l[i] * l[i] + (double) r[i] * r[i];
        const float ms = (float) (sum / (2.0 * n));
        const float db = 10.0f * std::log10 (ms + 1.0e-12f);
        const float secs = (float) n / (float) sr;
        // Noise floor: falls quickly to quiet blocks, rises only slowly (someone talking for long keeps it down).
        if (db < noiseDb) noiseDb += (db - noiseDb) * (1.0f - std::exp (-secs / 0.3f));
        else              noiseDb += juce::jmin (db - noiseDb, 0.5f * secs);
        noiseDb = juce::jlimit (-90.0f, -30.0f, noiseDb);
        // Speech level: only blocks clearly above the noise.
        if (db > noiseDb + 10.0f && db > -65.0f)
        {
            if (levelPow <= 0.0f) levelPow = ms;
            else levelPow += (ms - levelPow) * (1.0f - std::exp (-secs / recipe (profile).levelSeconds));
        }
        noiseFloorDb.store (noiseDb, std::memory_order_relaxed);
        fullLevelGain.store (levelGainDb(), std::memory_order_relaxed);
    }

    double sr = 48000.0;
    VoiceEq<VoiceDesign::kBands> eq;
    std::array<Match, 2> matches {};
    std::atomic<int> activeMatch { 0 };
    Gate gate;
    Compressor comp, comp2;
    Saturator sat;
    Limiter limiter;
    float noiseDb = -70.0f;
    float levelPow = -1.0f;   // mean power of speech (< 0: nothing heard yet)
    int lastProfile = 0;
    bool idle = true;
};
} // namespace hydra::dsp

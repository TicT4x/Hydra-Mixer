#pragma once
#include "Params.h"
#include "Dsp.h"

namespace hydra
{
namespace dsp { class HiResFir; }

/** 16-band parametric EQ bound to parameters; used by every input strip and every bus.
    Each band has its own on/off and filter type (bell, notch, band pass, cuts, shelves). A band only costs
    CPU while it is in use; bands 1-8 keep the defaults they had when the EQ had eight bands (sessions store
    only values that differ from the default, so changing them would change old sessions). */
struct ParamEq
{
    static constexpr int kBands = 16;
    static constexpr int kV2Bands = 8;   // sessions before 2026-11-09 had eight bands
    static constexpr int kV1Bands = 6;   // sessions before 2026-10-15 had six bands
    // Same order (and indices) as Voicemeeter's EQ cell types.
    enum BandType { Bell = 0, Notch, BandPass, HighCut, LowCut, LowShelf, HighShelf, NumTypes };

    static juce::StringArray typeNames()
    {
        return { "Bell", "Notch", "Band Pass", "High Cut", "Low Cut", "Low Shelf", "High Shelf" };
    }

    static dsp::BiquadCoeffs::Type toDsp (int t) noexcept
    {
        switch (t)
        {
            case Notch:     return dsp::BiquadCoeffs::Type::Notch;
            case BandPass:  return dsp::BiquadCoeffs::Type::BandPass;
            case LowCut:    return dsp::BiquadCoeffs::Type::LowCut;
            case HighCut:   return dsp::BiquadCoeffs::Type::HighCut;
            case LowShelf:  return dsp::BiquadCoeffs::Type::LowShelf;
            case HighShelf: return dsp::BiquadCoeffs::Type::HighShelf;
            default:        return dsp::BiquadCoeffs::Type::Peak;
        }
    }

    /** Frequency a band starts at (its default). */
    static float defaultFreq (int k) noexcept
    {
        static const float f[kBands] = { 80, 200, 500, 1000, 2500, 5000, 8000, 12000,
                                         40, 120, 320, 700, 1600, 3500, 6500, 16000 };
        return f[juce::jlimit (0, kBands - 1, k)];
    }

    /** Low cut / high cut are 24 dB per octave (13.11., user: 12 dB was not steep enough): two biquads, at Q 0.707 a
        4th-order Butterworth. Q shapes the knee: the second section's Q follows sqrt (q / 0.707), so Q 1 gives a small
        bump at the cutoff like before, Q 0.5 a softer knee. */
    static bool isCut (int t) noexcept { return t == LowCut || t == HighCut; }

    /** The biquad(s) of a band: returns 2 for the cuts (a then b), otherwise 1 (only a). */
    static int sections (int t, double sr, double freq, double q, double gainDb, dsp::BiquadCoeffs& a, dsp::BiquadCoeffs& b)
    {
        if (! isCut (t)) { a = dsp::BiquadCoeffs::make (toDsp (t), sr, freq, q, gainDb); return 1; }
        a = dsp::BiquadCoeffs::make (toDsp (t), sr, freq, 0.54119610, 0.0);
        b = dsp::BiquadCoeffs::make (toDsp (t), sr, freq, 1.30656296 * std::sqrt (juce::jmax (0.05, q) / 0.70710678), 0.0);
        return 2;
    }

    /** Response of a band in dB (curves). */
    static double bandDb (int t, double sr, double freq, double q, double gainDb, double f)
    {
        dsp::BiquadCoeffs a, b;
        const int n = sections (t, sr, freq, q, gainDb, a, b);
        return a.magnitudeDb (f, sr) + (n > 1 ? b.magnitudeDb (f, sr) : 0.0);
    }

    /** Types whose gain knob has no effect. */
    static bool typeUsesGain (int t) noexcept { return t == Bell || t == LowShelf || t == HighShelf; }

    ParamEq() = default;
    ~ParamEq();   // ParamEq.cpp (frees the match filter)
    ParamEq (const ParamEq&) = delete;

    Param* on {};
    // bandOn = band in use (has a point); enabled = switched on (off: kept with its settings but bypassed)
    std::array<Param*, kBands> bandOn {}, enabled {}, type {}, gain {}, freq {}, q {};
    /** Stereo processing (14.11.): the EQ's channel mode, and per band which side it works on in that mode. In
        Stereo mode every band works on both channels (as before). Delta: only what the EQ changes is heard. */
    enum Mode { stereo = 0, leftRight, midSide };
    enum Side { both = 0, first, second };   // first = left / mid, second = right / side
    Param* mode {};
    Param* delta {};
    std::array<Param*, kBands> chan {};
    int modeIndex() const noexcept { return juce::jlimit (0, 2, juce::roundToInt (mode->get())); }
    /** The side band k works on now (both in Stereo mode). */
    int bandSide (int k) const noexcept { return modeIndex() == stereo ? both : juce::jlimit (0, 2, juce::roundToInt (chan[(size_t) k]->get())); }
    /** Does band k work on side s (0 = left / mid, 1 = right / side, 2 = either; in Stereo mode always)? */
    bool bandOnSide (int k, int s) const noexcept { const int c = bandSide (k); return s >= 2 || c == both || c == s + 1; }

    /** part / label: "eq" / "EQ" for the first EQ of a channel, "eq2" / "EQ 2" for further instances. */
    void create (ParamRegistry& reg, const juce::String& idPrefix, const juce::String& namePrefix,
                 const juce::String& part = "eq", const juce::String& label = "EQ")
    {
        static const int defType[kBands] = { LowShelf, Bell, Bell, Bell, Bell, Bell, Bell, HighShelf,
                                             Bell, Bell, Bell, Bell, Bell, Bell, Bell, Bell };

        juce::NormalisableRange<float> freqRange (20.0f, 20000.0f,
            [] (float s, float e, float n) { return s * std::pow (e / s, n); },
            [] (float s, float e, float v) { return std::log (v / s) / std::log (e / s); },
            [] (float, float, float v) { return v; });
        juce::NormalisableRange<float> qRange (0.1f, 100.0f,
            [] (float s, float e, float n) { return s * std::pow (e / s, n); },
            [] (float s, float e, float v) { return std::log (v / s) / std::log (e / s); },
            [] (float, float, float v) { return v; });

        on = &reg.addToggle (idPrefix + part + ".on", namePrefix + label + " On");
        for (int k = 0; k < kBands; ++k)
        {
            const auto bp = idPrefix + part + ".b" + juce::String (k + 1) + ".";
            const auto bn = namePrefix + label + " Band " + juce::String (k + 1) + " ";
            bandOn[(size_t) k] = &reg.addToggle (bp + "on", bn + "On", false); // off = band not in use (no point shown)
            enabled[(size_t) k] = &reg.addToggle (bp + "enabled", bn + "Enabled", true);
            type[(size_t) k]   = &reg.addChoice (bp + "type", bn + "Type", typeNames(), defType[k]);
            gain[(size_t) k]   = &reg.add (bp + "gain", bn + "Gain", ParamType::Continuous, { -18.0f, 18.0f, 0.1f }, 0.0f, " dB");
            freq[(size_t) k]   = &reg.add (bp + "freq", bn + "Freq", ParamType::Continuous, freqRange, defaultFreq (k), " Hz");
            q[(size_t) k]      = &reg.add (bp + "q", bn + "Q", ParamType::Continuous, qRange, (k == 0 || k == kV2Bands - 1) ? 0.7f : 1.0f);
            chan[(size_t) k]   = &reg.addChoice (bp + "chan", bn + "Channel", { "Both", "Left / Mid", "Right / Side" }, both);
        }
        mode  = &reg.addChoice (idPrefix + part + ".mode", namePrefix + label + " Channel Mode", { "Stereo", "Left / Right", "Mid / Side" }, stereo);
        delta = &reg.addToggle (idPrefix + part + ".delta", namePrefix + label + " Delta");
        delta->persistent = false;   // listening to the difference is never what a session should start with
        const auto mp = idPrefix + part + ".match.";
        const auto mn = namePrefix + label + " Match ";
        matchOn     = &reg.addToggle (mp + "on", mn + "On", true);
        matchSmooth = &reg.add (mp + "smooth", mn + "Smoothing", ParamType::Continuous, { 0.0f, 100.0f, 1.0f }, 50.0f, " %");
        matchAmount = &reg.add (mp + "amount", mn + "Amount", ParamType::Continuous, { 0.0f, 100.0f, 1.0f }, 100.0f, " %");
        matchLow    = &reg.add (mp + "lo", mn + "Low", ParamType::Continuous, freqRange, 20.0f, " Hz");
        matchHigh   = &reg.add (mp + "hi", mn + "High", ParamType::Continuous, freqRange, 20000.0f, " Hz");
        matchSplit  = &reg.addToggle (mp + "split", mn + "Separate Sides");
    }

    //--------------------------------------------------------------------------
    /** Match (EQ window, 14.11.): a high-resolution curve that moves this channel's spectrum (source, captured before
        the EQ) toward a reference, smoothed / scaled by SMOOTH / AMOUNT within LOW..HIGH. It runs as a minimum-phase FIR
        after the bands (dsp::HiResFir, no latency) until it is turned into bands. The captured spectra are 1/48-octave
        curves (match::Fine); they are saved with the session (Engine::eqMatchToXml), the settings are parameters. */
    Param *matchOn {}, *matchSmooth {}, *matchAmount {}, *matchLow {}, *matchHigh {};
    /** Off (default): one match from the mono sum on both channels, whatever the channel mode. On: left / right or
        mid / side separately (the EQ's channel mode; in Stereo mode it stays one). */
    Param* matchSplit {};
    int matchModeIndex() const noexcept { return matchSplit->isOn() ? modeIndex() : stereo; }
    struct MatchData
    {
        std::vector<float> source, reference;   // normalised dB per match::fineFreq point (mono sum); empty = not captured
        // The single channels of the same captures (left, right, side), normalised like the mono sum, so their level
        // relative to it stays (a wider reference has a louder side). Empty for built-in references / old sessions.
        std::vector<float> sourceL, sourceR, sourceS, referenceL, referenceR, referenceS;
        juce::String sourceName, referenceName; // where they came from ("IN 2  Mic", "song", "Standard speech")
        bool converted = false;                 // turned into bands (the window shows the bands again)
        uint32_t version = 1;                   // bumped on every change
        bool ready() const noexcept { return ! source.empty() && ! reference.empty(); }
        bool empty() const noexcept { return source.empty() && reference.empty(); }
    };
    MatchData matchData;   // message thread
    /** The curve the match applies on side s (0 = both / left / mid, 1 = right / side, by the channel mode; dB per
        fine point); nullptr while source or reference is missing. Message thread. */
    const std::vector<float>* matchCurve (int side = 0);
    /** The captured spectrum shown for side s in the current mode (left / right, mid = mono sum / side). Can be empty
        (no side captured, e.g. a built-in reference). */
    const std::vector<float>& matchSpectrum (bool reference, int s) const noexcept
    {
        const auto& d = matchData;
        const auto& mono = reference ? d.reference : d.source;
        if (s >= 2) return mono;   // both sides at once: the mono sum
        switch (matchModeIndex())
        {
            case leftRight:
            {
                const auto& c = reference ? (s == 0 ? d.referenceL : d.referenceR) : (s == 0 ? d.sourceL : d.sourceR);
                return c.empty() ? mono : c;
            }
            case midSide: return s == 0 ? mono : (reference ? d.referenceS : d.sourceS);
            default:      return mono;
        }
    }
    bool matchActive() const noexcept { return on != nullptr && matchOn->isOn() && matchData.ready(); }
    /** The EQ works as a match (until it is turned into bands): its window opens with the match, its block shows
        SMOOTH / AMOUNT and the tag "MATCH". */
    bool matchMode() const noexcept { return on != nullptr && matchData.ready() && (matchOn->isOn() || ! matchData.converted); }
    uint64_t matchKey() const noexcept
    {
        return (((((uint64_t) matchData.version * 1000003ull + matchSmooth->getVersion()) * 1000003ull + matchAmount->getVersion()) * 1000003ull
                + matchLow->getVersion()) * 1000003ull + matchHigh->getVersion()) * 1000003ull + mode->getVersion() * 7ull + matchSplit->getVersion();
    }
    /** The filter of the match (created by Engine::updateEqMatches the first time it is needed). */
    std::atomic<dsp::HiResFir*> fir { nullptr };
    uint64_t firKey = 0;   // what the current filter was designed for (Engine)

    /** Does band k currently change the signal? */
    bool bandActive (int k) const noexcept
    {
        if (! bandOn[(size_t) k]->isOn() || ! enabled[(size_t) k]->isOn()) return false;
        const int t = (int) type[(size_t) k]->get();
        return ! typeUsesGain (t) || gain[(size_t) k]->get() != 0.0f;
    }

    /** Switches off bands that are on but change nothing (bell / shelf at 0 dB), so a flat EQ shows no points. */
    void hideNeutralBands()
    {
        for (int k = 0; k < kBands; ++k)
            if (bandOn[(size_t) k]->isOn() && typeUsesGain ((int) type[(size_t) k]->get()) && std::abs (gain[(size_t) k]->get()) < 0.05f)
                bandOn[(size_t) k]->set (0.0f);
    }

    /** Response of band k in dB at f (both sections of a cut). */
    double bandDb (int k, double f, double sr) const
    {
        return bandDb ((int) type[(size_t) k]->get(), sr, freq[(size_t) k]->get(), q[(size_t) k]->get(), gain[(size_t) k]->get(), f);
    }

    // ---- audio thread ----
    void prepare() noexcept
    {
        resetFir();
        for (auto& f : filters) f.reset();
        for (auto& f : filters2) f.reset();
        for (auto& c : cache) { c[0] = -1.0f; c[4] = -1.0f; }
    }

    void process (float* l, float* r, int n, double sr) noexcept
    {
        if (! on->isOn()) return;
        if (! delta->isOn()) { processCore (l, r, n, sr); return; }
        // Delta: output minus input (what the EQ adds or takes away), in chunks so nothing is allocated.
        constexpr int chunk = 256;
        float dl[chunk], dr[chunk];
        for (int i = 0; i < n; i += chunk)
        {
            const int m = juce::jmin (chunk, n - i);
            std::copy (l + i, l + i + m, dl);
            std::copy (r + i, r + i + m, dr);
            processCore (l + i, r + i, m, sr);
            for (int j = 0; j < m; ++j) { l[i + j] -= dl[j]; r[i + j] -= dr[j]; }
        }
    }

    void processCore (float* l, float* r, int n, double sr) noexcept
    {
        const int md = modeIndex();
        bool anyMidSide = false;
        auto run = [&] (int k, int side, float* x)
        {
            filters[k].processOne (side, x, n);
            if (numSections[k] > 1) filters2[k].processOne (side, x, n);
        };
        for (int k = 0; k < kBands; ++k)
        {
            if (! bandActive (k)) continue;
            const float t = type[(size_t) k]->get(), g = gain[(size_t) k]->get(),
                        f = freq[(size_t) k]->get(), qq = q[(size_t) k]->get();
            const int side = bandSide (k);
            const float where = (float) (side * 3 + md);
            auto& c = cache[k];
            if (c[4] != where)   // another channel: the filter starts from silence
            {
                filters[k].reset();
                filters2[k].reset();
                c[4] = where;
            }
            if (c[0] != t || c[1] != g || c[2] != f || c[3] != qq)
            {
                const int was = numSections[k];
                numSections[k] = sections ((int) t, sr, f, qq, g, filters[k].c, filters2[k].c);
                if (numSections[k] > 1 && was < 2) filters2[k].reset();
                c[0] = t; c[1] = g; c[2] = f; c[3] = qq;
            }
            if (side == both)
            {
                filters[k].process (l, r, n);
                if (numSections[k] > 1) filters2[k].process (l, r, n);
            }
            else if (md == leftRight) run (k, side - 1, side == first ? l : r);
            else anyMidSide = true;
        }
        if (anyMidSide)
        {
            for (int i = 0; i < n; ++i) { const float m = 0.5f * (l[i] + r[i]), d = 0.5f * (l[i] - r[i]); l[i] = m; r[i] = d; }
            for (int k = 0; k < kBands; ++k)
                if (const int side = bandSide (k); side != both && bandActive (k)) run (k, side - 1, side == first ? l : r);
            for (int i = 0; i < n; ++i) { const float m = l[i], d = r[i]; l[i] = m + d; r[i] = m - d; }
        }
        if (fir.load (std::memory_order_acquire) != nullptr) processFir (l, r, n);
    }

    uint64_t versionSum() const noexcept
    {
        uint64_t s = on->getVersion();
        for (int k = 0; k < kBands; ++k)
            s += bandOn[(size_t) k]->getVersion() + enabled[(size_t) k]->getVersion() + type[(size_t) k]->getVersion() + gain[(size_t) k]->getVersion()
               + freq[(size_t) k]->getVersion() + q[(size_t) k]->getVersion() + chan[(size_t) k]->getVersion();
        return s + matchKey() + matchOn->getVersion() + mode->getVersion();
    }

private:
    void processFir (float* l, float* r, int n) noexcept;
    void resetFir() noexcept;
    std::vector<float> curveCache[2];
    uint64_t curveKey = ~0ull;
    dsp::StereoBiquad filters[kBands], filters2[kBands];   // filters2: second section of a cut
    int numSections[kBands] {};
    float cache[kBands][5] {};
};
} // namespace hydra

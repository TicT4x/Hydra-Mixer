#pragma once
#include "Params.h"
#include "Dsp.h"

namespace hydra
{
/** 8-band parametric EQ bound to parameters; used by every input strip and every bus.
    Each band has its own on/off and filter type (bell, notch, band pass, cuts, shelves). */
struct ParamEq
{
    static constexpr int kBands = 8;
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
        static const float f[kBands] = { 80, 200, 500, 1000, 2500, 5000, 8000, 12000 };
        return f[juce::jlimit (0, kBands - 1, k)];
    }

    /** Types whose gain knob has no effect. */
    static bool typeUsesGain (int t) noexcept { return t == Bell || t == LowShelf || t == HighShelf; }

    Param* on {};
    std::array<Param*, kBands> bandOn {}, type {}, gain {}, freq {}, q {};

    void create (ParamRegistry& reg, const juce::String& idPrefix, const juce::String& namePrefix)
    {
        static const int defType[kBands] = { LowShelf, Bell, Bell, Bell, Bell, Bell, Bell, HighShelf };

        juce::NormalisableRange<float> freqRange (20.0f, 20000.0f,
            [] (float s, float e, float n) { return s * std::pow (e / s, n); },
            [] (float s, float e, float v) { return std::log (v / s) / std::log (e / s); },
            [] (float, float, float v) { return v; });
        juce::NormalisableRange<float> qRange (0.1f, 100.0f,
            [] (float s, float e, float n) { return s * std::pow (e / s, n); },
            [] (float s, float e, float v) { return std::log (v / s) / std::log (e / s); },
            [] (float, float, float v) { return v; });

        on = &reg.addToggle (idPrefix + "eq.on", namePrefix + "EQ On");
        for (int k = 0; k < kBands; ++k)
        {
            const auto bp = idPrefix + "eq.b" + juce::String (k + 1) + ".";
            const auto bn = namePrefix + "EQ Band " + juce::String (k + 1) + " ";
            bandOn[(size_t) k] = &reg.addToggle (bp + "on", bn + "On", false); // off = band not in use (no point shown)
            type[(size_t) k]   = &reg.addChoice (bp + "type", bn + "Type", typeNames(), defType[k]);
            gain[(size_t) k]   = &reg.add (bp + "gain", bn + "Gain", ParamType::Continuous, { -18.0f, 18.0f, 0.1f }, 0.0f, " dB");
            freq[(size_t) k]   = &reg.add (bp + "freq", bn + "Freq", ParamType::Continuous, freqRange, defaultFreq (k), " Hz");
            q[(size_t) k]      = &reg.add (bp + "q", bn + "Q", ParamType::Continuous, qRange, (k == 0 || k == kBands - 1) ? 0.7f : 1.0f);
        }
    }

    /** Does band k currently change the signal? */
    bool bandActive (int k) const noexcept
    {
        if (! bandOn[(size_t) k]->isOn()) return false;
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

    dsp::BiquadCoeffs coeffs (int k, double sr) const
    {
        return dsp::BiquadCoeffs::make (toDsp ((int) type[(size_t) k]->get()), sr,
                                        freq[(size_t) k]->get(), q[(size_t) k]->get(), gain[(size_t) k]->get());
    }

    // ---- audio thread ----
    void prepare() noexcept
    {
        for (auto& f : filters) f.reset();
        for (auto& c : cache) c[0] = -1.0f;
    }

    void process (float* l, float* r, int n, double sr) noexcept
    {
        if (! on->isOn()) return;
        for (int k = 0; k < kBands; ++k)
        {
            if (! bandActive (k)) continue;
            const float t = type[(size_t) k]->get(), g = gain[(size_t) k]->get(),
                        f = freq[(size_t) k]->get(), qq = q[(size_t) k]->get();
            auto& c = cache[k];
            if (c[0] != t || c[1] != g || c[2] != f || c[3] != qq)
            {
                filters[k].c = coeffs (k, sr);
                c[0] = t; c[1] = g; c[2] = f; c[3] = qq;
            }
            filters[k].process (l, r, n);
        }
    }

    uint64_t versionSum() const noexcept
    {
        uint64_t s = on->getVersion();
        for (int k = 0; k < kBands; ++k)
            s += bandOn[(size_t) k]->getVersion() + type[(size_t) k]->getVersion() + gain[(size_t) k]->getVersion()
               + freq[(size_t) k]->getVersion() + q[(size_t) k]->getVersion();
        return s;
    }

private:
    dsp::StereoBiquad filters[kBands];
    float cache[kBands][4] {};
};
} // namespace hydra

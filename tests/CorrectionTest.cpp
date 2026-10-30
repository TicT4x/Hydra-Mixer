// Correction stage (core/Correction): profiles (Equalizer APO / AutoEq files, XML), the filter itself, and how
// the engine keeps a channel's profile (session, reorder, remove).
#include "state/Session.h"
#include <cstdio>

namespace
{
/** Level (dB) of a sine at f after the correction, measured after it settled. */
double levelThrough (hydra::Correction& c, double f, double sr)
{
    c.reset();
    constexpr int n = 256;
    std::vector<float> l (n), r (n);
    double phase = 0.0, peak = 0.0;
    for (int block = 0; block < 400; ++block)
    {
        for (int i = 0; i < n; ++i) { l[(size_t) i] = r[(size_t) i] = (float) std::sin (phase); phase += 2.0 * juce::MathConstants<double>::pi * f / sr; }
        c.process (l.data(), r.data(), n, sr);
        if (block >= 300)
            for (float v : l) peak = std::max (peak, (double) std::abs (v));
    }
    return 20.0 * std::log10 (peak);
}
} // namespace

int runCorrectionTest()
{
    using namespace hydra;
    juce::ScopedJuceInitialiser_GUI juceInit;
    int fails = 0;
    auto check = [&] (bool c, const char* what) { std::printf ("%s %s\n", c ? "ok:  " : "FAIL:", what); if (! c) ++fails; };

    // AutoEq "ParametricEQ.txt" style file.
    const juce::String apo = "Preamp: -6.2 dB\n"
                             "Filter 1: ON LSC Fc 105 Hz Gain 5.5 dB Q 0.70\n"
                             "Filter 2: ON PK Fc 2100 Hz Gain -3.4 dB Q 1.41\n"
                             "Filter 3: OFF PK Fc 3000 Hz Gain 9 dB Q 1\n"
                             "Filter 4: ON HSC Fc 10000 Hz Gain -2.0 dB Q 0.70\n"
                             "Filter 5: ON XYZ Fc 100 Hz\n";
    CorrectionProfile p;
    juce::String err;
    check (CorrectionProfile::fromApo (apo, p, err) && p.bands.size() == 3 && std::abs (p.preampDb + 6.2f) < 1.0e-4f,
           "APO / AutoEq file: preamp and the three ON filters read");
    check (p.bands[0].type == ParamEq::LowShelf && p.bands[1].type == ParamEq::Bell && p.bands[2].type == ParamEq::HighShelf
           && std::abs (p.bands[1].freq - 2100.0f) < 0.01f && std::abs (p.bands[1].gainDb + 3.4f) < 1.0e-4f && std::abs (p.bands[1].q - 1.41f) < 1.0e-4f,
           "filter types, frequency, gain and Q");
    check (err.contains ("1 filter"), "unknown filter type reported");
    {
        CorrectionProfile again;
        juce::String e2;
        check (CorrectionProfile::fromApo (p.toApo(), again, e2) && again.bands == p.bands && again.preampDb == p.preampDb,
               "export to APO and back gives the same profile");
        const auto x = p.toXml();
        const auto back = CorrectionProfile::fromXml (x.get());
        check (back.bands == p.bands && back.preampDb == p.preampDb && back.kind == p.kind, "XML round trip");
        CorrectionProfile none;
        check (! CorrectionProfile::fromApo ("just text", none, e2), "a file without filters is refused");
    }

    {   // The filter does what the curve says: bell -6 dB at 100 Hz, preamp -2 dB.
        Engine engine;
        CorrectionProfile q;
        q.preampDb = -2.0f;
        q.bands.push_back ({ ParamEq::Bell, 100.0f, -6.0f, 4.0f });
        engine.setCorrection (0, q);
        auto& c = engine.inputs[0].correction;
        const double sr = engine.getSampleRate();
        c.on->set (0.0f);
        check (std::abs (levelThrough (c, 100.0, sr)) < 0.05, "off: the signal passes unchanged");
        c.on->set (1.0f);
        const double at100 = levelThrough (c, 100.0, sr), at1k = levelThrough (c, 1000.0, sr);
        std::printf ("      100 Hz %.2f dB (curve %.2f), 1 kHz %.2f dB (curve %.2f)\n", at100, q.magnitudeDb (100.0, sr), at1k, q.magnitudeDb (1000.0, sr));
        check (std::abs (at100 - q.magnitudeDb (100.0, sr)) < 0.1 && std::abs (at1k - q.magnitudeDb (1000.0, sr)) < 0.1,
               "measured response matches the profile's curve (bands + preamp)");
        check (std::abs (levelThrough (c, 100.0, sr * 2.0)) < 0.05, "built for another sample rate: bypassed until rebuilt");
    }

    {   // The engine keeps the profile with its channel: session, reorder, remove.
        Engine engine;
        MidiMapper midi (engine.params);
        Session session (engine, midi);
        CorrectionProfile q = p;
        q.name = "HD 600";
        q.kind = CorrectionProfile::Kind::headphone;
        engine.setCorrection (kMaxInputs + 1, q);   // A2
        engine.buses[1].correction.on->set (1.0f);
        auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("hydra_correction_test.xml");
        session.save (f);
        engine.setCorrection (kMaxInputs + 1, {});
        session.load (f);
        f.deleteFile();
        const auto& back = engine.buses[1].correction.getProfile();
        check (back.name == "HD 600" && back.kind == CorrectionProfile::Kind::headphone && back.bands == q.bands
               && engine.buses[1].correction.on->isOn(), "profile and its switch survive save / load");

        // A2 and A1 swap places: the profile moves with A2.
        check (engine.reorderChannels (Engine::Group::hwBuses, { 1, 0, 2, 3, 4 }) && engine.buses[0].correction.getProfile().name == "HD 600"
               && engine.buses[1].correction.getProfile().isEmpty(), "reordering moves the profile with its bus");
        engine.removeBus (0);
        check (engine.buses[0].correction.getProfile().isEmpty() && ! engine.buses[0].correction.on->isOn(), "removing the bus clears its correction");
    }
    return fails;
}

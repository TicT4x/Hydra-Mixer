// Sync Delay: the delay line (core/Dsp.h) and the per-channel parameters (Engine).
#include "engine/Engine.h"
#include <cstdio>

int runSyncDelayTest()
{
    int fails = 0;
    auto check = [&] (bool c, const char* what) { std::printf ("%s %s\n", c ? "ok:  " : "FAIL:", what); if (! c) ++fails; };
    using hydra::dsp::DelayLine;

    constexpr double sr = 48000.0;
    constexpr int n = 256;

    {   // Delay 0: the signal passes untouched (and the line stays idle).
        DelayLine d;
        d.prepare (sr);
        std::vector<float> l (n), r (n);
        for (int i = 0; i < n; ++i) { l[(size_t) i] = (float) i; r[(size_t) i] = (float) -i; }
        auto l0 = l, r0 = r;
        d.process (l.data(), r.data(), n, 0);
        check (l == l0 && r == r0 && d.getDelay() == 0, "delay 0 leaves the signal as it is");
    }

    {   // A fixed delay of 100 samples: after the 10 ms crossfade, an impulse comes out exactly 100 samples later.
        DelayLine d;
        d.prepare (sr);
        const int delay = 100;
        std::vector<float> l (n), r (n);
        for (int b = 0; b < 10; ++b)   // settle (fade from the direct signal)
        {
            std::fill (l.begin(), l.end(), 0.0f);
            std::fill (r.begin(), r.end(), 0.0f);
            d.process (l.data(), r.data(), n, delay);
        }
        std::vector<float> outL, outR;
        for (int b = 0; b < 4; ++b)
        {
            std::fill (l.begin(), l.end(), 0.0f);
            std::fill (r.begin(), r.end(), 0.0f);
            if (b == 0) { l[10] = 1.0f; r[10] = -0.5f; }
            d.process (l.data(), r.data(), n, delay);
            outL.insert (outL.end(), l.begin(), l.end());
            outR.insert (outR.end(), r.begin(), r.end());
        }
        int peakAt = -1;
        for (size_t i = 0; i < outL.size(); ++i) if (outL[i] > 0.5f) peakAt = (int) i;
        check (d.getDelay() == delay, "delay reached");
        check (peakAt == 10 + delay && std::abs (outR[(size_t) peakAt] + 0.5f) < 1.0e-6f, "impulse delayed by exactly 100 samples (both channels)");
    }

    {   // Changing the delay crossfades: a steady sine shows no jump larger than a normal sample step.
        DelayLine d;
        d.prepare (sr);
        std::vector<float> l (n), r (n);
        double phase = 0.0;
        float prev = 0.0f, maxStep = 0.0f;
        const double inc = 2.0 * juce::MathConstants<double>::pi * 200.0 / sr;
        for (int b = 0; b < 200; ++b)
        {
            for (int i = 0; i < n; ++i) { l[(size_t) i] = r[(size_t) i] = (float) std::sin (phase); phase += inc; }
            const int delay = b < 40 ? 0 : b < 100 ? 480 : b < 150 ? 1200 : 0;   // 0 -> 10 ms -> 25 ms -> 0
            d.process (l.data(), r.data(), n, delay);
            for (int i = 0; i < n; ++i)
            {
                if (b > 0) maxStep = juce::jmax (maxStep, std::abs (l[(size_t) i] - prev));
                prev = l[(size_t) i];
            }
        }
        // A 200 Hz full-scale sine moves at most ~0.026 per sample; a hard switch would jump up to 2.
        check (maxStep < 0.06f, "delay changes are click-free (crossfaded)");
        check (d.getDelay() == 0, "back to zero delay");
    }

    {   // The delay is limited to the maximum.
        DelayLine d;
        d.prepare (sr);
        std::vector<float> l (n), r (n);
        d.process (l.data(), r.data(), n, 10'000'000);
        check (d.getDelay() == d.maxDelay() && d.maxDelay() >= (int) (sr * DelayLine::kMaxMs / 1000.0), "delay limited to 500 ms");
    }

    {   // Engine: every channel has the parameters, a manual delay alone is the total delay.
        juce::ScopedJuceInitialiser_GUI juceInit;
        hydra::ParamPoller poller;
        hydra::Engine engine;
        check (engine.params.find ("in1.sync") != nullptr && engine.params.find ("in16.delay") != nullptr
                   && engine.params.find ("bus.A1.sync") != nullptr && engine.params.find ("bus.B8.delay") != nullptr,
               "sync / delay parameters exist for inputs and buses");
        engine.params.get ("in2.delay").set (12.5f);
        check (std::abs (engine.getChannelDelayMs (1) - 12.5) < 1.0e-6 && engine.getChannelDelayMs (0) == 0.0,
               "manual delay is the channel's delay (no auto sync)");
        engine.params.get ("bus.A2.delay").set (3.0f);
        check (std::abs (engine.getChannelDelayMs (hydra::Engine::busChannel (1)) - 3.0) < 1.0e-6, "bus delay");
    }
    return fails;
}

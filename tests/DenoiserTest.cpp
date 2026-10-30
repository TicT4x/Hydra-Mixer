// Offline checks for the RNNoise integration: latency per mode, wet/dry alignment, noise reduction,
// odd block sizes, CPU cost.
#include "core/Denoiser.h"
#include "core/Dsp.h"
#include <cstdio>
#include <random>

using hydra::dsp::Denoiser;
using hydra::dsp::DenoiserSettings;

static int fails = 0;
#define CHECK(cond, ...) do { std::printf ((cond) ? "ok:   " : "FAIL: "); std::printf (__VA_ARGS__); std::printf ("\n"); if (! (cond)) ++fails; } while (0)

static constexpr int sr = 48000, len = sr * 4;

static void runBlocks (Denoiser& d, std::vector<float>& l, std::vector<float>& r, const DenoiserSettings& s, bool stereo, unsigned seed)
{
    std::mt19937 rng (seed);
    std::uniform_int_distribution<int> bs (1, 700);
    for (size_t i = 0; i < l.size();)
    {
        const int n = (int) std::min<size_t> ((size_t) bs (rng), l.size() - i);
        d.process (l.data() + i, r.data() + i, n, s, stereo);
        i += (size_t) n;
    }
}

static std::vector<float> voice (bool withPauses, float noiseLevel, unsigned seed)
{
    std::mt19937 rng (seed);
    std::normal_distribution<float> noise (0.0f, noiseLevel);
    std::vector<float> x (len);
    for (int i = 0; i < len; ++i)
    {
        const double t = (double) i / sr;
        const bool voiced = ! withPauses || std::fmod (t, 1.0) < 0.5;
        double v = 0;
        if (voiced)
            for (int h = 1; h <= 8; ++h) v += 0.08 / h * std::sin (2 * 3.14159265 * 137.0 * h * t + h);
        v *= 0.6 + 0.4 * std::sin (2 * 3.14159265 * 3.0 * t);
        x[(size_t) i] = (float) v + (noiseLevel > 0 ? noise (rng) : 0.0f);
    }
    return x;
}

static double pauseLevelDb (const std::vector<float>& x, int offset)
{
    double e = 0; int cnt = 0;
    for (int s = 1; s < 3; ++s)
        for (int i = (int) ((s + 0.6) * sr); i < (int) ((s + 0.95) * sr); ++i)
        { const float v = x[(size_t) (i + offset)]; e += v * v; ++cnt; }
    return 10.0 * std::log10 (e / cnt + 1e-20);
}

int runSessionTest();
int runVoicemeeterImportTest();
int runMidiRangeTest();
int runAppCaptureTest();
int runLedTest();
int runRecorderTest();
int runAsioSecondaryTest();
int runReorderTest();
int runUndoTest();
int runSceneTest();
int runSyncDelayTest();
int runRoomAnalysisTest();
int runCorrectionTest();

int main()
{
    fails += runSessionTest();
    fails += runVoicemeeterImportTest();
    fails += runMidiRangeTest();
    fails += runAppCaptureTest();
    fails += runLedTest();
    fails += runRecorderTest();
    fails += runAsioSecondaryTest();
    fails += runUndoTest();
    fails += runReorderTest();
    fails += runSceneTest();
    fails += runSyncDelayTest();
    fails += runRoomAnalysisTest();
    fails += runCorrectionTest();

    const auto noisy = voice (true, 0.03f, 1);
    const auto clean = voice (false, 0.0f, 2);

    for (int mode = 0; mode < Denoiser::NumModes; ++mode)
    {
        const auto name = Denoiser::modeNames()[mode];
        std::printf ("\n--- %s ---\n", name.toRawUTF8());
        const int lat = Denoiser::latencyForMode (mode);
        DenoiserSettings s; s.mode = mode;

        {   // dry path: exact delay
            Denoiser d; d.prepare (sr);
            s.amount = 0.0f;
            auto l = noisy, r = noisy;
            runBlocks (d, l, r, s, true, 7);
            double err = 0;
            for (int i = lat; i < len; ++i) err = std::max (err, (double) std::abs (l[(size_t) i] - noisy[(size_t) (i - lat)]));
            CHECK (err < 1e-5 && d.getLatencySamples() == lat, "dry path delayed by exactly %d samples (%.0f ms), err %.1e", lat, lat / 48.0, err);
        }
        {   // wet path lag
            Denoiser d; d.prepare (sr);
            s.amount = 1.0f;
            auto l = clean, r = clean;
            runBlocks (d, l, r, s, true, 11);
            int bestLag = 0; double best = -1e9;
            for (int lag = 0; lag < 2000; ++lag)
            {
                double c = 0;
                for (int i = sr; i < 3 * sr; ++i) c += (double) l[(size_t) (i + lag)] * clean[(size_t) i];
                if (c > best) { best = c; bestLag = lag; }
            }
            CHECK (std::abs (bestLag - lat) <= 3, "wet path lag %d samples matches latency (wet/dry aligned)", bestLag);
        }
        {   // noise reduction + cost
            Denoiser d; d.prepare (sr);
            s.amount = 1.0f;
            auto l = noisy, r = noisy;
            const auto t0 = juce::Time::getHighResolutionTicks();
            runBlocks (d, l, r, s, true, 9);
            const double secs = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
            const double before = pauseLevelDb (noisy, 0), after = pauseLevelDb (l, lat);
            CHECK (before - after > 10.0, "noise in pauses reduced by %.1f dB (%.1f -> %.1f dBFS)", before - after, before, after);
            std::printf ("info: stereo CPU %.2f %% of one core\n", 100.0 * secs / 4.0);
        }
    }

    std::printf ("\n--- misc ---\n");
    {
        Denoiser d; d.prepare (sr);
        DenoiserSettings s;
        auto l = noisy, r = std::vector<float> (len, 0.5f);
        runBlocks (d, l, r, s, false, 3);
        CHECK (l == r, "mono mode mirrors L into R");
    }
    {   // strip gate keyed by the denoiser's voice probability (level threshold wide open)
        Denoiser d; d.prepare (sr);
        hydra::dsp::Gate gate; gate.prepare (sr);
        hydra::dsp::GateSettings gs; gs.thresholdDb = -80.0f; gs.rangeDb = -60.0f; gs.holdMs = 200.0f; gs.releaseMs = 150.0f;
        gs.voiceKey = true;
        DenoiserSettings s;
        auto l = noisy, r = noisy;
        std::mt19937 rng (4);
        std::uniform_int_distribution<int> bs (32, 700);
        for (size_t i = 0; i < l.size();)
        {
            const int n = (int) std::min<size_t> ((size_t) bs (rng), l.size() - i);
            d.process (l.data() + i, r.data() + i, n, s, true);
            gs.voice = d.getVad();
            gate.process (l.data() + i, r.data() + i, n, gs);
            i += (size_t) n;
        }
        const double before = pauseLevelDb (noisy, 0), after = pauseLevelDb (l, Denoiser::latencyForMode (s.mode));
        CHECK (before - after > 20.0, "gate voice key closes in pauses (%.1f dB reduction)", before - after);

        hydra::dsp::Gate g2; g2.prepare (sr);
        gs.voice = -1.0f; // denoiser off: plain level gate, stays open above threshold
        auto x = noisy, y = noisy;
        g2.process (x.data(), y.data(), (int) x.size(), gs);
        CHECK (std::abs (pauseLevelDb (x, 0) - before) < 0.5, "voice key without denoiser falls back to the level gate");
    }
    {
        Denoiser d; d.prepare (44100);
        DenoiserSettings s;
        auto l = noisy, r = noisy;
        runBlocks (d, l, r, s, true, 5);
        CHECK (! d.isSupported() && l == noisy && d.getLatencySamples() == 0, "44.1 kHz: bypassed, signal untouched, 0 latency");
    }

    std::printf (fails == 0 ? "\nALL TESTS PASSED\n" : "\n%d TEST(S) FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}

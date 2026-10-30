// Room measurement maths (core/RoomAnalysis): a simulated room is "measured" with the sweep, and the
// impulse response, latency, resonance, decay and the automatic EQ are checked against what was built in.
#include "core/RoomAnalysis.h"
#include <cmath>
#include <cstdio>
#include <random>

namespace
{
constexpr double kPi = 3.14159265358979323846;

/** Two-pole resonator impulse response (a room mode): rings at f with the given 60 dB decay time. */
std::vector<double> mode (double f, double t60, double gain, double sr, int len)
{
    std::vector<double> h ((size_t) len);
    const double r = std::pow (10.0, -3.0 / (t60 * sr));
    for (int i = 0; i < len; ++i)
        h[(size_t) i] = gain * std::pow (r, i) * std::sin (2.0 * kPi * f * i / sr);
    return h;
}

/** Linear convolution (FFT). */
std::vector<double> convolve (const std::vector<float>& x, const std::vector<double>& h)
{
    const int n = hydra::room::nextPow2 ((int) (x.size() + h.size()));
    std::vector<double> xr (x.begin(), x.end()), xi ((size_t) n, 0.0), hr (h), hi ((size_t) n, 0.0);
    xr.resize ((size_t) n, 0.0); hr.resize ((size_t) n, 0.0);
    hydra::room::fft (xr, xi, false);
    hydra::room::fft (hr, hi, false);
    for (int k = 0; k < n; ++k)
    {
        const double re = xr[(size_t) k] * hr[(size_t) k] - xi[(size_t) k] * hi[(size_t) k];
        const double im = xr[(size_t) k] * hi[(size_t) k] + xi[(size_t) k] * hr[(size_t) k];
        xr[(size_t) k] = re; xi[(size_t) k] = im;
    }
    hydra::room::fft (xr, xi, true);
    xr.resize (x.size());
    return xr;
}
} // namespace

int runRoomAnalysisTest()
{
    using namespace hydra::room;
    int fails = 0;
    auto check = [&] (bool c, const char* what) { std::printf ("%s %s\n", c ? "ok:  " : "FAIL:", what); if (! c) ++fails; };

    SweepSpec spec;
    spec.sampleRate = 48000.0;
    spec.seconds = 3.0;
    spec.repeats = 2;
    spec.levelDb = -12.0;
    const double sr = spec.sampleRate;

    // Room: direct sound after 7 ms (device + 1 m of air), half as loud, an 80 Hz mode that rings for 0.8 s,
    // a little diffuse reverb.
    const int delay = (int) (0.007 * sr);
    const int len = (int) (1.2 * sr);
    std::vector<double> room ((size_t) len, 0.0);
    room[(size_t) delay] = 0.5;
    const auto m = mode (80.0, 0.8, 0.0015, sr, len - delay);
    for (size_t i = 0; i < m.size(); ++i) room[(size_t) delay + i] += m[i];
    std::mt19937 rng (7);
    std::normal_distribution<double> noise (0.0, 1.0);
    for (int i = delay + 1; i < len; ++i) room[(size_t) i] += 0.01 * noise (rng) * std::exp (-6.9 * (i - delay) / (0.4 * sr));

    const auto stimulus = makeStimulus (spec);
    check ((int) stimulus.size() == spec.totalLength(), "stimulus = lead + repeats x (sweep + tail)");
    const auto played = convolve (stimulus, room);
    std::vector<float> capture (played.size());
    for (size_t i = 0; i < played.size(); ++i) capture[i] = (float) (played[i] + 1.0e-4 * noise (rng));

    const auto ir = deconvolve (capture, spec);
    std::printf ("      latency %.2f ms, noise %.1f dBFS, peak %.1f dBFS, IR SNR %.1f dB\n",
                 ir.latencyMs, ir.noiseFloorDb, ir.capturePeakDb, ir.irSnrDb);
    check (std::abs (ir.latencyMs - 7.0) < 0.05, "latency of the direct sound found (7 ms)");
    check (! ir.clipped, "no clipping reported");
    check (ir.irSnrDb > 40.0, "impulse response well above the noise");

    AnalysisOptions opts;
    const auto a = analyse (ir, opts, spec.startHz, spec.endHz);
    const double at1k = valueAt (a.freqs, smooth (a.magDb, 3.0), 1000.0);
    std::printf ("      1 kHz %.1f dB (expected %.1f), reference %.1f dB\n", at1k, 20.0 * std::log10 (0.5), a.referenceDb);
    check (std::abs (at1k - 20.0 * std::log10 (0.5)) < 1.5, "response level at 1 kHz matches the direct sound");

    check (! a.resonances.empty(), "a resonance is found");
    if (! a.resonances.empty())
    {
        const auto& r = a.resonances.front();
        std::printf ("      resonance %.1f Hz, +%.1f dB, Q %.1f, rings %.0f ms\n", r.freq, r.excessDb, r.q, r.ringMs);
        check (std::abs (r.freq - 80.0) < 3.0, "strongest resonance at 80 Hz");
        check (r.ringMs > 100.0, "the mode rings longer than 100 ms in the waterfall");
    }
    check (a.waterfall.size() == (size_t) opts.waterfallSlices && a.waterfall[0].size() == a.freqs.size(), "waterfall has all slices");
    {
        const int i80 = (int) (std::log2 (80.0 / 20.0) * Analysis::kPointsPerOctave);
        const int i2k = (int) (std::log2 (2000.0 / 20.0) * Analysis::kPointsPerOctave);
        const auto& late = a.waterfall[10];   // 100 ms
        std::printf ("      waterfall at 100 ms: 80 Hz %.1f dB, 2 kHz %.1f dB\n", late[(size_t) i80], late[(size_t) i2k]);
        check (late[(size_t) i80] - late[(size_t) i2k] > 20.0, "after 100 ms the mode still rings, 2 kHz has decayed");
    }
    for (auto& d : a.decay)
        std::printf ("      %5.0f Hz  EDT %.2f  T20 %.2f  T30 %.2f s\n", d.centreHz, d.edt, d.t20, d.t30);
    {
        const auto& b1k = a.decay[4];
        check (b1k.t20 > 0.25 && b1k.t20 < 0.6, "T20 at 1 kHz near the built-in 0.4 s");
        const auto& b63 = a.decay[0];
        check (b63.t30 > 0.5, "63 Hz band decays slowly (the mode)");
    }

    AutoEqSettings es;
    es.sampleRate = sr;
    const auto eq = designEq (a, es);
    std::printf ("      EQ: %d bands, deviation %.2f -> %.2f dB rms, highest peak +%.1f -> +%.1f dB\n", (int) eq.bands.size(),
                 eq.rmsBeforeDb, eq.rmsAfterDb, eq.peakBeforeDb, eq.peakAfterDb);
    for (auto& b : eq.bands) std::printf ("        %.1f Hz  %.1f dB  Q %.2f\n", b.freq, b.gainDb, b.q);
    check (! eq.bands.empty() && eq.bands.size() <= 8, "EQ has 1..8 bands");
    bool cutAt80 = false, noBoost = true;
    for (auto& b : eq.bands)
    {
        if (std::abs (b.freq - 80.0) < 6.0 && b.gainDb < -3.0) cutAt80 = true;
        if (b.gainDb > 0.0) noBoost = false;
    }
    check (cutAt80, "EQ cuts the 80 Hz resonance");
    check (noBoost, "no boost when boosting is not allowed");
    check (eq.rmsAfterDb < eq.rmsBeforeDb, "EQ reduces the deviation from the target");
    check (eq.peakAfterDb < 3.0 && eq.peakAfterDb < eq.peakBeforeDb * 0.4, "EQ brings the peaks down to the target");

    {   // The EQ curve matches the RBJ bell of the mixer's EQ: +6 dB at the centre, 0 dB far away.
        EqBand b; b.freq = 1000.0; b.gainDb = 6.0; b.q = 2.0;
        check (std::abs (bandMagnitudeDb (b, 1000.0, sr) - 6.0) < 0.01 && std::abs (bandMagnitudeDb (b, 50.0, sr)) < 0.05,
               "bell magnitude: full gain at the centre, none far away");
    }
    return fails;
}

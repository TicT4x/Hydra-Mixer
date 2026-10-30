#pragma once
#include <string>
#include <vector>

/** Room measurement maths (Tools > Room Measurement). Plain C++ without JUCE, so it can be tested anywhere.

    Method: an exponential sine sweep is played through a speaker and recorded with a microphone. Dividing
    the recording's spectrum by the sweep's spectrum gives the impulse response of speaker + room + mic
    (harmonic distortion of the speaker lands at "negative time" and is cut away). Everything else is
    derived from that impulse response:
    - frequency response on a logarithmic grid, with fractional-octave smoothing,
    - waterfall (cumulative spectral decay): the spectrum of the response starting later and later,
      which shows how long each frequency keeps ringing,
    - decay times per octave band (EDT, T20, T30 from the Schroeder integral),
    - resonances: narrow peaks that stand out of the response (room modes), with their ringing time,
    - an automatic EQ (up to 8 bell bands) that flattens the response towards a target. */
namespace hydra::room
{
//==============================================================================
/** In-place radix-2 FFT (size = power of two). inverse = true also divides by the size. */
void fft (std::vector<double>& re, std::vector<double>& im, bool inverse);
int nextPow2 (int n);

//==============================================================================
struct SweepSpec
{
    double sampleRate = 48000.0;
    double startHz = 20.0, endHz = 20000.0;
    double seconds = 5.0;        // length of one sweep
    double levelDb = -15.0;      // peak level of the sweep (dBFS)
    int repeats = 1;             // sweeps in a row; their recordings are averaged (better signal-to-noise)
    double leadSeconds = 0.5;    // silence before the first sweep (devices settle, noise floor is measured)
    double tailSeconds = 1.5;    // silence after every sweep (the room decays, device latency)

    int sweepLength() const noexcept   { return (int) (seconds * sampleRate); }
    int tailLength() const noexcept    { return (int) (tailSeconds * sampleRate); }
    int leadLength() const noexcept    { return (int) (leadSeconds * sampleRate); }
    int segmentLength() const noexcept { return sweepLength() + tailLength(); }
    /** Samples of the whole stimulus (lead + repeats x (sweep + tail)), = samples to record. */
    int totalLength() const noexcept   { return leadLength() + repeats * segmentLength(); }
};

/** One exponential sweep at the spec's level, with short fades at both ends. */
std::vector<float> makeSweep (const SweepSpec&);
/** What is played: lead silence, then repeats x (sweep + silence). */
std::vector<float> makeStimulus (const SweepSpec&);

//==============================================================================
/** Impulse response computed from a recording of the stimulus. */
struct ImpulseResponse
{
    std::vector<float> ir;        // starts 10 ms before the sweep left the output, ends after the tail
    double sampleRate = 48000.0;
    int peak = 0;                 // index of the direct sound (largest sample)
    double latencyMs = 0.0;       // output -> microphone: device latency + sound travel time
    double noiseFloorDb = -120.0; // microphone level in the silence before the sweep (dBFS RMS)
    double capturePeakDb = -120.0;// loudest sample of the recording (dBFS)
    double irSnrDb = 0.0;         // direct sound peak above the noise at the end of the impulse response
    bool clipped = false;         // the recording reached full scale: the result is distorted
};

/** capture: the recording, same length and timing as makeStimulus (spec). */
ImpulseResponse deconvolve (const std::vector<float>& capture, const SweepSpec&);

//==============================================================================
struct BandDecay
{
    double centreHz = 0.0;
    double edt = -1.0, t20 = -1.0, t30 = -1.0;   // seconds (extrapolated to 60 dB), -1 = not enough dynamic range
};

struct Resonance
{
    double freq = 0.0;
    double excessDb = 0.0;   // how far the peak stands above the surrounding response
    double q = 0.0;          // width (centre / -3 dB bandwidth of the peak)
    double ringMs = 0.0;     // time until this frequency has decayed by 20 dB (from the waterfall)
};

/** Microphone calibration: the microphone's own response, taken out of every measurement made with it. */
struct Calibration
{
    std::string name;
    std::vector<std::pair<double, double>> points;   // (Hz, dB), ascending

    bool empty() const noexcept { return points.empty(); }
    /** The microphone's level at f in dB: interpolated on a log frequency axis, held beyond the first / last
        point, limited to +-20 dB (no calibration should turn a measurement upside down). */
    double at (double f) const;
    /** Reads a calibration file: one "frequency dB [phase]" pair per line (REW, miniDSP UMIK, Dayton,
        .txt / .cal / .frd), separated by spaces, tabs, commas or semicolons; other lines (headers,
        "Sens Factor", comments) are skipped. Returns false (with error) if fewer than 3 points were found. */
    static bool parse (const std::string& text, Calibration& out, std::string& error);
};

struct AnalysisOptions
{
    double windowMs = 500.0;      // impulse response length used for the frequency response
    double waterfallMs = 300.0;   // time span of the waterfall
    int waterfallSlices = 31;     // slices at 0 .. waterfallMs
    double waterfallWindowMs = 300.0;
    double resonanceMinHz = 20.0, resonanceMaxHz = 400.0;
    double resonanceThresholdDb = 3.0;
    Calibration calibration;      // taken out of the response and the waterfall
};

struct Analysis
{
    double sampleRate = 48000.0;
    std::vector<double> freqs;          // logarithmic grid, 20 Hz .. 20 kHz (kPointsPerOctave)
    std::vector<double> magDb;          // frequency response (dB re. digital full scale, light 1/48 oct smoothing)
    double referenceDb = 0.0;           // mean level in the band of the sweep: charts show the response relative to it

    std::vector<double> waterfallMs;            // start time of each slice
    std::vector<std::vector<double>> waterfall; // per slice: dB on 'freqs', relative to the top of slice 0

    std::vector<double> etcMs, etcDb;   // energy time curve (impulse view), dB relative to the peak

    std::vector<BandDecay> decay;       // octave bands 63 Hz .. 8 kHz
    std::vector<Resonance> resonances;  // most prominent first
    double sweepStartHz = 20.0, sweepEndHz = 20000.0;

    static constexpr int kPointsPerOctave = 48;
};

/** Frequencies of the analysis grid (20 Hz .. 20 kHz). */
std::vector<double> logGrid();
/** Fractional-octave smoothing (power average over 1/fraction octave) of dB values on logGrid().
    fraction <= 0: unchanged. */
std::vector<double> smooth (const std::vector<double>& db, double fraction);
/** Level in dB at f, linearly interpolated on the grid. */
double valueAt (const std::vector<double>& freqs, const std::vector<double>& db, double f);

Analysis analyse (const ImpulseResponse&, const AnalysisOptions&, double sweepStartHz, double sweepEndHz);

//==============================================================================
/** One band of the designed EQ (Hydra's EQ band types: 0 = Bell). */
struct EqBand
{
    int type = 0;
    double freq = 1000.0, gainDb = 0.0, q = 1.0;
};

struct AutoEqSettings
{
    static constexpr int kMaxBands = 24;
    double lowHz = 25.0, highHz = 500.0;   // range that is corrected
    int maxBands = 8;          // up to kMaxBands
    double maxBoostDb = 0.0;   // boosting dips (often cancellations that no EQ can fill) is limited to this
    double maxCutDb = 15.0;
    double smoothing = 12.0;   // response smoothing for the fit (1/n octave)
    double targetOffsetDb = 0.0;   // target relative to the mean level in the range
    double sampleRate = 48000.0;   // EQ runs at this rate (bell shape near Nyquist)
};

struct AutoEqResult
{
    std::vector<EqBand> bands;        // sorted by frequency
    double preampDb = 0.0;            // -(highest boost of the EQ curve): keeps boosts from clipping
    std::vector<double> eqDb;         // EQ curve on the analysis grid
    std::vector<double> correctedDb;  // smoothed response + EQ (relative to the reference level)
    std::vector<double> smoothedDb;   // smoothed response used for the fit (relative)
    double targetDb = 0.0;            // flat target (relative to the reference level)
    double rmsBeforeDb = 0.0, rmsAfterDb = 0.0;   // deviation from the target in the range
    double peakBeforeDb = 0.0, peakAfterDb = 0.0; // highest peak above the target in the range
};

/** Magnitude (dB) of one EQ band at f (same RBJ filters as Hydra's EQ). */
double bandMagnitudeDb (const EqBand&, double f, double sampleRate);

/** Designs bell bands that bring the response in the range as close to a flat target as the limits allow. */
AutoEqResult designEq (const Analysis&, const AutoEqSettings&);
} // namespace hydra::room

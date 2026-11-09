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
/** Pink noise (equal power per octave, like music on average) with the given RMS level (dBFS), for the level check.
    Deterministic for a seed. */
std::vector<float> makePinkNoise (int length, double rmsDb, unsigned seed = 1);

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
    /** Only the frequency response (and the resonances without their ringing time): what a correction needs, a
        fraction of the time. Waterfall, impulse view and decay stay empty. */
    bool responseOnly = false;
    /** Several positions: each on its own thread (fast). False: one after another on the caller's thread, which leaves
        the other cores to the audio (the plugin inside a DAW). */
    bool parallel = true;
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

    // Multi-point measurement: the response at each microphone position (magDb is their power average). Empty for one.
    std::vector<std::vector<double>> positionDb;

    // Near-field measurement (microphone 1-2 cm in front of the speaker): the speaker's own response without the room,
    // dB re. full scale on 'freqs' (its level is unrelated to magDb). Empty if there is none.
    std::vector<double> nearDb;

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
/** Multi-point measurement (several microphone positions around the listening position): response and waterfall
    are power averages over the positions, decay times their mean; resonances are searched in the average, so only
    what all positions share (the room's modes) stands out, while dips of a single position average out.
    The impulse view shows the first position. */
Analysis analyse (const std::vector<const ImpulseResponse*>& positions, const AnalysisOptions&, double sweepStartHz, double sweepEndHz);

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
    double maxCutDb = 15.0;    // strength: deepest cut a band (or bands together) may make
    double smoothing = 12.0;   // response smoothing for the fit (1/n octave, any value > 0)
    bool matchLoudness = false;    // preamp keeps the loudness (pink noise level) instead of the headroom
    double minSpacing = 0.6;   // band centres at least this many bandwidths apart (mean of both, in octaves): bands spread
                               // over the range instead of piling up (and fighting each other) on one deep dip or peak
    double maxQ = 20.0;            // narrower bands would only fit this one microphone position
    double targetOffsetDb = 0.0;   // target relative to the mean level in the range
    double sampleRate = 48000.0;   // EQ runs at this rate (bell shape near Nyquist)
    /** Near-field response of the speaker (Analysis::nearDb; empty = none). Above nearAboveHz (crossfade over one octave)
        the fit follows it instead of the response at the listening position: there the room's reflections make
        position-dependent ripples that no EQ can fix, while the speaker's own errors are the same everywhere. */
    std::vector<double> nearDb;
    double nearAboveHz = 400.0;
};

struct AutoEqResult
{
    std::vector<EqBand> bands;        // sorted by frequency
    double preampDb = 0.0;            // the chosen one of the two below
    double headroomPreampDb = 0.0;    // -(highest boost of the EQ curve): boosts never clip, but everything gets quieter
    double loudnessPreampDb = 0.0;    // pink noise has the same level with and without the correction
    std::vector<double> eqDb;         // EQ curve on the analysis grid
    std::vector<double> correctedDb;  // smoothed response + EQ (relative to the reference level)
    std::vector<double> smoothedDb;   // smoothed response at the listening position (relative)
    std::vector<double> fitDb;        // what the bands were fitted to: smoothedDb, above nearAboveHz the near field (relative)
    bool usedNear = false;            // the near field took part
    double targetDb = 0.0;            // flat target (relative to the reference level)
    double rmsBeforeDb = 0.0, rmsAfterDb = 0.0;   // deviation from the target in the range
    double peakBeforeDb = 0.0, peakAfterDb = 0.0; // highest peak above the target in the range
};

/** Magnitude (dB) of one EQ band at f (same RBJ filters as Hydra's EQ). */
double bandMagnitudeDb (const EqBand&, double f, double sampleRate);

/** Designs bell bands that bring the response in the range as close to a flat target as the limits allow. */
AutoEqResult designEq (const Analysis&, const AutoEqSettings&);

//==============================================================================
/** Arrival times for the picture of speakers and microphone (ms from the output to the microphone, with the clock-sync
    buffering of the devices already taken out; < 0 = not measured). */
struct Geometry
{
    static constexpr double kNearDistanceM = 0.015;   // microphone in front of the speaker in the near-field measurement
    double nearMs[2] { -1.0, -1.0 };    // speaker k heard with the microphone at it: the system's own latency
    double crossMs[2] { -1.0, -1.0 };   // the other speaker heard with the microphone at speaker k: their distance
    std::vector<double> posMs[2];       // speaker k heard at each listening position (both: left / right separately)
    bool empty() const noexcept { return nearMs[0] < 0.0 && nearMs[1] < 0.0; }
};

/** Where speakers and microphone positions are, from the arrival times. Speakers at (-spacing/2, 0) and (spacing/2, 0),
    y points from the speakers to the listener (metres, seen from above; heights are folded into the distances). */
struct Layout
{
    bool valid = false;        // spacing known (needs a near-field reference and one cross measurement)
    double spacingM = 0.0;
    struct Spot
    {
        bool ok = false;       // both distances measured and they fit the spacing
        double x = 0.0, y = 0.0, distL = 0.0, distR = 0.0;
        double angleL = 0.0, angleR = 0.0;   // degrees off straight ahead (towards the speaker line), left negative
    };
    std::vector<Spot> mics;    // the listening positions in measuring order
};
Layout locate (const Geometry&, double speedOfSound = 343.0);
} // namespace hydra::room

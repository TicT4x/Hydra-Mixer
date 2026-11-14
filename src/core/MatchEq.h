#pragma once
#include "Correction.h"
#include <juce_dsp/juce_dsp.h>

/** Match EQ core: the long-term spectrum of a signal on a 1/3-octave grid, and an EQ that moves one spectrum toward
    another. Used by Studio Voice ("Learn my mic": the voice through this microphone -> a reference voice spectrum plus
    the profile's character); meant for a Match EQ effect too. Message thread / worker only, never the audio thread. */
namespace hydra::match
{
constexpr int kBands = 26;   // 1/3 octave, 50 Hz .. 16 kHz
inline double bandFreq (int i) noexcept { return 50.0 * std::pow (2.0, i / 3.0); }
using Curve = std::array<float, kBands>;   // dB

/** Moves a curve so its mean between 300 Hz and 3 kHz is 0 dB (spectra of any level become comparable). */
void normalise (Curve&);

/** The long-term average spectrum of speech (ANSI S3.5-1997 / SII standard speech spectrum, normal vocal effort;
    1/3-octave band levels, 160 Hz .. 8 kHz from the standard, the ends extrapolated), normalised. */
Curve speechReference();

/** Collects the long-term spectrum of a signal (mono), only of the frames where something plays: frames more than
    12 dB above the quiet ones (the noise floor) and above -60 dBFS. FFT 4096, Hann, half overlap. */
class Analyzer
{
public:
    /** maxSeconds: frames kept (later ones are ignored); a song needs more than a voice. */
    void reset (double sampleRate, double maxSeconds = 85.0);
    void feed (const float* x, int n);
    /** Seconds of the signal heard / of it that counted (above the noise). */
    double totalSeconds() const noexcept;
    double activeSeconds() const;
    /** The normalised spectrum of the active frames; false if there are fewer than minSeconds of them. */
    bool result (Curve& out, double minSeconds = 3.0) const;

private:
    struct Frame { float levelDb; Curve db; };
    double sr = 48000.0;
    std::vector<float> pending, window, fftData;
    std::vector<Frame> frames;
    std::unique_ptr<juce::dsp::FFT> fft;
    long long samples = 0;
    size_t maxFrames = 2000;
    float noiseDb() const;
};

/** How far a fit may go. The defaults are for voices (Studio Voice): below 250 Hz at most +-4 dB, boosts at most
    +6 dB, cuts at most -9 dB, nothing outside 70 Hz .. 14 kHz (a microphone's own response is not reliable there in a
    voice). The Match EQ effect allows more (whole mixes, instruments). */
struct FitLimits
{
    double lowHz = 70.0, highHz = 14000.0;   // outside: no correction
    double bassHz = 250.0, bassDb = 4.0;     // below bassHz at most +-bassDb
    double boostDb = 6.0, cutDb = 9.0;
    double smoothOct = 0.0;                  // the wanted curve is averaged over this many octaves (0 = as measured)
    double taperOct = 0.0;                   // the correction fades in over this many octaves inside lowHz / highHz
};

/** Value of a 1/3-octave curve at f (linear in log frequency, held at the ends). */
double curveAt (const Curve&, double f);

/** What the EQ should do at f (dB) when 'from' is moved toward 'to': the difference at strength (0..1), limited,
    smoothed and faded at the range ends as in 'limits'. fit() designs its bands for exactly this curve; the EQ window
    draws it as the target. */
double wantedDb (const Curve& from, const Curve& to, double f, float strength, const FitLimits& limits);

/** EQ (low / high shelf and bells, the Correction's / ParamEq's biquads) that moves 'from' toward 'to' by strength
    (0..1), within limits. extra (optional) is added after the limits, unscaled: e.g. a sound character that should
    always be there in full. */
std::vector<CorrectionProfile::Band> fit (const Curve& from, const Curve& to, int maxBands, float strength, double sampleRate = 48000.0,
                                          const Curve* extra = nullptr, const FitLimits& limits = {});

/** The long-term spectrum of an audio file (WAV, AIFF, FLAC, Ogg, MP3; channels summed, any rate). Blocking: run it on
    a worker. cancel stops it (returns false). */
bool analyseFile (const juce::File& file, Curve& out, juce::String& error, const std::atomic<bool>& cancel);

//==============================================================================
// Fine resolution (the Match function of the EQ, 14.11.): 1/48 octave from 20 Hz to 20.5 kHz, like the Match EQs of
// mastering suites. Spectra are band powers on this grid (pink noise = flat), normalised (mean 300 Hz .. 3 kHz = 0 dB).

constexpr int kFinePoints = 481;
inline double fineFreq (int i) noexcept { return 20.0 * std::pow (2.0, i / 48.0); }
using Fine = std::vector<float>;   // kFinePoints values in dB

/** Value at f (linear in log frequency, held at the ends). An empty curve is 0 everywhere. */
double fineAt (const Fine&, double f);
void normaliseFine (Fine&);
/** The standard speech spectrum (speechReference) on the fine grid; pink noise = all 0. */
Fine speechFine();
/** Moving average (triangular) over widthOct octaves; below 1/48 octave the curve stays as it is. */
Fine smoothFine (const Fine&, double widthOct);
/** SMOOTH knob 0..1 -> averaging width: 0 = every detail (1/48 octave), 1 = 1.5 octaves. */
double smoothingOctaves (float smooth01) noexcept;

struct MatchSettings
{
    float smooth = 0.5f, amount = 1.0f;     // 0..1
    double lowHz = 20.0, highHz = 20000.0;  // the range the match works in (fades in over 1/4 octave inside)
};
/** What the EQ does (dB per fine point) to move 'source' toward 'reference': the smoothed difference, its mean over the
    range removed (the level stays about the same), times amount, nothing outside the range, limited to +-24 dB. */
Fine matchCurve (const Fine& source, const Fine& reference, const MatchSettings&, const double* mean = nullptr);
/** The mean difference over the range that matchCurve removes (pass it to the single channels' curves, so they keep
    their level relative to the mono sum). */
double matchMean (const Fine& source, const Fine& reference, const MatchSettings&);

/** Long-term spectra on the fine grid of a stereo signal: the mono sum, left, right and side (FFT 16384, Hann, 75 %
    overlap), only frames where something plays (mono level 12 dB above the quiet ones and above -60 dBFS). The power is
    collected per 1-dB level step, so memory stays the same however long it listens. result() can be asked any time
    (the curve builds up while it listens). */
class FineAnalyzer
{
public:
    enum Which { mono = 0, left, right, side, numWhich };
    /** allChannels false: only the mono sum (a quarter of the work); the single channels then have no result. */
    void reset (double sampleRate, bool allChannels = true);
    void feed (const float* monoSignal, int n) { feed (monoSignal, monoSignal, n); }
    void feed (const float* l, const float* r, int n);
    double totalSeconds() const noexcept;
    double activeSeconds() const;
    /** Normalised like the mono sum (its mean 300 Hz .. 3 kHz = 0 dB), so the channels keep their level relative to it. */
    bool result (Fine& out, double minSeconds = 1.0, int which = mono) const;

private:
    static constexpr int kLevels = 140;          // -130 .. +10 dBFS in 1-dB steps
    double sr = 48000.0;
    std::vector<float> pendingL, pendingR, window, fftData;
    std::vector<double> sums;                    // kLevels x numWhich x kFinePoints band powers
    std::vector<int> counts;                     // frames per level step
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<int> binLo, binHi;               // per fine point: bins of its band (binLo > binHi: narrower than a bin)
    std::vector<float> binPos, bandBins;         // centre in bins, band width in bins
    long long samples = 0;
    bool all = true;
    float noiseDb() const;
    int gateStep() const;
    void bandPowers (const float* x, double* out);   // FFT of one windowed frame -> kFinePoints band powers
};

/** The fine spectra of an audio file (mono sum, left, right, side; blocking, run it on a worker; cancel stops it). */
bool analyseFileFine (const juce::File& file, std::array<Fine, FineAnalyzer::numWhich>& out, juce::String& error, const std::atomic<bool>& cancel);

/** Analyses an audio file on a low-priority thread. */
struct FileJob final : juce::Thread
{
    explicit FileJob (juce::File f) : juce::Thread ("Match EQ file"), file (std::move (f)), name (file.getFileNameWithoutExtension()) {}
    ~FileJob() override { cancel = true; stopThread (10000); }
    void run() override { ok = analyseFileFine (file, curves, error, cancel); done = true; }
    juce::File file;
    std::atomic<bool> cancel { false }, done { false };
    bool ok = false;
    std::array<Fine, FineAnalyzer::numWhich> curves;
    juce::String error, name;
};

/** Up to maxBands EQ bands (bell, low / high shelf; ParamEq types) that come as close as they can to 'target' (dB per
    fine point): greedy placement on the largest deviation, then all bands optimised together. rmsDb (optional): what
    is left (dB RMS over the range). */
std::vector<CorrectionProfile::Band> fitBands (const Fine& target, int maxBands, double sampleRate, double lowHz, double highHz,
                                               double* rmsDb = nullptr);
} // namespace hydra::match

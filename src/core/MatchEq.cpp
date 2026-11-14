#include "MatchEq.h"
#include "HeadphoneEq.h"
#include "ParamEq.h"
#include <juce_dsp/juce_dsp.h>

namespace hydra::match
{
namespace
{
constexpr int kOrder = 12, kSize = 1 << kOrder, kHop = kSize / 2;

/** Value of a 1/3-octave curve at f (linear in log frequency, held at the ends). */
double at (const Curve& c, double f)
{
    const double pos = 3.0 * std::log2 (juce::jmax (1.0, f) / 50.0);
    if (pos <= 0.0) return c[0];
    if (pos >= kBands - 1) return c[kBands - 1];
    const int i = (int) pos;
    const double t = pos - i;
    return c[(size_t) i] * (1.0 - t) + c[(size_t) i + 1] * t;
}
} // namespace

void normalise (Curve& c)
{
    double sum = 0.0;
    int n = 0;
    for (int i = 0; i < kBands; ++i)
        if (bandFreq (i) >= 300.0 && bandFreq (i) <= 3000.0) { sum += c[(size_t) i]; ++n; }
    const float m = (float) (sum / juce::jmax (1, n));
    for (auto& v : c) v -= m;
}

Curve speechReference()
{
    // ANSI S3.5-1997 table 3, normal effort: speech spectrum level (dB per Hz), 160 Hz .. 8 kHz. Band level =
    // spectrum level + 10 log10 (1/3-octave bandwidth = 0.2316 f).
    static const float spectrum[18] = { 32.41f, 34.48f, 34.75f, 33.98f, 34.59f, 34.27f, 32.06f, 28.30f, 25.01f,
                                        23.00f, 20.15f, 17.32f, 13.18f, 11.55f, 9.33f, 5.31f, 2.59f, 1.13f };
    Curve c {};
    for (int i = 0; i < kBands; ++i)
    {
        const int s = i - 5;   // band 5 = 158 Hz
        if (s >= 0 && s < 18) c[(size_t) i] = spectrum[s] + 10.0f * (float) std::log10 (0.2316 * bandFreq (i));
    }
    // Below 160 Hz: the fundamental region of low voices, falling about 4 dB per third; above 8 kHz about 2.5 dB.
    for (int i = 4; i >= 0; --i) c[(size_t) i] = c[(size_t) i + 1] - 4.0f;
    for (int i = 23; i < kBands; ++i) c[(size_t) i] = c[(size_t) i - 1] - 2.5f;
    normalise (c);
    return c;
}

//==============================================================================
void Analyzer::reset (double sampleRate, double maxSeconds)
{
    sr = sampleRate;
    maxFrames = (size_t) juce::jmax (16.0, maxSeconds * sr / kHop);
    pending.clear();
    frames.clear();
    samples = 0;
    window.resize (kSize);
    for (int i = 0; i < kSize; ++i) window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) kSize);
    fftData.assign (2 * kSize, 0.0f);
    if (fft == nullptr) fft = std::make_unique<juce::dsp::FFT> (kOrder);
}

void Analyzer::feed (const float* x, int n)
{
    samples += n;
    pending.insert (pending.end(), x, x + n);
    size_t used = 0;
    while (pending.size() - used >= (size_t) kSize)
    {
        const float* f = pending.data() + used;
        double ms = 0.0;
        for (int i = 0; i < kSize; ++i) { fftData[(size_t) i] = f[i] * window[(size_t) i]; ms += (double) f[i] * f[i]; }
        std::fill (fftData.begin() + kSize, fftData.end(), 0.0f);
        fft->performFrequencyOnlyForwardTransform (fftData.data(), true);
        Frame fr;
        fr.levelDb = (float) (10.0 * std::log10 (ms / kSize + 1.0e-20));
        const double binHz = sr / kSize;
        for (int b = 0; b < kBands; ++b)
        {
            const double lo = bandFreq (b) * std::pow (2.0, -1.0 / 6.0), hi = bandFreq (b) * std::pow (2.0, 1.0 / 6.0);
            double p = 0.0;
            for (int k = juce::jmax (1, (int) std::ceil (lo / binHz)); k <= juce::jmin (kSize / 2 - 1, (int) std::floor (hi / binHz)); ++k)
                p += (double) fftData[(size_t) k] * fftData[(size_t) k];
            if (p <= 0.0)   // band narrower than a bin (low bands): the nearest bin
            {
                const int k = juce::jlimit (1, kSize / 2 - 1, juce::roundToInt (bandFreq (b) / binHz));
                p = (double) fftData[(size_t) k] * fftData[(size_t) k] * ((hi - lo) / binHz);
            }
            fr.db[(size_t) b] = (float) (10.0 * std::log10 (p + 1.0e-20));
        }
        if (frames.size() < maxFrames) frames.push_back (fr);
        used += kHop;
    }
    pending.erase (pending.begin(), pending.begin() + (long) used);
}

double Analyzer::totalSeconds() const noexcept { return (double) samples / sr; }

float Analyzer::noiseDb() const
{
    if (frames.empty()) return -120.0f;
    std::vector<float> lv;
    lv.reserve (frames.size());
    for (auto& f : frames) lv.push_back (f.levelDb);
    std::nth_element (lv.begin(), lv.begin() + (long) (lv.size() / 10), lv.end());
    return lv[lv.size() / 10];
}

double Analyzer::activeSeconds() const
{
    const float gate = juce::jmax (noiseDb() + 12.0f, -60.0f);
    int n = 0;
    for (auto& f : frames) if (f.levelDb > gate) ++n;
    return n * (double) kHop / sr;
}

bool Analyzer::result (Curve& out, double minSeconds) const
{
    if (activeSeconds() < minSeconds) return false;
    const float gate = juce::jmax (noiseDb() + 12.0f, -60.0f);
    std::array<double, kBands> pow {};
    int n = 0;
    for (auto& f : frames)
    {
        if (f.levelDb <= gate) continue;
        for (int b = 0; b < kBands; ++b) pow[(size_t) b] += std::pow (10.0, f.db[(size_t) b] / 10.0);
        ++n;
    }
    for (int b = 0; b < kBands; ++b) out[(size_t) b] = (float) (10.0 * std::log10 (pow[(size_t) b] / juce::jmax (1, n) + 1.0e-20));
    normalise (out);
    return true;
}

//==============================================================================
double curveAt (const Curve& c, double f) { return at (c, f); }

double wantedDb (const Curve& from, const Curve& to, double f, float strength, const FitLimits& lim)
{
    const double s = juce::jlimit (0.0f, 1.0f, strength);
    auto raw = [&] (double ff)
    {
        const double d = (at (to, ff) - at (from, ff)) * s;
        return ff < lim.bassHz ? juce::jlimit (-lim.bassDb, lim.bassDb, d) : juce::jlimit (-lim.cutDb, lim.boostDb, d);
    };
    // The range: nothing outside; inside the correction fades in over taperOct (0 = a hard edge).
    double w = 1.0;
    if (f < lim.lowHz || f > lim.highHz) return 0.0;
    if (lim.taperOct > 0.0)
        w = juce::jlimit (0.0, 1.0, juce::jmin (std::log2 (f / lim.lowHz), std::log2 (lim.highHz / f)) / lim.taperOct);
    if (w <= 0.0) return 0.0;
    double d;
    if (lim.smoothOct > 0.01)
    {
        constexpr int taps = 9;
        d = 0.0;
        for (int t = 0; t < taps; ++t)
            d += raw (f * std::pow (2.0, lim.smoothOct * ((double) t / (taps - 1) - 0.5)));
        d /= taps;
    }
    else d = raw (f);
    return d * w;
}

std::vector<CorrectionProfile::Band> fit (const Curve& from, const Curve& to, int maxBands, float strength, double sr, const Curve* extra,
                                          const FitLimits& lim)
{
    // What the EQ should do on the headphone fit's 1/12-octave grid, then the same fit (shelves + bells).
    hp::Curve zero ((size_t) hp::kPoints, 0.0f), wanted ((size_t) hp::kPoints, 0.0f);
    for (int i = 0; i < hp::kPoints; ++i)
    {
        const double f = hp::gridFreq (i);
        double d = wantedDb (from, to, f, strength, lim);
        if (extra != nullptr) d += at (*extra, f);
        wanted[(size_t) i] = (float) d;
    }
    hp::Settings s;
    s.maxBands = maxBands;
    s.maxBoostDb = (float) lim.boostDb;
    const auto d = hp::design (zero, wanted, s, sr);
    return d.profile.bands;
}

bool analyseFile (const juce::File& file, Curve& out, juce::String& error, const std::atomic<bool>& cancel)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
    if (reader == nullptr) { error = "This file cannot be read (WAV, AIFF, FLAC, Ogg and MP3 work)"; return false; }
    if (reader->lengthInSamples < (juce::int64) (reader->sampleRate * 3.0)) { error = "The file is shorter than 3 seconds"; return false; }
    Analyzer a;
    a.reset (reader->sampleRate, 20.0 * 60.0);
    const int chans = juce::jmax (1, (int) reader->numChannels);
    juce::AudioBuffer<float> buf (chans, 65536);
    std::vector<float> mono (65536);
    for (juce::int64 pos = 0; pos < reader->lengthInSamples; pos += buf.getNumSamples())
    {
        if (cancel.load()) { error = "Cancelled"; return false; }
        const int n = (int) juce::jmin ((juce::int64) buf.getNumSamples(), reader->lengthInSamples - pos);
        reader->read (&buf, 0, n, pos, true, true);
        for (int i = 0; i < n; ++i)
        {
            float v = 0.0f;
            for (int c = 0; c < chans; ++c) v += buf.getSample (c, i);
            mono[(size_t) i] = v / (float) chans;
        }
        a.feed (mono.data(), n);
    }
    if (! a.result (out, 2.0)) { error = "The file is (almost) silent"; return false; }
    return true;
}

//==============================================================================
// Fine resolution
double fineAt (const Fine& c, double f)
{
    if (c.size() < 2) return 0.0;
    const double pos = 48.0 * std::log2 (juce::jmax (1.0, f) / 20.0);
    if (pos <= 0.0) return c.front();
    const int last = (int) c.size() - 1;
    if (pos >= last) return c.back();
    const int i = (int) pos;
    const double t = pos - i;
    return c[(size_t) i] * (1.0 - t) + c[(size_t) i + 1] * t;
}

void normaliseFine (Fine& c)
{
    double sum = 0.0;
    int n = 0;
    for (int i = 0; i < (int) c.size(); ++i)
        if (fineFreq (i) >= 300.0 && fineFreq (i) <= 3000.0) { sum += c[(size_t) i]; ++n; }
    const float m = (float) (sum / juce::jmax (1, n));
    for (auto& v : c) v -= m;
}

Fine speechFine()
{
    const auto s = speechReference();
    Fine out ((size_t) kFinePoints);
    for (int i = 0; i < kFinePoints; ++i) out[(size_t) i] = (float) curveAt (s, fineFreq (i));
    normaliseFine (out);
    return out;
}

Fine smoothFine (const Fine& c, double widthOct)
{
    const double half = widthOct * 48.0 * 0.5;
    if (c.size() < 2 || half < 0.5) return c;
    const int h = (int) std::ceil (half);
    const int n = (int) c.size();
    Fine out ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        double sum = 0.0, wsum = 0.0;
        for (int d = -h; d <= h; ++d)
        {
            const int j = i + d;
            if (j < 0 || j >= n) continue;
            const double w = 1.0 - std::abs (d) / (half + 1.0);
            if (w <= 0.0) continue;
            sum += w * c[(size_t) j];
            wsum += w;
        }
        out[(size_t) i] = (float) (sum / wsum);
    }
    return out;
}

double smoothingOctaves (float s) noexcept
{
    if (s <= 0.001f) return 0.0;
    return (1.0 / 48.0) * std::pow (72.0, (double) juce::jlimit (0.0f, 1.0f, s));
}

namespace
{
std::vector<double> rangeWeights (const MatchSettings& m)
{
    std::vector<double> w ((size_t) kFinePoints, 0.0);
    for (int i = 0; i < kFinePoints; ++i)
    {
        const double f = fineFreq (i);
        if (f < m.lowHz || f > m.highHz) continue;
        w[(size_t) i] = juce::jlimit (0.0, 1.0, juce::jmin (std::log2 (f / m.lowHz), std::log2 (m.highHz / f)) / 0.25);
    }
    return w;
}

Fine smoothedDifference (const Fine& source, const Fine& reference, const MatchSettings& m)
{
    Fine diff ((size_t) kFinePoints);
    for (int i = 0; i < kFinePoints; ++i) diff[(size_t) i] = reference[(size_t) i] - source[(size_t) i];
    return smoothFine (diff, smoothingOctaves (m.smooth));
}
} // namespace

double matchMean (const Fine& source, const Fine& reference, const MatchSettings& m)
{
    if ((int) source.size() != kFinePoints || (int) reference.size() != kFinePoints) return 0.0;
    const auto diff = smoothedDifference (source, reference, m);
    const auto w = rangeWeights (m);
    double sum = 0.0, wsum = 0.0;
    for (int i = 0; i < kFinePoints; ++i) { sum += w[(size_t) i] * diff[(size_t) i]; wsum += w[(size_t) i]; }
    return wsum > 0.0 ? sum / wsum : 0.0;
}

Fine matchCurve (const Fine& source, const Fine& reference, const MatchSettings& m, const double* meanIn)
{
    Fine out ((size_t) kFinePoints, 0.0f);
    if ((int) source.size() != kFinePoints || (int) reference.size() != kFinePoints) return out;
    const auto diff = smoothedDifference (source, reference, m);
    const auto w = rangeWeights (m);
    const double mean = meanIn != nullptr ? *meanIn : matchMean (source, reference, m);
    for (int i = 0; i < kFinePoints; ++i)
        out[(size_t) i] = (float) juce::jlimit (-24.0, 24.0, (diff[(size_t) i] - mean) * m.amount * w[(size_t) i]);
    return out;
}

namespace
{
constexpr int kFineOrder = 14, kFineSize = 1 << kFineOrder, kFineHop = kFineSize / 4;
}

void FineAnalyzer::reset (double sampleRate, bool allChannels)
{
    sr = sampleRate;
    all = allChannels;
    pendingL.clear();
    pendingR.clear();
    samples = 0;
    sums.assign ((size_t) kLevels * numWhich * kFinePoints, 0.0);
    counts.assign ((size_t) kLevels, 0);
    window.resize (kFineSize);
    for (int i = 0; i < kFineSize; ++i) window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) kFineSize);
    fftData.assign (2 * kFineSize, 0.0f);
    if (fft == nullptr) fft = std::make_unique<juce::dsp::FFT> (kFineOrder);
    const double binHz = sr / kFineSize;
    binLo.assign (kFinePoints, 0); binHi.assign (kFinePoints, 0);
    binPos.assign (kFinePoints, 0.0f); bandBins.assign (kFinePoints, 0.0f);
    for (int i = 0; i < kFinePoints; ++i)
    {
        const double f = fineFreq (i), lo = f * std::pow (2.0, -1.0 / 96.0), hi = f * std::pow (2.0, 1.0 / 96.0);
        binLo[(size_t) i] = juce::jlimit (1, kFineSize / 2 - 1, (int) std::ceil (lo / binHz));
        binHi[(size_t) i] = juce::jlimit (0, kFineSize / 2 - 1, (int) std::floor (hi / binHz));
        binPos[(size_t) i] = (float) (f / binHz);
        bandBins[(size_t) i] = (float) ((hi - lo) / binHz);
    }
}

void FineAnalyzer::bandPowers (const float* x, double* out)
{
    for (int i = 0; i < kFineSize; ++i) fftData[(size_t) i] = x[i] * window[(size_t) i];
    std::fill (fftData.begin() + kFineSize, fftData.end(), 0.0f);
    fft->performFrequencyOnlyForwardTransform (fftData.data(), true);
    auto pw = [this] (int k) { const double m = fftData[(size_t) juce::jlimit (1, kFineSize / 2 - 1, k)]; return m * m; };
    for (int i = 0; i < kFinePoints; ++i)
    {
        const auto ui = (size_t) i;
        double p = 0.0;
        if (binHi[ui] - binLo[ui] >= 1)
            for (int k = binLo[ui]; k <= binHi[ui]; ++k) p += pw (k);
        else   // band narrower than about two bins (low frequencies): the interpolated bin, scaled to the band's width
        {
            const int k0 = (int) binPos[ui];
            const double t = binPos[ui] - k0;
            p = (pw (k0) * (1.0 - t) + pw (k0 + 1) * t) * bandBins[ui];
        }
        out[ui] = p;
    }
}

void FineAnalyzer::feed (const float* l, const float* r, int n)
{
    samples += n;
    pendingL.insert (pendingL.end(), l, l + n);
    pendingR.insert (pendingR.end(), r, r + n);
    size_t used = 0;
    std::vector<float> x ((size_t) kFineSize);
    std::vector<double> p ((size_t) kFinePoints);
    while (pendingL.size() - used >= (size_t) kFineSize)
    {
        const float* fl = pendingL.data() + used;
        const float* fr = pendingR.data() + used;
        double ms = 0.0;
        for (int i = 0; i < kFineSize; ++i) { const double m = 0.5 * ((double) fl[i] + fr[i]); ms += m * m; }
        const double levelDb = 10.0 * std::log10 (ms / kFineSize + 1.0e-20);
        const int step = juce::jlimit (0, kLevels - 1, (int) std::floor (levelDb + 130.0));
        ++counts[(size_t) step];
        for (int w = 0; w < (all ? (int) numWhich : 1); ++w)
        {
            for (int i = 0; i < kFineSize; ++i)
                x[(size_t) i] = w == mono ? 0.5f * (fl[i] + fr[i]) : w == left ? fl[i] : w == right ? fr[i] : 0.5f * (fl[i] - fr[i]);
            bandPowers (x.data(), p.data());
            double* dst = sums.data() + ((size_t) step * numWhich + (size_t) w) * kFinePoints;
            for (int i = 0; i < kFinePoints; ++i) dst[i] += p[(size_t) i];
        }
        used += kFineHop;
    }
    pendingL.erase (pendingL.begin(), pendingL.begin() + (long) used);
    pendingR.erase (pendingR.begin(), pendingR.begin() + (long) used);
}

double FineAnalyzer::totalSeconds() const noexcept { return (double) samples / sr; }

float FineAnalyzer::noiseDb() const
{
    long long total = 0;
    for (int c : counts) total += c;
    if (total == 0) return -120.0f;
    long long seen = 0;
    for (int s = 0; s < kLevels; ++s)
    {
        seen += counts[(size_t) s];
        if (seen * 10 > total) return (float) s - 130.0f + 0.5f;   // the quietest 10 %
    }
    return 10.0f;
}

int FineAnalyzer::gateStep() const
{
    const float gate = juce::jmax (noiseDb() + 12.0f, -60.0f);
    return juce::jlimit (0, kLevels, (int) std::ceil (gate + 130.0f));
}

double FineAnalyzer::activeSeconds() const
{
    int n = 0;
    for (int s = gateStep(); s < kLevels; ++s) n += counts[(size_t) s];
    return n * (double) kFineHop / sr;
}

bool FineAnalyzer::result (Fine& out, double minSeconds, int which) const
{
    if (counts.empty() || activeSeconds() < minSeconds || (which != mono && ! all)) return false;
    const int g = gateStep();
    auto collect = [&] (int w)
    {
        Fine c ((size_t) kFinePoints);
        for (int i = 0; i < kFinePoints; ++i)
        {
            double sum = 0.0;
            for (int s = g; s < kLevels; ++s) sum += sums[((size_t) s * numWhich + (size_t) w) * kFinePoints + (size_t) i];
            c[(size_t) i] = (float) (10.0 * std::log10 (sum + 1.0e-20));
        }
        return c;
    };
    // All channels move by the mono sum's normalisation (keeps their level relative to it).
    auto m = collect (mono);
    auto normalised = m;
    normaliseFine (normalised);
    const float offset = normalised.empty() ? 0.0f : normalised[0] - m[0];
    out = which == mono ? normalised : collect (juce::jlimit (0, numWhich - 1, which));
    if (which != mono) for (auto& v : out) v += offset;
    return true;
}

bool analyseFileFine (const juce::File& file, std::array<Fine, FineAnalyzer::numWhich>& out, juce::String& error, const std::atomic<bool>& cancel)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
    if (reader == nullptr) { error = "This file cannot be read (WAV, AIFF, FLAC, Ogg and MP3 work)"; return false; }
    if (reader->lengthInSamples < (juce::int64) (reader->sampleRate * 3.0)) { error = "The file is shorter than 3 seconds"; return false; }
    FineAnalyzer a;
    a.reset (reader->sampleRate);
    const int chans = juce::jmax (1, (int) reader->numChannels);
    juce::AudioBuffer<float> buf (chans, 65536);
    const auto len = juce::jmin (reader->lengthInSamples, (juce::int64) (reader->sampleRate * 600.0));
    for (juce::int64 pos = 0; pos < len; pos += buf.getNumSamples())
    {
        if (cancel.load()) { error = "Cancelled"; return false; }
        const int n = (int) juce::jmin ((juce::int64) buf.getNumSamples(), len - pos);
        reader->read (&buf, 0, n, pos, true, true);
        a.feed (buf.getReadPointer (0), buf.getReadPointer (juce::jmin (1, chans - 1)), n);
    }
    if (! a.result (out[0], 2.0)) { error = "The file is (almost) silent"; return false; }
    for (int w = 1; w < FineAnalyzer::numWhich; ++w) a.result (out[(size_t) w], 0.0, w);
    return true;
}

//==============================================================================
namespace
{
struct FitPoint { double cw, c2w, sw, s2w, weight, target; };

struct FitBand
{
    int type = ParamEq::Bell;
    double f = 1000.0, g = 0.0, q = 1.0;
    std::vector<double> resp;
};

void respond (FitBand& b, const std::vector<FitPoint>& pts, double sr)
{
    const auto c = dsp::BiquadCoeffs::make (ParamEq::toDsp (b.type), sr, b.f, b.q, b.g);
    b.resp.resize (pts.size());
    for (size_t i = 0; i < pts.size(); ++i)
    {
        const auto& p = pts[i];
        const double nr = c.b0 + c.b1 * p.cw + c.b2 * p.c2w, ni = -(c.b1 * p.sw + c.b2 * p.s2w);
        const double dr = 1.0 + c.a1 * p.cw + c.a2 * p.c2w, di = -(c.a1 * p.sw + c.a2 * p.s2w);
        b.resp[i] = 10.0 * std::log10 ((nr * nr + ni * ni) / juce::jmax (1.0e-30, dr * dr + di * di) + 1.0e-30);
    }
}
} // namespace

std::vector<CorrectionProfile::Band> fitBands (const Fine& target, int maxBands, double sr, double lowHz, double highHz, double* rmsDb)
{
    std::vector<CorrectionProfile::Band> result;
    if ((int) target.size() != kFinePoints || maxBands <= 0) return result;
    std::vector<FitPoint> pts;
    std::vector<int> fineIndex;
    for (int i = 0; i < kFinePoints; ++i)
    {
        const double f = fineFreq (i);
        if (f >= sr * 0.45) break;
        const double w = juce::MathConstants<double>::twoPi * f / sr;
        const bool inside = f >= lowHz && f <= highHz;
        pts.push_back ({ std::cos (w), std::cos (2.0 * w), std::sin (w), std::sin (2.0 * w), inside ? 1.0 : 0.3, target[(size_t) i] });
        fineIndex.push_back (i);
    }
    const size_t np = pts.size();
    std::vector<FitBand> bands;
    std::vector<double> sum (np, 0.0);   // response of all bands
    auto errorWith = [&] (const std::vector<double>& minus, const std::vector<double>& plus)
    {
        double e = 0.0;
        for (size_t i = 0; i < np; ++i)
        {
            const double r = pts[i].target - (sum[i] - minus[i] + plus[i]);
            e += pts[i].weight * r * r;
        }
        return e;
    };
    const double fLo = juce::jmax (20.0, lowHz * 0.8), fHi = juce::jmin (sr * 0.45, highHz * 1.2);
    auto clampBand = [&] (FitBand& b)
    {
        b.f = juce::jlimit (fLo, fHi, b.f);
        b.g = juce::jlimit (-18.0, 18.0, b.g);
        b.q = b.type == ParamEq::Bell ? juce::jlimit (0.3, 12.0, b.q) : juce::jlimit (0.4, 1.5, b.q);
    };
    auto optimise = [&] (int sweeps)
    {
        for (int s = 0; s < sweeps; ++s)
        {
            const double scale = 1.0 / (1.0 + 0.6 * s);
            for (auto& b : bands)
            {
                for (int param = 0; param < 3; ++param)
                {
                    double base = errorWith (b.resp, b.resp);
                    for (int dir : { 1, -1 })
                    {
                        for (int tries = 0; tries < 4; ++tries)
                        {
                            FitBand t = b;
                            if (param == 0) t.g += dir * 1.0 * scale;
                            if (param == 1) t.f *= std::pow (2.0, dir * scale / 8.0);
                            if (param == 2) t.q *= std::pow (2.0, dir * 0.35 * scale);
                            clampBand (t);
                            respond (t, pts, sr);
                            const double e = errorWith (b.resp, t.resp);
                            if (e >= base - 1.0e-9) break;
                            for (size_t i = 0; i < np; ++i) sum[i] += t.resp[i] - b.resp[i];
                            b = std::move (t);
                            base = e;
                        }
                    }
                }
            }
        }
    };

    for (int n = 0; n < maxBands; ++n)
    {
        Fine residual ((size_t) kFinePoints, 0.0f);
        for (size_t i = 0; i < np; ++i) residual[(size_t) fineIndex[i]] = (float) (pts[i].weight >= 1.0 ? pts[i].target - sum[i] : 0.0);
        const auto rs = smoothFine (residual, 1.0 / 6.0);
        int best = -1;
        float bestAbs = 0.25f;
        for (size_t i = 0; i < np; ++i)
            if (pts[i].weight >= 1.0 && std::abs (rs[(size_t) fineIndex[i]]) > bestAbs) { bestAbs = std::abs (rs[(size_t) fineIndex[i]]); best = fineIndex[i]; }
        if (best < 0) break;
        const float peak = rs[(size_t) best];
        int left = best, right = best;
        while (left > 0 && rs[(size_t) left - 1] * peak > 0.0f && std::abs (rs[(size_t) left - 1]) > std::abs (peak) * 0.5f) --left;
        while (right < kFinePoints - 1 && rs[(size_t) right + 1] * peak > 0.0f && std::abs (rs[(size_t) right + 1]) > std::abs (peak) * 0.5f) ++right;
        FitBand b;
        b.g = peak;
        const double bw = juce::jmax (1.0 / 12.0, (right - left) / 48.0);
        const double r2 = std::pow (2.0, bw);
        b.q = std::sqrt (r2) / (r2 - 1.0);
        b.f = fineFreq (best);
        if (fineFreq (left) <= lowHz * std::pow (2.0, 1.0 / 3.0)) { b.type = ParamEq::LowShelf; b.f = fineFreq (right); b.q = 0.707; }
        else if (fineFreq (right) >= highHz * std::pow (2.0, -1.0 / 3.0)) { b.type = ParamEq::HighShelf; b.f = fineFreq (left); b.q = 0.707; }
        clampBand (b);
        respond (b, pts, sr);
        for (size_t i = 0; i < np; ++i) sum[i] += b.resp[i];
        bands.push_back (std::move (b));
        optimise (3);
    }
    optimise (8);

    double e = 0.0;
    int count = 0;
    for (size_t i = 0; i < np; ++i)
        if (pts[i].weight >= 1.0) { const double r = pts[i].target - sum[i]; e += r * r; ++count; }
    if (rmsDb != nullptr) *rmsDb = std::sqrt (e / juce::jmax (1, count));
    for (const auto& b : bands)
        if (std::abs (b.g) >= 0.1)
            result.push_back ({ b.type, (float) b.f, (float) b.g, (float) b.q });
    return result;
}
} // namespace hydra::match

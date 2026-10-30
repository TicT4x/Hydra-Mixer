#include "RoomAnalysis.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <numeric>

namespace hydra::room
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kPreMs = 10.0;   // impulse response starts this long before the sweep left the output

double toDb (double power) { return 10.0 * std::log10 (power + 1.0e-30); }

/** Power spectrum of x (zero padded to n) mapped onto the analysis grid: power average of the FFT bins
    inside each grid cell (1/48 octave), interpolated where a cell holds no bin (low frequencies). */
std::vector<double> gridPower (const std::vector<double>& x, int n, double sr, const std::vector<double>& grid)
{
    std::vector<double> re (x.begin(), x.end()), im;
    re.resize ((size_t) n, 0.0);
    im.assign ((size_t) n, 0.0);
    fft (re, im, false);
    const int half = n / 2;
    std::vector<double> p ((size_t) half + 1);
    for (int k = 0; k <= half; ++k) p[(size_t) k] = re[(size_t) k] * re[(size_t) k] + im[(size_t) k] * im[(size_t) k];

    const double binHz = sr / n;
    const double cell = std::pow (2.0, 0.5 / Analysis::kPointsPerOctave);
    std::vector<double> out (grid.size());
    for (size_t i = 0; i < grid.size(); ++i)
    {
        const double f = grid[i];
        const int lo = (int) std::ceil (f / cell / binHz), hi = (int) std::floor (f * cell / binHz);
        if (hi >= lo && lo >= 0 && hi <= half)
        {
            double s = 0.0;
            for (int k = lo; k <= hi; ++k) s += p[(size_t) k];
            out[i] = s / (hi - lo + 1);
        }
        else
        {
            const double pos = std::min ((double) half, f / binHz);
            const int k0 = std::min (half - 1, (int) pos);
            const double t = pos - k0;
            out[i] = p[(size_t) k0] * (1.0 - t) + p[(size_t) k0 + 1] * t;
        }
    }
    return out;
}

/** Linear regression slope (dB per second) of curve[from..to) sampled every step samples. */
double slope (const std::vector<double>& curve, int from, int to, double sr, int step)
{
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int count = 0;
    for (int i = from; i < to; i += step)
    {
        const double x = i / sr, y = curve[(size_t) i];
        sx += x; sy += y; sxx += x * x; sxy += x * y; ++count;
    }
    if (count < 3) return 0.0;
    const double d = count * sxx - sx * sx;
    return d != 0.0 ? (count * sxy - sx * sy) / d : 0.0;
}

double median (std::vector<double> v)
{
    if (v.empty()) return 0.0;
    const auto mid = v.begin() + (std::ptrdiff_t) (v.size() / 2);
    std::nth_element (v.begin(), mid, v.end());
    return *mid;
}
} // namespace

//==============================================================================
int nextPow2 (int n)
{
    int p = 1;
    while (p < n) p <<= 1;
    return p;
}

void fft (std::vector<double>& re, std::vector<double>& im, bool inverse)
{
    const size_t n = re.size();
    for (size_t i = 1, j = 0; i < n; ++i)   // bit reversal
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap (re[i], re[j]); std::swap (im[i], im[j]); }
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = 2.0 * kPi / (double) len * (inverse ? 1.0 : -1.0);
        const double wr = std::cos (ang), wi = std::sin (ang);
        const size_t halfLen = len / 2;
        for (size_t i = 0; i < n; i += len)
        {
            double cr = 1.0, ci = 0.0;
            for (size_t k = 0; k < halfLen; ++k)
            {
                const size_t a = i + k, b = a + halfLen;
                const double tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr;        im[a] += ti;
                const double nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }
    if (inverse)
        for (size_t i = 0; i < n; ++i) { re[i] /= (double) n; im[i] /= (double) n; }
}

//==============================================================================
std::vector<float> makeSweep (const SweepSpec& s)
{
    const int n = s.sweepLength();
    std::vector<float> x ((size_t) std::max (0, n));
    if (n <= 0) return x;
    const double f1 = s.startHz, f2 = std::min (s.endHz, s.sampleRate * 0.48);
    const double T = s.seconds, L = T / std::log (f2 / f1);
    const double amp = std::pow (10.0, s.levelDb / 20.0);
    const int fadeIn = std::min (n / 4, (int) (s.sampleRate / f1)), fadeOut = std::min (n / 4, (int) (0.005 * s.sampleRate));
    for (int i = 0; i < n; ++i)
    {
        const double t = i / s.sampleRate;
        double v = amp * std::sin (2.0 * kPi * f1 * L * (std::exp (t / L) - 1.0));
        if (i < fadeIn)           v *= 0.5 - 0.5 * std::cos (kPi * i / fadeIn);
        if (i >= n - fadeOut)     v *= 0.5 - 0.5 * std::cos (kPi * (n - 1 - i) / fadeOut);
        x[(size_t) i] = (float) v;
    }
    return x;
}

std::vector<float> makeStimulus (const SweepSpec& s)
{
    std::vector<float> out ((size_t) s.totalLength(), 0.0f);
    const auto sweep = makeSweep (s);
    for (int r = 0; r < s.repeats; ++r)
        std::copy (sweep.begin(), sweep.end(), out.begin() + (std::ptrdiff_t) (s.leadLength() + r * s.segmentLength()));
    return out;
}

//==============================================================================
ImpulseResponse deconvolve (const std::vector<float>& capture, const SweepSpec& s)
{
    ImpulseResponse res;
    res.sampleRate = s.sampleRate;
    const int seg = s.segmentLength(), lead = s.leadLength(), sw = s.sweepLength();
    if (seg <= 0 || sw <= 0) return res;

    // Level checks on the raw recording.
    double peak = 0.0;
    for (float v : capture) peak = std::max (peak, (double) std::abs (v));
    res.capturePeakDb = 20.0 * std::log10 (peak + 1.0e-12);
    res.clipped = peak >= 0.99;
    {
        double e = 0.0;
        int count = 0;
        for (int i = lead / 5; i < lead && i < (int) capture.size(); ++i) { e += (double) capture[(size_t) i] * capture[(size_t) i]; ++count; }
        res.noiseFloorDb = count > 0 ? toDb (e / count) : -120.0;
    }

    // Synchronous average of the repeats.
    std::vector<double> y ((size_t) seg, 0.0);
    int used = 0;
    for (int r = 0; r < s.repeats; ++r)
    {
        const int start = lead + r * seg;
        if (start + seg > (int) capture.size()) break;
        for (int i = 0; i < seg; ++i) y[(size_t) i] += capture[(size_t) (start + i)];
        ++used;
    }
    if (used == 0) return res;
    for (auto& v : y) v /= used;

    // H = Y X* / (|X|^2 + eps): eps is tiny inside the sweep's band and large outside it (band limit).
    const int n = nextPow2 (seg + sw);
    const auto sweep = makeSweep (s);
    std::vector<double> xr (sweep.begin(), sweep.end()), xi, yr (y), yi;
    xr.resize ((size_t) n, 0.0); xi.assign ((size_t) n, 0.0);
    yr.resize ((size_t) n, 0.0); yi.assign ((size_t) n, 0.0);
    fft (xr, xi, false);
    fft (yr, yi, false);

    const double f1 = s.startHz, f2 = std::min (s.endHz, s.sampleRate * 0.48);
    double maxX = 0.0;
    for (int k = 0; k <= n / 2; ++k) maxX = std::max (maxX, xr[(size_t) k] * xr[(size_t) k] + xi[(size_t) k] * xi[(size_t) k]);
    std::vector<double> hr ((size_t) n), hi ((size_t) n);
    for (int k = 0; k < n; ++k)
    {
        const double f = (k <= n / 2 ? k : n - k) * s.sampleRate / n;
        // Inside the band w = 1; falls to 0 over half an octave outside it.
        double outside = 0.0;
        if (f < f1)      outside = f > 0.0 ? std::log2 (f1 / f) : 1.0;
        else if (f > f2) outside = std::log2 (f / f2);
        const double w = outside <= 0.0 ? 1.0 : outside >= 0.5 ? 0.0 : 0.5 + 0.5 * std::cos (kPi * outside / 0.5);
        const double x2 = xr[(size_t) k] * xr[(size_t) k] + xi[(size_t) k] * xi[(size_t) k];
        const double eps = maxX * (1.0e-6 + (1.0 - w));
        const std::complex<double> Y (yr[(size_t) k], yi[(size_t) k]), X (xr[(size_t) k], xi[(size_t) k]);
        const auto H = Y * std::conj (X) * w / (x2 + eps);
        hr[(size_t) k] = H.real(); hi[(size_t) k] = H.imag();
    }
    fft (hr, hi, true);

    // Causal part (the room decays within the tail) plus a short stretch of negative time, so a direct sound
    // that arrives with (almost) no latency keeps its start. Harmonic distortion lies much further back.
    const int pre = (int) (kPreMs * 0.001 * s.sampleRate);
    const int len = std::min (s.tailLength(), n / 2);
    res.ir.resize ((size_t) (pre + len));
    for (int i = 0; i < pre; ++i) res.ir[(size_t) i] = (float) hr[(size_t) (n - pre + i)];
    for (int i = 0; i < len; ++i) res.ir[(size_t) (pre + i)] = (float) hr[(size_t) i];

    double best = -1.0;
    for (size_t i = 0; i < res.ir.size(); ++i)
        if (std::abs (res.ir[i]) > best) { best = std::abs (res.ir[i]); res.peak = (int) i; }
    res.latencyMs = (res.peak - pre) * 1000.0 / s.sampleRate;

    const size_t tail0 = res.ir.size() - res.ir.size() / 5;
    double tail = 0.0;
    for (size_t i = tail0; i < res.ir.size(); ++i) tail += (double) res.ir[i] * res.ir[i];
    tail /= (double) std::max<size_t> (1, res.ir.size() - tail0);
    res.irSnrDb = toDb (best * best) - toDb (tail);
    return res;
}

//==============================================================================
std::vector<double> logGrid()
{
    std::vector<double> g;
    for (int i = 0;; ++i)
    {
        const double f = 20.0 * std::pow (2.0, (double) i / Analysis::kPointsPerOctave);
        if (f > 20000.0 * 1.0001) break;
        g.push_back (f);
    }
    return g;
}

std::vector<double> smooth (const std::vector<double>& db, double fraction)
{
    const int h = fraction > 0.0 ? (int) (Analysis::kPointsPerOctave / (2.0 * fraction)) : 0;
    if (h <= 0 || db.empty()) return db;
    const int n = (int) db.size();
    std::vector<double> sum ((size_t) n + 1, 0.0);
    for (int i = 0; i < n; ++i) sum[(size_t) i + 1] = sum[(size_t) i] + std::pow (10.0, db[(size_t) i] / 10.0);
    std::vector<double> out ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        const int w = std::min ({ h, i, n - 1 - i });   // symmetric window (shrinks at the ends)
        out[(size_t) i] = toDb ((sum[(size_t) (i + w + 1)] - sum[(size_t) (i - w)]) / (2 * w + 1));
    }
    return out;
}

double valueAt (const std::vector<double>& freqs, const std::vector<double>& db, double f)
{
    if (freqs.empty() || db.size() != freqs.size()) return 0.0;
    if (f <= freqs.front()) return db.front();
    if (f >= freqs.back()) return db.back();
    const auto it = std::upper_bound (freqs.begin(), freqs.end(), f);
    const size_t i = (size_t) (it - freqs.begin());
    const double t = std::log (f / freqs[i - 1]) / std::log (freqs[i] / freqs[i - 1]);
    return db[i - 1] + (db[i] - db[i - 1]) * t;
}

//==============================================================================
Analysis analyse (const ImpulseResponse& ir, const AnalysisOptions& o, double sweepStartHz, double sweepEndHz)
{
    Analysis a;
    a.sampleRate = ir.sampleRate;
    a.sweepStartHz = sweepStartHz;
    a.sweepEndHz = std::min (sweepEndHz, ir.sampleRate * 0.48);
    a.freqs = logGrid();
    const double sr = ir.sampleRate;
    const int size = (int) ir.ir.size();
    if (size == 0) return a;
    const int peak = std::clamp (ir.peak, 0, size - 1);
    auto sample = [&] (int i) { return i >= 0 && i < size ? (double) ir.ir[(size_t) i] : 0.0; };

    // Frequency response: from 2 ms before the direct sound over windowMs, tapered at both ends.
    {
        const int rise = (int) (0.002 * sr), len = (int) (o.windowMs * 0.001 * sr);
        const int fall = len / 4;
        std::vector<double> x ((size_t) (rise + len));
        for (int i = 0; i < rise + len; ++i)
        {
            double w = 1.0;
            if (i < rise) w = 0.5 - 0.5 * std::cos (kPi * i / rise);
            else if (i >= rise + len - fall) w = 0.5 + 0.5 * std::cos (kPi * (i - (rise + len - fall)) / fall);
            x[(size_t) i] = sample (peak - rise + i) * w;
        }
        const auto p = gridPower (x, std::max (65536, nextPow2 ((int) x.size()) * 2), sr, a.freqs);
        a.magDb.resize (p.size());
        for (size_t i = 0; i < p.size(); ++i) a.magDb[i] = toDb (p[i]);

        const auto third = smooth (a.magDb, 3.0);
        const double lo = std::max (40.0, a.sweepStartHz * 1.5), hi = std::min (12000.0, a.sweepEndHz / 1.5);
        double s = 0.0;
        int count = 0;
        for (size_t i = 0; i < a.freqs.size(); ++i)
            if (a.freqs[i] >= lo && a.freqs[i] <= hi) { s += third[i]; ++count; }
        a.referenceDb = count > 0 ? s / count : 0.0;
    }

    // Waterfall: the response starting later and later (fixed window length, the end tapered).
    {
        const int slices = std::max (2, o.waterfallSlices);
        const int rise = (int) (0.0005 * sr), len = (int) (o.waterfallWindowMs * 0.001 * sr);
        const int fall = len / 2;
        const int fftSize = std::max (32768, nextPow2 (rise + len) * 2);
        double top = -1.0e9;
        for (int sIdx = 0; sIdx < slices; ++sIdx)
        {
            const double t = o.waterfallMs * sIdx / (slices - 1);
            const int start = peak - rise + (int) (t * 0.001 * sr);
            std::vector<double> x ((size_t) (rise + len));
            for (int i = 0; i < rise + len; ++i)
            {
                double w = 1.0;
                if (i < rise) w = 0.5 - 0.5 * std::cos (kPi * i / rise);
                else if (i >= rise + len - fall) w = 0.5 + 0.5 * std::cos (kPi * (i - (rise + len - fall)) / fall);
                x[(size_t) i] = sample (start + i) * w;
            }
            auto p = gridPower (x, fftSize, sr, a.freqs);
            std::vector<double> db (p.size());
            for (size_t i = 0; i < p.size(); ++i) db[i] = toDb (p[i]);
            db = smooth (db, 24.0);
            if (sIdx == 0)
                for (size_t i = 0; i < db.size(); ++i)
                    if (a.freqs[i] >= a.sweepStartHz && a.freqs[i] <= a.sweepEndHz) top = std::max (top, db[i]);
            a.waterfallMs.push_back (t);
            a.waterfall.push_back (std::move (db));
        }
        for (auto& sl : a.waterfall)
            for (auto& v : sl) v -= top;
    }

    // Energy time curve: peak of each 0.5 ms block, from 10 ms before the direct sound.
    {
        const int block = std::max (1, (int) (0.0005 * sr));
        const double p2 = std::max (1.0e-30, sample (peak) * sample (peak));
        for (int i = std::max (0, peak - (int) (0.01 * sr)); i + block <= size; i += block)
        {
            double m = 0.0;
            for (int k = 0; k < block; ++k) m = std::max (m, sample (i + k) * sample (i + k));
            a.etcMs.push_back ((i - peak) * 1000.0 / sr);
            a.etcDb.push_back (toDb (m) - toDb (p2));
        }
    }

    // Decay per octave band (Schroeder backward integration of the band-filtered impulse response).
    {
        const int len = size - peak;
        const int n = nextPow2 (len) * 2;
        std::vector<double> re ((size_t) n, 0.0), im ((size_t) n, 0.0);
        for (int i = 0; i < len; ++i) re[(size_t) i] = sample (peak + i);
        fft (re, im, false);
        for (double fc : { 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0 })
        {
            BandDecay d;
            d.centreHz = fc;
            if (fc < a.sweepStartHz * 1.4 || fc > a.sweepEndHz / 1.4 || len < (int) (0.05 * sr)) { a.decay.push_back (d); continue; }

            // Octave filter with raised-cosine edges (neighbouring bands add up to 1), zero phase.
            std::vector<double> br ((size_t) n), bi ((size_t) n);
            for (int k = 0; k < n; ++k)
            {
                const double f = (k <= n / 2 ? k : n - k) * sr / n;
                const double x = f > 0.0 ? std::abs (std::log2 (f / fc)) : 10.0;
                const double w = x < 0.35 ? 1.0 : x > 0.65 ? 0.0 : 0.5 + 0.5 * std::cos (kPi * (x - 0.35) / 0.3);
                br[(size_t) k] = re[(size_t) k] * w;
                bi[(size_t) k] = im[(size_t) k] * w;
            }
            fft (br, bi, true);

            std::vector<double> e ((size_t) len);
            for (int i = 0; i < len; ++i) e[(size_t) i] = br[(size_t) i] * br[(size_t) i];
            const int tail0 = len - len / 10;
            double noise = 0.0;
            for (int i = tail0; i < len; ++i) noise += e[(size_t) i];
            noise /= std::max (1, len - tail0);

            // Truncate where the decay (10 ms blocks) reaches the noise.
            const int blk = std::max (1, (int) (0.01 * sr));
            int end = len;
            double maxBlock = 0.0;
            for (int i = 0; i + blk <= len; i += blk)
            {
                double m = 0.0;
                for (int k = 0; k < blk; ++k) m += e[(size_t) (i + k)];
                m /= blk;
                maxBlock = std::max (maxBlock, m);
                if (maxBlock > 0.0 && m <= noise * 2.0 && i > 0) { end = i; break; }
            }

            std::vector<double> sch ((size_t) end);
            double acc = 0.0;
            for (int i = end - 1; i >= 0; --i)
            {
                acc += std::max (0.0, e[(size_t) i] - noise);
                sch[(size_t) i] = acc;
            }
            if (acc <= 0.0) { a.decay.push_back (d); continue; }
            for (auto& v : sch) v = toDb (v / acc);

            auto fit = [&] (double fromDb, double toDbLevel)
            {
                int i0 = -1, i1 = -1;
                for (int i = 0; i < end; ++i)
                {
                    if (i0 < 0 && sch[(size_t) i] <= fromDb) i0 = i;
                    if (sch[(size_t) i] <= toDbLevel) { i1 = i; break; }
                }
                if (i0 < 0 || i1 <= i0) return -1.0;
                const double sl = slope (sch, i0, i1 + 1, sr, std::max (1, (i1 - i0) / 2000));
                return sl < 0.0 ? -60.0 / sl : -1.0;
            };
            d.edt = fit (-0.01, -10.0);
            d.t20 = fit (-5.0, -25.0);
            d.t30 = fit (-5.0, -35.0);
            a.decay.push_back (d);
        }
    }

    // Resonances: peaks of the 1/12 octave response that stand out of the 1 octave response.
    {
        const auto fine = smooth (a.magDb, 12.0), broad = smooth (a.magDb, 1.0), shape = smooth (a.magDb, 24.0);
        const double lo = std::max (o.resonanceMinHz, a.sweepStartHz), hi = std::min (o.resonanceMaxHz, a.sweepEndHz);
        const int n = (int) a.freqs.size(), span = Analysis::kPointsPerOctave / 6;
        std::vector<Resonance> found;
        for (int i = 1; i + 1 < n; ++i)
        {
            const double f = a.freqs[(size_t) i];
            if (f < lo || f > hi) continue;
            bool isMax = true;
            for (int k = std::max (0, i - span); k <= std::min (n - 1, i + span) && isMax; ++k)
                if (fine[(size_t) k] > fine[(size_t) i] || (fine[(size_t) k] == fine[(size_t) i] && k < i)) isMax = false;
            const double excess = fine[(size_t) i] - broad[(size_t) i];
            if (! isMax || excess < o.resonanceThresholdDb) continue;

            Resonance r;
            r.freq = f;
            r.excessDb = excess;
            // Width at half the peak's height above its surroundings (as a bell filter's Q is defined).
            const double level = shape[(size_t) i] - excess * 0.5;
            int l = i, h = i;
            while (l > 0 && shape[(size_t) l] > level) --l;
            while (h < n - 1 && shape[(size_t) h] > level) ++h;
            const double bw = a.freqs[(size_t) h] - a.freqs[(size_t) l];
            r.q = std::clamp (bw > 0.0 ? f / bw : 10.0, 0.5, 30.0);

            // Ringing: when this frequency has fallen 20 dB below its level in the first slice.
            r.ringMs = a.waterfallMs.empty() ? 0.0 : a.waterfallMs.back();
            if (! a.waterfall.empty())
            {
                const double start = a.waterfall[0][(size_t) i];
                for (size_t sIdx = 1; sIdx < a.waterfall.size(); ++sIdx)
                {
                    const double v = a.waterfall[sIdx][(size_t) i];
                    if (v <= start - 20.0)
                    {
                        const double prev = a.waterfall[sIdx - 1][(size_t) i];
                        const double t = prev > v ? (prev - (start - 20.0)) / (prev - v) : 0.0;
                        r.ringMs = a.waterfallMs[sIdx - 1] + t * (a.waterfallMs[sIdx] - a.waterfallMs[sIdx - 1]);
                        break;
                    }
                }
            }
            found.push_back (r);
        }
        std::sort (found.begin(), found.end(), [] (const Resonance& x, const Resonance& y) { return x.excessDb > y.excessDb; });
        if (found.size() > 16) found.resize (16);
        a.resonances = std::move (found);
    }
    return a;
}

//==============================================================================
double bandMagnitudeDb (const EqBand& b, double f, double sr)
{
    // RBJ peaking EQ, as dsp::BiquadCoeffs::make (Type::Peak) builds it.
    const double freq = std::clamp (b.freq, 10.0, sr * 0.49), q = std::max (0.05, b.q);
    const double A = std::pow (10.0, b.gainDb / 40.0);
    const double w0 = 2.0 * kPi * freq / sr, cw = std::cos (w0), alpha = std::sin (w0) / (2.0 * q);
    const double b0 = 1 + alpha * A, b1 = -2 * cw, b2 = 1 - alpha * A;
    const double a0 = 1 + alpha / A, a1 = -2 * cw, a2 = 1 - alpha / A;
    const std::complex<double> z = std::polar (1.0, -2.0 * kPi * f / sr);
    const auto num = b0 + b1 * z + b2 * z * z, den = a0 + a1 * z + a2 * z * z;
    return 20.0 * std::log10 (std::abs (num / den) + 1.0e-12);
}

AutoEqResult designEq (const Analysis& a, const AutoEqSettings& s)
{
    AutoEqResult res;
    const auto& f = a.freqs;
    const int n = (int) f.size();
    if (n == 0 || a.magDb.size() != f.size()) return res;

    res.smoothedDb = smooth (a.magDb, s.smoothing);
    for (auto& v : res.smoothedDb) v -= a.referenceDb;
    const auto& R = res.smoothedDb;

    const double lo = std::max ({ s.lowHz, a.sweepStartHz, 20.0 }), hi = std::min ({ s.highHz, a.sweepEndHz, 20000.0 });
    std::vector<int> in, out;   // grid points inside the range / up to an octave around it
    for (int i = 0; i < n; ++i)
    {
        if (f[(size_t) i] >= lo && f[(size_t) i] <= hi) in.push_back (i);
        else if (f[(size_t) i] >= lo * 0.5 && f[(size_t) i] <= hi * 2.0) out.push_back (i);
    }
    res.eqDb.assign ((size_t) n, 0.0);
    res.correctedDb = R;
    if (in.size() < 4) return res;

    {
        std::vector<double> v;
        for (int i : in) v.push_back (R[(size_t) i]);
        res.targetDb = median (v) + s.targetOffsetDb;
    }
    const double T = res.targetDb;
    const double sr = s.sampleRate > 0.0 ? s.sampleRate : a.sampleRate;

    std::vector<EqBand> bands;
    std::vector<double> eq ((size_t) n, 0.0);
    auto curve = [&] (const std::vector<EqBand>& bs, std::vector<double>& c, const std::vector<int>& points)
    {
        for (int i : points)
        {
            double v = 0.0;
            for (auto& b : bs) v += bandMagnitudeDb (b, f[(size_t) i], sr);
            c[(size_t) i] = v;
        }
    };
    // Grid points the cost looks at: the range, then the surroundings.
    std::vector<int> idx (in);
    idx.insert (idx.end(), out.begin(), out.end());
    const size_t m = idx.size(), nIn = in.size();
    auto bandCurve = [&] (const EqBand& b, std::vector<double>& c)
    {
        c.resize (m);
        for (size_t j = 0; j < m; ++j) c[j] = bandMagnitudeDb (b, f[(size_t) idx[j]], sr);
    };
    // Cost: squared deviation from the target inside the range (peaks count more than dips, which a cut
    // cannot fill), plus whatever the EQ changes outside the range.
    auto score = [&] (const std::vector<double>& total)
    {
        double sIn = 0.0, sOut = 0.0;
        for (size_t j = 0; j < nIn; ++j)
        {
            const double e = R[(size_t) idx[j]] + total[j] - T;
            sIn += e > 0.0 ? e * e : 0.5 * e * e;
        }
        for (size_t j = nIn; j < m; ++j) sOut += total[j] * total[j];
        return sIn / (double) nIn + (m > nIn ? sOut / (double) (m - nIn) : 0.0);
    };
    auto clampBand = [&] (EqBand& b)
    {
        b.freq = std::clamp (b.freq, std::max (20.0, lo / 1.2), std::min (20000.0, hi * 1.2));
        b.gainDb = std::clamp (b.gainDb, -s.maxCutDb, std::max (0.0, s.maxBoostDb));
        b.q = std::clamp (b.q, 0.3, 20.0);   // narrower filters would only fit this one microphone position
    };
    // Pattern search over frequency, gain and Q of every band (one parameter at a time; only the changed
    // band's curve is recomputed).
    auto optimise = [&] (std::vector<EqBand>& bs, int rounds)
    {
        std::vector<std::vector<double>> bc (bs.size());
        std::vector<double> total (m, 0.0), trialCurve, trialTotal (m);
        for (size_t k = 0; k < bs.size(); ++k)
        {
            bandCurve (bs[k], bc[k]);
            for (size_t j = 0; j < m; ++j) total[j] += bc[k][j];
        }
        double best = score (total);
        std::vector<std::array<double, 3>> step (bs.size(), { std::log (std::pow (2.0, 1.0 / 12.0)), 1.0, 0.25 });
        for (int r = 0; r < rounds; ++r)
        {
            bool improved = false;
            for (size_t k = 0; k < bs.size(); ++k)
                for (int p = 0; p < 3; ++p)
                    for (int dir : { 1, -1 })
                    {
                        auto b = bs[k];
                        if (p == 0)      b.freq *= std::exp (dir * step[k][0]);
                        else if (p == 1) b.gainDb += dir * step[k][1];
                        else             b.q *= std::exp (dir * step[k][2]);
                        clampBand (b);
                        bandCurve (b, trialCurve);
                        for (size_t j = 0; j < m; ++j) trialTotal[j] = total[j] - bc[k][j] + trialCurve[j];
                        const double c = score (trialTotal);
                        if (c < best - 1.0e-9)
                        {
                            best = c;
                            bs[k] = b;
                            std::swap (bc[k], trialCurve);
                            std::swap (total, trialTotal);
                            improved = true;
                            break;
                        }
                    }
            if (! improved)
            {
                bool tiny = true;
                for (auto& st : step) { for (auto& v : st) v *= 0.5; tiny = tiny && st[1] < 0.02; }
                if (tiny) break;
            }
        }
    };

    for (int k = 0; k < std::clamp (s.maxBands, 0, 8); ++k)
    {
        curve (bands, eq, in);
        // Worst deviation: peaks above the target (dips only if boosting is allowed, and weighted less).
        int at = -1;
        double worst = 0.0;
        for (int i : in)
        {
            const double e = R[(size_t) i] + eq[(size_t) i] - T;
            const double weight = e > 0.0 ? e : (s.maxBoostDb > 0.0 ? -0.5 * e : 0.0);
            if (weight > worst) { worst = weight; at = i; }
        }
        if (at < 0 || worst < 0.75) break;

        const double e0 = R[(size_t) at] + eq[(size_t) at] - T;
        int l = at, h = at;
        auto err = [&] (int i) { return R[(size_t) i] + eq[(size_t) i] - T; };
        while (l > in.front() && std::abs (err (l)) > std::abs (e0) * 0.5 && (err (l) > 0.0) == (e0 > 0.0)) --l;
        while (h < in.back() && std::abs (err (h)) > std::abs (e0) * 0.5 && (err (h) > 0.0) == (e0 > 0.0)) ++h;
        const double bw = f[(size_t) h] - f[(size_t) l];
        EqBand b;
        b.freq = f[(size_t) at];
        b.gainDb = -e0;
        b.q = bw > 0.0 ? f[(size_t) at] / bw : 8.0;
        clampBand (b);
        bands.push_back (b);
        optimise (bands, 60);
    }
    optimise (bands, 200);
    bands.erase (std::remove_if (bands.begin(), bands.end(), [] (const EqBand& b) { return std::abs (b.gainDb) < 0.3; }), bands.end());
    std::sort (bands.begin(), bands.end(), [] (const EqBand& x, const EqBand& y) { return x.freq < y.freq; });
    for (auto& b : bands)   // the EQ's parameter resolution
    {
        b.gainDb = std::round (b.gainDb * 10.0) / 10.0;
        b.freq = std::round (b.freq * 10.0) / 10.0;
        b.q = std::round (b.q * 100.0) / 100.0;
    }
    res.bands = bands;

    std::vector<int> all ((size_t) n);
    std::iota (all.begin(), all.end(), 0);
    curve (bands, res.eqDb, all);
    double before = 0.0, after = 0.0;
    for (int i = 0; i < n; ++i) res.correctedDb[(size_t) i] = R[(size_t) i] + res.eqDb[(size_t) i];
    for (int i : in)
    {
        before += (R[(size_t) i] - T) * (R[(size_t) i] - T);
        after += (res.correctedDb[(size_t) i] - T) * (res.correctedDb[(size_t) i] - T);
        res.peakBeforeDb = std::max (res.peakBeforeDb, R[(size_t) i] - T);
        res.peakAfterDb = std::max (res.peakAfterDb, res.correctedDb[(size_t) i] - T);
    }
    res.rmsBeforeDb = std::sqrt (before / (double) in.size());
    res.rmsAfterDb = std::sqrt (after / (double) in.size());
    return res;
}
} // namespace hydra::room

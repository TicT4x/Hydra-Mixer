#include "KeyDetect.h"
#include <juce_dsp/juce_dsp.h>
#include <complex>

namespace hydra::dsp
{
namespace
{
constexpr double kTwoPi = juce::MathConstants<double>::twoPi;
inline double princarg (double a) noexcept { return a - kTwoPi * std::floor ((a + juce::MathConstants<double>::pi) / kTwoPi); }

// Krumhansl & Kessler (1982), tonic first.
constexpr float kMajor[12] = { 6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f };
constexpr float kMinor[12] = { 6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f };

float correlate (const std::array<float, 12>& chroma, const float* profile, int tonic)
{
    float mx = 0, my = 0;
    for (int i = 0; i < 12; ++i) { mx += chroma[(size_t) i]; my += profile[i]; }
    mx /= 12; my /= 12;
    float sxy = 0, sxx = 0, syy = 0;
    for (int i = 0; i < 12; ++i)
    {
        const float x = chroma[(size_t) ((i + tonic) % 12)] - mx, y = profile[i] - my;
        sxy += x * y; sxx += x * x; syy += y * y;
    }
    return sxx > 0 && syy > 0 ? sxy / std::sqrt (sxx * syy) : 0.0f;
}

struct Vote { float cents; float weight; };
} // namespace

std::array<int, 7> scaleOf (int key)
{
    static const int major[] = { 0, 2, 4, 5, 7, 9, 11 }, minor[] = { 0, 2, 3, 5, 7, 8, 10 };
    std::array<int, 7> s {};
    for (int i = 0; i < 7; ++i) s[(size_t) i] = ((key % 12) + (key >= 12 ? minor[i] : major[i])) % 12;
    return s;
}

juce::String keyName (int key, int shift)
{
    if (key < 0 || key >= 24) return {};
    static const char* names[] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
    const int pc = (((key % 12) + shift) % 12 + 12) % 12;
    return juce::String (names[pc]) + (key < 12 ? " major" : " minor");
}

KeyResult analyseKey (const float* mono, int numSamples, double sampleRate)
{
    KeyResult res;
    const int n = juce::nextPowerOfTwo (juce::roundToInt (sampleRate * 0.17));   // 8192 at 48 kHz: ~6 Hz bins
    const int d = n / 8;                                                           // second FFT for the true frequency
    if (numSamples < n + d + 1) return res;
    juce::dsp::FFT fft (juce::roundToInt (std::log2 ((double) n)));
    std::vector<float> win ((size_t) n), a ((size_t) n * 2), b ((size_t) n * 2), mag ((size_t) n / 2 + 1);
    for (int i = 0; i < n; ++i) win[(size_t) i] = (float) (0.5 - 0.5 * std::cos (kTwoPi * i / n));

    std::vector<Vote> votes;
    const int lo = juce::jmax (2, (int) std::ceil (80.0 * n / sampleRate));
    const int hi = juce::jmin (n / 2 - 2, (int) (5000.0 * n / sampleRate));
    for (int start = 0; start + n + d <= numSamples; start += n / 2)
    {
        for (int i = 0; i < n; ++i)
        {
            a[(size_t) i] = mono[start + i] * win[(size_t) i];
            b[(size_t) i] = mono[start + d + i] * win[(size_t) i];
        }
        fft.performRealOnlyForwardTransform (a.data(), true);
        fft.performRealOnlyForwardTransform (b.data(), true);
        float maxMag = 0.0f;
        for (int k = lo - 2; k <= hi + 2; ++k)
        {
            mag[(size_t) k] = std::sqrt (a[(size_t) (2 * k)] * a[(size_t) (2 * k)] + a[(size_t) (2 * k + 1)] * a[(size_t) (2 * k + 1)]);
            maxMag = juce::jmax (maxMag, mag[(size_t) k]);
        }
        if (maxMag < 1.0e-4f) continue;   // silence
        const float floor = maxMag * 0.01f;   // peaks within 40 dB of the loudest
        for (int k = lo; k <= hi; ++k)
        {
            const float m = mag[(size_t) k];
            if (m < floor || m <= mag[(size_t) k - 1] || m < mag[(size_t) k + 1] || m <= mag[(size_t) k - 2] || m < mag[(size_t) k + 2]) continue;
            const std::complex<double> x (a[(size_t) (2 * k)], a[(size_t) (2 * k + 1)]), y (b[(size_t) (2 * k)], b[(size_t) (2 * k + 1)]);
            const double expected = kTwoPi * k * d / n;
            const double dev = princarg (std::arg (y * std::conj (x)) - expected);
            const double freq = (k + dev * n / (kTwoPi * d)) * sampleRate / n;
            if (freq < 60.0) continue;
            votes.push_back ({ (float) (1200.0 * std::log2 (freq / 440.0)), m / maxMag });
        }
    }
    if (votes.size() < 50) return res;

    // Tuning: circular mean of the distance to the nearest note (period 100 cents).
    std::complex<double> sum;
    double total = 0.0;
    for (auto& v : votes)
    {
        const double frac = v.cents - 100.0 * std::round (v.cents / 100.0);
        sum += (double) v.weight * std::polar (1.0, kTwoPi * frac / 100.0);
        total += v.weight;
    }
    res.cents = (float) (std::arg (sum) * 100.0 / kTwoPi);
    res.tuningStrength = (float) (std::abs (sum) / juce::jmax (1.0e-9, total));

    // Key: pitch classes of the peaks, in tune after the offset.
    std::array<float, 12> chroma {};
    for (auto& v : votes)
    {
        const int semisFromA = (int) std::lround ((v.cents - res.cents) / 100.0);
        chroma[(size_t) (((semisFromA + 9) % 12 + 12) % 12)] += v.weight;   // A = pitch class 9
    }
    const float mx = *std::max_element (chroma.begin(), chroma.end());
    if (mx <= 0.0f) return res;
    for (auto& c : chroma) c /= mx;
    res.chroma = chroma;
    rankKeys (res);
    res.valid = true;
    return res;
}

void rankKeys (KeyResult& res)
{
    std::array<float, 24> r {};
    for (int t = 0; t < 12; ++t)
    {
        r[(size_t) t] = correlate (res.chroma, kMajor, t);
        r[(size_t) (12 + t)] = correlate (res.chroma, kMinor, t);
    }
    int best = 0, second = 1;
    if (r[1] > r[0]) std::swap (best, second);
    for (int k = 2; k < 24; ++k)
    {
        if (r[(size_t) k] > r[(size_t) best]) { second = best; best = k; }
        else if (r[(size_t) k] > r[(size_t) second]) second = k;
    }
    // Probabilities: softmax of the correlations (kKeySharpness: a lead of 0.1 in correlation = 4.5 times as likely).
    double sum = 0.0;
    for (float c : r) sum += std::exp (kKeySharpness * (c - r[(size_t) best]));
    res.key = best;
    res.key2 = second;
    res.keyStrength = juce::jmax (0.0f, r[(size_t) best]);
    res.probability = (float) (1.0 / sum);
    res.probability2 = (float) (std::exp (kKeySharpness * (r[(size_t) second] - r[(size_t) best])) / sum);
}
} // namespace hydra::dsp

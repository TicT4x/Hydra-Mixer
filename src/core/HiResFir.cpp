#include "HiResFir.h"
#include <complex>

namespace hydra::dsp
{
HiResFir::HiResFir()
{
    for (auto& c : chans) c.fdl.assign ((size_t) kMaxParts * 2 * kBins, 0.0f);
    work.assign (4 * kBlock, 0.0f);
    acc.assign (2 * kBins, 0.0f);
}

HiResFir::~HiResFir()
{
    auto drop = [] (Filter* f) { if (f != nullptr && f != offMarker()) delete f; };
    drop (pending.exchange (nullptr));
    drop (retired.exchange (nullptr));
    if (prev != cur) drop (prev);
    drop (cur);
}

HiResFir::Filter* HiResFir::offMarker() noexcept
{
    static Filter marker;
    return &marker;
}

namespace
{
void designSide (HiResFir::Side& out, const std::function<double (double)>& dbAt, double sr, int taps)
{
    constexpr int B = HiResFir::kBlock, bins = HiResFir::kBins;
    const int M = taps * 4;
    const int order = juce::roundToInt (std::log2 ((double) M));
    juce::dsp::FFT fft (order);
    std::vector<std::complex<float>> a ((size_t) M), b ((size_t) M);
    // Real cepstrum of the log magnitude, folded = minimum phase (Oppenheim & Schafer).
    const double toNeper = std::log (10.0) / 20.0;
    for (int k = 0; k <= M / 2; ++k)
    {
        const float lm = (float) (dbAt (k * sr / M) * toNeper);
        a[(size_t) k] = lm;
        if (k > 0 && k < M / 2) a[(size_t) (M - k)] = lm;
    }
    fft.perform (a.data(), b.data(), true);
    std::fill (a.begin(), a.end(), std::complex<float> {});
    a[0] = b[0].real();
    for (int n = 1; n < M / 2; ++n) a[(size_t) n] = 2.0f * b[(size_t) n].real();
    a[(size_t) (M / 2)] = b[(size_t) (M / 2)].real();
    fft.perform (a.data(), b.data(), false);
    for (auto& v : b) v = std::exp (v);
    fft.perform (b.data(), a.data(), true);

    out.taps.resize ((size_t) taps);
    const int fade = taps / 8;
    for (int n = 0; n < taps; ++n)
    {
        float w = 1.0f;
        if (n >= taps - fade) w = 0.5f + 0.5f * std::cos (juce::MathConstants<float>::pi * (float) (n - (taps - fade)) / (float) fade);
        out.taps[(size_t) n] = a[(size_t) n].real() * w;
    }
    const int parts = taps / B;
    out.h0rev.resize (B);
    for (int j = 0; j < B; ++j) out.h0rev[(size_t) j] = out.taps[(size_t) (B - 1 - j)];
    out.spectra.assign ((size_t) (parts - 1) * 2 * bins, 0.0f);
    juce::dsp::FFT small (8);
    std::vector<float> w (4 * B);
    for (int p = 1; p < parts; ++p)
    {
        std::fill (w.begin(), w.end(), 0.0f);
        std::copy (out.taps.begin() + p * B, out.taps.begin() + (p + 1) * B, w.begin());
        small.performRealOnlyForwardTransform (w.data(), true);
        std::copy (w.begin(), w.begin() + 2 * bins, out.spectra.begin() + (p - 1) * 2 * bins);
    }
}
} // namespace

std::unique_ptr<HiResFir::Filter> HiResFir::design (const std::function<double (double)>& dbAt, double sr,
                                                    const std::function<double (double)>& dbAt2, bool midSide)
{
    const int taps = sr > 50000.0 ? 16384 : 8192;
    auto f = std::make_unique<Filter>();
    f->parts = taps / kBlock;
    f->midSide = midSide;
    designSide (f->side[0], dbAt, sr, taps);
    if (dbAt2) designSide (f->side[1], dbAt2, sr, taps);
    else f->side[1] = f->side[0];
    return f;
}

void HiResFir::setFilter (std::unique_ptr<Filter> f)
{
    Filter* next = f != nullptr ? f.release() : offMarker();
    if (auto* old = pending.exchange (next); old != nullptr && old != offMarker()) delete old;
}

void HiResFir::collect()
{
    if (auto* r = retired.exchange (nullptr)) delete r;
}

void HiResFir::clearState() noexcept
{
    for (auto& c : chans)
    {
        std::fill (std::begin (c.hist), std::end (c.hist), 0.0f);
        std::fill (std::begin (c.frame), std::end (c.frame), 0.0f);
        std::fill (std::begin (c.tailNew), std::end (c.tailNew), 0.0f);
        std::fill (std::begin (c.tailOld), std::end (c.tailOld), 0.0f);
        std::fill (c.fdl.begin(), c.fdl.end(), 0.0f);
    }
    pos = hpos = fdlHead = 0;
}

void HiResFir::reset() noexcept { clearState(); }

void HiResFir::computeTail (const Filter& f, int side, Chan& c, float* out) noexcept
{
    std::fill (acc.begin(), acc.end(), 0.0f);
    for (int p = 1; p < f.parts; ++p)
    {
        const int slot = (fdlHead - (p - 1) + kMaxParts) % kMaxParts;
        const float* X = c.fdl.data() + (size_t) slot * 2 * kBins;
        const float* H = f.side[side].spectra.data() + (size_t) (p - 1) * 2 * kBins;
        float* A = acc.data();
        for (int k = 0; k < kBins; ++k)
        {
            const float xr = X[2 * k], xi = X[2 * k + 1], hr = H[2 * k], hi = H[2 * k + 1];
            A[2 * k]     += xr * hr - xi * hi;
            A[2 * k + 1] += xr * hi + xi * hr;
        }
    }
    std::copy (acc.begin(), acc.end(), work.begin());
    std::fill (work.begin() + 2 * kBins, work.end(), 0.0f);
    fft.performRealOnlyInverseTransform (work.data());
    std::copy (work.begin() + kBlock, work.begin() + 2 * kBlock, out);
}

void HiResFir::beginBlock() noexcept
{
    // 1. The spectrum of the block that just ended (with the one before: overlap-save frame).
    fdlHead = (fdlHead + 1) % kMaxParts;
    for (auto& c : chans)
    {
        std::copy (std::begin (c.frame), std::end (c.frame), work.begin());
        std::fill (work.begin() + 2 * kBlock, work.end(), 0.0f);
        fft.performRealOnlyForwardTransform (work.data(), true);
        std::copy (work.begin(), work.begin() + 2 * kBins, c.fdl.begin() + (size_t) fdlHead * 2 * kBins);
        std::copy (c.frame + kBlock, c.frame + 2 * kBlock, c.frame);
    }
    // 2. A new filter: the previous fade is over; hand its old filter back, then take the next one.
    fading = false;
    if (prev != nullptr)
    {
        Filter* none = nullptr;
        if (retired.compare_exchange_strong (none, prev, std::memory_order_acq_rel)) prev = nullptr;
    }
    if (prev == nullptr)
        if (Filter* p = pending.load (std::memory_order_acquire))
        {
            Filter* next = p == offMarker() ? nullptr : p;
            if (next != nullptr && cur != nullptr && next->midSide != domainMidSide)
            {
                // Left / right <-> mid / side: fade out to dry first, the new filter comes in the next block.
                prev = cur;
                cur = nullptr;
                fading = true;
            }
            else if (pending.compare_exchange_strong (p, nullptr, std::memory_order_acq_rel))
            {
                if (next != cur)
                {
                    if (cur == nullptr && next != nullptr && next->midSide != domainMidSide) clearState();   // history of the other kind
                    prev = cur;
                    cur = next;
                    fading = true;
                }
                if (cur != nullptr) domainMidSide = cur->midSide;
            }
        }
    // 3. What the partitions after the first add to this block.
    for (int ci = 0; ci < 2; ++ci)
    {
        auto& c = chans[ci];
        if (cur != nullptr) computeTail (*cur, ci, c, c.tailNew);
        if (fading && prev != nullptr) computeTail (*prev, ci, c, c.tailOld);
    }
}

void HiResFir::process (float* l, float* r, int n) noexcept
{
    if (idle)
    {
        if (prev != nullptr)
        {
            Filter* none = nullptr;
            if (! retired.compare_exchange_strong (none, prev, std::memory_order_acq_rel)) return;
            prev = nullptr;
        }
        Filter* p = pending.load (std::memory_order_acquire);
        if (p == nullptr) return;
        if (p == offMarker()) { pending.compare_exchange_strong (p, nullptr); return; }
        clearState();   // starts from silence: the filter fades in from the dry signal
        domainMidSide = p->midSide;
        idle = false;
    }
    float* io[2] = { l, r };
    int i = 0;
    while (i < n)
    {
        if (pos == 0)
        {
            beginBlock();
            if (cur == nullptr && ! fading) { idle = true; return; }   // switched off: the rest stays dry
        }
        const int run = juce::jmin (n - i, kBlock - pos);
        if (domainMidSide)
            for (int s = 0; s < run; ++s) { const float a = l[i + s], b = r[i + s]; l[i + s] = 0.5f * (a + b); r[i + s] = 0.5f * (a - b); }
        int hp = hpos;
        for (int c = 0; c < 2; ++c)
        {
            auto& ch = chans[c];
            float* x = io[c] + i;
            hp = hpos;
            const float* hNew = cur != nullptr ? cur->side[c].h0rev.data() : nullptr;
            const float* hOld = fading && prev != nullptr ? prev->side[c].h0rev.data() : nullptr;
            for (int s = 0; s < run; ++s)
            {
                const float in = x[s];
                ch.frame[kBlock + pos + s] = in;
                ch.hist[hp] = in;
                ch.hist[hp + kBlock] = in;
                const float* win = ch.hist + hp + 1;
                float yNew = in;
                if (hNew != nullptr)
                {
                    float a = 0.0f;
                    for (int j = 0; j < kBlock; ++j) a += hNew[j] * win[j];
                    yNew = ch.tailNew[pos + s] + a;
                }
                float y = yNew;
                if (fading)
                {
                    float yOld = in;
                    if (hOld != nullptr)
                    {
                        float a = 0.0f;
                        for (int j = 0; j < kBlock; ++j) a += hOld[j] * win[j];
                        yOld = ch.tailOld[pos + s] + a;
                    }
                    const float t = (float) (pos + s + 1) / (float) kBlock;
                    y = yOld + t * (yNew - yOld);
                }
                x[s] = y;
                hp = hp + 1 == kBlock ? 0 : hp + 1;
            }
        }
        hpos = hp;
        if (domainMidSide)
            for (int s = 0; s < run; ++s) { const float m = l[i + s], d = r[i + s]; l[i + s] = m + d; r[i + s] = m - d; }
        pos += run;
        i += run;
        if (pos == kBlock) pos = 0;
    }
}
} // namespace hydra::dsp

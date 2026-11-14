#include "HiResFir.h"
#include <complex>
#if defined (__AVX2__)
 #include <immintrin.h>
#endif

namespace hydra::dsp
{
HiResFir::HiResFir()
{
    for (auto& c : chans) c.fdl.assign ((size_t) kMaxParts * 2 * kBins, 0.0f);
    cin.assign (2 * kBlock, {});
    cout.assign (2 * kBlock, {});
    acc.assign (4 * kBins, 0.0f);
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
/** y[s] += sum_j h[j] * x[s + j] for s < run, j < kBlock (the first kBlock taps). Vectorised over the outputs, 32 of
    them in registers, instead of one dot product per sample whose additions form one long dependency chain. */
void directPart (const float* h, const float* x, float* y, int run) noexcept
{
    constexpr int B = HiResFir::kBlock;
    int s = 0;
#if defined (__AVX2__)
    for (; s + 32 <= run; s += 32)
    {
        __m256 a0 = _mm256_loadu_ps (y + s), a1 = _mm256_loadu_ps (y + s + 8);
        __m256 a2 = _mm256_loadu_ps (y + s + 16), a3 = _mm256_loadu_ps (y + s + 24);
        const float* xs = x + s;
        for (int j = 0; j < B; ++j)
        {
            const __m256 c = _mm256_broadcast_ss (h + j);
            a0 = _mm256_fmadd_ps (c, _mm256_loadu_ps (xs + j), a0);
            a1 = _mm256_fmadd_ps (c, _mm256_loadu_ps (xs + j + 8), a1);
            a2 = _mm256_fmadd_ps (c, _mm256_loadu_ps (xs + j + 16), a2);
            a3 = _mm256_fmadd_ps (c, _mm256_loadu_ps (xs + j + 24), a3);
        }
        _mm256_storeu_ps (y + s, a0);
        _mm256_storeu_ps (y + s + 8, a1);
        _mm256_storeu_ps (y + s + 16, a2);
        _mm256_storeu_ps (y + s + 24, a3);
    }
    for (; s + 8 <= run; s += 8)
    {
        __m256 a = _mm256_loadu_ps (y + s);
        for (int j = 0; j < B; ++j) a = _mm256_fmadd_ps (_mm256_broadcast_ss (h + j), _mm256_loadu_ps (x + s + j), a);
        _mm256_storeu_ps (y + s, a);
    }
#endif
    for (; s < run; ++s)
    {
        float a = 0.0f;
        for (int j = 0; j < B; ++j) a += h[j] * x[s + j];
        y[s] += a;
    }
}

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
        // Split layout (all real parts, then all imaginary parts): the multiply-add in computeTails vectorises.
        float* dst = out.spectra.data() + (size_t) (p - 1) * 2 * bins;
        for (int k = 0; k < bins; ++k) { dst[k] = w[(size_t) (2 * k)]; dst[bins + k] = w[(size_t) (2 * k + 1)]; }
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
    f->linked = ! dbAt2;
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
        std::fill (std::begin (c.frame), std::end (c.frame), 0.0f);
        std::fill (std::begin (c.tailNew), std::end (c.tailNew), 0.0f);
        std::fill (std::begin (c.tailOld), std::end (c.tailOld), 0.0f);
        std::fill (c.fdl.begin(), c.fdl.end(), 0.0f);
    }
    pos = fdlHead = 0;
}

void HiResFir::reset() noexcept { clearState(); }

void HiResFir::computeTails (const Filter& f, float* out0, float* out1) noexcept
{
    // Frequency-domain multiply-add of the partitions after the first (split re / im layout). Both channels in one
    // pass; a linked filter (the same for both) reads its spectra only once.
    float* acc0 = acc.data();
    float* acc1 = acc.data() + 2 * kBins;
    const float* X0[kMaxParts];
    const float* X1[kMaxParts];
    const float* H0[kMaxParts];
    const float* H1[kMaxParts];
    const int P = f.parts - 1;
    for (int p = 0; p < P; ++p)
    {
        const int slot = (fdlHead - p + kMaxParts) % kMaxParts;
        X0[p] = chans[0].fdl.data() + (size_t) slot * 2 * kBins;
        X1[p] = chans[1].fdl.data() + (size_t) slot * 2 * kBins;
        H0[p] = f.side[0].spectra.data() + (size_t) p * 2 * kBins;
        H1[p] = f.linked ? H0[p] : f.side[1].spectra.data() + (size_t) p * 2 * kBins;
    }
    int k = 0;
#if defined (__AVX2__)
    // Eight bins at a time with all partitions inside: the sums stay in registers.
    for (; k + 8 <= kBins; k += 8)
    {
        __m256 r0 = _mm256_setzero_ps(), i0 = r0, r1 = r0, i1 = r0;
        for (int p = 0; p < P; ++p)
        {
            const __m256 hr0 = _mm256_loadu_ps (H0[p] + k), hi0 = _mm256_loadu_ps (H0[p] + kBins + k);
            const __m256 hr1 = _mm256_loadu_ps (H1[p] + k), hi1 = _mm256_loadu_ps (H1[p] + kBins + k);
            const __m256 xr0 = _mm256_loadu_ps (X0[p] + k), xi0 = _mm256_loadu_ps (X0[p] + kBins + k);
            const __m256 xr1 = _mm256_loadu_ps (X1[p] + k), xi1 = _mm256_loadu_ps (X1[p] + kBins + k);
            r0 = _mm256_fmadd_ps (xr0, hr0, _mm256_fnmadd_ps (xi0, hi0, r0));
            i0 = _mm256_fmadd_ps (xr0, hi0, _mm256_fmadd_ps (xi0, hr0, i0));
            r1 = _mm256_fmadd_ps (xr1, hr1, _mm256_fnmadd_ps (xi1, hi1, r1));
            i1 = _mm256_fmadd_ps (xr1, hi1, _mm256_fmadd_ps (xi1, hr1, i1));
        }
        _mm256_storeu_ps (acc0 + k, r0);
        _mm256_storeu_ps (acc0 + kBins + k, i0);
        _mm256_storeu_ps (acc1 + k, r1);
        _mm256_storeu_ps (acc1 + kBins + k, i1);
    }
#endif
    for (; k < kBins; ++k)
    {
        float r0 = 0.0f, i0 = 0.0f, r1 = 0.0f, i1 = 0.0f;
        for (int p = 0; p < P; ++p)
        {
            const float hr0 = H0[p][k], hi0 = H0[p][kBins + k], hr1 = H1[p][k], hi1 = H1[p][kBins + k];
            const float xr0 = X0[p][k], xi0 = X0[p][kBins + k], xr1 = X1[p][k], xi1 = X1[p][kBins + k];
            r0 += xr0 * hr0 - xi0 * hi0;
            i0 += xr0 * hi0 + xi0 * hr0;
            r1 += xr1 * hr1 - xi1 * hi1;
            i1 += xr1 * hi1 + xi1 * hr1;
        }
        acc0[k] = r0; acc0[kBins + k] = i0; acc1[k] = r1; acc1[kBins + k] = i1;
    }
    // One complex inverse FFT for both: Z = Y0 + i Y1 with both half spectra extended conjugate-symmetric; the real
    // part is channel 0, the imaginary part channel 1. Bins 0 and N/2 of a real signal are real (JUCE's real-only
    // inverse ignores their imaginary parts too), so only their real parts are used.
    constexpr int N = 2 * kBlock;
    cin[0] = { acc0[0], acc1[0] };
    cin[(size_t) kBlock] = { acc0[kBlock], acc1[kBlock] };
    for (int k = 1; k < kBlock; ++k)
    {
        const float r0 = acc0[k], i0 = acc0[kBins + k], r1 = acc1[k], i1 = acc1[kBins + k];
        cin[(size_t) k]       = { r0 - i1, i0 + r1 };   // Y0 + i Y1
        cin[(size_t) (N - k)] = { r0 + i1, r1 - i0 };   // conj (Y0) + i conj (Y1)
    }
    fft.perform (cin.data(), cout.data(), true);
    for (int j = 0; j < kBlock; ++j)
    {
        out0[j] = cout[(size_t) (kBlock + j)].real();
        out1[j] = cout[(size_t) (kBlock + j)].imag();
    }
}

void HiResFir::beginBlock() noexcept
{
    // 1. The spectrum of the block that just ended (with the one before: overlap-save frame).
    // Both channels in one complex FFT (left = real, right = imaginary part), separated by symmetry:
    // X0[k] = (Z[k] + conj Z[N-k]) / 2, X1[k] = (Z[k] - conj Z[N-k]) / 2i.
    fdlHead = (fdlHead + 1) % kMaxParts;
    constexpr int N = 2 * kBlock;
    for (int j = 0; j < N; ++j) cin[(size_t) j] = { chans[0].frame[j], chans[1].frame[j] };
    fft.perform (cin.data(), cout.data(), false);
    float* d0 = chans[0].fdl.data() + (size_t) fdlHead * 2 * kBins;   // split re / im (see computeTails)
    float* d1 = chans[1].fdl.data() + (size_t) fdlHead * 2 * kBins;
    for (int k = 0; k < kBins; ++k)
    {
        const auto z = cout[(size_t) k], m = cout[(size_t) ((N - k) % N)];
        d0[k] = 0.5f * (z.real() + m.real());
        d0[kBins + k] = 0.5f * (z.imag() - m.imag());
        d1[k] = 0.5f * (z.imag() + m.imag());
        d1[kBins + k] = 0.5f * (m.real() - z.real());
    }
    for (auto& c : chans) std::copy (c.frame + kBlock, c.frame + 2 * kBlock, c.frame);
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
    if (cur != nullptr) computeTails (*cur, chans[0].tailNew, chans[1].tailNew);
    if (fading && prev != nullptr) computeTails (*prev, chans[0].tailOld, chans[1].tailOld);
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
        for (int c = 0; c < 2; ++c)
        {
            auto& ch = chans[c];
            float* x = io[c] + i;
            std::copy (x, x + run, ch.frame + kBlock + pos);
            // Output s reads frame[pos + 1 + s .. pos + kBlock + s]: the last kBlock inputs, oldest first (h0rev).
            const float* window = ch.frame + pos + 1;
            float yNew[kBlock], yOld[kBlock];
            if (cur != nullptr)
            {
                std::copy (ch.tailNew + pos, ch.tailNew + pos + run, yNew);
                directPart (cur->side[c].h0rev.data(), window, yNew, run);
            }
            else std::copy (x, x + run, yNew);
            if (fading)
            {
                if (prev != nullptr)
                {
                    std::copy (ch.tailOld + pos, ch.tailOld + pos + run, yOld);
                    directPart (prev->side[c].h0rev.data(), window, yOld, run);
                }
                else std::copy (x, x + run, yOld);
                for (int s = 0; s < run; ++s)
                {
                    const float t = (float) (pos + s + 1) / (float) kBlock;
                    x[s] = yOld[s] + t * (yNew[s] - yOld[s]);
                }
            }
            else std::copy (yNew, yNew + run, x);
        }
        if (domainMidSide)
            for (int s = 0; s < run; ++s) { const float m = l[i + s], d = r[i + s]; l[i + s] = m + d; r[i + s] = m - d; }
        pos += run;
        i += run;
        if (pos == kBlock) pos = 0;
    }
}
} // namespace hydra::dsp

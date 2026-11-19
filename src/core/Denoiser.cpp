#include "Denoiser.h"
#include <rnnoise.h>
#include <immintrin.h>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_set>
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <avrt.h>
#pragma comment (lib, "avrt.lib")

namespace hydra::dsp
{
namespace
{
/** The one thread that computes the RNNoise frames of all denoisers (see Denoiser). Woken per frame by the audio
    thread (one SetEvent every 10 ms per denoiser in use), MMCSS "Pro Audio" high: below the engine's audio thread
    (critical), above everything else. Sleeps without frames. */
class WorkerPool
{
public:
    static WorkerPool& get() { static WorkerPool p; return p; }

    void add (Denoiser* d)
    {
        std::lock_guard<std::mutex> l (lock);
        known.insert (d);
        if (! worker.joinable()) worker = std::thread ([this] { run(); });
    }
    /** Message thread (destructor): after this the worker never touches d again. */
    void remove (Denoiser* d)
    {
        std::unique_lock<std::mutex> l (lock);
        known.erase (d);
        idleAgain.wait (l, [&] { return busy != d; });
    }
    /** Audio thread: no lock, no allocation; one SetEvent. */
    void submit (Denoiser* d) noexcept
    {
        const uint32_t i = writeIndex.fetch_add (1, std::memory_order_relaxed);
        slots[i & kMask].store (d, std::memory_order_release);
        SetEvent (wake);
    }

private:
    static constexpr uint32_t kSlots = 256, kMask = kSlots - 1;   // a denoiser has at most one frame queued

    WorkerPool() : wake (CreateEventW (nullptr, FALSE, FALSE, nullptr)) {}
    ~WorkerPool()
    {
        stopping = true;
        SetEvent (wake);
        if (worker.joinable()) worker.join();
        CloseHandle (wake);
    }

    void run()
    {
        DWORD task = 0;
        if (HANDLE mmcss = AvSetMmThreadCharacteristicsW (L"Pro Audio", &task)) AvSetMmThreadPriority (mmcss, AVRT_PRIORITY_HIGH);
        else SetThreadPriority (GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        while (! stopping)
        {
            WaitForSingleObject (wake, 200);
            for (;;)
            {
                Denoiser* d = slots[readIndex & kMask].exchange (nullptr, std::memory_order_acquire);
                if (d == nullptr) break;   // empty (or the producer has not stored it yet: it signals again)
                ++readIndex;
                {
                    std::lock_guard<std::mutex> l (lock);
                    if (known.count (d) == 0 || ! d->claimJob()) continue;   // gone, or the audio thread took it back
                    busy = d;
                }
                d->runJob();
                {
                    std::lock_guard<std::mutex> l (lock);
                    busy = nullptr;
                }
                idleAgain.notify_all();
            }
        }
    }

    std::atomic<Denoiser*> slots[kSlots] {};
    std::atomic<uint32_t> writeIndex { 0 };
    uint32_t readIndex = 0;   // worker
    HANDLE wake;
    std::atomic<bool> stopping { false };
    std::mutex lock;
    std::condition_variable idleAgain;
    std::unordered_set<Denoiser*> known;
    Denoiser* busy = nullptr;
    std::thread worker;
};
} // namespace
void Denoiser::Channel::clear() noexcept
{
    if (st != nullptr) rnnoise_init (st, nullptr);
    std::fill (std::begin (fifo), std::end (fifo), 0.0f);
    std::fill (std::begin (jobIn), std::end (jobIn), 0.0f);
    std::fill (std::begin (jobOut), std::end (jobOut), 0.0f);
    std::fill (std::begin (nativeOut), std::end (nativeOut), 0.0f);
    std::fill (delay.begin(), delay.end(), 0.0f);
    std::fill (std::begin (z1), std::end (z1), 0.0);
    std::fill (std::begin (z2), std::end (z2), 0.0);
    std::fill (std::begin (w), std::end (w), 0.0);
    std::fill (std::begin (wStep), std::end (wStep), 0.0);
    wLast = 1.0; wLastStep = 0.0; rampLeft = 0;
}

Denoiser::Denoiser()
{
    jassert (rnnoise_get_frame_size() == kFrame);
    jassert (RNNOISE_NB_GAIN_BANDS == kBands);
    for (auto& c : ch) c.st = rnnoise_create (nullptr);
    prepare (48000.0);
    WorkerPool::get().add (this);
}

Denoiser::~Denoiser()
{
    WorkerPool::get().remove (this);
    settleJob();
    for (auto& c : ch)
        if (c.st != nullptr) rnnoise_destroy (c.st);
}

void Denoiser::configure (double sampleRate)
{
    rate = sampleRate >= 1000.0 ? sampleRate : 48000.0;
    resampled = std::abs (rate - 48000.0) >= 1.0;   // the RNNoise model is 48 kHz only
    const double ratio = resampled ? rate / 48000.0 : 1.0;   // audio samples per model sample

    // Crossovers halfway (geometrically) between RNNoise's band centres; 2nd-order Butterworth lowpasses.
    for (int j = 0; j < kXover; ++j)
    {
        const double fc = std::sqrt ((double) rnnoise_band_centre_hz (j) * rnnoise_band_centre_hz (j + 1));
        if (fc >= 0.45 * rate)   // above what this rate can carry: passes everything, the sum stays exact
        {
            b0[j] = 1.0;
            b1[j] = b2[j] = a1[j] = a2[j] = 0.0;
            continue;
        }
        const double w0 = juce::MathConstants<double>::twoPi * fc / rate;
        const double cw = std::cos (w0), alpha = std::sin (w0) / (2.0 * 0.70710678);
        const double a0 = 1.0 + alpha;
        b0[j] = (1.0 - cw) * 0.5 / a0;
        b1[j] = (1.0 - cw) / a0;
        b2[j] = b0[j];
        a1[j] = -2.0 * cw / a0;
        a2[j] = (1.0 - alpha) / a0;
    }
    for (int j = kXover; j < kPad; ++j) b0[j] = b1[j] = b2[j] = a1[j] = a2[j] = 0.0;   // padding: outputs 0, weight 0

    ramp = juce::jmax (8, juce::roundToInt (0.002 * rate));   // 2 ms gain interpolation
    rampInv = 1.0 / (double) ramp;

    if (resampled)
    {
        // Filter length follows the lower of the two rates, so the anti-aliasing is equally good at every rate.
        toModel.prepare (2, ratio, kChunk * 2, (int) std::ceil (16.0 * juce::jmax (1.0, ratio)));
        fromModel.prepare (2, 1.0 / ratio, kFrame * 2, (int) std::ceil (16.0 * juce::jmax (1.0, 1.0 / ratio)));
        const int inDelay = toModel.getLatencyFrames() + 1;      // audio samples
        const int outDelay = fromModel.getLatencyFrames() + 1;   // model samples

        // Native HQ: silence ahead of the model's output, enough that it never runs dry between two frames.
        // With the worker the frame's audio comes one engine block later (extraDelay).
        nativePreRoll = (int) std::ceil ((kFrame + outDelay) * ratio) + inDelay + kChunk + 2 + extraDelay;
        modelIn.allocate (2, 4096);
        modelOut.allocate (2, nativePreRoll + 2 * (int) std::ceil (kFrame * ratio) + 64);

        latency[UltraLow] = 0;
        latency[Low] = juce::roundToInt (0.01 * rate);
        latency[Aligned] = juce::roundToInt (0.02 * rate) + inDelay + extraDelay;   // also waits for the resampled copy
        latency[Native] = juce::roundToInt ((2 * kFrame + outDelay) * ratio) + inDelay + nativePreRoll;
    }
    else
    {
        for (int m = 0; m < NumModes; ++m) latency[m] = latencyForMode (m) + (m >= Aligned ? extraDelay : 0);
    }

    const int delaySize = juce::nextPowerOfTwo (juce::jmax (2048, latency[Native] + 8));
    delayMask = delaySize - 1;
    for (auto& c : ch) c.delay.assign ((size_t) delaySize, 0.0f);
}

void Denoiser::prepare (double sampleRate, int block)
{
    settleJob();
    engineBlock = juce::jmax (0, block);
    extraDelay = engineBlock;   // Aligned / Native: the time the worker has for a frame
    configure (sampleRate);
    reset();
}

void Denoiser::reset() noexcept
{
    settleJob();
    for (auto& c : ch) c.clear();
    pos = 0;
    writePos = 0;
    nativePos = kFrame;
    applyIn = 0;
    vad.store (0.0f);
    if (resampled)
    {
        toModel.reset();
        fromModel.reset();
        modelIn.skip (modelIn.available());
        modelOut.skip (modelOut.available());
        const float* silence[2] = { nullptr, nullptr };
        modelOut.write (silence, 2, nativePreRoll);
    }
}

bool Denoiser::claimJob() noexcept
{
    int q = queued;
    return jobState.compare_exchange_strong (q, running, std::memory_order_acq_rel);
}

void Denoiser::runJob() noexcept
{
    // Worker thread, or the audio thread when it computes the frame itself. Only touches the job's buffers and the
    // RNNoise states (nobody else uses them while a job is in flight).
    float v = 0.0f;
    for (int c = 0; c < jobChans; ++c)
    {
        auto& C = ch[c];
        if (jobMode == Native)
        {
            v = juce::jmax (v, rnnoise_process_frame (C.st, C.jobOut, C.jobIn));
            for (auto& x : C.jobOut) x *= (1.0f / 32768.0f);
        }
        else
            v = juce::jmax (v, rnnoise_process_frame_gains (C.st, C.jobGains, C.jobIn));
    }
    jobVad = v;
    jobState.store (done, std::memory_order_release);
}

void Denoiser::settleJob() noexcept
{
    int q = queued;
    jobState.compare_exchange_strong (q, idle, std::memory_order_acq_rel);   // not started: cancelled
    while (jobState.load (std::memory_order_acquire) == running) _mm_pause();   // the worker finishes it
    jobState.store (idle, std::memory_order_release);
    pending = false;
}

void Denoiser::submitFrame (int chans, bool realtime) noexcept
{
    for (int c = 0; c < chans; ++c) std::memcpy (ch[c].jobIn, ch[c].fifo, sizeof (ch[c].fifo));
    jobChans = chans;
    jobMode = mode;
    pending = true;
    applyIn = mode >= Aligned ? extraDelay : 0;
    if (realtime && engineBlock > 0)
    {
        jobState.store (queued, std::memory_order_release);
        WorkerPool::get().submit (this);
    }
    else
    {
        jobState.store (running, std::memory_order_relaxed);
        runJob();
    }
}

void Denoiser::applyFrame (bool force) noexcept
{
    if (jobState.load (std::memory_order_acquire) != done)
    {
        if (! force) return;   // Ultra Low / Low: the gains start as soon as they are there
        if (claimJob()) runJob();   // still queued: the audio thread computes it itself
        else while (jobState.load (std::memory_order_acquire) != done) _mm_pause();   // the worker is on it
    }
    if (jobMode == Native)
    {
        if (resampled)
        {
            const float* out[2] = { ch[0].jobOut, jobChans > 1 ? ch[1].jobOut : nullptr };
            fromModel.push (out, kFrame, modelOut);
        }
        else
            for (int c = 0; c < jobChans; ++c) std::memcpy (ch[c].nativeOut, ch[c].jobOut, sizeof (ch[c].jobOut));
        nativePos = 0;
    }
    else
        for (int c = 0; c < jobChans; ++c)
        {
            // y = g[31] * x + sum_j (g[j] - g[j+1]) * LP_j(x)  -> telescoping, exact when all gains are equal
            auto& C = ch[c];
            const float* g = C.jobGains;
            for (int j = 0; j < kXover; ++j)
                C.wStep[j] = ((double) (g[j] - g[j + 1]) - C.w[j]) * rampInv;
            C.wLastStep = ((double) g[kBands - 1] - C.wLast) * rampInv;
            C.rampLeft = ramp;
        }
    vad.store (jobVad, std::memory_order_relaxed);
    jobState.store (idle, std::memory_order_release);
    pending = false;
}

inline float Denoiser::filterbank (Channel& c, float in) noexcept
{
    // 31 lowpasses, all fed by the same sample: independent of each other, so four of them run in one AVX register.
    const double d = in;
#if defined (__AVX2__)
    if (c.rampLeft > 0)
    {
        for (int j = 0; j < kPad; j += 4)
            _mm256_store_pd (c.w + j, _mm256_add_pd (_mm256_load_pd (c.w + j), _mm256_load_pd (c.wStep + j)));
        c.wLast += c.wLastStep;
        --c.rampLeft;
    }
    const __m256d dv = _mm256_set1_pd (d);
    __m256d acc = _mm256_setzero_pd();
    for (int j = 0; j < kPad; j += 4)
    {
        const __m256d s1 = _mm256_load_pd (c.z1 + j), s2 = _mm256_load_pd (c.z2 + j);
        const __m256d o = _mm256_fmadd_pd (_mm256_load_pd (b0 + j), dv, s1);
        _mm256_store_pd (c.z1 + j, _mm256_fmadd_pd (_mm256_load_pd (b1 + j), dv, _mm256_fnmadd_pd (_mm256_load_pd (a1 + j), o, s2)));
        _mm256_store_pd (c.z2 + j, _mm256_fnmadd_pd (_mm256_load_pd (a2 + j), o, _mm256_mul_pd (_mm256_load_pd (b2 + j), dv)));
        acc = _mm256_fmadd_pd (_mm256_load_pd (c.w + j), o, acc);
    }
    const __m128d h = _mm_add_pd (_mm256_castpd256_pd128 (acc), _mm256_extractf128_pd (acc, 1));
    const double sum = _mm_cvtsd_f64 (_mm_add_sd (h, _mm_unpackhi_pd (h, h)));
    return (float) (c.wLast * d + sum);
#else
    if (c.rampLeft > 0)
    {
        for (int j = 0; j < kXover; ++j) c.w[j] += c.wStep[j];
        c.wLast += c.wLastStep;
        --c.rampLeft;
    }
    double y = c.wLast * d;
    for (int j = 0; j < kXover; ++j)
    {
        const double o = b0[j] * d + c.z1[j];
        c.z1[j] = b1[j] * d - a1[j] * o + c.z2[j];
        c.z2[j] = b2[j] * d - a2[j] * o;
        y += c.w[j] * o;
    }
    return (float) y;
#endif
}

void Denoiser::process (float* l, float* r, int n, const DenoiserSettings& s, bool stereo) noexcept
{
    if (ch[0].st == nullptr || ch[1].st == nullptr)
        return;

    const int newMode = juce::jlimit (0, (int) NumModes - 1, s.mode);
    if (newMode != mode)
    {
        mode = newMode;
        reset();
    }

    const float wet = juce::jlimit (0.0f, 1.0f, s.amount);
    float* io[2] = { l, r };
    if (resampled) processResampled (io, n, wet, 1.0f - wet, stereo ? 2 : 1, s.realtime);
    else           process48k (io, n, wet, 1.0f - wet, stereo ? 2 : 1, s.realtime);

    if (! stereo)
        std::memcpy (r, l, (size_t) n * sizeof (float));
}

void Denoiser::process48k (float** io, int n, float wet, float dry, int chans, bool realtime) noexcept
{
    const int delaySamples = latency[mode];
    const bool exact = mode >= Aligned;   // Aligned / Native: the result is used exactly when due
    int i = 0;
    while (i < n)
    {
        if (pending && applyIn <= 0) applyFrame (exact);
        int chunk = juce::jmin (n - i, kFrame - pos);
        if (pending && applyIn > 0) chunk = juce::jmin (chunk, applyIn);
        const int wp = writePos, np = nativePos;

        for (int c = 0; c < chans; ++c)
        {
            auto& C = ch[c];
            float* x = io[c] + i;
            float* delay = C.delay.data();
            int w = wp;
            for (int k = 0; k < chunk; ++k)
            {
                const float in = x[k];
                C.fifo[pos + k] = in * 32768.0f; // RNNoise expects 16-bit scale
                delay[w] = in;
                const float d = delay[(w - delaySamples) & delayMask];
                w = (w + 1) & delayMask;
                const float y = mode == Native ? (np + k < kFrame ? C.nativeOut[np + k] : 0.0f) : filterbank (C, d);
                x[k] = y * wet + d * dry;
            }
        }

        writePos = (wp + chunk) & delayMask;
        nativePos = juce::jmin (kFrame, np + chunk);
        pos += chunk;
        i += chunk;
        if (pending) applyIn -= chunk;

        if (pos == kFrame)
        {
            pos = 0;
            if (pending) applyFrame (true);   // the previous frame was never used (worker very late): now
            submitFrame (chans, realtime);
        }
    }
}

void Denoiser::processResampled (float** io, int n, float wet, float dry, int chans, bool realtime) noexcept
{
    const int delaySamples = latency[mode];
    const bool exact = mode >= Aligned;
    float nativeWet[2][kChunk];
    int i = 0;
    while (i < n)
    {
        int chunk = juce::jmin (n - i, kChunk);
        if (pending && applyIn > 0) chunk = juce::jmin (chunk, applyIn);

        // The model hears a 48 kHz copy of the input; a frame is ready every 10 ms.
        const float* in[2] = { io[0] + i, chans > 1 ? io[1] + i : nullptr };
        toModel.push (in, chunk, modelIn);
        while (modelIn.available() >= kFrame)
        {
            if (pending) applyFrame (true);   // the previous frame was never used (worker very late): now
            float* frame[2] = { ch[0].fifo, ch[1].fifo };
            modelIn.read (frame, 2, kFrame);
            for (int c = 0; c < chans; ++c)
                for (auto& v : ch[c].fifo) v *= 32768.0f; // RNNoise expects 16-bit scale
            submitFrame (chans, realtime);
        }
        // Due now (computed in place without the worker: at once, as before): gains / Native audio for this chunk.
        if (pending && applyIn <= 0) applyFrame (exact);

        if (mode == Native)
        {
            float* dst[2] = { nativeWet[0], nativeWet[1] };
            const int got = modelOut.read (dst, 2, chunk);
            for (int c = 0; c < 2; ++c)
                std::fill (nativeWet[c] + got, nativeWet[c] + chunk, 0.0f);
        }

        const int wp = writePos;
        for (int c = 0; c < chans; ++c)
        {
            auto& C = ch[c];
            float* x = io[c] + i;
            float* delay = C.delay.data();
            int w = wp;
            for (int k = 0; k < chunk; ++k)
            {
                delay[w] = x[k];
                const float d = delay[(w - delaySamples) & delayMask];
                w = (w + 1) & delayMask;
                const float y = mode == Native ? nativeWet[c][k] : filterbank (C, d);
                x[k] = y * wet + d * dry;
            }
        }
        writePos = (wp + chunk) & delayMask;
        if (pending) applyIn -= chunk;
        i += chunk;
    }
}
} // namespace hydra::dsp

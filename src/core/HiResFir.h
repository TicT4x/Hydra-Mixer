#pragma once
#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace hydra::dsp
{
/** High-resolution EQ curve as a minimum-phase FIR (the Match function of the EQ, 14.11.), without latency:
    the first kBlock taps are convolved directly, the rest in uniformly partitioned FFT blocks (that part only needs
    input that is at least one block old, so it is computed at the start of each block). 8192 taps at up to 50 kHz
    (16384 above), i.e. a resolution of about 6 Hz. Cost: about kBlock + taps / kBlock * 4 multiply-adds per sample and
    channel while a filter is set; nothing while off.

    A new filter is crossfaded over one block. Threading: setFilter / collect / design on the message thread, process
    on the audio thread (never allocates); filters change hands through atomics. */
class HiResFir
{
public:
    static constexpr int kBlock = 128, kMaxTaps = 16384, kMaxParts = kMaxTaps / kBlock;
    static constexpr int kBins = kBlock + 1;   // complex bins of a 2*kBlock real FFT

    struct Side
    {
        std::vector<float> h0rev;          // first kBlock taps, reversed
        std::vector<float> spectra;        // partitions 1 .. parts-1: 2 * kBins floats each (JUCE real-FFT layout)
        std::vector<float> taps;           // the whole impulse response (tests, views)
    };
    /** One filter per channel (the same twice for a linked match). midSide: the channels are mid and side (the
        input is converted before and back after). A filter of the other kind fades out to dry first. */
    struct Filter
    {
        int parts = 0;                     // taps / kBlock
        Side side[2];
        bool midSide = false;
    };

    HiResFir();
    ~HiResFir();

    /** Minimum-phase FIR whose magnitude follows dbAt (frequency in Hz -> dB), on both channels; with dbAt2 the second
        channel gets its own (right or side). */
    static std::unique_ptr<Filter> design (const std::function<double (double)>& dbAt, double sampleRate,
                                           const std::function<double (double)>& dbAt2 = {}, bool midSide = false);

    /** Message thread: the filter to fade to (nullptr = fade out and switch off). */
    void setFilter (std::unique_ptr<Filter>);
    /** Message thread: frees filters the audio thread no longer uses (call regularly). */
    void collect();

    /** Audio thread. */
    void process (float* l, float* r, int n) noexcept;
    /** Clears the history (prepare, while the audio thread does not run). */
    void reset() noexcept;

private:
    struct Chan
    {
        float hist[2 * kBlock] {};      // the last kBlock inputs, written twice (contiguous window)
        float frame[2 * kBlock] {};     // previous block | current block
        float tailNew[kBlock] {}, tailOld[kBlock] {};
        std::vector<float> fdl;         // kMaxParts input spectra (2 * kBins floats each)
    };
    void beginBlock() noexcept;
    void computeTail (const Filter&, int side, Chan&, float* out) noexcept;
    void clearState() noexcept;
    static Filter* offMarker() noexcept;

    juce::dsp::FFT fft { 8 };            // 2 * kBlock
    Chan chans[2];
    std::vector<float> work, acc;        // 4 * kBlock, 2 * kBins
    int pos = 0, hpos = 0, fdlHead = 0;
    Filter* cur = nullptr;               // audio thread
    Filter* prev = nullptr;              // fading from (or waiting to be retired)
    bool fading = false, idle = true, domainMidSide = false;
    std::atomic<Filter*> pending { nullptr }, retired { nullptr };
};
} // namespace hydra::dsp

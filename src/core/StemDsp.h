#pragma once
#include "Common.h"
#include <juce_dsp/juce_dsp.h>

/** Building blocks of the Stems effect (engine/Stems.h) that need no ONNX Runtime: the STFT the model expects,
    an offline resampler to / from its 44.1 kHz and the fix that makes the model run correctly on DirectML.
    None of this runs on the audio thread (the stems worker thread and the tests use it). */
namespace hydra::stems
{
/** STFT / inverse STFT exactly like torch.stft / torch.istft with n_fft 2048, hop 512, periodic Hann window,
    center = true (reflect padding), normalized = false, one-sided: what BS-RoFormer was trained with. */
class Stft
{
public:
    static constexpr int kFft = 2048, kHop = 512, kBins = kFft / 2 + 1;
    Stft();
    /** Frames of a signal of length samples (torch: 1 + samples / hop). */
    static int numFrames (int samples) noexcept { return 1 + samples / kHop; }
    /** x: samples values. re / im: [kBins][frames] (frequency-major, like the model's [.., 1025, T]). */
    void forward (const float* x, int samples, float* re, float* im);
    /** Inverse of forward: re / im [kBins][frames] -> y (samples values). */
    void inverse (const float* re, const float* im, int frames, float* y, int samples);

private:
    juce::dsp::FFT fft { 11 };
    std::vector<float> window, frame, padded, ola, env;
};

/** Windowed-sinc resampler for whole buffers (zero phase, not real-time): out[m] = in at position start + m * step
    (in input samples). Kaiser-windowed sinc, 32 zero crossings each side, cut off just below the lower Nyquist. */
class Resampler
{
public:
    /** step = input samples per output sample (in rate / out rate). */
    explicit Resampler (double step);
    /** Input samples needed on each side of a position. */
    int halfWidth() const noexcept { return (int) std::ceil (width) + 1; }
    /** in: inLen samples (positions outside count as 0). */
    void process (const float* in, int inLen, double start, float* out, int outLen) const;

private:
    double step, cutoff, width;   // width: half length of the kernel in input samples
    static constexpr int kTableRes = 512;   // table points per input sample
    std::vector<float> table;
    float kernel (double x) const noexcept;
    // Polyphase taps when step is a ratio p / q with a small q (all usual rates to / from 44.1 kHz): positions then
    // only fall on q fractional phases, each with fixed taps (one dot product per output sample).
    int phases = 0, taps = 0;
    std::vector<float> poly;   // [phase][tap]
};

/** BS-RoFormer's ONNX export splits tensors in halves with Split nodes (GLU of the mask estimators); DirectML
    computes some of them wrongly (output off by 80 %, found 10.11. against the PyTorch reference). Every equal
    two-way Split is replaced by two Slice nodes (Shape -> half -> Slice), which is the same maths. Works on the
    protobuf directly (no ONNX library). Returns the number of Splits replaced, -1 if the file is not a model
    this can handle (then nothing is written). */
int patchSplits (const juce::MemoryBlock& model, juce::MemoryBlock& out);

/** HT-Demucs' ONNX export does its inverse STFT with 1-D ConvTranspose nodes (kernel 4096, stride 1024); DirectML
    runs out of memory on them (E_OUTOFMEMORY on an 8 GB GPU, 10.11.). Each such node (group 1, no padding, no bias,
    kernel a multiple of the stride) becomes MatMul (frames) + overlap-add of kernel / stride shifted parts, the
    same maths. Returns the number of nodes replaced, -1 if the file cannot be handled. */
int patchConvTranspose (const juce::MemoryBlock& model, juce::MemoryBlock& out);

/** Both fixes (what a model gets after its download). Returns the number of nodes replaced, -1 on failure. */
int patchForDirectML (const juce::MemoryBlock& model, juce::MemoryBlock& out);
} // namespace hydra::stems

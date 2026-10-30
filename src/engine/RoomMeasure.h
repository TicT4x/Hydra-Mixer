#pragma once
#include "Common.h"

namespace hydra
{
/** One room measurement in progress (Tools > Room Measurement), see Engine::startMeasurement.

    The audio thread plays 'stimulus' on one bus and records one channel of one input into 'capture',
    sample-aligned (same engine callback). Both are raw: the stimulus replaces the bus's whole mix and
    chain (EQ, plugins, fader, limiter, delay; optionally its EQ is kept to verify a correction), and the
    input is taken from its source before trim, phase, denoiser, EQ, gate, compressor and plugins.
    Buffers are allocated before the run is handed to the engine and never resized while it is live. */
struct MeasureRun
{
    int input = 0;             // input strip whose source is the microphone
    int inputChannel = 0;      // 0 = left, 1 = right channel of that source
    int bus = 0;               // bus whose output plays the sweep
    int outChannels = 3;       // bit 0 = left, bit 1 = right speaker
    bool muteOthers = true;    // every other bus is silent while measuring
    bool busEq = false;        // the sweep passes the bus's EQ (to check a correction)

    std::vector<float> stimulus, capture;
    int length = 0;            // = stimulus.size() = capture.size()

    // Written by the audio thread.
    std::atomic<int> pos { 0 };           // samples played / recorded so far (length = finished)
    std::atomic<float> inputPeak { 0 };   // microphone peak since the UI last took it
    std::atomic<bool> noInput { false };  // the input had no signal source during the run
    // Written by the message thread: fade out and finish early.
    std::atomic<bool> stop { false };

    bool finished() const noexcept { return pos.load (std::memory_order_acquire) >= length; }
    float progress() const noexcept { return length > 0 ? (float) pos.load (std::memory_order_relaxed) / (float) length : 1.0f; }
};
} // namespace hydra

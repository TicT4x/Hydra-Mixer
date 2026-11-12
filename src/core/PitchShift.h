#pragma once
#include "Common.h"

namespace hydra::dsp
{
/** Real-time pitch shifter (Transpose effect) around Signalsmith Stretch 1.3.2 (MIT, external/signalsmith-stretch,
    FFTs from signalsmith-linear 0.6.6). Fast / Normal / High = its block length (kConfigs in PitchShift.cpp).

    Keep timbre (optional): the library's formant correction keeps the spectral envelope of the input while the
    pitch moves, so the shifted sound keeps its tone colour (no chipmunk / no muffled sound). It guesses the
    fundamental itself.

    Latency only while it shifts: at ratio 1 the signal passes straight through (no latency, no processing).
    Leaving ratio 1, the stretcher starts and the dry signal keeps playing until it is filled, then the shifted
    signal fades in over 10 ms; back at ratio 1 it fades out. A new quality fades out, restarts and fades in.
    Audio thread: no allocation, no locks (the library allocates only in configure, done in prepare). */
class PitchShifter
{
public:
    enum Quality { fast = 0, normal, high, numQualities };
    static juce::StringArray qualityNames() { return { "Fast", "Normal", "High" }; }

    PitchShifter();
    ~PitchShifter();

    /** Allocates everything for every quality (message thread). */
    void prepare (double sampleRate);
    void reset() noexcept;

    /** In-place stereo. ratio = frequency factor (2 = one octave up, 0.25 .. 4); 1 = straight through. */
    void process (float* l, float* r, int n, double ratio, int quality, bool keepTimbre = false) noexcept;
    /** Always through the stretcher (no dry signal, no fades), delayed by latencyFor (tests). */
    void processWet (float* l, float* r, int n, double ratio, int quality, bool keepTimbre = false) noexcept;

    /** Latency now: latencyFor (quality) while the shifted signal plays, 0 while the dry one does. */
    int getLatencySamples() const noexcept { return state == State::shifting ? latencyFor (quality) : 0; }
    /** Latency of a quality while it shifts (after prepare). */
    int latencyFor (int quality) const noexcept;
    /** Latency of a quality at a sample rate, without an instance (what latencyFor will say). */
    static int latencyAt (double sampleRate, int quality) noexcept;

private:
    enum class State { dry, starting, shifting };
    void run (float* l, float* r, int n, double ratio, bool keepTimbre) noexcept;

    struct Impl;                 // the Signalsmith instances (keeps the library's headers out of here)
    std::unique_ptr<Impl> impl;
    static constexpr int kChunk = 256;
    double sr = 48000.0;
    int quality = normal;
    std::array<std::vector<float>, 2> dry;
    State state = State::dry;
    int startLeft = 0;
    float mix = 0.0f, fadeStep = 0.0f;
    bool prepared = false;
};
} // namespace hydra::dsp

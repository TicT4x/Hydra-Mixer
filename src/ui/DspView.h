#pragma once
#include "BarList.h"
#include <deque>

namespace hydra
{
/** DSP load finder (own window, opened by clicking the DSP meter in the top bar): how much of the
    audio period each channel and each stage in it uses (denoiser, EQ, gate, compressor, every plugin,
    limiter), plus the engine's own work (resampling of other devices, recorder, mixing).

    Values are the average share of the time available per audio block over the last second
    (100 % = the engine would no longer keep up). The engine measures only while this window exists.
    Clicking a row selects that channel in the main window. Refreshed 4x per second. */
class DspView final : public juce::Component, private juce::Timer
{
public:
    explicit DspView (UiContext& c);
    ~DspView() override;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    /** One reading of all counters (ticks), keyed by what they measure. */
    struct Reading
    {
        double time = 0;            // ms (message thread clock)
        uint64_t frames = 0, total = 0, recorder = 0;
        std::unordered_map<std::string, uint64_t> ticks;
    };

    void timerCallback() override;
    void refresh();
    Reading read() const;

    UiContext& ctx;
    juce::Viewport viewport;
    BarList list;
    std::deque<Reading> history;   // last ~1.5 s of readings
    juce::String summary;
    double totalLoad = 0, peakLoad = 0, shownScale = 0.02;
};
} // namespace hydra

#pragma once
#include "Charts.h"
#include <deque>

namespace hydra
{
/** DSP load analyser (own window, opened by clicking the DSP meter in the top bar), built like the
    Latency Finder so the biggest consumer is visible at once:

    - top: "Top consumers" ranks every stage of every channel ("IN 1 Mic · Denoiser"), next to it the
      load per channel and per kind of component (donuts),
    - below: one lane per channel (heaviest first) with its stages in processing order (denoiser, EQ,
      gate, compressor, each plugin, limiter, the rest); expensive stages are tinted, the three biggest
      of all are tagged #1..#3. The engine's own work (resampling of other devices, recorder, mixing)
      is a lane of its own.

    Values are the average share of the time available per audio block over the last second
    (100 % = the engine would no longer keep up). The engine measures only while this window exists.
    Clicking a lane, stage or list entry selects that channel in the main window. Refreshed 4x per second. */
class DspView final : public juce::Component, private juce::Timer
{
public:
    explicit DspView (UiContext& c);
    ~DspView() override;
    void paint (juce::Graphics&) override;
    void resized() override;

    /** One processing stage of a channel (or a part of the engine's own work). */
    struct Stage
    {
        juce::String label, detail;
        double load = 0.0;            // % of the audio period
        juce::Colour colour;
        int selectIndex = -1;         // -1: the lane's channel
        int rank = 0;                 // 1..3: among the three biggest stages overall
    };
    struct Lane
    {
        juce::String key, title, note;   // note: e.g. "no source"
        juce::Colour colour;
        int selectIndex = -1;
        std::vector<Stage> stages;       // processing order
        double total = 0.0;
    };

private:
    class Chains;
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
    ShareChart top, byChannel, byKind;
    juce::Viewport viewport;
    std::unique_ptr<Chains> chains;
    std::deque<Reading> history;   // last ~1.5 s of readings
    std::vector<juce::String> laneOrder;
    juce::String summary, shownSignature;
    double totalLoad = 0, peakLoad = 0;
};
} // namespace hydra

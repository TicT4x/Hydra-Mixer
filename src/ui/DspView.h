#pragma once
#include "Charts.h"
#include <deque>

namespace hydra
{
/** DSP load analyser (own window, opened by clicking the DSP meter in the top bar or Tools > DSP Load).

    One calm table instead of moving charts:
    - top: total load of the audio engine as a bar (with the recent peak) and the three biggest consumers,
    - below: one row per channel in mixer order (inputs, then outputs) and a row for the engine's own work.
      Columns are the processing stages (Denoiser, EQ, Gate, Comp, Plugins, Limiter, Rest); every cell shows
      its share of the audio period and is tinted by how expensive it is. Stages that are switched off show "-".

    The order never changes. Measured 10x per second as the mean of the last second; bars and tints glide
    there every display frame, the numbers show the 1 s mean and are rewritten once per second. 100 % = the engine would no longer
    keep up. The engine measures only while this window exists. Clicking a row selects that channel. */
class DspView final : public juce::Component, private juce::Timer
{
public:
    explicit DspView (UiContext& c);
    ~DspView() override;
    void paint (juce::Graphics&) override;
    void resized() override;

    enum Column { denoiser, eq, gate, comp, plugins, limiter, rest, numColumns };

    struct Row
    {
        juce::String key, title, note, pluginInfo, engineText;
        juce::Colour colour;
        int selectIndex = -1;
        bool engine = false;
        std::array<double, numColumns> target {}, shown {}, label {};   // label: value as written (once per second)
        std::array<bool, numColumns> present {};
        double totalTarget = 0.0, totalShown = 0.0, totalLabel = 0.0;
    };

private:
    class Table;
    /** One reading of all counters (ticks), keyed by what they measure. */
    struct Reading
    {
        double time = 0;            // ms (message thread clock)
        uint64_t frames = 0, total = 0, recorder = 0;
        std::unordered_map<std::string, uint64_t> ticks;
    };

    void timerCallback() override;
    void measure();
    void frame (double timestamp);   // display frame: bars glide to the measurement
    Reading read() const;

    UiContext& ctx;
    juce::Viewport viewport;
    std::unique_ptr<Table> table;
    std::deque<Reading> history;   // last ~1.5 s of readings
    juce::String summary, hotspots;
    double totalTarget = 0, totalShown = 0, totalLabel = 0, peakLoad = 0, peakShown = 0, peakLabel = 0;
    double lastLabelTime = 0;   // ms
    FrameClock clock;
    juce::VBlankAttachment vblank { this, [this] (double t) { frame (t); } };
};
} // namespace hydra

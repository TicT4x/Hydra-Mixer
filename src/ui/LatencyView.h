#pragma once
#include "Charts.h"

namespace hydra
{
/** Latency finder (own window). Pick an output (bus) at the top; the window shows its signal flow:
    one lane per routed input (input device, denoiser, each plugin) that merges into the bus, followed by
    the bus plugins and the output device. Every stage shows the latency it adds (+ms), its share of the
    path's total (%) and the running total up to that point. Below, a chart shows how much each component
    contributes to the focused path (the longest one, or the lane that was clicked).
    Refreshed 4x per second while the window is open. */
class LatencyView final : public juce::Component, private juce::Timer
{
public:
    explicit LatencyView (UiContext& c);
    ~LatencyView() override;
    void paint (juce::Graphics&) override;
    void resized() override;

    /** One stage of a signal path that can add latency. */
    struct Node
    {
        juce::String label, detail;   // "Input device" / "ASIO: Ch 1+2", "Denoiser" / "Low (10 ms)", plugin name...
        double ms = 0.0;              // < 0: device offline
        juce::Colour colour;
        int selectIndex = -1;         // channel selected in the mixer on click
    };
    /** One routed input of the shown output, from its device up to the bus. */
    struct Lane
    {
        int input = -1;
        juce::String title;
        std::vector<Node> nodes;
        double ms = 0.0;              // input side only
    };
    struct Flow
    {
        int bus = -1;
        juce::String busTitle, problem;   // problem: no output / no routed input
        std::vector<Lane> lanes;          // longest first
        std::vector<Node> busNodes;       // bus plugins, output device
        double busMs = 0.0;
        bool hasOutput = false;           // the bus has an output device
        int focus = 0;                    // lane whose path is shown in the chart
        juce::String signature() const;   // to repaint only on change
    };

private:
    class OutputPicker;
    class FlowView;
    void timerCallback() override;
    void refresh();
    Flow build (int bus) const;
    int defaultBus() const;

    UiContext& ctx;
    std::unique_ptr<OutputPicker> picker;
    juce::Viewport viewport;
    std::unique_ptr<FlowView> flowView;
    std::unique_ptr<ShareChart> breakdown;   // latency share of the focused path
    juce::String summary, shownSignature;
    int bus = -1, focusInput = -1;   // picked output, clicked lane (-1 = longest)
};
} // namespace hydra

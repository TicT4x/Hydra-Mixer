#pragma once
#include "Charts.h"
#include <limits>

namespace hydra
{
/** Where the device-side latency of a source / output comes from, as text for a tooltip:
    buffer, driver / converters, clock sync buffer of a secondary device. */
juce::String explainIoLatency (const Engine&, const IoTarget&, bool input);

/** Latency finder (own window).

    Top: a matrix with one row per input strip and one column per output (bus). Every cell where the input
    is routed to the output shows the total latency of exactly that path (input device + denoiser + input
    plugins + sync delay + bus plugins + bus sync delay + output device), so with several inputs on one output it is clear which input
    has which latency. Clicking a cell shows that path below: its stages in order with the latency each
    adds (+ms), its share and the running total, and a chart of the shares. Default: the longest path.

    Device latencies contain the fill level of the drift buffers, which moves a little all the time. They are
    sampled 10x per second and shown as the mean of the last second (rewritten once per second), so the numbers
    stay calm; the chart glides to new values. Changes of the routing or the path show at once. */
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
        juce::String why;             // tooltip: how this latency comes about
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

    /** Matrix cell: total latency of input -> output (< 0: offline, NaN: not routed). */
    struct Cell { double ms = std::numeric_limits<double>::quiet_NaN(); bool routed = false; };

private:
    class Matrix;
    class FlowView;
    void timerCallback() override;
    void refresh();
    void measureDevices();
    Flow build (int bus) const;
    double inputSideMs (int input) const;
    double busSideMs (int bus) const;

    UiContext& ctx;
    juce::Viewport matrixView, viewport;
    std::unique_ptr<Matrix> matrix;
    std::unique_ptr<FlowView> flowView;
    std::unique_ptr<ShareChart> breakdown;   // latency share of the focused path
    juce::String summary, shownSignature;
    int bus = -1, focusInput = -1;   // shown path (-1 = the longest)
    bool userPicked = false;
    std::map<int, double> inDevMs, outDevMs;   // shown device latencies (input index / bus index), 1 s means
    std::map<int, Mean> inMeans, outMeans;     // samples of the current second
    double lastPublish = 0.0;                  // ms
};
} // namespace hydra

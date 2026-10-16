#pragma once
#include "BarList.h"

namespace hydra
{
/** Latency finder (own window): shows how much latency every part of each signal path adds
    (input device, denoiser, each plugin, output device), so the biggest contributor is easy to find.

    "Signal paths" lists every routed input -> bus pair whose bus has an output, longest first;
    "Channels" lists every channel on its own. Clicking a row selects that channel in the main window.
    Refreshed 4x per second while the window is open. */
class LatencyView final : public juce::Component, private juce::Timer
{
public:
    explicit LatencyView (UiContext& c);
    ~LatencyView() override;
    void paint (juce::Graphics&) override;
    void resized() override;

    using Segment = BarList::Segment;   // value in ms
    using Row = BarList::Row;

    /** Input side (device, denoiser, plugins) / output side (plugins, device) of a channel. */
    static std::vector<Segment> inputSegments (Engine&, int input);
    static std::vector<Segment> busSegments (Engine&, int bus);

private:
    void timerCallback() override;
    void refresh();

    UiContext& ctx;
    juce::Viewport viewport;
    BarList list;
    juce::String summary;
};
} // namespace hydra

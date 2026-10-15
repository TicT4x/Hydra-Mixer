#pragma once
#include "Widgets.h"

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

    struct Segment
    {
        juce::String label;   // "Input device", "Denoiser", plugin name...
        double ms = 0.0;
        juce::Colour colour;
    };
    struct Row
    {
        juce::String title, detail;   // e.g. "IN 1 Mic  ->  A1 Speakers", "offline"
        juce::Colour colour;
        std::vector<Segment> segments;
        double total = 0.0;
        int selectIndex = -1;         // channel selected on click
        bool operator== (const Row&) const;
    };

    /** Input side (device, denoiser, plugins) / output side (plugins, device) of a channel. */
    static std::vector<Segment> inputSegments (Engine&, int input);
    static std::vector<Segment> busSegments (Engine&, int bus);

private:
    class List;
    void timerCallback() override;
    void refresh();

    UiContext& ctx;
    juce::Viewport viewport;
    std::unique_ptr<List> list;
    juce::String summary;
};
} // namespace hydra

#pragma once
#include "Charts.h"

namespace hydra
{
/** Sync Delay (Tools > Sync Delay, own window).

    Channels whose devices have different latencies (two microphones on two interfaces, speakers on ASIO and
    a headset on WASAPI) reach the mix / the ear at different times. For every channel in use, grouped like
    the mixer (inputs, hardware outputs, virtual outputs), a row shows its measured latency, the "Auto sync"
    switch, the automatic and the manual delay and the resulting latency, plus a bar: solid = the channel's own
    latency, striped = added delay. Channels of a group with Auto sync on line up with the slowest of them
    (Engine::updateSyncDelays); their bars end at the same point. Measured 10x per second while open: the bars
    glide there every display frame, the numbers show the mean of the last second (rewritten once per second). */
class SyncDelayView final : public juce::Component, private juce::Timer
{
public:
    explicit SyncDelayView (UiContext& c);
    ~SyncDelayView() override;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class Row;
    class Content;
    void timerCallback() override;
    void frame (double timestamp);
    void rebuild();

    UiContext& ctx;
    juce::Viewport viewport;
    std::unique_ptr<Content> content;
    uint32_t shownLayout = ~0u;
    double lastPublish = 0.0;   // ms
    FrameClock clock;
    juce::VBlankAttachment vblank { this, [this] (double t) { frame (t); } };
};
} // namespace hydra

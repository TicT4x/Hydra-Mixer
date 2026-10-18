#pragma once
#include "Widgets.h"
#include <map>
#include <cmath>

namespace hydra
{
/** Live values in the tool windows (DSP Load, Latency Finder, Sync Delay):
    - bars, rings and tints glide to the measurement every display frame (glide, ~0.25 s time constant),
      so they move smoothly instead of jumping at the measuring rate;
    - numbers show the mean of the last second and are rewritten once per second (Mean, kLabelPeriod),
      so they can be read. */
inline constexpr double kGlideSeconds = 0.25, kLabelPeriodMs = 1000.0;

/** Moves shown towards target (exponential, frame-rate independent); snaps once it is within eps.
    Returns true if shown changed (i.e. it needs to be drawn again). */
inline bool glide (double& shown, double target, double dt, double eps = 1.0e-3)
{
    if (! std::isfinite (target) || ! std::isfinite (shown) || std::abs (target - shown) <= eps)
    {
        const bool changed = ! (shown == target) && ! (std::isnan (shown) && std::isnan (target));
        shown = target;
        return changed;
    }
    shown += (target - shown) * (1.0 - std::exp (-dt / kGlideSeconds));
    return true;
}

/** Average of the samples since the last take(). */
struct Mean
{
    double sum = 0.0;
    int count = 0;
    void add (double v) noexcept { sum += v; ++count; }
    void reset() noexcept { sum = 0.0; count = 0; }
    bool empty() const noexcept { return count == 0; }
    double take (double fallback) noexcept { const double m = count > 0 ? sum / count : fallback; reset(); return m; }
};

/** Seconds since the previous display frame (VBlankAttachment timestamps), limited to 0.1 s. */
struct FrameClock
{
    double last = 0.0;
    double step (double timestampSec) noexcept
    {
        const double dt = last > 0.0 ? juce::jlimit (0.0, 0.1, timestampSec - last) : 1.0 / 60.0;
        last = timestampSec;
        return dt;
    }
};

/** Legend for a tool window header: colour swatches with names, right-aligned in area. */
void drawLegend (juce::Graphics&, juce::Rectangle<int> area, const std::vector<std::pair<juce::String, juce::Colour>>& items);

/** Ranks items by value, biggest first, without letting near-equal items swap places on every refresh:
    the previous order (keys) is kept unless an item is clearly bigger than the one above it.
    Measured values (DSP load) jitter a little, so the lists stay readable. */
template <typename T, typename KeyFn, typename ValueFn>
void rankSteady (std::vector<T>& items, std::vector<juce::String>& order, KeyFn key, ValueFn value, double margin)
{
    std::map<juce::String, int> index;
    for (size_t i = 0; i < order.size(); ++i) index.emplace (order[i], (int) i);
    auto pos = [&] (const juce::String& k) { const auto it = index.find (k); return it == index.end() ? (int) order.size() : it->second; };
    std::stable_sort (items.begin(), items.end(), [&] (const T& a, const T& b)
    {
        const int pa = pos (key (a)), pb = pos (key (b));
        return pa != pb ? pa < pb : value (a) > value (b);
    });
    for (bool swapped = true; swapped;)   // bubble up what is clearly bigger
    {
        swapped = false;
        for (size_t i = 1; i < items.size(); ++i)
            if (value (items[i]) > value (items[i - 1]) * 1.1 + margin)
            {
                std::swap (items[i], items[i - 1]);
                swapped = true;
            }
    }
    order.clear();
    for (auto& it : items) order.push_back (key (it));
}

//==============================================================================
/** Ranked share of a total: optional donut (total in the middle) and one bar per part, biggest first.
    Used by the Latency Finder (latency per component of a path) and the DSP load view.
    Clicking a part with a selectIndex selects that channel in the main window. */
class ShareChart final : public juce::Component, public juce::SettableTooltipClient
{
public:
    struct Part
    {
        juce::String key, label, detail;   // key: stable identity for the ranking (label if empty)
        double value = 0.0;
        juce::Colour colour;
        int selectIndex = -1;
    };

    explicit ShareChart (UiContext& c) : ctx (c) {}

    juce::String caption, subtitle, centreCaption = "total", emptyText = "Nothing to show.";
    bool donut = true, showShare = true, steady = false;
    std::function<juce::String (double)> format = [] (double v) { return juce::String (v, 2); };

    /** Replaces the parts (zero parts are left out); total = 100 % of the donut and the bars.
        Numbers show the new values at once, the ring and the bars glide there. */
    void setParts (std::vector<Part> parts, double total);
    const std::vector<Part>& getParts() const noexcept { return parts; }

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    const Part* partAt (juce::Point<int>) const;
    void frame (double timestamp);

    UiContext& ctx;
    std::vector<Part> parts;
    std::vector<double> shown;    // per part: value as drawn by the ring and the bars (glides to value)
    double totalShown = 0.0;
    FrameClock clock;
    bool animating = false;
    juce::VBlankAttachment vblank { this, [this] (double t) { frame (t); } };
    std::vector<juce::String> order;
    double total = 0.0;
    std::vector<std::pair<juce::Rectangle<int>, int>> rowAreas;   // painted rows -> part index
};
} // namespace hydra

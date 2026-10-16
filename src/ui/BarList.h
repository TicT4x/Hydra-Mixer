#pragma once
#include "Widgets.h"

namespace hydra
{
/** Scrolling list of stacked bars in sections (Latency Finder, DSP view): one row per signal path /
    channel, one coloured segment per component, scaled to the longest row. Clicking a row selects
    its channel in the main window. */
class BarList final : public juce::Component, public juce::SettableTooltipClient
{
public:
    struct Segment
    {
        juce::String label;   // "Input device", "Denoiser", plugin name...
        double value = 0.0;   // < 0: unknown (e.g. device offline), drawn as nothing
        juce::Colour colour;
    };
    struct Row
    {
        juce::String title, detail;   // detail replaces the total (e.g. "offline", "no source")
        juce::Colour colour;
        std::vector<Segment> segments;
        double total = 0.0;
        int selectIndex = -1;         // channel selected on click
        bool operator== (const Row&) const;
    };
    struct Section
    {
        juce::String title, subtitle, emptyText;
        std::vector<Row> rows;
    };

    explicit BarList (UiContext& c) : ctx (c) {}

    std::vector<Section> sections;
    double scale = 10.0;                      // value of a full-width bar
    double warnAbove = 1.0e9;                 // totals above this are drawn in the warning colour
    std::function<juce::String (double)> format = [] (double v) { return juce::String (v, 2); };

    /** Replaces the content; returns false (and does nothing) if nothing visible changed. */
    bool update (std::vector<Section> newSections, double newScale);
    int heightNeeded() const;
    static double sum (const std::vector<Segment>&);

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    static constexpr int kRowH = 30, kHeaderH = 40, kGap = 12, kLabelW = 300, kTotalW = 90;

private:
    const Row* rowAt (int y) const;
    void row (juce::Graphics&, const Row&, juce::Rectangle<int> area, bool odd);

    UiContext& ctx;
};

/** Legend for a BarList window header: colour swatches with names, right-aligned in area. */
void drawBarLegend (juce::Graphics&, juce::Rectangle<int> area, const std::vector<std::pair<juce::String, juce::Colour>>& items);
} // namespace hydra

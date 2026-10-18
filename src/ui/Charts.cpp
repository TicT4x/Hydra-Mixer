#include "Charts.h"

namespace hydra
{
void drawLegend (juce::Graphics& g, juce::Rectangle<int> legend, const std::vector<std::pair<juce::String, juce::Colour>>& items)
{
    int x = legend.getRight();
    g.setFont (theme::font (12.0f));
    for (auto it = items.rbegin(); it != items.rend(); ++it)
    {
        const int w = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), it->first);
        x -= w;
        g.setColour (theme::textDim);
        g.drawText (it->first, x, legend.getY(), w, legend.getHeight(), juce::Justification::centredLeft);
        x -= 18;
        g.setColour (it->second);
        g.fillRoundedRectangle ((float) x, (float) legend.getCentreY() - 5.0f, 12.0f, 10.0f, 2.5f);
        x -= 18;
    }
}

//==============================================================================
void ShareChart::setParts (std::vector<Part> newParts, double newTotal)
{
    newParts.erase (std::remove_if (newParts.begin(), newParts.end(), [] (const Part& p) { return p.value <= 0.0; }), newParts.end());
    for (auto& p : newParts) if (p.key.isEmpty()) p.key = p.label;
    if (steady)
        rankSteady (newParts, order, [] (const Part& p) { return p.key; }, [] (const Part& p) { return p.value; }, newTotal * 0.002);
    else
        std::stable_sort (newParts.begin(), newParts.end(), [] (const Part& a, const Part& b) { return a.value > b.value; });
    // The ring and the bars start where the same part was drawn before (new parts grow from zero).
    std::vector<double> from;
    for (auto& p : newParts)
    {
        double v = 0.0;
        for (size_t k = 0; k < parts.size() && k < shown.size(); ++k)
            if (parts[k].key == p.key) { v = shown[k]; break; }
        from.push_back (v);
    }
    if (parts.empty()) { from.clear(); for (auto& p : newParts) from.push_back (p.value); totalShown = newTotal; }
    parts = std::move (newParts);
    shown = std::move (from);
    total = newTotal;
    animating = true;
    repaint();
}

void ShareChart::frame (double timestamp)
{
    const double dt = clock.step (timestamp);
    if (! animating) return;
    const double eps = juce::jmax (1.0e-6, total * 0.001);
    bool moved = glide (totalShown, total, dt, eps);
    for (size_t k = 0; k < parts.size() && k < shown.size(); ++k)
        moved |= glide (shown[k], parts[k].value, dt, eps);
    animating = moved;
    if (moved) repaint();
}

void ShareChart::paint (juce::Graphics& g)
{
    rowAreas.clear();
    auto r = getLocalBounds();
    auto head = r.removeFromTop (30);
    g.setColour (theme::text);
    g.setFont (theme::font (14.0f, true));
    const int cw = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), caption);
    g.drawText (caption, head.removeFromLeft (cw + 12), juce::Justification::centredLeft);
    g.setColour (theme::textDim);
    g.setFont (theme::font (12.0f));
    g.drawFittedText (subtitle, head, juce::Justification::centredLeft, 1, 0.8f);
    r.removeFromTop (4);

    if (parts.empty() || total <= 0.0)
    {
        g.setColour (theme::textDim);
        g.setFont (theme::font (12.5f));
        g.drawText (emptyText, r.removeFromTop (24), juce::Justification::centredLeft);
        return;
    }

    if (donut)
    {
        const float d = (float) juce::jmin (r.getHeight(), 150, r.getWidth() / 3);
        auto pie = r.removeFromLeft ((int) d + 24).toFloat().withSizeKeepingCentre (d, d);
        const float thickness = d * 0.2f, radius = d * 0.5f - thickness * 0.5f;
        const float gap = parts.size() > 1 ? 0.012f : 0.0f;
        float angle = 0.0f;
        double shownSum = 0.0;
        for (auto v : shown) shownSum += v;
        const double ring = juce::jmax (shownSum, totalShown, 1.0e-12);   // segments always close the ring
        for (size_t k = 0; k < parts.size(); ++k)
        {
            const auto& p = parts[k];
            const float span = juce::MathConstants<float>::twoPi * (float) juce::jmin (1.0, (k < shown.size() ? shown[k] : p.value) / ring);
            juce::Path seg;
            seg.addCentredArc (pie.getCentreX(), pie.getCentreY(), radius, radius, 0.0f, angle + gap, angle + juce::jmax (gap, span - gap), true);
            g.setColour (p.colour);
            g.strokePath (seg, juce::PathStrokeType (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
            angle += span;
        }
        const bool roomy = d >= 100.0f;   // small donut: only the number in the middle
        g.setColour (theme::text);
        g.setFont (theme::mono (d > 120.0f ? 15.0f : roomy ? 13.0f : 11.0f));
        g.drawFittedText (format (total), pie.withSizeKeepingCentre (d - 2.0f * thickness - 4.0f, 20.0f).translated (0.0f, roomy ? -6.0f : 0.0f)
                                              .toNearestInt(), juce::Justification::centred, 1, 0.7f);
        if (roomy)
        {
            g.setColour (theme::textDim);
            g.setFont (theme::font (10.5f));
            g.drawText (centreCaption, pie.withSizeKeepingCentre (d - 2.0f * thickness, 14.0f).translated (0.0f, 10.0f),
                        juce::Justification::centred);
        }
        r.removeFromLeft (6);
    }

    // Ranked bars; what does not fit is summed up in the last row.
    const int rowH = juce::jlimit (18, 24, r.getHeight() / juce::jmax (1, (int) parts.size()));
    const int rowsShown = juce::jmin ((int) parts.size(), juce::jmax (1, r.getHeight() / rowH));
    // Narrow chart: no bars (the donut shows the shares), the label gets the room.
    const int valueW = showShare ? 132 : 60;
    const bool bars = r.getWidth() - 18 - valueW - 90 >= 70;
    const int labelW = bars ? juce::jlimit (90, 260, r.getWidth() * 2 / 5) : juce::jmax (30, r.getWidth() - 18 - valueW);
    for (int i = 0; i < rowsShown; ++i)
    {
        auto row = r.removeFromTop (rowH);
        const bool rest = i == rowsShown - 1 && rowsShown < (int) parts.size();
        const auto& p = parts[(size_t) i];
        double value = p.value, barValue = (size_t) i < shown.size() ? shown[(size_t) i] : p.value;
        juce::String label = p.label;
        juce::Colour colour = p.colour;
        if (rest)
        {
            value = barValue = 0.0;
            for (size_t k = (size_t) i; k < parts.size(); ++k) { value += parts[k].value; barValue += k < shown.size() ? shown[k] : parts[k].value; }
            label = juce::String ((int) parts.size() - i) + " more";
            colour = theme::textFaint;
        }
        else
            rowAreas.push_back ({ row, i });
        g.setColour (colour);
        g.fillRoundedRectangle (row.removeFromLeft (10).withSizeKeepingCentre (10, 10).toFloat(), 2.5f);
        row.removeFromLeft (8);
        g.setColour (theme::text);
        g.setFont (theme::font (12.5f));
        g.drawFittedText (label, row.removeFromLeft (labelW), juce::Justification::centredLeft, 1, 0.75f);
        auto values = row.removeFromRight (valueW);
        g.setFont (theme::mono (12.5f));
        g.setColour (showShare ? theme::textDim : theme::text);
        g.drawText (format (value), values.removeFromLeft (juce::jmin (70, values.getWidth())), juce::Justification::centredRight);
        if (showShare)
        {
            g.setColour (theme::text);
            g.setFont (theme::font (12.5f, true));
            g.drawText (juce::String (juce::roundToInt (100.0 * value / total)) + " %", values, juce::Justification::centredRight);
        }
        if (! bars) continue;
        row.removeFromRight (6);
        auto bar = row.withSizeKeepingCentre (row.getWidth(), juce::jmin (12, rowH - 6)).toFloat();
        g.setColour (theme::panel2);
        g.fillRoundedRectangle (bar, 3.0f);
        g.setColour (colour.withAlpha (0.9f));
        g.fillRoundedRectangle (bar.withWidth (juce::jmax (3.0f, bar.getWidth() * (float) juce::jmin (1.0, barValue / juce::jmax (totalShown, 1.0e-12)))), 3.0f);
    }
}

const ShareChart::Part* ShareChart::partAt (juce::Point<int> p) const
{
    for (auto& [area, index] : rowAreas)
        if (area.contains (p)) return &parts[(size_t) index];
    return nullptr;
}

void ShareChart::mouseMove (const juce::MouseEvent& e)
{
    const auto* p = partAt (e.getPosition());
    juce::String tip;
    if (p != nullptr)
    {
        tip << p->label;
        if (p->detail.isNotEmpty()) tip << "\n" << p->detail;
        tip << "\n" << format (p->value) << "  (" << juce::roundToInt (100.0 * p->value / juce::jmax (1.0e-9, total)) << " % of the total)";
        if (p->selectIndex >= 0) tip << "\nclick: show it in the mixer";
    }
    setTooltip (tip);
    setMouseCursor (p != nullptr && p->selectIndex >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
}

void ShareChart::mouseUp (const juce::MouseEvent& e)
{
    if (const auto* p = partAt (e.getPosition()); p != nullptr && p->selectIndex >= 0 && e.mouseWasClicked() && ctx.select)
        ctx.select (p->selectIndex);
}
} // namespace hydra

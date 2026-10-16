#include "BarList.h"

namespace hydra
{
bool BarList::Row::operator== (const Row& o) const
{
    if (title != o.title || detail != o.detail || segments.size() != o.segments.size() || std::abs (total - o.total) > 0.005)
        return false;
    for (size_t i = 0; i < segments.size(); ++i)
        if (segments[i].label != o.segments[i].label || std::abs (segments[i].value - o.segments[i].value) > 0.005) return false;
    return true;
}

double BarList::sum (const std::vector<Segment>& segs)
{
    double t = 0.0;
    for (auto& s : segs) t += juce::jmax (0.0, s.value);
    return t;
}

bool BarList::update (std::vector<Section> newSections, double newScale)
{
    bool changed = newSections.size() != sections.size() || std::abs (newScale - scale) > 0.01;
    for (size_t i = 0; ! changed && i < sections.size(); ++i)
        changed = newSections[i].title != sections[i].title || newSections[i].rows != sections[i].rows;
    if (! changed) return false;
    sections = std::move (newSections);
    scale = newScale;
    repaint();
    return true;
}

int BarList::heightNeeded() const
{
    int h = 0;
    for (auto& sec : sections)
        h += kHeaderH + juce::jmax (1, (int) sec.rows.size()) * kRowH + kGap;
    return h;
}

const BarList::Row* BarList::rowAt (int y) const
{
    int top = 0;
    for (auto& sec : sections)
    {
        top += kHeaderH;
        if (y >= top && y < top + (int) sec.rows.size() * kRowH) return &sec.rows[(size_t) ((y - top) / kRowH)];
        top += juce::jmax (1, (int) sec.rows.size()) * kRowH + kGap;
    }
    return nullptr;
}

void BarList::mouseMove (const juce::MouseEvent& e)
{
    juce::String tip;
    if (auto* r = rowAt (e.y))
    {
        tip = r->title + "\n";
        for (auto& s : r->segments) tip << s.label << ":  " << format (s.value) << "\n";
        tip << "Total:  " << format (r->total);
        if (r->selectIndex >= 0)
            tip << "\n(click: show " << (r->selectIndex == kRecorderSelect ? juce::String ("the recorder") : channelCode (r->selectIndex)) << " in the mixer)";
    }
    setTooltip (tip);
    const auto* r = rowAt (e.y);
    setMouseCursor (r != nullptr && r->selectIndex >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
}

void BarList::mouseUp (const juce::MouseEvent& e)
{
    if (auto* r = rowAt (e.y); r != nullptr && e.mouseWasClicked() && r->selectIndex >= 0 && ctx.select)
        ctx.select (r->selectIndex);
}

void BarList::paint (juce::Graphics& g)
{
    int y = 0;
    for (auto& sec : sections)
    {
        auto head = juce::Rectangle<int> (14, y, getWidth() - 28, kHeaderH);
        g.setColour (theme::text);
        g.setFont (theme::font (15.0f, true));
        const int tw = juce::GlyphArrangement::getStringWidthInt (theme::font (15.0f, true), sec.title);
        g.drawText (sec.title, head.withTrimmedTop (10), juce::Justification::centredLeft);
        g.setColour (theme::textFaint);
        g.setFont (theme::font (12.0f));
        g.drawText (sec.subtitle, head.withTrimmedTop (10).withTrimmedLeft (tw + 12), juce::Justification::centredLeft);
        y += kHeaderH;

        if (sec.rows.empty())
        {
            g.setColour (theme::textDim);
            g.setFont (theme::font (12.5f));
            g.drawText (sec.emptyText, juce::Rectangle<int> (14, y, getWidth() - 28, kRowH), juce::Justification::centredLeft);
            y += kRowH;
        }
        for (size_t i = 0; i < sec.rows.size(); ++i, y += kRowH)
            row (g, sec.rows[i], juce::Rectangle<int> (14, y, getWidth() - 28, kRowH), i % 2 == 1);
        y += kGap;
    }
}

void BarList::row (juce::Graphics& g, const Row& r, juce::Rectangle<int> area, bool odd)
{
    if (odd)
    {
        g.setColour (theme::panel2.withAlpha (0.6f));
        g.fillRoundedRectangle (area.toFloat(), 5.0f);
    }
    auto a = area.reduced (8, 0);
    auto label = a.removeFromLeft (kLabelW);
    g.setColour (r.colour);
    g.setFont (theme::font (13.0f, true));
    g.drawFittedText (r.title, label.withTrimmedRight (8), juce::Justification::centredLeft, 1, 0.85f);

    auto total = a.removeFromRight (kTotalW);
    g.setColour (r.total >= warnAbove ? theme::warn : theme::text);
    g.setFont (theme::mono (13.0f));
    g.drawText (r.detail.isNotEmpty() ? r.detail : format (r.total), total, juce::Justification::centredRight);
    a.removeFromRight (10);

    // Stacked bar: one segment per component, scaled to the longest row.
    if (r.segments.empty()) return;
    auto bar = a.withSizeKeepingCentre (a.getWidth(), 18).toFloat();
    g.setColour (odd ? theme::bg : theme::panel2);
    g.fillRoundedRectangle (bar, 4.0f);
    float x = bar.getX();
    const float pxPerUnit = bar.getWidth() / (float) juce::jmax (1.0e-6, scale);
    for (auto& s : r.segments)
    {
        if (s.value <= 0.0 || x >= bar.getRight()) continue;
        const float w = juce::jmax (2.0f, (float) s.value * pxPerUnit);
        auto seg = juce::Rectangle<float> (x, bar.getY(), juce::jmin (w, bar.getRight() - x), bar.getHeight());
        g.setColour (s.colour.withAlpha (0.85f));
        g.fillRoundedRectangle (seg.reduced (0.5f, 0.0f), 3.0f);
        const auto text = s.label + "  " + format (s.value);
        g.setFont (theme::font (11.0f, true));
        g.setColour (theme::bg);
        if (juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), text) + 10 < (int) seg.getWidth())
            g.drawText (text, seg.reduced (5.0f, 0.0f), juce::Justification::centredLeft, false);
        else if (seg.getWidth() > 26.0f)
            g.drawText (juce::String (s.value, 1), seg, juce::Justification::centred, false);
        x += w;
    }
}

void drawBarLegend (juce::Graphics& g, juce::Rectangle<int> legend, const std::vector<std::pair<juce::String, juce::Colour>>& items)
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
} // namespace hydra

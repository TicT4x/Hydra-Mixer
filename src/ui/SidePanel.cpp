#include "SidePanel.h"

namespace hydra
{
namespace
{
constexpr int kHeaderH = 44, kCodesH = 24, kPadX = 14, kMaxRowH = 30, kMinRowH = 18, kMaxColW = 40;
} // namespace

SidePanel::SidePanel (UiContext& c) : ctx (c)
{
    rebuild();
    startTimerHz (10);   // selection / names: repaints only on a change
}

SidePanel::~SidePanel() = default;

void SidePanel::rebuild()
{
    cells.clear();
    inputs.clear();
    buses.clear();
    for (int i = 0; i < kMaxInputs; ++i)
        if (ctx.engine.isInputActive (i)) inputs.push_back (i);
    for (int b = 0; b < kMaxBuses; ++b)
        if (ctx.engine.isBusActive (b)) buses.push_back (b);
    for (int i : inputs)
        for (int b : buses)
        {
            auto t = std::make_unique<Toggle> (ctx, *ctx.engine.inputs[(size_t) i].route[(size_t) b], juce::String(), theme::busColour (b));
            t->soft = true;
            t->setTooltip (channelCode (i) + " to " + busLabel (b) + ". Right-click: MIDI Learn / keyboard shortcut");
            addAndMakeVisible (*t);
            cells.push_back (std::move (t));
        }
    refresh();
    resized();
    repaint();
}

void SidePanel::refresh()
{
    juce::StringArray names;
    for (int i : inputs) names.add (ctx.engine.inputs[(size_t) i].name);
    if (names != shownNames)
    {
        shownNames = names;
        repaint();
    }
}

void SidePanel::timerCallback()
{
    if (ctx.selected != shownSelected)
    {
        shownSelected = ctx.selected;
        repaint();
    }
    refresh();
}

void SidePanel::resized()
{
    auto r = getLocalBounds().reduced (kPadX, 0);
    r.removeFromTop (kHeaderH + kCodesH);
    r.removeFromBottom (12);
    nameWidth = juce::jlimit (90, 150, r.getWidth() / 3);
    r.removeFromLeft (nameWidth);
    const int rows = (int) inputs.size(), cols = (int) buses.size();
    rowH = rows > 0 ? juce::jlimit (kMinRowH, kMaxRowH, r.getHeight() / rows) : kMaxRowH;
    colW = cols > 0 ? juce::jmin (kMaxColW, r.getWidth() / cols) : kMaxColW;
    grid = r.withWidth (colW * cols).withHeight (rowH * rows);
    for (int row = 0; row < rows; ++row)
        for (int col = 0; col < cols; ++col)
        {
            auto& t = *cells[(size_t) (row * cols + col)];
            const auto cell = juce::Rectangle<int> (grid.getX() + col * colW, grid.getY() + row * rowH, colW, rowH);
            t.setBounds (cell.reduced (3, juce::jmax (2, (rowH - 20) / 2)));
            t.setVisible (cell.getBottom() <= getHeight() - 4);   // too many inputs for the height: the rest is cut
        }
}

void SidePanel::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (2.0f, 2.0f);
    const float corner = theme::deep() ? 4.0f : 10.0f;
    g.setColour (theme::panel);
    g.fillRoundedRectangle (r, corner);
    if (! theme::deep())
    {
        g.setColour (theme::outline);
        g.drawRoundedRectangle (r, corner, 1.0f);
    }

    auto head = getLocalBounds().reduced (kPadX + 4, 0).removeFromTop (kHeaderH);
    g.setColour (theme::text);
    g.setFont (theme::font (15.0f, true));
    g.drawText ("Routing", head, juce::Justification::centredLeft);
    g.setColour (theme::textFaint);
    g.setFont (theme::font (12.0f));
    g.drawText ("inputs to buses", head.withTrimmedLeft (78), juce::Justification::centredLeft);

    // Bus codes above the columns, a faint line between A and V buses
    g.setFont (theme::font (12.0f, true));
    for (size_t col = 0; col < buses.size(); ++col)
    {
        const int b = buses[col];
        const juce::Rectangle<int> code (grid.getX() + (int) col * colW, grid.getY() - kCodesH, colW, kCodesH);
        g.setColour (theme::busColour (b));
        g.drawText (busLabel (b), code, juce::Justification::centred);
        if (col > 0 && isHwBus (buses[col - 1]) && ! isHwBus (b))
        {
            g.setColour (theme::outline);
            g.fillRect (code.getX(), grid.getY() - 4, 1, grid.getHeight() + 8);
        }
    }

    // Input names; the selected input is highlighted across its row
    for (size_t row = 0; row < inputs.size(); ++row)
    {
        const int i = inputs[row];
        const juce::Rectangle<int> line (kPadX, grid.getY() + (int) row * rowH, nameWidth + grid.getWidth(), rowH);
        if (line.getBottom() > getHeight() - 4) break;
        if (i == ctx.selected)
        {
            g.setColour (theme::accentIn.withAlpha (0.10f));
            g.fillRoundedRectangle (line.toFloat().reduced (-4.0f, 1.0f), 4.0f);
        }
        auto name = line.withWidth (nameWidth).reduced (2, 0);
        g.setColour (theme::accentIn);
        g.setFont (theme::font (11.5f, true));
        const juce::String code (i + 1);
        g.drawText (code, name.removeFromLeft (20), juce::Justification::centredLeft);
        g.setColour (i == ctx.selected ? theme::text : theme::textDim);
        g.setFont (theme::font (12.5f));
        const auto& n = ctx.engine.inputs[(size_t) i].name;
        g.drawFittedText (n.isNotEmpty() ? n : channelCode (i), name.withTrimmedRight (6), juce::Justification::centredLeft, 1, 0.85f);
    }

    if (inputs.empty() || buses.empty())
    {
        g.setColour (theme::textFaint);
        g.setFont (theme::font (12.5f));
        g.drawFittedText ("Add inputs and outputs to route them here.", getLocalBounds().reduced (kPadX + 4, kHeaderH),
                          juce::Justification::topLeft, 3);
    }
}

int SidePanel::rowAt (int y) const
{
    if (rowH <= 0 || y < grid.getY()) return -1;
    const int row = (y - grid.getY()) / rowH;
    return row < (int) inputs.size() ? row : -1;
}

void SidePanel::mouseUp (const juce::MouseEvent& e)
{
    if (! e.mouseWasClicked() || e.x > grid.getX()) return;
    if (const int row = rowAt (e.y); row >= 0 && ctx.select)
        ctx.select (inputs[(size_t) row]);
}
} // namespace hydra

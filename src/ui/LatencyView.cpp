#include "LatencyView.h"

namespace hydra
{
namespace
{
const juce::Colour kDeviceIn = theme::accentIn;
const juce::Colour kDenoiser = theme::good;
const juce::Colour kPlugin   = theme::warn;

double samplesToMs (const Engine& e, int samples) { return 1000.0 * samples / juce::jmax (1.0, e.getSampleRate()); }

/** Plugins of a chain; neighbouring plugins get slightly different shades so they stay apart in the chart. */
void addPlugins (const Engine& e, std::vector<LatencyView::Node>& nodes, const PluginChain& chain, const Param& bypassAll, int sel)
{
    if (bypassAll.isOn()) return;
    int k = 0;
    for (int i = 0; i < chain.size(); ++i)
        if (auto* s = chain.getSlot (i); s != nullptr && ! s->bypassed)
            nodes.push_back ({ s->instance->getName(), "VST3 plugin", samplesToMs (e, s->instance->getLatencySamples()),
                               kPlugin.interpolatedWith (theme::solo, 0.3f * (float) (k++ % 3)), sel });
}

double sum (const std::vector<LatencyView::Node>& nodes)
{
    double t = 0.0;
    for (auto& n : nodes) t += juce::jmax (0.0, n.ms);
    return t;
}

juce::String ms (double v) { return v < 0.0 ? juce::String ("offline") : juce::String (v, v < 10.0 ? 2 : 1) + " ms"; }
juce::String percent (double part, double total)
{
    return total > 0.0 ? juce::String (juce::roundToInt (100.0 * juce::jmax (0.0, part) / total)) + " %" : juce::String ("0 %");
}
/** "A1  Speakers"; just "A3" while the channel still has its default name. */
juce::String titled (const juce::String& code, const juce::String& name)
{
    return name.isEmpty() || name == code ? code : code + "  " + name;
}
juce::String inputTitle (const Engine& e, int i) { return titled ("IN " + juce::String (i + 1), e.inputs[(size_t) i].name); }
juce::String busTitle (const Engine& e, int b)   { return titled (busCode (b), e.buses[(size_t) b].name); }
} // namespace

juce::String LatencyView::Flow::signature() const
{
    juce::String s;
    auto add = [&s] (const std::vector<Node>& nodes)
    {
        for (auto& n : nodes) s << n.label << '|' << n.detail << '|' << juce::String (n.ms, 2) << ';';
    };
    s << bus << '/' << busTitle << '/' << problem << '/' << focus << '#';
    for (auto& l : lanes) { s << l.input << l.title << ':'; add (l.nodes); s << '#'; }
    add (busNodes);
    return s;
}

//==============================================================================
/** Row of buttons, one per output in use; the picked one is filled. */
class LatencyView::OutputPicker final : public juce::Component
{
public:
    explicit OutputPicker (UiContext& c) : ctx (c) {}
    std::function<void (int)> onPick;
    int current = -1;

    void update (int picked)
    {
        std::vector<Item> next;
        for (int b = 0; b < kMaxBuses; ++b)
            if (ctx.engine.isBusActive (b))
                next.push_back ({ b, busTitle (ctx.engine, b), ctx.engine.buses[(size_t) b].output.kind != IoTarget::Kind::None, {} });
        if (picked == current && next.size() == items.size()
            && std::equal (next.begin(), next.end(), items.begin(), [] (const Item& a, const Item& b)
                           { return a.bus == b.bus && a.text == b.text && a.hasOutput == b.hasOutput; }))
            return;
        items = std::move (next);
        current = picked;
        resized();
        repaint();
    }

    void resized() override
    {
        // Buttons as wide as their text (up to 180 px); squeezed evenly when they do not fit.
        auto r = getLocalBounds();
        r.removeFromLeft (labelW);
        const auto f = theme::font (12.5f, true);
        int wanted = 0;
        for (auto& it : items) wanted += juce::jlimit (64, 180, juce::GlyphArrangement::getStringWidthInt (f, it.text) + 24) + 6;
        const float squeeze = wanted > r.getWidth() ? (float) r.getWidth() / (float) wanted : 1.0f;
        float x = (float) r.getX();
        for (auto& it : items)
        {
            const float w = (float) (juce::jlimit (64, 180, juce::GlyphArrangement::getStringWidthInt (f, it.text) + 24) + 6) * squeeze;
            it.area = juce::Rectangle<float> (x, (float) r.getY(), w - 6.0f, (float) r.getHeight()).toNearestInt();
            x += w;
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (theme::textDim);
        g.setFont (theme::font (12.5f));
        g.drawText ("Output", getLocalBounds().withWidth (labelW - 8), juce::Justification::centredLeft);
        for (auto& it : items)
        {
            const bool on = it.bus == current, over = it.bus == hover;
            const auto colour = theme::busColour (it.bus);
            auto r = it.area.toFloat().reduced (0.5f);
            g.setColour (on ? colour : over ? theme::raised.brighter (0.08f) : theme::panel2);
            g.fillRoundedRectangle (r, 6.0f);
            if (! on)
            {
                g.setColour (theme::outline);
                g.drawRoundedRectangle (r, 6.0f, 1.0f);
            }
            g.setColour (on ? theme::bg : it.hasOutput ? theme::text : theme::textFaint);
            g.setFont (theme::font (12.5f, true));
            g.drawFittedText (it.text, it.area.reduced (8, 0), juce::Justification::centred, 1, 0.75f);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int b = busAt (e.getPosition());
        if (b != hover) { hover = b; repaint(); }
        setMouseCursor (b >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    }
    void mouseExit (const juce::MouseEvent&) override { hover = -1; repaint(); }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (const int b = busAt (e.getPosition()); b >= 0 && e.mouseWasClicked() && onPick) onPick (b);
    }

private:
    struct Item { int bus; juce::String text; bool hasOutput; juce::Rectangle<int> area; };
    int busAt (juce::Point<int> p) const
    {
        for (auto& it : items) if (it.area.contains (p)) return it.bus;
        return -1;
    }

    static constexpr int labelW = 64;
    UiContext& ctx;
    std::vector<Item> items;
    int hover = -1;
};

//==============================================================================
/** The signal flow of one output: input lanes merging into the bus, then bus plugins and output device. */
class LatencyView::FlowView final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit FlowView (UiContext& c) : ctx (c) {}
    std::function<void (int lane)> onFocus;
    Flow flow;

    static constexpr int kPad = 14, kCaptionW = 150, kLaneH = 66, kBoxH = 54, kGap = 28, kTotalW = 110;

    /** Lays the boxes out for the available width; returns the size the view needs. */
    juce::Point<int> layout (int availableWidth)
    {
        size_t laneNodes = 0;
        for (auto& l : flow.lanes) laneNodes = juce::jmax (laneNodes, l.nodes.size());
        const int columns = (int) laneNodes + 1 + (int) flow.busNodes.size();   // + the bus itself
        const int fixed = 2 * kPad + kCaptionW + kTotalW;
        boxW = juce::jlimit (112, 170, (availableWidth - fixed) / juce::jmax (1, columns) - kGap);
        busX = kPad + kCaptionW + (int) laneNodes * (boxW + kGap);
        const int lanes = juce::jmax (1, (int) flow.lanes.size());
        // The bus sits in the middle of its first lanes (at most three, so it stays near the top).
        busY = kPad + (juce::jmin (lanes, 3) - 1) * kLaneH / 2 + (kLaneH - kBoxH) / 2;
        const int width = busX + (1 + (int) flow.busNodes.size()) * (boxW + kGap) - kGap + kTotalW + kPad;
        return { juce::jmax (availableWidth, width), kPad * 2 + lanes * kLaneH + (flow.lanes.empty() ? 30 : 0) };
    }

    void paint (juce::Graphics& g) override
    {
        const Lane* focused = flow.lanes.empty() ? nullptr : &flow.lanes[(size_t) flow.focus];
        const double focusTotal = (focused != nullptr ? focused->ms : 0.0) + flow.busMs;

        if (flow.lanes.empty())
        {
            g.setColour (theme::textDim);
            g.setFont (theme::font (12.5f));
            g.drawText (flow.problem, juce::Rectangle<int> (kPad, kPad, getWidth() - 2 * kPad, 24), juce::Justification::centredLeft);
        }

        // Input lanes (the focused one in full colour, the others dimmed)
        for (size_t i = 0; i < flow.lanes.size(); ++i)
        {
            const auto& lane = flow.lanes[i];
            const bool isFocus = (int) i == flow.focus;
            const auto row = laneRow ((int) i);
            if (isFocus)
            {
                g.setColour (theme::accentIn.withAlpha (0.06f));
                g.fillRoundedRectangle (row.withRight (busX - kGap / 2).toFloat(), 8.0f);
            }
            if (! isFocus) g.beginTransparencyLayer (0.55f);

            auto cap = juce::Rectangle<int> (row.getX() + 8, row.getY(), kCaptionW - 16, row.getHeight()).reduced (0, 12);
            g.setColour (theme::accentIn);
            g.setFont (theme::font (13.0f, true));
            g.drawFittedText (lane.title, cap.removeFromTop (cap.getHeight() / 2), juce::Justification::bottomLeft, 1, 0.8f);
            g.setColour (theme::textDim);
            g.setFont (theme::font (11.5f));
            g.drawText ("path " + ms (lane.ms + flow.busMs), cap, juce::Justification::topLeft);

            const double total = lane.ms + flow.busMs;
            double running = 0.0;
            for (size_t k = 0; k < lane.nodes.size(); ++k)
            {
                const auto box = laneBox ((int) i, (int) k);
                if (k > 0) arrow (g, laneBox ((int) i, (int) k - 1).getRight(), box.getX(), box.getCentreY());
                running += juce::jmax (0.0, lane.nodes[k].ms);
                drawNode (g, lane.nodes[k], box, total, running);
            }
            // Into the bus
            if (! lane.nodes.empty())
            {
                const auto last = laneBox ((int) i, (int) lane.nodes.size() - 1);
                const auto bus = busBox (-1);
                juce::Path p;
                const float x0 = (float) last.getRight(), y0 = (float) last.getCentreY();
                const float x1 = (float) bus.getX() - 2.0f, y1 = (float) bus.getCentreY();
                p.startNewSubPath (x0, y0);
                p.cubicTo (x0 + (x1 - x0) * 0.6f, y0, x1 - (x1 - x0) * 0.4f, y1, x1, y1);
                g.setColour (theme::textFaint);
                g.strokePath (p, juce::PathStrokeType (1.4f));
            }
            if (! isFocus) g.endTransparencyLayer();
        }

        // The bus (mix point), its plugins and the output device; totals refer to the focused path.
        const auto busRect = busBox (-1);
        if (! flow.lanes.empty())
        {
            juce::Path head;
            const float x = (float) busRect.getX(), y = (float) busRect.getCentreY();
            head.addTriangle (x - 7.0f, y - 4.5f, x - 7.0f, y + 4.5f, x - 1.0f, y);
            g.setColour (theme::textFaint);
            g.fillPath (head);
        }
        drawBus (g, busRect);
        double running = focused != nullptr ? focused->ms : 0.0;
        for (size_t k = 0; k < flow.busNodes.size(); ++k)
        {
            const auto box = busBox ((int) k);
            arrow (g, busBox ((int) k - 1).getRight(), box.getX(), box.getCentreY());
            running += juce::jmax (0.0, flow.busNodes[k].ms);
            drawNode (g, flow.busNodes[k], box, focusTotal, running);
        }

        // Total of the focused path
        if (! flow.hasOutput) return;
        auto totalArea = juce::Rectangle<int> (busBox ((int) flow.busNodes.size() - 1).getRight() + 14, busRect.getY(),
                                               kTotalW - 14, kBoxH);
        bool offline = false;
        for (auto& n : flow.busNodes) offline |= n.ms < 0.0;
        if (focused != nullptr) for (auto& n : focused->nodes) offline |= n.ms < 0.0;
        g.setColour (theme::textDim);
        g.setFont (theme::font (11.5f));
        g.drawText (focused != nullptr ? "total" : "output side", totalArea.removeFromTop (20), juce::Justification::bottomLeft);
        g.setColour (offline ? theme::mute : focusTotal >= 20.0 ? theme::warn : theme::text);
        g.setFont (theme::mono (18.0f));
        g.drawText (offline ? juce::String ("offline") : ms (focusTotal), totalArea, juce::Justification::topLeft);
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        juce::String tip;
        const Node* node = nullptr;
        double total = 0.0;
        if (auto hit = hitTestNode (e.getPosition()); hit.lane >= 0 || hit.busNode >= 0)
        {
            if (hit.busNode >= 0)
            {
                node = &flow.busNodes[(size_t) hit.busNode];
                total = flow.busMs + (flow.lanes.empty() ? 0.0 : flow.lanes[(size_t) flow.focus].ms);
            }
            else if (hit.node >= 0)
            {
                node = &flow.lanes[(size_t) hit.lane].nodes[(size_t) hit.node];
                total = flow.lanes[(size_t) hit.lane].ms + flow.busMs;
            }
            if (node != nullptr)
                tip << node->label << "\n" << node->detail << "\nadds " << ms (node->ms) << "  (" << percent (node->ms, total)
                    << " of this path)";
            else if (hit.lane >= 0)
                tip << flow.lanes[(size_t) hit.lane].title << ": click to show this path in the chart below";
        }
        setTooltip (tip);
        setMouseCursor (tip.isNotEmpty() ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! e.mouseWasClicked()) return;
        const auto hit = hitTestNode (e.getPosition());
        if (hit.lane >= 0)
        {
            if (onFocus) onFocus (hit.lane);
            if (ctx.select) ctx.select (flow.lanes[(size_t) hit.lane].input);
        }
        else if ((hit.busNode >= 0 || hit.onBus) && ctx.select)
            ctx.select (busSelectIndex (flow.bus));
    }

private:
    struct Hit { int lane = -1, node = -1, busNode = -1; bool onBus = false; };

    Hit hitTestNode (juce::Point<int> p) const
    {
        Hit h;
        for (size_t i = 0; i < flow.lanes.size(); ++i)
            if (laneRow ((int) i).withRight (busX - kGap / 2).contains (p))
            {
                h.lane = (int) i;
                for (size_t k = 0; k < flow.lanes[i].nodes.size(); ++k)
                    if (laneBox ((int) i, (int) k).contains (p)) h.node = (int) k;
                return h;
            }
        if (busBox (-1).contains (p)) h.onBus = true;
        for (size_t k = 0; k < flow.busNodes.size(); ++k)
            if (busBox ((int) k).contains (p)) h.busNode = (int) k;
        return h;
    }

    juce::Rectangle<int> laneRow (int i) const { return { kPad, kPad + i * kLaneH, getWidth() - 2 * kPad, kLaneH }; }
    juce::Rectangle<int> laneBox (int i, int k) const
    {
        return { kPad + kCaptionW + k * (boxW + kGap), kPad + i * kLaneH + (kLaneH - kBoxH) / 2, boxW, kBoxH };
    }
    /** k = -1: the bus itself, then its plugins and the output device. */
    juce::Rectangle<int> busBox (int k) const
    {
        const int y = flow.lanes.empty() ? kPad + 30 : busY;
        return { busX + (k + 1) * (boxW + kGap), y, boxW, kBoxH };
    }

    static void arrow (juce::Graphics& g, int x0, int x1, int y)
    {
        g.setColour (theme::textFaint);
        g.drawLine ((float) x0 + 2.0f, (float) y, (float) x1 - 3.0f, (float) y, 1.4f);
        juce::Path p;
        p.addTriangle ((float) x1 - 8.0f, (float) y - 4.5f, (float) x1 - 8.0f, (float) y + 4.5f, (float) x1 - 2.0f, (float) y);
        g.fillPath (p);
    }

    void box (juce::Graphics& g, juce::Rectangle<int> r, juce::Colour colour, bool zero) const
    {
        auto f = r.toFloat();
        g.setColour (theme::panel2);
        g.fillRoundedRectangle (f, 7.0f);
        g.setColour (zero ? theme::outline : colour.withAlpha (0.55f));
        g.drawRoundedRectangle (f.reduced (0.5f), 7.0f, 1.0f);
        g.setColour (zero ? colour.withAlpha (0.35f) : colour);
        g.fillRoundedRectangle (f.removeFromLeft (4.0f).reduced (0.0f, 8.0f), 2.0f);
    }

    /** Box of one stage: what it is, the latency it adds and its share of the path, the running total. */
    void drawNode (juce::Graphics& g, const Node& n, juce::Rectangle<int> r, double total, double running) const
    {
        const bool zero = n.ms == 0.0;
        box (g, r, n.colour, zero);
        auto a = r.reduced (12, 5);
        g.setColour (zero ? theme::textDim : theme::text);
        g.setFont (theme::font (12.0f, true));
        g.drawFittedText (n.label, a.removeFromTop (16), juce::Justification::centredLeft, 1, 0.8f);

        auto line = a.removeFromTop (18);
        if (n.ms < 0.0)
        {
            g.setColour (theme::mute);
            g.setFont (theme::font (12.0f, true));
            g.drawText ("offline", line, juce::Justification::centredLeft);
            return;
        }
        g.setColour (zero ? theme::textFaint : n.colour);
        g.setFont (theme::mono (13.0f));
        g.drawText ("+" + ms (n.ms), line, juce::Justification::centredLeft);
        g.setColour (zero ? theme::textFaint : theme::text);
        g.setFont (theme::font (12.0f, true));
        g.drawText (percent (n.ms, total), line, juce::Justification::centredRight);

        g.setColour (theme::textFaint);
        g.setFont (theme::font (10.5f));
        g.drawFittedText ("at " + ms (running), a, juce::Justification::centredLeft, 1, 0.8f);
    }

    void drawBus (juce::Graphics& g, juce::Rectangle<int> r) const
    {
        const auto colour = theme::busColour (flow.bus);
        auto f = r.toFloat();
        g.setColour (colour.withAlpha (0.16f));
        g.fillRoundedRectangle (f, 7.0f);
        g.setColour (colour.withAlpha (0.8f));
        g.drawRoundedRectangle (f.reduced (0.5f), 7.0f, 1.2f);
        auto a = r.reduced (10, 6);
        g.setColour (colour);
        g.setFont (theme::font (13.0f, true));
        g.drawFittedText (flow.busTitle, a.removeFromTop (22), juce::Justification::centredLeft, 1, 0.75f);
        g.setColour (theme::textDim);
        g.setFont (theme::font (11.0f));
        g.drawFittedText (flow.lanes.size() > 1 ? juce::String ((int) flow.lanes.size()) + " inputs mixed, +0 ms" : juce::String ("mix, +0 ms"),
                          a, juce::Justification::centredLeft, 1, 0.8f);
    }

    UiContext& ctx;
    int boxW = 140, busX = 0, busY = 0;
};

//==============================================================================
/** Chart of the focused path: donut with the total in the middle, components ranked by their share. */
class LatencyView::Breakdown final : public juce::Component
{
public:
    juce::String title;
    std::vector<Node> nodes;
    double total = 0.0;

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().reduced (18, 0);
        g.setColour (theme::outline.withAlpha (0.7f));
        g.fillRect (r.removeFromTop (1));
        auto head = r.removeFromTop (34);
        g.setColour (theme::text);
        g.setFont (theme::font (14.0f, true));
        const juce::String caption = "Latency share";
        const int cw = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), caption);
        g.drawText (caption, head.removeFromLeft (cw + 12), juce::Justification::centredLeft);
        g.setColour (theme::textDim);
        g.setFont (theme::font (12.0f));
        g.drawFittedText (title, head, juce::Justification::centredLeft, 1, 0.8f);
        r.removeFromBottom (12);

        std::vector<const Node*> parts;
        for (auto& n : nodes) if (n.ms > 0.0) parts.push_back (&n);
        if (parts.empty() || total <= 0.0)
        {
            g.setColour (theme::textDim);
            g.setFont (theme::font (12.5f));
            g.drawText (nodes.empty() ? "Nothing to show." : "No latency on this path.", r.removeFromTop (24), juce::Justification::centredLeft);
            return;
        }
        std::stable_sort (parts.begin(), parts.end(), [] (const Node* a, const Node* b) { return a->ms > b->ms; });

        // Donut
        const float d = (float) juce::jmin (r.getHeight(), 150);
        auto pie = r.removeFromLeft ((int) d + 30).toFloat().withSizeKeepingCentre (d, d);
        float angle = 0.0f;
        const float thickness = d * 0.2f;
        for (auto* n : parts)
        {
            const float span = juce::MathConstants<float>::twoPi * (float) (n->ms / total);
            juce::Path seg;
            seg.addCentredArc (pie.getCentreX(), pie.getCentreY(), d * 0.5f - thickness * 0.5f, d * 0.5f - thickness * 0.5f, 0.0f,
                               angle + (parts.size() > 1 ? 0.012f : 0.0f), angle + span - (parts.size() > 1 ? 0.012f : 0.0f), true);
            g.setColour (n->colour);
            g.strokePath (seg, juce::PathStrokeType (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
            angle += span;
        }
        g.setColour (theme::text);
        g.setFont (theme::mono (d > 110.0f ? 15.0f : 12.5f));
        g.drawText (ms (total), pie.withSizeKeepingCentre (d - 2.0f * thickness, 20.0f).translated (0.0f, -6.0f),
                    juce::Justification::centred);
        g.setColour (theme::textDim);
        g.setFont (theme::font (10.5f));
        g.drawText ("total", pie.withSizeKeepingCentre (d - 2.0f * thickness, 14.0f).translated (0.0f, 10.0f),
                    juce::Justification::centred);

        // Ranked bars
        r.removeFromLeft (10);
        const int rowH = juce::jlimit (16, 24, r.getHeight() / juce::jmax (1, (int) parts.size()));
        const int rowsShown = juce::jmin ((int) parts.size(), r.getHeight() / rowH);
        const int labelW = juce::jlimit (120, 260, r.getWidth() / 3), valueW = 150;
        for (int i = 0; i < rowsShown; ++i)
        {
            auto row = r.removeFromTop (rowH);
            const bool last = i == rowsShown - 1 && rowsShown < (int) parts.size();
            double partMs = parts[(size_t) i]->ms;
            juce::String label = parts[(size_t) i]->label;
            juce::Colour colour = parts[(size_t) i]->colour;
            if (last)   // the rest in one row
            {
                partMs = 0.0;
                for (size_t k = (size_t) i; k < parts.size(); ++k) partMs += parts[k]->ms;
                label = juce::String ((int) parts.size() - i) + " more";
                colour = theme::textFaint;
            }
            g.setColour (colour);
            g.fillRoundedRectangle (row.removeFromLeft (10).withSizeKeepingCentre (10, 10).toFloat(), 2.5f);
            row.removeFromLeft (8);
            g.setColour (theme::text);
            g.setFont (theme::font (12.5f));
            g.drawFittedText (label, row.removeFromLeft (labelW), juce::Justification::centredLeft, 1, 0.8f);
            auto value = row.removeFromRight (valueW);
            g.setFont (theme::mono (12.5f));
            g.setColour (theme::textDim);
            g.drawText (ms (partMs), value.removeFromLeft (80), juce::Justification::centredRight);
            g.setColour (theme::text);
            g.setFont (theme::font (12.5f, true));
            g.drawText (percent (partMs, total), value, juce::Justification::centredRight);
            row.removeFromRight (6);
            auto bar = row.withSizeKeepingCentre (row.getWidth(), juce::jmin (12, rowH - 6)).toFloat();
            g.setColour (theme::panel2);
            g.fillRoundedRectangle (bar, 3.0f);
            g.setColour (colour.withAlpha (0.9f));
            g.fillRoundedRectangle (bar.withWidth (juce::jmax (3.0f, bar.getWidth() * (float) (partMs / total))), 3.0f);
        }
    }
};

//==============================================================================
LatencyView::LatencyView (UiContext& c) : ctx (c)
{
    picker = std::make_unique<OutputPicker> (c);
    flowView = std::make_unique<FlowView> (c);
    breakdown = std::make_unique<Breakdown>();
    picker->onPick = [this] (int b) { bus = b; focusInput = -1; refresh(); };
    flowView->onFocus = [this] (int lane)
    {
        if (juce::isPositiveAndBelow (lane, (int) flowView->flow.lanes.size()))
            focusInput = flowView->flow.lanes[(size_t) lane].input;
        refresh();
    };
    viewport.setViewedComponent (flowView.get(), false);
    viewport.setScrollBarsShown (false, false, true, true);
    viewport.setScrollBarThickness (10);
    addAndMakeVisible (picker.get());
    addAndMakeVisible (viewport);
    addAndMakeVisible (breakdown.get());
    refresh();
    startTimerHz (4);
    setSize (1000, 640);
}

LatencyView::~LatencyView() { stopTimer(); }

void LatencyView::timerCallback() { refresh(); }

LatencyView::Flow LatencyView::build (int b) const
{
    auto& e = ctx.engine;
    Flow f;
    f.bus = b;
    if (b < 0) { f.problem = "No output in use."; return f; }
    const auto& bus = e.buses[(size_t) b];
    f.busTitle = busTitle (e, b);

    addPlugins (e, f.busNodes, bus.plugins, *bus.fxBypass, busSelectIndex (b));
    f.hasOutput = bus.output.kind != IoTarget::Kind::None;
    if (f.hasOutput)
        f.busNodes.push_back ({ "Output device", bus.output.describe(), e.getIoLatencyMs (bus.output, false),
                                theme::busColour (b), busSelectIndex (b) });
    f.busMs = sum (f.busNodes);

    for (int i = 0; i < kMaxInputs; ++i)
    {
        if (! e.isInputActive (i)) continue;
        const auto& s = e.inputs[(size_t) i];
        if (s.source.kind == IoTarget::Kind::None || ! s.route[(size_t) b]->isOn()) continue;
        Lane lane;
        lane.input = i;
        lane.title = inputTitle (e, i);
        lane.nodes.push_back ({ "Input device", s.source.describe(), e.getIoLatencyMs (s.source, true), kDeviceIn, i });
        if (s.dnOn->isOn())
        {
            const int mode = juce::roundToInt (s.dnMode->get());
            lane.nodes.push_back ({ "Denoiser", "RNNoise, " + s.dnMode->choices[mode] + " mode",
                                    samplesToMs (e, s.dspLatency.load()), kDenoiser, i });
        }
        addPlugins (e, lane.nodes, s.plugins, *s.fxBypass, i);
        lane.ms = sum (lane.nodes);
        f.lanes.push_back (std::move (lane));
    }
    std::stable_sort (f.lanes.begin(), f.lanes.end(), [] (const Lane& x, const Lane& y) { return x.ms > y.ms; });

    if (! f.hasOutput)          f.problem = busCode (b) + " has no output device - choose one in the mixer.";
    else if (f.lanes.empty())   f.problem = "No input is routed to " + busCode (b) + ".";
    for (size_t k = 0; k < f.lanes.size(); ++k)
        if (f.lanes[k].input == focusInput) f.focus = (int) k;   // else the longest path (first)
    return f;
}

int LatencyView::defaultBus() const
{
    // The output with the longest path; without routed inputs the first output in use.
    int best = -1, first = -1;
    double longest = -1.0;
    for (int b = 0; b < kMaxBuses; ++b)
    {
        if (! ctx.engine.isBusActive (b)) continue;
        if (first < 0) first = b;
        const auto f = build (b);
        if (f.lanes.empty() || ! f.hasOutput) continue;
        if (const double t = f.lanes.front().ms + f.busMs; t > longest) { longest = t; best = b; }
    }
    return best >= 0 ? best : first;
}

void LatencyView::refresh()
{
    auto& e = ctx.engine;
    if (bus < 0 || ! e.isBusActive (bus)) { bus = defaultBus(); focusInput = -1; }
    picker->update (bus);

    auto flow = build (bus);
    juce::String text;
    if (auto* d = e.getMasterDevice())
        text << "Master: " << d->getName() << "  ·  " << (int) e.getSampleRate() << " Hz  ·  " << d->getCurrentBufferSizeSamples()
             << " samples  ·  I/O " << ms (e.getMasterLatencyMs()) << "   (input + output buffers and converter latency)";
    else
        text = "Audio engine stopped - open Settings > Audio Engine.";

    const auto sig = flow.signature() + text;
    if (sig == shownSignature) return;
    shownSignature = sig;
    summary = text;

    // Chart: the focused path (input side + output side)
    breakdown->nodes.clear();
    if (! flow.lanes.empty())
    {
        const auto& lane = flow.lanes[(size_t) flow.focus];
        breakdown->nodes = lane.nodes;
        breakdown->title = lane.title + "   ->   " + flow.busTitle;
    }
    else
        breakdown->title = flow.busTitle + "  (output side only)";
    for (auto& n : flow.busNodes) breakdown->nodes.push_back (n);
    breakdown->total = sum (breakdown->nodes);

    flowView->flow = std::move (flow);
    resized();
    flowView->repaint();
    breakdown->repaint();
    repaint();
}

void LatencyView::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);
    auto head = getLocalBounds().reduced (18, 0).removeFromTop (64);
    g.setColour (theme::text);
    g.setFont (theme::font (15.0f, true));
    g.drawText ("Latency Finder", head.removeFromTop (36).withTrimmedTop (8), juce::Justification::centredLeft);
    g.setColour (theme::textDim);
    g.setFont (theme::font (12.0f));
    g.drawFittedText (summary, head.removeFromTop (20), juce::Justification::centredLeft, 1, 0.8f);

    // Legend
    auto legend = getLocalBounds().reduced (18, 0).removeFromTop (36).withTrimmedTop (8);
    int x = legend.getRight();
    g.setFont (theme::font (12.0f));
    for (auto [name, colour] : { std::pair<const char*, juce::Colour> { "Output device", theme::busColour (juce::jmax (0, bus)) },
                                 { "Plugin", kPlugin }, { "Denoiser", kDenoiser }, { "Input device", kDeviceIn } })
    {
        const int w = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), name);
        x -= w;
        g.setColour (theme::textDim);
        g.drawText (name, x, legend.getY(), w, legend.getHeight(), juce::Justification::centredLeft);
        x -= 18;
        g.setColour (colour);
        g.fillRoundedRectangle ((float) x, (float) legend.getCentreY() - 5.0f, 12.0f, 10.0f, 2.5f);
        x -= 18;
    }
}

void LatencyView::resized()
{
    auto r = getLocalBounds().withTrimmedTop (68);
    picker->setBounds (r.removeFromTop (30).reduced (18, 0));
    r.removeFromTop (8);
    breakdown->setBounds (r.removeFromBottom (juce::jlimit (120, 220, getHeight() * 33 / 100)));
    viewport.setBounds (r);
    const auto size = flowView->layout (r.getWidth());
    const bool scrollsDown = size.y > r.getHeight();
    const auto fitted = flowView->layout (r.getWidth() - (scrollsDown ? viewport.getScrollBarThickness() : 0));
    flowView->setSize (fitted.x, fitted.y);
}
} // namespace hydra

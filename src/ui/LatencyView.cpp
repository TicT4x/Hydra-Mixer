#include "LatencyView.h"

namespace hydra
{
namespace
{
const juce::Colour kDeviceIn = theme::accentIn;
const juce::Colour kDenoiser = theme::good;
const juce::Colour kPlugin   = theme::warn;
const juce::Colour kSync     = theme::accentVirt;

double samplesToMs (const Engine& e, int samples) { return 1000.0 * samples / juce::jmax (1.0, e.getSampleRate()); }

/** Plugins of a chain; neighbouring plugins get slightly different shades so they stay apart in the chart. */
void addPlugins (const Engine& e, std::vector<LatencyView::Node>& nodes, const PluginChain& chain, const Param& bypassAll, int sel)
{
    if (bypassAll.isOn()) return;
    int k = 0;
    for (int i = 0; i < chain.size(); ++i)
        if (auto* s = chain.getSlot (i); s != nullptr && ! s->bypassed)
            nodes.push_back ({ s->instance->getName(), "VST3 plugin", samplesToMs (e, s->instance->getLatencySamples()),
                               kPlugin.interpolatedWith (theme::solo, 0.3f * (float) (k++ % 3)), sel,
                               "VST3 plugin: the latency it reports itself (look-ahead, linear phase, ...).\n"
                               "A zero-latency / live mode of the plugin or bypassing it removes it." });
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
juce::String channelTitle (const Engine& e, int ch) { return ch < kMaxInputs ? inputTitle (e, ch) : busTitle (e, ch - kMaxInputs); }

/** Sync Delay stage of a channel (input index or Engine::busChannel), if it has one. */
void addSyncDelay (const Engine& e, std::vector<LatencyView::Node>& nodes, int ch, int sel)
{
    const double total = e.getChannelDelayMs (ch);
    if (total <= 0.0) return;
    const auto& st = e.getSyncState (ch);
    const double manual = total - st.autoMs;
    juce::String ref;
    for (int k = 0; k < kMaxInputs + kMaxBuses; ++k)
        if (const auto& o = e.getSyncState (k); o.reference && o.group == st.group) ref = channelTitle (e, k);

    juce::String detail, why;
    if (st.autoMs > 0.0)
    {
        detail = manual > 0.005 ? "auto + manual" : "auto";
        why << "Added on purpose (Tools > Sync Delay): this channel is faster than " << ref
            << ", so it waits " << ms (st.autoMs) << " to line up with it (Auto sync).";
        if (manual > 0.005) why << "\nManual delay on top: " << ms (manual) << ".";
    }
    else
    {
        detail = "manual";
        why << "Added on purpose (Tools > Sync Delay): manual delay of this channel.";
    }
    nodes.push_back ({ "Sync delay", detail, total, kSync, sel, why });
}
} // namespace

juce::String explainIoLatency (const Engine& e, const IoTarget& t, bool input)
{
    const auto l = e.getIoLatency (t, input);
    juce::String s;
    s << (input ? "Input device: " : "Output device: ") << t.describe();
    if (! l.online) return s << "\nOffline - no latency to show.";
    auto frames = [&l] (double msValue) { return juce::String (juce::roundToInt (msValue * l.deviceRate / 1000.0)) + " samples"; };
    if (l.masterClock)
    {
        s << "\nMaster device: it runs the engine's clock, so no clock sync is needed."
          << "\n   Buffer:  " << ms (l.bufferMs) << "  (" << l.bufferFrames << " samples at " << (int) l.deviceRate << " Hz)"
          << "\n   Driver / converters:  " << ms (l.driverMs) << "  (" << frames (l.driverMs) << ", as the driver reports it)"
          << "\nSmaller buffer = less latency (Settings > Audio Engine), but more CPU and a higher risk of dropouts.";
    }
    else
    {
        const bool asio = t.kind == IoTarget::Kind::Device && t.typeName == kAsioTypeName;
        s << "\nSecondary device: it has its own clock, bound to the master clock by a resampler"
          << "\nwith drift control (" << (asio ? "ASIO clock sync" : "clock sync") << ")."
          << "\n   Device buffer + driver:  " << ms (l.driverMs) << "  (period " << l.bufferFrames << " samples at "
          << (int) l.deviceRate << " Hz)"
          << "\n   Clock sync buffer:  " << ms (l.clockSyncMs) << "  (keeps the two clocks from running dry; moves a little)"
          << "\n" << (asio ? "A smaller buffer in the driver's control panel reduces both."
                           : "The device's ASIO driver, Exclusive or Low Latency mode reduce it.");
    }
    return s;
}

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
/** Input x output matrix: the total latency of every routed path; click a cell to show that path. */
class LatencyView::Matrix final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit Matrix (UiContext& c) : ctx (c) {}
    std::function<void (int input, int bus)> onPick;

    std::vector<int> inputs, buses;
    std::vector<juce::String> inTitles;
    std::vector<std::vector<Cell>> cells;   // [row][column]
    double longest = 0.0;
    int selIn = -1, selBus = -1;

    static constexpr int kHeadW = 190, kColW = 96, kRowH = 30, kHeadH = 30, kPad = 4;

    juce::Point<int> needed() const
    {
        return { 2 * kPad + kHeadW + (int) buses.size() * kColW, 2 * kPad + kHeadH + juce::jmax (1, (int) inputs.size()) * kRowH };
    }

    void paint (juce::Graphics& g) override
    {
        // Column heads: the outputs
        for (size_t c = 0; c < buses.size(); ++c)
        {
            const auto r = cellRect (-1, (int) c);
            const bool sel = buses[c] == selBus;
            g.setColour (theme::busColour (buses[c]).withAlpha (sel ? 1.0f : 0.75f));
            g.setFont (theme::font (12.5f, true));
            g.drawFittedText (titled (busCode (buses[c]), ctx.engine.buses[(size_t) buses[c]].name), r.reduced (4, 0),
                              juce::Justification::centred, 1, 0.7f);
        }
        if (inputs.empty())
        {
            g.setColour (theme::textDim);
            g.setFont (theme::font (12.5f));
            g.drawText ("No input in use.", cellRect (0, -1).withWidth (300), juce::Justification::centredLeft);
        }
        for (size_t row = 0; row < inputs.size(); ++row)
        {
            const auto head = cellRect ((int) row, -1);
            if (row % 2 == 0)
            {
                g.setColour (theme::panel.withAlpha (0.7f));
                g.fillRoundedRectangle (head.withRight (cellRect ((int) row, (int) buses.size() - 1).getRight()).toFloat(), 6.0f);
            }
            g.setColour (inputs[row] == selIn ? theme::accentIn : theme::text);
            g.setFont (theme::font (12.5f, true));
            g.drawFittedText (inTitles[row], head.reduced (10, 0), juce::Justification::centredLeft, 1, 0.75f);

            for (size_t c = 0; c < buses.size(); ++c)
                drawCell (g, cells[row][c], cellRect ((int) row, (int) c), inputs[row] == selIn && buses[c] == selBus);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        int row = -1, col = -1;
        hit (e.getPosition(), row, col);
        juce::String tip;
        if (row >= 0 && col >= 0)
        {
            const auto& cell = cells[(size_t) row][(size_t) col];
            const auto bus = busCode (buses[(size_t) col]);
            if (! cell.routed) tip = inTitles[(size_t) row] + " is not routed to " + bus;
            else tip = inTitles[(size_t) row] + "  ->  " + bus + ":  " + label (cell.ms) + "\nclick: show this path below";
        }
        setTooltip (tip);
        setMouseCursor (row >= 0 && col >= 0 && cells[(size_t) row][(size_t) col].routed ? juce::MouseCursor::PointingHandCursor
                                                                                         : juce::MouseCursor::NormalCursor);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        int row = -1, col = -1;
        hit (e.getPosition(), row, col);
        if (e.mouseWasClicked() && row >= 0 && col >= 0 && cells[(size_t) row][(size_t) col].routed && onPick)
            onPick (inputs[(size_t) row], buses[(size_t) col]);
    }

private:
    static juce::String label (double v)
    {
        if (v <= -2.0) return "no output";
        if (v < 0.0)   return "offline";
        return ms (v);
    }

    juce::Rectangle<int> cellRect (int row, int col) const
    {
        const int x = col < 0 ? kPad : kPad + kHeadW + col * kColW;
        const int y = row < 0 ? kPad : kPad + kHeadH + row * kRowH;
        return { x, y, col < 0 ? kHeadW : kColW, row < 0 ? kHeadH : kRowH };
    }

    void hit (juce::Point<int> p, int& row, int& col) const
    {
        for (size_t r = 0; r < inputs.size(); ++r)
            for (size_t c = 0; c < buses.size(); ++c)
                if (cellRect ((int) r, (int) c).contains (p)) { row = (int) r; col = (int) c; }
    }

    void drawCell (juce::Graphics& g, const Cell& cell, juce::Rectangle<int> r, bool selected) const
    {
        auto f = r.toFloat().reduced (3.0f, 3.0f);
        if (! cell.routed)
        {
            g.setColour (theme::textFaint.withAlpha (0.5f));
            g.fillEllipse (f.getCentreX() - 1.5f, f.getCentreY() - 1.5f, 3.0f, 3.0f);
            return;
        }
        const bool ok = cell.ms >= 0.0;
        const float heat = ok && longest > 0.0 ? (float) juce::jlimit (0.0, 1.0, cell.ms / longest) : 0.0f;
        const auto tone = ! ok ? theme::mute : cell.ms >= 20.0 ? theme::warn : theme::good.interpolatedWith (theme::warn, heat * 0.6f);
        g.setColour (theme::panel2.interpolatedWith (tone, 0.10f + 0.25f * heat));
        g.fillRoundedRectangle (f, 5.0f);
        g.setColour (selected ? theme::accentIn : tone.withAlpha (0.35f));
        g.drawRoundedRectangle (f, 5.0f, selected ? 1.8f : 1.0f);
        g.setColour (ok ? theme::text : theme::mute);
        g.setFont (ok ? theme::mono (12.5f) : theme::font (11.5f, true));
        g.drawText (label (cell.ms), f, juce::Justification::centred);
    }

    UiContext& ctx;
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
            {
                tip << node->label << "  -  adds " << ms (node->ms) << "  (" << percent (node->ms, total) << " of this path)\n";
                tip << (node->why.isNotEmpty() ? node->why : node->detail);
            }
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
LatencyView::LatencyView (UiContext& c) : ctx (c)
{
    matrix = std::make_unique<Matrix> (c);
    flowView = std::make_unique<FlowView> (c);
    breakdown = std::make_unique<ShareChart> (c);
    breakdown->caption = "Latency share";
    breakdown->emptyText = "No latency on this path.";
    breakdown->format = [] (double v) { return ms (v); };
    matrix->onPick = [this] (int input, int b)
    {
        bus = b;
        focusInput = input;
        userPicked = true;
        if (ctx.select) ctx.select (input);
        refresh();
    };
    flowView->onFocus = [] (int) {};
    matrixView.setViewedComponent (matrix.get(), false);
    matrixView.setScrollBarsShown (false, false, true, true);
    matrixView.setScrollBarThickness (10);
    viewport.setViewedComponent (flowView.get(), false);
    viewport.setScrollBarsShown (false, false, true, true);
    viewport.setScrollBarThickness (10);
    addAndMakeVisible (matrixView);
    addAndMakeVisible (viewport);
    addAndMakeVisible (breakdown.get());
    refresh();
    startTimerHz (10);
    setSize (1000, 680);
}

LatencyView::~LatencyView() { stopTimer(); }

void LatencyView::timerCallback() { refresh(); }

void LatencyView::measureDevices()
{
    // The device latency includes the drift buffer's fill level, which moves a little all the time.
    // Sampled 10x per second; the shown value is the mean of the last second, rewritten once per second.
    // A big jump (other device, offline / back) is shown at once.
    auto& e = ctx.engine;
    const double now = juce::Time::getMillisecondCounterHiRes();
    const bool publish = now - lastPublish >= kLabelPeriodMs;
    if (publish) lastPublish = now;
    auto sample = [publish] (std::map<int, double>& store, std::map<int, Mean>& means, int key, double v)
    {
        auto& mean = means[key];
        auto it = store.find (key);
        if (v < 0.0 || it == store.end() || it->second < 0.0 || std::abs (v - it->second) > 5.0)
        {
            store[key] = v;
            mean.reset();
            if (v >= 0.0) mean.add (v);
            return;
        }
        mean.add (v);
        if (publish) it->second = mean.take (v);
    };
    for (int i = 0; i < kMaxInputs; ++i)
    {
        const auto& s = e.inputs[(size_t) i];
        if (! e.isInputActive (i) || s.source.kind == IoTarget::Kind::None) { inDevMs.erase (i); inMeans.erase (i); continue; }
        sample (inDevMs, inMeans, i, e.getIoLatencyMs (s.source, true));
    }
    for (int b = 0; b < kMaxBuses; ++b)
    {
        const auto& bus = e.buses[(size_t) b];
        if (! e.isBusActive (b) || bus.output.kind == IoTarget::Kind::None) { outDevMs.erase (b); outMeans.erase (b); continue; }
        sample (outDevMs, outMeans, b, e.getIoLatencyMs (bus.output, false));
    }
}

double LatencyView::inputSideMs (int i) const
{
    const auto it = inDevMs.find (i);
    if (it == inDevMs.end() || it->second < 0.0) return -1.0;
    const auto& e = ctx.engine;
    const auto& s = e.inputs[(size_t) i];
    std::vector<Node> nodes;
    addPlugins (e, nodes, s.plugins, *s.fxBypass, i);
    return it->second + (s.dnOn->isOn() ? samplesToMs (e, s.dspLatency.load()) : 0.0) + sum (nodes) + e.getChannelDelayMs (i);
}

double LatencyView::busSideMs (int b) const
{
    const auto it = outDevMs.find (b);
    if (it == outDevMs.end()) return -2.0;   // no output device
    if (it->second < 0.0) return -1.0;
    const auto& e = ctx.engine;
    const auto& bus = e.buses[(size_t) b];
    std::vector<Node> nodes;
    addPlugins (e, nodes, bus.plugins, *bus.fxBypass, busSelectIndex (b));
    return it->second + sum (nodes) + e.getChannelDelayMs (Engine::busChannel (b));
}

LatencyView::Flow LatencyView::build (int b) const
{
    auto& e = ctx.engine;
    Flow f;
    f.bus = b;
    if (b < 0) { f.problem = "No path: route an input to an output with a device."; return f; }
    const auto& bus = e.buses[(size_t) b];
    f.busTitle = busTitle (e, b);

    addPlugins (e, f.busNodes, bus.plugins, *bus.fxBypass, busSelectIndex (b));
    addSyncDelay (e, f.busNodes, Engine::busChannel (b), busSelectIndex (b));
    f.hasOutput = bus.output.kind != IoTarget::Kind::None;
    if (f.hasOutput)
    {
        const auto it = outDevMs.find (b);
        f.busNodes.push_back ({ "Output device", bus.output.describe(), it != outDevMs.end() ? it->second : -1.0,
                                theme::busColour (b), busSelectIndex (b), explainIoLatency (e, bus.output, false) });
    }
    f.busMs = sum (f.busNodes);

    // Only the picked input: the matrix above shows all of them.
    for (int i = 0; i < kMaxInputs; ++i)
    {
        if (i != focusInput || ! e.isInputActive (i)) continue;
        const auto& s = e.inputs[(size_t) i];
        if (s.source.kind == IoTarget::Kind::None || ! s.route[(size_t) b]->isOn()) continue;
        Lane lane;
        lane.input = i;
        lane.title = inputTitle (e, i);
        const auto it = inDevMs.find (i);
        lane.nodes.push_back ({ "Input device", s.source.describe(), it != inDevMs.end() ? it->second : -1.0, kDeviceIn, i,
                                explainIoLatency (e, s.source, true) });
        if (s.dnOn->isOn())
        {
            const int mode = juce::roundToInt (s.dnMode->get());
            lane.nodes.push_back ({ "Denoiser", "RNNoise, " + s.dnMode->choices[mode] + " mode",
                                    samplesToMs (e, s.dspLatency.load()), kDenoiser, i,
                                    "RNNoise, " + s.dnMode->choices[mode] + " mode. It analyses the sound in 10 ms frames;\n"
                                    "the latency mode sets how far it looks ahead (Ultra Low 0 ms ... Native HQ 30 ms):\n"
                                    "more look-ahead = cleaner result." });
        }
        addPlugins (e, lane.nodes, s.plugins, *s.fxBypass, i);
        addSyncDelay (e, lane.nodes, i, i);
        lane.ms = sum (lane.nodes);
        f.lanes.push_back (std::move (lane));
    }

    if (! f.hasOutput)          f.problem = busCode (b) + " has no output device - choose one in the mixer.";
    else if (f.lanes.empty())   f.problem = "No input is routed to " + busCode (b) + ".";
    return f;
}

void LatencyView::refresh()
{
    auto& e = ctx.engine;
    measureDevices();

    // Matrix: every input in use x every output in use
    matrix->inputs.clear();
    matrix->buses.clear();
    matrix->inTitles.clear();
    matrix->cells.clear();
    for (int b = 0; b < kMaxBuses; ++b)
        if (e.isBusActive (b)) matrix->buses.push_back (b);
    double longest = -1.0;
    int longestIn = -1, longestBus = -1;
    bool pickedValid = false;
    for (int i = 0; i < kMaxInputs; ++i)
    {
        if (! e.isInputActive (i)) continue;
        const auto& s = e.inputs[(size_t) i];
        matrix->inputs.push_back (i);
        matrix->inTitles.push_back (inputTitle (e, i));
        const double in = inputSideMs (i);
        std::vector<Cell> row;
        for (int b : matrix->buses)
        {
            Cell cell;
            cell.routed = s.source.kind != IoTarget::Kind::None && s.route[(size_t) b]->isOn();
            if (cell.routed)
            {
                const double out = busSideMs (b);
                cell.ms = out <= -2.0 ? -2.0 : (in < 0.0 || out < 0.0) ? -1.0 : in + out;
                if (cell.ms > longest) { longest = cell.ms; longestIn = i; longestBus = b; }
                pickedValid |= i == focusInput && b == bus;
            }
            row.push_back (cell);
        }
        matrix->cells.push_back (std::move (row));
    }
    if (! userPicked || ! pickedValid)
    {
        userPicked = userPicked && pickedValid;
        focusInput = longestIn;
        bus = longestBus;
    }
    matrix->longest = juce::jmax (0.0, longest);
    matrix->selIn = focusInput;
    matrix->selBus = bus;

    auto flow = build (bus);
    juce::String text;
    if (auto* d = e.getMasterDevice())
        text << "Master: " << d->getName() << "  ·  " << (int) e.getSampleRate() << " Hz  ·  " << d->getCurrentBufferSizeSamples()
             << " samples  ·  I/O " << ms (e.getMasterLatencyMs()) << "   (input + output buffers and converter latency)";
    else
        text = "Audio engine stopped - open Settings > Audio Engine.";

    // Repaint only when something visible changed (values as shown).
    juce::String sig = flow.signature() + text;
    for (size_t r = 0; r < matrix->cells.size(); ++r)
    {
        sig << '|' << matrix->inTitles[r];
        for (auto& c : matrix->cells[r]) sig << (c.routed ? ms (c.ms) : juce::String ("-")) << ',';
    }
    for (int b : matrix->buses) sig << busTitle (e, b);
    if (sig == shownSignature) return;
    shownSignature = sig;
    summary = text;

    // Chart: the shown path (input side + output side)
    std::vector<Node> path;
    if (! flow.lanes.empty())
    {
        const auto& lane = flow.lanes.front();
        path = lane.nodes;
        breakdown->subtitle = lane.title + "   ->   " + flow.busTitle;
    }
    else
        breakdown->subtitle = flow.bus >= 0 ? flow.busTitle + "  (output side only)" : juce::String();
    for (auto& n : flow.busNodes) path.push_back (n);
    std::vector<ShareChart::Part> parts;
    for (size_t k = 0; k < path.size(); ++k)
        parts.push_back ({ juce::String ((int) k), path[k].label, path[k].detail, path[k].ms, path[k].colour, path[k].selectIndex });
    breakdown->setParts (std::move (parts), sum (path));

    flowView->flow = std::move (flow);
    resized();
    matrix->repaint();
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

    // Captions above the matrix and the path, separator above the chart
    g.setColour (theme::textDim);
    g.setFont (theme::font (11.5f, true));
    g.drawText ("TOTAL LATENCY PER INPUT AND OUTPUT  (click a cell)", matrixView.getBounds().translated (0, -20).withHeight (18),
                juce::Justification::centredLeft);
    g.drawText ("PATH", viewport.getBounds().translated (0, -20).withHeight (18), juce::Justification::centredLeft);
    g.setColour (theme::outline.withAlpha (0.7f));
    g.fillRect (breakdown->getBounds().getX(), breakdown->getY() - 8, breakdown->getWidth(), 1);

    drawLegend (g, getLocalBounds().reduced (18, 0).removeFromTop (36).withTrimmedTop (8),
                   { { "Input device", kDeviceIn }, { "Denoiser", kDenoiser }, { "Plugin", kPlugin }, { "Sync delay", kSync },
                     { "Output device", theme::busColour (juce::jmax (0, bus)) } });
}

void LatencyView::resized()
{
    auto r = getLocalBounds().withTrimmedTop (68).reduced (18, 0);
    breakdown->setBounds (r.removeFromBottom (juce::jlimit (120, 200, getHeight() * 28 / 100)).withTrimmedBottom (12));
    r.removeFromBottom (16);

    // Path: one lane
    const auto flowSize = flowView->layout (r.getWidth());
    r.removeFromTop (22);
    const auto need = matrix->needed();
    const int matrixH = juce::jmin (need.y + (need.x > r.getWidth() ? matrixView.getScrollBarThickness() : 0),
                                    juce::jmax (80, r.getHeight() - flowSize.y - 34));
    matrixView.setBounds (r.removeFromTop (matrixH));
    matrix->setSize (juce::jmax (need.x, r.getWidth() - (need.y > matrixH ? matrixView.getScrollBarThickness() : 0)), need.y);
    r.removeFromTop (30);
    viewport.setBounds (r);
    const auto fitted = flowView->layout (r.getWidth() - (flowSize.y > r.getHeight() ? viewport.getScrollBarThickness() : 0));
    flowView->setSize (fitted.x, fitted.y);
}
} // namespace hydra

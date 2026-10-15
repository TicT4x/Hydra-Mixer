#include "LatencyView.h"

namespace hydra
{
namespace
{
const juce::Colour kDeviceIn  = theme::accentIn;
const juce::Colour kDenoiser  = theme::good;
const juce::Colour kPlugin    = theme::warn;
const juce::Colour kDeviceOut = theme::accentHw;

double samplesToMs (Engine& e, int samples) { return 1000.0 * samples / juce::jmax (1.0, e.getSampleRate()); }

void addPlugins (Engine& e, std::vector<LatencyView::Segment>& segs, const PluginChain& chain, const Param& bypassAll)
{
    if (bypassAll.isOn()) return;
    for (int i = 0; i < chain.size(); ++i)
        if (auto* s = chain.getSlot (i); s != nullptr && ! s->bypassed)
            segs.push_back ({ s->instance->getName(), samplesToMs (e, s->instance->getLatencySamples()), kPlugin });
}

double sum (const std::vector<LatencyView::Segment>& segs)
{
    double t = 0.0;
    for (auto& s : segs) t += juce::jmax (0.0, s.ms);
    return t;
}

juce::String ms (double v) { return v < 0.0 ? juce::String ("offline") : juce::String (v, v < 10.0 ? 2 : 1) + " ms"; }
} // namespace

bool LatencyView::Row::operator== (const Row& o) const
{
    if (title != o.title || detail != o.detail || segments.size() != o.segments.size() || std::abs (total - o.total) > 0.005)
        return false;
    for (size_t i = 0; i < segments.size(); ++i)
        if (segments[i].label != o.segments[i].label || std::abs (segments[i].ms - o.segments[i].ms) > 0.005) return false;
    return true;
}

std::vector<LatencyView::Segment> LatencyView::inputSegments (Engine& e, int i)
{
    const auto& s = e.inputs[(size_t) i];
    std::vector<Segment> segs;
    segs.push_back ({ "Input device", e.getIoLatencyMs (s.source, true), kDeviceIn });
    if (s.dnOn->isOn())
        segs.push_back ({ "Denoiser", samplesToMs (e, s.dspLatency.load()), kDenoiser });
    addPlugins (e, segs, s.plugins, *s.fxBypass);
    return segs;
}

std::vector<LatencyView::Segment> LatencyView::busSegments (Engine& e, int b)
{
    const auto& bus = e.buses[(size_t) b];
    std::vector<Segment> segs;
    addPlugins (e, segs, bus.plugins, *bus.fxBypass);
    segs.push_back ({ "Output device", e.getIoLatencyMs (bus.output, false), kDeviceOut });
    return segs;
}

//==============================================================================
/** The rows: section headers, then one bar per path / channel. */
class LatencyView::List final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit List (UiContext& c) : ctx (c) {}

    std::vector<Row> paths, channels;
    double scaleMs = 10.0;   // full bar width

    static constexpr int kRowH = 30, kHeaderH = 40, kLabelW = 300, kTotalW = 90;

    int heightNeeded() const
    {
        return kHeaderH + juce::jmax (1, (int) paths.size()) * kRowH + 12 + kHeaderH + (int) channels.size() * kRowH + 12;
    }

    void paint (juce::Graphics& g) override
    {
        int y = 0;
        y = section (g, y, "Signal paths", "input -> bus -> output, longest first", paths,
                     "No routed path: route an input to a bus that has an output device.");
        y += 12;
        section (g, y, "Channels", "each input and output on its own", channels, {});
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        juce::String tip;
        if (auto* r = rowAt (e.y))
        {
            tip = r->title + "\n";
            for (auto& s : r->segments) tip << s.label << ":  " << ms (s.ms) << "\n";
            tip << "Total:  " << ms (r->total);
            if (r->selectIndex >= 0) tip << "\n(click: show " << channelCode (r->selectIndex) << " in the mixer)";
        }
        setTooltip (tip);
        setMouseCursor (rowAt (e.y) != nullptr ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (auto* r = rowAt (e.y); r != nullptr && e.mouseWasClicked() && r->selectIndex >= 0 && ctx.select)
            ctx.select (r->selectIndex);
    }

private:
    const Row* rowAt (int y) const
    {
        const int pathTop = kHeaderH;
        if (y >= pathTop && y < pathTop + (int) paths.size() * kRowH) return &paths[(size_t) ((y - pathTop) / kRowH)];
        const int chanTop = kHeaderH + juce::jmax (1, (int) paths.size()) * kRowH + 12 + kHeaderH;
        if (y >= chanTop && y < chanTop + (int) channels.size() * kRowH) return &channels[(size_t) ((y - chanTop) / kRowH)];
        return nullptr;
    }

    int section (juce::Graphics& g, int y, const juce::String& title, const juce::String& sub, const std::vector<Row>& rows,
                 const juce::String& emptyText)
    {
        auto head = juce::Rectangle<int> (14, y, getWidth() - 28, kHeaderH);
        g.setColour (theme::text);
        g.setFont (theme::font (15.0f, true));
        const int tw = juce::GlyphArrangement::getStringWidthInt (theme::font (15.0f, true), title);
        g.drawText (title, head.withTrimmedTop (10), juce::Justification::centredLeft);
        g.setColour (theme::textFaint);
        g.setFont (theme::font (12.0f));
        g.drawText (sub, head.withTrimmedTop (10).withTrimmedLeft (tw + 12), juce::Justification::centredLeft);
        y += kHeaderH;

        if (rows.empty())
        {
            g.setColour (theme::textDim);
            g.setFont (theme::font (12.5f));
            g.drawText (emptyText, juce::Rectangle<int> (14, y, getWidth() - 28, kRowH), juce::Justification::centredLeft);
            return y + kRowH;
        }
        for (size_t i = 0; i < rows.size(); ++i, y += kRowH)
            row (g, rows[i], juce::Rectangle<int> (14, y, getWidth() - 28, kRowH), i % 2 == 1);
        return y;
    }

    void row (juce::Graphics& g, const Row& r, juce::Rectangle<int> area, bool odd)
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
        g.setColour (r.total >= 20.0 ? theme::warn : theme::text);
        g.setFont (theme::mono (13.0f));
        g.drawText (r.detail.isNotEmpty() ? r.detail : ms (r.total), total, juce::Justification::centredRight);
        a.removeFromRight (10);

        // Stacked bar: one segment per component, scaled to the longest path.
        if (r.segments.empty()) return;
        auto bar = a.withSizeKeepingCentre (a.getWidth(), 18).toFloat();
        g.setColour (odd ? theme::bg : theme::panel2);
        g.fillRoundedRectangle (bar, 4.0f);
        float x = bar.getX();
        const float pxPerMs = bar.getWidth() / (float) juce::jmax (1.0, scaleMs);
        for (auto& s : r.segments)
        {
            if (s.ms <= 0.0) continue;
            const float w = juce::jmax (2.0f, (float) s.ms * pxPerMs);
            auto seg = juce::Rectangle<float> (x, bar.getY(), juce::jmin (w, bar.getRight() - x), bar.getHeight());
            g.setColour (s.colour.withAlpha (0.85f));
            g.fillRoundedRectangle (seg.reduced (0.5f, 0.0f), 3.0f);
            const auto text = s.label + "  " + ms (s.ms);
            g.setFont (theme::font (11.0f, true));
            if (juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), text) + 10 < (int) seg.getWidth())
            {
                g.setColour (theme::bg);
                g.drawText (text, seg.reduced (5.0f, 0.0f), juce::Justification::centredLeft, false);
            }
            else if (seg.getWidth() > 26.0f)
            {
                g.setColour (theme::bg);
                g.drawText (juce::String (s.ms, 1), seg, juce::Justification::centred, false);
            }
            x += w;
        }
    }

    UiContext& ctx;
};

//==============================================================================
LatencyView::LatencyView (UiContext& c) : ctx (c)
{
    list = std::make_unique<List> (c);
    viewport.setViewedComponent (list.get(), false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (10);
    addAndMakeVisible (viewport);
    refresh();
    startTimerHz (4);
    setSize (1000, 640);
}

LatencyView::~LatencyView() { stopTimer(); }

void LatencyView::timerCallback() { refresh(); }

void LatencyView::refresh()
{
    auto& e = ctx.engine;
    std::vector<Row> paths, channels;

    for (int i = 0; i < kMaxInputs; ++i)
    {
        if (! e.isInputActive (i)) continue;
        const auto& s = e.inputs[(size_t) i];
        Row in;
        in.title = "IN " + juce::String (i + 1) + "   " + s.name;
        in.colour = theme::accentIn;
        in.selectIndex = i;
        const bool hasSource = s.source.kind != IoTarget::Kind::None;
        in.segments = hasSource ? inputSegments (e, i) : std::vector<Segment>();
        in.total = sum (in.segments);
        if (! hasSource) in.detail = "no source";
        else if (! in.segments.empty() && in.segments.front().ms < 0.0) in.detail = "offline";

        if (hasSource)
            for (int b = 0; b < kMaxBuses; ++b)
            {
                const auto& bus = e.buses[(size_t) b];
                if (! e.isBusActive (b) || ! s.route[(size_t) b]->isOn() || bus.output.kind == IoTarget::Kind::None) continue;
                Row p;
                p.title = "IN " + juce::String (i + 1) + " " + s.name + "   ->   " + busCode (b) + " " + bus.name;
                p.colour = theme::text;
                p.selectIndex = i;
                p.segments = in.segments;
                for (auto& seg : busSegments (e, b)) p.segments.push_back (seg);
                p.total = sum (p.segments);
                for (auto& seg : p.segments) if (seg.ms < 0.0) p.detail = "offline";
                paths.push_back (std::move (p));
            }
        channels.push_back (std::move (in));
    }
    for (int b = 0; b < kMaxBuses; ++b)
    {
        if (! e.isBusActive (b)) continue;
        const auto& bus = e.buses[(size_t) b];
        Row r;
        r.title = busCode (b) + "   " + bus.name;
        r.colour = theme::busColour (b);
        r.selectIndex = busSelectIndex (b);
        const bool hasOutput = bus.output.kind != IoTarget::Kind::None;
        r.segments = busSegments (e, b);
        if (! hasOutput) r.segments.pop_back();
        r.total = sum (r.segments);
        if (! hasOutput) r.detail = "no output";
        else if (r.segments.back().ms < 0.0) r.detail = "offline";
        channels.push_back (std::move (r));
    }
    std::stable_sort (paths.begin(), paths.end(), [] (const Row& a, const Row& b) { return a.total > b.total; });

    double longest = 10.0;
    for (auto* rows : { &paths, &channels })
        for (auto& r : *rows) longest = juce::jmax (longest, r.total);

    juce::String text;
    if (auto* d = e.getMasterDevice())
        text << "Master: " << d->getName() << "  ·  " << (int) e.getSampleRate() << " Hz  ·  " << d->getCurrentBufferSizeSamples()
            << " samples  ·  I/O " << ms (e.getMasterLatencyMs()) << "   (input + output buffers and converter latency)";
    else
        text = "Audio engine stopped - open Settings > Audio Engine.";

    const bool changed = ! (paths == list->paths) || ! (channels == list->channels) || text != summary
                         || std::abs (longest - list->scaleMs) > 0.01;
    if (! changed) return;
    list->paths = std::move (paths);
    list->channels = std::move (channels);
    list->scaleMs = longest;
    summary = text;
    resized();
    list->repaint();
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
    for (auto [name, colour] : { std::pair<const char*, juce::Colour> { "Output device", kDeviceOut }, { "Plugin", kPlugin },
                                 { "Denoiser", kDenoiser }, { "Input device", kDeviceIn } })
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
    viewport.setBounds (r);
    list->setSize (r.getWidth() - (list->heightNeeded() > r.getHeight() ? viewport.getScrollBarThickness() : 0),
                   juce::jmax (r.getHeight(), list->heightNeeded()));
}
} // namespace hydra

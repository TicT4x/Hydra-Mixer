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

juce::String ms (double v) { return v < 0.0 ? juce::String ("offline") : juce::String (v, v < 10.0 ? 2 : 1) + " ms"; }
} // namespace

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
LatencyView::LatencyView (UiContext& c) : ctx (c), list (c)
{
    list.format = [] (double v) { return ms (v); };
    list.warnAbove = 20.0;
    viewport.setViewedComponent (&list, false);
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
        in.total = BarList::sum (in.segments);
        if (! hasSource) in.detail = "no source";
        else if (! in.segments.empty() && in.segments.front().value < 0.0) in.detail = "offline";

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
                p.total = BarList::sum (p.segments);
                for (auto& seg : p.segments) if (seg.value < 0.0) p.detail = "offline";
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
        r.total = BarList::sum (r.segments);
        if (! hasOutput) r.detail = "no output";
        else if (r.segments.back().value < 0.0) r.detail = "offline";
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

    std::vector<BarList::Section> sections (2);
    sections[0] = { "Signal paths", "input -> bus -> output, longest first",
                    "No routed path: route an input to a bus that has an output device.", std::move (paths) };
    sections[1] = { "Channels", "each input and output on its own", {}, std::move (channels) };
    const bool changed = list.update (std::move (sections), longest);
    if (! changed && text == summary) return;
    summary = text;
    resized();
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

    drawBarLegend (g, getLocalBounds().reduced (18, 0).removeFromTop (36).withTrimmedTop (8),
                   { { "Input device", kDeviceIn }, { "Denoiser", kDenoiser }, { "Plugin", kPlugin }, { "Output device", kDeviceOut } });
}

void LatencyView::resized()
{
    auto r = getLocalBounds().withTrimmedTop (68);
    viewport.setBounds (r);
    list.setSize (r.getWidth() - (list.heightNeeded() > r.getHeight() ? viewport.getScrollBarThickness() : 0),
                  juce::jmax (r.getHeight(), list.heightNeeded()));
}
} // namespace hydra

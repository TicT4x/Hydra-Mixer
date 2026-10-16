#include "DspView.h"

namespace hydra
{
namespace
{
const juce::Colour kDenoiser { theme::good };
const juce::Colour kEq       { theme::accentIn };
const juce::Colour kGate     { theme::solo };
const juce::Colour kComp     { theme::accentHw };
const juce::Colour kPlugin   { theme::warn };
const juce::Colour kLimiter  { theme::accentVirt };
const juce::Colour kStream   { 0xff5b7cff };
const juce::Colour kOther    { theme::textFaint };

juce::String percent (double v) { return juce::String (v, v < 10.0 ? 2 : 1) + " %"; }

std::string key (const char* group, int index, int stage) { return std::string (group) + std::to_string (index) + "." + std::to_string (stage); }
std::string pluginKey (const PluginSlot* s) { return "plug." + std::to_string ((uintptr_t) s); }
} // namespace

DspView::DspView (UiContext& c) : ctx (c), list (c)
{
    ctx.engine.profile.users.fetch_add (1);   // the audio thread measures from now on
    list.format = [] (double v) { return percent (v); };
    list.warnAbove = 25.0;
    viewport.setViewedComponent (&list, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (10);
    addAndMakeVisible (viewport);
    history.push_back (read());
    refresh();
    startTimerHz (4);
    setSize (1000, 640);
}

DspView::~DspView()
{
    stopTimer();
    ctx.engine.profile.users.fetch_sub (1);
}

void DspView::timerCallback() { refresh(); }

DspView::Reading DspView::read() const
{
    auto& e = ctx.engine;
    Reading r;
    r.time = juce::Time::getMillisecondCounterHiRes();
    r.frames = e.profile.frames.load();
    r.total = e.profile.total.load();
    r.recorder = e.profile.recorder.load();
    auto chain = [&r] (const PluginChain& c)
    {
        for (int k = 0; k < c.size(); ++k)
            if (auto* s = c.getSlot (k)) r.ticks[pluginKey (s)] = s->dspTicks.load();
    };
    for (int i = 0; i < kMaxInputs; ++i)
    {
        const auto& s = e.inputs[(size_t) i];
        for (int st = 0; st < DspTimes::numStages; ++st) r.ticks[key ("in", i, st)] = s.dspTimes.get ((DspTimes::Stage) st);
        chain (s.plugins);
    }
    for (int b = 0; b < kMaxBuses; ++b)
    {
        const auto& bus = e.buses[(size_t) b];
        for (int st = 0; st < DspTimes::numStages; ++st) r.ticks[key ("bus", b, st)] = bus.dspTimes.get ((DspTimes::Stage) st);
        chain (bus.plugins);
    }
    for (auto& [label, ticks] : e.getStreamDspTicks())
        r.ticks["stream." + label.toStdString()] += ticks;
    return r;
}

void DspView::refresh()
{
    auto& e = ctx.engine;
    history.push_back (read());
    // Average over about one second: compare with the reading closest to 1 s ago.
    while (history.size() > 2 && history.back().time - history[1].time >= 1000.0)
        history.pop_front();
    const auto& now = history.back();
    const auto& base = history.front();
    peakLoad = 0.5 * peakLoad;
    peakLoad = juce::jmax (peakLoad, (double) e.profile.peak.exchange (0.0f) * 100.0);

    const double frames = (double) (now.frames - base.frames);
    const double budget = frames / juce::jmax (1.0, e.getSampleRate()) * e.getTicksPerSecond();   // ticks available
    auto pct = [&] (uint64_t a, uint64_t b) { return budget > 0.0 ? 100.0 * (double) (a - juce::jmin (a, b)) / budget : 0.0; };
    auto delta = [&] (const std::string& k)
    {
        const auto it = now.ticks.find (k);
        if (it == now.ticks.end()) return 0.0;
        const auto old = base.ticks.find (k);
        return pct (it->second, old != base.ticks.end() ? old->second : 0);
    };

    double accounted = 0.0;
    auto addPlugins = [&] (std::vector<BarList::Segment>& segs, const PluginChain& chain, double stageTotal, const Param& bypassAll)
    {
        double inPlugins = 0.0;
        for (int k = 0; k < chain.size(); ++k)
            if (auto* s = chain.getSlot (k))
            {
                const double v = delta (pluginKey (s));
                inPlugins += v;
                if (v > 0.0 || (! s->bypassed && ! bypassAll.isOn()))
                    segs.push_back ({ s->instance->getName(), v, kPlugin });
            }
        return juce::jmax (0.0, stageTotal - inPlugins);   // buffer copies around the plugins
    };

    std::vector<BarList::Row> channels, engineRows;
    for (int i = 0; i < kMaxInputs; ++i)
    {
        if (! e.isInputActive (i)) continue;
        const auto& s = e.inputs[(size_t) i];
        auto st = [&] (DspTimes::Stage stage) { return delta (key ("in", i, stage)); };
        BarList::Row r;
        r.title = "IN " + juce::String (i + 1) + "   " + s.name;
        r.colour = theme::accentIn;
        r.selectIndex = i;
        if (s.dnOn->isOn() || st (DspTimes::denoiser) > 0.0) r.segments.push_back ({ "Denoiser", st (DspTimes::denoiser), kDenoiser });
        r.segments.push_back ({ "EQ", st (DspTimes::eq), kEq });
        if (s.gateOn->isOn()) r.segments.push_back ({ "Gate", st (DspTimes::gate), kGate });
        if (s.compOn->isOn()) r.segments.push_back ({ "Compressor", st (DspTimes::comp), kComp });
        const double copies = addPlugins (r.segments, s.plugins, st (DspTimes::plugins), *s.fxBypass);
        double other = st (DspTimes::other) + copies;
        if (! s.gateOn->isOn()) other += st (DspTimes::gate);
        if (! s.compOn->isOn()) other += st (DspTimes::comp);
        r.segments.push_back ({ "Gain, pan, routing, meters", other, kOther });
        r.total = BarList::sum (r.segments);
        if (s.source.kind == IoTarget::Kind::None && r.total <= 0.0) r.detail = "no source";
        accounted += r.total;
        channels.push_back (std::move (r));
    }
    for (int b = 0; b < kMaxBuses; ++b)
    {
        if (! e.isBusActive (b)) continue;
        const auto& bus = e.buses[(size_t) b];
        auto st = [&] (DspTimes::Stage stage) { return delta (key ("bus", b, stage)); };
        BarList::Row r;
        r.title = busCode (b) + "   " + bus.name;
        r.colour = theme::busColour (b);
        r.selectIndex = busSelectIndex (b);
        r.segments.push_back ({ "EQ", st (DspTimes::eq), kEq });
        const double copies = addPlugins (r.segments, bus.plugins, st (DspTimes::plugins), *bus.fxBypass);
        if (bus.limOn->isOn() || st (DspTimes::limiter) > 0.0) r.segments.push_back ({ "Limiter", st (DspTimes::limiter), kLimiter });
        r.segments.push_back ({ "Fader, mono, meters", st (DspTimes::other) + copies, kOther });
        r.total = BarList::sum (r.segments);
        accounted += r.total;
        channels.push_back (std::move (r));
    }

    // Engine work outside the channels
    for (auto& [k, ticks] : now.ticks)
    {
        if (k.rfind ("stream.", 0) != 0) continue;
        BarList::Row r;
        r.title = juce::String (k.substr (7));
        r.colour = theme::textDim;
        r.segments.push_back ({ "Resampling + drift control", delta (k), kStream });
        r.total = BarList::sum (r.segments);
        accounted += r.total;
        engineRows.push_back (std::move (r));
    }
    std::sort (engineRows.begin(), engineRows.end(), [] (const BarList::Row& a, const BarList::Row& b) { return a.title < b.title; });
    {
        BarList::Row r;
        r.title = "Recorder";
        r.colour = theme::mute;
        r.selectIndex = kRecorderSelect;
        r.segments.push_back ({ "Recording / playback", pct (now.recorder, base.recorder), kOther });
        r.total = BarList::sum (r.segments);
        accounted += r.total;
        engineRows.push_back (std::move (r));
    }
    totalLoad = pct (now.total, base.total);
    {
        BarList::Row r;
        r.title = "Mixing, bus sums, outputs";
        r.colour = theme::textDim;
        r.segments.push_back ({ "Engine", juce::jmax (0.0, totalLoad - accounted), kOther });
        r.total = BarList::sum (r.segments);
        engineRows.push_back (std::move (r));
    }

    // Bar scale: follows the heaviest row, grows at once and shrinks slowly (no jumping bars).
    double heaviest = 1.0;
    for (auto* rows : { &channels, &engineRows })
        for (auto& r : *rows) heaviest = juce::jmax (heaviest, r.total * 1.15);
    shownScale = heaviest > shownScale ? heaviest : shownScale + 0.15 * (heaviest - shownScale);

    juce::String text;
    if (auto* d = e.getMasterDevice(); d != nullptr && frames > 0.0)
    {
        const double blockMs = 1000.0 * d->getCurrentBufferSizeSamples() / juce::jmax (1.0, e.getSampleRate());
        text << "Total " << percent (totalLoad) << " of the audio period (average over 1 s)  ·  peak " << percent (peakLoad)
             << "  ·  " << d->getCurrentBufferSizeSamples() << " samples at " << (int) e.getSampleRate() << " Hz = "
             << juce::String (blockMs, 2) << " ms per block";
    }
    else
        text = e.getMasterDevice() == nullptr ? "Audio engine stopped - open Settings > Audio Engine." : "Measuring...";

    std::vector<BarList::Section> sections (2);
    sections[0] = { "Channels", "share of the audio period used by each channel and its stages", {}, std::move (channels) };
    sections[1] = { "Engine", "resampling of other devices, recorder, mixing (device threads not included)", {}, std::move (engineRows) };
    const bool changed = list.update (std::move (sections), shownScale);
    if (! changed && text == summary) return;
    summary = text;
    resized();
    repaint (getLocalBounds().withHeight (68));
}

void DspView::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);
    auto head = getLocalBounds().reduced (18, 0).removeFromTop (64);
    g.setColour (theme::text);
    g.setFont (theme::font (15.0f, true));
    g.drawText ("DSP Load", head.removeFromTop (36).withTrimmedTop (8), juce::Justification::centredLeft);
    g.setColour (totalLoad >= 70.0 ? theme::warn : theme::textDim);
    g.setFont (theme::font (12.0f));
    g.drawFittedText (summary, head.removeFromTop (20), juce::Justification::centredLeft, 1, 0.8f);

    drawBarLegend (g, getLocalBounds().reduced (18, 0).removeFromTop (36).withTrimmedTop (8),
                   { { "Denoiser", kDenoiser }, { "EQ", kEq }, { "Gate", kGate }, { "Comp", kComp }, { "Plugin", kPlugin },
                     { "Limiter", kLimiter }, { "Resampling", kStream }, { "Other", kOther } });
}

void DspView::resized()
{
    auto r = getLocalBounds().withTrimmedTop (68);
    viewport.setBounds (r);
    list.setSize (r.getWidth() - (list.heightNeeded() > r.getHeight() ? viewport.getScrollBarThickness() : 0),
                  juce::jmax (r.getHeight(), list.heightNeeded()));
}
} // namespace hydra

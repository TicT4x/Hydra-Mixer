#include "DspView.h"

namespace hydra
{
namespace
{
/** Below this a switched-off stage counts as measuring noise (added to the rest instead of shown). */
constexpr double kNoise = 0.02;

const juce::Colour kDenoiser { theme::good };
const juce::Colour kEq       { theme::accentIn };
const juce::Colour kGate     { theme::solo };
const juce::Colour kComp     { theme::accentHw };
const juce::Colour kPlugin   { theme::warn };
const juce::Colour kLimiter  { theme::accentVirt };
const juce::Colour kStream   { 0xff5b7cff };
const juce::Colour kRecorder { theme::mute };
const juce::Colour kOther    { theme::textFaint };

/** Kinds of components for the "By component" chart (plugins are summed up). */
const std::vector<std::pair<juce::String, juce::Colour>>& kinds()
{
    static const std::vector<std::pair<juce::String, juce::Colour>> k {
        { "Denoiser", kDenoiser }, { "EQ", kEq }, { "Gate", kGate }, { "Compressor", kComp }, { "Plugins", kPlugin },
        { "Limiter", kLimiter }, { "Resampling", kStream }, { "Recorder", kRecorder }, { "Mixing + rest", kOther } };
    return k;
}

juce::String kindOf (const juce::Colour& c)
{
    for (auto& [name, colour] : kinds()) if (colour == c) return name;
    return "Mixing + rest";
}

juce::String percent (double v) { return juce::String (v, v < 10.0 ? 2 : 1) + " %"; }
juce::String titled (const juce::String& code, const juce::String& name) { return name.isEmpty() || name == code ? code : code + "  " + name; }

std::string key (const char* group, int index, int stage) { return std::string (group) + std::to_string (index) + "." + std::to_string (stage); }
std::string pluginKey (const PluginSlot* s) { return "plug." + std::to_string ((uintptr_t) s); }

/** Steady colour per channel for the "By channel" donut. */
juce::Colour channelColour (int selectIndex)
{
    return selectIndex < 0 ? theme::textDim : theme::bandColour ((selectIndex * 5 + selectIndex / kMaxInputs) % 8);
}
} // namespace

//==============================================================================
/** One lane per channel: caption with the channel's load, then its stages in processing order. */
class DspView::Chains final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit Chains (UiContext& c) : ctx (c) {}
    std::vector<Lane> lanes;
    double maxStage = 0.0, maxLane = 0.0;

    static constexpr int kPad = 14, kCaptionW = 176, kLaneH = 60, kBoxH = 48, kGap = 22;

    juce::Point<int> layout (int availableWidth)
    {
        size_t most = 1;
        for (auto& l : lanes) most = juce::jmax (most, l.stages.size());
        boxW = juce::jlimit (104, 150, (availableWidth - 2 * kPad - kCaptionW) / (int) most - kGap);
        const int width = 2 * kPad + kCaptionW + (int) most * (boxW + kGap);
        return { juce::jmax (availableWidth, width), 2 * kPad + juce::jmax (1, (int) lanes.size()) * kLaneH };
    }

    void paint (juce::Graphics& g) override
    {
        for (size_t i = 0; i < lanes.size(); ++i)
        {
            const auto& lane = lanes[i];
            const auto row = laneRow ((int) i);
            if (i % 2 == 0)
            {
                g.setColour (theme::panel.withAlpha (0.6f));
                g.fillRoundedRectangle (row.toFloat(), 8.0f);
            }
            // Caption: channel, its load and a bar relative to the heaviest channel
            auto cap = row.withWidth (kCaptionW - 12).reduced (10, 8);
            g.setColour (lane.colour);
            g.setFont (theme::font (13.0f, true));
            g.drawFittedText (lane.title, cap.removeFromTop (17), juce::Justification::centredLeft, 1, 0.75f);
            auto line = cap.removeFromTop (18);
            g.setColour (lane.total >= 25.0 ? theme::warn : theme::text);
            g.setFont (theme::mono (13.0f));
            g.drawText (lane.note.isNotEmpty() && lane.total <= 0.0 ? lane.note : percent (lane.total), line.removeFromLeft (72),
                        juce::Justification::centredLeft);
            auto bar = line.reduced (0, 5).toFloat();
            g.setColour (theme::panel2);
            g.fillRoundedRectangle (bar, 3.0f);
            if (maxLane > 0.0 && lane.total > 0.0)
            {
                g.setColour (lane.colour.withAlpha (0.8f));
                g.fillRoundedRectangle (bar.withWidth (juce::jmax (3.0f, bar.getWidth() * (float) (lane.total / maxLane))), 3.0f);
            }

            for (size_t k = 0; k < lane.stages.size(); ++k)
            {
                const auto box = stageBox ((int) i, (int) k);
                if (k > 0) arrow (g, stageBox ((int) i, (int) k - 1).getRight(), box.getX(), box.getCentreY());
                drawStage (g, lane.stages[k], box, lane.total);
            }
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        juce::String tip;
        int lane = -1, stage = -1;
        hit (e.getPosition(), lane, stage);
        if (stage >= 0)
        {
            const auto& l = lanes[(size_t) lane];
            const auto& s = l.stages[(size_t) stage];
            tip << l.title << "  ·  " << s.label;
            if (s.detail.isNotEmpty()) tip << "\n" << s.detail;
            tip << "\n" << percent (s.load) << " of the audio period";
            if (l.total > 0.0) tip << ",  " << juce::roundToInt (100.0 * s.load / l.total) << " % of this channel";
        }
        else if (lane >= 0)
            tip << lanes[(size_t) lane].title << ": " << percent (lanes[(size_t) lane].total) << " of the audio period";
        if (tip.isNotEmpty() && selectFor (lane, stage) >= 0) tip << "\nclick: show it in the mixer";
        setTooltip (tip);
        setMouseCursor (selectFor (lane, stage) >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        int lane = -1, stage = -1;
        hit (e.getPosition(), lane, stage);
        if (const int sel = selectFor (lane, stage); sel >= 0 && e.mouseWasClicked() && ctx.select) ctx.select (sel);
    }

private:
    juce::Rectangle<int> laneRow (int i) const { return { kPad, kPad + i * kLaneH, getWidth() - 2 * kPad, kLaneH - 4 }; }
    juce::Rectangle<int> stageBox (int i, int k) const
    {
        return { kPad + kCaptionW + k * (boxW + kGap), kPad + i * kLaneH + (kLaneH - 4 - kBoxH) / 2, boxW, kBoxH };
    }

    void hit (juce::Point<int> p, int& lane, int& stage) const
    {
        for (size_t i = 0; i < lanes.size(); ++i)
            if (laneRow ((int) i).contains (p))
            {
                lane = (int) i;
                for (size_t k = 0; k < lanes[i].stages.size(); ++k)
                    if (stageBox ((int) i, (int) k).contains (p)) stage = (int) k;
            }
    }

    int selectFor (int lane, int stage) const
    {
        if (lane < 0) return -1;
        const auto& l = lanes[(size_t) lane];
        if (stage >= 0 && l.stages[(size_t) stage].selectIndex >= 0) return l.stages[(size_t) stage].selectIndex;
        return l.selectIndex;
    }

    static void arrow (juce::Graphics& g, int x0, int x1, int y)
    {
        g.setColour (theme::textFaint.withAlpha (0.7f));
        g.drawLine ((float) x0 + 2.0f, (float) y, (float) x1 - 3.0f, (float) y, 1.2f);
        juce::Path p;
        p.addTriangle ((float) x1 - 7.0f, (float) y - 4.0f, (float) x1 - 7.0f, (float) y + 4.0f, (float) x1 - 2.0f, (float) y);
        g.fillPath (p);
    }

    /** Box of one stage, tinted by how expensive it is compared to the most expensive stage. */
    void drawStage (juce::Graphics& g, const Stage& s, juce::Rectangle<int> r, double laneTotal) const
    {
        const float heat = maxStage > 0.0 ? (float) juce::jlimit (0.0, 1.0, s.load / maxStage) : 0.0f;
        const bool idle = s.load < 0.005;
        auto f = r.toFloat();
        g.setColour (theme::panel2.interpolatedWith (s.colour, 0.32f * heat));
        g.fillRoundedRectangle (f, 7.0f);
        g.setColour (s.rank > 0 ? theme::warn : idle ? theme::outline : s.colour.withAlpha (0.25f + 0.55f * heat));
        g.drawRoundedRectangle (f.reduced (0.5f), 7.0f, s.rank > 0 ? 1.6f : 1.0f);
        g.setColour (idle ? s.colour.withAlpha (0.35f) : s.colour);
        g.fillRoundedRectangle (f.removeFromLeft (4.0f).reduced (0.0f, 8.0f), 2.0f);

        auto a = r.reduced (11, 4);
        if (s.rank > 0)   // #1..#3 of all stages
        {
            auto tag = a.removeFromRight (24).removeFromTop (16).toFloat();
            g.setColour (theme::warn);
            g.fillRoundedRectangle (tag, 4.0f);
            g.setColour (theme::bg);
            g.setFont (theme::font (10.5f, true));
            g.drawText ("#" + juce::String (s.rank), tag, juce::Justification::centred);
            a.removeFromRight (2);
        }
        g.setColour (idle ? theme::textDim : theme::text);
        g.setFont (theme::font (11.5f, true));
        g.drawFittedText (s.label, a.removeFromTop (16), juce::Justification::centredLeft, 1, 0.75f);
        a = r.reduced (11, 4).withTrimmedTop (16);
        g.setColour (idle ? theme::textFaint : s.load >= 10.0 ? theme::warn : theme::text);
        g.setFont (theme::mono (13.0f));
        g.drawText (percent (s.load), a.removeFromTop (16), juce::Justification::centredLeft);
        g.setColour (theme::textFaint);
        g.setFont (theme::font (10.5f));
        g.drawFittedText (laneTotal > 0.0 ? juce::String (juce::roundToInt (100.0 * s.load / laneTotal)) + " % of channel" : juce::String(),
                          a, juce::Justification::centredLeft, 1, 0.8f);
    }

    UiContext& ctx;
    int boxW = 130;
};

//==============================================================================
DspView::DspView (UiContext& c) : ctx (c), top (c), byChannel (c), byKind (c)
{
    ctx.engine.profile.users.fetch_add (1);   // the audio thread measures from now on
    chains = std::make_unique<Chains> (c);

    for (auto* chart : { &top, &byChannel, &byKind })
    {
        chart->format = [] (double v) { return percent (v); };
        chart->showShare = false;
        chart->steady = true;
        chart->centreCaption = "of period";
        chart->emptyText = "Measuring...";
        addAndMakeVisible (chart);
    }
    top.caption = "Top consumers";
    top.subtitle = "component in channel";
    top.donut = false;
    byChannel.caption = "By channel";
    byKind.caption = "By component";

    viewport.setViewedComponent (chains.get(), false);
    viewport.setScrollBarsShown (false, false, true, true);
    viewport.setScrollBarThickness (10);
    addAndMakeVisible (viewport);
    history.push_back (read());
    refresh();
    startTimerHz (4);
    setSize (1100, 700);
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

    // Plugins of a chain; returns the chain's time outside the plugins (buffer copies around them).
    auto addPlugins = [&] (std::vector<Stage>& stages, const PluginChain& chain, double stageTotal, const Param& bypassAll)
    {
        double inPlugins = 0.0;
        for (int k = 0; k < chain.size(); ++k)
            if (auto* s = chain.getSlot (k))
            {
                const double v = delta (pluginKey (s));
                inPlugins += v;
                if (v > kNoise || (! s->bypassed && ! bypassAll.isOn()))
                    stages.push_back ({ s->instance->getName(), "VST3 plugin", v, kPlugin });
            }
        return juce::jmax (0.0, stageTotal - inPlugins);
    };

    std::vector<Lane> lanes;
    double accounted = 0.0;
    for (int i = 0; i < kMaxInputs; ++i)
    {
        if (! e.isInputActive (i)) continue;
        const auto& s = e.inputs[(size_t) i];
        auto st = [&] (DspTimes::Stage stage) { return delta (key ("in", i, stage)); };
        Lane l;
        l.key = "in" + juce::String (i);
        l.title = titled ("IN " + juce::String (i + 1), s.name);
        l.colour = theme::accentIn;
        l.selectIndex = i;
        double other = st (DspTimes::other);
        if (s.dnOn->isOn() || st (DspTimes::denoiser) > kNoise)
            l.stages.push_back ({ "Denoiser", "RNNoise, " + s.dnMode->choices[juce::roundToInt (s.dnMode->get())] + " mode",
                                  st (DspTimes::denoiser), kDenoiser });
        else other += st (DspTimes::denoiser);
        if (s.eq.on->isOn() || st (DspTimes::eq) > kNoise) l.stages.push_back ({ "EQ", "8-band EQ", st (DspTimes::eq), kEq });
        else other += st (DspTimes::eq);
        if (s.gateOn->isOn()) l.stages.push_back ({ "Gate", {}, st (DspTimes::gate), kGate });
        else other += st (DspTimes::gate);
        if (s.compOn->isOn()) l.stages.push_back ({ "Compressor", {}, st (DspTimes::comp), kComp });
        else other += st (DspTimes::comp);
        other += addPlugins (l.stages, s.plugins, st (DspTimes::plugins), *s.fxBypass);
        l.stages.push_back ({ "Gain, pan, routing", "trim, phase, mono, fader, pan, sends, meters", other, kOther });
        if (s.source.kind == IoTarget::Kind::None) l.note = "no source";
        lanes.push_back (std::move (l));
    }
    for (int b = 0; b < kMaxBuses; ++b)
    {
        if (! e.isBusActive (b)) continue;
        const auto& bus = e.buses[(size_t) b];
        auto st = [&] (DspTimes::Stage stage) { return delta (key ("bus", b, stage)); };
        Lane l;
        l.key = "bus" + juce::String (b);
        l.title = titled (busCode (b), bus.name);
        l.colour = theme::busColour (b);
        l.selectIndex = busSelectIndex (b);
        double other = st (DspTimes::other);
        if (bus.eq.on->isOn() || st (DspTimes::eq) > kNoise) l.stages.push_back ({ "EQ", "8-band EQ", st (DspTimes::eq), kEq });
        else other += st (DspTimes::eq);
        other += addPlugins (l.stages, bus.plugins, st (DspTimes::plugins), *bus.fxBypass);
        if (bus.limOn->isOn() || st (DspTimes::limiter) > kNoise) l.stages.push_back ({ "Limiter", {}, st (DspTimes::limiter), kLimiter });
        else other += st (DspTimes::limiter);
        l.stages.push_back ({ "Fader, mono, meters", {}, other, kOther });
        if (bus.output.kind == IoTarget::Kind::None) l.note = "no output";
        lanes.push_back (std::move (l));
    }
    for (auto& l : lanes)
        for (auto& s : l.stages) { l.total += s.load; accounted += s.load; }

    // The engine's own work: resampling of other devices, recorder, mixing (= total minus the channels)
    totalLoad = pct (now.total, base.total);
    {
        Lane l;
        l.key = "engine";
        l.title = "Engine";
        l.colour = theme::textDim;
        std::vector<Stage> streams;
        for (auto& [k, ticks] : now.ticks)
            if (k.rfind ("stream.", 0) == 0)
                streams.push_back ({ "Resampling", juce::String (k.substr (7)) + " (resampling + drift control)", delta (k), kStream });
        std::sort (streams.begin(), streams.end(), [] (const Stage& a, const Stage& b) { return a.detail < b.detail; });
        for (auto& s : streams) { s.label = s.detail.upToFirstOccurrenceOf (" (", false, false); l.stages.push_back (s); }
        const double recorder = pct (now.recorder, base.recorder);
        if (recorder > kNoise || e.recorder.isRecording() || e.recorder.isPlaying())
            l.stages.push_back ({ "Recorder", "recording / playback", recorder, kRecorder, kRecorderSelect });
        double engineOwn = 0.0;
        for (auto& s : l.stages) engineOwn += s.load;
        l.stages.push_back ({ "Mixing, bus sums", "summing the buses, writing the outputs",
                              juce::jmax (0.0, totalLoad - accounted - engineOwn), kOther });
        for (auto& s : l.stages) l.total += s.load;
        lanes.push_back (std::move (l));
    }
    const double shownTotal = juce::jmax (totalLoad, accounted);

    // Heaviest channel first (steady order), the three biggest stages of all tagged
    rankSteady (lanes, laneOrder, [] (const Lane& l) { return l.key; }, [] (const Lane& l) { return l.total; }, shownTotal * 0.002);
    std::vector<Stage*> all;
    for (auto& l : lanes) for (auto& s : l.stages) all.push_back (&s);
    std::stable_sort (all.begin(), all.end(), [] (const Stage* a, const Stage* b) { return a->load > b->load; });
    for (size_t k = 0; k < juce::jmin ((size_t) 3, all.size()); ++k)
        if (all[k]->load >= 0.01) all[k]->rank = (int) k + 1;

    // Overview charts
    std::vector<ShareChart::Part> topParts, channelParts, kindParts;
    std::map<juce::String, double> perKind;
    for (auto& l : lanes)
    {
        for (size_t k = 0; k < l.stages.size(); ++k)
        {
            const auto& s = l.stages[k];
            const int sel = s.selectIndex >= 0 ? s.selectIndex : l.selectIndex;
            topParts.push_back ({ l.key + "/" + juce::String ((int) k) + s.label, l.title + "  ·  " + s.label, s.detail, s.load, s.colour, sel });
            perKind[kindOf (s.colour)] += s.load;
        }
        channelParts.push_back ({ l.key, l.title, l.note, l.total, channelColour (l.selectIndex), l.selectIndex });
    }
    for (auto& [name, colour] : kinds())
        kindParts.push_back ({ name, name, {}, perKind[name], colour });

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

    // Repaint only when something visible changed (values rounded as shown).
    juce::String sig = text;
    for (auto& l : lanes)
    {
        sig << l.key << l.title << l.note << ':';
        for (auto& s : l.stages) sig << s.label << '=' << juce::String (s.load, 2) << '#' << s.rank << ';';
    }
    if (sig == shownSignature) return;
    shownSignature = sig;
    summary = text;

    top.setParts (std::move (topParts), shownTotal);
    byChannel.setParts (std::move (channelParts), shownTotal);
    byKind.setParts (std::move (kindParts), shownTotal);
    chains->maxStage = all.empty() ? 0.0 : all.front()->load;
    chains->maxLane = 0.0;
    for (auto& l : lanes) chains->maxLane = juce::jmax (chains->maxLane, l.total);
    chains->lanes = std::move (lanes);
    resized();
    chains->repaint();
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

    drawLegend (g, getLocalBounds().reduced (18, 0).removeFromTop (36).withTrimmedTop (8),
                { { "Denoiser", kDenoiser }, { "EQ", kEq }, { "Gate", kGate }, { "Comp", kComp }, { "Plugin", kPlugin },
                  { "Limiter", kLimiter }, { "Resampling", kStream }, { "Recorder", kRecorder }, { "Other", kOther } });

    // Separator between the overview and the channel lanes
    g.setColour (theme::outline.withAlpha (0.7f));
    g.fillRect (18, viewport.getY() - 7, getWidth() - 36, 1);
}

void DspView::resized()
{
    auto r = getLocalBounds().withTrimmedTop (70);
    auto overview = r.removeFromTop (juce::jlimit (150, 210, getHeight() * 30 / 100)).reduced (18, 0);
    const int w = overview.getWidth();
    top.setBounds (overview.removeFromLeft (w * 40 / 100));
    overview.removeFromLeft (24);
    byChannel.setBounds (overview.removeFromLeft ((overview.getWidth() - 24) / 2));
    overview.removeFromLeft (24);
    byKind.setBounds (overview);
    r.removeFromTop (14);

    viewport.setBounds (r);
    const auto size = chains->layout (r.getWidth());
    const auto fitted = chains->layout (r.getWidth() - (size.y > r.getHeight() ? viewport.getScrollBarThickness() : 0));
    chains->setSize (fitted.x, fitted.y);
}
} // namespace hydra

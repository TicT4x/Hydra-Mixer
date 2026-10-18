#include "DspView.h"

namespace hydra
{
namespace
{
/** Below this a switched-off stage counts as measuring noise (added to the rest instead of shown). */
constexpr double kNoise = 0.02;

const std::array<juce::String, DspView::numColumns> kColumnNames { "Denoiser", "EQ", "Gate", "Comp", "Plugins", "Limiter", "Rest" };
const std::array<juce::Colour, DspView::numColumns> kColumnColours { theme::good, theme::accentIn, theme::solo, theme::accentHw,
                                                                     theme::warn, theme::accentVirt, theme::textDim };

juce::String percent (double v) { return juce::String (v, v < 10.0 ? 2 : 1) + " %"; }
juce::String titled (const juce::String& code, const juce::String& name) { return name.isEmpty() || name == code ? code : code + "  " + name; }

std::string key (const char* group, int index, int stage) { return std::string (group) + std::to_string (index) + "." + std::to_string (stage); }
std::string pluginKey (const PluginSlot* s) { return "plug." + std::to_string ((uintptr_t) s); }
} // namespace

//==============================================================================
/** The table: one row per channel in mixer order plus the engine row. */
class DspView::Table final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit Table (UiContext& c) : ctx (c) {}
    std::vector<Row> rows;
    double maxCell = 0.0, maxTotal = 0.0;

    static constexpr int kPad = 6, kNameW = 210, kTotalW = 180, kHeadH = 26, kRowH = 34, kMinColW = 74;

    int neededWidth() const { return 2 * kPad + kNameW + kTotalW + numColumns * kMinColW; }
    int neededHeight() const { return 2 * kPad + kHeadH + (int) rows.size() * kRowH; }

    void paint (juce::Graphics& g) override
    {
        const int colW = columnWidth();
        // Header
        g.setFont (theme::font (11.0f, true));
        g.setColour (theme::textDim);
        g.drawText ("CHANNEL", juce::Rectangle<int> (kPad + 10, kPad, kNameW, kHeadH), juce::Justification::centredLeft);
        g.drawText ("TOTAL", juce::Rectangle<int> (kPad + kNameW, kPad, kTotalW, kHeadH), juce::Justification::centredLeft);
        for (int c = 0; c < numColumns; ++c)
        {
            auto r = juce::Rectangle<int> (kPad + kNameW + kTotalW + c * colW, kPad, colW, kHeadH);
            g.setColour (kColumnColours[(size_t) c]);
            g.fillRoundedRectangle (r.removeFromLeft (3).reduced (0, 7).toFloat(), 1.5f);
            g.setColour (theme::textDim);
            g.drawText (kColumnNames[(size_t) c].toUpperCase(), r.withTrimmedLeft (6), juce::Justification::centredLeft);
        }

        for (size_t i = 0; i < rows.size(); ++i)
        {
            const auto& row = rows[i];
            auto r = rowRect ((int) i);
            if (i % 2 == 0 || row.engine)
            {
                g.setColour (row.engine ? theme::panel2 : theme::panel.withAlpha (0.7f));
                g.fillRoundedRectangle (r.toFloat(), 6.0f);
            }
            if ((int) i == hoverRow && row.selectIndex >= 0)
            {
                g.setColour (theme::raised.withAlpha (0.5f));
                g.fillRoundedRectangle (r.toFloat(), 6.0f);
            }

            // Name
            auto name = r.removeFromLeft (kNameW).reduced (10, 0);
            g.setColour (row.colour);
            g.setFont (theme::font (12.5f, true));
            g.drawFittedText (row.title, name.removeFromTop (row.note.isEmpty() ? name.getHeight() : 20),
                              row.note.isEmpty() ? juce::Justification::centredLeft : juce::Justification::bottomLeft, 1, 0.75f);
            if (row.note.isNotEmpty())
            {
                g.setColour (theme::textFaint);
                g.setFont (theme::font (10.5f));
                g.drawText (row.note, name, juce::Justification::topLeft);
            }

            // Total: number + bar relative to the heaviest row
            auto total = r.removeFromLeft (kTotalW).reduced (4, 0);
            g.setFont (theme::mono (12.5f));
            g.setColour (row.totalLabel >= 25.0 ? theme::warn : theme::text);
            g.drawText (percent (row.totalLabel), total.removeFromLeft (68), juce::Justification::centredLeft);
            auto bar = total.withSizeKeepingCentre (total.getWidth() - 8, 6).toFloat();
            g.setColour (theme::bg);
            g.fillRoundedRectangle (bar, 3.0f);
            if (maxTotal > 0.0 && row.totalShown > 0.0)
            {
                g.setColour (row.colour.withAlpha (0.85f));
                g.fillRoundedRectangle (bar.withWidth (juce::jmax (2.0f, bar.getWidth() * (float) juce::jmin (1.0, row.totalShown / maxTotal))), 3.0f);
            }

            if (row.engine)
            {
                g.setColour (theme::textDim);
                g.setFont (theme::font (11.5f));
                g.drawFittedText (row.engineText, r.reduced (8, 0), juce::Justification::centredLeft, 1, 0.8f);
                continue;
            }
            for (int c = 0; c < numColumns; ++c)
                drawCell (g, row, c, r.removeFromLeft (colW));
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int row = rowAt (e.getPosition());
        if (row != hoverRow) { hoverRow = row; repaint(); }
        juce::String tip;
        if (row >= 0)
        {
            const auto& r = rows[(size_t) row];
            const int col = columnAt (e.x);
            if (r.engine) tip = r.engineText;
            else if (col == plugins && r.pluginInfo.isNotEmpty()) tip = r.pluginInfo;
            else if (col >= 0) tip = r.title + "  ·  " + kColumnNames[(size_t) col] + ":  " + (r.present[(size_t) col] ? percent (r.label[(size_t) col]) : juce::String ("off"));
            else tip = r.title + ":  " + percent (r.totalLabel) + " of the audio period";
            if (r.selectIndex >= 0) tip << "\nclick: show it in the mixer";
        }
        setTooltip (tip);
        setMouseCursor (row >= 0 && rows[(size_t) row].selectIndex >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    }

    void mouseExit (const juce::MouseEvent&) override { hoverRow = -1; repaint(); }

    void mouseUp (const juce::MouseEvent& e) override
    {
        const int row = rowAt (e.getPosition());
        if (row >= 0 && e.mouseWasClicked() && rows[(size_t) row].selectIndex >= 0 && ctx.select)
            ctx.select (rows[(size_t) row].selectIndex);
    }

private:
    int columnWidth() const { return juce::jmax (kMinColW, (getWidth() - 2 * kPad - kNameW - kTotalW) / numColumns); }
    juce::Rectangle<int> rowRect (int i) const { return { kPad, kPad + kHeadH + i * kRowH, getWidth() - 2 * kPad, kRowH - 3 }; }
    int rowAt (juce::Point<int> p) const
    {
        for (size_t i = 0; i < rows.size(); ++i) if (rowRect ((int) i).contains (p)) return (int) i;
        return -1;
    }
    int columnAt (int x) const
    {
        const int x0 = kPad + kNameW + kTotalW;
        return x < x0 ? -1 : juce::jmin ((int) numColumns - 1, (x - x0) / columnWidth());
    }

    void drawCell (juce::Graphics& g, const Row& row, int c, juce::Rectangle<int> r) const
    {
        auto f = r.toFloat().reduced (3.0f, 4.0f);
        if (! row.present[(size_t) c])
        {
            g.setColour (theme::textFaint.withAlpha (0.6f));
            g.setFont (theme::font (11.5f));
            g.drawText ("-", f, juce::Justification::centred);
            return;
        }
        const double v = row.shown[(size_t) c];
        const float heat = maxCell > 0.0 ? (float) juce::jlimit (0.0, 1.0, v / maxCell) : 0.0f;
        const auto colour = kColumnColours[(size_t) c];
        g.setColour (theme::panel2.interpolatedWith (colour, 0.08f + 0.40f * heat));
        g.fillRoundedRectangle (f, 5.0f);
        g.setColour (colour.withAlpha (0.2f + 0.6f * heat));
        g.drawRoundedRectangle (f, 5.0f, 1.0f);
        const double shownValue = row.label[(size_t) c];
        g.setColour (shownValue >= 10.0 ? theme::warn : shownValue < 0.005 ? theme::textFaint : theme::text);
        g.setFont (theme::mono (12.0f));
        g.drawText (percent (shownValue), f, juce::Justification::centred);
    }

    UiContext& ctx;
    int hoverRow = -1;
};

//==============================================================================
DspView::DspView (UiContext& c) : ctx (c)
{
    ctx.engine.profile.users.fetch_add (1);   // the audio thread measures from now on
    table = std::make_unique<Table> (c);
    viewport.setViewedComponent (table.get(), false);
    viewport.setScrollBarsShown (false, false, true, true);
    viewport.setScrollBarThickness (10);
    addAndMakeVisible (viewport);
    history.push_back (read());
    measure();
    startTimerHz (10);
    setSize (1100, 640);
}

DspView::~DspView()
{
    stopTimer();
    ctx.engine.profile.users.fetch_sub (1);
}

void DspView::timerCallback() { measure(); }

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
        if (! e.isInputActive (i)) continue;
        const auto& s = e.inputs[(size_t) i];
        for (int st = 0; st < DspTimes::numStages; ++st) r.ticks[key ("in", i, st)] = s.dspTimes.get ((DspTimes::Stage) st);
        chain (s.plugins);
    }
    for (int b = 0; b < kMaxBuses; ++b)
    {
        if (! e.isBusActive (b)) continue;
        const auto& bus = e.buses[(size_t) b];
        for (int st = 0; st < DspTimes::numStages; ++st) r.ticks[key ("bus", b, st)] = bus.dspTimes.get ((DspTimes::Stage) st);
        chain (bus.plugins);
    }
    for (auto& [label, ticks] : e.getStreamDspTicks())
        r.ticks["stream." + label.toStdString()] += ticks;
    return r;
}

void DspView::measure()
{
    auto& e = ctx.engine;
    history.push_back (read());
    // Average over about one second: compare with the reading closest to 1 s ago.
    while (history.size() > 2 && history.back().time - history[1].time >= 1000.0)
        history.pop_front();
    const auto& now = history.back();
    const auto& base = history.front();
    peakLoad = juce::jmax (peakLoad * 0.93, (double) e.profile.peak.exchange (0.0f) * 100.0);

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

    // Previous shown values by key: the new rows glide from there.
    std::map<juce::String, const Row*> previous;
    for (auto& r : table->rows) previous[r.key] = &r;

    std::vector<Row> rows;
    double accounted = 0.0;
    auto finish = [&] (Row& r)
    {
        for (int c = 0; c < numColumns; ++c) r.totalTarget += r.target[(size_t) c];
        accounted += r.totalTarget;
    };
    auto pluginsOf = [&] (Row& r, const PluginChain& chain, double stageTotal, const Param& bypassAll)
    {
        double inPlugins = 0.0;
        juce::StringArray lines;
        for (int k = 0; k < chain.size(); ++k)
            if (auto* s = chain.getSlot (k))
            {
                const double v = delta (pluginKey (s));
                inPlugins += v;
                lines.add (s->instance->getName() + ":  " + percent (v) + (s->bypassed || bypassAll.isOn() ? "  (bypassed)" : ""));
            }
        r.present[plugins] = chain.size() > 0;
        r.target[plugins] = inPlugins;
        r.pluginInfo = lines.isEmpty() ? juce::String() : r.title + "  ·  plugins\n" + lines.joinIntoString ("\n");
        return juce::jmax (0.0, stageTotal - inPlugins);   // copies around the plugins
    };
    // A stage that is off still shows its time if it is above measuring noise.
    auto stage = [] (Row& r, Column c, bool on, double v, double& other)
    {
        if (on || v > kNoise) { r.present[(size_t) c] = true; r.target[(size_t) c] = v; }
        else other += v;
    };

    for (int i = 0; i < kMaxInputs; ++i)
    {
        if (! e.isInputActive (i)) continue;
        const auto& s = e.inputs[(size_t) i];
        auto st = [&] (DspTimes::Stage x) { return delta (key ("in", i, x)); };
        Row r;
        r.key = "in" + juce::String (i);
        r.title = titled ("IN " + juce::String (i + 1), s.name);
        r.colour = theme::accentIn;
        r.selectIndex = i;
        double other = st (DspTimes::other);
        stage (r, denoiser, s.dnOn->isOn(), st (DspTimes::denoiser), other);
        stage (r, eq, s.eq.on->isOn(), st (DspTimes::eq), other);
        stage (r, gate, s.gateOn->isOn(), st (DspTimes::gate), other);
        stage (r, comp, s.compOn->isOn(), st (DspTimes::comp), other);
        other += pluginsOf (r, s.plugins, st (DspTimes::plugins), *s.fxBypass);
        r.present[rest] = true;
        r.target[rest] = other;
        if (s.source.kind == IoTarget::Kind::None) r.note = "no source";
        finish (r);
        rows.push_back (std::move (r));
    }
    for (int b = 0; b < kMaxBuses; ++b)
    {
        if (! e.isBusActive (b)) continue;
        const auto& bus = e.buses[(size_t) b];
        auto st = [&] (DspTimes::Stage x) { return delta (key ("bus", b, x)); };
        Row r;
        r.key = "bus" + juce::String (b);
        r.title = titled (busCode (b), bus.name);
        r.colour = theme::busColour (b);
        r.selectIndex = busSelectIndex (b);
        double other = st (DspTimes::other);
        stage (r, eq, bus.eq.on->isOn(), st (DspTimes::eq), other);
        other += pluginsOf (r, bus.plugins, st (DspTimes::plugins), *bus.fxBypass);
        stage (r, limiter, bus.limOn->isOn(), st (DspTimes::limiter), other);
        r.present[rest] = true;
        r.target[rest] = other;
        if (bus.output.kind == IoTarget::Kind::None) r.note = "no output";
        finish (r);
        rows.push_back (std::move (r));
    }

    // The engine's own work: resampling of other devices, recorder, mixing (= total minus the channels)
    totalTarget = pct (now.total, base.total);
    {
        Row r;
        r.key = "engine";
        r.title = "Engine";
        r.note = "resampling, recorder, mixing";
        r.colour = theme::textDim;
        r.engine = true;
        double resampling = 0.0;
        juce::StringArray parts;
        for (auto& [k, ticks] : now.ticks)
            if (k.rfind ("stream.", 0) == 0)
            {
                const double v = delta (k);
                resampling += v;
                parts.add (juce::String (k.substr (7)) + " " + percent (v));
            }
        const double recorder = pct (now.recorder, base.recorder);
        const double mixing = juce::jmax (0.0, totalTarget - accounted - resampling - recorder);
        r.totalTarget = resampling + recorder + mixing;
        r.engineText = "Resampling " + percent (resampling) + (parts.isEmpty() ? juce::String() : "  (" + parts.joinIntoString (",  ") + ")")
                     + "     Recorder " + percent (recorder) + "     Mixing, bus sums " + percent (mixing);
        rows.push_back (std::move (r));
    }

    // The shown values continue from where they are (frame() glides them to the new targets); the order is the
    // mixer's and never changes. Numbers are rewritten once per second with the mean of that second.
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const bool writeText = nowMs - lastLabelTime >= kLabelPeriodMs;
    if (writeText) lastLabelTime = nowMs;
    for (auto& r : rows)
    {
        const auto it = previous.find (r.key);
        const Row* old = it != previous.end() ? it->second : nullptr;
        r.totalShown = old != nullptr ? old->totalShown : r.totalTarget;
        r.shown = old != nullptr ? old->shown : r.target;
        const bool write = writeText || old == nullptr;
        r.label = write ? r.target : old->label;
        r.totalLabel = write ? r.totalTarget : old->totalLabel;
        if (! write) { r.engineText = old->engineText; r.pluginInfo = old->pluginInfo; }
    }
    if (writeText) { totalLabel = totalTarget; peakLabel = peakLoad; }

    if (writeText)
    {
        std::vector<std::tuple<double, juce::String>> cells;
        for (auto& r : rows)
            if (! r.engine)
                for (int c = 0; c < numColumns; ++c)
                    if (r.present[(size_t) c] && c != rest)
                        cells.emplace_back (r.label[(size_t) c], r.title + "  ·  " + kColumnNames[(size_t) c]);
        std::sort (cells.begin(), cells.end(), [] (auto& x, auto& y) { return std::get<0> (x) > std::get<0> (y); });
        juce::StringArray top;
        for (size_t k = 0; k < juce::jmin ((size_t) 3, cells.size()); ++k)
            if (std::get<0> (cells[k]) >= 0.01)
                top.add ("#" + juce::String ((int) k + 1) + "  " + std::get<1> (cells[k]) + "  " + percent (std::get<0> (cells[k])));
        hotspots = top.isEmpty() ? juce::String ("Nothing measurable yet.") : top.joinIntoString ("        ");

        if (auto* d = e.getMasterDevice(); d != nullptr && frames > 0.0)
        {
            const double blockMs = 1000.0 * d->getCurrentBufferSizeSamples() / juce::jmax (1.0, e.getSampleRate());
            summary = "Share of each audio block's time, averaged over 1 s  ·  " + juce::String (d->getCurrentBufferSizeSamples())
                    + " samples at " + juce::String ((int) e.getSampleRate()) + " Hz = " + juce::String (blockMs, 2) + " ms per block";
        }
        else
            summary = e.getMasterDevice() == nullptr ? "Audio engine stopped - open Settings > Audio Engine." : "Measuring...";
    }
    const bool newLayout = rows.size() != table->rows.size();
    table->rows = std::move (rows);
    if ((int) table->rows.size() * Table::kRowH + 2 * Table::kPad + Table::kHeadH != table->getHeight()) resized();
    if (writeText || newLayout)
    {
        table->repaint();
        repaint (getLocalBounds().withHeight (Table::kPad + 140));
    }
}

void DspView::frame (double timestamp)
{
    // Bars, tints and the peak marker glide to the latest measurement on every display frame.
    const double dt = clock.step (timestamp);
    constexpr double eps = 0.002;   // % of the audio period: far below a pixel
    bool rowsMoved = false, headMoved = false;
    double maxCell = 0.0, maxTotal = 0.0;
    for (auto& r : table->rows)
    {
        rowsMoved |= glide (r.totalShown, r.totalTarget, dt, eps);
        for (int c = 0; c < numColumns; ++c)
        {
            rowsMoved |= glide (r.shown[(size_t) c], r.target[(size_t) c], dt, eps);
            if (r.present[(size_t) c] && ! r.engine) maxCell = juce::jmax (maxCell, r.shown[(size_t) c]);
        }
        maxTotal = juce::jmax (maxTotal, r.totalShown);
    }
    headMoved |= glide (totalShown, totalTarget, dt, eps);
    headMoved |= glide (peakShown, peakLoad, dt, eps);
    rowsMoved |= std::abs (maxCell - table->maxCell) > 1.0e-9 || std::abs (maxTotal - table->maxTotal) > 1.0e-9;
    table->maxCell = maxCell;
    table->maxTotal = maxTotal;
    if (rowsMoved) table->repaint();
    if (headMoved) repaint (getLocalBounds().withHeight (Table::kPad + 140));
}

void DspView::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);
    auto head = getLocalBounds().reduced (18, 0);
    g.setColour (theme::text);
    g.setFont (theme::font (15.0f, true));
    g.drawText ("DSP Load", head.removeFromTop (36).withTrimmedTop (8), juce::Justification::centredLeft);
    g.setColour (theme::textDim);
    g.setFont (theme::font (12.0f));
    g.drawFittedText (summary, head.removeFromTop (20), juce::Justification::centredLeft, 1, 0.8f);
    head.removeFromTop (10);

    // Total: big number + bar with the recent peak
    auto total = head.removeFromTop (34);
    g.setFont (theme::mono (22.0f));
    g.setColour (totalLabel >= 70.0 ? theme::mute : totalLabel >= 40.0 ? theme::warn : theme::text);
    g.drawText (percent (totalLabel), total.removeFromLeft (130), juce::Justification::centredLeft);
    auto bar = total.removeFromLeft (juce::jmin (520, total.getWidth() - 140)).withSizeKeepingCentre (juce::jmin (520, total.getWidth() - 140), 10).toFloat();
    g.setColour (theme::panel2);
    g.fillRoundedRectangle (bar, 5.0f);
    const auto load = (float) juce::jlimit (0.0, 1.0, totalShown / 100.0);
    g.setColour (load < 0.4f ? theme::good : load < 0.7f ? theme::solo : theme::mute);
    g.fillRoundedRectangle (bar.withWidth (juce::jmax (3.0f, bar.getWidth() * load)), 5.0f);
    const float px = bar.getX() + bar.getWidth() * (float) juce::jlimit (0.0, 1.0, peakShown / 100.0);
    g.setColour (theme::text);
    g.fillRect (px - 1.0f, bar.getY() - 4.0f, 2.0f, bar.getHeight() + 8.0f);
    g.setColour (theme::textDim);
    g.setFont (theme::font (12.0f));
    g.drawText ("peak " + percent (peakLabel), total.withTrimmedLeft (14), juce::Justification::centredLeft);
    head.removeFromTop (8);

    g.setColour (theme::textDim);
    g.setFont (theme::font (11.0f, true));
    g.drawText ("BIGGEST CONSUMERS", head.removeFromTop (16), juce::Justification::centredLeft);
    g.setColour (theme::text);
    g.setFont (theme::font (12.5f));
    g.drawFittedText (hotspots, head.removeFromTop (22), juce::Justification::centredLeft, 1, 0.8f);

    g.setColour (theme::outline.withAlpha (0.7f));
    g.fillRect (18, viewport.getY() - 7, getWidth() - 36, 1);
}

void DspView::resized()
{
    auto r = getLocalBounds().withTrimmedTop (36 + 20 + 10 + 34 + 8 + 16 + 22 + 16).reduced (12, 0).withTrimmedBottom (8);
    viewport.setBounds (r);
    const bool scrollsDown = table->neededHeight() > r.getHeight();
    table->setSize (juce::jmax (table->neededWidth(), r.getWidth() - (scrollsDown ? viewport.getScrollBarThickness() : 0)),
                    table->neededHeight());
}
} // namespace hydra

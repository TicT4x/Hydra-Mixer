#include "SyncDelayView.h"
#include "LatencyView.h"

namespace hydra
{
namespace
{
constexpr int kRowH = 40, kSectionHeadH = 54, kPad = 18, kSectionGap = 18;
// Columns (the bar takes the rest)
constexpr int kColChannel = 170, kColDevice = 190, kColLatency = 84, kColSync = 112, kColAuto = 132, kColManual = 96,
              kColResult = 88, kColGap = 10, kMinBar = 140;

juce::String ms (double v) { return v < 0.0 ? juce::String ("offline") : juce::String (v, v < 10.0 ? 2 : 1) + " ms"; }

juce::String titleOf (const Engine& e, int ch)
{
    if (ch < kMaxInputs)
    {
        const auto& n = e.inputs[(size_t) ch].name;
        const auto code = "IN " + juce::String (ch + 1);
        return n.isEmpty() || n == "Input " + juce::String (ch + 1) ? code : code + "  " + n;
    }
    const int b = ch - kMaxInputs;
    const auto& n = e.buses[(size_t) b].name;
    return n.isEmpty() || n == busCode (b) ? busCode (b) : busCode (b) + "  " + n;
}

const IoTarget& ioOf (const Engine& e, int ch)
{
    return ch < kMaxInputs ? e.inputs[(size_t) ch].source : e.buses[(size_t) (ch - kMaxInputs)].output;
}

Param& syncParam (Engine& e, int ch)  { return ch < kMaxInputs ? *e.inputs[(size_t) ch].syncOn : *e.buses[(size_t) (ch - kMaxInputs)].syncOn; }
Param& delayParam (Engine& e, int ch) { return ch < kMaxInputs ? *e.inputs[(size_t) ch].delayMs : *e.buses[(size_t) (ch - kMaxInputs)].delayMs; }

juce::Colour colourOf (int ch) { return ch < kMaxInputs ? theme::accentIn : theme::busColour (ch - kMaxInputs); }

/** Striped fill (added delay). */
void stripes (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c)
{
    juce::Graphics::ScopedSaveState s (g);
    g.reduceClipRegion (r.toNearestInt());
    g.setColour (c.withAlpha (0.18f));
    g.fillRect (r);
    g.setColour (c.withAlpha (0.55f));
    for (float x = r.getX() - r.getHeight(); x < r.getRight(); x += 6.0f)
        g.drawLine (x, r.getBottom(), x + r.getHeight(), r.getY(), 1.4f);
}
} // namespace

//==============================================================================
/** One channel: name, device, measured latency, Auto sync switch, automatic / manual delay, result, bar. */
class SyncDelayView::Row final : public juce::Component, public juce::SettableTooltipClient
{
public:
    Row (UiContext& c, int channel)
        : ctx (c), ch (channel),
          sync (c, syncParam (c.engine, channel), "Auto sync", theme::good),
          manual (c, delayParam (c.engine, channel), colourOf (channel))
    {
        addAndMakeVisible (sync);
        addAndMakeVisible (manual);
        sync.setTooltip ("Auto sync: delay this channel so it lines up with the slowest channel of its group that also has "
                         "Auto sync on. Follows the live latency.");
        manual.setTooltip ("Manual delay on top of the automatic one (e.g. for a microphone that is further away: "
                           "about 3 ms per metre). Drag up / down, Shift = fine, click to type.");
    }

    const int ch;
    double scaleMs = 10.0;   // bar width = this latency (set by the section from the shown results)

    /** Takes a new measurement (10x per second). Numbers show the mean of the last second and are rewritten
        once per second (publish); a change of state (switch, device, offline) is shown at once.
        Returns true if anything visible changed. */
    bool update (bool publish)
    {
        auto& e = ctx.engine;
        const auto& st = e.getSyncState (ch);
        Info n;
        n.title = titleOf (e, ch);
        n.device = ioOf (e, ch).describe();
        n.syncOn = syncParam (e, ch).isOn();
        n.member = st.member;
        n.reference = st.reference;
        n.offline = st.measuredMs < 0.0;
        n.members = 0;
        for (int k = 0; k < kMaxInputs + kMaxBuses; ++k)
            if (const auto& o = e.getSyncState (k); o.group == st.group && o.member) ++n.members;

        const double measured = st.measuredMs, autoMs = st.autoMs, total = e.getChannelDelayMs (ch);
        const bool stateChanged = ! (n == info);
        if (n.offline || stateChanged) { meanMeasured.reset(); meanAuto.reset(); meanTotal.reset(); }
        if (! n.offline) { meanMeasured.add (measured); meanAuto.add (autoMs); meanTotal.add (total); }

        // Bars glide to the latest value (frame by frame, see animate).
        target = { n.offline ? 0.0 : measured, total, scaleMs, true };
        if (! bar.valid) bar = target;

        bool changed = stateChanged;
        if (publish || stateChanged || ! label.valid)
        {
            Label l;
            l.measured = n.offline ? -1.0 : meanMeasured.take (measured);
            l.autoMs = meanAuto.take (autoMs);
            l.total = meanTotal.take (total);
            l.valid = true;
            changed |= ! (l == label);
            label = l;
        }
        info = n;
        if (changed)
        {
            setTooltip (ioOf (e, ch).kind == IoTarget::Kind::None ? juce::String ("No device chosen for this channel.")
                                                                  : explainIoLatency (e, ioOf (e, ch), ch < kMaxInputs));
            repaint();
        }
        return changed;
    }

    /** Display frame: the bar glides to the latest measurement. */
    void animate (double dt)
    {
        constexpr double eps = 0.01;   // ms
        bool moved = glide (bar.measured, target.measured, dt, eps);
        moved |= glide (bar.total, target.total, dt, eps);
        moved |= glide (bar.scale, target.scale, dt, eps);
        if (moved) repaint();
    }

    /** Result as shown in the numbers (mean of the last second). */
    double resultMs() const { return label.measured < 0.0 ? -1.0 : label.measured + label.total; }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        g.setColour (theme::panel.withAlpha (info.member ? 0.95f : 0.6f));
        g.fillRoundedRectangle (r.toFloat().reduced (0.0f, 2.0f), 7.0f);
        if (info.member)
        {
            g.setColour (theme::good.withAlpha (0.35f));
            g.drawRoundedRectangle (r.toFloat().reduced (0.5f, 2.5f), 7.0f, 1.0f);
        }
        r.reduce (12, 0);

        g.setColour (colourOf (ch));
        g.setFont (theme::font (12.5f, true));
        g.drawFittedText (info.title, r.removeFromLeft (kColChannel - 12), juce::Justification::centredLeft, 1, 0.75f);
        r.removeFromLeft (kColGap);
        g.setColour (theme::textDim);
        g.setFont (theme::font (12.0f));
        g.drawFittedText (info.device, r.removeFromLeft (kColDevice), juce::Justification::centredLeft, 1, 0.75f);
        r.removeFromLeft (kColGap);

        g.setColour (label.measured < 0.0 ? theme::mute : theme::text);
        g.setFont (theme::mono (12.5f));
        g.drawText (ms (label.measured), r.removeFromLeft (kColLatency), juce::Justification::centredRight);
        r.removeFromLeft (kColGap + kColSync + kColGap);   // switch

        // Automatic part
        juce::String autoText;
        auto autoColour = theme::textDim;
        if (! info.syncOn)                 autoText = "-";
        else if (info.offline)             autoText = "device offline";
        else if (info.members < 2)         { autoText = "needs a 2nd channel"; autoColour = theme::warn; }
        else if (info.reference)           { autoText = "slowest: sets the time"; autoColour = theme::good; }
        else                               { autoText = "+" + ms (label.autoMs); autoColour = theme::good; }
        g.setColour (autoColour);
        g.setFont (info.reference || info.members < 2 ? theme::font (11.5f, true) : theme::mono (12.5f));
        g.drawFittedText (autoText, r.removeFromLeft (kColAuto), juce::Justification::centredLeft, 1, 0.75f);
        r.removeFromLeft (kColGap + kColManual + kColGap);   // manual box

        const double result = resultMs();
        g.setColour (result < 0.0 ? theme::mute : theme::text);
        g.setFont (theme::mono (13.0f));
        g.drawText (ms (result), r.removeFromLeft (kColResult), juce::Justification::centredRight);
        r.removeFromLeft (kColGap + 4);

        // Bar: own latency solid, added delay striped
        if (result >= 0.0 && r.getWidth() > 20)
        {
            auto area = r.toFloat().withSizeKeepingCentre ((float) r.getWidth(), 12.0f);
            g.setColour (theme::bg);
            g.fillRoundedRectangle (area, 3.0f);
            const float px = area.getWidth() / (float) juce::jmax (1.0, bar.scale);
            auto own = area.withWidth (juce::jmin (area.getWidth(), (float) bar.measured * px));
            g.setColour (colourOf (ch).withAlpha (0.8f));
            g.fillRoundedRectangle (own, 3.0f);
            if (bar.total > 0.005)
                stripes (g, juce::Rectangle<float> (own.getRight(), area.getY(),
                                                    juce::jmin (area.getRight() - own.getRight(), (float) bar.total * px), area.getHeight()),
                         theme::good);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12, 0);
        r.removeFromLeft (kColChannel - 12 + kColGap + kColDevice + kColGap + kColLatency + kColGap);
        sync.setBounds (r.removeFromLeft (kColSync).withSizeKeepingCentre (kColSync, 24));
        r.removeFromLeft (kColGap + kColAuto + kColGap);
        manual.setBounds (r.removeFromLeft (kColManual).withSizeKeepingCentre (kColManual, 24));
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mouseWasClicked() && ctx.select) ctx.select (ch < kMaxInputs ? ch : busSelectIndex (ch - kMaxInputs));
    }

private:
    /** What the row is (changes are shown at once). */
    struct Info
    {
        juce::String title, device;
        bool syncOn = false, member = false, reference = false, offline = true;
        int members = -1;
        bool operator== (const Info& o) const
        {
            return title == o.title && device == o.device && syncOn == o.syncOn && member == o.member
                && reference == o.reference && offline == o.offline && members == o.members;
        }
    };
    /** Numbers as written (mean of the last second). */
    struct Label
    {
        double measured = -1.0, autoMs = 0.0, total = 0.0;
        bool valid = false;
        bool operator== (const Label& o) const
        {
            auto same = [] (double a, double b) { return std::abs (a - b) < 0.005; };
            return valid == o.valid && same (measured, o.measured) && same (autoMs, o.autoMs) && same (total, o.total);
        }
    };
    /** Bar lengths in ms: target = latest measurement, bar = as drawn (glides to target). */
    struct Bar { double measured = 0.0, total = 0.0, scale = 10.0; bool valid = false; };

    UiContext& ctx;
    Switch sync;
    ValueBox manual;
    Info info;
    Label label;
    Bar target, bar;
    Mean meanMeasured, meanAuto, meanTotal;
};

//==============================================================================
/** Explanation, then one section per group: caption with "All on" / "All off", column heads, rows. */
class SyncDelayView::Content final : public juce::Component
{
public:
    explicit Content (UiContext& c) : ctx (c) {}

    struct Section
    {
        juce::String title;
        juce::Colour colour;
        Engine::Group group;
        std::vector<std::unique_ptr<Row>> rows;
        std::unique_ptr<Chip> allOn, allOff;
        juce::Rectangle<int> head;
        juce::String summary;
    };
    std::vector<Section> sections;

    void build()
    {
        sections.clear();
        auto& e = ctx.engine;
        auto add = [&] (juce::String title, juce::Colour colour, Engine::Group group, std::vector<int> channels)
        {
            Section s { std::move (title), colour, group };
            for (int ch : channels)
            {
                s.rows.push_back (std::make_unique<Row> (ctx, ch));
                addAndMakeVisible (*s.rows.back());
            }
            s.allOn = std::make_unique<Chip> ("Sync all", theme::good);
            s.allOff = std::make_unique<Chip> ("Sync none", theme::text);
            for (auto* chip : { s.allOn.get(), s.allOff.get() })
            {
                chip->showArrow = false;
                chip->centred = true;
                addAndMakeVisible (chip);
            }
            s.allOn->setTooltip ("Auto sync on for every " + s.title.toLowerCase().trimCharactersAtEnd ("s") + " in use");
            s.allOff->setTooltip ("Auto sync off for the whole group (manual delays stay)");
            auto setAll = [this, channels] (bool on)
            {
                for (int ch : channels) syncParam (ctx.engine, ch).set (on ? 1.0f : 0.0f);
            };
            s.allOn->onClick = [setAll] { setAll (true); };
            s.allOff->onClick = [setAll] { setAll (false); };
            sections.push_back (std::move (s));
        };
        std::vector<int> ins, hw, virt;
        for (int i = 0; i < kMaxInputs; ++i) if (e.isInputActive (i)) ins.push_back (i);
        for (int b = 0; b < kMaxBuses; ++b)
            if (e.isBusActive (b)) (isHwBus (b) ? hw : virt).push_back (Engine::busChannel (b));
        add ("Inputs", theme::accentIn, Engine::Group::inputs, ins);
        add ("Hardware Outputs", theme::accentHw, Engine::Group::hwBuses, hw);
        add ("Virtual Outputs", theme::accentVirt, Engine::Group::virtBuses, virt);
    }

    static constexpr int kIntroH = 70;

    int neededHeight() const
    {
        int h = kIntroH;
        for (auto& s : sections) h += kSectionHeadH + juce::jmax (1, (int) s.rows.size()) * kRowH + kSectionGap;
        return h + kPad;
    }

    static int neededWidth()
    {
        return 2 * kPad + 24 + kColChannel + kColDevice + kColLatency + kColSync + kColAuto + kColManual + kColResult
             + 8 * kColGap + kMinBar;
    }

    /** Display frame: bars glide. */
    void animate (double dt)
    {
        for (auto& s : sections)
            for (auto& r : s.rows) r->animate (dt);
    }

    /** Updates every row (publish: numbers are rewritten); repaints the section heads if their summary changed. */
    void update (bool publish)
    {
        for (auto& s : sections)
        {
            // Bars of a group share one scale: the longest result in it (at least 10 ms).
            double longest = 10.0;
            for (auto& r : s.rows) longest = juce::jmax (longest, r->resultMs());
            for (auto& r : s.rows) r->scaleMs = longest * 1.05;
            for (auto& r : s.rows) r->update (publish);

            int members = 0;
            double synced = -1.0;
            for (auto& r : s.rows)
                if (const auto& st = ctx.engine.getSyncState (r->ch); st.member)
                {
                    ++members;
                    synced = juce::jmax (synced, r->resultMs());
                }
            juce::String summary = members >= 2 ? juce::String (members) + " channels in sync at " + ms (synced)
                                 : members == 1 ? juce::String ("Turn on Auto sync for one more channel to align them")
                                                : juce::String();
            if (summary != s.summary)
            {
                s.summary = summary;
                repaint (s.head);
            }
        }
    }

    void paint (juce::Graphics& g) override
    {
        auto intro = juce::Rectangle<int> (kPad, 6, getWidth() - 2 * kPad, kIntroH - 14);
        g.setColour (theme::textDim);
        g.setFont (theme::font (12.5f));
        g.drawFittedText ("Devices have different latencies, so e.g. two microphones on two interfaces reach the mix at slightly "
                          "different times. Switch on Auto sync for the channels that belong together: Hydra delays the faster "
                          "ones so all of them line up with the slowest, following the live latency. A manual delay is added on "
                          "top. Hover a row to see where its latency comes from.",
                          intro, juce::Justification::topLeft, 3, 0.9f);

        for (auto& s : sections)
        {
            auto h = s.head;
            g.setColour (s.colour);
            g.setFont (theme::font (15.0f, true));
            const int tw = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), s.title);
            auto top = h.removeFromTop (30);
            g.drawText (s.title, top.removeFromLeft (tw + 12), juce::Justification::centredLeft);
            g.setColour (s.summary.startsWith ("Turn") ? theme::warn : theme::good);
            g.setFont (theme::font (12.0f, true));
            g.drawFittedText (s.summary, top.withTrimmedLeft (8).withRight (s.allOn->getX() - 10), juce::Justification::centredLeft, 1, 0.8f);

            // Column heads
            auto cols = h.withTrimmedLeft (12);
            g.setColour (theme::textFaint);
            g.setFont (theme::font (10.5f, true));
            auto col = [&] (int w, const char* text, juce::Justification j = juce::Justification::centredLeft)
            {
                g.drawText (text, cols.removeFromLeft (w), j);
                cols.removeFromLeft (kColGap);
            };
            col (kColChannel - 12, "CHANNEL");
            col (kColDevice, "DEVICE");
            col (kColLatency, "LATENCY", juce::Justification::centredRight);
            col (kColSync, "AUTO SYNC");
            col (kColAuto, "AUTO DELAY");
            col (kColManual, "MANUAL DELAY", juce::Justification::centred);
            col (kColResult, "RESULT", juce::Justification::centredRight);
            cols.removeFromLeft (4);
            g.drawText ("OWN LATENCY + DELAY", cols, juce::Justification::centredLeft);

            if (s.rows.empty())
            {
                g.setColour (theme::textFaint);
                g.setFont (theme::font (12.0f));
                g.drawText ("No channel in use.", s.head.getX() + 12, s.head.getBottom(), 300, kRowH, juce::Justification::centredLeft);
            }
        }
    }

    void resized() override
    {
        int y = kIntroH;
        for (auto& s : sections)
        {
            s.head = { kPad, y, getWidth() - 2 * kPad, kSectionHeadH };
            auto top = s.head.withHeight (30);
            s.allOff->setBounds (top.removeFromRight (96).withSizeKeepingCentre (96, 24));
            top.removeFromRight (8);
            s.allOn->setBounds (top.removeFromRight (96).withSizeKeepingCentre (96, 24));
            y += kSectionHeadH;
            for (auto& r : s.rows)
            {
                r->setBounds (kPad, y, getWidth() - 2 * kPad, kRowH);
                y += kRowH;
            }
            if (s.rows.empty()) y += kRowH;
            y += kSectionGap;
        }
    }

private:
    UiContext& ctx;
};

//==============================================================================
SyncDelayView::SyncDelayView (UiContext& c) : ctx (c)
{
    content = std::make_unique<Content> (c);
    viewport.setViewedComponent (content.get(), false);
    viewport.setScrollBarsShown (true, true, true, true);
    viewport.setScrollBarThickness (10);
    addAndMakeVisible (viewport);
    rebuild();
    startTimerHz (10);
    setSize (1180, 640);
}

SyncDelayView::~SyncDelayView() { stopTimer(); }

void SyncDelayView::rebuild()
{
    shownLayout = ctx.engine.getLayoutVersion();
    content->build();
    content->update (true);
    lastPublish = juce::Time::getMillisecondCounterHiRes();
    resized();
}

void SyncDelayView::timerCallback()
{
    if (ctx.engine.getLayoutVersion() != shownLayout) { rebuild(); return; }
    const double now = juce::Time::getMillisecondCounterHiRes();
    const bool publish = now - lastPublish >= kLabelPeriodMs;
    if (publish) lastPublish = now;
    content->update (publish);
}

void SyncDelayView::frame (double timestamp)
{
    content->animate (clock.step (timestamp));
}

void SyncDelayView::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);
    g.setColour (theme::text);
    g.setFont (theme::font (15.0f, true));
    g.drawText ("Sync Delay", getLocalBounds().reduced (kPad, 0).removeFromTop (40).withTrimmedTop (8), juce::Justification::centredLeft);
}

void SyncDelayView::resized()
{
    auto r = getLocalBounds().withTrimmedTop (40);
    viewport.setBounds (r);
    const int w = juce::jmax (Content::neededWidth(), r.getWidth() - (content->neededHeight() > r.getHeight() ? 10 : 0));
    content->setSize (w, content->neededHeight());
}
} // namespace hydra

#include "CableManager.h"
#include <array>
#include "state/Session.h"

namespace hydra
{
namespace
{
constexpr int kPad = 16, kHeader = 54, kColumns = 26, kRow = 64, kGroup = 34, kFooter = 46;

// Column layout (x from the left edge of a row); the health column takes the rest.
struct Columns
{
    juce::Rectangle<int> num, windows, hydra, apps, format, timing, chans, health, remove;
    explicit Columns (juce::Rectangle<int> r)
    {
        r = r.reduced (8, 0);
        remove  = r.removeFromRight (32);
        num     = r.removeFromLeft (40);
        windows = r.removeFromLeft (juce::jmax (210, r.getWidth() * 25 / 100)); r.removeFromLeft (10);
        hydra   = r.removeFromLeft (juce::jmax (190, r.getWidth() * 27 / 100)); r.removeFromLeft (10);
        apps    = r.removeFromLeft (juce::jmax (140, r.getWidth() * 20 / 100)); r.removeFromLeft (10);
        format  = r.removeFromLeft (112); r.removeFromLeft (8);
        timing  = r.removeFromLeft (78);  r.removeFromLeft (8);
        chans   = r.removeFromLeft (70);  r.removeFromLeft (10);
        health  = r;
    }
};

juce::String rateText (int hz)
{
    return hz % 1000 == 0 ? juce::String (hz / 1000) + "k" : juce::String (hz / 1000.0, 1) + "k";
}

juce::String shortName (const juce::String& friendly) { return friendly.upToLastOccurrenceOf (" (", false, false).trim(); }

/** Up to three lines of text in an area, each with its own colour. */
struct Lines
{
    struct Line { juce::String text; juce::Colour colour; bool bold = false; };
    std::vector<Line> lines;
    void add (const juce::String& t, juce::Colour c, bool bold = false) { lines.push_back ({ t, c, bold }); }
    void draw (juce::Graphics& g, juce::Rectangle<int> r) const
    {
        const int h = 17, total = h * (int) lines.size();
        int y = r.getCentreY() - total / 2;
        for (auto& l : lines)
        {
            g.setColour (l.colour);
            g.setFont (theme::font (l.bold ? 13.0f : 12.0f, l.bold));
            g.drawText (l.text, r.getX(), y, r.getWidth(), h, juce::Justification::centredLeft, true);
            y += h;
        }
    }
};
} // namespace

//==============================================================================
/** Caption of a group ("Inputs", "Outputs", "Unused"). */
class CableManager::Group final : public juce::Component
{
public:
    Group (const juce::String& t, const juce::String& h) : title (t), hint (h) {}
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().withTrimmedTop (10).reduced (8, 0);
        g.setColour (theme::text);
        g.setFont (theme::font (14.0f, true));
        const int w = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), title) + 14;
        g.drawText (title, r.removeFromLeft (w), juce::Justification::centredLeft, false);
        g.setColour (theme::textFaint);
        g.setFont (theme::font (12.0f));
        g.drawText (hint, r, juce::Justification::centredLeft, true);
    }
    juce::String title, hint;
};

//==============================================================================
class CableManager::Row final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit Row (CableManager& o) : owner (o)
    {
        addAndMakeVisible (timing);
        for (int ms : { 1, 2, 3, 4, 5, 7, 10, 15, 20, 30, 50 })
            timing.addItem (juce::String (ms) + " ms", ms);
        timing.setTooltip ("Timing (\"ms per interrupt\"): how often the cable moves audio. Smaller = less latency, "
                           "larger = more robust against dropouts on a busy system. Takes effect when the streams "
                           "on the cable restart (for Hydra: Tools > Restart Audio Engine).");
        timing.onChange = [this]
        {
            const int ms = timing.getSelectedId();
            if (ms <= 0 || ms == cable.msPerInt) return;
            const bool ok = vac::setMsPerInt (cable.number, ms);
            owner.report (ok, ok ? "Cable " + juce::String (cable.number) + ": timing " + juce::String (ms) + " ms (applies when its streams restart)."
                                 : "The driver did not accept the timing.");
        };

        addAndMakeVisible (chans);
        for (int c : { 1, 2, 4, 6, 8, 16, 32 })
            chans.addItem (c == 1 ? "Mono" : juce::String (c) + " ch", c);
        chans.setTooltip ("Most channels an app may use on this cable (2 = stereo). Takes effect for new streams.");
        chans.onChange = [this]
        {
            const int c = chans.getSelectedId();
            if (c <= 0 || c == cable.range.maxChannels) return;
            auto r = cable.range;
            r.maxChannels = c;
            r.minChannels = juce::jmin (r.minChannels, c);
            const bool ok = vac::setFormatRange (cable.number, r);
            owner.report (ok, ok ? "Cable " + juce::String (cable.number) + ": up to " + juce::String (c) + " channels."
                                 : "The driver did not accept the channel count.");
        };

        addChildComponent (remove);
        remove.setTooltip ("Remove this cable (the driver restarts: all cables are gone for a moment)");
        remove.onClick = [this]
        {
            if (auto* v = owner.vac())
                v->setCount (cable.number - 1, [safe = juce::Component::SafePointer<CableManager> (&owner)] (bool ok, const juce::String& m)
                             { if (safe) safe->report (ok, m); });
        };
    }

    void set (const vac::Cable& c, const std::vector<vac::Stream>& streams, bool removable, bool busy)
    {
        cable = c;
        auto* v = owner.vac();
        auto& engine = owner.ctx.engine;
        const auto* hiddenByHydra = owner.ctx.session != nullptr ? &owner.ctx.session->vacHidden : nullptr;

        // Windows: both sides, their names and whether apps see them.
        windows = {};
        for (bool capture : { false, true })
        {
            const juce::String side = capture ? "Rec" : "Play";
            const vac::Endpoint* ep = nullptr;
            if (v != nullptr) for (auto& e : v->getEndpoints()) if (e.cable == c.number && e.capture == capture) ep = &e;
            if (ep == nullptr) { windows.add (side + ": missing in Windows", theme::warn); continue; }
            const bool ours = hiddenByHydra != nullptr && std::count (hiddenByHydra->begin(), hiddenByHydra->end(), ep->id) > 0;
            windows.add (side + ": " + shortName (ep->friendly) + (ep->visible ? "" : ours ? "  (hidden)" : "  (off in Windows)"),
                         ep->visible ? theme::text : ours ? theme::textFaint : theme::warn, false);
        }

        // Hydra: channels on the cable, how they are connected, their health.
        hydra = {};
        auto addUse = [&] (const juce::String& label, const IoTarget& t, bool input)
        {
            const auto lat = engine.getIoLatency (t, input);
            const auto health = engine.getStreamHealth (t, input);
            juce::String how;
            juce::Colour col = theme::textDim;
            using S = Engine::StreamHealth::State;
            if (health.state == S::offline)      { how = "offline" + (health.error.isNotEmpty() ? ": " + health.error : juce::String()); col = theme::mute; }
            else if (health.state == S::waiting) { how = "opening..."; col = theme::warn; }
            else if (lat.transport.isNotEmpty()) how = lat.steeredClock ? "Kernel Streaming, clock steered" : "Kernel Streaming, resampled";
            else                                 how = "through Windows (WASAPI)";
            int drops = 0;
            for (auto& d : health.recent) drops += d.count;
            if (drops > 0) { how << ", " << drops << " dropout" << (drops == 1 ? "" : "s") << " ("
                                 << juce::String ((int) health.recent.front().secondsAgo) << " s ago)"; col = theme::warn; }
            hydra.add (label + (input ? "  records it" : "  plays into it"), theme::text, true);
            hydra.add (how, col);
        };
        for (int i = 0; i < kMaxInputs; ++i)
            if (engine.isInputActive (i) && v != nullptr && v->cableOf (engine.inputs[(size_t) i].source) == c.number)
                addUse (("In " + juce::String (i + 1) + " " + engine.inputs[(size_t) i].name).trim(), engine.inputs[(size_t) i].source, true);
        for (int b = 0; b < kMaxBuses; ++b)
            if (engine.isBusActive (b) && v != nullptr && v->cableOf (engine.buses[(size_t) b].output) == c.number)
                addUse ((busLabel (b) + " " + engine.buses[(size_t) b].name).trim(), engine.buses[(size_t) b].output, false);
        used = ! hydra.lines.empty();
        if (! used)
        {
            const bool reusable = v != nullptr && v->isReusable (c.number);
            hydra.add ("not used", theme::textFaint, true);
            hydra.add (reusable ? "free: the next new cable takes it" : "not reusable while an app streams on it", reusable ? theme::good : theme::warn);
        }

        // Windows: other apps streaming on the cable (audiodg = through Windows' audio engine).
        apps = {};
        for (auto& st : streams)
            if (st.cable == c.number && ! VacCables::isOwn (st))
                apps.add (st.process.upToLastOccurrenceOf (".exe", false, true) + (st.capture ? " records" : " plays")
                              + " (PID " + juce::String ((int) st.processId) + ")",
                          theme::text);
        if (apps.lines.empty()) apps.add ("no app", theme::textFaint);
        else if (apps.lines.size() > 3) { apps.lines.resize (2); apps.add ("...", theme::textDim); }
        setTooltip (apps.lines.size() > 0 ? "audiodg = an app playing / recording through Windows' audio engine "
                                             "(e.g. \"Listen to this device\", or any app that has the cable open)."
                                           : juce::String());

        if (! timing.isPopupActive())
        {
            if (timing.indexOfItemId (c.msPerInt) < 0) timing.addItem (juce::String (c.msPerInt) + " ms", c.msPerInt);
            timing.setSelectedId (c.msPerInt, juce::dontSendNotification);
        }
        if (! chans.isPopupActive())
        {
            if (chans.indexOfItemId (c.range.maxChannels) < 0) chans.addItem (juce::String (c.range.maxChannels) + " ch", c.range.maxChannels);
            chans.setSelectedId (c.range.maxChannels, juce::dontSendNotification);
        }
        remove.setVisible (removable);
        remove.setEnabled (! busy);
        timing.setEnabled (! busy);
        chans.setEnabled (! busy);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const Columns col (getLocalBounds());
        g.setColour (theme::panel);
        g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (0.0f, 2.0f), 7.0f);

        g.setFont (theme::font (15.0f, true));
        g.setColour (used ? theme::text : theme::textDim);
        g.drawText (juce::String (cable.number), col.num, juce::Justification::centred);

        windows.draw (g, col.windows);
        hydra.draw (g, col.hydra);
        apps.draw (g, col.apps);

        // Format now, and the range below it.
        const int streams = cable.inStreams + cable.outStreams;
        Lines f;
        f.add (streams > 0 && cable.rate > 0 ? rateText (cable.rate) + " / " + juce::String (cable.bits) + " bit / " + juce::String (cable.channels) + " ch"
                                             : juce::String ("idle"), streams > 0 ? theme::text : theme::textFaint);
        f.add (rateText (cable.range.minRate) + "-" + rateText (cable.range.maxRate) + " Hz, "
                   + juce::String (cable.range.minBits) + "-" + juce::String (cable.range.maxBits) + " bit", theme::textFaint);
        f.draw (g, col.format);

        // Health: VAC's own buffer problems and the regularity of its timer.
        Lines h;
        const bool trouble = cable.overflows + cable.underflows > 0;
        h.add (trouble ? juce::String (cable.underflows) + " underflows, " + juce::String (cable.overflows) + " overflows"
                       : streams > 0 ? juce::String ("no over-/underflows") : juce::String ("-"),
               trouble ? theme::warn : (streams > 0 ? theme::good : theme::textFaint));
        if (streams > 0 && cable.eventAvgUs > 0)
        {
            const bool jittery = cable.eventMaxUs > cable.msPerInt * 1000 * 2;   // late timer events make a cable stutter
            h.add ("period " + juce::String (cable.eventAvgUs / 1000.0, 2) + " ms (max " + juce::String (cable.eventMaxUs / 1000.0, 1) + ")",
                   jittery ? theme::warn : theme::textFaint);
        }
        if (cable.minRtBufferRender > 1 || cable.minRtBufferCapture > 1)   // VAC 4.71 (API 7)
            h.add ("app buffer min. " + juce::String (juce::jmax (cable.minRtBufferRender, cable.minRtBufferCapture)) + " periods", theme::textFaint);
        h.draw (g, col.health);
    }

    /** Right-click: show / hide a side in Windows by hand (Hydra corrects the sides it manages again). */
    void mouseDown (const juce::MouseEvent& e) override
    {
        auto* v = owner.vac();
        if (! e.mods.isPopupMenu() || v == nullptr) return;
        juce::PopupMenu m;
        m.addSectionHeader ("Cable " + juce::String (cable.number) + " in Windows");
        int id = 1;
        std::vector<std::pair<juce::String, bool>> actions;
        for (auto& ep : v->getEndpoints())
        {
            if (ep.cable != cable.number) continue;
            const juce::String side = ep.capture ? "recording side" : "playback side";
            m.addItem (id++, (ep.visible ? "Hide " : "Show ") + side + " (\"" + ep.friendly + "\")");
            actions.emplace_back (ep.id, ! ep.visible);
        }
        m.showMenuAsync (juce::PopupMenu::Options(), [safe = juce::Component::SafePointer<CableManager> (&owner), actions] (int r)
        {
            if (safe == nullptr || r <= 0 || r > (int) actions.size()) return;
            const auto& [endpoint, show] = actions[(size_t) r - 1];
            const bool ok = vac::setEndpointVisible (endpoint, show);
            if (auto* vc = safe->vac()) vc->refresh();
            safe->report (ok, ok ? juce::String (show ? "Shown in Windows (Hydra hides it again if it manages this side)." : "Hidden in Windows.")
                                 : juce::String ("Windows did not accept the change."));
        });
    }

    void resized() override
    {
        const Columns col (getLocalBounds());
        timing.setBounds (col.timing.withSizeKeepingCentre (col.timing.getWidth(), 26));
        chans.setBounds (col.chans.withSizeKeepingCentre (col.chans.getWidth(), 26));
        remove.setBounds (col.remove.withSizeKeepingCentre (24, 24));
    }

    vac::Cable cable;
    bool used = false;

private:
    CableManager& owner;
    juce::ComboBox timing, chans;
    TrashButton remove;
    Lines windows, hydra, apps;
};

//==============================================================================
CableManager::CableManager (UiContext& c) : ctx (c)
{
    setSize (1040, 520);
    addAndMakeVisible (view);
    view.setViewedComponent (&rowHolder, false);
    view.setScrollBarsShown (true, false);
    addAndMakeVisible (help);
    help.setText ("Virtual Audio Cable\nEach cable has a playback side (apps play into it) and a recording side (apps record from it). "
                  "Adding or removing a cable restarts the driver: every cable is gone for a moment and apps using one may have to "
                  "reconnect. Windows asks for administrator rights.\n\n"
                  "Health shows the cable's own over-/underflows (its buffer ran over or empty) and how regularly its timer runs. "
                  "Dropouts that Hydra counts on a channel are in Hydra's sync buffer instead; it adapts after a dropout.");
    for (auto* b : { &addIn, &addOut, &reset, &apply, &done }) addAndMakeVisible (b);
    addIn.setTooltip ("A new input with a cable of its own: apps play into \"Hydra In N\", the input records it. Takes a free "
                      "cable, or adds one (driver restart, Windows asks for administrator rights).");
    addOut.setTooltip ("A new virtual output (V bus) with a cable of its own: it plays into the cable, apps record from "
                       "\"Hydra Out N\". Takes a free cable, or adds one (driver restart, Windows asks for administrator rights).");
    addIn.onClick  = [this] { addChannelWithCable (true); };
    addOut.onClick = [this] { addChannelWithCable (false); };
    reset.onClick = [this]
    {
        for (auto& cab : cables) vac::resetCounters (cab.number);
        report (true, "Counters reset.");
    };
    apply.setTooltip ("A change waits for a driver restart: restart it now");
    apply.onClick = [this]
    {
        if (auto* v = vac())
            v->setCount (v->info().numCables, [safe = juce::Component::SafePointer<CableManager> (this)] (bool ok, const juce::String& m)
                         { if (safe) safe->report (ok, m); });
    };
    done.onClick = [this] { if (onClose) onClose(); };
    if (auto* v = vac()) v->addChangeListener (this);
    reload();
    startTimer (500);
}

CableManager::~CableManager()
{
    if (auto* v = vac()) v->removeChangeListener (this);
    view.setViewedComponent (nullptr, false);
}

void CableManager::addChannelWithCable (bool input)
{
    auto* v = vac();
    if (v == nullptr || v->isBusy() || ! ctx.addChannel) return;
    auto& e = ctx.engine;
    std::array<bool, kMaxInputs> inBefore {};
    std::array<bool, kMaxBuses> busBefore {};
    for (int i = 0; i < kMaxInputs; ++i) inBefore[(size_t) i] = e.isInputActive (i);
    for (int b = 0; b < kMaxBuses; ++b) busBefore[(size_t) b] = e.isBusActive (b);
    ctx.addChannel (input ? 0 : 2);   // the mixer adds the strip (or says that all slots are in use)
    int index = -1;
    if (input) { for (int i = 0; i < kMaxInputs && index < 0; ++i) if (! inBefore[(size_t) i] && e.isInputActive (i)) index = i; }
    else       { for (int b = 0; b < kMaxBuses && index < 0; ++b) if (! busBefore[(size_t) b] && e.isBusActive (b)) index = b; }
    if (index < 0) { report (false, input ? "No free input slot." : "No free virtual output slot."); return; }
    v->addCableFor (input, index, [safe = juce::Component::SafePointer<CableManager> (this)] (bool ok, const juce::String& m)
                    { if (safe) safe->report (ok, m); });
}

VacCables* CableManager::vac() const { return ctx.session != nullptr ? ctx.session->vacCables : nullptr; }

void CableManager::visibilityChanged()
{
    if (isVisible()) reload();
}

void CableManager::timerCallback()
{
    if (! isShowing()) return;
    if (++tick % 2 == 0)
        if (auto* v = vac(); v != nullptr && ! v->isBusy()) v->refresh();   // names, endpoints: 1 Hz
    reload();
}

void CableManager::changeListenerCallback (juce::ChangeBroadcaster*)
{
    reload();
}

void CableManager::report (bool, const juce::String& msg)
{
    message = msg;
    reload();
}

void CableManager::reload()
{
    auto* v = vac();
    const bool busy = v != nullptr && v->isBusy();
    if (! busy) { cables = vac::cables(); streams = vac::streams(); }

    // Groups: cables an input records, cables a bus plays into, unused ones. Rebuilt when the grouping changes.
    std::vector<int> order[3];
    for (size_t i = 0; i < cables.size(); ++i)
    {
        const int sides = v != nullptr ? v->usedSides (cables[i].number) : 0;
        order[(sides & 1) != 0 ? 0 : sides != 0 ? 1 : 2].push_back ((int) i);
    }
    juce::String key;
    for (auto& o : order) { for (int i : o) key << i << ','; key << '|'; }
    if (key != layoutKey)
    {
        layoutKey = key;
        rows.clear();
        groups.clear();
        rowOrder.clear();
        static const char* titles[] = { "Inputs", "Outputs", "Unused" };
        static const char* hints[]  = { "apps play into these cables, an input of Hydra records them",
                                        "a bus of Hydra plays into these cables, apps record from them",
                                        "hidden in Windows; a new cable for a channel takes one of these first" };
        for (int gi = 0; gi < 3; ++gi)
        {
            auto* grp = groups.add (new Group (titles[gi], hints[gi]));
            grp->title << "  (" << (int) order[gi].size() << ")";
            rowHolder.addAndMakeVisible (grp);
            for (int i : order[gi]) { rowHolder.addAndMakeVisible (rows.add (new Row (*this))); rowOrder.push_back (i); }
            groupSizes[gi] = (int) order[gi].size();
        }
        resized();
    }
    const int removable = v != nullptr && ! busy ? v->removableAtEnd (cables) : 0;
    for (int r = 0; r < rows.size(); ++r)
    {
        const auto i = (size_t) rowOrder[(size_t) r];
        rows[r]->set (cables[i], streams, i + 1 == cables.size() && removable > 0, busy);
    }

    const auto& info = v != nullptr ? v->info() : vac::Info();
    const bool canAdd = v != nullptr && info.installed && ! busy
                     && ((info.edition != "lite" && info.numCables < 256) || v->freeCable() > 0);
    addIn.setEnabled (canAdd);
    addOut.setEnabled (canAdd);
    if (apply.isVisible() != info.restartPending) { apply.setVisible (info.restartPending); resized(); }
    apply.setEnabled (! busy);
    reset.setEnabled (! busy && ! cables.empty());
    repaint (headerArea);
    repaint (footerText);
}

void CableManager::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);
    auto* v = vac();
    const auto& info = v != nullptr ? v->info() : vac::Info();

    auto h = headerArea;
    g.setColour (theme::text);
    g.setFont (theme::font (18.0f, true));
    g.drawText ("Virtual Audio Cable", h.removeFromTop (h.getHeight() / 2 + 2), juce::Justification::bottomLeft, false);
    g.setFont (theme::font (13.0f));
    g.setColour (theme::textDim);
    g.drawText (info.installed ? "Version " + info.version + " (" + info.edition + ") - " + juce::String (info.numCables)
                                     + (info.numCables == 1 ? " cable" : " cables") + (info.restartPending ? " - restart pending" : "")
                               : "Not available: " + info.error,
                h, juce::Justification::topLeft, true);

    const Columns col (columnArea);
    g.setFont (theme::font (12.0f, true));
    g.setColour (theme::textDim);
    g.drawText ("VAC #", col.num, juce::Justification::centred);
    g.drawText ("Sides in Windows", col.windows, juce::Justification::centredLeft);
    g.drawText ("In Hydra", col.hydra, juce::Justification::centredLeft);
    g.drawText ("Other apps", col.apps, juce::Justification::centredLeft);
    g.drawText ("Format", col.format, juce::Justification::centredLeft);
    g.drawText ("Timing", col.timing, juce::Justification::centredLeft);
    g.drawText ("Channels", col.chans, juce::Justification::centredLeft);
    g.drawText ("Health", col.health, juce::Justification::centredLeft);

    const auto st = v != nullptr && v->isBusy() ? v->status() : message;
    g.setFont (theme::font (13.0f));
    g.setColour (v != nullptr && v->isBusy() ? theme::text : theme::textDim);
    g.drawFittedText (st, footerText, juce::Justification::centredLeft, 2, 0.9f);
}

void CableManager::resized()
{
    auto r = getLocalBounds().reduced (kPad);
    headerArea = r.removeFromTop (kHeader);
    addOut.setBounds (headerArea.removeFromRight (140).withSizeKeepingCentre (140, 32));
    headerArea.removeFromRight (8);
    addIn.setBounds (headerArea.removeFromRight (130).withSizeKeepingCentre (130, 32));
    headerArea.removeFromRight (8);
    help.setBounds (headerArea.getX() + 190, headerArea.getY() + headerArea.getHeight() / 2 - InfoButton::kSize - 2, InfoButton::kSize, InfoButton::kSize);

    auto footer = r.removeFromBottom (kFooter).withTrimmedTop (12);
    done.setBounds (footer.removeFromRight (100));
    footer.removeFromRight (8);
    if (apply.isVisible()) { apply.setBounds (footer.removeFromRight (180)); footer.removeFromRight (8); }
    reset.setBounds (footer.removeFromRight (130));
    footer.removeFromRight (12);
    footerText = footer;

    columnArea = r.removeFromTop (kColumns);
    view.setBounds (r);
    const int total = groups.size() * kGroup + rows.size() * kRow;
    const int w = view.getWidth() - (total > r.getHeight() ? view.getScrollBarThickness() : 0);
    rowHolder.setSize (w, total);
    int y = 0, row = 0;
    for (int gi = 0; gi < groups.size(); ++gi)
    {
        groups[gi]->setBounds (0, y, w, kGroup);
        y += kGroup;
        for (int k = 0; k < groupSizes[gi] && row < rows.size(); ++k, ++row)
        {
            rows[row]->setBounds (0, y, w, kRow);
            y += kRow;
        }
    }
    columnArea = columnArea.withWidth (w);
}
} // namespace hydra

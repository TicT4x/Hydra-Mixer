#include "Strips.h"
#include "state/Session.h"
#include "state/ChannelClipboard.h"
#include "ChannelIcons.h"

namespace hydra
{

//==============================================================================
StripHeader::StripHeader (UiContext& c, int sel, juce::String b, juce::Colour col,
                          std::function<juce::String()> gn, std::function<void (juce::String)> sn)
    : ctx (c), selectIndex (sel), badge (std::move (b)), colour (col), getName (std::move (gn))
{
    addAndMakeVisible (name);
    name.setFont (theme::font (13.5f, true));
    name.setColour (juce::Label::textColourId, theme::text);
    name.setEditable (false, true, false);
    name.setInterceptsMouseClicks (false, false);
    name.onTextChange = [this, sn] { sn (name.getText()); };
    name.setTooltip ("Double-click to rename");
    refresh();
}

void StripHeader::resized()
{
    name.setBounds (getLocalBounds().withTrimmedLeft (30));
}

void StripHeader::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        ctx.select (selectIndex);
        showMenu();
    }
}

void StripHeader::mouseUp (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) return;
    if (e.getNumberOfClicks() >= 2) name.showEditor();
    else ctx.select (selectIndex);
}

void StripHeader::showMenu()
{
    juce::Component::SafePointer<StripHeader> safe (this);
    showChannelMenu (ctx, selectIndex, *this, [safe] { if (safe != nullptr) safe->name.showEditor(); });
}

void showChannelMenu (UiContext& ctx, int sel, juce::Component& target, std::function<void()> rename)
{
    auto& e = ctx.engine;
    const bool isInput = sel < kMaxInputs;
    const int bus = sel - kMaxInputs;
    const int group = isInput ? 0 : isHwBus (bus) ? 1 : 2;
    const auto name = isInput ? e.inputs[(size_t) sel].name : e.buses[(size_t) bus].name;

    juce::String blocked;
    if (isInput && e.numActiveInputs() <= 1)                         blocked = "the last input stays";
    else if (! isInput && isHwBus (bus) && e.numActiveBuses (false) <= 1) blocked = "the last hardware output stays";

    // Paste puts what was copied onto this channel: a channel's settings, an effect chain or one effect (at the end).
    const auto held = clip::fromClipboard();
    const auto what = clip::whatOf (held.get());
    juce::String pasteText = "Paste";
    bool canPaste = false;
    if (what == clip::What::channel)    { pasteText = "Paste settings of the copied " + clip::describe (held.get()); canPaste = clip::canPasteChannel (held.get(), sel); }
    else if (what == clip::What::chain) { pasteText = "Paste effect chain"; canPaste = true; }
    else if (what == clip::What::fx)    { pasteText = "Paste  " + clip::describe (held.get()) + "  into the effect chain"; canPaste = clip::canInsertBlock (e, held.get(), sel); }

    juce::PopupMenu m;
    m.addSectionHeader (channelCode (sel) + "   " + name);
    m.addItem ("Rename", std::move (rename));
    if (! isInput)
        icons::addMenu (m, e.buses[(size_t) bus].icon, [&ctx, bus] (juce::String id)
        {
            ctx.engine.buses[(size_t) bus].icon = id;
            ctx.engine.sendChangeMessage();   // strips, routing buttons and the inspector show it
        });
    m.addSeparator();
    m.addItem ("Copy", [&ctx, sel] { clip::toClipboard (*clip::copyChannel (ctx.engine, sel)); });
    m.addItem ("Cut" + (blocked.isEmpty() ? juce::String() : "   (" + blocked + ")"), blocked.isEmpty(), false,
               [&ctx, sel] { if (ctx.cutChannel) ctx.cutChannel (sel); });
    m.addItem (pasteText, canPaste, false, [&ctx, sel]
    {
        auto x = clip::fromClipboard();
        if (x == nullptr) return;
        switch (clip::whatOf (x.get()))
        {
            case clip::What::channel: clip::pasteChannel (ctx.engine, sel, *x); break;
            case clip::What::chain:   clip::pasteChain (ctx.engine, sel, *x); break;
            case clip::What::fx:      clip::insertBlock (ctx.engine, sel, *x, -1); break;
            case clip::What::none:    break;
        }
    });
    if (clip::canPasteAsNew (held.get(), group))
        m.addItem ("Paste as new channel", [&ctx, group] { if (ctx.pasteNewChannel) ctx.pasteNewChannel (group); });
    m.addItem ("Duplicate", [&ctx, sel] { if (ctx.duplicateChannel) ctx.duplicateChannel (sel); });
    m.addItem ("Delete " + channelCode (sel) + (blocked.isEmpty() ? juce::String ("...") : "   (" + blocked + ")"),
               blocked.isEmpty(), false, [&ctx, sel] { if (ctx.removeChannel) ctx.removeChannel (sel); });
    m.addSeparator();
    m.addItem ("Change Order...", [&ctx] { if (ctx.changeOrder) ctx.changeOrder(); });
    m.addItem (isInput ? "Add Input" : isHwBus (bus) ? "Add Hardware Output" : "Add Virtual Output",
               [&ctx, group] { if (ctx.addChannel) ctx.addChannel (group); });
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target).withMousePosition());
}

void showGroupMenu (UiContext& ctx, int group)
{
    const auto held = clip::fromClipboard();
    const char* kind = group == 0 ? "input" : group == 1 ? "hardware output" : "virtual output";
    juce::PopupMenu m;
    m.addSectionHeader (group == 0 ? "Inputs" : group == 1 ? "Hardware Outputs" : "Virtual Outputs");
    m.addItem (juce::String ("Paste as new ") + kind + (held != nullptr && clip::whatOf (held.get()) == clip::What::channel ? "  (" + clip::describe (held.get()) + ")" : juce::String()),
               clip::canPasteAsNew (held.get(), group), false, [&ctx, group] { if (ctx.pasteNewChannel) ctx.pasteNewChannel (group); });
    m.addItem (juce::String ("Add ") + (group == 0 ? "Input" : group == 1 ? "Hardware Output" : "Virtual Output"),
               [&ctx, group] { if (ctx.addChannel) ctx.addChannel (group); });
    m.addSeparator();
    m.addItem ("Change Order...", [&ctx] { if (ctx.changeOrder) ctx.changeOrder(); });
    m.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
}

void makeEditableDbLabel (juce::Label& l, Param& gain)
{
    l.setFont (theme::mono (11.5f));
    l.setJustificationType (juce::Justification::centred);
    l.setColour (juce::Label::textColourId, theme::textDim);
    l.setColour (juce::Label::backgroundWhenEditingColourId, theme::bg);
    l.setColour (juce::Label::textWhenEditingColourId, theme::text);
    l.setColour (juce::Label::outlineWhenEditingColourId, theme::accentIn);
    l.setEditable (true, false, true);
    l.setTooltip ("Click to type a value");
    l.setMouseCursor (juce::MouseCursor::IBeamCursor);
    l.onEditorShow = [&l, &gain]
    {
        if (auto* ed = l.getCurrentTextEditor())
        {
            ed->setJustification (juce::Justification::centred);
            ed->setText (gain.get() <= gain.range.start + 0.001f ? juce::String ("-inf") : juce::String (gain.get(), 1), false);
            ed->selectAll();
        }
    };
    l.onTextChange = [&l, &gain]
    {
        setParamFromText (gain, l.getText());
        l.setText (gain.toText(), juce::dontSendNotification);
    };
}

void StripHeader::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    auto b = r.removeFromLeft (24.0f).withSizeKeepingCentre (24.0f, 22.0f);
    g.setColour (colour.withAlpha (0.18f));
    g.fillRoundedRectangle (b, 6.0f);
    g.setColour (colour);
    if (const auto icon = getIcon ? getIcon() : juce::String(); icons::exists (icon))
    {
        icons::draw (g, icon, b.reduced (4.0f, 3.0f), colour);
        return;
    }
    g.setFont (theme::font (12.5f, true));
    g.drawText (badge, b, juce::Justification::centred);
}

//==============================================================================
namespace
{
void layoutRow (juce::Rectangle<int> row, std::initializer_list<juce::Component*> comps, int gap = 3)
{
    const int n = (int) comps.size();
    const int w = (row.getWidth() - gap * (n - 1)) / n;
    for (auto* c : comps)
    {
        c->setBounds (row.removeFromLeft (w));
        row.removeFromLeft (gap);
    }
}

juce::String agoText (double seconds)
{
    const int s = juce::jmax (0, (int) seconds);
    if (s < 60) return juce::String (s) + " s ago";
    return juce::String (s / 60) + " min " + juce::String (s % 60).paddedLeft ('0', 2) + " s ago";
}

/** Status symbols of a source / output chip: offline (red), dropouts of the last 2 minutes (yellow "!"),
    something that needs attention (yellow dot). Each one explains itself in its tooltip. */
std::vector<Chip::Status> statusFor (const Engine& engine, const IoTarget& t, bool input)
{
    using State = Engine::StreamHealth::State;
    std::vector<Chip::Status> out;
    if (demoMode) return out;
    const auto h = engine.getStreamHealth (t, input);
    const auto name = t.describe();
    switch (h.state)
    {
        case State::none:
            break;
        case State::ok:
            if (t.kind == IoTarget::Kind::Network)
            {
                const auto how = engine.getIoLatency (t, input).transport;
                out.push_back ({ Chip::Marker::connected, juce::String ("Hydra Link: connected.") + (how.isNotEmpty() ? "\n" + how : juce::String()) });
            }
            break;
        case State::offline:
            out.push_back ({ Chip::Marker::offline,
                             "Offline: " + name + " is not running."
                                 + (h.error.isNotEmpty() ? "\n" + h.error : juce::String())
                                 + "\nIs it plugged in and not used exclusively by another app? Hydra retries every few seconds." });
            break;
        case State::waiting:
            if (t.kind == IoTarget::Kind::Network)
            {
                out.push_back ({ Chip::Marker::attention,
                                 input ? "Hydra Link: not connected. Nothing is received from " + name + ".\n"
                                         "Is Hydra running on the other computer and does it send this stream?"
                                       : juce::String ("Hydra Link: not connected. Nobody receives this output yet.\n"
                                                       "On the other computer, choose this stream as the source of an input.") });
                break;
            }
            out.push_back ({ Chip::Marker::attention,
                             "Waiting for the app: " + t.deviceName + " is not running or plays no sound yet.\n"
                             "It is connected automatically as soon as it plays." });
            break;
        case State::noDriver:
            out.push_back ({ Chip::Marker::attention,
                             "Hydra Virtual has no driver yet - nothing is " + juce::String (input ? "received" : "sent") + " here.\n"
                             "Use a virtual cable (VB-Cable, Virtual Audio Cable) instead: Settings > Virtual Cables." });
            break;
    }
    if (const int n = h.recentTotal(); n > 0)
    {
        juce::String tip;
        tip << n << (n == 1 ? " dropout" : " dropouts") << " in the last 2 minutes  (" << name << ")";
        int lines = 0;
        for (auto& d : h.recent)
        {
            if (++lines > 6) { tip << "\n   ..."; break; }
            tip << "\n   " << (d.count > 1 ? juce::String (d.count) + " x  " : juce::String()) << agoText (d.secondsAgo);
        }
        tip << "\nA dropout is a short gap: the device's buffer ran empty or overflowed."
               "\nIf it keeps happening, raise this device's buffer size (or the master's).";
        out.push_back ({ Chip::Marker::dropouts, tip });
    }
    return out;
}

/** Lower part of a strip: fader with its scale, and the level meter beside it. Default theme: fader cap and
    meter together are centred in the strip; the scale takes the room left of the cap. */
void placeFaderAndMeter (juce::Rectangle<int> r, juce::Component& fader, juce::Component& meter)
{
    if (theme::deep())
    {
        constexpr int meterWidth = 18, gap = 4, groupWidth = 30 /* cap, Fader::paint */ + gap + meterWidth;
        auto meterArea = r.withSizeKeepingCentre (juce::jmin (r.getWidth(), groupWidth), r.getHeight()).removeFromRight (meterWidth);
        meter.setBounds (meterArea.reduced (0, 14));
        fader.setBounds (r.withRight (meterArea.getX() - gap));
        return;
    }
    meter.setBounds (r.removeFromRight (12).reduced (0, 14));
    r.removeFromRight (2);
    fader.setBounds (r);
}

void styleSmallLabel (juce::Label& l)
{
    l.setFont (theme::font (10.5f));
    l.setColour (juce::Label::textColourId, theme::textFaint);
    l.setJustificationType (juce::Justification::centred);
    l.setInterceptsMouseClicks (false, false);
}
} // namespace

namespace
{
/** Deep theme: strips of a group form one block (rounded only at its ends) with a line in the group colour on top. */
void paintDeepStrip (juce::Graphics& g, juce::Component& strip, juce::Colour accent, bool selected, juce::Colour tint, bool rows = false)
{
    auto* parent = strip.getParentComponent();
    auto stripAt = [&] (juce::Point<int> p)
    {
        // Only visible strips count (in Change Order mode a transparent layer lies over them).
        if (parent != nullptr)
            for (auto* c : parent->getChildren())
                if (c != &strip && c->isVisible() && c->getBounds().contains (p)
                    && (dynamic_cast<InputStripView*> (c) != nullptr || dynamic_cast<BusStripView*> (c) != nullptr))
                    return true;
        return false;
    };
    // Neighbours: left / right of a strip, above / below a row.
    const auto b = strip.getBounds();
    const bool before = rows ? stripAt ({ b.getCentreX(), b.getY() - 1 }) : stripAt ({ b.getX() - 1, b.getCentreY() });
    const bool after  = rows ? stripAt ({ b.getCentreX(), b.getBottom() }) : stripAt ({ b.getRight(), b.getCentreY() });
    const bool tl = ! before, tr = rows ? ! before : ! after, bl = rows ? ! after : ! before, br = ! after;
    const auto r = strip.getLocalBounds().toFloat();
    juce::Path shape;
    shape.addRoundedRectangle (r.getX(), r.getY(), r.getWidth(), r.getHeight(), 8.0f, 8.0f, tl, tr, bl, br);
    juce::Graphics::ScopedSaveState clip (g);
    g.reduceClipRegion (shape);
    // Selected (user 13.11.): slightly lighter, the line on top thicker and (with glow) lighting the strip from
    // there; no frame any more.
    g.fillAll (selected ? theme::panel2 : theme::panel);
    if (! tint.isTransparent()) g.fillAll (tint);
    if (after)
    {
        g.setColour (theme::outline);
        g.fillRect (rows ? r.withTop (r.getBottom() - 1.0f) : r.withLeft (r.getRight() - 1.0f));
    }
    if (selected)
        theme::drawSelectedTop (g, r, accent, rows);
    else
    {
        g.setColour (accent);
        g.fillRect (rows ? r.withWidth (2.0f) : r.withHeight (2.0f));
    }
}

/** Rows: name column, routing button cell, the switches at the right end (the same width in input and bus rows,
    so all faders end at the same x) and the room from the fader's value to the switches. */
constexpr int kRowName = 170, kRouteCell = 34, kRowSwitches = 138, kRowRight = 10 + kRowSwitches;

/** Right part of a horizontal row: fader with its scale, the meter below the track, the value at the right. */
void placeRowFader (juce::Rectangle<int> r, juce::Component& fader, juce::Component& meter, juce::Component& value)
{
    auto valueArea = r.removeFromRight (StripArea::kRowValue);
    value.setBounds (valueArea.withSizeKeepingCentre (valueArea.getWidth(), 16).translated (0, 8));
    r.removeFromRight (6);
    meter.setBounds (r.removeFromBottom (8).reduced (16, 0));   // as long as the fader track
    r.removeFromBottom (2);
    fader.setBounds (r);
}
} // namespace

InputStripView::InputStripView (UiContext& c, int i)
    : ctx (c), index (i), strip (c.engine.inputs[(size_t) i]),
      header (c, i, juce::String (i + 1), theme::accentIn,
              [this] { return strip.name; }, [this] (juce::String n) { strip.name = n; }),
      trim (c, *strip.trim, "GAIN", theme::accentIn),
      pan (c, *strip.pan, "PAN", theme::accentIn),
      mono (c, *strip.mono, "MONO", theme::good),
      solo (c, *strip.solo, "SOLO", theme::solo),
      mute (c, *strip.mute, "MUTE", theme::mute),
      meter (strip.meter)
{
    solo.exclusiveGroup = [&e = c.engine]
    {
        std::vector<Param*> v;
        for (auto& s : e.inputs) v.push_back (s.solo);
        return v;
    };
    solo.setTooltip ("Solo (Shift + click: solo several channels at once)");
    for (auto* comp : std::initializer_list<juce::Component*> { &header, &source, &trim, &pan,
                                                               &mono, &solo, &mute, &meter, &dbLabel })
        addAndMakeVisible (comp);

    // Routing buttons only for the buses in use.
    for (int b = 0; b < kMaxBuses; ++b)
    {
        if (! c.engine.isBusActive (b)) continue;
        auto& list = isHwBus (b) ? hwRoutes : virtRoutes;
        list.push_back (std::make_unique<Toggle> (c, *strip.route[(size_t) b], busLabel (b), theme::busColour (b)));
        list.back()->soft = true;
        list.back()->setComponentID ("in" + juce::String (i) + ".route." + juce::String (b));
        // Hover: "Spotify -> [icon] Headphones" with the names of today (they may change meanwhile).
        list.back()->tooltipText = [this, b]
        {
            const auto& bus = ctx.engine.buses[(size_t) b];
            const bool on = strip.route[(size_t) b]->isOn();
            return icons::routeTip ({}, strip.name, bus.icon, bus.name,
                                    on ? "Routed. Click: stop sending it there." : "Not routed. Click: send it there.");
        };
        addAndMakeVisible (*list.back());
    }

    source.flat = true;
    // Ids for the guided tour (ui/Tour.cpp finds the controls it explains by them).
    setComponentID ("in" + juce::String (i));
    source.setComponentID ("in" + juce::String (i) + ".source");
    mute.setComponentID ("in" + juce::String (i) + ".mute");
    solo.setComponentID ("in" + juce::String (i) + ".solo");
    trim.setComponentID ("in" + juce::String (i) + ".gain");
    source.onClick = [this] { ctx.select (index); if (ctx.openDevicePicker) ctx.openDevicePicker (index); };
    source.setTooltip ("Select input source (hardware, Windows device, virtual input or loopback)");
    addMouseListener (this, true);

    dbPoll.v = this;
    setFaderTarget (-1);
    ParamPoller::attach (&dbPoll);
    refresh();
}

int InputStripView::minimumHeight (int numHw, int numVirt)
{
    auto group = [] (int count) { const int rows = (count + 2) / 3; return count > 0 ? rows * 20 + (rows - 1) * 3 + 6 + 6 : 0; };
    return 16 + 28 + 4 + 24 + 4 + 54 + 10 + group (numHw) + group (numVirt) + 2
         + 90 /* fader */ + 16 + 4 + 20 + 3 + 24;
}

void InputStripView::mouseDown (const juce::MouseEvent& e)
{
    ctx.select (index);
    if (e.mods.isPopupMenu() && e.eventComponent == this)
        header.showMenu();
}

int InputStripView::routeBusOf (const juce::Component* c) const
{
    for (auto* list : { &hwRoutes, &virtRoutes })
        for (auto& t : *list)
            if (t.get() == c)
                for (int b = 0; b < kMaxBuses; ++b)
                    if (strip.route[(size_t) b] == &t->param) return b;
    return -1;
}

void InputStripView::mouseEnter (const juce::MouseEvent& e)
{
    if (const int b = routeBusOf (e.eventComponent); b >= 0)
        if (auto* area = findParentComponentOfClass<StripArea>()) area->setHoverBus (b);
}

void InputStripView::mouseExit (const juce::MouseEvent& e)
{
    if (routeBusOf (e.eventComponent) >= 0)
        if (auto* area = findParentComponentOfClass<StripArea>()) area->setHoverBus (-1);
}

void InputStripView::setFaderTarget (int bus)
{
    if (bus == faderBus && fader != nullptr) return;
    faderBus = bus;
    faderParam = bus < 0 ? strip.gain : strip.send[(size_t) bus];
    fader = std::make_unique<Fader> (ctx, *faderParam, bus < 0 ? theme::accentIn : theme::busColour (bus));
    fader->horizontal = rows;
    fader->setComponentID ("in" + juce::String (index) + ".fader");
    if (bus >= 0)
        fader->setTooltip ("Level of " + strip.name + " on " + busLabel (bus) + "  (bus " + busLabel (bus) + " is selected with SEL)");
    addAndMakeVisible (*fader);
    makeEditableDbLabel (dbLabel, *faderParam);
    dbPoll.ver = 0;
    faderRouted = true;
    dbPoll.pollParams();
    resized();
    repaint();
}

void InputStripView::DbPoll::pollParams()
{
    if (const auto ver = v->faderParam->getVersion(); ver != this->ver)
    {
        this->ver = ver;
        v->dbLabel.setText (v->faderParam->toText(), juce::dontSendNotification);
    }
    // A send fader of a bus this input is not routed to is shown dimmed.
    const bool routed = v->faderBus < 0 || v->strip.route[(size_t) v->faderBus]->isOn();
    if (routed != v->faderRouted)
    {
        v->faderRouted = routed;
        v->fader->setAlpha (routed ? 1.0f : 0.4f);
        v->repaint();
    }
}

void InputStripView::paintOverChildren (juce::Graphics& g)
{
    drop.paintOver (g, getLocalBounds().toFloat());
    if (faderBus < 0 || fader == nullptr) return;
    // Tag above the fader: which bus the fader currently controls.
    const auto colour = theme::busColour (faderBus);
    auto tag = (rows ? juce::Rectangle<int> (dbLabel.getX() + 4, dbLabel.getY() - 18, dbLabel.getWidth() - 8, 15)
                     : juce::Rectangle<int> (fader->getX(), fader->getY() - 2, juce::jmin (fader->getWidth(), 44), 15)).toFloat();
    g.setColour (colour.withAlpha (faderRouted ? 0.9f : 0.35f));
    g.fillRoundedRectangle (tag, 4.0f);
    g.setColour (theme::bg);
    g.setFont (theme::font (10.5f, true));
    g.drawText ("> " + busLabel (faderBus), tag, juce::Justification::centred);
}

void InputStripView::refresh()
{
    header.refresh();
    source.text = strip.source.describe();
    source.active = strip.source.kind != IoTarget::Kind::None;
    source.colour = source.active ? theme::text : theme::textDim;
    statusTick = 15;   // check the new source's state with the next meter update
    source.repaint();
    // Buses with an icon show it instead of their code.
    for (auto* list : { &hwRoutes, &virtRoutes })
        for (auto& t : *list)
            if (const int b = routeBusOf (t.get()); b >= 0 && t->glyph != ctx.engine.buses[(size_t) b].icon)
            {
                t->glyph = ctx.engine.buses[(size_t) b].icon;
                t->repaint();
            }
    repaint();
}

void InputStripView::updateMeters (float dt)
{
    meter.update (dt);
    if ((++statusTick & 15) == 0)
        source.setStatus (statusFor (ctx.engine, strip.source, true));
}

void InputStripView::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (2.0f);
    const bool sel = ctx.selected == index;
    if (theme::deep())
    {
        paintDeepStrip (g, *this, theme::accentIn, sel, juce::Colours::transparentBlack, rows);
        if (rows) return;
        // Fine grey lines set the A and B routing buttons apart.
        auto line = [&] (int y)
        {
            g.setColour (theme::textFaint.withAlpha (0.4f));
            g.fillRect (source.getX(), y, source.getWidth(), 1);
        };
        if (! hwRoutes.empty())   line (hwRoutes.front()->getY() - 6);
        if (! virtRoutes.empty()) line (virtRoutes.front()->getY() - 6);
        if (! virtRoutes.empty())     line (virtRoutes.back()->getBottom() + 6);
        else if (! hwRoutes.empty())  line (hwRoutes.back()->getBottom() + 6);
        return;
    }
    if (rows) r = r.reduced (0.0f, 1.0f);   // a little room between the rows
    g.setColour (theme::panel);
    g.fillRoundedRectangle (r, 10.0f);
    g.setColour (sel ? theme::accentIn.withAlpha (0.7f) : theme::outline.withAlpha (0.6f));
    g.drawRoundedRectangle (r, 10.0f, sel ? 1.5f : 1.0f);

    // Routing groups: hardware outputs (A) and virtual outputs (B)
    auto group = [&g] (juce::Rectangle<float> area, juce::Colour colour)
    {
        g.setColour (colour.withAlpha (0.07f));
        g.fillRoundedRectangle (area, 6.0f);
        g.setColour (colour.withAlpha (0.28f));
        g.drawRoundedRectangle (area, 6.0f, 1.0f);
    };
    if (! hwGroup.isEmpty())   group (hwGroup, theme::accentHw);
    if (! virtGroup.isEmpty()) group (virtGroup, theme::accentVirt);
}

int InputStripView::rowControlsWidth (int numHw, int numVirt)
{
    const int n = juce::jmax (1, numHw, numVirt);
    return 10 + kRowName + 10 + 92 + 10 + (n * kRouteCell + 3) + 12 + kRowRight;
}

void InputStripView::setHorizontal (bool r)
{
    if (r == rows) return;
    rows = r;
    if (fader != nullptr) fader->horizontal = r;
    meter.horizontal = r;
    resized();
    repaint();
}

void InputStripView::resized()
{
    if (rows)
    {
        // One row: name + source | GAIN PAN | A and B routing in two lines | fader, meter, value | MONO SOLO / MUTE.
        auto r = getLocalBounds().reduced (10, 6);
        auto sw = r.removeFromRight (kRowSwitches).withSizeKeepingCentre (kRowSwitches, 46);
        layoutRow (sw.removeFromTop (20), { &mono, &solo });
        sw.removeFromTop (4);
        mute.setBounds (sw.removeFromTop (22));
        r.removeFromRight (kRowRight - kRowSwitches);
        auto col = r.removeFromLeft (kRowName);
        col = col.withSizeKeepingCentre (col.getWidth(), 48);
        header.setBounds (col.removeFromTop (24));
        col.removeFromTop (2);
        source.setBounds (col.removeFromTop (22));
        r.removeFromLeft (10);
        layoutRow (r.removeFromLeft (92), { &trim, &pan }, 4);
        r.removeFromLeft (10);
        const int n = juce::jmax (1, (int) hwRoutes.size(), (int) virtRoutes.size());
        auto routes = r.removeFromLeft (n * kRouteCell + 3);
        const bool both = ! hwRoutes.empty() && ! virtRoutes.empty();
        auto area = routes.withSizeKeepingCentre (routes.getWidth(), both ? 46 : 20);
        auto line = [] (std::vector<std::unique_ptr<Toggle>>& list, juce::Rectangle<int> row) -> juce::Rectangle<float>
        {
            if (list.empty()) return {};
            const auto group = row.toFloat().expanded (0.0f, 2.5f);
            row = row.reduced (3, 0);
            for (auto& t : list)
            {
                t->setBounds (row.removeFromLeft (kRouteCell - 3));
                row.removeFromLeft (3);
            }
            return group.withWidth ((float) list.size() * (float) kRouteCell + 3.0f);
        };
        hwGroup = line (hwRoutes, area.removeFromTop (20));
        if (both) area.removeFromTop (6);
        virtGroup = line (virtRoutes, area.removeFromTop (20));
        r.removeFromLeft (12);
        if (fader != nullptr) placeRowFader (r, *fader, meter, dbLabel);
        return;
    }
    auto r = getLocalBounds().reduced (9, 8);
    header.setBounds (r.removeFromTop (28));
    r.removeFromTop (4);
    source.setBounds (r.removeFromTop (24));
    r.removeFromTop (4);
    auto knobs = r.removeFromTop (54);
    layoutRow (knobs, { &trim, &pan }, 4);
    r.removeFromTop (10);

    // Routing: the H buses in one group, the V buses in a separate group below (three per row).
    auto routeGroup = [&r] (std::vector<std::unique_ptr<Toggle>>& list) -> juce::Rectangle<float>
    {
        if (list.empty()) return {};
        const int rows = ((int) list.size() + 2) / 3;
        auto area = r.removeFromTop (rows * 20 + (rows - 1) * 3 + 6).reduced (0, 3);
        const auto group = area.toFloat().expanded (3.0f, 3.0f).reduced (0.0f, 0.5f);
        area = area.reduced (3, 0);
        const int cell = (area.getWidth() - 6) / 3;
        for (int row = 0; row < rows; ++row)
        {
            auto line = area.removeFromTop (20);
            area.removeFromTop (3);
            for (int k = row * 3; k < juce::jmin ((int) list.size(), row * 3 + 3); ++k)
            {
                list[(size_t) k]->setBounds (line.removeFromLeft (cell));
                line.removeFromLeft (3);
            }
        }
        r.removeFromTop (6);
        return group;
    };
    hwGroup = routeGroup (hwRoutes);
    virtGroup = routeGroup (virtRoutes);
    r.removeFromTop (2);

    mute.setBounds (r.removeFromBottom (24));
    r.removeFromBottom (3);
    layoutRow (r.removeFromBottom (20), { &mono, &solo });
    r.removeFromBottom (4);
    dbLabel.setBounds (r.removeFromBottom (16));

    if (fader != nullptr) placeFaderAndMeter (r, *fader, meter);
}

//==============================================================================
BusStripView::BusStripView (UiContext& c, int i)
    : ctx (c), index (i), bus (c.engine.buses[(size_t) i]),
      header (c, busSelectIndex (i), busLabel (i), theme::busColour (i),
              [this] { return bus.name; }, [this] (juce::String n) { bus.name = n; }),
      mono (c, *bus.mono, "MONO", theme::good),
      sel (c, *bus.sel, "SEL", theme::learn),
      mute (c, *bus.mute, "MUTE", theme::mute),
      fader (c, *bus.gain, theme::busColour (i)),
      meter (bus.meter)
{
    for (auto* comp : std::initializer_list<juce::Component*> { &header, &output, &outputStatus, &mono, &sel, &mute,
                                                               &fader, &meter, &dbLabel })
        addAndMakeVisible (comp);

    header.getIcon = [this] { return bus.icon; };
    setComponentID ("bus" + juce::String (i));   // ids for the guided tour
    output.setComponentID ("bus" + juce::String (i) + ".output");
    fader.setComponentID ("bus" + juce::String (i) + ".fader");
    sel.setComponentID ("bus" + juce::String (i) + ".sel");
    mute.setComponentID ("bus" + juce::String (i) + ".mute");
    output.flat = true;
    output.onClick = [this] { ctx.select (busSelectIndex (index)); if (ctx.openDevicePicker) ctx.openDevicePicker (busSelectIndex (index)); };
    output.setTooltip ("Select output device for this bus");
    sel.setTooltip ("SEL: the input faders set the level of each input on " + busLabel (i)
                    + " (separate from the other buses). With monitoring enabled in Settings > Audio Engine, "
                      "this bus is also heard on the monitor output.");
    styleSmallLabel (outputStatus);
    outputStatus.setColour (juce::Label::textColourId, theme::warn);
    makeEditableDbLabel (dbLabel, *bus.gain);
    addMouseListener (this, true);

    dbPoll.v = this;
    ParamPoller::attach (&dbPoll);
    refresh();
}

void BusStripView::mouseDown (const juce::MouseEvent& e)
{
    ctx.select (busSelectIndex (index));
    if (e.mods.isPopupMenu() && e.eventComponent == this)
        header.showMenu();
}

void BusStripView::DbPoll::pollParams()
{
    if (const auto ver = v->bus.gain->getVersion(); ver != this->ver)
    {
        this->ver = ver;
        v->dbLabel.setText (v->bus.gain->toText(), juce::dontSendNotification);
    }
}

void BusStripView::refresh()
{
    header.refresh();
    header.repaint();
    output.text = bus.output.describe();
    output.active = bus.output.kind != IoTarget::Kind::None;
    output.colour = output.active ? theme::text : theme::textDim;
    output.repaint();
    repaint();
}

void BusStripView::updateMeters (float dt)
{
    meter.update (dt);
    const int monSrc = ctx.engine.getMonitorBus() == index ? ctx.engine.getSelectedBus() : -1;
    if ((monSrc >= 0) != monitorShown)
    {
        monitorShown = monSrc >= 0;
        outputStatus.setColour (juce::Label::textColourId, monitorShown ? theme::learn : theme::warn);
        tick = 15;
        if (rows) resized();   // the note takes room beside the name
        repaint();
    }
    if ((++tick & 15) == 0)
    {
        // Monitoring note below the output; problems are symbols in the output chip.
        outputStatus.setText (monitorShown ? "monitoring " + busLabel (monSrc) : juce::String(), juce::dontSendNotification);
        output.setStatus (statusFor (ctx.engine, bus.output, false));
    }
}

void BusStripView::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (2.0f);
    const bool sel = ctx.selected == busSelectIndex (index);
    if (theme::deep())
    {
        const bool lit = bus.sel->isOn() || monitorShown;
        paintDeepStrip (g, *this, theme::busColour (index), sel,
                        hoverLit ? theme::busColour (index).withAlpha (0.12f)
                                 : lit ? theme::learn.withAlpha (monitorShown ? 0.08f : 0.05f) : juce::Colours::transparentBlack, rows);
        return;
    }
    if (rows) r = r.reduced (0.0f, 1.0f);
    g.setColour (theme::panel);
    g.fillRoundedRectangle (r, 10.0f);
    if (hoverLit)
    {
        g.setColour (theme::busColour (index).withAlpha (0.12f));
        g.fillRoundedRectangle (r, 10.0f);
    }
    if (bus.sel->isOn() || monitorShown)
    {
        g.setColour (theme::learn.withAlpha (monitorShown ? 0.08f : 0.05f));
        g.fillRoundedRectangle (r, 10.0f);
    }
    g.setColour (sel ? theme::busColour (index).withAlpha (0.7f) : theme::outline.withAlpha (0.6f));
    g.drawRoundedRectangle (r, 10.0f, sel ? 1.5f : 1.0f);
}

int BusStripView::rowControlsWidth() { return 10 + kRowName + 12 + kRowRight; }

void BusStripView::setHoverLit (bool lit)
{
    if (lit == hoverLit) return;
    hoverLit = lit;
    repaint();
}

void BusStripView::setHorizontal (bool r)
{
    if (r == rows) return;
    rows = r;
    fader.horizontal = r;
    meter.horizontal = r;
    resized();
    repaint();
}

void BusStripView::resized()
{
    if (rows)
    {
        // One row: name + output (monitoring note beside the name) | fader, meter, value | SEL / MONO MUTE.
        auto r = getLocalBounds().reduced (10, 6);
        auto sw = r.removeFromRight (kRowSwitches).withSizeKeepingCentre (kRowSwitches, 46);
        sel.setBounds (sw.removeFromTop (20));
        sw.removeFromTop (4);
        layoutRow (sw.removeFromTop (22), { &mono, &mute });
        r.removeFromRight (kRowRight - kRowSwitches);
        auto col = r.removeFromLeft (kRowName).withSizeKeepingCentre (kRowName, 48);
        auto nameRow = col.removeFromTop (24);
        if (monitorShown) outputStatus.setBounds (nameRow.removeFromRight (86));
        else              outputStatus.setBounds ({});
        header.setBounds (nameRow);
        col.removeFromTop (2);
        output.setBounds (col.removeFromTop (22));
        r.removeFromLeft (12);
        placeRowFader (r, fader, meter, dbLabel);
        return;
    }
    auto r = getLocalBounds().reduced (9, 8);
    header.setBounds (r.removeFromTop (28));
    r.removeFromTop (4);
    output.setBounds (r.removeFromTop (24));
    outputStatus.setBounds (r.removeFromTop (15));
    r.removeFromTop (10);
    sel.setBounds (r.removeFromTop (20));
    r.removeFromTop (8);

    mute.setBounds (r.removeFromBottom (24));
    r.removeFromBottom (3);
    mono.setBounds (r.removeFromBottom (20));
    r.removeFromBottom (4);
    dbLabel.setBounds (r.removeFromBottom (16));

    placeFaderAndMeter (r, fader, meter);
}
//==============================================================================
StripPane::StripPane()
{
    view.setViewedComponent (&holder, false);
    view.setScrollBarsShown (false, true);   // sideways only, and only while needed
    view.setScrollBarThickness (kScrollBar);
    view.setSingleStepSizes (24, 24);
    view.moved = [this] { if (onScroll) onScroll(); };
    holder.addAndMakeVisible (content);
    content.onMenu = [this] (juce::Point<int> p) { if (onBackgroundMenu) onBackgroundMenu (p); };
    content.onClick = [this] (juce::Point<int> p) { if (onBackgroundClick) onBackgroundClick (p); };
    content.onPaint = [this] (juce::Graphics& g) { if (paintContent) paintContent (g); };
    addAndMakeVisible (view);
}

void StripPane::resized() { view.setBounds (getLocalBounds()); }

void StripPane::View::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    ParamControl::notePaneScrolled();   // faders / knobs passing under the mouse stay put meanwhile
    const bool canH = getHorizontalScrollBar().isVisible(), canV = getVerticalScrollBar().isVisible();
    if (w.isSmooth || w.isInertial || e.mods.isAltDown() || e.mods.isCtrlDown() || e.mods.isCommandDown() || (! canH && ! canV))
    {
        stopGlide();
        juce::Viewport::mouseWheelMove (e, w);   // touchpads are smooth already; otherwise up to the parent
        return;
    }
    // The same distances as juce::Viewport (single step 24 px), but reached in a glide.
    auto scaled = [] (float d) { return d == 0.0f ? 0.0f : (d < 0.0f ? juce::jmin (d * 14.0f * 24.0f, -1.0f) : juce::jmax (d * 14.0f * 24.0f, 1.0f)); };
    const float dx = scaled (w.deltaX), dy = scaled (w.deltaY);
    if (! gliding)
    {
        target = shown = getViewPosition().toFloat();
        lastSet = getViewPosition();
    }
    if (canH && (dx != 0.0f || e.mods.isShiftDown() || ! canV)) target.x -= dx != 0.0f ? dx : dy;
    else if (canV && dy != 0.0f)                                 target.y -= dy;
    if (auto* c = getViewedComponent())
    {
        target.x = juce::jlimit (0.0f, (float) juce::jmax (0, c->getWidth() - getViewWidth()), target.x);
        target.y = juce::jlimit (0.0f, (float) juce::jmax (0, c->getHeight() - getViewHeight()), target.y);
    }
    if (! gliding && target.getDistanceFrom (shown) >= 0.5f)
    {
        gliding = true;
        lastFrame = 0.0;
        if (vblank == nullptr) vblank = std::make_unique<juce::VBlankAttachment> (this, [this] (double t) { glideFrame (t); });
    }
}

void StripPane::View::glideFrame (double t)
{
    // Moved by someone else meanwhile (scroll bar, scrollToShow): the glide ends there.
    // Ending the glide removes the attachment after this callback (never from inside it).
    auto finish = [this]
    {
        gliding = false;
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<View> (this)] { if (safe != nullptr && ! safe->gliding) safe->vblank.reset(); });
    };
    if (! gliding) return;
    if (getViewPosition() != lastSet) { finish(); return; }
    const double dt = lastFrame > 0.0 ? juce::jlimit (0.0, 0.05, t - lastFrame) : 1.0 / 60.0;
    lastFrame = t;
    const float k = 1.0f - (float) std::exp (-dt / 0.055);   // ~0.15 s to arrive
    shown += (target - shown) * k;
    if (target.getDistanceFrom (shown) < 0.5f) shown = target;
    lastSet = shown.roundToInt();
    setViewPosition (lastSet);
    if (shown == target) finish();
}

void StripPane::setContentSize (int w, int h, float s)
{
    scale = s;
    content.setTransform (juce::AffineTransform::scale (s));
    content.setBounds (0, 0, w, h);
    holder.setSize (juce::roundToInt ((float) w * s), juce::roundToInt ((float) h * s));
    if (onScroll) onScroll();
}

float StripPane::getScrollX() const { return (float) view.getViewPositionX() / scale; }

void StripPane::setVertical (bool v)
{
    if (v == vertical) return;
    vertical = v;
    view.setScrollBarsShown (v, true);
    view.setViewPosition (0, 0);
}

void StripPane::scrollToShow (juce::Rectangle<int> area)
{
    const auto a = area.toFloat().transformedBy (content.getTransform()).toNearestInt();
    const auto v = view.getViewArea();
    if (vertical)
    {
        if (a.getY() < v.getY())              view.setViewPosition (v.getX(), a.getY() - 8);
        else if (a.getBottom() > v.getBottom()) view.setViewPosition (v.getX(), a.getBottom() + 8 - v.getHeight());
        return;
    }
    if (a.getX() < v.getX())              view.setViewPosition (a.getX() - 8, 0);
    else if (a.getRight() > v.getRight()) view.setViewPosition (a.getRight() + 8 - v.getWidth(), 0);
}

//==============================================================================
PaneDivider::PaneDivider()
{
    setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
    setTooltip ("Drag to give one side more room.  Double-click: default position.");
}

void PaneDivider::paint (juce::Graphics& g)
{
    const float x = (float) getWidth() * 0.5f;
    const bool active = hover || dragging;
    g.setColour (active ? theme::accentIn.withAlpha (0.8f) : theme::textFaint.withAlpha (0.55f));
    g.fillRect (juce::Rectangle<float> (x - (active ? 1.0f : 0.5f), 8.0f, active ? 2.0f : 1.0f, (float) getHeight() - 16.0f));
    // Grip in the middle
    auto grip = juce::Rectangle<float> (0.0f, 0.0f, 10.0f, 38.0f).withCentre ({ x, (float) getHeight() * 0.5f });
    g.setColour (theme::raised.brighter (active ? 0.5f : 0.3f));
    g.fillRoundedRectangle (grip, 5.0f);
    g.setColour (active ? theme::text : theme::textDim.brighter (0.2f));
    for (int k = 0; k < 3; ++k)
        g.fillEllipse (x - 1.3f, grip.getCentreY() - 1.3f + (float) (k - 1) * 8.0f, 2.6f, 2.6f);
}

void PaneDivider::mouseDown (const juce::MouseEvent&)
{
    startCentre = getBounds().getCentreX();
    dragging = true;
    repaint();
}

void PaneDivider::mouseDrag (const juce::MouseEvent& e)
{
    if (onDrag) onDrag (startCentre + e.getDistanceFromDragStartX());
}

//==============================================================================
namespace
{
constexpr int kPad = 8, kPlusSize = 26, kMinPaneWidth = 120;
juce::Font captionFont() { return theme::font (17.0f, true); }
} // namespace

//==============================================================================
/** Covers a pane's strips in Change Order mode: drag a strip sideways (a row up or down) to move it within its group.
    The other strips make room as soon as the dragged one passes half of a neighbour. Each strip shows a trash can:
    a click hides the channel, it is deleted when the new order is applied (Discard brings it back). */
class StripArea::DragLayer final : public juce::Component, public juce::TooltipClient
{
public:
    DragLayer (StripArea& o, int g) : owner (o), group (g)
    {
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    }

    juce::String getTooltip() override
    {
        if (hoverTrash < 0) return {};
        if (! owner.canRemoveInReorder (group)) return group == 0 ? "The last input stays" : "The last hardware output stays";
        return "Delete this channel\nIt is removed when you click Done; Discard brings it back.";
    }

    void paint (juce::Graphics& g) override
    {
        const bool allowed = owner.canRemoveInReorder (group);
        for (int k = 0; k < owner.stripCount (group); ++k)
        {
            const auto r = (trashArea (*owner.stripAt (group, k)) - getPosition()).toFloat();
            const bool hot = allowed && k == hoverTrash;
            g.setColour (hot ? theme::mute.withAlpha (0.25f) : theme::panel2.withAlpha (0.95f));
            g.fillRoundedRectangle (r, 6.0f);
            g.setColour (hot ? theme::mute.withAlpha (0.85f) : theme::outline);
            g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
            TrashButton::drawIcon (g, r.withSizeKeepingCentre (14.0f, 14.0f),
                                   hot ? theme::mute : allowed ? theme::textDim : theme::textFaint.withAlpha (0.4f));
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int k = trashAt (e.getPosition() + getPosition());
        if (k == hoverTrash) return;
        hoverTrash = k;
        setMouseCursor (k >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::DraggingHandCursor);
        repaint();
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (hoverTrash < 0) return;
        hoverTrash = -1;
        repaint();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragIndex = pressedTrash = -1;
        const auto p = e.getPosition() + getPosition();   // in the coordinates of the strips (the layer may cover only its group)
        if (const int k = trashAt (p); k >= 0) { pressedTrash = k; return; }
        const bool rows = owner.shownRows;
        for (int k = 0; k < owner.stripCount (group); ++k)
            if (auto* c = owner.stripAt (group, k); c->getBounds().contains (p))
            {
                dragIndex = k;
                grabOffset = rows ? p.y - c->getY() : p.x - c->getX();
                c->toFront (false);
                toFront (false);   // stays on top for the next click
                return;
            }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragIndex < 0) return;
        const bool rows = owner.shownRows;
        auto* c = owner.stripAt (group, dragIndex);
        const auto& sec = owner.sections[group];
        const int count = owner.stripCount (group);
        const int step = rows ? owner.stripHeight[group] : owner.stripWidth[group];
        const int start = rows ? sec.y : sec.x;
        const int pos = juce::jlimit (start, start + (count - 1) * step, (rows ? e.y + getY() : e.x + getX()) - grabOffset);
        owner.animator.forget (*c);
        c->setTopLeftPosition (rows ? sec.x : pos, rows ? pos : sec.y);
        const int target = juce::jlimit (0, count - 1, juce::roundToInt ((float) (pos - start) / (float) step));
        if (target != dragIndex)
        {
            move (dragIndex, target);
            dragIndex = target;
            owner.placeStrips (group, c, true);   // the neighbour glides out of the way
        }
        repaint();
        auto& pane = owner.paneOf (group);
        pane.autoScroll (e.getEventRelativeTo (&pane).getPosition());
        beginDragAutoRepeat (40);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (pressedTrash >= 0)
        {
            const int k = pressedTrash;
            pressedTrash = -1;
            if (trashAt (e.getPosition() + getPosition()) == k)
            {
                hoverTrash = -1;
                owner.removeInReorder (group, k);   // may relayout everything (this layer stays)
            }
            return;
        }
        dragIndex = -1;
        owner.placeStrips (group, nullptr, true);
    }

private:
    /** Trash can of a strip (in the coordinates of the strips): at the end of its name line, at the right end of a row. */
    juce::Rectangle<int> trashArea (const juce::Component& c) const
    {
        if (owner.shownRows) return { c.getRight() - 12 - 26, c.getBounds().getCentreY() - 13, 26, 26 };
        return { c.getRight() - 9 - 24, c.getY() + 10, 24, 24 };
    }

    int trashAt (juce::Point<int> p) const
    {
        for (int k = 0; k < owner.stripCount (group); ++k)
            if (trashArea (*owner.stripAt (group, k)).contains (p)) return k;
        return -1;
    }

    void move (int from, int to)
    {
        auto rotate = [] (auto& v, int a, int b)
        {
            if (a < b) std::rotate (v.begin() + a, v.begin() + a + 1, v.begin() + b + 1);
            else       std::rotate (v.begin() + b, v.begin() + a, v.begin() + a + 1);
        };
        if (group == 0) rotate (owner.inStrips, from, to);
        else
        {
            const int offset = group == 1 ? 0 : owner.numHw;   // busStrips: H buses, then V buses
            rotate (owner.busStrips, offset + from, offset + to);
        }
    }

    StripArea& owner;
    const int group;
    int dragIndex = -1, grabOffset = 0, hoverTrash = -1, pressedTrash = -1;
};

/** Bar above the strips in Change Order mode: what to do, Discard and Done. */
class StripArea::ReorderBar final : public juce::Component
{
public:
    explicit ReorderBar (StripArea& o) : owner (o)
    {
        for (auto* b : { &discard, &done })
        {
            addAndMakeVisible (b);
            b->showArrow = false;
            b->centred = true;
        }
        done.active = true;
        discard.setTooltip ("Leave without changing the order (Esc)");
        done.setTooltip ("Apply the new order");
        discard.onClick = [this] { if (owner.onReorderDiscard) owner.onReorderDiscard(); };
        done.onClick = [this] { if (owner.onReorderDone) owner.onReorderDone(); };
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (theme::learn.withAlpha (0.10f));
        g.fillRoundedRectangle (r, 8.0f);
        g.setColour (theme::learn.withAlpha (0.6f));
        g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, 1.0f);
        auto text = getLocalBounds().reduced (14, 0).withRight (discard.getX() - 12);
        g.setColour (theme::learn);
        g.setFont (theme::font (13.5f, true));
        const juce::String title ("Change Order");
        const int tw = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), title);
        g.drawText (title, text.removeFromLeft (tw + 14), juce::Justification::centredLeft);
        g.setColour (theme::text);
        g.setFont (theme::font (12.5f));
        g.drawFittedText (juce::String (owner.shownRows ? "Drag the channels up or down" : "Drag the channels sideways")
                              + " into the new order (inputs, hardware and virtual outputs each keep their group), "
                                "or delete them with the trash can. Nothing changes before Done.",
                          text, juce::Justification::centredLeft, 2, 0.85f);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8, 7);
        done.setBounds (r.removeFromRight (96));
        r.removeFromRight (8);
        discard.setBounds (r.removeFromRight (96));
    }

private:
    StripArea& owner;
    Chip discard { "Discard", theme::text }, done { "Done", theme::good };
};

//==============================================================================
/** A collapsed group: one narrow column per channel (one cell per channel in horizontal rows) with its number,
    level, MUTE and (inputs) SOLO. Click = select the channel, double-click = show the strips again. */
class StripArea::Summary final : public juce::Component, public juce::TooltipClient
{
public:
    Summary (StripArea& o, int g) : owner (o), group (g)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void setChannels (const std::vector<int>& sels)
    {
        cells.clear();
        for (int sel : sels)
        {
            auto& c = cells.emplace_back();
            c.sel = sel;
            c.meter = std::make_unique<MeterView> (sel < kMaxInputs ? owner.ctx.engine.inputs[(size_t) sel].meter
                                                                    : owner.ctx.engine.buses[(size_t) (sel - kMaxInputs)].meter, true);
            c.meter->horizontal = rows;
            addAndMakeVisible (*c.meter);
        }
        resized();
    }

    void setRows (bool r)
    {
        if (r == rows) return;
        rows = r;
        for (auto& c : cells) c.meter->horizontal = r;
        resized();
    }

    void update (float dt)
    {
        bool changed = false;
        for (auto& c : cells)
        {
            c.meter->update (dt);
            const bool m = muteOf (c.sel), s = soloOf (c.sel);
            if (m != c.mute || s != c.solo) { c.mute = m; c.solo = s; changed = true; }
        }
        if (owner.ctx.selected != shownSel) { shownSel = owner.ctx.selected; changed = true; }
        if (changed) repaint();
    }

    void resized() override
    {
        for (int k = 0; k < (int) cells.size(); ++k)
        {
            const auto r = cellArea (k);
            if (rows) cells[(size_t) k].meter->setBounds (r.getX() + 32, r.getCentreY() - 5, juce::jmax (8, r.getWidth() - 32 - letters() - 8), 10);
            else      cells[(size_t) k].meter->setBounds (r.getCentreX() - 5, r.getY() + 24, 10, juce::jmax (8, r.getHeight() - 24 - letters() - 6));
        }
    }

    void paint (juce::Graphics& g) override
    {
        for (int k = 0; k < (int) cells.size(); ++k)
        {
            const auto& c = cells[(size_t) k];
            const auto r = cellArea (k).toFloat();
            const auto colour = colourOf (c.sel);
            const bool sel = owner.ctx.selected == c.sel;
            g.setColour (theme::panel2);
            g.fillRoundedRectangle (r, 5.0f);
            g.setColour (sel ? colour.withAlpha (0.9f) : theme::outline);
            g.drawRoundedRectangle (r.reduced (0.5f), 5.0f, sel ? 1.5f : 1.0f);
            g.setColour (colour.withAlpha (sel ? 1.0f : 0.75f));
            if (rows) g.fillRoundedRectangle (r.withWidth (3.0f).reduced (0.0f, 5.0f), 1.5f);
            else      g.fillRoundedRectangle (r.withHeight (3.0f).reduced (5.0f, 0.0f), 1.5f);

            g.setFont (theme::font (rows ? 12.0f : 10.5f, true));
            g.setColour (sel ? theme::text : theme::textDim);
            const auto label = rows ? r.withWidth (30.0f).withTrimmedLeft (4.0f) : r.withHeight (22.0f).withTrimmedTop (4.0f);
            g.drawFittedText (labelOf (c.sel), label.toNearestInt(), juce::Justification::centred, 1, 0.7f);

            // MUTE (and SOLO): lit while on
            auto letter = [&] (juce::Rectangle<float> b, const char* t, bool on, juce::Colour col)
            {
                g.setColour (on ? col : theme::bg.withAlpha (0.6f));
                g.fillRoundedRectangle (b, 3.0f);
                g.setColour (on ? theme::bg : theme::textFaint);
                g.setFont (theme::font (9.5f, true));
                g.drawText (t, b, juce::Justification::centred, false);
            };
            const bool input = c.sel < kMaxInputs;
            if (rows)
            {
                const auto b = juce::Rectangle<float> (r.getRight() - 4.0f - 14.0f, r.getCentreY() - 7.0f, 14.0f, 14.0f);
                letter (b, "M", c.mute, theme::mute);
                if (input) letter (b.translated (-16.0f, 0.0f), "S", c.solo, theme::solo);
            }
            else
            {
                const auto b = juce::Rectangle<float> (r.getCentreX() - 8.0f, r.getBottom() - 4.0f - 14.0f, 16.0f, 14.0f);
                letter (b, "M", c.mute, theme::mute);
                if (input) letter (b.translated (0.0f, -16.0f), "S", c.solo, theme::solo);
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (const int k = cellAt (e.getPosition()); k >= 0)
            owner.ctx.select (cells[(size_t) k].sel);
        if (e.mods.isPopupMenu()) showGroupMenu (owner.ctx, group);
    }

    void mouseDoubleClick (const juce::MouseEvent&) override { owner.toggleCollapsed (group); }

    juce::String getTooltip() override
    {
        const int k = cellAt (getMouseXYRelative());
        if (k < 0) return {};
        const int sel = cells[(size_t) k].sel;
        auto& e = owner.ctx.engine;
        juce::String t;
        if (sel < kMaxInputs)
        {
            auto& in = e.inputs[(size_t) sel];
            t << juce::String (sel + 1) << "  " << in.name;
            juce::StringArray to;
            for (int b = 0; b < kMaxBuses; ++b)
                if (e.isBusActive (b) && in.route[(size_t) b] != nullptr && in.route[(size_t) b]->get() > 0.5f)
                    to.add (e.buses[(size_t) b].name.isNotEmpty() ? e.buses[(size_t) b].name : busLabel (b));
            t << "\n" << (to.isEmpty() ? juce::String ("not routed") : "-> " + to.joinIntoString (", "));
        }
        else
            t << busLabel (sel - kMaxInputs) << "  " << e.buses[(size_t) (sel - kMaxInputs)].name;
        if (cells[(size_t) k].mute) t << "\nmuted";
        if (cells[(size_t) k].solo) t << "\nsolo";
        return t + "\nClick: select.  Double-click: show the strips again.";
    }

    /** Lines of cells the horizontal rows need at this width. */
    static int linesFor (int count, int width) noexcept
    {
        const int perLine = juce::jmax (1, width / kMiniCell);
        return juce::jmax (1, (count + perLine - 1) / perLine);
    }

private:
    struct Cell { int sel = 0; std::unique_ptr<MeterView> meter; bool mute = false, solo = false; };

    int letters() const noexcept { return group == 0 ? 30 : 14; }   // room for S / M
    juce::Rectangle<int> cellArea (int k) const
    {
        if (rows)
        {
            const int perLine = juce::jmax (1, getWidth() / kMiniCell);
            return juce::Rectangle<int> ((k % perLine) * kMiniCell, (k / perLine) * kMiniLine, kMiniCell, kMiniLine).reduced (2);
        }
        return juce::Rectangle<int> (k * kMiniWidth, 0, kMiniWidth, getHeight()).reduced (2, 2);
    }
    int cellAt (juce::Point<int> p) const
    {
        for (int k = 0; k < (int) cells.size(); ++k)
            if (cellArea (k).expanded (2).contains (p)) return k;
        return -1;
    }
    bool muteOf (int sel) const
    {
        auto& e = owner.ctx.engine;
        auto* m = sel < kMaxInputs ? e.inputs[(size_t) sel].mute : e.buses[(size_t) (sel - kMaxInputs)].mute;
        return m != nullptr && m->get() > 0.5f;
    }
    bool soloOf (int sel) const
    {
        if (sel >= kMaxInputs) return false;
        auto* s = owner.ctx.engine.inputs[(size_t) sel].solo;
        return s != nullptr && s->get() > 0.5f;
    }
    static juce::Colour colourOf (int sel) { return sel < kMaxInputs ? theme::accentIn : theme::busColour (sel - kMaxInputs); }
    static juce::String labelOf (int sel) { return sel < kMaxInputs ? juce::String (sel + 1) : busLabel (sel - kMaxInputs); }

    StripArea& owner;
    const int group;
    std::vector<Cell> cells;
    bool rows = false;
    int shownSel = -1;
};

/** Symbol beside a selected group caption: collapse the group or pin it. */
class StripArea::CaptionTool final : public juce::Component, public juce::SettableTooltipClient
{
public:
    CaptionTool (StripArea& o, int g, bool pinKind) : owner (o), group (g), pin (pinKind)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setWantsKeyboardFocus (false);
        setMouseClickGrabsKeyboardFocus (false);
        updateTooltip();
    }

    bool isOn() const
    {
        if (owner.ctx.session == nullptr) return false;
        const int mask = pin ? owner.ctx.session->prefs.stripPinned : owner.ctx.session->prefs.stripCollapsed;
        return ((mask >> group) & 1) != 0;
    }

    void updateTooltip()
    {
        if (pin) setTooltip (isOn() ? "Pinned: always shown in full, the other groups scroll. Click to unpin."
                                    : "Pin: always show this group in full, the other groups scroll.");
        else     setTooltip (isOn() ? "Show the strips again" : "Collapse to a narrow summary (level, mute, solo of each channel)");
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto colour = owner.sections[group].colour;
        const bool on = isOn();
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        if (hover || on)
        {
            g.setColour (colour.withAlpha (on ? 0.22f : 0.12f));
            g.fillRoundedRectangle (r, 5.0f);
        }
        const auto ink = on ? colour : hover ? theme::text : theme::textDim;
        const auto c = r.getCentre();
        const float u = r.getWidth() * 0.5f;
        juce::Path p;
        if (pin)
        {
            // Push pin: upright while pinned, tilted otherwise.
            p.addRoundedRectangle (c.x - 0.34f * u, c.y - 0.72f * u, 0.68f * u, 0.22f * u, 0.08f * u);
            p.addRectangle (c.x - 0.18f * u, c.y - 0.50f * u, 0.36f * u, 0.42f * u);
            p.addRoundedRectangle (c.x - 0.46f * u, c.y - 0.10f * u, 0.92f * u, 0.18f * u, 0.08f * u);
            p.addRectangle (c.x - 0.05f * u, c.y + 0.08f * u, 0.10f * u, 0.62f * u);
            if (! on) p.applyTransform (juce::AffineTransform::rotation (0.75f, c.x, c.y));
            g.setColour (ink);
            g.fillPath (p);
            return;
        }
        // Two chevrons at a bar: pointing at it = collapse, away from it = expand. Rows: one above the other.
        const float a = 0.28f * u, depth = 0.26f * u;
        p.startNewSubPath (c.x, c.y - 0.42f * u);
        p.lineTo (c.x, c.y + 0.42f * u);
        for (float side : { -1.0f, 1.0f })
        {
            const float tip = c.x + side * (on ? 0.70f * u : 0.20f * u);
            const float back = on ? tip - side * depth : tip + side * depth;
            p.startNewSubPath (back, c.y - a);
            p.lineTo (tip, c.y);
            p.lineTo (back, c.y + a);
        }
        if (owner.shownRows) p.applyTransform (juce::AffineTransform::rotation (juce::MathConstants<float>::halfPi, c.x, c.y));
        g.setColour (ink);
        g.strokePath (p, juce::PathStrokeType (1.7f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { hover = false; repaint(); }
    void mouseDown (const juce::MouseEvent&) override  { owner.captionClick = true; }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! getLocalBounds().contains (e.getPosition())) return;
        if (pin) owner.togglePinned (group);
        else     owner.toggleCollapsed (group);
    }

private:
    StripArea& owner;
    const int group;
    const bool pin;
    bool hover = false;
};

void StripArea::ClickElsewhere::mouseDown (const juce::MouseEvent&)
{
    if (owner.captionClick) { owner.captionClick = false; return; }
    owner.clearCaptionSelection();
}

StripArea::~StripArea()
{
    removeMouseListener (&clickElsewhere);
}

int StripArea::pinMask() const noexcept
{
    return ctx.session != nullptr ? ctx.session->prefs.stripPinned & 7 : 0;
}

bool StripArea::collapsed (int g) const noexcept
{
    return ! reordering && ctx.session != nullptr && ((ctx.session->prefs.stripCollapsed >> g) & 1) != 0;
}

int StripArea::paneIndex (int g) const noexcept
{
    if (shownRows)
    {
        // One pane for the pinned groups, one for the others.
        for (int k = 0; k < kGroups; ++k)
            if (pinned (k) == pinned (g)) return k;
        return 0;
    }
    if (! shownJoined || pinned (g)) return g;
    int k = g;   // joined: neighbours that are not pinned share one pane (with one scroll bar)
    while (k > 0 && ! pinned (k - 1)) --k;
    return k;
}

int StripArea::captionTitleWidth (int g) const
{
    return collapsed (g) && ! shownRows ? sections[g].shortWidth : sections[g].captionWidth;
}

int StripArea::toolsWidth (int g) const
{
    // Grows with the selection (selAnim): the caption box, the symbols and the "+" glide open together.
    const float base = pinned (g) ? (float) (kToolSize + 4) : 0.0f, full = (float) (2 * kToolSize + 6);
    return juce::roundToInt (base + (full - base) * selAnim[g]);
}

void StripArea::placeTools (int g, juce::Rectangle<int> caption, int titleWidth, bool animated)
{
    const float a = reordering ? 0.0f : selAnim[g];
    const int y = caption.getY() + (caption.getHeight() - kToolSize) / 2;
    const int x0 = caption.getX() + titleWidth + 8;
    auto put = [&] (CaptionTool& t, int x, float alpha)
    {
        if (alpha <= 0.01f) { t.setVisible (false); return; }
        const juce::Rectangle<int> target (x, y, kToolSize, kToolSize);
        if (animated && t.isVisible()) animator.moveTo (t, target, true);
        else                           animator.place (t, target);
        t.setAlpha (alpha);
        // Vertical strips: the tools are children of this component and stay inside their pane like the caption.
        const auto& pane = paneOf (g);
        t.setVisible (shownRows || (target.getX() >= pane.getX() && target.getRight() <= pane.getRight()));
    };
    // Collapse fades in at the first place; a pin that is shown anyway slides over to make room for it.
    put (*collapseTools[g], x0, a);
    put (*pinTools[g], pinned (g) ? x0 + juce::roundToInt ((float) (kToolSize + 2) * a) : x0 + kToolSize + 2, pinned (g) ? 1.0f : a);
}

void StripArea::selectCaption (int g)
{
    if (reordering) g = -1;
    if (g == captionSel) return;
    captionSel = g;
    if (g >= 0 && isShowing()) grabKeyboardFocus();   // Esc ends it
    startCaptionGlide();
}

void StripArea::clearCaptionSelection()
{
    if (captionSel < 0) return;
    captionSel = -1;
    startCaptionGlide();
}

void StripArea::startCaptionGlide()
{
    if (! isShowing())
    {
        // Not on screen (no display frames): straight to the end.
        for (int g = 0; g < kGroups; ++g) selAnim[g] = captionSel == g && ! reordering ? 1.0f : 0.0f;
        placeCaptions();
        repaint();
        return;
    }
    captionGliding = true;
    captionLastFrame = 0.0;
    if (captionVBlank == nullptr)
        captionVBlank = std::make_unique<juce::VBlankAttachment> (this, [this] (double t) { captionFrame (t); });
}

void StripArea::captionFrame (double t)
{
    if (! captionGliding) return;
    const double dt = captionLastFrame > 0.0 ? juce::jlimit (0.0, 0.05, t - captionLastFrame) : 1.0 / 60.0;
    captionLastFrame = t;
    bool moving = false;
    for (int g = 0; g < kGroups; ++g)
    {
        const float want = captionSel == g && ! reordering ? 1.0f : 0.0f;
        const float step = (float) (dt / 0.16);   // 0.16 s open or closed
        selAnim[g] = want > selAnim[g] ? juce::jmin (want, selAnim[g] + step) : juce::jmax (want, selAnim[g] - step);
        moving = moving || selAnim[g] != want;
    }
    placeCaptions();
    if (! moving)
    {
        // The attachment goes after this callback (never from inside it).
        captionGliding = false;
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<StripArea> (this)]
                                         { if (safe != nullptr && ! safe->captionGliding) safe->captionVBlank.reset(); });
    }
}

void StripArea::placeCaptions()
{
    // Only the captions move: "+", symbols and box (the strips stay where they are).
    if (shownRows)
    {
        for (int g = 0; g < kGroups; ++g) placeRowCaption (g, false);
        for (int g = 0; g < kGroups; ++g) paneOf (g).getContent().repaint (sections[g].caption.withWidth (sections[g].caption.getWidth() + 80).expanded (8, 0));
    }
    else
        layoutCaptions (false);
}

void StripArea::placeRowCaption (int g, bool animated)
{
    // Horizontal rows: caption (title, symbols, "+") of a group in its pane's content, at sec.caption's height.
    static constexpr int maxStrips[kGroups] { kMaxInputs, kMaxHwBuses, kMaxVirtBuses };
    auto& sec = sections[g];
    const int tools = toolsWidth (g), y = sec.caption.getY();
    sec.caption = { kPad + 6, y, sec.captionWidth + 10 + tools + kPlusSize + 6, kRowCaption };
    auto& plus = *addButtons[g];
    const juce::Rectangle<int> target (sec.caption.getX() + sec.captionWidth + 10 + tools, y + (kRowCaption - kPlusSize) / 2 + 1, kPlusSize, kPlusSize - 2);
    if (animated && plus.isVisible()) animator.moveTo (plus, target, true);
    else                              animator.place (plus, target);
    plus.setVisible (! reordering && ! collapsed (g) && stripCount (g) < maxStrips[g]);
    placeTools (g, sec.caption, sec.captionWidth, animated);
}

bool StripArea::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey && captionSel >= 0)
    {
        clearCaptionSelection();
        return true;
    }
    return false;
}

void StripArea::toggleCollapsed (int g)
{
    if (ctx.session == nullptr) return;
    ctx.session->prefs.stripCollapsed ^= 1 << g;
    ctx.session->prefsVersion.fetch_add (1);   // stored with the session
    collapseTools[g]->updateTooltip();
    layout (true);
    repaint();
}

void StripArea::togglePinned (int g)
{
    if (ctx.session == nullptr) return;
    ctx.session->prefs.stripPinned ^= 1 << g;
    ctx.session->prefsVersion.fetch_add (1);
    pinTools[g]->updateTooltip();
    layout (true);   // the panes change: placed directly
    repaint();
}

void StripArea::applyCollapsed()
{
    for (int g = 0; g < kGroups; ++g)
    {
        const bool c = collapsed (g);
        for (int k = 0; k < stripCount (g); ++k) stripAt (g, k)->setVisible (! c);
        if (summaries[g] != nullptr)
        {
            summaries[g]->setRows (shownRows);
            summaries[g]->setVisible (c);
        }
    }
}

void StripArea::mouseDown (const juce::MouseEvent& e)
{
    // Vertical strips: the captions are drawn here, above the panes.
    if (shownRows || e.y < captionTop() || e.y >= captionTop() + kCaptionHeight) return;
    for (int g = 0; g < kGroups; ++g)
    {
        const auto& pane = paneOf (g);
        const auto& cap = sections[g].caption;
        const juce::Rectangle<int> title (cap.getX() - 6, cap.getY(), captionTitleWidth (g) + 14, cap.getHeight());
        if (! pane.isVisible() || ! title.contains (e.getPosition()) || e.x < pane.getX() || e.x >= pane.getRight()) continue;
        captionClick = true;
        if (e.mods.isPopupMenu()) showGroupMenu (ctx, g);
        else                      selectCaption (captionSel == g ? -1 : g);
        return;
    }
}

int StripArea::groupAt (int pane, juce::Point<int> p) const
{
    int best = -1;
    for (int k = 0; k < kGroups; ++k)
    {
        if (paneIndex (k) != pane) continue;
        if (best < 0 || (shownRows ? p.y >= sections[k].caption.getY() : p.x >= sections[k].x - kPad)) best = k;
    }
    return juce::jmax (0, best);
}

void StripArea::paintOverChildren (juce::Graphics& g)
{
    // Horizontal rows with pinned groups: a line under their pane, the rest scrolls below it.
    if (! shownRows || shownPinned == 0 || shownPinned == 7) return;
    for (int k = 0; k < kGroups; ++k)
        if (pinned (k))
        {
            const int y = paneOf (k).getBottom();
            g.setColour (theme::outline);
            g.fillRect (0, y, getWidth(), 1);
            g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.22f), 0.0f, (float) y + 1.0f,
                                                     juce::Colours::transparentBlack, 0.0f, (float) y + 7.0f, false));
            g.fillRect (0, y + 1, getWidth(), 6);
            return;
        }
}

void StripArea::beginReorder()
{
    if (reordering) return;
    reordering = true;
    for (int g = 0; g < kGroups; ++g)
    {
        layers[g] = std::make_unique<DragLayer> (*this, g);
        paneOf (g).getContent().addAndMakeVisible (*layers[g]);
    }
    reorderBar = std::make_unique<ReorderBar> (*this);
    addAndMakeVisible (*reorderBar);
    resized();
    repaint();
}

void StripArea::endReorder()
{
    if (! reordering) return;
    reordering = false;
    for (auto& layer : layers) layer.reset();
    reorderBar.reset();
    resized();
    repaint();
}

std::array<std::vector<int>, 3> StripArea::getShownOrder() const
{
    std::array<std::vector<int>, 3> order;
    for (auto& s : inStrips) order[0].push_back (s->getSelectIndex());
    for (size_t k = 0; k < busStrips.size(); ++k)
        order[(int) k < numHw ? 1 : 2].push_back (busStrips[k]->getSelectIndex() - kMaxInputs);
    return order;
}

bool StripArea::canRemoveInReorder (int group) const
{
    // At least one input and one hardware output stay (as with Delete in the channel menu).
    return group == 2 || stripCount (group) > 1;
}

void StripArea::removeInReorder (int group, int k)
{
    if (! reordering || ! canRemoveInReorder (group) || k < 0 || k >= stripCount (group)) return;
    std::unique_ptr<juce::Component> strip;
    int sel = -1;
    if (group == 0)
    {
        sel = inStrips[(size_t) k]->getSelectIndex();
        strip = std::move (inStrips[(size_t) k]);
        inStrips.erase (inStrips.begin() + k);
    }
    else
    {
        const auto at = (size_t) (group == 1 ? k : numHw + k);
        sel = busStrips[at]->getSelectIndex();
        strip = std::move (busStrips[at]);
        busStrips.erase (busStrips.begin() + (std::ptrdiff_t) at);
        --(group == 1 ? numHw : numVirt);
    }
    animator.forget (*strip);
    strip->setVisible (false);
    removedStrips.push_back (std::move (strip));
    removedChannels.push_back (sel);
    layout (true);   // the others close the gap
    for (auto& layer : layers) if (layer != nullptr) layer->repaint();
    repaintStrips();
    repaint();
}

int StripArea::stripCount (int group) const
{
    return group == 0 ? (int) inStrips.size() : group == 1 ? numHw : numVirt;
}

juce::Component* StripArea::stripAt (int group, int k) const
{
    if (group == 0) return inStrips[(size_t) k].get();
    return busStrips[(size_t) (group == 1 ? k : numHw + k)].get();
}

void StripArea::placeStrips (int group, juce::Component* except, bool animated)
{
    const int w = stripWidth[group], h = stripHeight[group];
    const auto& sec = sections[group];
    const int count = stripCount (group);
    for (int k = 0; k < count; ++k)
    {
        auto* c = stripAt (group, k);
        if (c == except) continue;
        const auto target = shownRows ? juce::Rectangle<int> (sec.x, sec.y + k * h, w, h)
                                      : juce::Rectangle<int> (sec.x + k * w, sec.y, w, h);
        if (animated) animator.moveTo (*c, target, true);
        else          animator.place (*c, target);
    }

    if (summaries[group] != nullptr && summaries[group]->isVisible())
    {
        // Collapsed: the summary takes the group's place (the strips are hidden).
        summaries[group]->setBounds (shownRows ? juce::Rectangle<int> (sec.x, sec.y, sec.width, rowsGroupHeight (group, sec.width + 2 * kPad) - kRowCaption - kRowGroupGap)
                                               : juce::Rectangle<int> (sec.x, sec.y, juce::jmax (1, count) * kMiniWidth, h));
    }

    if (auto* layer = layers[group].get())
    {
        bool sharesPane = false;
        for (int g = 0; g < kGroups; ++g) sharesPane = sharesPane || (g != group && paneIndex (g) == paneIndex (group));
        // Sharing a pane, each group's layer covers only that group.
        layer->setBounds (shownRows   ? juce::Rectangle<int> (0, sec.y, paneOf (group).getContent().getWidth(), juce::jmax (1, count) * h)
                        : sharesPane  ? juce::Rectangle<int> (sec.x - kPad, 0, sec.width + 2 * kPad, h)
                                      : paneOf (group).getContent().getLocalBounds());
        layer->toFront (false);
        layer->repaint();
    }
}

StripArea::StripArea (UiContext& c) : ctx (c)
{
    sections[0] = { "Inputs", "Inputs", theme::accentIn };
    sections[1] = { "Hardware Outputs", "Hardware", theme::accentHw };
    sections[2] = { "Virtual Outputs", "Virtual", theme::accentVirt };
    setWantsKeyboardFocus (true);              // Esc ends a caption selection
    setMouseClickGrabsKeyboardFocus (false);   // focus only then
    addMouseListener (&clickElsewhere, true);
    for (int g = 0; g < kGroups; ++g)
    {
        collapseTools[g] = std::make_unique<CaptionTool> (*this, g, false);
        pinTools[g] = std::make_unique<CaptionTool> (*this, g, true);
        addChildComponent (*collapseTools[g]);
        addChildComponent (*pinTools[g]);
    }
    addIn.setTooltip ("Add an input channel");
    addHw.setTooltip ("Add a hardware output (H bus)");
    addVirt.setTooltip ("Add a virtual output (V bus)");
    for (int g = 0; g < kGroups; ++g)
    {
        addButtons[g]->onClick = [this, g] { if (ctx.addChannel) ctx.addChannel (g); };
        panes[g].onBackgroundMenu = [this, g] (juce::Point<int> p)
        {
            // Sharing a pane: the group whose part of the row (rows: of the list) was clicked.
            showGroupMenu (ctx, groupAt (g, p));
        };
        panes[g].onBackgroundClick = [this, g] (juce::Point<int> p)
        {
            // Horizontal rows: the captions are part of the content.
            if (! shownRows) return;
            for (int k = 0; k < kGroups; ++k)
            {
                const auto& cap = sections[k].caption;
                if (paneIndex (k) != g || ! juce::Rectangle<int> (cap.getX() - 6, cap.getY(), sections[k].captionWidth + 14, cap.getHeight()).contains (p)) continue;
                captionClick = true;
                selectCaption (captionSel == k ? -1 : k);
                return;
            }
        };
        panes[g].paintContent = [this, g] (juce::Graphics& gr) { if (shownRows) paintRowCaptions (gr, g); };
        panes[g].onScroll = [this] { if (! layingOut && ! shownRows) layoutCaptions(); };
        addAndMakeVisible (panes[g]);
    }
    for (int d = 0; d < kGroups - 1; ++d)
    {
        addAndMakeVisible (dividers[d]);
        dividers[d].onDrag = [this, d] (int x) { setSplit (d, x); };
        dividers[d].onReset = [this, d] { setSplit (d, -1); };
    }
    for (auto* b : addButtons)
        addAndMakeVisible (b);
    addIn.setComponentID ("add.in");
    addHw.setComponentID ("add.hw");
    addVirt.setComponentID ("add.virt");
    animator.onFrame = [this]
    {
        // Their corners depend on who is next to them; after a size change only once at the end. The captions follow the "+".
        if (! resizeGlide || ! animator.isMoving()) repaintStrips();
        for (auto& layer : layers) if (layer != nullptr) layer->repaint();   // trash cans follow their strips
        repaint (getLocalBounds().withHeight (captionTop() + kCaptionHeight));
    };
    rebuild();
}

void StripArea::rebuild()
{
    // Channels that stay glide to their new places when one is added or removed. (Not after Change Order:
    // there the channels have just taken new numbers, and they already are where they belong.)
    std::map<int, juce::Rectangle<int>> before;
    if (! reordering && isShowing())
    {
        for (auto& s : inStrips)  before[s->getSelectIndex()] = s->getBounds();
        for (auto& s : busStrips) before[s->getSelectIndex()] = s->getBounds();
    }
    animator.clear();

    // Leaves Change Order mode: the layers point at the strips that are recreated here.
    reordering = false;
    for (auto& layer : layers) layer.reset();
    reorderBar.reset();
    inStrips.clear();
    busStrips.clear();
    removedStrips.clear();
    removedChannels.clear();
    numHw = numVirt = 0;
    hoverBus = -1;
    shownJoined = joined();
    shownRows = rowsMode();
    shownPinned = pinMask();
    for (auto& sum : summaries) sum.reset();
    attachPlusButtons();
    for (auto& p : panes) p.setVertical (shownRows);
    auto& e = ctx.engine;
    for (int i = 0; i < kMaxInputs; ++i)
        if (e.isInputActive (i))
        {
            inStrips.push_back (std::make_unique<InputStripView> (ctx, i));
            paneOf (0).getContent().addAndMakeVisible (*inStrips.back());
        }
    for (int b = 0; b < kMaxBuses; ++b)
        if (e.isBusActive (b))
        {
            busStrips.push_back (std::make_unique<BusStripView> (ctx, b));
            paneOf (isHwBus (b) ? 1 : 2).getContent().addAndMakeVisible (*busStrips.back());
            ++(isHwBus (b) ? numHw : numVirt);
        }
    routesHw = numHw;
    routesVirt = numVirt;
    for (int g = 0; g < kGroups; ++g)
    {
        std::vector<int> sels;
        for (auto& st : inStrips) if (g == 0) sels.push_back (st->getSelectIndex());
        for (size_t k = 0; k < busStrips.size(); ++k)
            if (g == ((int) k < numHw ? 1 : 2)) sels.push_back (busStrips[k]->getSelectIndex());
        summaries[g] = std::make_unique<Summary> (*this, g);
        summaries[g]->setChannels (sels);
        paneOf (g).getContent().addChildComponent (*summaries[g]);
    }
    addIn.setVisible (e.numActiveInputs() < kMaxInputs);
    addHw.setVisible (numHw < kMaxHwBuses);
    addVirt.setVisible (numVirt < kMaxVirtBuses);

    const auto f = captionFont();
    for (auto& sec : sections)
    {
        sec.captionWidth = juce::GlyphArrangement::getStringWidthInt (f, sec.title);
        sec.shortWidth = juce::GlyphArrangement::getStringWidthInt (f, sec.shortTitle);
    }
    layout (false);
    resizeGlide = false;
    auto glide = [&] (juce::Component& c, int sel)
    {
        const auto from = before.find (sel);
        if (from == before.end() || from->second.getPosition() == c.getPosition()) return;
        const auto target = c.getBounds();
        c.setTopLeftPosition (shownRows ? target.getX() : from->second.getX(), shownRows ? from->second.getY() : target.getY());
        animator.moveTo (c, target);
    };
    for (auto& s : inStrips)  glide (*s, s->getSelectIndex());
    for (auto& s : busStrips) glide (*s, s->getSelectIndex());
    repaint();
}

int StripArea::groupWidth (int g, int strips, int w) const
{
    const auto& sec = sections[g];
    // Collapsed: the narrow columns, wide enough for the short caption and its symbols.
    if (collapsed (g) && ! rowsMode()) return juce::jmax (strips * kMiniWidth, sec.shortWidth + 10 + 2 * kToolSize + 6);
    // Wide enough for the caption and its "+" button even with a single (or no) strip.
    return juce::jmax (strips * w, sec.captionWidth + 10 + kPlusSize + 8);
}

int StripArea::paneWidthFor (int group, int w) const
{
    return 2 * kPad + groupWidth (group, stripCount (group), w);
}

float StripArea::scaleFor (int paneHeight) const
{
    // No vertical scrolling: below the strips' minimum height they are drawn smaller.
    const int minHeight = InputStripView::minimumHeight (routesHw, routesVirt);
    return juce::jlimit (0.4f, 1.0f, (float) paneHeight / (float) minHeight);
}

int StripArea::idealWidth() const
{
    return idealWidthFor (getHeight());
}

int StripArea::idealWidthFor (int areaHeight) const
{
    if (rowsMode())
        return InputStripView::rowControlsWidth (routesHw, routesVirt) + kRowFader + 6 + kRowValue + 2 * kPad + StripPane::kScrollBar;
    // Same scale as layoutPane; each pane rounded up so none of them needs its scroll bar.
    const float s = scaleFor (areaHeight - captionTop() - kCaptionHeight - StripPane::kScrollBar);
    int total = 1;
    for (int g = 0; g < kGroups; ++g)
        total += (int) std::ceil ((float) paneWidthFor (g, kStripWidth) * s);
    return total;
}

juce::Point<int> StripArea::fittingSize() const
{
    if (rowsMode())
        return { idealWidthFor (0), captionTop() + rowsContentHeight() };
    // Strips at full size with a comfortable fader (the same length adaptive strips keep when they grow).
    const int h = captionTop() + kCaptionHeight + StripPane::kScrollBar + InputStripView::minimumHeight (routesHw, routesVirt) + kComfortFader;
    return { idealWidthFor (h), h };
}

bool StripArea::rowsMode() const { return ctx.session != nullptr && ctx.session->prefs.horizontalStrips; }

int StripArea::rowControlsWidth() const
{
    // Each row's fader fills what its own controls leave free (input faders are shorter than bus faders).
    return juce::jmax (InputStripView::rowControlsWidth (routesHw, routesVirt), BusStripView::rowControlsWidth());
}

int StripArea::rowsGroupHeight (int g, int width) const
{
    const int body = collapsed (g) ? Summary::linesFor (stripCount (g), width - 2 * kPad) * kMiniLine : stripCount (g) * kRowHeight;
    return kRowCaption + body + kRowGroupGap;
}

int StripArea::rowsPaneHeight (int pane, int width) const
{
    int h = 4;
    for (int g = 0; g < kGroups; ++g)
        if (paneIndex (g) == pane) h += rowsGroupHeight (g, width);
    return h;
}

int StripArea::rowsContentHeight() const
{
    const int w = idealWidthFor (0);
    int h = 4;
    for (int g = 0; g < kGroups; ++g)
        h += rowsGroupHeight (g, w);
    return h;
}

void StripArea::attachPlusButtons()
{
    for (int g = 0; g < kGroups; ++g)
    {
        auto& parent = shownRows ? paneOf (g).getContent() : static_cast<juce::Component&> (*this);
        juce::Component* parts[] { addButtons[g], collapseTools[g].get(), pinTools[g].get() };
        for (auto* b : parts)
            if (b != nullptr && b->getParentComponent() != &parent) parent.addChildComponent (b);
    }
}

void StripArea::layoutRows (bool animated)
{
    // One row per channel, the groups one below the other, each with its caption and "+". Rows stretch to the
    // width (longer faders); below the minimum they scroll sideways, and up / down when there are too many.
    // Pinned groups have their own pane on top (it scrolls only when it would take more than 3/5 of the height).
    const int top = captionTop();
    int ids[kGroups] {}, numPanes = 0;
    auto addPane = [&] (int id) { for (int k = 0; k < numPanes; ++k) if (ids[k] == id) return; ids[numPanes++] = id; };
    for (int g = 0; g < kGroups; ++g) if (pinned (g)) addPane (paneIndex (g));
    for (int g = 0; g < kGroups; ++g) if (! pinned (g)) addPane (paneIndex (g));
    for (int p = 0; p < kGroups; ++p)
    {
        bool used = false;
        for (int k = 0; k < numPanes; ++k) used = used || ids[k] == p;
        panes[p].setVisible (used);
    }

    const int minW = rowControlsWidth() + kMinRowFader + 6 + kRowValue + 2 * kPad;
    int paneY = top;
    for (int k = 0; k < numPanes; ++k)
    {
        auto& pane = panes[ids[k]];
        int contentH = rowsPaneHeight (ids[k], juce::jmax (minW, getWidth()));
        const int paneH = k + 1 < numPanes ? juce::jmin (contentH, (getHeight() - top) * 3 / 5) : getHeight() - paneY;
        pane.setBounds (0, paneY, getWidth(), juce::jmax (1, paneH));
        paneY += paneH;
        const int w = juce::jmax (minW, pane.getWidth() - (contentH > paneH ? StripPane::kScrollBar : 0));
        contentH = rowsPaneHeight (ids[k], w);
        pane.setContentSize (w, contentH, 1.0f);

        int y = 4;
        for (int g = 0; g < kGroups; ++g)
        {
            if (paneIndex (g) != ids[k]) continue;
            auto& sec = sections[g];
            sec.caption.setY (y);
            placeRowCaption (g, animated);
            y += kRowCaption;
            sec.x = kPad;
            sec.y = y;
            sec.width = w - 2 * kPad;
            stripWidth[g] = sec.width;
            stripHeight[g] = kRowHeight;
            for (int n = 0; n < stripCount (g); ++n)
            {
                auto* c = stripAt (g, n);
                if (auto* in = dynamic_cast<InputStripView*> (c)) in->setHorizontal (true);
                else if (auto* bus = dynamic_cast<BusStripView*> (c)) bus->setHorizontal (true);
            }
            placeStrips (g, nullptr, animated);
            y += rowsGroupHeight (g, w) - kRowCaption;
        }
        pane.getContent().repaint();
    }
}

void StripArea::paintRowCaptions (juce::Graphics& g, int pane)
{
    for (int k = 0; k < kGroups; ++k)
    {
        if (paneIndex (k) != pane) continue;
        const auto& sec = sections[k];
        if (const float a = selAnim[k]; a > 0.0f)
        {
            const auto r = juce::Rectangle<int> (sec.caption.getX() - 6, sec.caption.getY() + 2, sec.captionWidth + 10 + toolsWidth (k) + 4, kRowCaption - 4).toFloat();
            g.setColour (sec.colour.withAlpha (0.12f * a));
            g.fillRoundedRectangle (r, 6.0f);
            g.setColour (sec.colour.withAlpha (0.5f * a));
            g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
        }
        g.setFont (captionFont());
        g.setColour (sec.colour);
        g.drawText (sec.title, sec.caption.withWidth (sec.captionWidth + 4), juce::Justification::centredLeft, false);
    }
    if (selBus >= 0 && paneIndex (0) == pane)
    {
        const auto& cap = sections[0].caption;
        g.setColour (theme::busColour (selBus));
        g.setFont (theme::font (11.5f, true));
        g.drawFittedText ("FADERS = LEVEL ON " + busLabel (selBus)
                              + (monBus >= 0 ? "   (monitoring on " + busLabel (monBus) + ")" : juce::String()),
                          juce::Rectangle<int> (cap.getRight() + 6, cap.getY(), juce::jmax (0, paneOf (0).getContent().getWidth() - cap.getRight() - 6 - kPad), kRowCaption),
                          juce::Justification::centredLeft, 1, 0.8f);
    }
}

bool StripArea::adaptive() const { return ctx.session == nullptr || ctx.session->prefs.adaptiveStrips; }

bool StripArea::computeFit (int paneHeight, Fit& fit)
{
    const int width = getWidth();
    if (! adaptive() || width < idealWidth()) return false;

    const int stripsHeight = paneHeight - StripPane::kScrollBar;
    const float s0 = scaleFor (stripsHeight);
    float logical = 0.0f;
    for (int g = 0; g < kGroups; ++g)
        logical += (float) paneWidthFor (g, kStripWidth);
    // Larger strips: limited by the width, and by the height (the fader keeps some extra length).
    const float byHeight = (float) stripsHeight / (float) (InputStripView::minimumHeight (routesHw, routesVirt) + kComfortFader);
    const float byWidth = (float) (width - 2) / logical;
    const float s = juce::jmax (s0, juce::jmin (kMaxGrow, byHeight, byWidth));

    // What is left goes between the groups (some) and to both sides (the rest): the mixer sits centred.
    const float left = juce::jmax (0.0f, (float) width - 2.0f - logical * s);
    const float gap = juce::jmin (left * 0.15f, (float) kMaxGroupGap * s);
    const float margin = (left - 2.0f * gap) * 0.5f;
    fit.scale = s;
    fit.margin = margin;
    fit.gap = gap;
    const float p0 = (float) paneWidthFor (0, kStripWidth) * s, p1 = (float) paneWidthFor (1, kStripWidth) * s;
    fit.splits[0] = juce::roundToInt (margin + p0 + gap * 0.5f);
    fit.splits[1] = juce::roundToInt (margin + p0 + gap + p1 + gap * 0.5f);
    return true;
}

bool StripArea::joined() const { return ctx.session != nullptr && ! ctx.session->prefs.stripDividers; }

void StripArea::attachStrips()
{
    for (int g = 0; g < kGroups; ++g)
    {
        for (int k = 0; k < stripCount (g); ++k)
        {
            auto* c = stripAt (g, k);
            paneOf (g).getContent().addAndMakeVisible (*c);
            if (! shownRows)
            {
                if (auto* in = dynamic_cast<InputStripView*> (c)) in->setHorizontal (false);
                else if (auto* bus = dynamic_cast<BusStripView*> (c)) bus->setHorizontal (false);
            }
        }
        if (layers[g] != nullptr)
            paneOf (g).getContent().addAndMakeVisible (*layers[g]);
    }
    for (int g = 0; g < kGroups; ++g)
        if (summaries[g] != nullptr) paneOf (g).getContent().addChildComponent (*summaries[g]);
    for (auto& s : removedStrips) paneOf (0).getContent().addChildComponent (*s);
    for (auto& p : panes) p.setVertical (shownRows);
    attachPlusButtons();
}

bool StripArea::dividersShown() const
{
    return (ctx.session == nullptr || ctx.session->prefs.stripDividers) && getWidth() < idealWidth();
}

std::array<int, 2> StripArea::defaultSplits() const
{
    // Each group gets exactly the room its strips need: inputs at the left, virtual outputs at the right, the hardware
    // outputs take what remains (right-aligned there). If not everything fits, each pane gets a share proportional
    // to what it needs, so they all scroll alike (the dividers only take room in that case).
    const float s = scaleFor (getHeight() - captionTop() - kCaptionHeight - StripPane::kScrollBar);
    if (getWidth() < idealWidth())
    {
        const int dividerWidth = dividersShown() ? kDividerWidth : 0, half = dividerWidth / 2;
        const int avail = getWidth() - (kGroups - 1) * dividerWidth;
        const int inW = paneWidthFor (0, kStripWidth), hwW = paneWidthFor (1, kStripWidth);
        const int total = inW + hwW + paneWidthFor (2, kStripWidth);
        return { juce::roundToInt ((float) avail * (float) inW / (float) total) + half,
                 juce::roundToInt ((float) avail * (float) (inW + hwW) / (float) total) + dividerWidth + half };
    }
    // Rounded up like idealWidth, so no pane needs its scroll bar.
    return { (int) std::ceil ((float) paneWidthFor (0, kStripWidth) * s),
             getWidth() - (int) std::ceil ((float) paneWidthFor (2, kStripWidth) * s) };
}

void StripArea::setSplit (int dividerIndex, int x)
{
    if (ctx.session == nullptr) return;
    auto& split = dividerIndex == 0 ? ctx.session->prefs.stripSplit : ctx.session->prefs.stripSplit2;
    const float wanted = x < 0 ? -1.0f : (float) x / (float) juce::jmax (1, getWidth());
    if (std::abs (wanted - split) < 0.0005f) return;
    split = wanted;
    ctx.session->prefsVersion.fetch_add (1); // stored with the session
    layout (false);   // the strips follow the mouse directly
    repaint();
}

void StripArea::layoutPane (int group, int paneHeight, bool animated, const Fit* fit)
{
    // Strips keep their width; a pane that is too narrow scrolls. With room to spare, the inputs stay
    // left-aligned and the outputs right-aligned. The scroll bar's height is always kept free, so the
    // strips do not change size when it appears.
    // Adaptive (fit): the panes meet in the middle of the gaps between the groups, each group sits right after its gap.
    auto& pane = panes[group];
    const int stripsHeight = paneHeight - StripPane::kScrollBar;
    const float s = fit != nullptr ? fit->scale : scaleFor (stripsHeight);
    const int logicalWidth = (int) std::floor ((float) pane.getWidth() / s);
    const int w = kStripWidth;
    const int needed = paneWidthFor (group, w);
    const int h = juce::roundToInt ((float) stripsHeight / s);
    const int total = juce::jmax (needed, logicalWidth);
    pane.setContentSize (total, h, s);

    auto& sec = sections[group];
    if (fit != nullptr)
        sec.x = kPad + juce::jlimit (0, total - needed, juce::roundToInt ((group == 0 ? fit->margin : fit->gap * 0.5f) / s));
    else
        sec.x = group == 0 ? kPad : kPad + (total - needed);
    sec.y = 0;
    sec.width = groupWidth (group, stripCount (group), w);
    stripWidth[group] = w;
    stripHeight[group] = h;
    placeStrips (group, nullptr, animated);
}

void StripArea::layoutJoined (int paneHeight, bool animated, const Fit* fit)
{
    // One row in one pane: inputs at the left, outputs at the right, as with separate panes.
    // Adaptive (fit): centred, with the gaps between the groups.
    auto& pane = panes[0];
    const int stripsHeight = paneHeight - StripPane::kScrollBar;
    const float s = fit != nullptr ? fit->scale : scaleFor (stripsHeight);
    const int logicalWidth = (int) std::floor ((float) pane.getWidth() / s);
    const int w = kStripWidth;
    const int needed = paneWidthFor (0, w) + paneWidthFor (1, w) + paneWidthFor (2, w);
    const int h = juce::roundToInt ((float) stripsHeight / s);
    const int total = juce::jmax (needed, logicalWidth);
    pane.setContentSize (total, h, s);

    int x = kPad + (fit != nullptr ? juce::roundToInt (fit->margin / s) : 0);
    for (int g = 0; g < kGroups; ++g)
    {
        auto& sec = sections[g];
        sec.x = x + (fit == nullptr && g == 1 ? total - needed : 0);
        sec.y = 0;
        sec.width = groupWidth (g, stripCount (g), w);
        x = sec.x + sec.width + 2 * kPad + (fit != nullptr ? juce::roundToInt (fit->gap / s) : 0);
        stripWidth[g] = w;
        stripHeight[g] = h;
        placeStrips (g, nullptr, animated);
    }
}

void StripArea::resized()
{
    layout (! ctx.instantLayout);   // while the window glides to its fitting size, the strips follow directly
}

void StripArea::layout (bool animated)
{
    if (joined() != shownJoined || rowsMode() != shownRows || pinMask() != shownPinned)
    {
        shownJoined = joined();
        shownRows = rowsMode();
        shownPinned = pinMask();
        animator.clear();
        attachStrips();
        animated = false;
    }
    applyCollapsed();
    layingOut = true;
    resizeGlide = animated;
    if (shownRows)
    {
        if (reorderBar != nullptr)
            reorderBar->setBounds (kPad, 4, getWidth() - 2 * kPad, kBannerHeight - 6);
        for (auto& d : dividers) d.setVisible (false);
        panes[1].setVisible (false);
        panes[2].setVisible (false);
        layoutRows (animated);
        layingOut = false;
        repaint();
        return;
    }
    const int oldPaneX[kGroups] { paneOf (0).getX(), paneOf (1).getX(), paneOf (2).getX() };

    const int width = getWidth(), height = getHeight();
    const int top = captionTop();
    const int paneH = height - top - kCaptionHeight, paneY = top + kCaptionHeight;
    Fit fit;
    const bool fitted = computeFit (paneH, fit);
    if (shownPinned != 0 || collapsed (0) || collapsed (1) || collapsed (2))
    {
        // Pinned or collapsed groups keep the width they need; the others share the rest (no dividers).
        if (reorderBar != nullptr)
            reorderBar->setBounds (kPad, 4, width - 2 * kPad, kBannerHeight - 6);
        for (auto& d : dividers) d.setVisible (false);
        layoutUnits (paneY, paneH, animated, fitted ? &fit : nullptr);
        layoutCaptions (animated);
        layingOut = false;
        return;
    }
    // The dividers only exist while the channels do not fit; their positions count only then.
    const bool shown = ! fitted && dividersShown();
    const int dividerWidth = shown ? kDividerWidth : 0, half = dividerWidth / 2;
    const auto defaults = defaultSplits();
    auto splitOf = [&] (float stored, int fallback) { return shown && stored >= 0.0f ? juce::roundToInt (stored * (float) width) : fallback; };
    int a = splitOf (ctx.session != nullptr ? ctx.session->prefs.stripSplit : -1.0f, defaults[0]);
    int b = splitOf (ctx.session != nullptr ? ctx.session->prefs.stripSplit2 : -1.0f, defaults[1]);
    const int minA = kMinPaneWidth + half;
    a = juce::jlimit (minA, juce::jmax (minA, width - 2 * kMinPaneWidth - dividerWidth - half), a);
    const int minB = a + dividerWidth + kMinPaneWidth;
    b = juce::jlimit (minB, juce::jmax (minB, width - kMinPaneWidth - half), b);
    if (fitted) { a = fit.splits[0]; b = fit.splits[1]; }

    if (reorderBar != nullptr)
        reorderBar->setBounds (kPad, 4, width - 2 * kPad, kBannerHeight - 6);
    dividers[0].setBounds (a - half, top, dividerWidth, height - top);
    dividers[1].setBounds (b - half, top, dividerWidth, height - top);
    for (auto& d : dividers) d.setVisible (shown);
    panes[0].setVisible (true);
    panes[1].setVisible (! shownJoined);
    panes[2].setVisible (! shownJoined);
    if (shownJoined)
    {
        panes[0].setBounds (0, paneY, width, paneH);
        layoutJoined (paneH, animated, fitted ? &fit : nullptr);
    }
    else
    {
        panes[0].setBounds (0, paneY, dividers[0].getX(), paneH);
        panes[1].setBounds (dividers[0].getRight(), paneY, dividers[1].getX() - dividers[0].getRight(), paneH);
        panes[2].setBounds (dividers[1].getRight(), paneY, width - dividers[1].getRight(), paneH);
        // A pane jumps to its new place; its strips stay where they are on screen and glide from there.
        if (animated)
            for (int g = 0; g < kGroups; ++g)
                if (const int moved = oldPaneX[g] - panes[g].getX(); moved != 0)
                    for (int k = 0; k < stripCount (g); ++k)
                        animator.shift (*stripAt (g, k), juce::roundToInt ((float) moved / panes[g].getScale()));
        for (int g = 0; g < kGroups; ++g)
            layoutPane (g, paneH, animated, fitted ? &fit : nullptr);
    }
    layoutCaptions (animated);
    layingOut = false;
}

void StripArea::layoutUnits (int paneY, int paneH, bool animated, const Fit* fit)
{
    // A unit = one pane with its groups (several only with one shared scroll bar). A unit whose groups are all
    // pinned or collapsed is fixed: it gets the width it needs, the others share what is left and scroll.
    const int width = getWidth();
    const int stripsHeight = paneH - StripPane::kScrollBar;
    const float s = fit != nullptr ? fit->scale : scaleFor (stripsHeight);
    const int h = juce::roundToInt ((float) stripsHeight / s);
    struct Unit { int first = 0, last = 0, logical = 0, x = 0, w = 0; bool fixed = true; };
    Unit units[kGroups];
    int n = 0;
    for (int g = 0; g < kGroups; ++g)
    {
        if (n == 0 || paneIndex (g) == g) { units[n] = {}; units[n].first = g; ++n; }
        auto& u = units[n - 1];
        u.last = g;
        u.logical += paneWidthFor (g, kStripWidth);
        u.fixed = u.fixed && (pinned (g) || collapsed (g));
    }
    const int oldX[kGroups] { panes[0].getX(), panes[1].getX(), panes[2].getX() };

    float gx[kGroups] {};   // everything fits: where each group starts (as in the adaptive layout)
    if (fit != nullptr)
    {
        float x = fit->margin;
        for (int g = 0; g < kGroups; ++g) { gx[g] = x; x += (float) paneWidthFor (g, kStripWidth) * s + fit->gap; }
        for (int u = 0; u < n; ++u)
        {
            const int left = u == 0 ? 0 : juce::roundToInt (gx[units[u].first] - fit->gap * 0.5f);
            const int right = u + 1 == n ? width : juce::roundToInt (gx[units[u + 1].first] - fit->gap * 0.5f);
            units[u].x = left;
            units[u].w = right - left;
        }
    }
    else
    {
        int need[kGroups] {}, widths[kGroups] {};
        int fixedSum = 0, flexSum = 0, numFlex = 0;
        for (int u = 0; u < n; ++u)
        {
            need[u] = (int) std::ceil ((float) units[u].logical * s);
            if (units[u].fixed) fixedSum += need[u];
            else { flexSum += need[u]; ++numFlex; }
        }
        if (fixedSum + flexSum <= width)
        {
            // Room to spare: to the hardware outputs (as without pins), else the last group that scrolls, else the last.
            int extra = -1;
            for (int u = 0; u < n; ++u) if (! units[u].fixed && units[u].first <= 1 && units[u].last >= 1) extra = u;
            if (extra < 0) for (int u = 0; u < n; ++u) if (! units[u].fixed) extra = u;
            if (extra < 0) extra = n - 1;
            for (int u = 0; u < n; ++u) widths[u] = need[u];
            widths[extra] += width - fixedSum - flexSum;
        }
        else if (numFlex > 0 && fixedSum + numFlex * kMinPaneWidth <= width)
        {
            int lastFlex = 0, sum = 0;
            for (int u = 0; u < n; ++u)
            {
                if (units[u].fixed) widths[u] = need[u];
                else { widths[u] = juce::jmax (kMinPaneWidth, juce::roundToInt ((float) (width - fixedSum) * (float) need[u] / (float) flexSum)); lastFlex = u; }
                sum += widths[u];
            }
            widths[lastFlex] += width - sum;
        }
        else
        {
            // Not even the fixed groups fit: all of them scroll, each in proportion to what it needs.
            int sum = 0;
            for (int u = 0; u < n; ++u) { widths[u] = juce::roundToInt ((float) width * (float) need[u] / (float) (fixedSum + flexSum)); sum += widths[u]; }
            widths[n - 1] += width - sum;
        }
        int x = 0;
        for (int u = 0; u < n; ++u) { units[u].x = x; units[u].w = widths[u]; x += widths[u]; }
    }

    for (auto& p : panes) p.setVisible (false);
    for (int u = 0; u < n; ++u)
    {
        const auto& unit = units[u];
        auto& pane = panes[unit.first];
        pane.setVisible (true);
        pane.setBounds (unit.x, paneY, juce::jmax (1, unit.w), paneH);
        const int total = juce::jmax (unit.logical, (int) std::floor ((float) pane.getWidth() / s));
        pane.setContentSize (total, h, s);
        const int moved = oldX[unit.first] - pane.getX();
        int x = 0;
        bool spread = false;
        for (int g = unit.first; g <= unit.last; ++g)
        {
            auto& sec = sections[g];
            if (fit != nullptr)
                sec.x = kPad + juce::jlimit (0, juce::jmax (0, total - paneWidthFor (g, kStripWidth)), juce::roundToInt ((gx[g] - (float) unit.x) / s));
            else
            {
                // Spare room: the inputs stay at the left, the outputs move right.
                if (g != 0 && ! spread) { x += total - unit.logical; spread = true; }
                sec.x = x + kPad;
                x += paneWidthFor (g, kStripWidth);
            }
            sec.y = 0;
            sec.width = groupWidth (g, stripCount (g), kStripWidth);
            stripWidth[g] = kStripWidth;
            stripHeight[g] = h;
            if (animated && moved != 0)   // the pane jumps, its strips glide from where they were on screen
                for (int k = 0; k < stripCount (g); ++k)
                    animator.shift (*stripAt (g, k), juce::roundToInt ((float) moved / s));
            placeStrips (g, nullptr, animated);
        }
    }
}

void StripArea::layoutCaptions (bool animated)
{
    // Captions follow their group of strips but stay in view (sticky) while the pane is scrolled.
    // The "+" is hidden while pushed out of the pane by scrolling, and while reordering.
    const int maxStrips[kGroups] { kMaxInputs, kMaxHwBuses, kMaxVirtBuses };
    for (int g = 0; g < kGroups; ++g)
    {
        auto& sec = sections[g];
        const auto& pane = paneOf (g);
        auto& plus = *addButtons[g];
        const float s = pane.getScale();
        const int titleW = captionTitleWidth (g), tools = toolsWidth (g);
        const int full = titleW + 10 + tools + (collapsed (g) ? 0 : kPlusSize + 6);
        const int groupX = pane.getX() + juce::roundToInt (((float) sec.x - pane.getScrollX()) * s);
        const int groupR = groupX + juce::roundToInt ((float) sec.width * s);
        const int x = juce::jmax (juce::jmin (juce::jmax (groupX, pane.getX() + kPad), groupR - full), pane.getX() + kPad) + 6;
        sec.caption = { x, captionTop(), full, kCaptionHeight };
        const juce::Rectangle<int> target (x + titleW + 10 + tools, captionTop() + (kCaptionHeight - kPlusSize) / 2 + 1, kPlusSize, kPlusSize - 2);
        if (animated) animator.moveTo (plus, target, true);
        else          animator.place (plus, target);
        plus.setVisible (! reordering && ! collapsed (g) && target.getX() >= pane.getX() && target.getRight() <= pane.getRight() && stripCount (g) < maxStrips[g]);
        placeTools (g, sec.caption, titleW, animated);
    }
    repaint (getLocalBounds().withHeight (captionTop() + kCaptionHeight));
}

void StripArea::scrollToChannel (int sel)
{
    for (auto& s : inStrips)  if (s->getSelectIndex() == sel) { paneOf (0).scrollToShow (s->getBounds()); return; }
    for (size_t k = 0; k < busStrips.size(); ++k)
        if (busStrips[k]->getSelectIndex() == sel) { paneOf ((int) k < numHw ? 1 : 2).scrollToShow (busStrips[k]->getBounds()); return; }
}

void StripArea::paint (juce::Graphics& g)
{
    if (shownRows) return;   // the captions are part of the rows (paintRowCaptions)
    // Section captions, each clipped to its pane
    g.setFont (captionFont());
    const int top = captionTop();
    for (int k = 0; k < kGroups; ++k)
    {
        const auto& pane = paneOf (k);
        if (! pane.isVisible()) continue;
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (pane.getX(), top, pane.getWidth(), kCaptionHeight);
        const auto& sec = sections[k];
        const int titleW = captionTitleWidth (k), tools = toolsWidth (k);
        // Follows its "+" button, which glides when the layout changes.
        const int x = addButtons[k]->getX() - tools - titleW - 10;
        if (const float a = selAnim[k]; a > 0.0f)
        {
            const auto r = juce::Rectangle<int> (x - 6, top + 3, titleW + 10 + tools + 4, kCaptionHeight - 6).toFloat();
            g.setColour (sec.colour.withAlpha (0.12f * a));
            g.fillRoundedRectangle (r, 6.0f);
            g.setColour (sec.colour.withAlpha (0.5f * a));
            g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
        }
        g.setColour (sec.colour);
        g.drawText (collapsed (k) ? sec.shortTitle : sec.title, juce::Rectangle<int> (x, top, titleW + 4, kCaptionHeight),
                    juce::Justification::centredLeft, false);
    }

    // SEL: the input faders set the level on the selected bus.
    if (selBus >= 0)
    {
        const int x = sections[0].caption.getRight() + 6;
        g.setColour (theme::busColour (selBus));
        g.setFont (theme::font (11.5f, true));
        g.drawFittedText ("FADERS = LEVEL ON " + busLabel (selBus)
                              + (monBus >= 0 ? "   (monitoring on " + busLabel (monBus) + ")" : juce::String()),
                          juce::Rectangle<int> (x, top, juce::jmax (0, juce::jmin (paneOf (0).getRight(), sections[1].caption.getX()) - x - 4), kCaptionHeight),
                          juce::Justification::centredLeft, 1, 0.8f);
    }
}

void StripArea::setSelInfo (int sb, int mb)
{
    if (sb == selBus && mb == monBus) return;
    selBus = sb;
    monBus = mb;
    if (shownRows) paneOf (0).getContent().repaint();
    else           repaint (0, captionTop(), paneOf (0).getRight(), kCaptionHeight);
}

void StripArea::updateMeters (float dt)
{
    // Hidden strips (collapsed group) leave their meters to the summary (a meter has one reader).
    for (auto& s : inStrips)  if (s->isVisible()) s->updateMeters (dt);
    for (auto& s : busStrips) if (s->isVisible()) s->updateMeters (dt);
    for (auto& sum : summaries) if (sum != nullptr && sum->isVisible()) sum->update (dt);
}

void StripArea::refreshStrips()
{
    for (auto& s : inStrips) s->refresh();
    for (auto& s : busStrips) s->refresh();
}

void StripArea::repaintStrips()
{
    for (auto& s : inStrips) s->repaint();
    for (auto& s : busStrips) s->repaint();
}

void StripArea::setHoverBus (int bus)
{
    if (bus == hoverBus) return;
    hoverBus = bus;
    for (auto& s : busStrips) s->setHoverLit (s->getBusIndex() == bus);
}

void StripArea::setFaderTarget (int bus)
{
    for (auto& s : inStrips) s->setFaderTarget (bus);
}
} // namespace hydra

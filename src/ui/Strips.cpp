#include "Strips.h"
#include "state/Session.h"

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
    const auto name = isInput ? e.inputs[(size_t) sel].name : e.buses[(size_t) bus].name;

    juce::String blocked;
    if (isInput && e.numActiveInputs() <= 1)                         blocked = "the last input stays";
    else if (! isInput && isHwBus (bus) && e.numActiveBuses (false) <= 1) blocked = "the last hardware output stays";

    juce::PopupMenu m;
    m.addSectionHeader (channelCode (sel) + "   " + name);
    m.addItem ("Rename", std::move (rename));
    m.addItem ("Change Order...", [&ctx] { if (ctx.changeOrder) ctx.changeOrder(); });
    m.addSeparator();
    m.addItem ("Remove " + channelCode (sel) + (blocked.isEmpty() ? juce::String ("...") : "   (" + blocked + ")"),
               blocked.isEmpty(), false, [&ctx, sel] { if (ctx.removeChannel) ctx.removeChannel (sel); });
    const int group = isInput ? 0 : isHwBus (bus) ? 1 : 2;
    m.addItem (isInput ? "Add Input" : isHwBus (bus) ? "Add Hardware Output" : "Add Virtual Output",
               [&ctx, group] { if (ctx.addChannel) ctx.addChannel (group); });
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target));
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
    const auto h = engine.getStreamHealth (t, input);
    const auto name = t.describe();
    switch (h.state)
    {
        case State::none:
        case State::ok:
            break;
        case State::offline:
            out.push_back ({ Chip::Marker::offline,
                             "Offline: " + name + " is not running."
                                 + (h.error.isNotEmpty() ? "\n" + h.error : juce::String())
                                 + "\nIs it plugged in and not used exclusively by another app? Hydra retries every few seconds." });
            break;
        case State::waiting:
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

void styleSmallLabel (juce::Label& l)
{
    l.setFont (theme::font (10.5f));
    l.setColour (juce::Label::textColourId, theme::textFaint);
    l.setJustificationType (juce::Justification::centred);
    l.setInterceptsMouseClicks (false, false);
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
    for (auto* comp : std::initializer_list<juce::Component*> { &header, &source, &trim, &pan,
                                                               &mono, &solo, &mute, &meter, &dbLabel })
        addAndMakeVisible (comp);

    // Routing buttons only for the buses in use.
    for (int b = 0; b < kMaxBuses; ++b)
    {
        if (! c.engine.isBusActive (b)) continue;
        auto& list = isHwBus (b) ? hwRoutes : virtRoutes;
        list.push_back (std::make_unique<Toggle> (c, *strip.route[(size_t) b], busCode (b), theme::busColour (b)));
        addAndMakeVisible (*list.back());
    }

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

void InputStripView::setFaderTarget (int bus)
{
    if (bus == faderBus && fader != nullptr) return;
    faderBus = bus;
    faderParam = bus < 0 ? strip.gain : strip.send[(size_t) bus];
    fader = std::make_unique<Fader> (ctx, *faderParam, bus < 0 ? theme::accentIn : theme::busColour (bus));
    if (bus >= 0)
        fader->setTooltip ("Level of " + strip.name + " on " + busCode (bus) + "  (bus " + busCode (bus) + " is selected with SEL)");
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
    if (faderBus < 0 || fader == nullptr) return;
    // Tag above the fader: which bus the fader currently controls.
    const auto colour = theme::busColour (faderBus);
    auto tag = juce::Rectangle<int> (fader->getX(), fader->getY() - 2, juce::jmin (fader->getWidth(), 44), 15).toFloat();
    g.setColour (colour.withAlpha (faderRouted ? 0.9f : 0.35f));
    g.fillRoundedRectangle (tag, 4.0f);
    g.setColour (theme::bg);
    g.setFont (theme::font (10.5f, true));
    g.drawText ("> " + busCode (faderBus), tag, juce::Justification::centred);
}

void InputStripView::refresh()
{
    header.refresh();
    source.text = strip.source.describe();
    source.active = strip.source.kind != IoTarget::Kind::None;
    source.colour = source.active ? theme::text : theme::textDim;
    statusTick = 15;   // check the new source's state with the next meter update
    source.repaint();
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

void InputStripView::resized()
{
    auto r = getLocalBounds().reduced (9, 8);
    header.setBounds (r.removeFromTop (28));
    r.removeFromTop (4);
    source.setBounds (r.removeFromTop (24));
    r.removeFromTop (4);
    auto knobs = r.removeFromTop (54);
    layoutRow (knobs, { &trim, &pan }, 4);
    r.removeFromTop (10);

    // Routing: the A buses in one group, the B buses in a separate group below (three per row).
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

    auto meterArea = r.removeFromRight (12);
    meter.setBounds (meterArea.reduced (0, 14));
    r.removeFromRight (2);
    if (fader != nullptr) fader->setBounds (r);
}

//==============================================================================
BusStripView::BusStripView (UiContext& c, int i)
    : ctx (c), index (i), bus (c.engine.buses[(size_t) i]),
      header (c, busSelectIndex (i), busCode (i), theme::busColour (i),
              [this] { return bus.name; }, [this] (juce::String n) { bus.name = n; }),
      lim (c, *bus.limOn, "LIM", theme::busColour (i)),
      mono (c, *bus.mono, "MONO", theme::good),
      sel (c, *bus.sel, "SEL", theme::learn),
      mute (c, *bus.mute, "MUTE", theme::mute),
      fader (c, *bus.gain, theme::busColour (i)),
      meter (bus.meter)
{
    for (auto* comp : std::initializer_list<juce::Component*> { &header, &output, &outputStatus, &lim, &mono, &sel, &mute,
                                                               &fader, &meter, &dbLabel })
        addAndMakeVisible (comp);

    output.onClick = [this] { ctx.select (busSelectIndex (index)); if (ctx.openDevicePicker) ctx.openDevicePicker (busSelectIndex (index)); };
    output.setTooltip ("Select output device for this bus");
    sel.setTooltip ("SEL: the input faders set the level of each input on " + busCode (i)
                    + " (separate from the other buses). With monitoring enabled in Settings > General, "
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
        repaint();
    }
    if ((++tick & 15) == 0)
    {
        // Monitoring note below the output; problems are symbols in the output chip.
        outputStatus.setText (monitorShown ? "monitoring " + busCode (monSrc) : juce::String(), juce::dontSendNotification);
        output.setStatus (statusFor (ctx.engine, bus.output, false));
    }
}

void BusStripView::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (2.0f);
    const bool sel = ctx.selected == busSelectIndex (index);
    g.setColour (theme::panel);
    g.fillRoundedRectangle (r, 10.0f);
    if (bus.sel->isOn() || monitorShown)
    {
        g.setColour (theme::learn.withAlpha (monitorShown ? 0.08f : 0.05f));
        g.fillRoundedRectangle (r, 10.0f);
    }
    g.setColour (sel ? theme::busColour (index).withAlpha (0.7f) : theme::outline.withAlpha (0.6f));
    g.drawRoundedRectangle (r, 10.0f, sel ? 1.5f : 1.0f);
}

void BusStripView::resized()
{
    auto r = getLocalBounds().reduced (9, 8);
    header.setBounds (r.removeFromTop (28));
    r.removeFromTop (4);
    output.setBounds (r.removeFromTop (24));
    outputStatus.setBounds (r.removeFromTop (15));
    r.removeFromTop (10);
    lim.setBounds (r.removeFromTop (20));
    r.removeFromTop (3);
    sel.setBounds (r.removeFromTop (20));
    r.removeFromTop (8);

    mute.setBounds (r.removeFromBottom (24));
    r.removeFromBottom (3);
    mono.setBounds (r.removeFromBottom (20));
    r.removeFromBottom (4);
    dbLabel.setBounds (r.removeFromBottom (16));

    auto meterArea = r.removeFromRight (12);
    meter.setBounds (meterArea.reduced (0, 14));
    r.removeFromRight (2);
    fader.setBounds (r);
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
    addAndMakeVisible (view);
}

void StripPane::resized() { view.setBounds (getLocalBounds()); }

void StripPane::setContentSize (int w, int h, float s)
{
    scale = s;
    content.setTransform (juce::AffineTransform::scale (s));
    content.setBounds (0, 0, w, h);
    holder.setSize (juce::roundToInt ((float) w * s), juce::roundToInt ((float) h * s));
    if (onScroll) onScroll();
}

float StripPane::getScrollX() const { return (float) view.getViewPositionX() / scale; }

void StripPane::scrollToShow (juce::Rectangle<int> area)
{
    const auto a = area.toFloat().transformedBy (content.getTransform()).toNearestInt();
    const auto v = view.getViewArea();
    if (a.getX() < v.getX())              view.setViewPosition (a.getX() - 8, 0);
    else if (a.getRight() > v.getRight()) view.setViewPosition (a.getRight() + 8 - v.getWidth(), 0);
}

//==============================================================================
PaneDivider::PaneDivider()
{
    setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
    setTooltip ("Drag to give the inputs or the outputs more room.  Double-click: default position.");
}

void PaneDivider::paint (juce::Graphics& g)
{
    const float x = (float) getWidth() * 0.5f;
    const bool active = hover || dragging;
    g.setColour (active ? theme::accentIn.withAlpha (0.8f) : theme::outline.withAlpha (0.7f));
    g.fillRect (juce::Rectangle<float> (x - (active ? 1.0f : 0.5f), 8.0f, active ? 2.0f : 1.0f, (float) getHeight() - 16.0f));
    // Grip in the middle
    auto grip = juce::Rectangle<float> (0.0f, 0.0f, 8.0f, 34.0f).withCentre ({ x, (float) getHeight() * 0.5f });
    g.setColour (active ? theme::raised.brighter (0.1f) : theme::raised);
    g.fillRoundedRectangle (grip, 4.0f);
    g.setColour (active ? theme::text : theme::textFaint);
    for (int k = 0; k < 3; ++k)
        g.fillEllipse (x - 1.2f, grip.getY() + 9.0f + (float) k * 7.0f, 2.4f, 2.4f);
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
constexpr int kPad = 8, kBusGroupGap = 14, kPlusSize = 26, kMinPaneWidth = 120;
juce::Font captionFont() { return theme::font (17.0f, true); }
} // namespace

//==============================================================================
/** Covers a pane's strips in Change Order mode: drag a strip sideways to move it within its group.
    The other strips make room as soon as the dragged one passes half of a neighbour. */
class StripArea::DragLayer final : public juce::Component
{
public:
    DragLayer (StripArea& o, bool isOutputs) : owner (o), outputs (isOutputs)
    {
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragIndex = -1;
        for (int k = 0; k < count(); ++k)
            if (auto* c = strip (k); c->getX() <= e.x && e.x < c->getRight())
            {
                dragIndex = k;
                grabOffset = e.x - c->getX();
                c->toFront (false);
                toFront (false);   // stays on top for the next click
                repaint();
                return;
            }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragIndex < 0) return;
        auto* c = strip (dragIndex);
        const auto [lo, hi, sec] = groupOf (dragIndex);
        const int w = owner.stripWidth[outputs ? 1 : 0];
        const int x = juce::jlimit (sec->x, sec->x + (hi - lo - 1) * w, e.x - grabOffset);
        c->setTopLeftPosition (x, 0);
        const int target = lo + juce::jlimit (0, hi - lo - 1, juce::roundToInt ((float) (x - sec->x) / (float) w));
        if (target != dragIndex)
        {
            move (dragIndex, target);
            dragIndex = target;
            owner.placeStrips (outputs, c);
        }
        repaint();
        auto& pane = outputs ? owner.outPane : owner.inPane;
        pane.autoScroll (e.getEventRelativeTo (&pane).getPosition());
        beginDragAutoRepeat (40);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        dragIndex = -1;
        owner.placeStrips (outputs);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        for (int k = 0; k < count(); ++k)
        {
            auto* c = strip (k);
            auto r = c->getBounds().toFloat().reduced (2.0f, 1.0f);
            const auto colour = std::get<2> (groupOf (k))->colour;
            if (k == dragIndex)
            {
                g.setColour (colour.withAlpha (0.08f));
                g.fillRoundedRectangle (r, 8.0f);
                g.setColour (colour);
                g.drawRoundedRectangle (r, 8.0f, 2.0f);
                continue;
            }
            g.setColour (theme::bg.withAlpha (0.45f));
            g.fillRoundedRectangle (r, 8.0f);
            g.setColour (colour.withAlpha (0.35f));
            g.drawRoundedRectangle (r.reduced (1.0f), 8.0f, 1.0f);
            drawGrip (g, r.getCentre(), colour);
        }
    }

private:
    int count() const { return outputs ? (int) owner.busStrips.size() : (int) owner.inStrips.size(); }
    juce::Component* strip (int k) const
    {
        return outputs ? (juce::Component*) owner.busStrips[(size_t) k].get() : (juce::Component*) owner.inStrips[(size_t) k].get();
    }
    /** Range [lo, hi) of the strips in the same group as strip k, and the group's section. */
    std::tuple<int, int, Section*> groupOf (int k) const
    {
        if (! outputs) return { 0, count(), &owner.inSec };
        if (k < owner.numHw) return { 0, owner.numHw, &owner.hwSec };
        return { owner.numHw, count(), &owner.virtSec };
    }
    void move (int from, int to)
    {
        auto rotate = [from, to] (auto& v)
        {
            if (from < to) std::rotate (v.begin() + from, v.begin() + from + 1, v.begin() + to + 1);
            else           std::rotate (v.begin() + to, v.begin() + from, v.begin() + from + 1);
        };
        if (outputs) rotate (owner.busStrips);
        else         rotate (owner.inStrips);
    }
    static void drawGrip (juce::Graphics& g, juce::Point<float> c, juce::Colour colour)
    {
        // Pill with a left / right arrow: "drag me sideways".
        auto pill = juce::Rectangle<float> (40.0f, 24.0f).withCentre (c);
        g.setColour (theme::raised.withAlpha (0.95f));
        g.fillRoundedRectangle (pill, 12.0f);
        g.setColour (colour.withAlpha (0.8f));
        g.drawRoundedRectangle (pill, 12.0f, 1.0f);
        juce::Path p;
        p.startNewSubPath (c.x - 9.0f, c.y);
        p.lineTo (c.x + 9.0f, c.y);
        p.addTriangle (c.x - 13.0f, c.y, c.x - 8.0f, c.y - 4.5f, c.x - 8.0f, c.y + 4.5f);
        p.addTriangle (c.x + 13.0f, c.y, c.x + 8.0f, c.y - 4.5f, c.x + 8.0f, c.y + 4.5f);
        g.strokePath (p, juce::PathStrokeType (1.6f));
        g.fillPath (p);
    }

    StripArea& owner;
    const bool outputs;
    int dragIndex = -1, grabOffset = 0;
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
        g.drawFittedText ("Drag the channels sideways into the new order. Inputs, hardware and virtual outputs each keep their "
                          "own group; every channel takes the number of its new place.",
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

StripArea::~StripArea() = default;

void StripArea::beginReorder()
{
    if (reordering) return;
    reordering = true;
    inLayer = std::make_unique<DragLayer> (*this, false);
    outLayer = std::make_unique<DragLayer> (*this, true);
    inPane.getContent().addAndMakeVisible (*inLayer);
    outPane.getContent().addAndMakeVisible (*outLayer);
    reorderBar = std::make_unique<ReorderBar> (*this);
    addAndMakeVisible (*reorderBar);
    resized();
    repaint();
}

void StripArea::endReorder()
{
    if (! reordering) return;
    reordering = false;
    inLayer.reset();
    outLayer.reset();
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

void StripArea::placeStrips (bool outputs, juce::Component* except)
{
    const int w = stripWidth[outputs ? 1 : 0], h = stripHeight[outputs ? 1 : 0];
    auto put = [&] (const Section& sec, int k, juce::Component* c)
    {
        if (c != except) c->setBounds (sec.x + k * w, 0, w, h);
    };
    if (! outputs)
        for (size_t k = 0; k < inStrips.size(); ++k) put (inSec, (int) k, inStrips[k].get());
    else
        for (size_t k = 0; k < busStrips.size(); ++k)
            put ((int) k < numHw ? hwSec : virtSec, (int) k < numHw ? (int) k : (int) k - numHw, busStrips[k].get());

    if (auto* layer = outputs ? outLayer.get() : inLayer.get())
    {
        auto& content = (outputs ? outPane : inPane).getContent();
        layer->setBounds (content.getLocalBounds());
        layer->toFront (false);
        layer->repaint();
    }
}

StripArea::StripArea (UiContext& c) : ctx (c)
{
    inSec   = { "Inputs", theme::accentIn };
    hwSec   = { "Hardware Outputs", theme::accentHw };
    virtSec = { "Virtual Outputs", theme::accentVirt };
    addIn.setTooltip ("Add an input channel");
    addHw.setTooltip ("Add a hardware output (A bus)");
    addVirt.setTooltip ("Add a virtual output (B bus)");
    addIn.onClick   = [this] { if (ctx.addChannel) ctx.addChannel (0); };
    addHw.onClick   = [this] { if (ctx.addChannel) ctx.addChannel (1); };
    addVirt.onClick = [this] { if (ctx.addChannel) ctx.addChannel (2); };
    for (auto* pane : { &inPane, &outPane })
    {
        addAndMakeVisible (pane);
        pane->onScroll = [this] { layoutCaptions(); };
    }
    addAndMakeVisible (divider);
    divider.onDrag = [this] (int x) { setSplit (x); };
    divider.onReset = [this] { setSplit (-1); };
    for (auto* b : { &addIn, &addHw, &addVirt })
        addAndMakeVisible (b);
    rebuild();
}

void StripArea::rebuild()
{
    // Leaves Change Order mode: the layers point at the strips that are recreated here.
    reordering = false;
    inLayer.reset();
    outLayer.reset();
    reorderBar.reset();
    inStrips.clear();
    busStrips.clear();
    numHw = numVirt = 0;
    auto& e = ctx.engine;
    for (int i = 0; i < kMaxInputs; ++i)
        if (e.isInputActive (i))
        {
            inStrips.push_back (std::make_unique<InputStripView> (ctx, i));
            inPane.getContent().addAndMakeVisible (*inStrips.back());
        }
    for (int b = 0; b < kMaxBuses; ++b)
        if (e.isBusActive (b))
        {
            busStrips.push_back (std::make_unique<BusStripView> (ctx, b));
            outPane.getContent().addAndMakeVisible (*busStrips.back());
            ++(isHwBus (b) ? numHw : numVirt);
        }
    addIn.setVisible (e.numActiveInputs() < kMaxInputs);
    addHw.setVisible (numHw < kMaxHwBuses);
    addVirt.setVisible (numVirt < kMaxVirtBuses);

    const auto f = captionFont();
    for (auto* sec : { &inSec, &hwSec, &virtSec })
        sec->captionWidth = juce::GlyphArrangement::getStringWidthInt (f, sec->title);
    resized();
    repaint();
}

int StripArea::groupWidth (const Section& sec, int strips, int w) const
{
    // Wide enough for the caption and its "+" button even with a single (or no) strip.
    return juce::jmax (strips * w, sec.captionWidth + 10 + kPlusSize + 8);
}

int StripArea::inputsWidth (int w) const  { return 2 * kPad + groupWidth (inSec, (int) inStrips.size(), w); }
int StripArea::outputsWidth (int w) const { return 2 * kPad + groupWidth (hwSec, numHw, w) + kBusGroupGap + groupWidth (virtSec, numVirt, w); }

float StripArea::scaleFor (int paneHeight) const
{
    // No vertical scrolling: below the strips' minimum height they are drawn smaller.
    const int minHeight = InputStripView::minimumHeight (numHw, numVirt);
    return juce::jlimit (0.4f, 1.0f, (float) paneHeight / (float) minHeight);
}

int StripArea::idealWidth() const
{
    // Same scale as layoutPane; each pane rounded up so neither of them needs its scroll bar.
    const float s = scaleFor (getHeight() - captionTop() - kCaptionHeight - StripPane::kScrollBar);
    return (int) std::ceil ((float) inputsWidth (kStripWidth) * s) + (int) std::ceil ((float) outputsWidth (kStripWidth) * s)
         + kDividerWidth + 1;
}

int StripArea::defaultSplit() const
{
    // The inputs get exactly the room their strips need, the outputs the rest (right-aligned there).
    // If not everything fits, each pane gets a share proportional to what it needs, so both scroll alike.
    const int avail = getWidth() - kDividerWidth;
    const float s = scaleFor (getHeight() - kCaptionHeight - StripPane::kScrollBar);
    const int inW = inputsWidth (kStripWidth), outW = outputsWidth (kStripWidth);
    const int split = (float) (inW + outW) * s <= (float) avail
                          ? juce::roundToInt ((float) inW * s)
                          : juce::roundToInt ((float) avail * (float) inW / (float) (inW + outW));
    return split + kDividerWidth / 2;
}

void StripArea::setSplit (int x)
{
    if (ctx.session == nullptr) return;
    auto& split = ctx.session->prefs.stripSplit;
    const float wanted = x < 0 ? -1.0f : (float) x / (float) juce::jmax (1, getWidth());
    if (std::abs (wanted - split) < 0.0005f) return;
    split = wanted;
    ctx.session->prefsVersion.fetch_add (1); // stored with the session
    resized();
    repaint();
}

void StripArea::layoutPane (StripPane& pane, bool outputs, int paneWidth, int paneHeight)
{
    // Strips keep their width; a pane that is too narrow scrolls. With room to spare, the inputs stay
    // left-aligned and the outputs right-aligned. The scroll bar's height is always kept free, so the
    // strips do not change size when it appears.
    const int stripsHeight = paneHeight - StripPane::kScrollBar;
    const float s = scaleFor (stripsHeight);
    const int w = kStripWidth;
    const int needed = outputs ? outputsWidth (w) : inputsWidth (w);
    const int h = juce::roundToInt ((float) stripsHeight / s);
    const int total = juce::jmax (needed, (int) std::floor ((float) paneWidth / s));
    pane.setContentSize (total, h, s);

    auto place = [&] (Section& sec, int x, int count)
    {
        sec.x = x;
        sec.width = groupWidth (sec, count, w);
        return x + sec.width;
    };
    if (! outputs)
        place (inSec, kPad, (int) inStrips.size());
    else
        place (virtSec, place (hwSec, kPad + (total - needed), numHw) + kBusGroupGap, numVirt);
    stripWidth[outputs ? 1 : 0] = w;
    stripHeight[outputs ? 1 : 0] = h;
    placeStrips (outputs);
}

void StripArea::resized()
{
    const int width = getWidth(), height = getHeight();
    const float stored = ctx.session != nullptr ? ctx.session->prefs.stripSplit : -1.0f;
    int split = stored >= 0.0f ? juce::roundToInt (stored * (float) width) : defaultSplit();
    split = juce::jlimit (kMinPaneWidth + kDividerWidth / 2, juce::jmax (kMinPaneWidth + kDividerWidth / 2, width - kMinPaneWidth - kDividerWidth / 2), split);

    const int top = captionTop();
    if (reorderBar != nullptr)
        reorderBar->setBounds (kPad, 4, width - 2 * kPad, kBannerHeight - 6);
    divider.setBounds (split - kDividerWidth / 2, top, kDividerWidth, height - top);
    const int paneH = height - top - kCaptionHeight;
    inPane.setBounds (0, top + kCaptionHeight, divider.getX(), paneH);
    outPane.setBounds (divider.getRight(), top + kCaptionHeight, width - divider.getRight(), paneH);
    layoutPane (inPane, false, inPane.getWidth(), paneH);
    layoutPane (outPane, true, outPane.getWidth(), paneH);
    layoutCaptions();
}

void StripArea::layoutCaptions()
{
    // Captions follow their group of strips but stay in view (sticky) while the pane is scrolled.
    auto place = [this] (Section& sec, const StripPane& pane, PlusButton& plus)
    {
        const float s = pane.getScale();
        const int full = sec.captionWidth + 10 + kPlusSize + 6;
        const int groupX = pane.getX() + juce::roundToInt (((float) sec.x - pane.getScrollX()) * s);
        const int groupR = groupX + juce::roundToInt ((float) sec.width * s);
        const int x = juce::jmin (juce::jmax (groupX, pane.getX() + kPad), groupR - full) + 6;
        sec.caption = { x, captionTop(), full, kCaptionHeight };
        plus.setBounds (x + sec.captionWidth + 10, captionTop() + (kCaptionHeight - kPlusSize) / 2 + 1, kPlusSize, kPlusSize - 2);
    };
    place (inSec, inPane, addIn);
    place (hwSec, outPane, addHw);
    place (virtSec, outPane, addVirt);
    // Hidden while pushed out of the pane by scrolling (or by the other caption), and while reordering.
    addIn.setVisible (! reordering && addIn.getRight() <= inPane.getRight() && ctx.engine.numActiveInputs() < kMaxInputs);
    addHw.setVisible (! reordering && addHw.getX() >= outPane.getX() && addHw.getRight() <= virtSec.caption.getX() - 4 && numHw < kMaxHwBuses);
    addVirt.setVisible (! reordering && addVirt.getRight() <= getWidth() && addVirt.getX() >= outPane.getX() && numVirt < kMaxVirtBuses);
    repaint (getLocalBounds().withHeight (captionTop() + kCaptionHeight));
}

void StripArea::scrollToChannel (int sel)
{
    for (auto& s : inStrips)  if (s->getSelectIndex() == sel) { inPane.scrollToShow (s->getBounds()); return; }
    for (auto& b : busStrips) if (b->getSelectIndex() == sel) { outPane.scrollToShow (b->getBounds()); return; }
}

void StripArea::paint (juce::Graphics& g)
{
    // Section captions, each clipped to its pane
    g.setFont (captionFont());
    auto caption = [&] (const Section& sec, juce::Rectangle<int> clip)
    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (clip);
        g.setColour (sec.colour);
        g.drawText (sec.title, sec.caption.withWidth (sec.captionWidth + 4), juce::Justification::centredLeft, false);
    };
    const int top = captionTop();
    caption (inSec, { 0, top, inPane.getRight(), kCaptionHeight });
    caption (hwSec, { outPane.getX(), top, juce::jmax (0, virtSec.caption.getX() - outPane.getX() - 4), kCaptionHeight });
    caption (virtSec, { outPane.getX(), top, outPane.getWidth(), kCaptionHeight });

    // SEL: the input faders set the level on the selected bus.
    if (selBus >= 0)
    {
        const int x = inSec.caption.getRight() + 6;
        g.setColour (theme::busColour (selBus));
        g.setFont (theme::font (11.5f, true));
        g.drawFittedText ("FADERS = LEVEL ON " + busCode (selBus)
                              + (monBus >= 0 ? "   (monitoring on " + busCode (monBus) + ")" : juce::String()),
                          juce::Rectangle<int> (x, top, juce::jmax (0, inPane.getRight() - x - 4), kCaptionHeight),
                          juce::Justification::centredLeft, 1, 0.8f);
    }
}

void StripArea::setSelInfo (int sb, int mb)
{
    if (sb == selBus && mb == monBus) return;
    selBus = sb;
    monBus = mb;
    repaint (0, captionTop(), inPane.getRight(), kCaptionHeight);
}

void StripArea::updateMeters (float dt)
{
    for (auto& s : inStrips) s->updateMeters (dt);
    for (auto& s : busStrips) s->updateMeters (dt);
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

void StripArea::setFaderTarget (int bus)
{
    for (auto& s : inStrips) s->setFaderTarget (bus);
}
} // namespace hydra

#include "Strips.h"
#include "state/Session.h"

namespace hydra
{
namespace IoMenus
{
namespace
{
const char* kLowLatency = "Windows Audio (Low Latency Mode)";
const char* kExclusive  = "Windows Audio (Exclusive Mode)";
const char* kShared     = "Windows Audio";

/** Stereo pairs + mono channels of a multichannel device (the master or a secondary ASIO driver). */
void addChannelList (juce::PopupMenu& menu, const juce::StringArray& names, const IoTarget& base, const IoTarget& current,
                     std::function<void (IoTarget)> pick)
{
    for (int c = 0; c + 1 < names.size(); c += 2)
    {
        IoTarget t = base; t.channel = c; t.mono = false;
        menu.addItem (juce::String (c + 1) + "/" + juce::String (c + 2) + ":  " + names[c] + " + " + names[c + 1],
                      true, current == t, [pick, t] { pick (t); });
    }
    juce::PopupMenu monoMenu;
    for (int c = 0; c < names.size(); ++c)
    {
        IoTarget t = base; t.channel = c; t.mono = true;
        monoMenu.addItem (juce::String (c + 1) + ":  " + names[c], true, current == t, [pick, t] { pick (t); });
    }
    if (names.size() > 0)
        menu.addSubMenu ("Mono channel", monoMenu);
}

void addMasterChannels (juce::PopupMenu& menu, const juce::StringArray& names, const IoTarget& current,
                        std::function<void (IoTarget)> pick)
{
    IoTarget base; base.kind = IoTarget::Kind::Master;
    addChannelList (menu, names, base, current, std::move (pick));
}

/** Further ASIO interfaces next to the master: each has its own clock and is synchronised to the
    master like a Windows device, but with ASIO's low latency and all of its channels. */
void addSecondaryAsio (juce::PopupMenu& menu, Engine& engine, bool inputs, const IoTarget& current,
                       std::function<void (IoTarget)> pick)
{
    const auto drivers = engine.getSecondaryAsioDrivers();
    if (drivers.isEmpty()) return;

    menu.addSectionHeader ("Other ASIO Devices (synced to the master clock)");
    for (auto& driver : drivers)
    {
        IoTarget base; base.kind = IoTarget::Kind::Device; base.typeName = kAsioTypeName; base.deviceName = driver;
        juce::PopupMenu sub;
        const auto names = engine.getAsioChannelNames (driver, inputs);
        if (names.isEmpty())
            sub.addItem (juce::String ("(driver cannot be loaded or has no ") + (inputs ? "inputs" : "outputs")
                         + " - in use by another program?)", false, false, nullptr);
        addChannelList (sub, names, base, current, pick);
        menu.addSubMenu (driver, sub, true, nullptr, current.sameStream (base));
    }
}

void addDeviceList (juce::PopupMenu& menu, Engine& engine, const juce::String& typeName, bool inputs,
                    const IoTarget& current, std::function<void (IoTarget)> pick)
{
    auto* type = engine.findType (typeName);
    if (type == nullptr) return;
    for (auto& name : type->getDeviceNames (inputs))
    {
        IoTarget st; st.kind = IoTarget::Kind::Device; st.typeName = typeName; st.deviceName = name;
        IoTarget m1 = st; m1.mono = true;
        juce::PopupMenu sub;
        sub.addItem ("Stereo (1/2)", true, current == st, [pick, st] { pick (st); });
        sub.addItem ("Mono (Ch 1)", true, current == m1, [pick, m1] { pick (m1); });
        IoTarget m2 = m1; m2.channel = 1;
        sub.addItem ("Mono (Ch 2)", true, current == m2, [pick, m2] { pick (m2); });
        menu.addSubMenu (name, sub, true, nullptr, current.sameStream (st));
    }
}
} // namespace

void showInputMenu (juce::Component& target, UiContext& ctx, int strip, std::function<void()> changed)
{
    auto& engine = ctx.engine;
    const auto current = engine.inputs[(size_t) strip].source;
    auto pick = [&engine, strip, changed] (IoTarget t) { engine.setInputSource (strip, t); if (changed) changed(); };

    juce::PopupMenu m;
    m.addItem ("None", true, current.kind == IoTarget::Kind::None, [pick] { pick ({}); });

    if (auto* dev = engine.getMasterDevice())
    {
        m.addSectionHeader ("Hardware - " + dev->getName() + " (" + dev->getTypeName() + ")");
        addMasterChannels (m, engine.getMasterInputNames(), current, pick);
        if (engine.getMasterInputNames().isEmpty())
            m.addItem ("(master device has no inputs - choose an ASIO driver for direct inputs)", false, false, nullptr);
    }
    addSecondaryAsio (m, engine, true, current, pick);

    m.addSectionHeader ("Windows Recording Devices");
    juce::PopupMenu ll, ex, sh;
    addDeviceList (ll, engine, kLowLatency, true, current, pick);
    addDeviceList (ex, engine, kExclusive, true, current, pick);
    addDeviceList (sh, engine, kShared, true, current, pick);
    m.addSubMenu ("Low Latency (shared)", ll);
    m.addSubMenu ("Exclusive (lowest latency)", ex);
    m.addSubMenu ("Shared (compatibility)", sh);

    m.addSectionHeader ("Software Inputs");

    // Per-application capture (no driver needed)
    juce::PopupMenu apps;
    auto running = listAudioApps();
    if (current.kind == IoTarget::Kind::App) running.addIfNotAlreadyThere (current.deviceName, true);
    for (auto& exe : running)
    {
        IoTarget t; t.kind = IoTarget::Kind::App; t.deviceName = exe;
        apps.addItem (exe, true, current == t, [pick, t] { pick (t); });
    }
    if (running.isEmpty())
        apps.addItem ("(no app is playing audio right now)", false, false, nullptr);
    m.addSubMenu ("Application (per-app capture)", apps);

    // Virtual cables: apps play into the cable, Hydra records its other end
    juce::PopupMenu cables;
    for (auto& c : findVirtualCables (engine.getDeviceTypes()))
    {
        IoTarget t; t.kind = IoTarget::Kind::Device; t.typeName = kLowLatency; t.deviceName = c.recordingName;
        cables.addItem (c.label + "      (apps play to \"" + c.playbackName + "\")", true, current.sameStream (t), [pick, t] { pick (t); });
    }
    if (cables.getNumItems() == 0)
        cables.addItem ("(no virtual cable installed - see Settings > Virtual Cables)", false, false, nullptr);
    m.addSubMenu ("Virtual Cable", cables);

    juce::PopupMenu loop;
    for (auto& name : listRenderEndpoints())
    {
        IoTarget t; t.kind = IoTarget::Kind::Loopback; t.deviceName = name;
        loop.addItem (name, true, current == t, [pick, t] { pick (t); });
    }
    m.addSubMenu ("Capture a Playback Device (Loopback)", loop);

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target).withMinimumWidth (target.getWidth()));
}

void showOutputMenu (juce::Component& target, UiContext& ctx, int bus, std::function<void()> changed)
{
    auto& engine = ctx.engine;
    const auto current = engine.buses[(size_t) bus].output;
    auto pick = [&engine, bus, changed] (IoTarget t) { engine.setBusOutput (bus, t); if (changed) changed(); };

    juce::PopupMenu m;
    m.addItem ("None", true, current.kind == IoTarget::Kind::None, [pick] { pick ({}); });

    if (auto* dev = engine.getMasterDevice())
    {
        m.addSectionHeader ("Master - " + dev->getName() + " (" + dev->getTypeName() + ")");
        addMasterChannels (m, engine.getMasterOutputNames(), current, pick);
    }
    addSecondaryAsio (m, engine, false, current, pick);

    m.addSectionHeader ("Windows Playback Devices");
    juce::PopupMenu ll, ex, sh;
    addDeviceList (ll, engine, kLowLatency, false, current, pick);
    addDeviceList (ex, engine, kExclusive, false, current, pick);
    addDeviceList (sh, engine, kShared, false, current, pick);
    m.addSubMenu ("Low Latency (shared)", ll);
    m.addSubMenu ("Exclusive (lowest latency)", ex);
    m.addSubMenu ("Shared (compatibility)", sh);

    m.addSectionHeader ("Software Outputs");
    juce::PopupMenu cables;
    for (auto& c : findVirtualCables (engine.getDeviceTypes()))
    {
        IoTarget t; t.kind = IoTarget::Kind::Device; t.typeName = kLowLatency; t.deviceName = c.playbackName;
        cables.addItem (c.label + "      (apps record from \"" + c.recordingName + "\")", true, current.sameStream (t), [pick, t] { pick (t); });
    }
    if (cables.getNumItems() == 0)
        cables.addItem ("(no virtual cable installed - see Settings > Virtual Cables)", false, false, nullptr);
    m.addSubMenu ("Virtual Cable", cables);

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target).withMinimumWidth (target.getWidth()));
}
} // namespace IoMenus

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
    for (auto* comp : std::initializer_list<juce::Component*> { &header, &source, &sourceStatus, &trim, &pan,
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

    source.onClick = [this] { IoMenus::showInputMenu (source, ctx, index, [this] { refresh(); }); };
    source.setTooltip ("Select input source (hardware, Windows device, virtual input or loopback)");
    styleSmallLabel (sourceStatus);
    addMouseListener (this, true);

    dbPoll.v = this;
    setFaderTarget (-1);
    ParamPoller::attach (&dbPoll);
    refresh();
}

int InputStripView::minimumHeight (int numHw, int numVirt)
{
    auto group = [] (int count) { const int rows = (count + 2) / 3; return count > 0 ? rows * 20 + (rows - 1) * 3 + 6 + 6 : 0; };
    return 16 + 28 + 4 + 24 + 15 + 54 + 10 + group (numHw) + group (numVirt) + 2
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
    source.repaint();
    repaint();
}

void InputStripView::updateMeters (float dt)
{
    meter.update (dt);
    if ((++statusTick & 15) == 0)
    {
        const bool needsDriver = strip.source.kind == IoTarget::Kind::Virtual;
        sourceStatus.setText (strip.source.kind == IoTarget::Kind::None ? juce::String()
                              : needsDriver ? juce::String ("no driver - use a cable")
                              : ctx.engine.getStreamStatus (strip.source, true), juce::dontSendNotification);
    }
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
    sourceStatus.setBounds (r.removeFromTop (15));
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

    output.onClick = [this] { IoMenus::showOutputMenu (output, ctx, index, [this] { refresh(); }); };
    output.setTooltip ("Select output device for this bus");
    sel.setTooltip ("SEL: the input faders set the level of each input on " + busCode (i)
                    + " (separate from the other buses). With monitoring enabled in Settings > General, "
                      "this bus is also heard on the monitor output.");
    styleSmallLabel (outputStatus);
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
        outputStatus.setColour (juce::Label::textColourId, monitorShown ? theme::learn : theme::textFaint);
        tick = 15;
        repaint();
    }
    if ((++tick & 15) == 0)
    {
        if (monitorShown)
            outputStatus.setText ("monitoring " + busCode (monSrc), juce::dontSendNotification);
        else
        outputStatus.setText (bus.output.kind == IoTarget::Kind::None ? juce::String()
                              : bus.output.kind == IoTarget::Kind::Virtual ? juce::String ("no driver - use a cable")
                              : ctx.engine.getStreamStatus (bus.output, false), juce::dontSendNotification);
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

int StripArea::defaultSplit() const
{
    // Both panes get the same (widest possible) strip width. If even the narrowest strips do not
    // fit, each pane gets a share proportional to what it needs, so both scroll alike.
    const int avail = getWidth() - kDividerWidth;
    const float s = scaleFor (getHeight() - kCaptionHeight);
    auto fits = [&] (int w) { return (float) (inputsWidth (w) + outputsWidth (w)) * s <= (float) avail; };
    int w = kMaxStripWidth;
    while (w > kMinStripWidth && ! fits (w)) --w;
    int inW;
    if (fits (w))
        inW = juce::roundToInt ((float) inputsWidth (w) * s);
    else
        inW = juce::roundToInt ((float) avail * (float) inputsWidth (kMinStripWidth)
                                / (float) (inputsWidth (kMinStripWidth) + outputsWidth (kMinStripWidth)));
    return inW + kDividerWidth / 2;
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
    // Strips stretch to fill the pane (up to kMaxStripWidth); below kMinStripWidth the pane scrolls.
    float s = scaleFor (paneHeight);
    auto contentWidth = [&] (int w) { return outputs ? outputsWidth (w) : inputsWidth (w); };
    auto fit = [&]
    {
        int w = kMaxStripWidth;
        while (w > kMinStripWidth && (float) contentWidth (w) * s > (float) paneWidth) --w;
        return w;
    };
    int w = fit();
    if ((float) contentWidth (w) * s > (float) paneWidth) // scrolls: the scroll bar takes some height
    {
        s = scaleFor (paneHeight - StripPane::kScrollBar);
        w = fit();
    }
    const bool scrolls = (float) contentWidth (w) * s > (float) paneWidth + 0.5f;
    const int h = juce::roundToInt ((float) (paneHeight - (scrolls ? StripPane::kScrollBar : 0)) / s);
    pane.setContentSize (juce::jmax (contentWidth (w), juce::roundToInt ((float) paneWidth / s)), h, s);

    auto place = [&] (Section& sec, int x, int count)
    {
        sec.x = x;
        sec.width = groupWidth (sec, count, w);
        return x + sec.width;
    };
    if (! outputs)
        place (inSec, kPad, (int) inStrips.size());
    else
        place (virtSec, place (hwSec, kPad, numHw) + kBusGroupGap, numVirt);
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

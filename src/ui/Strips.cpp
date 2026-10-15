#include "Strips.h"

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

void StripHeader::mouseUp (const juce::MouseEvent& e)
{
    if (e.getNumberOfClicks() >= 2) name.showEditor();
    else ctx.select (selectIndex);
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
      eq (c, *strip.eq.on, "EQ", theme::accentIn),
      gate (c, *strip.gateOn, "GATE", theme::accentIn),
      comp (c, *strip.compOn, "COMP", theme::accentIn),
      denoise (c, *strip.dnOn, "DENOISE", theme::good),
      mono (c, *strip.mono, "MONO", theme::good),
      solo (c, *strip.solo, "SOLO", theme::solo),
      mute (c, *strip.mute, "MUTE", theme::mute),
      meter (strip.meter)
{
    for (auto* comp : std::initializer_list<juce::Component*> { &header, &source, &sourceStatus, &trim, &pan, &eq, &gate,
                                                               &this->comp, &denoise, &mono, &solo, &mute, &meter, &dbLabel })
        addAndMakeVisible (comp);

    for (int b = 0; b < kNumBuses; ++b)
    {
        routes.push_back (std::make_unique<Toggle> (c, *strip.route[(size_t) b], busCode (b), theme::busColour (b)));
        addAndMakeVisible (*routes.back());
    }

    source.onClick = [this] { IoMenus::showInputMenu (source, ctx, index, [this] { refresh(); }); };
    source.setTooltip ("Select input source (hardware, Windows device, virtual input or loopback)");
    styleSmallLabel (sourceStatus);
    denoise.setTooltip ("RNNoise denoiser (adds 10 ms latency in the default mode, 48 kHz only)");
    addMouseListener (this, true);

    dbPoll.v = this;
    setFaderTarget (-1);
    ParamPoller::attach (&dbPoll);
    refresh();
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
    static int tick = 0;
    if ((++tick & 15) == 0)
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

    // Routing groups: hardware outputs A1-A5 and virtual outputs B1-B3
    auto group = [&g] (juce::Rectangle<float> area, juce::Colour colour)
    {
        g.setColour (colour.withAlpha (0.07f));
        g.fillRoundedRectangle (area, 6.0f);
        g.setColour (colour.withAlpha (0.28f));
        g.drawRoundedRectangle (area, 6.0f, 1.0f);
    };
    group (hwGroup, theme::accentHw);
    group (virtGroup, theme::accentVirt);
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
    r.removeFromTop (6);
    layoutRow (r.removeFromTop (20), { &eq, &gate, &comp });
    r.removeFromTop (3);
    denoise.setBounds (r.removeFromTop (20));
    r.removeFromTop (8);

    // A1 A2 A3 / A4 A5 in one group, B1 B2 B3 in a separate group below
    auto hw = r.removeFromTop (20 + 3 + 20 + 6).reduced (0, 3);
    hwGroup = hw.toFloat().expanded (3.0f, 3.0f).reduced (0.0f, 0.5f);
    hw = hw.reduced (3, 0);
    layoutRow (hw.removeFromTop (20), { routes[0].get(), routes[1].get(), routes[2].get() }, 3);
    hw.removeFromTop (3);
    auto rowA2 = hw.removeFromTop (20);
    const int cell = (rowA2.getWidth() - 6) / 3;
    routes[3]->setBounds (rowA2.removeFromLeft (cell));
    rowA2.removeFromLeft (3);
    routes[4]->setBounds (rowA2.removeFromLeft (cell));
    r.removeFromTop (6);
    auto virt = r.removeFromTop (20 + 6).reduced (0, 3);
    virtGroup = virt.toFloat().expanded (3.0f, 3.0f).reduced (0.0f, 0.5f);
    layoutRow (virt.reduced (3, 0), { routes[5].get(), routes[6].get(), routes[7].get() }, 3);
    r.removeFromTop (8);

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
      header (c, kNumInputs + i, busCode (i), theme::busColour (i),
              [this] { return bus.name; }, [this] (juce::String n) { bus.name = n; }),
      eq (c, *bus.eq.on, "EQ", theme::busColour (i)),
      lim (c, *bus.limOn, "LIM", theme::busColour (i)),
      mono (c, *bus.mono, "MONO", theme::good),
      sel (c, *bus.sel, "SEL", theme::learn),
      mute (c, *bus.mute, "MUTE", theme::mute),
      fader (c, *bus.gain, theme::busColour (i)),
      meter (bus.meter)
{
    for (auto* comp : std::initializer_list<juce::Component*> { &header, &output, &outputStatus, &eq, &lim, &mono, &sel, &mute,
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
    const bool sel = ctx.selected == kNumInputs + index;
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
    r.removeFromTop (6);
    layoutRow (r.removeFromTop (20), { &eq, &lim });
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
} // namespace hydra

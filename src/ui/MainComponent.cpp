#include "MainComponent.h"

namespace hydra
{
//==============================================================================
RecorderMini::RecorderMini (UiContext& c)
    : ctx (c), rec (c, *c.engine.recorder.record, "REC", theme::mute), play (c, *c.engine.recorder.play, "PLAY", theme::good)
{
    addAndMakeVisible (rec);
    addAndMakeVisible (play);
    rec.setTooltip ("Record the selected B bus (details: click the time)");
    play.setTooltip ("Play the loaded file / last recording");
    setTooltip ("Recorder - click to show it below");
}

void RecorderMini::resized()
{
    auto r = getLocalBounds().reduced (5, 5);
    rec.setBounds (r.removeFromLeft (50));
    r.removeFromLeft (4);
    play.setBounds (r.removeFromLeft (54));
}

void RecorderMini::mouseUp (const juce::MouseEvent& e)
{
    if (e.mouseWasClicked()) ctx.select (kRecorderSelect);
}

void RecorderMini::update()
{
    const auto& r = ctx.engine.recorder;
    const bool recording = r.isRecording(), playing = r.isPlaying();
    juce::String t;
    const double secs = recording ? r.getRecordSeconds() : playing ? r.getPlaySeconds() : 0.0;
    const int whole = (int) secs;
    t << (recording ? busCode (r.getRecordSourceBus()) + "  " : playing ? juce::String ("PLAY  ") : juce::String ("REC  "))
      << juce::String (whole / 60).paddedLeft ('0', 2) << ":" << juce::String (whole % 60).paddedLeft ('0', 2);
    if (t != shown || (recording || playing) != shownActive)
    {
        shown = t;
        shownActive = recording || playing;
        repaint();
    }
}

void RecorderMini::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    const bool sel = ctx.selected == kRecorderSelect;
    g.setColour (theme::panel2);
    g.fillRoundedRectangle (r, 8.0f);
    if (sel)
    {
        g.setColour (theme::mute.withAlpha (0.5f));
        g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, 1.0f);
    }
    const auto& rc = ctx.engine.recorder;
    g.setColour (rc.isRecording() ? theme::mute : rc.isPlaying() ? theme::good : theme::textDim);
    g.setFont (theme::mono (12.5f));
    g.drawText (shown, getLocalBounds().withTrimmedLeft (118).withTrimmedRight (8), juce::Justification::centred);
}

//==============================================================================
void IconButton::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    const auto c = r.getCentre();

    if (isWindowControl())
    {
        auto* window = findParentComponentOfClass<juce::DocumentWindow>();
        const bool isClose = kind == Kind::close;
        if (hover || down)
        {
            g.setColour (isClose ? theme::mute.withAlpha (down ? 0.7f : 0.9f) : theme::raised.brighter (down ? 0.15f : 0.0f));
            g.fillRect (r);
        }
        const bool active = window == nullptr || window->isActiveWindow();
        const auto glyph = isClose && hover ? juce::Colours::white : (active || hover ? theme::text : theme::textDim);
        g.setColour (glyph);
        const float s = 5.0f;
        if (isClose)
        {
            juce::Path p;
            p.startNewSubPath (c.x - s, c.y - s); p.lineTo (c.x + s, c.y + s);
            p.startNewSubPath (c.x + s, c.y - s); p.lineTo (c.x - s, c.y + s);
            g.strokePath (p, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        else if (kind == Kind::minimise)
            g.fillRoundedRectangle (c.x - s, c.y - 0.6f, s * 2.0f, 1.3f, 0.6f);
        else if (window != nullptr && window->isFullScreen())
        {
            // restore: two overlapping frames
            g.drawRoundedRectangle (c.x - s + 2.0f, c.y - s, s * 2.0f - 2.0f, s * 2.0f - 2.0f, 1.5f, 1.1f);
            g.setColour (hover || down ? theme::raised.brighter (down ? 0.15f : 0.0f) : theme::panel);
            g.fillRoundedRectangle (c.x - s - 0.5f, c.y - s + 1.5f, s * 2.0f - 1.0f, s * 2.0f - 1.0f, 1.5f);
            g.setColour (glyph);
            g.drawRoundedRectangle (c.x - s, c.y - s + 2.0f, s * 2.0f - 2.0f, s * 2.0f - 2.0f, 1.5f, 1.1f);
        }
        else
            g.drawRoundedRectangle (c.x - s, c.y - s, s * 2.0f, s * 2.0f, 1.8f, 1.1f);
        return;
    }

    // Round-cornered chip like the other top bar buttons
    auto chip = r.reduced (0.5f);
    g.setColour (down ? theme::raised.brighter (0.15f) : hover ? theme::raised.brighter (0.07f) : theme::raised);
    g.fillRoundedRectangle (chip, 7.0f);
    g.setColour (theme::outline);
    g.drawRoundedRectangle (chip, 7.0f, 1.0f);

    const float rad = juce::jmin (r.getWidth(), r.getHeight()) * 0.27f;
    if (kind == Kind::gear)
    {
        g.setColour (hover ? theme::text : theme::textDim);
        juce::Path p;
        const int teeth = 8;
        for (int i = 0; i < teeth; ++i)
        {
            const float a = juce::MathConstants<float>::twoPi * (float) i / (float) teeth;
            juce::Path tooth;
            tooth.addRoundedRectangle (-rad * 0.22f, -rad * 1.18f, rad * 0.44f, rad * 0.5f, rad * 0.08f);
            p.addPath (tooth, juce::AffineTransform::rotation (a).translated (c.x, c.y));
        }
        p.addEllipse (c.x - rad * 0.86f, c.y - rad * 0.86f, rad * 1.72f, rad * 1.72f);
        p.setUsingNonZeroWinding (true);
        g.fillPath (p);
        g.setColour (down ? theme::raised.brighter (0.15f) : hover ? theme::raised.brighter (0.07f) : theme::raised);
        g.fillEllipse (c.x - rad * 0.36f, c.y - rad * 0.36f, rad * 0.72f, rad * 0.72f);
    }
    else // power
    {
        g.setColour (hover ? theme::mute : theme::textDim);
        juce::Path p;
        p.addCentredArc (c.x, c.y + rad * 0.08f, rad * 0.95f, rad * 0.95f, 0.0f, 0.75f, juce::MathConstants<float>::twoPi - 0.75f, true);
        p.startNewSubPath (c.x, c.y - rad * 1.15f);
        p.lineTo (c.x, c.y - rad * 0.15f);
        g.strokePath (p, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}

//==============================================================================
TopBar::TopBar (UiContext& c) : ctx (c), recorder (c)
{
    addAndMakeVisible (recorder);
    for (auto* chip : { &midiMap, &learn })
    {
        addAndMakeVisible (chip);
        chip->showArrow = false;
        chip->centred = true;
    }
    for (auto* b : { &settings, &power, &minimiseBtn, &maximiseBtn, &closeBtn })
        addAndMakeVisible (b);
    // Normally Windows handles these clicks (native caption buttons); this covers the few pixels
    // at the window edge that are reported as client area for resizing.
    auto window = [this] { return findParentComponentOfClass<juce::DocumentWindow>(); };
    minimiseBtn.onClick = [window] { if (auto* w = window()) w->minimiseButtonPressed(); };
    maximiseBtn.onClick = [window] { if (auto* w = window()) w->maximiseButtonPressed(); };
    closeBtn.onClick    = [window] { if (auto* w = window()) w->closeButtonPressed(); };
    learn.colour = theme::learn;
    learn.setTooltip ("Learn mode: click a control, then move a MIDI control to assign it. Esc to leave.");
    midiMap.setTooltip ("Show and edit all MIDI assignments");
    settings.setTooltip ("Settings: audio engine, MIDI, plugins, monitoring, tray");
    power.setTooltip ("Shut down Hydra Mixer");
}

auto TopBar::windowControlAt (juce::Point<float> p) const -> WindowControlKind
{
    const auto* window = findParentComponentOfClass<juce::DocumentWindow>();
    const bool maximised = window != nullptr && window->isFullScreen();

    // Keep a few pixels at the top / right edge free, so Windows can offer its resize border there.
    if (! maximised && (p.y < 5.0f || p.x > (float) getWidth() - 5.0f))
        return WindowControlKind::client;

    if (minimiseBtn.getBounds().toFloat().contains (p)) return WindowControlKind::minimise;
    if (maximiseBtn.getBounds().toFloat().contains (p)) return WindowControlKind::maximise;
    if (closeBtn.getBounds().toFloat().contains (p))    return WindowControlKind::close;

    for (auto* c : getChildren())
        if (c->isVisible() && c->getBounds().toFloat().contains (p))
            return WindowControlKind::client;

    return WindowControlKind::caption; // logo, status and empty space drag the window
}

void TopBar::update()
{
    auto& e = ctx.engine;
    juce::String s;
    if (auto* d = e.getMasterDevice())
        s << d->getTypeName() << "  ·  " << d->getName() << "  ·  " << (int) e.getSampleRate() << " Hz  ·  "
          << d->getCurrentBufferSizeSamples() << " smp  ·  " << juce::String (e.getMasterLatencyMs(), 1) << " ms I/O";
    else
        s << "Audio engine stopped - open Settings";

    // DSP load: the engine value reacts instantly to peaks, which makes the display jitter.
    // Smooth it here (~0.5 s time constant at 30 Hz) and refresh the number only 4x per second.
    cpuSmooth += 0.07f * (e.getCpuLoad() - cpuSmooth);
    float newCpu = cpu;
    if (++cpuTick >= 8) { cpuTick = 0; newCpu = cpuSmooth; }
    const auto act = ctx.midi.activity.load();
    bool dirty = s != status || std::abs (newCpu - cpu) > 0.002f;
    if (act != lastActivity) { lastActivity = act; midiGlow = 1.0f; dirty = true; }
    else if (midiGlow > 0.0f) { midiGlow = juce::jmax (0.0f, midiGlow - 0.12f); dirty = true; }
    status = s;
    cpu = newCpu;

    if (learn.active != ctx.learnMode)
    {
        learn.active = ctx.learnMode;
        learn.text = ctx.learnMode ? "Learning..." : "MIDI Learn";
        learn.repaint();
    }
    if (dirty) repaint (statusArea);
    recorder.update();
}

void TopBar::paint (juce::Graphics& g)
{
    auto r = getLocalBounds();
    g.setColour (theme::panel);
    g.fillRect (r);
    g.setColour (theme::outline.withAlpha (0.6f));
    g.fillRect (r.removeFromBottom (1));

    // Logo
    auto logo = r.removeFromLeft (200).reduced (16, 0);
    juce::ColourGradient grad (theme::accentIn, (float) logo.getX(), 0, theme::accentHw, (float) logo.getX() + 26.0f, 0, false);
    for (int i = 0; i < 3; ++i)
    {
        const float h = i == 1 ? 22.0f : 15.0f;
        g.setGradientFill (grad);
        g.fillRoundedRectangle ((float) logo.getX() + (float) i * 9.0f, (float) logo.getCentreY() + 11.0f - h, 5.0f, h, 2.5f);
    }
    g.setColour (theme::text);
    g.setFont (theme::font (17.0f, true));
    g.drawText ("HYDRA", logo.withTrimmedLeft (34), juce::Justification::centredLeft);
    g.setColour (theme::textDim);
    g.setFont (theme::font (17.0f));
    g.drawText ("MIXER", logo.withTrimmedLeft (98), juce::Justification::centredLeft);

    // Status
    auto st = statusArea;
    g.setColour (theme::panel2);
    g.fillRoundedRectangle (st.toFloat(), 8.0f);
    auto inner = st.reduced (12, 0);

    auto cpuArea = inner.removeFromRight (150);
    g.setColour (theme::textDim);
    g.setFont (theme::font (12.0f));
    g.drawText ("DSP", cpuArea.removeFromLeft (30), juce::Justification::centredLeft);
    auto barR = cpuArea.removeFromLeft (70).withSizeKeepingCentre (70, 6).toFloat();
    g.setColour (theme::bg);
    g.fillRoundedRectangle (barR, 3.0f);
    g.setColour (cpu < 0.5f ? theme::good : cpu < 0.8f ? theme::solo : theme::mute);
    g.fillRoundedRectangle (barR.withWidth (barR.getWidth() * juce::jlimit (0.0f, 1.0f, cpu)), 3.0f);
    g.setColour (theme::text);
    g.drawText (juce::String (cpu * 100.0f, 1) + "%", cpuArea, juce::Justification::centredRight);

    auto midiArea = inner.removeFromRight (64);
    g.setColour (theme::learn.withAlpha (0.15f + 0.85f * midiGlow));
    g.fillEllipse (midiArea.getX() + 6.0f, (float) midiArea.getCentreY() - 4.0f, 8.0f, 8.0f);
    g.setColour (theme::textDim);
    g.drawText ("MIDI", midiArea.withTrimmedLeft (20), juce::Justification::centredLeft);

    g.setColour (ctx.engine.getMasterDevice() != nullptr ? theme::good : theme::mute);
    g.fillEllipse ((float) inner.getX(), (float) inner.getCentreY() - 4.0f, 8.0f, 8.0f);
    g.setColour (theme::text);
    g.setFont (theme::font (13.0f));
    g.drawText (status, inner.withTrimmedLeft (16), juce::Justification::centredLeft);
}

void TopBar::resized()
{
    auto r = getLocalBounds().withTrimmedBottom (1);
    // Window controls: full height at the right edge, like a native title bar.
    closeBtn.setBounds (r.removeFromRight (46));
    maximiseBtn.setBounds (r.removeFromRight (46));
    minimiseBtn.setBounds (r.removeFromRight (46));
    r.removeFromRight (6);

    r = r.reduced (12, 10);
    power.setBounds (r.removeFromRight (36));
    r.removeFromRight (6);
    settings.setBounds (r.removeFromRight (36));
    r.removeFromRight (10);
    midiMap.setBounds (r.removeFromRight (96));
    r.removeFromRight (8);
    learn.setBounds (r.removeFromRight (110));
    r.removeFromRight (12);
    recorder.setBounds (r.removeFromRight (220));
    r.removeFromRight (12);
    statusArea = r.withLeft (204);
}

//==============================================================================
PanelHost::PanelHost()
{
    closeButton.showArrow = false;
    closeButton.centred = true;
    closeButton.onClick = [this] { close(); };
    addAndMakeVisible (closeButton);
    setVisible (false);
}

void PanelHost::show (std::unique_ptr<juce::Component> c, const juce::String& t, juce::Colour col)
{
    content = std::move (c);
    title = t;
    colour = col;
    addAndMakeVisible (*content);
    setSize (content->getWidth() + 2, content->getHeight() + 38);
    setVisible (true);
    toFront (false);
    resized();
    repaint();
}

void PanelHost::close()
{
    content.reset();
    setVisible (false);
}

void PanelHost::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    juce::DropShadow (juce::Colours::black.withAlpha (0.6f), 18, {}).drawForRectangle (g, getLocalBounds());
    g.setColour (theme::panel);
    g.fillRoundedRectangle (r, 10.0f);
    g.setColour (colour.withAlpha (0.7f));
    g.drawRoundedRectangle (r.reduced (0.5f), 10.0f, 1.2f);
    g.setColour (colour);
    g.setFont (theme::font (13.0f, true));
    g.drawText (title, getLocalBounds().removeFromTop (36).reduced (14, 0), juce::Justification::centredLeft);
}

void PanelHost::resized()
{
    closeButton.setBounds (getWidth() - 36, 7, 26, 24);
    if (content != nullptr)
        content->setBounds (getLocalBounds().withTrimmedTop (36).reduced (1, 0).withTrimmedBottom (2));
}

//==============================================================================
MainComponent::MainComponent (Engine& engine, MidiMapper& midi, Hotkeys& hotkeys, Session& session)
    : ctx { engine, midi, hotkeys, &session }, top (ctx), inspector (ctx)
{
    setOpaque (true);
    ctx.select = [this] (int i) { select (i); };
    ctx.learnModeChanged = [this] { top.update(); repaint(); };
    ctx.armHotkey = [this] (Param& p)
    {
        hotkeyTarget = &p;
        ctx.hotkeys.setSuspended (true); // so already used global shortcuts can be captured too
        ctx.hotkeys.setMouseCapture ([this] (const juce::KeyPress& k) { assignCapturedKey (k); }); // mouse buttons 3-5
        // Called from a popup-menu callback: take the focus once the menu has fully gone,
        // otherwise the key presses can end up nowhere.
        juce::Component::SafePointer<MainComponent> safe (this);
        juce::MessageManager::callAsync ([safe]
        {
            if (safe == nullptr || safe->hotkeyTarget == nullptr) return;
            if (auto* top = safe->getTopLevelComponent()) top->toFront (true);
            safe->grabKeyboardFocus();
        });
        showToast ("Press a key or mouse button for  " + p.name + "   (Ctrl/Alt/F-keys/mouse = global,  Esc = cancel)", 30.0);
    };

    addAndMakeVisible (top);
    for (int i = 0; i < kNumInputs; ++i)
    {
        inStrips.push_back (std::make_unique<InputStripView> (ctx, i));
        addAndMakeVisible (*inStrips.back());
    }
    for (int b = 0; b < kNumBuses; ++b)
    {
        busStrips.push_back (std::make_unique<BusStripView> (ctx, b));
        addAndMakeVisible (*busStrips.back());
    }
    addAndMakeVisible (inspector);
    addChildComponent (panelHost);
    ctx.openPanel = [this] (std::unique_ptr<juce::Component> content, juce::String title, juce::Colour colour)
    {
        panelHost.show (std::move (content), title, colour);
        panelHost.setTopLeftPosition (juce::jlimit (8, getWidth() - panelHost.getWidth() - 8, (getWidth() - panelHost.getWidth()) / 2),
                                      inspector.getY() - panelHost.getHeight() - 10);
    };
    inspector.onScanRequested = [this] { openSettings (2); };
    ctx.openEqWindow = [this] (int i) { openEqWindow (i); };
    ctx.prefsChanged = [this] { if (onPrefsChanged) onPrefsChanged(); };
    ctx.shutdown = [this]
    {
        // Async: the caller may be a button inside a window that is destroyed while quitting.
        juce::Component::SafePointer<MainComponent> safe (this);
        juce::MessageManager::callAsync ([safe] { if (safe != nullptr && safe->onShutdown) safe->onShutdown(); });
    };
    inspector.showChannel (0);

    top.settings.onClick = [this] { openSettings(); };
    top.power.onClick = [this]
    {
        juce::Component::SafePointer<MainComponent> safe (this);
        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                          .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                          .withTitle ("Shut down Hydra Mixer?")
                                          .withMessage ("All audio routing stops until Hydra Mixer is started again.")
                                          .withButton ("Shut down")
                                          .withButton ("Cancel")
                                          .withAssociatedComponent (this),
                                      [safe] (int result)
                                      {
                                          if (result == 1 && safe != nullptr && safe->ctx.shutdown) safe->ctx.shutdown();
                                      });
    };
    top.midiMap.onClick = [this] { openMidiMap(); };
    top.learn.onClick = [this]
    {
        ctx.learnMode = ! ctx.learnMode;
        if (! ctx.learnMode) ctx.midi.cancelLearn();
        ctx.learnModeChanged();
    };

    midi.onLearned = [this] (Param* p)
    {
        if (p == nullptr) return;
        const auto maps = ctx.midi.getMappingsFor (p);
        showToast ("MIDI mapped:  " + p->name + "  <-  " + (maps.empty() ? juce::String() : maps.back().describe()), 2.5);
    };

    engine.addChangeListener (this);
    setWantsKeyboardFocus (true);
    setSize (1640, 960);
    startTimerHz (30);
}

void MainComponent::showToast (const juce::String& text, double seconds)
{
    toast = text;
    toastUntil = juce::Time::getMillisecondCounterHiRes() + seconds * 1000.0;
    repaint();
}

void MainComponent::finishHotkeyCapture()
{
    hotkeyTarget = nullptr;
    ctx.hotkeys.setMouseCapture (nullptr);
    ctx.hotkeys.setSuspended (false);
    toastUntil = 0;
    repaint();
}

MainComponent::~MainComponent()
{
    stopTimer();
    if (hotkeyTarget != nullptr) { ctx.hotkeys.setMouseCapture (nullptr); ctx.hotkeys.setSuspended (false); }
    ctx.engine.removeChangeListener (this);
    ctx.midi.onLearned = nullptr;
    settingsWindow.reset();
    midiWindow.reset();
    eqWindow.reset();
}

void MainComponent::select (int index)
{
    if (index == ctx.selected) return;
    ctx.selected = index;
    panelHost.close();
    inspector.showChannel (index);
    for (auto& s : inStrips) s->repaint();
    for (auto& s : busStrips) s->repaint();
    top.repaint();

    // An open EQ window follows the selected channel.
    if (eqWindow != nullptr && eqWindow->isVisible() && index != kRecorderSelect)
        openEqWindow (index, false);
}

auto MainComponent::windowControlAt (juce::Point<float> p) const -> WindowControlKind
{
    if (top.getBounds().toFloat().contains (p))
        return top.windowControlAt (p - top.getPosition().toFloat());
    return WindowControlKind::client;
}

void MainComponent::openEqWindow (int index, bool bringToFront)
{
    if (index < 0 || index >= kRecorderSelect) return;
    auto* editor = new EqEditor (ctx, index);
    const auto title = "Hydra Mixer - EQ " + (index < kNumInputs ? "IN " + juce::String (index + 1) : busCode (index - kNumInputs));
    if (eqWindow == nullptr)
        eqWindow = std::make_unique<ToolWindow> (title, editor, editor->getWidth(), editor->getHeight(), juce::DocumentWindow::allButtons);
    else
        eqWindow->replaceContent (editor, title);
    eqWindow->setResizeLimits (760, 520, 8000, 4000);
    eqWindow->setVisible (true);
    if (bringToFront) eqWindow->toFront (true);
}

void MainComponent::refreshAll()
{
    for (auto& s : inStrips) s->refresh();
    for (auto& s : busStrips) s->refresh();
    inspector.repaint();
}

void MainComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refreshAll();
    top.update();
}

void MainComponent::openSettings (int tab)
{
    if (settingsWindow == nullptr)
    {
        auto* panel = new SettingsPanel (ctx);
        panel->onEngineChanged = [this] { refreshAll(); };
        panel->onSessionReplaced = [this]
        {
            refreshAll();
            panelHost.close();
            inspector.reload();
            showToast ("Settings loaded", 2.5);
        };
        settingsWindow = std::make_unique<ToolWindow> ("Hydra Mixer - Settings", panel, 1040, 680);
    }
    if (auto* p = dynamic_cast<SettingsPanel*> (settingsWindow->getContentComponent()))
        p->showTab (tab);
    settingsWindow->setVisible (true);
    settingsWindow->toFront (true);
}

void MainComponent::openMidiMap()
{
    if (midiWindow == nullptr)
        midiWindow = std::make_unique<ToolWindow> ("Hydra Mixer - MIDI Mapping", new MidiMapPanel (ctx), 1180, 560);
    midiWindow->setVisible (true);
    midiWindow->toFront (true);
}

void MainComponent::assignCapturedKey (const juce::KeyPress& k)
{
    auto* p = hotkeyTarget;
    if (p == nullptr) return;
    ctx.hotkeys.assign (*p, k);
    finishHotkeyCapture();
    const bool global = Hotkeys::wantsGlobal (k);
    bool registered = ! global;
    for (auto& b : ctx.hotkeys.getBindings())
        if (b.param == p) registered = registered || b.global;
    showToast (p->name + "  <-  " + Hotkeys::describe (k)
               + (! global ? "   (works while Hydra has focus)"
                           : registered ? "   (global)" : "   (taken by another app - only while Hydra has focus)"), 3.0);
}

bool MainComponent::keyPressed (const juce::KeyPress& k)
{
    if (hotkeyTarget != nullptr)
    {
        if (k == juce::KeyPress::escapeKey)
        {
            finishHotkeyCapture();
            return true;
        }
        assignCapturedKey (k);
        return true;
    }
    if (k == juce::KeyPress::escapeKey && panelHost.isOpen() && ! ctx.learnMode)
    {
        panelHost.close();
        return true;
    }
    if (k == juce::KeyPress::escapeKey && ctx.learnMode)
    {
        ctx.learnMode = false;
        ctx.midi.cancelLearn();
        ctx.learnModeChanged();
        return true;
    }
    return ctx.hotkeys.handleLocalKey (k);
}

void MainComponent::meterFrame (double t)
{
    const double dt = t - lastMeterFrame;
    if (dt < 1.0 / 66.0) return; // high-refresh displays: stay at ~60 Hz
    lastMeterFrame = t;
    const float d = (float) juce::jlimit (0.0, 0.2, dt);
    for (auto& s : inStrips) s->updateMeters (d);
    for (auto& s : busStrips) s->updateMeters (d);
}

void MainComponent::timerCallback()
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    inspector.updateMeters();
    top.update();

    // SEL: exclusive, and the input faders switch to the sends of the selected bus.
    ctx.engine.enforceExclusiveSelect();
    const int sb = ctx.engine.getSelectedBus(), mb = ctx.engine.getMonitorBus();
    if (sb != shownSelBus)
    {
        shownSelBus = sb;
        for (auto& s : inStrips) s->setFaderTarget (sb);
    }
    if (sb != shownCaptionSel || mb != shownMonBus)
    {
        shownCaptionSel = sb;
        shownMonBus = mb;
        repaint (0, top.getBottom(), getWidth(), 22);
    }

    if (toastUntil > 0 && now > toastUntil)
    {
        if (hotkeyTarget != nullptr) finishHotkeyCapture();
        toastUntil = 0;
        repaint();
    }
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);

    // Divider between inputs and output section.
    if (! inStrips.empty() && ! busStrips.empty())
    {
        const int y0 = top.getBottom() + 2, y1 = busStrips.front()->getBottom() + 4;
        const float x = (float) (inStrips.back()->getRight() + busStrips.front()->getX()) * 0.5f;
        g.setColour (theme::outline.withAlpha (0.7f));
        g.fillRect (juce::Rectangle<float> (x - 0.5f, (float) y0 + 8.0f, 1.0f, (float) (y1 - y0) - 16.0f));
    }

    // Section captions
    if (! inStrips.empty())
    {
        g.setFont (theme::font (11.0f, true));
        g.setColour (theme::accentIn.withAlpha (0.8f));
        g.drawText ("INPUTS", inStrips.front()->getX() + 6, top.getBottom() + 4, 200, 16, juce::Justification::centredLeft);
        if (shownSelBus >= 0)
        {
            const int x = inStrips.front()->getX() + 66;
            g.setColour (theme::busColour (shownSelBus));
            g.drawText ("FADERS = LEVEL ON " + busCode (shownSelBus)
                            + (ctx.engine.getMonitorBus() >= 0 ? "   (monitoring on " + busCode (ctx.engine.getMonitorBus()) + ")" : juce::String()),
                        x, top.getBottom() + 4, 420, 16, juce::Justification::centredLeft);
        }
        g.setColour (theme::accentHw.withAlpha (0.8f));
        g.drawText ("HARDWARE OUTPUTS  A1-A5", busStrips.front()->getX() + 6, top.getBottom() + 4, 300, 16, juce::Justification::centredLeft);
        g.setColour (theme::accentVirt.withAlpha (0.8f));
        g.drawText ("VIRTUAL  B1-B3", busStrips[kNumHwBuses]->getX() + 6, top.getBottom() + 4, 200, 16, juce::Justification::centredLeft);
    }

    if (ctx.learnMode)
    {
        g.setColour (theme::learn.withAlpha (0.9f));
        g.setFont (theme::font (12.0f, true));
        g.drawText ("MIDI LEARN MODE - click a control, then move/press a MIDI control.  Esc = exit",
                    getLocalBounds().withTop (top.getBottom() + 4).withHeight (16).withTrimmedRight (16),
                    juce::Justification::centredRight);
    }

}

void MainComponent::paintOverChildren (juce::Graphics& g)
{
    // Over the strips, otherwise the toast is hidden behind them.
    if (toastUntil > 0)
    {
        auto r = juce::Rectangle<int> (0, 0, juce::jmin (720, getWidth() - 40), 40)
                     .withCentre ({ getWidth() / 2, inspector.getY() - 30 });
        g.setColour (theme::raised.withAlpha (0.97f));
        g.fillRoundedRectangle (r.toFloat(), 10.0f);
        g.setColour (theme::learn);
        g.drawRoundedRectangle (r.toFloat(), 10.0f, 1.0f);
        g.setColour (theme::text);
        g.setFont (theme::font (13.5f, true));
        g.drawFittedText (toast, r.reduced (14, 0), juce::Justification::centred, 1, 0.8f);
    }
}

void MainComponent::resized()
{
    auto r = getLocalBounds();
    top.setBounds (r.removeFromTop (54));
    r.removeFromTop (22);
    inspector.setBounds (r.removeFromBottom (236).reduced (8, 4));

    auto strips = r.reduced (8, 0);
    const int gap = 14, sectionGap = 30; // wider gap: inputs | output section
    const int total = kNumInputs + kNumBuses;
    const int w = (strips.getWidth() - gap - sectionGap) / total;
    for (auto& s : inStrips) s->setBounds (strips.removeFromLeft (w));
    strips.removeFromLeft (sectionGap);
    for (int b = 0; b < kNumBuses; ++b)
    {
        if (b == kNumHwBuses) strips.removeFromLeft (gap);
        busStrips[(size_t) b]->setBounds (strips.removeFromLeft (w));
    }
}
} // namespace hydra

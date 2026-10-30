#include "MainComponent.h"
#include "state/Session.h"

namespace hydra
{
//==============================================================================
RecorderMini::RecorderMini (UiContext& c)
    : ctx (c), rec (c, *c.engine.recorder.record, "REC", theme::mute, Toggle::Icon::record),
      play (c, *c.engine.recorder.play, "PLAY", theme::good, Toggle::Icon::playPause),
      stop (c, *c.engine.recorder.stop, "STOP", theme::text, Toggle::Icon::stop)
{
    addAndMakeVisible (rec);
    addAndMakeVisible (play);
    addAndMakeVisible (stop);
    rec.setTooltip ("Record the selected B bus (details: click the time)");
    play.setTooltip ("Play / pause the loaded file or the last recording");
    stop.setTooltip ("Stop recording / playback and go back to the start");
    setTooltip ("Recorder - click to show it below");
}

void RecorderMini::resized()
{
    auto r = getLocalBounds().reduced (5, 5);
    for (auto* b : { &rec, &play, &stop })
    {
        b->setBounds (r.removeFromLeft (32));
        r.removeFromLeft (3);
    }
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
    g.drawText (shown, getLocalBounds().withTrimmedLeft (110).withTrimmedRight (8), juce::Justification::centred);
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
    const auto chipColour = down ? theme::raised.brighter (0.15f) : hover ? theme::raised.brighter (0.07f) : theme::raised;
    const bool isLit = ! lit.isTransparent();
    g.setColour (isLit ? chipColour.interpolatedWith (lit, 0.18f) : chipColour);
    g.fillRoundedRectangle (chip, 7.0f);
    g.setColour (isLit ? lit.withAlpha (0.8f) : theme::outline);
    g.drawRoundedRectangle (chip, 7.0f, isLit ? 1.3f : 1.0f);

    const float rad = juce::jmin (r.getWidth(), r.getHeight()) * 0.27f;
    const auto glyph = isLit ? lit : hover ? theme::text : theme::textDim;
    if (kind == Kind::wrench)
    {
        // Open-end wrench, diagonal: round head top right with a notch, handle to the bottom left.
        g.setColour (glyph);
        const float s = rad * 1.25f;
        juce::Path handle;
        handle.addRoundedRectangle (-s * 0.17f, -s * 0.2f, s * 0.34f, s * 1.25f, s * 0.17f);
        g.fillPath (handle, juce::AffineTransform::rotation (juce::MathConstants<float>::pi * 0.25f).translated (c.x, c.y));
        const auto head = c.translated (s * 0.42f, -s * 0.42f);
        g.fillEllipse (head.x - s * 0.42f, head.y - s * 0.42f, s * 0.84f, s * 0.84f);
        juce::Path notch;   // jaw opening, cut out with the chip colour
        notch.addRectangle (-s * 0.14f, -s * 0.5f, s * 0.28f, s * 0.5f);
        g.setColour (isLit ? chipColour.interpolatedWith (lit, 0.18f) : chipColour);
        g.fillPath (notch, juce::AffineTransform::rotation (juce::MathConstants<float>::pi * 0.25f).translated (head.x, head.y));
    }
    else if (kind == Kind::cassette)
    {
        g.setColour (glyph);
        auto body = juce::Rectangle<float> (rad * 3.0f, rad * 2.0f).withCentre (c);
        g.drawRoundedRectangle (body, rad * 0.3f, 1.4f);
        const float rr = rad * 0.33f;
        for (float dx : { -rad * 0.62f, rad * 0.62f })
            g.drawEllipse (c.x + dx - rr, c.y - rad * 0.28f - rr, rr * 2.0f, rr * 2.0f, 1.3f);
        g.drawLine (c.x - rad * 0.62f, c.y - rad * 0.28f - rr, c.x + rad * 0.62f, c.y - rad * 0.28f - rr, 1.1f);
        juce::Path base;
        base.startNewSubPath (body.getX() + rad * 0.55f, body.getBottom());
        base.lineTo (body.getX() + rad * 0.8f, body.getBottom() - rad * 0.55f);
        base.lineTo (body.getRight() - rad * 0.8f, body.getBottom() - rad * 0.55f);
        base.lineTo (body.getRight() - rad * 0.55f, body.getBottom());
        g.strokePath (base, juce::PathStrokeType (1.2f));
    }
    else if (kind == Kind::gear)
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
        g.setColour (hover || isLit ? theme::mute : theme::textDim);
        juce::Path p;
        p.addCentredArc (c.x, c.y + rad * 0.08f, rad * 0.95f, rad * 0.95f, 0.0f, 0.75f, juce::MathConstants<float>::twoPi - 0.75f, true);
        p.startNewSubPath (c.x, c.y - rad * 1.15f);
        p.lineTo (c.x, c.y - rad * 0.15f);
        g.strokePath (p, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}

//==============================================================================
DspMeter::DspMeter()
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setTooltip ("DSP load of the audio engine (share of each audio block's time). Click: what uses it, per channel and plugin");
}

void DspMeter::setLoad (float l)
{
    if (std::abs (l - load) <= 0.002f) return;
    load = l;
    repaint();
}

void DspMeter::paint (juce::Graphics& g)
{
    auto r = getLocalBounds();
    if (hover)
    {
        g.setColour (theme::raised.brighter (0.07f));
        g.fillRoundedRectangle (r.toFloat().reduced (0.0f, 6.0f), 6.0f);
    }
    r.reduce (6, 0);
    g.setColour (hover ? theme::text : theme::textDim);
    g.setFont (theme::font (12.0f));
    g.drawText ("DSP", r.removeFromLeft (30), juce::Justification::centredLeft);
    auto bar = r.removeFromLeft (70).withSizeKeepingCentre (70, 6).toFloat();
    g.setColour (theme::bg);
    g.fillRoundedRectangle (bar, 3.0f);
    g.setColour (load < 0.5f ? theme::good : load < 0.8f ? theme::solo : theme::mute);
    g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, load)), 3.0f);
    g.setColour (theme::text);
    g.drawText (juce::String (load * 100.0f, 1) + "%", r, juce::Justification::centredRight);
}

//==============================================================================
StatusLink::StatusLink()
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setTooltip ("Audio engine: click to open its settings (driver, device, sample rate, buffer)");
}

void StatusLink::setStatus (const juce::String& t, bool isRunning, bool learnMode)
{
    if (t == text && isRunning == running && learnMode == learn) return;
    text = t;
    running = isRunning;
    learn = learnMode;
    repaint();
}

void StatusLink::paint (juce::Graphics& g)
{
    auto r = getLocalBounds();
    if (hover && ! learn)
    {
        g.setColour (theme::raised.brighter (0.07f));
        g.fillRoundedRectangle (r.toFloat().reduced (0.0f, 6.0f), 6.0f);
    }
    r.removeFromLeft (6);
    g.setColour (running ? theme::good : theme::mute);
    g.fillEllipse ((float) r.getX(), (float) r.getCentreY() - 4.0f, 8.0f, 8.0f);
    g.setColour (learn ? theme::learn : hover ? juce::Colours::white : theme::text);
    g.setFont (theme::font (13.0f, learn));
    g.drawFittedText (text, r.withTrimmedLeft (16).withTrimmedRight (6), juce::Justification::centredLeft, 1, 0.9f);
}

//==============================================================================
TopBar::TopBar (UiContext& c) : ctx (c), recorder (c)
{
    addAndMakeVisible (recorder);
    addChildComponent (cassette);
    addAndMakeVisible (dsp);
    addAndMakeVisible (statusLink);
    for (auto* b : { &tools, &settings, &power, &minimiseBtn, &maximiseBtn, &closeBtn })
        addAndMakeVisible (b);
    // Normally Windows handles these clicks (native caption buttons); this covers the few pixels
    // at the window edge that are reported as client area for resizing.
    auto window = [this] { return findParentComponentOfClass<juce::DocumentWindow>(); };
    minimiseBtn.onClick = [window] { if (auto* w = window()) w->minimiseButtonPressed(); };
    maximiseBtn.onClick = [window] { if (auto* w = window()) w->maximiseButtonPressed(); };
    closeBtn.onClick    = [window] { if (auto* w = window()) w->closeButtonPressed(); };
    cassette.onClick = [this] { ctx.select (kRecorderSelect); };
    tools.setTooltip ("Tools: Scene Editor, MIDI Learn, MIDI Map, Latency Finder, Sync Delay, DSP Load, Recorder, Restart Audio Engine");
    cassette.setTooltip ("Recorder - click to show it below");
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

    return WindowControlKind::caption; // logo, status background and empty space drag the window
}

void TopBar::update()
{
    auto& e = ctx.engine;
    juce::String s;
    if (auto* d = e.getMasterDevice())
        s << d->getTypeName() << "  ·  " << d->getName() << "  ·  " << (int) e.getSampleRate() << " Hz  ·  "
          << d->getCurrentBufferSizeSamples() << " smp  ·  " << juce::String (e.getMasterLatencyMs(), 1) << " ms I/O";
    else
        s << "Audio engine stopped - click to open the settings";
    if (ctx.learnMode)
        s = "MIDI LEARN MODE - click a control, then move / press a MIDI control.   Esc = exit";
    statusLink.setStatus (s, e.getMasterDevice() != nullptr, ctx.learnMode);

    // DSP load: the engine value reacts instantly to peaks, which makes the display jitter.
    // Smooth it here (~0.5 s time constant at 30 Hz) and refresh the number only 4x per second.
    cpuSmooth += 0.07f * (e.getCpuLoad() - cpuSmooth);
    if (++cpuTick >= 8) { cpuTick = 0; cpu = cpuSmooth; }
    dsp.setLoad (cpu);

    const auto act = ctx.midi.activity.load();
    bool dirty = false;
    if (act != lastActivity) { lastActivity = act; midiGlow = 1.0f; dirty = true; }
    else if (midiGlow > 0.0f) { midiGlow = juce::jmax (0.0f, midiGlow - 0.12f); dirty = true; }
    if (dirty && ! midiArea.isEmpty()) repaint (midiArea);

    tools.setLit (ctx.learnMode ? theme::learn : juce::Colours::transparentBlack);
    const auto& rc = e.recorder;
    cassette.setLit (rc.isRecording() ? theme::mute : rc.isPlaying() ? theme::good : juce::Colours::transparentBlack);
    recorder.update();
}

void TopBar::paint (juce::Graphics& g)
{
    auto r = getLocalBounds();
    g.setColour (theme::panel);
    g.fillRect (r);
    g.setColour (theme::outline.withAlpha (0.6f));
    g.fillRect (r.removeFromBottom (1));

    // Logo: same as the tray icon and the exe icon (resources/icon.png); only the icon when space is short.
    const auto logo = logoArea;
    theme::drawLogo (g, { (float) logo.getX(), (float) logo.getCentreY() - 11.0f, 23.0f, 22.0f });
    if (! compactLogo)
    {
        g.setColour (theme::text);
        const auto bold = theme::font (17.0f, true);
        g.setFont (bold);
        g.drawText ("HYDRA", logo.withTrimmedLeft (34), juce::Justification::centredLeft);
        g.setColour (theme::textDim);
        g.setFont (theme::font (17.0f));
        g.drawText ("MIXER", logo.withTrimmedLeft (34 + juce::GlyphArrangement::getStringWidthInt (bold, "HYDRA") + 8),
                    juce::Justification::centredLeft);
    }

    // Status background; the text and the DSP meter are child components, the MIDI dot is drawn here.
    if (statusArea.getWidth() > 0)
    {
        g.setColour (theme::panel2);
        g.fillRoundedRectangle (statusArea.toFloat(), 8.0f);
    }
    if (! midiArea.isEmpty())
    {
        g.setFont (theme::font (12.0f));
        g.setColour (theme::learn.withAlpha (0.15f + 0.85f * midiGlow));
        g.fillEllipse (midiArea.getX() + 6.0f, (float) midiArea.getCentreY() - 4.0f, 8.0f, 8.0f);
        g.setColour (theme::textDim);
        g.drawText ("MIDI", midiArea.withTrimmedLeft (20), juce::Justification::centredLeft);
    }
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
    r.removeFromRight (6);
    tools.setBounds (r.removeFromRight (36));
    r.removeFromRight (12);

    // What is left is shared by the logo, the recorder and the status. Narrow windows give up the
    // full recorder first (cassette button instead), then the logo text, then the status text.
    constexpr int kLogoFull = 190, kLogoIcon = 34, kRecFull = 220, kRecIcon = 36, kDspW = 162, kMidiW = 64, kTextMin = 150;
    const int avail = juce::jmax (0, r.getWidth());
    const int statusWanted = kDspW + kMidiW + kTextMin;
    const bool fullRecorder = avail >= kLogoFull + 12 + kRecFull + 12 + statusWanted;
    const int recW = fullRecorder ? kRecFull : kRecIcon;
    compactLogo = avail < kLogoFull + 12 + recW + 12 + statusWanted;

    logoArea = r.removeFromLeft (juce::jmin (r.getWidth(), compactLogo ? kLogoIcon : kLogoFull)).withTrimmedLeft (4);
    r.removeFromLeft (juce::jmin (r.getWidth(), 8));
    recorder.setVisible (fullRecorder);
    cassette.setVisible (! fullRecorder);
    const auto recArea = r.removeFromRight (juce::jmin (r.getWidth(), recW));
    if (fullRecorder) recorder.setBounds (recArea);
    else              cassette.setBounds (recArea);
    r.removeFromRight (juce::jmin (r.getWidth(), 12));

    // Status: DSP meter at the right end (visible as long as there is room for it), MIDI activity
    // next to it, the engine status (click = audio settings) gets the rest.
    statusArea = r;
    auto inner = statusArea.reduced (6, 0);
    const bool showDsp = inner.getWidth() >= kDspW;
    dsp.setVisible (showDsp);
    if (showDsp) dsp.setBounds (inner.removeFromRight (kDspW));
    midiArea = inner.getWidth() >= kMidiW + 40 ? inner.removeFromRight (kMidiW) : juce::Rectangle<int>();
    statusLink.setVisible (inner.getWidth() >= 40);
    statusLink.setBounds (inner);
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
    : ctx { engine, midi, hotkeys, &session }, undo (engine.params), top (ctx), strips (ctx), inspector (ctx)
{
    ctx.undo = &undo;
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
    // Inputs and outputs scroll sideways separately when they do not fit (many channels or a large zoom).
    addAndMakeVisible (strips);
    shownLayout = engine.getLayoutVersion();
    addAndMakeVisible (inspector);
    addChildComponent (panelHost);
    ctx.openPanel = [this] (std::unique_ptr<juce::Component> content, juce::String title, juce::Colour colour)
    {
        panelHost.show (std::move (content), title, colour);
        const int w = logicalWidth();
        panelHost.setTopLeftPosition (juce::jlimit (8, w - panelHost.getWidth() - 8, (w - panelHost.getWidth()) / 2),
                                      inspector.getY() - panelHost.getHeight() - 10);
    };
    ctx.addChannel = [this] (int group) { addChannel (group); };
    ctx.changeOrder = [this] { beginReorder(); };
    ctx.openRoomMeasurement = [this] { openRoomMeasurement(); };
    // Async: the buttons that call these are destroyed when the strips are rebuilt.
    strips.onReorderDone = [this]
    {
        juce::Component::SafePointer<MainComponent> safe (this);
        juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->finishReorder(); });
    };
    strips.onReorderDiscard = [this]
    {
        juce::Component::SafePointer<MainComponent> safe (this);
        juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->discardReorder(); });
    };
    ctx.removeChannel = [this] (int sel) { removeChannel (sel); };
    inspector.onScanRequested = [this] { openSettings (3); }; // Plugins tab
    ctx.openEqWindow = [this] (int i) { openEqWindow (i); };
    ctx.openDevicePicker = [this] (int i) { openDevicePicker (i); };
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
        dialog::confirm (this, dialog::Kind::danger, "Shut down Hydra Mixer?",
                         "All audio routing stops until Hydra Mixer is started again.", "Shut down",
                         [safe] { if (safe != nullptr && safe->ctx.shutdown) safe->ctx.shutdown(); });
    };
    top.tools.onClick = [this] { showToolsMenu(); };
    top.dsp.onClick = [this] { openDspView(); };
    top.statusLink.onClick = [this] { openSettings (1); };   // Audio Engine tab

    midi.onLearned = [this] (Param* p)
    {
        if (p == nullptr) return;
        const auto maps = ctx.midi.getMappingsFor (p);
        showToast ("MIDI mapped:  " + p->name + "  <-  " + (maps.empty() ? juce::String() : maps.back().describe()), 2.5);
    };

    // Scenes recalled by MIDI / shortcut (or in the editor): short confirmation.
    session.scenes.onRecalled = [this] (int i)
    {
        auto& bank = ctx.session->scenes;
        showToast ("Scene " + juce::String (i + 1) + ":  " + bank.get (i).name, 1.5);
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
    if (ctx.session != nullptr) ctx.session->scenes.onRecalled = nullptr;
    sceneBar.reset();
    settingsWindow.reset();
    midiWindow.reset();
    eqWindow.reset();
    latencyWindow.reset();
    dspWindow.reset();
    syncWindow.reset();
    deviceWindow.reset();
    roomWindow.reset();
}

void MainComponent::select (int index)
{
    if (index == ctx.selected) return;
    ctx.selected = index;
    panelHost.close();
    inspector.showChannel (index);
    strips.repaintStrips();
    top.repaint();

    // An open EQ window follows the selected channel.
    if (eqWindow != nullptr && eqWindow->isVisible() && index != kRecorderSelect)
        openEqWindow (index, false);
}

auto MainComponent::windowControlAt (juce::Point<float> p) const -> WindowControlKind
{
    if (top.getBoundsInParent().toFloat().contains (p))
        return top.windowControlAt (top.getLocalPoint (this, p));
    return WindowControlKind::client;
}

bool MainComponent::isChannelShown (int sel) const
{
    if (sel == kRecorderSelect) return true;
    if (sel < 0 || sel > kRecorderSelect) return false;
    return sel < kMaxInputs ? ctx.engine.isInputActive (sel) : ctx.engine.isBusActive (sel - kMaxInputs);
}

void MainComponent::rebuildStrips()
{
    shownLayout = ctx.engine.getLayoutVersion();
    strips.rebuild();
    shownSelBus = -2; // re-apply the SEL fader target to the new strips
    resized();
    if (! isChannelShown (ctx.selected))
    {
        for (int i = 0; i < kMaxInputs; ++i)
            if (ctx.engine.isInputActive (i)) { select (i); break; }
    }
    if (eqWindow != nullptr)
        if (auto* ed = dynamic_cast<EqEditor*> (eqWindow->getContentComponent()); ed != nullptr && ! isChannelShown (ed->getSelectIndex()))
            eqWindow.reset();
}

void MainComponent::scrollToChannel (int sel)
{
    strips.scrollToChannel (sel);
}

void MainComponent::addChannel (int group)
{
    auto& e = ctx.engine;
    const int idx = group == 0 ? e.addInput() : e.addBus (group == 2);
    if (idx < 0)
    {
        showToast (group == 0 ? "All " + juce::String (kMaxInputs) + " inputs are in use"
                   : group == 1 ? "All " + juce::String (kMaxHwBuses) + " hardware outputs are in use"
                                : "All " + juce::String (kMaxVirtBuses) + " virtual outputs are in use", 2.5);
        return;
    }
    rebuildStrips();
    const int sel = group == 0 ? idx : busSelectIndex (idx);
    select (sel);
    scrollToChannel (sel);
}

void MainComponent::removeChannel (int sel)
{
    if (! isChannelShown (sel) || sel == kRecorderSelect) return;
    const bool isInput = sel < kMaxInputs;
    const auto name = isInput ? ctx.engine.inputs[(size_t) sel].name : ctx.engine.buses[(size_t) (sel - kMaxInputs)].name;
    juce::Component::SafePointer<MainComponent> safe (this);
    dialog::confirm (this, dialog::Kind::danger, "Remove " + channelCode (sel) + "  \"" + name + "\"?",
                     juce::String ("Its settings, plugins, MIDI mappings and shortcuts are reset")
                         + (isInput ? "." : ", and no input is routed to it any more.")
                         + " The channel can be added again with the + button.",
                     "Remove", [safe, sel]
    {
        if (safe == nullptr) return;
        auto& self = *safe;
        auto& e = self.ctx.engine;
        // Leave the channel first: the inspector / EQ window hold references to its plugins and parameters.
        if (self.ctx.selected == sel)
        {
            int other = -1;
            for (int i = 0; i < kMaxInputs && other < 0; ++i)
                if (i != sel && e.isInputActive (i)) other = i;
            self.select (other >= 0 ? other : busSelectIndex (0));
        }
        if (self.eqWindow != nullptr)
            if (auto* ed = dynamic_cast<EqEditor*> (self.eqWindow->getContentComponent()); ed != nullptr && ed->getSelectIndex() == sel)
                self.eqWindow.reset();

        const auto reset = sel < kMaxInputs ? e.removeInput (sel) : e.removeBus (sel - kMaxInputs);
        for (auto* p : reset)
        {
            if (self.ctx.midi.hasMapping (p)) self.ctx.midi.removeMappingsFor (p);
            if (self.ctx.hotkeys.getKeyFor (p).isValid()) self.ctx.hotkeys.remove (p);
        }
        self.undo.forget (reset);
        self.rebuildStrips();
        self.showToast (channelCode (sel) + " removed", 2.0);
    });
}

void MainComponent::beginReorder()
{
    panelHost.close();
    if (ctx.learnMode)
    {
        ctx.learnMode = false;
        ctx.midi.cancelLearn();
        ctx.learnModeChanged();
    }
    strips.beginReorder();
    grabKeyboardFocus(); // Esc = discard
}

void MainComponent::discardReorder()
{
    if (! strips.isReordering()) return;
    rebuildStrips(); // back to the real order
}

void MainComponent::finishReorder()
{
    if (! strips.isReordering()) return;
    auto& e = ctx.engine;
    const auto order = strips.getShownOrder();
    std::vector<std::pair<Param*, Param*>> moves;
    for (int g = 0; g < 3; ++g)
        for (auto& m : e.reorderPlan ((Engine::Group) g, order[(size_t) g]))
            moves.push_back (m);
    if (moves.empty())
    {
        rebuildStrips();
        showToast ("Order unchanged", 1.5);
        return;
    }

    const int maps = ctx.midi.countMappings (moves), keys = ctx.hotkeys.countBindings (moves);
    if (maps + keys == 0)
    {
        applyReorder (order, true);
        return;
    }

    // Assignments exist on moved channels: they can follow their channel or stay with the position.
    // An example with the first MIDI mapping on a moved strip makes the choice concrete.
    juce::String example;
    auto nameOf = [&e] (int g, int slot) { return g == 0 ? e.inputs[(size_t) slot].name : e.buses[(size_t) slot].name; };
    auto codeOf = [] (int g, int slot) { return g == 0 ? channelCode (slot) : busCode (slot); };
    const auto mappings = ctx.midi.getMappings();
    for (int g = 0; g < 3 && example.isEmpty(); ++g)
    {
        const auto& shown = order[(size_t) g];   // shown[k] moves to the k-th slot in use
        auto used = shown;
        std::sort (used.begin(), used.end());
        for (size_t k = 0; k < shown.size() && example.isEmpty(); ++k)
        {
            const int from = shown[k], to = used[k];
            if (from == to) continue;
            const auto prefix = g == 0 ? "in" + juce::String (from + 1) + "." : "bus." + busCode (from) + ".";
            int into = from;   // channel that takes over slot 'from'
            for (size_t j = 0; j < used.size(); ++j)
                if (used[j] == from) into = shown[j];
            for (auto& m : mappings)
                if (m.param != nullptr && m.param->id.startsWith (prefix))
                {
                    example << "Example: " << m.describe() << " now controls " << codeOf (g, from) << " \"" << nameOf (g, from) << "\".\n"
                            << "Follow: it controls \"" << nameOf (g, from) << "\" at its new place " << codeOf (g, to) << ".\n"
                            << "Keep positions: it controls " << codeOf (g, from) << ", which will be \"" << nameOf (g, into) << "\".";
                    break;
                }
        }
    }

    juce::String what;
    if (maps > 0) what << maps << (maps == 1 ? " MIDI assignment" : " MIDI assignments");
    if (keys > 0) what << (maps > 0 ? " and " : "") << keys << (keys == 1 ? " keyboard shortcut" : " keyboard shortcuts");

    juce::Component::SafePointer<MainComponent> safe (this);
    using Role = dialog::Button::Role;
    dialog::show (this, dialog::Kind::question, "Apply the new order",
                  what + (maps + keys == 1 ? " belongs" : " belong") + " to channels that move.\n\n"
                  "Follow the channels: your controller keeps controlling the same channels "
                  "(e.g. the fader for your mic still moves the mic).\n"
                  "Keep positions: each control stays on its position (fader 1 controls "
                  "whatever channel is first now).\n\n" + example,
                  { { "Follow the channels", Role::primary }, { "Keep positions", Role::normal }, { "Back", Role::cancel } },
                  [safe, order] (int result)
                  {
                      if (safe == nullptr || ! safe->strips.isReordering()) return;
                      if (result == 0) safe->applyReorder (order, true);
                      else if (result == 1) safe->applyReorder (order, false);
                      // Back (or cancelled): stay in Change Order mode.
                  });
}

void MainComponent::applyReorder (const std::array<std::vector<int>, 3>& order, bool follow)
{
    auto& e = ctx.engine;
    // Where the selected channel / the EQ window's channel goes.
    auto newIndex = [&order] (int sel)
    {
        if (sel < 0 || sel >= kRecorderSelect) return sel;
        const int g = sel < kMaxInputs ? 0 : isHwBus (sel - kMaxInputs) ? 1 : 2;
        const int slot = g == 0 ? sel : sel - kMaxInputs;
        auto used = order[(size_t) g];
        std::sort (used.begin(), used.end());
        for (size_t k = 0; k < used.size(); ++k)
            if (order[(size_t) g][k] == slot) return g == 0 ? used[k] : busSelectIndex (used[k]);
        return sel;
    };
    int eqChannel = -1;
    if (eqWindow != nullptr)
        if (auto* ed = dynamic_cast<EqEditor*> (eqWindow->getContentComponent())) eqChannel = ed->getSelectIndex();
    eqWindow.reset();       // holds references to the old slot's EQ
    panelHost.close();

    // One group after the other: a routing parameter can move with its input and then with its bus,
    // so assignments are remapped step by step too.
    int assigned = 0;
    for (int g = 0; g < 3; ++g)
    {
        const auto plan = e.reorderPlan ((Engine::Group) g, order[(size_t) g]);
        assigned += ctx.midi.countMappings (plan) + ctx.hotkeys.countBindings (plan);
        if (! e.reorderChannels ((Engine::Group) g, order[(size_t) g])) continue;
        if (follow)
        {
            ctx.midi.remapParams (plan);
            ctx.hotkeys.remapParams (plan);
        }
    }
    undo.clear(); // the values moved between channels: undo must not move them back one by one
    deviceWindow.reset(); // its channel number may now belong to another channel

    const int sel = newIndex (ctx.selected);
    rebuildStrips();
    ctx.selected = -1;
    select (sel);
    inspector.reload();
    if (eqChannel >= 0) openEqWindow (newIndex (eqChannel), false);
    showToast (assigned == 0 ? juce::String ("New order applied")
               : follow ? juce::String ("New order applied  -  MIDI assignments and shortcuts follow their channels")
                        : juce::String ("New order applied  -  MIDI assignments and shortcuts stay on their positions"), 3.0);
}

void MainComponent::setUiScale (float scale)
{
    scale = juce::jlimit (0.5f, 3.0f, scale);
    if (std::abs (scale - uiScale) < 0.001f) return;
    uiScale = scale;
    resized();
    repaint();
}

void MainComponent::fontScaleChanged()
{
    strips.rebuild();
    shownSelBus = -2;
    inspector.reload();
    panelHost.close();
    resized();
    repaint();
}

void MainComponent::openEqWindow (int index, bool bringToFront)
{
    if (index < 0 || index >= kRecorderSelect) return;
    auto* editor = new EqEditor (ctx, index);
    if (! isChannelShown (index)) return;
    const auto title = "Hydra Mixer - EQ " + channelCode (index);
    if (eqWindow == nullptr)
        makeToolWindow (eqWindow, title, editor, editor->getWidth(), editor->getHeight(), juce::DocumentWindow::allButtons);
    else
        eqWindow->replaceContent (editor, title);
    eqWindow->setResizeLimits (760, 520, 8000, 4000);
    eqWindow->setVisible (true);
    if (bringToFront)
    {
        eqWindow->toFront (true);
        editor->grabKeyboardFocus(); // Ctrl+Z / Ctrl+Y work right away
    }
}

void MainComponent::refreshAll()
{
    if (ctx.engine.getLayoutVersion() != shownLayout)
        rebuildStrips();
    strips.refreshStrips();
    inspector.repaint();
}

void MainComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refreshAll();
    top.update();
}

void MainComponent::toggleLearnMode()
{
    ctx.learnMode = ! ctx.learnMode;
    if (! ctx.learnMode) ctx.midi.cancelLearn();
    ctx.learnModeChanged();
}

void MainComponent::toggleSceneEditor()
{
    if (sceneBar != nullptr)
    {
        sceneBar.reset();
    }
    else if (ctx.session != nullptr)
    {
        sceneBar = std::make_unique<SceneSidebar> (ctx, ctx.session->scenes);
        juce::Component::SafePointer<MainComponent> safe (this);
        // Async: the close button belongs to the sidebar that is destroyed.
        sceneBar->onClose = [safe] { juce::MessageManager::callAsync ([safe] { if (safe != nullptr && safe->sceneBar != nullptr) safe->toggleSceneEditor(); }); };
        sceneBar->onMessage = [this] (const juce::String& text) { showToast (text, 2.5); };
        addAndMakeVisible (*sceneBar);
    }
    resized();
    repaint();
}

void MainComponent::restartAudioEngine()
{
    // Closing the master releases the graph and with it every stream; secondary drivers close when their last
    // stream goes and are opened again by the new graph.
    const auto cfg = ctx.engine.getMasterConfig();
    const auto err = ctx.engine.setMasterConfig (cfg);
    refreshAll();
    showToast (err.isEmpty() ? juce::String ("Audio engine restarted") : "Restart failed: " + err, 3.5);
}

void MainComponent::showToolsMenu()
{
    // All tools in one place (the top bar only has room for the wrench).
    juce::Component::SafePointer<MainComponent> safe (this);
    auto run = [safe] (void (MainComponent::*fn)()) { return [safe, fn] { if (safe != nullptr) (safe.getComponent()->*fn)(); }; };
    juce::PopupMenu m;
    m.addSectionHeader ("Tools");
    m.addItem ("Scene Editor", true, sceneBar != nullptr, run (&MainComponent::toggleSceneEditor));
    m.addSeparator();
    m.addItem ("MIDI Learn Mode", true, ctx.learnMode, run (&MainComponent::toggleLearnMode));
    m.addItem ("MIDI Map...", run (&MainComponent::openMidiMap));
    m.addSeparator();
    m.addItem ("Latency Finder...", run (&MainComponent::openLatencyFinder));
    m.addItem ("Sync Delay...", run (&MainComponent::openSyncDelay));
    m.addItem ("DSP Load...", run (&MainComponent::openDspView));
    m.addItem ("Room Measurement...", run (&MainComponent::openRoomMeasurement));
    m.addItem ("Recorder", [safe] { if (safe != nullptr) safe->select (kRecorderSelect); });
    m.addSeparator();
    m.addItem ("Restart Audio Engine", run (&MainComponent::restartAudioEngine));
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&top.tools).withMinimumWidth (200));
}

void MainComponent::openSettings (int tab)
{
    if (settingsWindow == nullptr)
    {
        auto* panel = new SettingsPanel (ctx);
        panel->onEngineChanged = [this] { refreshAll(); };
        panel->onSessionReplaced = [this]
        {
            undo.clear();
            refreshAll();
            panelHost.close();
            inspector.reload();
            showToast ("Settings loaded", 2.5);
        };
        makeToolWindow (settingsWindow, "Hydra Mixer - Settings", panel, 1040, 680);
    }
    if (auto* p = dynamic_cast<SettingsPanel*> (settingsWindow->getContentComponent()))
        p->showTab (tab);
    settingsWindow->setVisible (true);
    settingsWindow->toFront (true);
}

void MainComponent::openMidiMap()
{
    if (midiWindow == nullptr)
        makeToolWindow (midiWindow, "Hydra Mixer - MIDI Mapping", new MidiMapPanel (ctx), 1180, 560);
    midiWindow->setVisible (true);
    midiWindow->toFront (true);
}

void MainComponent::openLatencyFinder()
{
    if (latencyWindow == nullptr)
        makeToolWindow (latencyWindow, "Hydra Mixer - Latency Finder", new LatencyView (ctx), 1000, 640);
    latencyWindow->setResizeLimits (640, 360, 8000, 4000);
    latencyWindow->setVisible (true);
    latencyWindow->toFront (true);
}

void MainComponent::openDspView()
{
    if (dspWindow == nullptr)
        makeToolWindow (dspWindow, "Hydra Mixer - DSP Load", new DspView (ctx), 1100, 720);
    dspWindow->setResizeLimits (760, 480, 8000, 4000);
    dspWindow->setVisible (true);
    dspWindow->toFront (true);
}

void MainComponent::openSyncDelay()
{
    if (syncWindow == nullptr)
        makeToolWindow (syncWindow, "Hydra Mixer - Sync Delay", new SyncDelayView (ctx), 1180, 640);
    syncWindow->setResizeLimits (720, 360, 8000, 4000);
    syncWindow->setVisible (true);
    syncWindow->toFront (true);
}

void MainComponent::openRoomMeasurement()
{
    if (roomWindow == nullptr)
        makeToolWindow (roomWindow, "Hydra Mixer - Room Measurement", new RoomView (ctx), 1320, 860, juce::DocumentWindow::allButtons);
    roomWindow->setResizeLimits (1100, 720, 8000, 4000);
    roomWindow->setVisible (true);
    roomWindow->toFront (true);
}

void MainComponent::openDevicePicker (int channel)
{
    if (channel < 0 || channel >= kRecorderSelect) return;
    auto* picker = new DevicePicker (ctx, channel);
    picker->onChanged = [this] { refreshAll(); };
    juce::Component::SafePointer<MainComponent> safe (this);
    juce::Component::SafePointer<DevicePicker> self (picker);
    // Async: the call comes from inside the window that is destroyed. Only if it still shows this picker.
    picker->onClose = [safe, self]
    {
        juce::MessageManager::callAsync ([safe, self]
        {
            if (safe != nullptr && self != nullptr && safe->deviceWindow != nullptr
                && safe->deviceWindow->getContentComponent() == self.getComponent())
                safe->deviceWindow.reset();
        });
    };
    const auto title = DevicePicker::windowTitle (ctx, channel);
    if (deviceWindow == nullptr)
        makeToolWindow (deviceWindow, title, picker, picker->getWidth(), picker->getHeight());
    else
        deviceWindow->replaceContent (picker, title);
    deviceWindow->setResizeLimits (640, 420, 8000, 4000);
    deviceWindow->setVisible (true);
    deviceWindow->toFront (true);
    picker->grabKeyboardFocus();
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

bool MainComponent::handleUndoKey (const juce::KeyPress& k)
{
    // Ctrl+Z / Ctrl+Y: undo / redo the last change anywhere in the mixer (EQ, faders, routing, ...).
    juce::String message;
    if (! ctx.handleUndoKey (k, &message)) return false;
    showToast (message, 1.5);
    return true;
}

ToolWindow& MainComponent::makeToolWindow (std::unique_ptr<ToolWindow>& slot, const juce::String& title, juce::Component* content,
                                           int w, int h, int buttons)
{
    slot = std::make_unique<ToolWindow> (title, content, w, h, buttons);
    slot->onKey = [this] (const juce::KeyPress& k) { return handleUndoKey (k); };
    return *slot;
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
    if (strips.isReordering())
    {
        if (k == juce::KeyPress::escapeKey) { discardReorder(); return true; }
        if (k == juce::KeyPress::returnKey) { finishReorder(); return true; }
    }
    if (handleUndoKey (k))
        return true;
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
    strips.updateMeters ((float) juce::jlimit (0.0, 0.2, dt));
}

void MainComponent::timerCallback()
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    // Channels added / removed elsewhere (settings loaded, Voicemeeter import, ...)
    if (ctx.engine.getLayoutVersion() != shownLayout)
        rebuildStrips();
    inspector.updateMeters();
    top.update();

    // SEL: exclusive, and the input faders switch to the sends of the selected bus.
    ctx.engine.enforceExclusiveSelect();
    const int sb = ctx.engine.getSelectedBus(), mb = ctx.engine.getMonitorBus();
    if (sb != shownSelBus)
    {
        shownSelBus = sb;
        strips.setFaderTarget (sb);
    }
    strips.setSelInfo (sb, mb);

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
}

void MainComponent::paintOverChildren (juce::Graphics& g)
{
    // Over the strips, otherwise the toast is hidden behind them.
    if (toastUntil > 0)
    {
        g.addTransform (juce::AffineTransform::scale (uiScale));
        const int w = logicalWidth();
        auto r = juce::Rectangle<int> (0, 0, juce::jmin (720, w - 40), 40)
                     .withCentre ({ w / 2, inspector.getY() - 30 });
        g.setColour (theme::raised.withAlpha (0.97f));
        g.fillRoundedRectangle (r.toFloat(), 10.0f);
        g.setColour (theme::learn);
        g.drawRoundedRectangle (r.toFloat(), 10.0f, 1.0f);
        g.setColour (theme::text);
        g.setFont (theme::font (13.5f, true));
        g.drawFittedText (toast, r.reduced (14, 0), juce::Justification::centred, 1, 0.8f);
    }
}

int MainComponent::idealWidth() const
{
    const int logical = strips.idealWidth() + (sceneBar != nullptr ? sceneBar->getWidth() : 0);
    return (int) std::ceil ((float) logical * uiScale);
}

void MainComponent::resized()
{
    // Children are laid out in logical units and drawn scaled by uiScale.
    const auto scale = juce::AffineTransform::scale (uiScale);
    for (juce::Component* c : { (juce::Component*) &top, (juce::Component*) &strips, (juce::Component*) &inspector, (juce::Component*) &panelHost })
        c->setTransform (scale);

    auto r = juce::Rectangle<int> (0, 0, logicalWidth(), logicalHeight());
    top.setBounds (r.removeFromTop (54));
    r.removeFromTop (4);
    if (sceneBar != nullptr)
    {
        sceneBar->setTransform (scale);
        sceneBar->setBounds (r.removeFromLeft (juce::jmin (SceneSidebar::kWidth, r.getWidth() / 2)).withTrimmedBottom (4));
    }
    inspector.setBounds (r.removeFromBottom (236).reduced (8, 4));
    strips.setBounds (r);
    if (panelHost.isOpen())
        panelHost.setTopLeftPosition (juce::jlimit (8, juce::jmax (8, logicalWidth() - panelHost.getWidth() - 8), panelHost.getX()),
                                      inspector.getY() - panelHost.getHeight() - 10);
}
} // namespace hydra

#include "MainComponent.h"
#include "CableManager.h"
#include "state/VacCables.h"
#include "state/ChannelClipboard.h"
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
    rec.setTooltip ("Record the selected V bus (details: click the time)");
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
    t << (recording ? busLabel (r.getRecordSourceBus()) + "  " : playing ? juce::String ("PLAY  ") : juce::String ("REC  "))
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
        const bool hover = this->hover || nativeHover;
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
            g.setColour (hover || down ? theme::raised.brighter (down ? 0.15f : 0.0f) : theme::deep() ? theme::bg : theme::panel);
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
    setLoad (0.0f, 0.0f);
}

void DspMeter::setLoad (float l, float peak)
{
    setTooltip ("DSP load\nAverage share of real time the audio engine needs (like a DAW's CPU meter). Slowest single block: "
                + juce::String (juce::roundToInt (peak * 100.0f)) + " %. Click: what uses it, per channel and plugin");
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
    addAndMakeVisible (recorderSlot);
    recorderSlot.setInterceptsMouseClicks (false, true);
    recorderSlot.addAndMakeVisible (recorder);
    recorderSlot.addChildComponent (cassette);
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

    if (maximiseBtn.getBounds().toFloat().contains (p))
    {
        // Snap Layouts need the native button, and Windows asks this for every mouse move over it.
        const_cast<IconButton&> (maximiseBtn).setNativeHover (true);
        maximiseHover.startTimerHz (30);
        return WindowControlKind::maximise;
    }

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

    // DSP load: average over the last ~0.25 s (like a DAW's CPU meter), refreshed 4x per second.
    // The peak (single slowest callback) goes into the tooltip.
    peakSmooth = juce::jmax (e.getPeakLoad(), peakSmooth * 0.97f);
    if (++cpuTick >= 8)
    {
        cpuTick = 0;
        dsp.setLoad (cpuMeter.update (e), peakSmooth);
    }

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
    g.setColour (theme::deep() ? theme::bg : theme::panel);   // Default theme: same as the background behind strips and effects
    g.fillRect (r);
    g.setColour (theme::outline.withAlpha (0.6f));
    g.fillRect (r.removeFromBottom (1));

    // Logo: same as the tray icon and the exe icon (resources/icon.png); only the icon when space is short.
    const auto logo = logoArea;
    theme::drawLogo (g, { (float) logo.getX(), (float) logo.getCentreY() - 11.0f, 23.0f, 22.0f });
    if (logoShown > 0.01f)
    {
        // Fades and is covered from the right while the logo shrinks to its icon.
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (logo);
        const auto wide = logo.withWidth (400);
        g.setColour (theme::text.withMultipliedAlpha (logoShown));
        const auto bold = theme::font (17.0f, true);
        g.setFont (bold);
        g.drawText ("HYDRA", wide.withTrimmedLeft (34), juce::Justification::centredLeft);
        g.setColour (theme::textDim.withMultipliedAlpha (logoShown));
        g.setFont (theme::font (17.0f));
        g.drawText ("MIXER", wide.withTrimmedLeft (34 + juce::GlyphArrangement::getStringWidthInt (bold, "HYDRA") + 8),
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

namespace
{
constexpr int kBarLogoFull = 190, kBarLogoIcon = 34, kBarRecFull = 220, kBarRecIcon = 36, kBarDspW = 162, kBarMidiW = 64, kBarTextMin = 150;
constexpr double kBarGlideSeconds = 0.07;
}

void TopBar::resized()
{
    // What is left next to the buttons is shared by the logo, the recorder and the status. Narrow windows give
    // up the full recorder first (cassette button instead), then the logo text, then the status text.
    const int avail = juce::jmax (0, getWidth() - (46 * 3 + 6) - 2 * 12 - (36 * 3 + 6 + 6 + 12));
    const int statusWanted = kBarDspW + kBarMidiW + kBarTextMin;
    const bool fullRecorder = avail >= kBarLogoFull + 12 + kBarRecFull + 12 + statusWanted;
    const bool fullLogo = avail >= kBarLogoFull + 12 + (fullRecorder ? kBarRecFull : kBarRecIcon) + 12 + statusWanted;
    recorderTarget = fullRecorder ? 1.0f : 0.0f;
    logoTarget = fullLogo ? 1.0f : 0.0f;
    if (recorderShown < 0.0f || ! isShowing())
    {
        recorderShown = recorderTarget;
        logoShown = logoTarget;
    }
    layoutBar();
}

void TopBar::glideFrame (double t)
{
    if (recorderShown == recorderTarget && logoShown == logoTarget) { lastGlideFrame = 0.0; return; }
    const double dt = lastGlideFrame > 0.0 ? juce::jlimit (0.0, 0.05, t - lastGlideFrame) : 1.0 / 60.0;
    lastGlideFrame = t;
    const float k = 1.0f - (float) std::exp (-dt / kBarGlideSeconds);
    auto step = [k] (float& shown, float target)
    {
        shown += (target - shown) * k;
        if (std::abs (target - shown) < 0.01f) shown = target;
    };
    step (recorderShown, recorderTarget);
    step (logoShown, logoTarget);
    layoutBar();
    repaint();
}

void TopBar::layoutBar()
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

    // While they change size (see resized) the slot shrinks and clips, the recorder fades into the cassette:
    // one out in the first half of the way, the other in during the second.
    const int logoW = juce::roundToInt (juce::jmap (logoShown, (float) kBarLogoIcon, (float) kBarLogoFull));
    const int recW = juce::roundToInt (juce::jmap (recorderShown, (float) kBarRecIcon, (float) kBarRecFull));
    logoArea = r.removeFromLeft (juce::jmin (r.getWidth(), logoW)).withTrimmedLeft (4);
    r.removeFromLeft (juce::jmin (r.getWidth(), 8));
    const auto recArea = r.removeFromRight (juce::jmin (r.getWidth(), recW));
    recorderSlot.setBounds (recArea);
    recorder.setBounds (recArea.getWidth() - kBarRecFull, 0, kBarRecFull, recArea.getHeight());
    cassette.setBounds (recArea.getWidth() - kBarRecIcon, 0, kBarRecIcon, recArea.getHeight());
    const float recorderAlpha = juce::jlimit (0.0f, 1.0f, recorderShown * 2.0f - 1.0f);
    const float cassetteAlpha = juce::jlimit (0.0f, 1.0f, 1.0f - recorderShown * 2.0f);
    recorder.setAlpha (recorderAlpha);
    cassette.setAlpha (cassetteAlpha);
    recorder.setVisible (recorderAlpha > 0.0f);
    cassette.setVisible (cassetteAlpha > 0.0f);
    r.removeFromRight (juce::jmin (r.getWidth(), 12));

    // Status: DSP meter at the right end (visible as long as there is room for it), MIDI activity
    // next to it, the engine status (click = audio settings) gets the rest.
    statusArea = r;
    auto inner = statusArea.reduced (6, 0);
    const bool showDsp = inner.getWidth() >= kBarDspW;
    dsp.setVisible (showDsp);
    if (showDsp) dsp.setBounds (inner.removeFromRight (kBarDspW));
    midiArea = inner.getWidth() >= kBarMidiW + 40 ? inner.removeFromRight (kBarMidiW) : juce::Rectangle<int>();
    statusLink.setVisible (inner.getWidth() >= 40);
    statusLink.setBounds (inner);
}

//==============================================================================
MainComponent::MainComponent (Engine& engine, MidiMapper& midi, Hotkeys& hotkeys, Session& session)
    : ctx { engine, midi, hotkeys, &session }, undo (engine.params), top (ctx), strips (ctx), inspector (ctx)
{
    ctx.undo = &undo;
    appComponent = this;
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
    ctx.openPanel = [this] (std::unique_ptr<juce::Component> content, juce::String title) { openPanelWindow (std::move (content), title); };
    ctx.addChannel = [this] (int group) { addChannel (group); };
    ctx.changeOrder = [this] { beginReorder(); };
    ctx.openRoomMeasurement = [this] (int output) { openRoomMeasurement (output); };
    ctx.openCorrectionWindow = [this] (int i) { openCorrectionWindow (i); };
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
    ctx.cutChannel = [this] (int sel) { cutChannel (sel); };
    ctx.duplicateChannel = [this] (int sel) { duplicateChannel (sel); };
    ctx.pasteNewChannel = [this] (int group) { pasteNewChannel (group); };
    inspector.onScanRequested = [this] { openSettings (SettingsPanel::plugins); };
    ctx.openEqWindow = [this] (int i, int k) { openEqWindow (i, true, k); };
    ctx.openDevicePicker = [this] (int i) { openDevicePicker (i); };
    ctx.openCableManager = [this] { openCableManager(); };
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
    top.statusLink.onClick = [this] { openSettings (SettingsPanel::audio); };

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
    if (appComponent == this) appComponent = nullptr;
    stopTimer();
    if (hotkeyTarget != nullptr) { ctx.hotkeys.setMouseCapture (nullptr); ctx.hotkeys.setSuspended (false); }
    ctx.engine.removeChangeListener (this);
    ctx.midi.onLearned = nullptr;
    if (ctx.session != nullptr) ctx.session->scenes.onRecalled = nullptr;
    sceneBar.reset();
    sidePanel.reset();
    settingsWindow.reset();
    midiWindow.reset();
    eqWindow.reset();
    latencyWindow.reset();
    dspWindow.reset();
    syncWindow.reset();
    deviceWindow.reset();
    corrWindow.reset();
    cableWindow.reset();
}

void MainComponent::select (int index)
{
    if (index == ctx.selected) return;
    ctx.selected = index;
    inspector.showChannel (index);
    strips.repaintStrips();
    top.repaint();

    // An open EQ window follows the selected channel.
    if (eqWindow != nullptr && eqWindow->isVisible() && index != kRecorderSelect)
        openEqWindow (index, false);
    if (auto* v = correctionView(); v != nullptr && corrWindow->isVisible() && ! v->isBusy())
        v->showOutput (index);   // ignored for anything but a hardware output
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
    if (sidePanel != nullptr) sidePanel->rebuild();
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
    if (onChannelsChanged) onChannelsChanged();
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
                     "Remove", [safe, sel] { if (safe != nullptr) safe->removeChannelNow (sel); });
}

void MainComponent::removeChannelNow (int sel)
{
    removeChannelCore (sel);
    rebuildStrips();
    showToast (channelCode (sel) + " removed", 2.0);
}

void MainComponent::removeChannelCore (int sel)
{
    auto& e = ctx.engine;
    if (! isChannelShown (sel)) return;
    closePanelWindow();
    // Leave the channel first: the inspector / EQ window hold references to its plugins and parameters.
    if (ctx.selected == sel)
    {
        int other = -1;
        for (int i = 0; i < kMaxInputs && other < 0; ++i)
            if (i != sel && e.isInputActive (i)) other = i;
        select (other >= 0 ? other : busSelectIndex (0));
    }
    if (eqWindow != nullptr)
        if (auto* ed = dynamic_cast<EqEditor*> (eqWindow->getContentComponent()); ed != nullptr && ed->getSelectIndex() == sel)
            eqWindow.reset();

    const auto reset = sel < kMaxInputs ? e.removeInput (sel) : e.removeBus (sel - kMaxInputs);
    for (auto* p : reset)
    {
        if (ctx.midi.hasMapping (p)) ctx.midi.removeMappingsFor (p);
        if (ctx.hotkeys.getKeyFor (p).isValid()) ctx.hotkeys.remove (p);
    }
    undo.forget (reset);
}

void MainComponent::cutChannel (int sel)
{
    if (! isChannelShown (sel) || sel == kRecorderSelect) return;
    auto& e = ctx.engine;
    if (sel < kMaxInputs ? e.numActiveInputs() <= 1 : (isHwBus (sel - kMaxInputs) && e.numActiveBuses (false) <= 1)) return;
    clip::toClipboard (*clip::copyChannel (e, sel));
    removeChannelNow (sel);
    showToast (channelCode (sel) + " cut  -  right-click next to the strips: Paste as new channel", 3.0);
}

void MainComponent::pasteNewChannel (int group)
{
    auto x = clip::fromClipboard();
    if (x == nullptr) return;
    const int sel = clip::pasteAsNewChannel (ctx.engine, group, *x);
    if (sel < 0)
    {
        showToast (clip::canPasteAsNew (x.get(), group) ? "No free channel left in this group" : "The clipboard holds no channel of this kind", 2.5);
        return;
    }
    rebuildStrips();
    select (sel);
    scrollToChannel (sel);
}

void MainComponent::duplicateChannel (int sel)
{
    if (! isChannelShown (sel) || sel == kRecorderSelect) return;
    auto& e = ctx.engine;
    const bool isInput = sel < kMaxInputs;
    const int slot = isInput ? sel : sel - kMaxInputs;
    const int group = isInput ? 0 : isHwBus (slot) ? 1 : 2;
    auto xml = clip::copyChannel (e, sel);
    xml->setAttribute ("name", xml->getStringAttribute ("name") + " 2");
    const int copy = clip::pasteAsNewChannel (e, group, *xml);
    if (copy < 0) { showToast ("No free channel left in this group", 2.5); return; }

    // The copy goes right after its original: the channels after it move one place (with their MIDI / shortcuts).
    const int copySlot = isInput ? copy : copy - kMaxInputs;
    std::vector<int> order;
    const int first = group == 2 ? virtBusIndex (0) : 0;
    const int count = group == 0 ? kMaxInputs : group == 1 ? kMaxHwBuses : kMaxVirtBuses;
    for (int k = first; k < first + count; ++k)
        if ((isInput ? e.isInputActive (k) : e.isBusActive (k)) && k != copySlot)
        {
            order.push_back (k);
            if (k == slot) order.push_back (copySlot);
        }
    int target = copy;
    const auto plan = e.reorderPlan ((Engine::Group) group, order);
    if (e.reorderChannels ((Engine::Group) group, order))
    {
        ctx.midi.remapParams (plan);
        ctx.hotkeys.remapParams (plan);
        undo.clear();
        target = sel + 1;   // the copy now sits in the slot after the original
        auto used = order;
        std::sort (used.begin(), used.end());
        for (size_t k = 0; k < order.size(); ++k)
            if (order[k] == copySlot) target = isInput ? used[k] : busSelectIndex (used[k]);
    }
    deviceWindow.reset();
    rebuildStrips();
    ctx.selected = -1;
    select (target);
    inspector.reload();
    scrollToChannel (target);
}

void MainComponent::openPanelWindow (std::unique_ptr<juce::Component> content, const juce::String& title)
{
    // One window for the settings of all effects: it keeps its place when another effect's settings open in it.
    const int w = content->getWidth(), h = content->getHeight();
    if (panelWindow == nullptr)
    {
        auto& window = makeToolWindow (panelWindow, title, content.release(), w, h);
        window.setResizable (false, false);
        window.onKey = [this] (const juce::KeyPress& k)
        {
            if (k == juce::KeyPress::escapeKey) { closePanelWindow(); return true; }
            return handleUndoKey (k);
        };
        window.onClose = [this] { closePanelWindow(); };
    }
    else
        panelWindow->replaceContent (content.release(), title);
    panelWindow->setBackgroundColour (theme::panel);
    panelWindow->setContentComponentSize (w, h);
    panelWindow->setVisible (true);
    panelWindow->toFront (true);
}

void MainComponent::closePanelWindow()
{
    if (panelWindow == nullptr) return;
    panelWindow->setVisible (false);
    panelWindow->clearContentComponent();   // stops the panel's timer; it is created anew when opened again
}

void MainComponent::beginReorder()
{
    closePanelWindow();
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
    reorderRemoved = 0;
    rebuildStrips(); // back to the real order
}

void MainComponent::finishReorder()
{
    if (! strips.isReordering()) return;
    auto& e = ctx.engine;
    // Channels deleted with the trash can go first: the order then covers exactly the channels that stay.
    for (int sel : strips.getRemovedChannels())
    {
        removeChannelCore (sel);
        ++reorderRemoved;
    }
    strips.clearRemovedChannels();
    const auto order = strips.getShownOrder();
    std::vector<std::pair<Param*, Param*>> moves;
    for (int g = 0; g < 3; ++g)
        for (auto& m : e.reorderPlan ((Engine::Group) g, order[(size_t) g]))
            moves.push_back (m);
    if (moves.empty())
    {
        rebuildStrips();
        showToast (reorderRemoved > 0 ? juce::String (reorderRemoved) + (reorderRemoved == 1 ? " channel deleted" : " channels deleted")
                                      : juce::String ("Order unchanged"), 2.0);
        reorderRemoved = 0;
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
    auto codeOf = [] (int g, int slot) { return g == 0 ? channelCode (slot) : busLabel (slot); };
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
    const int corrChannel = correctionView() != nullptr ? correctionView()->getOutput() : -1;
    closePanelWindow();

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
    if (corrChannel >= 0) correctionView()->showOutput (newIndex (corrChannel));   // the window follows its output
    const auto deleted = reorderRemoved > 0 ? juce::String (", ") + juce::String (reorderRemoved) + (reorderRemoved == 1 ? " channel deleted" : " channels deleted")
                                            : juce::String();
    reorderRemoved = 0;
    showToast (assigned == 0 ? "New order applied" + deleted
               : follow ? "New order applied" + deleted + "  -  MIDI assignments and shortcuts follow their channels"
                        : "New order applied" + deleted + "  -  MIDI assignments and shortcuts stay on their positions", 3.0);
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
    sidePanel.reset();   // recreated by resized() with the new colours / fonts
    shownSelBus = -2;
    inspector.reload();
    closePanelWindow();
    resized();
    repaint();
}

void MainComponent::themeChanged()
{
    fontScaleChanged();   // both rebuild what captured its colours when it was created
    for (int i = juce::TopLevelWindow::getNumTopLevelWindows(); --i >= 0;)
        if (auto* w = juce::TopLevelWindow::getTopLevelWindow (i))
        {
            if (auto* doc = dynamic_cast<juce::DocumentWindow*> (w)) doc->setBackgroundColour (theme::bg);
            w->sendLookAndFeelChange();
        }
}

void MainComponent::openEqWindow (int index, bool bringToFront, int instance)
{
    if (index < 0 || index >= kRecorderSelect || ! isChannelShown (index)) return;
    auto* editor = new EqEditor (ctx, index, instance);
    const auto title = "Hydra Mixer - EQ " + channelCode (index) + (instance > 0 ? " (EQ " + juce::String (instance + 1) + ")" : juce::String());
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

CorrectionView* MainComponent::correctionView() const
{
    return corrWindow != nullptr ? dynamic_cast<CorrectionView*> (corrWindow->getContentComponent()) : nullptr;
}

void MainComponent::openCorrectionWindow (int index, bool measure)
{
    // One window for the correction and the measurement; created once (a running measurement lives in it).
    if (corrWindow == nullptr)
    {
        auto* view = new CorrectionView (ctx);
        makeToolWindow (corrWindow, "Hydra Mixer - Room Correction", view, view->getWidth(), view->getHeight(), juce::DocumentWindow::allButtons);
        corrWindow->setResizeLimits (1100, 820, 8000, 4000);
    }
    auto* view = correctionView();
    // Which output: the one asked for, else the one shown, else the selected one, else the first hardware output.
    int output = ctx.engine.correctionFor (index) != nullptr ? index : view->getOutput();
    if (output < 0 && ctx.engine.correctionFor (ctx.selected) != nullptr) output = ctx.selected;
    for (int b = 0; output < 0 && b < kMaxBuses; ++b)
        if (ctx.engine.isBusActive (b) && ctx.engine.correctionFor (kMaxInputs + b) != nullptr) output = kMaxInputs + b;
    if (! view->isBusy()) view->showOutput (output);
    view->showPage (measure ? CorrectionView::Page::wizard : CorrectionView::Page::correction);
    corrWindow->setVisible (true);
    corrWindow->toFront (true);
}

void MainComponent::stripLayoutChanged()
{
    resized();   // the side panel may come or go
    strips.resized();
    strips.repaint();
}

void MainComponent::refreshAll()
{
    if (ctx.engine.getLayoutVersion() != shownLayout)
        rebuildStrips();
    strips.refreshStrips();
    if (sidePanel != nullptr) sidePanel->refresh();
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
    m.addItem ("Room Correction...", [safe] { if (safe != nullptr) safe->openRoomMeasurement(); });
    if (ctx.session != nullptr && ctx.session->vacCables != nullptr && ctx.session->vacCables->isAvailable())
        m.addItem ("Cable Manager (debug)...", run (&MainComponent::openCableManager));
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
            closePanelWindow();
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

void MainComponent::openCableManager()
{
    if (cableWindow == nullptr)
    {
        auto* manager = new CableManager (ctx);
        manager->onClose = [safe = juce::Component::SafePointer<MainComponent> (this)]
        {
            if (safe != nullptr && safe->cableWindow != nullptr) safe->cableWindow->setVisible (false);
        };
        makeToolWindow (cableWindow, "Hydra Mixer - Cable Manager", manager, 1100, 560);
    }
    cableWindow->setResizeLimits (860, 320, 8000, 4000);
    cableWindow->setVisible (true);
    cableWindow->toFront (true);
}

void MainComponent::openRoomMeasurement (int outputSelect)
{
    openCorrectionWindow (outputSelect, true);
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

juce::Point<int> MainComponent::fittingSize() const
{
    // Logical units as in resized(): top bar, strips, inspector; scene bar left, side panel right (if it would be shown).
    const auto s = strips.fittingSize();
    int w = s.x;
    if (ctx.session != nullptr && ctx.session->prefs.sidePanel)
    {
        const int needed = (int) std::ceil ((float) strips.idealWidthFor (s.y) * 1.12f);
        int total = juce::jmax (s.x, needed);
        for (int k = 0; k < 4; ++k)   // the side panel's width depends on the room it gets
            total = juce::jmax (s.x, needed) + juce::jlimit (SidePanel::kMinWidth, SidePanel::kMaxWidth, total * 26 / 100) + 8 + 1;
        w = total;
    }
    w += sceneBar != nullptr ? sceneBar->getWidth() : 0;
    const int h = 54 + 4 + s.y + 236;
    return { (int) std::ceil ((float) w * uiScale), (int) std::ceil ((float) h * uiScale) };
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
    for (juce::Component* c : { (juce::Component*) &top, (juce::Component*) &strips, (juce::Component*) &inspector })
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

    // Side panel: only while the strips (with their "+" placeholders and some room to grow) leave enough space.
    const int sideW = juce::jlimit (SidePanel::kMinWidth, SidePanel::kMaxWidth, r.getWidth() * 26 / 100);
    const bool side = ctx.session != nullptr && ctx.session->prefs.sidePanel
                   && (float) strips.idealWidthFor (r.getHeight()) * 1.12f + (float) sideW + 8.0f <= (float) r.getWidth();
    if (side && sidePanel == nullptr)
    {
        sidePanel = std::make_unique<SidePanel> (ctx);
        addAndMakeVisible (*sidePanel);
    }
    else if (! side)
        sidePanel.reset();
    if (sidePanel != nullptr)
    {
        sidePanel->setTransform (scale);
        sidePanel->setBounds (r.removeFromRight (sideW).reduced (0, 4).withTrimmedRight (6));
        r.removeFromRight (8);
    }
    strips.setBounds (r);
}
} // namespace hydra

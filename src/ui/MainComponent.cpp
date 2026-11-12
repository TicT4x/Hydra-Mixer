#include "UiWatch.h"
#include "MainComponent.h"
#include "CableManager.h"
#include "state/VacCables.h"
#include "state/ChannelClipboard.h"
#include "state/Session.h"
#include "state/Updater.h"
#include "state/Install.h"

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

void DspMeter::setLoad (float l, float peak, int overloadsNow, uint64_t overloadsTotal)
{
    juce::String tip ("DSP load\nBar and number: average share of real time the audio engine needs (like a DAW's CPU meter).\n"
                      "Mark: slowest single block of the last second, now " + juce::String (juce::roundToInt (peak * 100.0f)) + " %. "
                      "Above 100 % a block was not ready in time: that clicks on all devices at once (red).");
    if (overloadsTotal > 0) tip << "\nBlocks over 100 % since the start: " << juce::String ((juce::int64) overloadsTotal) << " (also in hydra.log)";
    tip << "\nClick: what uses it, per channel and plugin";
    setTooltip (tip);
    if (overloadsNow > 0) alarm = 1.0f;   // shown for ~3 s (fades in tick)
    if (std::abs (l - load) <= 0.002f && std::abs (peak - peakLoad) <= 0.01f && overloadsNow == 0) return;
    load = l;
    peakLoad = peak;
    repaint();
}

void DspMeter::tick()
{
    if (alarm <= 0.0f) return;
    alarm = juce::jmax (0.0f, alarm - 1.0f / 12.0f);   // called 4x per second: ~3 s
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
    g.setColour (alarm > 0.0f ? theme::bg.interpolatedWith (theme::mute, 0.55f * alarm) : theme::bg);
    g.fillRoundedRectangle (bar, 3.0f);
    g.setColour (load < 0.5f ? theme::good : load < 0.8f ? theme::solo : theme::mute);
    g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, load)), 3.0f);
    // Peak mark: the slowest block of the last second (an average of 20 % can hide single blocks at 120 %).
    if (peakLoad > load + 0.01f)
    {
        const float x = bar.getX() + bar.getWidth() * juce::jlimit (0.0f, 1.0f, peakLoad) - 1.0f;
        g.setColour (peakLoad < 0.5f ? theme::good : peakLoad < 0.8f ? theme::solo : theme::mute);
        g.fillRect (juce::Rectangle<float> (juce::jmax (bar.getX(), x), bar.getY() - 2.0f, 2.0f, bar.getHeight() + 4.0f));
    }
    g.setColour (alarm > 0.0f ? theme::text.interpolatedWith (theme::mute, alarm) : theme::text);
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
    addChildComponent (updateBadge);
    // Ids for the guided tour (ui/Tour.cpp).
    tools.setComponentID ("top.tools");
    settings.setComponentID ("top.settings");
    statusLink.setComponentID ("top.status");
    dsp.setComponentID ("top.dsp");
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
    if (demoMode) s = demoStatus;

    // Update available / downloading (Updater): a badge left of the tools.
    if (ctx.updater != nullptr)
    {
        const auto st = ctx.updater->getStatus();
        const bool skipped = ctx.session != nullptr && st.release.version == ctx.session->prefs.updateSkip;
        juce::String badge;
        float progress = -1.0f;
        using S = Updater::State;
        if (st.state == S::available && ! skipped) badge = "Update " + st.release.version;
        else if (st.state == S::downloading) { badge = "Downloading " + juce::String (juce::roundToInt (st.progress * 100.0f)) + " %"; progress = st.progress; }
        else if (st.state == S::ready) { badge = "Installing..."; progress = 1.0f; }
        if (badge.isNotEmpty() != updateBadge.isVisible())
        {
            updateBadge.setVisible (badge.isNotEmpty());
            resized();
        }
        updateBadge.set (badge, progress);
        updateBadge.setTooltip (st.state == S::available
            ? "Hydra Mixer " + st.release.version + (st.release.prerelease ? " (beta)" : "") + " is available. Click to see what is new and update."
            : "Downloading the update. Hydra restarts by itself when it is done (audio stops for a few seconds).");
    }
    statusLink.setStatus (s, demoMode || e.getMasterDevice() != nullptr, ctx.learnMode);

    // DSP load: average over the last ~0.25 s (like a DAW's CPU meter), refreshed 4x per second.
    // The peak (single slowest callback) goes into the tooltip.
    if (++cpuTick >= 8)
    {
        cpuTick = 0;
        const auto ov = e.getOverloads();
        const int fresh = (int) juce::jmin ((uint64_t) 1000, ov - juce::jmin (ov, overloadsSeen));
        overloadsSeen = ov;
        dsp.setLoad (demoMode ? demoDspLoad : cpuMeter.update (e), demoMode ? demoDspLoad * 1.4f : e.getRecentPeak(),
                     demoMode ? 0 : fresh, demoMode ? 0 : ov);
        dsp.tick();
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
    const int avail = juce::jmax (0, getWidth() - (46 * 3 + 6) - 2 * 12 - (36 * 3 + 6 + 6 + 12)
                                     - (updateBadge.isVisible() ? updateBadge.idealWidth() + 10 : 0));
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
    if (updateBadge.isVisible())
    {
        updateBadge.setBounds (r.removeFromRight (juce::jmin (r.getWidth(), updateBadge.idealWidth())).reduced (0, 3));
        r.removeFromRight (10);
    }

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
    strips.setComponentID ("strips");      // ids for the guided tour
    inspector.setComponentID ("inspector");
    top.updateBadge.onClick = [this] { showUpdateDialog(); };
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
    tour.reset();
    welcome.reset();
    tourResume.reset();
    pluginScan.reset();   // cancels a scan still running
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
    int output = Engine::isHwOutput (index) ? index : view->getOutput();
    if (output < 0 && Engine::isHwOutput (ctx.selected)) output = ctx.selected;
    for (int b = 0; output < 0 && b < kMaxBuses; ++b)
        if (ctx.engine.isBusActive (b) && Engine::isHwOutput (kMaxInputs + b)) output = kMaxInputs + b;
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

bool MainComponent::browserOpen() const { return ctx.session != nullptr && ctx.session->prefs.fxBrowser; }

void MainComponent::toggleEffectBrowser()
{
    if (ctx.session == nullptr) return;
    ctx.session->prefs.fxBrowser = ! ctx.session->prefs.fxBrowser;
    ctx.session->prefsVersion.fetch_add (1);
    // Glides open / shut (browserFrame); meanwhile strips and inspector follow directly instead of jumping.
    browserGlideFrom = browserShown;
    browserGlideStart = juce::Time::getMillisecondCounterHiRes();
    browserGliding = true;
    resized();
}

void MainComponent::browserFrame()
{
    if (! browserGliding) return;
    constexpr double kMs = 230.0;
    const float target = browserOpen() ? 1.0f : 0.0f;
    const double t = juce::jlimit (0.0, 1.0, (juce::Time::getMillisecondCounterHiRes() - browserGlideStart) / kMs);
    const float eased = 1.0f - std::pow (1.0f - (float) t, 3.0f);   // ease out
    browserShown = browserGlideFrom + (target - browserGlideFrom) * eased;
    const bool done = t >= 1.0;
    if (done) { browserShown = target; browserGliding = false; }
    const bool wasInstant = ctx.instantLayout;
    ctx.instantLayout = ! done;
    resized();
    ctx.instantLayout = wasInstant;
    if (done && target == 0.0f) browser.reset();
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
    m.addItem ("Effect Browser   (Ctrl+Alt+B)", true, browserOpen(), run (&MainComponent::toggleEffectBrowser));
    m.addSeparator();
    m.addItem ("MIDI Learn Mode", true, ctx.learnMode, run (&MainComponent::toggleLearnMode));
    m.addItem ("MIDI Map...", run (&MainComponent::openMidiMap));
    m.addItem ("MIDI faders stop at 0 dB", true, ctx.midi.getFaderTopAt0dB(), [safe]
    {
        // The range of every fader mapping ends at 0 dB (off again: back to the full range). Same as in Settings > MIDI.
        if (safe == nullptr) return;
        auto& midi = safe->ctx.midi;
        midi.setFaderTopAt0dB (! midi.getFaderTopAt0dB());
        safe->showToast (midi.getFaderTopAt0dB() ? "MIDI faders now stop at 0 dB" : "MIDI faders use their full range again", 2.5);
    });
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
    m.addSeparator();
    {
        juce::PopupMenu help;
        Tour::Kind kind;
        int step = 0;
        if (ctx.session != nullptr && Tour::parseState (ctx.session->prefs.tour, kind, step))
            help.addItem ("Continue the tour (step " + juce::String (step + 1) + ")", [safe, kind, step] { if (safe != nullptr) safe->startTour (kind, step); });
        help.addItem ("Full tour (2 min)", [safe] { if (safe != nullptr) safe->startTour (Tour::Kind::full, 0); });
        help.addItem ("Coming from Voicemeeter (1 min)", [safe] { if (safe != nullptr) safe->startTour (Tour::Kind::voicemeeter, 0); });
        m.addSubMenu ("Guided tour", help);
        m.addItem ("Setup wizard...", [safe] { if (safe != nullptr) safe->startWelcome(); });
    }
    m.addSeparator();
    m.addItem ("Wiki (hydra-mixer.de)", [] { juce::URL ("https://hydra-mixer.de/#docs").launchInDefaultBrowser(); });
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&top.tools).withMinimumWidth (200));
}

void MainComponent::openSettings (int tab)
{
    UiActivity busy ("settings: opening the window");
    const double t0 = juce::Time::getMillisecondCounterHiRes();
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
    {
        p->showTab (tab);
        p->measureFirstPaint (t0);
    }
    settingsWindow->setVisible (true);
    settingsWindow->toFront (true);
    if (const double ms = juce::Time::getMillisecondCounterHiRes() - t0; ms > 100.0)
        logLine ("settings: opening the window took " + juce::String (juce::roundToInt (ms)) + " ms");
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
    if (tour != nullptr && tour->isVisible() && k == juce::KeyPress::escapeKey)
    {
        tour->pause();
        return true;
    }
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
    if (k.getModifiers().isCtrlDown() && k.getModifiers().isAltDown() && (k.getKeyCode() == 'B' || k.getKeyCode() == 'b'))
    {
        toggleEffectBrowser();
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
    tickUpdates();
    if (pluginScan != nullptr && ! pluginScan->isThreadRunning())
    {
        pluginScan.reset();
        showToast ("Plugin scan done: " + juce::String (ctx.engine.pluginHost.known.getNumTypes()) + " plugins found", 3.0);
        inspector.reload();
    }

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
    w += browserWidth();
    const int h = 54 + 4 + s.y + 236;
    return { (int) std::ceil ((float) w * uiScale), (int) std::ceil ((float) h * uiScale) };
}

int MainComponent::idealWidth() const
{
    const int logical = strips.idealWidth() + (sceneBar != nullptr ? sceneBar->getWidth() : 0) + browserWidth();
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
    // Effect browser at the left edge like Ableton's, the full height below the top bar; its tab sits at its
    // right edge and moves out with it (click: fold in / out). Opening and closing glide (browserFrame).
    if (! browserGliding) browserShown = browserOpen() ? 1.0f : 0.0f;
    if (browserTab.getParentComponent() != this)
    {
        browserTab.onClick = [this] { toggleEffectBrowser(); };
        addAndMakeVisible (browserTab);
    }
    if (browserShown > 0.0f)
    {
        if (browser == nullptr)
        {
            browser = std::make_unique<EffectBrowser> (ctx);
            browser->onMessage = [this] (const juce::String& text) { showToast (text, 2.5); };
            addAndMakeVisible (*browser);
        }
        // Slides in from the left: always laid out at its full width, the part that is out shows.
        const int full = juce::jmin (EffectBrowser::kWidth, r.getWidth() / 2);
        const int shown = juce::roundToInt ((float) full * browserShown);
        auto area = r.removeFromLeft (shown).withTrimmedBottom (4);
        browser->setTransform (scale);
        browser->setBounds (area.withX (area.getRight() - full).withWidth (full));
    }
    browserTab.setTransform (scale);
    browserTab.setOpen (browserOpen());
    browserTab.setBounds (r.removeFromLeft (BrowserTab::kWidth + 4).withTrimmedBottom (4).withTrimmedLeft (browserShown > 0.0f ? 2 : 4).withTrimmedRight (browserShown > 0.0f ? 2 : 0));
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
    if (tourResume != nullptr)
        tourResume->setTopLeftPosition (getWidth() - TourResumeCard::kWidth - 16, getHeight() - TourResumeCard::kHeight - 16);
}

//==============================================================================
void UpdateBadge::set (const juce::String& t, float p)
{
    if (t == text && std::abs (p - progress) < 0.002f) return;
    text = t;
    progress = p;
    repaint();
}

int UpdateBadge::idealWidth() const
{
    return juce::jmax (120, juce::GlyphArrangement::getStringWidthInt (theme::font (12.5f, true), "Downloading 100 %") + 44);
}

void UpdateBadge::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    const float rad = r.getHeight() / 2.0f;
    juce::ColourGradient grad (theme::accentIn, r.getX(), 0, theme::accentHw, r.getRight(), 0, false);
    if (progress < 0.0f)
    {
        g.setGradientFill (grad);
        g.setOpacity (hover ? 1.0f : 0.88f);
        g.fillRoundedRectangle (r, rad);
        g.setOpacity (1.0f);
    }
    else
    {
        g.setColour (theme::raised);
        g.fillRoundedRectangle (r, rad);
        juce::Graphics::ScopedSaveState s (g);
        juce::Path clip;
        clip.addRoundedRectangle (r, rad);
        g.reduceClipRegion (clip);
        g.setGradientFill (grad);
        g.setOpacity (0.55f);
        g.fillRect (r.withWidth (r.getWidth() * juce::jlimit (0.0f, 1.0f, progress)));
    }
    // Arrow down into a tray.
    const auto icon = juce::Rectangle<float> (r.getX() + 12.0f, r.getCentreY() - 7.0f, 14.0f, 14.0f);
    g.setColour (progress < 0.0f ? theme::bg : theme::text);
    juce::Path a;
    a.startNewSubPath (icon.getCentreX(), icon.getY());
    a.lineTo (icon.getCentreX(), icon.getBottom() - 4.0f);
    a.startNewSubPath (icon.getX() + 3.0f, icon.getBottom() - 8.0f);
    a.lineTo (icon.getCentreX(), icon.getBottom() - 4.0f);
    a.lineTo (icon.getRight() - 3.0f, icon.getBottom() - 8.0f);
    a.startNewSubPath (icon.getX(), icon.getBottom() - 2.0f);
    a.lineTo (icon.getX(), icon.getBottom());
    a.lineTo (icon.getRight(), icon.getBottom());
    a.lineTo (icon.getRight(), icon.getBottom() - 2.0f);
    g.strokePath (a, juce::PathStrokeType (1.7f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setFont (theme::font (12.5f, true));
    g.drawText (text, r.withTrimmedLeft (34.0f).withTrimmedRight (10.0f), juce::Justification::centredLeft, true);
}

//==============================================================================
void MainComponent::setUpdater (Updater* u, std::function<void()> install, std::function<void()> check)
{
    ctx.updater = u;
    ctx.installUpdate = std::move (install);
    ctx.checkForUpdates = std::move (check);
}

void MainComponent::tickUpdates()
{
    if (ctx.updater == nullptr || ctx.updater->version.load() == shownUpdate) return;
    shownUpdate = ctx.updater->version.load();
    const auto st = ctx.updater->getStatus();
    if (st.state == Updater::State::ready && installWhenReady)
    {
        installWhenReady = false;
        if (ctx.installUpdate) ctx.installUpdate();
    }
    else if (st.state == Updater::State::failed && installWhenReady)
    {
        installWhenReady = false;
        dialog::message (this, dialog::Kind::warning, "Update failed", st.error + "\n\nHydra keeps running as it is. Try again later "
                         "(the update badge stays) or download it from the website.");
    }
}

void MainComponent::showUpdateDialog()
{
    if (ctx.updater == nullptr) return;
    const auto st = ctx.updater->getStatus();
    juce::Component::SafePointer<MainComponent> safe (this);
    if (st.state == Updater::State::downloading)
    {
        dialog::confirm (this, dialog::Kind::question, "Cancel the update?", "The download stops. You can start it again with the badge.",
                         "Cancel download", [safe]
                         {
                             if (safe == nullptr || safe->ctx.updater == nullptr) return;
                             safe->installWhenReady = false;
                             safe->ctx.updater->cancelDownload();
                         });
        return;
    }
    if (st.state != Updater::State::available && st.state != Updater::State::failed) return;
    // Release notes, shortened and without Markdown marks.
    juce::StringArray lines;
    for (auto line : juce::StringArray::fromLines (st.release.notes))
    {
        line = line.trim();
        while (line.startsWithChar ('#')) line = line.substring (1).trim();
        if (line.startsWith ("- ") || line.startsWith ("* ")) line = juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xa2 ")) + line.substring (2);
        line = line.replace ("**", "").replace ("`", "");
        if (line.isNotEmpty()) lines.add (line);
        if (lines.size() >= 12) { lines.add ("..."); break; }
    }
    auto text = lines.joinIntoString ("\n");
    if (text.length() > 1100) text = text.substring (0, 1100) + "...";
    const auto v = st.release.version;
    dialog::show (this, dialog::Kind::info, "Hydra Mixer " + v + (st.release.prerelease ? " (beta)" : "") + " is available",
                  (text.isNotEmpty() ? text + "\n\n" : juce::String()) + "You are using " + install::currentVersion()
                      + ". Updating downloads it, saves your settings and restarts Hydra (audio stops for a few seconds).",
                  { { "Update now", dialog::Button::Role::primary }, { "Later", dialog::Button::Role::cancel }, { "Skip this version" } },
                  [safe, v] (int choice)
                  {
                      if (safe == nullptr || safe->ctx.updater == nullptr) return;
                      if (choice == 0)
                      {
                          safe->installWhenReady = true;
                          safe->ctx.updater->download();
                      }
                      else if (choice == 2 && safe->ctx.session != nullptr)
                      {
                          safe->ctx.session->prefs.updateSkip = v;
                          safe->ctx.session->prefsVersion.fetch_add (1);
                      }
                  });
}

//==============================================================================
void MainComponent::startWelcome()
{
    if (welcome != nullptr) return;
    tour.reset();
    tourResume.reset();
    welcome = std::make_unique<WelcomeWizard> (ctx, *this);
    welcome->onChanged = [this]
    {
        undo.clear();
        refreshAll();
        closePanelWindow();
        inspector.reload();
        if (onChannelsChanged) onChannelsChanged();
    };
    welcome->onScanPlugins = [this]
    {
        if (pluginScan != nullptr) return;
        pluginScan = std::make_unique<PluginScanJob> (ctx.engine);
        pluginScan->startThread (juce::Thread::Priority::low);
        showToast ("Looking for VST3 plugins in the background...", 3.0);
    };
    welcome->onDone = [this] (WelcomeWizard::Tour t)
    {
        if (ctx.session != nullptr)
        {
            ctx.session->prefs.setupDone = true;
            ctx.session->prefsVersion.fetch_add (1);
        }
        // Later: this is called from inside the wizard.
        juce::Component::SafePointer<MainComponent> safe (this);
        juce::MessageManager::callAsync ([safe, t]
        {
            if (safe == nullptr) return;
            safe->welcome.reset();
            safe->grabKeyboardFocus();
            if (t == WelcomeWizard::Tour::full) safe->startTour (Tour::Kind::full, 0);
            else if (t == WelcomeWizard::Tour::voicemeeter) safe->startTour (Tour::Kind::voicemeeter, 0);
        });
    };
}

void MainComponent::startTour (Tour::Kind kind, int step)
{
    tour.reset();
    tourResume.reset();
    Tour::Host host;
    host.reveal = [this] (int sel) { scrollToChannel (sel); };
    host.windowOpen = [this] (const juce::String& w)
    {
        auto* win = w == "device" ? deviceWindow.get() : w == "settings" ? settingsWindow.get() : nullptr;
        return win != nullptr && win->isVisible();
    };
    host.moveDeviceWindowAside = [this] (juce::Rectangle<int> keepFree)
    {
        if (deviceWindow == nullptr || ! deviceWindow->isVisible()) return;
        const auto* d = juce::Desktop::getInstance().getDisplays().getDisplayForRect (deviceWindow->getScreenBounds());
        if (d == nullptr) return;
        const auto area = d->userBounds.getSmallestIntegerContainer();
        auto b = deviceWindow->getBounds();
        if (! b.intersects (keepFree)) return;
        // Right of the card if there is room, else left of it, else below.
        if (keepFree.getRight() + 16 + b.getWidth() <= area.getRight()) b.setPosition (keepFree.getRight() + 16, juce::jlimit (area.getY(), area.getBottom() - b.getHeight(), keepFree.getY()));
        else if (keepFree.getX() - 16 - b.getWidth() >= area.getX()) b.setPosition (keepFree.getX() - 16 - b.getWidth(), juce::jlimit (area.getY(), area.getBottom() - b.getHeight(), keepFree.getY()));
        else b.setY (juce::jmin (area.getBottom() - b.getHeight(), keepFree.getBottom() + 16));
        deviceWindow->setBounds (b);
    };
    tour = std::make_unique<Tour> (ctx, *this, std::move (host), kind, step);
    tour->onClose = [this, kind] (bool finished, int at)
    {
        if (ctx.session != nullptr)
        {
            ctx.session->prefs.tour = finished ? juce::String ("done") : Tour::kindId (kind) + ":" + juce::String (at);
            ctx.session->prefsVersion.fetch_add (1);
        }
        juce::Component::SafePointer<MainComponent> safe (this);
        juce::MessageManager::callAsync ([safe, finished]
        {
            if (safe == nullptr) return;
            safe->tour.reset();
            safe->grabKeyboardFocus();
            if (finished) safe->showToast ("Tour finished. Watch it again any time: Tools > Guided tour", 3.5);
            else safe->showTourResume();
        });
    };
}

void MainComponent::showTourResume()
{
    Tour::Kind kind;
    int step = 0;
    if (ctx.session == nullptr || ! Tour::parseState (ctx.session->prefs.tour, kind, step) || tour != nullptr || welcome != nullptr) return;
    tourResume = std::make_unique<TourResumeCard> ("Tour paused at step " + juce::String (step + 1)
                                                   + (kind == Tour::Kind::voicemeeter ? " (Voicemeeter)" : ""));
    tourResume->onChoice = [this, kind, step] (int choice)
    {
        juce::Component::SafePointer<MainComponent> safe (this);
        juce::MessageManager::callAsync ([safe, choice, kind, step]
        {
            if (safe == nullptr) return;
            safe->tourResume.reset();
            if (choice == 0) safe->startTour (kind, step);
            else if (choice == 1) safe->startTour (kind, 0);
            else safe->showToast ("Continue the tour any time: Tools > Guided tour", 3.0);
        });
    };
    addAndMakeVisible (*tourResume);
    tourResume->setAlwaysOnTop (true);
    resized();
}
} // namespace hydra


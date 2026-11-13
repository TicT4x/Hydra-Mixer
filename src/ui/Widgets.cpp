#include "Widgets.h"
#include "ChannelIcons.h"
#include "state/UndoHistory.h"

namespace hydra
{
//==============================================================================
bool UiContext::handleUndoKey (const juce::KeyPress& key, juce::String* message)
{
    const auto mods = key.getModifiers();
    if (! mods.isCommandDown() || mods.isAltDown()) return false;
    const int code = juce::CharacterFunctions::toUpperCase ((juce::juce_wchar) key.getKeyCode());
    const bool isUndo = code == 'Z' && ! mods.isShiftDown();
    const bool isRedo = code == 'Y' || (code == 'Z' && mods.isShiftDown());
    if (! isUndo && ! isRedo) return false;
    if (undo == nullptr) return true;

    const auto what = isUndo ? undo->undo() : undo->redo();
    if (message != nullptr)
        *message = what.isNotEmpty() ? (isUndo ? "Undo:  " : "Redo:  ") + what
                                     : juce::String (isUndo ? "Nothing to undo" : "Nothing to redo");
    return true;
}

//==============================================================================
//==============================================================================
bool setParamFromText (Param& p, const juce::String& text)
{
    auto t = text.trim().toLowerCase();
    if (t.isEmpty()) return false;

    if (! p.choices.isEmpty())
    {
        for (int i = 0; i < p.choices.size(); ++i)
            if (p.choices[i].toLowerCase().startsWith (t)) { p.set ((float) i); return true; }
    }
    if (p.type == ParamType::Toggle)
    {
        if (t == "on" || t == "1")  { p.set (1.0f); return true; }
        if (t == "off" || t == "0") { p.set (0.0f); return true; }
        return false;
    }
    if (t.startsWith ("-inf") || t == "inf" || t == "off")
    {
        p.set (p.range.start);
        return true;
    }

    const auto number = t.retainCharacters ("0123456789.,-+").replaceCharacter (',', '.');
    if (! number.containsAnyOf ("0123456789")) return false;
    float v = number.getFloatValue();
    const auto unit = t.trimCharactersAtStart ("0123456789.,-+ ");
    if (p.unit == " Hz" && unit.startsWith ("k")) v *= 1000.0f;
    if (p.unit == " ms" && unit.startsWith ("s"))  v *= 1000.0f;
    p.set (v);
    return true;
}

//==============================================================================
//==============================================================================
// Mouse wheel between controls and scrolling panes. Without rules, scrolling through the strips moved every
// fader and knob that passed under the mouse, and a control the user wanted to turn scrolled the pane instead.
// Rules (only inside a pane that can scroll; elsewhere the wheel always turns the control):
//  - The pane keeps the wheel while it is being scrolled (latch): controls that slide under the mouse stay put.
//  - A control takes the wheel once the mouse really moved onto it and rested there briefly (kRestMs), or right
//    after it was clicked. It then keeps the wheel for the whole wheel gesture, even if the mouse drifts off it.
//  - Ctrl + wheel always turns the control under the mouse (Shift: fine steps, as before).
namespace
{
constexpr double kWheelLatchMs = 450.0, kWheelRestMs = 280.0;
double paneScrolledMs = -1.0e9, ownerWheelMs = -1.0e9;
const ParamControl* wheelOwner = nullptr;
// Last mouse position seen by any control (or when a pane scrolled): an event at the same place is a control
// sliding under a resting mouse, not the user moving onto it.
juce::Point<float> lastMouseScreen { -1.0e6f, -1.0e6f };
double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }

bool inScrollablePane (const juce::Component& c)
{
    for (auto* p = c.getParentComponent(); p != nullptr; p = p->getParentComponent())
        if (auto* v = dynamic_cast<juce::Viewport*> (p))
            if (v->canScrollHorizontally() || v->canScrollVertically()) return true;
    return false;
}
} // namespace

void ParamControl::notePaneScrolled()
{
    paneScrolledMs = nowMs();
    wheelOwner = nullptr;
    lastMouseScreen = juce::Desktop::getInstance().getMainMouseSource().getScreenPosition();
}

void ParamControl::HoverClock::track (const juce::MouseEvent& e)
{
    // Only a real movement of the mouse counts: when the pane scrolls, controls move under a resting mouse.
    const auto p = e.getScreenPosition().toFloat();
    if (p == lastMouseScreen) return;
    lastMouseScreen = p;
    if (since == 0.0) since = nowMs();
}

bool ParamControl::takesWheel (const juce::MouseEvent& e)
{
    const double now = nowMs();
    if (e.mods.isCtrlDown()) return true;
    if (wheelOwner == this && now - ownerWheelMs < kWheelLatchMs) return true;
    if (now - paneScrolledMs < kWheelLatchMs) return false;
    if (! inScrollablePane (*this)) return true;
    if (isMouseButtonDown()) return true;
    return hoverClock.since > 0.0 && now - hoverClock.since >= kWheelRestMs;
}

ParamControl::ParamControl (UiContext& c, Param& p) : param (p), ctx (c)
{
    ParamPoller::attach (this);
    setTooltip (p.name);
    setRepaintsOnMouseActivity (false);
    addMouseListener (&hoverClock, false);
}

ParamControl::~ParamControl()
{
    removeMouseListener (&hoverClock);
    if (wheelOwner == this) wheelOwner = nullptr;
    ParamPoller::detach (this);
}

juce::String ParamControl::getTooltip()
{
    if (! tooltipText) return juce::SettableTooltipClient::getTooltip();
    const auto key = ctx.hotkeys.getKeyFor (&param);
    return key.isValid() ? tooltipText() + "   [" + Hotkeys::describe (key) + "]" : tooltipText();
}

void ParamControl::resized()
{
    if (valueEditor != nullptr) closeValueEditor (false);
}

void ParamControl::editValueAsText (juce::Rectangle<int> area)
{
    if (valueEditor != nullptr) return;
    valueEditor = std::make_unique<juce::TextEditor>();
    auto& ed = *valueEditor;
    ed.setJustification (juce::Justification::centred);
    ed.setFont (theme::font (11.0f));
    ed.setIndents (2, 0);
    ed.setColour (juce::TextEditor::backgroundColourId, theme::bg);
    ed.setColour (juce::TextEditor::textColourId, theme::text);
    ed.setColour (juce::TextEditor::outlineColourId, theme::outline);
    ed.setColour (juce::TextEditor::focusedOutlineColourId, theme::accentIn);
    ed.setColour (juce::TextEditor::highlightColourId, theme::accentIn.withAlpha (0.4f));

    // Plain number to type over ("2.50k" for 2.5 kHz, "-inf" for a closed fader)
    const auto v = param.get();
    juce::String txt;
    if (param.unit == " Hz" && v >= 1000.0f) txt = juce::String (v / 1000.0f, 2) + "k";
    else if (param.unit == " dB" && v <= param.range.start + 0.001f && param.range.start <= -60.0f) txt = "-inf";
    else txt = param.toText().upToFirstOccurrenceOf (" ", false, false);
    ed.setText (txt, false);
    ed.setBounds (area.expanded (2, 1).constrainedWithin (getLocalBounds()));
    addAndMakeVisible (ed);
    ed.selectAll();
    ed.grabKeyboardFocus();

    // Destroying the editor inside its own callback is unsafe, so close it asynchronously.
    juce::Component::SafePointer<ParamControl> safe (this);
    auto finish = [safe] (bool apply)
    {
        juce::MessageManager::callAsync ([safe, apply] { if (safe != nullptr) safe->closeValueEditor (apply); });
    };
    ed.onReturnKey = [finish] { finish (true); };
    ed.onEscapeKey = [finish] { finish (false); };
    ed.onFocusLost = [finish] { finish (true); };
}

void ParamControl::closeValueEditor (bool apply)
{
    if (valueEditor == nullptr) return;
    const auto text = valueEditor->getText();
    valueEditor.reset();
    if (apply) setParamFromText (param, text);
    repaint();
}

void ParamControl::pollParams()
{
    bool dirty = false;
    if (const auto v = param.getVersion(); v != seenVersion) { seenVersion = v; dirty = true; }

    const bool nowArmed = ctx.midi.getArmedParam() == &param;
    if (nowArmed != armed) { armed = nowArmed; dirty = true; }
    if (armed) dirty = true; // pulse animation

    if (const auto mv = ctx.midi.mapVersion.load(); mv != seenMapVersion)
    {
        seenMapVersion = mv;
        const bool m = ctx.midi.hasMapping (&param);
        if (m != mapped) { mapped = m; dirty = true; }
        float lo = 0.0f, hi = 1.0f;
        for (auto& mp : ctx.midi.getMappingsFor (&param))
            if (! mp.isSwitch()) { lo = mp.minN; hi = mp.maxN; }
        if (lo != midiMin || hi != midiMax) { midiMin = lo; midiMax = hi; dirty = true; }
    }
    if (learnShown != ctx.learnMode) { learnShown = ctx.learnMode; dirty = true; }

    if (const auto hv = ctx.hotkeys.version.load(); hv != seenHotkeyVersion)
    {
        if (seenHotkeyVersion == ~0u) baseTooltip = getTooltip();
        seenHotkeyVersion = hv;
        const auto key = ctx.hotkeys.getKeyFor (&param);
        setTooltip (key.isValid() ? baseTooltip + "   [" + Hotkeys::describe (key) + "]" : baseTooltip);
    }

    if (dirty) repaint();
}

void ParamControl::mouseDown (const juce::MouseEvent& e)
{
    swallowGesture = false;
    if (e.mods.isPopupMenu())
    {
        swallowGesture = true;
        showMenu();
        return;
    }
    if (ctx.learnMode)
    {
        swallowGesture = true;
        ctx.midi.armParam (armed ? nullptr : &param);
        pollParams();
        return;
    }
    hoverClock.since = nowMs() - kWheelRestMs;   // clicked: the wheel belongs to this control right away
    press (e);
}

void ParamControl::mouseDrag (const juce::MouseEvent& e) { if (! swallowGesture) drag (e); }
void ParamControl::mouseUp (const juce::MouseEvent& e)   { if (! swallowGesture) release (e); }

void ParamControl::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! ctx.learnMode && ! e.mods.isPopupMenu() && param.type == ParamType::Continuous)
        param.reset();
}

void ParamControl::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (param.type != ParamType::Continuous || ! takesWheel (e))
    {
        // Not for this control: the pane around it scrolls (Component passes the wheel on to the parent).
        if (inScrollablePane (*this)) notePaneScrolled();
        juce::Component::mouseWheelMove (e, w);
        return;
    }
    wheelOwner = this;
    ownerWheelMs = nowMs();
    const float dir = (w.deltaY != 0 ? w.deltaY : w.deltaX) > 0 ? 1.0f : -1.0f;
    const float step = wheelStep() * (e.mods.isShiftDown() ? 0.2f : 1.0f);
    param.setNormalised (param.getNormalised() + dir * step);
}

void ParamControl::showMenu()
{
    juce::PopupMenu m;
    m.addSectionHeader (param.name + "  =  " + param.toText());
    m.addItem (1, armed ? "Cancel MIDI Learn" : "MIDI Learn");

    const auto maps = ctx.midi.getMappingsFor (&param);
    bool hasContinuous = false;
    for (auto& mp : maps)
    {
        m.addItem (-1, "   " + mp.describe() + "  (" + MidiMapping::modeName (mp.mode) + ")"
                       + (mp.isSwitch() ? juce::String() : "   range " + mp.rangeText()), false);
        hasContinuous |= ! mp.isSwitch();
    }
    m.addItem (2, "Clear MIDI Mapping", ! maps.empty());

    if (Hotkeys::canAssign (param))
    {
        m.addSeparator();
        const auto key = ctx.hotkeys.getKeyFor (&param);
        if (key.isValid())
            m.addItem (-1, "   Shortcut:  " + Hotkeys::describe (key)
                           + (Hotkeys::wantsGlobal (key) ? "  (global)" : "  (while Hydra has focus)"), false);
        m.addItem (20, key.isValid() ? "Change Keyboard Shortcut..." : "Assign Keyboard Shortcut...");
        m.addItem (21, "Remove Keyboard Shortcut", key.isValid());
    }

    if (hasContinuous && param.type == ParamType::Continuous)
    {
        juce::PopupMenu range;
        range.addItem (10, "Set MIDI maximum to current value  (" + param.toText() + ")");
        range.addItem (11, "Set MIDI minimum to current value  (" + param.toText() + ")");
        if (param.unit == " dB" && param.range.end > 0.0f && param.range.start <= -60.0f)
        {
            range.addSeparator();
            range.addItem (12, "Fader: -inf .. 0 dB");
            range.addItem (13, "Fader: -inf .. +6 dB");
        }
        range.addSeparator();
        range.addItem (14, "Full range");
        m.addSubMenu ("MIDI Range (calibrate)", range);
    }
    m.addSeparator();
    m.addItem (3, "Reset to Default", param.get() != param.defaultValue);

    juce::Component::SafePointer<ParamControl> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this), [safe] (int r)
    {
        if (safe == nullptr) return;
        auto& self = *safe;
        if (r == 1) self.ctx.midi.armParam (self.armed ? nullptr : &self.param);
        if (r == 2) self.ctx.midi.removeMappingsFor (&self.param);
        if (r == 3) self.param.reset();
        const float cur = self.param.getNormalised();
        if (r == 10) self.ctx.midi.setRangeForParam (&self.param, -1.0f, cur);
        if (r == 11) self.ctx.midi.setRangeForParam (&self.param, cur, -1.0f);
        if (r == 12) self.ctx.midi.setRangeForParam (&self.param, 0.0f, self.param.range.convertTo0to1 (0.0f));
        if (r == 13) self.ctx.midi.setRangeForParam (&self.param, 0.0f, self.param.range.convertTo0to1 (6.0f));
        if (r == 14) self.ctx.midi.setRangeForParam (&self.param, 0.0f, 1.0f);
        if (r == 20 && self.ctx.armHotkey) self.ctx.armHotkey (self.param);
        if (r == 21) self.ctx.hotkeys.remove (&self.param);
        self.pollParams();
    });
}

void ParamControl::paintOverChildren (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    if (armed)
    {
        const float pulse = 0.55f + 0.45f * std::sin ((float) juce::Time::getMillisecondCounter() * 0.012f);
        g.setColour (theme::learn.withAlpha (0.15f * pulse));
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour (theme::learn.withAlpha (pulse));
        g.drawRoundedRectangle (r, 5.0f, 2.0f);
    }
    else if (ctx.learnMode)
    {
        g.setColour (theme::learn.withAlpha (mapped ? 0.55f : 0.18f));
        g.drawRoundedRectangle (r, 5.0f, 1.0f);
    }

    if (mapped && (ctx.learnMode || armed))
    {
        g.setColour (theme::learn);
        g.fillEllipse (r.getRight() - 7.0f, r.getY() + 1.0f, 6.0f, 6.0f);
    }
}

//==============================================================================
Fader::Fader (UiContext& c, Param& p, juce::Colour a) : ParamControl (c, p), accent (a) {}

juce::Rectangle<float> Fader::track() const
{
    auto r = getLocalBounds().toFloat();
    return horizontal ? r.reduced (16.0f, 0.0f) : r.reduced (0.0f, 14.0f);
}

void Fader::press (const juce::MouseEvent& e)
{
    dragStartNorm = param.getNormalised();
    // Click on the track (not the thumb) jumps there.
    const auto t = track();
    if (horizontal)
    {
        const float thumbX = t.getX() + dragStartNorm * t.getWidth();
        if (std::abs ((float) e.x - thumbX) > 14.0f && ! e.mods.isShiftDown())
        {
            param.setNormalised (((float) e.x - t.getX()) / t.getWidth());
            dragStartNorm = param.getNormalised();
            animating = true;
        }
        return;
    }
    const float thumbY = t.getBottom() - dragStartNorm * t.getHeight();
    if (std::abs ((float) e.y - thumbY) > 14.0f && ! e.mods.isShiftDown())
    {
        param.setNormalised ((t.getBottom() - (float) e.y) / t.getHeight());
        dragStartNorm = param.getNormalised();
        animating = true; // glide to the clicked position
    }
}

void Fader::drag (const juce::MouseEvent& e)
{
    const float sens = e.mods.isShiftDown() ? 0.15f : 1.0f;
    if (horizontal) param.setNormalised (dragStartNorm + (float) e.getDistanceFromDragStartX() * sens / track().getWidth());
    else            param.setNormalised (dragStartNorm - (float) e.getDistanceFromDragStartY() * sens / track().getHeight());
    // Direct manipulation: follow the mouse without smoothing.
    shown = param.getNormalised();
    animating = false;
    repaint();
}

void Fader::pollParams()
{
    ParamControl::pollParams();
    const float target = param.getNormalised();
    if (shown < 0.0f) shown = target;
    else if (target != shown && ! animating && ! isMouseButtonDown()) animating = true;
}

void Fader::frame (double t)
{
    const double dt = juce::jlimit (0.0, 0.1, t - lastFrame);
    lastFrame = t;
    if (! animating) return;
    const float target = param.getNormalised();
    shown += (target - shown) * (1.0f - (float) std::exp (-dt / 0.045)); // ~45 ms glide
    if (std::abs (target - shown) < 0.0008f) { shown = target; animating = false; }
    repaint();
}

/** Rectangular fader cap of the current look, with its shadow. Light theme: white with an edge and a soft
    shadow (the dark themes' grey cap looked dirty on white). */
static void paintFaderCap (juce::Graphics& g, juce::Rectangle<float> cap)
{
    const bool l = theme::light();
    g.setColour (juce::Colours::black.withAlpha (l ? 0.16f : 0.5f));
    g.fillRoundedRectangle (cap.translated (0.0f, l ? 1.5f : 2.0f).expanded (1.0f), 4.0f);
    g.setGradientFill (juce::ColourGradient (juce::Colour (l ? 0xffffffff : 0xffe2e6ee), cap.getCentreX(), cap.getY(),
                                             juce::Colour (l ? 0xffdde2e9 : 0xff9aa3b3), cap.getCentreX(), cap.getBottom(), false));
    g.fillRoundedRectangle (cap, 3.0f);
    if (l)
    {
        g.setColour (juce::Colour (0xffb9c1cc));
        g.drawRoundedRectangle (cap.reduced (0.5f), 3.0f, 1.0f);
    }
}

void Fader::paint (juce::Graphics& g)
{
    if (horizontal) { paintHorizontal (g); return; }
    const auto t = track();
    const float cx = theme::deep() ? t.getRight() - 15.0f   // cap at the right edge, next to the meter
                                   : t.getCentreX() + 6.0f;

    // Scale
    g.setFont (theme::font (10.0f));
    for (float db : { 12.0f, 6.0f, 0.0f, -6.0f, -12.0f, -24.0f, -40.0f, -60.0f })
    {
        const float y = t.getBottom() - param.range.convertTo0to1 (db) * t.getHeight();
        g.setColour (db == 0.0f ? theme::textDim : theme::textFaint);
        g.fillRect (cx - 13.0f, y - 0.5f, 5.0f, 1.0f);
        g.drawText (db <= -60.0f ? "-inf" : juce::String ((int) db), juce::Rectangle<float> (0, y - 6.0f, cx - 15.0f, 12.0f),
                    juce::Justification::centredRight);
    }

    // Track
    auto tr = juce::Rectangle<float> (cx - 3.0f, t.getY(), 6.0f, t.getHeight());
    g.setColour (theme::bg);
    g.fillRoundedRectangle (tr, 3.0f);
    const float norm = shown >= 0.0f ? shown : param.getNormalised();
    const float thumbY = t.getBottom() - norm * t.getHeight();
    g.setGradientFill (juce::ColourGradient (accent.withAlpha (0.35f), cx, t.getBottom(), accent, cx, thumbY, false));
    g.fillRoundedRectangle (tr.withTop (thumbY), 3.0f);

    // (A calibrated MIDI range is no longer drawn beside the track: the user found the yellow bracket ugly, 12.11.)
    if (theme::deep())
    {
        // Thumb: light rectangular cap (stands out from the slot in any channel colour) with a centre line in the accent colour
        auto cap = juce::Rectangle<float> (28.0f, 18.0f).withCentre ({ cx, thumbY });
        paintFaderCap (g, cap);
        g.setColour (accent.darker (0.15f));
        g.fillRect (cap.withSizeKeepingCentre (cap.getWidth() - 2.0f, 3.0f));
        return;
    }

    // Thumb: round knob with an accent ring
    const float d = 24.0f;
    auto thumb = juce::Rectangle<float> (d, d).withCentre ({ cx, thumbY });
    g.setColour (juce::Colours::black.withAlpha (0.4f));
    g.fillEllipse (thumb.translated (0.0f, 2.0f).expanded (1.0f));
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff434b5c), thumb.getX(), thumb.getY(),
                                             juce::Colour (0xff252b35), thumb.getX(), thumb.getBottom(), false));
    g.fillEllipse (thumb);
    g.setColour (accent);
    g.drawEllipse (thumb.reduced (1.0f), 2.0f);
    g.setColour (theme::text.withAlpha (0.85f));
    g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre ({ cx, thumbY }));
}

void Fader::paintHorizontal (juce::Graphics& g)
{
    // Track in the upper part, the scale below it; the cap is the vertical one turned sideways.
    const auto t = track();
    const float cy = juce::jmin (t.getCentreY() - 5.0f, t.getY() + 13.0f);
    auto xOf = [&] (float n) { return t.getX() + n * t.getWidth(); };

    g.setFont (theme::font (10.0f));
    for (float db : { 12.0f, 6.0f, 0.0f, -6.0f, -12.0f, -24.0f, -40.0f, -60.0f })
    {
        const float x = xOf (param.range.convertTo0to1 (db));
        g.setColour (db == 0.0f ? theme::textDim : theme::textFaint);
        g.fillRect (x - 0.5f, cy + 9.0f, 1.0f, 4.0f);
        g.drawText (db <= -60.0f ? "-inf" : juce::String ((int) db), juce::Rectangle<float> (x - 16.0f, cy + 13.0f, 32.0f, 11.0f),
                    juce::Justification::centred);
    }

    auto tr = juce::Rectangle<float> (t.getX(), cy - 3.0f, t.getWidth(), 6.0f);
    g.setColour (theme::bg);
    g.fillRoundedRectangle (tr, 3.0f);
    const float norm = shown >= 0.0f ? shown : param.getNormalised();
    const float thumbX = xOf (norm);
    g.setGradientFill (juce::ColourGradient (accent.withAlpha (0.35f), t.getX(), cy, accent, thumbX, cy, false));
    g.fillRoundedRectangle (tr.withRight (thumbX), 3.0f);

    if (theme::deep())
    {
        auto cap = juce::Rectangle<float> (18.0f, 24.0f).withCentre ({ thumbX, cy });
        paintFaderCap (g, cap);
        g.setColour (accent.darker (0.15f));
        g.fillRect (cap.withSizeKeepingCentre (3.0f, cap.getHeight() - 2.0f));
        return;
    }
    const float d = 22.0f;
    auto thumb = juce::Rectangle<float> (d, d).withCentre ({ thumbX, cy });
    g.setColour (juce::Colours::black.withAlpha (0.4f));
    g.fillEllipse (thumb.translated (0.0f, 2.0f).expanded (1.0f));
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff434b5c), thumb.getX(), thumb.getY(),
                                             juce::Colour (0xff252b35), thumb.getX(), thumb.getBottom(), false));
    g.fillEllipse (thumb);
    g.setColour (accent);
    g.drawEllipse (thumb.reduced (1.0f), 2.0f);
    g.setColour (theme::text.withAlpha (0.85f));
    g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre ({ thumbX, cy }));
}

//==============================================================================
Knob::Knob (UiContext& c, Param& p, juce::String l, juce::Colour a)
    : ParamControl (c, p), label (std::move (l)), accent (a) {}

void Knob::press (const juce::MouseEvent& e)
{
    if (valueArea().contains (e.getPosition()))
    {
        editValueAsText (valueArea());
        return;
    }
    dragStartNorm = param.getNormalised();
    dragging = true;
}

void Knob::drag (const juce::MouseEvent& e)
{
    if (! dragging) return;
    const float sens = e.mods.isShiftDown() ? 0.0015f : 0.006f;
    const float d = (float) (-e.getDistanceFromDragStartY() + e.getDistanceFromDragStartX()) * sens;
    param.setNormalised (dragStartNorm + d);
}

void Knob::setGlow (float v)
{
    v = v < 0.0f ? -1.0f : juce::jmin (1.0f, v);
    if (std::abs (v - glow) < 0.01f && (v > 0.0f) == (glow > 0.0f)) return;
    glow = v;
    repaint();
}

void Knob::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    auto textArea = r.removeFromBottom (alwaysShowValue ? 16.0f : 14.0f);
    if (alwaysShowValue)
    {
        g.setFont (theme::font (10.5f));
        g.setColour (theme::textDim);
        g.drawText (label, r.removeFromTop (14.0f), juce::Justification::centred);
    }
    const float size = juce::jmin (r.getWidth(), r.getHeight()) - 4.0f;
    auto k = r.withSizeKeepingCentre (size, size);
    const auto c = k.getCentre();
    const float rad = size * 0.5f;

    constexpr float a0 = -2.35619f, a1 = 2.35619f;
    const float norm = param.getNormalised();
    const float ang = a0 + norm * (a1 - a0);
    const bool bipolar = param.range.start < 0.0f && param.range.end > 0.0f;
    const float startAng = bipolar ? a0 + param.range.convertTo0to1 (0.0f) * (a1 - a0) : a0;

    juce::Path bgArc, valArc;
    bgArc.addCentredArc (c.x, c.y, rad - 2.0f, rad - 2.0f, 0.0f, a0, a1, true);
    const juce::PathStrokeType stroke (3.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    g.setColour (theme::bg);
    g.strokePath (bgArc, stroke);
    if (glow >= 0.0f)
    {
        // The track lights up with the level: a faint tint at rest, bright with a soft halo on loud parts.
        if (glow > 0.0f)
        {
            g.setColour (accent.withAlpha (0.22f * glow));
            g.strokePath (bgArc, juce::PathStrokeType (3.5f + 5.0f * glow, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        g.setColour (accent.withAlpha (0.14f + 0.62f * glow));
        g.strokePath (bgArc, stroke);
    }
    valArc.addCentredArc (c.x, c.y, rad - 2.0f, rad - 2.0f, 0.0f, juce::jmin (startAng, ang), juce::jmax (startAng, ang), true);
    g.setColour (glow > 0.0f ? accent.brighter (0.6f * glow) : accent);
    g.strokePath (valArc, stroke);

    const float inner = rad - 7.0f;
    g.setColour (theme::raised);
    g.fillEllipse (c.x - inner, c.y - inner, inner * 2.0f, inner * 2.0f);
    g.setColour (theme::text);
    const juce::Point<float> tip (c.x + std::sin (ang) * (inner - 2.0f), c.y - std::cos (ang) * (inner - 2.0f));
    g.drawLine ({ c.getPointOnCircumference (inner * 0.25f, ang), tip }, 2.0f);

    if (isEditingText()) return;
    if (alwaysShowValue)
    {
        // Plain text like in the EQ block; it can be clicked to type a value.
        g.setFont (theme::font (10.5f));
        g.setColour (hover || dragging ? theme::text : theme::textDim);
        g.drawText (param.toText(), textArea, juce::Justification::centred);
        return;
    }
    g.setFont (theme::font (10.5f));
    g.setColour (hover || dragging ? theme::text : theme::textDim);
    g.drawText (hover || dragging ? param.toText() : label, textArea, juce::Justification::centred);
}

//==============================================================================
Toggle::Toggle (UiContext& c, Param& p, juce::String t, juce::Colour on, Icon i)
    : ParamControl (c, p), text (std::move (t)), onColour (on), icon (i) {}

void Toggle::drawIcon (juce::Graphics& g, Icon icon, bool on, juce::Rectangle<float> area, juce::Colour colour)
{
    const float s = juce::jmin (area.getWidth(), area.getHeight());
    const auto c = area.getCentre();
    g.setColour (colour);
    switch (icon)
    {
        case Icon::record:
            g.fillEllipse (juce::Rectangle<float> (s * 0.62f, s * 0.62f).withCentre (c));
            break;
        case Icon::playPause:
            if (on)
            {
                const float h = s * 0.62f, w = s * 0.19f, gap = s * 0.14f;
                g.fillRoundedRectangle (c.x - gap * 0.5f - w, c.y - h * 0.5f, w, h, w * 0.3f);
                g.fillRoundedRectangle (c.x + gap * 0.5f, c.y - h * 0.5f, w, h, w * 0.3f);
            }
            else
            {
                const float h = s * 0.66f, w = h * 0.88f;
                juce::Path p;
                p.addTriangle (c.x - w * 0.4f, c.y - h * 0.5f, c.x - w * 0.4f, c.y + h * 0.5f, c.x + w * 0.6f, c.y);
                g.strokePath (p, juce::PathStrokeType (s * 0.06f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                g.fillPath (p);
            }
            break;
        case Icon::stop:
            g.fillRoundedRectangle (juce::Rectangle<float> (s * 0.52f, s * 0.52f).withCentre (c), s * 0.07f);
            break;
        case Icon::loop:
        {
            // Repeat symbol: two half loops, each ending in an arrow (top to the right, bottom to the left).
            auto r = juce::Rectangle<float> (s * 0.8f, s * 0.52f).withCentre (c);
            const float stroke = juce::jmax (1.3f, s * 0.08f), rad = r.getHeight() * 0.5f, a = s * 0.12f;
            const float cxL = r.getX() + rad, cxR = r.getRight() - rad, pi = juce::MathConstants<float>::pi;
            const float tipTop = cxR - a * 0.2f, tipBottom = cxL + a * 0.2f;
            juce::Path p;
            p.addCentredArc (cxL, c.y, rad, rad, 0.0f, pi, 2.0f * pi, true);
            p.lineTo (tipTop - a * 1.2f, r.getY());
            p.addCentredArc (cxR, c.y, rad, rad, 0.0f, 0.0f, pi, true);
            p.lineTo (tipBottom + a * 1.2f, r.getBottom());
            g.strokePath (p, juce::PathStrokeType (stroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            juce::Path heads;
            heads.addTriangle (tipTop, r.getY(), tipTop - a * 1.6f, r.getY() - a, tipTop - a * 1.6f, r.getY() + a);
            heads.addTriangle (tipBottom, r.getBottom(), tipBottom + a * 1.6f, r.getBottom() - a, tipBottom + a * 1.6f, r.getBottom() + a);
            g.fillPath (heads);
            break;
        }
        case Icon::phase:
        {
            // Polarity symbol: a circle crossed by a diagonal line.
            const float stroke = juce::jmax (1.4f, s * 0.085f);
            auto r = juce::Rectangle<float> (s * 0.56f, s * 0.56f).withCentre (c);
            g.drawEllipse (r, stroke);
            const float d = s * 0.38f;
            g.drawLine (c.x - d, c.y + d, c.x + d, c.y - d, stroke);
            break;
        }
        case Icon::none:
        default:
            break;
    }
}

namespace
{
constexpr int kToggleFrameHz = 60;
constexpr float kToggleStep = 0.3f;   // per frame: settled after ~100 ms

/** Returns true when shown has reached target. */
bool glideToggle (float& shown, float target)
{
    shown += (target - shown) * kToggleStep;
    if (std::abs (target - shown) >= 0.02f) return false;
    shown = target;
    return true;
}
} // namespace

void Toggle::pollParams()
{
    ParamControl::pollParams();
    const float target = isLit() ? 1.0f : 0.0f;
    if (shown < 0.0f) shown = target;
    else if (shown != target && ! isTimerRunning()) startTimerHz (kToggleFrameHz);
}

void Toggle::timerCallback()
{
    if (glideToggle (shown, isLit() ? 1.0f : 0.0f)) stopTimer();
    repaint();
}

void Toggle::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    const bool on = isLit();
    const float pos = shown >= 0.0f ? shown : (on ? 1.0f : 0.0f);
    if (theme::deep() && icon == Icon::none && text == "ON")
    {
        // On/off of a block: toggle switch, at the right end of its slot
        const float h = juce::jmin (18.0f, r.getHeight());
        auto line = r.withSizeKeepingCentre (r.getWidth(), h);
        theme::drawSwitch (g, line.withLeft (line.getRight() - h * 1.8f), pos, onColour, hover, true);
        return;
    }
    if (onText.isNotEmpty())
    {
        g.setColour (hover ? theme::raised.brighter (0.08f) : theme::raised);
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour (on ? onColour.withAlpha (0.8f) : theme::outline);
        g.drawRoundedRectangle (r, 5.0f, 1.0f);
        g.setColour (on ? onColour : theme::text);
        g.setFont (theme::font (juce::jmin (11.5f, r.getHeight() * 0.6f), true));
        g.drawText (on ? onText : text, r, juce::Justification::centred);
        return;
    }
    const bool tinted = soft && theme::deep();
    const auto offFill = hover ? theme::raised.brighter (0.08f) : theme::raised;
    g.setColour (offFill.interpolatedWith (onColour, tinted ? pos * 0.62f : pos));
    g.fillRoundedRectangle (r, 5.0f);
    if (pos < 1.0f)
    {
        g.setColour (theme::outline.withMultipliedAlpha (1.0f - pos));
        g.drawRoundedRectangle (r, 5.0f, 1.0f);
    }
    if (tinted && pos > 0.0f)
    {
        // Routing, LIM: the frame in the full colour makes the active ones easy to pick out in a column of buttons.
        g.setColour (onColour.withMultipliedAlpha (pos));
        g.drawRoundedRectangle (r.reduced (0.5f), 4.5f, 1.0f);
    }
    if (icon != Icon::none)
    {
        // Off: the symbol in its colour (record red, play green); on: dark symbol on the coloured button.
        const auto colour = on ? theme::bg : icon == Icon::stop ? (hover ? theme::text : theme::textDim)
                                                                : onColour.withAlpha (hover ? 1.0f : 0.85f);
        drawIcon (g, icon, on, r.withSizeKeepingCentre (juce::jmin (r.getHeight(), 22.0f), juce::jmin (r.getHeight(), 22.0f)), colour);
        return;
    }
    const auto ink = tinted ? theme::textDim.interpolatedWith (juce::Colours::white, pos) : on ? theme::bg : theme::textDim;
    g.setColour (ink);
    if (icons::exists (glyph))
    {
        const float s = juce::jmin (r.getHeight() - 4.0f, r.getWidth() - 4.0f, 15.0f);
        icons::draw (g, glyph, r.withSizeKeepingCentre (s, s), ink);
        return;
    }
    auto f = theme::font (juce::jmin (11.5f, r.getHeight() * 0.6f), true);
    const float tw = juce::GlyphArrangement::getStringWidth (f, text);
    if (tw > r.getWidth() - 4.0f)
        f = f.withHeight (juce::jmax (8.0f, f.getHeight() * (r.getWidth() - 4.0f) / tw));
    g.setFont (f);
    g.drawText (text, r, juce::Justification::centred);
}

//==============================================================================
Switch::Switch (UiContext& c, Param& p, juce::String t, juce::Colour on)
    : ParamControl (c, p), text (std::move (t)), onColour (on) {}

void Switch::pollParams()
{
    ParamControl::pollParams();
    const float target = param.isOn() ? 1.0f : 0.0f;
    if (shown < 0.0f) shown = target;
    else if (shown != target && ! isTimerRunning()) startTimerHz (kToggleFrameHz);
}

void Switch::timerCallback()
{
    if (glideToggle (shown, param.isOn() ? 1.0f : 0.0f)) stopTimer();
    repaint();
}

void Switch::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    const float h = juce::jmin (22.0f, r.getHeight() - 2.0f);
    theme::drawSwitch (g, r.withSizeKeepingCentre (r.getWidth(), h), shown >= 0.0f ? shown : (param.isOn() ? 1.0f : 0.0f), onColour, hover, true);
    g.setColour (param.isOn() ? theme::text : theme::textDim);
    g.setFont (theme::font (12.5f, true));
    g.drawFittedText (text, r.withTrimmedLeft (h * 1.8f + 10.0f).toNearestInt(), juce::Justification::centredLeft, 1);
}

//==============================================================================
ValueBox::ValueBox (UiContext& c, Param& p, juce::Colour a) : ParamControl (c, p), accent (a)
{
    setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
}

void ValueBox::press (const juce::MouseEvent&) { dragStartNorm = param.getNormalised(); }

void ValueBox::drag (const juce::MouseEvent& e)
{
    const float sens = e.mods.isShiftDown() ? 0.0002f : 0.002f;
    param.setNormalised (dragStartNorm - (float) e.getDistanceFromDragStartY() * sens);
}

void ValueBox::release (const juce::MouseEvent& e)
{
    if (! e.mouseWasDraggedSinceMouseDown()) editValueAsText (getLocalBounds());
}

void ValueBox::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    const bool zero = param.get() == param.range.start;
    g.setColour (hover ? theme::raised.brighter (0.06f) : theme::bg);
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (zero ? theme::outline : accent.withAlpha (0.7f));
    g.drawRoundedRectangle (r, 5.0f, 1.0f);
    g.setColour (zero ? theme::textDim : theme::text);
    g.setFont (theme::mono (12.5f));
    g.drawFittedText (param.toText(), getLocalBounds().reduced (6, 0), juce::Justification::centred, 1, 0.8f);
}

//==============================================================================
ChoiceSegments::ChoiceSegments (UiContext& c, Param& p, juce::Colour a, juce::StringArray l, std::vector<int> v)
    : ParamControl (c, p), accent (a), labels (l.isEmpty() ? p.choices : l), values (std::move (v)) {}

int ChoiceSegments::valueOf (int segment) const
{
    return juce::isPositiveAndBelow (segment, (int) values.size()) ? values[(size_t) segment] : segment;
}

void ChoiceSegments::press (const juce::MouseEvent& e)
{
    const int n = juce::jmax (1, labels.size());
    param.set ((float) valueOf (juce::jlimit (0, n - 1, e.x * n / juce::jmax (1, getWidth()))));
}

void ChoiceSegments::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (theme::bg);
    g.fillRoundedRectangle (r, 6.0f);
    const int n = juce::jmax (1, labels.size());
    const int cur = juce::roundToInt (param.get());
    const float w = r.getWidth() / (float) n;
    for (int i = 0; i < n; ++i)
    {
        auto seg = juce::Rectangle<float> (r.getX() + w * (float) i, r.getY(), w, r.getHeight()).reduced (2.0f);
        const bool on = valueOf (i) == cur;
        if (on)
        {
            g.setColour (accent);
            g.fillRoundedRectangle (seg, 5.0f);
        }
        g.setColour (on ? theme::bg : theme::textDim);
        g.setFont (theme::font (11.0f, on));
        g.drawFittedText (labels[i], seg.toNearestInt(), juce::Justification::centred, 2, 0.8f);
    }
}

//==============================================================================
EqTypeBar::EqTypeBar (UiContext& c, Param& p, juce::Colour a) : ParamControl (c, p), accent (a) {}

void EqTypeBar::press (const juce::MouseEvent& e)
{
    const int n = (int) param.range.end + 1;
    param.set ((float) juce::jlimit (0, n - 1, e.y * n / juce::jmax (1, getHeight())));
}

void EqTypeBar::drawIcon (juce::Graphics& g, int type, juce::Rectangle<float> r, juce::Colour c)
{
    r = r.reduced (r.getWidth() * 0.18f, r.getHeight() * 0.25f);
    const float x0 = r.getX(), x1 = r.getRight(), y0 = r.getY(), y1 = r.getBottom(), ym = r.getCentreY(), xm = r.getCentreX();
    const float w = r.getWidth();
    juce::Path p;
    switch (type)
    {
        case 0: // bell
            p.startNewSubPath (x0, ym + 2);
            p.cubicTo (xm - w * 0.2f, ym + 2, xm - w * 0.15f, y0, xm, y0);
            p.cubicTo (xm + w * 0.15f, y0, xm + w * 0.2f, ym + 2, x1, ym + 2);
            break;
        case 1: // notch
            p.startNewSubPath (x0, y0 + 2);
            p.lineTo (xm - w * 0.15f, y0 + 2);
            p.cubicTo (xm - w * 0.05f, y0 + 2, xm - 1, y1, xm, y1);
            p.cubicTo (xm + 1, y1, xm + w * 0.05f, y0 + 2, xm + w * 0.15f, y0 + 2);
            p.lineTo (x1, y0 + 2);
            break;
        case 2: // band pass
            p.startNewSubPath (x0, y1);
            p.cubicTo (xm - w * 0.25f, y1, xm - w * 0.2f, y0, xm, y0);
            p.cubicTo (xm + w * 0.2f, y0, xm + w * 0.25f, y1, x1, y1);
            break;
        case 3: // high cut (low pass)
            p.startNewSubPath (x0, y0);
            p.lineTo (xm, y0);
            p.cubicTo (x1 - w * 0.3f, y0, x1 - w * 0.25f, y1, x1, y1);
            break;
        case 4: // low cut (high pass)
            p.startNewSubPath (x0, y1);
            p.cubicTo (x0 + w * 0.25f, y1, x0 + w * 0.3f, y0, xm, y0);
            p.lineTo (x1, y0);
            break;
        case 5: // low shelf
            p.startNewSubPath (x0, y0);
            p.lineTo (x0 + w * 0.3f, y0);
            p.cubicTo (xm, y0, xm, y1, x1 - w * 0.3f, y1);
            p.lineTo (x1, y1);
            break;
        default: // high shelf
            p.startNewSubPath (x0, y1);
            p.lineTo (x0 + w * 0.3f, y1);
            p.cubicTo (xm, y1, xm, y0, x1 - w * 0.3f, y0);
            p.lineTo (x1, y0);
            break;
    }
    g.setColour (c);
    g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void EqTypeBar::paint (juce::Graphics& g)
{
    const int n = (int) param.range.end + 1;
    const int cur = juce::roundToInt (param.get());
    const float h = (float) getHeight() / (float) n;
    for (int i = 0; i < n; ++i)
    {
        auto cell = juce::Rectangle<float> (0.0f, h * (float) i, (float) getWidth(), h).reduced (2.0f, 1.5f);
        g.setColour (i == cur ? accent.withAlpha (0.2f) : theme::panel2);
        g.fillRoundedRectangle (cell, 5.0f);
        g.setColour (i == cur ? accent : theme::outline);
        g.drawRoundedRectangle (cell, 5.0f, i == cur ? 1.4f : 1.0f);
        drawIcon (g, i, cell, i == cur ? accent : theme::textDim);
    }
}

//==============================================================================
void MeterView::update (float dt)
{
    const auto [a, b] = meter.take();
    const float in[2] = { gainToDb (a), gainToDb (b) };
    const bool m = meter.muted.load (std::memory_order_relaxed);
    bool dirty = m != dimmed;
    dimmed = m;
    for (int c = 0; c < 2; ++c)
    {
        level[c] = juce::jmax (in[c], level[c] - 26.0f * dt); // 26 dB/s fall-back
        if (in[c] >= hold[c]) { hold[c] = in[c]; holdTime[c] = 1.2f; }
        else if ((holdTime[c] -= dt) <= 0.0f) hold[c] = juce::jmax (level[c], hold[c] - 40.0f * dt);

        if (std::abs (level[c] - shown[c]) > 0.25f || std::abs (hold[c] - shownHold[c]) > 0.25f)
        {
            shown[c] = level[c];
            shownHold[c] = hold[c];
            dirty = true;
        }
    }
    if (dirty) repaint();
}

void MeterView::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    const float gap = 2.0f;
    if (horizontal)
    {
        const float h = (r.getHeight() - gap) * 0.5f;
        for (int c = 0; c < 2; ++c)
        {
            auto bar = juce::Rectangle<float> (r.getX(), r.getY() + (float) c * (h + gap), r.getWidth(), h);
            g.setColour (theme::bg);
            g.fillRoundedRectangle (bar, 2.0f);
            if (const float p = toPos (shown[c]); p > 0.0f)
            {
                if (dimmed)
                    g.setColour (theme::textFaint.withAlpha (0.55f));
                else
                {
                    juce::ColourGradient grad (theme::good, bar.getX(), 0, theme::mute, bar.getRight(), 0, false);
                    grad.addColour (toPos (-12.0f), theme::good);
                    grad.addColour (toPos (-3.0f), theme::solo);
                    g.setGradientFill (grad);
                }
                g.fillRoundedRectangle (bar.withWidth (p * bar.getWidth()), 2.0f);
            }
            if (const float hp = toPos (shownHold[c]); hp > 0.0f)
            {
                g.setColour (dimmed ? theme::textFaint : shownHold[c] > -0.1f ? theme::mute : theme::text.withAlpha (0.8f));
                g.fillRect (bar.getX() + hp * bar.getWidth() - 1.5f, bar.getY(), 1.5f, bar.getHeight());
            }
        }
        if (! compact)
        {
            g.setColour (theme::bg.withAlpha (0.55f));
            for (float db : { 0.0f, -6.0f, -12.0f, -24.0f, -40.0f })
                g.fillRect (r.getX() + toPos (db) * r.getWidth(), r.getY(), 1.0f, r.getHeight());
        }
        return;
    }
    const float w = (r.getWidth() - gap) * 0.5f;

    for (int c = 0; c < 2; ++c)
    {
        auto bar = juce::Rectangle<float> (r.getX() + (float) c * (w + gap), r.getY(), w, r.getHeight());
        g.setColour (theme::bg);
        g.fillRoundedRectangle (bar, 2.0f);

        const float p = toPos (shown[c]);
        if (p > 0.0f)
        {
            if (dimmed)
                g.setColour (theme::textFaint.withAlpha (0.55f));
            else
            {
                juce::ColourGradient grad (theme::good, 0, bar.getBottom(), theme::mute, 0, bar.getY(), false);
                grad.addColour (toPos (-12.0f), theme::good);
                grad.addColour (toPos (-3.0f), theme::solo);
                g.setGradientFill (grad);
            }
            g.fillRoundedRectangle (bar.withTop (bar.getBottom() - p * bar.getHeight()), 2.0f);
        }

        const float hp = toPos (shownHold[c]);
        if (hp > 0.0f)
        {
            g.setColour (dimmed ? theme::textFaint : shownHold[c] > -0.1f ? theme::mute : theme::text.withAlpha (0.8f));
            g.fillRect (bar.getX(), bar.getBottom() - hp * bar.getHeight(), bar.getWidth(), 1.5f);
        }
    }

    if (! compact)
    {
        g.setColour (theme::bg.withAlpha (0.55f));
        for (float db : { 0.0f, -6.0f, -12.0f, -24.0f, -40.0f })
            g.fillRect (r.getX(), r.getBottom() - toPos (db) * r.getHeight(), r.getWidth(), 1.0f);
    }
}

} // namespace hydra

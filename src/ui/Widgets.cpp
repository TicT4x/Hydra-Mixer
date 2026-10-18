#include "Widgets.h"
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
ParamControl::ParamControl (UiContext& c, Param& p) : param (p), ctx (c)
{
    ParamPoller::attach (this);
    setTooltip (p.name);
    setRepaintsOnMouseActivity (false);
}

ParamControl::~ParamControl() { ParamPoller::detach (this); }

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
    if (param.type != ParamType::Continuous) return;
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
    return r.reduced (0.0f, 14.0f);
}

void Fader::press (const juce::MouseEvent& e)
{
    dragStartNorm = param.getNormalised();
    // Click on the track (not the thumb) jumps there.
    const auto t = track();
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
    param.setNormalised (dragStartNorm - (float) e.getDistanceFromDragStartY() * sens / track().getHeight());
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

void Fader::paint (juce::Graphics& g)
{
    const auto t = track();
    const float cx = t.getCentreX() + 6.0f;

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

    // Calibrated MIDI range: yellow bracket beside the track
    if (hasCustomMidiRange())
    {
        const float yMin = t.getBottom() - midiMin * t.getHeight();
        const float yMax = t.getBottom() - midiMax * t.getHeight();
        g.setColour (theme::learn.withAlpha (0.8f));
        g.fillRect (cx + 6.0f, juce::jmin (yMin, yMax), 2.0f, std::abs (yMin - yMax));
        g.fillRect (cx + 6.0f, yMax - 1.0f, 6.0f, 2.0f);
        g.fillRect (cx + 6.0f, yMin - 1.0f, 6.0f, 2.0f);
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

void Knob::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    auto textArea = r.removeFromBottom (14.0f);
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
    g.setColour (theme::bg);
    g.strokePath (bgArc, juce::PathStrokeType (3.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    valArc.addCentredArc (c.x, c.y, rad - 2.0f, rad - 2.0f, 0.0f, juce::jmin (startAng, ang), juce::jmax (startAng, ang), true);
    g.setColour (accent);
    g.strokePath (valArc, juce::PathStrokeType (3.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const float inner = rad - 7.0f;
    g.setColour (theme::raised);
    g.fillEllipse (c.x - inner, c.y - inner, inner * 2.0f, inner * 2.0f);
    g.setColour (theme::text);
    const juce::Point<float> tip (c.x + std::sin (ang) * (inner - 2.0f), c.y - std::cos (ang) * (inner - 2.0f));
    g.drawLine ({ c.getPointOnCircumference (inner * 0.25f, ang), tip }, 2.0f);

    if (isEditingText()) return;
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
        case Icon::none:
        default:
            break;
    }
}

void Toggle::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    const bool on = choiceValue >= 0 ? juce::roundToInt (param.get()) == choiceValue : param.isOn();
    g.setColour (on ? onColour : (hover ? theme::raised.brighter (0.08f) : theme::raised));
    g.fillRoundedRectangle (r, 5.0f);
    if (! on)
    {
        g.setColour (theme::outline);
        g.drawRoundedRectangle (r, 5.0f, 1.0f);
    }
    if (icon != Icon::none)
    {
        // Off: the symbol in its colour (record red, play green); on: dark symbol on the coloured button.
        const auto colour = on ? theme::bg : icon == Icon::stop ? (hover ? theme::text : theme::textDim)
                                                                : onColour.withAlpha (hover ? 1.0f : 0.85f);
        drawIcon (g, icon, on, r.withSizeKeepingCentre (juce::jmin (r.getHeight(), 22.0f), juce::jmin (r.getHeight(), 22.0f)), colour);
        return;
    }
    g.setColour (on ? theme::bg : theme::textDim);
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

void Switch::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    const float h = juce::jmin (22.0f, r.getHeight() - 2.0f);
    theme::drawSwitch (g, r.withSizeKeepingCentre (r.getWidth(), h), param.isOn(), onColour, hover);
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

//==============================================================================
void Chip::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (active ? colour.withAlpha (0.22f) : (hover ? theme::raised.brighter (0.06f) : theme::panel2));
    g.fillRoundedRectangle (r, 6.0f);
    bool offline = false;
    for (auto& s : status) offline |= s.kind == Marker::offline;
    g.setColour (offline ? theme::mute.withAlpha (0.7f) : ! status.empty() ? theme::warn.withAlpha (0.8f)
                 : active ? colour.withAlpha (0.8f) : theme::outline);
    g.drawRoundedRectangle (r, 6.0f, 1.0f);

    auto tr = r.reduced (7.0f, 0.0f);
    if (showArrow)
    {
        auto ar = tr.removeFromRight (8.0f);
        juce::Path p;
        const float cy = ar.getCentreY();
        p.addTriangle (ar.getX(), cy - 2.0f, ar.getRight(), cy - 2.0f, ar.getCentreX(), cy + 2.5f);
        g.setColour (theme::textDim);
        g.fillPath (p);
        tr.removeFromRight (3.0f);
    }
    // Status dots left of the name (red first)
    markerAreas.clear();
    for (auto& st : status)
    {
        auto m = tr.removeFromLeft (9.0f).withSizeKeepingCentre (9.0f, 9.0f);
        drawMarker (g, st.kind, m);
        markerAreas.push_back (m.expanded (3.0f, 6.0f));
        tr.removeFromLeft (4.0f);
    }
    if (icon != Icon::none)
    {
        const float s = juce::jmin (14.0f, tr.getHeight() - 6.0f);
        auto ir = text.isEmpty() ? tr.withSizeKeepingCentre (s, s) : tr.removeFromLeft (s).withSizeKeepingCentre (s, s);
        drawIcon (g, icon, ir, active ? theme::text : colour);
        if (text.isEmpty()) return;
        tr.removeFromLeft (6.0f);
        if (centred) tr.removeFromRight (s + 6.0f);   // keep the text centred under the whole chip
    }
    g.setColour (active ? theme::text : colour);
    g.setFont (theme::font (12.0f));
    g.drawFittedText (text, tr.toNearestInt(), centred ? juce::Justification::centred : juce::Justification::centredLeft, 1, 0.8f);
}

void Chip::setStatus (std::vector<Status> s)
{
    // One dot per colour: dropouts and "needs attention" are both yellow and share a dot (and its tooltip).
    std::vector<Status> merged;
    for (int pass = 0; pass < 2; ++pass)
    {
        const bool red = pass == 0;
        Status dot { red ? Marker::offline : Marker::attention, {} };
        bool any = false;
        for (auto& x : s)
            if ((x.kind == Marker::offline) == red)
            {
                if (any) dot.tip << "\n\n";
                dot.tip << x.tip;
                if (x.kind == Marker::dropouts && ! red) dot.kind = Marker::dropouts;
                any = true;
            }
        if (any) merged.push_back (std::move (dot));
    }

    auto same = [] (const std::vector<Status>& a, const std::vector<Status>& b)
    {
        if (a.size() != b.size()) return false;
        for (size_t k = 0; k < a.size(); ++k)
            if (a[k].kind != b[k].kind || a[k].tip != b[k].tip) return false;
        return true;
    };
    if (same (merged, status)) return;
    const bool symbolsChanged = merged.size() != status.size()
                             || ! std::equal (merged.begin(), merged.end(), status.begin(), [] (auto& a, auto& b) { return a.kind == b.kind; });
    status = std::move (merged);
    if (symbolsChanged) repaint();
}

juce::String Chip::getTooltip()
{
    // Over a dot: just what it means. Elsewhere: the problems first, then the chip's own tooltip.
    const auto p = getMouseXYRelative().toFloat();
    for (size_t k = 0; k < markerAreas.size() && k < status.size(); ++k)
        if (markerAreas[k].contains (p)) return status[k].tip;
    juce::String tip;
    for (auto& st : status) tip << st.tip << "\n\n";
    return tip + juce::SettableTooltipClient::getTooltip();
}

void Chip::drawMarker (juce::Graphics& g, Marker kind, juce::Rectangle<float> r)
{
    // Red dot = offline, yellow dot = dropouts / needs attention, with a faint halo so it reads on any chip.
    const auto c = kind == Marker::offline ? theme::mute : theme::warn;
    const auto d = juce::jmin (r.getWidth(), r.getHeight());
    const auto dot = juce::Rectangle<float> (d, d).withCentre (r.getCentre());
    g.setColour (c.withAlpha (0.25f));
    g.fillEllipse (dot.expanded (1.5f));
    g.setColour (c);
    g.fillEllipse (dot.reduced (0.5f));
}

void Chip::drawIcon (juce::Graphics& g, Icon icon, juce::Rectangle<float> r, juce::Colour colour)
{
    const juce::PathStrokeType stroke (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    g.setColour (colour);
    if (icon == Icon::save)
    {
        // Floppy disk: body with a cut corner, shutter at the top, label at the bottom.
        auto b = r.reduced (0.5f);
        const float cut = b.getWidth() * 0.24f;
        juce::Path body;
        body.startNewSubPath (b.getX(), b.getY());
        body.lineTo (b.getRight() - cut, b.getY());
        body.lineTo (b.getRight(), b.getY() + cut);
        body.lineTo (b.getRight(), b.getBottom());
        body.lineTo (b.getX(), b.getBottom());
        body.closeSubPath();
        g.strokePath (body, stroke);
        g.fillRect (juce::Rectangle<float> (b.getX() + b.getWidth() * 0.26f, b.getY(), b.getWidth() * 0.4f, b.getHeight() * 0.3f));
        g.drawRoundedRectangle (juce::Rectangle<float> (b.getX() + b.getWidth() * 0.2f, b.getY() + b.getHeight() * 0.56f,
                                                        b.getWidth() * 0.6f, b.getHeight() * 0.44f), 1.0f, 1.2f);
    }
    else if (icon == Icon::revert)
    {
        // Counter-clockwise arrow (back to the stored state).
        const auto c = r.getCentre();
        const float rad = r.getWidth() * 0.4f;
        juce::Path arc;
        arc.addCentredArc (c.x, c.y, rad, rad, 0.0f, -juce::MathConstants<float>::pi * 0.35f,
                           juce::MathConstants<float>::pi * 1.25f, true);
        g.strokePath (arc, stroke);
        // Arrow head at the start of the arc, pointing against the drawing direction (= counter-clockwise).
        const float sa = -juce::MathConstants<float>::pi * 0.35f;
        const juce::Point<float> start (c.x + rad * std::sin (sa), c.y - rad * std::cos (sa));
        const juce::Point<float> dir (-std::cos (sa), -std::sin (sa));   // tangent, counter-clockwise
        const juce::Point<float> nrm (dir.y, -dir.x);
        juce::Path head;
        const float s = r.getWidth() * 0.26f;
        head.addTriangle (start + dir * s, start - nrm * (s * 0.8f), start + nrm * (s * 0.8f));
        g.fillPath (head);
    }
}
//==============================================================================
void TrashButton::drawIcon (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour)
{
    // Trash can: lid with handle, tapered bin with three ribs.
    const float s = juce::jmin (area.getWidth(), area.getHeight());
    auto r = juce::Rectangle<float> (s * 0.8f, s).withCentre (area.getCentre());
    const float stroke = juce::jmax (1.2f, s * 0.09f);
    const juce::PathStrokeType pst (stroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    juce::Path p;
    const float lidY = r.getY() + r.getHeight() * 0.2f;
    p.startNewSubPath (r.getX(), lidY);
    p.lineTo (r.getRight(), lidY);
    const float hw = r.getWidth() * 0.18f;
    p.startNewSubPath (r.getCentreX() - hw, lidY);
    p.lineTo (r.getCentreX() - hw, r.getY() + stroke * 0.5f);
    p.lineTo (r.getCentreX() + hw, r.getY() + stroke * 0.5f);
    p.lineTo (r.getCentreX() + hw, lidY);
    const float top = lidY + stroke * 0.9f, bottom = r.getBottom() - stroke * 0.5f;
    const float inset = r.getWidth() * 0.12f, taper = r.getWidth() * 0.06f;
    p.startNewSubPath (r.getX() + inset, top);
    p.lineTo (r.getX() + inset + taper, bottom);
    p.lineTo (r.getRight() - inset - taper, bottom);
    p.lineTo (r.getRight() - inset, top);
    for (float f : { 0.36f, 0.5f, 0.64f })
    {
        const float x = r.getX() + r.getWidth() * f;
        p.startNewSubPath (x, top + r.getHeight() * 0.14f);
        p.lineTo (x, bottom - r.getHeight() * 0.14f);
    }
    g.setColour (colour);
    g.strokePath (p, pst);
}

void TrashButton::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    const bool hot = hover && isEnabled();
    g.setColour (hot ? theme::mute.withAlpha (0.2f) : theme::panel2);
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (hot ? theme::mute.withAlpha (0.8f) : theme::outline);
    g.drawRoundedRectangle (r, 6.0f, 1.0f);
    drawIcon (g, r.withSizeKeepingCentre (14.0f, 14.0f), hot ? theme::mute : isEnabled() ? theme::textDim : theme::textFaint.withAlpha (0.5f));
}

//==============================================================================
void PlusButton::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    const float corner = juce::jmin (8.0f, r.getHeight() * 0.3f);
    g.setColour (hover ? colour.withAlpha (0.22f) : theme::panel2);
    g.fillRoundedRectangle (r, corner);
    g.setColour (colour.withAlpha (hover ? 0.9f : 0.45f));
    g.drawRoundedRectangle (r, corner, 1.0f);

    const auto c = r.getCentre();
    const float s = juce::jmin (r.getWidth(), r.getHeight()) * 0.22f;
    g.setColour (hover ? theme::text : colour);
    g.fillRoundedRectangle (c.x - s, c.y - 1.0f, s * 2.0f, 2.0f, 1.0f);
    g.fillRoundedRectangle (c.x - 1.0f, c.y - s, 2.0f, s * 2.0f, 1.0f);
}
} // namespace hydra

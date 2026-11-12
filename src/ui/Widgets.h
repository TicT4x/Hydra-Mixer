#pragma once
#include "Controls.h"
#include "engine/Engine.h"
#include "midi/MidiMapper.h"
#include "midi/Hotkeys.h"

namespace hydra
{
class Session;
class UndoHistory;
class Updater;

/** Inspector selection index of the recorder (after the input slots and the bus slots). */
constexpr int kRecorderSelect = kMaxInputs + kMaxBuses;

/** Selection index scheme: 0..kMaxInputs-1 = inputs, then the buses (A1..A8, B1..B8), then the recorder. */
constexpr int busSelectIndex (int bus) noexcept { return kMaxInputs + bus; }

/** "IN 3", "A2", "B1" for a selection index. */
inline juce::String channelCode (int selectIndex)
{
    return selectIndex < kMaxInputs ? "IN " + juce::String (selectIndex + 1) : busLabel (selectIndex - kMaxInputs);
}

/** Shared UI state handed to every widget. */
struct UiContext
{
    Engine& engine;
    MidiMapper& midi;
    Hotkeys& hotkeys;
    Session* session = nullptr;
    UndoHistory* undo = nullptr;   // undo / redo of all parameter changes (Ctrl+Z / Ctrl+Y)
    bool learnMode = false;
    std::function<void()> learnModeChanged;
    /** The window glides to a new size: layouts follow directly instead of animating on their own. */
    bool instantLayout = false;

    /** Selected channel for the inspector (see busSelectIndex / kRecorderSelect). */
    int selected = 0;
    std::function<void (int)> select;

    /** Waits for the next key press and assigns it as keyboard shortcut to the parameter. */
    std::function<void (Param&)> armHotkey;

    /** Opens a floating sub-panel (advanced settings) above the inspector. */
    std::function<void (std::unique_ptr<juce::Component>, juce::String title)> openPanel;

    /** Opens the large EQ editor window for EQ instance k of a channel (same index scheme as 'selected'). */
    std::function<void (int, int)> openEqWindow;

    /** Opens the device selection window of a channel (same index scheme): input source / bus output. */
    std::function<void (int)> openDevicePicker;
    std::function<void()> openCableManager;   // Virtual Audio Cable: add / edit / remove cables

    /** Application preferences in session->prefs were changed (tray icon etc.). */
    std::function<void()> prefsChanged;

    /** Quits the application (also when "close to tray" is active). */
    std::function<void()> shutdown;

    /** Asks for confirmation, then removes the channel (selection index) with its settings. */
    std::function<void (int)> removeChannel;

    /** Adds a channel: 0 = input, 1 = hardware output (A), 2 = virtual output (B). */
    std::function<void (int)> addChannel;

    /** Copies the channel to the clipboard and removes it (no question: it can be pasted back). */
    std::function<void (int)> cutChannel;
    /** Adds a copy of the channel (settings, effect chain, name, device) right after it. */
    std::function<void (int)> duplicateChannel;
    /** Pastes a copied channel as a new channel of the group (0 inputs, 1 hardware, 2 virtual outputs). */
    std::function<void (int)> pasteNewChannel;

    /** Opens Tools > Room Measurement; with a hardware output (selection index) it is preset as the speaker
        and as the channel the correction goes to (-1 = keep the window's choice). */
    std::function<void (int)> openRoomMeasurement;

    /** Opens the Correction window of a hardware output (selection index). */
    std::function<void (int)> openCorrectionWindow;

    /** Enters Change Order mode (drag the strips into a new order). */
    std::function<void()> changeOrder;

    /** Updates from GitHub (state/Updater.h), owned by the app; nullptr in tools / tests. */
    Updater* updater = nullptr;
    /** Saves, installs the downloaded update and restarts Hydra (the app does it: it owns the session). */
    std::function<void()> installUpdate;
    /** Looks for updates now (Settings > General > Check now). */
    std::function<void()> checkForUpdates;

    /** EQ of a channel (selection index), or nullptr for the recorder. */
    ParamEq* eqFor (int selectIndex) const
    {
        if (selectIndex < 0 || selectIndex >= kRecorderSelect) return nullptr;
        return selectIndex < kMaxInputs ? &engine.inputs[(size_t) selectIndex].eq : &engine.buses[(size_t) (selectIndex - kMaxInputs)].eq;
    }

    /** Ctrl+Z (undo) / Ctrl+Y, Ctrl+Shift+Z (redo) of the last parameter change anywhere in the mixer.
        Returns true if the key was one of them; message describes what was undone / redone. */
    bool handleUndoKey (const juce::KeyPress& key, juce::String* message = nullptr);
};



//==============================================================================
/** Parses typed text ("-6", "2.5k", "120 ms", "-inf") into a parameter value. Returns false if unusable. */
bool setParamFromText (Param& p, const juce::String& text);

/** Base for every parameter-bound control: right-click -> MIDI learn / clear / reset / keyboard shortcut.
    In global learn mode a left click arms the control instead. */
class ParamControl : public juce::Component,
                     public ParamPoller::Client,
                     public juce::SettableTooltipClient
{
public:
    ParamControl (UiContext& c, Param& p);
    ~ParamControl() override;

    Param& param;

    void pollParams() override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) final;
    void mouseDrag (const juce::MouseEvent&) final;
    void mouseUp (const juce::MouseEvent&) final;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    /** Tooltip made when it is shown (names that change, icons::routeTip); the keyboard shortcut is appended. */
    std::function<juce::String()> tooltipText;
    juce::String getTooltip() override;

    /** The mouse wheel scrolled a pane of channels (strips, effect rack): controls under the mouse leave the
        wheel to the pane for a moment (see mouseWheelMove). */
    static void notePaneScrolled();

protected:
    virtual void press (const juce::MouseEvent&) {}
    virtual void drag (const juce::MouseEvent&) {}
    virtual void release (const juce::MouseEvent&) {}
    virtual float wheelStep() const { return 0.02f; }

    /** Draws learn highlight / mapping badge on top of the control. */
    void paintOverChildren (juce::Graphics&) override;
    bool isArmed() const noexcept { return armed; }
    /** Shows an inline text field over area to type a value (Enter = apply, Esc = cancel). */
    void editValueAsText (juce::Rectangle<int> area);
    bool isEditingText() const noexcept { return valueEditor != nullptr; }
    bool hasCustomMidiRange() const noexcept { return mapped && (midiMin != 0.0f || midiMax != 1.0f); }
    float midiMin = 0.0f, midiMax = 1.0f; // calibrated MIDI range (normalised)
    UiContext& ctx;

private:
    void showMenu();
    void closeValueEditor (bool apply);
    std::unique_ptr<juce::TextEditor> valueEditor;
    juce::Rectangle<int> valueEditorArea;
    juce::String baseTooltip;
    /** Where the mouse really moved onto this control (content scrolled under a resting mouse does not count). */
    struct HoverClock final : juce::MouseListener
    {
        void mouseEnter (const juce::MouseEvent& e) override { track (e); }
        void mouseMove (const juce::MouseEvent& e) override  { track (e); }
        void mouseExit (const juce::MouseEvent&) override    { since = 0.0; }
        void track (const juce::MouseEvent& e);
        double since = 0.0;   // ms (hi-res counter) of the first real movement inside; 0 = not yet
    } hoverClock;
    bool takesWheel (const juce::MouseEvent&);
    uint32_t seenVersion = 0, seenMapVersion = ~0u, seenHotkeyVersion = ~0u;
    bool armed = false, mapped = false, learnShown = false, swallowGesture = false;
};

//==============================================================================
class Fader final : public ParamControl
{
public:
    Fader (UiContext& c, Param& p, juce::Colour accent);
    void paint (juce::Graphics&) override;
    void pollParams() override;
    /** Sideways fader (horizontal channel rows): low values at the left, the scale below the track. */
    bool horizontal = false;

private:
    void press (const juce::MouseEvent&) override;
    void drag (const juce::MouseEvent&) override;
    float wheelStep() const override { return 0.01f; }
    juce::Rectangle<float> track() const;
    void frame (double timestamp);
    void paintHorizontal (juce::Graphics&);

    juce::Colour accent;
    float dragStartNorm = 0;
    float shown = -1.0f;        // displayed position; glides towards the parameter (MIDI, wheel, reset)
    bool animating = false;
    double lastFrame = 0.0;
    juce::VBlankAttachment vblank { this, [this] (double t) { frame (t); } };
};

//==============================================================================
class Knob final : public ParamControl
{
public:
    Knob (UiContext& c, Param& p, juce::String label, juce::Colour accent);
    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { hover = false; repaint(); }

    /** Label above the knob and the value always shown below it (click it to type a value). */
    bool alwaysShowValue = false;
    /** Lights up the knob's arc track in its colour, 0..1 (a level, e.g. the Stems block). Negative = a plain knob.
        Repaints only when the value changed visibly. */
    void setGlow (float v);

private:
    void press (const juce::MouseEvent&) override;
    void drag (const juce::MouseEvent&) override;
    void release (const juce::MouseEvent&) override { dragging = false; repaint(); }
    juce::Rectangle<int> valueArea() const { return getLocalBounds().removeFromBottom (alwaysShowValue ? 16 : 14); }

    juce::String label;
    juce::Colour accent;
    float glow = -1.0f;
    float dragStartNorm = 0;
    bool hover = false, dragging = false;
};

//==============================================================================
class Toggle final : public ParamControl, private juce::Timer
{
public:
    /** Symbol drawn instead of the text (transport buttons). playPause shows pause while the parameter is on. */
    enum class Icon { none, record, playPause, stop, loop, phase };

    Toggle (UiContext& c, Param& p, juce::String text, juce::Colour onColour, Icon icon = Icon::none);
    void paint (juce::Graphics&) override;
    void pollParams() override;
    static void drawIcon (juce::Graphics&, Icon, bool on, juce::Rectangle<float> area, juce::Colour);
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { hover = false; repaint(); }
    void mouseDoubleClick (const juce::MouseEvent&) override {}

    juce::String text;
    juce::String glyph;   // ui/ChannelIcons.h id: drawn instead of the text (routing buttons of buses with an icon)
    juce::Colour onColour;
    /** -1: on/off parameter. Otherwise one value of a choice parameter: lit while the parameter has this
        value, a click selects it (a row of such toggles works like radio buttons). */
    bool soft = false;      // Deep theme: "on" is a strong tint with a frame and white text instead of a full colour (routing, LIM)
    int choiceValue = -1;
    Icon icon;
    /** Solo-like: switching it on with a click switches these others off; with Shift held, they stay (several at
        once). Only mouse clicks (MIDI and hotkeys toggle just this one). */
    std::function<std::vector<Param*>()> exclusiveGroup;

private:
    void press (const juce::MouseEvent& e) override
    {
        if (choiceValue >= 0) { param.set ((float) choiceValue); return; }
        const bool turnsOn = ! param.isOn();
        param.toggle();
        if (turnsOn && exclusiveGroup && ! e.mods.isShiftDown())
            for (auto* p : exclusiveGroup())
                if (p != &param && p->isOn()) p->set (0.0f);
    }
    bool isLit() const { return choiceValue >= 0 ? juce::roundToInt (param.get()) == choiceValue : param.isOn(); }
    void timerCallback() override;
    bool hover = false;
    float shown = -1.0f;   // 0 = off .. 1 = on: glides for ~100 ms after a change (timer only runs meanwhile)
};

//==============================================================================
/** On/off parameter shown as an oval switch with a label to its right. */
class Switch final : public ParamControl, private juce::Timer
{
public:
    Switch (UiContext& c, Param& p, juce::String text, juce::Colour onColour);
    void paint (juce::Graphics&) override;
    void pollParams() override;
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { hover = false; repaint(); }
    void mouseDoubleClick (const juce::MouseEvent&) override {}

private:
    void press (const juce::MouseEvent&) override { param.toggle(); }
    void timerCallback() override;
    juce::String text;
    juce::Colour onColour;
    bool hover = false;
    float shown = -1.0f;   // see Toggle
};

//==============================================================================
/** Numeric value in a box (e.g. "12.5 ms"): drag up / down to change (Shift = fine), click to type a value. */
class ValueBox final : public ParamControl
{
public:
    ValueBox (UiContext& c, Param& p, juce::Colour accent);
    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { hover = false; repaint(); }

private:
    void press (const juce::MouseEvent&) override;
    void drag (const juce::MouseEvent&) override;
    void release (const juce::MouseEvent&) override;
    float wheelStep() const override { return 0.002f; }
    juce::Colour accent;
    float dragStartNorm = 0;
    bool hover = false;
};

//==============================================================================
/** Segmented selector for a choice parameter (e.g. denoiser latency mode). */
class ChoiceSegments final : public ParamControl
{
public:
    /** values: parameter value of each segment (default: segment i = value i), so only some choices can be offered. */
    ChoiceSegments (UiContext& c, Param& p, juce::Colour accent, juce::StringArray labels = {}, std::vector<int> values = {});
    int getNumSegments() const noexcept { return labels.size(); }
    void paint (juce::Graphics&) override;

private:
    void press (const juce::MouseEvent&) override;
    int valueOf (int segment) const;
    juce::Colour accent;
    juce::StringArray labels;
    std::vector<int> values;
};

/** Vertical filter-type picker for one EQ band (bell, notch, band pass, cuts, shelves). */
class EqTypeBar final : public ParamControl
{
public:
    EqTypeBar (UiContext& c, Param& typeParam, juce::Colour accent);
    void paint (juce::Graphics&) override;
    static void drawIcon (juce::Graphics&, int type, juce::Rectangle<float> r, juce::Colour);

private:
    void press (const juce::MouseEvent&) override;
    juce::Colour accent;
};

//==============================================================================
/** Stereo peak meter with hold. update() is driven by the main UI timer. */
class MeterView final : public juce::Component
{
public:
    explicit MeterView (Meter& m, bool compactStyle = false) : meter (m), compact (compactStyle)
    {
        setInterceptsMouseClicks (false, false);
        setOpaque (false);
    }

    void update (float dtSeconds);
    void paint (juce::Graphics&) override;
    /** Bars from left to right (one above the other) instead of bottom to top. */
    bool horizontal = false;

private:
    static float toPos (float db) noexcept { return juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 66.0f); }

    Meter& meter;
    bool compact;
    float level[2] { -100.f, -100.f }, hold[2] { -100.f, -100.f }, holdTime[2] {};
    float shown[2] { -100.f, -100.f }, shownHold[2] { -100.f, -100.f };
    bool dimmed = false; // channel muted: level drawn grey
};

} // namespace hydra

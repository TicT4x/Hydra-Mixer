#pragma once
#include "Theme.h"
#include "Dialog.h"
#include "engine/Engine.h"
#include "midi/MidiMapper.h"
#include "midi/Hotkeys.h"

namespace hydra
{
class Session;
class UndoHistory;

/** Inspector selection index of the recorder (after the input slots and the bus slots). */
constexpr int kRecorderSelect = kMaxInputs + kMaxBuses;

/** Selection index scheme: 0..kMaxInputs-1 = inputs, then the buses (A1..A8, B1..B8), then the recorder. */
constexpr int busSelectIndex (int bus) noexcept { return kMaxInputs + bus; }

/** "IN 3", "A2", "B1" for a selection index. */
inline juce::String channelCode (int selectIndex)
{
    return selectIndex < kMaxInputs ? "IN " + juce::String (selectIndex + 1) : busCode (selectIndex - kMaxInputs);
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

    /** Selected channel for the inspector (see busSelectIndex / kRecorderSelect). */
    int selected = 0;
    std::function<void (int)> select;

    /** Waits for the next key press and assigns it as keyboard shortcut to the parameter. */
    std::function<void (Param&)> armHotkey;

    /** Opens a floating sub-panel (advanced settings) above the inspector. */
    std::function<void (std::unique_ptr<juce::Component>, juce::String title, juce::Colour)> openPanel;

    /** Opens the large EQ editor window for a channel (same index scheme as 'selected'). */
    std::function<void (int)> openEqWindow;

    /** Opens the device selection window of a channel (same index scheme): input source / bus output. */
    std::function<void (int)> openDevicePicker;

    /** Application preferences in session->prefs were changed (tray icon etc.). */
    std::function<void()> prefsChanged;

    /** Quits the application (also when "close to tray" is active). */
    std::function<void()> shutdown;

    /** Asks for confirmation, then removes the channel (selection index) with its settings. */
    std::function<void (int)> removeChannel;

    /** Adds a channel: 0 = input, 1 = hardware output (A), 2 = virtual output (B). */
    std::function<void (int)> addChannel;

    /** Enters Change Order mode (drag the strips into a new order). */
    std::function<void()> changeOrder;

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

private:
    void press (const juce::MouseEvent&) override;
    void drag (const juce::MouseEvent&) override;
    float wheelStep() const override { return 0.01f; }
    juce::Rectangle<float> track() const;
    void frame (double timestamp);

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

private:
    void press (const juce::MouseEvent&) override;
    void drag (const juce::MouseEvent&) override;
    void release (const juce::MouseEvent&) override { dragging = false; repaint(); }
    juce::Rectangle<int> valueArea() const { return getLocalBounds().removeFromBottom (14); }

    juce::String label;
    juce::Colour accent;
    float dragStartNorm = 0;
    bool hover = false, dragging = false;
};

//==============================================================================
class Toggle final : public ParamControl
{
public:
    /** Symbol drawn instead of the text (transport buttons). playPause shows pause while the parameter is on. */
    enum class Icon { none, record, playPause, stop, loop };

    Toggle (UiContext& c, Param& p, juce::String text, juce::Colour onColour, Icon icon = Icon::none);
    void paint (juce::Graphics&) override;
    static void drawIcon (juce::Graphics&, Icon, bool on, juce::Rectangle<float> area, juce::Colour);
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { hover = false; repaint(); }
    void mouseDoubleClick (const juce::MouseEvent&) override {}

    juce::String text;
    juce::Colour onColour;
    /** -1: on/off parameter. Otherwise one value of a choice parameter: lit while the parameter has this
        value, a click selects it (a row of such toggles works like radio buttons). */
    int choiceValue = -1;
    Icon icon;

private:
    void press (const juce::MouseEvent&) override
    {
        if (choiceValue >= 0) param.set ((float) choiceValue);
        else                  param.toggle();
    }
    bool hover = false;
};

//==============================================================================
/** On/off parameter shown as an oval switch with a label to its right. */
class Switch final : public ParamControl
{
public:
    Switch (UiContext& c, Param& p, juce::String text, juce::Colour onColour);
    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { hover = false; repaint(); }
    void mouseDoubleClick (const juce::MouseEvent&) override {}

private:
    void press (const juce::MouseEvent&) override { param.toggle(); }
    juce::String text;
    juce::Colour onColour;
    bool hover = false;
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

private:
    static float toPos (float db) noexcept { return juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 66.0f); }

    Meter& meter;
    bool compact;
    float level[2] { -100.f, -100.f }, hold[2] { -100.f, -100.f }, holdTime[2] {};
    float shown[2] { -100.f, -100.f }, shownHold[2] { -100.f, -100.f };
    bool dimmed = false; // channel muted: level drawn grey
};

//==============================================================================
/** Clickable text chip (source/output selectors, header buttons). */
class Chip : public juce::Component, public juce::SettableTooltipClient
{
public:
    Chip (juce::String t = {}, juce::Colour c = theme::textDim) : text (std::move (t)), colour (c) {}
    std::function<void()> onClick;
    juce::String text;
    juce::Colour colour;
    bool active = false, showArrow = true, centred = false;

    /** Symbol drawn left of the text (or alone, centred, when the text is empty). */
    enum class Icon { none, save, revert };
    Icon icon = Icon::none;
    static void drawIcon (juce::Graphics&, Icon, juce::Rectangle<float> area, juce::Colour);

    /** Status dots left of the name (device problems): red = offline, yellow = dropouts or something that
        needs attention. Problems of the same colour share one dot. The chip's tooltip starts with what the
        dots mean (dropouts: how many and when). */
    enum class Marker { attention, offline, dropouts };
    struct Status { Marker kind; juce::String tip; };
    /** Replaces the status dots; repaints only if they changed. */
    void setStatus (std::vector<Status>);
    const std::vector<Status>& getStatus() const noexcept { return status; }
    static void drawMarker (juce::Graphics&, Marker, juce::Rectangle<float> area);

    void paint (juce::Graphics&) override;
    juce::String getTooltip() override;
    void mouseUp (const juce::MouseEvent& e) override { if (e.mouseWasClicked() && onClick) onClick(); }
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { hover = false; repaint(); }

protected:
    bool hover = false;

private:
    std::vector<Status> status;
    std::vector<juce::Rectangle<float>> markerAreas;   // where each status symbol was drawn
};

/** Chip with a trash-can icon (remove a plugin, delete a recording); turns red on hover, dimmed when disabled. */
class TrashButton final : public Chip
{
public:
    TrashButton() { setMouseCursor (juce::MouseCursor::PointingHandCursor); }
    void paint (juce::Graphics&) override;
    static void drawIcon (juce::Graphics&, juce::Rectangle<float> area, juce::Colour);
};

//==============================================================================
/** Round-cornered "+" button (add a channel, add a plugin). */
class PlusButton final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit PlusButton (juce::Colour c = theme::textDim) : colour (c) { setMouseCursor (juce::MouseCursor::PointingHandCursor); }
    std::function<void()> onClick;
    juce::Colour colour;

    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent& e) override { if (e.mouseWasClicked() && onClick) onClick(); }
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { hover = false; repaint(); }

private:
    bool hover = false;
};
} // namespace hydra

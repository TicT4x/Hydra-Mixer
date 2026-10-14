#pragma once
#include "Theme.h"
#include "engine/Engine.h"
#include "midi/MidiMapper.h"
#include "midi/Hotkeys.h"

namespace hydra
{
class Session;

/** Inspector selection index of the recorder (after the 8 inputs and 8 buses). */
constexpr int kRecorderSelect = kNumInputs + kNumBuses;

/** Shared UI state handed to every widget. */
struct UiContext
{
    Engine& engine;
    MidiMapper& midi;
    Hotkeys& hotkeys;
    Session* session = nullptr;
    bool learnMode = false;
    std::function<void()> learnModeChanged;

    /** Selected channel for the inspector: 0..7 = inputs, 8..15 = buses, 16 = recorder. */
    int selected = 0;
    std::function<void (int)> select;

    /** Waits for the next key press and assigns it as keyboard shortcut to the parameter. */
    std::function<void (Param&)> armHotkey;

    /** Opens a floating sub-panel (advanced settings) above the inspector. */
    std::function<void (std::unique_ptr<juce::Component>, juce::String title, juce::Colour)> openPanel;

    /** Opens the large EQ editor window for a channel (same index scheme as 'selected'). */
    std::function<void (int)> openEqWindow;

    /** Application preferences in session->prefs were changed (tray icon etc.). */
    std::function<void()> prefsChanged;

    /** Quits the application (also when "close to tray" is active). */
    std::function<void()> shutdown;
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
    Toggle (UiContext& c, Param& p, juce::String text, juce::Colour onColour);
    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { hover = false; repaint(); }
    void mouseDoubleClick (const juce::MouseEvent&) override {}

    juce::String text;
    juce::Colour onColour;

private:
    void press (const juce::MouseEvent&) override { param.toggle(); }
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
/** Segmented selector for a choice parameter (e.g. denoiser latency mode). */
class ChoiceSegments final : public ParamControl
{
public:
    ChoiceSegments (UiContext& c, Param& p, juce::Colour accent, juce::StringArray labels = {});
    void paint (juce::Graphics&) override;

private:
    void press (const juce::MouseEvent&) override;
    juce::Colour accent;
    juce::StringArray labels;
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

    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent& e) override { if (e.mouseWasClicked() && onClick) onClick(); }
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { hover = false; repaint(); }

private:
    bool hover = false;
};
} // namespace hydra

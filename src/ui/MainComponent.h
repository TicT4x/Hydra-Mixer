#pragma once
#include "Strips.h"
#include "Inspector.h"
#include "Settings.h"
#include "LatencyView.h"
#include "DspView.h"
#include "SyncDelayView.h"
#include "DevicePicker.h"
#include "SceneEditor.h"
#include "state/UndoHistory.h"

namespace hydra
{
/** Compact recorder transport in the top bar; clicking the time shows the recorder in the inspector. */
class RecorderMini final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit RecorderMini (UiContext& c);
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;
    void update();

private:
    UiContext& ctx;
    Toggle rec, play, stop;
    juce::String shown;
    bool shownActive = false;
};

/** Flat icon button for the top bar: tools (wrench), recorder (cassette), settings gear, power (shutdown)
    and the window controls. The window controls do not act on clicks themselves: Windows reports them
    as native caption buttons (see TopBar::windowControlAt), and the window receives minimise / maximise / close. */
class IconButton final : public juce::Component, public juce::SettableTooltipClient
{
public:
    enum class Kind { gear, power, wrench, cassette, minimise, maximise, close };
    explicit IconButton (Kind k) : kind (k) {}

    std::function<void()> onClick;
    const Kind kind;
    /** Highlight (learn mode for the tools, recording / playing for the cassette); transparent = none. */
    juce::Colour lit { juce::Colours::transparentBlack };
    void setLit (juce::Colour c) { if (c != lit) { lit = c; repaint(); } }

    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { hover = false; down = false; repaint(); }
    void mouseDown (const juce::MouseEvent&) override  { down = true; repaint(); }
    void mouseUp (const juce::MouseEvent& e) override
    {
        down = false;
        repaint();
        if (e.mouseWasClicked() && onClick) onClick();
    }
    bool isWindowControl() const noexcept { return kind == Kind::minimise || kind == Kind::maximise || kind == Kind::close; }

private:
    bool hover = false, down = false;
};

/** DSP load meter in the top bar (bar + percentage); click opens the DSP Load window. */
class DspMeter final : public juce::Component, public juce::SettableTooltipClient
{
public:
    DspMeter();
    std::function<void()> onClick;
    void setLoad (float load);
    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { hover = false; repaint(); }
    void mouseUp (const juce::MouseEvent& e) override  { if (e.mouseWasClicked() && onClick) onClick(); }

private:
    float load = 0;
    bool hover = false;
};

/** Engine status in the top bar (driver, device, rate, buffer, I/O latency); click opens the audio engine settings. */
class StatusLink final : public juce::Component, public juce::SettableTooltipClient
{
public:
    StatusLink();
    std::function<void()> onClick;
    void setStatus (const juce::String& text, bool running, bool learnMode);
    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { hover = false; repaint(); }
    void mouseUp (const juce::MouseEvent& e) override  { if (e.mouseWasClicked() && onClick) onClick(); }

private:
    juce::String text;
    bool running = false, learn = false, hover = false;
};

/** Top bar (also the window's title bar): logo, engine status, DSP load, MIDI activity, recorder, tools menu,
    settings, shutdown and the window controls. When the window gets narrow, the recorder shrinks to a
    cassette button, then the logo to its icon, then the status text disappears (the DSP meter stays). */
class TopBar final : public juce::Component
{
public:
    explicit TopBar (UiContext& c);
    void paint (juce::Graphics&) override;
    void resized() override;
    void update();
    /** Native hit-testing: what the point (local coordinates) is for the window (caption, buttons...). */
    WindowControlKind windowControlAt (juce::Point<float>) const;

    IconButton tools { IconButton::Kind::wrench }, cassette { IconButton::Kind::cassette };
    IconButton settings { IconButton::Kind::gear }, power { IconButton::Kind::power };
    IconButton minimiseBtn { IconButton::Kind::minimise }, maximiseBtn { IconButton::Kind::maximise }, closeBtn { IconButton::Kind::close };
    DspMeter dsp;
    StatusLink statusLink;

private:
    UiContext& ctx;
    RecorderMini recorder;
    juce::Rectangle<int> statusArea, logoArea, midiArea;
    bool compactLogo = false;
    juce::String status;
    float cpu = 0, cpuSmooth = 0;
    int cpuTick = 0;
    uint32_t lastActivity = 0;
    float midiGlow = 0;
};

/** Floating container for advanced sub-panels (gate / denoiser / compressor settings). */
class PanelHost final : public juce::Component
{
public:
    PanelHost();
    void show (std::unique_ptr<juce::Component> content, const juce::String& title, juce::Colour colour);
    void close();
    bool isOpen() const noexcept { return content != nullptr; }
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    std::unique_ptr<juce::Component> content;
    juce::String title;
    juce::Colour colour;
    Chip closeButton { "X", theme::text };
};

class MainComponent final : public juce::Component, private juce::Timer, private juce::ChangeListener
{
public:
    MainComponent (Engine& engine, MidiMapper& midi, Hotkeys& hotkeys, Session& session);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    void openSettings (int tab = 0);
    void openMidiMap();
    void openLatencyFinder();
    void openDspView();
    void openSyncDelay();
    /** Device selection window of a channel (input source / bus output); one window, follows the last strip. */
    void openDevicePicker (int channel);
    void toggleLearnMode();
    /** Shows / hides the scene sidebar on the left. */
    void toggleSceneEditor();
    /** Closes and reopens the master device and every other device (same settings), e.g. after a driver
        stopped delivering audio without reporting an error. */
    void restartAudioEngine();
    void openEqWindow (int selectIndex, bool bringToFront = true);
    void refreshAll();
    /** Interface zoom (1 = 100 %): everything in the main window is drawn larger / smaller;
        the strips scroll horizontally when they no longer fit. */
    void setUiScale (float scale);
    /** Rebuilds strips and inspector after the text size changed (labels keep their font otherwise). */
    void fontScaleChanged();
    /** For the window's native hit-testing (point in this component's coordinates). */
    WindowControlKind windowControlAt (juce::Point<float> p) const;

    /** Set by the application: preferences changed / quit requested from the UI. */
    std::function<void()> onPrefsChanged, onShutdown;

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void select (int index);
    bool isChannelShown (int selectIndex) const;
    void rebuildStrips();
    void scrollToChannel (int selectIndex);
    void addChannel (int group);
    void removeChannel (int selectIndex);
    void beginReorder();
    /** Done in Change Order mode: asks what the MIDI assignments / shortcuts of moved channels should do
        (only if there are any), then applies the shown order. */
    void finishReorder();
    void applyReorder (const std::array<std::vector<int>, 3>& order, bool assignmentsFollow);
    void discardReorder();
    /** Size of the main window's contents in unscaled (logical) units. */
public:
    /** Window content width (pixels) at which all channels fit side by side (see StripArea::idealWidth). */
    int idealWidth() const;
private:
    int logicalWidth() const  { return juce::roundToInt ((float) getWidth() / uiScale); }
    int logicalHeight() const { return juce::roundToInt ((float) getHeight() / uiScale); }
    void showToast (const juce::String& text, double seconds);
    void showToolsMenu();
    void meterFrame (double timestamp);
    void finishHotkeyCapture();
    void assignCapturedKey (const juce::KeyPress&);
    bool handleUndoKey (const juce::KeyPress&);
    ToolWindow& makeToolWindow (std::unique_ptr<ToolWindow>& slot, const juce::String& title, juce::Component* content,
                                int w, int h, int buttons = juce::DocumentWindow::closeButton);

    UiContext ctx;
    UndoHistory undo;
    TopBar top;
    StripArea strips;
    Inspector inspector;
    PanelHost panelHost;
    std::unique_ptr<SceneSidebar> sceneBar;   // Tools > Scene Editor
    std::unique_ptr<ToolWindow> settingsWindow, midiWindow, eqWindow, latencyWindow, dspWindow, syncWindow, deviceWindow;
    int shownSelBus = -1;     // bus selected with SEL as shown by the input faders
    uint32_t shownLayout = 0; // engine channel layout the strips were built for
    float uiScale = 1.0f;
    juce::String toast;
    double toastUntil = 0;
    double lastMeterFrame = 0;
    Param* hotkeyTarget = nullptr;   // waiting for a key press to assign
    // Tooltips of every window (main and tool windows); without one JUCE shows none at all.
    juce::TooltipWindow tooltips { nullptr, 600 };
    // Meters follow the display refresh (capped at ~60 Hz) instead of a coarse timer.
    juce::VBlankAttachment vblank { this, [this] (double t) { meterFrame (t); } };
};
} // namespace hydra

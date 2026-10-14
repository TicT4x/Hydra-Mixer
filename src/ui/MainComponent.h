#pragma once
#include "Strips.h"
#include "Inspector.h"
#include "Settings.h"

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
    Toggle rec, play;
    juce::String shown;
    bool shownActive = false;
};

/** Flat icon button for the top bar: settings gear, power (shutdown) and the window controls.
    The window controls do not act on clicks themselves: Windows reports them as native caption
    buttons (see TopBar::windowControlAt), and the window receives minimise / maximise / close. */
class IconButton final : public juce::Component, public juce::SettableTooltipClient
{
public:
    enum class Kind { gear, power, minimise, maximise, close };
    explicit IconButton (Kind k) : kind (k) {}

    std::function<void()> onClick;
    const Kind kind;

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

/** Top bar (also the window's title bar): logo, engine status, CPU, MIDI activity, recorder,
    learn mode, MIDI map, settings, shutdown and the window controls. */
class TopBar final : public juce::Component
{
public:
    explicit TopBar (UiContext& c);
    void paint (juce::Graphics&) override;
    void resized() override;
    void update();
    /** Native hit-testing: what the point (local coordinates) is for the window (caption, buttons...). */
    WindowControlKind windowControlAt (juce::Point<float>) const;

    Chip midiMap { "MIDI Map", theme::text }, learn { "MIDI Learn", theme::text };
    IconButton settings { IconButton::Kind::gear }, power { IconButton::Kind::power };
    IconButton minimiseBtn { IconButton::Kind::minimise }, maximiseBtn { IconButton::Kind::maximise }, closeBtn { IconButton::Kind::close };

private:
    UiContext& ctx;
    RecorderMini recorder;
    juce::Rectangle<int> statusArea;
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
    void openEqWindow (int selectIndex, bool bringToFront = true);
    void refreshAll();
    /** For the window's native hit-testing (point in this component's coordinates). */
    WindowControlKind windowControlAt (juce::Point<float> p) const;

    /** Set by the application: preferences changed / quit requested from the UI. */
    std::function<void()> onPrefsChanged, onShutdown;

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void select (int index);
    void showToast (const juce::String& text, double seconds);
    void meterFrame (double timestamp);
    void finishHotkeyCapture();
    void assignCapturedKey (const juce::KeyPress&);

    UiContext ctx;
    TopBar top;
    std::vector<std::unique_ptr<InputStripView>> inStrips;
    std::vector<std::unique_ptr<BusStripView>> busStrips;
    Inspector inspector;
    PanelHost panelHost;
    std::unique_ptr<ToolWindow> settingsWindow, midiWindow, eqWindow;
    int shownSelBus = -1;     // bus selected with SEL as shown by the input faders
    int shownCaptionSel = -1, shownMonBus = -1;
    juce::String toast;
    double toastUntil = 0;
    double lastMeterFrame = 0;
    Param* hotkeyTarget = nullptr;   // waiting for a key press to assign
    // Meters follow the display refresh (capped at ~60 Hz) instead of a coarse timer.
    juce::VBlankAttachment vblank { this, [this] (double t) { meterFrame (t); } };
};
} // namespace hydra

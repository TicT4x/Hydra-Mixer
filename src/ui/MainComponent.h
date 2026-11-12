#pragma once
#include "Strips.h"
#include "Inspector.h"
#include "Settings.h"
#include "LatencyView.h"
#include "DspView.h"
#include "SyncDelayView.h"
#include "CorrectionView.h"
#include "DevicePicker.h"
#include "SceneEditor.h"
#include "SidePanel.h"
#include "state/UndoHistory.h"
#include "Tour.h"
#include "Welcome.h"

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
    and the window controls. Minimise and close are ordinary buttons. Maximise is a native caption button for
    Windows (Snap Layouts on hover, see TopBar::windowControlAt): Windows reports its clicks to the window. */
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
    /** Mouse over a native caption button: Windows reports that by hit-testing, not as a mouse event. */
    void setNativeHover (bool h) { if (h != nativeHover) { nativeHover = h; repaint(); } }

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
    bool hover = false, down = false, nativeHover = false;
};

/** DSP load meter in the top bar (bar + percentage); click opens the DSP Load window. */
class DspMeter final : public juce::Component, public juce::SettableTooltipClient
{
public:
    DspMeter();
    std::function<void()> onClick;
    /** average / peak: share of real time; overloadsNow: blocks over 100 % since the last call. */
    void setLoad (float average, float peak, int overloadsNow = 0, uint64_t overloadsTotal = 0);
    void tick();   // 4x per second: fades the overload warning
    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { hover = false; repaint(); }
    void mouseUp (const juce::MouseEvent& e) override  { if (e.mouseWasClicked() && onClick) onClick(); }

private:
    float load = 0, peakLoad = 0, alarm = 0;
    bool hover = false;
};

/** "Update 0.9" in the top bar while a new version is available (Updater); shows the download progress. */
class UpdateBadge final : public juce::Component, public juce::SettableTooltipClient
{
public:
    UpdateBadge() { setMouseCursor (juce::MouseCursor::PointingHandCursor); }
    std::function<void()> onClick;
    /** progress < 0: no download running. */
    void set (const juce::String& text, float progress);
    int idealWidth() const;
    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { hover = false; repaint(); }
    void mouseUp (const juce::MouseEvent& e) override  { if (e.mouseWasClicked() && onClick) onClick(); }

private:
    juce::String text;
    float progress = -1.0f;
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
    UpdateBadge updateBadge;   // visible while an update is available

private:
    /** Clears the maximise button's hover once the mouse has left it (runs only while it is hovered). */
    struct NativeHoverWatch final : juce::Timer
    {
        explicit NativeHoverWatch (IconButton& b) : button (b) {}
        void timerCallback() override
        {
            if (button.getScreenBounds().contains (juce::Desktop::getMousePosition())) return;
            button.setNativeHover (false);
            stopTimer();
        }
        IconButton& button;
    };
    mutable NativeHoverWatch maximiseHover { maximiseBtn };

    void layoutBar();
    void glideFrame (double t);

    UiContext& ctx;
    RecorderMini recorder;
    juce::Component recorderSlot;   // clips recorder / cassette while the one turns into the other
    juce::Rectangle<int> statusArea, logoArea, midiArea;
    // Narrow windows: recorder -> cassette, logo -> icon. 1 = full, 0 = small; glides to the target, runs only while it moves.
    float recorderShown = -1.0f, logoShown = -1.0f, recorderTarget = 1.0f, logoTarget = 1.0f;
    double lastGlideFrame = 0.0;
    juce::VBlankAttachment glideVBlank { this, [this] (double t) { glideFrame (t); } };
    juce::String status;
    Engine::LoadMeter cpuMeter;
    uint64_t overloadsSeen = 0;
    int cpuTick = 0;
    uint32_t lastActivity = 0;
    float midiGlow = 0;
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
    /** The window became active / inactive: only the window buttons change (they dim). */
    void repaintWindowControls() { for (auto* b : { &top.minimiseBtn, &top.maximiseBtn, &top.closeBtn }) b->repaint(); }
    /** Lays the strips out again (e.g. "Fill window" was switched). */
    void stripLayoutChanged();
    void openMidiMap();
    void openLatencyFinder();
    void openDspView();
    void openSyncDelay();
    void openCableManager();
    /** Room measurement window; outputSelect = a hardware output to preset as speaker and target (-1 = none). */
    void openRoomMeasurement (int outputSelect = -1);
    /** Device selection window of a channel (input source / bus output); one window, follows the last strip. */
    void openDevicePicker (int channel);
    void toggleLearnMode();
    /** Shows / hides the scene sidebar on the left. */
    void toggleSceneEditor();
    /** Closes and reopens the master device and every other device (same settings), e.g. after a driver
        stopped delivering audio without reporting an error. */
    void restartAudioEngine();
    void openEqWindow (int selectIndex, bool bringToFront = true, int instance = 0);
    /** Room Correction window (correction + measurement of the hardware outputs, one window that follows the selected
        hardware output). selectIndex: the output to show (-1 = keep / choose one); measure: open the measurement page. */
    void openCorrectionWindow (int selectIndex, bool measure = false);
    CorrectionView* correctionView() const;
    void refreshAll();
    /** Interface zoom (1 = 100 %): everything in the main window is drawn larger / smaller;
        the strips scroll horizontally when they no longer fit. */
    void setUiScale (float scale);
    /** Rebuilds strips and inspector after the text size changed (labels keep their font otherwise). */
    void fontScaleChanged();
    /** The palette changed (theme::apply): rebuilds the strips and inspector, repaints all windows. */
    void themeChanged();
    /** For the window's native hit-testing (point in this component's coordinates). */
    WindowControlKind windowControlAt (juce::Point<float> p) const;

    /** Screenshot tool (tools/Shots.cpp) only: select a channel / advance the meters without a window and v-sync. */
    void demoSelect (int index)       { select (index); }
    void demoMeterFrame (double t)    { meterFrame (t); }
    std::unique_ptr<juce::Component> demoStemsPanel (int input) { return makeStemsPanel (ctx, ctx.engine.inputs[(size_t) input]); }
    std::unique_ptr<juce::Component> demoUtilityPanel (int input) { return makeUtilityPanel (ctx, ctx.engine.inputs[(size_t) input].utilities[0]); }
    std::unique_ptr<juce::Component> demoCompPanel (int input) { return makeCompPanel (ctx, ctx.engine.inputs[(size_t) input].comps[0]); }
    std::unique_ptr<juce::Component> demoTransposePanel (int input) { return makeTransposePanel (ctx, ctx.engine.inputs[(size_t) input]); }

    /** Set by the application: preferences changed / quit requested from the UI. */
    std::function<void()> onPrefsChanged, onShutdown;

    /** Updates (state/Updater.h): the updater, "install the downloaded update and restart", "check now". */
    void setUpdater (Updater*, std::function<void()> install, std::function<void()> check);
    /** First start: the welcome wizard over the mixer (ui/Welcome.h). */
    void startWelcome();
    /** Guided tour (ui/Tour.h) from a step; closing it pauses there (prefs.tour). */
    void startTour (Tour::Kind, int step);
    /** The small "Tour paused - Continue / Restart" card, if a tour is paused. */
    void showTourResume();
    /** A short message over the mixer. */
    void notify (const juce::String& text, double seconds) { showToast (text, seconds); }
    /** Screenshot tool only: the open wizard / tour. */
    WelcomeWizard* demoWelcome() const noexcept { return welcome.get(); }
    Tour* demoTour() const noexcept { return tour.get(); }

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void select (int index);
    bool isChannelShown (int selectIndex) const;
    void rebuildStrips();
    void scrollToChannel (int selectIndex);
    void addChannel (int group);
    void removeChannel (int selectIndex);              // asks first
    void removeChannelNow (int selectIndex);
    /** Removes a channel without asking, rebuilding or a message (removeChannelNow, Change Order trash can). */
    void removeChannelCore (int sel);
    int reorderRemoved = 0;   // channels deleted in Change Order, for the message after applying
    void cutChannel (int selectIndex);
    void duplicateChannel (int selectIndex);
    void pasteNewChannel (int group);
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
    /** Window content size (pixels) the channels need: Settings > Display "Window always as large as needed". */
    juce::Point<int> fittingSize() const;
    /** While the window glides to a new size: lay out directly (no own animations). */
    void setInstantLayout (bool b) { ctx.instantLayout = b; }
    /** The channels were added / removed / rebuilt: the window may take its fitting size. */
    std::function<void()> onChannelsChanged;
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
    std::unique_ptr<SceneSidebar> sceneBar;   // Tools > Scene Editor
    std::unique_ptr<SidePanel> sidePanel;     // Settings > Display > Side panel, only while there is room (resized)
    /** Settings of an effect ("..." in its header: gate / denoiser / compressor) in their own window. */
    void openPanelWindow (std::unique_ptr<juce::Component> content, const juce::String& title);
    void closePanelWindow();

    void showUpdateDialog();
    void tickUpdates();
    std::unique_ptr<WelcomeWizard> welcome;
    std::unique_ptr<Tour> tour;
    std::unique_ptr<TourResumeCard> tourResume;
    std::unique_ptr<PluginScanJob> pluginScan;
    uint32_t shownUpdate = ~0u;
    bool installWhenReady = false;   // "Update now": install as soon as the download is done

    std::unique_ptr<ToolWindow> settingsWindow, midiWindow, eqWindow, latencyWindow, dspWindow, syncWindow, deviceWindow, corrWindow, panelWindow, cableWindow;
    int shownSelBus = -1;     // bus selected with SEL as shown by the input faders
    uint32_t shownLayout = 0; // engine channel layout the strips were built for
    float uiScale = 1.0f;
    juce::String toast;
    double toastUntil = 0;
    double lastMeterFrame = 0;
    Param* hotkeyTarget = nullptr;   // waiting for a key press to assign
    // Tooltips of every window (main and tool windows); without one JUCE shows none at all.
    theme::TooltipWindow tooltips;
    // Meters follow the display refresh (capped at ~60 Hz) instead of a coarse timer.
    juce::VBlankAttachment vblank { this, [this] (double t) { meterFrame (t); } };
};
} // namespace hydra

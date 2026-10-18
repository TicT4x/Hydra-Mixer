#include "ui/MainComponent.h"
#include "state/Session.h"
#include "state/Autostart.h"

#include <windows.h>

namespace hydra
{
/** Process-wide tweaks for stable low-latency audio on Windows 10/11. */
static void tuneProcessForAudio()
{
    SetPriorityClass (GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    // Opt out of EcoQoS / power throttling so the scheduler never parks our audio threads on slow cores.
    PROCESS_POWER_THROTTLING_STATE state {};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    state.StateMask = 0;
    SetProcessInformation (GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof (state));
}

/** Main window with a title bar drawn in the app style (theme::LookAndFeel). Snap, resize and the
    Windows 11 snap-layout flyout still work: JUCE maps our title bar areas to the native hit-test codes. */
class MainWindow final : public juce::DocumentWindow
{
public:
    MainWindow (Engine& e, MidiMapper& m, Hotkeys& k, Session& s)
        : juce::DocumentWindow ("Hydra Mixer", theme::bg, 0)
    {
        // No separate title bar: the top bar of MainComponent is the title bar (drag area + window buttons).
        setUsingNativeTitleBar (false);
        setTitleBarHeight (0);
        setContentOwned (new MainComponent (e, m, k, s), true);
        setResizable (true, false);
        setResizeLimits (900, 700, 8000, 4000);
        auto area = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->userArea;
        centreWithSize (juce::jmin (1840, (int) (area.getWidth() * 0.96)), juce::jmin (1000, (int) (area.getHeight() * 0.92)));
    }

    MainComponent* getMain() const { return dynamic_cast<MainComponent*> (getContentComponent()); }

    /** Width so that all channels sit side by side without a scroll bar or a gap (height and position are
        kept, unless the window would leave the screen). Returns false if they do not fit on the screen. */
    bool fitToChannels()
    {
        auto* mc = getMain();
        if (mc == nullptr || isFullScreen() || isMinimised()) return false;
        const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (getScreenBounds());
        if (display == nullptr) return false;
        const auto area = display->userBounds.getSmallestIntegerContainer();
        const int wanted = mc->idealWidth() + (getWidth() - mc->getWidth());
        const int w = juce::jlimit (getConstrainer() != nullptr ? getConstrainer()->getMinimumWidth() : 0, area.getWidth(), wanted);
        const int x = juce::jlimit (area.getX(), juce::jmax (area.getX(), area.getRight() - w), getX());
        setBounds (x, getY(), w, getHeight());
        return wanted <= area.getWidth();
    }

    std::function<void()> onClose, onMinimised;

    void closeButtonPressed() override { if (onClose) onClose(); }

    void minimisationStateChanged (bool isNowMinimised) override
    {
        if (isNowMinimised && onMinimised) onMinimised();
    }

    void activeWindowStatusChanged() override
    {
        juce::DocumentWindow::activeWindowStatusChanged();
        repaint(); // window buttons dim when inactive
    }

    int getDesktopWindowStyleFlags() const override
    {
        // The buttons are drawn by the top bar, but Windows must know the window has them
        // (minimise / maximise animations, snap layouts, taskbar behaviour).
        return juce::DocumentWindow::getDesktopWindowStyleFlags() | juce::ComponentPeer::windowHasMinimiseButton
             | juce::ComponentPeer::windowHasMaximiseButton | juce::ComponentPeer::windowHasCloseButton;
    }

    WindowControlKind findControlAtPoint (juce::Point<float> p) const override
    {
        if (auto* mc = getMain())
            if (const auto kind = mc->windowControlAt (p - mc->getPosition().toFloat()); kind != WindowControlKind::client)
                return kind;
        return juce::DocumentWindow::findControlAtPoint (p);
    }
};

/** Notification-area icon: left click shows the mixer, right click opens a menu with Shutdown. */
class TrayIcon final : public juce::SystemTrayIconComponent
{
public:
    TrayIcon (std::function<void()> showFn, std::function<void()> quitFn)
        : show (std::move (showFn)), quit (std::move (quitFn))
    {
        juce::Image img (juce::Image::ARGB, 32, 32, true);
        {
            juce::Graphics g (img);
            theme::drawLogo (g, { 4.0f, 3.0f, 24.0f, 26.0f });
        }
        setIconImage (img, img);
        setIconTooltip ("Hydra Mixer");
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu())
        {
            show();
            return;
        }
        juce::PopupMenu m;
        m.addItem ("Show Hydra Mixer", show);
        m.addSeparator();
        m.addItem ("Shut down Hydra Mixer", quit);
        juce::Process::makeForegroundProcess(); // otherwise the menu does not close when clicking elsewhere
        m.showMenuAsync (juce::PopupMenu::Options());
    }

private:
    std::function<void()> show, quit;
};

class App final : public juce::JUCEApplication, private juce::Timer
{
public:
    const juce::String getApplicationName() override    { return "Hydra Mixer"; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String& commandLine) override
    {
        tuneProcessForAudio();
        juce::LookAndFeel::setDefaultLookAndFeel (&lnf);

        poller = std::make_unique<ParamPoller>();
        engine = std::make_unique<Engine>();
        midi = std::make_unique<MidiMapper> (engine->params);
        // While a bus is selected (SEL), MIDI faders of the input levels drive the sends to that bus.
        midi->redirect = [e = engine.get()] (Param* p) { return e->faderTarget (p); };
        hotkeys = std::make_unique<Hotkeys> (engine->params);
        session = std::make_unique<Session> (*engine, *midi);
        session->hotkeys = hotkeys.get();
        const bool loaded = session->loadDefault();

        theme::fontScale = session->prefs.fontScale; // before the UI creates its labels
        // The window starts exactly as wide as the channels need, so the divider goes to its default place (no gap).
        if (session->prefs.stripSplit >= 0.0f)
        {
            session->prefs.stripSplit = -1.0f;
            session->prefsVersion.fetch_add (1);
        }
        window = std::make_unique<MainWindow> (*engine, *midi, *hotkeys, *session);
        if (session->windowState.isNotEmpty())
            window->restoreWindowStateFromString (session->windowState);
        if (auto* mc = window->getMain()) mc->setUiScale (session->prefs.uiScale);
        window->fitToChannels();   // just wide enough for all channels
        window->onClose = [this]
        {
            if (session->prefs.closeToTray) hideToTray();
            else systemRequestedQuit();
        };
        window->onMinimised = [this]
        {
            // Hide asynchronously: we are inside the minimise notification here.
            if (session->prefs.minimiseToTray)
                juce::MessageManager::callAsync ([this] { if (window != nullptr && window->isMinimised()) hideToTray(); });
        };
        if (auto* mc = window->getMain())
        {
            mc->onPrefsChanged = [this] { applyPrefs(); };
            mc->onShutdown = [this] { systemRequestedQuit(); };
        }
        if (session->prefs.startInTray) hideToTray();
        else                            window->setVisible (true);
        applyPrefs();

        engine->addChangeListener (&changeCounter);
        lastState = stateKey();
        startedWithWindows = commandLine.contains (Autostart::kArgument);
        logLine ("started" + juce::String (startedWithWindows ? " with Windows" : "") + ", session "
                 + juce::String (loaded ? "loaded" : "not found / defaults"));
        checkMasterAfterStart();
        startTimer (1000);
        // Keeps the autostart entry pointing at this exe (rebuilt / moved); off the critical startup path.
        juce::Timer::callAfterDelay (8000, [] { Autostart::refreshIfMoved(); });
    }

    void shutdown() override
    {
        stopTimer();
        save();
        engine->removeChangeListener (&changeCounter);
        tray.reset();
        window.reset();
        hotkeys.reset();
        midi.reset();
        session.reset();
        engine.reset();
        poller.reset();
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    void systemRequestedQuit() override { quit(); }
    void anotherInstanceStarted (const juce::String&) override { showWindow(); }

private:
    void showWindow()
    {
        if (window == nullptr) return;
        window->setVisible (true);
        window->setMinimised (false);
        window->toFront (true);
        applyPrefs();
    }

    void hideToTray()
    {
        if (window == nullptr) return;
        ensureTray(); // never hide without a way back
        window->setVisible (false);
    }

    void ensureTray()
    {
        if (tray == nullptr)
            tray = std::make_unique<TrayIcon> ([this] { showWindow(); }, [this] { systemRequestedQuit(); });
    }

    /** Tray icon visible if enabled, if a tray behaviour needs it, or while the window is hidden. */
    void applyPrefs()
    {
        if (session == nullptr || window == nullptr) return;
        const auto& p = session->prefs;
        if (auto* mc = window->getMain())
        {
            mc->setUiScale (p.uiScale);
            if (std::abs (theme::fontScale - p.fontScale) > 0.001f)
            {
                theme::fontScale = p.fontScale;
                mc->fontScaleChanged();
            }
        }
        if (p.trayIcon || p.closeToTray || p.minimiseToTray || p.startInTray || ! window->isVisible())
            ensureTray();
        else
            tray.reset();
    }

    struct ChangeCounter : juce::ChangeListener
    {
        int changes = 0;
        void changeListenerCallback (juce::ChangeBroadcaster*) override { ++changes; }
    };

    void save()
    {
        if (session == nullptr) return;
        try
        {
            if (window != nullptr) session->windowState = window->getWindowStateAsString();
            const bool ok = session->saveDefault();
            logLine (ok ? "session saved" : "session save FAILED");
        }
        catch (const std::exception& e) { logLine (juce::String ("session save threw: ") + e.what()); }
        catch (...)                     { logLine ("session save threw an unknown exception"); }
        lastState = stateKey();
        changeCounter.changes = 0;
    }

    /** Cheap fingerprint of everything that is persisted (params, names, routing changes). */
    juce::String stateKey() const
    {
        juce::String k (juce::String ((juce::int64) engine->params.versionSum()));
        for (auto& s : engine->inputs) k << '|' << s.name << ':' << s.plugins.size();
        for (auto& b : engine->buses)  k << '|' << b.name << ':' << b.plugins.size();
        k << '|' << (int) midi->mapVersion.load() << '|' << (int) hotkeys->version.load()
          << '|' << (int) session->prefsVersion.load() << '|' << (int) engine->recorder.stateVersion.load()
          << '|' << (int) session->scenes.version.load();
        return k;
    }

    /** Right after Windows started, the audio driver of the master device may not be ready yet: the engine then
        runs on another device (or none). Keep asking for the session's device for a while. */
    bool masterMissing() const
    {
        const auto& want = session->requestedMaster;
        auto* d = engine->getMasterDevice();
        return d == nullptr || (want.deviceName.isNotEmpty() && (d->getName() != want.deviceName || d->getTypeName() != want.typeName));
    }

    void checkMasterAfterStart()
    {
        session->waitingForMaster = masterMissing() && session->requestedMaster.deviceName.isNotEmpty();
        if (session->waitingForMaster)
            logLine ("master device " + session->requestedMaster.deviceName + " not available yet, retrying");
    }

    void retryMaster()
    {
        if (! session->waitingForMaster) return;
        if (++masterRetryTicks > kMasterRetrySeconds)
        {
            session->waitingForMaster = false;   // give up: the device now running is kept
            logLine ("master device " + session->requestedMaster.deviceName + " did not appear, keeping "
                     + engine->getMasterConfig().deviceName);
            return;
        }
        if (masterRetryTicks % 3 != 0) return;
        auto* type = engine->findType (session->requestedMaster.typeName);
        if (type == nullptr) return;
        type->scanForDevices();
        if (! type->getDeviceNames (false).contains (session->requestedMaster.deviceName)) return;
        if (engine->setMasterConfig (session->requestedMaster).isEmpty() && ! masterMissing())
        {
            session->waitingForMaster = false;
            logLine ("master device " + session->requestedMaster.deviceName + " opened after "
                     + juce::String (masterRetryTicks) + " s");
            if (auto* mc = window->getMain()) mc->refreshAll();
        }
    }

    void timerCallback() override
    {
        retryMaster();

        // Debounced autosave: write ~2 s after the last change.
        const auto key = stateKey();
        if (key != lastState || changeCounter.changes > 0)
        {
            if (key != pendingState || changeCounter.changes != pendingChanges)
            {
                pendingState = key;
                pendingChanges = changeCounter.changes;
                quietTicks = 0;
                return;
            }
            if (++quietTicks >= 2)
                save();
        }
    }

    theme::LookAndFeel lnf;
    std::unique_ptr<ParamPoller> poller;
    std::unique_ptr<Engine> engine;
    std::unique_ptr<MidiMapper> midi;
    std::unique_ptr<Hotkeys> hotkeys;
    std::unique_ptr<Session> session;
    std::unique_ptr<MainWindow> window;
    std::unique_ptr<TrayIcon> tray;
    ChangeCounter changeCounter;
    juce::String lastState, pendingState;
    int pendingChanges = -1, quietTicks = 0;
    bool startedWithWindows = false;
    int masterRetryTicks = 0;
    static constexpr int kMasterRetrySeconds = 90;
};
} // namespace hydra

START_JUCE_APPLICATION (hydra::App)

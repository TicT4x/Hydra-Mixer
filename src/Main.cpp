#include "ui/MainComponent.h"
#include "state/Session.h"
#include "state/Autostart.h"
#include "state/CorrectionSync.h"
#include "state/VacCables.h"
#include "engine/VacStream.h"
#include "engine/NetLink.h"
#include "remote/RemoteControl.h"
#include "ui/Dialog.h"
#include "ui/UiWatch.h"
#include "state/Install.h"
#include "state/Updater.h"
#include <random>

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
        setResizeLimits (760, 520, 8000, 4000);   // horizontal rows with few channels are compact
        auto area = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->userArea;
        centreWithSize (juce::jmin (1840, (int) (area.getWidth() * 0.96)), juce::jmin (1000, (int) (area.getHeight() * 0.92)));
    }

    MainComponent* getMain() const { return dynamic_cast<MainComponent*> (getContentComponent()); }

    /** No visible frame: the content fills the window, the resize zone lies on top of it (native hit test). */
    juce::BorderSize<int> getContentComponentBorder() const override { return {}; }

    void visibilityChanged() override
    {
        juce::DocumentWindow::visibilityChanged();
        if (isVisible()) removeNativeWindowBorder (*this);
    }

    /** Width so that all channels sit side by side without a scroll bar or a gap (height and position are
        kept, unless the window would leave the screen). Returns false if they do not fit on the screen. */
    bool fitToChannels (bool growOnly = false)
    {
        auto* mc = getMain();
        if (mc == nullptr || isFullScreen() || isMinimised()) return false;
        const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (getScreenBounds());
        if (display == nullptr) return false;
        const auto area = display->userBounds.getSmallestIntegerContainer();
        const int wanted = mc->idealWidth() + (getWidth() - mc->getWidth());
        if (growOnly && getWidth() >= wanted) return true;
        const int w = juce::jlimit (getConstrainer() != nullptr ? getConstrainer()->getMinimumWidth() : 0, area.getWidth(), wanted);
        const int x = juce::jlimit (area.getX(), juce::jmax (area.getX(), area.getRight() - w), getX());
        setBounds (x, getY(), w, getHeight());
        return wanted <= area.getWidth();
    }

    /** Takes the size the channels need (Settings > Display: "Window always as large as needed"). The position is
        kept as far as the window stays on its screen. animated: glides there (~0.2 s), otherwise at once. */
    void fitWindow (bool animated)
    {
        auto* mc = getMain();
        if (mc == nullptr || isFullScreen() || isMinimised()) return;
        const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (getScreenBounds());
        if (display == nullptr) return;
        const auto area = display->userBounds.getSmallestIntegerContainer();
        const auto wanted = mc->fittingSize();   // content = window (no frame)
        const auto* limits = getConstrainer();
        const int w = juce::jlimit (limits != nullptr ? limits->getMinimumWidth() : 0, area.getWidth(), wanted.x);
        const int h = juce::jlimit (limits != nullptr ? limits->getMinimumHeight() : 0, area.getHeight(), wanted.y);
        const juce::Rectangle<int> target (juce::jlimit (area.getX(), juce::jmax (area.getX(), area.getRight() - w), getX()),
                                           juce::jlimit (area.getY(), juce::jmax (area.getY(), area.getBottom() - h), getY()), w, h);
        if (target == (gliding ? glideTo : getBounds())) return;
        if (! animated || ! isShowing())
        {
            stopGlide();
            setBounds (target);
            return;
        }
        glideFrom = getBounds();
        glideTo = target;
        glideStart = juce::Time::getMillisecondCounterHiRes();
        gliding = true;
        lastSet = getBounds();
        mc->setInstantLayout (true);
    }

    std::function<void()> onClose, onMinimised, onRestored, onLeftFullScreen;

    /** Maximised -> normal window (button, double-click on the bar, dragged off the top): Windows restores the old
        size, which rarely fits the channels any more. onLeftFullScreen takes the right size. */
    void resized() override
    {
        juce::DocumentWindow::resized();
        if (isMinimised()) return;
        const bool full = isFullScreen();
        if (wasFullScreen && ! full && onLeftFullScreen) onLeftFullScreen();
        wasFullScreen = full;
    }

    void closeButtonPressed() override { if (onClose) onClose(); }

    void minimisationStateChanged (bool isNowMinimised) override
    {
        if (isNowMinimised) { stopGlide(); if (onMinimised) onMinimised(); }
        else if (onRestored) onRestored();
    }

    void activeWindowStatusChanged() override
    {
        juce::DocumentWindow::activeWindowStatusChanged();
        removeNativeWindowBorder (*this);   // also here: JUCE may have recreated the native window since it was shown
        // Only the window buttons dim when inactive. Why not repaint(): that redrew the whole mixer (all strips,
        // meters, the inspector) every time another window got the focus, e.g. right when the settings opened.
        if (auto* mc = getMain()) mc->repaintWindowControls();
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

private:
    static constexpr double kGlideMs = 220.0;

    void glideFrame()
    {
        if (! gliding) return;
        // The user grabbed the window (resize / move / snap): stop gliding.
        if (getBounds() != lastSet || isFullScreen() || isMinimised()) { stopGlide(); return; }
        const double t = juce::jlimit (0.0, 1.0, (juce::Time::getMillisecondCounterHiRes() - glideStart) / kGlideMs);
        const float e = 1.0f - (float) std::pow (1.0 - t, 3.0);   // ease out
        auto lerp = [e] (int a, int b) { return a + juce::roundToInt ((float) (b - a) * e); };
        const juce::Rectangle<int> r (lerp (glideFrom.getX(), glideTo.getX()), lerp (glideFrom.getY(), glideTo.getY()),
                                      lerp (glideFrom.getWidth(), glideTo.getWidth()), lerp (glideFrom.getHeight(), glideTo.getHeight()));
        setBounds (r);
        lastSet = getBounds();
        if (t >= 1.0) stopGlide();
    }

    void stopGlide()
    {
        if (! gliding) return;
        gliding = false;
        if (auto* mc = getMain())
        {
            mc->setInstantLayout (false);
            mc->resized();   // the last layout once more with the normal rules
        }
    }

    bool gliding = false, wasFullScreen = false;
    juce::Rectangle<int> glideFrom, glideTo, lastSet;
    double glideStart = 0.0;
    juce::VBlankAttachment vblank { this, [this] (double) { glideFrame(); } };
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
    // The elevated helpers (device names, cables, firewall) and the installer run next to the mixer.
    // After an update the new exe first waits until the old one has quit (it holds the single-instance lock).
    bool moreThanOneInstanceAllowed() override
    {
        const auto c = getCommandLineParameters();
        if (isDeviceNameJob (c) || isVacJob (c) || isFirewallJob (c) || c.contains (install::kUninstallArgument)) return true;
        if (install::shouldOfferInstall (c)) return true;
        install::waitForPreviousInstance (c);
        return false;
    }

    void initialise (const juce::String& commandLine) override
    {
        if (isVacJob (commandLine))
        {
            // Started elevated by VacCables: set the number of cables, restart the driver and quit.
            setApplicationReturnValue (vac::runJob (commandLine));
            quit();
            return;
        }
        if (isDeviceNameJob (commandLine))
        {
            // Started elevated by DeviceNamer: apply the job file and quit, nothing else.
            const auto file = commandLine.fromFirstOccurrenceOf (DeviceNamer::kJobArgument, false, false).trim().unquoted();
            setApplicationReturnValue (DeviceNamer::runJob (juce::File (file)));
            quit();
            return;
        }

        if (isFirewallJob (commandLine))
        {
            // Started elevated by install::requestFirewall: add / remove the rule and quit.
            setApplicationReturnValue (install::runFirewallJob (commandLine));
            quit();
            return;
        }
        if (commandLine.contains (install::kUninstallArgument) || install::shouldOfferInstall (commandLine))
        {
            // Windows "Apps" > Uninstall, or a downloaded release: only the installer window, no mixer.
            juce::LookAndFeel::setDefaultLookAndFeel (&lnf);
            theme::apply (theme::Style::dark);
            lnf.refreshColours();
            const bool uninstalling = commandLine.contains (install::kUninstallArgument);
            auto* panel = new InstallerPanel (uninstalling ? InstallerPanel::Mode::uninstall : InstallerPanel::Mode::install);
            installer = std::make_unique<ToolWindow> (uninstalling ? "Hydra Mixer" : "Hydra Mixer Setup", panel, panel->getWidth(), panel->getHeight() + 32);
            installer->setResizable (false, false);
            installer->onClose = [this] { quit(); };
            panel->onFinished = [this, commandLine] (bool quitNow)
            {
                juce::MessageManager::callAsync ([this, commandLine, quitNow]
                {
                    installer.reset();
                    if (quitNow) quit();
                    else startMixer (commandLine);   // "Run without installing"
                });
            };
            return;
        }
        startMixer (commandLine);
    }

    void startMixer (const juce::String& commandLine)
    {
        tuneProcessForAudio();
        juce::LookAndFeel::setDefaultLookAndFeel (&lnf);
        install::cleanUpAfterUpdate();   // the exe of the previous version, the download
        install::quitRequested();        // creates the event the installer uses to close this mixer

        poller = std::make_unique<ParamPoller>();
        engine = std::make_unique<Engine>();
        midi = std::make_unique<MidiMapper> (engine->params);
        // While a bus is selected (SEL), MIDI faders of the input levels drive the sends to that bus.
        midi->redirect = [e = engine.get()] (Param* p) { return e->faderTarget (p); };
        hotkeys = std::make_unique<Hotkeys> (engine->params);
        session = std::make_unique<Session> (*engine, *midi);
        session->hotkeys = hotkeys.get();
        const bool loaded = session->loadDefault();
        if (! loaded) session->prefs.setupDone = false;   // first start: the welcome wizard
        deviceNamer = std::make_unique<DeviceNamer> (*engine, session->deviceRenames);
        session->deviceNamer = deviceNamer.get();
        vacCables = std::make_unique<VacCables> (*engine, session->deviceRenames);
        session->vacCables = vacCables.get();
        vacCables->onRoutingChanged = [this]
        {
            session->prefsVersion.fetch_add (1);   // sources / outputs may have changed
            if (auto* mc = window != nullptr ? window->getMain() : nullptr) mc->refreshAll();
        };
        correctionSync = std::make_unique<CorrectionPoolSync> (*engine);   // after the session: its profiles get linked
        remoteControl = std::make_unique<RemoteControl> (*engine, session->scenes);
        remoteControl->nameProvider = [this]
        {
            auto& hub = net::Hub::get();
            if (hub.isRunning()) return hub.getName();
            return session->prefs.netName.isNotEmpty() ? session->prefs.netName : net::Hub::computerName();
        };
        deviceNamer->onRenamed = [this]
        {
            session->prefsVersion.fetch_add (1);   // the rename list is saved with the session
            if (auto* mc = window != nullptr ? window->getMain() : nullptr) mc->refreshAll();
        };

        theme::fontScale = session->prefs.fontScale; // before the UI creates its labels
        theme::glow = true;   // no longer an option (13.11.)
        theme::apply (theme::styleFromName (session->prefs.theme));
        lnf.refreshColours();
        // The window starts exactly as wide as the channels need, so the dividers go to their default places (no gap).
        if (session->prefs.stripSplit >= 0.0f || session->prefs.stripSplit2 >= 0.0f)
        {
            session->prefs.stripSplit = session->prefs.stripSplit2 = -1.0f;
            session->prefsVersion.fetch_add (1);
        }
        window = std::make_unique<MainWindow> (*engine, *midi, *hotkeys, *session);
        if (session->windowState.isNotEmpty())
            window->restoreWindowStateFromString (session->windowState);
        if (auto* mc = window->getMain()) mc->setUiScale (session->prefs.uiScale);
        if (session->prefs.fitWindow) window->fitWindow (false);   // exactly the size the channels need
        else window->fitToChannels (session->prefs.adaptiveStrips);   // all channels fit (adaptive: only grows, the strips use extra room)
        window->onRestored = [this] { fitWindowLater(); };   // back from the taskbar / the tray
        window->onLeftFullScreen = [this] { fitAfterFullScreen(); };
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
            mc->setUpdater (&updater, [this] { installUpdate(); }, [this] { checkForUpdates (true); });
            mc->onPrefsChanged = [this] { applyPrefs(); };
            mc->onShutdown = [this] { systemRequestedQuit(); };
            mc->onChannelsChanged = [this] { fitWindowLater(); };   // a channel added / removed
        }
        shownLayoutKey = layoutKey();
        if (session->prefs.startInTray) hideToTray();
        else                            window->setVisible (true);
        applyPrefs();

        engine->addChangeListener (&changeCounter);
        lastState = stateKey();
        startedWithWindows = commandLine.contains (Autostart::kArgument);
        logLine ("started" + juce::String (startedWithWindows ? " with Windows" : "") + ", session "
                 + juce::String (loaded ? "loaded" : "not found / defaults"));
        checkMasterAfterStart();
        if (auto* mc = window->getMain())
        {
            if (! session->prefs.setupDone) mc->startWelcome();
            else mc->showTourResume();
            if (commandLine.contains (install::kUpdatedArgument))
                mc->notify ("Hydra Mixer " + install::currentVersion() + " is installed", 4.0);
        }
        if (commandLine.contains (install::kUpdatedArgument)) logLine ("running " + install::currentVersion() + " after an update / installation");
        startTimer (1000);
        uiWatch = std::make_unique<UiWatch>();
        // Keeps the autostart entry pointing at this exe (rebuilt / moved); off the critical startup path.
        // schtasks.exe can take seconds: on a background thread, the window stays responsive.
        juce::Timer::callAfterDelay (8000, [] { juce::Thread::launch ([] { Autostart::refreshIfMoved(); }); });
    }

    void shutdown() override
    {
        stopTimer();
        installer.reset();
        uiWatch.reset();
        if (engine == nullptr) return;   // device name helper
        save();
        engine->removeChangeListener (&changeCounter);
        tray.reset();
        window.reset();
        if (session != nullptr) session->deviceNamer = nullptr;
        deviceNamer.reset();
        if (session != nullptr) session->vacCables = nullptr;
        vacCables.reset();
        correctionSync.reset();
        remoteControl.reset();   // before the engine: it reads its meters and parameters
        hotkeys.reset();
        midi.reset();
        session.reset();
        engine.reset();
        stems::Service::shutdown();   // the separation worker and its GPU model (no channel uses it any more)
        net::Hub::get().stop();   // closes Hydra Link's port (its streams are gone with the engine)
        poller.reset();
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    void systemRequestedQuit() override { quit(); }
    void anotherInstanceStarted (const juce::String&) override { showWindow(); }

private:
    static bool isDeviceNameJob (const juce::String& commandLine) { return commandLine.contains (DeviceNamer::kJobArgument); }
    static bool isVacJob (const juce::String& commandLine) { return commandLine.contains (vac::kJobArgument); }
    static bool isFirewallJob (const juce::String& commandLine) { return commandLine.contains (install::kFirewallArgument); }

    /** Updates: at start (after 20 s) and every 6 h, if wanted. manual = Settings > Check now (also when switched off). */
    void checkForUpdates (bool manual)
    {
        if (session == nullptr || (! manual && ! session->prefs.updateCheck)) return;
        updater.check (session->prefs.updateBeta);
        lastUpdateCheck = juce::Time::getMillisecondCounterHiRes();
    }

    /** The update is downloaded: save, put the new exe in place, start it (it waits for us) and quit. */
    void installUpdate()
    {
        const auto st = updater.getStatus();
        if (st.state != Updater::State::ready || ! st.file.existsAsFile()) return;
        save();
        install::Options o;
        o.startMenu = true;
        o.firewall = (session->prefs.netEnabled || session->prefs.remoteEnabled) && ! install::firewallAllows (install::installedExe());
        const auto err = install::installFrom (st.file, st.release.version, o);
        if (err.isNotEmpty())
        {
            logLine ("update " + st.release.version + " not installed: " + err);
            if (window != nullptr)
                dialog::message (window->getMain(), dialog::Kind::warning, "Update not installed", err);
            return;
        }
        if (! install::launchInstalled (true))
        {
            if (window != nullptr)
                dialog::message (window->getMain(), dialog::Kind::warning, "Update installed",
                                 "The new version is installed but did not start. Start Hydra Mixer again from the Start menu.");
            return;
        }
        logLine ("update " + st.release.version + " installed, restarting");
        quit();
    }

    void showWindow()
    {
        if (window == nullptr) return;
        // "Window always as large as needed": each time it appears. Still hidden: at once (nobody sees it);
        // minimised: when it is restored (onRestored); already visible: it glides.
        const bool hidden = ! window->isVisible(), minimised = window->isMinimised();
        if (session->prefs.fitWindow && hidden && ! minimised) window->fitWindow (false);
        window->setVisible (true);
        window->setMinimised (false);
        window->toFront (true);
        applyPrefs();
        if (! hidden && ! minimised) fitWindowLater();
    }

    /** Glides the window to its fitting size, if that option is on (after the current layout is done). */
    /** Out of the maximised state: the size the channels need (whatever "Window always as large as needed" says,
        the size from before maximising is stale). While the window is still being dragged (pulled off the top of
        the screen) wait for the mouse to be released, resizing under the mouse would fight Windows. */
    void fitAfterFullScreen()
    {
        juce::Timer::callAfterDelay (juce::ModifierKeys::getCurrentModifiersRealtime().isAnyMouseButtonDown() ? 120 : 0, [this]
        {
            if (window == nullptr || ! window->isVisible() || window->isFullScreen() || window->isMinimised()) return;
            if (juce::ModifierKeys::getCurrentModifiersRealtime().isAnyMouseButtonDown()) { fitAfterFullScreen(); return; }
            window->fitWindow (true);
        });
    }

    void fitWindowLater()
    {
        if (session == nullptr || ! session->prefs.fitWindow) return;
        juce::MessageManager::callAsync ([this] { if (window != nullptr && window->isVisible()) window->fitWindow (true); });
    }

    /** Settings that change the size the channels need. */
    juce::String layoutKey() const
    {
        const auto& p = session->prefs;
        juce::String key;
        key << (int) p.fitWindow << (int) p.horizontalStrips << (int) p.adaptiveStrips
            << juce::String (p.uiScale, 3) << juce::String (p.fontScale, 3) << p.theme;
        return key;
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
        engine->autoReconnect = p.autoReconnect;
        if (p.vacDirect != isVacDirectEnabled())
        {
            setVacDirectEnabled (p.vacDirect);
            // Streams on cables reopen the other way: let go of them once, then take them again.
            engine->holdDevice = [] (const IoTarget& t) { return vac::isCableEndpoint (t.deviceName); };
            engine->rebuildGraph();
            engine->holdDevice = nullptr;
            engine->rebuildGraph();
        }
        applyRemote();    // first: a new access code goes into Hydra Link's config (phones get it when they pair)
        applyNetwork();
        if (auto* mc = window->getMain())
        {
            mc->setUiScale (p.uiScale);
            if (std::abs (theme::fontScale - p.fontScale) > 0.001f)
            {
                theme::fontScale = p.fontScale;
                mc->fontScaleChanged();
            }
            if (const auto style = theme::styleFromName (p.theme); style != theme::style)
            {
                theme::apply (style);
                lnf.refreshColours();
                mc->themeChanged();
            }
            mc->stripLayoutChanged();   // adaptive strips / side panel / dividers may have changed
        }
        if (const auto key = layoutKey(); key != shownLayoutKey)
        {
            shownLayoutKey = key;
            fitWindowLater();
        }
        if (p.trayIcon || p.closeToTray || p.minimiseToTray || p.startInTray || ! window->isVisible())
            ensureTray();
        else
            tray.reset();
    }

    /** Hydra Link on / off / changed. Network channels that could not open while it was off open once it runs. */
    void applyNetwork()
    {
        const auto& p = session->prefs;
        auto& hub = net::Hub::get();
        const bool was = hub.isRunning();
        if (! p.netEnabled)
        {
            if (was)
            {
                hub.stop();
                logLine ("Hydra Link off");
                engine->rebuildGraph();
            }
            return;
        }
        if (net::parseDeviceId (p.netDeviceId) == 0)
        {
            std::random_device rd;
            session->prefs.netDeviceId = net::deviceIdText ((((uint64_t) rd() << 32) ^ (uint64_t) rd() ^ (uint64_t) juce::Time::getHighResolutionTicks()) | 1);
            session->prefsVersion.fetch_add (1);
        }
        if (! trustLoaded)
        {
            std::vector<net::TrustedDevice> list;
            for (auto& t : p.netTrusted)
            {
                const auto rest = t.fromFirstOccurrenceOf ("|", false, false);
                list.push_back ({ net::parseDeviceId (t.upToFirstOccurrenceOf ("|", false, false)),
                                  rest.fromFirstOccurrenceOf ("|", false, false),
                                  rest.getIntValue() == 1 ? net::DeviceKind::phone : net::DeviceKind::computer });
            }
            hub.setTrusted (std::move (list));
            shownTrust = hub.getTrustVersion();
            trustLoaded = true;
        }
        net::Config c;
        c.deviceId = net::parseDeviceId (p.netDeviceId);
        c.remoteCode = p.remoteEnabled ? p.remoteCode : juce::String();
        c.port = p.netPort;
        c.name = p.netName;
        c.password = p.netPassword;
        c.peers = p.netPeers;
        c.remotePort = p.remoteEnabled ? p.remotePort : 0;   // the phone app finds the web control through the announcements
        const auto before = hub.getConfig();
        juce::String err;
        if (! hub.start (c, err))
        {
            logLine ("Hydra Link: " + err);
            if (was) engine->rebuildGraph();
            return;
        }
        if (! was || before.port != hub.getConfig().port)
            logLine ("Hydra Link on, port " + juce::String (hub.getConfig().port) + ", name " + hub.getName()
                     + (c.password.isNotEmpty() ? ", encrypted" : ", local network only"));
        if (! was) engine->rebuildGraph();
    }

    /** Hydra Remote (phones / tablets) on / off / changed. The first time it is switched on it gets an access code. */
    void applyRemote()
    {
        auto& p = session->prefs;
        if (p.remoteEnabled && p.remoteCode.isEmpty())
        {
            p.remoteCode = remote::Server::newCode();
            session->prefsVersion.fetch_add (1);
        }
        juce::String err;
        remoteControl->apply (p.remoteEnabled, p.remotePort, p.remoteCode, err);
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
        for (auto& s : engine->inputs) k << '|' << s.name << ':' << (int) s.plugins.getVersion();   // effect chain edits (add / move / remove)
        for (auto& b : engine->buses)  k << '|' << b.name << ':' << b.icon << ':' << (int) b.plugins.getVersion() << ':' << (int) b.correction.getVersion();
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

    /** Hydra Link pairing: store what changed, ask the user about devices that want to connect. */
    void tickPairing()
    {
        auto& hub = net::Hub::get();
        if (const auto v = hub.getTrustVersion(); v != shownTrust)
        {
            shownTrust = v;
            juce::StringArray list;
            for (auto& t : hub.getTrusted())
                list.add (net::deviceIdText (t.id) + "|" + juce::String ((int) t.kind) + "|" + t.name);
            if (list != session->prefs.netTrusted)
            {
                session->prefs.netTrusted = list;
                session->prefsVersion.fetch_add (1);
                logLine ("Hydra Link: paired devices " + juce::String (list.size()));
            }
        }
        if (pairDialogOpen || ! hub.isRunning() || window == nullptr) return;
        const auto requests = hub.getRequests();
        if (requests.empty()) return;
        const auto r = requests.front();
        const bool phone = r.kind == net::DeviceKind::phone;
        pairDialogOpen = true;
        showWindow();
        logLine ("Hydra Link: " + r.name + " (" + r.address + ") asks to connect");
        const juce::String what = phone
            ? "The phone \"" + r.name + "\" (" + r.address + ") wants to connect to this Hydra: audio in both directions "
              "and control of the mixer from the Hydra Link app."
            : "The computer \"" + r.name + "\" (" + r.address + ") wants to connect to this Hydra: send audio to it and "
              "receive audio from it over Hydra Link.";
        dialog::show (window->getMain(), dialog::Kind::question, "Connect " + r.name + "?",
                      what + "\n\nOnce allowed it connects without asking again (Settings > Network lists the connected devices).",
                      { { "Allow", dialog::Button::Role::primary }, { "Deny", dialog::Button::Role::cancel } },
                      [this, r, phone] (int choice)
                      {
                          pairDialogOpen = false;
                          const bool allow = choice == 0;
                          if (allow && phone && ! session->prefs.remoteEnabled)
                          {
                              // The phone controls the mixer too: Hydra Remote on (it gets the code with the answer).
                              session->prefs.remoteEnabled = true;
                              session->prefsVersion.fetch_add (1);
                              applyPrefs();
                              logLine ("Hydra Remote on (a phone was allowed to connect)");
                          }
                          net::Hub::get().answer (r.id, allow);
                          logLine ("Hydra Link: " + r.name + (allow ? " allowed" : " denied"));
                      });
    }

    void timerCallback() override
    {
        if (install::quitRequested())   // the installer replaces this exe
        {
            logLine ("quitting: the installer asked");
            systemRequestedQuit();
            return;
        }
        {
            const double now = juce::Time::getMillisecondCounterHiRes();
            if (lastUpdateCheck == 0.0 ? now - startedAt > 20000.0 : now - lastUpdateCheck > 6.0 * 3600.0 * 1000.0)
            {
                lastUpdateCheck = now;   // also when switched off: look again in 6 h
                checkForUpdates (false);
            }
        }
        { UiActivity a ("app timer: master retry"); retryMaster(); }
        { UiActivity a ("app timer: pairing"); tickPairing(); }
        deviceNamer->useTask = session->prefs.renameNoPrompt;
        if (! session->waitingForMaster) { UiActivity a ("app timer: device names"); deviceNamer->tick (false, session->prefs.vacAutoName); }   // only VAC cables are renamed (13.11.); others get their old names back
        if (! session->waitingForMaster)
        {
            UiActivity a ("app timer: cable visibility");
            vacCables->tickHiding (session->prefs.vacHideOwnSide, session->prefs.vacHideUnused, session->vacHidden);
        }
        UiActivity a ("app timer: autosave");

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
    std::unique_ptr<DeviceNamer> deviceNamer;
    std::unique_ptr<VacCables> vacCables;
    std::unique_ptr<CorrectionPoolSync> correctionSync;
    std::unique_ptr<RemoteControl> remoteControl;
    std::unique_ptr<MainWindow> window;
    std::unique_ptr<TrayIcon> tray;
    std::unique_ptr<UiWatch> uiWatch;
    std::unique_ptr<ToolWindow> installer;   // installer / uninstaller mode (no mixer)
    Updater updater;
    double lastUpdateCheck = 0.0, startedAt = juce::Time::getMillisecondCounterHiRes();
    juce::String shownLayoutKey;
    ChangeCounter changeCounter;
    juce::String lastState, pendingState;
    int pendingChanges = -1, quietTicks = 0;
    bool startedWithWindows = false;
    int masterRetryTicks = 0;
    bool trustLoaded = false, pairDialogOpen = false;
    uint32_t shownTrust = 0;
    static constexpr int kMasterRetrySeconds = 90;
};
} // namespace hydra

START_JUCE_APPLICATION (hydra::App)

#pragma once
#include "Widgets.h"
#include "Dialog.h"

namespace hydra
{
/** First start: a wizard over the main window (only when there was no session.xml).

      1. How do you start? Fresh / Import from Voicemeeter / I have Hydra settings
      2. Fresh: main device -> number of channels -> virtual cables -> VST3 plugins -> extras (autostart, phone,
         network, updates) -> "Take the tour?"
         Voicemeeter: pick the XML, import, then "See what's different?" (short tour)
         Hydra settings: load a file or a backup, done (no tour)
    Every choice is applied right away to the running mixer. "Skip setup" keeps the defaults. When done,
    onDone says whether (and which) tour to start; prefs.setupDone is set by the caller. */
class WelcomeWizard final : public juce::Component
{
public:
    enum class Tour { none, full, voicemeeter };
    WelcomeWizard (UiContext&, juce::Component& host);
    ~WelcomeWizard() override;

    std::function<void (Tour)> onDone;
    /** The mixer changed a lot (channels, devices, a session loaded): refresh the strips. */
    std::function<void()> onChanged;
    /** Plugin scan wanted: runs in the background after the wizard (MainComponent). */
    std::function<void()> onScanPlugins;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    void parentSizeChanged() override { if (auto* p = getParentComponent()) setBounds (p->getLocalBounds()); }

    enum Page { start, device, channels, cables, plugins, extras, finish, voicemeeter, vmDone, hydra };
    /** Screenshot tool (tools/Shots.cpp) only: show a page. */
    void demoGo (Page p) { go (p); }

private:
    class PageBase;
    class StartPage;
    class DevicePage;
    class ChannelsPage;
    class CablesPage;
    class PluginsPage;
    class ExtrasPage;
    class FinishPage;
    class VoicemeeterPage;
    class VmDonePage;
    class HydraPage;

    void go (Page, bool forward = true);
    void continuePressed();
    void backPressed();
    void done (Tour);
    void updateButtons();

    UiContext& ctx;
    Page page = start;
    std::vector<Page> history;
    std::unique_ptr<PageBase> content;
    juce::Rectangle<int> card;
    dialog::ActionButton backBtn { { "Back" }, theme::accentIn }, nextBtn { { "Continue", dialog::Button::Role::primary }, theme::accentIn },
                         skipBtn { { "Skip setup" }, theme::accentIn };
    juce::StringArray vmReport;   // shown after the Voicemeeter import
};

/** Plugin scan in the background (after the wizard). Owned by the main window; cancelled when it closes. */
class PluginScanJob final : public juce::Thread
{
public:
    explicit PluginScanJob (Engine& e) : juce::Thread ("Plugin scan"), engine (e) {}
    ~PluginScanJob() override { cancel = true; stopThread (15000); }
    void run() override { engine.pluginHost.scan ([] (float, const juce::String&) {}, cancel); }

private:
    Engine& engine;
    std::atomic<bool> cancel { false };
};

/** Installer / uninstaller window content (the exe started as installer: no mixer runs). */
class InstallerPanel final : public juce::Component, private juce::Timer
{
public:
    enum class Mode { install, uninstall };
    explicit InstallerPanel (Mode);
    ~InstallerPanel() override;
    /** Finished: quit = true closes the app; false = run the mixer from here (not installed). */
    std::function<void (bool quit)> onFinished;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void startInstall();
    void startUninstall();

    Mode mode;
    juce::ToggleButton startMenu { "Start menu entry" }, desktop { "Desktop shortcut" },
                       firewall { "Allow Hydra Link and phone control in the Windows Firewall (one administrator prompt)" },
                       launch { "Start Hydra Mixer afterwards" }, keepSettings { "Keep my settings (mixer, measurements, corrections)" };
    dialog::ActionButton primary { { "Install", dialog::Button::Role::primary }, theme::accentIn },
                         secondary { { "Run without installing" }, theme::accentIn };
    juce::String title, text, status;
    std::atomic<int> state { 0 };   // 0 idle, 1 working, 2 done, 3 failed
    juce::String error;
    juce::CriticalSection lock;
    std::unique_ptr<juce::Thread> worker;
};
} // namespace hydra

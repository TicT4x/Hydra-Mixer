#pragma once
#include "engine/Engine.h"
#include "midi/MidiMapper.h"
#include "midi/Hotkeys.h"
#include "Scenes.h"
#include "DeviceNames.h"

namespace hydra { class VacCables; }

namespace hydra
{
/** Persists the complete mixer state (devices, routing, parameters, plugins, MIDI map, hotkeys).

    Protection against losing settings:
      - every start keeps a copy of the session in backups/ (identical copies are skipped, newest 40 kept)
      - an unreadable session.xml is moved to backups/ instead of being overwritten with defaults
      - if session.xml was changed by someone else since we read it, that version is backed up first
      - import / restore / Voicemeeter import back up the current state first */
class Session
{
public:
    Session (Engine& e, MidiMapper& m, juce::File directory = appDataDir())
        : scenes (e), engine (e), midi (m), dir (std::move (directory)) {}

    /** Scenes (snapshots of all parameters), stored with the session. */
    SceneBank scenes;

    juce::File defaultFile() const { return dir.getChildFile ("session.xml"); }
    juce::File backupDir() const   { return dir.getChildFile ("backups"); }

    bool save (const juce::File& f) const;
    bool load (const juce::File& f);   // also opens the master device

    /** Startup: loads session.xml (quarantining it if unreadable) and backs it up. */
    bool loadDefault();
    /** Autosave to session.xml with the safety checks above. */
    bool saveDefault();

    /** Writes the current state to any file (settings export). */
    bool exportTo (const juce::File& f) const { return save (f); }
    /** Replaces the current state with a file (export or backup). Backs up the current state first.
        Returns false (and changes nothing) if the file is not a Hydra session. */
    bool importFrom (const juce::File& f, juce::String& error);

    /** Copies session.xml into backups/ ("session_<date>_<reason>.xml"). Returns the copy or {} if skipped. */
    juce::File backup (const juce::String& reason);
    /** Backups, newest first. */
    juce::Array<juce::File> listBackups() const;

    static bool isSessionFile (const juce::File& f);

    /** Application preferences (window / system tray), stored with the session. */
    struct AppPrefs
    {
        bool trayIcon = true;          // show an icon in the notification area
        bool closeToTray = false;      // the close button hides the window, audio keeps running
        bool minimiseToTray = false;   // minimising hides the window into the tray
        bool startInTray = false;      // start hidden in the tray
        float uiScale = 1.0f;          // zoom of the main window (1 = 100 %)
        float fontScale = 1.0f;        // text size factor for all windows
        float stripSplit = -1.0f;      // divider between inputs and hardware outputs (share of the width), < 0 = default
        float stripSplit2 = -1.0f;     // divider between hardware and virtual outputs (share of the width), < 0 = default
        bool renameDevices = false;    // name software devices in Windows after their channels (DeviceNamer)
        bool vacDirect = true;         // Virtual Audio Cable over Kernel Streaming, cable clock synced (VacStream)
        bool vacHideOwnSide = true;    // hide the cable side Hydra uses in Windows (VacCables::tickHiding)
        bool vacHideUnused = true;     // hide both sides of cables no channel uses (VacCables::tickHiding)
        bool vacAutoName = true;
        bool renameNoPrompt = true;    // rename via the task set up once, without a UAC prompt each time (DeviceNamer::useTask)       // name cables after their channel: "Hydra In 2", "Hydra Out 1" (DeviceNamer)
        bool expertDeviceModes = false; // device picker: all three Windows modes (else only Low Latency shared, the sensible one)
        bool pickerShowAll = false;    // device picker: every device, also those that do not fit the channel (Hydra's own cables, hardware on B buses ...)
        bool autoReconnect = true;     // reopen devices that went offline as soon as they are back (Engine::autoReconnect)
        bool adaptiveStrips = true;    // with room to spare the strips grow (up to StripArea::kMaxGrow), groups stay compact and centred
        bool horizontalStrips = false; // channels as rows (fader sideways) instead of vertical strips
        bool fitWindow = false;        // the window takes the size the channels need each time it appears (and when channels change)
        bool sidePanel = false;        // side panel (routing matrix) right of the strips while there is room for it
        bool stripDividers = true;     // draggable dividers between the groups of strips while they do not fit
        juce::String theme = "default"; // look of the app (theme::styleFromName)
        bool netEnabled = false;       // Hydra Link: audio over the network (engine/NetLink.h); off = no port open
        int netPort = 6980;            // UDP port of Hydra Link (net::kDefaultPort)
        juce::String netName;          // this computer as other Hydras see it (empty = Windows computer name)
        juce::String netPassword;      // network password (in the file only encrypted for this Windows user, DPAPI)
        juce::StringArray netPeers;    // other computers to announce to directly: "host[:port]" (VPN, internet)
        bool remoteEnabled = false;    // Hydra Remote: phones / tablets control Hydra in the browser (remote/RemoteControl.h)
        int remotePort = 6981;         // TCP port of the web app (remote::kDefaultPort)
        juce::String remoteCode;       // access code (in the file only encrypted for this Windows user, DPAPI); empty = new one when switched on
        bool operator== (const AppPrefs&) const = default;
    };

    juce::String windowState;          // stored with the session
    MasterConfig requestedMaster;      // master device as the loaded session asked for it
    bool waitingForMaster = false;     // requestedMaster is not open yet (startup with Windows): save it, not the fallback
    AppPrefs prefs;
    std::atomic<uint32_t> prefsVersion { 0 };   // bump after changing prefs (autosave + tray update)
    Hotkeys* hotkeys = nullptr;        // optional: keyboard shortcuts are stored with the session
    std::vector<DeviceRename> deviceRenames;   // Windows endpoints Hydra renamed (original names to restore)
    std::vector<juce::String> vacHidden;       // VAC endpoint ids Hydra hid in Windows (shown again when unused)
    DeviceNamer* deviceNamer = nullptr;        // optional: renames them (settings page shows its state)
    VacCables* vacCables = nullptr;            // optional: adds / removes Virtual Audio Cable cables

    static constexpr int kMaxBackups = 40;

private:
    void rememberDiskState();
    bool diskChangedSinceRemembered() const;
    void pruneBackups() const;

    Engine& engine;
    MidiMapper& midi;
    juce::File dir;
    juce::Time knownModTime;
    juce::int64 knownSize = -1;
};
} // namespace hydra

#pragma once
#include "engine/Engine.h"
#include "midi/MidiMapper.h"
#include "midi/Hotkeys.h"
#include "Scenes.h"

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
        float stripSplit = -1.0f;      // divider between inputs and outputs (share of the width), < 0 = default
        bool operator== (const AppPrefs&) const = default;
    };

    juce::String windowState;          // stored with the session
    MasterConfig requestedMaster;      // master device as the loaded session asked for it
    bool waitingForMaster = false;     // requestedMaster is not open yet (startup with Windows): save it, not the fallback
    AppPrefs prefs;
    std::atomic<uint32_t> prefsVersion { 0 };   // bump after changing prefs (autosave + tray update)
    Hotkeys* hotkeys = nullptr;        // optional: keyboard shortcuts are stored with the session

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

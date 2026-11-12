#pragma once
#include "Common.h"

#ifndef HYDRA_VERSION_SUFFIX
 #define HYDRA_VERSION_SUFFIX ""   // CMakeLists: "beta.1" for a prerelease (GitHub release marked as pre-release)
#endif

namespace hydra
{
/** Installation of Hydra Mixer for the current Windows user (no administrator rights needed, so updates run
    without a UAC prompt).

    A release is one exe (HydraMixer-<ver>-win64.exe). Started from anywhere else it offers to install itself:
    it copies itself to %LOCALAPPDATA%\Programs\Hydra Mixer\Hydra Mixer.exe, adds a Start menu entry, an entry
    under Windows "Apps" (uninstall) and, with one UAC prompt, a Windows Firewall rule for that path.
    Why the firewall rule: Windows remembers "allow network access" per exe path. Every downloaded version
    (HydraMixer-0.8-win64.exe, ...(1).exe, build folders) had its own path and asked again. The installed exe
    keeps its path across updates, so the rule (all network profiles) is made once.

    Updates use the same code: the new exe is written next to the installed one, the running one is renamed to
    "Hydra Mixer.old.exe" (Windows allows renaming a running exe), the new one takes its name and is started
    with kWaitArgument; it waits until this process has quit and deletes the .old file. */
namespace install
{
    constexpr const char* kWaitArgument = "--wait-pid";       // --wait-pid <pid>: wait for that process to end first
    constexpr const char* kUpdatedArgument = "--updated";     // started after an update / installation (shows a note)
    constexpr const char* kUninstallArgument = "--uninstall"; // Windows "Apps" > Uninstall
    constexpr const char* kFirewallArgument = "--firewall";   // elevated helper: --firewall add|remove "<exe>"
    constexpr const char* kInstallArgument = "--install";     // shows the installer even if the exe name does not look like a release

    /** This build's version, e.g. "0.8.1" or "0.9.0-beta.1". */
    juce::String currentVersion();
    /** Compares versions like "v0.9", "0.9.1-beta.2": < 0, 0, > 0. A prerelease is older than the same version without one. */
    int compareVersions (const juce::String& a, const juce::String& b);

    juce::File installDir();          // %LOCALAPPDATA%\Programs\Hydra Mixer
    juce::File installedExe();        // ...\Hydra Mixer.exe
    juce::File runningExe();
    /** The running exe is the installed one. */
    bool isRunningInstalled();
    /** An installed copy exists (registered in Windows "Apps" or the exe is there). */
    bool isInstalled();
    /** Version of the installed copy as registered (empty if unknown). */
    juce::String installedVersion();
    /** Started from a downloaded release (HydraMixer-*.exe) outside the installation, or with kInstallArgument:
        show the installer instead of the mixer. Build folders (Hydra Mixer.exe) are never offered. */
    bool shouldOfferInstall (const juce::String& commandLine);

    struct Options
    {
        bool startMenu = true, desktop = false, firewall = true;
    };
    /** Copies source (an exe of Hydra) into the installation (replacing an installed copy, also a running one),
        writes the shortcuts and the "Apps" entry, and with options.firewall the firewall rule (UAC prompt, waits).
        Returns an error text, empty on success. Any thread except the message thread for the firewall prompt is fine. */
    juce::String installFrom (const juce::File& source, const juce::String& version, const Options& options);
    /** Starts the installed exe (waiting for this process to quit) and returns true if it was started. */
    bool launchInstalled (bool updated);

    /** Removes shortcuts, the "Apps" entry, autostart and (UAC) the firewall rule; the folder is deleted right after this
        process quits. keepSettings = false also deletes %APPDATA%\HydraMixer. */
    void uninstall (bool keepSettings);

    /** Deletes leftovers of an update (Hydra Mixer.old.exe, unfinished downloads) next to the installed exe. */
    void cleanUpAfterUpdate();
    /** Waits (up to 20 s) for the process given with kWaitArgument to end. */
    void waitForPreviousInstance (const juce::String& commandLine);

    /** The firewall allows this exe in every network profile (read without administrator rights). */
    bool firewallAllows (const juce::File& exe);
    /** Adds / removes Hydra's firewall rule for exe in an elevated helper (one UAC prompt). Waits for it. */
    bool requestFirewall (const juce::File& exe, bool add);
    /** The elevated helper (kFirewallArgument): returns the process exit code. */
    int runFirewallJob (const juce::String& commandLine);

    /** Asks a running Hydra Mixer to quit (installer / uninstaller) and waits up to timeoutMs for it. */
    bool closeRunningInstance (int timeoutMs);
    /** The mixer listens for that request (call once at startup; true when it was asked to quit). */
    bool quitRequested();
    /** A mixer (another process of Hydra Mixer, not an installer) runs right now. */
    bool isMixerRunning();
}
} // namespace hydra

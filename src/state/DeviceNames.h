#pragma once
#include "engine/Engine.h"

namespace hydra
{
/** An endpoint Hydra renamed. Kept with the session so the original name can be restored. */
struct DeviceRename
{
    juce::String id;        // endpoint id (stays the same when the name changes)
    juce::String original;  // device description before Hydra renamed it ("Voicemeeter In 4")
    juce::String applied;   // description Hydra set ("Hydra Chrome")
};

/** Names the Windows endpoints through which apps reach a software input / output after its channel:
    "Hydra Chrome", or "Hydra In 4" / "Hydra B1" for an unnamed channel. Windows then lists them as
    "Hydra Chrome (VB-Audio Voicemeeter VAIO)": the part in brackets is the driver's and stays.

    Only software devices are renamed:
      - input with a loopback of a virtual playback device (Voicemeeter VAIO, VB-Cable ...): that device
      - input recording a virtual cable: the cable's playback side (where apps play into)
      - bus playing into a virtual cable: the cable's recording side (where apps record from)
    The side Hydra itself uses keeps its name, except a loopback device (it is both). Sources and outputs
    that name a renamed device are updated to its new name.

    The original name of every renamed endpoint is kept; it comes back when no channel uses the device any
    more or the option is switched off. Writing a name needs administrator rights: Hydra tries it in-process,
    then starts itself elevated (`--set-device-names <job file>`, one UAC prompt per batch). Message thread. */
class DeviceNamer
{
public:
    DeviceNamer (Engine& e, std::vector<DeviceRename>& renames);
    ~DeviceNamer();

    /** ~1 Hz. Renames / restores when the channels, their devices or the options changed. The first call
        after the start never shows a UAC prompt (only what works without one).
        enabled: software devices as described above. cables: Virtual Audio Cable cables a channel uses, both
        sides (the driver tells which endpoints belong to a cable, also a side Hydra hid in Windows):
        "Hydra In 2" for a cable input 2 records, "Hydra Out 1" for one B1 plays into ("Hydra Out A1" for a
        hardware bus), with the channel's own name: "Hydra In 4 - Discord". A cable no channel uses keeps its last
        name (it is hidden in Windows then; restoring it would cost another UAC prompt); all names come back when
        the option is switched off. */
    void tick (bool enabled, bool cables = false);

    /** Names endpoints by hand (Cable Manager: endpoint id -> description). Not kept as Hydra's rename, so it is
        never restored; a rename Hydra had made of such an endpoint is forgotten. May show a UAC prompt.
        done(all applied) on the message thread; false right away while another batch runs. */
    void renameNow (const std::vector<std::pair<juce::String, juce::String>>& names, std::function<void (bool)> done);

    /** After a batch: names, sources / outputs and the rename list changed (save + refresh the UI). */
    std::function<void()> onRenamed;

    /** Current renames and the result of the last batch, for the settings page. */
    juce::String describe() const;
    bool isBusy() const noexcept { return busy; }

    /** "Hydra Chrome"; unnamed channel: "Hydra " + code ("In 4", "B1"). */
    static juce::String nameFor (const juce::String& channelName, const juce::String& code);
    /** Name of a Virtual Audio Cable a channel uses: input i (0-based) -> "Hydra In <i+1>", bus b -> "Hydra Out 1"
        for B1, "Hydra Out A1" for A1; a channel with a name of its own adds it: "Hydra In 4 - Discord". */
    static juce::String cableNameFor (bool input, int index, const juce::String& channelName = {});

    /** Friendly name the endpoint had before the rename (for finding the other side of a cable):
        "Hydra Chrome (VB-Audio Cable A)" -> "CABLE-A Input (VB-Audio Cable A)". */
    static juce::String originalFriendly (const juce::String& friendly, const juce::String& desc, const DeviceRename* r);

    /** Command-line switch of the elevated helper and its entry point (applies a job file, returns the exit code).
        The job may also set up / remove the rename task (see below). The helper deletes the job file when done. */
    static constexpr const char* kJobArgument = "--set-device-names";
    static int runJob (const juce::File& jobFile);

    /** Renaming without a UAC prompt each time: the first elevated batch also registers a Task Scheduler task
        ("Hydra Mixer Device Names", run level highest, no trigger) that starts this exe with a fixed job file
        (%APPDATA%/HydraMixer/device-names-job.xml). Later batches write that file and start the task
        (`schtasks /Run`, no prompt). The task only renames audio endpoints. It is re-registered (one prompt) when it
        points to another exe, and removed when the option is switched off. */
    bool useTask = true;
    /** The task exists and starts this exe (asks Task Scheduler; worker threads too). */
    static bool isTaskReady();
    /** Task Scheduler XML of the task (for tests). */
    static juce::String taskDefinition();
    /** Removes the task (elevated helper if Windows refuses; may show a UAC prompt). Message thread. */
    void removeTask();

private:
    struct Want { juce::String id, name; };
    struct Job;

    juce::String stateKey (bool enabled, bool cables) const;
    /** keep: endpoints Hydra renamed that stay as they are although no channel wants them (unused cables). */
    std::vector<Want> resolve (bool enabled, bool cables, std::vector<juce::String>& keep) const;
    void start (const std::vector<Want>& wants, bool allowPrompt, bool manual = false, std::function<void (bool)> done = {},
                const std::vector<juce::String>& keep = {});
    void finish (Job& job);

    Engine& engine;
    std::vector<DeviceRename>& renames;
    juce::String lastKey, lastResult;
    bool busy = false;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
};
} // namespace hydra

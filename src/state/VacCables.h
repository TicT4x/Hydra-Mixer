#pragma once
#include "engine/Engine.h"
#include "state/DeviceNames.h"
#include "state/VacControl.h"
#include <map>

namespace hydra
{
/** Adds, edits and removes Virtual Audio Cable cables from Hydra (see vac:: in VacControl.h).

    Changing the number of cables runs in steps, all driven from the message thread:
      1. release: Hydra closes its own streams on VAC endpoints (Engine::holdDevice) and waits until no
         stream is left on any cable (max. 3 s; other apps may still hold one, then Windows may only apply
         the change at the next boot),
      2. set the number and restart the driver on a worker thread (elevated helper if needed: one UAC prompt),
      3. wait until Windows lists exactly the expected cables again (polled every 250 ms, max. 15 s), then
         reopen Hydra's streams.
    "Add a cable for a channel" then routes that channel to the new cable (an input records it, a bus plays
    into it). Only cables at the end can be removed (VAC numbers them), and only those no channel of Hydra
    uses and no app is streaming on. Listeners (ChangeListener) hear about every state change. */
class VacCables final : public juce::ChangeBroadcaster, private juce::Timer
{
public:
    VacCables (Engine&, std::vector<DeviceRename>& renames);
    ~VacCables() override;

    using Done = std::function<void (bool ok, const juce::String& message)>;

    /** Driver state and endpoints as last read (refresh() reads them again; a few ms). */
    const vac::Info& info() const noexcept { return state; }
    const std::vector<vac::Endpoint>& getEndpoints() const noexcept { return eps; }
    void refresh();
    bool isAvailable() const noexcept { return state.installed; }
    bool isBusy() const noexcept { return phase != Phase::idle; }
    /** What is happening or happened last ("Restarting the Virtual Audio Cable driver...", the result). */
    juce::String status() const;

    /** Sets the number of cables (1..256); the same number applies a pending change (driver restart). */
    void setCount (int n, Done done = {});
    /** A cable of its own for a channel (input: index 0..kMaxInputs-1; bus: 0..kMaxBuses-1): a free cable (no
        channel of Hydra and no app on it) if there is one, otherwise one more (driver restart). */
    void addCableFor (bool input, int index, Done done = {});
    /** Lowest cable that no channel of Hydra uses and no app streams on; 0 if none. */
    int freeCable() const;
    /** Cable k could be given to a channel now (see freeCable). */
    bool isReusable (int cable) const;
    /** Another process (not this Hydra) streams on cable k's recording (capture) or playback side. VAC lists
        Hydra's own Kernel Streaming pins too ("Hydra Mixer"), so they are told apart by process id. */
    static bool othersOn (const std::vector<vac::Stream>&, int cable, bool capture);
    static bool othersOn (const std::vector<vac::Stream>&, int cable);
    /** The stream is one of this Hydra's own. */
    static bool isOwn (const vac::Stream&);
    /** Removes the cables at the end that no channel uses and no app is streaming on. */
    void removeUnused (Done done = {});
    /** How many cables removeUnused would remove (streams: from vac::cables(), empty = ask the driver). */
    int removableAtEnd (const std::vector<vac::Cable>& cables = {}) const;

    /** Names both endpoints of a cable in Windows ("Line 3" -> "Game"); one UAC prompt. Sources / outputs on
        the cable follow the new name. */
    void rename (int cable, const juce::String& name, DeviceNamer&, Done done = {});

    /** VAC cable number a source / output uses (1..256), 0 if none. */
    int cableOf (const IoTarget&) const;
    /** Which way Hydra uses cable k: 1 = an input records it, 2 = a bus plays into it (3 = both, 0 = unused). */
    int usedSides (int cable) const;
    /** Channels of Hydra on cable k ("In 3 Chrome", "B1"). */
    juce::StringArray usersOf (int cable) const;
    /** Friendly name of a cable's endpoint (playback or recording side); default name if Windows lists none. */
    juce::String endpointName (int cable, bool capture) const;

    /** ~1 Hz, only with the direct connection (Kernel Streaming):
          - ownSide: hides in Windows the side of each cable that Hydra itself uses (an input's recording side, a
            bus's playback side), so apps only see the side meant for them. Only while that stream runs over
            Kernel Streaming (a hidden endpoint cannot be opened through WASAPI); a stream that is reopening keeps
            its side hidden for a few seconds.
          - unused: hides both sides of every cable no channel of Hydra uses, until a channel uses it.
        A side another app is streaming on stays visible. What Hydra hid is kept in `hidden` (saved with the
        session) and shown again when it is no longer wanted. Endpoints shown / hidden by hand stay as they are. */
    void tickHiding (bool ownSide, bool unused, std::vector<juce::String>& hidden);
    /** A source / output on a VAC cable whose name is not that of its side (both sides of a cable can have
        different names; an older rename could point it to the other one): set to the right endpoint. True if
        something changed. Called by tickHiding. */
    bool repairTargets();

    /** Sources / outputs or device names changed (save the session, refresh the strips). */
    std::function<void()> onRoutingChanged;

private:
    enum class Phase { idle, releasing, working, appearing };
    struct Assign { bool input = false; int index = -1; };

    void timerCallback() override;
    void start (int n, Assign assign, Done done);
    void runDriverChange();
    /** Routes the channel in `assign` to cable k; returns the message for the user. */
    juce::String assignTo (int k);
    void finish (bool ok, const juce::String& message);
    /** Windows lists again what it listed before the change (endpoints a user disabled stay away), the new
        cables on both sides, and nothing above the new number. */
    bool endpointsMatch() const;

    Engine& engine;
    std::vector<DeviceRename>& renames;
    vac::Info state;
    std::vector<vac::Endpoint> eps;
    std::map<juce::String, int> notStreaming;   // hidden own side: ticks its stream has not run over Kernel Streaming
    std::vector<std::pair<int, bool>> expected;   // (cable, capture) Windows must list after the change
    Phase phase = Phase::idle;
    int target = 0, before = 0;
    Assign assign;
    Done done;
    juce::uint32 phaseStart = 0;
    juce::String lastResult;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
};
} // namespace hydra

#pragma once
#include <juce_core/juce_core.h>
#include <vector>

/** Virtual Audio Cable (VAC 4.70+, Eugene Muzychenko) driver control through its documented API
    (external/vacapi/VACAPI.h, API version 6): how many cables exist, and setting that number.

    VAC creates N cables "Line 1" .. "Line N" (each a playback and a recording endpoint
    "Line k (Virtual Audio Cable)"). Cables are not added one by one: the number is set and the driver
    restarts, which removes all cable endpoints for a moment. Setting the number needs no special rights,
    restarting the driver needs administrator rights; Hydra then starts itself elevated
    (`--vac-cables <n>`, one UAC prompt). A restart only works while no stream uses a cable (otherwise
    Windows applies it at the next boot). No audio thread involvement; any thread. */
namespace hydra::vac
{
struct Info
{
    bool installed = false;      // driver found and its API answered
    juce::String edition;        // "full" / "trial" / "lite"
    juce::String version;        // "4.71.0"
    int numCables = 0;
    int totalStreams = 0;        // audio streams on all cables (any process)
    bool restartPending = false; // a new number of cables waits for a driver restart
    juce::String error;          // why it is not available
};

/** Reads the driver state (no administrator rights needed). */
Info query();

/** Who streams on the cables right now: "audiodg (Line 6, recording)" ... (audiodg = an app through Windows). */
juce::StringArray streamOwners();

/** One stream on a cable (through Windows or an app's own Kernel Streaming; Hydra's own pins are not listed). */
struct Stream
{
    int cable = 0;
    bool capture = false;      // records the cable (its recording side)
    juce::String process;      // "audiodg.exe" = an app through Windows' audio engine
    unsigned int processId = 0;
};
std::vector<Stream> streams();

/** Streams (recording + playback) on each cable, index 0 = cable 1. Empty if not available. */
std::vector<int> streamsPerCable();

/** Formats a cable accepts (VAC "format range"). */
struct FormatRange
{
    int minRate = 0, maxRate = 0, minBits = 0, maxBits = 0, minChannels = 0, maxChannels = 0;
    bool operator== (const FormatRange&) const = default;
};

/** State of one cable as the driver reports it. */
struct Cable
{
    int number = 0;            // 1..N
    int msPerInt = 0;          // timing period of the cable ("ms per interrupt")
    int maxInstances = 0;      // how many clients can use each side at once
    FormatRange range;
    int rate = 0, bits = 0, channels = 0;   // current format (0 while unused)
    int inStreams = 0, outStreams = 0;      // recording / playback streams now
    int overflows = 0, underflows = 0;      // the cable's own buffer ran over / empty (since reset)
    int eventMinUs = 0, eventMaxUs = 0, eventAvgUs = 0;   // measured period of its timer events
    int actualRate = 0;        // measured sample rate
};
/** All cables; empty if VAC is not available. */
std::vector<Cable> cables();

/** Changes a cable setting (no administrator rights needed; applies when its streams restart). */
bool setMsPerInt (int cable, int ms);
bool setFormatRange (int cable, const FormatRange&);
bool resetCounters (int cable);

/** Cable clock speed relative to its nominal rate (1.0 = exact; VAC's "clock correction", precision 1e-9).
    Takes effect immediately, also while streams run. Cheap after the first call (the control path is kept). */
double getClockCorrection (int cable);
bool setClockCorrection (int cable, double ratio);

/** A Windows audio endpoint of a VAC cable, with the cable it belongs to (asked from the driver, so it
    also works for renamed endpoints). Needs COM on the calling thread. */
struct Endpoint
{
    juce::String id, friendly, desc;   // desc = the name part before the brackets ("Line 3", "Hydra Chrome")
    juce::String filterPath;           // its Kernel Streaming wave filter (for direct streaming)
    int cable = 0;                     // 1..N
    bool capture = false;              // recording side
    bool visible = true;               // false: disabled in Windows (apps do not see it; Kernel Streaming still works)
};
/** All VAC endpoints Windows knows, active and disabled. */
std::vector<Endpoint> endpoints();

/** Shows / hides an endpoint in Windows (the "Disable" of the sound settings: apps no longer list it). The
    cable keeps working over Kernel Streaming. Needs COM; no administrator rights. */
bool setEndpointVisible (const juce::String& endpointId, bool visible);

enum class Result
{
    done = 0,          // the driver runs with the new number of cables
    needsReboot = 2,   // number set, Windows restarts the driver at the next boot (a cable was in use)
    notInstalled = 3,
    setFailed = 4,
    restartFailed = 5, // usually: no administrator rights
    declined = 6,      // UAC prompt declined
};
juce::String describe (Result);

/** Sets the number of cables (1..256) and restarts the driver, in this process. Blocks (up to a few s).
    restart = false: only sets the number (e.g. to take back a change whose restart was declined). */
Result setNumCables (int n, bool restart = true);

/** The same, elevated in a second instance of Hydra (UAC prompt). Blocks until it finished (max. 60 s). */
Result setNumCablesElevated (int n);

/** Command-line switch of the elevated helper and its entry point (returns the Result as exit code). */
inline constexpr const char* kJobArgument = "--vac-cables";
int runJob (const juce::String& commandLine);

/** True for a VAC endpoint ("Line 3 (Virtual Audio Cable)", also after Hydra renamed it:
    "Hydra Chrome (Virtual Audio Cable)"). */
bool isCableEndpoint (const juce::String& friendlyName);

/** Cable number of an endpoint description ("Line 3" -> 3), 0 if it is not of that form. */
int cableNumberOf (const juce::String& description);

/** Friendly name VAC gives cable k on both sides: "Line k (Virtual Audio Cable)". */
juce::String defaultEndpointName (int cable);
} // namespace hydra::vac

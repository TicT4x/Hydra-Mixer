#include "VacCables.h"
#include "engine/VacStream.h"
#include <windows.h>

namespace hydra
{
namespace
{
constexpr const char* kLowLatencyType = "Windows Audio (Low Latency Mode)";

bool usesWindowsEndpoint (const IoTarget& t)
{
    return (t.kind == IoTarget::Kind::Device && t.typeName.startsWith ("Windows Audio")) || t.kind == IoTarget::Kind::Loopback;
}
} // namespace

VacCables::VacCables (Engine& e, std::vector<DeviceRename>& r) : engine (e), renames (r)
{
    refresh();
}

VacCables::~VacCables()
{
    alive->store (false);
    if (phase != Phase::idle) engine.holdDevice = nullptr;
}

void VacCables::refresh()
{
    if (phase == Phase::working) return;   // the driver is restarting
    state = vac::query();
    eps = state.installed ? vac::endpoints() : std::vector<vac::Endpoint>();
}

juce::String VacCables::status() const
{
    switch (phase)
    {
        case Phase::releasing: return "Closing Hydra's cable streams...";
        case Phase::working:   return "Restarting the Virtual Audio Cable driver (confirm the Windows prompt if it appears)...";
        case Phase::appearing: return "Waiting for Windows to list the cables...";
        case Phase::idle:      break;
    }
    return lastResult;
}

int VacCables::cableOf (const IoTarget& t) const
{
    if (! usesWindowsEndpoint (t) || ! vac::isCableEndpoint (t.deviceName)) return 0;
    for (auto& e : eps) if (e.friendly == t.deviceName) return e.cable;   // asked from the driver
    const auto desc = t.deviceName.upToLastOccurrenceOf (" (", false, false).trim();
    if (const int k = vac::cableNumberOf (desc); k > 0) return k;
    for (auto& r : renames)   // renamed by Hydra: its original name has the number
        if (r.applied == desc)
            if (const int k = vac::cableNumberOf (r.original); k > 0) return k;
    return 0;
}

juce::String VacCables::endpointName (int cable, bool capture) const
{
    for (auto& e : eps) if (e.cable == cable && e.capture == capture) return e.friendly;
    return vac::defaultEndpointName (cable);
}

juce::StringArray VacCables::usersOf (int cable) const
{
    juce::StringArray users;
    for (int i = 0; i < kMaxInputs; ++i)
    {
        const auto& s = engine.inputs[(size_t) i];
        if (engine.isInputActive (i) && cableOf (s.source) == cable)
            users.add (("In " + juce::String (i + 1) + " " + s.name).trim());
    }
    for (int b = 0; b < kMaxBuses; ++b)
    {
        const auto& bus = engine.buses[(size_t) b];
        if (engine.isBusActive (b) && cableOf (bus.output) == cable)
            users.add ((busLabel (b) + " " + bus.name).trim());
    }
    return users;
}

int VacCables::usedSides (int cable) const
{
    int sides = 0;
    for (int i = 0; i < kMaxInputs; ++i)
        if (engine.isInputActive (i) && cableOf (engine.inputs[(size_t) i].source) == cable) sides |= 1;
    for (int b = 0; b < kMaxBuses; ++b)
        if (engine.isBusActive (b) && cableOf (engine.buses[(size_t) b].output) == cable) sides |= 2;
    return sides;
}

bool VacCables::repairTargets()
{
    if (eps.empty()) return false;
    bool changed = false;
    auto fix = [&] (IoTarget& t, bool capture)
    {
        if (t.kind != IoTarget::Kind::Device || ! t.typeName.startsWith ("Windows Audio")) return;
        const int k = cableOf (t);
        if (k <= 0) return;
        for (auto& e : eps) if (e.cable == k && e.capture == capture && e.friendly == t.deviceName) return;   // fine
        for (auto& e : eps)
            if (e.cable == k && e.capture == capture)
            {
                logLine ("VAC: " + juce::String (capture ? "source" : "output") + " \"" + t.deviceName + "\" -> \"" + e.friendly
                         + "\" (cable " + juce::String (k) + ", " + (capture ? "recording" : "playback") + " side)");
                t.deviceName = e.friendly;
                changed = true;
                return;
            }
    };
    for (int i = 0; i < kMaxInputs; ++i)
        if (engine.isInputActive (i)) fix (engine.inputs[(size_t) i].source, true);
    for (int b = 0; b < kMaxBuses; ++b)
        if (engine.isBusActive (b)) fix (engine.buses[(size_t) b].output, false);
    return changed;
}

bool VacCables::othersOn (const std::vector<vac::Stream>& streams, int cable, bool capture)
{
    const auto self = (unsigned int) GetCurrentProcessId();
    for (auto& s : streams) if (s.cable == cable && s.capture == capture && s.processId != self) return true;
    return false;
}

bool VacCables::isOwn (const vac::Stream& s)
{
    return s.processId == (unsigned int) GetCurrentProcessId();
}

bool VacCables::othersOn (const std::vector<vac::Stream>& streams, int cable)
{
    return othersOn (streams, cable, false) || othersOn (streams, cable, true);
}

bool VacCables::isReusable (int cable) const
{
    if (! state.installed || usedSides (cable) != 0) return false;
    if (cable < 1 || cable > state.numCables || othersOn (vac::streams(), cable)) return false;
    int sides = 0;   // both sides must be known to Windows (shown or hidden)
    for (auto& e : eps) if (e.cable == cable) sides |= e.capture ? 2 : 1;
    return sides == 3;
}

int VacCables::freeCable() const
{
    if (! state.installed) return 0;
    for (int k = 1; k <= state.numCables; ++k)
        if (isReusable (k)) return k;
    return 0;
}

int VacCables::removableAtEnd (const std::vector<vac::Cable>& known) const
{
    if (! state.installed) return 0;
    const auto list = known.empty() ? vac::cables() : known;
    int n = 0;
    for (int k = state.numCables; k > 1; --k)   // VAC needs at least one cable
    {
        const bool streaming = (size_t) (k - 1) < list.size() && list[(size_t) (k - 1)].inStreams + list[(size_t) (k - 1)].outStreams > 0;
        if (streaming || ! usersOf (k).isEmpty()) break;
        ++n;
    }
    return n;
}

void VacCables::tickHiding (bool ownSide, bool unused, std::vector<juce::String>& hidden)
{
    if (phase != Phase::idle || ! state.installed) return;
    if (! ownSide && ! unused && hidden.empty()) return;
    refresh();

    if (repairTargets())
    {
        engine.rebuildGraph();
        if (onRoutingChanged) onRoutingChanged();
    }

    std::vector<juce::String> want;
    auto wantHidden = [&] (int k, bool capture)
    {
        for (auto& e : eps)
            if (e.cable == k && e.capture == capture && std::find (want.begin(), want.end(), e.id) == want.end())
                want.push_back (e.id);
    };
    if ((ownSide || unused) && isVacDirectEnabled())
    {
        const auto list = vac::cables();
        const auto streamList = vac::streams();
        // Another app streaming on that side still needs to see it (Hydra's own pins do not count).
        auto othersOn = [&] (int k, bool capture) { return VacCables::othersOn (streamList, k, capture); };
        auto isHidden = [&] (int k, bool capture)
        {
            for (auto& e : eps)
                if (e.cable == k && e.capture == capture)
                    return std::find (hidden.begin(), hidden.end(), e.id) != hidden.end();
            return false;
        };
        auto consider = [&] (const IoTarget& t, bool input)
        {
            if (! ownSide || t.kind != IoTarget::Kind::Device || ! t.typeName.startsWith ("Windows Audio")) return;
            const int k = cableOf (t);
            if (k <= 0 || othersOn (k, input)) return;
            const auto key = juce::String (k) + (input ? "c" : "r");
            if (engine.getIoLatency (t, input).transport.isNotEmpty()) notStreaming.erase (key);
            // Not (yet) over Kernel Streaming: a side Hydra hid stays hidden while the stream reopens (a few seconds,
            // e.g. after a rename), otherwise it has to be visible for WASAPI.
            else if (! isHidden (k, input) || ++notStreaming[key] > 5) return;
            wantHidden (k, input);
        };
        for (int i = 0; i < kMaxInputs; ++i)
            if (engine.isInputActive (i)) consider (engine.inputs[(size_t) i].source, true);
        for (int b = 0; b < kMaxBuses; ++b)
            if (engine.isBusActive (b)) consider (engine.buses[(size_t) b].output, false);
        if (unused)
            for (auto& c : list)
                if (usedSides (c.number) == 0)
                    for (bool capture : { false, true })
                        if (! othersOn (c.number, capture)) wantHidden (c.number, capture);

        // The side apps use of a cable a channel uses must be visible, whoever hid it (Windows may hide an endpoint
        // again when its name is written): it counts as Hydra's to show.
        for (auto& e : eps)
        {
            const int sides = usedSides (e.cable);
            const bool appsSide = e.capture ? (sides & 2) != 0 : (sides & 1) != 0;
            if (appsSide && ! e.visible && std::find (want.begin(), want.end(), e.id) == want.end()
                && std::find (hidden.begin(), hidden.end(), e.id) == hidden.end())
                hidden.push_back (e.id);
        }
    }

    bool changed = false;
    for (auto& id : want)
    {
        auto e = std::find_if (eps.begin(), eps.end(), [&] (const vac::Endpoint& x) { return x.id == id; });
        if (e == eps.end()) continue;
        const bool listed = std::find (hidden.begin(), hidden.end(), id) != hidden.end();
        if (! e->visible)
        {
            if (! listed) { hidden.push_back (id); changed = true; }   // hidden already (by hand or by Windows): Hydra's now
            continue;
        }
        // Listed but visible again (shown by hand or by Windows): hidden again, the state is checked every tick.
        if (vac::setEndpointVisible (id, false))
        {
            if (! listed) hidden.push_back (id);
            changed = true;
            logLine ("VAC: hid \"" + e->friendly + "\" (" + (e->capture ? "recording" : "playback") + " side) in Windows");
        }
    }
    for (auto it = hidden.begin(); it != hidden.end();)
    {
        if (std::find (want.begin(), want.end(), *it) != want.end()) { ++it; continue; }
        auto e = std::find_if (eps.begin(), eps.end(), [&] (const vac::Endpoint& x) { return x.id == *it; });
        if (e == eps.end() && eps.empty()) { ++it; continue; }   // endpoints not readable right now: keep it
        if (e != eps.end() && ! e->visible && ! vac::setEndpointVisible (*it, true)) { ++it; continue; }   // try again later
        if (e != eps.end()) logLine ("VAC: showed \"" + e->friendly + "\" in Windows again");
        it = hidden.erase (it);
        changed = true;
    }
    if (changed)
    {
        refresh();
        sendChangeMessage();
        if (onRoutingChanged) onRoutingChanged();   // the list is saved with the session
    }
}

void VacCables::setCount (int n, Done d) { start (n, {}, std::move (d)); }

void VacCables::addCableFor (bool input, int index, Done d)
{
    if (phase != Phase::idle) { if (d) d (false, "A cable change is already running."); return; }
    refresh();
    if (const int k = freeCable(); k > 0)
    {
        // A free cable: no driver restart needed.
        assign = { input, index };
        const auto msg = assignTo (k);
        assign = {};
        engine.rebuildGraph();
        if (onRoutingChanged) onRoutingChanged();
        lastResult = msg;
        logLine ("VAC: " + msg);
        sendChangeMessage();
        if (d) d (true, msg);
        return;
    }
    start (state.numCables + 1, { input, index }, std::move (d));
}

juce::String VacCables::assignTo (int k)
{
    IoTarget t;
    t.kind = IoTarget::Kind::Device;
    t.typeName = kLowLatencyType;
    t.deviceName = endpointName (k, assign.input);
    if (assign.input)
    {
        engine.inputs[(size_t) assign.index].source = t;
        return "Cable " + juce::String (k) + " is now the cable of In " + juce::String (assign.index + 1)
             + ": apps play into it, this input records it.";
    }
    engine.buses[(size_t) assign.index].output = t;
    return "Cable " + juce::String (k) + " is now the cable of " + busLabel (assign.index)
         + ": this output plays into it, apps record from it.";
}

void VacCables::removeUnused (Done d)
{
    refresh();
    const int n = removableAtEnd();
    if (n == 0)
    {
        if (d) d (false, "No unused cable at the end to remove.");
        return;
    }
    start (state.numCables - n, {}, std::move (d));
}

void VacCables::rename (int cable, const juce::String& name, DeviceNamer& namer, Done d)
{
    const auto clean = name.trim();
    if (clean.isEmpty() || clean.containsAnyOf ("()")) { if (d) d (false, "Please choose a name without brackets."); return; }
    std::vector<std::pair<juce::String, juce::String>> names;
    for (auto& e : eps) if (e.cable == cable && e.id.isNotEmpty()) names.emplace_back (e.id, clean);
    if (names.empty()) { if (d) d (false, "Windows does not list this cable right now."); return; }
    lastResult = "Renaming cable " + juce::String (cable) + "... (confirm the Windows prompt if it appears)";
    sendChangeMessage();
    namer.renameNow (names, [this, cable, clean, d, token = alive] (bool ok)
    {
        if (! token->load()) return;
        refresh();
        lastResult = ok ? "Cable " + juce::String (cable) + " is now \"" + clean + "\"."
                        : "Cable " + juce::String (cable) + " was not renamed (administrator rights needed).";
        sendChangeMessage();
        if (d) d (ok, lastResult);
    });
}

void VacCables::start (int n, Assign a, Done d)
{
    if (phase != Phase::idle) { if (d) d (false, "A cable change is already running."); return; }
    refresh();
    if (! state.installed) { if (d) d (false, state.error.isNotEmpty() ? state.error : "Virtual Audio Cable is not installed."); return; }
    n = juce::jlimit (1, 256, n);
    if (state.edition == "lite" && n > 1) { if (d) d (false, "The Lite version of Virtual Audio Cable has only one cable."); return; }
    if (n == state.numCables && ! state.restartPending) { if (d) d (true, "Nothing to change."); return; }

    target = n;
    before = state.numCables;
    assign = a;
    expected.clear();
    for (auto& e : eps) if (e.cable <= target) expected.emplace_back (e.cable, e.capture);
    for (int k = before + 1; k <= target; ++k) { expected.emplace_back (k, false); expected.emplace_back (k, true); }
    done = std::move (d);
    logLine ("VAC: cables " + juce::String (before) + " -> " + juce::String (target));

    // 1. Hydra lets go of every cable (the restart removes them all for a moment anyway).
    engine.holdDevice = [] (const IoTarget& t) { return usesWindowsEndpoint (t) && vac::isCableEndpoint (t.deviceName); };
    engine.rebuildGraph();
    phase = Phase::releasing;
    phaseStart = juce::Time::getMillisecondCounter();
    startTimer (100);
    sendChangeMessage();
}

void VacCables::timerCallback()
{
    const auto elapsed = juce::Time::getMillisecondCounter() - phaseStart;
    if (phase == Phase::releasing)
    {
        // Hydra's streams close on the opener thread shortly after the graph swap.
        const auto now = vac::query();
        if (elapsed < 200 || (now.totalStreams > 0 && elapsed < 3000)) return;
        if (now.totalStreams > 0)
        {
            // Restarting now would fail and leave the driver waiting for a reboot (Windows then refuses every
            // further restart until one). Say who is in the way instead.
            const auto who = vac::streamOwners();
            logLine ("VAC: not restarted, still in use by " + who.joinIntoString (", "));
            finish (false, "Not changed: the cables are still in use by " + (who.isEmpty() ? juce::String ("another app") : who.joinIntoString (", "))
                               + ". \"audiodg\" is an app using the cable through Windows (also \"Listen to this device\"). "
                               "Close it and try again.");
            return;
        }
        runDriverChange();
    }
    else if (phase == Phase::appearing)
    {
        eps = vac::endpoints();
        if (! endpointsMatch() && elapsed < 15000) return;
        if (! endpointsMatch()) logLine ("VAC: Windows did not list all cables within 15 s");
        finish (true, "Virtual Audio Cable now has " + juce::String (target) + (target == 1 ? " cable." : " cables."));
    }
}

bool VacCables::endpointsMatch() const
{
    for (auto& e : eps)
        if (e.cable > target) return false;   // a removed cable is still listed
    for (auto& [cable, capture] : expected)
        if (std::none_of (eps.begin(), eps.end(), [&] (const vac::Endpoint& e) { return e.cable == cable && e.capture == capture; }))
            return false;
    return true;
}

void VacCables::runDriverChange()
{
    stopTimer();
    phase = Phase::working;
    sendChangeMessage();

    juce::Thread::launch ([this, n = target, old = before, token = alive]
    {
        auto res = vac::setNumCables (n);
        if (res == vac::Result::restartFailed) res = vac::setNumCablesElevated (n);
        if (res == vac::Result::declined || res == vac::Result::restartFailed)
            vac::setNumCables (old, false);   // nothing pending for the next boot

        juce::MessageManager::callAsync ([this, res, token]
        {
            if (! token->load()) return;
            state = vac::query();
            if (res == vac::Result::done)
            {
                phase = Phase::appearing;
                phaseStart = juce::Time::getMillisecondCounter();
                sendChangeMessage();
                startTimer (250);
            }
            else
                finish (false, vac::describe (res));
        });
    });
}

void VacCables::finish (bool ok, const juce::String& text)
{
    stopTimer();
    phase = Phase::idle;
    engine.holdDevice = nullptr;
    refresh();
    // JUCE's device lists (pickers, settings) see the new endpoints only after a rescan.
    for (auto* t : engine.getDeviceTypes())
        if (t->getTypeName().startsWith ("Windows Audio")) t->scanForDevices();

    auto msg = text;
    if (ok && assign.index >= 0 && state.numCables >= target)
        msg = "New c" + assignTo (target).substring (1);
    engine.rebuildGraph();
    if (onRoutingChanged) onRoutingChanged();
    assign = {};
    lastResult = msg;
    logLine ("VAC: " + msg);
    sendChangeMessage();
    if (auto d = std::move (done)) { done = nullptr; d (ok, msg); }
}
} // namespace hydra

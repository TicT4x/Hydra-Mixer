#pragma once
#include "RemoteServer.h"
#include "engine/Engine.h"
#include <map>
#include <set>

namespace hydra
{
class SceneBank;

/** Hydra Remote: control and display Hydra from a phone or tablet browser (web app served by remote::Server).

    Message thread only. While at least one phone is connected a timer (25 Hz) sends what changed:
      - "state": channels (names, sources, bus labels / icons), scenes and every exposed parameter. Sent when a
        phone joins and whenever the layout, a name or the scene list changes.
      - "p": parameters whose version changed since the last tick: { id: [normalised, text] }.
      - binary meter frames (only to phones that show the mixer): type byte 1, then per channel in state order
        (inputs, then buses) L, R (0 = -60 dB or less, 255 = +6 dB) and flags (bit 0 = muted).
      - "s" once a second: DSP load, sample rate, block size.
    Commands from the phone: set (normalised value), toggle, reset (default), scene (recall), meters (on / off).
    Only the exposed parameters can be changed (strip controls, routing, sends, bus switches, monitoring): no
    devices, no effects, nothing that removes channels. Changes count like MIDI (not undo steps, Param::set external).
    Without phones the timer runs at 4 Hz, only to notice new ones. */
class RemoteControl final : private juce::Timer
{
public:
    RemoteControl (Engine&, SceneBank&);
    ~RemoteControl() override;

    /** The app's instance (settings page), nullptr if none. */
    static RemoteControl* get() noexcept;

    /** On / off / changed port or code. False + error if the port cannot be opened. */
    bool apply (bool enabled, int port, const juce::String& code, juce::String& error);
    remote::Server& getServer() noexcept { return server; }

    /** Name shown in the phone's title bar (the Hydra Link name). */
    std::function<juce::String()> nameProvider;

    /** Addresses a phone can open ("http://192.168.1.20:6981/"), one per local IPv4 address; empty if off. */
    juce::StringArray links() const;
    /** The address to show first (the most likely home network). */
    juce::String preferredLink() const;

    /** Parameters a phone can change for the current layout (tests). */
    bool isExposed (const juce::String& id) const { return byId.count (id) != 0; }

    /** Handles one command as if a phone had sent it (tests; client 0). */
    void handleMessage (int client, const juce::String& json);

private:
    void timerCallback() override;
    void sendState (int client);
    juce::String layoutKey() const;
    void rebuildExposed();
    void sendDiffs();
    void sendMeters();
    void sendStatus();
    void updateMeterUsers();

    Engine& engine;
    SceneBank& scenes;
    remote::Server server;

    struct Exposed { Param* p; uint32_t version; };
    std::vector<Exposed> exposed;
    std::map<juce::String, Param*> byId;
    std::vector<int> shownInputs, shownBuses;   // meter order
    std::set<int> clients, meterClients;
    bool meterUsed = false;
    juce::String shownLayout;
    Engine::LoadMeter load;
    int ticks = 0;
};
} // namespace hydra

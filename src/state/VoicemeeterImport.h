#pragma once
#include "engine/Engine.h"
#include "midi/MidiMapper.h"

namespace hydra
{
/** Imports a Voicemeeter (Potato) settings XML: devices (ASIO driver, channel patches, WDM devices),
    strip/bus names, routing, faders, mute/solo/mono, EQ, gate, compressor, denoiser and the MIDI map.
    Returns a human-readable report of what was mapped and what could not be. */
struct VoicemeeterImport
{
    struct Result
    {
        bool ok = false;
        juce::StringArray report;
    };

    /** applyDevices=false only reads and maps parameters (used by tests / dry runs). */
    static Result run (const juce::File& vmXml, Engine& engine, MidiMapper& midi, bool applyDevices = true);

    /** Finds Voicemeeter settings files in the user's Documents folder (newest first). */
    static juce::Array<juce::File> findCandidates();
};
} // namespace hydra

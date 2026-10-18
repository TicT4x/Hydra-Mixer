#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <array>
#include <complex>
#include <atomic>
#include <memory>
#include <vector>

namespace hydra
{
// Channel slots. Every slot exists (parameters, buffers) from the start; the user adds / removes
// channels by switching slots on and off (inX.active / bus.X.active), so nothing is allocated or
// renumbered while the engine runs. Bus index layout: 0..7 = A1..A8, 8..15 = B1..B8.
constexpr int kMaxInputs    = 16;  // input strips 1..16 (hardware or software)
constexpr int kMaxHwBuses   = 8;   // A1..A8
constexpr int kMaxVirtBuses = 8;   // B1..B8
constexpr int kMaxBuses     = kMaxHwBuses + kMaxVirtBuses;
// Layout of a new session (= Voicemeeter Potato): 8 inputs, A1-A5, B1-B3.
constexpr int kDefaultInputs = 8, kDefaultHwBuses = 5, kDefaultVirtBuses = 3;
constexpr int kMaxBlock     = 4096; // engine processes in chunks of at most this size
constexpr int kMaxDeviceChannels = 32;
constexpr int kMaxStreamChannels = 32; // channels carried by one asynchronous stream (16 stereo strips / buses)
constexpr int kMaxScenes = 64;          // scene slots (each has a recall parameter for MIDI / shortcuts)

constexpr bool isHwBus (int b) noexcept      { return b < kMaxHwBuses; }
constexpr int hwBusIndex (int a) noexcept    { return a; }                 // A(a+1)
constexpr int virtBusIndex (int v) noexcept  { return kMaxHwBuses + v; }   // B(v+1)

inline juce::String busCode (int b)
{
    return isHwBus (b) ? "A" + juce::String (b + 1) : "B" + juce::String (b - kMaxHwBuses + 1);
}

inline juce::File appDataDir()
{
    auto d = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("HydraMixer");
    d.createDirectory();
    return d;
}

/** Appends a timestamped line to %APPDATA%/HydraMixer/hydra.log (message thread only, never audio). */
inline void logLine (const juce::String& text)
{
    auto f = appDataDir().getChildFile ("hydra.log");
    if (f.getSize() > 512 * 1024)
        f.deleteFile();
    f.appendText (juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H:%M:%S  ") + text + "\n", false, false, "\r\n");
}

inline float dbToGain (float db, float floorDb = -60.0f) noexcept
{
    return db <= floorDb ? 0.0f : std::pow (10.0f, db * 0.05f);
}

inline float gainToDb (float g) noexcept
{
    return g > 1.0e-6f ? 20.0f * std::log10 (g) : -120.0f;
}
} // namespace hydra

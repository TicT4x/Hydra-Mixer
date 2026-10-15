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
constexpr int kNumInputs    = 8;   // input strips 1..8 (hardware or software)
constexpr int kNumHwBuses   = 5;   // A1..A5
constexpr int kNumVirtBuses = 3;   // B1..B3
constexpr int kNumBuses     = kNumHwBuses + kNumVirtBuses;
constexpr int kMaxBlock     = 4096; // engine processes in chunks of at most this size
constexpr int kMaxDeviceChannels = 32;
constexpr int kMaxStreamChannels = 16; // channels carried by one asynchronous stream (8 stereo strips / buses)

inline juce::String busCode (int b)
{
    return b < kNumHwBuses ? "A" + juce::String (b + 1) : "B" + juce::String (b - kNumHwBuses + 1);
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

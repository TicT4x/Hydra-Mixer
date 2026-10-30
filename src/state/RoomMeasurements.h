#pragma once
#include "Common.h"
#include "core/RoomAnalysis.h"

namespace hydra
{
/** A finished room measurement as kept in the library (%APPDATA%/HydraMixer/Measurements, one .hydrameasure file each).
    Only the impulse response and how it was measured are stored; everything shown (frequency response,
    waterfall, decay, resonances) is computed from it again (room::analyse). */
struct RoomMeasurement
{
    juce::String name;
    juce::Time created;
    juce::String microphone, output;   // what was measured (descriptions at measuring time)
    room::SweepSpec spec;
    bool throughEq = false;            // the sweep passed the output's EQ and Correction (a check of a correction)
    room::Calibration calibration;     // microphone calibration taken out of the results (empty = none)
    room::ImpulseResponse ir;
    juce::File file;                   // library file (empty until saved)

    room::Analysis analysis;           // computed, not stored
    bool analysed = false;
};

namespace measurements
{
inline constexpr const char* kExtension = ".hydrameasure";

/** Library folder (created on demand). */
juce::File folder();
/** All measurements of the library, newest first (not analysed yet). */
std::vector<std::unique_ptr<RoomMeasurement>> loadLibrary();

/** Writes m to file (JSON with the impulse response as base64 float32). */
bool save (const RoomMeasurement& m, const juce::File& file);
/** Stores m in the library (a new file the first time, the same file afterwards). */
bool saveToLibrary (RoomMeasurement& m);
std::unique_ptr<RoomMeasurement> load (const juce::File& file, juce::String& error);

/** Frequency response as text: "freq dB" per line (REW and most measurement tools import this). */
bool exportResponse (const RoomMeasurement& m, const juce::File& file, double smoothingFraction);
/** Impulse response as 32-bit float WAV (convolution, other analysers). */
bool exportImpulse (const RoomMeasurement& m, const juce::File& file);
/** Waterfall as CSV: one row per frequency, one column per slice (ms). */
bool exportWaterfall (const RoomMeasurement& m, const juce::File& file);

/** Analysis options for m (its microphone calibration). */
room::AnalysisOptions analysisOptions (const RoomMeasurement& m);
} // namespace measurements

/** Microphone calibrations: the built-in presets plus files imported into %APPDATA%/HydraMixer/Calibrations. */
namespace calibrations
{
juce::File folder();
/** Presets first, then the imported files (name = file name without extension). */
std::vector<room::Calibration> all();
/** Reads a calibration file and copies it into the folder. Returns the calibration, or an empty one with error set. */
room::Calibration import (const juce::File& file, juce::String& error);
} // namespace calibrations
} // namespace hydra

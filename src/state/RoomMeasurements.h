#pragma once
#include "Common.h"
#include "core/RoomAnalysis.h"
#include "core/Correction.h"

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
    room::ImpulseResponse ir;           // the (first) microphone position
    std::vector<room::ImpulseResponse> positions;   // multi-point measurement: the further positions
    int speakers = 3;                  // what played the sweep: 1 = left speaker, 2 = right, 3 = both
    /** Left and right measured separately (the calibration wizard): this is the left speaker (speakers = 1), right
        the right one. Both are kept in one file and used together. Name, time, microphone, sweep and calibration
        are the same for both. */
    std::shared_ptr<RoomMeasurement> right;
    /** Near-field measurement(s) of this side's speaker(s), microphone 1-2 cm in front of it (both speakers: two). */
    std::vector<room::ImpulseResponse> nearFields;
    static constexpr double kNearSweepStartHz = 150.0;   // the near-field sweep spares the drivers the deep bass
    /** Arrival times for the picture of speakers and microphone positions (kept with the left / only side). */
    room::Geometry geometry;
    int sides() const noexcept { return right != nullptr ? 2 : 1; }
    RoomMeasurement* side (int k) noexcept { return k == 1 && right != nullptr ? right.get() : this; }

    int numPositions() const noexcept { return 1 + (int) positions.size(); }
    /** All positions, for room::analyse. */
    std::vector<const room::ImpulseResponse*> impulses() const
    {
        std::vector<const room::ImpulseResponse*> v { &ir };
        for (auto& p : positions) v.push_back (&p);
        return v;
    }
    juce::File file;                   // library file (empty until saved)

    room::Analysis analysis;           // computed, not stored
    bool analysed = false;
};

namespace measurements
{
inline constexpr const char* kExtension = ".hydrameasure";

/** Library folder (created on demand). */
juce::File folder();
/** All measurements of the library, newest first (not analysed yet). Left / right pairs from separate files (made
    before both were kept in one) are merged into the left one's file. */
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
/** Arrival time of the left minus the right speaker at the listening position (ms; the first position), NaN if m has
    no separate left and right speaker. */
double arrivalDifferenceMs (const RoomMeasurement& m);
/** Time alignment of p from m: the speaker that arrives first is delayed by the difference. */
void alignSpeakers (CorrectionProfile& p, const RoomMeasurement& m);
/** room::analyse of m's positions, plus the near-field response (Analysis::nearDb) if there is one.
    Results are cached on disk (cacheFolder, shared by the mixer and the plugin) under a fingerprint of everything they
    depend on (impulse responses, sweep range, calibration, options), so a measurement is analysed only once: renaming
    keeps the result, another calibration or waterfall length analyses again. A full result also serves a responseOnly
    request. */
room::Analysis analyse (const RoomMeasurement& m, const room::AnalysisOptions&);
/** %APPDATA%/HydraMixer/AnalysisCache (created on demand): the kMaxCached most recently used results. */
juce::File cacheFolder();
inline constexpr int kMaxCached = 64;
/** Tests: a folder of their own instead of the user's cache. */
inline juce::File cacheOverride;
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

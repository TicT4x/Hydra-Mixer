#pragma once
#include "Correction.h"

/** Headphone Correction: makes headphones sound neutral (Harman target) or like other headphones.

    Every headphone is stored as its deviation from the Harman target of the rig it was measured on (dB on a
    1/12-octave grid, 20 Hz .. 19.4 kHz). That makes measurements of different rigs comparable: headphones A shall
    sound like B = EQ (deviation B - deviation A); neutral = EQ (-deviation A).

    Built-in database: resources/headphones.txt (tools/fetch-headphones.py, data of the AutoEq project, MIT license).
    Own headphones (imported measurements) live as .hydrahp files in %APPDATA%/HydraMixer/Headphones.
    Message thread only. */
namespace hydra::hp
{
constexpr int kPoints = 120;
/** Frequency of grid point i (20 Hz * 2^(i/12)). */
inline double gridFreq (int i) noexcept { return 20.0 * std::pow (2.0, i / 12.0); }
using Curve = std::vector<float>;   // kPoints values in dB

struct Entry
{
    juce::String id;      // "db:<name>", "user:<file name>", "target:neutral", "target:<name>"
    juce::String name;    // shown
    juce::String kind;    // "over-ear" / "in-ear" ("" for targets)
    juce::String source;  // who measured it (built-in) or "Imported"
    Curve db;             // deviation from neutral
    bool isUser() const noexcept   { return id.startsWith ("user:"); }
    bool isTarget() const noexcept { return id.startsWith ("target:"); }
};

/** A measurement rig: its Harman target as a raw measurement on it would show neutral headphones. */
struct Rig
{
    juce::String id, name;
    Curve db;
};

class Library
{
public:
    static Library& get();

    /** Built-in headphones, then the user's own (the folder is scanned again at most every 2 s). */
    const std::vector<Entry>& headphones();
    /** Sound targets: Neutral (Harman) first, then the others. */
    const std::vector<Entry>& targets();
    const std::vector<Rig>& rigs();
    /** Any entry (headphone or target) by id, nullptr if unknown. */
    const Entry* find (const juce::String& id);

    static juce::File userFolder();
    /** Imports a frequency response file as own headphones. rigId: what it was measured on ("gras", "hms", "5128",
        "iem"; ignored for files that carry their own target, like AutoEq results). Returns the new id, or empty with
        error. */
    juce::String import (const juce::File& file, const juce::String& name, const juce::String& kind,
                         const juce::String& rigId, juce::String& error);
    /** Deletes own headphones (built-ins cannot be removed). */
    bool remove (const juce::String& id);
    bool rename (const juce::String& id, const juce::String& newName);

    /** Extensions accepted by import. */
    static juce::String filePatterns() { return "*.csv;*.txt;*.frd;*.hydrahp"; }

private:
    Library();
    void scanUser (bool force = false);
    std::vector<Entry> builtIn, user, all, targetList;
    std::vector<Rig> rigList;
    juce::uint32 lastScan = 0;
};

/** Parses a frequency response text: two or more columns (frequency, level ...), separated by commas, semicolons,
    tabs or spaces; comments and headers are skipped. AutoEq CSVs ("frequency,raw,...,target") give their deviation
    directly (hasOwnTarget). REW / squig.link / Sonarworks-style exports work. Returns false with error. */
struct Parsed
{
    std::vector<double> freq, db;
    bool hasOwnTarget = false;   // db already is the deviation from the rig's target
};
bool parseResponse (const juce::String& text, Parsed& out, juce::String& error);
/** Resamples a response onto the grid (mean over each 1/12-octave cell). */
Curve toGrid (const std::vector<double>& freq, const std::vector<double>& db);
/** Moves a curve so its mean between 300 Hz and 3 kHz is 0 dB. */
void normalise (Curve&);

//==============================================================================
struct Settings
{
    int maxBands = 10;
    float maxBoostDb = 8.0f;   // dips are filled up to this much
};

struct Design
{
    CorrectionProfile profile;   // the EQ at full strength (bands + preamp against clipping)
    Curve wanted;                // what the EQ should do (target - headphones, smoothed, limited)
    double rmsBeforeDb = 0.0, rmsAfterDb = 0.0;   // deviation from the target, 30 Hz .. 10 kHz
};
/** Fits low shelf, high shelf and bells (zero latency, the Correction's biquads) that turn 'from' into 'to'. */
Design design (const Curve& from, const Curve& to, const Settings& = {}, double sampleRate = 48000.0);
/** The profile at amount (0..1) of its strength: band gains scaled, preamp recomputed. */
CorrectionProfile scaled (const CorrectionProfile& full, float amount);
/** Level change (dB) of pink noise through the profile, preamp included: < 0 = it plays quieter. */
double loudnessDb (const CorrectionProfile&, double sampleRate = 48000.0);
/** Response of a profile on the grid (preamp left out). */
Curve responseOf (const CorrectionProfile&, double sampleRate = 48000.0);
} // namespace hydra::hp

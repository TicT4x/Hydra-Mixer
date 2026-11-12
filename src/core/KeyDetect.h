#pragma once
#include "Common.h"

namespace hydra::dsp
{
/** Tuning and key of a piece of music (Transpose: Auto Tune / Find Key). Offline, a few ms for 10 s of audio.
    Tuning: every spectral peak (80 Hz - 5 kHz, true frequency from the phase advance between two FFTs) votes
    with its distance to the nearest equal-tempered note of A = 440 Hz; the circular mean of the votes is the
    offset of the whole piece in cents (-50 .. +50). Key: pitch-class profile of the same peaks (in tune after
    the offset) correlated with the Krumhansl-Kessler major / minor profiles. */
struct KeyResult
{
    bool valid = false;
    float cents = 0.0f;         // how far the piece is from A = 440 Hz (+ = sharp)
    float tuningStrength = 0;   // 0..1: how much the peaks agree on the offset
    int key = -1;               // 0..11 major (C, C#, ... B), 12..23 minor, -1 = unknown
    int key2 = -1;              // second choice
    float keyStrength = 0;      // correlation of the best key (0..1)
    float probability = 0, probability2 = 0;   // how likely the two choices are (softmax over all 24 keys)
    std::array<float, 12> chroma {};   // pitch-class profile (C = 0), normalised to max 1
};

/** mono: the signal at sampleRate. */
KeyResult analyseKey (const float* mono, int numSamples, double sampleRate);
/** Key, second choice and their probabilities from res.chroma (e.g. after combining several parts of a song). */
void rankKeys (KeyResult& res);
constexpr double kKeySharpness = 15.0;
/** The 7 pitch classes of a key (0 = C), tonic first. */
std::array<int, 7> scaleOf (int key);

/** "C major", "F# minor" ... ("" for -1). shiftSemitones transposes the name. */
juce::String keyName (int key, int shiftSemitones = 0);
} // namespace hydra::dsp

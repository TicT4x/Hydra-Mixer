#pragma once
#include "Common.h"

namespace hydra
{
/** A clip on a recorder track: plays [offset, offset + length) of the track's file from 'start' on the timeline.
    Everything is non-destructive: the recorded file is never changed by editing. Seconds. */
struct RecClip
{
    double start = 0.0;     // position on the timeline
    double offset = 0.0;    // where in the file the clip begins
    double length = 0.0;
    double fadeIn = 0.0, fadeOut = 0.0;
    float gainDb = 0.0f;

    double end() const noexcept { return start + length; }
    bool operator== (const RecClip&) const = default;
};

/** One recorded input of a take: its file in the take's folder and the clips cut from it. */
struct RecTrackInfo
{
    juce::String name;      // the input's name when it was recorded (also the file name)
    juce::String file;      // file name inside the project folder
    int channels = 2;       // 1 = mono source
    int sourceInput = -1;   // input slot it was recorded from (information only)
    double fileSeconds = 0.0;
    std::vector<RecClip> clips;

    bool operator== (const RecTrackInfo&) const = default;
};

/** The editable part of a recorder project (a multitrack take): tracks with their clips and the loop range.
    The tracks' mixer settings and effect chains are the parameters of Engine::recTracks and are stored next to
    this in the project file (state/RecorderProject). Message thread; the recorder thread gets copies. */
struct RecProjectData
{
    double sampleRate = 48000.0;
    std::vector<RecTrackInfo> tracks;
    double loopStart = 0.0, loopEnd = 0.0;   // loop / export range; loopEnd <= loopStart = the whole project

    /** End of the last clip. */
    double length() const noexcept
    {
        double e = 0.0;
        for (auto& t : tracks)
            for (auto& c : t.clips) e = juce::jmax (e, c.end());
        return e;
    }
    bool hasLoop() const noexcept { return loopEnd > loopStart + 0.01; }
    bool operator== (const RecProjectData&) const = default;

    std::unique_ptr<juce::XmlElement> toXml() const;
    /** Reads what toXml wrote (TRACKS / LOOP children of the given element). */
    void fromXml (const juce::XmlElement&);

    //--------------------------------------------------------------------------
    // Editing (all keep clips of a track sorted and non-overlapping).
    static constexpr double kMinClip = 0.01;
    /** Splits every clip under time t on the given track (-1 = all tracks). Returns true if something was split. */
    bool split (int track, double t);
    /** Removes [from, to) on the given track (-1 = all): clips are shortened / split, nothing moves. */
    bool erase (int track, double from, double to);
    /** Keeps only [from, to) on every track and moves it to the start (crop). */
    bool crop (double from, double to);
    /** Sorts the clips of a track and cuts away overlaps (a later clip wins over what it covers). */
    void tidy (int track);
};
} // namespace hydra

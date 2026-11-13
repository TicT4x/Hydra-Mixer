#pragma once
#include "core/Params.h"
#include "core/RingBuffer.h"
#include "RecProject.h"

namespace hydra
{
/** Peak overview of a recording or a loaded file for the waveform display: one level (max |sample| of
    both channels) per 1/kPeaksPerSecond s. Written by the recorder threads, read by the UI. */
class Waveform
{
public:
    static constexpr int kPeaksPerSecond = 200;

    void clear();
    void append (const float* peaks, int n);
    double getSeconds() const;
    /** Highest level per column for numColumns equal columns covering [startSec, endSec). */
    void getColumns (double startSec, double endSec, float* out, int numColumns) const;
    /** Bumped on every change (repaint the display). */
    uint32_t getVersion() const noexcept { return version.load(); }
    /** Takes over the peaks of another overview (the finished take becomes the loaded file). */
    void copyFrom (const Waveform& other);

private:
    juce::CriticalSection lock;   // never taken by the audio thread
    std::vector<float> peaks;
    std::atomic<uint32_t> version { 1 };
};

/** Recorder + player.

    Recording, two sources (rec.mode):
    - a virtual bus (B1-B8, post fader / limiter) into one stereo WAV file;
    - several inputs (rec.arm.inN). rec.separate: every input into its own WAV file (named after the input) in a
      new folder per take, together with a project file (<take>.hydrarec): the take opens as a multitrack project.
      Otherwise the inputs are summed into one stereo file. rec.tap: what is recorded of an input, Pre effects (after
      its Gain knob, before the effects; the take's tracks get a copy of the inputs' effect chains, so they can still
      be changed, its MONO too) or Post effects (after MONO and the effect chain, before pan / fader).
    The audio thread copies into a wait-free ring; a background thread writes the files.

    Playback: a single file (the background thread decodes and resamples it) or the loaded multitrack project
    (its clips, rendered per track; the engine plays every track through its own strip, Engine::recTracks, and
    mixes them). The result goes to the chosen buses (rec.out.*).

    Transport is driven by parameters (rec.record / rec.play / rec.stop), so it is MIDI-learnable and can
    have keyboard shortcuts. Switching play off pauses (the next play continues there); rec.stop ends
    recording and playback and returns to the start. The audio thread never locks or allocates here. */
class Recorder : private juce::Thread, private juce::Timer
{
public:
    Recorder();
    ~Recorder() override;

    void createParams (ParamRegistry&);
    Param *record {}, *play {}, *stop {}, *loop {}, *source {}, *level {};
    Param *mode {};       // 0 = a V bus, 1 = inputs
    Param *separate {};   // inputs: one file per input (multitrack take) instead of one mixed file
    Param *tap {};        // inputs: 0 = raw (before the effects), 1 = with effects
    std::array<Param*, kMaxBuses> out {};
    std::array<Param*, kMaxInputs> arm {};
    enum Mode { modeBus = 0, modeInputs = 1 };

    // Settings (message thread, stored with the session)
    juce::File folder;          // empty = Documents/Hydra Recordings
    int bitDepth = 24;          // 16, 24 or 32 (= 32-bit float)
    juce::File getFolder() const;

    /** Message thread: what the recorder needs from the engine to record inputs (set by the engine). */
    std::function<juce::String (int input)> inputName;
    std::function<bool (int input)> inputIsMono;   // mono source: the track gets a mono file

    /** Project hooks (set by state/RecorderProject): store / restore the settings of the tracks (mixer, effect
        chains) in / from the project file, reset them for a new take, and a version that changes with them. */
    struct ProjectHooks
    {
        std::function<void (juce::XmlElement& project, int numTracks)> saveTracks;
        std::function<void (const juce::XmlElement* project, int numTracks)> loadTracks;
        std::function<uint64_t()> tracksVersion;
        /** The effect chain of an input as a chain clip (recording "Pre effects": the track gets a copy of it). */
        std::function<std::unique_ptr<juce::XmlElement> (int input)> inputChain;
        /** The input's MONO switch (it comes before the effects; Pre effects records without it, the track gets it). */
        std::function<bool (int input)> inputMono;
    } hooks;

    // Message thread
    bool loadFile (const juce::File& f, juce::String& error);
    /** Deletes the loaded file (moved to the recycle bin if possible) and unloads it. Not while recording. */
    bool deleteLoadedFile (juce::String& error);
    juce::File getLoadedFile() const     { return playFile; }
    juce::File getRecordingFile() const  { return recFile; }
    juce::File getLastRecording() const  { return lastRecording; }
    juce::String getLastError() const    { return lastError; }
    bool isRecording() const noexcept    { return recording.load(); }
    bool isPlaying() const noexcept      { return playing.load(); }
    double getRecordSeconds() const;
    double getPlaySeconds() const;
    double getPlayLengthSeconds() const;
    int getRecordSourceBus() const noexcept;
    bool recordsInputs() const noexcept  { return juce::roundToInt (mode->get()) == modeInputs; }
    /** While a multitrack take is recorded (message thread): its tracks (names of the inputs, input slots). */
    int getTakeTrackCount() const                 { return recording.load() && recSeparate.load() ? (int) take.size() : 0; }
    juce::String getTakeTrackName (int k) const   { return juce::isPositiveAndBelow (k, (int) take.size()) ? take[(size_t) k].name : juce::String(); }
    int getTakeTrackInput (int k) const           { return juce::isPositiveAndBelow (k, (int) take.size()) ? take[(size_t) k].input : -1; }

    /** Moves the playback position (like clicking into a DAW's timeline). While stopped, it sets
        where the next PLAY starts. */
    void seek (double seconds);
    /** Where PLAY starts (set by seek, 0 for a new file). */
    double getStartSeconds() const noexcept { return startSeconds.load(); }

    /** Trim of the loaded file, like the in / out points of a clip in a video editor: playback (and loop)
        only runs between in and out. Nothing is written until applyTrim(). Reset when another file is loaded. */
    void setTrim (double inSeconds, double outSeconds);
    void resetTrim() { setTrim (0.0, getPlayLengthSeconds()); }
    double getTrimIn() const noexcept { return trimIn.load(); }
    double getTrimOut() const;
    bool isTrimmed() const;
    /** Cuts the loaded file to the trim range. A WAV file is rewritten (the original goes to the Recycle
        Bin), other formats are saved as "<name>_trimmed.wav". The result is loaded. Not while recording. */
    bool applyTrim (juce::String& error);
    /** Saves the trim range of the loaded file as a new file next to it ("<name> (trimmed).wav"); the original stays
        loaded and unchanged. Returns the new file. */
    juce::File saveTrimmedCopy (juce::String& error);

    /** Overview of the take being recorded / of the loaded file (also while it is still being scanned). */
    const Waveform& getRecordWaveform() const noexcept { return recWave; }
    const Waveform& getPlayWaveform() const noexcept   { return playWave; }
    /** Overview of a track of the project / of the multitrack take being recorded (file seconds). */
    const Waveform& getTrackWaveform (int t) const noexcept { return trackWaves[(size_t) juce::jlimit (0, kMaxRecTracks - 1, t)]; }
    /** Bumped whenever the shown state (file, recording, playing, error, project) changes. */
    std::atomic<uint32_t> stateVersion { 1 };

    //--------------------------------------------------------------------------
    // Multitrack project (message thread)
    bool hasProject() const noexcept           { return projectFile != juce::File(); }
    juce::File getProjectFile() const          { return projectFile; }
    juce::File getProjectFolder() const        { return projectFile.getParentDirectory(); }
    const RecProjectData& getProject() const   { return project; }
    /** Number of tracks of the loaded project (0 without one). Any thread. */
    int getNumTracks() const noexcept          { return numTracks.load (std::memory_order_acquire); }
    /** Replaces the clips / loop of the project (an edit): playback follows at once, saved shortly after. */
    void setProject (const RecProjectData&);
    /** Opens a project file (.hydrarec), or the project of a take folder. */
    bool openProject (const juce::File&, juce::String& error);
    /** Saves the project file now (also done automatically ~1 s after changes). */
    bool saveProject (juce::String& error);
    void closeProject();
    /** Tracks added / removed / reordered: newData has the new tracks, settingsFrom[k] is the old track whose
        settings (strip, effect chain) new track k takes (-1 = fresh). Playback stops. */
    bool restructureProject (const RecProjectData& newData, const std::vector<int>& settingsFrom, juce::String& error);
    /** Adds an audio file as a new track (copied into the project folder, converted to the project's rate if needed),
        its clip starting at 'at' seconds. */
    bool addTrackFromFile (const juce::File& audio, double at, juce::String& error);
    bool removeTrack (int track, juce::String& error);
    /** The loaded single file becomes a multitrack recording with one track (a project file next to it, keeps the
        trim as the clip), so tracks can be added and it can be edited like a take. */
    bool convertFileToProject (juce::String& error);
    /** Project files of the recording folder (newest first). */
    juce::Array<juce::File> findProjects() const;
    static constexpr const char* kProjectExtension = ".hydrarec";

    //--------------------------------------------------------------------------
    // Export of the project (message thread starts it, a background thread renders). Needs the engine's track
    // processing: Engine::renderTracks, given by the engine.
    struct ExportOptions
    {
        bool stems = true;          // one file per track
        bool mixdown = true;        // the sum of all tracks
        bool withMixer = true;      // track gain, pan, mute, solo, effect chains; false = only the edits
        bool loopOnly = false;      // only the loop range
        int bitDepth = 24;
        juce::File folder;          // empty = <project folder>/Export <date>
    };
    /** Renders block by block: fills l[t] / r[t] (numTracks channels pairs) with the tracks through their strips.
        Called on the export thread while playback is paused for it. Set by the engine. */
    std::function<void (float* const* l, float* const* r, int numTracks, int n, bool withMixer)> renderTracks;
    std::function<void()> resetTracks;            // before an export: fresh effect states
    std::function<int (int track)> trackLatency;  // samples the chain of a track delays (compensated)
    std::function<bool (int track)> trackAudible; // not muted / soloed away (for the mixdown and stems)
    std::function<bool()> needsRealtime;          // an effect only works in real time (Stems): export paced
    bool startExport (const ExportOptions&, juce::String& error);
    void cancelExport()                       { exportCancel.store (true); }
    bool isExporting() const noexcept         { return exporting.load (std::memory_order_acquire); }
    float getExportProgress() const noexcept  { return exportProgress.load(); }
    juce::File getLastExportFolder() const    { return lastExport; }

    //--------------------------------------------------------------------------
    // Audio thread
    void setSampleRate (double sr) noexcept { sampleRate.store (sr); }
    bool wantsRecord() const noexcept       { return recording.load (std::memory_order_acquire) && ! recInputs.load (std::memory_order_relaxed); }
    void pushRecord (const float* l, const float* r, int n) noexcept;
    /** Recording inputs: the engine taps these inputs (raw or with effects). */
    bool wantsInputs() const noexcept      { return recording.load (std::memory_order_acquire) && recInputs.load (std::memory_order_relaxed); }
    bool tapsInput (int i) const noexcept  { return (tapMask.load (std::memory_order_relaxed) >> i) & 1u; }
    bool tapsWithEffects() const noexcept  { return tapFx.load (std::memory_order_relaxed); }
    /** l[i] / r[i]: tap of input i (all kMaxInputs; only the armed ones are read). */
    void pushInputs (const float* const* l, const float* const* r, int n) noexcept;
    /** Fills l/r with n frames of playback. Returns false (and leaves l/r untouched) when idle. */
    bool pullPlayback (float* l, float* r, int n) noexcept;
    /** Project playback: fills l[t] / r[t] of every track. Returns the number of tracks (0 = idle). */
    int pullTracks (float* const* l, float* const* r, int n) noexcept;

private:
    bool startRecording();
    void stopRecording();
    bool startPlayback();
    void stopPlayback();
    void unloadFile();
    void ensureThread();
    /** writerLock held. maxChunks > 0: stop after that many chunks (the lock is let go in between, so the message
        thread never waits long for it); returns true if more is waiting. */
    bool drainRecording (int maxChunks = 0);
    bool drainInputs (int maxChunks = 0);
    bool fillPlayback (int maxChunks = 0);
    void scanFile (const juce::File&);   // starts building playWave in the background
    void scanProject();
    void run() override;
    void timerCallback() override;
    void changed() { stateVersion.fetch_add (1); }
    bool startInputRecording (double sr);
    void finishInputRecording();
    void loadProjectInternal (const juce::File& file, const RecProjectData& data, const juce::XmlElement* settings);
    void restartProjectSource();   // writerLock held: new clips / position
    void exportThread (ExportOptions, juce::File dir);
    std::unique_ptr<juce::AudioFormatWriter> makeWriter (const juce::File&, double rate, int channels, int bits, juce::String& error);
    double projectEnd() const;   // where playback of the project stops (loop end or the end of the last clip)
    double loopStartSec() const;
    double loopEndSec() const;

    juce::AudioFormatManager formats;
    juce::CriticalSection writerLock;   // message thread <-> recorder thread only, never the audio thread

    // recording
    SpscAudioRing recRing;
    std::unique_ptr<juce::AudioFormatWriter> writer;
    juce::File recFile, lastRecording;
    std::atomic<bool> recording { false };
    std::atomic<int> inPush { 0 };   // the audio thread is inside pushRecord / pushInputs
    std::atomic<juce::int64> recFrames { 0 };
    double recRate = 48000.0;
    juce::AudioBuffer<float> recScratch { 2, 4096 };
    Waveform recWave;
    float recPeak = 0.0f;       // current (unfinished) waveform bucket
    int recPeakFrames = 0;

    // recording inputs
    std::atomic<bool> recInputs { false }, tapFx { false }, recSeparate { false };
    std::atomic<uint32_t> tapMask { 0 };
    struct TakeTrack { int input = 0; int channels = 2; int ringChannel = 0; std::unique_ptr<juce::AudioFormatWriter> writer;
                       float peak = 0.0f; int peakFrames = 0; juce::String name; juce::File file;
                       std::shared_ptr<juce::XmlElement> chain;      // Pre effects: the input's chain, for the track
                       bool mono = false; };                          // Pre effects: the input's MONO, for the track
    std::vector<TakeTrack> take;                       // message thread builds it before recording starts
    std::array<int, kMaxInputs> ringChannelOf {};      // audio thread: first ring channel of an input (-1 = not armed)
    std::array<int, kMaxInputs> ringChannelsOf {};
    int takeRingChannels = 0;
    juce::File takeFolder;
    std::vector<float> sumL, sumR;                      // inputs mixed into one file (audio thread)
    std::array<Waveform, kMaxRecTracks> trackWaves;

    // playback
    SpscAudioRing playRing;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;
    std::unique_ptr<juce::ResamplingAudioSource> resampler;
    juce::File playFile;
    double fileRate = 48000.0, playRate = 48000.0;
    juce::int64 fileLength = 0;
    bool producerDone = false;
    std::atomic<bool> playing { false }, flushPending { false }, eof { false }, finished { false };
    std::atomic<juce::int64> playFrames { 0 };     // played since playBase
    std::atomic<double> playBase { 0.0 }, startSeconds { 0.0 };
    juce::AudioBuffer<float> playScratch { 2, 4096 };
    Waveform playWave;
    class Scanner;
    std::unique_ptr<Scanner> scanner;
    class RangeSource;   // plays only the trimmed part of the file (recorder thread)
    std::unique_ptr<RangeSource> range;
    std::atomic<double> trimIn { 0.0 }, trimOut { -1.0 };   // seconds; out < 0 = end of file

    // project
    class ProjectSource;   // renders the clips of all tracks (recorder thread)
    std::unique_ptr<ProjectSource> projectSource;
    juce::File projectFile;
    RecProjectData project;
    std::atomic<int> numTracks { 0 };
    std::atomic<double> projLoopStart { 0.0 }, projLoopEnd { 0.0 }, projLength { 0.0 };
    bool projectDirty = false;
    double dirtySince = 0.0;
    uint64_t savedTracksVersion = 0;

    // export
    std::atomic<bool> exporting { false }, exportCancel { false };
    std::atomic<float> exportProgress { 0.0f };
    std::unique_ptr<juce::Thread> exporter;
    juce::File lastExport;
    juce::String exportError;

    std::atomic<double> sampleRate { 0.0 };
    juce::String lastError;
};
} // namespace hydra

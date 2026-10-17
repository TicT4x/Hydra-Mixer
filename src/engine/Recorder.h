#pragma once
#include "core/Params.h"
#include "core/RingBuffer.h"

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

/** Bus recorder + file player.

    Recording: the audio thread copies the selected virtual bus (B1-B8, post fader / limiter) into a
    wait-free ring; a background thread writes it to a WAV file.
    Playback: the background thread decodes (and resamples) a file into a second ring; the audio
    thread pulls it and the engine mixes it into the chosen buses (A or B).

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
    std::array<Param*, kMaxBuses> out {};

    // Settings (message thread, stored with the session)
    juce::File folder;          // empty = Documents/Hydra Recordings
    int bitDepth = 24;          // 16, 24 or 32 (= 32-bit float)
    juce::File getFolder() const;

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

    /** Overview of the take being recorded / of the loaded file (also while it is still being scanned). */
    const Waveform& getRecordWaveform() const noexcept { return recWave; }
    const Waveform& getPlayWaveform() const noexcept   { return playWave; }
    /** Bumped whenever the shown state (file, recording, playing, error) changes. */
    std::atomic<uint32_t> stateVersion { 1 };

    // Audio thread
    void setSampleRate (double sr) noexcept { sampleRate.store (sr); }
    bool wantsRecord() const noexcept       { return recording.load (std::memory_order_acquire); }
    void pushRecord (const float* l, const float* r, int n) noexcept;
    /** Fills l/r with n frames of playback. Returns false (and leaves l/r untouched) when idle. */
    bool pullPlayback (float* l, float* r, int n) noexcept;

private:
    bool startRecording();
    void stopRecording();
    bool startPlayback();
    void stopPlayback();
    void unloadFile();
    void ensureThread();
    void drainRecording();      // writerLock held
    void fillPlayback();        // writerLock held
    void scanFile (const juce::File&);   // starts building playWave in the background
    void run() override;
    void timerCallback() override;
    void changed() { stateVersion.fetch_add (1); }

    juce::AudioFormatManager formats;
    juce::CriticalSection writerLock;   // message thread <-> recorder thread only, never the audio thread

    // recording
    SpscAudioRing recRing;
    std::unique_ptr<juce::AudioFormatWriter> writer;
    juce::File recFile, lastRecording;
    std::atomic<bool> recording { false };
    std::atomic<juce::int64> recFrames { 0 };
    double recRate = 48000.0;
    juce::AudioBuffer<float> recScratch { 2, 4096 };
    Waveform recWave;
    float recPeak = 0.0f;       // current (unfinished) waveform bucket
    int recPeakFrames = 0;

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

    std::atomic<double> sampleRate { 0.0 };
    juce::String lastError;
};
} // namespace hydra

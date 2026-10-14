#pragma once
#include "core/Params.h"
#include "core/RingBuffer.h"

namespace hydra
{
/** Bus recorder + file player.

    Recording: the audio thread copies the selected virtual bus (B1-B3, post fader / limiter) into a
    wait-free ring; a background thread writes it to a WAV file.
    Playback: the background thread decodes (and resamples) a file into a second ring; the audio
    thread pulls it and the engine mixes it into the chosen buses (A1-A5 / B1-B3).

    Transport is driven by parameters (rec.record / rec.play), so it is MIDI-learnable and can have
    keyboard shortcuts. The audio thread never locks or allocates here. */
class Recorder : private juce::Thread, private juce::Timer
{
public:
    Recorder();
    ~Recorder() override;

    void createParams (ParamRegistry&);
    Param *record {}, *play {}, *loop {}, *source {}, *level {};
    std::array<Param*, kNumBuses> out {};

    // Settings (message thread, stored with the session)
    juce::File folder;          // empty = Documents/Hydra Recordings
    int bitDepth = 24;          // 16, 24 or 32 (= 32-bit float)
    juce::File getFolder() const;

    // Message thread
    bool loadFile (const juce::File& f, juce::String& error);
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
    void ensureThread();
    void drainRecording();      // writerLock held
    void fillPlayback();        // writerLock held
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

    // playback
    SpscAudioRing playRing;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;
    std::unique_ptr<juce::ResamplingAudioSource> resampler;
    juce::File playFile;
    double fileRate = 48000.0, playRate = 48000.0;
    juce::int64 fileLength = 0;
    bool producerDone = false;
    std::atomic<bool> playing { false }, flushPending { false }, eof { false }, finished { false };
    std::atomic<juce::int64> playFrames { 0 };
    juce::AudioBuffer<float> playScratch { 2, 4096 };

    std::atomic<double> sampleRate { 0.0 };
    juce::String lastError;
};
} // namespace hydra

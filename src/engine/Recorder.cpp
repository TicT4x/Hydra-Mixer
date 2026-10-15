#include "Recorder.h"

namespace hydra
{
namespace
{
constexpr int kChunk = 4096;
}

Recorder::Recorder() : juce::Thread ("Hydra Recorder")
{
    formats.registerBasicFormats();
    recRing.allocate (2, 1 << 19);   // ~10 s at 48 kHz: the writer thread may stall that long without loss
    playRing.allocate (2, 1 << 15);  // ~0.7 s read-ahead
    startTimerHz (20);
}

Recorder::~Recorder()
{
    stopTimer();
    if (recording.load()) stopRecording();
    playing.store (false);
    stopThread (2000);
}

void Recorder::createParams (ParamRegistry& params)
{
    juce::NormalisableRange<float> fader (-60.0f, 12.0f, 0.1f);
    fader.setSkewForCentre (-15.0f);

    record = &params.addToggle ("rec.record", "Recorder Record");
    play   = &params.addToggle ("rec.play", "Recorder Play");
    record->persistent = false; // never resume recording / playback on the next start
    play->persistent = false;
    loop   = &params.addToggle ("rec.loop", "Recorder Loop");
    juce::StringArray virtCodes;
    for (int v = 0; v < kMaxVirtBuses; ++v) virtCodes.add (busCode (virtBusIndex (v)));
    source = &params.addChoice ("rec.src", "Recorder Source", virtCodes, 0);
    level  = &params.add ("rec.level", "Recorder Playback Level", ParamType::Continuous, fader, 0.0f, " dB");
    for (int b = 0; b < kMaxBuses; ++b)
        out[(size_t) b] = &params.addToggle ("rec.out." + busCode (b), "Recorder -> " + busCode (b), b == 0);
}

juce::File Recorder::getFolder() const
{
    if (folder != juce::File()) return folder;
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Hydra Recordings");
}

int Recorder::getRecordSourceBus() const noexcept
{
    return virtBusIndex (juce::jlimit (0, kMaxVirtBuses - 1, juce::roundToInt (source->get())));
}

double Recorder::getRecordSeconds() const { return (double) recFrames.load() / juce::jmax (1.0, recRate); }

double Recorder::getPlayLengthSeconds() const { return (double) fileLength / juce::jmax (1.0, fileRate); }

double Recorder::getPlaySeconds() const
{
    const double t = (double) playFrames.load() / juce::jmax (1.0, playRate);
    const double len = getPlayLengthSeconds();
    return len > 0.0 ? std::fmod (t, len) : t;
}

//==============================================================================
void Recorder::ensureThread()
{
    if (! isThreadRunning())
        startThread (juce::Thread::Priority::normal);
}

void Recorder::run()
{
    while (! threadShouldExit())
    {
        {
            const juce::ScopedLock sl (writerLock);
            drainRecording();
            fillPlayback();
        }
        wait (10);
    }
}

void Recorder::timerCallback()
{
    const bool wantRec = record->isOn();
    if (wantRec && ! recording.load())
    {
        if (! startRecording()) record->set (0.0f);
    }
    else if (! wantRec && recording.load())
        stopRecording();

    if (finished.exchange (false))
    {
        playing.store (false);
        play->set (0.0f);
        changed();
    }

    const bool wantPlay = play->isOn();
    if (wantPlay && ! playing.load())
    {
        if (! startPlayback()) play->set (0.0f);
    }
    else if (! wantPlay && playing.load())
        stopPlayback();

    if (! recording.load() && ! playing.load() && isThreadRunning())
        stopThread (1000);
}

//==============================================================================
bool Recorder::startRecording()
{
    const double sr = sampleRate.load();
    if (sr <= 0.0) { lastError = "The audio engine is not running."; changed(); return false; }

    const auto dir = getFolder();
    if (! dir.createDirectory())
    {
        lastError = "Cannot create the folder " + dir.getFullPathName();
        changed();
        return false;
    }

    const auto file = dir.getNonexistentChildFile ("Hydra_" + busCode (getRecordSourceBus()) + "_"
                                                       + juce::Time::getCurrentTime().formatted ("%Y-%m-%d_%H-%M-%S"),
                                                   ".wav", false);
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    if (static_cast<juce::FileOutputStream*> (stream.get())->failedToOpen())
    {
        lastError = "Cannot write " + file.getFullPathName();
        changed();
        return false;
    }

    const bool isFloat = bitDepth >= 32;
    auto options = juce::AudioFormatWriterOptions {}
                       .withSampleRate (sr)
                       .withNumChannels (2)
                       .withBitsPerSample (isFloat ? 32 : juce::jlimit (16, 24, bitDepth))
                       .withSampleFormat (isFloat ? juce::AudioFormatWriterOptions::SampleFormat::floatingPoint
                                                  : juce::AudioFormatWriterOptions::SampleFormat::integral);
    juce::WavAudioFormat wav;
    auto w = wav.createWriterFor (stream, options);
    if (w == nullptr)
    {
        lastError = "Cannot create a WAV writer for " + file.getFullPathName();
        file.deleteFile();
        changed();
        return false;
    }

    {
        const juce::ScopedLock sl (writerLock);
        recRing.skip (recRing.available()); // leftovers from the previous take
        writer = std::move (w);
        recFile = file;
        recRate = sr;
        recFrames.store (0);
        recording.store (true, std::memory_order_release);
    }
    lastError.clear();
    ensureThread();
    changed();
    logLine ("recording " + busCode (getRecordSourceBus()) + " to " + file.getFullPathName());
    return true;
}

void Recorder::stopRecording()
{
    recording.store (false, std::memory_order_release);
    {
        const juce::ScopedLock sl (writerLock);
        drainRecording();
        writer.reset(); // finalises the WAV header
    }
    lastRecording = recFile;
    recFile = juce::File();
    logLine ("recording stopped: " + lastRecording.getFileName() + " (" + juce::String (getRecordSeconds(), 1) + " s)");

    // Ready to listen back with Play right away.
    if (! playing.load())
    {
        juce::String err;
        loadFile (lastRecording, err);
    }
    changed();
}

void Recorder::drainRecording()
{
    if (writer == nullptr) return;
    while (recRing.available() > 0)
    {
        auto* const* ch = recScratch.getArrayOfWritePointers();
        const int got = recRing.read (ch, 2, kChunk);
        if (got <= 0) break;
        writer->writeFromFloatArrays (recScratch.getArrayOfReadPointers(), 2, got);
    }
}

void Recorder::pushRecord (const float* l, const float* r, int n) noexcept
{
    const float* src[2] = { l, r };
    const int written = recRing.write (src, 2, n);
    recFrames.fetch_add (written, std::memory_order_relaxed);
}

//==============================================================================
bool Recorder::loadFile (const juce::File& f, juce::String& error)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (f));
    if (reader == nullptr)
    {
        error = "Cannot read " + f.getFileName() + " (supported: WAV, AIFF, FLAC, Ogg, MP3/WMA)";
        return false;
    }
    if (playing.load()) stopPlayback();

    const juce::ScopedLock sl (writerLock);
    resampler.reset();
    fileRate = reader->sampleRate;
    fileLength = reader->lengthInSamples;
    readerSource = std::make_unique<juce::AudioFormatReaderSource> (reader.release(), true);
    playFile = f;
    playFrames.store (0);
    changed();
    return true;
}

bool Recorder::startPlayback()
{
    const double sr = sampleRate.load();
    if (sr <= 0.0) { lastError = "The audio engine is not running."; changed(); return false; }

    if (readerSource == nullptr)
    {
        juce::String err;
        if (lastRecording == juce::File() || ! loadFile (lastRecording, err))
        {
            lastError = lastRecording == juce::File() ? juce::String ("Nothing to play - record something or open a file.") : err;
            changed();
            return false;
        }
    }

    {
        const juce::ScopedLock sl (writerLock);
        readerSource->setNextReadPosition (0);
        readerSource->setLooping (loop->isOn());
        resampler = std::make_unique<juce::ResamplingAudioSource> (readerSource.get(), false, 2);
        resampler->setResamplingRatio (fileRate / sr);
        resampler->prepareToPlay (kChunk, sr);
        playRate = sr;
        producerDone = false;
        eof.store (false);
        finished.store (false);
        playFrames.store (0);
        flushPending.store (true, std::memory_order_release); // audio thread drops stale frames first
        playing.store (true, std::memory_order_release);
    }
    lastError.clear();
    ensureThread();
    changed();
    return true;
}

void Recorder::stopPlayback()
{
    playing.store (false, std::memory_order_release);
    flushPending.store (true, std::memory_order_release);
    changed();
}

void Recorder::fillPlayback()
{
    if (! playing.load() || flushPending.load (std::memory_order_acquire) || producerDone || resampler == nullptr)
        return;

    const bool looping = loop->isOn();
    readerSource->setLooping (looping);

    while (playRing.freeSpace() >= kChunk)
    {
        juce::AudioSourceChannelInfo info (&playScratch, 0, kChunk);
        resampler->getNextAudioBlock (info);
        playRing.write (playScratch.getArrayOfReadPointers(), 2, kChunk);

        if (! looping && readerSource->getNextReadPosition() >= fileLength)
        {
            producerDone = true;
            eof.store (true, std::memory_order_release);
            break;
        }
    }
}

bool Recorder::pullPlayback (float* l, float* r, int n) noexcept
{
    if (flushPending.load (std::memory_order_acquire))
    {
        playRing.skip (playRing.available());
        flushPending.store (false, std::memory_order_release);
    }
    if (! playing.load (std::memory_order_acquire))
        return false;

    float* dst[2] = { l, r };
    const int got = playRing.read (dst, 2, n);
    if (got < n)
    {
        std::memset (l + got, 0, (size_t) (n - got) * sizeof (float));
        std::memset (r + got, 0, (size_t) (n - got) * sizeof (float));
        if (eof.load (std::memory_order_acquire))
            finished.store (true, std::memory_order_release);
    }
    playFrames.fetch_add (got, std::memory_order_relaxed);
    return true;
}
} // namespace hydra

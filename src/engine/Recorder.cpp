#include "Recorder.h"

namespace hydra
{
namespace
{
constexpr int kChunk = 4096;
}

//==============================================================================
void Waveform::clear()
{
    const juce::ScopedLock sl (lock);
    peaks.clear();
    version.fetch_add (1);
}

void Waveform::append (const float* p, int n)
{
    if (n <= 0) return;
    const juce::ScopedLock sl (lock);
    peaks.insert (peaks.end(), p, p + n);
    version.fetch_add (1);
}

void Waveform::copyFrom (const Waveform& other)
{
    std::vector<float> copy;
    {
        const juce::ScopedLock sl (other.lock);
        copy = other.peaks;
    }
    const juce::ScopedLock sl (lock);
    peaks = std::move (copy);
    version.fetch_add (1);
}

double Waveform::getSeconds() const
{
    const juce::ScopedLock sl (lock);
    return (double) peaks.size() / kPeaksPerSecond;
}

void Waveform::getColumns (double startSec, double endSec, float* out, int numColumns) const
{
    const juce::ScopedLock sl (lock);
    const double first = startSec * kPeaksPerSecond;
    const double per = (endSec - startSec) * kPeaksPerSecond / juce::jmax (1, numColumns);
    for (int c = 0; c < numColumns; ++c)
    {
        const auto a = (size_t) juce::jmax (0.0, first + c * per);
        const auto b = juce::jmin (peaks.size(), (size_t) juce::jmax ((double) a + 1.0, first + (c + 1) * per));
        float m = 0.0f;
        for (size_t i = a; i < b; ++i) m = juce::jmax (m, peaks[i]);
        out[c] = m;
    }
}

//==============================================================================
/** Builds the overview of a loaded file in the background (long files take a moment to decode). */
class Recorder::Scanner final : public juce::Thread
{
public:
    Scanner (juce::File f, Waveform& w) : juce::Thread ("Hydra Waveform"), file (std::move (f)), wave (w)
    {
        startThread (juce::Thread::Priority::low);
    }
    ~Scanner() override { stopThread (4000); }

    void run() override
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
        if (reader == nullptr) return;
        const int bucket = juce::jmax (1, juce::roundToInt (reader->sampleRate / Waveform::kPeaksPerSecond));
        const int channels = (int) juce::jmin (2u, reader->numChannels);
        juce::AudioBuffer<float> buf (channels, bucket * 256);
        std::vector<float> peaks;
        for (juce::int64 pos = 0; pos < reader->lengthInSamples && ! threadShouldExit();)
        {
            const int n = (int) juce::jmin ((juce::int64) buf.getNumSamples(), reader->lengthInSamples - pos);
            reader->read (&buf, 0, n, pos, true, channels > 1);
            peaks.clear();
            for (int start = 0; start < n; start += bucket)
            {
                const int len = juce::jmin (bucket, n - start);
                float m = 0.0f;
                for (int c = 0; c < channels; ++c) m = juce::jmax (m, buf.getMagnitude (c, start, len));
                peaks.push_back (m);
            }
            wave.append (peaks.data(), (int) peaks.size());
            pos += n;
        }
    }

private:
    juce::File file;
    Waveform& wave;
};

/** Plays only [trim in, trim out) of the file and loops inside it. Recorder thread only. */
class Recorder::RangeSource final : public juce::PositionableAudioSource
{
public:
    RangeSource (juce::AudioFormatReaderSource& s, const Recorder& r) : src (s), rec (r) {}

    bool looping = false;
    bool ended = false;   // reached the out point without looping

    void prepareToPlay (int block, double sr) override { src.prepareToPlay (block, sr); }
    void releaseResources() override                   { src.releaseResources(); }

    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
    {
        const auto start = (juce::int64) (rec.getTrimIn() * rec.fileRate);
        const auto end = juce::jmax (start, (juce::int64) (rec.getTrimOut() * rec.fileRate));
        int done = 0;
        while (done < info.numSamples)
        {
            auto pos = src.getNextReadPosition();
            if (pos < start) { src.setNextReadPosition (start); pos = start; }
            if (pos >= end)
            {
                if (looping && end > start) { src.setNextReadPosition (start); continue; }
                info.buffer->clear (info.startSample + done, info.numSamples - done);
                ended = true;
                return;
            }
            const int n = (int) juce::jmin ((juce::int64) (info.numSamples - done), end - pos);
            juce::AudioSourceChannelInfo part (info.buffer, info.startSample + done, n);
            src.getNextAudioBlock (part);
            done += n;
        }
    }

    void setNextReadPosition (juce::int64 p) override { src.setNextReadPosition (p); ended = false; }
    juce::int64 getNextReadPosition() const override  { return src.getNextReadPosition(); }
    juce::int64 getTotalLength() const override       { return src.getTotalLength(); }
    bool isLooping() const override                   { return looping; }
    void setLooping (bool l) override                 { looping = l; }

private:
    juce::AudioFormatReaderSource& src;
    const Recorder& rec;
};

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
    scanner.reset();
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
    stop   = &params.addToggle ("rec.stop", "Recorder Stop");   // momentary: switched off again once handled
    record->persistent = false; // never resume recording / playback on the next start
    play->persistent = false;
    stop->persistent = false;
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
    const double t = playBase.load() + (double) playFrames.load() / juce::jmax (1.0, playRate);
    const double in = getTrimIn(), out = getTrimOut();
    return t >= out && out > in ? in + std::fmod (t - in, out - in) : t;   // looping inside the trim range
}

double Recorder::getTrimOut() const
{
    const double len = getPlayLengthSeconds(), out = trimOut.load();
    return out < 0.0 ? len : juce::jmin (out, len);
}

bool Recorder::isTrimmed() const
{
    return playFile != juce::File() && (getTrimIn() > 0.001 || getTrimOut() < getPlayLengthSeconds() - 0.001);
}

void Recorder::setTrim (double in, double out)
{
    const double len = getPlayLengthSeconds();
    constexpr double minLength = 0.05;
    in = juce::jlimit (0.0, juce::jmax (0.0, len - minLength), in);
    out = juce::jlimit (juce::jmin (len, in + minLength), len, out);
    trimIn.store (in);
    trimOut.store (out >= len - 1.0e-6 ? -1.0 : out);
    // Keep the start marker / play head inside the range.
    const double pos = playing.load() ? getPlaySeconds() : startSeconds.load();
    if (pos < in || pos > out) seek (in);
    changed();
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
    // Stop: ends recording and playback and goes back to the start of the file.
    if (stop->isOn())
    {
        stop->set (0.0f);
        record->set (0.0f);
        play->set (0.0f);
        if (playing.load()) stopPlayback();
        startSeconds.store (getTrimIn());
        changed();
    }

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
        startSeconds.store (getTrimIn()); // played to the end: the next play starts from the beginning (of the trim)
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
        recWave.clear();
        recPeak = 0.0f;
        recPeakFrames = 0;
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
        if (recPeakFrames > 0) recWave.append (&recPeak, 1);
        recPeak = 0.0f;
        recPeakFrames = 0;
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

        // Waveform overview of the take
        const int bucket = juce::jmax (1, juce::roundToInt (recRate / Waveform::kPeaksPerSecond));
        float peaks[kChunk / 32 + 2];
        int numPeaks = 0;
        const float* l = recScratch.getReadPointer (0);
        const float* r = recScratch.getReadPointer (1);
        for (int i = 0; i < got; ++i)
        {
            recPeak = juce::jmax (recPeak, std::abs (l[i]), std::abs (r[i]));
            if (++recPeakFrames >= bucket)
            {
                if (numPeaks < (int) std::size (peaks)) peaks[numPeaks++] = recPeak;
                recPeak = 0.0f;
                recPeakFrames = 0;
            }
        }
        recWave.append (peaks, numPeaks);
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

    {
        const juce::ScopedLock sl (writerLock);
        resampler.reset();
        fileRate = reader->sampleRate;
        fileLength = reader->lengthInSamples;
        range.reset();
        readerSource = std::make_unique<juce::AudioFormatReaderSource> (reader.release(), true);
        range = std::make_unique<RangeSource> (*readerSource, *this);
        trimIn.store (0.0);
        trimOut.store (-1.0);
        playFile = f;
        playFrames.store (0);
        playBase.store (0.0);
        startSeconds.store (0.0);
    }
    scanFile (f);
    changed();
    return true;
}

void Recorder::unloadFile()
{
    play->set (0.0f);
    if (playing.load()) stopPlayback();
    scanner.reset(); // closes its reader of the file
    {
        const juce::ScopedLock sl (writerLock);
        resampler.reset();
        range.reset();
        readerSource.reset();
        playFile = juce::File();
        trimIn.store (0.0);
        trimOut.store (-1.0);
        fileLength = 0;
        playFrames.store (0);
        playBase.store (0.0);
        startSeconds.store (0.0);
    }
    playWave.clear();
}

bool Recorder::deleteLoadedFile (juce::String& error)
{
    const auto f = playFile;
    if (recording.load()) { error = "Stop the recording first."; return false; }
    if (f == juce::File()) { error = "No file loaded."; return false; }

    unloadFile(); // the file must not be open while it is deleted
    if (f == lastRecording)
    {
        lastRecording = juce::File();
        recWave.clear();
    }
    changed();
    if (f.existsAsFile() && ! f.moveToTrash() && ! f.deleteFile())
    {
        error = "Cannot delete " + f.getFullPathName();
        lastError = error;
        return false;
    }
    logLine ("recording deleted: " + f.getFullPathName());
    return true;
}

void Recorder::scanFile (const juce::File& f)
{
    scanner.reset();
    playWave.clear();
    // The take just recorded: its overview exists already.
    if (f == lastRecording && ! recording.load() && recWave.getSeconds() > 0.0)
        playWave.copyFrom (recWave);
    else
        scanner = std::make_unique<Scanner> (f, playWave);
}

void Recorder::seek (double seconds)
{
    seconds = juce::jlimit (getTrimIn(), juce::jmax (getTrimIn(), getTrimOut()), seconds);
    startSeconds.store (seconds);
    if (playing.load())
    {
        const juce::ScopedLock sl (writerLock);
        if (readerSource != nullptr && resampler != nullptr)
        {
            range->setNextReadPosition ((juce::int64) (seconds * fileRate));
            resampler->flushBuffers();
            producerDone = false;
            eof.store (false);
            finished.store (false);
            playBase.store (seconds);
            playFrames.store (0);
            flushPending.store (true, std::memory_order_release); // audio thread drops the old position's audio
        }
        notify();
    }
    changed();
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
        // Start at the position set by clicking into the waveform (from the beginning if that is the end).
        double start = startSeconds.load();
        if (start < getTrimIn() || start >= getTrimOut() - 0.05) start = getTrimIn();
        readerSource->setLooping (false);   // the range source loops inside the trim
        range->setNextReadPosition ((juce::int64) (start * fileRate));
        range->setLooping (loop->isOn());
        resampler = std::make_unique<juce::ResamplingAudioSource> (range.get(), false, 2);
        resampler->setResamplingRatio (fileRate / sr);
        resampler->prepareToPlay (kChunk, sr);
        playRate = sr;
        producerDone = false;
        eof.store (false);
        finished.store (false);
        playBase.store (start);
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
    // Pause: the next play continues where it stopped.
    if (! finished.load()) startSeconds.store (juce::jlimit (getTrimIn(), juce::jmax (getTrimIn(), getTrimOut()), getPlaySeconds()));
    playing.store (false, std::memory_order_release);
    flushPending.store (true, std::memory_order_release);
    changed();
}

void Recorder::fillPlayback()
{
    if (! playing.load() || flushPending.load (std::memory_order_acquire) || producerDone || resampler == nullptr)
        return;

    range->setLooping (loop->isOn());

    while (playRing.freeSpace() >= kChunk)
    {
        juce::AudioSourceChannelInfo info (&playScratch, 0, kChunk);
        resampler->getNextAudioBlock (info);
        playRing.write (playScratch.getArrayOfReadPointers(), 2, kChunk);

        if (range->ended)
        {
            producerDone = true;
            eof.store (true, std::memory_order_release);
            break;
        }
    }
}

bool Recorder::applyTrim (juce::String& error)
{
    const auto src = playFile;
    if (recording.load())    { error = "Stop the recording first."; return false; }
    if (src == juce::File()) { error = "No file loaded."; return false; }
    if (! isTrimmed())       { error = "Drag the markers at the start / end of the waveform first."; return false; }

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (src));
    if (reader == nullptr) { error = "Cannot read " + src.getFileName(); return false; }
    const auto from = (juce::int64) (getTrimIn() * reader->sampleRate);
    const auto to = juce::jmin (reader->lengthInSamples, (juce::int64) (getTrimOut() * reader->sampleRate));
    const int channels = (int) juce::jmax (1u, reader->numChannels);

    // Write the kept part to a new file next to the original.
    const auto tmp = src.getParentDirectory().getNonexistentChildFile (src.getFileNameWithoutExtension() + "_trim", ".tmp", false);
    {
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (tmp);
        if (static_cast<juce::FileOutputStream*> (stream.get())->failedToOpen()) { error = "Cannot write " + tmp.getFullPathName(); return false; }
        const bool isFloat = reader->usesFloatingPointData;
        auto options = juce::AudioFormatWriterOptions {}
                           .withSampleRate (reader->sampleRate)
                           .withNumChannels (channels)
                           .withBitsPerSample (isFloat ? 32 : juce::jlimit (16, 24, (int) reader->bitsPerSample))
                           .withSampleFormat (isFloat ? juce::AudioFormatWriterOptions::SampleFormat::floatingPoint
                                                      : juce::AudioFormatWriterOptions::SampleFormat::integral);
        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor (stream, options);
        if (writer == nullptr) { tmp.deleteFile(); error = "Cannot create a WAV writer."; return false; }
        juce::AudioBuffer<float> buf (channels, 65536);
        for (auto pos = from; pos < to;)
        {
            const int n = (int) juce::jmin ((juce::int64) buf.getNumSamples(), to - pos);
            reader->read (&buf, 0, n, pos, true, channels > 1);
            if (! writer->writeFromAudioSampleBuffer (buf, 0, n))
            {
                writer.reset();
                tmp.deleteFile();
                error = "Writing failed (disk full?).";
                return false;
            }
            pos += n;
        }
    }
    reader.reset();

    // The file must not be open while it is replaced.
    unloadFile();
    juce::File result;
    if (src.hasFileExtension ("wav"))
    {
        if (! src.moveToTrash() && ! src.deleteFile())
        {
            error = "Cannot replace " + src.getFullPathName() + " - the trimmed version is " + tmp.getFileName();
            juce::String ignored;
            loadFile (src, ignored);
            return false;
        }
        result = src;
    }
    else
        result = src.getParentDirectory().getNonexistentChildFile (src.getFileNameWithoutExtension() + "_trimmed", ".wav", false);
    if (! tmp.moveFileTo (result)) { error = "Cannot rename " + tmp.getFullPathName(); return false; }

    if (src == lastRecording)
    {
        lastRecording = result;
        recWave.clear();   // the overview is read from the new file
    }
    logLine ("recording trimmed: " + result.getFullPathName());
    juce::String err;
    loadFile (result, err);
    changed();
    return true;
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

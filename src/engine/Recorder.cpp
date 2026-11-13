#include "Recorder.h"
#include <set>

namespace hydra
{
namespace
{
constexpr int kChunk = 4096;

/** A name usable as a file name ("Mic / Voice" -> "Mic - Voice"). */
juce::String safeFileName (juce::String s)
{
    s = juce::File::createLegalFileName (s.replaceCharacters ("/\\:", "---")).trim();
    return s.isEmpty() ? juce::String ("Track") : s;
}

juce::String timestamp() { return juce::Time::getCurrentTime().formatted ("%Y-%m-%d_%H-%M-%S"); }
} // namespace

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
        const double a0 = first + c * per;
        if (a0 < 0.0 || a0 >= (double) peaks.size()) { out[c] = 0.0f; continue; }
        const auto a = (size_t) a0;
        const auto b = juce::jmin (peaks.size(), (size_t) juce::jmax ((double) a + 1.0, first + (c + 1) * per));
        float m = 0.0f;
        for (size_t i = a; i < b; ++i) m = juce::jmax (m, peaks[i]);
        out[c] = m;
    }
}

//==============================================================================
/** Builds the overview of one or more files in the background (long files take a moment to decode). */
class Recorder::Scanner final : public juce::Thread
{
public:
    Scanner (std::vector<std::pair<juce::File, Waveform*>> j) : juce::Thread ("Hydra Waveform"), jobs (std::move (j))
    {
        startThread (juce::Thread::Priority::low);
    }
    ~Scanner() override { stopThread (4000); }

    void run() override
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        for (auto& [file, wave] : jobs)
        {
            if (threadShouldExit()) return;
            wave->clear();
            std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
            if (reader == nullptr) continue;
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
                wave->append (peaks.data(), (int) peaks.size());
                pos += n;
            }
        }
    }

private:
    std::vector<std::pair<juce::File, Waveform*>> jobs;
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

//==============================================================================
/** Renders the clips of every track of a project at the project's rate: channels 2t / 2t+1 = track t (a mono
    track has the same signal on both). Loops over the loop range (or the whole project), or plays to 'end'.
    Recorder thread / export thread only. */
class Recorder::ProjectSource final : public juce::AudioSource
{
public:
    ProjectSource (const RecProjectData& d, const juce::File& dir, juce::AudioFormatManager& fm) : data (d)
    {
        for (auto& t : data.tracks)
            readers.emplace_back (fm.createReaderFor (dir.getChildFile (t.file)));
        tmp.setSize (2, kChunk);
    }

    RecProjectData data;
    std::vector<std::unique_ptr<juce::AudioFormatReader>> readers;
    juce::int64 pos = 0;         // project frames
    bool looping = false;
    bool ended = false;
    juce::int64 endOverride = -1;   // export: stop here instead of the end of the project

    int numTracks() const noexcept { return (int) data.tracks.size(); }
    juce::int64 frames (double s) const noexcept { return (juce::int64) std::llround (s * data.sampleRate); }

    /** Keeps readers (the files stay the same while editing): only the clips / loop change. */
    void setData (const RecProjectData& d)
    {
        const auto keepTracks = d.tracks.size() == data.tracks.size();
        jassert (keepTracks);
        if (keepTracks) data = d;
    }

    void prepareToPlay (int, double) override {}
    void releaseResources() override {}

    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
    {
        const double len = data.length();
        const juce::int64 loopA = data.hasLoop() ? frames (data.loopStart) : 0;
        const juce::int64 loopB = data.hasLoop() ? frames (data.loopEnd) : frames (len);
        int done = 0;
        while (done < info.numSamples)
        {
            const juce::int64 end = endOverride >= 0 ? endOverride : looping ? loopB : frames (len);
            if (pos >= end)
            {
                if (looping && endOverride < 0 && loopB > loopA + 32) { pos = loopA; continue; }
                info.buffer->clear (info.startSample + done, info.numSamples - done);
                ended = true;
                return;
            }
            const int n = (int) juce::jmin ((juce::int64) (info.numSamples - done), end - pos, (juce::int64) kChunk);
            render (*info.buffer, info.startSample + done, n);
            pos += n;
            done += n;
        }
    }

private:
    void render (juce::AudioBuffer<float>& out, int at, int n)
    {
        const double sr = data.sampleRate;
        for (int t = 0; t < numTracks() && 2 * t + 1 < out.getNumChannels(); ++t)
        {
            float* L = out.getWritePointer (2 * t, at);
            float* R = out.getWritePointer (2 * t + 1, at);
            juce::FloatVectorOperations::clear (L, n);
            juce::FloatVectorOperations::clear (R, n);
            auto* reader = readers[(size_t) t].get();
            if (reader == nullptr) continue;
            for (auto& c : data.tracks[(size_t) t].clips)
            {
                const auto cs = frames (c.start), ce = frames (c.end());
                const auto a = juce::jmax (pos, cs), b = juce::jmin (pos + n, ce);
                if (b <= a) continue;
                const int len = (int) (b - a);
                const auto fileStart = frames (c.offset) + (a - cs);
                reader->read (&tmp, 0, len, fileStart, true, true);   // a mono file fills both channels
                const float g = juce::Decibels::decibelsToGain (c.gainDb);
                const double fi = c.fadeIn * sr, fo = c.fadeOut * sr, clipLen = (double) (ce - cs);
                const float* sL = tmp.getReadPointer (0);
                const float* sR = tmp.getReadPointer (reader->numChannels > 1 ? 1 : 0);
                float* dL = L + (a - pos);
                float* dR = R + (a - pos);
                for (int i = 0; i < len; ++i)
                {
                    const double x = (double) (a - cs + i);   // frames into the clip
                    double k = g;
                    if (x < fi) k *= x / fi;
                    if (x > clipLen - fo) k *= juce::jmax (0.0, (clipLen - x) / fo);
                    dL[i] += sL[i] * (float) k;
                    dR[i] += sR[i] * (float) k;
                }
            }
        }
    }

    juce::AudioBuffer<float> tmp;
};

//==============================================================================
Recorder::Recorder() : juce::Thread ("Hydra Recorder")
{
    formats.registerBasicFormats();
    recRing.allocate (2, 1 << 19);   // ~10 s at 48 kHz: the writer thread may stall that long without loss
    playRing.allocate (2, 1 << 15);  // ~0.7 s read-ahead
    sumL.assign (kMaxBlock, 0.0f);
    sumR.assign (kMaxBlock, 0.0f);
    ringChannelOf.fill (-1);
    startTimerHz (20);
}

Recorder::~Recorder()
{
    stopTimer();
    if (exporter != nullptr) { exportCancel.store (true); exporter->stopThread (10000); exporter.reset(); }
    scanner.reset();
    if (recording.load()) stopRecording();
    if (hasProject()) { juce::String ignored; saveProject (ignored); }
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
    for (int v = 0; v < kMaxVirtBuses; ++v) virtCodes.add (busLabel (virtBusIndex (v)));
    source = &params.addChoice ("rec.src", "Recorder Source", virtCodes, 0);
    level  = &params.add ("rec.level", "Recorder Playback Level", ParamType::Continuous, fader, 0.0f, " dB");
    for (int b = 0; b < kMaxBuses; ++b)
        out[(size_t) b] = &params.addToggle ("rec.out." + busCode (b), "Recorder -> " + busLabel (b), b == 0);
    mode     = &params.addChoice ("rec.mode", "Recorder Records", { "V bus", "Inputs" }, modeBus);
    separate = &params.addToggle ("rec.separate", "Recorder Separate Tracks", true);
    tap      = &params.addChoice ("rec.tap", "Recorder Input Tap", { "Pre effects", "Post effects" }, 0);
    for (int i = 0; i < kMaxInputs; ++i)
        arm[(size_t) i] = &params.addToggle ("rec.arm.in" + juce::String (i + 1), "Recorder Arm In " + juce::String (i + 1));
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

double Recorder::getPlayLengthSeconds() const
{
    if (hasProject()) return projLength.load();
    return (double) fileLength / juce::jmax (1.0, fileRate);
}

double Recorder::loopStartSec() const { return hasProject() ? projLoopStart.load() : getTrimIn(); }
double Recorder::loopEndSec() const
{
    if (! hasProject()) return getTrimOut();
    const double e = projLoopEnd.load();
    return e > projLoopStart.load() + 0.01 ? e : projLength.load();
}
double Recorder::projectEnd() const { return loop->isOn() ? loopEndSec() : projLength.load(); }

double Recorder::getPlaySeconds() const
{
    const double t = playBase.load() + (double) playFrames.load() / juce::jmax (1.0, playRate);
    const double in = loopStartSec(), out = loopEndSec();
    if (hasProject() && ! loop->isOn()) return t;
    return t >= out && out > in ? in + std::fmod (t - in, out - in) : t;   // looping inside the trim / loop range
}

double Recorder::getTrimOut() const
{
    const double len = (double) fileLength / juce::jmax (1.0, fileRate), out = trimOut.load();
    return out < 0.0 ? len : juce::jmin (out, len);
}

bool Recorder::isTrimmed() const
{
    const double len = (double) fileLength / juce::jmax (1.0, fileRate);
    return playFile != juce::File() && (getTrimIn() > 0.001 || getTrimOut() < len - 0.001);
}

void Recorder::setTrim (double in, double out)
{
    if (hasProject()) return;
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
        // A few chunks at a time: the message thread (seek, edits, Stop) takes the same lock and must not wait for
        // a long write (a slow disk / OneDrive folder) or a long read-ahead.
        bool more = false;
        {
            const juce::ScopedLock sl (writerLock);
            more |= recInputs.load() ? drainInputs (4) : drainRecording (4);
            more |= fillPlayback (4);
        }
        if (! more) wait (10);
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
        startSeconds.store (hasProject() ? (loop->isOn() ? loopStartSec() : 0.0) : getTrimIn());
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
        // played to the end: the next play starts from the beginning (of the trim)
        startSeconds.store (hasProject() ? 0.0 : getTrimIn());
        changed();
    }

    const bool wantPlay = play->isOn();
    if (wantPlay && ! playing.load() && ! exporting.load())
    {
        if (! startPlayback()) play->set (0.0f);
    }
    else if (! wantPlay && playing.load())
        stopPlayback();
    else if (wantPlay && exporting.load())
        play->set (0.0f);

    if (! exporting.load() && exporter != nullptr)
    {
        exporter.reset();   // finished: joins the thread
        if (exportError.isNotEmpty()) { lastError = exportError; exportError.clear(); }
        changed();
    }

    // Project autosave ~1 s after the last change (clips, loop, track settings).
    if (hasProject())
    {
        const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
        if (hooks.tracksVersion != nullptr)
            if (const auto v = hooks.tracksVersion(); v != savedTracksVersion && ! projectDirty)
            {
                projectDirty = true;
                dirtySince = now;
            }
        if (projectDirty && now - dirtySince > 1.0 && ! recording.load())
        {
            juce::String err;
            saveProject (err);
        }
    }

    if (! recording.load() && ! playing.load() && isThreadRunning())
        stopThread (1000);
}

//==============================================================================
std::unique_ptr<juce::AudioFormatWriter> Recorder::makeWriter (const juce::File& file, double rate, int channels, int bits,
                                                               juce::String& error)
{
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    if (static_cast<juce::FileOutputStream*> (stream.get())->failedToOpen())
    {
        error = "Cannot write " + file.getFullPathName();
        return nullptr;
    }
    const bool isFloat = bits >= 32;
    auto options = juce::AudioFormatWriterOptions {}
                       .withSampleRate (rate)
                       .withNumChannels (channels)
                       .withBitsPerSample (isFloat ? 32 : juce::jlimit (16, 24, bits))
                       .withSampleFormat (isFloat ? juce::AudioFormatWriterOptions::SampleFormat::floatingPoint
                                                  : juce::AudioFormatWriterOptions::SampleFormat::integral);
    juce::WavAudioFormat wav;
    auto w = wav.createWriterFor (stream, options);
    if (w == nullptr)
    {
        error = "Cannot create a WAV writer for " + file.getFullPathName();
        file.deleteFile();
    }
    return w;
}

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
    if (recordsInputs()) return startInputRecording (sr);

    const auto file = dir.getNonexistentChildFile ("Hydra_" + busLabel (getRecordSourceBus()) + "_" + timestamp(), ".wav", false);
    juce::String err;
    auto w = makeWriter (file, sr, 2, bitDepth, err);
    if (w == nullptr) { lastError = err; changed(); return false; }

    {
        const juce::ScopedLock sl (writerLock);
        if (recRing.getNumChannels() != 2 || recRing.getCapacity() < (1 << 19)) recRing.allocate (2, 1 << 19);
        recRing.skip (recRing.available()); // leftovers from the previous take
        writer = std::move (w);
        recFile = file;
        recRate = sr;
        recFrames.store (0);
        recWave.clear();
        recPeak = 0.0f;
        recPeakFrames = 0;
        recInputs.store (false);
        recording.store (true, std::memory_order_release);
    }
    lastError.clear();
    ensureThread();
    changed();
    logLine ("recording " + busLabel (getRecordSourceBus()) + " to " + file.getFullPathName());
    return true;
}

bool Recorder::startInputRecording (double sr)
{
    std::vector<int> armed;
    for (int i = 0; i < kMaxInputs; ++i)
        if (arm[(size_t) i]->isOn()) armed.push_back (i);
    if (armed.empty())
    {
        lastError = "Arm at least one input to record (the R buttons in the recorder).";
        changed();
        return false;
    }

    const bool sep = separate->isOn();
    const auto dir = getFolder();
    std::vector<TakeTrack> tracks;
    juce::File folderOfTake;
    juce::String err;
    if (sep)
    {
        folderOfTake = dir.getNonexistentChildFile ("Hydra " + timestamp(), {}, false);
        if (! folderOfTake.createDirectory()) { lastError = "Cannot create the folder " + folderOfTake.getFullPathName(); changed(); return false; }
        int ringCh = 0;
        for (int i : armed)
        {
            TakeTrack t;
            t.input = i;
            t.name = inputName != nullptr ? inputName (i) : "Input " + juce::String (i + 1);
            t.channels = inputIsMono != nullptr && inputIsMono (i) ? 1 : 2;
            t.ringChannel = ringCh;
            ringCh += t.channels;
            t.file = folderOfTake.getNonexistentChildFile (safeFileName (t.name), ".wav", false);
            // Pre effects: the track gets a copy of the input's effect chain as it is now (editable afterwards).
            if (juce::roundToInt (tap->get()) == 0)
            {
                if (hooks.inputChain != nullptr) t.chain = std::shared_ptr<juce::XmlElement> (hooks.inputChain (i).release());
                t.mono = hooks.inputMono != nullptr && hooks.inputMono (i);
            }
            t.writer = makeWriter (t.file, sr, t.channels, bitDepth, err);
            if (t.writer == nullptr) break;
            tracks.push_back (std::move (t));
        }
        if (tracks.size() != armed.size())
        {
            tracks.clear();
            folderOfTake.deleteRecursively();
            lastError = err;
            changed();
            return false;
        }
    }
    else
    {
        const auto file = dir.getNonexistentChildFile ("Hydra_Inputs_" + timestamp(), ".wav", false);
        auto w = makeWriter (file, sr, 2, bitDepth, err);
        if (w == nullptr) { lastError = err; changed(); return false; }
        const juce::ScopedLock sl (writerLock);
        writer = std::move (w);
        recFile = file;
    }

    {
        const juce::ScopedLock sl (writerLock);
        // The audio thread uses the ring and the layout only while 'recording' is set (it is not now).
        ringChannelOf.fill (-1);
        ringChannelsOf.fill (0);
        uint32_t mask = 0;
        int ringChannels = 2;
        if (sep)
        {
            ringChannels = 0;
            for (auto& t : tracks)
            {
                ringChannelOf[(size_t) t.input] = t.ringChannel;
                ringChannelsOf[(size_t) t.input] = t.channels;
                ringChannels += t.channels;
            }
        }
        for (int i : armed) mask |= 1u << i;
        takeRingChannels = ringChannels;
        if (recRing.getNumChannels() != ringChannels) recRing.allocate (ringChannels, 1 << 18);   // ~5 s per channel at 48 kHz
        recRing.skip (recRing.available());
        recScratch.setSize (ringChannels, kChunk);
        take = std::move (tracks);
        takeFolder = folderOfTake;
        if (sep) recFile = folderOfTake;
        recRate = sr;
        recFrames.store (0);
        recWave.clear();
        for (auto& w : trackWaves) w.clear();
        recPeak = 0.0f;
        recPeakFrames = 0;
        tapMask.store (mask);
        tapFx.store (juce::roundToInt (tap->get()) == 1);
        recSeparate.store (sep);
        recInputs.store (true);
        recording.store (true, std::memory_order_release);
    }
    lastError.clear();
    ensureThread();
    changed();
    logLine ("recording " + juce::String ((int) armed.size()) + " input(s) " + (sep ? "as separate tracks to " : "mixed to ")
             + recFile.getFullPathName());
    return true;
}

void Recorder::stopRecording()
{
    recording.store (false, std::memory_order_seq_cst);
    while (inPush.load (std::memory_order_seq_cst) != 0) juce::Thread::yield();   // the audio thread left pushRecord
    if (recInputs.load() && recSeparate.load())
    {
        finishInputRecording();
        return;
    }
    {
        const juce::ScopedLock sl (writerLock);
        if (recInputs.load()) drainInputs();
        else                  drainRecording();
        writer.reset(); // finalises the WAV header
        if (recPeakFrames > 0) recWave.append (&recPeak, 1);
        recPeak = 0.0f;
        recPeakFrames = 0;
        recInputs.store (false);
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

void Recorder::finishInputRecording()
{
    RecProjectData data;
    juce::XmlElement settings ("HYDRARECORDING");   // the tracks' settings for the new project (Pre effects: the chains)
    {
        const juce::ScopedLock sl (writerLock);
        drainInputs();
        data.sampleRate = recRate;
        const double seconds = (double) recFrames.load() / juce::jmax (1.0, recRate);
        for (auto& t : take)
        {
            if (t.chain != nullptr || t.mono)
            {
                auto* x = settings.createNewChildElement ("TRACKSETTINGS");
                x->setAttribute ("index", (int) data.tracks.size());
                if (t.chain != nullptr) x->addChildElement (new juce::XmlElement (*t.chain));
                if (t.mono)
                {
                    auto* px = x->createNewChildElement ("P");
                    px->setAttribute ("id", "mono");
                    px->setAttribute ("v", 1.0);
                }
            }
            t.writer.reset();
            if (t.peakFrames > 0) trackWaves[data.tracks.size()].append (&t.peak, 1);
            RecTrackInfo info;
            info.name = t.name;
            info.file = t.file.getFileName();
            info.channels = t.channels;
            info.sourceInput = t.input;
            info.fileSeconds = seconds;
            if (seconds >= RecProjectData::kMinClip) info.clips.push_back ({ 0.0, 0.0, seconds });
            data.tracks.push_back (std::move (info));
        }
        take.clear();
        recInputs.store (false);
        recSeparate.store (false);
        tapMask.store (0);
        recRing.allocate (2, 1 << 19);   // back to the size for bus recording (frees the multitrack ring)
        recScratch.setSize (2, kChunk);
    }
    const auto folderOfTake = takeFolder;
    recFile = juce::File();
    logLine ("recording stopped: " + folderOfTake.getFileName() + " (" + juce::String (getRecordSeconds(), 1) + " s, "
             + juce::String ((int) data.tracks.size()) + " tracks)");

    // The take becomes the loaded project (fresh track settings), its file is written right away.
    if (playing.load()) stopPlayback();
    const auto file = folderOfTake.getChildFile (folderOfTake.getFileName() + kProjectExtension);
    // The waveforms recorded live stay (loadProjectInternal would rescan them).
    std::array<std::unique_ptr<Waveform>, kMaxRecTracks> saved;
    for (size_t t = 0; t < data.tracks.size(); ++t) { saved[t] = std::make_unique<Waveform>(); saved[t]->copyFrom (trackWaves[t]); }
    loadProjectInternal (file, data, settings.getNumChildElements() > 0 ? &settings : nullptr);
    scanner.reset();
    for (size_t t = 0; t < data.tracks.size(); ++t) trackWaves[t].copyFrom (*saved[t]);
    lastRecording = file;
    juce::String err;
    if (! saveProject (err)) lastError = err;
    changed();
}

bool Recorder::drainRecording (int maxChunks)
{
    if (writer == nullptr) return false;
    for (int chunks = 0; recRing.available() > 0; ++chunks)
    {
        if (maxChunks > 0 && chunks >= maxChunks) return true;
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
    return false;
}

bool Recorder::drainInputs (int maxChunks)
{
    if (! recSeparate.load()) return drainRecording (maxChunks);   // mixed into one file: like a bus
    if (take.empty()) return false;
    const int C = takeRingChannels;
    const int bucket = juce::jmax (1, juce::roundToInt (recRate / Waveform::kPeaksPerSecond));
    for (int chunks = 0; recRing.available() > 0; ++chunks)
    {
        if (maxChunks > 0 && chunks >= maxChunks) return true;
        const int got = recRing.read (recScratch.getArrayOfWritePointers(), C, kChunk);
        if (got <= 0) break;
        for (size_t k = 0; k < take.size(); ++k)
        {
            auto& t = take[k];
            const float* chans[2] = { recScratch.getReadPointer (t.ringChannel),
                                      recScratch.getReadPointer (t.ringChannel + (t.channels > 1 ? 1 : 0)) };
            if (t.writer != nullptr) t.writer->writeFromFloatArrays (chans, t.channels, got);
            float peaks[kChunk / 32 + 2];
            int numPeaks = 0;
            for (int i = 0; i < got; ++i)
            {
                t.peak = juce::jmax (t.peak, std::abs (chans[0][i]), std::abs (chans[1][i]));
                if (++t.peakFrames >= bucket)
                {
                    if (numPeaks < (int) std::size (peaks)) peaks[numPeaks++] = t.peak;
                    t.peak = 0.0f;
                    t.peakFrames = 0;
                }
            }
            if (k < (size_t) kMaxRecTracks) trackWaves[k].append (peaks, numPeaks);
        }
    }
    return false;
}

void Recorder::pushRecord (const float* l, const float* r, int n) noexcept
{
    inPush.fetch_add (1, std::memory_order_seq_cst);
    if (recording.load (std::memory_order_seq_cst))
    {
        const float* src[2] = { l, r };
        const int written = recRing.write (src, 2, n);
        recFrames.fetch_add (written, std::memory_order_relaxed);
    }
    inPush.fetch_sub (1, std::memory_order_release);
}

void Recorder::pushInputs (const float* const* l, const float* const* r, int n) noexcept
{
    inPush.fetch_add (1, std::memory_order_seq_cst);
    if (recording.load (std::memory_order_seq_cst) && recInputs.load (std::memory_order_relaxed))
    {
        const uint32_t mask = tapMask.load (std::memory_order_relaxed);
        if (recSeparate.load (std::memory_order_relaxed))
        {
            const float* src[kMaxStreamChannels] {};
            for (int i = 0; i < kMaxInputs; ++i)
            {
                const int rc = ringChannelOf[(size_t) i];
                if (rc < 0 || ! ((mask >> i) & 1u)) continue;
                src[rc] = l[i];
                if (ringChannelsOf[(size_t) i] > 1) src[rc + 1] = r[i];
            }
            const int written = recRing.write (src, takeRingChannels, n);
            recFrames.fetch_add (written, std::memory_order_relaxed);
        }
        else
        {
            juce::FloatVectorOperations::clear (sumL.data(), n);
            juce::FloatVectorOperations::clear (sumR.data(), n);
            for (int i = 0; i < kMaxInputs; ++i)
            {
                if (! ((mask >> i) & 1u)) continue;
                juce::FloatVectorOperations::add (sumL.data(), l[i], n);
                juce::FloatVectorOperations::add (sumR.data(), r[i], n);
            }
            const float* src[2] = { sumL.data(), sumR.data() };
            const int written = recRing.write (src, 2, n);
            recFrames.fetch_add (written, std::memory_order_relaxed);
        }
    }
    inPush.fetch_sub (1, std::memory_order_release);
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
    if (hasProject()) closeProject();

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
        scanner = std::make_unique<Scanner> (std::vector<std::pair<juce::File, Waveform*>> { { f, &playWave } });
}

void Recorder::scanProject()
{
    scanner.reset();
    std::vector<std::pair<juce::File, Waveform*>> jobs;
    for (size_t t = 0; t < project.tracks.size() && t < (size_t) kMaxRecTracks; ++t)
        jobs.push_back ({ getProjectFolder().getChildFile (project.tracks[t].file), &trackWaves[t] });
    for (size_t t = project.tracks.size(); t < (size_t) kMaxRecTracks; ++t) trackWaves[t].clear();
    scanner = std::make_unique<Scanner> (std::move (jobs));
}

void Recorder::seek (double seconds)
{
    if (hasProject())
        seconds = juce::jlimit (0.0, juce::jmax (0.0, projLength.load()), seconds);
    else
        seconds = juce::jlimit (getTrimIn(), juce::jmax (getTrimIn(), getTrimOut()), seconds);
    startSeconds.store (seconds);
    if (playing.load())
    {
        const juce::ScopedLock sl (writerLock);
        if (resampler != nullptr && (projectSource != nullptr || readerSource != nullptr))
        {
            if (projectSource != nullptr)
            {
                projectSource->pos = projectSource->frames (seconds);
                projectSource->ended = false;
            }
            else
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

    if (hasProject())
    {
        const juce::ScopedLock sl (writerLock);
        if (projectSource == nullptr) { lastError = "The project cannot be played."; changed(); return false; }
        double start = startSeconds.load();
        if (start >= projectEnd() - 0.05) start = loop->isOn() ? loopStartSec() : 0.0;
        const int C = 2 * projectSource->numTracks();
        if (playRing.getNumChannels() != C) playRing.allocate (C, 1 << 15);
        playScratch.setSize (C, kChunk);
        projectSource->pos = projectSource->frames (start);
        projectSource->ended = false;
        projectSource->looping = loop->isOn();
        resampler = std::make_unique<juce::ResamplingAudioSource> (projectSource.get(), false, C);
        resampler->setResamplingRatio (project.sampleRate / sr);
        resampler->prepareToPlay (kChunk, sr);
        playRate = sr;
        producerDone = false;
        eof.store (false);
        finished.store (false);
        playBase.store (start);
        playFrames.store (0);
        flushPending.store (true, std::memory_order_release);
        playing.store (true, std::memory_order_release);
        lastError.clear();
        ensureThread();
        changed();
        return true;
    }

    if (readerSource == nullptr)
    {
        juce::String err;
        if (lastRecording == juce::File() || ! loadFile (lastRecording, err))
        {
            lastError = lastRecording == juce::File() ? juce::String ("Nothing to play - record something or open a file.") : err;
            changed();
            return false;
        }
        if (hasProject()) return startPlayback();   // the last recording was a multitrack take
    }

    {
        const juce::ScopedLock sl (writerLock);
        if (playRing.getNumChannels() != 2) playRing.allocate (2, 1 << 15);
        playScratch.setSize (2, kChunk);
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
    if (! finished.load())
    {
        if (hasProject()) startSeconds.store (juce::jlimit (0.0, juce::jmax (0.0, projLength.load()), getPlaySeconds()));
        else              startSeconds.store (juce::jlimit (getTrimIn(), juce::jmax (getTrimIn(), getTrimOut()), getPlaySeconds()));
    }
    playing.store (false, std::memory_order_release);
    flushPending.store (true, std::memory_order_release);
    changed();
}

bool Recorder::fillPlayback (int maxChunks)
{
    if (! playing.load() || flushPending.load (std::memory_order_acquire) || producerDone || resampler == nullptr)
        return false;

    const int C = playRing.getNumChannels();
    if (projectSource != nullptr) projectSource->looping = loop->isOn();
    else if (range != nullptr)    range->setLooping (loop->isOn());
    else return false;

    for (int chunks = 0; playRing.freeSpace() >= kChunk; ++chunks)
    {
        if (maxChunks > 0 && chunks >= maxChunks) return true;
        juce::AudioSourceChannelInfo info (&playScratch, 0, kChunk);
        resampler->getNextAudioBlock (info);
        playRing.write (playScratch.getArrayOfReadPointers(), C, kChunk);

        if (projectSource != nullptr ? projectSource->ended : range->ended)
        {
            producerDone = true;
            eof.store (true, std::memory_order_release);
            break;
        }
    }
    return false;
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
        auto writer = makeWriter (tmp, reader->sampleRate, channels,
                                  reader->usesFloatingPointData ? 32 : (int) reader->bitsPerSample, error);
        if (writer == nullptr) return false;
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

juce::File Recorder::saveTrimmedCopy (juce::String& error)
{
    const auto src = playFile;
    if (src == juce::File()) { error = "No file loaded."; return {}; }
    if (! isTrimmed())       { error = "Drag the markers at the start / end of the waveform first."; return {}; }
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (src));
    if (reader == nullptr) { error = "Cannot read " + src.getFileName(); return {}; }
    const auto from = (juce::int64) (getTrimIn() * reader->sampleRate);
    const auto to = juce::jmin (reader->lengthInSamples, (juce::int64) (getTrimOut() * reader->sampleRate));
    const int channels = (int) juce::jmax (1u, reader->numChannels);
    const auto dst = src.getParentDirectory().getNonexistentChildFile (src.getFileNameWithoutExtension() + " (trimmed)", ".wav", false);
    {
        auto writer = makeWriter (dst, reader->sampleRate, channels,
                                  reader->usesFloatingPointData ? 32 : juce::jmax (16, (int) reader->bitsPerSample), error);
        if (writer == nullptr) return {};
        juce::AudioBuffer<float> buf (channels, 65536);
        for (auto pos = from; pos < to;)
        {
            const int n = (int) juce::jmin ((juce::int64) buf.getNumSamples(), to - pos);
            reader->read (&buf, 0, n, pos, true, channels > 1);
            if (! writer->writeFromAudioSampleBuffer (buf, 0, n))
            {
                writer.reset();
                dst.deleteFile();
                error = "Writing failed (disk full?).";
                return {};
            }
            pos += n;
        }
    }
    logLine ("trimmed copy saved: " + dst.getFullPathName());
    return dst;
}

bool Recorder::pullPlayback (float* l, float* r, int n) noexcept
{
    if (numTracks.load (std::memory_order_acquire) > 0) return false;   // a project is loaded: pullTracks plays
    if (flushPending.load (std::memory_order_acquire))
    {
        playRing.skip (playRing.available());
        flushPending.store (false, std::memory_order_release);
    }
    if (! playing.load (std::memory_order_acquire) || playRing.getNumChannels() != 2)
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

int Recorder::pullTracks (float* const* l, float* const* r, int n) noexcept
{
    const int T = numTracks.load (std::memory_order_acquire);
    if (T <= 0 || exporting.load (std::memory_order_acquire)) return 0;
    if (flushPending.load (std::memory_order_acquire))
    {
        playRing.skip (playRing.available());
        flushPending.store (false, std::memory_order_release);
    }
    if (! playing.load (std::memory_order_acquire) || playRing.getNumChannels() != 2 * T)
        return 0;
    float* dst[2 * kMaxRecTracks];
    for (int t = 0; t < T; ++t) { dst[2 * t] = l[t]; dst[2 * t + 1] = r[t]; }
    const int got = playRing.read (dst, 2 * T, n);
    if (got < n)
    {
        for (int c = 0; c < 2 * T; ++c) std::memset (dst[c] + got, 0, (size_t) (n - got) * sizeof (float));
        if (eof.load (std::memory_order_acquire))
            finished.store (true, std::memory_order_release);
    }
    playFrames.fetch_add (got, std::memory_order_relaxed);
    return T;
}

//==============================================================================
// Project
void Recorder::loadProjectInternal (const juce::File& file, const RecProjectData& data, const juce::XmlElement* settings)
{
    if (playing.load()) stopPlayback();
    play->set (0.0f);
    if (playFile != juce::File()) unloadFile();
    {
        const juce::ScopedLock sl (writerLock);
        resampler.reset();
        numTracks.store (0, std::memory_order_release);   // the audio thread stops processing the tracks
    }
    // Let a running block finish with the old tracks before their settings change (the audio thread only reads
    // the parameters; values switching mid-block are harmless, a short wait keeps it clean).
    juce::Thread::sleep (5);
    {
        const juce::ScopedLock sl (writerLock);
        project = data;
        projectFile = file;
        projectSource = std::make_unique<ProjectSource> (project, file.getParentDirectory(), formats);
        projLength.store (project.length());
        projLoopStart.store (project.loopStart);
        projLoopEnd.store (project.loopEnd);
        playFrames.store (0);
        playBase.store (0.0);
        startSeconds.store (0.0);
    }
    if (hooks.loadTracks != nullptr) hooks.loadTracks (settings, (int) project.tracks.size());
    savedTracksVersion = hooks.tracksVersion != nullptr ? hooks.tracksVersion() : 0;
    projectDirty = false;
    numTracks.store ((int) project.tracks.size(), std::memory_order_release);
    scanProject();
    changed();
}

void Recorder::setProject (const RecProjectData& data)
{
    if (! hasProject() || exporting.load() || data.tracks.size() != project.tracks.size()) return;
    if (data == project) return;
    {
        const juce::ScopedLock sl (writerLock);
        project = data;
        projLength.store (project.length());
        projLoopStart.store (project.loopStart);
        projLoopEnd.store (project.loopEnd);
        if (projectSource != nullptr) projectSource->setData (project);
    }
    if (playing.load()) seek (getPlaySeconds());   // what is already read ahead is dropped: the edit is heard at once
    projectDirty = true;
    dirtySince = juce::Time::getMillisecondCounterHiRes() * 0.001;
    changed();
}

bool Recorder::openProject (const juce::File& f, juce::String& error)
{
    auto file = f;
    if (file.isDirectory())
    {
        auto found = file.findChildFiles (juce::File::findFiles, false, juce::String ("*") + kProjectExtension);
        if (found.isEmpty()) { error = "No recorder project in " + file.getFullPathName(); return false; }
        file = found.getFirst();
    }
    auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr || ! xml->hasTagName ("HYDRARECORDING"))
    {
        error = "Not a Hydra recorder project: " + file.getFileName();
        return false;
    }
    if (recording.load()) { error = "Stop the recording first."; return false; }
    if (hasProject() && file != projectFile) { juce::String ignored; saveProject (ignored); }
    RecProjectData data;
    if (auto* d = xml->getChildByName ("PROJECTDATA")) data.fromXml (*d);
    for (auto& t : data.tracks)
        if (! file.getParentDirectory().getChildFile (t.file).existsAsFile())
            logLine ("recorder project " + file.getFileName() + ": missing " + t.file);
    loadProjectInternal (file, data, xml.get());
    lastRecording = file;
    return true;
}

bool Recorder::saveProject (juce::String& error)
{
    if (! hasProject()) return true;
    juce::XmlElement root ("HYDRARECORDING");
    root.setAttribute ("version", 1);
    root.setAttribute ("app", juce::JUCEApplicationBase::getInstance() != nullptr
                                  ? juce::JUCEApplicationBase::getInstance()->getApplicationVersion() : juce::String());
    root.addChildElement (project.toXml().release());
    if (hooks.saveTracks != nullptr) hooks.saveTracks (root, (int) project.tracks.size());
    projectDirty = false;
    savedTracksVersion = hooks.tracksVersion != nullptr ? hooks.tracksVersion() : 0;
    // Written next to the old one, then swapped (a crash never leaves a half file).
    const auto tmp = projectFile.getSiblingFile (projectFile.getFileName() + ".tmp");
    if (! root.writeTo (tmp) || ! tmp.moveFileTo (projectFile))
    {
        tmp.deleteFile();
        error = "Cannot save " + projectFile.getFullPathName();
        return false;
    }
    return true;
}

void Recorder::closeProject()
{
    if (! hasProject()) return;
    juce::String ignored;
    saveProject (ignored);
    if (playing.load()) stopPlayback();
    play->set (0.0f);
    scanner.reset();
    {
        const juce::ScopedLock sl (writerLock);
        numTracks.store (0, std::memory_order_release);
        resampler.reset();
        projectSource.reset();
        projectFile = juce::File();
        project = {};
        projLength.store (0.0);
        playRing.allocate (2, 1 << 15);   // frees the multitrack read-ahead
        playScratch.setSize (2, kChunk);
    }
    for (auto& w : trackWaves) w.clear();
    if (hooks.loadTracks != nullptr) hooks.loadTracks (nullptr, 0);   // track settings back to defaults
    changed();
}

bool Recorder::restructureProject (const RecProjectData& newData, const std::vector<int>& settingsFrom, juce::String& error)
{
    if (! hasProject())                          { error = "No multitrack recording loaded."; return false; }
    if (recording.load() || exporting.load())    { error = "Not while recording or exporting."; return false; }
    if (newData.tracks.empty())                  { error = "A recording needs at least one track."; return false; }
    if ((int) newData.tracks.size() > kMaxRecTracks) { error = "At most " + juce::String (kMaxRecTracks) + " tracks."; return false; }
    // The settings of the tracks (strip values, effect chains) follow them to their new places.
    juce::XmlElement old ("HYDRARECORDING");
    if (hooks.saveTracks != nullptr) hooks.saveTracks (old, (int) project.tracks.size());
    juce::XmlElement settings ("HYDRARECORDING");
    for (int k = 0; k < (int) newData.tracks.size(); ++k)
    {
        const int from = k < (int) settingsFrom.size() ? settingsFrom[(size_t) k] : -1;
        if (from < 0) continue;
        for (auto* x : old.getChildWithTagNameIterator ("TRACKSETTINGS"))
            if (x->getIntAttribute ("index", -1) == from)
            {
                auto* copy = new juce::XmlElement (*x);
                copy->setAttribute ("index", k);
                settings.addChildElement (copy);
                break;
            }
    }
    const auto file = projectFile;
    loadProjectInternal (file, newData, &settings);
    return saveProject (error);
}

bool Recorder::addTrackFromFile (const juce::File& audio, double at, juce::String& error)
{
    if (! hasProject()) { error = "No multitrack recording loaded."; return false; }
    if ((int) project.tracks.size() >= kMaxRecTracks) { error = "At most " + juce::String (kMaxRecTracks) + " tracks."; return false; }
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (audio));
    if (reader == nullptr || reader->lengthInSamples <= 0)
    {
        error = "Cannot read " + audio.getFileName() + " (supported: WAV, AIFF, FLAC, Ogg, MP3/WMA)";
        return false;
    }
    const int channels = (int) juce::jlimit (1u, 2u, reader->numChannels);
    const double rate = project.sampleRate, fileRate = reader->sampleRate;
    const auto dst = getProjectFolder().getNonexistentChildFile (safeFileName (audio.getFileNameWithoutExtension()), ".wav", false);
    double seconds = (double) reader->lengthInSamples / fileRate;
    // The track's file belongs to the recording (copied into its folder), at the project's rate (the tracks are
    // read sample by sample at that rate).
    if (std::abs (fileRate - rate) < 0.5 && audio.hasFileExtension ("wav") && reader->numChannels <= 2)
    {
        reader.reset();
        if (! audio.copyFileTo (dst)) { error = "Cannot copy " + audio.getFileName() + " into " + getProjectFolder().getFullPathName(); return false; }
    }
    else
    {
        auto writer = makeWriter (dst, rate, channels, 32, error);
        if (writer == nullptr) return false;
        const auto total = (juce::int64) std::ceil ((double) reader->lengthInSamples * rate / fileRate);
        juce::AudioFormatReaderSource src (reader.release(), true);
        juce::ResamplingAudioSource resample (&src, false, channels);
        resample.setResamplingRatio (fileRate / rate);
        resample.prepareToPlay (kChunk, rate);
        juce::AudioBuffer<float> buf (channels, kChunk);
        for (juce::int64 done = 0; done < total;)
        {
            const int n = (int) juce::jmin ((juce::int64) kChunk, total - done);
            juce::AudioSourceChannelInfo info (&buf, 0, n);
            resample.getNextAudioBlock (info);
            if (! writer->writeFromAudioSampleBuffer (buf, 0, n))
            {
                writer.reset();
                dst.deleteFile();
                error = "Writing failed (disk full?).";
                return false;
            }
            done += n;
        }
        seconds = (double) total / rate;
    }
    auto data = project;
    RecTrackInfo info;
    info.name = audio.getFileNameWithoutExtension();
    info.file = dst.getFileName();
    info.channels = channels;
    info.fileSeconds = seconds;
    info.clips.push_back ({ juce::jmax (0.0, at), 0.0, seconds });
    data.tracks.push_back (std::move (info));
    std::vector<int> from;
    for (int t = 0; t < (int) project.tracks.size(); ++t) from.push_back (t);
    from.push_back (-1);
    logLine ("recorder: added track " + dst.getFileName() + " to " + projectFile.getFileName());
    return restructureProject (data, from, error);
}

bool Recorder::removeTrack (int track, juce::String& error)
{
    if (! hasProject() || ! juce::isPositiveAndBelow (track, (int) project.tracks.size())) { error = "No such track."; return false; }
    if (project.tracks.size() == 1) { error = "A recording needs at least one track (close it instead)."; return false; }
    auto data = project;
    data.tracks.erase (data.tracks.begin() + track);
    std::vector<int> from;
    for (int t = 0; t < (int) project.tracks.size(); ++t) if (t != track) from.push_back (t);
    // The file stays in the take folder (nothing recorded is ever deleted by editing).
    return restructureProject (data, from, error);
}

bool Recorder::convertFileToProject (juce::String& error)
{
    if (hasProject()) return true;
    if (recording.load()) { error = "Stop the recording first."; return false; }
    const auto f = playFile;
    if (f == juce::File()) { error = "No file loaded."; return false; }
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (f));
    if (reader == nullptr) { error = "Cannot read " + f.getFileName(); return false; }
    RecProjectData data;
    data.sampleRate = reader->sampleRate;
    RecTrackInfo info;
    info.name = f.getFileNameWithoutExtension();
    info.file = f.getFileName();
    info.channels = (int) juce::jlimit (1u, 2u, reader->numChannels);
    info.fileSeconds = (double) reader->lengthInSamples / reader->sampleRate;
    const double in = getTrimIn(), out = getTrimOut();
    info.clips.push_back ({ 0.0, in, juce::jmax (RecProjectData::kMinClip, out - in) });
    data.tracks.push_back (std::move (info));
    reader.reset();
    // The project file sits next to the audio file (which stays as it is).
    const auto file = f.getParentDirectory().getNonexistentChildFile (f.getFileNameWithoutExtension(), kProjectExtension, false);
    loadProjectInternal (file, data, nullptr);
    lastRecording = file;
    logLine ("recorder: " + f.getFileName() + " opened as multitrack recording " + file.getFileName());
    return saveProject (error);
}

juce::Array<juce::File> Recorder::findProjects() const
{
    juce::Array<juce::File> out;
    const auto dir = getFolder();
    for (auto& d : dir.findChildFiles (juce::File::findDirectories, false))
        for (auto& f : d.findChildFiles (juce::File::findFiles, false, juce::String ("*") + kProjectExtension))
            out.add (f);
    for (auto& f : dir.findChildFiles (juce::File::findFiles, false, juce::String ("*") + kProjectExtension)) out.add (f);
    std::sort (out.begin(), out.end(), [] (const juce::File& a, const juce::File& b)
               { return a.getLastModificationTime() > b.getLastModificationTime(); });
    return out;
}

//==============================================================================
// Export
bool Recorder::startExport (const ExportOptions& o, juce::String& error)
{
    if (! hasProject())       { error = "No multitrack recording loaded."; return false; }
    if (exporting.load())     { error = "An export is already running."; return false; }
    if (recording.load())     { error = "Stop the recording first."; return false; }
    if (! o.stems && ! o.mixdown) { error = "Choose stems, a mixdown or both."; return false; }
    if (o.withMixer && (renderTracks == nullptr || sampleRate.load() <= 0.0))
    {
        error = "The audio engine must run to export with the track settings and effects.";
        return false;
    }
    auto dir = o.folder;
    if (dir == juce::File())
        dir = getProjectFolder().getNonexistentChildFile ("Export " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M"), {}, false);
    if (! dir.createDirectory()) { error = "Cannot create " + dir.getFullPathName(); return false; }

    play->set (0.0f);
    if (playing.load()) stopPlayback();
    { juce::String ignored; saveProject (ignored); }
    exportCancel.store (false);
    exportProgress.store (0.0f);
    exportError.clear();
    exporting.store (true, std::memory_order_seq_cst);   // the audio thread leaves the tracks alone from now on

    struct Runner final : juce::Thread
    {
        Runner (Recorder& r, ExportOptions op, juce::File d) : juce::Thread ("Hydra Export"), rec (r), opt (std::move (op)), out (std::move (d)) {}
        void run() override { rec.exportThread (opt, out); }
        Recorder& rec;
        ExportOptions opt;
        juce::File out;
    };
    exporter = std::make_unique<Runner> (*this, o, dir);
    exporter->startThread (juce::Thread::Priority::normal);
    lastExport = dir;
    changed();
    return true;
}

void Recorder::exportThread (ExportOptions o, juce::File dir)
{
    const RecProjectData data = project;   // the message thread does not edit meanwhile (the window is locked)
    const int T = (int) data.tracks.size();
    const double engineRate = o.withMixer ? sampleRate.load() : data.sampleRate;
    const double from = o.loopOnly && data.hasLoop() ? data.loopStart : 0.0;
    const double to = o.loopOnly && data.hasLoop() ? data.loopEnd : data.length();
    const auto total = (juce::int64) std::ceil ((to - from) * engineRate);

    ProjectSource src (data, getProjectFolder(), formats);
    src.pos = src.frames (from);
    src.endOverride = src.frames (to);
    const int C = 2 * T;
    juce::ResamplingAudioSource resample (&src, false, juce::jmax (2, C));
    resample.setResamplingRatio (data.sampleRate / engineRate);
    constexpr int kBlock = 1024;
    resample.prepareToPlay (kBlock, engineRate);

    if (o.withMixer && resetTracks != nullptr) resetTracks();
    std::vector<int> lat ((size_t) T, 0);
    std::vector<bool> audible ((size_t) T, true);
    int maxLat = 0;
    for (int t = 0; t < T; ++t)
    {
        if (o.withMixer && trackLatency != nullptr) lat[(size_t) t] = juce::jmax (0, trackLatency (t));
        if (o.withMixer && trackAudible != nullptr) audible[(size_t) t] = trackAudible (t);
        maxLat = juce::jmax (maxLat, lat[(size_t) t]);
    }
    const bool realtime = o.withMixer && needsRealtime != nullptr && needsRealtime();

    // Writers: stems named after the tracks, the mixdown after the project.
    juce::String err;
    std::vector<std::unique_ptr<juce::AudioFormatWriter>> stems ((size_t) T);
    std::vector<juce::File> files;
    std::set<juce::String> used;
    if (o.stems)
        for (int t = 0; t < T; ++t)
        {
            if (! audible[(size_t) t]) continue;
            auto name = safeFileName (data.tracks[(size_t) t].name);
            for (int k = 2; used.count (name.toLowerCase()) > 0; ++k) name = safeFileName (data.tracks[(size_t) t].name) + " " + juce::String (k);
            used.insert (name.toLowerCase());
            const auto f = dir.getChildFile (name + ".wav");
            const int ch = o.withMixer ? 2 : data.tracks[(size_t) t].channels;
            stems[(size_t) t] = makeWriter (f, engineRate, ch, o.bitDepth, err);
            if (stems[(size_t) t] == nullptr) break;
            files.push_back (f);
        }
    std::unique_ptr<juce::AudioFormatWriter> mix;
    if (o.mixdown && err.isEmpty())
    {
        const auto f = dir.getChildFile (safeFileName (getProjectFile().getFileNameWithoutExtension()) + " - Mix.wav");
        mix = makeWriter (f, engineRate, 2, o.bitDepth, err);
        files.push_back (f);
    }

    // Every track is delayed to the longest chain latency, then that much is skipped at the start (aligned stems
    // and mixdown, nothing missing at the end).
    juce::AudioBuffer<float> buf (juce::jmax (2, C), kBlock);
    std::vector<juce::AudioBuffer<float>> delayBuf ((size_t) T);
    std::vector<int> delayPos ((size_t) T, 0);
    for (int t = 0; t < T; ++t) if (maxLat - lat[(size_t) t] > 0) delayBuf[(size_t) t].setSize (2, maxLat - lat[(size_t) t], false, true);
    juce::AudioBuffer<float> mixBuf (2, kBlock);
    std::vector<float*> lp ((size_t) juce::jmax (1, T)), rp ((size_t) juce::jmax (1, T));

    juce::int64 rendered = 0, written = 0;
    const auto start = juce::Time::getMillisecondCounterHiRes();
    while (err.isEmpty() && written < total && ! exportCancel.load())
    {
        const int n = kBlock;
        juce::AudioSourceChannelInfo info (&buf, 0, n);
        if (rendered < total) resample.getNextAudioBlock (info);
        else buf.clear();   // tail: lets the effects' latency come out
        rendered += n;
        for (int t = 0; t < T; ++t) { lp[(size_t) t] = buf.getWritePointer (2 * t); rp[(size_t) t] = buf.getWritePointer (2 * t + 1); }
        if (o.withMixer) renderTracks (lp.data(), rp.data(), T, n, true);

        for (int t = 0; t < T; ++t)
        {
            auto& d = delayBuf[(size_t) t];
            if (d.getNumSamples() == 0) continue;
            for (int c = 0; c < 2; ++c)
            {
                float* x = c == 0 ? lp[(size_t) t] : rp[(size_t) t];
                float* line = d.getWritePointer (c);
                int p = delayPos[(size_t) t];
                for (int i = 0; i < n; ++i)
                {
                    std::swap (x[i], line[p]);
                    if (++p >= d.getNumSamples()) p = 0;
                }
            }
            delayPos[(size_t) t] = (delayPos[(size_t) t] + n) % d.getNumSamples();
        }
        // Skip the shared latency at the start, stop at the length.
        const auto skip = (int) juce::jlimit ((juce::int64) 0, (juce::int64) n, (juce::int64) maxLat - (rendered - n));
        const int keep = (int) juce::jmin ((juce::int64) (n - skip), total - written);
        if (keep > 0)
        {
            mixBuf.clear();
            for (int t = 0; t < T; ++t)
            {
                if (! audible[(size_t) t]) continue;
                const float* ch[2] = { lp[(size_t) t] + skip, rp[(size_t) t] + skip };
                if (auto& w = stems[(size_t) t]; w != nullptr && ! w->writeFromFloatArrays (ch, (int) w->getNumChannels(), keep))
                    err = "Writing failed (disk full?).";
                mixBuf.addFrom (0, 0, ch[0], keep);
                mixBuf.addFrom (1, 0, ch[1], keep);
            }
            if (mix != nullptr && ! mix->writeFromAudioSampleBuffer (mixBuf, 0, keep)) err = "Writing failed (disk full?).";
            written += keep;
        }
        exportProgress.store ((float) ((double) written / (double) juce::jmax ((juce::int64) 1, total)));
        if (realtime)   // Stems needs its GPU worker: as fast as real time, not faster
        {
            const double due = start + 1000.0 * (double) rendered / engineRate;
            const double wait = due - juce::Time::getMillisecondCounterHiRes();
            if (wait > 1.0) juce::Thread::sleep ((int) wait);
        }
    }
    for (auto& w : stems) w.reset();
    mix.reset();
    if (o.withMixer && resetTracks != nullptr) resetTracks();
    const bool cancelled = exportCancel.load();
    if (cancelled || err.isNotEmpty())
    {
        for (auto& f : files) f.deleteFile();
        if (dir.getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0) dir.deleteFile();
        exportError = cancelled ? juce::String() : err;
        logLine ("recorder export " + juce::String (cancelled ? "cancelled" : "failed: " + err));
    }
    else
        logLine ("recorder export: " + juce::String ((int) files.size()) + " file(s) to " + dir.getFullPathName());
    exportProgress.store (1.0f);
    exporting.store (false, std::memory_order_release);
}
} // namespace hydra

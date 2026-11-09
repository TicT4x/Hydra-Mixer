#include "RoomMeasurements.h"
#include "core/MicCalibrations.h"

namespace hydra::measurements
{
namespace
{
constexpr int kFormatVersion = 1;

juce::String encode (const std::vector<float>& v)
{
    juce::MemoryOutputStream out;
    for (float x : v) out.writeFloat (x);   // little endian
    return out.getMemoryBlock().toBase64Encoding();
}

std::vector<float> decode (const juce::String& s)
{
    juce::MemoryBlock mb;
    if (! mb.fromBase64Encoding (s)) return {};
    juce::MemoryInputStream in (mb, false);
    std::vector<float> v ((size_t) (mb.getSize() / sizeof (float)));
    for (auto& x : v) x = in.readFloat();
    return v;
}

juce::var irToVar (const room::ImpulseResponse& r)
{
    auto* ir = new juce::DynamicObject();
    ir->setProperty ("sampleRate", r.sampleRate);
    ir->setProperty ("peak", r.peak);
    ir->setProperty ("latencyMs", r.latencyMs);
    ir->setProperty ("noiseFloorDb", r.noiseFloorDb);
    ir->setProperty ("capturePeakDb", r.capturePeakDb);
    ir->setProperty ("irSnrDb", r.irSnrDb);
    ir->setProperty ("clipped", r.clipped);
    ir->setProperty ("samples", encode (r.ir));
    return juce::var (ir);
}

bool irFromVar (const juce::var& ir, double fallbackRate, room::ImpulseResponse& r)
{
    if (! ir.isObject()) return false;
    r.sampleRate = (double) ir.getProperty ("sampleRate", fallbackRate);
    r.peak = (int) ir["peak"];
    r.latencyMs = (double) ir["latencyMs"];
    r.noiseFloorDb = (double) ir["noiseFloorDb"];
    r.capturePeakDb = (double) ir["capturePeakDb"];
    r.irSnrDb = (double) ir["irSnrDb"];
    r.clipped = (bool) ir["clipped"];
    r.ir = decode (ir["samples"].toString());
    if (r.ir.empty() || r.sampleRate < 8000.0) return false;
    r.peak = juce::jlimit (0, (int) r.ir.size() - 1, r.peak);
    return true;
}

/** The fields that differ between the left and the right speaker of one measurement. */
void sideToVar (juce::DynamicObject& o, const RoomMeasurement& m)
{
    o.setProperty ("output", m.output);
    o.setProperty ("speakers", m.speakers);
    o.setProperty ("impulseResponse", irToVar (m.ir));
    if (! m.positions.empty())
    {
        juce::Array<juce::var> more;
        for (auto& p : m.positions) more.add (irToVar (p));
        o.setProperty ("morePositions", more);
    }
    if (! m.nearFields.empty())
    {
        juce::Array<juce::var> near;
        for (auto& p : m.nearFields) near.add (irToVar (p));
        o.setProperty ("nearField", near);
    }
}

juce::var timesToVar (const double* t, size_t n)
{
    juce::Array<juce::var> a;
    for (size_t i = 0; i < n; ++i) a.add (t[i]);
    return a;
}

std::vector<double> timesFromVar (const juce::var& v)
{
    std::vector<double> t;
    if (auto* a = v.getArray())
        for (auto& x : *a) t.push_back ((double) x);
    return t;
}

bool sideFromVar (const juce::var& v, RoomMeasurement& m)
{
    m.output = v["output"].toString();
    if (! irFromVar (v["impulseResponse"], m.spec.sampleRate, m.ir)) return false;
    if (auto* more = v["morePositions"].getArray())
        for (auto& p : *more)
        {
            room::ImpulseResponse r;
            if (irFromVar (p, m.spec.sampleRate, r)) m.positions.push_back (std::move (r));
        }
    if (auto* near = v["nearField"].getArray())
        for (auto& p : *near)
        {
            room::ImpulseResponse r;
            if (irFromVar (p, m.spec.sampleRate, r)) m.nearFields.push_back (std::move (r));
        }
    // Older files: which speaker played is only in the output's name.
    m.speakers = v.hasProperty ("speakers") ? juce::jlimit (1, 3, (int) v["speakers"])
               : m.output.endsWith (" (L)") ? 1 : m.output.endsWith (" (R)") ? 2 : 3;
    return true;
}

juce::String withoutSide (const juce::String& s) { return s.replace (" (L)", "").replace (" (R)", ""); }

/** Left and right of one wizard run saved as two files (before 30.10.): the right one goes into the left one's file. */
void mergePairs (std::vector<std::unique_ptr<RoomMeasurement>>& list)
{
    for (auto& left : list)
    {
        if (left == nullptr || left->speakers != 1 || left->right != nullptr) continue;
        std::unique_ptr<RoomMeasurement>* best = nullptr;
        double bestGap = 0.0;
        for (auto& x : list)
        {
            if (x == nullptr || x->speakers != 2 || x->right != nullptr || x->throughEq != left->throughEq
                || withoutSide (x->output) != withoutSide (left->output))
                continue;
            const double gap = std::abs ((x->created - left->created).inSeconds());
            if (gap <= 600.0 && (best == nullptr || gap < bestGap)) { best = &x; bestGap = gap; }
        }
        if (best == nullptr) continue;
        const auto rightFile = (*best)->file;
        left->right = std::shared_ptr<RoomMeasurement> (std::move (*best));
        left->right->file = juce::File();
        left->output = withoutSide (left->output);
        left->name = withoutSide (left->name);
        if (save (*left, left->file))
        {
            rightFile.deleteFile();
            logLine ("Room measurement: merged " + rightFile.getFileName() + " into " + left->file.getFileName());
        }
    }
    list.erase (std::remove (list.begin(), list.end(), nullptr), list.end());
}

juce::String fileSafe (juce::String s)
{
    s = juce::File::createLegalFileName (s).replaceCharacters (" ", "_");
    return s.isEmpty() ? juce::String ("measurement") : s.substring (0, 60);
}
} // namespace

juce::File folder()
{
    auto d = appDataDir().getChildFile ("Measurements");
    d.createDirectory();
    return d;
}

std::vector<std::unique_ptr<RoomMeasurement>> loadLibrary()
{
    std::vector<std::unique_ptr<RoomMeasurement>> list;
    for (const auto& f : folder().findChildFiles (juce::File::findFiles, false, juce::String ("*") + kExtension))
    {
        juce::String err;
        if (auto m = load (f, err)) list.push_back (std::move (m));
        else logLine ("Room measurement " + f.getFileName() + ": " + err);
    }
    mergePairs (list);
    std::sort (list.begin(), list.end(), [] (const auto& a, const auto& b) { return a->created > b->created; });
    return list;
}

bool save (const RoomMeasurement& m, const juce::File& file)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("format", "Hydra Mixer room measurement");
    o->setProperty ("version", kFormatVersion);
    o->setProperty ("name", m.name);
    o->setProperty ("created", m.created.toISO8601 (true));
    o->setProperty ("microphone", m.microphone);
    o->setProperty ("throughEq", m.throughEq);
    if (! m.calibration.empty())
    {
        auto* cal = new juce::DynamicObject();
        cal->setProperty ("name", juce::String (m.calibration.name));
        juce::Array<juce::var> pts;
        for (auto& [f, db] : m.calibration.points) pts.add (juce::Array<juce::var> { f, db });
        cal->setProperty ("points", pts);
        o->setProperty ("calibration", juce::var (cal));
    }

    auto* sw = new juce::DynamicObject();
    sw->setProperty ("sampleRate", m.spec.sampleRate);
    sw->setProperty ("startHz", m.spec.startHz);
    sw->setProperty ("endHz", m.spec.endHz);
    sw->setProperty ("seconds", m.spec.seconds);
    sw->setProperty ("levelDb", m.spec.levelDb);
    sw->setProperty ("repeats", m.spec.repeats);
    o->setProperty ("sweep", juce::var (sw));

    sideToVar (*o, m);
    if (! m.geometry.empty() || ! m.geometry.posMs[0].empty())
    {
        const auto& g = m.geometry;
        auto* geo = new juce::DynamicObject();
        geo->setProperty ("nearMs", timesToVar (g.nearMs, 2));
        geo->setProperty ("crossMs", timesToVar (g.crossMs, 2));
        geo->setProperty ("posLeftMs", timesToVar (g.posMs[0].data(), g.posMs[0].size()));
        geo->setProperty ("posRightMs", timesToVar (g.posMs[1].data(), g.posMs[1].size()));
        o->setProperty ("geometry", juce::var (geo));
    }
    if (m.right != nullptr)
    {
        auto* r = new juce::DynamicObject();
        sideToVar (*r, *m.right);
        o->setProperty ("right", juce::var (r));
    }

    return file.replaceWithText (juce::JSON::toString (juce::var (o), false));
}

bool saveToLibrary (RoomMeasurement& m)
{
    if (m.file == juce::File())
        m.file = folder().getNonexistentChildFile (m.created.formatted ("%Y-%m-%d_%H-%M-%S_") + fileSafe (m.name), kExtension, false);
    return save (m, m.file);
}

std::unique_ptr<RoomMeasurement> load (const juce::File& file, juce::String& error)
{
    const auto v = juce::JSON::parse (file.loadFileAsString());
    if (! v.isObject() || ! v.hasProperty ("impulseResponse"))
    {
        error = "not a Hydra room measurement";
        return nullptr;
    }
    if ((int) v["version"] > kFormatVersion)
    {
        error = "made by a newer version of Hydra Mixer";
        return nullptr;
    }
    auto m = std::make_unique<RoomMeasurement>();
    m->file = file;
    m->name = v["name"].toString();
    m->created = juce::Time::fromISO8601 (v["created"].toString());
    m->microphone = v["microphone"].toString();
    m->throughEq = (bool) v["throughEq"];
    if (const auto cal = v["calibration"]; cal.isObject())
    {
        m->calibration.name = cal["name"].toString().toStdString();
        if (auto* pts = cal["points"].getArray())
            for (auto& p : *pts)
                if (p.isArray() && p.size() >= 2) m->calibration.points.emplace_back ((double) p[0], (double) p[1]);
    }

    const auto sw = v["sweep"];
    m->spec.sampleRate = (double) sw.getProperty ("sampleRate", 48000.0);
    m->spec.startHz = (double) sw.getProperty ("startHz", 20.0);
    m->spec.endHz = (double) sw.getProperty ("endHz", 20000.0);
    m->spec.seconds = (double) sw.getProperty ("seconds", 5.0);
    m->spec.levelDb = (double) sw.getProperty ("levelDb", -15.0);
    m->spec.repeats = (int) sw.getProperty ("repeats", 1);

    if (! sideFromVar (v, *m))
    {
        error = "the impulse response is missing";
        return nullptr;
    }
    if (const auto geo = v["geometry"]; geo.isObject())
    {
        const auto nearMs = timesFromVar (geo["nearMs"]), crossMs = timesFromVar (geo["crossMs"]);
        for (size_t k = 0; k < 2; ++k)
        {
            m->geometry.nearMs[k] = k < nearMs.size() ? nearMs[k] : -1.0;
            m->geometry.crossMs[k] = k < crossMs.size() ? crossMs[k] : -1.0;
        }
        m->geometry.posMs[0] = timesFromVar (geo["posLeftMs"]);
        m->geometry.posMs[1] = timesFromVar (geo["posRightMs"]);
    }
    if (const auto r = v["right"]; r.isObject())
    {
        // The right speaker shares everything but its recording.
        auto right = std::make_shared<RoomMeasurement>();
        right->name = m->name;
        right->created = m->created;
        right->microphone = m->microphone;
        right->throughEq = m->throughEq;
        right->calibration = m->calibration;
        right->spec = m->spec;
        if (sideFromVar (r, *right)) m->right = std::move (right);
    }
    if (m->name.isEmpty()) m->name = file.getFileNameWithoutExtension();
    return m;
}

//==============================================================================
bool exportResponse (const RoomMeasurement& m, const juce::File& file, double smoothingFraction)
{
    if (! m.analysed) return false;
    const auto& a = m.analysis;
    const auto db = room::smooth (a.magDb, smoothingFraction);
    juce::String t;
    t << "* Hydra Mixer room measurement: " << m.name << "\n"
      << "* " << m.created.toString (true, true) << ", " << m.microphone << " <- " << m.output << "\n"
      << "* Smoothing: " << (smoothingFraction > 0 ? "1/" + juce::String (juce::roundToInt (smoothingFraction)) + " octave" : juce::String ("none"))
      << ", level in dB re. digital full scale (output -> microphone)\n"
      << "* Freq(Hz) SPL(dB)\n";
    for (size_t i = 0; i < a.freqs.size(); ++i)
        if (a.freqs[i] >= a.sweepStartHz && a.freqs[i] <= a.sweepEndHz)
            t << juce::String (a.freqs[i], 3) << " " << juce::String (db[i], 3) << "\n";
    return file.replaceWithText (t);
}

bool exportImpulse (const RoomMeasurement& m, const juce::File& file)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    if (static_cast<juce::FileOutputStream*> (stream.get())->failedToOpen()) return false;
    auto options = juce::AudioFormatWriterOptions {}
                       .withSampleRate (m.ir.sampleRate)
                       .withNumChannels (1)
                       .withBitsPerSample (32)
                       .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    juce::WavAudioFormat wav;
    auto w = wav.createWriterFor (stream, options);
    if (w == nullptr) return false;
    // Starts 10 ms before the sweep left the output, like the stored response.
    const float* ch[1] = { m.ir.ir.data() };
    return w->writeFromFloatArrays (ch, 1, (int) m.ir.ir.size());
}

bool exportWaterfall (const RoomMeasurement& m, const juce::File& file)
{
    if (! m.analysed) return false;
    const auto& a = m.analysis;
    juce::String t ("Freq (Hz)");
    for (double ms : a.waterfallMs) t << ";" << juce::String (ms, 1) << " ms";
    t << "\n";
    for (size_t i = 0; i < a.freqs.size(); ++i)
    {
        if (a.freqs[i] < a.sweepStartHz || a.freqs[i] > a.sweepEndHz) continue;
        t << juce::String (a.freqs[i], 2);
        for (auto& slice : a.waterfall) t << ";" << juce::String (slice[i], 2);
        t << "\n";
    }
    return file.replaceWithText (t);
}

room::AnalysisOptions analysisOptions (const RoomMeasurement& m)
{
    room::AnalysisOptions o;
    o.calibration = m.calibration;
    return o;
}

double arrivalDifferenceMs (const RoomMeasurement& m)
{
    if (m.right == nullptr) return std::numeric_limits<double>::quiet_NaN();
    const auto& g = m.geometry;
    // Clock-sync buffering taken out (geometry); older measurements: the impulse responses' own latency.
    const double d = ! g.posMs[0].empty() && ! g.posMs[1].empty() && g.posMs[0][0] >= 0.0 && g.posMs[1][0] >= 0.0
                         ? g.posMs[0][0] - g.posMs[1][0]
                         : m.ir.latencyMs - m.right->ir.latencyMs;
    return std::abs (d) <= CorrectionProfile::kMaxAlignMs ? d : std::numeric_limits<double>::quiet_NaN();
}

void alignSpeakers (CorrectionProfile& p, const RoomMeasurement& m)
{
    const double d = arrivalDifferenceMs (m);
    p.alignMs[0] = p.alignMs[1] = 0.0f;
    if (std::isnan (d)) return;
    p.alignMs[d > 0.0 ? 1 : 0] = (float) std::abs (d);   // the left arrives later: the right waits
}

//==============================================================================
namespace
{
/** 128-bit fingerprint (two 64-bit FNV-1a lanes over 32-bit words). */
struct Fingerprint
{
    uint64_t h[2] { 0xcbf29ce484222325ull, 0x84222325cbf29ce4ull };
    void word (uint32_t w) noexcept
    {
        h[0] = (h[0] ^ w) * 0x100000001b3ull;
        h[1] = (h[1] ^ (w * 0x9e3779b9u)) * 0x100000001b3ull;
    }
    void add (double v) noexcept { uint64_t b; std::memcpy (&b, &v, 8); word ((uint32_t) b); word ((uint32_t) (b >> 32)); }
    void add (int v) noexcept    { word ((uint32_t) v); }
    void add (const std::vector<float>& v) noexcept
    {
        add ((int) v.size());
        for (float x : v) { uint32_t b; std::memcpy (&b, &x, 4); word (b); }
    }
    void add (const room::ImpulseResponse& r) noexcept { add (r.ir); add (r.sampleRate); add (r.peak); }
    juce::String hex() const
    {
        return juce::String::toHexString ((juce::int64) h[0]).paddedLeft ('0', 16) + juce::String::toHexString ((juce::int64) h[1]).paddedLeft ('0', 16);
    }
};

constexpr int kCacheVersion = 1;    // raise when room::analyse changes its results
constexpr int kCacheMagic = 0x4e414e48;

/** Key of m's analysis with these options; full = with waterfall / decay / impulse (their options count too). */
juce::String cacheKey (const RoomMeasurement& m, const room::AnalysisOptions& o, bool full)
{
    Fingerprint f;
    f.add (kCacheVersion);
    f.add (full ? 1 : 0);
    for (auto* ir : m.impulses()) f.add (*ir);
    f.add ((int) m.nearFields.size());
    for (auto& ir : m.nearFields) f.add (ir);
    f.add (m.spec.startHz);
    f.add (m.spec.endHz);
    f.add (o.windowMs);
    f.add (o.resonanceMinHz);
    f.add (o.resonanceMaxHz);
    f.add (o.resonanceThresholdDb);
    f.add ((int) o.calibration.points.size());
    for (auto& [hz, db] : o.calibration.points) { f.add (hz); f.add (db); }
    if (full)
    {
        f.add (o.waterfallMs);
        f.add (o.waterfallSlices);
        f.add (o.waterfallWindowMs);
    }
    return f.hex();
}

void writeVec (juce::OutputStream& out, const std::vector<double>& v)
{
    out.writeInt ((int) v.size());
    for (double x : v) out.writeDouble (x);
}

bool readVec (juce::InputStream& in, std::vector<double>& v)
{
    const int n = in.readInt();
    if (n < 0 || (juce::int64) n * 8 > in.getNumBytesRemaining()) return false;
    v.resize ((size_t) n);
    for (auto& x : v) x = in.readDouble();
    return true;
}

void writeVecs (juce::OutputStream& out, const std::vector<std::vector<double>>& v)
{
    out.writeInt ((int) v.size());
    for (auto& x : v) writeVec (out, x);
}

bool readVecs (juce::InputStream& in, std::vector<std::vector<double>>& v)
{
    const int n = in.readInt();
    if (n < 0 || n > 100000) return false;
    v.resize ((size_t) n);
    for (auto& x : v)
        if (! readVec (in, x)) return false;
    return true;
}

juce::MemoryBlock serialise (const room::Analysis& a)
{
    juce::MemoryOutputStream out;
    out.writeInt (kCacheMagic);
    out.writeInt (kCacheVersion);
    out.writeDouble (a.sampleRate);
    writeVec (out, a.freqs);
    writeVec (out, a.magDb);
    out.writeDouble (a.referenceDb);
    writeVec (out, a.waterfallMs);
    writeVecs (out, a.waterfall);
    writeVec (out, a.etcMs);
    writeVec (out, a.etcDb);
    out.writeInt ((int) a.decay.size());
    for (auto& d : a.decay) { out.writeDouble (d.centreHz); out.writeDouble (d.edt); out.writeDouble (d.t20); out.writeDouble (d.t30); }
    out.writeInt ((int) a.resonances.size());
    for (auto& r : a.resonances) { out.writeDouble (r.freq); out.writeDouble (r.excessDb); out.writeDouble (r.q); out.writeDouble (r.ringMs); }
    out.writeDouble (a.sweepStartHz);
    out.writeDouble (a.sweepEndHz);
    writeVecs (out, a.positionDb);
    writeVec (out, a.nearDb);
    out.writeInt (kCacheMagic);   // complete
    return out.getMemoryBlock();
}

bool deserialise (const juce::MemoryBlock& mb, room::Analysis& a)
{
    juce::MemoryInputStream in (mb, false);
    if (in.readInt() != kCacheMagic || in.readInt() != kCacheVersion) return false;
    a.sampleRate = in.readDouble();
    if (! readVec (in, a.freqs) || ! readVec (in, a.magDb)) return false;
    a.referenceDb = in.readDouble();
    if (! readVec (in, a.waterfallMs) || ! readVecs (in, a.waterfall) || ! readVec (in, a.etcMs) || ! readVec (in, a.etcDb)) return false;
    const int decays = in.readInt();
    if (decays < 0 || decays > 1000) return false;
    a.decay.resize ((size_t) decays);
    for (auto& d : a.decay) { d.centreHz = in.readDouble(); d.edt = in.readDouble(); d.t20 = in.readDouble(); d.t30 = in.readDouble(); }
    const int resonances = in.readInt();
    if (resonances < 0 || resonances > 10000) return false;
    a.resonances.resize ((size_t) resonances);
    for (auto& r : a.resonances) { r.freq = in.readDouble(); r.excessDb = in.readDouble(); r.q = in.readDouble(); r.ringMs = in.readDouble(); }
    a.sweepStartHz = in.readDouble();
    a.sweepEndHz = in.readDouble();
    if (! readVecs (in, a.positionDb) || ! readVec (in, a.nearDb)) return false;
    return in.readInt() == kCacheMagic;
}

juce::File cacheFile (const juce::String& key) { return cacheFolder().getChildFile (key + ".hydraanalysis"); }

bool loadCached (const juce::String& key, room::Analysis& a)
{
    const auto f = cacheFile (key);
    juce::MemoryBlock mb;
    room::Analysis loaded;
    if (! f.existsAsFile() || ! f.loadFileAsData (mb) || ! deserialise (mb, loaded)) return false;
    a = std::move (loaded);
    f.setLastModificationTime (juce::Time::getCurrentTime());   // recently used: kept when the cache is trimmed
    return true;
}

void storeCached (const juce::String& key, const room::Analysis& a)
{
    // Atomic: the mixer and the plugin may analyse the same measurement at the same time.
    const auto mb = serialise (a);
    juce::TemporaryFile tmp (cacheFile (key));
    if (! tmp.getFile().replaceWithData (mb.getData(), mb.getSize()) || ! tmp.overwriteTargetFileWithTemporary()) return;
    auto files = cacheFolder().findChildFiles (juce::File::findFiles, false, "*.hydraanalysis");
    if (files.size() <= kMaxCached) return;
    std::sort (files.begin(), files.end(), [] (const juce::File& x, const juce::File& y)
               { return x.getLastModificationTime() > y.getLastModificationTime(); });
    for (int i = kMaxCached; i < files.size(); ++i) files.getReference (i).deleteFile();
}

room::Analysis compute (const RoomMeasurement& m, const room::AnalysisOptions& opts)
{
    auto a = room::analyse (m.impulses(), opts, m.spec.startHz, m.spec.endHz);
    if (! m.nearFields.empty())
    {
        // Only the response is needed; several speakers: power average.
        auto nearOpts = opts;
        nearOpts.responseOnly = true;
        std::vector<double> power;
        for (auto& ir : m.nearFields)
        {
            const auto n = room::analyse (ir, nearOpts, RoomMeasurement::kNearSweepStartHz, m.spec.endHz);
            power.resize (n.magDb.size(), 0.0);
            for (size_t i = 0; i < n.magDb.size(); ++i) power[i] += std::pow (10.0, n.magDb[i] / 10.0) / (double) m.nearFields.size();
        }
        a.nearDb.resize (power.size());
        for (size_t i = 0; i < power.size(); ++i) a.nearDb[i] = 10.0 * std::log10 (std::max (power[i], 1.0e-20));
    }
    return a;
}
} // namespace

juce::File cacheFolder()
{
    auto d = cacheOverride != juce::File() ? cacheOverride : appDataDir().getChildFile ("AnalysisCache");
    d.createDirectory();
    return d;
}

room::Analysis analyse (const RoomMeasurement& m, const room::AnalysisOptions& opts)
{
    room::Analysis a;
    const auto fullKey = cacheKey (m, opts, true);
    if (loadCached (fullKey, a)) return a;   // serves a responseOnly request too
    const auto quickKey = cacheKey (m, opts, false);
    if (opts.responseOnly && loadCached (quickKey, a)) return a;
    a = compute (m, opts);
    storeCached (opts.responseOnly ? quickKey : fullKey, a);
    return a;
}

} // namespace hydra::measurements

//==============================================================================
namespace hydra::calibrations
{
juce::File folder()
{
    auto d = appDataDir().getChildFile ("Calibrations");
    d.createDirectory();
    return d;
}

std::vector<room::Calibration> all()
{
    auto list = room::builtInCalibrations();
    auto files = folder().findChildFiles (juce::File::findFiles, false, "*.txt;*.cal;*.frd;*.csv");
    files.sort();
    for (const auto& f : files)
    {
        room::Calibration c;
        std::string err;
        if (! room::Calibration::parse (f.loadFileAsString().toStdString(), c, err)) continue;
        c.name = f.getFileNameWithoutExtension().toStdString();
        list.push_back (std::move (c));
    }
    return list;
}

room::Calibration import (const juce::File& file, juce::String& error)
{
    room::Calibration c;
    std::string err;
    if (! room::Calibration::parse (file.loadFileAsString().toStdString(), c, err))
    {
        error = juce::String (err);
        return {};
    }
    const auto dest = folder().getChildFile (file.getFileName());
    if (dest != file && ! file.copyFileTo (dest))
    {
        error = "cannot copy it to " + folder().getFullPathName();
        return {};
    }
    c.name = dest.getFileNameWithoutExtension().toStdString();
    return c;
}
} // namespace hydra::calibrations

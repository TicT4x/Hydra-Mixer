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
    o->setProperty ("output", m.output);
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

    auto* ir = new juce::DynamicObject();
    ir->setProperty ("sampleRate", m.ir.sampleRate);
    ir->setProperty ("peak", m.ir.peak);
    ir->setProperty ("latencyMs", m.ir.latencyMs);
    ir->setProperty ("noiseFloorDb", m.ir.noiseFloorDb);
    ir->setProperty ("capturePeakDb", m.ir.capturePeakDb);
    ir->setProperty ("irSnrDb", m.ir.irSnrDb);
    ir->setProperty ("clipped", m.ir.clipped);
    ir->setProperty ("samples", encode (m.ir.ir));
    o->setProperty ("impulseResponse", juce::var (ir));

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
    m->output = v["output"].toString();
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

    const auto ir = v["impulseResponse"];
    m->ir.sampleRate = (double) ir.getProperty ("sampleRate", m->spec.sampleRate);
    m->ir.peak = (int) ir["peak"];
    m->ir.latencyMs = (double) ir["latencyMs"];
    m->ir.noiseFloorDb = (double) ir["noiseFloorDb"];
    m->ir.capturePeakDb = (double) ir["capturePeakDb"];
    m->ir.irSnrDb = (double) ir["irSnrDb"];
    m->ir.clipped = (bool) ir["clipped"];
    m->ir.ir = decode (ir["samples"].toString());
    if (m->ir.ir.empty() || m->ir.sampleRate < 8000.0)
    {
        error = "the impulse response is missing";
        return nullptr;
    }
    m->ir.peak = juce::jlimit (0, (int) m->ir.ir.size() - 1, m->ir.peak);
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

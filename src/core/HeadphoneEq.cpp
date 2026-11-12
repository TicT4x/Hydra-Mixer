#include "HeadphoneEq.h"
#include "ParamEq.h"
#include "HeadphoneData.h"

namespace hydra::hp
{
namespace
{
constexpr const char* kUserExt = ".hydrahp";

double interp (const std::vector<double>& f, const std::vector<double>& v, double x)
{
    if (f.empty()) return 0.0;
    if (x <= f.front()) return v.front();
    if (x >= f.back()) return v.back();
    const auto it = std::upper_bound (f.begin(), f.end(), x);
    const auto hi = (size_t) (it - f.begin()), lo = hi - 1;
    const double t = (std::log (x) - std::log (f[lo])) / (std::log (f[hi]) - std::log (f[lo]));
    return v[lo] + t * (v[hi] - v[lo]);
}

Curve parseValues (const juce::String& line)
{
    Curve c;
    for (auto& t : juce::StringArray::fromTokens (line, " \t", ""))
        if (t.isNotEmpty()) c.push_back (t.getFloatValue());
    return c;
}

/** Reads the database format: "@hp name|kind|source", "@rig id|name", "@target name", each followed by a line of values. */
void parseDatabase (const juce::String& text, const juce::String& idPrefix, std::vector<Entry>* heads,
                    std::vector<Rig>* rigs, std::vector<Entry>* targets)
{
    juce::StringArray lines;
    lines.addLines (text);
    for (int i = 0; i + 1 < lines.size(); ++i)
    {
        const auto l = lines[i].trim();
        if (! l.startsWithChar ('@')) continue;
        auto values = parseValues (lines[i + 1]);
        if ((int) values.size() != kPoints) continue;
        const auto head = l.upToFirstOccurrenceOf (" ", false, false);
        auto fields = juce::StringArray::fromTokens (l.fromFirstOccurrenceOf (" ", false, false), "|", "");
        while (fields.size() < 3) fields.add ({});
        if (head == "@hp" && heads != nullptr)
            heads->push_back ({ idPrefix + fields[0], fields[0], fields[1], fields[2], std::move (values) });
        else if (head == "@rig" && rigs != nullptr)
            rigs->push_back ({ fields[0], fields[1], std::move (values) });
        else if (head == "@target" && targets != nullptr)
            targets->push_back ({ "target:" + fields[0], fields[0], {}, "Target", std::move (values) });
        ++i;
    }
}

juce::String formatEntry (const Entry& e)
{
    juce::String s;
    s << "# Hydra Mixer headphones (Headphone Correction): deviation from neutral (Harman), 1/12 octave from 20 Hz\n";
    s << "@hp " << e.name << "|" << e.kind << "|" << e.source << "\n";
    for (int i = 0; i < kPoints; ++i) s << (i > 0 ? " " : "") << juce::String (e.db[(size_t) i], 2);
    return s << "\n";
}

juce::String safeFileName (const juce::String& name)
{
    auto s = juce::File::createLegalFileName (name).trim();
    return s.isEmpty() ? juce::String ("Headphones") : s;
}
} // namespace

//==============================================================================
Library& Library::get()
{
    static Library lib;
    return lib;
}

Library::Library()
{
    parseDatabase (juce::String::fromUTF8 (HeadphoneData::headphones_txt, HeadphoneData::headphones_txtSize),
                   "db:", &builtIn, &rigList, &targetList);
    std::stable_sort (builtIn.begin(), builtIn.end(), [] (const Entry& a, const Entry& b) { return a.name.compareNatural (b.name, false) < 0; });
    targetList.insert (targetList.begin(), Entry { "target:neutral", "Neutral (Harman target)", {}, "Target", Curve ((size_t) kPoints, 0.0f) });
    scanUser (true);
}

juce::File Library::userFolder() { return appDataDir().getChildFile ("Headphones"); }

void Library::scanUser (bool force)
{
    const auto now = juce::Time::getMillisecondCounter();
    if (! force && now - lastScan < 2000) return;
    lastScan = now;
    user.clear();
    for (auto& f : userFolder().findChildFiles (juce::File::findFiles, false, juce::String ("*") + kUserExt))
    {
        std::vector<Entry> one;
        parseDatabase (f.loadFileAsString(), {}, &one, nullptr, nullptr);
        if (one.empty()) continue;
        one[0].id = "user:" + f.getFileNameWithoutExtension();
        user.push_back (std::move (one[0]));
    }
    std::sort (user.begin(), user.end(), [] (const Entry& a, const Entry& b) { return a.name.compareNatural (b.name) < 0; });
    all = builtIn;
    all.insert (all.end(), user.begin(), user.end());
}

const std::vector<Entry>& Library::headphones() { scanUser(); return all; }
const std::vector<Entry>& Library::targets()    { return targetList; }
const std::vector<Rig>& Library::rigs()         { return rigList; }

const Entry* Library::find (const juce::String& id)
{
    if (id.isEmpty()) return nullptr;
    for (auto* list : { &targetList, &builtIn, &user })
        for (auto& e : *list)
            if (e.id == id) return &e;
    if (id.startsWith ("user:"))   // added since the last scan
    {
        scanUser (true);
        for (auto& e : user) if (e.id == id) return &e;
    }
    return nullptr;
}

juce::String Library::import (const juce::File& file, const juce::String& name, const juce::String& kind,
                              const juce::String& rigId, juce::String& error)
{
    const auto text = file.loadFileAsString();
    Entry e;
    if (file.hasFileExtension (kUserExt))
    {
        std::vector<Entry> one;
        parseDatabase (text, {}, &one, nullptr, nullptr);
        if (one.empty()) { error = "The file holds no headphones."; return {}; }
        e = one[0];
    }
    else
    {
        Parsed p;
        if (! parseResponse (text, p, error)) return {};
        e.db = toGrid (p.freq, p.db);
        if (! p.hasOwnTarget)
        {
            const Rig* rig = nullptr;
            const auto want = kind == "in-ear" && rigId == "gras" ? juce::String ("iem") : rigId;
            for (auto& r : rigList) if (r.id == want) rig = &r;
            if (rig == nullptr && ! rigList.empty()) rig = &rigList.front();
            if (rig != nullptr)
                for (int i = 0; i < kPoints; ++i) e.db[(size_t) i] -= rig->db[(size_t) i];
        }
        normalise (e.db);
        e.kind = kind;
        e.source = "Imported";
    }
    if (name.trim().isNotEmpty()) e.name = name.trim();
    if (e.name.isEmpty()) e.name = file.getFileNameWithoutExtension();

    const auto folder = userFolder();
    folder.createDirectory();
    auto dest = folder.getChildFile (safeFileName (e.name) + kUserExt);
    if (dest.exists()) dest = dest.getNonexistentSibling (false);
    if (! dest.replaceWithText (formatEntry (e))) { error = "Could not write " + dest.getFullPathName(); return {}; }
    scanUser (true);
    return "user:" + dest.getFileNameWithoutExtension();
}

bool Library::remove (const juce::String& id)
{
    if (! id.startsWith ("user:")) return false;
    const auto f = userFolder().getChildFile (id.fromFirstOccurrenceOf (":", false, false) + kUserExt);
    const bool ok = f.moveToTrash() || f.deleteFile();
    scanUser (true);
    return ok;
}

bool Library::rename (const juce::String& id, const juce::String& newName)
{
    if (! id.startsWith ("user:") || newName.trim().isEmpty()) return false;
    const auto f = userFolder().getChildFile (id.fromFirstOccurrenceOf (":", false, false) + kUserExt);
    std::vector<Entry> one;
    parseDatabase (f.loadFileAsString(), {}, &one, nullptr, nullptr);
    if (one.empty()) return false;
    one[0].name = newName.trim();
    const bool ok = f.replaceWithText (formatEntry (one[0]));   // the file (and so the id) keeps its name
    scanUser (true);
    return ok;
}

//==============================================================================
bool parseResponse (const juce::String& text, Parsed& out, juce::String& error)
{
    out = {};
    juce::StringArray lines;
    lines.addLines (text);
    int valueCol = 1, targetCol = -1, errorCol = -1;
    std::vector<double> target;
    for (auto line : lines)
    {
        line = line.trim();
        if (line.isEmpty() || line.startsWithChar ('*') || line.startsWithChar ('#') || line.startsWithChar (';')
            || line.startsWithChar ('@'))
            continue;
        juce::StringArray tok;
        if (line.containsChar (';')) tok = juce::StringArray::fromTokens (line.replaceCharacter (',', '.'), ";", "\"");
        else                         tok = juce::StringArray::fromTokens (line, ", \t", "\"");
        tok.trim();
        tok.removeEmptyStrings();
        if (tok.size() < 2) continue;
        if (! juce::CharacterFunctions::isDigit (tok[0][0]) && tok[0][0] != '.')
        {
            // A header ("frequency,raw,...,target"): AutoEq results carry their own target.
            if (out.freq.empty())
                for (int i = 1; i < tok.size(); ++i)
                {
                    const auto h = tok[i].toLowerCase().trim();
                    if (h == "raw") valueCol = i;
                    else if (h == "target") targetCol = i;
                    else if (h == "error") errorCol = i;
                }
            continue;
        }
        const double f = tok[0].getDoubleValue();
        if (f <= 0.0 || (! out.freq.empty() && f <= out.freq.back())) continue;
        const int col = errorCol >= 0 && targetCol < 0 ? errorCol : valueCol;
        if (col >= tok.size()) continue;
        out.freq.push_back (f);
        out.db.push_back (tok[col].getDoubleValue());
        target.push_back (targetCol >= 0 && targetCol < tok.size() ? tok[targetCol].getDoubleValue() : 0.0);
    }
    if (out.freq.size() < 10 || out.freq.front() > 200.0 || out.freq.back() < 5000.0)
    {
        error = out.freq.size() < 10 ? "No frequency response found in the file (expected columns: frequency, level)."
                                     : "The response must cover at least 200 Hz to 5 kHz.";
        return false;
    }
    if (targetCol >= 0)
    {
        for (size_t i = 0; i < out.db.size(); ++i) out.db[i] -= target[i];
        out.hasOwnTarget = true;
    }
    else if (errorCol >= 0)
        out.hasOwnTarget = true;
    return true;
}

Curve toGrid (const std::vector<double>& freq, const std::vector<double>& db)
{
    Curve c ((size_t) kPoints);
    for (int i = 0; i < kPoints; ++i)
    {
        const double g = gridFreq (i), a = g * std::pow (2.0, -1.0 / 24.0), b = g * std::pow (2.0, 1.0 / 24.0);
        double sum = 0.0;
        int n = 0;
        for (size_t k = 0; k < freq.size(); ++k)
            if (freq[k] >= a && freq[k] < b) { sum += db[k]; ++n; }
        c[(size_t) i] = (float) (n > 0 ? sum / n : interp (freq, db, g));
    }
    return c;
}

void normalise (Curve& c)
{
    double sum = 0.0;
    int n = 0;
    for (int i = 0; i < (int) c.size(); ++i)
        if (gridFreq (i) >= 300.0 && gridFreq (i) <= 3000.0) { sum += c[(size_t) i]; ++n; }
    if (n > 0) for (auto& v : c) v -= (float) (sum / n);
}

//==============================================================================
namespace
{
struct FitBand
{
    int type = ParamEq::Bell;
    double logF = 0.0, gain = 0.0, logQ = 0.0;
    std::array<double, kPoints> db {};
};

/** cos (w) and cos (2w) of every grid point at the sample rate: the magnitude of a biquad without trigonometry. */
struct GridTrig
{
    std::array<double, kPoints> c1 {}, c2 {};
    double sr = 0.0;
    void prepare (double rate)
    {
        if (rate == sr) return;
        sr = rate;
        for (int i = 0; i < kPoints; ++i)
        {
            const double w = 2.0 * juce::MathConstants<double>::pi * juce::jmin (gridFreq (i), sr * 0.4999) / sr;
            c1[(size_t) i] = std::cos (w);
            c2[(size_t) i] = std::cos (2.0 * w);
        }
    }
};

void evalBand (FitBand& b, double sr, const GridTrig& t)
{
    const auto c = dsp::BiquadCoeffs::make (ParamEq::toDsp (b.type), sr, std::exp (b.logF), std::exp (b.logQ), b.gain);
    // |H|^2 = (b0^2 + b1^2 + b2^2 + 2 (b0 b1 + b1 b2) cos w + 2 b0 b2 cos 2w) / (same with 1, a1, a2)
    const double b0 = c.b0, b1 = c.b1, b2 = c.b2, a1 = c.a1, a2 = c.a2;
    const double nb = b0 * b0 + b1 * b1 + b2 * b2, nb1 = 2.0 * (b0 * b1 + b1 * b2), nb2 = 2.0 * b0 * b2;
    const double na = 1.0 + a1 * a1 + a2 * a2, na1 = 2.0 * (a1 + a1 * a2), na2 = 2.0 * a2;
    for (int i = 0; i < kPoints; ++i)
    {
        const double num = nb + nb1 * t.c1[(size_t) i] + nb2 * t.c2[(size_t) i];
        const double den = na + na1 * t.c1[(size_t) i] + na2 * t.c2[(size_t) i];
        b.db[(size_t) i] = 10.0 * std::log10 (juce::jmax (1.0e-20, num) / juce::jmax (1.0e-20, den));
    }
}

/** Bounds of a band (frequency, gain, Q). */
void clampBand (FitBand& b, double maxBoost)
{
    const bool lowShelf = b.type == ParamEq::LowShelf, highShelf = b.type == ParamEq::HighShelf;
    const double fLo = highShelf ? 2000.0 : 20.0, fHi = lowShelf ? 300.0 : 16000.0;
    b.logF = juce::jlimit (std::log (fLo), std::log (fHi), b.logF);
    b.gain = juce::jlimit (-16.0, maxBoost + 2.0, b.gain);
    // Narrow boosts sound worse than narrow cuts (and fit only this one measurement): boosts at most Q 3, in the
    // treble Q 2.
    const double f = std::exp (b.logF);
    const double qBell = b.gain > 0.0 ? (f > 5000.0 ? 2.0 : 3.0) : 8.0;
    const double qLo = lowShelf || highShelf ? 0.4 : 0.3, qHi = lowShelf || highShelf ? 1.2 : qBell;
    b.logQ = juce::jlimit (std::log (qLo), std::log (qHi), b.logQ);
}
} // namespace

Design design (const Curve& from, const Curve& to, const Settings& s, double sr)
{
    Design d;
    if ((int) from.size() != kPoints || (int) to.size() != kPoints) return d;

    // What the EQ should do, smoothed: 1/6 octave, wider in the treble (measurements differ there with the fit
    // on the head, an EQ should not follow every narrow peak).
    Curve raw ((size_t) kPoints);
    for (int i = 0; i < kPoints; ++i) raw[(size_t) i] = to[(size_t) i] - from[(size_t) i];
    d.wanted.assign ((size_t) kPoints, 0.0f);
    for (int i = 0; i < kPoints; ++i)
    {
        const double f = gridFreq (i);
        const int h = f < 5000.0 ? 1 : f < 10000.0 ? 2 : 4;
        double sum = 0.0;
        int n = 0;
        for (int k = juce::jmax (0, i - h); k <= juce::jmin (kPoints - 1, i + h); ++k) { sum += raw[(size_t) k]; ++n; }
        d.wanted[(size_t) i] = (float) (sum / n);
    }
    normalise (d.wanted);
    for (int i = 0; i < kPoints; ++i)
    {
        // Boosts in the treble are limited further: there a narrow dip often is a cancellation that moves with the
        // fit on the head, filling it would make a sharp peak on another head.
        const double f = gridFreq (i);
        const float boost = f < 6000.0 ? s.maxBoostDb : f < 10000.0 ? s.maxBoostDb * 0.6f : s.maxBoostDb * 0.35f;
        d.wanted[(size_t) i] = juce::jlimit (-20.0f, boost, d.wanted[(size_t) i]);
    }

    std::array<double, kPoints> w {};
    for (int i = 0; i < kPoints; ++i)
    {
        const double f = gridFreq (i);
        w[(size_t) i] = f < 30.0 ? 0.5 : f <= 10000.0 ? 1.0 : f <= 14000.0 ? 0.5 : 0.15;
    }

    GridTrig trig;
    trig.prepare (sr);
    std::vector<FitBand> bands;
    bands.reserve ((size_t) s.maxBands);
    std::array<double, kPoints> eq {};
    const auto sumEq = [&]
    {
        eq.fill (0.0);
        for (auto& b : bands) for (int i = 0; i < kPoints; ++i) eq[(size_t) i] += b.db[(size_t) i];
    };
    const auto cost = [&]
    {
        double c = 0.0;
        for (int i = 0; i < kPoints; ++i) { const double r = d.wanted[(size_t) i] - eq[(size_t) i]; c += w[(size_t) i] * r * r; }
        return c;
    };
    const auto meanWanted = [&] (double lo, double hi)
    {
        double sum = 0.0;
        int n = 0;
        for (int i = 0; i < kPoints; ++i) if (gridFreq (i) >= lo && gridFreq (i) <= hi) { sum += d.wanted[(size_t) i]; ++n; }
        return n > 0 ? sum / n : 0.0;
    };

    const double maxBoost = s.maxBoostDb;
    const auto addBand = [&] (int type, double f, double gain, double q)
    {
        FitBand b;
        b.type = type;
        b.logF = std::log (f);
        b.gain = gain;
        b.logQ = std::log (q);
        clampBand (b, maxBoost);
        evalBand (b, sr, trig);
        bands.push_back (b);
        sumEq();
    };
    // Optimises every band, a coordinate at a time, until no step helps.
    const auto optimise = [&]
    {
        double best = cost();
        double steps[3] = { std::log (2.0) / 24.0, 0.25, 0.08 };
        for (int round = 0; round < 6; ++round)
        {
            for (int pass = 0; pass < 40; ++pass)
            {
                bool improved = false;
                for (auto& b : bands)
                    for (int p = 0; p < 3; ++p)
                        for (double dir : { 1.0, -1.0 })
                        {
                            const FitBand old = b;
                            double& v = p == 0 ? b.logF : p == 1 ? b.gain : b.logQ;
                            v += dir * steps[p];
                            clampBand (b, maxBoost);
                            evalBand (b, sr, trig);
                            for (int i = 0; i < kPoints; ++i) eq[(size_t) i] += b.db[(size_t) i] - old.db[(size_t) i];
                            const double c = cost();
                            if (c < best - 1.0e-9) { best = c; improved = true; break; }
                            for (int i = 0; i < kPoints; ++i) eq[(size_t) i] -= b.db[(size_t) i] - old.db[(size_t) i];
                            b = old;
                        }
                if (! improved) break;
            }
            for (auto& st : steps) st *= 0.5;
        }
    };

    if (s.maxBands >= 2)
    {
        addBand (ParamEq::LowShelf, 105.0, meanWanted (20.0, 80.0), 0.7);
        addBand (ParamEq::HighShelf, 10000.0, 0.5 * meanWanted (10000.0, 16000.0), 0.7);
        optimise();
    }
    while ((int) bands.size() < s.maxBands)
    {
        int at = -1;
        double top = 0.4;
        for (int i = 0; i < kPoints; ++i)
        {
            if (gridFreq (i) > 16000.0) break;
            const double r = w[(size_t) i] * std::abs (d.wanted[(size_t) i] - eq[(size_t) i]);
            if (r > top) { top = r; at = i; }
        }
        if (at < 0) break;   // close enough
        const double r = d.wanted[(size_t) at] - eq[(size_t) at];
        int lo = at, hi = at;
        const auto res = [&] (int i) { return d.wanted[(size_t) i] - eq[(size_t) i]; };
        while (lo > 0 && res (lo - 1) * r > 0 && std::abs (res (lo - 1)) > std::abs (r) * 0.5) --lo;
        while (hi < kPoints - 1 && res (hi + 1) * r > 0 && std::abs (res (hi + 1)) > std::abs (r) * 0.5) ++hi;
        const double bw = juce::jmax (1.0 / 6.0, (hi - lo + 1) / 12.0);   // octaves
        const double q = std::sqrt (std::pow (2.0, bw)) / (std::pow (2.0, bw) - 1.0);
        addBand (ParamEq::Bell, gridFreq (at), r, q);
        optimise();
    }

    // Results: drop bands that do (almost) nothing, preamp against clipping.
    auto& p = d.profile;
    p.kind = CorrectionProfile::Kind::headphone;
    for (auto& b : bands)
    {
        if (std::abs (b.gain) < 0.2) continue;
        CorrectionProfile::Band out;
        out.type = b.type;
        out.freq = (float) std::exp (b.logF);
        out.gainDb = (float) b.gain;
        out.q = (float) std::exp (b.logQ);
        p.bands.push_back (out);
    }
    std::sort (p.bands.begin(), p.bands.end(), [] (auto& a, auto& b) { return a.freq < b.freq; });
    p.preampDb = (float) -p.maxBoostDb (sr);

    const auto rms = [&] (bool after)
    {
        double sum = 0.0;
        int n = 0;
        const auto resp = responseOf (p, sr);
        for (int i = 0; i < kPoints; ++i)
        {
            if (gridFreq (i) < 30.0 || gridFreq (i) > 10000.0) continue;
            const double r = d.wanted[(size_t) i] - (after ? resp[(size_t) i] : 0.0);
            sum += r * r;
            ++n;
        }
        return n > 0 ? std::sqrt (sum / n) : 0.0;
    };
    d.rmsBeforeDb = rms (false);
    d.rmsAfterDb = rms (true);
    return d;
}

CorrectionProfile scaled (const CorrectionProfile& full, float amount)
{
    auto p = full;
    amount = juce::jlimit (0.0f, 1.0f, amount);
    if (amount <= 0.0f) p.bands.clear();
    for (auto& b : p.bands) b.gainDb *= amount;
    p.preampDb = p.bands.empty() ? 0.0f : (float) -p.maxBoostDb();
    if (p.preampDb > -0.01f) p.preampDb = 0.0f;
    return p;
}

double loudnessDb (const CorrectionProfile& profile, double sr)
{
    // Pink noise: the same power in every octave, so the mean power over a logarithmic grid (20 Hz .. 20 kHz).
    double sum = 0.0;
    int n = 0;
    for (double f = 20.0; f <= 20000.0; f *= std::pow (2.0, 1.0 / 24.0), ++n)
        sum += std::pow (10.0, profile.magnitudeDb (f, sr) / 10.0);
    return n > 0 ? 10.0 * std::log10 (sum / n) : 0.0;
}

Curve responseOf (const CorrectionProfile& profile, double sr)
{
    Curve c ((size_t) kPoints);
    for (int i = 0; i < kPoints; ++i)
        c[(size_t) i] = (float) (profile.magnitudeDb (gridFreq (i), sr) - profile.preampDb);
    return c;
}
} // namespace hydra::hp

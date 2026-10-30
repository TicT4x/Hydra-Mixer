#include "Correction.h"
#include "ParamEq.h"

namespace hydra
{
namespace
{
dsp::BiquadCoeffs coeffsOf (const CorrectionProfile::Band& b, double sr)
{
    return dsp::BiquadCoeffs::make (ParamEq::toDsp (b.type), sr, b.freq, b.q, b.gainDb);
}
} // namespace

juce::String CorrectionProfile::describe() const
{
    if (isEmpty()) return "No profile";
    juce::String s = kindNames()[(int) kind] + ", " + juce::String ((int) bands.size()) + (bands.size() == 1 ? " band" : " bands");
    if (preampDb != 0.0f) s << ", preamp " << juce::String (preampDb, 1) << " dB";
    return s;
}

double CorrectionProfile::magnitudeDb (double f, double sampleRate) const
{
    double db = preampDb;
    for (auto& b : bands) db += coeffsOf (b, sampleRate).magnitudeDb (f, sampleRate);
    return db;
}

double CorrectionProfile::maxBoostDb (double sampleRate) const
{
    double top = 0.0;
    for (double f = 20.0; f <= 20000.0; f *= 1.02)
    {
        double db = 0.0;
        for (auto& b : bands) db += coeffsOf (b, sampleRate).magnitudeDb (f, sampleRate);
        top = juce::jmax (top, db);
    }
    return top;
}

std::unique_ptr<juce::XmlElement> CorrectionProfile::toXml() const
{
    auto x = std::make_unique<juce::XmlElement> ("CORRECTION");
    x->setAttribute ("name", name);
    x->setAttribute ("kind", (int) kind);
    x->setAttribute ("preamp", preampDb);
    for (auto& b : bands)
    {
        auto* e = x->createNewChildElement ("BAND");
        e->setAttribute ("type", b.type);
        e->setAttribute ("freq", b.freq);
        e->setAttribute ("gain", b.gainDb);
        e->setAttribute ("q", b.q);
    }
    return x;
}

CorrectionProfile CorrectionProfile::fromXml (const juce::XmlElement* x)
{
    CorrectionProfile p;
    if (x == nullptr) return p;
    p.name = x->getStringAttribute ("name");
    p.kind = (Kind) juce::jlimit (0, 3, x->getIntAttribute ("kind", 0));
    p.preampDb = juce::jlimit (-30.0f, 12.0f, (float) x->getDoubleAttribute ("preamp", 0.0));
    for (auto* e : x->getChildWithTagNameIterator ("BAND"))
    {
        if ((int) p.bands.size() >= kMaxBands) break;
        Band b;
        b.type = juce::jlimit (0, (int) ParamEq::NumTypes - 1, e->getIntAttribute ("type", 0));
        b.freq = juce::jlimit (10.0f, 24000.0f, (float) e->getDoubleAttribute ("freq", 1000.0));
        b.gainDb = juce::jlimit (-30.0f, 30.0f, (float) e->getDoubleAttribute ("gain", 0.0));
        b.q = juce::jlimit (0.05f, 100.0f, (float) e->getDoubleAttribute ("q", 0.707));
        p.bands.push_back (b);
    }
    return p;
}

bool CorrectionProfile::fromApo (const juce::String& text, CorrectionProfile& out, juce::String& error)
{
    CorrectionProfile p;
    p.kind = out.kind;
    p.name = out.name;
    int skipped = 0;
    for (auto line : juce::StringArray::fromLines (text))
    {
        line = line.trim();
        if (line.startsWithIgnoreCase ("Preamp:"))
        {
            p.preampDb = juce::jlimit (-30.0f, 12.0f, line.fromFirstOccurrenceOf (":", false, false).trim().getFloatValue());
            continue;
        }
        if (! line.startsWithIgnoreCase ("Filter")) continue;
        juce::StringArray t;
        t.addTokens (line.fromFirstOccurrenceOf (":", false, false), " \t", {});
        t.removeEmptyStrings();
        if (t.size() < 2 || ! t[0].equalsIgnoreCase ("ON")) continue;

        const auto kind = t[1].toUpperCase();
        Band b;
        if (kind == "PK" || kind == "PEQ" || kind == "MODAL") b.type = ParamEq::Bell;
        else if (kind.startsWith ("LS"))                      b.type = ParamEq::LowShelf;
        else if (kind.startsWith ("HS"))                      b.type = ParamEq::HighShelf;
        else if (kind.startsWith ("HP"))                      b.type = ParamEq::LowCut;
        else if (kind.startsWith ("LP"))                      b.type = ParamEq::HighCut;
        else if (kind == "NO")                                b.type = ParamEq::Notch;
        else if (kind == "BP")                                b.type = ParamEq::BandPass;
        else { ++skipped; continue; }

        auto valueAfter = [&t] (const char* key, float fallback)
        {
            for (int i = 0; i + 1 < t.size(); ++i)
                if (t[i].equalsIgnoreCase (key)) return t[i + 1].getFloatValue();
            return fallback;
        };
        b.freq = juce::jlimit (10.0f, 24000.0f, valueAfter ("Fc", 1000.0f));
        b.gainDb = juce::jlimit (-30.0f, 30.0f, valueAfter ("Gain", 0.0f));
        b.q = juce::jlimit (0.05f, 100.0f, valueAfter ("Q", 0.707f));
        if ((int) p.bands.size() < kMaxBands) p.bands.push_back (b);
        else ++skipped;
    }
    if (p.bands.empty())
    {
        error = "No usable filter lines (\"Filter: ON PK Fc ... Hz Gain ... dB Q ...\") found.";
        return false;
    }
    if (skipped > 0) error = juce::String (skipped) + " filter line(s) could not be used.";
    out = std::move (p);
    return true;
}

juce::String CorrectionProfile::toApo() const
{
    static const char* codes[] = { "PK", "NO", "BP", "LPQ", "HPQ", "LSC", "HSC" };
    juce::String t;
    t << "# " << name << " (" << describe() << "), exported from Hydra Mixer\n";
    t << "Preamp: " << juce::String (preampDb, 1) << " dB\n";
    int k = 1;
    for (auto& b : bands)
    {
        t << "Filter " << k++ << ": ON " << codes[juce::jlimit (0, 6, b.type)] << " Fc " << juce::String (b.freq, 1) << " Hz";
        if (ParamEq::typeUsesGain (b.type)) t << " Gain " << juce::String (b.gainDb, 1) << " dB";
        t << " Q " << juce::String (b.q, 3) << "\n";
    }
    return t;
}

//==============================================================================
void Correction::setProfile (CorrectionProfile p, double sampleRate, uint64_t callbackCount)
{
    if ((int) p.bands.size() > CorrectionProfile::kMaxBands) p.bands.resize ((size_t) CorrectionProfile::kMaxBands);
    profile = std::move (p);
    ++version;
    publish (sampleRate, callbackCount);
}

void Correction::publish (double sampleRate, uint64_t callbackCount)
{
    std::unique_ptr<Coeffs> c;
    if (! profile.isEmpty())
    {
        c = std::make_unique<Coeffs>();
        c->sampleRate = sampleRate;
        c->count = (int) profile.bands.size();
        c->preamp = dbToGain (profile.preampDb, -100.0f);
        for (int k = 0; k < c->count; ++k) c->c[(size_t) k] = coeffsOf (profile.bands[(size_t) k], sampleRate);
    }
    live.store (c.get(), std::memory_order_release);
    if (owned != nullptr) retired.emplace_back (std::move (owned), callbackCount + 2);
    owned = std::move (c);
}

void Correction::update (double sampleRate, uint64_t callbackCount, bool running)
{
    if (owned != nullptr && owned->sampleRate != sampleRate)
        publish (sampleRate, callbackCount);
    retired.erase (std::remove_if (retired.begin(), retired.end(),
                                   [&] (const auto& r) { return callbackCount >= r.second || ! running; }),
                   retired.end());
}

void Correction::reset() noexcept
{
    for (auto& z : z1) z[0] = z[1] = 0.0f;
    for (auto& z : z2) z[0] = z[1] = 0.0f;
    wasOn = false;
}

void Correction::process (float* l, float* r, int n, double sampleRate) noexcept
{
    const Coeffs* c = live.load (std::memory_order_acquire);
    // Off, no profile, or built for another sample rate (rebuilt by the message thread within 0.1 s).
    if (! on->isOn() || c == nullptr || c->sampleRate != sampleRate)
    {
        wasOn = false;
        return;
    }
    if (! wasOn)
    {
        reset();
        lastPreamp = c->preamp;
        wasOn = true;
    }

    float* ch[2] = { l, r };
    for (int k = 0; k < c->count; ++k)
    {
        const auto& q = c->c[(size_t) k];
        for (int s = 0; s < 2; ++s)
        {
            float* x = ch[s];
            float s1 = z1[k][s], s2 = z2[k][s];
            for (int i = 0; i < n; ++i)
            {
                const float in = x[i];
                const float out = q.b0 * in + s1;
                s1 = q.b1 * in - q.a1 * out + s2;
                s2 = q.b2 * in - q.a2 * out;
                x[i] = out;
            }
            z1[k][s] = s1; z2[k][s] = s2;
        }
    }
    dsp::applyRamp (l, n, lastPreamp, c->preamp);
    dsp::applyRamp (r, n, lastPreamp, c->preamp);
    lastPreamp = c->preamp;
}
} // namespace hydra

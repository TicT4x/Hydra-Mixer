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

void bandsToXml (juce::XmlElement& x, const std::vector<CorrectionProfile::Band>& bands)
{
    for (auto& b : bands)
    {
        auto* e = x.createNewChildElement ("BAND");
        e->setAttribute ("type", b.type);
        e->setAttribute ("freq", b.freq);
        e->setAttribute ("gain", b.gainDb);
        e->setAttribute ("q", b.q);
    }
}

std::vector<CorrectionProfile::Band> bandsFromXml (const juce::XmlElement& x)
{
    std::vector<CorrectionProfile::Band> bands;
    for (auto* e : x.getChildWithTagNameIterator ("BAND"))
    {
        if ((int) bands.size() >= CorrectionProfile::kMaxBands) break;
        CorrectionProfile::Band b;
        b.type = juce::jlimit (0, (int) ParamEq::NumTypes - 1, e->getIntAttribute ("type", 0));
        b.freq = juce::jlimit (10.0f, 24000.0f, (float) e->getDoubleAttribute ("freq", 1000.0));
        b.gainDb = juce::jlimit (-30.0f, 30.0f, (float) e->getDoubleAttribute ("gain", 0.0));
        b.q = juce::jlimit (0.05f, 100.0f, (float) e->getDoubleAttribute ("q", 0.707));
        bands.push_back (b);
    }
    return bands;
}

double sideDb (const CorrectionProfile::Side& s, double f, double sr)
{
    double db = s.preampDb;
    for (auto& b : s.bands) db += coeffsOf (b, sr).magnitudeDb (f, sr);
    return db;
}
} // namespace

juce::String CorrectionProfile::describe() const
{
    if (isEmpty()) return "No profile";
    auto count = [] (size_t n) { return juce::String ((int) n) + (n == 1 ? " band" : " bands"); };
    juce::String s;
    if (alignMs[0] > 0.0f || alignMs[1] > 0.0f)
        s << (alignMs[0] > 0.0f ? "L" : "R") << " delayed " << juce::String (juce::jmax (alignMs[0], alignMs[1]), 2) << " ms, ";
    if (separate)
    {
        s << "L " << (int) bands.size() << " / R " << count (right.bands.size());
        if (preampDb != 0.0f || right.preampDb != 0.0f)
            s << ", preamp " << juce::String (preampDb, 1) << " / " << juce::String (right.preampDb, 1) << " dB";
        return s;
    }
    s << count (bands.size());
    if (preampDb != 0.0f) s << ", preamp " << juce::String (preampDb, 1) << " dB";
    return s;
}

void CorrectionProfile::setSide (int channel, Side s)
{
    if (channel < 0)
    {
        separate = false;
        preampDb = s.preampDb;
        bands = std::move (s.bands);
        right = {};
        return;
    }
    if (! separate)   // the other side keeps what both had so far
    {
        right = { preampDb, bands };
        separate = true;
    }
    if (channel == 0) { preampDb = s.preampDb; bands = std::move (s.bands); }
    else              right = std::move (s);
}

double CorrectionProfile::magnitudeDb (double f, double sampleRate, int channel) const
{
    return sideDb (side (channel), f, sampleRate);
}

double CorrectionProfile::maxBoostDb (double sampleRate) const
{
    double top = 0.0;
    for (int ch = 0; ch < (separate ? 2 : 1); ++ch)
    {
        auto s = side (ch);
        s.preampDb = 0.0f;
        for (double f = 20.0; f <= 20000.0; f *= 1.02) top = juce::jmax (top, sideDb (s, f, sampleRate));
    }
    return top;
}

double CorrectionProfile::maxGainDb (double sampleRate) const
{
    double top = -100.0;
    for (int ch = 0; ch < (separate ? 2 : 1); ++ch)
        for (double f = 20.0; f <= 20000.0; f *= 1.02) top = juce::jmax (top, magnitudeDb (f, sampleRate, ch));
    return top;
}

std::unique_ptr<juce::XmlElement> CorrectionProfile::toXml() const
{
    auto x = std::make_unique<juce::XmlElement> ("CORRECTION");
    x->setAttribute ("name", name);
    if (source.isNotEmpty()) x->setAttribute ("source", source);
    if (poolId.isNotEmpty()) x->setAttribute ("pool", poolId);
    if (alignMs[0] > 0.0f) x->setAttribute ("delayL", alignMs[0]);
    if (alignMs[1] > 0.0f) x->setAttribute ("delayR", alignMs[1]);
    if (design.valid)
    {
        auto* d = x->createNewChildElement ("DESIGN");
        d->setAttribute ("low", design.lowHz);
        d->setAttribute ("high", design.highHz);
        d->setAttribute ("cut", design.maxCutDb);
        d->setAttribute ("boost", design.maxBoostDb);
        d->setAttribute ("detail", design.detail);
        d->setAttribute ("bands", design.bands);
        d->setAttribute ("loudness", design.keepLoudness);
        d->setAttribute ("near", design.useNear);
        d->setAttribute ("align", design.timeAlign);
        d->setAttribute ("levelOffset", design.levelOffsetDb);
        if (design.refined) d->setAttribute ("refined", true);
    }
    x->setAttribute ("kind", (int) kind);
    x->setAttribute ("preamp", preampDb);
    bandsToXml (*x, bands);
    if (separate)
    {
        auto* r = x->createNewChildElement ("RIGHT");
        r->setAttribute ("preamp", right.preampDb);
        bandsToXml (*r, right.bands);
    }
    return x;
}

CorrectionProfile CorrectionProfile::fromXml (const juce::XmlElement* x)
{
    CorrectionProfile p;
    if (x == nullptr) return p;
    p.name = x->getStringAttribute ("name");
    p.source = x->getStringAttribute ("source");
    p.poolId = x->getStringAttribute ("pool");
    p.alignMs[0] = juce::jlimit (0.0f, kMaxAlignMs, (float) x->getDoubleAttribute ("delayL", 0.0));
    p.alignMs[1] = juce::jlimit (0.0f, kMaxAlignMs, (float) x->getDoubleAttribute ("delayR", 0.0));
    if (auto* d = x->getChildByName ("DESIGN"))
    {
        auto& g = p.design;
        g.valid = true;
        g.lowHz = d->getDoubleAttribute ("low", g.lowHz);
        g.highHz = d->getDoubleAttribute ("high", g.highHz);
        g.maxCutDb = d->getDoubleAttribute ("cut", g.maxCutDb);
        g.maxBoostDb = d->getDoubleAttribute ("boost", g.maxBoostDb);
        g.detail = d->getDoubleAttribute ("detail", g.detail);
        g.bands = d->getIntAttribute ("bands", g.bands);
        g.keepLoudness = d->getBoolAttribute ("loudness", g.keepLoudness);
        g.useNear = d->getBoolAttribute ("near", g.useNear);
        g.timeAlign = d->getBoolAttribute ("align", g.timeAlign);
        g.levelOffsetDb = d->getDoubleAttribute ("levelOffset", 0.0);
        g.refined = d->getBoolAttribute ("refined", false);
    }
    const int kind = x->getIntAttribute ("kind", 0);
    p.kind = kind == (int) Kind::headphone ? Kind::headphone : kind == (int) Kind::other ? Kind::other : Kind::room;
    p.preampDb = juce::jlimit (-30.0f, 12.0f, (float) x->getDoubleAttribute ("preamp", 0.0));
    p.bands = bandsFromXml (*x);
    if (auto* r = x->getChildByName ("RIGHT"))
    {
        p.separate = true;
        p.right.preampDb = juce::jlimit (-30.0f, 12.0f, (float) r->getDoubleAttribute ("preamp", 0.0));
        p.right.bands = bandsFromXml (*r);
    }
    return p;
}

bool CorrectionProfile::fromApo (const juce::String& text, CorrectionProfile& out, juce::String& error)
{
    // Equalizer APO applies every line to the channels of the last "Channel:" line (all at first).
    Side left, rightSide;
    bool toLeft = true, toRight = true, anySeparate = false;
    float delay[2] {};
    int skipped = 0;
    for (auto line : juce::StringArray::fromLines (text))
    {
        line = line.trim();
        if (line.startsWithIgnoreCase ("Channel:"))
        {
            juce::StringArray ch;
            ch.addTokens (line.fromFirstOccurrenceOf (":", false, false).toUpperCase(), " \t,", {});
            ch.removeEmptyStrings();
            const bool all = ch.isEmpty() || ch.contains ("ALL");
            toLeft = all || ch.contains ("L") || ch.contains ("1");
            toRight = all || ch.contains ("R") || ch.contains ("2");
            continue;
        }
        if (line.startsWithIgnoreCase ("Delay:"))   // time alignment of one channel (ms; "samples" are converted at 48 kHz)
        {
            const auto v = line.fromFirstOccurrenceOf (":", false, false).trim();
            const float ms = juce::jlimit (0.0f, kMaxAlignMs, v.getFloatValue() / (v.containsIgnoreCase ("sample") ? 48.0f : 1.0f));
            if (toLeft != toRight) delay[toLeft ? 0 : 1] = ms;
            continue;
        }
        if (line.startsWithIgnoreCase ("Preamp:") || line.startsWithIgnoreCase ("Filter")) anySeparate = anySeparate || toLeft != toRight;
        if (line.startsWithIgnoreCase ("Preamp:"))
        {
            const float db = juce::jlimit (-30.0f, 12.0f, line.fromFirstOccurrenceOf (":", false, false).trim().getFloatValue());
            if (toLeft) left.preampDb += db;
            if (toRight) rightSide.preampDb += db;
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
        bool used = false;
        for (auto* s : { toLeft ? &left : nullptr, toRight ? &rightSide : nullptr })
            if (s != nullptr && (int) s->bands.size() < kMaxBands) { s->bands.push_back (b); used = true; }
        if (! used) ++skipped;
    }
    if (left.bands.empty() && rightSide.bands.empty())
    {
        error = "No usable filter lines (\"Filter: ON PK Fc ... Hz Gain ... dB Q ...\") found.";
        return false;
    }
    if (skipped > 0) error = juce::String (skipped) + " filter line(s) could not be used.";
    CorrectionProfile p;
    p.kind = out.kind;
    p.name = out.name;
    p.setSide (-1, left);
    if (anySeparate && ! (left == rightSide)) p.setSide (1, rightSide);
    p.alignMs[0] = delay[0];
    p.alignMs[1] = delay[1];
    out = std::move (p);
    return true;
}

juce::String CorrectionProfile::toApo() const
{
    static const char* codes[] = { "PK", "NO", "BP", "LPQ", "HPQ", "LSC", "HSC" };
    juce::String t;
    t << "# " << name << " (" << describe() << "), exported from Hydra Mixer\n";
    int k = 1;
    auto write = [&] (const Side& s)
    {
        t << "Preamp: " << juce::String (s.preampDb, 1) << " dB\n";
        for (auto& b : s.bands)
        {
            t << "Filter " << k++ << ": ON " << codes[juce::jlimit (0, 6, b.type)] << " Fc " << juce::String (b.freq, 1) << " Hz";
            if (ParamEq::typeUsesGain (b.type)) t << " Gain " << juce::String (b.gainDb, 1) << " dB";
            t << " Q " << juce::String (b.q, 3) << "\n";
        }
    };
    if (! separate)
        write (side (0));
    else
    {
        t << "Channel: L\n";
        write (side (0));
        t << "Channel: R\n";
        write (side (1));
        t << "Channel: all\n";
    }
    for (int ch = 0; ch < 2; ++ch)   // time alignment
        if (alignMs[ch] > 0.0f)
            t << "Channel: " << (ch == 0 ? "L" : "R") << "\nDelay: " << juce::String (alignMs[ch], 3) << " ms\nChannel: all\n";
    return t;
}

//==============================================================================
void Correction::setProfile (CorrectionProfile p, double sampleRate, uint64_t callbackCount)
{
    if ((int) p.bands.size() > CorrectionProfile::kMaxBands) p.bands.resize ((size_t) CorrectionProfile::kMaxBands);
    if ((int) p.right.bands.size() > CorrectionProfile::kMaxBands) p.right.bands.resize ((size_t) CorrectionProfile::kMaxBands);
    if (! p.separate) p.right = {};
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
        for (int ch = 0; ch < 2; ++ch)
        {
            const auto s = profile.side (ch);
            c->count[ch] = (int) s.bands.size();
            c->preamp[ch] = dbToGain (s.preampDb, -100.0f);
            for (int k = 0; k < c->count[ch]; ++k) c->c[ch][(size_t) k] = coeffsOf (s.bands[(size_t) k], sampleRate);
            c->delay[ch] = juce::jlimit (0, kDelaySize - 1, juce::roundToInt (profile.alignMs[ch] * 0.001 * sampleRate));
        }
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
    for (auto& b : delayBuf) std::fill (b.begin(), b.end(), 0.0f);
    delayPos = 0;
    wasOn = false;
}

void Correction::process (float* l, float* r, int n, double sampleRate) noexcept
{
    const Coeffs* c = live.load (std::memory_order_acquire);
    // Off, no profile, or built for another sample rate (rebuilt by the message thread within 0.1 s).
    if (on == nullptr || ! on->isOn() || c == nullptr || c->sampleRate != sampleRate)
    {
        wasOn = false;
        return;
    }
    if (! wasOn)
    {
        reset();
        lastPreamp[0] = c->preamp[0];
        lastPreamp[1] = c->preamp[1];
        wasOn = true;
    }

    // Each channel through its own bands (the same ones unless the profile is separate).
    float* ch[2] = { l, r };
    for (int s = 0; s < 2; ++s)
    {
        float* x = ch[s];
        for (int k = 0; k < c->count[s]; ++k)
        {
            const auto& q = c->c[s][(size_t) k];
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
        dsp::applyRamp (x, n, lastPreamp[s], c->preamp[s]);
        lastPreamp[s] = c->preamp[s];
    }

    // Time alignment: the nearer speaker waits (a ring per channel; delay 0 passes straight through it).
    if (c->delay[0] > 0 || c->delay[1] > 0)
    {
        for (int s = 0; s < 2; ++s)
        {
            float* x = ch[s];
            float* b = delayBuf[s].data();
            const int d = c->delay[s];
            int w = delayPos;
            for (int i = 0; i < n; ++i)
            {
                b[w] = x[i];
                int rd = w - d;
                if (rd < 0) rd += kDelaySize;
                x[i] = b[rd];
                if (++w == kDelaySize) w = 0;
            }
        }
        delayPos = (delayPos + n) % kDelaySize;
    }
}
} // namespace hydra

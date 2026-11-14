#include "ParamEq.h"
#include "HiResFir.h"
#include "MatchEq.h"

namespace hydra
{
ParamEq::~ParamEq() { delete fir.load(); }

void ParamEq::processFir (float* l, float* r, int n) noexcept
{
    if (auto* f = fir.load (std::memory_order_acquire)) f->process (l, r, n);
}

void ParamEq::resetFir() noexcept
{
    if (auto* f = fir.load (std::memory_order_acquire)) f->reset();
}

const std::vector<float>* ParamEq::matchCurve (int side)
{
    if (! matchData.ready()) return nullptr;
    if (const auto key = matchKey(); key != curveKey)
    {
        curveKey = key;
        match::MatchSettings m;
        m.smooth = matchSmooth->get() * 0.01f;
        m.amount = matchAmount->get() * 0.01f;
        m.lowHz = matchLow->get();
        m.highHz = matchHigh->get();
        const auto& d = matchData;
        // The level stays as the mono sum's match keeps it; the single channels keep their level relative to it.
        const double mean = match::matchMean (d.source, d.reference, m);
        curveCache[0] = match::matchCurve (d.source, d.reference, m, &mean);
        curveCache[1] = curveCache[0];
        auto pick = [] (const std::vector<float>& c, const std::vector<float>& fallback) -> const std::vector<float>& { return c.empty() ? fallback : c; };
        if (matchModeIndex() == leftRight)
        {
            curveCache[0] = match::matchCurve (pick (d.sourceL, d.source), pick (d.referenceL, d.reference), m, &mean);
            curveCache[1] = match::matchCurve (pick (d.sourceR, d.source), pick (d.referenceR, d.reference), m, &mean);
        }
        else if (matchModeIndex() == midSide)
        {
            if (! d.sourceS.empty() && ! d.referenceS.empty()) curveCache[1] = match::matchCurve (d.sourceS, d.referenceS, m, &mean);
            else std::fill (curveCache[1].begin(), curveCache[1].end(), 0.0f);   // nothing known about the side: unchanged
        }
    }
    return &curveCache[juce::jlimit (0, 1, side)];
}
} // namespace hydra

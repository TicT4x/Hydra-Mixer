#include "EqUndo.h"

namespace hydra
{
EqUndo::EqUndo (Engine& e) : engine (e)
{
    forgetAll();
    startTimerHz (20);
}

EqUndo::~EqUndo() { stopTimer(); }

EqUndo::Snapshot EqUndo::snapshot (const ParamEq& eq)
{
    Snapshot s;
    s.reserve (1 + 5 * ParamEq::kBands);
    s.push_back (eq.on->get());
    for (int k = 0; k < ParamEq::kBands; ++k)
    {
        const auto i = (size_t) k;
        for (auto* p : { eq.bandOn[i], eq.type[i], eq.gain[i], eq.freq[i], eq.q[i] })
            s.push_back (p->get());
    }
    return s;
}

void EqUndo::apply (ParamEq& eq, const Snapshot& s)
{
    if (s.size() != (size_t) (1 + 5 * ParamEq::kBands)) return;
    size_t n = 0;
    eq.on->set (s[n++]);
    for (int k = 0; k < ParamEq::kBands; ++k)
    {
        const auto i = (size_t) k;
        for (auto* p : { eq.bandOn[i], eq.type[i], eq.gain[i], eq.freq[i], eq.q[i] })
            p->set (s[n++]);
    }
}

EqUndo::Track& EqUndo::track (ParamEq& eq)
{
    auto it = tracks.find (&eq);
    if (it == tracks.end())
    {
        Track t;
        t.committed = snapshot (eq);
        t.seenVersion = eq.versionSum();
        it = tracks.emplace (&eq, std::move (t)).first;
    }
    return it->second;
}

void EqUndo::commit (ParamEq& eq, Track& t)
{
    t.pending = false;
    t.seenVersion = eq.versionSum();
    auto now = snapshot (eq);
    if (now == t.committed) return;
    t.undoSteps.push_back (std::move (t.committed));
    if ((int) t.undoSteps.size() > kMaxSteps) t.undoSteps.erase (t.undoSteps.begin());
    t.redoSteps.clear();
    t.committed = std::move (now);
}

void EqUndo::timerCallback()
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    const bool mouseDown = juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown();
    for (auto& [eq, t] : tracks)
    {
        if (const auto v = eq->versionSum(); v != t.seenVersion)
        {
            t.seenVersion = v;
            t.changedAt = now;
            t.pending = true;
        }
        if (t.pending && ! mouseDown && now - t.changedAt > 250.0)
            commit (*eq, t);
    }
}

bool EqUndo::undo (ParamEq& eq)
{
    auto& t = track (eq);
    commit (eq, t); // a change still in progress becomes its own step first
    if (t.undoSteps.empty()) return false;
    t.redoSteps.push_back (std::move (t.committed));
    t.committed = std::move (t.undoSteps.back());
    t.undoSteps.pop_back();
    apply (eq, t.committed);
    t.seenVersion = eq.versionSum(); // our own change is not a new step
    return true;
}

bool EqUndo::redo (ParamEq& eq)
{
    auto& t = track (eq);
    if (snapshot (eq) != t.committed) return false; // edited since the undo: the redo history is stale
    if (t.redoSteps.empty()) return false;
    t.undoSteps.push_back (std::move (t.committed));
    t.committed = std::move (t.redoSteps.back());
    t.redoSteps.pop_back();
    apply (eq, t.committed);
    t.seenVersion = eq.versionSum();
    t.pending = false;
    return true;
}

bool EqUndo::canUndo (ParamEq& eq)
{
    auto& t = track (eq);
    return ! t.undoSteps.empty() || snapshot (eq) != t.committed;
}

void EqUndo::forget (ParamEq& eq)
{
    tracks.erase (&eq);
    track (eq);
}

void EqUndo::forgetAll()
{
    tracks.clear();
    for (auto& s : engine.inputs) track (s.eq);
    for (auto& b : engine.buses)  track (b.eq);
}
} // namespace hydra

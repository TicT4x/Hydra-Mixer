#pragma once
#include "engine/Engine.h"
#include <map>

namespace hydra
{
/** Undo / redo for EQ edits (Ctrl+Z, Ctrl+Y / Ctrl+Shift+Z) of every input and bus.

    Works on snapshots of all EQ parameters, so every way of editing counts as a step: dragging a point,
    knobs, typed values, mouse wheel, adding / removing bands, MIDI. A step is recorded once the change
    has settled (no mouse button down and ~250 ms without further changes), so one drag = one step.
    Message thread only. */
class EqUndo : private juce::Timer
{
public:
    explicit EqUndo (Engine& engine);
    ~EqUndo() override;

    /** Returns false if there is nothing to undo / redo for this EQ. */
    bool undo (ParamEq& eq);
    bool redo (ParamEq& eq);
    bool canUndo (ParamEq& eq);

    /** Drops the history of one EQ (channel removed) or of all (other settings loaded). */
    void forget (ParamEq& eq);
    void forgetAll();

    static constexpr int kMaxSteps = 100;

private:
    using Snapshot = std::vector<float>;
    struct Track
    {
        Snapshot committed;                 // state after the last recorded step
        std::vector<Snapshot> undoSteps, redoSteps;
        uint64_t seenVersion = 0;
        double changedAt = 0.0;
        bool pending = false;
    };

    void timerCallback() override;
    Track& track (ParamEq& eq);
    void commit (ParamEq& eq, Track& t);   // records a pending change as a step
    static Snapshot snapshot (const ParamEq& eq);
    static void apply (ParamEq& eq, const Snapshot& s);

    Engine& engine;
    std::map<ParamEq*, Track> tracks;
};
} // namespace hydra

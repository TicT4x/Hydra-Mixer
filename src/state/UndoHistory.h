#pragma once
#include "core/Params.h"

namespace hydra
{
/** Undo / redo (Ctrl+Z, Ctrl+Y / Ctrl+Shift+Z) for every stored parameter of every channel, independent
    of where the change came from in the UI (faders, knobs, EQ curve, typed values, shortcuts). Changes
    made by MIDI controllers are not recorded (Param::isExternalChange): the hardware position would
    no longer match after an undo.

    Runs on the message thread: watches the parameter versions (20 Hz) and groups changes into steps.
    A step ends when nothing changed for 0.4 s and no mouse button is held, so one fader drag = one step.
    Names, devices, plugins and adding / removing channels are not covered (only parameter values). */
class UndoHistory : private juce::Timer
{
public:
    explicit UndoHistory (ParamRegistry& registry);
    ~UndoHistory() override;

    bool canUndo() const noexcept { return ! undoStack.empty() || ! pending.empty(); }
    bool canRedo() const noexcept { return ! redoStack.empty() && pending.empty(); }

    /** Return a short description of what was changed ("IN 1 Fader", "... (+2 more)"), empty if nothing. */
    juce::String undo();
    juce::String redo();

    /** Forgets the history and takes the current values as the new starting point
        (after loading / importing a session). */
    void clear();

    /** A channel was removed: its parameters were reset. Drops them from the history (an undo must not
        bring back values of a removed channel) and takes the reset values without recording a step. */
    void forget (const std::vector<Param*>& params);

    static constexpr size_t kMaxSteps = 200;

private:
    struct Change { size_t index; float before, after; };
    using Step = std::vector<Change>;

    void timerCallback() override;
    void scan();
    void commitPending();
    void apply (const Step&, bool useBefore);
    juce::String describe (const Step&) const;
    void resync();

    std::vector<Param*> tracked;           // stored parameters, fixed after construction
    std::vector<float> values;             // last known value per tracked parameter
    std::vector<uint32_t> versions;
    std::vector<std::pair<size_t, float>> pending; // parameter index, value before the open step
    std::vector<Step> undoStack, redoStack;
    double lastChange = 0;
};
} // namespace hydra

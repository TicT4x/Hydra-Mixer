#include "UndoHistory.h"

namespace hydra
{
UndoHistory::UndoHistory (ParamRegistry& registry)
{
    // Whether a channel slot is in use is changed by adding / removing the channel, never by undo.
    for (auto& p : registry.all())
        if (p->persistent && ! p->id.endsWith (".active"))
            tracked.push_back (p.get());
    values.resize (tracked.size());
    versions.resize (tracked.size());
    resync();
    startTimerHz (20);
}

UndoHistory::~UndoHistory() { stopTimer(); }

void UndoHistory::resync()
{
    for (size_t i = 0; i < tracked.size(); ++i)
    {
        versions[i] = tracked[i]->getVersion();
        values[i] = tracked[i]->get();
    }
}

void UndoHistory::clear()
{
    undoStack.clear();
    redoStack.clear();
    pending.clear();
    resync();
}

void UndoHistory::forget (const std::vector<Param*>& params)
{
    // Edits made before the removal stay their own steps.
    scan();
    commitPending();

    std::vector<bool> drop (tracked.size(), false);
    for (size_t i = 0; i < tracked.size(); ++i)
        drop[i] = std::find (params.begin(), params.end(), tracked[i]) != params.end();
    for (auto* stack : { &undoStack, &redoStack })
    {
        for (auto& step : *stack)
            step.erase (std::remove_if (step.begin(), step.end(), [&] (const Change& c) { return drop[c.index]; }), step.end());
        stack->erase (std::remove_if (stack->begin(), stack->end(), [] (const Step& s) { return s.empty(); }), stack->end());
    }
    resync();
}

void UndoHistory::scan()
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    for (size_t i = 0; i < tracked.size(); ++i)
    {
        const auto v = tracked[i]->getVersion();
        if (v == versions[i]) continue;
        versions[i] = v;
        const float value = tracked[i]->get();
        if (value == values[i]) continue;

        if (! tracked[i]->isExternalChange()) // MIDI moves are not undo steps
        {
            bool known = false;
            for (auto& [idx, before] : pending)
                if (idx == i) { known = true; break; }
            if (! known) pending.emplace_back (i, values[i]);
            lastChange = now;
        }
        values[i] = value;
    }
}

void UndoHistory::commitPending()
{
    if (pending.empty()) return;
    Step step;
    for (auto& [i, before] : pending)
        if (before != values[i])            // a value that went back to where it was is no change
            step.push_back ({ i, before, values[i] });
    pending.clear();
    if (step.empty()) return;

    undoStack.push_back (std::move (step));
    if (undoStack.size() > kMaxSteps) undoStack.erase (undoStack.begin());
    redoStack.clear();
}

void UndoHistory::timerCallback()
{
    scan();
    // Keep the step open while a mouse button is held (dragging a fader or an EQ point).
    if (! pending.empty()
        && juce::Time::getMillisecondCounterHiRes() - lastChange > 400.0
        && ! juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown())
        commitPending();
}

void UndoHistory::apply (const Step& step, bool useBefore)
{
    for (auto& c : step)
    {
        auto& p = *tracked[c.index];
        p.set (useBefore ? c.before : c.after);
        values[c.index] = p.get();
        versions[c.index] = p.getVersion(); // our own change: do not record it as a new step
    }
}

juce::String UndoHistory::undo()
{
    scan();
    commitPending();
    if (undoStack.empty()) return {};
    auto step = std::move (undoStack.back());
    undoStack.pop_back();
    apply (step, true);
    const auto text = describe (step);
    redoStack.push_back (std::move (step));
    return text;
}

juce::String UndoHistory::redo()
{
    scan();
    if (! pending.empty()) { commitPending(); return {}; } // a new edit makes redo meaningless
    if (redoStack.empty()) return {};
    auto step = std::move (redoStack.back());
    redoStack.pop_back();
    apply (step, false);
    const auto text = describe (step);
    undoStack.push_back (std::move (step));
    return text;
}

juce::String UndoHistory::describe (const Step& step) const
{
    if (step.empty()) return {};
    const auto& first = *tracked[step.front().index];
    // SEL switches two buttons at once; name the one that was turned on.
    if (step.size() == 2)
        for (auto& c : step)
            if (tracked[c.index]->type == ParamType::Toggle && c.after >= 0.5f)
                return tracked[c.index]->name;
    if (step.size() == 1) return first.name;
    return first.name + "  (+" + juce::String ((int) step.size() - 1) + " more)";
}
} // namespace hydra

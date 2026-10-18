// Undo / redo of parameter changes (state/UndoHistory).
#include "engine/Engine.h"
#include "state/UndoHistory.h"
#include <cstdio>

int runUndoTest()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    hydra::ParamPoller poller;
    int fails = 0;
    auto check = [&] (bool c, const char* what) { std::printf ("%s %s\n", c ? "ok:  " : "FAIL:", what); if (! c) ++fails; };

    hydra::Engine engine;
    auto& fader = engine.params.get ("in1.gain");
    auto& mute = engine.params.get ("in1.mute");
    fader.set (0.0f);
    mute.set (0.0f);
    hydra::UndoHistory undo (engine.params);

    check (! undo.canUndo() && undo.undo().isEmpty(), "nothing to undo at start");

    fader.set (-12.0f);
    check (undo.undo().isNotEmpty() && fader.get() == 0.0f, "undo restores the fader");
    check (undo.canRedo(), "redo available after undo");
    check (undo.redo().isNotEmpty() && fader.get() == -12.0f, "redo applies it again");

    mute.set (1.0f);
    undo.undo();
    check (! mute.isOn() && fader.get() == -12.0f, "second step undone on its own");
    undo.undo();
    check (fader.get() == 0.0f, "first step undone next");
    check (! undo.canUndo(), "history empty");

    // A new edit after undo drops the redo branch.
    undo.redo();
    fader.set (-3.0f);
    check (undo.redo().isEmpty() && fader.get() == -3.0f, "new edit discards redo");

    // Several changes before the step closes are one step; a change that returns to its value is none.
    hydra::UndoHistory grouped (engine.params);
    fader.set (-6.0f); fader.set (-9.0f); mute.set (1.0f);
    grouped.undo();
    check (fader.get() == -3.0f && ! mute.isOn(), "changes within one step are undone together");
    hydra::UndoHistory noop (engine.params);
    fader.set (-1.0f); fader.set (-3.0f);
    check (noop.undo().isEmpty(), "change back to the old value is not a step");

    // SEL: one click switches two buttons; the step is named after the one turned on.
    auto& sel1 = engine.params.get ("bus.A1.sel");
    auto& sel2 = engine.params.get ("bus.A2.sel");
    sel1.set (0.0f); sel2.set (0.0f);
    hydra::UndoHistory sel (engine.params);
    sel1.set (1.0f); sel.undo(); sel.redo();
    sel2.set (1.0f); sel1.set (0.0f);
    check (sel.undo() == sel2.name && sel1.isOn() && ! sel2.isOn(), "SEL switch undone as one step");
    sel1.set (0.0f);

    // The timer closes a step after 0.4 s without changes.
    hydra::UndoHistory timed (engine.params);
    fader.set (-30.0f);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (700);
    fader.set (-40.0f);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (700);
    timed.undo();
    check (fader.get() == -30.0f, "steps are closed by the timer");

    // MIDI changes are not recorded, also not the SEL buttons a MIDI SEL press switches off.
    hydra::UndoHistory midi (engine.params);
    fader.set (-50.0f, true);
    mute.set (1.0f, true);
    check (! midi.canUndo() && midi.undo().isEmpty() && fader.get() == -50.0f && mute.isOn(), "MIDI changes are not undo steps");
    fader.set (-8.0f);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
    fader.set (-20.0f, true);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
    midi.undo();
    check (fader.get() == -50.0f, "undo of a UI edit goes back to the value before it");
    sel1.set (1.0f);
    engine.enforceExclusiveSelect();
    hydra::UndoHistory midiSel (engine.params);
    sel2.set (1.0f, true);
    engine.enforceExclusiveSelect();
    check (! sel1.isOn() && sel2.isOn() && midiSel.undo().isEmpty(), "SEL switched by MIDI: no undo step");

    // clear(): loaded / imported state is the new start
    fader.set (-20.0f);
    grouped.clear();
    check (! grouped.canUndo() && ! grouped.canRedo(), "clear forgets the history");
    return fails;
}

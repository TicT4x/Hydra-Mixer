// Scenes (state/Scenes): store / recall, unsaved-change detection, triggers, remove / move, persistence.
#include "state/Session.h"
#include <cstdio>

int runSceneTest()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    hydra::ParamPoller poller;
    int fails = 0;
    auto check = [&] (bool c, const char* what) { std::printf ("%s %s\n", c ? "ok:  " : "FAIL:", what); if (! c) ++fails; };

    hydra::Engine engine;
    hydra::SceneBank bank (engine);
    auto& fader = engine.params.get ("in1.gain");
    auto& mute = engine.params.get ("in2.mute");
    auto& active = engine.params.get ("in3.active");

    fader.set (-6.0f);
    const int a = bank.addFromMixer ("Talk");
    fader.set (-20.0f);
    mute.set (1.0f);
    const int b = bank.addFromMixer ("Music");
    check (a == 0 && b == 1 && bank.size() == 2 && bank.getCurrent() == 1, "two scenes added, the last is current");

    active.set (0.0f);
    bank.recall (a);
    check (fader.get() == -6.0f && ! mute.isOn(), "recall restores the stored values (defaults included)");
    check (! active.isOn(), "recall does not add or remove channels");
    active.set (1.0f);
    check (! bank.differsFromMixer (a), "no unsaved changes right after recall");

    fader.set (-3.0f);
    check (bank.differsFromMixer (a) && bank.countDifferences (a) == 1, "an edit shows as unsaved change");
    bank.recall (b);   // switching scenes never asks
    check (fader.get() == -20.0f && mute.isOn(), "switching discards the unsaved edit");
    bank.recall (a);
    check (fader.get() == -6.0f, "the edit was not stored in the scene");
    fader.set (-9.0f);
    bank.store (a);
    bank.recall (b);
    bank.recall (a);
    check (fader.get() == -9.0f, "store saves the edit");

    // Triggers (MIDI / shortcuts): the parameter is switched on, the timer recalls and resets it.
    engine.sceneRecall[1]->set (1.0f, true);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (120);
    check (bank.getCurrent() == 1 && mute.isOn() && ! engine.sceneRecall[1]->isOn(), "trigger parameter recalls the scene");
    engine.sceneNext->set (1.0f, true);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (120);
    check (bank.getCurrent() == 0 && ! mute.isOn(), "next scene wraps around");
    engine.sceneRecall[5]->set (1.0f, true);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (120);
    check (bank.getCurrent() == 0 && ! engine.sceneRecall[5]->isOn(), "trigger of an empty slot is ignored");

    // Remove / move: the triggers of the following scenes move with them.
    bank.addFromMixer ("Third");
    hydra::Param* removed = nullptr;
    auto moves = bank.remove (0, removed);
    check (bank.size() == 2 && bank.get (0).name == "Music" && removed == engine.sceneRecall[0], "remove shifts the scenes up");
    check (moves.size() == 2 && moves[0].first == engine.sceneRecall[1] && moves[0].second == engine.sceneRecall[0],
           "remove returns the trigger moves");
    moves = bank.move (1, 0);
    check (bank.get (0).name == "Third" && bank.get (1).name == "Music" && moves.size() == 2, "move reorders the scenes");

    // Persistence: with the session, and as an export file.
    auto xml = bank.toXml();
    hydra::SceneBank loaded (engine);
    loaded.fromXml (*xml, false);
    check (loaded.size() == 2 && loaded.get (1).name == "Music" && loaded.get (1).values.at ("in2.mute") == 1.0f,
           "scenes survive save / load");
    auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("hydra_scene_test.hydrascenes");
    check (bank.exportTo (file, { 1 }), "export writes a file");
    juce::String err;
    check (loaded.importFrom (file, err) == 1 && loaded.size() == 3 && loaded.get (2).name == "Music", "import appends");
    file.deleteFile();

    // Session round trip
    {
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("hydra_scene_session");
        dir.deleteRecursively();
        dir.createDirectory();
        hydra::MidiMapper midi (engine.params);
        hydra::Session session (engine, midi, dir);
        session.scenes.addFromMixer ("Saved");
        check (session.save (dir.getChildFile ("s.xml")), "session with scenes saved");
        hydra::Session other (engine, midi, dir);
        other.load (dir.getChildFile ("s.xml"));
        check (other.scenes.size() == 1 && other.scenes.get (0).name == "Saved", "session load restores the scenes");
        dir.deleteRecursively();
    }
    return fails;
}

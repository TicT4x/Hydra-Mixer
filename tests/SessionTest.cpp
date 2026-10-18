// Checks that a session with changed parameters is written to disk and read back.
#include "state/Session.h"
#include <cstdio>

int runSessionTest()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    hydra::ParamPoller poller;
    int fails = 0;
    {
        hydra::Engine engine;
        hydra::MidiMapper midi (engine.params);
        hydra::Session session (engine, midi);

        engine.params.get ("in1.comp.thr").set (-14.6f);
        engine.inputs[0].name = "Mic";
        auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("hydra_session_test.xml");
        f.deleteFile();
        const bool ok = session.save (f);
        const auto text = f.loadFileAsString();
        std::printf ("%s session.save returned %d, file size %d\n", ok && text.contains ("Mic") ? "ok:  " : "FAIL:", (int) ok, (int) text.length());
        if (! ok || ! text.contains ("Mic") || ! text.contains ("in1_comp_thr")) ++fails;
        engine.inputs[0].name = "Mic2";
        const bool ok2 = session.save (f);
        const bool has2 = f.loadFileAsString().contains ("Mic2");
        std::printf ("%s overwrite existing session file (returned %d)\n", ok2 && has2 ? "ok:  " : "FAIL:", (int) ok2);
        if (! ok2 || ! has2) ++fails;
        f.deleteFile();
    }
    {   // keyboard shortcuts round trip (plain key = local only, nothing is registered with Windows)
        hydra::Engine engine;
        hydra::MidiMapper midi (engine.params);
        hydra::Hotkeys keys (engine.params);
        hydra::Session session (engine, midi);
        session.hotkeys = &keys;
        keys.assign (engine.params.get ("in2.mute"), juce::KeyPress ('M'));
        keys.assign (engine.params.get ("in1.mute"), juce::KeyPress ('M')); // steals the key from in2
        auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("hydra_hotkey_test.xml");
        session.save (f);
        keys.clearAll();
        session.load (f);
        f.deleteFile();
        const auto b = keys.getBindings();
        const bool okLoad = b.size() == 1 && b[0].paramId == "in1.mute" && b[0].key == juce::KeyPress ('M') && ! b[0].global;
        std::printf ("%s hotkeys saved and restored (%d binding)\n", okLoad ? "ok:  " : "FAIL:", (int) b.size());
        if (! okLoad) ++fails;
        engine.params.get ("in1.mute").set (0.0f);
        const bool hit = keys.handleLocalKey (juce::KeyPress ('M')) && engine.params.get ("in1.mute").isOn();
        std::printf ("%s local hotkey toggles the parameter\n", hit ? "ok:  " : "FAIL:");
        if (! hit) ++fails;
        const bool glob = hydra::Hotkeys::wantsGlobal (juce::KeyPress ('M', juce::ModifierKeys::ctrlModifier, 0))
                          && hydra::Hotkeys::wantsGlobal (juce::KeyPress (juce::KeyPress::F9Key))
                          && ! hydra::Hotkeys::wantsGlobal (juce::KeyPress ('M'));
        std::printf ("%s Ctrl/Alt/F-keys are global, plain keys local\n", glob ? "ok:  " : "FAIL:");
        if (! glob) ++fails;
    }
    {   // settings protection: own temp directory, never the user's %APPDATA%
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("hydra_backup_test");
        dir.deleteRecursively();
        dir.createDirectory();
        auto check = [&] (bool c, const char* what) { std::printf ("%s %s\n", c ? "ok:  " : "FAIL:", what); if (! c) ++fails; };

        hydra::Engine engine;
        hydra::MidiMapper midi (engine.params);
        hydra::Session session (engine, midi, dir);

        session.defaultFile().replaceWithText ("<HYDRASESSION garbage");
        session.loadDefault();
        session.saveDefault();
        auto b = session.listBackups();
        check (b.size() == 1 && b[0].getFileName().contains ("unreadable") && b[0].loadFileAsString() == "<HYDRASESSION garbage",
               "unreadable session is moved to backups, not overwritten");

        engine.inputs[0].name = "Studio Mic";
        session.saveDefault();
        session.loadDefault();
        session.loadDefault(); // second start without changes
        b = session.listBackups();
        check (b.size() == 2 && b[0].getFileName().contains ("start") && b[0].loadFileAsString().contains ("Studio Mic"),
               "start backup taken once, identical starts are not duplicated");

        auto exported = dir.getChildFile ("export.xml");
        check (session.exportTo (exported) && hydra::Session::isSessionFile (exported), "export writes a session file");

        engine.inputs[0].name = "Changed";
        engine.params.get ("in1.gain").set (-12.0f);
        juce::String err;
        auto bogus = dir.getChildFile ("bogus.xml");
        bogus.replaceWithText ("<OTHER/>");
        check (! session.importFrom (bogus, err) && engine.inputs[0].name == "Changed", "import rejects foreign files without changing anything");
        check (session.importFrom (exported, err) && engine.inputs[0].name == "Studio Mic"
               && engine.params.get ("in1.gain").get() == engine.params.get ("in1.gain").defaultValue,
               "import restores names and parameters");
        bool hasPreImport = false;
        for (auto& f : session.listBackups())
            hasPreImport |= f.getFileName().contains ("before-import") && f.loadFileAsString().contains ("Changed");
        check (hasPreImport, "state before the import is kept as backup");

        juce::Thread::sleep (20);
        session.defaultFile().replaceWithText (session.defaultFile().loadFileAsString().replace ("Studio Mic", "Other Instance"));
        session.saveDefault();
        bool kept = false;
        for (auto& f : session.listBackups())
            kept |= f.getFileName().contains ("changed-elsewhere") && f.loadFileAsString().contains ("Other Instance");
        check (kept, "file changed by someone else is backed up before autosave overwrites it");

        for (int i = 0; i < hydra::Session::kMaxBackups + 5; ++i)
            session.backupDir().getChildFile ("session_2000-01-01_00-00-" + juce::String (i).paddedLeft ('0', 2) + "-000_old.xml").replaceWithText ("x");
        engine.inputs[1].name = "prune";
        session.saveDefault();
        session.backup ("manual");
        check (session.listBackups().size() == hydra::Session::kMaxBackups && session.listBackups()[0].getFileName().contains ("manual"),
               "old backups are pruned, newest kept");
        dir.deleteRecursively();
    }
    {
        // EQ bands: version-1 sessions had all bands on by default. Used bands must survive,
        // flat ones disappear (an EQ without changes shows no points).
        auto check = [&] (bool c, const char* what) { std::printf ("%s %s\n", c ? "ok:  " : "FAIL:", what); if (! c) ++fails; };
        hydra::Engine engine;
        hydra::MidiMapper midi (engine.params);
        hydra::Session session (engine, midi);
        check (! engine.params.get ("in1.eq.b1.on").isOn(), "new EQ: no band in use");
        auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("hydra_eq_v1_test.xml");
        f.replaceWithText ("<HYDRASESSION version=\"1\"><PARAMS in1_eq_b2_gain=\"4.5\" in1_eq_b5_type=\"4\" in1_eq_b6_on=\"0\"/></HYDRASESSION>");
        session.load (f);
        f.deleteFile();
        auto on = [&] (int b) { return engine.params.get ("in1.eq.b" + juce::String (b) + ".on").isOn(); };
        check (on (2) && on (5), "v1 session: boosted bell and low cut stay in use");
        check (! on (1) && ! on (3) && ! on (6), "v1 session: flat bands / switched-off bands are hidden");
        check (! on (7) && ! on (8), "v1 session: the new bands 7 / 8 start unused");
    }
    return fails;
}

#include "state/VoicemeeterImport.h"

int runVoicemeeterImportTest()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    hydra::ParamPoller poller;
    auto f = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("VM26.xml");
    if (! f.existsAsFile()) { std::printf ("skip: no VM26.xml\n"); return 0; }
    hydra::Engine engine;
    hydra::MidiMapper midi (engine.params);
    auto r = hydra::VoicemeeterImport::run (f, engine, midi, false);
    for (auto& line : r.report) std::printf ("  | %s\n", line.toRawUTF8());
    int fails = 0;
    auto check = [&] (bool c, const char* what) { std::printf ("%s %s\n", c ? "ok:  " : "FAIL:", what); if (! c) ++fails; };
    check (r.ok, "import ran");
    check (engine.inputs[0].name == "Mic", "strip 1 named Mic");
    check (engine.inputs[0].source.kind == hydra::IoTarget::Kind::Master && engine.inputs[0].source.mono && engine.inputs[0].source.channel == 0, "mic = ASIO input 1 mono");
    check (! engine.params.get ("in1.mono").isOn(), "strip mono button not used for the mic");
    check (std::abs (engine.params.get ("in1.comp.thr").get() + 14.64f) < 0.05f, "comp threshold -14.64");
    check (std::abs (engine.params.get ("in1.comp.makeup").get() - 3.02f) < 0.05f, "comp makeup 3.02");
    check (std::abs (engine.params.get ("in1.eq.b1.gain").get() - 5.5f) < 0.05f, "strip 1 EQ band 1 +5.5 dB");
    check (std::abs (engine.params.get ("bus.A1.eq.b2.q").get() - 68.4f) < 0.5f, "bus A1 EQ narrow Q 68.4");
    check (engine.params.get ("in1.route.B1").isOn() && ! engine.params.get ("in1.route.A1").isOn(), "strip 1 routing A2+B1..B3");
    check (engine.buses[1].output.kind == hydra::IoTarget::Kind::Master && engine.buses[1].output.channel == 2, "A2 = ASIO out 3/4");
    check (midi.getMappings().size() == 9, "9 MIDI fader mappings");
    return fails;
}

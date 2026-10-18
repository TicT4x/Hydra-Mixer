// Recorder (bus -> WAV -> playback with resampling), SEL / monitor logic, persistence of the new settings.
#include "engine/Engine.h"
#include "state/Session.h"
#include "state/UndoHistory.h"
#include <algorithm>
#include <cstdio>

int runRecorderTest()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    hydra::ParamPoller poller;
    int fails = 0;
    auto check = [&] (bool c, const char* what) { std::printf ("%s %s\n", c ? "ok:  " : "FAIL:", what); if (! c) ++fails; };
    auto pump = [] (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); };

    std::printf ("\n--- recorder / SEL / monitor ---\n");
    hydra::Engine engine;
    auto& rec = engine.recorder;
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("HydraRecorderTest");
    dir.deleteRecursively();
    rec.folder = dir;
    rec.setSampleRate (48000.0);

    // 1. Record 1 s of a -6 dBFS 1 kHz sine (as the audio thread would, in 256-frame blocks).
    rec.record->set (1.0f);
    pump (150);
    check (rec.isRecording(), "REC parameter starts a recording");
    std::vector<float> l (256), r (256);
    int phase = 0;
    for (int block = 0; block < 48000 / 256 + 1; ++block)
    {
        const int n = juce::jmin (256, 48000 - block * 256);
        if (n <= 0) break;
        for (int i = 0; i < n; ++i, ++phase)
            l[(size_t) i] = r[(size_t) i] = 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * phase / 48000.0);
        rec.pushRecord (l.data(), r.data(), n);
    }
    rec.record->set (0.0f);
    pump (150);
    check (! rec.isRecording(), "REC off stops the recording");

    const auto file = rec.getLastRecording();
    check (file.existsAsFile() && file.getFileName().startsWith ("Hydra_B1_"), "WAV written to the recording folder (named after the bus)");
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
        check (reader != nullptr && reader->lengthInSamples == 48000 && reader->numChannels == 2 && reader->bitsPerSample == 24,
               "WAV is 48000 frames, stereo, 24 bit");
        if (reader != nullptr)
        {
            juce::AudioBuffer<float> b (2, 48000);
            reader->read (&b, 0, 48000, 0, true, true);
            std::printf ("info: recorded peak %.4f\n", b.getMagnitude (0, 48000));
            check (std::abs (b.getMagnitude (0, 48000) - 0.5f) < 0.001f, "recorded level matches (-6 dBFS)");
        }
    }
    check (rec.getLoadedFile() == file, "the new take is loaded for playback");

    // 2. Play it back at another engine rate (44.1 kHz) -> resampled, same duration.
    rec.setSampleRate (44100.0);
    rec.play->set (1.0f);
    pump (100);
    check (rec.isPlaying(), "PLAY parameter starts playback");
    int produced = 0, nonSilent = 0;
    float peak = 0.0f;
    for (int i = 0; i < 2000 && rec.isPlaying(); ++i)
    {
        if (! rec.pullPlayback (l.data(), r.data(), 256)) break;
        produced += 256;
        for (int k = 0; k < 256; ++k)
        {
            peak = juce::jmax (peak, std::abs (l[(size_t) k]));
            if (std::abs (l[(size_t) k]) > 1.0e-4f) ++nonSilent;
        }
        pump (2); // let the timer see the end; the producer thread keeps the ring filled
    }
    std::printf ("info: played %d non-silent frames at 44.1 kHz (expected ~44100), peak %.3f\n", nonSilent, peak);
    check (std::abs (nonSilent - 44100) < 600, "playback length correct after resampling 48k -> 44.1k");
    check (std::abs (peak - 0.5f) < 0.02f, "playback level correct");
    pump (100);
    check (! rec.isPlaying() && ! rec.play->isOn(), "PLAY switches itself off at the end of the file");

    // 2b. Waveform overview and seeking (click into the waveform).
    {
        const auto& wave = rec.getPlayWaveform();
        check (std::abs (wave.getSeconds() - 1.0) < 0.01, "waveform of the take covers its length");
        float cols[4] {};
        wave.getColumns (0.0, 1.0, cols, 4);
        check (std::abs (cols[0] - 0.5f) < 0.01f && std::abs (cols[3] - 0.5f) < 0.01f, "waveform shows the recorded level");

        auto playCount = [&]
        {
            int frames = 0;
            rec.play->set (1.0f);
            pump (100);
            for (int i = 0; i < 2000 && rec.isPlaying(); ++i)
            {
                if (! rec.pullPlayback (l.data(), r.data(), 256)) break;
                for (int k = 0; k < 256; ++k) if (std::abs (l[(size_t) k]) > 1.0e-4f) ++frames;
                pump (2);
            }
            pump (100);
            return frames;
        };
        rec.seek (0.5);
        check (std::abs (rec.getStartSeconds() - 0.5) < 1.0e-6, "seek while stopped sets the start position");
        const int half = playCount();
        std::printf ("info: played %d frames from 0.5 s (expected ~22050)\n", half);
        check (std::abs (half - 22050) < 600, "PLAY starts at the seek position");

        // Seek during playback: jump to 0.9 s, about 0.1 s remains.
        rec.seek (0.0);
        rec.play->set (1.0f);
        pump (100);
        for (int i = 0; i < 20; ++i) { rec.pullPlayback (l.data(), r.data(), 256); pump (2); }
        rec.seek (0.9);
        pump (50);
        int rest = 0;
        for (int i = 0; i < 2000 && rec.isPlaying(); ++i)
        {
            if (! rec.pullPlayback (l.data(), r.data(), 256)) break;
            for (int k = 0; k < 256; ++k) if (std::abs (l[(size_t) k]) > 1.0e-4f) ++rest;
            pump (2);
        }
        pump (100);
        std::printf ("info: after seeking to 0.9 s: %d frames (expected ~4410)\n", rest);
        check (std::abs (rest - 4410) < 700, "seek during playback jumps there");

        // A file that was not just recorded is scanned in the background.
        const auto copy = dir.getChildFile ("copy.wav");
        file.copyFileTo (copy);
        juce::String err;
        check (rec.loadFile (copy, err), "load another file");
        for (int i = 0; i < 100 && rec.getPlayWaveform().getSeconds() < 0.99; ++i) pump (20);
        check (std::abs (rec.getPlayWaveform().getSeconds() - 1.0) < 0.01 && rec.getStartSeconds() == 0.0,
               "loaded file: waveform built in the background, start at 0");
        rec.loadFile (file, err);
        rec.setSampleRate (48000.0);
    }

    // 2c. Pause keeps the position, STOP returns to the start, delete removes the take and its file.
    {
        juce::String err;
        rec.seek (0.0);
        rec.play->set (1.0f);
        pump (100);
        for (int i = 0; i < 40; ++i) { rec.pullPlayback (l.data(), r.data(), 256); pump (2); }
        const double heard = rec.getPlaySeconds();   // what was actually played (the ring may start short)
        rec.play->set (0.0f);
        pump (100);
        const double paused = rec.getStartSeconds();
        std::printf ("info: paused at %.3f s (played %.3f s)\n", paused, heard);
        check (! rec.isPlaying() && heard > 0.1 && std::abs (paused - heard) < 0.001, "PLAY off pauses at the current position");
        rec.play->set (1.0f);
        pump (100);
        check (rec.isPlaying() && std::abs (rec.getPlaySeconds() - paused) < 0.005, "PLAY again continues where it paused");
        rec.stop->set (1.0f);
        pump (100);
        check (! rec.isPlaying() && ! rec.play->isOn() && ! rec.stop->isOn() && rec.getStartSeconds() == 0.0,
               "STOP ends playback, resets itself and returns to the start");

        const auto victim = dir.getChildFile ("victim.wav");
        file.copyFileTo (victim);
        rec.loadFile (victim, err);
        rec.play->set (1.0f);
        pump (100);
        check (rec.deleteLoadedFile (err) && ! victim.existsAsFile(), "delete removes the file (also while playing)");
        pump (100);
        check (rec.getLoadedFile() == juce::File() && rec.getPlayWaveform().getSeconds() == 0.0 && ! rec.isPlaying() && ! rec.play->isOn(),
               "delete unloads the take and stops playback");
        check (! rec.deleteLoadedFile (err), "nothing left to delete");
        rec.loadFile (file, err);
    }

    // 2d. Trim: playback only between the handles; Cut writes the shorter file (the original to the bin).
    {
        juce::String err;
        const auto clip = dir.getChildFile ("clip.wav");
        file.copyFileTo (clip);
        rec.loadFile (clip, err);
        rec.setTrim (0.2, 0.7);
        check (rec.isTrimmed() && std::abs (rec.getTrimIn() - 0.2) < 1e-6 && std::abs (rec.getTrimOut() - 0.7) < 1e-6
                   && rec.getStartSeconds() >= 0.2 - 1e-9,
               "trim sets in / out and moves the start marker inside");
        rec.play->set (1.0f);
        pump (100);
        int frames = 0;
        for (int i = 0; i < 4000 && rec.isPlaying(); ++i)
        {
            if (! rec.pullPlayback (l.data(), r.data(), 256)) break;
            for (int k = 0; k < 256; ++k) if (std::abs (l[(size_t) k]) > 1.0e-4f) ++frames;
            pump (2);
        }
        pump (100);
        std::printf ("info: trimmed playback %d frames (expected ~24000)\n", frames);
        check (std::abs (frames - 24000) < 2400 && ! rec.isPlaying(), "playback runs from the in to the out point and stops");
        check (rec.getStartSeconds() >= 0.2 - 1e-9, "after the end, the next play starts at the in point");
        check (rec.applyTrim (err) && std::abs (rec.getPlayLengthSeconds() - 0.5) < 0.01 && ! rec.isTrimmed() && rec.getLoadedFile() == clip,
               "cut writes the trimmed file in place and loads it");
        rec.loadFile (file, err);
    }

    // 3. SEL is exclusive; monitoring only when enabled and not onto the selected bus itself.
    engine.buses[(size_t) hydra::virtBusIndex (0)].sel->set (1.0f);   // B1
    engine.enforceExclusiveSelect();
    engine.buses[(size_t) hydra::virtBusIndex (1)].sel->set (1.0f);   // B2 pressed later
    engine.enforceExclusiveSelect();
    check (! engine.buses[(size_t) hydra::virtBusIndex (0)].sel->isOn() && engine.buses[(size_t) hydra::virtBusIndex (1)].sel->isOn() && engine.getSelectedBus() == hydra::virtBusIndex (1), "SEL is exclusive (newest wins)");
    check (engine.getMonitorBus() == -1, "no monitoring while it is disabled");
    engine.monOn->set (1.0f);
    engine.monBus->set (1.0f);         // A2
    check (engine.getMonitorBus() == 1, "monitoring B2 on A2");
    engine.buses[(size_t) hydra::virtBusIndex (1)].sel->set (0.0f);
    engine.enforceExclusiveSelect();
    check (engine.getMonitorBus() == -1 && engine.getSelectedBus() == -1, "no monitoring without a selected bus");
    engine.buses[1].sel->set (1.0f);   // select A2 = the monitor bus itself
    check (engine.getMonitorBus() == -1, "selecting the monitor bus itself does not monitor");

    // MIDI mapped to an input's main fader drives its send while a bus is selected.
    check (engine.faderTarget (engine.inputs[2].gain) == engine.inputs[2].send[1], "SEL A2: main fader mapping drives the send to A2");
    check (engine.faderTarget (engine.inputs[2].mute) == engine.inputs[2].mute, "other parameters are not redirected");
    engine.buses[1].sel->set (0.0f);
    check (engine.faderTarget (engine.inputs[2].gain) == engine.inputs[2].gain, "without SEL the main fader is driven");

    // 3b. Channel layout: slots are added / removed, removed ones are reset and never processed.
    {
        check (engine.numActiveInputs() == hydra::kDefaultInputs && engine.numActiveBuses (false) == hydra::kDefaultHwBuses
               && engine.numActiveBuses (true) == hydra::kDefaultVirtBuses, "default layout 8 inputs / A1-A5 / B1-B3");
        const auto v0 = engine.getLayoutVersion();
        const int in9 = engine.addInput();
        check (in9 == 8 && engine.isInputActive (8) && engine.getLayoutVersion() != v0, "add input -> In 9");
        const int b4 = engine.addBus (true);
        check (b4 == hydra::virtBusIndex (3) && hydra::busCode (b4) == "B4", "add virtual bus -> B4");
        const int a6 = engine.addBus (false);
        check (a6 == hydra::hwBusIndex (5) && hydra::busCode (a6) == "A6", "add hardware bus -> A6");

        engine.params.get ("in9.gain").set (-12.0f);
        engine.params.get ("in2.route.B4").set (1.0f);
        engine.buses[(size_t) b4].sel->set (1.0f);
        engine.enforceExclusiveSelect();
        auto resetIn = engine.removeInput (in9);
        check (! engine.isInputActive (8) && engine.params.get ("in9.gain").get() == 0.0f && ! resetIn.empty(), "remove input resets it");
        auto resetBus = engine.removeBus (b4);
        check (! engine.isBusActive (b4) && ! engine.params.get ("in2.route.B4").isOn() && engine.getSelectedBus() == -1,
               "remove bus resets the routing to it and its SEL");
        check (std::find (resetBus.begin(), resetBus.end(), &engine.params.get ("in2.route.B4")) != resetBus.end(),
               "removed bus reports the routing parameters (for MIDI / shortcut cleanup)");
        engine.removeBus (a6);
        check (engine.addInput() == 8, "re-added input reuses the free slot");
        engine.removeInput (8);

        for (int b = 1; b < hydra::kMaxHwBuses; ++b) engine.removeBus (hydra::hwBusIndex (b));
        check (engine.removeBus (hydra::hwBusIndex (0)).empty() && engine.isBusActive (0), "the last A bus cannot be removed");
        for (int b = 1; b < hydra::kDefaultHwBuses; ++b) engine.buses[(size_t) b].active->set (1.0f);
    }

    // 3c. Undo / redo (Ctrl+Z / Ctrl+Y) across the EQ and other controls: one step per settled change.
    {
        hydra::UndoHistory history (engine.params);
        auto& eq = engine.inputs[0].eq;
        eq.bandOn[0]->set (1.0f);
        eq.gain[0]->set (6.0f);
        pump (600);
        engine.inputs[1].gain->set (-10.0f);
        pump (600);
        check (history.undo().isNotEmpty() && engine.inputs[1].gain->get() == 0.0f && eq.gain[0]->get() == 6.0f, "undo: fader of IN 2 first");
        check (history.undo().isNotEmpty() && eq.gain[0]->get() == 0.0f && ! eq.bandOn[0]->isOn(), "undo: then the EQ band of IN 1");
        check (history.undo().isEmpty(), "undo: nothing left");
        check (history.redo().isNotEmpty() && eq.gain[0]->get() == 6.0f, "redo of the EQ step");
        check (history.redo().isNotEmpty() && engine.inputs[1].gain->get() == -10.0f, "redo of the fader step");
        eq.gain[1]->set (2.0f);   // not settled yet: undo still reverts it as its own step
        check (history.undo().isNotEmpty() && eq.gain[1]->get() == 0.0f && eq.gain[0]->get() == 6.0f, "undo of an unsettled change");

        // Removing a channel: its parameters leave the history.
        const int in9 = engine.addInput();
        hydra::UndoHistory removal (engine.params);
        engine.inputs[(size_t) in9].gain->set (-20.0f);
        pump (600);
        removal.forget (engine.removeInput (in9));
        check (removal.undo().isEmpty() && engine.inputs[(size_t) in9].gain->get() == 0.0f, "removed channel: no undo into its reset parameters");
        for (auto& p : engine.params.all()) if (p->id.startsWith ("in1.eq.")) p->reset();
        engine.inputs[1].gain->reset();
    }

    // 4. Persistence: transport is never stored, prefs and recorder settings are.
    {
        hydra::MidiMapper midi (engine.params);
        hydra::Session session (engine, midi, dir);
        session.prefs.closeToTray = true;
        rec.bitDepth = 32;
        rec.record->set (1.0f); // pretend the app quits while recording
        const auto f = dir.getChildFile ("persist.xml");
        check (session.save (f), "session saved");
        auto xml = juce::XmlDocument::parse (f);
        auto* params = xml != nullptr ? xml->getChildByName ("PARAMS") : nullptr;
        check (params != nullptr && ! params->hasAttribute ("rec_record") && params->hasAttribute ("in1_send_A1") == false,
               "REC/PLAY are not stored (and default sends are omitted)");
        auto* app = xml != nullptr ? xml->getChildByName ("APP") : nullptr;
        auto* recXml = xml != nullptr ? xml->getChildByName ("RECORDER") : nullptr;
        check (app != nullptr && app->getBoolAttribute ("closeToTray") && ! app->getBoolAttribute ("startInTray"), "tray preferences stored");
        check (recXml != nullptr && recXml->getIntAttribute ("bits") == 32 && recXml->getStringAttribute ("folder") == dir.getFullPathName(),
               "recorder folder / format stored");
        rec.record->set (0.0f);
        pump (150);
    }

    rec.getLastRecording().deleteFile();
    for (auto& f : dir.findChildFiles (juce::File::findFiles, false, "Hydra_*.wav")) f.deleteFile();
    dir.deleteRecursively();
    return fails;
}

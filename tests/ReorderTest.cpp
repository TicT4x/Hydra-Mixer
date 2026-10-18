// Change Order: channels move with all their settings; MIDI mappings / shortcuts can follow them.
#include "engine/Engine.h"
#include "midi/MidiMapper.h"
#include "midi/Hotkeys.h"
#include <cstdio>

int runReorderTest()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    int fails = 0;
    auto check = [&] (bool c, const char* what) { std::printf ("%s %s\n", c ? "ok:  " : "FAIL:", what); if (! c) ++fails; };
    using G = hydra::Engine::Group;

    std::printf ("\n--- change order ---\n");
    hydra::Engine engine;
    auto& p = engine.params;

    // Inputs: IN 1 "Mic" (fader -6, routed to B1, EQ band on), IN 3 "Music" (mute, source Master 3/4).
    engine.inputs[0].name = "Mic";
    p.get ("in1.gain").set (-6.0f);
    p.get ("in1.route.B1").set (1.0f);
    p.get ("in1.eq.b1.on").set (1.0f);
    engine.inputs[2].name = "Music";
    p.get ("in3.mute").set (1.0f);
    engine.inputs[2].source.kind = hydra::IoTarget::Kind::Master;
    engine.inputs[2].source.channel = 2;

    // Same order / invalid orders do nothing.
    check (engine.reorderPlan (G::inputs, { 0, 1, 2, 3, 4, 5, 6, 7 }).empty(), "unchanged order: no plan");
    check (engine.reorderPlan (G::inputs, { 1, 0, 2 }).empty(), "order missing channels: no plan");
    check (! engine.reorderChannels (G::inputs, { 0, 0, 2, 3, 4, 5, 6, 7 }), "duplicate slots rejected");

    hydra::MidiMapper midi (p);
    hydra::Hotkeys keys (p);
    {
        auto xml = juce::XmlDocument::parse (R"(<MIDI><MAP param="in1.gain" device="" channel="1" source="0" number="7" mode="0" invert="0"/>
                                                    <MAP param="in3.mute" device="" channel="1" source="1" number="40" mode="1" invert="0"/></MIDI>)");
        midi.fromXml (*xml);
    }
    keys.assign (p.get ("in1.mute"), juce::KeyPress ('m', juce::ModifierKeys::ctrlModifier, 0));
    check (midi.getMappings().size() == 2, "two MIDI mappings loaded");

    // Move Music (slot 2) to the front: new order Music, Mic, In 2, In 4..8.
    const std::vector<int> order { 2, 0, 1, 3, 4, 5, 6, 7 };
    const auto plan = engine.reorderPlan (G::inputs, order);
    check (midi.countMappings (plan) == 2 && keys.countBindings (plan) == 1, "both mappings and the shortcut are on moved channels");
    check (engine.reorderChannels (G::inputs, order), "reorder applied");
    check (engine.inputs[0].name == "Music" && engine.inputs[1].name == "Mic" && engine.inputs[2].name == "Input 2", "names moved");
    check (p.get ("in1.mute").isOn() && engine.inputs[0].source.kind == hydra::IoTarget::Kind::Master
           && engine.inputs[0].source.channel == 2, "Music's mute and source moved to IN 1");
    check (p.get ("in2.gain").get() == -6.0f && p.get ("in2.route.B1").isOn() && p.get ("in2.eq.b1.on").isOn(),
           "Mic's fader, routing and EQ moved to IN 2");
    check (p.get ("in3.gain").get() == 0.0f && ! p.get ("in3.mute").isOn() && ! p.get ("in3.route.B1").isOn(), "In 2 moved to IN 3 with its defaults");
    check (engine.isInputActive (0) && engine.isInputActive (7) && ! engine.isInputActive (8), "same slots in use");

    // MIDI follows: CC 7 still drives Mic's fader (now in2.gain), the note still mutes Music (now in1.mute).
    midi.remapParams (plan);
    keys.remapParams (plan);
    bool ccOnMic = false, noteOnMusic = false;
    for (auto& m : midi.getMappings())
    {
        ccOnMic |= m.number == 7 && m.param == &p.get ("in2.gain") && m.paramId == "in2.gain";
        noteOnMusic |= m.number == 40 && m.param == &p.get ("in1.mute");
    }
    check (ccOnMic && noteOnMusic, "MIDI mappings follow their channels");
    check (keys.getKeyFor (&p.get ("in2.mute")).isValid() && ! keys.getKeyFor (&p.get ("in1.mute")).isValid(), "shortcut follows Mic");
    auto saved = midi.toXml();
    check (saved->toString().contains ("in2.gain"), "remapped MIDI mapping is saved with its new parameter");

    // Buses: put B2 before B1. Routing of the inputs, recorder output / source and SEL move along.
    const int b1 = hydra::virtBusIndex (0), b2 = hydra::virtBusIndex (1);
    engine.buses[(size_t) b1].name = "Stream";
    engine.buses[(size_t) b2].name = "Discord";
    p.get ("bus.B2.gain").set (-3.0f);
    p.get ("in1.route.B2").set (1.0f);       // Music -> Discord
    p.get ("in1.send.B2").set (-10.0f);
    engine.recorder.source->set (1.0f);     // records B2 (Discord)
    p.get ("rec.out.B1").set (1.0f);         // playback into Stream
    engine.buses[(size_t) b2].sel->set (1.0f);
    engine.enforceExclusiveSelect();

    const std::vector<int> busOrder { b2, b1, hydra::virtBusIndex (2) };
    const auto busPlan = engine.reorderPlan (G::virtBuses, busOrder);
    check (engine.reorderChannels (G::virtBuses, busOrder), "bus reorder applied");
    check (engine.buses[(size_t) b1].name == "Discord" && engine.buses[(size_t) b2].name == "Stream", "bus names moved");
    check (p.get ("bus.B1.gain").get() == -3.0f && p.get ("bus.B2.gain").get() == 0.0f, "bus fader moved");
    check (p.get ("in1.route.B1").isOn() && p.get ("in1.send.B1").get() == -10.0f, "input routing / send follow the bus");
    check (p.get ("in2.route.B2").isOn() && ! p.get ("in2.route.B1").isOn(), "Mic's routing to Stream follows Stream to B2");
    check (engine.recorder.getRecordSourceBus() == b1 && p.get ("rec.out.B2").isOn() && ! p.get ("rec.out.B1").isOn(),
           "recorder source and output follow their buses");
    engine.enforceExclusiveSelect();
    check (engine.getSelectedBus() == b1, "SEL follows its bus");
    check (engine.reorderPlan (G::hwBuses, { 0, 1, 2, 3, 4 }).empty(), "A buses untouched");

    std::printf ("info: %d parameters moved with the buses\n", (int) busPlan.size());

    // Inputs and buses reordered together: a mapping on a routing button follows both moves.
    {
        auto xml = juce::XmlDocument::parse (R"(<MIDI><MAP param="in1.route.A1" device="" channel="1" source="1" number="50" mode="1" invert="0"/></MIDI>)");
        midi.fromXml (*xml);
        p.get ("in1.route.A1").set (1.0f);
        p.get ("in1.route.A2").set (0.0f);
        const std::vector<int> inOrder { 1, 0, 2, 3, 4, 5, 6, 7 }, aOrder { 1, 0, 2, 3, 4 };
        const auto inPlan = engine.reorderPlan (G::inputs, inOrder);
        engine.reorderChannels (G::inputs, inOrder);
        midi.remapParams (inPlan);
        const auto aPlan = engine.reorderPlan (G::hwBuses, aOrder);
        engine.reorderChannels (G::hwBuses, aOrder);
        midi.remapParams (aPlan);
        const auto maps = midi.getMappings();
        check (maps.size() == 1 && maps[0].paramId == "in2.route.A2" && p.get ("in2.route.A2").isOn(),
               "routing mapping follows its input and its bus");
    }
    return fails;
}

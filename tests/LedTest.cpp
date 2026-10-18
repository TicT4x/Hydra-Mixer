// LED grid colours for an APC mini style controller.
#include "engine/Engine.h"
#include "midi/MidiMapper.h"
#include <cstdio>

int runLedTest()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    hydra::ParamPoller poller;
    int fails = 0;
    auto check = [&] (bool c, const char* what) { std::printf ("%s %s\n", c ? "ok:  " : "FAIL:", what); if (! c) ++fails; };

    hydra::Engine engine;
    hydra::MidiMapper midi (engine.params);

    // Row 0 (notes 0..7): input 1 routes A1 (note 0) and mute (note 7). Row 1 (notes 8..15): bus A1 mute (note 8).
    juce::XmlElement xml ("MIDI");
    auto add = [&] (const char* param, int note)
    {
        auto* e = xml.createNewChildElement ("MAP");
        e->setAttribute ("param", param);
        e->setAttribute ("channel", 1);
        e->setAttribute ("source", 1); // note
        e->setAttribute ("number", note);
        e->setAttribute ("mode", 1);   // toggle
    };
    add ("in1.route.A1", 0);
    add ("in1.mute", 7);
    add ("bus.A1.mute", 8);
    midi.fromXml (xml);
    midi.setLedProfile (hydra::LedProfile::presets()[1]); // APC mini mk1: on 1, off 5, unbound 0, muted 3

    engine.params.get ("in1.route.A1").set (1.0f);
    engine.params.get ("in1.mute").set (0.0f);
    auto leds = midi.computeGridLeds();
    check (leds[0] == 1, "bound + active pad = green (1)");
    check (leds[7] == 5, "bound + inactive pad = yellow (5)");
    check (leds[3] == 0, "unbound pad = off (0)");
    check (leds[63] == 0, "unbound pad in empty row = off");

    engine.params.get ("in1.mute").set (1.0f);
    leds = midi.computeGridLeds();
    check (leds[7] == 1, "mute button itself is active = green");
    check (leds[3] == 3 && leds[1] == 3, "channel muted: unbound pads of its row = red (3)");
    check (leds[9] == 0, "other rows unaffected");

    engine.params.get ("bus.A1.mute").set (1.0f);
    leds = midi.computeGridLeds();
    check (leds[9] == 3 && leds[8] == 1, "bus row: muted bus turns its unbound pads red");
    check (leds[100] == -1, "notes outside the grid are not touched");

    // Column layout (one channel per column, like the APC faders): in1..in3 in columns 0..2,
    // route A1 in row 0, route A2 in row 1, mute in the top row.
    hydra::MidiMapper cols (engine.params);
    juce::XmlElement colXml ("MIDI");
    auto addTo = [&] (juce::XmlElement& x, const juce::String& param, int note)
    {
        auto* e = x.createNewChildElement ("MAP");
        e->setAttribute ("param", param);
        e->setAttribute ("channel", 1);
        e->setAttribute ("source", 1);
        e->setAttribute ("number", note);
        e->setAttribute ("mode", 1);
    };
    for (int c = 0; c < 3; ++c)
    {
        const auto in = "in" + juce::String (c + 1);
        addTo (colXml, in + ".route.A1", c);
        addTo (colXml, in + ".route.A2", 8 + c);
        addTo (colXml, in + ".mute", 56 + c);
    }
    cols.fromXml (colXml);
    cols.setLedProfile (hydra::LedProfile::presets()[1]);
    for (auto* id : { "in1.mute", "in2.mute", "in3.mute", "bus.A1.mute" }) engine.params.get (id).set (0.0f);
    engine.params.get ("in2.mute").set (1.0f);
    leds = cols.computeGridLeds();
    check (leds[17] == 3 && leds[25] == 3 && leds[49] == 3, "column layout: muted channel turns its column red");
    check (leds[16] == 0 && leds[18] == 0 && leds[3] == 0, "column layout: other columns and rows unaffected");
    check (leds[57] == 1, "column layout: the mute pad itself stays green");
    return fails;
}

// MIDI range calibration: mapping math and persistence.
#include "engine/Engine.h"
#include "midi/MidiMapper.h"
#include <cstdio>

int runMidiRangeTest()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    hydra::ParamPoller poller;
    int fails = 0;
    auto check = [&] (bool c, const char* what) { std::printf ("%s %s\n", c ? "ok:  " : "FAIL:", what); if (! c) ++fails; };

    hydra::Engine engine;
    hydra::MidiMapper midi (engine.params);
    auto& gain = engine.params.get ("in1.gain");
    const float zeroDb = gain.range.convertTo0to1 (0.0f);

    juce::XmlElement xml ("MIDI");
    auto* e = xml.createNewChildElement ("MAP");
    e->setAttribute ("param", "in1.gain");
    e->setAttribute ("channel", 1);
    e->setAttribute ("source", 0);
    e->setAttribute ("number", 48);
    e->setAttribute ("mode", 0);
    midi.fromXml (xml);
    midi.setRangeForParam (&gain, 0.0f, zeroDb);

    auto maps = midi.getMappings();
    check (maps.size() == 1 && std::abs (maps[0].maxN - zeroDb) < 1e-6f, "range stored on mapping");

    gain.setNormalised (maps[0].toParam (1.0f));
    check (std::abs (gain.get()) < 0.06f, "MIDI 127 -> 0 dB (not +12 dB)");
    gain.setNormalised (maps[0].toParam (0.0f));
    check (gain.get() <= gain.range.start + 0.01f, "MIDI 0 -> -inf");
    check (std::abs (maps[0].fromParam (zeroDb) - 1.0f) < 1e-5f, "feedback: 0 dB -> controller top");
    std::printf ("info: range text \"%s\"\n", maps[0].rangeText().toRawUTF8());

    auto saved = midi.toXml();
    hydra::MidiMapper midi2 (engine.params);
    midi2.fromXml (*saved);
    auto maps2 = midi2.getMappings();
    check (maps2.size() == 1 && std::abs (maps2[0].maxN - zeroDb) < 1e-5f && maps2[0].minN == 0.0f, "range survives save/load");

    // Settings > MIDI: "faders stop at 0 dB" for every fader mapping at once.
    {
        hydra::MidiMapper m3 (engine.params);
        juce::XmlElement x ("MIDI");
        auto add = [&x] (const char* id, int cc, int mode)
        {
            auto* el = x.createNewChildElement ("MAP");
            el->setAttribute ("param", id); el->setAttribute ("channel", 1); el->setAttribute ("source", 0);
            el->setAttribute ("number", cc); el->setAttribute ("mode", mode);
        };
        add ("in2.gain", 1, 0); add ("bus.A1.gain", 2, 0); add ("in3.send.B1", 3, 0); add ("in2.trim", 4, 0); add ("in2.mute", 5, 1);
        m3.fromXml (x);
        m3.setFaderTopAt0dB (true);
        int capped = 0, untouched = 0;
        for (auto& mm : m3.getMappings())
        {
            const bool fader = hydra::MidiMapper::isFaderParam (*mm.param);
            if (fader && std::abs (mm.maxN - mm.param->range.convertTo0to1 (0.0f)) < 1e-5f) ++capped;
            if (! fader && mm.maxN == 1.0f) ++untouched;
        }
        check (capped == 3 && untouched == 2, "0 dB option caps the channel, bus and send faders only");
        auto again = m3.toXml();
        hydra::MidiMapper m4 (engine.params);
        m4.fromXml (*again);
        check (m4.getFaderTopAt0dB(), "0 dB option survives save/load");
        m3.setFaderTopAt0dB (false);
        bool restored = true;
        for (auto& mm : m3.getMappings()) restored = restored && mm.maxN == 1.0f;
        check (restored, "switching the option off returns the faders to +12 dB");
    }

    // APC mini answers the introduction message with its fader positions: they drive the mapped faders.
    {
        hydra::MidiMapper m5 (engine.params);
        juce::XmlElement x ("MIDI");
        auto* el = x.createNewChildElement ("MAP");
        el->setAttribute ("param", "in4.gain"); el->setAttribute ("channel", 1); el->setAttribute ("source", 0);
        el->setAttribute ("number", 49); el->setAttribute ("mode", 0);
        m5.fromXml (x);
        auto& g4 = engine.params.get ("in4.gain");
        g4.setNormalised (0.0f);
        const juce::uint8 reply[] = { 0xf0, 0x47, 0x7f, 0x28, 0x61, 0x00, 0x09, 10, 127, 20, 30, 40, 50, 60, 70, 80, 0xf7 };
        const bool handled = m5.handleStateReply (nullptr, juce::MidiMessage (reply, (int) sizeof (reply)));
        check (handled && std::abs (g4.getNormalised() - 1.0f) < 1e-4f, "APC mini fader reply sets the mapped fader (2nd fader = 127)");
        const juce::uint8 other[] = { 0xf0, 0x47, 0x7f, 0x28, 0x62, 0x00, 0x01, 5, 0xf7 };
        check (! m5.handleStateReply (nullptr, juce::MidiMessage (other, (int) sizeof (other))), "other SysEx is ignored");
    }
    return fails;
}

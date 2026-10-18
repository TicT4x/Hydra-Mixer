#include "VoicemeeterImport.h"

#include <windows.h>

namespace hydra
{
namespace
{
constexpr int kVmHwStrips = 5; // Potato: 5 hardware strips + 3 virtual strips
constexpr int kVmStrips = 8;
constexpr int kVmHwBuses = 5;  // Potato: A1-A5 + B1-B3
constexpr int kVmBuses = 8;

/** Hydra bus index of Voicemeeter bus b (1..8: A1-A5, B1-B3). */
int vmBus (int b) { return b <= kVmHwBuses ? hwBusIndex (b - 1) : virtBusIndex (b - 1 - kVmHwBuses); }

/** Voicemeeter stores the ASIO driver as its COM CLSID; JUCE lists drivers by their registry key name. */
juce::String asioNameFromClsid (const juce::String& clsid)
{
    HKEY root = nullptr;
    if (RegOpenKeyExW (HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ | KEY_WOW64_64KEY, &root) != ERROR_SUCCESS)
        return {};

    juce::String found;
    for (DWORD i = 0;; ++i)
    {
        wchar_t name[256];
        DWORD len = 256;
        if (RegEnumKeyExW (root, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;

        wchar_t value[128] {};
        DWORD size = sizeof (value);
        if (RegGetValueW (root, name, L"CLSID", RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS
            && juce::String (value).equalsIgnoreCase (clsid))
        {
            found = juce::String (name);
            break;
        }
    }
    RegCloseKey (root);
    return found;
}

float attrF (const juce::XmlElement* e, const char* name, float def = 0.0f)
{
    return e != nullptr && e->hasAttribute (name) ? (float) e->getDoubleAttribute (name) : def;
}

bool attrB (const juce::XmlElement* e, const char* name)
{
    return e != nullptr && e->getIntAttribute (name) != 0;
}

/** First <tag index='i'> element whose attribute `marker` exists (Voicemeeter repeats tags with different attributes). */
const juce::XmlElement* findIndexed (const juce::XmlElement& parent, const juce::String& tag, int index, const char* marker)
{
    for (auto* e : parent.getChildWithTagNameIterator (tag))
        if (e->getIntAttribute ("index") == index && (marker == nullptr || e->hasAttribute (marker)))
            return e;
    return nullptr;
}

void setP (Engine& eng, const juce::String& id, float v)
{
    if (auto* p = eng.params.find (id)) p->set (v);
}

/** Maps a Voicemeeter MIDI control id ("InGainFader1", "Strip3B2", ...) to a Hydra parameter id. */
juce::String mapMidiId (const juce::String& id, bool& isSwitch)
{
    isSwitch = false;
    auto num = [&] (const juce::String& prefix) { return id.fromFirstOccurrenceOf (prefix, false, false).getIntValue(); };
    auto in = [] (int n) { return "in" + juce::String (n) + "."; };

    if (id.startsWith ("InGainFader"))   return in (num ("InGainFader")) + "gain";
    if (id.startsWith ("VInGainFader"))  return in (kVmHwStrips + num ("VInGainFader")) + "gain";
    if (id.startsWith ("OutGainFader"))  return "bus.A" + juce::String (num ("OutGainFader")) + ".gain";
    if (id.startsWith ("VOutGainFader")) return "bus.B" + juce::String (num ("VOutGainFader")) + ".gain";
    if (id.startsWith ("LRPanIn"))       return in (num ("LRPanIn")) + "pan";
    if (id.startsWith ("LRPanVIn"))      return in (kVmHwStrips + num ("LRPanVIn")) + "pan";
    if (id.startsWith ("DenoiserIn"))    return in (num ("DenoiserIn")) + "dn.amount";

    isSwitch = true;
    if (id.startsWith ("InMute"))   return in (num ("InMute")) + "mute";
    if (id.startsWith ("VInMute") && ! id.startsWith ("VInMuteC")) return in (kVmHwStrips + num ("VInMute")) + "mute";
    if (id.startsWith ("InSolo"))   return in (num ("InSolo")) + "solo";
    if (id.startsWith ("VInSolo"))  return in (kVmHwStrips + num ("VInSolo")) + "solo";
    if (id.startsWith ("OutMute"))  return "bus.A" + juce::String (num ("OutMute")) + ".mute";
    if (id.startsWith ("VOutMute")) return "bus.B" + juce::String (num ("VOutMute")) + ".mute";
    if (id.startsWith ("OutEqOn"))  return "bus.A" + juce::String (num ("OutEqOn")) + ".eq.on";
    if (id.startsWith ("VOutEqOn")) return "bus.B" + juce::String (num ("VOutEqOn")) + ".eq.on";
    if (id.startsWith ("Strip"))    // Strip<n>A<k> / Strip<n>B<k>
    {
        const auto rest = id.substring (5);
        const int n = rest.getIntValue();
        const auto bus = rest.trimCharactersAtStart ("0123456789");
        if (n > 0 && bus.length() == 2 && (bus[0] == 'A' || bus[0] == 'B'))
            return in (n) + "route." + bus;
    }
    return {};
}
} // namespace

juce::Array<juce::File> VoicemeeterImport::findCandidates()
{
    juce::Array<juce::File> files;
    auto docs = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
    for (auto& f : docs.findChildFiles (juce::File::findFiles, false, "*.xml"))
        if (f.getSize() < 4 * 1024 * 1024 && f.loadFileAsString().contains ("VBAudioVoicemeeterSettings"))
            files.add (f);
    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b)
               { return a.getLastModificationTime() > b.getLastModificationTime(); });
    return files;
}

VoicemeeterImport::Result VoicemeeterImport::run (const juce::File& file, Engine& eng, MidiMapper& midi, bool applyDevices)
{
    Result res;
    auto& rep = res.report;
    auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr || ! xml->hasTagName ("VBAudioVoicemeeterSettings"))
    {
        rep.add ("Not a Voicemeeter settings file: " + file.getFileName());
        return res;
    }

    auto* devCfg = xml->getChildByName ("VoiceMeeterDeviceConfiguration");
    auto* params = xml->getChildByName ("VoiceMeeterParameters");
    if (devCfg == nullptr || params == nullptr)
    {
        rep.add ("File has no device configuration / parameters section.");
        return res;
    }

    // Start from a clean state so nothing from the previous Hydra session lingers.
    for (auto& p : eng.params.all()) p->reset();
    // Voicemeeter Potato's layout (8 inputs, A1-A5, B1-B3); the other slots are cleared.
    for (int i = kVmStrips; i < kMaxInputs; ++i)
    {
        auto& st = eng.inputs[(size_t) i];
        st.name = "Input " + juce::String (i + 1);
        st.source = {};
        st.plugins.clear();
    }
    for (int b = 0; b < kMaxBuses; ++b)
        if (! eng.buses[(size_t) b].active->isOn())
        {
            eng.buses[(size_t) b].name = busCode (b);
            eng.buses[(size_t) b].output = {};
            eng.buses[(size_t) b].plugins.clear();
        }

    //--------------------------------------------------------------------------
    // Devices
    const auto* out1 = findIndexed (*devCfg, "OutputDev", 1, nullptr);
    const bool asio = out1 != nullptr && out1->getIntAttribute ("type") == 256;
    MasterConfig master;
    if (asio)
    {
        const auto driver = asioNameFromClsid (out1->getStringAttribute ("name"));
        master.typeName = "ASIO";
        master.deviceName = driver;
        if (const int bs = devCfg->getChildByName ("OptionDev") != nullptr ? devCfg->getChildByName ("OptionDev")->getIntAttribute ("asio") : 0; bs > 0)
            master.bufferSize = bs;
        rep.add ("A1 / master clock: ASIO \"" + (driver.isNotEmpty() ? driver : juce::String ("unknown driver")) + "\"");
    }
    else if (out1 != nullptr && out1->getStringAttribute ("name") != "-")
    {
        master.deviceName = out1->getStringAttribute ("name");
        rep.add ("A1 / master clock: " + master.deviceName + " (Windows Audio, low latency)");
    }

    auto wdmTarget = [] (const juce::String& name, bool mono)
    {
        IoTarget t;
        t.kind = IoTarget::Kind::Device;
        t.typeName = "Windows Audio (Low Latency Mode)";
        t.deviceName = name;
        t.mono = mono;
        return t;
    };

    //--------------------------------------------------------------------------
    // Input strips
    for (int s = 1; s <= kVmStrips; ++s)
    {
        auto& strip = eng.inputs[(size_t) (s - 1)];
        const auto pre = "in" + juce::String (s) + ".";
        const bool hw = s <= kVmHwStrips;

        const auto label = hw ? params->getChildElementAllSubText ("LabelStrip" + juce::String (s), {})
                              : params->getChildElementAllSubText ("LabelVirtualStrip" + juce::String (s - kVmHwStrips), {});
        strip.name = label.isNotEmpty() ? label : "Input " + juce::String (s);

        const auto* st = findIndexed (*params, "Strip", s, "mute");
        bool mono = attrB (st, "mono");

        IoTarget src;
        if (hw)
        {
            const int pl = asio && out1 != nullptr ? out1->getIntAttribute ("patch" + juce::String (2 * s - 1)) : 0;
            const int pr = asio && out1 != nullptr ? out1->getIntAttribute ("patch" + juce::String (2 * s)) : 0;
            const auto* idev = findIndexed (*devCfg, "InputDev", s, nullptr);
            if (pl > 0)
            {
                src.kind = IoTarget::Kind::Master;
                src.channel = pl - 1;
                // Voicemeeter "mono" on an ASIO pair: take the left channel at full level instead of
                // averaging it with a (usually silent) second input.
                if (mono || pr <= 0) { src.mono = true; mono = false; }
                rep.add ("In " + juce::String (s) + " \"" + strip.name + "\": ASIO input " + juce::String (pl)
                         + (src.mono ? " (mono)" : " + " + juce::String (pr)));
            }
            else if (idev != nullptr && idev->getStringAttribute ("name") != "-" && idev->getStringAttribute ("name").isNotEmpty())
            {
                src = wdmTarget (idev->getStringAttribute ("name"), mono);
                if (mono) mono = false;
                rep.add ("In " + juce::String (s) + " \"" + strip.name + "\": " + src.deviceName + (src.mono ? " (mono)" : ""));
            }
            else
                rep.add ("In " + juce::String (s) + " \"" + strip.name + "\": no device");
        }
        else
        {
            src.kind = IoTarget::Kind::Virtual;
            src.virtualIndex = s - 1;
            rep.add ("In " + juce::String (s) + " \"" + strip.name + "\": Hydra virtual input " + juce::String (s)
                     + " (was a Voicemeeter virtual input; needs the Hydra driver)");
        }
        strip.source = src;

        if (st != nullptr)
        {
            setP (eng, pre + "mute", attrF (st, "mute"));
            setP (eng, pre + "solo", attrF (st, "solo"));
            setP (eng, pre + "mono", mono ? 1.0f : 0.0f);
            setP (eng, pre + "gain", attrF (st, "dblevel"));
            setP (eng, pre + "eq.on", attrF (st, "EQon"));
            static const char* busAttr[kVmBuses] = { "busa", "busa2", "busa3", "busa4", "busa5", "busb", "busb2", "busb3" };
            for (int b = 1; b <= kVmBuses; ++b)
                setP (eng, pre + "route." + busCode (vmBus (b)), attrF (st, busAttr[b - 1]));
        }

        const auto* pan = findIndexed (*params, "Strip", s, "paneltype");
        const bool compOn = attrB (st, "prg_c") || attrF (pan, "audibility_c") > 0.0f;
        const bool gateOn = attrB (st, "prg_g") || attrF (pan, "audibility_g") > 0.0f;
        setP (eng, pre + "comp.on", compOn ? 1.0f : 0.0f);
        setP (eng, pre + "gate.on", gateOn ? 1.0f : 0.0f);

        if (const auto* c = findIndexed (*params, "StripComp", s, nullptr))
        {
            setP (eng, pre + "comp.input", attrF (c, "gainin"));
            setP (eng, pre + "comp.thr", attrF (c, "threshold", -20.0f));
            setP (eng, pre + "comp.ratio", attrF (c, "comprate", 3.0f));
            setP (eng, pre + "comp.knee", attrF (c, "knee", 0.5f) * 12.0f); // Voicemeeter knee is 0..1
            setP (eng, pre + "comp.attack", attrF (c, "attack", 5.0f));
            setP (eng, pre + "comp.release", attrF (c, "release", 150.0f));
            setP (eng, pre + "comp.makeup", attrF (c, "gainout"));
            setP (eng, pre + "comp.auto", attrF (c, "automakeup"));
        }
        if (const auto* g = findIndexed (*params, "StripGate", s, nullptr))
        {
            setP (eng, pre + "gate.thr", attrF (g, "thresin", -50.0f));
            setP (eng, pre + "gate.range", attrF (g, "damping", -80.0f));
            setP (eng, pre + "gate.attack", attrF (g, "attack", 1.0f));
            setP (eng, pre + "gate.hold", attrF (g, "hold", 80.0f));
            setP (eng, pre + "gate.release", attrF (g, "release", 120.0f));
            const float sc = attrF (g, "bpsidechain", 100.0f);
            setP (eng, pre + "gate.sc.on", sc > 100.5f ? 1.0f : 0.0f); // 100 Hz = Voicemeeter's "off" position
            setP (eng, pre + "gate.sc.freq", sc);
        }
        if (const float dn = attrF (pan, "denoiser_g"); dn > 0.0f)
        {
            setP (eng, pre + "dn.on", 1.0f);
            setP (eng, pre + "dn.amount", juce::jmin (100.0f, dn * 10.0f));
        }
    }

    //--------------------------------------------------------------------------
    // EQ cells (strips and buses use the same 6-band layout; channel 1 is taken)
    auto importEq = [&] (const char* section, const char* tag, std::function<juce::String (int)> prefixFor, int count)
    {
        const juce::XmlElement* sec = nullptr;
        for (auto* top : xml->getChildIterator())
            if ((sec = top->hasTagName (section) ? top : top->getChildByName (section)) != nullptr)
                break;
        if (sec == nullptr) { rep.add (juce::String ("EQ section not found: ") + section); return; }
        for (auto* e : sec->getChildWithTagNameIterator (tag))
        {
            const int idx = e->getIntAttribute ("index");
            const int cell = e->getIntAttribute ("cell");
            if (e->getIntAttribute ("channel") != 1 || idx < 1 || idx > count || cell < 1 || cell > ParamEq::kBands)
                continue;
            const auto bp = prefixFor (idx) + "eq.b" + juce::String (cell) + ".";
            setP (eng, bp + "on", attrF (e, "EQon"));
            setP (eng, bp + "type", (float) juce::jlimit (0, ParamEq::NumTypes - 1, e->getIntAttribute ("EQtype")));
            setP (eng, bp + "gain", attrF (e, "dblevel"));
            setP (eng, bp + "freq", attrF (e, "freq", 1000.0f));
            setP (eng, bp + "q", attrF (e, "Q", 1.0f));
        }
    };
    importEq ("VoiceMeeterStripEQ", "Strip", [] (int i) { return "in" + juce::String (i) + "."; }, kVmStrips);
    importEq ("VoiceMeeterBUSEQ", "Bus", [] (int i) { return "bus." + busCode (vmBus (i)) + "."; }, kVmBuses);
    for (auto& st : eng.inputs) st.eq.hideNeutralBands();
    for (auto& bu : eng.buses)  bu.eq.hideNeutralBands();

    //--------------------------------------------------------------------------
    // Buses
    for (int b = 1; b <= kVmBuses; ++b)
    {
        const int hb = vmBus (b);
        auto& bus = eng.buses[(size_t) hb];
        const auto pre = "bus." + busCode (hb) + ".";
        const bool hw = b <= kVmHwBuses;
        const auto label = hw ? params->getChildElementAllSubText ("LabelBus" + juce::String (b), {})
                              : params->getChildElementAllSubText ("LabelVirtualBus" + juce::String (b - kVmHwBuses), {});
        bus.name = label.isNotEmpty() ? label : busCode (hb);

        if (const auto* e = findIndexed (*params, "Bus", b, "mute"))
        {
            setP (eng, pre + "mute", attrF (e, "mute"));
            setP (eng, pre + "mono", attrF (e, "mono"));
            setP (eng, pre + "gain", attrF (e, "dblevel"));
            setP (eng, pre + "eq.on", attrF (e, "EQon"));
        }

        IoTarget out;
        if (hw)
        {
            const auto* od = findIndexed (*devCfg, "OutputDev", b, nullptr);
            if (b == 1 && od != nullptr && (asio || od->getStringAttribute ("name") != "-"))
                out.kind = IoTarget::Kind::Master;
            else if (asio && od != nullptr && od->getIntAttribute ("pOut1") > 0)
            {
                out.kind = IoTarget::Kind::Master;
                out.channel = od->getIntAttribute ("pOut1") - 1;
            }
            else if (od != nullptr && od->getStringAttribute ("name") != "-" && od->getStringAttribute ("name").isNotEmpty())
                out = wdmTarget (od->getStringAttribute ("name"), false);
        }
        else
        {
            out.kind = IoTarget::Kind::Virtual;
            out.virtualIndex = b - 1 - kVmHwBuses;
        }
        bus.output = out;
        rep.add (busCode (hb) + (bus.name != busCode (hb) ? " \"" + bus.name + "\"" : juce::String()) + ": "
                 + (out.kind == IoTarget::Kind::Master && b > 1 ? "ASIO out " + juce::String (out.channel + 1) + "/" + juce::String (out.channel + 2)
                                                              : out.describe()));
    }

    //--------------------------------------------------------------------------
    // MIDI
    if (const auto* in = findIndexed (*devCfg, "MIDIDevIn", 1, nullptr); in != nullptr && in->getStringAttribute ("name") != "-")
    {
        midi.setInputEnabled (in->getStringAttribute ("name"), true);
        rep.add ("MIDI input: " + in->getStringAttribute ("name"));
    }
    if (const auto* outDev = devCfg->getChildByName ("MIDIDevOut"); outDev != nullptr && outDev->getStringAttribute ("name") != "-")
    {
        midi.setFeedbackOutput (outDev->getStringAttribute ("name"));
        rep.add ("MIDI feedback output: " + outDev->getStringAttribute ("name"));
    }

    if (auto* mm = xml->getChildByName ("VoiceMeeterMidiMap"))
    {
        juce::XmlElement mapXml ("MIDI");
        int mapped = 0, skipped = 0;
        for (auto* item : mm->getChildWithTagNameIterator ("MidiMapItem"))
        {
            const int status = item->getStringAttribute ("b1").getHexValue32();
            if (status == 0 || item->getIntAttribute ("disabled") != 0) continue;

            bool isSwitch = false;
            const auto id = mapMidiId (item->getStringAttribute ("id"), isSwitch);
            if (id.isEmpty() || eng.params.find (id) == nullptr) { ++skipped; continue; }

            const int type = status & 0xF0;
            int source = -1;
            if (type == 0xB0) source = (int) MidiMapping::Source::CC;
            else if (type == 0x90 || type == 0x80) source = (int) MidiMapping::Source::Note;
            else if (type == 0xE0) source = (int) MidiMapping::Source::PitchBend;
            if (source < 0) { ++skipped; continue; }

            MidiMapping::Mode mode = isSwitch ? MidiMapping::Mode::Toggle : MidiMapping::Mode::Absolute;
            if (item->getIntAttribute ("ptt") != 0) mode = MidiMapping::Mode::Momentary;
            if (item->getIntAttribute ("coder") != 0) mode = MidiMapping::Mode::Relative;

            auto* e = mapXml.createNewChildElement ("MAP");
            e->setAttribute ("param", id);
            e->setAttribute ("device", "");
            e->setAttribute ("channel", (status & 0x0F) + 1);
            e->setAttribute ("source", source);
            e->setAttribute ("number", item->getStringAttribute ("b2").getHexValue32());
            e->setAttribute ("mode", (int) mode);
            e->setAttribute ("invert", false);
            ++mapped;
        }
        // Keep the user's existing Hydra mappings unless the same parameter or the same control is
        // now taken by a Voicemeeter mapping.
        if (auto existing = midi.toXml())
        {
            for (auto* old : existing->getChildWithTagNameIterator ("MAP"))
            {
                bool clash = false;
                for (auto* fresh : mapXml.getChildWithTagNameIterator ("MAP"))
                    clash |= fresh->getStringAttribute ("param") == old->getStringAttribute ("param")
                          || (fresh->getIntAttribute ("source") == old->getIntAttribute ("source")
                              && fresh->getIntAttribute ("number") == old->getIntAttribute ("number")
                              && fresh->getIntAttribute ("channel") == old->getIntAttribute ("channel"));
                if (! clash)
                    mapXml.addChildElement (new juce::XmlElement (*old));
            }
            for (auto* in : existing->getChildWithTagNameIterator ("INPUT"))
                mapXml.addChildElement (new juce::XmlElement (*in));
        }
        mapXml.setAttribute ("feedback", midi.getFeedbackOutput());
        if (const auto* in = findIndexed (*devCfg, "MIDIDevIn", 1, nullptr); in != nullptr && in->getStringAttribute ("name") != "-")
            mapXml.createNewChildElement ("INPUT")->setAttribute ("name", in->getStringAttribute ("name"));
        midi.fromXml (mapXml);
        rep.add ("MIDI mappings imported: " + juce::String (mapped) + (skipped > 0 ? "  (" + juce::String (skipped) + " controls have no Hydra equivalent)" : juce::String()));
    }

    //--------------------------------------------------------------------------
    if (applyDevices)
    {
        const auto err = eng.setMasterConfig (master);
        if (err.isNotEmpty())
            rep.add ("WARNING: master device could not be opened: " + err
                     + (asio ? "  (Is Voicemeeter still running and holding the ASIO driver? Close it and press Apply in Settings.)" : juce::String()));
        eng.rebuildGraph();
    }
    rep.add ("Not imported (no equivalent / not wanted): VBAN, FX returns, recorder, strip limiter, 3D panner, macro buttons.");
    res.ok = true;
    return res;
}
} // namespace hydra

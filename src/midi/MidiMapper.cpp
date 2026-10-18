#include "MidiMapper.h"
#include <unordered_set>
#include <map>

namespace hydra
{
juce::String MidiMapping::modeName (Mode m)
{
    switch (m)
    {
        case Mode::Absolute:  return "Absolute";
        case Mode::Toggle:    return "Toggle";
        case Mode::Momentary: return "Momentary";
        case Mode::Relative:  return "Relative (Encoder)";
    }
    return {};
}

juce::String MidiMapping::rangeText() const
{
    if (param == nullptr) return {};
    const auto at = [this] (float n) { return param->range.convertFrom0to1 (juce::jlimit (0.0f, 1.0f, n)); };
    auto fmt = [this] (float v)
    {
        if (param->unit == " dB" && v <= param->range.start + 0.001f && param->range.start <= -60.0f) return juce::String ("-inf dB");
        return juce::String (v, std::abs (v) >= 100.0f ? 0 : 1) + param->unit;
    };
    return fmt (at (minN)) + "  ->  " + fmt (at (maxN));
}

juce::String MidiMapping::describe() const
{
    juce::String s;
    switch (source)
    {
        case Source::CC:        s << "CC " << number; break;
        case Source::Note:      s << juce::MidiMessage::getMidiNoteName (number, true, true, 4) << " (Note " << number << ")"; break;
        case Source::PitchBend: s << "Pitch Bend"; break;
    }
    s << "  Ch " << (channel == 0 ? juce::String ("any") : juce::String (channel));
    if (device.isNotEmpty()) s << "  [" << device << "]";
    return s;
}

//==============================================================================
MidiMapper::MidiMapper (ParamRegistry& registry) : params (registry)
{
    startTimerHz (30);
}

MidiMapper::~MidiMapper()
{
    stopTimer();
    switchOffLeds();
    for (auto& in : openInputs) in->stop();
    openInputs.clear();
}

void MidiMapper::setInputEnabled (const juce::String& name, bool enabled)
{
    if (enabled)
    {
        if (isInputEnabled (name)) return;
        for (auto& info : juce::MidiInput::getAvailableDevices())
        {
            if (info.name != name) continue;
            if (auto in = juce::MidiInput::openDevice (info.identifier, this))
            {
                in->start();
                openInputs.push_back (std::move (in));
                enabledInputs.addIfNotAlreadyThere (name);
                stateRequestAt = juce::Time::getMillisecondCounterHiRes() + 500.0;
            }
            return;
        }
        enabledInputs.addIfNotAlreadyThere (name); // remember for when it gets plugged in
    }
    else
    {
        enabledInputs.removeString (name);
        for (auto it = openInputs.begin(); it != openInputs.end(); ++it)
            if ((*it)->getName() == name) { (*it)->stop(); openInputs.erase (it); break; }
    }
}

bool MidiMapper::isInputEnabled (const juce::String& name) const
{
    for (auto& in : openInputs)
        if (in->getName() == name) return true;
    return false;
}

void MidiMapper::setFeedbackOutput (const juce::String& name)
{
    feedback.reset();
    feedbackId = name;
    if (name.isEmpty()) return;
    for (auto& info : juce::MidiOutput::getAvailableDevices())
        if (info.name == name) { feedback = juce::MidiOutput::openDevice (info.identifier); break; }

    // Pick the matching LED profile automatically the first time an APC mini is chosen.
    if (! led.grid && name.containsIgnoreCase ("APC MINI"))
        for (auto& p : LedProfile::presets())
            if (p.grid && name.containsIgnoreCase ("mk2") == p.name.containsIgnoreCase ("mk2"))
                led = p;

    const juce::ScopedLock sl (lock);
    for (auto& m : mappings) sendFeedback (m, true);
    updateGridLeds (true);
}

void MidiMapper::setLedProfile (const LedProfile& p)
{
    clearGridLeds();
    {
        const juce::ScopedLock sl (lock);
        led = p;
        ++mapVersion;
    }
    updateGridLeds (true);
}

void MidiMapper::clearGridLeds()
{
    if (feedback == nullptr || ! led.grid) return;
    for (int n = led.firstNote; n <= led.lastNote && n < 128; ++n)
        feedback->sendMessageNow (juce::MidiMessage::noteOn (led.channel, n, (juce::uint8) 0));
    ledSent.fill (-1);
}

void MidiMapper::switchOffLeds()
{
    if (feedback == nullptr) return;
    clearGridLeds();
    const juce::ScopedLock sl (lock);
    // Mapped buttons outside the grid (APC mini: track / scene buttons used as mutes) keep their LED
    // otherwise. Faders and knobs (CC) are left where they are: a motor fader must not move on exit.
    for (auto& m : mappings)
        if (m.source == MidiMapping::Source::Note && ! led.contains (m.number))
            feedback->sendMessageNow (juce::MidiMessage::noteOn (m.channel == 0 ? 1 : m.channel, m.number, (juce::uint8) 0));
    if (led.grid)
        for (int n = 0; n < 128; ++n)
            if (! led.contains (n))
                feedback->sendMessageNow (juce::MidiMessage::noteOn (1, n, (juce::uint8) 0));
}

namespace
{
constexpr juce::uint8 kAkai = 0x47, kApcMiniMk1 = 0x28, kApcMiniMk2 = 0x4f;
constexpr int kApcFirstFaderCc = 48;   // faders 1-8 + master on CC 48..56, channel 1

/** Akai model id for a port name (0 = not a controller that can report its faders). */
juce::uint8 akaiModelFor (const juce::String& portName)
{
    if (! portName.containsIgnoreCase ("APC MINI")) return 0;
    return portName.containsIgnoreCase ("mk2") ? kApcMiniMk2 : kApcMiniMk1;
}
} // namespace

void MidiMapper::requestControllerState()
{
    // Most controllers cannot be asked where their faders are (they only send when moved). The Akai APC
    // mini answers the introduction message (the one Ableton sends) with the position of all 9 faders.
    for (auto& in : openInputs)
    {
        const auto model = akaiModelFor (in->getName());
        if (model == 0) continue;
        // Same port name on the output side; reuse the feedback output if it is that device.
        std::unique_ptr<juce::MidiOutput> temp;
        juce::MidiOutput* out = feedback != nullptr && feedbackId == in->getName() ? feedback.get() : nullptr;
        if (out == nullptr)
            for (auto& info : juce::MidiOutput::getAvailableDevices())
                if (info.name == in->getName()) { temp = juce::MidiOutput::openDevice (info.identifier); out = temp.get(); break; }
        if (out == nullptr) continue;

        const juce::uint8 mode = model == kApcMiniMk1 ? 0x41 : 0x00;   // mk1: Ableton Live mode, mk2: no modes
        const juce::uint8 msg[] = { 0xf0, kAkai, 0x7f, model, 0x60, 0x00, 0x04, mode, 0x01, 0x00, 0x00, 0xf7 };
        out->sendMessageNow (juce::MidiMessage (msg, (int) sizeof (msg)));
    }
}

bool MidiMapper::handleStateReply (juce::MidiInput* sourceDev, const juce::MidiMessage& msg)
{
    // Reply: F0 47 <dev> <model> 61 <len hi> <len lo> <fader 1> ... <fader 9> F7  (values 0..127)
    const auto* d = msg.getSysExData();
    const int n = msg.getSysExDataSize();
    if (n < 6 || d[0] != kAkai || d[3] != 0x61 || (d[2] != kApcMiniMk1 && d[2] != kApcMiniMk2)) return false;
    const int len = juce::jmin (n - 6, (d[4] << 7) | d[5], 9);
    const int channel = 1;
    for (int k = 0; k < len; ++k)
    {
        // Fed through the normal path, exactly as if the fader had been moved to where it stands.
        const auto cc = juce::MidiMessage::controllerEvent (channel, kApcFirstFaderCc + k, d[6 + k] & 0x7f);
        handleIncomingMidiMessage (sourceDev, cc);
    }
    return true;
}

std::array<int, 128> MidiMapper::computeGridLeds() const
{
    std::array<int, 128> want;
    want.fill (-1);
    if (! led.grid) return want;

    // "in3.route.A1" -> "in3", "bus.A1.mute" -> "bus.A1"
    auto ownerOf = [] (const juce::String& id)
    {
        if (id.startsWith ("bus."))
            return "bus." + id.fromFirstOccurrenceOf ("bus.", false, false).upToFirstOccurrenceOf (".", false, false);
        return id.upToFirstOccurrenceOf (".", false, false);
    };

    // Pads are grouped per row and per column. A channel "owns" a line when it has a strict majority
    // of the mapped pads in it. Whether the controller is laid out with one channel per row or one
    // per column is decided by which orientation explains more pads (lines with >= 2 pads count).
    using Counts = std::map<juce::String, int>;
    std::map<int, Counts> rows, cols;
    const int rowSize = juce::jmax (1, led.rowSize);
    for (auto& m : mappings)
    {
        if (m.param == nullptr || m.source != MidiMapping::Source::Note || ! led.contains (m.number)) continue;
        const bool on = m.param->getNormalised() >= 0.5f;
        want[(size_t) m.number] = (on != m.invert) ? led.velOn : led.velOff;
        const auto owner = ownerOf (m.paramId);
        ++rows[(m.number - led.firstNote) / rowSize][owner];
        ++cols[(m.number - led.firstNote) % rowSize][owner];
    }

    auto majority = [] (const Counts& counts, int& best) -> juce::String
    {
        int total = 0;
        best = 0;
        juce::String owner;
        for (auto& [o, n] : counts) { total += n; if (n > best) { best = n; owner = o; } }
        return best * 2 > total ? owner : juce::String();
    };
    auto score = [&] (const std::map<int, Counts>& lines)
    {
        int sum = 0;
        for (auto& [line, counts] : lines)
        {
            int best = 0, total = 0;
            for (auto& [o, n] : counts) total += n;
            if (total >= 2 && majority (counts, best).isNotEmpty()) sum += best;
        }
        return sum;
    };
    const bool byColumn = score (cols) > score (rows);
    const auto& lines = byColumn ? cols : rows;

    for (int n = led.firstNote; n <= led.lastNote && n < 128; ++n)
    {
        if (want[(size_t) n] >= 0) continue;
        int vel = led.velUnbound;
        const int line = byColumn ? (n - led.firstNote) % rowSize : (n - led.firstNote) / rowSize;
        if (auto it = lines.find (line); it != lines.end())
        {
            int best = 0;
            const auto owner = majority (it->second, best);
            if (auto* mute = owner.isEmpty() ? nullptr : params.find (owner + ".mute"); mute != nullptr && mute->isOn())
                vel = led.velMuted;
        }
        want[(size_t) n] = vel;
    }
    return want;
}

void MidiMapper::updateGridLeds (bool force)
{
    if (feedback == nullptr || ! led.grid) return;
    const juce::ScopedLock sl (lock);
    const auto want = computeGridLeds();
    for (int n = led.firstNote; n <= led.lastNote && n < 128; ++n)
    {
        if (! force && ledSent[(size_t) n] == want[(size_t) n]) continue;
        feedback->sendMessageNow (juce::MidiMessage::noteOn (led.channel, n, (juce::uint8) juce::jlimit (0, 127, want[(size_t) n])));
        ledSent[(size_t) n] = want[(size_t) n];
    }
}

void MidiMapper::armParam (Param* p)
{
    armed.store (p);
}

//==============================================================================
std::vector<MidiMapping> MidiMapper::getMappings() const
{
    const juce::ScopedLock sl (lock);
    return mappings;
}

std::vector<MidiMapping> MidiMapper::getMappingsFor (const Param* p) const
{
    const juce::ScopedLock sl (lock);
    std::vector<MidiMapping> r;
    for (auto& m : mappings) if (m.param == p) r.push_back (m);
    return r;
}

void MidiMapper::setMappingRange (int index, float minN, float maxN)
{
    const juce::ScopedLock sl (lock);
    if (! juce::isPositiveAndBelow (index, (int) mappings.size())) return;
    auto& m = mappings[(size_t) index];
    m.minN = juce::jlimit (0.0f, 1.0f, minN);
    m.maxN = juce::jlimit (0.0f, 1.0f, maxN);
    m.lastSent = -1;
    ++mapVersion;
}

void MidiMapper::setRangeForParam (const Param* p, float minN, float maxN)
{
    const juce::ScopedLock sl (lock);
    for (auto& m : mappings)
    {
        if (m.param != p) continue;
        if (minN >= 0.0f) m.minN = juce::jlimit (0.0f, 1.0f, minN);
        if (maxN >= 0.0f) m.maxN = juce::jlimit (0.0f, 1.0f, maxN);
        m.lastSent = -1;
    }
    ++mapVersion;
}

bool MidiMapper::hasMapping (const Param* p) const
{
    const juce::ScopedLock sl (lock);
    for (auto& m : mappings) if (m.param == p) return true;
    return false;
}

void MidiMapper::removeMappingsFor (const Param* p)
{
    const juce::ScopedLock sl (lock);
    ++mapVersion;
    mappings.erase (std::remove_if (mappings.begin(), mappings.end(), [p] (auto& m) { return m.param == p; }), mappings.end());
}

void MidiMapper::remapParams (const std::vector<std::pair<Param*, Param*>>& moves)
{
    std::unordered_map<Param*, Param*> to;
    for (auto& [a, b] : moves) to[a] = b;
    const juce::ScopedLock sl (lock);
    for (auto& m : mappings)
        if (auto it = to.find (m.param); it != to.end())
        {
            m.param = it->second;
            m.paramId = it->second->id;
            m.lastSent = -1;          // send the new value as feedback
            m.lastTarget = nullptr;
        }
    lastVersions.clear();
    ++mapVersion;
}

int MidiMapper::countMappings (const std::vector<std::pair<Param*, Param*>>& moves) const
{
    std::unordered_set<const Param*> from;
    for (auto& [a, b] : moves) from.insert (a);
    const juce::ScopedLock sl (lock);
    int n = 0;
    for (auto& m : mappings) n += from.count (m.param) > 0 ? 1 : 0;
    return n;
}

void MidiMapper::removeMapping (int index)
{
    const juce::ScopedLock sl (lock);
    ++mapVersion;
    if (juce::isPositiveAndBelow (index, (int) mappings.size()))
        mappings.erase (mappings.begin() + index);
}

void MidiMapper::setMappingMode (int index, MidiMapping::Mode mode)
{
    const juce::ScopedLock sl (lock);
    ++mapVersion;
    if (juce::isPositiveAndBelow (index, (int) mappings.size()))
        mappings[(size_t) index].mode = mode;
}

void MidiMapper::setMappingInvert (int index, bool inv)
{
    const juce::ScopedLock sl (lock);
    ++mapVersion;
    if (juce::isPositiveAndBelow (index, (int) mappings.size()))
        mappings[(size_t) index].invert = inv;
}

bool MidiMapper::isFaderParam (const Param& p)
{
    return (p.id.endsWith (".gain") && (p.id.startsWith ("in") || p.id.startsWith ("bus."))) || p.id.contains (".send.");
}

void MidiMapper::setFaderTopAt0dB (bool on)
{
    const juce::ScopedLock sl (lock);
    faderTop0dB = on;
    for (auto& m : mappings)
    {
        if (m.param == nullptr || m.isSwitch() || ! isFaderParam (*m.param)) continue;
        const float zero = m.param->range.convertTo0to1 (0.0f);
        if (on && m.maxN > zero + 1.0e-4f)                   m.maxN = zero;
        else if (! on && std::abs (m.maxN - zero) < 1.0e-4f) m.maxN = 1.0f;
        m.lastSent = -1;   // move motor faders to the new scale
    }
    ++mapVersion;
}

void MidiMapper::clearAll()
{
    const juce::ScopedLock sl (lock);
    ++mapVersion;
    mappings.clear();
}

//==============================================================================
void MidiMapper::handleIncomingMidiMessage (juce::MidiInput* sourceDev, const juce::MidiMessage& msg)
{
    if (msg.isSysEx())
    {
        handleStateReply (sourceDev, msg);
        return;
    }

    MidiMapping::Source src;
    int number = 0, raw = 0, rawMax = 127;
    bool press = false, release = false;

    if (msg.isController())         { src = MidiMapping::Source::CC; number = msg.getControllerNumber(); raw = msg.getControllerValue(); press = raw >= 64; release = raw < 64; }
    else if (msg.isNoteOn())        { src = MidiMapping::Source::Note; number = msg.getNoteNumber(); raw = msg.getVelocity(); press = true; }
    else if (msg.isNoteOff())       { src = MidiMapping::Source::Note; number = msg.getNoteNumber(); raw = 0; release = true; }
    else if (msg.isPitchWheel())    { src = MidiMapping::Source::PitchBend; raw = msg.getPitchWheelValue(); rawMax = 16383; }
    else return;

    activity.fetch_add (1, std::memory_order_relaxed);
    const int channel = msg.getChannel();
    const auto devName = sourceDev != nullptr ? sourceDev->getName() : juce::String();

    if (auto* p = armed.exchange (nullptr))
    {
        if (src == MidiMapping::Source::Note && release)
        {
            armed.store (p); // ignore note-offs while learning
            return;
        }

        MidiMapping m;
        m.paramId = p->id;
        m.param = p;
        m.device = devName;
        m.channel = channel;
        m.source = src;
        m.number = number;
        if (p->type == ParamType::Toggle || src == MidiMapping::Source::Note)
            m.mode = MidiMapping::Mode::Toggle;
        else
            m.mode = MidiMapping::Mode::Absolute;

        {
            const juce::ScopedLock sl (lock);
            if (faderTop0dB && ! m.isSwitch() && isFaderParam (*p))
                m.maxN = p->range.convertTo0to1 (0.0f);
            for (auto& o : mappings) // re-learning keeps a calibrated range
                if (o.param == p && ! m.isSwitch()) { m.minN = o.minN; m.maxN = o.maxN; }
            mappings.erase (std::remove_if (mappings.begin(), mappings.end(), [&] (const MidiMapping& o)
            {
                return o.param == p
                    || (o.source == m.source && o.number == m.number && o.channel == m.channel && o.device == m.device);
            }), mappings.end());
            mappings.push_back (m);
            ++mapVersion;
        }

        juce::MessageManager::callAsync ([this, p] { if (onLearned) onLearned (p); });
        return;
    }

    const juce::ScopedLock sl (lock);
    for (auto& m : mappings)
    {
        if (m.source != src || m.number != number || m.param == nullptr) continue;
        if (m.channel != 0 && m.channel != channel) continue;
        if (m.device.isNotEmpty() && m.device != devName) continue;

        auto& p = *targetOf (m);
        switch (m.mode)
        {
            case MidiMapping::Mode::Absolute:
            {
                float n = (float) raw / (float) rawMax;
                if (m.invert) n = 1.0f - n;
                p.setNormalised (m.toParam (n), true);
                m.lastSent = raw;
                break;
            }
            case MidiMapping::Mode::Toggle:
                if (press) p.toggle (true);
                break;
            case MidiMapping::Mode::Momentary:
                if (press)   p.set (m.invert ? p.range.start : p.range.end, true);
                if (release) p.set (m.invert ? p.range.end : p.range.start, true);
                break;
            case MidiMapping::Mode::Relative:
            {
                int delta = src == MidiMapping::Source::CC ? (raw < 64 ? raw : raw - 128) : 0;
                if (m.invert) delta = -delta;
                const float lo = juce::jmin (m.minN, m.maxN), hi = juce::jmax (m.minN, m.maxN);
                p.setNormalised (juce::jlimit (lo, hi, p.getNormalised() + (float) delta / 200.0f * (hi - lo)), true);
                break;
            }
        }
    }
}

void MidiMapper::sendFeedback (MidiMapping& m, bool force)
{
    if (feedback == nullptr || m.param == nullptr) return;
    if (m.source == MidiMapping::Source::Note && led.contains (m.number)) return; // handled by the LED grid engine

    const auto& p = *targetOf (m);
    const int ch = m.channel == 0 ? 1 : m.channel;
    int raw;
    const bool isSwitch = m.mode == MidiMapping::Mode::Toggle || m.mode == MidiMapping::Mode::Momentary;
    if (isSwitch)
        raw = (p.getNormalised() >= 0.5f) != m.invert ? 127 : 0;
    else
    {
        float n = m.fromParam (p.getNormalised());
        if (m.invert) n = 1.0f - n;
        raw = juce::roundToInt (n * (m.source == MidiMapping::Source::PitchBend ? 16383.0f : 127.0f));
    }

    if (! force && raw == m.lastSent) return;
    m.lastSent = raw;

    switch (m.source)
    {
        case MidiMapping::Source::CC:        feedback->sendMessageNow (juce::MidiMessage::controllerEvent (ch, m.number, raw)); break;
        case MidiMapping::Source::Note:      feedback->sendMessageNow (juce::MidiMessage::noteOn (ch, m.number, (juce::uint8) raw)); break;
        case MidiMapping::Source::PitchBend: feedback->sendMessageNow (juce::MidiMessage::pitchWheel (ch, raw)); break;
    }
}

void MidiMapper::timerCallback()
{
    // Re-open enabled devices that were plugged in later.
    static int hotplugTick = 0;
    if (++hotplugTick >= 60)
    {
        hotplugTick = 0;
        for (auto& name : juce::StringArray (enabledInputs))
            if (! isInputEnabled (name))
            {
                for (auto& info : juce::MidiInput::getAvailableDevices())
                    if (info.name == name)
                        if (auto in = juce::MidiInput::openDevice (info.identifier, this))
                        {
                            in->start();
                            openInputs.push_back (std::move (in));
                            stateRequestAt = juce::Time::getMillisecondCounterHiRes() + 500.0;
                        }
            }
        if (feedback == nullptr && feedbackId.isNotEmpty())
            setFeedbackOutput (feedbackId);
    }

    if (stateRequestAt > 0.0 && juce::Time::getMillisecondCounterHiRes() >= stateRequestAt)
    {
        stateRequestAt = 0.0;
        requestControllerState();
    }

    if (feedback == nullptr) return;
    const juce::ScopedLock sl (lock);
    for (auto& m : mappings)
    {
        if (m.param == nullptr) continue;
        auto* t = targetOf (m);
        if (t != m.lastTarget)
        {
            // SEL switched what this control drives: move the motor fader / LED to the new value.
            m.lastTarget = t;
            sendFeedback (m, true);
            continue;
        }
        const auto v = t->getVersion();
        auto& last = lastVersions[t];
        if (last != v)
            sendFeedback (m, false);
    }
    for (auto& m : mappings)
        if (m.param != nullptr) { auto* t = targetOf (m); lastVersions[t] = t->getVersion(); }

    updateGridLeds (false); // only changed pads are sent
}

//==============================================================================
std::unique_ptr<juce::XmlElement> MidiMapper::toXml() const
{
    auto xml = std::make_unique<juce::XmlElement> ("MIDI");
    for (auto& n : enabledInputs)
        xml->createNewChildElement ("INPUT")->setAttribute ("name", n);
    xml->setAttribute ("feedback", feedbackId);
    xml->setAttribute ("faderTop0dB", faderTop0dB);
    {
        auto* l = xml->createNewChildElement ("LED");
        l->setAttribute ("name", led.name);
        l->setAttribute ("grid", led.grid);
        l->setAttribute ("first", led.firstNote);
        l->setAttribute ("last", led.lastNote);
        l->setAttribute ("row", led.rowSize);
        l->setAttribute ("channel", led.channel);
        l->setAttribute ("on", led.velOn);
        l->setAttribute ("off", led.velOff);
        l->setAttribute ("unbound", led.velUnbound);
        l->setAttribute ("muted", led.velMuted);
    }

    const juce::ScopedLock sl (lock);
    for (auto& m : mappings)
    {
        auto* e = xml->createNewChildElement ("MAP");
        e->setAttribute ("param", m.paramId);
        e->setAttribute ("device", m.device);
        e->setAttribute ("channel", m.channel);
        e->setAttribute ("source", (int) m.source);
        e->setAttribute ("number", m.number);
        e->setAttribute ("mode", (int) m.mode);
        e->setAttribute ("invert", m.invert);
        if (m.hasCustomRange())
        {
            e->setAttribute ("min", m.minN);
            e->setAttribute ("max", m.maxN);
        }
    }
    return xml;
}

void MidiMapper::fromXml (const juce::XmlElement& xml)
{
    {
        const juce::ScopedLock sl (lock);
        mappings.clear();
        faderTop0dB = xml.getBoolAttribute ("faderTop0dB", false);
        for (auto* e : xml.getChildWithTagNameIterator ("MAP"))
        {
            MidiMapping m;
            m.paramId = e->getStringAttribute ("param");
            m.param = params.find (m.paramId);
            if (m.param == nullptr) continue;
            m.device = e->getStringAttribute ("device");
            m.channel = e->getIntAttribute ("channel");
            m.source = (MidiMapping::Source) juce::jlimit (0, 2, e->getIntAttribute ("source"));
            m.number = e->getIntAttribute ("number");
            m.mode = (MidiMapping::Mode) juce::jlimit (0, 3, e->getIntAttribute ("mode"));
            m.invert = e->getBoolAttribute ("invert");
            m.minN = (float) juce::jlimit (0.0, 1.0, e->getDoubleAttribute ("min", 0.0));
            m.maxN = (float) juce::jlimit (0.0, 1.0, e->getDoubleAttribute ("max", 1.0));
            mappings.push_back (m);
        }
        ++mapVersion;
    }

    for (auto* e : xml.getChildWithTagNameIterator ("INPUT"))
        setInputEnabled (e->getStringAttribute ("name"), true);
    if (auto* l = xml.getChildByName ("LED"))
    {
        LedProfile p;
        p.name = l->getStringAttribute ("name", p.name);
        p.grid = l->getBoolAttribute ("grid");
        p.firstNote = juce::jlimit (0, 127, l->getIntAttribute ("first", 0));
        p.lastNote = juce::jlimit (0, 127, l->getIntAttribute ("last", 63));
        p.rowSize = juce::jmax (1, l->getIntAttribute ("row", 8));
        p.channel = juce::jlimit (1, 16, l->getIntAttribute ("channel", 1));
        p.velOn = l->getIntAttribute ("on", 127);
        p.velOff = l->getIntAttribute ("off", 0);
        p.velUnbound = l->getIntAttribute ("unbound", 0);
        p.velMuted = l->getIntAttribute ("muted", 0);
        const juce::ScopedLock sl (lock);
        led = p;
    }
    setFeedbackOutput (xml.getStringAttribute ("feedback"));
}
} // namespace hydra

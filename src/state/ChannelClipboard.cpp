#include "ChannelClipboard.h"
#include <set>

namespace hydra::clip
{
namespace
{
juce::String prefixOf (int ch)
{
    return ch < kMaxInputs ? "in" + juce::String (ch + 1) + "." : "bus." + busCode (ch - kMaxInputs) + ".";
}

/** Id part of instance k of a built-in effect including the channel prefix ("in1.gate2"). */
juce::String fxBase (int ch, Fx f, int k) { return prefixOf (ch) + Engine::fxPart (f, k); }

std::unique_ptr<juce::XmlElement> newClip (What w, int ch)
{
    auto xml = std::make_unique<juce::XmlElement> ("HYDRACLIP");
    xml->setAttribute ("version", 2);
    xml->setAttribute ("what", w == What::fx ? "fx" : w == What::chain ? "chain" : "channel");
    xml->setAttribute ("kind", ch < kMaxInputs ? "input" : "bus");
    return xml;
}

void addParam (juce::XmlElement& xml, const juce::String& id, float v)
{
    auto* x = xml.createNewChildElement ("P");
    x->setAttribute ("id", id);
    x->setAttribute ("v", (double) v);
}

/** Ids (without the channel prefix) of every parameter of the channel's built-in effects (all instances). */
std::set<juce::String> chainSuffixes (const Engine& e, int ch)
{
    std::set<juce::String> out;
    const auto prefix = prefixOf (ch);
    for (auto f : e.allowedFx (ch))
        for (int k = 0; k < Engine::maxInstances (f); ++k)
            for (auto* p : e.fxParams (ch, f, k)) out.insert (p->id.substring (prefix.length()));
    return out;
}

/** Ids (without the channel prefix) of the channel's settings: everything except the slot switch and momentary states. */
std::set<juce::String> channelSuffixes (const Engine& e, int ch)
{
    std::set<juce::String> out;
    const auto prefix = prefixOf (ch);
    for (auto& p : e.params.all())
    {
        if (! p->persistent || ! p->id.startsWith (prefix)) continue;
        const auto suffix = p->id.substring (prefix.length());
        if (suffix != "active" && suffix != "mute" && suffix != "solo" && suffix != "sel") out.insert (suffix);
    }
    return out;
}

/** Defaults first, then the clip's values for the wanted suffixes. */
void applyParams (Engine& e, int ch, const juce::XmlElement& clip, const std::set<juce::String>& wanted)
{
    const auto prefix = prefixOf (ch);
    for (auto& suffix : wanted)
        if (auto* p = e.params.find (prefix + suffix)) p->reset();
    for (auto f : e.allowedFx (ch))   // only the clip's effects are switched on below (e.g. the first limiter)
        for (int k = 0; k < Engine::maxInstances (f); ++k)
            if (auto* on = e.fxOnParam (ch, f, k); on != nullptr && wanted.count (on->id.substring (prefix.length())) > 0) on->set (0.0f);
    for (auto* x : clip.getChildWithTagNameIterator ("P"))
    {
        const auto suffix = x->getStringAttribute ("id");
        if (wanted.count (suffix) == 0) continue;
        if (auto* p = e.params.find (prefix + suffix); p != nullptr && p->persistent)
            p->set ((float) x->getDoubleAttribute ("v", p->get()));
    }
}

const juce::XmlElement* slotOf (const juce::XmlElement* clip)
{
    return clip != nullptr && whatOf (clip) == What::fx ? clip->getChildByName ("SLOT") : nullptr;
}

Fx builtinOf (const juce::XmlElement* clip)
{
    if (clip != nullptr && clip->getStringAttribute ("what") == "eq") return Fx::eq;   // EQ clips before version 2
    const auto* slot = slotOf (clip);
    return slot != nullptr && slot->hasAttribute ("builtin") ? fxFromId (slot->getStringAttribute ("builtin")) : Fx::plugin;
}

/** First instance of f that is not in the channel's chain, or -1. */
int freeInstance (const Engine& e, int ch, Fx f)
{
    const auto allowed = e.allowedFx (ch);
    if (std::find (allowed.begin(), allowed.end(), f) == allowed.end()) return -1;
    const auto& chain = ch < kMaxInputs ? e.inputs[(size_t) ch].plugins : e.buses[(size_t) (ch - kMaxInputs)].plugins;
    for (int k = 0; k < Engine::maxInstances (f); ++k)
        if (! chain.hasBuiltin (f, k)) return k;
    return -1;
}

const PluginChain& chainOf (const Engine& e, int ch)
{
    return ch < kMaxInputs ? e.inputs[(size_t) ch].plugins : e.buses[(size_t) (ch - kMaxInputs)].plugins;
}
} // namespace

What whatOf (const juce::XmlElement* clip)
{
    if (clip == nullptr) return What::none;
    const auto w = clip->getStringAttribute ("what");
    if (w == "fx" || w == "eq") return What::fx;
    if (w == "chain") return What::chain;
    if (w == "channel") return What::channel;
    return What::none;
}

juce::String describe (const juce::XmlElement* clip)
{
    switch (whatOf (clip))
    {
        case What::fx:
        {
            if (const auto f = builtinOf (clip); f != Fx::plugin) return fxName (f);
            juce::PluginDescription d;
            if (auto* pd = slotOf (clip)->getChildByName ("PLUGIN"); pd != nullptr && d.loadFromXml (*pd)) return "VST3: " + d.name;
            return "plugin";
        }
        case What::chain:   return "effect chain";
        case What::channel: return "channel " + clip->getStringAttribute ("name");
        case What::none:    break;
    }
    return {};
}

//==============================================================================
std::unique_ptr<juce::XmlElement> copyBuiltin (const Engine& e, int ch, Fx f, int k)
{
    auto xml = newClip (What::fx, ch);
    auto* slot = xml->createNewChildElement ("SLOT");
    slot->setAttribute ("builtin", fxId (f));
    const auto base = fxBase (ch, f, k);
    for (auto* p : e.fxParams (ch, f, k))   // ids relative to the instance: ".thr", "" for phase
        addParam (*xml, p->id.substring (base.length()), p->get());
    return xml;
}

std::unique_ptr<juce::XmlElement> copyBlock (const Engine& e, int ch, int slotIndex)
{
    const auto& chain = chainOf (e, ch);
    auto* s = chain.getSlot (slotIndex);
    if (s == nullptr) return nullptr;
    if (! s->isPlugin()) return copyBuiltin (e, ch, s->builtin, s->fxInstance);
    auto xml = newClip (What::fx, ch);
    xml->addChildElement (chain.slotToXml (slotIndex).release());
    return xml;
}

std::unique_ptr<juce::XmlElement> copyChain (const Engine& e, int ch)
{
    auto xml = newClip (What::chain, ch);
    const auto prefix = prefixOf (ch);
    for (auto& suffix : chainSuffixes (e, ch))
        if (auto* p = e.params.find (prefix + suffix)) addParam (*xml, suffix, p->get());
    xml->addChildElement (chainOf (e, ch).toXml().release());
    return xml;
}

std::unique_ptr<juce::XmlElement> copyChannel (const Engine& e, int ch)
{
    auto xml = newClip (What::channel, ch);
    const bool input = ch < kMaxInputs;
    xml->setAttribute ("name", input ? e.inputs[(size_t) ch].name : e.buses[(size_t) (ch - kMaxInputs)].name);
    xml->addChildElement ((input ? e.inputs[(size_t) ch].source : e.buses[(size_t) (ch - kMaxInputs)].output).toXml ("IO").release());
    const auto prefix = prefixOf (ch);
    for (auto& suffix : channelSuffixes (e, ch))
        if (auto* p = e.params.find (prefix + suffix)) addParam (*xml, suffix, p->get());
    xml->addChildElement (chainOf (e, ch).toXml().release());
    return xml;
}

std::unique_ptr<juce::XmlElement> parse (const juce::String& text)
{
    if (! text.trimStart().startsWith ("<")) return nullptr;
    auto xml = juce::XmlDocument::parse (text);
    return xml != nullptr && xml->hasTagName ("HYDRACLIP") && whatOf (xml.get()) != What::none ? std::move (xml) : nullptr;
}

void toClipboard (const juce::XmlElement& xml) { juce::SystemClipboard::copyTextToClipboard (xml.toString()); }

std::unique_ptr<juce::XmlElement> fromClipboard() { return parse (juce::SystemClipboard::getTextFromClipboard()); }

//==============================================================================
bool canInsertBlock (const Engine& e, const juce::XmlElement* clip, int ch)
{
    if (whatOf (clip) != What::fx || slotOf (clip) == nullptr) return false;
    const auto f = builtinOf (clip);
    return f == Fx::plugin || freeInstance (e, ch, f) >= 0;
}

bool insertBlock (Engine& e, int ch, const juce::XmlElement& clip, int index)
{
    if (! canInsertBlock (e, &clip, ch)) return false;
    auto& chain = e.chainFor (ch);
    const auto f = builtinOf (&clip);
    if (f == Fx::plugin)
        return chain.insertPluginFromXml (*slotOf (&clip), e.pluginHost, index);
    const int k = freeInstance (e, ch, f);
    pasteBuiltinSettings (e, ch, f, k, clip);
    if (f == Fx::denoiser) e.prepareDenoiser (e.inputs[(size_t) ch].denoisers[(size_t) k]);
    chain.addBuiltin (f, k, index);
    return true;
}

bool pasteBuiltinSettings (Engine& e, int ch, Fx f, int k, const juce::XmlElement& clip)
{
    if (builtinOf (&clip) != f || e.fxOnParam (ch, f, k) == nullptr) return false;
    const bool oldEq = clip.getStringAttribute ("what") == "eq";   // ids "eq.b1.gain" instead of ".b1.gain"
    const auto base = fxBase (ch, f, k);
    for (auto* p : e.fxParams (ch, f, k)) p->reset();
    if (auto* on = e.fxOnParam (ch, f, k)) on->set (0.0f);
    for (auto* x : clip.getChildWithTagNameIterator ("P"))
    {
        auto rel = x->getStringAttribute ("id");
        if (oldEq) { if (! rel.startsWith ("eq.")) continue; rel = rel.substring (2); }
        if (auto* p = e.params.find (base + rel); p != nullptr && p->persistent)
            p->set ((float) x->getDoubleAttribute ("v", p->get()));
    }
    return true;
}

bool canPasteSettings (const Engine& e, const juce::XmlElement* clip, int ch, int slotIndex)
{
    auto* s = chainOf (e, ch).getSlot (slotIndex);
    if (s == nullptr || whatOf (clip) != What::fx) return false;
    if (! s->isPlugin()) return builtinOf (clip) == s->builtin;
    // The same plugin: its state can be taken over.
    juce::PluginDescription d;
    const auto* slot = slotOf (clip);
    auto* pd = slot != nullptr ? slot->getChildByName ("PLUGIN") : nullptr;
    return pd != nullptr && d.loadFromXml (*pd) && d.isDuplicateOf (s->description);
}

bool pasteSettings (Engine& e, int ch, int slotIndex, const juce::XmlElement& clip)
{
    if (! canPasteSettings (e, &clip, ch, slotIndex)) return false;
    auto* s = e.chainFor (ch).getSlot (slotIndex);
    if (! s->isPlugin()) return pasteBuiltinSettings (e, ch, s->builtin, s->fxInstance, clip);
    if (auto* st = slotOf (&clip)->getChildByName ("STATE"))
    {
        juce::MemoryBlock state;
        if (state.fromBase64Encoding (st->getAllSubText()))
            s->instance->setStateInformation (state.getData(), (int) state.getSize());
    }
    s->bypassed = slotOf (&clip)->getBoolAttribute ("bypass");
    return true;
}

bool pasteChain (Engine& e, int ch, const juce::XmlElement& clip)
{
    const auto w = whatOf (&clip);
    if (w != What::chain && w != What::channel) return false;
    applyParams (e, ch, clip, chainSuffixes (e, ch));
    auto& chain = e.chainFor (ch);
    if (auto* c = clip.getChildByName ("CHAIN")) chain.fromXml (*c, e.pluginHost);
    else chain.clear();
    e.ensureBuiltins();   // drops effects this channel cannot have (e.g. a gate onto a bus), creates denoisers
    e.sendChangeMessage();
    return true;
}

bool canPasteChannel (const juce::XmlElement* clip, int ch)
{
    return whatOf (clip) == What::channel && (clip->getStringAttribute ("kind") == "input") == (ch < kMaxInputs);
}

bool pasteChannel (Engine& e, int ch, const juce::XmlElement& clip)
{
    if (! canPasteChannel (&clip, ch)) return false;
    applyParams (e, ch, clip, channelSuffixes (e, ch));
    auto& chain = e.chainFor (ch);
    if (auto* c = clip.getChildByName ("CHAIN")) chain.fromXml (*c, e.pluginHost);
    else chain.clear();
    e.ensureBuiltins();
    e.rebuildGraph();
    e.sendChangeMessage();
    return true;
}

bool canPasteAsNew (const juce::XmlElement* clip, int group)
{
    return whatOf (clip) == What::channel && (clip->getStringAttribute ("kind") == "input") == (group == 0);
}

int pasteAsNewChannel (Engine& e, int group, const juce::XmlElement& clip)
{
    if (! canPasteAsNew (&clip, group)) return -1;
    const int idx = group == 0 ? e.addInput() : e.addBus (group == 2);
    if (idx < 0) return -1;
    const int ch = group == 0 ? idx : kMaxInputs + idx;
    pasteChannel (e, ch, clip);
    const auto name = clip.getStringAttribute ("name");
    const auto io = IoTarget::fromXml (clip.getChildByName ("IO"));
    if (group == 0)
    {
        if (name.isNotEmpty()) e.inputs[(size_t) idx].name = name;
        e.setInputSource (idx, io);
    }
    else
    {
        if (name.isNotEmpty()) e.buses[(size_t) idx].name = name;
        e.setBusOutput (idx, io);
    }
    e.sendChangeMessage();
    return ch;
}

//==============================================================================
bool duplicateBlock (Engine& e, int ch, int slotIndex)
{
    auto xml = copyBlock (e, ch, slotIndex);
    return xml != nullptr && insertBlock (e, ch, *xml, slotIndex + 1);
}

void deleteBlock (Engine& e, int ch, int slotIndex)
{
    auto& chain = e.chainFor (ch);
    auto* s = chain.getSlot (slotIndex);
    if (s == nullptr) return;
    if (s->isPlugin()) chain.remove (slotIndex);
    else               e.removeBuiltin (ch, s->builtin, s->fxInstance);
}
} // namespace hydra::clip

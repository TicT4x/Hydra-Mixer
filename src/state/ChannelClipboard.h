#pragma once
#include "engine/Engine.h"

namespace hydra::clip
{
/** What a clip holds (XML root HYDRACLIP, kept as text in the system clipboard):
    fx      = one block of an effect chain: a built-in effect with its settings or a plugin with its state,
    chain   = a whole effect chain (blocks in order, built-in effects with their settings, plugins with their state),
    channel = every setting of a channel (fader, pan, routing, effect chain ...; not mute / solo / SEL) plus its name
              and device, which are only used when it becomes a new channel. */
enum class What { none, fx, chain, channel };

What whatOf (const juce::XmlElement* clip);
/** "Gate", "VST3: Pro-Q 3", "effect chain", "channel IN 2 Mic" ... for menu entries. */
juce::String describe (const juce::XmlElement* clip);

// Copy (message thread). Channels use the selection index scheme (inputs, then buses).
std::unique_ptr<juce::XmlElement> copyBlock (const Engine&, int channel, int slot);
std::unique_ptr<juce::XmlElement> copyBuiltin (const Engine&, int channel, Fx, int instance);
std::unique_ptr<juce::XmlElement> copyChain (const Engine&, int channel);
/** Several blocks of a chain (slot indices, any order: they keep the chain's order) as a chain clip. */
std::unique_ptr<juce::XmlElement> copyBlocks (const Engine&, int channel, std::vector<int> slots);
/** Number of blocks in a chain clip (or the CHAIN of a channel clip). */
int numBlocks (const juce::XmlElement* clip);
std::unique_ptr<juce::XmlElement> copyChannel (const Engine&, int channel);

/** Parses text (clipboard, .hydraeq file); nullptr if it is not a Hydra clip. */
std::unique_ptr<juce::XmlElement> parse (const juce::String& text);
void toClipboard (const juce::XmlElement&);
std::unique_ptr<juce::XmlElement> fromClipboard();

// Paste (message thread)
/** An fx clip can go into the channel's chain (the effect is allowed there and an instance is free). */
bool canInsertBlock (const Engine&, const juce::XmlElement* clip, int channel);
/** Inserts the clip's block as a new block at index (-1 = end). Returns false if it cannot be added. */
bool insertBlock (Engine&, int channel, const juce::XmlElement& clip, int index);
/** The clip holds the same kind of built-in effect as the block: its settings can replace the block's. */
bool canPasteSettings (const Engine&, const juce::XmlElement* clip, int channel, int slot);
bool pasteSettings (Engine&, int channel, int slot, const juce::XmlElement& clip);
/** Settings onto instance k of a built-in effect (EQ import: also the old "eq" clips). */
bool pasteBuiltinSettings (Engine&, int channel, Fx, int instance, const juce::XmlElement& clip);
/** Inserts the blocks of a chain clip (or of an fx clip: its one block) at index (-1 = end), each built-in
    effect as a new instance with its settings. Blocks the channel cannot have (or has the most of) are left out.
    Returns the number of blocks inserted. */
int insertBlocks (Engine&, int channel, const juce::XmlElement& clip, int index);
/** Replaces the channel's whole chain (chain or channel clip). */
bool pasteChain (Engine&, int channel, const juce::XmlElement& clip);
/** Replaces the channel's settings (channel clip of the same kind: input / bus); keeps its name and device. */
bool canPasteChannel (const juce::XmlElement* clip, int channel);
bool pasteChannel (Engine&, int channel, const juce::XmlElement& clip);
/** Group (0 inputs, 1 hardware outputs, 2 virtual outputs) a channel clip can become a new channel in:
    inputs -> 0, buses -> 1 or 2 (any bus). */
bool canPasteAsNew (const juce::XmlElement* clip, int group);
/** Adds a channel to the group with the clip's settings, name and device. Returns its selection index or -1. */
int pasteAsNewChannel (Engine&, int group, const juce::XmlElement& clip);

// Blocks
bool duplicateBlock (Engine&, int channel, int slot);
void deleteBlock (Engine&, int channel, int slot);

/** File type of an exported EQ (an fx clip of an EQ). */
inline const char* const kEqExtension = ".hydraeq";
} // namespace hydra::clip

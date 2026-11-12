#pragma once
#include "ChannelClipboard.h"

/** Preset library: saved effect settings (one built-in effect or plugin) and saved effect chains.

    A preset is a clip (state/ChannelClipboard.h: what="fx" or "chain") in a file <name>.hydrapreset with the
    attribute preset="<name>". Files live in %APPDATA%\HydraMixer\Presets:
        Chains\<name>.hydrapreset                 effect chains
        <fx id>\<name>.hydrapreset                built-in effects ("eq", "gate", "comp" ...)
        VST3\<vendor>\<plugin>\<name>.hydrapreset plugins (with their state)
    Users may copy files in there themselves; the list notices that (rescans at most every 2 s).
    Message thread only. */
namespace hydra::presets
{
inline const char* const kExtension = ".hydrapreset";

struct Preset
{
    juce::File file;
    juce::String name;
    bool chain = false;
    Fx fx = Fx::plugin;           // effect preset: the built-in effect, or Fx::plugin
    juce::String plugin, vendor;  // plugin presets: name and vendor of the plugin
    int blocks = 0;               // chain presets: number of blocks
    juce::String describe() const;   // "EQ preset", "Pro-Q 3 preset", "chain, 3 effects"
};

juce::File rootDir();

/** Every preset, sorted by kind and name. Rescans the folder if it may have changed. */
const std::vector<Preset>& all();
/** Changes whenever the list changes (for views). */
uint32_t version();
/** Forces a rescan on the next call of all(). */
void invalidate();

std::vector<Preset> forFx (Fx);
std::vector<Preset> forPlugin (const juce::PluginDescription&);
std::vector<Preset> chains();

/** The clip of a preset (nullptr if the file is gone or unreadable). */
std::unique_ptr<juce::XmlElement> load (const Preset&);

/** Where save() would write the clip under this name (to ask before overwriting). Empty for an unusable clip. */
juce::File fileFor (const juce::XmlElement& clip, const juce::String& name);
/** Saves an fx / chain clip as a preset (overwrites one of the same name). Returns false with an error text. */
bool save (const juce::XmlElement& clip, const juce::String& name, juce::String& error);
bool rename (const Preset&, const juce::String& newName, juce::String& error);
/** Moves the file to the recycle bin. */
bool remove (const Preset&);
/** Copies .hydrapreset files (made anywhere: export, other PCs) into the library. Returns how many. */
int importFiles (const juce::Array<juce::File>&, juce::String& error);
bool exportTo (const Preset&, const juce::File& destination);

/** Folder of a plugin's presets (also used to match a plugin to its presets). */
juce::File pluginDir (const juce::String& vendor, const juce::String& plugin);
} // namespace hydra::presets

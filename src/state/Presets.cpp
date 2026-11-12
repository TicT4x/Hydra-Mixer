#include "Presets.h"

namespace hydra::presets
{
namespace
{
struct Library
{
    std::vector<Preset> list;
    std::map<juce::String, juce::Time> seen;   // path -> modification time of the last scan
    double lastScan = -1.0e9;
    bool dirty = true;
    uint32_t version = 1;
};

Library& lib()
{
    static Library l;
    return l;
}

juce::String legal (const juce::String& s)
{
    auto t = juce::File::createLegalFileName (s.trim()).trim();
    return t.isEmpty() ? juce::String ("Unnamed") : t;
}

/** Plugin description stored in an fx clip, if it holds a plugin. */
bool pluginOf (const juce::XmlElement& clip, juce::PluginDescription& d)
{
    auto* slot = clip.getChildByName ("SLOT");
    auto* pd = slot != nullptr ? slot->getChildByName ("PLUGIN") : nullptr;
    return pd != nullptr && d.loadFromXml (*pd);
}

Fx builtinOf (const juce::XmlElement& clip)
{
    auto* slot = clip.getChildByName ("SLOT");
    return slot != nullptr && slot->hasAttribute ("builtin") ? fxFromId (slot->getStringAttribute ("builtin")) : Fx::plugin;
}

juce::File dirFor (const juce::XmlElement& clip)
{
    const auto w = clip::whatOf (&clip);
    if (w == clip::What::chain) return rootDir().getChildFile ("Chains");
    if (w != clip::What::fx) return {};
    if (const auto f = builtinOf (clip); f != Fx::plugin) return rootDir().getChildFile (fxId (f));
    juce::PluginDescription d;
    if (! pluginOf (clip, d)) return {};
    return pluginDir (d.manufacturerName, d.name);
}

bool readPreset (const juce::File& f, Preset& p)
{
    auto xml = clip::parse (f.loadFileAsString());
    if (xml == nullptr) return false;
    const auto w = clip::whatOf (xml.get());
    if (w != clip::What::fx && w != clip::What::chain) return false;
    p.file = f;
    p.name = xml->getStringAttribute ("preset", f.getFileNameWithoutExtension());
    p.chain = w == clip::What::chain;
    p.blocks = clip::numBlocks (xml.get());
    if (! p.chain)
    {
        p.fx = builtinOf (*xml);
        juce::PluginDescription d;
        if (p.fx == Fx::plugin)
        {
            if (! pluginOf (*xml, d)) return false;
            p.plugin = d.name;
            p.vendor = d.manufacturerName;
        }
    }
    return true;
}

void scan()
{
    auto& l = lib();
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (! l.dirty && now - l.lastScan < 2000.0) return;
    l.lastScan = now;
    std::map<juce::String, juce::Time> found;
    for (auto& f : rootDir().findChildFiles (juce::File::findFiles, true, juce::String ("*") + kExtension))
        found[f.getFullPathName()] = f.getLastModificationTime();
    if (! l.dirty && found == l.seen) return;
    l.dirty = false;
    l.seen = found;
    std::vector<Preset> list;
    for (auto& [path, time] : found)
    {
        Preset p;
        if (readPreset (juce::File (path), p)) list.push_back (std::move (p));
    }
    std::sort (list.begin(), list.end(), [] (const Preset& a, const Preset& b)
    {
        if (a.chain != b.chain) return b.chain;
        if (a.fx != b.fx) return (int) a.fx < (int) b.fx;
        if (a.vendor != b.vendor) return a.vendor.compareNatural (b.vendor) < 0;
        if (a.plugin != b.plugin) return a.plugin.compareNatural (b.plugin) < 0;
        return a.name.compareNatural (b.name) < 0;
    });
    l.list = std::move (list);
    ++l.version;
}

void changed()
{
    lib().dirty = true;
    scan();
}
} // namespace

juce::String Preset::describe() const
{
    if (chain) return "effect chain, " + juce::String (blocks) + (blocks == 1 ? " effect" : " effects");
    return (fx == Fx::plugin ? plugin : fxName (fx)) + " preset";
}

juce::File rootDir() { return appDataDir().getChildFile ("Presets"); }

juce::File pluginDir (const juce::String& vendor, const juce::String& plugin)
{
    return rootDir().getChildFile ("VST3").getChildFile (legal (vendor.isEmpty() ? juce::String ("Unknown") : vendor)).getChildFile (legal (plugin));
}

const std::vector<Preset>& all()
{
    scan();
    return lib().list;
}

uint32_t version()
{
    scan();
    return lib().version;
}

void invalidate() { lib().dirty = true; }

std::vector<Preset> forFx (Fx f)
{
    std::vector<Preset> out;
    for (auto& p : all())
        if (! p.chain && p.fx == f && f != Fx::plugin) out.push_back (p);
    return out;
}

std::vector<Preset> forPlugin (const juce::PluginDescription& d)
{
    std::vector<Preset> out;
    const auto dir = pluginDir (d.manufacturerName, d.name);
    for (auto& p : all())
        if (! p.chain && p.fx == Fx::plugin && p.file.getParentDirectory() == dir) out.push_back (p);
    return out;
}

std::vector<Preset> chains()
{
    std::vector<Preset> out;
    for (auto& p : all())
        if (p.chain) out.push_back (p);
    return out;
}

std::unique_ptr<juce::XmlElement> load (const Preset& p)
{
    return p.file.existsAsFile() ? clip::parse (p.file.loadFileAsString()) : nullptr;
}

juce::File fileFor (const juce::XmlElement& clip, const juce::String& name)
{
    const auto dir = dirFor (clip);
    if (dir == juce::File() || name.trim().isEmpty()) return {};
    return dir.getChildFile (legal (name) + kExtension);
}

bool save (const juce::XmlElement& clip, const juce::String& name, juce::String& error)
{
    const auto file = fileFor (clip, name);
    if (file == juce::File())
    {
        error = name.trim().isEmpty() ? "The preset needs a name." : "This cannot be saved as a preset.";
        return false;
    }
    juce::XmlElement copy (clip);
    copy.setAttribute ("preset", name.trim());
    if (! file.getParentDirectory().createDirectory() || ! copy.writeTo (file))
    {
        error = "Could not write " + file.getFullPathName();
        return false;
    }
    changed();
    return true;
}

bool rename (const Preset& p, const juce::String& newName, juce::String& error)
{
    auto xml = load (p);
    if (xml == nullptr) { error = "The preset file is gone."; return false; }
    const auto target = p.file.getParentDirectory().getChildFile (legal (newName) + kExtension);
    if (target != p.file && target.existsAsFile()) { error = "A preset with this name already exists."; return false; }
    xml->setAttribute ("preset", newName.trim());
    if (! xml->writeTo (target)) { error = "Could not write " + target.getFullPathName(); return false; }
    if (target != p.file) p.file.deleteFile();
    changed();
    return true;
}

bool remove (const Preset& p)
{
    const bool ok = p.file.moveToTrash() || p.file.deleteFile();
    changed();
    return ok;
}

int importFiles (const juce::Array<juce::File>& files, juce::String& error)
{
    int n = 0;
    juce::StringArray failed;
    for (auto& f : files)
    {
        auto xml = clip::parse (f.loadFileAsString());
        const auto w = clip::whatOf (xml.get());
        if (xml == nullptr || (w != clip::What::fx && w != clip::What::chain)) { failed.add (f.getFileName()); continue; }
        const auto name = xml->getStringAttribute ("preset", f.getFileNameWithoutExtension());
        auto target = fileFor (*xml, name);
        // Same name already there: keep both ("name 2").
        for (int k = 2; target.existsAsFile() && k < 100; ++k)
            target = fileFor (*xml, name + " " + juce::String (k));
        xml->setAttribute ("preset", target.getFileNameWithoutExtension());
        if (target == juce::File() || ! target.getParentDirectory().createDirectory() || ! xml->writeTo (target)) { failed.add (f.getFileName()); continue; }
        ++n;
    }
    if (! failed.isEmpty()) error = "Not a Hydra preset: " + failed.joinIntoString (", ");
    changed();
    return n;
}

bool exportTo (const Preset& p, const juce::File& destination)
{
    return p.file.copyFileTo (destination);
}
} // namespace hydra::presets

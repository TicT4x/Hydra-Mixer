#include "CorrectionPool.h"

namespace hydra::pool
{
juce::File folder()
{
    auto d = folderOverride != juce::File() ? folderOverride : appDataDir().getChildFile ("Corrections");
    d.createDirectory();
    return d;
}

juce::File fileFor (const juce::String& id)
{
    return folder().getChildFile (juce::File::createLegalFileName (id) + kExtension);
}

juce::String newId()
{
    return juce::Uuid().toDashedString().substring (0, 18);
}

bool read (const juce::File& file, Entry& out)
{
    auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr || ! xml->hasTagName ("HYDRACORRECTION")) return false;
    out.id = xml->getStringAttribute ("id", file.getFileNameWithoutExtension());
    out.revision = xml->getIntAttribute ("revision", 1);
    out.modified = juce::Time (xml->getStringAttribute ("modified").getLargeIntValue());
    if (out.modified.toMilliseconds() == 0) out.modified = file.getLastModificationTime();
    out.madeIn = xml->getStringAttribute ("madeIn");
    out.profile = CorrectionProfile::fromXml (xml->getChildByName ("CORRECTION"));
    out.profile.poolId = out.id;
    out.file = file;
    return true;
}

bool read (const juce::String& id, Entry& out)
{
    const auto f = fileFor (id);
    return f.existsAsFile() && read (f, out);
}

std::vector<Entry> list()
{
    std::vector<Entry> v;
    for (const auto& f : folder().findChildFiles (juce::File::findFiles, false, juce::String ("*") + kExtension))
    {
        Entry e;
        if (read (f, e)) v.push_back (std::move (e));
    }
    std::sort (v.begin(), v.end(), [] (const Entry& a, const Entry& b) { return a.modified > b.modified; });
    return v;
}

int write (const CorrectionProfile& profile, const juce::String& madeIn)
{
    if (profile.poolId.isEmpty()) return 0;
    const auto file = fileFor (profile.poolId);
    Entry old;
    const int revision = (file.existsAsFile() && read (file, old) ? old.revision : 0) + 1;

    juce::XmlElement xml ("HYDRACORRECTION");
    xml.setAttribute ("version", 1);
    xml.setAttribute ("id", profile.poolId);
    xml.setAttribute ("revision", revision);
    xml.setAttribute ("modified", juce::String (juce::Time::getCurrentTime().toMilliseconds()));
    xml.setAttribute ("madeIn", madeIn);
    xml.addChildElement (profile.toXml().release());

    // Atomic: the other app may read it at any moment.
    juce::TemporaryFile tmp (file);
    if (! xml.writeTo (tmp.getFile()) || ! tmp.overwriteTargetFileWithTemporary()) return 0;
    return revision;
}

bool remove (const juce::String& id)
{
    const auto f = fileFor (id);
    if (! f.existsAsFile()) return false;
    return (folderOverride == juce::File() && f.moveToTrash()) || f.deleteFile();
}

bool rename (const juce::String& id, const juce::String& name, const juce::String& madeIn)
{
    Entry e;
    if (! read (id, e)) return false;
    e.profile.name = name;
    return write (e.profile, madeIn) > 0;
}

bool same (const CorrectionProfile& a, const CorrectionProfile& b)
{
    auto x = a, y = b;
    x.poolId = y.poolId = {};
    return x.toXml()->toString() == y.toXml()->toString();
}

//==============================================================================
void Link::stamp (const juce::File& f)
{
    stampTime = f.getLastModificationTime();
    stampSize = f.getSize();
}

void Link::sync (CorrectionTarget& t)
{
    if (! first && t.getVersion() == seenVersion) return pollFile (t);
    const bool wasFirst = std::exchange (first, false);
    auto p = t.getProfile();
    if (p.isEmpty() || (p.poolId.isEmpty() && t.getVersion() == unlinkedVersion))
    {
        id = {};
        seenFile = false;
        seenVersion = t.getVersion();
        return;
    }
    if (p.poolId.isEmpty())
    {
        // A new correction.
        p.poolId = newId();
        t.setProfile (p);
    }
    else if (wasFirst || p.poolId != id)
    {
        // Loaded or chosen: the pool's version wins.
        Entry e;
        if (read (p.poolId, e))
        {
            id = p.poolId;
            seenFile = true;
            seenRevision = e.revision;
            stamp (e.file);
            if (! same (e.profile, p)) t.setProfile (e.profile);
            seenVersion = t.getVersion();
            return;
        }
    }
    id = p.poolId;
    seenRevision = write (p, madeIn);
    seenFile = seenRevision > 0;
    stamp (fileFor (id));
    seenVersion = t.getVersion();
}

void Link::pollFile (CorrectionTarget& t)
{
    if (id.isEmpty()) return;
    const auto f = fileFor (id);
    if (! f.existsAsFile())
    {
        if (seenFile)
        {
            // Deleted from the pool: the correction stays, unlinked.
            auto p = t.getProfile();
            p.poolId = {};
            t.setProfile (p);
            unlinkedVersion = seenVersion = t.getVersion();
            id = {};
            seenFile = false;
        }
        return;
    }
    if (f.getLastModificationTime() == stampTime && f.getSize() == stampSize) return;
    stamp (f);
    Entry e;
    if (! read (f, e) || e.revision == seenRevision) return;
    seenRevision = e.revision;
    seenFile = true;
    if (! same (e.profile, t.getProfile()))
    {
        t.setProfile (e.profile);
        seenVersion = t.getVersion();
    }
}
} // namespace hydra::pool

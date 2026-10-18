#include "Session.h"

namespace hydra
{
bool Session::save (const juce::File& f) const
{
    juce::XmlElement root ("HYDRASESSION");
    root.setAttribute ("version", 2);
    root.setAttribute ("window", windowState);

    // While the master device the session asks for is still missing (just after Windows started),
    // the session keeps asking for it instead of storing the fallback device.
    const auto mc = waitingForMaster ? requestedMaster : engine.getMasterConfig();
    auto* m = root.createNewChildElement ("MASTER");
    m->setAttribute ("type", mc.typeName);
    m->setAttribute ("device", mc.deviceName);
    m->setAttribute ("rate", mc.sampleRate);
    m->setAttribute ("buffer", mc.bufferSize);

    auto* ps = root.createNewChildElement ("PARAMS");
    for (auto& p : engine.params.all())
        if (p->persistent && p->get() != p->defaultValue)
            ps->setAttribute (p->id.replaceCharacter ('.', '_'), p->get());

    for (int i = 0; i < kMaxInputs; ++i)
    {
        const auto& s = engine.inputs[(size_t) i];
        auto* e = root.createNewChildElement ("INPUT");
        e->setAttribute ("index", i);
        e->setAttribute ("name", s.name);
        e->addChildElement (s.source.toXml ("SOURCE").release());
        e->addChildElement (s.plugins.toXml().release());
    }
    for (int b = 0; b < kMaxBuses; ++b)
    {
        const auto& bus = engine.buses[(size_t) b];
        auto* e = root.createNewChildElement ("BUS");
        e->setAttribute ("index", b);
        e->setAttribute ("name", bus.name);
        e->addChildElement (bus.output.toXml ("OUTPUT").release());
        e->addChildElement (bus.plugins.toXml().release());
    }

    auto* app = root.createNewChildElement ("APP");
    app->setAttribute ("tray", prefs.trayIcon);
    app->setAttribute ("closeToTray", prefs.closeToTray);
    app->setAttribute ("minimiseToTray", prefs.minimiseToTray);
    app->setAttribute ("startInTray", prefs.startInTray);
    app->setAttribute ("uiScale", prefs.uiScale);
    app->setAttribute ("fontScale", prefs.fontScale);
    app->setAttribute ("stripSplit", prefs.stripSplit);

    auto* rec = root.createNewChildElement ("RECORDER");
    rec->setAttribute ("folder", engine.recorder.folder.getFullPathName());
    rec->setAttribute ("bits", engine.recorder.bitDepth);
    rec->setAttribute ("file", engine.recorder.getLoadedFile().getFullPathName());

    root.addChildElement (midi.toXml().release());
    if (hotkeys != nullptr)
        root.addChildElement (hotkeys->toXml().release());
    root.addChildElement (scenes.toXml().release());

    // Write atomically so a crash mid-save never destroys the session.
    auto tmp = f.getSiblingFile (f.getFileName() + ".tmp");
    if (! root.writeTo (tmp))
    {
        logLine ("session save: cannot write " + tmp.getFullPathName());
        return false;
    }
    if (! tmp.moveFileTo (f))
    {
        logLine ("session save: cannot replace " + f.getFullPathName());
        return false;
    }
    return true;
}

bool Session::load (const juce::File& f)
{
    auto root = juce::XmlDocument::parse (f);
    MasterConfig mc;

    if (root == nullptr || ! root->hasTagName ("HYDRASESSION"))
    {
        engine.setMasterConfig (mc);
        return false;
    }

    windowState = root->getStringAttribute ("window");

    prefs = {};
    if (auto* app = root->getChildByName ("APP"))
    {
        prefs.trayIcon = app->getBoolAttribute ("tray", prefs.trayIcon);
        prefs.closeToTray = app->getBoolAttribute ("closeToTray", prefs.closeToTray);
        prefs.minimiseToTray = app->getBoolAttribute ("minimiseToTray", prefs.minimiseToTray);
        prefs.startInTray = app->getBoolAttribute ("startInTray", prefs.startInTray);
        prefs.uiScale = juce::jlimit (0.5f, 3.0f, (float) app->getDoubleAttribute ("uiScale", 1.0));
        prefs.fontScale = juce::jlimit (0.5f, 2.0f, (float) app->getDoubleAttribute ("fontScale", 1.0));
        prefs.stripSplit = juce::jlimit (-1.0f, 1.0f, (float) app->getDoubleAttribute ("stripSplit", -1.0));
    }
    prefsVersion.fetch_add (1);

    if (auto* rec = root->getChildByName ("RECORDER"))
    {
        const auto folder = rec->getStringAttribute ("folder");
        engine.recorder.folder = juce::File::isAbsolutePath (folder) ? juce::File (folder) : juce::File();
        engine.recorder.bitDepth = rec->getIntAttribute ("bits", 24);
        const auto file = rec->getStringAttribute ("file");
        if (juce::File::isAbsolutePath (file) && juce::File (file).existsAsFile())
        {
            juce::String err;
            engine.recorder.loadFile (juce::File (file), err);
        }
    }

    // Version 1 sessions: EQ bands were on by default (and therefore not stored when on).
    const int fileVersion = root->getIntAttribute ("version", 1);
    if (auto* ps = root->getChildByName ("PARAMS"))
        for (auto& p : engine.params.all())
        {
            if (! p->persistent) continue;
            const auto key = p->id.replaceCharacter ('.', '_');
            float v = p->defaultValue;
            if (ps->hasAttribute (key)) v = (float) ps->getDoubleAttribute (key);
            else if (fileVersion < 2 && p->id.contains (".eq.b") && p->id.endsWith (".on")
                     && p->id.fromLastOccurrenceOf (".eq.b", false, false).getIntValue() <= ParamEq::kV1Bands) v = 1.0f;
            p->set (v);
        }
    // A flat EQ starts without points.
    for (auto& s : engine.inputs) s.eq.hideNeutralBands();
    for (auto& b : engine.buses)  b.eq.hideNeutralBands();

    for (auto* e : root->getChildWithTagNameIterator ("INPUT"))
    {
        const int i = e->getIntAttribute ("index", -1);
        if (! juce::isPositiveAndBelow (i, kMaxInputs)) continue;
        auto& s = engine.inputs[(size_t) i];
        s.name = e->getStringAttribute ("name", s.name);
        s.source = IoTarget::fromXml (e->getChildByName ("SOURCE"));
    }
    for (auto* e : root->getChildWithTagNameIterator ("BUS"))
    {
        const int b = e->getIntAttribute ("index", -1);
        if (! juce::isPositiveAndBelow (b, kMaxBuses)) continue;
        auto& bus = engine.buses[(size_t) b];
        bus.name = e->getStringAttribute ("name", bus.name);
        bus.output = IoTarget::fromXml (e->getChildByName ("OUTPUT"));
    }

    if (auto* m = root->getChildByName ("MASTER"))
    {
        mc.typeName = m->getStringAttribute ("type", mc.typeName);
        mc.deviceName = m->getStringAttribute ("device");
        mc.sampleRate = m->getDoubleAttribute ("rate", 48000.0);
        mc.bufferSize = m->getIntAttribute ("buffer", 0);
    }
    requestedMaster = mc;
    engine.setMasterConfig (mc);

    // Plugins after the engine runs, so they are instantiated at the real sample rate.
    for (auto* e : root->getChildWithTagNameIterator ("INPUT"))
    {
        const int i = e->getIntAttribute ("index", -1);
        if (auto* c = e->getChildByName ("CHAIN"); c != nullptr && juce::isPositiveAndBelow (i, kMaxInputs))
            engine.inputs[(size_t) i].plugins.fromXml (*c, engine.pluginHost);
    }
    for (auto* e : root->getChildWithTagNameIterator ("BUS"))
    {
        const int b = e->getIntAttribute ("index", -1);
        if (auto* c = e->getChildByName ("CHAIN"); c != nullptr && juce::isPositiveAndBelow (b, kMaxBuses))
            engine.buses[(size_t) b].plugins.fromXml (*c, engine.pluginHost);
    }

    if (auto* mx = root->getChildByName ("MIDI"))
        midi.fromXml (*mx);
    if (hotkeys != nullptr)
    {
        if (auto* hk = root->getChildByName ("HOTKEYS")) hotkeys->fromXml (*hk);
        else                                             hotkeys->clearAll();
    }
    if (auto* sc = root->getChildByName ("SCENES")) scenes.fromXml (*sc, false);
    else                                            scenes.clear();

    return true;
}
//==============================================================================
namespace
{
/** Sortable time stamp for backup names: 2026-10-13_20-30-05-123 (strictly increasing within a run). */
juce::String stamp()
{
    static juce::int64 last = 0;
    const auto ms = juce::jmax (last + 1, juce::Time::currentTimeMillis());
    last = ms;
    const juce::Time t (ms);
    return t.formatted ("%Y-%m-%d_%H-%M-%S-") + juce::String (t.getMilliseconds()).paddedLeft ('0', 3);
}
} // namespace

bool Session::isSessionFile (const juce::File& f)
{
    if (! f.existsAsFile()) return false;
    auto root = juce::XmlDocument::parse (f);
    return root != nullptr && root->hasTagName ("HYDRASESSION");
}

bool Session::loadDefault()
{
    const auto f = defaultFile();
    if (f.existsAsFile() && ! isSessionFile (f))
    {
        // Never replace an unreadable session with defaults - keep it for manual recovery.
        backupDir().createDirectory();
        const auto keep = backupDir().getNonexistentChildFile ("session_" + stamp() + "_unreadable", ".xml", false);
        const bool moved = f.moveFileTo (keep);
        logLine ("session.xml unreadable, moved to " + keep.getFullPathName() + (moved ? "" : " FAILED"));
    }

    const bool ok = load (f);
    if (ok) backup ("start");
    rememberDiskState();
    return ok;
}

bool Session::saveDefault()
{
    if (diskChangedSinceRemembered())
    {
        backup ("changed-elsewhere");
        logLine ("session.xml was changed by another program - kept a backup before saving");
    }
    const bool ok = save (defaultFile());
    rememberDiskState();
    return ok;
}

bool Session::importFrom (const juce::File& f, juce::String& error)
{
    if (! isSessionFile (f))
    {
        error = f.getFileName() + " is not a Hydra Mixer settings file.";
        return false;
    }
    // Back up what is running right now (it may contain changes not yet autosaved).
    saveDefault();
    backup ("before-import");

    // Copy first: f may itself be a backup that the pruning below would remove.
    const auto text = f.loadFileAsString();
    juce::TemporaryFile tmp (".xml");
    tmp.getFile().replaceWithText (text);
    const bool ok = load (tmp.getFile());
    if (! ok) { error = "Could not read " + f.getFileName(); return false; }
    saveDefault();
    logLine ("settings imported from " + f.getFullPathName());
    return true;
}

juce::File Session::backup (const juce::String& reason)
{
    const auto f = defaultFile();
    if (! f.existsAsFile()) return {};
    backupDir().createDirectory();

    // Skip if the newest backup is identical (e.g. several starts without changes).
    const auto existing = listBackups();
    if (! existing.isEmpty() && existing.getFirst().hasIdenticalContentTo (f))
        return existing.getFirst();

    const auto target = backupDir().getNonexistentChildFile ("session_" + stamp() + "_" + reason, ".xml", false);
    if (! f.copyFileTo (target))
    {
        logLine ("backup FAILED: " + target.getFullPathName());
        return {};
    }
    pruneBackups();
    return target;
}

juce::Array<juce::File> Session::listBackups() const
{
    auto files = backupDir().findChildFiles (juce::File::findFiles, false, "session_*.xml");
    // Names start with a sortable timestamp.
    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b) { return a.getFileName() > b.getFileName(); });
    return files;
}

void Session::pruneBackups() const
{
    const auto files = listBackups();
    for (int i = kMaxBackups; i < files.size(); ++i)
        files[i].deleteFile();
}

void Session::rememberDiskState()
{
    const auto f = defaultFile();
    knownModTime = f.getLastModificationTime();
    knownSize = f.existsAsFile() ? f.getSize() : -1;
}

bool Session::diskChangedSinceRemembered() const
{
    const auto f = defaultFile();
    if (! f.existsAsFile()) return false;
    return f.getSize() != knownSize || f.getLastModificationTime() != knownModTime;
}
} // namespace hydra

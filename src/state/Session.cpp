#include "Session.h"
#include "engine/NetLink.h"
#include "core/NetCrypto.h"
#include "remote/RemoteServer.h"

namespace hydra
{
/** The network password for the file, encrypted for this Windows user. DPAPI gives a different text every time:
    the same password keeps its text, so autosaves of an unchanged session stay identical (backups skip duplicates). */
struct ProtectedText
{
    juce::String get (const juce::String& text)
    {
        const juce::ScopedLock sl (lock);
        if (text != plain || stored.isEmpty())
        {
            stored = net::protectForUser (text);
            plain = text;
        }
        return stored;
    }
    juce::CriticalSection lock;
    juce::String plain, stored;
};

static juce::String protectedNetPassword (const juce::String& password)
{
    static ProtectedText cache;
    return cache.get (password);
}

/** The access code of Hydra Remote, like the network password. */
static juce::String protectedRemoteCode (const juce::String& code)
{
    static ProtectedText cache;
    return cache.get (code);
}

bool Session::save (const juce::File& f) const
{
    juce::XmlElement root ("HYDRASESSION");
    root.setAttribute ("version", 3);   // 3: bus limiters off by default (9.11.)
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
        // Inputs can have a correction since 12.11. (old sessions' CORRECTION of inputs stays ignored).
        if (! s.correction.getProfile().isEmpty())
            e->createNewChildElement ("FXCORRECTION")->addChildElement (s.correction.getProfile().toXml().release());
    }
    for (int b = 0; b < kMaxBuses; ++b)
    {
        const auto& bus = engine.buses[(size_t) b];
        auto* e = root.createNewChildElement ("BUS");
        e->setAttribute ("index", b);
        e->setAttribute ("name", bus.name);
        if (bus.icon.isNotEmpty()) e->setAttribute ("icon", bus.icon);
        e->addChildElement (bus.output.toXml ("OUTPUT").release());
        e->addChildElement (bus.plugins.toXml().release());
        if (! bus.correction.getProfile().isEmpty()) e->addChildElement (bus.correction.getProfile().toXml().release());
    }

    auto* app = root.createNewChildElement ("APP");
    app->setAttribute ("tray", prefs.trayIcon);
    app->setAttribute ("closeToTray", prefs.closeToTray);
    app->setAttribute ("minimiseToTray", prefs.minimiseToTray);
    app->setAttribute ("startInTray", prefs.startInTray);
    app->setAttribute ("uiScale", prefs.uiScale);
    app->setAttribute ("fontScale", prefs.fontScale);
    app->setAttribute ("stripSplit", prefs.stripSplit);
    app->setAttribute ("stripSplit2", prefs.stripSplit2);
    app->setAttribute ("renameDevices", prefs.renameDevices);
    app->setAttribute ("vacDirect", prefs.vacDirect);
    app->setAttribute ("vacHideOwnSide", prefs.vacHideOwnSide);
    app->setAttribute ("vacHideUnused", prefs.vacHideUnused);
    app->setAttribute ("vacAutoName", prefs.vacAutoName);
    app->setAttribute ("renameNoPrompt", prefs.renameNoPrompt);
    app->setAttribute ("autoReconnect", prefs.autoReconnect);
    app->setAttribute ("expertDeviceModes", prefs.expertDeviceModes);
    app->setAttribute ("pickerShowAll", prefs.pickerShowAll);
    app->setAttribute ("eqShowInput", prefs.eqShowInput);
    app->setAttribute ("adaptiveStrips", prefs.adaptiveStrips);
    app->setAttribute ("sidePanel", prefs.sidePanel);
    app->setAttribute ("fxBrowser", prefs.fxBrowser);
    app->setAttribute ("fxWarnOff", prefs.fxWarnOff);
    app->setAttribute ("horizontalStrips", prefs.horizontalStrips);
    app->setAttribute ("fitWindow", prefs.fitWindow);
    app->setAttribute ("stripDividers", prefs.stripDividers);
    app->setAttribute ("theme", prefs.theme);
    app->setAttribute ("netEnabled", prefs.netEnabled);
    if (prefs.netPort != net::kDefaultPort) app->setAttribute ("netPort", prefs.netPort);
    if (prefs.netName.isNotEmpty()) app->setAttribute ("netName", prefs.netName);
    if (prefs.netPassword.isNotEmpty()) app->setAttribute ("netKey", protectedNetPassword (prefs.netPassword));
    if (! prefs.netPeers.isEmpty()) app->setAttribute ("netPeers", prefs.netPeers.joinIntoString (";"));
    app->setAttribute ("remoteEnabled", prefs.remoteEnabled);
    if (prefs.remotePort != remote::kDefaultPort) app->setAttribute ("remotePort", prefs.remotePort);
    if (prefs.remoteCode.isNotEmpty()) app->setAttribute ("remoteKey", protectedRemoteCode (prefs.remoteCode));
    app->setAttribute ("updateCheck", prefs.updateCheck);
    app->setAttribute ("updateBeta", prefs.updateBeta);
    if (prefs.updateSkip.isNotEmpty()) app->setAttribute ("updateSkip", prefs.updateSkip);
    if (! prefs.setupDone) app->setAttribute ("setupDone", false);
    if (prefs.tour.isNotEmpty()) app->setAttribute ("tour", prefs.tour);

    if (prefs.netDeviceId.isNotEmpty()) app->setAttribute ("netDeviceId", prefs.netDeviceId);
    if (! prefs.netTrusted.isEmpty())
    {
        auto* nt = root.createNewChildElement ("NETTRUST");
        for (auto& t : prefs.netTrusted)
        {
            auto* e = nt->createNewChildElement ("DEVICE");
            e->setAttribute ("id", t.upToFirstOccurrenceOf ("|", false, false));
            e->setAttribute ("kind", t.fromFirstOccurrenceOf ("|", false, false).upToFirstOccurrenceOf ("|", false, false).getIntValue());
            e->setAttribute ("name", t.fromFirstOccurrenceOf ("|", false, false).fromFirstOccurrenceOf ("|", false, false));
        }
    }

    if (! vacHidden.empty())
    {
        auto* vh = root.createNewChildElement ("VACHIDDEN");
        for (auto& id : vacHidden) vh->createNewChildElement ("ENDPOINT")->setAttribute ("id", id);
    }

    if (! deviceRenames.empty())
    {
        auto* dn = root.createNewChildElement ("DEVICENAMES");
        for (auto& r : deviceRenames)
        {
            auto* e = dn->createNewChildElement ("DEVICE");
            e->setAttribute ("id", r.id);
            e->setAttribute ("original", r.original);
            e->setAttribute ("applied", r.applied);
        }
    }

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
        prefs.stripSplit2 = juce::jlimit (-1.0f, 1.0f, (float) app->getDoubleAttribute ("stripSplit2", -1.0));
        prefs.renameDevices = app->getBoolAttribute ("renameDevices", false);
        prefs.vacDirect = app->getBoolAttribute ("vacDirect", true);
        prefs.vacHideOwnSide = app->getBoolAttribute ("vacHideOwnSide", true);
        prefs.vacHideUnused = app->getBoolAttribute ("vacHideUnused", true);
        prefs.vacAutoName = app->getBoolAttribute ("vacAutoName", true);
        prefs.renameNoPrompt = app->getBoolAttribute ("renameNoPrompt", true);
        prefs.autoReconnect = app->getBoolAttribute ("autoReconnect", true);
        prefs.expertDeviceModes = app->getBoolAttribute ("expertDeviceModes", false);
        prefs.pickerShowAll = app->getBoolAttribute ("pickerShowAll", false);
        prefs.eqShowInput = app->getBoolAttribute ("eqShowInput", false);
        prefs.adaptiveStrips = app->getBoolAttribute ("adaptiveStrips", true);
        prefs.sidePanel = app->getBoolAttribute ("sidePanel", false);
        prefs.fxBrowser = app->getBoolAttribute ("fxBrowser", false);
        prefs.fxWarnOff = app->getStringAttribute ("fxWarnOff");
        prefs.horizontalStrips = app->getBoolAttribute ("horizontalStrips", false);
        prefs.fitWindow = app->getBoolAttribute ("fitWindow", false);
        prefs.stripDividers = app->getBoolAttribute ("stripDividers", true);
        prefs.theme = app->getStringAttribute ("theme", "default");
        prefs.netEnabled = app->getBoolAttribute ("netEnabled", false);
        prefs.netPort = juce::jlimit (1, 65535, app->getIntAttribute ("netPort", net::kDefaultPort));
        prefs.netName = app->getStringAttribute ("netName");
        // Encrypted for the Windows user who saved it: on another PC / account it is empty (enter it again there).
        prefs.netPassword = net::unprotectForUser (app->getStringAttribute ("netKey"));
        prefs.netPeers = juce::StringArray::fromTokens (app->getStringAttribute ("netPeers"), ";", {});
        prefs.netPeers.trim();
        prefs.netPeers.removeEmptyStrings();
        prefs.remoteEnabled = app->getBoolAttribute ("remoteEnabled", false);
        prefs.remotePort = juce::jlimit (1, 65535, app->getIntAttribute ("remotePort", remote::kDefaultPort));
        // Encrypted for this Windows user: on another PC / account it is empty and a new code is made.
        prefs.remoteCode = net::unprotectForUser (app->getStringAttribute ("remoteKey"));
        prefs.updateCheck = app->getBoolAttribute ("updateCheck", true);
        prefs.updateBeta = app->getBoolAttribute ("updateBeta", false);
        prefs.updateSkip = app->getStringAttribute ("updateSkip");
        prefs.setupDone = app->getBoolAttribute ("setupDone", true);   // sessions from before the wizard: set up
        prefs.tour = app->getStringAttribute ("tour");
    }
    // Renamed devices belong to this PC, not to the file: an imported session adds its entries but never
    // drops the ones we know (their original names would be lost).
    // Like the renames: endpoints hidden on this PC are only ever added (so they can be shown again).
    if (auto* app = root->getChildByName ("APP"))
        if (app->hasAttribute ("netDeviceId")) prefs.netDeviceId = app->getStringAttribute ("netDeviceId");
    if (auto* nt = root->getChildByName ("NETTRUST"))
    {
        prefs.netTrusted.clear();
        for (auto* e : nt->getChildWithTagNameIterator ("DEVICE"))
            if (const auto id = e->getStringAttribute ("id"); id.isNotEmpty())
                prefs.netTrusted.add (id + "|" + juce::String (e->getIntAttribute ("kind")) + "|" + e->getStringAttribute ("name"));
    }
    if (auto* vh = root->getChildByName ("VACHIDDEN"))
        for (auto* e : vh->getChildWithTagNameIterator ("ENDPOINT"))
            if (const auto id = e->getStringAttribute ("id"); id.isNotEmpty() && ! std::count (vacHidden.begin(), vacHidden.end(), id))
                vacHidden.push_back (id);
    if (auto* dn = root->getChildByName ("DEVICENAMES"))
        for (auto* e : dn->getChildWithTagNameIterator ("DEVICE"))
        {
            DeviceRename r { e->getStringAttribute ("id"), e->getStringAttribute ("original"), e->getStringAttribute ("applied") };
            const bool known = std::any_of (deviceRenames.begin(), deviceRenames.end(), [&] (const DeviceRename& d) { return d.id == r.id; });
            if (r.id.isNotEmpty() && r.original.isNotEmpty() && ! known) deviceRenames.push_back (r);
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
            // Version < 3: the first limiter of every bus was on by default (not stored while on). It stays on.
            else if (fileVersion < 3 && p->id.startsWith ("bus.") && p->id.endsWith (".lim.on")) v = 1.0f;
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
        auto* fc = e->getChildByName ("FXCORRECTION");
        engine.setCorrection (i, CorrectionProfile::fromXml (fc != nullptr ? fc->getChildByName ("CORRECTION") : nullptr));
    }
    for (auto* e : root->getChildWithTagNameIterator ("BUS"))
    {
        const int b = e->getIntAttribute ("index", -1);
        if (! juce::isPositiveAndBelow (b, kMaxBuses)) continue;
        auto& bus = engine.buses[(size_t) b];
        bus.name = e->getStringAttribute ("name", bus.name);
        if (isDefaultBusName (b, bus.name)) bus.name = busLabel (b);   // "A1" of older sessions -> "H1"
        bus.icon = e->getStringAttribute ("icon");
        bus.output = IoTarget::fromXml (e->getChildByName ("OUTPUT"));
        engine.setCorrection (kMaxInputs + b, CorrectionProfile::fromXml (e->getChildByName ("CORRECTION")));
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

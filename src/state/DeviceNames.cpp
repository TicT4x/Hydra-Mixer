#include "DeviceNames.h"
#include "state/VacControl.h"

#include <windows.h>
#include <shellapi.h>
#include <mmdeviceapi.h>

#pragma comment (lib, "shell32.lib")

namespace hydra
{
namespace
{
// PKEY_Device_DeviceDesc / FriendlyName / EnumeratorName (functiondiscoverykeys_devpkey.h)
constexpr GUID kDeviceKeys { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } };
const PROPERTYKEY kDeviceDesc   { kDeviceKeys, 2 };
const PROPERTYKEY kFriendlyName { kDeviceKeys, 14 };
const PROPERTYKEY kEnumerator   { kDeviceKeys, 24 };

template <typename T> struct Com
{
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

/** COM on a worker thread / in the helper (the message thread already has it). */
struct ComScope
{
    HRESULT hr = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    ~ComScope() { if (SUCCEEDED (hr)) CoUninitialize(); }
};

bool enumerator (Com<IMMDeviceEnumerator>& en)
{
    return SUCCEEDED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                        __uuidof (IMMDeviceEnumerator), (void**) &en));
}

juce::String readString (IPropertyStore* ps, const PROPERTYKEY& k)
{
    PROPVARIANT v;
    PropVariantInit (&v);
    juce::String s;
    if (SUCCEEDED (ps->GetValue (k, &v)) && v.vt == VT_LPWSTR) s = juce::String (v.pwszVal);
    PropVariantClear (&v);
    return s;
}

struct Endpoint
{
    juce::String id, friendly, desc, enumerator;
    bool capture = false;   // recording side

    /** Software device (virtual driver): root-enumerated, or a known virtual device by name. */
    bool isSoftware() const
    {
        return enumerator.equalsIgnoreCase ("ROOT") || friendly.containsIgnoreCase ("Voicemeeter")
            || friendly.containsIgnoreCase ("Cable") || friendly.containsIgnoreCase ("Virtual");
    }
};

bool readEndpoint (IMMDevice* d, Endpoint& out)
{
    LPWSTR id = nullptr;
    if (FAILED (d->GetId (&id))) return false;
    out.id = juce::String (id);
    CoTaskMemFree (id);
    Com<IMMEndpoint> ep;
    EDataFlow flow = eRender;
    if (SUCCEEDED (d->QueryInterface (__uuidof (IMMEndpoint), (void**) &ep)) && SUCCEEDED (ep->GetDataFlow (&flow)))
        out.capture = flow == eCapture;
    Com<IPropertyStore> ps;
    if (FAILED (d->OpenPropertyStore (STGM_READ, &ps))) return false;
    out.friendly = readString (ps.p, kFriendlyName);
    out.desc = readString (ps.p, kDeviceDesc);
    out.enumerator = readString (ps.p, kEnumerator);
    return true;
}

std::vector<Endpoint> listEndpoints (EDataFlow flow)
{
    std::vector<Endpoint> list;
    Com<IMMDeviceEnumerator> en;
    if (! enumerator (en)) return list;
    Com<IMMDeviceCollection> col;
    if (FAILED (en->EnumAudioEndpoints (flow, DEVICE_STATE_ACTIVE, &col))) return list;
    UINT count = 0;
    col->GetCount (&count);
    for (UINT i = 0; i < count; ++i)
    {
        Com<IMMDevice> d;
        Endpoint e;
        if (SUCCEEDED (col->Item (i, &d)) && readEndpoint (d.p, e)) list.push_back (e);
    }
    return list;
}

bool readEndpoint (const juce::String& id, Endpoint& out)
{
    Com<IMMDeviceEnumerator> en;
    Com<IMMDevice> d;
    return enumerator (en) && SUCCEEDED (en->GetDevice (id.toWideCharPointer(), &d)) && readEndpoint (d.p, out);
}

/** Sets the device description (the part of the name before the brackets). Needs administrator rights. */
HRESULT writeDesc (const juce::String& id, const juce::String& desc)
{
    Com<IMMDeviceEnumerator> en;
    if (! enumerator (en)) return E_FAIL;
    Com<IMMDevice> d;
    HRESULT hr = en->GetDevice (id.toWideCharPointer(), &d);
    if (FAILED (hr)) return hr;
    Com<IPropertyStore> ps;
    hr = d->OpenPropertyStore (STGM_READWRITE, &ps);
    if (FAILED (hr)) return hr;
    PROPVARIANT v;
    PropVariantInit (&v);
    v.vt = VT_LPWSTR;
    v.pwszVal = const_cast<wchar_t*> (desc.toWideCharPointer());   // not owned: no PropVariantClear
    hr = ps->SetValue (kDeviceDesc, v);
    if (SUCCEEDED (hr)) hr = ps->Commit();
    return hr;
}

const Endpoint* byFriendly (const std::vector<Endpoint>& list, const juce::String& name)
{
    for (auto& e : list) if (e.friendly == name) return &e;
    return nullptr;
}

bool isWindowsAudio (const IoTarget& t) { return t.kind == IoTarget::Kind::Device && t.typeName.startsWith ("Windows Audio"); }

//==============================================================================
// Rename task (Task Scheduler): renames later batches elevated without a UAC prompt.
const juce::String kTaskName ("Hydra Mixer Device Names");

juce::File exeFile() { return juce::File::getSpecialLocation (juce::File::currentExecutableFile); }
juce::File taskJobFile() { return appDataDir().getChildFile ("device-names-job.xml"); }

int schtasks (const juce::StringArray& args, juce::String* output = nullptr)
{
    juce::StringArray cmd ("schtasks.exe");
    cmd.addArray (args);
    juce::ChildProcess p;
    if (! p.start (cmd, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr)) return -1;
    const auto text = p.readAllProcessOutput();
    if (! p.waitForProcessToFinish (10000)) { p.kill(); return -1; }
    if (output != nullptr) *output = text;
    return (int) p.getExitCode();
}

juce::String taskXml()
{
    auto esc = [] (const juce::String& t) { return t.replace ("&", "&amp;").replace ("<", "&lt;").replace (">", "&gt;").replace ("\"", "&quot;"); };
    const auto domain = juce::SystemStats::getEnvironmentVariable ("USERDOMAIN", {});
    const auto user = domain.isNotEmpty() ? domain + "\\" + juce::SystemStats::getLogonName() : juce::SystemStats::getLogonName();
    juce::String x;
    x << "<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
      << "<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
      << "  <RegistrationInfo><Description>Renames Hydra Mixer's virtual audio devices in Windows (started by Hydra Mixer "
         "when needed, no trigger). Delete it or switch off the option in Hydra Mixer to be asked each time again.</Description></RegistrationInfo>\r\n"
      << "  <Principals><Principal id=\"Author\"><UserId>" << esc (user) << "</UserId>"
      << "<LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>\r\n"
      << "  <Settings>\r\n"
      << "    <MultipleInstancesPolicy>Queue</MultipleInstancesPolicy>\r\n"
      << "    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n"
      << "    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n"
      << "    <AllowStartOnDemand>true</AllowStartOnDemand>\r\n"
      << "    <Enabled>true</Enabled>\r\n"
      << "    <Hidden>true</Hidden>\r\n"
      << "    <ExecutionTimeLimit>PT1M</ExecutionTimeLimit>\r\n"
      << "    <Priority>4</Priority>\r\n"
      << "  </Settings>\r\n"
      << "  <Actions Context=\"Author\"><Exec>"
      << "<Command>\"" << esc (exeFile().getFullPathName()) << "\"</Command>"
      << "<Arguments>" << DeviceNamer::kJobArgument << " &quot;" << esc (taskJobFile().getFullPathName()) << "&quot;</Arguments>"
      << "</Exec></Actions>\r\n"
      << "</Task>\r\n";
    return x;
}

/** Elevated (helper): registers the task for this exe. */
bool installTask()
{
    auto file = juce::File::createTempFile (".xml");
    file.replaceWithText (taskXml(), true, true, "\r\n");   // schtasks wants UTF-16
    juce::String out;
    const bool ok = schtasks ({ "/Create", "/F", "/TN", kTaskName, "/XML", file.getFullPathName() }, &out) == 0;
    file.deleteFile();
    return ok;
}

std::atomic<int> taskState { -1 };   // cached isTaskReady: -1 unknown, 0 no, 1 yes

/** Starts the task with the job and waits until the helper has deleted the job file. */
bool runViaTask (const juce::XmlElement& job)
{
    const auto file = taskJobFile();
    if (! job.writeTo (file)) return false;
    if (schtasks ({ "/Run", "/TN", kTaskName }) != 0) { file.deleteFile(); taskState = 0; return false; }
    for (int i = 0; i < 150 && file.existsAsFile(); ++i) juce::Thread::sleep (100);   // max. 15 s
    if (file.existsAsFile()) { file.deleteFile(); return false; }
    return true;
}
} // namespace

juce::String DeviceNamer::taskDefinition() { return taskXml(); }

bool DeviceNamer::isTaskReady()
{
    if (const int st = taskState.load(); st >= 0) return st == 1;
    juce::String xml;
    const bool ok = schtasks ({ "/Query", "/TN", kTaskName, "/XML" }, &xml) == 0
                 && xml.containsIgnoreCase (exeFile().getFullPathName().replace ("&", "&amp;"));
    taskState = ok ? 1 : 0;
    return ok;
}

void DeviceNamer::removeTask()
{
    juce::Thread::launch ([]
    {
        if (schtasks ({ "/Query", "/TN", kTaskName }) != 0) { taskState = 0; return; }
        if (schtasks ({ "/Delete", "/F", "/TN", kTaskName }) == 0) { taskState = 0; return; }
        // Registered elevated: deleting may need administrator rights as well.
        juce::XmlElement x ("HYDRADEVICENAMES");
        x.setAttribute ("removeTask", 1);
        const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("hydra-device-names", ".xml");
        if (! x.writeTo (file)) return;
        const auto exe = exeFile().getFullPathName();
        const auto args = juce::String (kJobArgument) + " \"" + file.getFullPathName() + "\"";
        SHELLEXECUTEINFOW sei {};
        sei.cbSize = sizeof (sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
        sei.lpVerb = L"runas";
        sei.lpFile = exe.toWideCharPointer();
        sei.lpParameters = args.toWideCharPointer();
        sei.nShow = SW_HIDE;
        if (ShellExecuteExW (&sei) && sei.hProcess != nullptr)
        {
            WaitForSingleObject (sei.hProcess, 30000);
            CloseHandle (sei.hProcess);
        }
        file.deleteFile();
        taskState = -1;
    });
}

//==============================================================================
struct DeviceNamer::Job
{
    struct Item
    {
        juce::String id, desc, original;   // desc = name to set; original = name before Hydra renamed it
        bool restore = false;
        juce::String oldFriendly, newFriendly;
        bool capture = false;   // recording side: only sources recording it follow the new name
        bool ok = false;
    };
    std::vector<Item> items;
    bool allowPrompt = false, declined = false;
    bool manual = false;               // renameNow: by hand, not recorded
    std::function<void (bool)> done;
};

DeviceNamer::DeviceNamer (Engine& e, std::vector<DeviceRename>& r) : engine (e), renames (r) {}
DeviceNamer::~DeviceNamer() { alive->store (false); }

juce::String DeviceNamer::nameFor (const juce::String& channelName, const juce::String& code)
{
    const auto n = channelName.trim();
    if (n.isEmpty()) return "Hydra " + code;
    return n.startsWithIgnoreCase ("Hydra") ? n : "Hydra " + n;
}

juce::String DeviceNamer::cableNameFor (bool input, int index, const juce::String& channelName)
{
    const auto base = input ? "Hydra In " + juce::String (index + 1)
                            : "Hydra Out " + (isHwBus (index) ? busLabel (index) : juce::String (index - kMaxHwBuses + 1));
    // Default channel names ("Input 4", "In 4", "B1") add nothing.
    const auto n = channelName.trim();
    const bool own = n.isNotEmpty() && (input ? n != "Input " + juce::String (index + 1) && n != "In " + juce::String (index + 1)
                                              : ! isDefaultBusName (index, n));
    return own ? base + " - " + n : base;
}

juce::String DeviceNamer::originalFriendly (const juce::String& friendly, const juce::String& desc, const DeviceRename* r)
{
    if (r == nullptr || desc != r->applied || ! friendly.startsWith (desc)) return friendly;
    return r->original + friendly.substring (desc.length());
}

juce::String DeviceNamer::stateKey (bool enabled, bool cables) const
{
    juce::String k (enabled ? "1" : "0");
    k << (cables ? "1" : "0");
    if (! enabled && ! cables) return k + juce::String ((int) renames.size());
    for (int i = 0; i < kMaxInputs; ++i)
    {
        const auto& s = engine.inputs[(size_t) i];
        if (engine.isInputActive (i)) k << '|' << (int) s.source.kind << s.source.typeName << '/' << s.source.deviceName << '=' << s.name;
    }
    for (int b = 0; b < kMaxBuses; ++b)
    {
        const auto& bus = engine.buses[(size_t) b];
        if (engine.isBusActive (b)) k << '|' << (int) bus.output.kind << bus.output.typeName << '/' << bus.output.deviceName << '=' << bus.name;
    }
    return k;
}

std::vector<DeviceNamer::Want> DeviceNamer::resolve (bool enabled, bool cablesToo, std::vector<juce::String>& keep) const
{
    const auto renders = listEndpoints (eRender), captures = listEndpoints (eCapture);
    // Virtual Audio Cable: which endpoints (active or hidden) belong to which cable, from the driver.
    const auto vacEps = cablesToo || enabled ? vac::endpoints() : std::vector<vac::Endpoint>();
    auto vacCable = [&] (const IoTarget& t, bool capture)
    {
        if (! isWindowsAudio (t)) return 0;
        for (auto& e : vacEps) if (e.friendly == t.deviceName && e.capture == capture) return e.cable;
        return 0;
    };

    // Cables pair their two sides by name, so a renamed side is matched under its original name.
    auto renameOf = [this] (const juce::String& id) -> const DeviceRename*
    {
        for (auto& r : renames) if (r.id == id) return &r;
        return nullptr;
    };
    juce::StringArray outs, ins;
    for (auto& e : renders)  outs.add (originalFriendly (e.friendly, e.desc, renameOf (e.id)));
    for (auto& e : captures) ins.add (originalFriendly (e.friendly, e.desc, renameOf (e.id)));
    const auto cables = findVirtualCables (outs, ins);

    // The other side of a cable: endpoint (by current name) -> cable -> other endpoint.
    auto otherSide = [&] (const juce::String& name, bool fromRecording) -> const Endpoint*
    {
        const auto& own = fromRecording ? captures : renders;
        const auto& ownNames = fromRecording ? ins : outs;
        const auto& other = fromRecording ? renders : captures;
        const auto& otherNames = fromRecording ? outs : ins;
        for (size_t i = 0; i < own.size(); ++i)
        {
            if (own[i].friendly != name) continue;
            for (auto& c : cables)
                if ((fromRecording ? c.recordingName : c.playbackName) == ownNames[(int) i])
                {
                    const int k = otherNames.indexOf (fromRecording ? c.playbackName : c.recordingName);
                    return k >= 0 ? &other[(size_t) k] : nullptr;
                }
        }
        return nullptr;
    };

    std::vector<Want> wants;
    auto add = [&] (const Endpoint* e, const juce::String& name)
    {
        if (e == nullptr) return;
        for (auto& w : wants) if (w.id == e->id) return;   // two channels on one device: the first names it
        wants.push_back ({ e->id, name });
    };

    // The side apps use gets the full name ("Hydra In 4 - Discord"), Hydra's own side the plain one ("Hydra In 4"): it changes only when the cable
    // gets another channel, not when the channel is renamed, so Hydra's stream is not reopened while the user types a name.
    auto addCable = [&] (int k, bool appsCapture, const juce::String& name)
    {
        for (auto& e : vacEps)
            if (e.cable == k && e.capture == appsCapture
                && std::none_of (wants.begin(), wants.end(), [&] (const Want& w) { return w.id == e.id; }))
                wants.push_back ({ e.id, name });
    };

    for (int i = 0; i < kMaxInputs; ++i)
    {
        if (! engine.isInputActive (i)) continue;
        const auto& s = engine.inputs[(size_t) i];
        if (const int k = vacCable (s.source, true); k > 0)
        {
            if (cablesToo) { addCable (k, false, cableNameFor (true, i, s.name)); addCable (k, true, cableNameFor (true, i)); }
            if (cablesToo || ! enabled) continue;
        }
        if (! enabled) continue;
        const auto name = nameFor (s.name, "In " + juce::String (i + 1));
        if (s.source.kind == IoTarget::Kind::Loopback)
        {
            if (auto* e = byFriendly (renders, s.source.deviceName); e != nullptr && e->isSoftware()) add (e, name);
        }
        else if (isWindowsAudio (s.source))
            add (otherSide (s.source.deviceName, true), name);
    }
    for (int b = 0; b < kMaxBuses; ++b)
    {
        if (! engine.isBusActive (b)) continue;
        const auto& bus = engine.buses[(size_t) b];
        if (const int k = vacCable (bus.output, false); k > 0)
        {
            if (cablesToo) { addCable (k, true, cableNameFor (false, b, bus.name)); addCable (k, false, cableNameFor (false, b)); }
            if (cablesToo) continue;
        }
        if (enabled && isWindowsAudio (bus.output))
            add (otherSide (bus.output.deviceName, false), nameFor (bus.name, busLabel (b)));
    }
    // Unused cables keep the name Hydra gave them (hidden in Windows anyway; no UAC prompt for nothing).
    if (cablesToo)
        for (auto& e : vacEps)
            if (std::none_of (wants.begin(), wants.end(), [&] (const Want& w) { return w.id == e.id; })) keep.push_back (e.id);
    return wants;
}

void DeviceNamer::tick (bool enabled, bool cables)
{
    if (busy) return;
    const auto key = stateKey (enabled, cables);
    if (key == lastKey) return;
    const bool allowPrompt = lastKey.isNotEmpty();
    lastKey = key;
    if (! enabled && ! cables && renames.empty()) return;
    std::vector<juce::String> keep;
    const auto wants = enabled || cables ? resolve (enabled, cables, keep) : std::vector<Want>();
    start (wants, allowPrompt, false, {}, keep);
}

void DeviceNamer::renameNow (const std::vector<std::pair<juce::String, juce::String>>& names, std::function<void (bool)> done)
{
    if (busy) { if (done) done (false); return; }
    std::vector<Want> wants;
    for (auto& [id, name] : names) wants.push_back ({ id, name });
    start (wants, true, true, std::move (done));
}

void DeviceNamer::start (const std::vector<Want>& wants, bool allowPrompt, bool manual, std::function<void (bool)> done,
                         const std::vector<juce::String>& keep)
{
    auto job = std::make_shared<Job>();
    job->allowPrompt = allowPrompt;
    job->manual = manual;
    job->done = std::move (done);

    for (auto& w : wants)
    {
        Endpoint e;
        if (! readEndpoint (w.id, e) || e.desc == w.name) continue;
        Job::Item it;
        it.id = w.id;
        it.desc = w.name;
        it.original = e.desc;
        for (auto& r : renames) if (r.id == w.id) it.original = r.original;
        it.oldFriendly = e.friendly;
        it.capture = e.capture;
        job->items.push_back (it);
    }

    for (auto r = renames.begin(); ! manual && r != renames.end();)
    {
        const bool wanted = std::any_of (wants.begin(), wants.end(), [&] (const Want& w) { return w.id == r->id; })
                         || std::find (keep.begin(), keep.end(), r->id) != keep.end();
        Endpoint e;
        if (wanted || ! readEndpoint (r->id, e)) { ++r; continue; }   // device unplugged: restore later
        if (e.desc != r->applied) { r = renames.erase (r); continue; } // renamed by someone else since: leave it
        Job::Item it;
        it.id = r->id;
        it.desc = it.original = r->original;
        it.restore = true;
        it.oldFriendly = e.friendly;
        it.capture = e.capture;
        job->items.push_back (it);
        ++r;
    }

    if (job->items.empty()) { if (job->done) job->done (true); return; }
    busy = true;

    const bool withTask = useTask;
    juce::Thread::launch ([this, job, withTask, token = alive]
    {
        ComScope com;
        juce::StringArray denied;
        for (auto& it : job->items)
            if (FAILED (writeDesc (it.id, it.desc))) denied.add (it.id);

        juce::XmlElement x ("HYDRADEVICENAMES");
        for (auto& it : job->items)
            if (denied.contains (it.id))
            {
                auto* e = x.createNewChildElement ("SET");
                e->setAttribute ("id", it.id);
                e->setAttribute ("name", it.desc);
            }

        // Administrator rights needed: the rename task does it without a prompt, once it is set up.
        if (! denied.isEmpty() && withTask && isTaskReady() && runViaTask (x))
        {
            denied.clear();
            for (auto& it : job->items)
            {
                Endpoint e;
                if (! readEndpoint (it.id, e) || e.desc != it.desc) denied.add (it.id);
            }
        }

        if (! denied.isEmpty() && job->allowPrompt)
        {
            // Otherwise the same exe applies the batch elevated (one UAC prompt) and sets up the task for next time.
            if (withTask) { x.setAttribute ("installTask", 1); taskState = -1; }
            const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getNonexistentChildFile ("hydra-device-names", ".xml");
            if (x.writeTo (file))
            {
                const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName();
                const auto args = juce::String (kJobArgument) + " \"" + file.getFullPathName() + "\"";
                SHELLEXECUTEINFOW sei {};
                sei.cbSize = sizeof (sei);
                sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
                sei.lpVerb = L"runas";
                sei.lpFile = exe.toWideCharPointer();
                sei.lpParameters = args.toWideCharPointer();
                sei.nShow = SW_HIDE;
                if (ShellExecuteExW (&sei))
                {
                    if (sei.hProcess != nullptr)
                    {
                        WaitForSingleObject (sei.hProcess, 30000);
                        CloseHandle (sei.hProcess);
                    }
                }
                else
                    job->declined = GetLastError() == ERROR_CANCELLED;
                file.deleteFile();
            }
        }

        // What counts is what Windows now reports.
        for (auto& it : job->items)
        {
            Endpoint e;
            it.ok = readEndpoint (it.id, e) && e.desc == it.desc;
            it.newFriendly = it.ok ? e.friendly : it.oldFriendly;
        }

        juce::MessageManager::callAsync ([this, job, token]
        {
            if (token->load()) finish (*job);
        });
    });
}

void DeviceNamer::finish (Job& job)
{
    busy = false;
    int done = 0;
    juce::StringArray failed;
    struct Moved { juce::String from, to; bool capture; };
    std::vector<Moved> moved;   // old friendly name -> new, per side (both sides of a cable may share a name)

    for (auto& it : job.items)
    {
        if (! it.ok) { failed.add ("\"" + it.desc + "\""); continue; }
        ++done;
        auto r = std::find_if (renames.begin(), renames.end(), [&] (const DeviceRename& d) { return d.id == it.id; });
        if (job.manual)            { if (r != renames.end()) renames.erase (r); }
        else if (it.restore)            { if (r != renames.end()) renames.erase (r); }
        else if (r != renames.end()) r->applied = it.desc;
        else                         renames.push_back ({ it.id, it.original, it.desc });
        if (it.newFriendly != it.oldFriendly) moved.push_back ({ it.oldFriendly, it.newFriendly, it.capture });
        logLine ("device name: \"" + it.oldFriendly + "\" -> \"" + it.newFriendly + "\"");
    }

    // Sources / outputs that name a renamed device follow its new name.
    if (! moved.empty())
    {
        for (auto* t : engine.getDeviceTypes())
            if (t->getTypeName().startsWith ("Windows Audio")) t->scanForDevices();
        // A source records a recording device (a loopback: a playback device), an output plays to a playback device.
        auto follow = [&] (IoTarget& t, bool recordsCapture)
        {
            if (t.kind != IoTarget::Kind::Loopback && ! isWindowsAudio (t)) return;
            const bool capture = recordsCapture && t.kind != IoTarget::Kind::Loopback;
            for (auto& m : moved) if (m.capture == capture && t.deviceName == m.from) { t.deviceName = m.to; return; }
        };
        for (auto& s : engine.inputs) follow (s.source, true);
        for (auto& b : engine.buses)  follow (b.output, false);
        engine.rebuildGraph();
    }

    if (failed.isEmpty())
        lastResult = done > 0 ? "Done: " + juce::String (done) + " device name(s) changed." : juce::String();
    else
    {
        lastResult = (job.declined ? "Not renamed (administrator rights declined): "
                                   : job.allowPrompt ? "Windows did not accept the name: " : "Waiting for a change to ask for administrator rights: ")
                   + failed.joinIntoString (", ") + ".";
        logLine ("device name: " + lastResult);
    }
    if (onRenamed) onRenamed();
    if (job.done) job.done (failed.isEmpty());
}

juce::String DeviceNamer::describe() const
{
    juce::String t;
    if (busy) t << "Renaming... (confirm the Windows prompt if it appears)\n";
    for (auto& r : renames) t << "  " << r.original << "  ->  " << r.applied << "\n";
    if (renames.empty() && ! busy) t << "  (no device renamed)\n";
    if (lastResult.isNotEmpty()) t << "\n" << lastResult << "\n";
    return t;
}

int DeviceNamer::runJob (const juce::File& jobFile)
{
    const auto x = juce::XmlDocument::parse (jobFile);
    if (x == nullptr || ! x->hasTagName ("HYDRADEVICENAMES")) { jobFile.deleteFile(); return 1; }
    ComScope com;
    int failures = 0;
    for (auto* e : x->getChildWithTagNameIterator ("SET"))
        if (FAILED (writeDesc (e->getStringAttribute ("id"), e->getStringAttribute ("name")))) ++failures;
    if (x->getBoolAttribute ("installTask") && ! installTask()) ++failures;
    if (x->getBoolAttribute ("removeTask")) schtasks ({ "/Delete", "/F", "/TN", kTaskName });
    jobFile.deleteFile();   // the task's caller waits for this
    return failures;
}
} // namespace hydra

#include "Install.h"
#include "Autostart.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <netfw.h>

namespace hydra::install
{
namespace
{
const wchar_t* kUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\HydraMixer";
const wchar_t* kQuitEvent = L"Local\\HydraMixer.QuitRequest";
constexpr const char* kExeName = "Hydra Mixer.exe";
constexpr const char* kOldName = "Hydra Mixer.old.exe";

juce::File knownFolder (REFKNOWNFOLDERID id)
{
    PWSTR path = nullptr;
    juce::File f;
    if (SUCCEEDED (SHGetKnownFolderPath (id, 0, nullptr, &path)) && path != nullptr) f = juce::File (juce::String (path));
    CoTaskMemFree (path);
    return f;
}

/** One rule per exe path: "Hydra Mixer" for the installed exe, "Hydra Mixer (<path>)" for any other copy
    (a build folder), so allowing one never takes the permission of another. */
juce::String ruleName (const juce::String& exePath)
{
    if (exePath.equalsIgnoreCase (installedExe().getFullPathName())) return "Hydra Mixer";
    return "Hydra Mixer (" + exePath + ")";
}

juce::File startMenuLink() { return knownFolder (FOLDERID_Programs).getChildFile ("Hydra Mixer.lnk"); }
juce::File desktopLink()   { return knownFolder (FOLDERID_Desktop).getChildFile ("Hydra Mixer.lnk"); }

/** Version numbers and the prerelease part ("beta.1") of "v0.9.1-beta.1". */
struct Version
{
    std::vector<int> numbers;
    juce::String pre;
};

Version parse (juce::String s)
{
    s = s.trim();
    if (s.startsWithIgnoreCase ("v")) s = s.substring (1);
    Version v;
    const auto core = s.upToFirstOccurrenceOf ("-", false, false).upToFirstOccurrenceOf ("+", false, false);
    v.pre = s.fromFirstOccurrenceOf ("-", false, false).upToFirstOccurrenceOf ("+", false, false).toLowerCase();
    for (auto& part : juce::StringArray::fromTokens (core, ".", {}))
        v.numbers.push_back (part.getIntValue());
    while (! v.numbers.empty() && v.numbers.back() == 0) v.numbers.pop_back();   // 0.9 == 0.9.0
    return v;
}

/** "beta.10" > "beta.9" > "alpha.3": compares dot-separated parts, numbers as numbers. */
int comparePre (const juce::String& a, const juce::String& b)
{
    const auto pa = juce::StringArray::fromTokens (a, ".", {}), pb = juce::StringArray::fromTokens (b, ".", {});
    for (int i = 0; i < juce::jmax (pa.size(), pb.size()); ++i)
    {
        if (i >= pa.size()) return -1;
        if (i >= pb.size()) return 1;
        const bool na = pa[i].containsOnly ("0123456789"), nb = pb[i].containsOnly ("0123456789");
        int c = 0;
        if (na && nb) c = pa[i].getIntValue() - pb[i].getIntValue();
        else c = pa[i].compare (pb[i]);
        if (c != 0) return c < 0 ? -1 : 1;
    }
    return 0;
}

bool setRegString (HKEY key, const wchar_t* name, const juce::String& value)
{
    const auto w = value.toWideCharPointer();
    return RegSetValueExW (key, name, 0, REG_SZ, (const BYTE*) w, (DWORD) ((wcslen (w) + 1) * sizeof (wchar_t))) == ERROR_SUCCESS;
}

void setRegDword (HKEY key, const wchar_t* name, DWORD value)
{
    RegSetValueExW (key, name, 0, REG_DWORD, (const BYTE*) &value, sizeof (value));
}

juce::String regString (const wchar_t* name)
{
    wchar_t buf[1024] {};
    DWORD size = sizeof (buf);
    if (RegGetValueW (HKEY_CURRENT_USER, kUninstallKey, name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    return juce::String (buf);
}

bool writeUninstallEntry (const juce::String& version, juce::int64 bytes)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW (HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    const auto exe = installedExe().getFullPathName();
    bool ok = setRegString (key, L"DisplayName", "Hydra Mixer");
    ok &= setRegString (key, L"DisplayVersion", version);
    ok &= setRegString (key, L"Publisher", "Hydra");
    ok &= setRegString (key, L"InstallLocation", installDir().getFullPathName());
    ok &= setRegString (key, L"DisplayIcon", exe + ",0");
    ok &= setRegString (key, L"UninstallString", "\"" + exe + "\" " + kUninstallArgument);
    setRegString (key, L"URLInfoAbout", "https://github.com/TicT4x/Hydra-Mixer");
    setRegString (key, L"InstallDate", juce::Time::getCurrentTime().formatted ("%Y%m%d"));
    setRegDword (key, L"EstimatedSize", (DWORD) (bytes / 1024));
    setRegDword (key, L"NoModify", 1);
    setRegDword (key, L"NoRepair", 1);
    RegCloseKey (key);
    return ok;
}

/** Shell link to the installed exe. COM is initialised by JUCE on the message thread; here per call. */
bool makeLink (const juce::File& link, const juce::File& target)
{
    const HRESULT init = CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED);
    bool ok = false;
    IShellLinkW* sl = nullptr;
    if (SUCCEEDED (CoCreateInstance (CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS (&sl))))
    {
        sl->SetPath (target.getFullPathName().toWideCharPointer());
        sl->SetWorkingDirectory (target.getParentDirectory().getFullPathName().toWideCharPointer());
        sl->SetDescription (L"Hydra Mixer - low-latency audio mixer");
        sl->SetIconLocation (target.getFullPathName().toWideCharPointer(), 0);
        IPersistFile* pf = nullptr;
        if (SUCCEEDED (sl->QueryInterface (IID_PPV_ARGS (&pf))))
        {
            link.getParentDirectory().createDirectory();
            ok = SUCCEEDED (pf->Save (link.getFullPathName().toWideCharPointer(), TRUE));
            pf->Release();
        }
        sl->Release();
    }
    if (SUCCEEDED (init)) CoUninitialize();
    return ok;
}

/** Starts exe elevated (UAC prompt) with args and waits for it. Returns its exit code, -1 = declined / failed. */
int runElevated (const juce::File& exe, const juce::String& args, DWORD timeoutMs)
{
    const auto file = exe.getFullPathName();
    SHELLEXECUTEINFOW sei {};
    sei.cbSize = sizeof (sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas";
    sei.lpFile = file.toWideCharPointer();
    sei.lpParameters = args.toWideCharPointer();
    sei.nShow = SW_HIDE;
    if (! ShellExecuteExW (&sei) || sei.hProcess == nullptr) return -1;
    int code = -1;
    if (WaitForSingleObject (sei.hProcess, timeoutMs) == WAIT_OBJECT_0)
    {
        DWORD c = 0;
        if (GetExitCodeProcess (sei.hProcess, &c)) code = (int) c;
    }
    CloseHandle (sei.hProcess);
    return code;
}

/** Runs f with the firewall policy (COM initialised for this call). */
template <typename F>
bool withFirewall (F&& f)
{
    const HRESULT init = CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED);
    bool ok = false;
    INetFwPolicy2* policy = nullptr;
    if (SUCCEEDED (CoCreateInstance (__uuidof (NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER, __uuidof (INetFwPolicy2), (void**) &policy)))
    {
        INetFwRules* rules = nullptr;
        if (SUCCEEDED (policy->get_Rules (&rules)))
        {
            ok = f (*rules);
            rules->Release();
        }
        policy->Release();
    }
    if (SUCCEEDED (init)) CoUninitialize();
    return ok;
}
} // namespace

//==============================================================================
juce::String currentVersion()
{
   #ifdef JUCE_APPLICATION_VERSION_STRING
    juce::String v (JUCE_APPLICATION_VERSION_STRING);
   #else
    juce::String v ("0.0.0");   // tools (HydraShots) without the app's version
   #endif
    if (juce::String (HYDRA_VERSION_SUFFIX).isNotEmpty()) v << "-" << HYDRA_VERSION_SUFFIX;
    return v;
}

int compareVersions (const juce::String& a, const juce::String& b)
{
    const auto va = parse (a), vb = parse (b);
    for (size_t i = 0; i < juce::jmax (va.numbers.size(), vb.numbers.size()); ++i)
    {
        const int x = i < va.numbers.size() ? va.numbers[i] : 0, y = i < vb.numbers.size() ? vb.numbers[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    if (va.pre == vb.pre) return 0;
    if (va.pre.isEmpty()) return 1;    // 0.9 > 0.9-beta.3
    if (vb.pre.isEmpty()) return -1;
    return comparePre (va.pre, vb.pre);
}

juce::File installDir()   { return knownFolder (FOLDERID_UserProgramFiles).getChildFile ("Hydra Mixer"); }
juce::File installedExe() { return installDir().getChildFile (kExeName); }
juce::File runningExe()   { return juce::File::getSpecialLocation (juce::File::currentExecutableFile); }

bool isRunningInstalled()
{
    return runningExe().getFullPathName().equalsIgnoreCase (installedExe().getFullPathName());
}

bool isInstalled() { return installedExe().existsAsFile(); }

juce::String installedVersion() { return regString (L"DisplayVersion"); }

bool shouldOfferInstall (const juce::String& commandLine)
{
    if (commandLine.contains (kInstallArgument)) return ! isRunningInstalled();
    if (isRunningInstalled()) return false;
    // Downloaded release: "HydraMixer-0.9-win64.exe", "HydraMixer-0.9-win64 (1).exe" (browser copies).
    const auto name = runningExe().getFileName();
    return name.startsWithIgnoreCase ("HydraMixer-") && name.containsIgnoreCase ("win64");
}

juce::String installFrom (const juce::File& source, const juce::String& version, const Options& options)
{
    const auto dir = installDir();
    if (! dir.createDirectory()) return "Cannot create " + dir.getFullPathName();
    const auto target = installedExe();
    const auto old = dir.getChildFile (kOldName);
    if (! source.getFullPathName().equalsIgnoreCase (target.getFullPathName()))
    {
        // Copy next to the target first (same volume), then swap names: the installed exe may be running.
        const auto fresh = dir.getChildFile ("Hydra Mixer.new.exe");
        fresh.deleteFile();
        if (! source.copyFileTo (fresh)) return "Cannot write " + fresh.getFullPathName() + " (disk full?)";
        if (fresh.getSize() != source.getSize()) { fresh.deleteFile(); return "The copy is incomplete"; }
        old.deleteFile();   // an older leftover (fails if it is still running: then take another name)
        if (target.existsAsFile())
        {
            auto aside = old.exists() ? dir.getNonexistentChildFile ("Hydra Mixer.old", ".exe", false) : old;
            if (! target.moveFileTo (aside)) { fresh.deleteFile(); return "Cannot replace " + target.getFullPathName(); }
        }
        if (! fresh.moveFileTo (target)) return "Cannot move the new exe into place";
    }
    writeUninstallEntry (version, target.getSize());
    if (options.startMenu) makeLink (startMenuLink(), target);
    if (options.desktop)   makeLink (desktopLink(), target);
    if (options.firewall && ! firewallAllows (target)) requestFirewall (target, true);   // the user may decline: Windows asks later then
    logLine ("installed " + version + " to " + target.getFullPathName());
    return {};
}

bool launchInstalled (bool updated)
{
    juce::String args;
    args << kWaitArgument << " " << (int) GetCurrentProcessId();
    if (updated) args << " " << kUpdatedArgument;
    return installedExe().startAsProcess (args);
}

void uninstall (bool keepSettings)
{
    juce::String error;
    if (Autostart::isEnabled()) Autostart::setEnabled (false);
    // The task that renames devices without a UAC prompt (DeviceNamer): may need administrator rights; best effort.
    juce::ChildProcess p;
    if (p.start (juce::StringArray { "schtasks.exe", "/Delete", "/F", "/TN", "Hydra Mixer Device Names" })) p.waitForProcessToFinish (10000);
    startMenuLink().deleteFile();
    desktopLink().deleteFile();
    if (firewallAllows (installedExe())) requestFirewall (installedExe(), false);
    RegDeleteTreeW (HKEY_CURRENT_USER, kUninstallKey);
    if (! keepSettings) appDataDir().deleteRecursively();
    // This exe runs from the folder: a hidden cmd deletes it a moment after we quit.
    const auto dir = installDir().getFullPathName();
    juce::String cmd = "cmd.exe /c ping -n 3 127.0.0.1 >nul & rmdir /s /q \"" + dir + "\"";
    STARTUPINFOW si {};
    si.cb = sizeof (si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi {};
    std::wstring line (cmd.toWideCharPointer());
    juce::String sysDir = juce::File::getSpecialLocation (juce::File::windowsSystemDirectory).getFullPathName();
    if (CreateProcessW (nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, sysDir.toWideCharPointer(), &si, &pi))
    {
        CloseHandle (pi.hThread);
        CloseHandle (pi.hProcess);
    }
}

void cleanUpAfterUpdate()
{
    if (! isRunningInstalled()) return;
    for (auto& f : installDir().findChildFiles (juce::File::findFiles, false, "Hydra Mixer.old*.exe"))
        f.deleteFile();   // may still be locked a moment: then next start
    installDir().getChildFile ("Hydra Mixer.new.exe").deleteFile();
    // The downloaded release the update was installed from (Updater writes it here).
    for (auto& f : installDir().findChildFiles (juce::File::findFiles, false, "HydraMixer-*-win64.exe;update-*.part"))
        f.deleteFile();
}

void waitForPreviousInstance (const juce::String& commandLine)
{
    if (! commandLine.contains (kWaitArgument)) return;
    const auto pid = (DWORD) commandLine.fromFirstOccurrenceOf (kWaitArgument, false, false).trim().getLargeIntValue();
    if (pid == 0 || pid == GetCurrentProcessId()) return;
    if (HANDLE h = OpenProcess (SYNCHRONIZE, FALSE, pid))
    {
        WaitForSingleObject (h, 20000);
        CloseHandle (h);
    }
}

//==============================================================================
bool firewallAllows (const juce::File& exe)
{
    const auto path = exe.getFullPathName();
    return withFirewall ([&] (INetFwRules& rules)
    {
        INetFwRule* rule = nullptr;
        BSTR name = SysAllocString (ruleName (path).toWideCharPointer());
        const bool found = SUCCEEDED (rules.Item (name, &rule)) && rule != nullptr;
        SysFreeString (name);
        if (! found) return false;
        bool ok = false;
        BSTR app = nullptr;
        long profiles = 0;
        NET_FW_ACTION action {};
        VARIANT_BOOL enabled = VARIANT_FALSE;
        if (SUCCEEDED (rule->get_ApplicationName (&app)) && app != nullptr && SUCCEEDED (rule->get_Profiles (&profiles))
            && SUCCEEDED (rule->get_Action (&action)) && SUCCEEDED (rule->get_Enabled (&enabled)))
            ok = path.equalsIgnoreCase (juce::String (app)) && action == NET_FW_ACTION_ALLOW && enabled == VARIANT_TRUE
                 && (profiles & NET_FW_PROFILE2_ALL) == NET_FW_PROFILE2_ALL;
        SysFreeString (app);
        rule->Release();
        return ok;
    });
}

bool requestFirewall (const juce::File& exe, bool add)
{
    // The helper is this exe itself (or the installed one: the same code), run elevated.
    const auto args = juce::String (kFirewallArgument) + (add ? " add \"" : " remove \"") + exe.getFullPathName() + "\"";
    const int code = runElevated (runningExe(), args, 30000);
    logLine (juce::String ("firewall rule ") + (add ? "add" : "remove") + (code == 0 ? ": done" : code < 0 ? ": declined / failed" : ": error " + juce::String (code)));
    return code == 0;
}

int runFirewallJob (const juce::String& commandLine)
{
    const auto rest = commandLine.fromFirstOccurrenceOf (kFirewallArgument, false, false).trim();
    const bool add = rest.startsWith ("add");
    const auto path = rest.fromFirstOccurrenceOf (" ", false, false).trim().unquoted();
    const bool ok = withFirewall ([&] (INetFwRules& rules)
    {
        // One rule per path (ruleName): an older one with that name is replaced.
        BSTR name = SysAllocString (ruleName (path).toWideCharPointer());
        for (int i = 0; i < 8; ++i)
        {
            INetFwRule* existing = nullptr;
            if (FAILED (rules.Item (name, &existing)) || existing == nullptr) break;
            existing->Release();
            if (FAILED (rules.Remove (name))) break;
        }
        if (! add) { SysFreeString (name); return true; }
        INetFwRule* rule = nullptr;
        bool done = false;
        if (SUCCEEDED (CoCreateInstance (__uuidof (NetFwRule), nullptr, CLSCTX_INPROC_SERVER, __uuidof (INetFwRule), (void**) &rule)))
        {
            BSTR app = SysAllocString (path.toWideCharPointer());
            BSTR desc = SysAllocString (L"Hydra Link (audio over the network) and Hydra Remote (phone / tablet control)");
            BSTR group = SysAllocString (L"Hydra Mixer");
            rule->put_Name (name);
            rule->put_Description (desc);
            rule->put_Grouping (group);
            rule->put_ApplicationName (app);
            rule->put_Protocol (NET_FW_IP_PROTOCOL_ANY);
            rule->put_Direction (NET_FW_RULE_DIR_IN);
            rule->put_Action (NET_FW_ACTION_ALLOW);
            rule->put_Profiles (NET_FW_PROFILE2_ALL);
            rule->put_Enabled (VARIANT_TRUE);
            done = SUCCEEDED (rules.Add (rule));
            SysFreeString (app);
            SysFreeString (desc);
            SysFreeString (group);
            rule->Release();
        }
        SysFreeString (name);
        return done;
    });
    return ok ? 0 : 2;
}

//==============================================================================
bool closeRunningInstance (int timeoutMs)
{
    HANDLE e = OpenEventW (EVENT_MODIFY_STATE, FALSE, kQuitEvent);
    if (e == nullptr) return true;   // none running
    SetEvent (e);
    CloseHandle (e);
    // Gone when the event no longer exists (the mixer closes its handle when it quits).
    for (int waited = 0; waited < timeoutMs; waited += 100)
    {
        Sleep (100);
        HANDLE again = OpenEventW (SYNCHRONIZE, FALSE, kQuitEvent);
        if (again == nullptr) return true;
        CloseHandle (again);
    }
    return false;
}

bool isMixerRunning()
{
    HANDLE e = OpenEventW (SYNCHRONIZE, FALSE, kQuitEvent);
    if (e == nullptr) return false;
    CloseHandle (e);
    return true;
}

bool quitRequested()
{
    static HANDLE event = CreateEventW (nullptr, FALSE, FALSE, kQuitEvent);   // auto-reset, lives as long as the process
    return event != nullptr && WaitForSingleObject (event, 0) == WAIT_OBJECT_0;
}
} // namespace hydra::install

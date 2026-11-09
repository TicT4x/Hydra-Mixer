#include "VacControl.h"

#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <winioctl.h>
#include <setupapi.h>
#include <shellapi.h>
#include <mmdeviceapi.h>
#include <ks.h>
#include <ksmedia.h>
#include <atomic>

#include "../../external/vacapi/VACAPI.h"

#pragma comment (lib, "setupapi.lib")
#pragma comment (lib, "shell32.lib")

namespace hydra::vac
{
namespace
{
namespace V = VirtualAudioCable;

// Product GUID of the common VAC version (its device interface class), from the API example (params.h).
constexpr GUID kProductGuid { 0x83ed7f0e, 0x2028, 0x4956, { 0xb0, 0xb4, 0x39, 0xc7, 0x6f, 0xda, 0xef, 0x1d } };
constexpr const wchar_t* kApiInterfaceGuid = L"0d13c528-a6fc-47f1-8fee-c5dd46f1fbb1";

/** The driver's device (for the restart) and its control interface. */
struct Driver
{
    HDEVINFO list = INVALID_HANDLE_VALUE;
    SP_DEVINFO_DATA dev {};
    HANDLE handle = INVALID_HANDLE_VALUE;
    juce::String error;

    ~Driver() { close(); freeList(); }

    void close()
    {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle (handle);
        handle = INVALID_HANDLE_VALUE;
    }
    void freeList()
    {
        if (list != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList (list);
        list = INVALID_HANDLE_VALUE;
    }

    bool open()
    {
        close();
        freeList();
        list = SetupDiGetClassDevsW (&kProductGuid, nullptr, nullptr, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
        if (list == INVALID_HANDLE_VALUE) { error = "no device list"; return false; }

        SP_DEVICE_INTERFACE_DATA ifData {};
        ifData.cbSize = sizeof (ifData);
        if (! SetupDiEnumDeviceInterfaces (list, nullptr, &kProductGuid, 0, &ifData))
        {
            error = GetLastError() == ERROR_NO_MORE_ITEMS ? "Virtual Audio Cable is not installed" : "driver interface not found";
            return false;
        }

        struct Detail : SP_DEVICE_INTERFACE_DETAIL_DATA_W { wchar_t more[MAX_PATH]; } detail {};
        detail.cbSize = sizeof (SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        dev = {};
        dev.cbSize = sizeof (dev);
        if (! SetupDiGetDeviceInterfaceDetailW (list, &ifData, &detail, sizeof (detail), nullptr, &dev))
        {
            error = "driver interface details not available";
            return false;
        }

        const auto path = juce::String (detail.DevicePath) + "\\" + juce::String (kApiInterfaceGuid);
        handle = CreateFileW (path.toWideCharPointer(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            error = "cannot open the driver (error " + juce::String ((int) GetLastError()) + ")";
            return false;
        }
        return true;
    }

    bool ioctl (DWORD code, V::DriverReq& req, DWORD reqSize, void* out = nullptr, DWORD outSize = 0) const
    {
        req.ApiVer = apiVersion.load();
        DWORD got = 0;
        return DeviceIoControl (handle, code, &req, reqSize, out, outSize, &got, nullptr) != FALSE;
    }

    /** VACAPI.h is API 6 (VAC 4.70); VAC 4.71 only answers API 7 with the same structures (checked for
        DriverInfo and CableInfo, the driver rejects any size mismatch). The first version that answers is kept. */
    bool driverInfo (V::DriverInfo& di) const
    {
        V::DriverInfoReq r {};
        di = {};
        if (ioctl (V::IoCtl_Local_GetDriverInfo, r, sizeof (r), &di, sizeof (di))) return true;
        const UINT tried = apiVersion.load();
        for (UINT v : { (UINT) ApiVersion, (UINT) ApiVersion + 1 })   // ApiVersion: a macro in VACAPI.h
        {
            if (v == tried) continue;
            apiVersion = v;
            if (ioctl (V::IoCtl_Local_GetDriverInfo, r, sizeof (r), &di, sizeof (di))) return true;
        }
        apiVersion = tried;
        return false;
    }

    static inline std::atomic<UINT> apiVersion { ApiVersion };

    /** Reloads the driver (DIF_PROPERTYCHANGE). Needs administrator rights. */
    Result restart()
    {
        close();   // the driver cannot unload while its interface is open here
        SP_PROPCHANGE_PARAMS pcp {};
        pcp.ClassInstallHeader.cbSize = sizeof (pcp.ClassInstallHeader);
        pcp.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
        pcp.StateChange = DICS_PROPCHANGE;
        pcp.Scope = DICS_FLAG_GLOBAL;
        if (! SetupDiSetClassInstallParamsW (list, &dev, &pcp.ClassInstallHeader, sizeof (pcp))
            || ! SetupDiCallClassInstaller (DIF_PROPERTYCHANGE, list, &dev))
            return Result::restartFailed;

        SP_DEVINSTALL_PARAMS_W dip {};
        dip.cbSize = sizeof (dip);
        if (SetupDiGetDeviceInstallParamsW (list, &dev, &dip) && (dip.Flags & (DI_NEEDRESTART | DI_NEEDREBOOT)) != 0)
            return Result::needsReboot;
        return Result::done;
    }
};
} // namespace

Info query()
{
    Info info;
    Driver d;
    if (! d.open()) { info.error = d.error; return info; }
    V::DriverInfo di;
    if (! d.driverInfo (di))
    {
        info.error = "the driver did not answer (Hydra speaks API 6 and 7, VAC 4.70 / 4.71; error " + juce::String ((int) GetLastError()) + ")";
        return info;
    }
    info.installed = true;
    info.edition = juce::String (GetProductConfig ((di.Flags & V::DriverFlags::Trial) != 0, (di.Flags & V::DriverFlags::Lite) != 0));
    info.version = juce::String ((int) di.VerMajor) + "." + juce::String ((int) di.VerMinor) + "." + juce::String ((int) di.VerRev);
    info.numCables = (int) di.NumCables;
    info.totalStreams = (int) di.TotalStreams;
    info.restartPending = (di.Flags & V::DriverFlags::RestartRequired) != 0;
    return info;
}

juce::StringArray streamOwners()
{
    juce::StringArray owners;
    Driver d;
    V::DriverInfo di;
    if (! d.open() || ! d.driverInfo (di)) return owners;
    std::vector<V::StreamInfo> si (64);
    for (UINT k = 0; k < di.NumCables; ++k)
    {
        V::CableReq r {};
        r.CableNum = k;
        r.ApiVer = Driver::apiVersion.load();
        DWORD got = 0;
        if (! DeviceIoControl (d.handle, V::IoCtl_Local_GetCableStreamsInfo, &r, sizeof (r), si.data(), (DWORD) (si.size() * sizeof (V::StreamInfo)), &got, nullptr))
            continue;
        for (DWORD i = 0; i < got / sizeof (V::StreamInfo); ++i)
            owners.add (juce::String (si[i].ProcessName) + " (cable " + juce::String ((int) k + 1)
                        + ((si[i].Flags & V::StreamFlags::Capture) != 0 ? ", recording)" : ", playback)"));
    }
    return owners;
}

std::vector<Stream> streams()
{
    std::vector<Stream> list;
    Driver d;
    V::DriverInfo di;
    if (! d.open() || ! d.driverInfo (di)) return list;
    std::vector<V::StreamInfo> si (64);
    for (UINT k = 0; k < di.NumCables; ++k)
    {
        V::CableReq r {};
        r.CableNum = k;
        r.ApiVer = Driver::apiVersion.load();
        DWORD got = 0;
        if (! DeviceIoControl (d.handle, V::IoCtl_Local_GetCableStreamsInfo, &r, sizeof (r), si.data(), (DWORD) (si.size() * sizeof (V::StreamInfo)), &got, nullptr))
            continue;
        for (DWORD i = 0; i < got / sizeof (V::StreamInfo); ++i)
            list.push_back ({ (int) k + 1, (si[i].Flags & V::StreamFlags::Capture) != 0, juce::String (si[i].ProcessName), si[i].ProcessId });
    }
    return list;
}

std::vector<int> streamsPerCable()
{
    std::vector<int> streams;
    Driver d;
    V::DriverInfo di;
    if (! d.open() || ! d.driverInfo (di)) return streams;
    for (UINT k = 0; k < di.NumCables; ++k)
    {
        V::CableReq r {};
        r.CableNum = k;
        V::CableInfo ci {};
        if (! d.ioctl (V::IoCtl_Local_GetCableInfo, r, sizeof (r), &ci, sizeof (ci))) return {};
        streams.push_back ((int) (ci.InStreams + ci.OutStreams));
    }
    return streams;
}

std::vector<Cable> cables()
{
    std::vector<Cable> list;
    Driver d;
    V::DriverInfo di;
    if (! d.open() || ! d.driverInfo (di)) return list;
    for (UINT k = 0; k < di.NumCables; ++k)
    {
        V::CableReq r {};
        r.CableNum = k;
        V::CableInfo ci {};
        if (! d.ioctl (V::IoCtl_Local_GetCableInfo, r, sizeof (r), &ci, sizeof (ci))) return {};
        Cable c;
        c.number = (int) k + 1;
        c.msPerInt = (int) ci.MsPerInt;
        c.maxInstances = (int) ci.MaxInstances;
        c.range = { (int) ci.DataRange.MinSampleRate, (int) ci.DataRange.MaxSampleRate, (int) ci.DataRange.MinBitsPerSample,
                    (int) ci.DataRange.MaxBitsPerSample, (int) ci.DataRange.MinNumChannels, (int) ci.DataRange.MaxNumChannels };
        c.rate = (int) ci.Fmt.Format.nSamplesPerSec;
        c.bits = (int) ci.Fmt.Format.wBitsPerSample;
        c.channels = (int) ci.Fmt.Format.nChannels;
        c.inStreams = (int) ci.InStreams;
        c.outStreams = (int) ci.OutStreams;
        c.overflows = (int) ci.DataOverflows;
        c.underflows = (int) ci.DataUnderflows;
        c.eventMinUs = (int) ci.EventPeriodMcsMin;
        c.eventMaxUs = (int) ci.EventPeriodMcsMax;
        c.eventAvgUs = (int) ci.EventPeriodMcsAvg;
        c.actualRate = (int) ci.ActualSR;
        list.push_back (c);
    }
    return list;
}

namespace
{
template <typename Req> bool cableRequest (DWORD code, Req& r, int cable)
{
    Driver d;
    V::DriverInfo di;
    if (cable < 1 || ! d.open() || ! d.driverInfo (di) || cable > (int) di.NumCables) return false;
    r.CableNum = (UINT) (cable - 1);
    return d.ioctl (code, r, sizeof (r));
}
} // namespace

bool setMsPerInt (int cable, int ms)
{
    V::CableSetMsPerIntReq r {};
    r.MsPerInt = (UINT) juce::jlimit (1, 100, ms);
    return cableRequest (V::IoCtl_Local_SetMsPerInt, r, cable);
}

bool setFormatRange (int cable, const FormatRange& f)
{
    V::CableSetDataRangeReq r {};
    r.DataRange = { (UINT) f.minRate, (UINT) f.maxRate, (UINT) f.minBits, (UINT) f.maxBits, (UINT) f.minChannels, (UINT) f.maxChannels };
    return cableRequest (V::IoCtl_Local_SetDataRange, r, cable);
}

bool resetCounters (int cable)
{
    V::CableReq r {};
    return cableRequest (V::IoCtl_Local_ResetCableCounters, r, cable);
}

namespace
{
/** Control interface path, found once (SetupAPI is slow); the handle is opened per request, as the
    driver allows only one open handle at a time. */
juce::String controlPath()
{
    static juce::CriticalSection lock;
    static juce::String path;
    const juce::ScopedLock sl (lock);
    if (path.isEmpty())
    {
        query();   // finds the API version the driver speaks
        HDEVINFO list = SetupDiGetClassDevsW (&kProductGuid, nullptr, nullptr, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
        if (list == INVALID_HANDLE_VALUE) return {};
        SP_DEVICE_INTERFACE_DATA ifData {};
        ifData.cbSize = sizeof (ifData);
        struct Detail : SP_DEVICE_INTERFACE_DETAIL_DATA_W { wchar_t more[MAX_PATH]; } detail {};
        detail.cbSize = sizeof (SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiEnumDeviceInterfaces (list, nullptr, &kProductGuid, 0, &ifData)
            && SetupDiGetDeviceInterfaceDetailW (list, &ifData, &detail, sizeof (detail), nullptr, nullptr))
            path = juce::String (detail.DevicePath) + "\\" + juce::String (kApiInterfaceGuid);
        SetupDiDestroyDeviceInfoList (list);
    }
    return path;
}

template <typename Req, typename Out = int>
bool quickRequest (DWORD code, Req& r, Out* out = nullptr, DWORD outSize = 0)
{
    const auto path = controlPath();
    if (path.isEmpty()) return false;
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 5 && h == INVALID_HANDLE_VALUE; ++attempt)   // another request may hold it
    {
        h = CreateFileW (path.toWideCharPointer(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) Sleep (1);
    }
    if (h == INVALID_HANDLE_VALUE) return false;
    r.ApiVer = Driver::apiVersion.load();
    DWORD got = 0;
    const bool ok = DeviceIoControl (h, code, &r, sizeof (r), out, outSize, &got, nullptr) != FALSE;
    CloseHandle (h);
    return ok;
}
} // namespace

double getClockCorrection (int cable)
{
    V::CableReq r {};
    r.CableNum = (UINT) juce::jmax (0, cable - 1);
    V::CableInfo ci {};
    if (cable < 1 || ! quickRequest (V::IoCtl_Local_GetCableInfo, r, &ci, sizeof (ci))) return 1.0;
    return ci.ClockCorrection > 0 ? ci.ClockCorrection / (double) V::ClockCorrDenom : 1.0;
}

bool setClockCorrection (int cable, double ratio)
{
    V::CableSetClockCorrectionReq r {};
    r.CableNum = (UINT) juce::jmax (0, cable - 1);
    r.Multiplier = (UINT) juce::jlimit (0.9 * V::ClockCorrDenom, 1.1 * V::ClockCorrDenom, std::round (ratio * V::ClockCorrDenom));
    return cable >= 1 && quickRequest (V::IoCtl_Local_SetClockCorrection, r);
}

namespace
{
template <typename T> struct ComPtr
{
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
};

juce::String readString (IPropertyStore* ps, const PROPERTYKEY& k)
{
    PROPVARIANT v;
    PropVariantInit (&v);
    juce::String s;
    if (SUCCEEDED (ps->GetValue (k, &v)) && v.vt == VT_LPWSTR) s = juce::String (v.pwszVal);
    PropVariantClear (&v);
    return s;
}

/** Cable number (1..N) and side of a VAC KS filter, asked from the filter itself; 0 if it is not VAC's. */
int cableOfFilter (const juce::String& filterPath, bool& capture)
{
    HANDLE h = CreateFileW (filterPath.toWideCharPointer(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    KSPROPERTY prop {};
    prop.Set = V::KSPROPSETID_VAC;
    prop.Id = V::KSPROPERTY_VAC_GetKsInterfaceDetails;
    prop.Flags = KSPROPERTY_TYPE_GET;
    V::KsInterfaceDetailsReq details {};
    DWORD got = 0;
    const bool ok = DeviceIoControl (h, IOCTL_KS_PROPERTY, &prop, sizeof (prop), &details, sizeof (details), &got, nullptr) != FALSE;
    CloseHandle (h);
    if (! ok || got < sizeof (details)) return 0;
    capture = details.SubdeviceType == V::Subdevice_Capture;
    return (int) details.CableNumber + 1;
}
} // namespace

std::vector<Endpoint> endpoints()
{
    // PKEY_Device_DeviceDesc / FriendlyName, and the endpoint's KS filter path ("{n}\\?\...").
    static const GUID deviceKeys { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } };
    static const PROPERTYKEY kDesc { deviceKeys, 2 }, kFriendly { deviceKeys, 14 };
    static const PROPERTYKEY kFilterPath { { 0x233164c8, 0x1b2c, 0x4c7d, { 0xbc, 0x68, 0xb6, 0x71, 0x68, 0x7a, 0x25, 0x67 } }, 1 };

    std::vector<Endpoint> list;
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof (IMMDeviceEnumerator), (void**) &en)))
        return list;
    for (auto flow : { eRender, eCapture })
    {
        ComPtr<IMMDeviceCollection> col;
        if (FAILED (en->EnumAudioEndpoints (flow, DEVICE_STATE_ACTIVE | DEVICE_STATE_DISABLED, &col))) continue;
        UINT count = 0;
        col->GetCount (&count);
        for (UINT i = 0; i < count; ++i)
        {
            ComPtr<IMMDevice> dev;
            ComPtr<IPropertyStore> ps;
            if (FAILED (col->Item (i, &dev)) || FAILED (dev->OpenPropertyStore (STGM_READ, &ps))) continue;
            Endpoint e;
            e.friendly = readString (ps.p, kFriendly);
            e.desc = readString (ps.p, kDesc);
            auto path = readString (ps.p, kFilterPath);
            if (! path.containsIgnoreCase ("83ed7f0e-2028-4956-b0b4-39c76fdaef1d")) continue;   // not VAC
            // "{2}.\\?\root#..." -> "\\?\root#..."
            if (const int k = path.indexOf ("\\\\?\\"); k > 0) path = path.substring (k);
            bool capture = flow == eCapture;
            e.cable = cableOfFilter (path, capture);
            e.filterPath = path;
            if (e.cable == 0) e.cable = cableNumberOf (e.desc);   // fallback: "Line 3"
            if (e.cable == 0) continue;
            e.capture = flow == eCapture;
            DWORD state = 0;
            e.visible = SUCCEEDED (dev->GetState (&state)) && state == DEVICE_STATE_ACTIVE;
            LPWSTR id = nullptr;
            if (SUCCEEDED (dev->GetId (&id))) { e.id = juce::String (id); CoTaskMemFree (id); }
            list.push_back (e);
        }
    }
    return list;
}

namespace
{
// IPolicyConfig (Windows 7 and later): the interface the sound control panel uses. Not in the SDK; only the
// method used here is called, the others keep the vtable layout.
struct __declspec (uuid ("f8679f50-850a-41cf-9c72-430f290290c8")) IPolicyConfig : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat (PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat (PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat (PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat (PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod (PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod (PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode (PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode (PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue (PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue (PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint (PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility (PCWSTR, INT) = 0;
};
constexpr GUID kPolicyConfigClient { 0x870af99c, 0x171d, 0x4f9e, { 0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9 } };
} // namespace

bool setEndpointVisible (const juce::String& endpointId, bool visible)
{
    ComPtr<IPolicyConfig> pc;
    if (FAILED (CoCreateInstance (kPolicyConfigClient, nullptr, CLSCTX_ALL, __uuidof (IPolicyConfig), (void**) &pc)))
        return false;
    return SUCCEEDED (pc->SetEndpointVisibility (endpointId.toWideCharPointer(), visible ? 1 : 0));
}

juce::String describe (Result r)
{
    switch (r)
    {
        case Result::done:          return "Done.";
        case Result::needsReboot:   return "A cable is still in use, so Windows applies the new number of cables at the next restart. "
                                           "Close the apps that use Virtual Audio Cable and try again to apply it now.";
        case Result::notInstalled:  return "Virtual Audio Cable is not installed.";
        case Result::setFailed:     return "The driver did not accept the number of cables.";
        case Result::restartFailed: return "The driver could not be restarted (administrator rights needed).";
        case Result::declined:      return "Not changed: administrator rights were declined.";
    }
    return {};
}

Result setNumCables (int n, bool restart)
{
    n = juce::jlimit (1, 256, n);
    Driver d;
    V::DriverInfo di;
    if (! d.open() || ! d.driverInfo (di)) return Result::notInstalled;   // also finds the API version
    V::DriverSetNumCablesReq r {};
    r.NumCables = (UINT) n;
    if (! d.ioctl (V::IoCtl_Local_SetNumCables, r, sizeof (r))) return Result::setFailed;
    if (! restart) return Result::done;
    if (! d.driverInfo (di)) return Result::setFailed;
    if ((di.Flags & V::DriverFlags::RestartRequired) == 0) return Result::done;   // already that number

    const auto res = d.restart();
    if (res != Result::done) return res;

    // What counts is what the driver reports after the restart.
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        if (d.open() && d.driverInfo (di))
            return (int) di.NumCables == n && (di.Flags & V::DriverFlags::RestartRequired) == 0 ? Result::done : Result::needsReboot;
        Sleep (100);
    }
    return Result::needsReboot;
}

Result setNumCablesElevated (int n)
{
    const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName();
    const auto args = juce::String (kJobArgument) + " " + juce::String (juce::jlimit (1, 256, n));
    SHELLEXECUTEINFOW sei {};
    sei.cbSize = sizeof (sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.toWideCharPointer();
    sei.lpParameters = args.toWideCharPointer();
    sei.nShow = SW_HIDE;
    if (! ShellExecuteExW (&sei))
        return GetLastError() == ERROR_CANCELLED ? Result::declined : Result::restartFailed;
    if (sei.hProcess == nullptr) return Result::restartFailed;

    DWORD code = (DWORD) Result::restartFailed;
    if (WaitForSingleObject (sei.hProcess, 60000) == WAIT_OBJECT_0) GetExitCodeProcess (sei.hProcess, &code);
    CloseHandle (sei.hProcess);
    switch (code)
    {
        case (DWORD) Result::done:         return Result::done;
        case (DWORD) Result::needsReboot:  return Result::needsReboot;
        case (DWORD) Result::notInstalled: return Result::notInstalled;
        case (DWORD) Result::setFailed:    return Result::setFailed;
        default:                           return Result::restartFailed;
    }
}

int runJob (const juce::String& commandLine)
{
    const int n = commandLine.fromFirstOccurrenceOf (kJobArgument, false, false).trim().getIntValue();
    if (n < 1 || n > 256) return (int) Result::setFailed;
    return (int) setNumCables (n);
}

bool isCableEndpoint (const juce::String& friendlyName)
{
    return friendlyName.containsIgnoreCase ("(Virtual Audio Cable)");
}

int cableNumberOf (const juce::String& description)
{
    const auto t = description.trim();
    if (! t.startsWithIgnoreCase ("Line ")) return 0;
    const auto num = t.substring (5).trim();
    return num.containsOnly ("0123456789") && num.isNotEmpty() ? num.getIntValue() : 0;
}

juce::String defaultEndpointName (int cable)
{
    return "Line " + juce::String (cable) + " (Virtual Audio Cable)";
}
} // namespace hydra::vac

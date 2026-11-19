// Media Foundation parts of Hydra Camera (see VideoCodec.h). No JUCE here: the virtual camera API is declared only for
// Windows 11 SDK targets, which JUCE's headers would lower.
#undef WINVER
#undef _WIN32_WINNT
#undef NTDDI_VERSION
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#define NTDDI_VERSION 0x0A00000B   // NTDDI_WIN10_CO (Windows 11): MFCreateVirtualCamera
#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <initguid.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mftransform.h>
#include <codecapi.h>
#include <strmif.h>
#include <mfvirtualcamera.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "VideoCodec.h"
#include "CameraShared.h"

#pragma comment (lib, "mfplat.lib")
#pragma comment (lib, "mfuuid.lib")
#pragma comment (lib, "ole32.lib")
#pragma comment (lib, "shell32.lib")
#pragma comment (lib, "advapi32.lib")

using Microsoft::WRL::ComPtr;

namespace hydra::cam
{
namespace
{
std::string hrText (const char* what, HRESULT hr)
{
    char b[96];
    std::snprintf (b, sizeof (b), "%s (0x%08lx)", what, (unsigned long) hr);
    return b;
}

/** The first synchronous software MFT of a category that converts in -> out. */
ComPtr<IMFTransform> findTransform (const GUID& category, const GUID& in, const GUID& out)
{
    MFT_REGISTER_TYPE_INFO ti { MFMediaType_Video, in }, to { MFMediaType_Video, out };
    IMFActivate** list = nullptr;
    UINT32 count = 0;
    ComPtr<IMFTransform> t;
    if (SUCCEEDED (MFTEnumEx (category, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER, &ti, &to, &list, &count)))
    {
        for (UINT32 i = 0; i < count && ! t; ++i)
            list[i]->ActivateObject (IID_PPV_ARGS (&t));
        for (UINT32 i = 0; i < count; ++i) list[i]->Release();
        CoTaskMemFree (list);
    }
    return t;
}

void setCodecValue (IMFTransform* t, const GUID& key, UINT32 value)
{
    ComPtr<ICodecAPI> api;
    if (SUCCEEDED (t->QueryInterface (IID_PPV_ARGS (&api))))
    {
        VARIANT v;
        VariantInit (&v);
        v.vt = VT_UI4;
        v.ulVal = value;
        api->SetValue (&key, &v);
    }
}

ComPtr<IMFSample> memorySample (DWORD bytes, DWORD alignment = 0)
{
    ComPtr<IMFSample> s;
    ComPtr<IMFMediaBuffer> b;
    if (FAILED (alignment > 1 ? MFCreateAlignedMemoryBuffer (bytes, alignment - 1, &b) : MFCreateMemoryBuffer (bytes, &b))) return nullptr;
    if (FAILED (MFCreateSample (&s))) return nullptr;
    s->AddBuffer (b.Get());
    return s;
}

/** One ProcessOutput round: S_OK with a sample, MF_E_TRANSFORM_NEED_MORE_INPUT, MF_E_TRANSFORM_STREAM_CHANGE, or an error. */
HRESULT pullOutput (IMFTransform* t, const MFT_OUTPUT_STREAM_INFO& info, ComPtr<IMFSample>& own, ComPtr<IMFSample>& result)
{
    MFT_OUTPUT_DATA_BUFFER ob {};
    const bool provides = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    if (! provides)
    {
        if (! own) own = memorySample (std::max<DWORD> (info.cbSize, 1), info.cbAlignment);
        if (! own) return E_OUTOFMEMORY;
        ComPtr<IMFMediaBuffer> b;
        if (SUCCEEDED (own->GetBufferByIndex (0, &b))) b->SetCurrentLength (0);
        ob.pSample = own.Get();
    }
    DWORD status = 0;
    const HRESULT hr = t->ProcessOutput (0, 1, &ob, &status);
    if (ob.pEvents != nullptr) ob.pEvents->Release();
    if (SUCCEEDED (hr))
    {
        result = ob.pSample;
        if (provides && ob.pSample != nullptr) ob.pSample->Release();   // result holds its own reference
    }
    return hr;
}

uint64_t fnv (const void* p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) { h ^= ((const uint8_t*) p)[i]; h *= 1099511628211ull; }
    return h;
}

const wchar_t* clsidKey() { static std::wstring k = std::wstring (L"SOFTWARE\\Classes\\CLSID\\") + camshare::kClsidText; return k.c_str(); }

std::wstring programFiles()
{
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED (SHGetKnownFolderPath (FOLDERID_ProgramFiles, 0, nullptr, &p))) s = p;
    CoTaskMemFree (p);
    return s.empty() ? std::wstring (L"C:\\Program Files") : s;
}

std::wstring cameraFolder() { return programFiles() + L"\\Hydra Mixer\\Camera"; }
} // namespace

//==============================================================================
bool comInit()
{
    const HRESULT hr = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    if (FAILED (hr) && hr != RPC_E_CHANGED_MODE) return false;
    return SUCCEEDED (MFStartup (MF_VERSION, MFSTARTUP_LITE));
}

void comShutdown()
{
    MFShutdown();
    CoUninitialize();
}

//==============================================================================
struct H264Decoder::Impl
{
    ComPtr<IMFTransform> mft;
    MFT_OUTPUT_STREAM_INFO info {};
    ComPtr<IMFSample> own;
    int width = 0, height = 0;
    LONG stride = 0;
    std::string err;

    bool selectNv12()
    {
        for (DWORD i = 0;; ++i)
        {
            ComPtr<IMFMediaType> t;
            if (FAILED (mft->GetOutputAvailableType (0, i, &t))) { err = "The H.264 decoder offers no NV12 output"; return false; }
            GUID sub {};
            t->GetGUID (MF_MT_SUBTYPE, &sub);
            if (sub != MFVideoFormat_NV12) continue;
            const HRESULT hr = mft->SetOutputType (0, t.Get(), 0);
            if (FAILED (hr)) { err = hrText ("The H.264 decoder refused NV12", hr); return false; }
            UINT32 w = 0, h = 0, s = 0;
            MFGetAttributeSize (t.Get(), MF_MT_FRAME_SIZE, &w, &h);
            width = (int) w;
            height = (int) h;
            stride = SUCCEEDED (t->GetUINT32 (MF_MT_DEFAULT_STRIDE, &s)) && (LONG) s > 0 ? (LONG) s : (LONG) w;
            mft->GetOutputStreamInfo (0, &info);
            own.Reset();
            return true;
        }
    }

    bool drain (const std::function<void (const Picture&)>& onPicture)
    {
        for (int guard = 0; guard < 16; ++guard)
        {
            ComPtr<IMFSample> s;
            const HRESULT hr = pullOutput (mft.Get(), info, own, s);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) { if (! selectNv12()) return false; continue; }
            if (FAILED (hr)) { err = hrText ("H.264 decoding failed", hr); return false; }
            if (! s) continue;
            ComPtr<IMFMediaBuffer> b;
            if (FAILED (s->GetBufferByIndex (0, &b))) continue;
            ComPtr<IMF2DBuffer> b2;
            BYTE* p = nullptr;
            LONG pitch = 0;
            if (SUCCEEDED (b.As (&b2)) && SUCCEEDED (b2->Lock2D (&p, &pitch)))
            {
                if (pitch > 0) onPicture ({ p, p + (size_t) pitch * height, (int) pitch, width, height });
                b2->Unlock2D();
            }
            else
            {
                DWORD maxLen = 0, len = 0;
                if (FAILED (b->Lock (&p, &maxLen, &len))) continue;
                if (len >= (DWORD) (stride * height * 3 / 2)) onPicture ({ p, p + (size_t) stride * height, (int) stride, width, height });
                b->Unlock();
            }
        }
        return true;
    }
};

H264Decoder::H264Decoder() : impl (std::make_unique<Impl>()) {}
H264Decoder::~H264Decoder() = default;
std::string H264Decoder::error() const { return impl->err; }

bool H264Decoder::open()
{
    auto& m = *impl;
    m.mft = findTransform (MFT_CATEGORY_VIDEO_DECODER, MFVideoFormat_H264, MFVideoFormat_NV12);
    if (! m.mft) { m.err = "No H.264 decoder in Windows (Windows N needs the Media Feature Pack)"; return false; }
    ComPtr<IMFAttributes> a;
    if (SUCCEEDED (m.mft->GetAttributes (&a)))
    {
        a->SetUINT32 (CODECAPI_AVLowLatencyMode, TRUE);
        a->SetUINT32 (MF_LOW_LATENCY, TRUE);
    }
    setCodecValue (m.mft.Get(), CODECAPI_AVLowLatencyMode, 1);
    ComPtr<IMFMediaType> in;
    MFCreateMediaType (&in);
    in->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID (MF_MT_SUBTYPE, MFVideoFormat_H264);
    in->SetUINT32 (MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    HRESULT hr = m.mft->SetInputType (0, in.Get(), 0);
    if (FAILED (hr)) { m.err = hrText ("The H.264 decoder refused its input", hr); return false; }
    if (! m.selectNv12()) return false;
    m.mft->ProcessMessage (MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m.mft->ProcessMessage (MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

bool H264Decoder::decode (const uint8_t* data, int bytes, int64_t time, const std::function<void (const Picture&)>& onPicture)
{
    auto& m = *impl;
    if (! m.mft || bytes <= 0) return false;
    auto s = memorySample ((DWORD) bytes);
    if (! s) return false;
    {
        ComPtr<IMFMediaBuffer> b;
        s->GetBufferByIndex (0, &b);
        BYTE* p = nullptr;
        b->Lock (&p, nullptr, nullptr);
        std::memcpy (p, data, (size_t) bytes);
        b->Unlock();
        b->SetCurrentLength ((DWORD) bytes);
    }
    s->SetSampleTime (time);
    s->SetSampleDuration (333333);
    HRESULT hr = m.mft->ProcessInput (0, s.Get(), 0);
    if (hr == MF_E_NOTACCEPTING)
    {
        if (! m.drain (onPicture)) { flush(); return false; }
        hr = m.mft->ProcessInput (0, s.Get(), 0);
    }
    if (FAILED (hr)) { m.err = hrText ("The H.264 decoder refused a frame", hr); flush(); return false; }
    if (! m.drain (onPicture)) { flush(); return false; }
    return true;
}

void H264Decoder::flush()
{
    if (impl->mft) impl->mft->ProcessMessage (MFT_MESSAGE_COMMAND_FLUSH, 0);
}

//==============================================================================
struct H264Encoder::Impl
{
    ComPtr<IMFTransform> mft;
    MFT_OUTPUT_STREAM_INFO info {};
    ComPtr<IMFSample> own;
    int width = 0, height = 0, fps = 30;
    std::string err;
};

H264Encoder::H264Encoder() : impl (std::make_unique<Impl>()) {}
H264Encoder::~H264Encoder() = default;
std::string H264Encoder::error() const { return impl->err; }

bool H264Encoder::open (int width, int height, int fps, int kbps)
{
    auto& m = *impl;
    m.width = width;
    m.height = height;
    m.fps = fps;
    m.mft = findTransform (MFT_CATEGORY_VIDEO_ENCODER, MFVideoFormat_NV12, MFVideoFormat_H264);
    if (! m.mft) { m.err = "No H.264 encoder"; return false; }
    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED (m.mft->GetAttributes (&attrs))) attrs->SetUINT32 (MF_LOW_LATENCY, TRUE);
    setCodecValue (m.mft.Get(), CODECAPI_AVLowLatencyMode, 1);
    setCodecValue (m.mft.Get(), CODECAPI_AVEncCommonRealTime, 1);
    setCodecValue (m.mft.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    setCodecValue (m.mft.Get(), CODECAPI_AVEncMPVGOPSize, (UINT32) fps * 2);
    ComPtr<IMFMediaType> out, in;
    MFCreateMediaType (&out);
    out->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID (MF_MT_SUBTYPE, MFVideoFormat_H264);
    out->SetUINT32 (MF_MT_AVG_BITRATE, (UINT32) kbps * 1000);
    MFSetAttributeSize (out.Get(), MF_MT_FRAME_SIZE, (UINT32) width, (UINT32) height);
    MFSetAttributeRatio (out.Get(), MF_MT_FRAME_RATE, (UINT32) fps, 1);
    MFSetAttributeRatio (out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    out->SetUINT32 (MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    out->SetUINT32 (MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
    HRESULT hr = m.mft->SetOutputType (0, out.Get(), 0);
    if (FAILED (hr)) { m.err = hrText ("H.264 encoder output", hr); return false; }
    MFCreateMediaType (&in);
    in->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID (MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize (in.Get(), MF_MT_FRAME_SIZE, (UINT32) width, (UINT32) height);
    MFSetAttributeRatio (in.Get(), MF_MT_FRAME_RATE, (UINT32) fps, 1);
    MFSetAttributeRatio (in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    in->SetUINT32 (MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = m.mft->SetInputType (0, in.Get(), 0);
    if (FAILED (hr)) { m.err = hrText ("H.264 encoder input", hr); return false; }
    m.mft->GetOutputStreamInfo (0, &m.info);
    m.mft->ProcessMessage (MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m.mft->ProcessMessage (MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

bool H264Encoder::encode (const uint8_t* nv12, int64_t time, bool forceKey, std::vector<uint8_t>& out, bool& key)
{
    auto& m = *impl;
    out.clear();
    key = false;
    if (! m.mft) return false;
    const DWORD bytes = (DWORD) (m.width * m.height * 3 / 2);
    auto s = memorySample (bytes);
    if (! s) return false;
    {
        ComPtr<IMFMediaBuffer> b;
        s->GetBufferByIndex (0, &b);
        BYTE* p = nullptr;
        b->Lock (&p, nullptr, nullptr);
        std::memcpy (p, nv12, bytes);
        b->Unlock();
        b->SetCurrentLength (bytes);
    }
    s->SetSampleTime (time);
    s->SetSampleDuration (10000000 / m.fps);
    if (forceKey) setCodecValue (m.mft.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
    HRESULT hr = m.mft->ProcessInput (0, s.Get(), 0);
    if (FAILED (hr)) { m.err = hrText ("H.264 encoding failed", hr); return false; }
    for (int guard = 0; guard < 8; ++guard)
    {
        ComPtr<IMFSample> o;
        hr = pullOutput (m.mft.Get(), m.info, m.own, o);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) { m.mft->GetOutputStreamInfo (0, &m.info); m.own.Reset(); continue; }
        if (FAILED (hr)) { m.err = hrText ("H.264 encoder output failed", hr); return false; }
        if (! o) continue;
        ComPtr<IMFMediaBuffer> b;
        if (FAILED (o->ConvertToContiguousBuffer (&b))) continue;
        BYTE* p = nullptr;
        DWORD len = 0;
        if (SUCCEEDED (b->Lock (&p, nullptr, &len)))
        {
            out.insert (out.end(), p, p + len);
            b->Unlock();
        }
        UINT32 clean = 0;
        if (SUCCEEDED (o->GetUINT32 (MFSampleExtension_CleanPoint, &clean)) && clean != 0) key = true;
    }
    return true;
}

std::vector<uint8_t> H264Encoder::config() const
{
    std::vector<uint8_t> c;
    ComPtr<IMFMediaType> t;
    if (impl->mft && SUCCEEDED (impl->mft->GetOutputCurrentType (0, &t)))
    {
        UINT32 n = 0;
        if (SUCCEEDED (t->GetBlobSize (MF_MT_MPEG_SEQUENCE_HEADER, &n)) && n > 0)
        {
            c.resize (n);
            t->GetBlob (MF_MT_MPEG_SEQUENCE_HEADER, c.data(), n, nullptr);
        }
    }
    return c;
}

//==============================================================================
namespace
{
using CreateFn = HRESULT (STDAPICALLTYPE*) (MFVirtualCameraType, MFVirtualCameraLifetime, MFVirtualCameraAccess, LPCWSTR, LPCWSTR,
                                            const GUID*, ULONG, IMFVirtualCamera**);
CreateFn createFunction()
{
    static HMODULE lib = LoadLibraryExW (L"mfsensorgroup.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return lib != nullptr ? (CreateFn) GetProcAddress (lib, "MFCreateVirtualCamera") : nullptr;
}
} // namespace

bool virtualCameraSupported() { return createFunction() != nullptr; }

struct VirtualCamera::Impl
{
    ComPtr<IMFVirtualCamera> camera;
};

VirtualCamera::VirtualCamera() : impl (std::make_unique<Impl>()) {}
VirtualCamera::~VirtualCamera() { stop(); }
bool VirtualCamera::isStarted() const noexcept { return impl->camera != nullptr; }

bool VirtualCamera::start (std::string& error)
{
    if (impl->camera) return true;
    auto create = createFunction();
    if (create == nullptr) { error = "Needs Windows 11"; return false; }
    ComPtr<IMFVirtualCamera> c;
    HRESULT hr = create (MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_Session, MFVirtualCameraAccess_CurrentUser,
                         camshare::kFriendlyName, camshare::kClsidText, nullptr, 0, &c);
    if (FAILED (hr)) { error = hrText ("Windows did not create the camera", hr); return false; }
    hr = c->Start (nullptr);
    if (FAILED (hr)) { error = hrText ("Windows did not start the camera", hr); c->Shutdown(); return false; }
    impl->camera = c;
    return true;
}

void VirtualCamera::stop()
{
    if (! impl->camera) return;
    impl->camera->Remove();
    impl->camera->Shutdown();
    impl->camera.Reset();
}

//==============================================================================
bool embeddedDll (const void*& data, size_t& bytes)
{
    HMODULE m = GetModuleHandleW (nullptr);
    HRSRC r = FindResourceW (m, MAKEINTRESOURCEW (203), MAKEINTRESOURCEW (10));   // RT_RCDATA
    if (r == nullptr) return false;
    HGLOBAL g = LoadResource (m, r);
    data = g != nullptr ? LockResource (g) : nullptr;
    bytes = SizeofResource (m, r);
    return data != nullptr && bytes > 0;
}

std::wstring installedDllPath()
{
    const void* d = nullptr;
    size_t n = 0;
    wchar_t hash[24] = L"0";
    if (embeddedDll (d, n)) swprintf_s (hash, L"%08llx", (unsigned long long) (fnv (d, n) & 0xFFFFFFFFull));
    return cameraFolder() + L"\\HydraCamera-" + hash + L".dll";
}

SetupState setupState()
{
    const void* d = nullptr;
    size_t n = 0;
    if (! embeddedDll (d, n)) return SetupState::noResource;
    wchar_t path[MAX_PATH * 2] {};
    DWORD bytes = sizeof (path);
    const std::wstring key = std::wstring (clsidKey()) + L"\\InprocServer32";
    if (RegGetValueW (HKEY_LOCAL_MACHINE, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, path, &bytes) != ERROR_SUCCESS) return SetupState::missing;
    const auto want = installedDllPath();
    if (_wcsicmp (path, want.c_str()) != 0 || GetFileAttributesW (want.c_str()) == INVALID_FILE_ATTRIBUTES) return SetupState::outdated;
    return SetupState::ok;
}

int setupMachine (bool install)
{
    const auto folder = cameraFolder();
    const auto target = installedDllPath();
    auto removeOld = [&] (bool all)
    {
        WIN32_FIND_DATAW f {};
        HANDLE h = FindFirstFileW ((folder + L"\\HydraCamera-*.dll").c_str(), &f);
        if (h == INVALID_HANDLE_VALUE) return;
        do
        {
            const auto p = folder + L"\\" + f.cFileName;
            if (! all && _wcsicmp (p.c_str(), target.c_str()) == 0) continue;
            // The Frame Server may still have it loaded: then it goes at the next restart.
            if (! DeleteFileW (p.c_str())) MoveFileExW (p.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        } while (FindNextFileW (h, &f));
        FindClose (h);
    };

    if (! install)
    {
        RegDeleteTreeW (HKEY_LOCAL_MACHINE, clsidKey());
        removeOld (true);
        RemoveDirectoryW (folder.c_str());
        RemoveDirectoryW ((programFiles() + L"\\Hydra Mixer").c_str());
        return 0;
    }

    const void* data = nullptr;
    size_t bytes = 0;
    if (! embeddedDll (data, bytes)) return 2;
    SHCreateDirectoryExW (nullptr, folder.c_str(), nullptr);
    if (GetFileAttributesW (target.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        const auto tmp = target + L".tmp";
        HANDLE f = CreateFileW (tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return 3;
        DWORD written = 0;
        const BOOL ok = WriteFile (f, data, (DWORD) bytes, &written, nullptr) && written == (DWORD) bytes;
        CloseHandle (f);
        if (! ok || ! MoveFileExW (tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileW (tmp.c_str()); return 4; }
    }

    HKEY k = nullptr, inproc = nullptr;
    if (RegCreateKeyExW (HKEY_LOCAL_MACHINE, clsidKey(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return 5;
    auto setString = [] (HKEY key, const wchar_t* name, const std::wstring& v)
    {
        return RegSetValueExW (key, name, 0, REG_SZ, (const BYTE*) v.c_str(), (DWORD) ((v.size() + 1) * sizeof (wchar_t))) == ERROR_SUCCESS;
    };
    bool ok = setString (k, nullptr, camshare::kFriendlyName);
    if (RegCreateKeyExW (k, L"InprocServer32", 0, nullptr, 0, KEY_WRITE, nullptr, &inproc, nullptr) == ERROR_SUCCESS)
    {
        ok = setString (inproc, nullptr, target) && ok;
        ok = setString (inproc, L"ThreadingModel", L"Both") && ok;
        RegCloseKey (inproc);
    }
    else ok = false;
    RegCloseKey (k);
    removeOld (false);
    return ok ? 0 : 6;
}
} // namespace hydra::cam

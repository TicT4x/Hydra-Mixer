// Hydra Camera: the media source behind Hydra's virtual webcam (Windows 11, MFCreateVirtualCamera). No kernel driver:
// Windows' Frame Server service loads this DLL (in-process COM server, registered under HKLM by Hydra's elevated
// helper) when an app opens "Hydra Camera", and every app that uses the camera gets its frames through the Frame
// Server. The frames come from Hydra through shared memory (camera/CameraShared.h); without Hydra (or while the
// phone sends nothing) the camera shows a dark frame.
//
// Built without JUCE and with the static C runtime: it runs inside a Windows service process.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <initguid.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <sddl.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <ks.h>        // after WRL: ks.h defines GUID_NULL as a macro, which breaks cguid.h
#include <ksmedia.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "CameraShared.h"

#pragma comment (lib, "mfplat.lib")
#pragma comment (lib, "mfuuid.lib")
#pragma comment (lib, "advapi32.lib")

// IKsControl (ks.h has it only for kernel streaming builds): the Frame Server asks every source and stream for it.
struct __declspec (uuid ("28F54685-06FD-11D2-B27A-00A0C9223196")) __declspec (novtable) HydraKsControl : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE KsProperty (PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) = 0;
    virtual HRESULT STDMETHODCALLTYPE KsMethod (PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) = 0;
    virtual HRESULT STDMETHODCALLTYPE KsEvent (PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) = 0;
};

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::ChainInterfaces;
using Microsoft::WRL::Make;

namespace
{
std::atomic<long> gObjects { 0 }, gLocks { 0 };
constexpr int kFps = 30;
constexpr DWORD kRepeatMs = 100;   // without new frames from Hydra: one sample every 100 ms (the last frame again)
constexpr LONGLONG kFrameTime = 10000000 / kFps;

struct Counted
{
    Counted()  { ++gObjects; }
    ~Counted() { --gObjects; }
};

//==============================================================================
/** The shared memory + event Hydra writes into (created here: see CameraShared.h). */
struct SharedFrames
{
    HANDLE mapping = nullptr, frameEvent = nullptr;
    void* view = nullptr;

    void open()
    {
        if (view != nullptr) return;
        SECURITY_ATTRIBUTES sa { sizeof (sa), nullptr, FALSE };
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW (hydra::camshare::kSddl, SDDL_REVISION_1, &sd, nullptr))
            sa.lpSecurityDescriptor = sd;
        const bool test = GetEnvironmentVariableW (L"HYDRA_CAMERA_TEST", nullptr, 0) > 0;
        mapping = CreateFileMappingW (INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, (DWORD) (hydra::camshare::kMappingBytes >> 32),
                                      (DWORD) hydra::camshare::kMappingBytes, hydra::camshare::objectName (hydra::camshare::kMappingName, test).c_str());
        if (mapping != nullptr)
            frameEvent = CreateEventW (&sa, FALSE, FALSE, hydra::camshare::objectName (hydra::camshare::kEventName, test).c_str());
        if (sd != nullptr) LocalFree (sd);
        if (mapping == nullptr) return;
        view = MapViewOfFile (mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (view == nullptr) { close(); return; }
        auto* h = (hydra::camshare::Header*) view;
        if (h->magic != hydra::camshare::kMagic || h->version != hydra::camshare::kVersion)
        {
            std::memset (h, 0, sizeof (*h));
            h->version = hydra::camshare::kVersion;
            h->magic = hydra::camshare::kMagic;
        }
    }

    void close()
    {
        if (view != nullptr) { UnmapViewOfFile (view); view = nullptr; }
        if (mapping != nullptr) { CloseHandle (mapping); mapping = nullptr; }
        if (frameEvent != nullptr) { CloseHandle (frameEvent); frameEvent = nullptr; }
    }

    hydra::camshare::Header* header() const noexcept { return (hydra::camshare::Header*) view; }
};

ComPtr<IMFMediaType> makeType (UINT32 w, UINT32 h)
{
    ComPtr<IMFMediaType> t;
    if (FAILED (MFCreateMediaType (&t))) return nullptr;
    t->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID (MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize (t.Get(), MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio (t.Get(), MF_MT_FRAME_RATE, kFps, 1);
    MFSetAttributeRatio (t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    t->SetUINT32 (MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    t->SetUINT32 (MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    t->SetUINT32 (MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    t->SetUINT32 (MF_MT_DEFAULT_STRIDE, w);
    t->SetUINT32 (MF_MT_SAMPLE_SIZE, w * h * 3 / 2);
    return t;
}

HRESULT notFound() { return HRESULT_FROM_WIN32 (ERROR_SET_NOT_FOUND); }

class Source;

//==============================================================================
class Stream final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ChainInterfaces<IMFMediaStream2, IMFMediaStream, IMFMediaEventGenerator>, HydraKsControl>,
                     private Counted
{
public:
    HRESULT init (IMFMediaSource* src, IMFStreamDescriptor* sd)
    {
        source = src;
        descriptor = sd;
        requestEvent = CreateEventW (nullptr, FALSE, FALSE, nullptr);
        stopEvent = CreateEventW (nullptr, TRUE, FALSE, nullptr);
        return MFCreateEventQueue (&queue);
    }

    ~Stream() override
    {
        stopWorker();
        if (requestEvent) CloseHandle (requestEvent);
        if (stopEvent) CloseHandle (stopEvent);
        shared.close();
    }

    // IMFMediaEventGenerator
    IFACEMETHODIMP GetEvent (DWORD flags, IMFMediaEvent** e) override
    {
        ComPtr<IMFMediaEventQueue> q;
        { std::lock_guard<std::mutex> l (lock); if (shut) return MF_E_SHUTDOWN; q = queue; }
        return q->GetEvent (flags, e);   // may block: never under the lock
    }
    IFACEMETHODIMP BeginGetEvent (IMFAsyncCallback* cb, IUnknown* state) override
    {
        std::lock_guard<std::mutex> l (lock);
        return shut ? MF_E_SHUTDOWN : queue->BeginGetEvent (cb, state);
    }
    IFACEMETHODIMP EndGetEvent (IMFAsyncResult* r, IMFMediaEvent** e) override
    {
        std::lock_guard<std::mutex> l (lock);
        return shut ? MF_E_SHUTDOWN : queue->EndGetEvent (r, e);
    }
    IFACEMETHODIMP QueueEvent (MediaEventType t, REFGUID g, HRESULT s, const PROPVARIANT* v) override
    {
        std::lock_guard<std::mutex> l (lock);
        return shut ? MF_E_SHUTDOWN : queue->QueueEventParamVar (t, g, s, v);
    }

    // IMFMediaStream
    IFACEMETHODIMP GetMediaSource (IMFMediaSource** s) override
    {
        if (s == nullptr) return E_POINTER;
        std::lock_guard<std::mutex> l (lock);
        if (shut) return MF_E_SHUTDOWN;
        return source.CopyTo (s);
    }
    IFACEMETHODIMP GetStreamDescriptor (IMFStreamDescriptor** d) override
    {
        if (d == nullptr) return E_POINTER;
        std::lock_guard<std::mutex> l (lock);
        if (shut) return MF_E_SHUTDOWN;
        return descriptor.CopyTo (d);
    }
    IFACEMETHODIMP RequestSample (IUnknown* token) override
    {
        std::lock_guard<std::mutex> l (lock);
        if (shut) return MF_E_SHUTDOWN;
        if (state != MF_STREAM_STATE_RUNNING) return MF_E_MEDIA_SOURCE_WRONGSTATE;
        if (pending.size() > 32) return MF_E_NOTACCEPTING;
        pending.emplace_back (token);
        SetEvent (requestEvent);
        return S_OK;
    }

    // IMFMediaStream2
    IFACEMETHODIMP SetStreamState (MF_STREAM_STATE s) override
    {
        std::lock_guard<std::mutex> l (lock);
        if (shut) return MF_E_SHUTDOWN;
        if (s == MF_STREAM_STATE_RUNNING && state != MF_STREAM_STATE_RUNNING) return MF_E_INVALID_STATE_TRANSITION;   // only through Start
        if (s != MF_STREAM_STATE_RUNNING) pending.clear();
        state = s;
        return S_OK;
    }
    IFACEMETHODIMP GetStreamState (MF_STREAM_STATE* s) override
    {
        if (s == nullptr) return E_POINTER;
        std::lock_guard<std::mutex> l (lock);
        if (shut) return MF_E_SHUTDOWN;
        *s = state;
        return S_OK;
    }

    // IKsControl: no camera controls.
    IFACEMETHODIMP KsProperty (PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override { return notFound(); }
    IFACEMETHODIMP KsMethod (PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override     { return notFound(); }
    IFACEMETHODIMP KsEvent (PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override       { return notFound(); }

    //--------------------------------------------------------------------------
    /** Source::Start: (re)starts with the media type the app chose. */
    HRESULT start (IMFMediaType* type)
    {
        stopWorker();
        UINT32 w = 0, h = 0;
        if (FAILED (MFGetAttributeSize (type, MF_MT_FRAME_SIZE, &w, &h)) || w == 0 || h == 0 || w > (UINT32) hydra::camshare::kMaxWidth
            || h > (UINT32) hydra::camshare::kMaxHeight)
            return MF_E_INVALIDMEDIATYPE;
        {
            std::lock_guard<std::mutex> l (lock);
            if (shut) return MF_E_SHUTDOWN;
            width = (int) w;
            height = (int) h;
            frame.assign ((size_t) w * h * 3 / 2, 0);
            fillIdle();
            allocator.Reset();
            ComPtr<IMFVideoSampleAllocatorEx> a;
            if (SUCCEEDED (MFCreateVideoSampleAllocatorEx (IID_PPV_ARGS (&a))) && SUCCEEDED (a->InitializeSampleAllocatorEx (4, 16, nullptr, type)))
                allocator = a;
            shared.open();
            if (auto* hd = shared.header())
            {
                InterlockedExchange (&hd->wantWidth, (LONG) w);
                InterlockedExchange (&hd->wantHeight, (LONG) h);
                InterlockedExchange64 (&hd->consumerTick, (LONG64) GetTickCount64());
            }
            pending.clear();
            state = MF_STREAM_STATE_RUNNING;
            lastFrames = -1;
            lastDelivery = 0;
            ResetEvent (stopEvent);
        }
        worker = std::thread ([this] { run(); });
        PROPVARIANT v;
        PropVariantInit (&v);
        v.vt = VT_I8;
        v.hVal.QuadPart = MFGetSystemTime();
        return QueueEvent (MEStreamStarted, GUID_NULL, S_OK, &v);
    }

    HRESULT stop()
    {
        stopWorker();
        {
            std::lock_guard<std::mutex> l (lock);
            if (shut) return MF_E_SHUTDOWN;
            pending.clear();
            state = MF_STREAM_STATE_STOPPED;
            if (auto* hd = shared.header()) { InterlockedExchange (&hd->wantWidth, 0); InterlockedExchange (&hd->wantHeight, 0); }
        }
        return QueueEvent (MEStreamStopped, GUID_NULL, S_OK, nullptr);
    }

    void shutdown()
    {
        stopWorker();
        std::lock_guard<std::mutex> l (lock);
        if (shut) return;
        if (auto* hd = shared.header()) { InterlockedExchange (&hd->wantWidth, 0); InterlockedExchange (&hd->wantHeight, 0); }
        shut = true;
        pending.clear();
        if (queue) queue->Shutdown();
        allocator.Reset();
        source.Reset();   // breaks the source <-> stream cycle
    }

private:
    std::mutex lock;
    ComPtr<IMFMediaSource> source;
    ComPtr<IMFStreamDescriptor> descriptor;
    ComPtr<IMFMediaEventQueue> queue;
    ComPtr<IMFVideoSampleAllocatorEx> allocator;
    MF_STREAM_STATE state = MF_STREAM_STATE_STOPPED;
    bool shut = false;
    std::deque<ComPtr<IUnknown>> pending;   // tokens of RequestSample (may be null)
    HANDLE requestEvent = nullptr, stopEvent = nullptr;
    std::thread worker;
    SharedFrames shared;
    int width = 0, height = 0;
    std::vector<uint8_t> frame;
    LONG lastFrames = -1;
    ULONGLONG lastDelivery = 0;

    void stopWorker()
    {
        if (! worker.joinable()) return;
        SetEvent (stopEvent);
        worker.join();
    }

    /** Before the first frame from Hydra: a dark grey picture. */
    void fillIdle()
    {
        const size_t y = (size_t) width * height;
        std::memset (frame.data(), 28, y);
        std::memset (frame.data() + y, 128, y / 2);
    }

    /** Delivers one sample per request: as soon as Hydra has a new frame, else every 100 ms (the last one again).
        So the camera runs at the phone's frame rate and never spins. */
    void run()
    {
        for (;;)
        {
            DWORD wait = INFINITE;
            HANDLE frameEvent = nullptr;
            {
                std::lock_guard<std::mutex> l (lock);
                frameEvent = shared.frameEvent;
                if (! pending.empty())
                {
                    const ULONGLONG since = GetTickCount64() - lastDelivery;
                    wait = since >= kRepeatMs ? 0 : (DWORD) (kRepeatMs - since);
                    if (auto* hd = shared.header(); hd != nullptr && hd->frames != lastFrames) wait = 0;
                }
            }
            HANDLE events[3] = { stopEvent, requestEvent, frameEvent };
            const DWORD r = WaitForMultipleObjects (frameEvent != nullptr ? 3 : 2, events, FALSE, wait);
            if (r == WAIT_OBJECT_0) return;
            deliverIfDue();
        }
    }

    void deliverIfDue()
    {
        ComPtr<IMFSample> sample;
        {
            std::lock_guard<std::mutex> l (lock);
            if (shut || state != MF_STREAM_STATE_RUNNING || pending.empty()) return;
            auto* hd = shared.header();
            if (hd == nullptr) shared.open();   // created lazily (open() in start may have failed)
            hd = shared.header();
            const LONG frames = hd != nullptr ? hd->frames : 0;
            const ULONGLONG now = GetTickCount64();
            if (frames == lastFrames && now - lastDelivery < kRepeatMs) return;
            if (hd != nullptr && frames != lastFrames) hydra::camshare::readFrame (shared.view, frame.data(), width, height);
            lastFrames = frames;
            lastDelivery = now;
            if (hd != nullptr) InterlockedExchange64 (&hd->consumerTick, (LONG64) now);

            if (FAILED (makeSample (sample))) return;
            ComPtr<IUnknown> token = pending.front();
            pending.pop_front();
            const LONGLONG t = MFGetSystemTime();
            sample->SetSampleTime (t);
            sample->SetSampleDuration (kFrameTime);
            if (token) sample->SetUnknown (MFSampleExtension_Token, token.Get());
            queue->QueueEventParamUnk (MEMediaSample, GUID_NULL, S_OK, sample.Get());
        }
    }

    HRESULT makeSample (ComPtr<IMFSample>& sample)
    {
        HRESULT hr = E_FAIL;
        if (allocator) hr = allocator->AllocateSample (&sample);
        if (FAILED (hr))
        {
            ComPtr<IMFMediaBuffer> buf;
            if (FAILED (hr = MFCreate2DMediaBuffer ((DWORD) width, (DWORD) height, MFVideoFormat_NV12.Data1, FALSE, &buf))) return hr;
            if (FAILED (hr = MFCreateSample (&sample))) return hr;
            sample->AddBuffer (buf.Get());
        }
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED (hr = sample->GetBufferByIndex (0, &buf))) return hr;
        const int rows = height * 3 / 2;
        ComPtr<IMF2DBuffer2> b2;
        if (SUCCEEDED (buf.As (&b2)))
        {
            BYTE* scan0 = nullptr;
            BYTE* start = nullptr;
            LONG pitch = 0;
            DWORD len = 0;
            if (FAILED (hr = b2->Lock2DSize (MF2DBuffer_LockFlags_Write, &scan0, &pitch, &start, &len))) return hr;
            for (int y = 0; y < rows; ++y) std::memcpy (scan0 + (ptrdiff_t) y * pitch, frame.data() + (size_t) y * width, (size_t) width);
            b2->Unlock2D();
        }
        else
        {
            BYTE* p = nullptr;
            DWORD maxLen = 0;
            if (FAILED (hr = buf->Lock (&p, &maxLen, nullptr))) return hr;
            const DWORD bytes = (DWORD) frame.size();
            std::memcpy (p, frame.data(), std::min (bytes, maxLen));
            buf->Unlock();
            buf->SetCurrentLength (std::min (bytes, maxLen));
        }
        return S_OK;
    }
};

//==============================================================================
class Source final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ChainInterfaces<IMFMediaSourceEx, IMFMediaSource, IMFMediaEventGenerator>,
                                         IMFGetService, HydraKsControl>,
                     private Counted
{
public:
    HRESULT init()
    {
        HRESULT hr;
        if (FAILED (hr = MFCreateEventQueue (&queue))) return hr;
        if (FAILED (hr = MFCreateAttributes (&attributes, 4))) return hr;

        // 720p first: the format an app gets without choosing (= what the phone sends by default, no scaling).
        // Hydra fits whatever the phone sends into the format the app chose.
        // 1440p / 4K since 18.11. (over the USB cable the phone sends that; else Hydra scales up).
        const UINT32 sizes[][2] = { { 1280, 720 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 }, { 640, 360 } };
        constexpr int kTypes = (int) (sizeof (sizes) / sizeof (sizes[0]));
        IMFMediaType* types[kTypes] {};
        std::vector<ComPtr<IMFMediaType>> keep;
        for (int i = 0; i < kTypes; ++i)
        {
            keep.push_back (makeType (sizes[i][0], sizes[i][1]));
            if (! keep.back()) return E_OUTOFMEMORY;
            types[i] = keep.back().Get();
        }
        ComPtr<IMFStreamDescriptor> sd;
        if (FAILED (hr = MFCreateStreamDescriptor (0, kTypes, types, &sd))) return hr;
        ComPtr<IMFMediaTypeHandler> handler;
        if (SUCCEEDED (sd->GetMediaTypeHandler (&handler))) handler->SetCurrentMediaType (types[0]);
        sd->SetGUID (MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE);
        sd->SetUINT32 (MF_DEVICESTREAM_STREAM_ID, 0);
        sd->SetUINT32 (MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
        sd->SetUINT32 (MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);

        IMFStreamDescriptor* list[1] = { sd.Get() };
        if (FAILED (hr = MFCreatePresentationDescriptor (1, list, &presentation))) return hr;
        presentation->SelectStream (0);

        auto s = Make<Stream>();
        if (! s) return E_OUTOFMEMORY;
        if (FAILED (hr = s->init (this, sd.Get()))) return hr;
        stream = s;
        streamAttributes = sd;
        return S_OK;
    }

    // IMFMediaEventGenerator
    IFACEMETHODIMP GetEvent (DWORD flags, IMFMediaEvent** e) override
    {
        ComPtr<IMFMediaEventQueue> q;
        { std::lock_guard<std::mutex> l (lock); if (shut) return MF_E_SHUTDOWN; q = queue; }
        return q->GetEvent (flags, e);
    }
    IFACEMETHODIMP BeginGetEvent (IMFAsyncCallback* cb, IUnknown* st) override
    {
        std::lock_guard<std::mutex> l (lock);
        return shut ? MF_E_SHUTDOWN : queue->BeginGetEvent (cb, st);
    }
    IFACEMETHODIMP EndGetEvent (IMFAsyncResult* r, IMFMediaEvent** e) override
    {
        std::lock_guard<std::mutex> l (lock);
        return shut ? MF_E_SHUTDOWN : queue->EndGetEvent (r, e);
    }
    IFACEMETHODIMP QueueEvent (MediaEventType t, REFGUID g, HRESULT s, const PROPVARIANT* v) override
    {
        std::lock_guard<std::mutex> l (lock);
        return shut ? MF_E_SHUTDOWN : queue->QueueEventParamVar (t, g, s, v);
    }

    // IMFMediaSource
    IFACEMETHODIMP GetCharacteristics (DWORD* c) override
    {
        if (c == nullptr) return E_POINTER;
        std::lock_guard<std::mutex> l (lock);
        if (shut) return MF_E_SHUTDOWN;
        *c = MFMEDIASOURCE_IS_LIVE;
        return S_OK;
    }
    IFACEMETHODIMP CreatePresentationDescriptor (IMFPresentationDescriptor** pd) override
    {
        if (pd == nullptr) return E_POINTER;
        std::lock_guard<std::mutex> l (lock);
        if (shut) return MF_E_SHUTDOWN;
        return presentation->Clone (pd);
    }
    IFACEMETHODIMP Start (IMFPresentationDescriptor* pd, const GUID* timeFormat, const PROPVARIANT*) override
    {
        if (pd == nullptr) return E_INVALIDARG;
        if (timeFormat != nullptr && *timeFormat != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;
        ComPtr<Stream> s;
        bool first = false;
        {
            std::lock_guard<std::mutex> l (lock);
            if (shut) return MF_E_SHUTDOWN;
            s = stream;
            first = ! announced;
            announced = true;
        }
        DWORD count = 0;
        pd->GetStreamDescriptorCount (&count);
        ComPtr<IMFMediaType> type;
        for (DWORD i = 0; i < count; ++i)
        {
            BOOL selected = FALSE;
            ComPtr<IMFStreamDescriptor> sd;
            if (FAILED (pd->GetStreamDescriptorByIndex (i, &selected, &sd)) || ! selected) continue;
            ComPtr<IMFMediaTypeHandler> handler;
            if (SUCCEEDED (sd->GetMediaTypeHandler (&handler))) handler->GetCurrentMediaType (&type);
        }
        if (! type)
        {
            // Nothing selected in the caller's copy: use our own current type.
            BOOL selected = FALSE;
            ComPtr<IMFStreamDescriptor> sd;
            ComPtr<IMFMediaTypeHandler> handler;
            if (SUCCEEDED (presentation->GetStreamDescriptorByIndex (0, &selected, &sd)) && SUCCEEDED (sd->GetMediaTypeHandler (&handler)))
                handler->GetCurrentMediaType (&type);
        }
        if (! type) return MF_E_INVALIDREQUEST;

        ComPtr<IUnknown> unk;
        s.As (&unk);
        HRESULT hr = queue->QueueEventParamUnk (first ? MENewStream : MEUpdatedStream, GUID_NULL, S_OK, unk.Get());
        if (FAILED (hr)) return hr;
        if (FAILED (hr = s->start (type.Get()))) return hr;
        PROPVARIANT v;
        PropVariantInit (&v);
        v.vt = VT_I8;
        v.hVal.QuadPart = MFGetSystemTime();
        return queue->QueueEventParamVar (MESourceStarted, GUID_NULL, S_OK, &v);
    }
    IFACEMETHODIMP Stop() override
    {
        ComPtr<Stream> s;
        { std::lock_guard<std::mutex> l (lock); if (shut) return MF_E_SHUTDOWN; s = stream; }
        s->stop();
        return queue->QueueEventParamVar (MESourceStopped, GUID_NULL, S_OK, nullptr);
    }
    IFACEMETHODIMP Pause() override { return MF_E_INVALID_STATE_TRANSITION; }   // live source
    IFACEMETHODIMP Shutdown() override
    {
        ComPtr<Stream> s;
        {
            std::lock_guard<std::mutex> l (lock);
            if (shut) return MF_E_SHUTDOWN;
            shut = true;
            s = stream;
            stream.Reset();
        }
        if (s) s->shutdown();
        queue->Shutdown();
        return S_OK;
    }

    // IMFMediaSourceEx
    IFACEMETHODIMP GetSourceAttributes (IMFAttributes** a) override
    {
        if (a == nullptr) return E_POINTER;
        std::lock_guard<std::mutex> l (lock);
        if (shut) return MF_E_SHUTDOWN;
        return attributes.CopyTo (a);
    }
    IFACEMETHODIMP GetStreamAttributes (DWORD id, IMFAttributes** a) override
    {
        if (a == nullptr) return E_POINTER;
        std::lock_guard<std::mutex> l (lock);
        if (shut) return MF_E_SHUTDOWN;
        if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
        return streamAttributes.CopyTo (a);
    }
    IFACEMETHODIMP SetD3DManager (IUnknown*) override { return S_OK; }   // frames are in system memory

    // IMFGetService
    IFACEMETHODIMP GetService (REFGUID, REFIID, LPVOID*) override { return MF_E_UNSUPPORTED_SERVICE; }

    // IKsControl
    IFACEMETHODIMP KsProperty (PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override { return notFound(); }
    IFACEMETHODIMP KsMethod (PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override     { return notFound(); }
    IFACEMETHODIMP KsEvent (PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override       { return notFound(); }

private:
    std::mutex lock;
    ComPtr<IMFMediaEventQueue> queue;
    ComPtr<IMFAttributes> attributes;
    ComPtr<IMFStreamDescriptor> streamAttributes;
    ComPtr<IMFPresentationDescriptor> presentation;
    ComPtr<Stream> stream;
    bool shut = false, announced = false;
};

//==============================================================================
/** What the Frame Server creates from the CLSID: an activation object that makes the source. */
class Activator final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ChainInterfaces<IMFActivate, IMFAttributes>>, private Counted
{
public:
    HRESULT init() { return MFCreateAttributes (&a, 4); }

    IFACEMETHODIMP ActivateObject (REFIID riid, void** ppv) override
    {
        if (ppv == nullptr) return E_POINTER;
        std::lock_guard<std::mutex> l (lock);
        if (! source)
        {
            auto s = Make<Source>();
            if (! s) return E_OUTOFMEMORY;
            const HRESULT hr = s->init();
            if (FAILED (hr)) return hr;
            source = s;
        }
        return source.CopyTo (riid, ppv);
    }
    IFACEMETHODIMP ShutdownObject() override
    {
        std::lock_guard<std::mutex> l (lock);
        if (source) source->Shutdown();
        source.Reset();
        return S_OK;
    }
    IFACEMETHODIMP DetachObject() override
    {
        std::lock_guard<std::mutex> l (lock);
        source.Reset();
        return S_OK;
    }

    // IMFAttributes: an ordinary attribute store.
    IFACEMETHODIMP GetItem (REFGUID k, PROPVARIANT* v) override                                  { return a->GetItem (k, v); }
    IFACEMETHODIMP GetItemType (REFGUID k, MF_ATTRIBUTE_TYPE* t) override                        { return a->GetItemType (k, t); }
    IFACEMETHODIMP CompareItem (REFGUID k, REFPROPVARIANT v, BOOL* r) override                   { return a->CompareItem (k, v, r); }
    IFACEMETHODIMP Compare (IMFAttributes* o, MF_ATTRIBUTES_MATCH_TYPE t, BOOL* r) override      { return a->Compare (o, t, r); }
    IFACEMETHODIMP GetUINT32 (REFGUID k, UINT32* v) override                                     { return a->GetUINT32 (k, v); }
    IFACEMETHODIMP GetUINT64 (REFGUID k, UINT64* v) override                                     { return a->GetUINT64 (k, v); }
    IFACEMETHODIMP GetDouble (REFGUID k, double* v) override                                     { return a->GetDouble (k, v); }
    IFACEMETHODIMP GetGUID (REFGUID k, GUID* v) override                                         { return a->GetGUID (k, v); }
    IFACEMETHODIMP GetStringLength (REFGUID k, UINT32* n) override                               { return a->GetStringLength (k, n); }
    IFACEMETHODIMP GetString (REFGUID k, LPWSTR s, UINT32 n, UINT32* len) override               { return a->GetString (k, s, n, len); }
    IFACEMETHODIMP GetAllocatedString (REFGUID k, LPWSTR* s, UINT32* n) override                 { return a->GetAllocatedString (k, s, n); }
    IFACEMETHODIMP GetBlobSize (REFGUID k, UINT32* n) override                                   { return a->GetBlobSize (k, n); }
    IFACEMETHODIMP GetBlob (REFGUID k, UINT8* b, UINT32 n, UINT32* len) override                 { return a->GetBlob (k, b, n, len); }
    IFACEMETHODIMP GetAllocatedBlob (REFGUID k, UINT8** b, UINT32* n) override                   { return a->GetAllocatedBlob (k, b, n); }
    IFACEMETHODIMP GetUnknown (REFGUID k, REFIID r, LPVOID* p) override                          { return a->GetUnknown (k, r, p); }
    IFACEMETHODIMP SetItem (REFGUID k, REFPROPVARIANT v) override                                { return a->SetItem (k, v); }
    IFACEMETHODIMP DeleteItem (REFGUID k) override                                               { return a->DeleteItem (k); }
    IFACEMETHODIMP DeleteAllItems() override                                                     { return a->DeleteAllItems(); }
    IFACEMETHODIMP SetUINT32 (REFGUID k, UINT32 v) override                                      { return a->SetUINT32 (k, v); }
    IFACEMETHODIMP SetUINT64 (REFGUID k, UINT64 v) override                                      { return a->SetUINT64 (k, v); }
    IFACEMETHODIMP SetDouble (REFGUID k, double v) override                                      { return a->SetDouble (k, v); }
    IFACEMETHODIMP SetGUID (REFGUID k, REFGUID v) override                                       { return a->SetGUID (k, v); }
    IFACEMETHODIMP SetString (REFGUID k, LPCWSTR v) override                                     { return a->SetString (k, v); }
    IFACEMETHODIMP SetBlob (REFGUID k, const UINT8* b, UINT32 n) override                        { return a->SetBlob (k, b, n); }
    IFACEMETHODIMP SetUnknown (REFGUID k, IUnknown* u) override                                  { return a->SetUnknown (k, u); }
    IFACEMETHODIMP LockStore() override                                                          { return a->LockStore(); }
    IFACEMETHODIMP UnlockStore() override                                                        { return a->UnlockStore(); }
    IFACEMETHODIMP GetCount (UINT32* n) override                                                 { return a->GetCount (n); }
    IFACEMETHODIMP GetItemByIndex (UINT32 i, GUID* k, PROPVARIANT* v) override                   { return a->GetItemByIndex (i, k, v); }
    IFACEMETHODIMP CopyAllItems (IMFAttributes* d) override                                      { return a->CopyAllItems (d); }

private:
    std::mutex lock;
    ComPtr<IMFAttributes> a;
    ComPtr<Source> source;
};

class Factory final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IClassFactory>
{
public:
    IFACEMETHODIMP CreateInstance (IUnknown* outer, REFIID riid, void** ppv) override
    {
        if (ppv == nullptr) return E_POINTER;
        *ppv = nullptr;
        if (outer != nullptr) return CLASS_E_NOAGGREGATION;
        if (riid == __uuidof (IMFMediaSource) || riid == __uuidof (IMFMediaSourceEx))
        {
            auto s = Make<Source>();
            if (! s) return E_OUTOFMEMORY;
            const HRESULT hr = s->init();
            return FAILED (hr) ? hr : s.CopyTo (riid, ppv);
        }
        auto act = Make<Activator>();
        if (! act) return E_OUTOFMEMORY;
        const HRESULT hr = act->init();
        return FAILED (hr) ? hr : act.CopyTo (riid, ppv);
    }
    IFACEMETHODIMP LockServer (BOOL lockIt) override
    {
        if (lockIt) ++gLocks; else --gLocks;
        return S_OK;
    }
};
} // namespace

extern "C" HRESULT __stdcall DllGetClassObject (REFCLSID clsid, REFIID riid, void** ppv)
{
    if (ppv == nullptr) return E_POINTER;
    *ppv = nullptr;
    if (clsid != hydra::camshare::kClsid) return CLASS_E_CLASSNOTAVAILABLE;
    auto f = Make<Factory>();
    return f ? f.CopyTo (riid, ppv) : E_OUTOFMEMORY;
}

extern "C" HRESULT __stdcall DllCanUnloadNow()
{
    return gObjects.load() == 0 && gLocks.load() == 0 ? S_OK : S_FALSE;
}

#pragma comment (linker, "/EXPORT:DllGetClassObject,PRIVATE")
#pragma comment (linker, "/EXPORT:DllCanUnloadNow,PRIVATE")

BOOL APIENTRY DllMain (HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls (module);
    return TRUE;
}

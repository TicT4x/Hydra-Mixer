#include "IoStreams.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <tlhelp32.h>

namespace hydra
{
//==============================================================================
juce::String IoTarget::describe() const
{
    const auto pair = mono ? juce::String (channel + 1) : juce::String (channel + 1) + "/" + juce::String (channel + 2);
    switch (kind)
    {
        case Kind::Master:   return "Master " + pair;
        case Kind::Device:   return deviceName + (channel > 0 || mono ? " [" + pair + "]" : juce::String());
        case Kind::Loopback: return "Loopback: " + deviceName;
        case Kind::Virtual:  return "Hydra Virtual " + juce::String (virtualIndex + 1);
        case Kind::App:      return "App: " + deviceName;
        case Kind::None:
        default:             return "- none -";
    }
}

std::unique_ptr<juce::XmlElement> IoTarget::toXml (const juce::String& tag) const
{
    auto e = std::make_unique<juce::XmlElement> (tag);
    e->setAttribute ("kind", (int) kind);
    e->setAttribute ("type", typeName);
    e->setAttribute ("device", deviceName);
    e->setAttribute ("channel", channel);
    e->setAttribute ("mono", mono);
    e->setAttribute ("virtual", virtualIndex);
    return e;
}

IoTarget IoTarget::fromXml (const juce::XmlElement* e)
{
    IoTarget t;
    if (e == nullptr) return t;
    t.kind = (Kind) juce::jlimit (0, 5, e->getIntAttribute ("kind"));
    t.typeName = e->getStringAttribute ("type");
    t.deviceName = e->getStringAttribute ("device");
    t.channel = e->getIntAttribute ("channel");
    t.mono = e->getBoolAttribute ("mono");
    t.virtualIndex = e->getIntAttribute ("virtual");
    return t;
}

//==============================================================================
/** Engine blocks the drift controller takes its fill minimum over: four device periods (all
    phases of the device/engine block pattern), at least 8 blocks. */
static int minWindow (double perBlock, int deviceBlock)
{
    return juce::jmax (8, (int) std::ceil (4.0 * (deviceBlock + perBlock) / juce::jmax (1.0, perBlock)));
}

void AsyncInput::setup (double devRate, int devBlock, int numChannels, double engRate, int engBlock)
{
    deviceRate = devRate; deviceBlock = juce::jmax (16, devBlock);
    engineRate = engRate; engineBlock = engBlock;
    channels = juce::jlimit (1, kMaxStreamChannels, numChannels);
    allocBuffers();

    const double ratio = deviceRate / engineRate;              // device frames per engine frame
    const double perBlock = engineBlock * ratio;
    ring.allocate (channels, (int) (perBlock * 4 + deviceBlock * 8 + 16384));
    resampler.prepare (channels, ratio, kMaxBlock);
    // The two clocks slip against each other: now and then an engine block finds one device
    // block less than usual. With a fractional block ratio that happens all the time and the
    // drift controller sees it; with a (near) whole-number ratio (64 vs 128 at the same rate)
    // only every few seconds, so it is kept as a fixed margin.
    // (not "small": <rpcndr.h> defines it as a macro for char)
    const double larger = juce::jmax (perBlock, (double) deviceBlock), smaller = juce::jmin (perBlock, (double) deviceBlock);
    const double frac = larger / smaller - std::floor (larger / smaller);
    slipMargin = (frac < 0.25 || frac > 0.75) ? smaller : 0.0;
    // Fill measured *before* the engine reads: must cover one engine block + the device's burst size.
    drift.prepare (juce::jmax (perBlock + deviceBlock + 32, perBlock + slipMargin + 32), perBlock + slipMargin + 16,
                   perBlock + deviceBlock * 6 + 4096,
                   juce::jmax (16.0, deviceBlock * 0.25), engineRate, engineBlock, deviceRate, minWindow (perBlock, deviceBlock));
    primed = false;
}

void AsyncInput::pullBlock (int n) noexcept
{
    const int avail = ring.available();
    const int need = resampler.inputNeededFor (n);

    if (! primed)
    {
        if (avail >= (int) drift.getTarget())
            primed = true;
        else
        {
            for (int c = 0; c < channels; ++c) std::memset (bufPtrs[c], 0, (size_t) n * sizeof (float));
            return;
        }
    }

    // Engine stalled / device burst: drop the excess instead of carrying latency forever.
    if (avail > (int) drift.getTarget() + deviceBlock * 8 + 2048)
        ring.skip (avail - (int) drift.getTarget());

    const double corr = drift.update ((double) ring.available(), need + slipMargin, false);
    resampler.setCorrection (corr);

    if (! resampler.pull (ring, bufPtrs, n))
    {
        underruns.fetch_add (1, std::memory_order_relaxed);
        drift.update ((double) ring.available(), need + slipMargin, true);
        primed = false;
    }

    latencyFrames.store (ring.available() + resampler.getLatencyFrames(), std::memory_order_relaxed);
}

//==============================================================================
void AsyncOutput::setup (double devRate, int devBlock, int numChannels, double engRate, int engBlock)
{
    deviceRate = devRate; deviceBlock = juce::jmax (16, devBlock);
    engineRate = engRate; engineBlock = engBlock;
    channels = juce::jlimit (1, kMaxStreamChannels, numChannels);
    allocBuffers();

    const double ratio = engineRate / deviceRate;               // engine frames per device frame
    const double perBlock = engineBlock / ratio;                 // device frames produced per engine block
    ring.allocate (channels, (int) (perBlock * 4 + deviceBlock * 8 + 16384));
    resampler.prepare (channels, ratio, (int) (kMaxBlock / ratio) + 64);
    // Fill measured *after* the engine wrote: must survive until the next engine block. Until then
    // the device takes one block, or more when its blocks are shorter than the engine's (an ASIO
    // device at 64 next to a master at 128 takes up to 3 blocks between two engine blocks).
    minSafeFill = (std::floor (perBlock / deviceBlock) + 1.0) * deviceBlock;
    drift.prepare (juce::jmax (perBlock + deviceBlock + 32, minSafeFill + 32), minSafeFill + 16, perBlock + deviceBlock * 6 + 4096,
                   juce::jmax (16.0, deviceBlock * 0.25), engineRate, engineBlock, deviceRate, minWindow (perBlock, deviceBlock));
    primeTarget.store ((int) drift.getTarget());
    primed = false;
}

void AsyncOutput::pushBlock (int n) noexcept
{
    const float* in[kMaxStreamChannels];
    for (int c = 0; c < channels; ++c) in[c] = bufPtrs[c];
    resampler.push (in, n, ring);

    const int avail = ring.available();
    const bool under = devUnderrun.exchange (false, std::memory_order_relaxed);
    if (under) underruns.fetch_add (1, std::memory_order_relaxed);
    resampler.setCorrection (drift.update ((double) avail, minSafeFill, under));
    primeTarget.store ((int) drift.getTarget(), std::memory_order_relaxed);
    latencyFrames.store (avail + resampler.getLatencyFrames(), std::memory_order_relaxed);
}

void AsyncOutput::consume (float* const* out, int numCh, int n) noexcept
{
    if (! primed && ring.available() >= primeTarget.load (std::memory_order_relaxed))
        primed = true;

    int got = 0;
    if (primed)
    {
        float* dst[kMaxDeviceChannels] {};
        const int useCh = juce::jmin (numCh, channels);
        for (int c = 0; c < useCh; ++c) dst[c] = out[c];
        got = ring.read (dst, useCh, n);
        if (got < n)
        {
            devUnderrun.store (true, std::memory_order_relaxed);
            primed = false;
        }
    }

    for (int c = 0; c < numCh; ++c)
    {
        if (out[c] == nullptr) continue;
        if (c >= channels) std::memset (out[c], 0, (size_t) n * sizeof (float));
        else if (got < n)  std::memset (out[c] + got, 0, (size_t) (n - got) * sizeof (float));
    }
}

//==============================================================================
namespace
{
juce::AudioIODeviceType* findType (juce::OwnedArray<juce::AudioIODeviceType>& types, const juce::String& name)
{
    for (auto* t : types)
        if (t->getTypeName() == name)
            return t;
    return nullptr;
}

double pickRate (juce::AudioIODevice& d, double preferred)
{
    auto rates = d.getAvailableSampleRates();
    if (rates.contains (preferred)) return preferred;
    const double cur = d.getCurrentSampleRate();
    if (cur > 0) return cur;
    return rates.isEmpty() ? preferred : rates.getLast();
}

int pickBuffer (juce::AudioIODevice& d)
{
    auto sizes = d.getAvailableBufferSizes();
    if (sizes.isEmpty()) return d.getDefaultBufferSize();
    // Smallest size that the device offers (low latency); WASAPI shared gives one fixed value.
    return sizes.getFirst();
}

/** Input from a JUCE device (WASAPI shared / low latency / exclusive). */
class DeviceInput final : public AsyncInput, private juce::AudioIODeviceCallback
{
public:
    bool open (juce::AudioIODeviceType& type, const juce::String& name, double engRate, int engBlock)
    {
        device.reset (type.createDevice ({}, name));
        if (device == nullptr) { error = "Cannot create device " + name; return false; }

        const int numCh = juce::jlimit (1, 8, device->getInputChannelNames().size());
        juce::BigInteger ins; ins.setRange (0, numCh, true);
        const double rate = pickRate (*device, engRate);
        error = device->open (ins, {}, rate, pickBuffer (*device));
        if (error.isNotEmpty()) return false;

        setup (device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples(), numCh, engRate, engBlock);
        deviceLatencyFrames = device->getInputLatencyInSamples();
        return true;
    }

    ~DeviceInput() override { stop(); if (device) device->close(); }
    void start() override   { if (device) device->start (this); }
    void stop() override    { if (device) device->stop(); }

private:
    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                           int n, const juce::AudioIODeviceCallbackContext&) override
    {
        produce (in, numIn, n);
        for (int c = 0; c < numOut; ++c) if (out[c]) std::memset (out[c], 0, (size_t) n * sizeof (float));
    }
    void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}
    void audioDeviceError (const juce::String& e) override { error = e; failed = true; }

    std::unique_ptr<juce::AudioIODevice> device;
};

/** Output to a JUCE device. */
class DeviceOutput final : public AsyncOutput, private juce::AudioIODeviceCallback
{
public:
    bool open (juce::AudioIODeviceType& type, const juce::String& name, double engRate, int engBlock)
    {
        device.reset (type.createDevice (name, {}));
        if (device == nullptr) { error = "Cannot create device " + name; return false; }

        const int numCh = juce::jlimit (1, 8, device->getOutputChannelNames().size());
        juce::BigInteger outs; outs.setRange (0, numCh, true);
        const double rate = pickRate (*device, engRate);
        error = device->open ({}, outs, rate, pickBuffer (*device));
        if (error.isNotEmpty()) return false;

        setup (device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples(), numCh, engRate, engBlock);
        deviceLatencyFrames = device->getOutputLatencyInSamples();
        return true;
    }

    ~DeviceOutput() override { stop(); if (device) device->close(); }
    void start() override    { if (device) device->start (this); }
    void stop() override     { if (device) device->stop(); }

private:
    void audioDeviceIOCallbackWithContext (const float* const*, int, float* const* out, int numOut,
                                           int n, const juce::AudioIODeviceCallbackContext&) override
    {
        consume (out, numOut, n);
    }
    void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}
    void audioDeviceError (const juce::String& e) override { error = e; failed = true; }

    std::unique_ptr<juce::AudioIODevice> device;
};

//==============================================================================
template <typename T> struct ComPtr
{
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

juce::String friendlyName (IMMDevice* dev)
{
    ComPtr<IPropertyStore> props;
    if (FAILED (dev->OpenPropertyStore (STGM_READ, &props))) return {};
    PROPVARIANT v;
    PropVariantInit (&v);
    juce::String name;
    if (SUCCEEDED (props->GetValue (PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR)
        name = juce::String (v.pwszVal);
    PropVariantClear (&v);
    return name;
}

bool findRenderEndpoint (const juce::String& name, IMMDevice** out)
{
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof (IMMDeviceEnumerator), (void**) &en)))
        return false;
    ComPtr<IMMDeviceCollection> col;
    if (FAILED (en->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, &col)))
        return false;
    UINT count = 0;
    col->GetCount (&count);
    for (UINT i = 0; i < count; ++i)
    {
        IMMDevice* d = nullptr;
        if (SUCCEEDED (col->Item (i, &d)))
        {
            if (friendlyName (d) == name) { *out = d; return true; }
            d->Release();
        }
    }
    return false;
}

/** Captures what Windows plays to a render endpoint (WASAPI loopback). Plays silence into the
    same endpoint so the loopback clock keeps running even when no application is playing. */
class LoopbackInput final : public AsyncInput, private juce::Thread
{
public:
    LoopbackInput() : juce::Thread ("Hydra Loopback") {}
    ~LoopbackInput() override { stop(); }

    bool open (const juce::String& name, double engRate, int engBlock)
    {
        endpointName = name;
        startThread (juce::Thread::Priority::highest);
        if (! ready.wait (3000) || ! initOk)
        {
            stop();
            if (error.isEmpty()) error = "Loopback init failed for " + name;
            return false;
        }
        setup (devRate, devBlock, devChannels, engRate, engBlock);
        running = true;
        return true;
    }

    void start() override { capturing = true; }
    void stop() override  { capturing = false; signalThreadShouldExit(); if (wake) SetEvent (wake); stopThread (2000); }

private:
    void run() override
    {
        CoInitializeEx (nullptr, COINIT_MULTITHREADED);
        DWORD taskIndex = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW (L"Pro Audio", &taskIndex);
        if (mmcss) AvSetMmThreadPriority (mmcss, AVRT_PRIORITY_HIGH);

        runCapture();

        if (mmcss) AvRevertMmThreadCharacteristics (mmcss);
        CoUninitialize();
    }

    void runCapture()
    {
        ComPtr<IMMDevice> dev;
        if (! findRenderEndpoint (endpointName, &dev)) { error = "Endpoint not found: " + endpointName; ready.signal(); return; }

        ComPtr<IAudioClient> cap, ren;
        if (FAILED (dev->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, (void**) &cap))
            || FAILED (dev->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, (void**) &ren)))
        { error = "Activate failed"; ready.signal(); return; }

        WAVEFORMATEX* fmt = nullptr;
        if (FAILED (cap->GetMixFormat (&fmt))) { error = "GetMixFormat failed"; ready.signal(); return; }

        bool isFloat = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
            isFloat = reinterpret_cast<WAVEFORMATEXTENSIBLE*> (fmt)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        const int bits = fmt->wBitsPerSample;
        const int nCh = fmt->nChannels;

        wake = CreateEventW (nullptr, FALSE, FALSE, nullptr);
        REFERENCE_TIME defPeriod = 0, minPeriod = 0;
        cap->GetDevicePeriod (&defPeriod, &minPeriod);

        HRESULT hr = cap->Initialize (AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                      defPeriod * 2, 0, fmt, nullptr);
        bool eventDriven = SUCCEEDED (hr) && SUCCEEDED (cap->SetEventHandle (wake));
        if (! eventDriven)
        {
            cap.p->Release(); cap.p = nullptr;
            dev->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, (void**) &cap);
            hr = cap->Initialize (AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, defPeriod * 2, 0, fmt, nullptr);
        }
        if (FAILED (hr)) { error = "Loopback Initialize failed"; CoTaskMemFree (fmt); ready.signal(); return; }

        // Silent keep-alive render stream.
        const bool haveRender = SUCCEEDED (ren->Initialize (AUDCLNT_SHAREMODE_SHARED, 0, 1000000 /*100 ms*/, 0, fmt, nullptr));
        ComPtr<IAudioRenderClient> rc;
        UINT32 renFrames = 0;
        if (haveRender && SUCCEEDED (ren->GetService (__uuidof (IAudioRenderClient), (void**) &rc)))
        {
            ren->GetBufferSize (&renFrames);
            BYTE* d = nullptr;
            if (SUCCEEDED (rc->GetBuffer (renFrames, &d))) rc->ReleaseBuffer (renFrames, AUDCLNT_BUFFERFLAGS_SILENT);
            ren->Start();
        }

        ComPtr<IAudioCaptureClient> cc;
        if (FAILED (cap->GetService (__uuidof (IAudioCaptureClient), (void**) &cc)))
        { error = "GetService failed"; CoTaskMemFree (fmt); ready.signal(); return; }

        devRate = fmt->nSamplesPerSec;
        devBlock = (int) (defPeriod * devRate / 10000000.0);
        devChannels = juce::jmin (8, nCh);
        initOk = true;
        ready.signal();

        std::vector<float> conv;
        cap->Start();
        while (! threadShouldExit())
        {
            WaitForSingleObject (wake, eventDriven ? 20 : 2);

            if (rc)
            {
                UINT32 pad = 0;
                if (SUCCEEDED (ren->GetCurrentPadding (&pad)) && renFrames > pad)
                {
                    BYTE* d = nullptr;
                    const UINT32 freeF = renFrames - pad;
                    if (SUCCEEDED (rc->GetBuffer (freeF, &d))) rc->ReleaseBuffer (freeF, AUDCLNT_BUFFERFLAGS_SILENT);
                }
            }

            UINT32 packet = 0;
            while (SUCCEEDED (cc->GetNextPacketSize (&packet)) && packet > 0)
            {
                BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
                if (FAILED (cc->GetBuffer (&data, &frames, &flags, nullptr, nullptr))) break;

                if (capturing.load (std::memory_order_relaxed) && running)
                {
                    conv.resize ((size_t) frames * (size_t) nCh);
                    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr)
                        std::fill (conv.begin(), conv.end(), 0.0f);
                    else if (isFloat && bits == 32)
                        std::memcpy (conv.data(), data, conv.size() * sizeof (float));
                    else if (bits == 16)
                        for (size_t i = 0; i < conv.size(); ++i) conv[i] = reinterpret_cast<const int16_t*> (data)[i] / 32768.0f;
                    else if (bits == 32)
                        for (size_t i = 0; i < conv.size(); ++i) conv[i] = (float) (reinterpret_cast<const int32_t*> (data)[i] / 2147483648.0);
                    else
                        std::fill (conv.begin(), conv.end(), 0.0f);

                    produceInterleaved (conv.data(), nCh, (int) frames);
                }
                cc->ReleaseBuffer (frames);
            }
        }

        cap->Stop();
        if (rc) ren->Stop();
        CoTaskMemFree (fmt);
        CloseHandle (wake);
        wake = nullptr;
    }

    juce::String endpointName;
    juce::WaitableEvent ready;
    std::atomic<bool> capturing { false };
    bool initOk = false, running = false;
    double devRate = 48000.0;
    int devBlock = 480, devChannels = 2;
    HANDLE wake = nullptr;
};

//==============================================================================
/** Completion handler for ActivateAudioInterfaceAsync (must be agile / free-threaded). */
class ActivationHandler final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject
{
public:
    ActivationHandler() { done = CreateEventW (nullptr, TRUE, FALSE, nullptr); }
    ~ActivationHandler() { if (client) client->Release(); CloseHandle (done); }

    HRESULT STDMETHODCALLTYPE QueryInterface (REFIID iid, void** out) override
    {
        if (iid == __uuidof (IUnknown) || iid == __uuidof (IActivateAudioInterfaceCompletionHandler))
            *out = static_cast<IActivateAudioInterfaceCompletionHandler*> (this);
        else if (iid == __uuidof (IAgileObject))
            *out = static_cast<IAgileObject*> (this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override  { return (ULONG) InterlockedIncrement (&refs); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const auto r = InterlockedDecrement (&refs);
        if (r == 0) delete this;
        return (ULONG) r;
    }

    HRESULT STDMETHODCALLTYPE ActivateCompleted (IActivateAudioInterfaceAsyncOperation* op) override
    {
        IUnknown* unk = nullptr;
        HRESULT hrAct = E_FAIL;
        result = op->GetActivateResult (&hrAct, &unk);
        if (SUCCEEDED (result)) result = hrAct;
        if (SUCCEEDED (result) && unk != nullptr)
            result = unk->QueryInterface (__uuidof (IAudioClient), (void**) &client);
        if (unk) unk->Release();
        SetEvent (done);
        return S_OK;
    }

    HANDLE done = nullptr;
    HRESULT result = E_PENDING;
    IAudioClient* client = nullptr;

private:
    LONG refs = 1;
};

juce::String exeNameOf (DWORD pid)
{
    juce::String name;
    if (HANDLE h = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))
    {
        wchar_t path[MAX_PATH];
        DWORD len = MAX_PATH;
        if (QueryFullProcessImageNameW (h, 0, path, &len))
            name = juce::File (juce::String (path)).getFileName();
        CloseHandle (h);
    }
    return name;
}

/** Root process of all running instances of `exe` (parent is not the same executable),
    so INCLUDE_TARGET_PROCESS_TREE also captures its child/renderer processes. */
DWORD findRootProcess (const juce::String& exe)
{
    HANDLE snap = CreateToolhelp32Snapshot (TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    std::vector<std::pair<DWORD, DWORD>> matches; // pid, parent
    juce::Array<DWORD> all;
    PROCESSENTRY32W pe { sizeof (pe) };
    for (BOOL ok = Process32FirstW (snap, &pe); ok; ok = Process32NextW (snap, &pe))
        if (juce::String (pe.szExeFile).equalsIgnoreCase (exe))
            matches.push_back ({ pe.th32ProcessID, pe.th32ParentProcessID });
    CloseHandle (snap);
    for (auto& [pid, parent] : matches)
    {
        bool parentIsSame = false;
        for (auto& [p2, unused] : matches) parentIsSame |= p2 == parent;
        if (! parentIsSame) return pid;
    }
    return matches.empty() ? 0 : matches.front().first;
}

/** Captures everything a single application (and its child processes) plays, via Windows'
    process-loopback virtual device. No driver needed; reconnects when the app restarts. */
class AppCaptureInput final : public AsyncInput, private juce::Thread
{
public:
    AppCaptureInput() : juce::Thread ("Hydra App Capture") {}
    ~AppCaptureInput() override { stop(); }

    bool open (const juce::String& exe, double engRate, int engBlock)
    {
        exeName = exe;
        // Fixed capture format; Windows converts the app's audio to it.
        setup (48000.0, 480, 2, engRate, engBlock);
        idle = true;
        startThread (juce::Thread::Priority::highest);
        return true;
    }

    void start() override { capturing = true; }
    void stop() override
    {
        capturing = false;
        signalThreadShouldExit();
        if (wake) SetEvent (wake);
        stopThread (3000);
    }

private:
    void run() override
    {
        CoInitializeEx (nullptr, COINIT_MULTITHREADED);
        DWORD taskIndex = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW (L"Pro Audio", &taskIndex);
        wake = CreateEventW (nullptr, FALSE, FALSE, nullptr);

        while (! threadShouldExit())
        {
            const DWORD pid = findRootProcess (exeName);
            if (pid == 0)
            {
                idle = true;
                wait (1000);
                continue;
            }
            captureProcess (pid);
            idle = true;
            wait (500);
        }

        CloseHandle (wake);
        wake = nullptr;
        if (mmcss) AvRevertMmThreadCharacteristics (mmcss);
        CoUninitialize();
    }

    void captureProcess (DWORD pid)
    {
        AUDIOCLIENT_ACTIVATION_PARAMS params {};
        params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        params.ProcessLoopbackParams.TargetProcessId = pid;
        params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

        PROPVARIANT pv {};
        pv.vt = VT_BLOB;
        pv.blob.cbSize = sizeof (params);
        pv.blob.pBlobData = reinterpret_cast<BYTE*> (&params);

        auto* handler = new ActivationHandler();
        IActivateAudioInterfaceAsyncOperation* op = nullptr;
        HRESULT hr = ActivateAudioInterfaceAsync (VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof (IAudioClient), &pv, handler, &op);
        if (SUCCEEDED (hr)) WaitForSingleObject (handler->done, 5000);
        IAudioClient* client = SUCCEEDED (hr) && SUCCEEDED (handler->result) ? handler->client : nullptr;
        if (client) client->AddRef();
        if (op) op->Release();
        handler->Release();
        if (client == nullptr) { error = "Process loopback activation failed"; return; }

        WAVEFORMATEX fmt {};
        fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        fmt.nChannels = 2;
        fmt.nSamplesPerSec = 48000;
        fmt.wBitsPerSample = 32;
        fmt.nBlockAlign = 8;
        fmt.nAvgBytesPerSec = 48000 * 8;

        hr = client->Initialize (AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                                   | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                 200000 /* 20 ms */, 0, &fmt, nullptr);
        IAudioCaptureClient* cc = nullptr;
        if (SUCCEEDED (hr)) hr = client->SetEventHandle (wake);
        if (SUCCEEDED (hr)) hr = client->GetService (__uuidof (IAudioCaptureClient), (void**) &cc);
        if (FAILED (hr) || cc == nullptr)
        {
            error = "Process loopback init failed";
            client->Release();
            return;
        }

        HANDLE proc = OpenProcess (SYNCHRONIZE, FALSE, pid);
        client->Start();
        idle = false;
        error.clear();

        while (! threadShouldExit())
        {
            WaitForSingleObject (wake, 20);
            if (proc != nullptr && WaitForSingleObject (proc, 0) == WAIT_OBJECT_0)
                break; // app closed -> look for it again

            UINT32 packet = 0;
            while (SUCCEEDED (cc->GetNextPacketSize (&packet)) && packet > 0)
            {
                BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
                if (FAILED (cc->GetBuffer (&data, &frames, &flags, nullptr, nullptr))) break;
                if (capturing.load (std::memory_order_relaxed))
                {
                    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr)
                    {
                        silence.assign ((size_t) frames * 2, 0.0f);
                        produceInterleaved (silence.data(), 2, (int) frames);
                    }
                    else
                        produceInterleaved (reinterpret_cast<const float*> (data), 2, (int) frames);
                }
                cc->ReleaseBuffer (frames);
            }
        }

        client->Stop();
        cc->Release();
        client->Release();
        if (proc) CloseHandle (proc);
    }

    juce::String exeName;
    std::atomic<bool> capturing { false };
    HANDLE wake = nullptr;
    std::vector<float> silence;
};
} // namespace

//==============================================================================
std::shared_ptr<AsyncInput> openAsyncInput (const IoTarget& t, juce::OwnedArray<juce::AudioIODeviceType>& types,
                                            double engineRate, int engineBlock, juce::String& error,
                                            const std::vector<int>& asioChannels)
{
    if (t.kind == IoTarget::Kind::Device)
    {
        auto* type = findType (types, t.typeName);
        if (type == nullptr) { error = "Unknown driver type " + t.typeName; return nullptr; }
        if (t.typeName == kAsioTypeName)
        {
            auto s = openAsioInput (*type, t.deviceName, engineRate, engineBlock, asioChannels, error);
            if (s != nullptr) s->target = t;
            return s;
        }
        auto s = std::make_shared<DeviceInput>();
        if (! s->open (*type, t.deviceName, engineRate, engineBlock)) { error = s->error; return nullptr; }
        s->target = t;
        return s;
    }

    if (t.kind == IoTarget::Kind::Loopback)
    {
        auto s = std::make_shared<LoopbackInput>();
        if (! s->open (t.deviceName, engineRate, engineBlock)) { error = s->error; return nullptr; }
        s->target = t;
        return s;
    }

    if (t.kind == IoTarget::Kind::App)
    {
        auto s = std::make_shared<AppCaptureInput>();
        if (! s->open (t.deviceName, engineRate, engineBlock)) { error = s->error; return nullptr; }
        s->target = t;
        return s;
    }

    error = "Not an asynchronous input";
    return nullptr;
}

std::shared_ptr<AsyncOutput> openAsyncOutput (const IoTarget& t, juce::OwnedArray<juce::AudioIODeviceType>& types,
                                              double engineRate, int engineBlock, juce::String& error,
                                              const std::vector<int>& asioChannels)
{
    if (t.kind == IoTarget::Kind::Device)
    {
        auto* type = findType (types, t.typeName);
        if (type == nullptr) { error = "Unknown driver type " + t.typeName; return nullptr; }
        if (t.typeName == kAsioTypeName)
        {
            auto s = openAsioOutput (*type, t.deviceName, engineRate, engineBlock, asioChannels, error);
            if (s != nullptr) s->target = t;
            return s;
        }
        auto s = std::make_shared<DeviceOutput>();
        if (! s->open (*type, t.deviceName, engineRate, engineBlock)) { error = s->error; return nullptr; }
        s->target = t;
        return s;
    }

    error = "Not an asynchronous output";
    return nullptr;
}

juce::StringArray listRenderEndpoints()
{
    juce::StringArray names;
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof (IMMDeviceEnumerator), (void**) &en)))
        return names;
    ComPtr<IMMDeviceCollection> col;
    if (FAILED (en->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, &col)))
        return names;
    UINT count = 0;
    col->GetCount (&count);
    for (UINT i = 0; i < count; ++i)
    {
        ComPtr<IMMDevice> d;
        if (SUCCEEDED (col->Item (i, &d)))
            names.add (friendlyName (d.p));
    }
    return names;
}

juce::StringArray listAudioApps()
{
    juce::StringArray names;
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof (IMMDeviceEnumerator), (void**) &en)))
        return names;
    ComPtr<IMMDeviceCollection> col;
    if (FAILED (en->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, &col)))
        return names;

    const DWORD self = GetCurrentProcessId();
    UINT count = 0;
    col->GetCount (&count);
    for (UINT i = 0; i < count; ++i)
    {
        ComPtr<IMMDevice> d;
        if (FAILED (col->Item (i, &d))) continue;
        ComPtr<IAudioSessionManager2> mgr;
        if (FAILED (d->Activate (__uuidof (IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**) &mgr))) continue;
        ComPtr<IAudioSessionEnumerator> sessions;
        if (FAILED (mgr->GetSessionEnumerator (&sessions))) continue;
        int n = 0;
        sessions->GetCount (&n);
        for (int k = 0; k < n; ++k)
        {
            ComPtr<IAudioSessionControl> ctl;
            if (FAILED (sessions->GetSession (k, &ctl))) continue;
            ComPtr<IAudioSessionControl2> ctl2;
            if (FAILED (ctl->QueryInterface (__uuidof (IAudioSessionControl2), (void**) &ctl2))) continue;
            DWORD pid = 0;
            if (FAILED (ctl2->GetProcessId (&pid)) || pid == 0 || pid == self) continue;
            const auto exe = exeNameOf (pid);
            if (exe.isNotEmpty()) names.addIfNotAlreadyThere (exe, true);
        }
    }
    names.sortNatural();
    return names;
}

std::vector<VirtualCable> findVirtualCables (juce::OwnedArray<juce::AudioIODeviceType>& types)
{
    std::vector<VirtualCable> cables;
    auto* type = findType (types, "Windows Audio (Low Latency Mode)");
    if (type == nullptr) type = findType (types, "Windows Audio");
    if (type == nullptr) return cables;

    const auto outs = type->getDeviceNames (false); // playback endpoints (apps play into these)
    const auto ins  = type->getDeviceNames (true);  // recording endpoints (Hydra captures these)

    auto isCandidate = [] (const juce::String& n)
    {
        return ! n.containsIgnoreCase ("Voicemeeter")
            && (n.containsIgnoreCase ("Cable") || n.containsIgnoreCase ("Virtual Audio"));
    };

    for (auto& play : outs)
    {
        if (! isCandidate (play)) continue;
        juce::String rec;
        // VB-Audio style: "CABLE-A Input (VB-Audio Cable A)" <-> "CABLE-A Output (VB-Audio Cable A)"
        const auto head = play.upToFirstOccurrenceOf ("(", false, false);
        const auto tail = play.fromFirstOccurrenceOf ("(", true, false);
        if (head.containsIgnoreCase ("Input"))
        {
            const auto candidate = head.replace ("Input", "Output", true) + tail;
            for (auto& i : ins) if (i.equalsIgnoreCase (candidate)) rec = i;
        }
        // Virtual Audio Cable style: identical name on both sides ("Line 1 (Virtual Audio Cable)")
        if (rec.isEmpty())
            for (auto& i : ins) if (i.equalsIgnoreCase (play)) rec = i;
        if (rec.isEmpty()) continue;

        VirtualCable c;
        c.playbackName = play;
        c.recordingName = rec;
        c.label = head.replace ("Input", "", true).trim();
        if (c.label.isEmpty()) c.label = play;
        cables.push_back (c);
    }
    return cables;
}
} // namespace hydra

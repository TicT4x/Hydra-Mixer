#include "IoStreams.h"
#include "VacStream.h"
#include "NetLink.h"

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
        case Kind::App:
        {
            juce::StringArray shown;
            for (auto& exe : appNames()) shown.add (exe.endsWithIgnoreCase (".exe") ? exe.dropLastCharacters (4) : exe);
            return (shown.size() > 1 ? "Apps: " : "App: ") + shown.joinIntoString (" + ");
        }
        case Kind::Network:
        {
            if (deviceName.startsWith ("hydralink://"))
            {
                const auto d = net::Destination::parse (deviceName);
                return "Network: " + d.stream + " to " + d.host;
            }
            const auto s = net::Source::parse (deviceName);
            return "Network: " + s.stream + " from " + s.sender;
        }
        case Kind::None:
        default:             return "- none -";
    }
}

juce::StringArray IoTarget::appNames() const
{
    juce::StringArray a;
    a.addTokens (deviceName, "|", {});
    a.trim();
    a.removeEmptyStrings();
    return a;
}

IoTarget IoTarget::apps (juce::StringArray exes)
{
    exes.trim();
    exes.removeEmptyStrings();
    exes.removeDuplicates (true);
    exes.sortNatural();   // one order, so the same set is the same stream
    IoTarget t;
    t.kind = exes.isEmpty() ? Kind::None : Kind::App;
    t.deviceName = exes.joinIntoString ("|");
    return t;
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
    t.kind = (Kind) juce::jlimit (0, (int) Kind::Network, e->getIntAttribute ("kind"));
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

    if (direct) deviceRate = engineRate;
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
    drift.setHoldFloor (preferStable);
    drift.prepare (juce::jmax (perBlock + deviceBlock + 32, perBlock + slipMargin + 32), juce::jmax (perBlock + slipMargin + 16, perBlock + jitterFloor),
                   perBlock + deviceBlock * 6 + 4096,
                   juce::jmax (16.0, deviceBlock * 0.25), engineRate, engineBlock, deviceRate,
                   juce::jmax (minWindow (perBlock, deviceBlock), (int) std::ceil (minWindowSeconds * engineRate / juce::jmax (1, engineBlock))));
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
    {
        ring.skip (avail - (int) drift.getTarget());
        underruns.fetch_add (1, std::memory_order_relaxed);   // audible like a dropout (a jump)
    }

    if (direct)
    {
        // Device at the engine rate, its clock steered: no resampling, just the ring.
        clockRatio.store (1.0 / drift.update ((double) ring.available(), n + slipMargin, false), std::memory_order_relaxed);
        const int got = ring.read (bufPtrs, channels, n);
        if (got < n)
        {
            for (int c = 0; c < channels; ++c) std::memset (bufPtrs[c] + got, 0, (size_t) (n - got) * sizeof (float));
            underruns.fetch_add (1, std::memory_order_relaxed);
            drift.update ((double) ring.available(), n + slipMargin, true);
            primed = false;
        }
        latencyFrames.store (ring.available(), std::memory_order_relaxed);
        return;
    }

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

    if (direct) deviceRate = engineRate;
    const double ratio = engineRate / deviceRate;               // engine frames per device frame
    const double perBlock = engineBlock / ratio;                 // device frames produced per engine block
    ring.allocate (channels, (int) (perBlock * 4 + deviceBlock * 8 + 16384));
    resampler.prepare (channels, ratio, (int) (kMaxBlock / ratio) + 64);
    // Fill measured *after* the engine wrote: must survive until the next engine block. Until then
    // the device takes one block, or more when its blocks are shorter than the engine's (an ASIO
    // device at 64 next to a master at 128 takes up to 3 blocks between two engine blocks).
    minSafeFill = (std::floor (perBlock / deviceBlock) + 1.0) * deviceBlock;
    drift.setHoldFloor (preferStable);
    drift.prepare (juce::jmax (perBlock + deviceBlock + 32, minSafeFill + 32), minSafeFill + 16, perBlock + deviceBlock * 6 + 4096,
                   juce::jmax (16.0, deviceBlock * 0.25), engineRate, engineBlock, deviceRate, minWindow (perBlock, deviceBlock));
    primeTarget.store ((int) drift.getTarget());
    primed = false;
}

void AsyncOutput::pushBlock (int n) noexcept
{
    const float* in[kMaxStreamChannels];
    for (int c = 0; c < channels; ++c) in[c] = bufPtrs[c];
    if (direct) ring.write (in, channels, n);   // device at the engine rate, its clock steered
    else        resampler.push (in, n, ring);

    const int avail = ring.available();
    const bool under = devUnderrun.exchange (false, std::memory_order_relaxed);
    if (under) underruns.fetch_add (1, std::memory_order_relaxed);
    const double corr = drift.update ((double) avail, minSafeFill, under);
    // Ring too full -> the device takes too little: resample to fewer frames, or (direct) run the device faster.
    if (direct) clockRatio.store (corr, std::memory_order_relaxed);
    else        resampler.setCorrection (corr);
    primeTarget.store ((int) drift.getTarget(), std::memory_order_relaxed);
    latencyFrames.store (avail + (direct ? 0 : resampler.getLatencyFrames()), std::memory_order_relaxed);
}

void AsyncOutput::consume (float* const* out, int numCh, int n) noexcept
{
    deviceCallbacks.fetch_add (1, std::memory_order_relaxed);
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

/** Creates a JUCE device by name. Runs on the StreamOpener's thread, so it must not scan: the engine makes sure
    (on the message thread) that a device plugged in after the last scan is in the type's list before it asks. */
juce::AudioIODevice* createNamedDevice (juce::AudioIODeviceType& type, const juce::String& name, bool input)
{
    return input ? type.createDevice ({}, name) : type.createDevice (name, {});
}

/** Watchdog for a JUCE device stream: a device that delivers no callbacks for ~2 s (unplugged, driver gone)
    counts as failed, so the engine reopens it (and choosing it again opens it anew instead of reusing it). */
struct StallWatch
{
    void poll (AsyncStream& s)
    {
        const auto n = s.deviceCallbacks.load (std::memory_order_relaxed);
        if (n != seen) { seen = n; quietPolls = 0; return; }
        if (++quietPolls == 20 && ! s.failed.load())
        {
            s.error = "device stopped (unplugged?)";
            s.failed = true;
        }
    }
    uint32_t seen = 0;
    int quietPolls = 0;
};

/** Input from a JUCE device (WASAPI shared / low latency / exclusive). */
/** Software endpoints (virtual cables, Voicemeeter): timer-driven, rare late wake-ups. */
bool isVirtualCableName (const juce::String& n)
{
    return n.containsIgnoreCase ("Cable") || n.containsIgnoreCase ("Virtual Audio") || n.containsIgnoreCase ("Voicemeeter");
}

class DeviceInput final : public AsyncInput, private juce::AudioIODeviceCallback
{
public:
    bool open (juce::AudioIODeviceType& type, const juce::String& name, double engRate, int engBlock)
    {
        device.reset (createNamedDevice (type, name, true));
        if (device == nullptr) { error = "Cannot create device " + name; return false; }

        const int numCh = juce::jlimit (1, 8, device->getInputChannelNames().size());
        juce::BigInteger ins; ins.setRange (0, numCh, true);
        const double rate = pickRate (*device, engRate);
        error = device->open (ins, {}, rate, pickBuffer (*device));
        if (error.isNotEmpty()) return false;

        preferStable = isVirtualCableName (name);
        setup (device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples(), numCh, engRate, engBlock);
        deviceLatencyFrames = device->getInputLatencyInSamples();
        return true;
    }

    ~DeviceInput() override { stop(); if (device) device->close(); }
    void start() override   { if (device) device->start (this); }
    void stop() override    { if (device) device->stop(); }
    void poll() override    { watch.poll (*this); }

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
    StallWatch watch;
};

/** Output to a JUCE device. */
class DeviceOutput final : public AsyncOutput, private juce::AudioIODeviceCallback
{
public:
    bool open (juce::AudioIODeviceType& type, const juce::String& name, double engRate, int engBlock)
    {
        device.reset (createNamedDevice (type, name, false));
        if (device == nullptr) { error = "Cannot create device " + name; return false; }

        const int numCh = juce::jlimit (1, 8, device->getOutputChannelNames().size());
        juce::BigInteger outs; outs.setRange (0, numCh, true);
        const double rate = pickRate (*device, engRate);
        error = device->open ({}, outs, rate, pickBuffer (*device));
        if (error.isNotEmpty()) return false;

        preferStable = isVirtualCableName (name);
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

/** Captures everything one or more applications (and their child processes) play, via Windows' process-loopback
    virtual device, mixed into one stereo stream. No driver needed; each app reconnects when it restarts.

    Several apps: one capture client per app on the same thread. Their packets come from the same Windows audio
    engine, so they are mixed sample by sample from small per-app queues. An app that delivers nothing (not
    playing, stalled) for 30 ms is filled with silence, so it never holds the others back for longer than that.
    With a single app nothing is queued (same latency as before). */
class AppCaptureInput final : public AsyncInput, private juce::Thread
{
public:
    AppCaptureInput() : juce::Thread ("Hydra App Capture") {}
    ~AppCaptureInput() override { stop(); }

    bool open (const juce::StringArray& exes, double engRate, int engBlock)
    {
        for (auto& e : exes) { auto c = std::make_unique<Client>(); c->exe = e; clients.push_back (std::move (c)); }
        if (clients.empty()) { error = "No app chosen"; return false; }
        // Fixed capture format; Windows converts the app's audio to it.
        preferStable = true;   // software routing like a cable: stability before latency
        setup (48000.0, 480, 2, engRate, engBlock);
        idle = true;
        for (auto& c : clients) c->event = CreateEventW (nullptr, FALSE, FALSE, nullptr);
        wake = CreateEventW (nullptr, FALSE, FALSE, nullptr);
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
        for (auto& c : clients) if (c->event) { CloseHandle (c->event); c->event = nullptr; }
        if (wake) { CloseHandle (wake); wake = nullptr; }
    }

private:
    struct Client
    {
        juce::String exe;
        IAudioClient* client = nullptr;
        IAudioCaptureClient* cc = nullptr;
        HANDLE event = nullptr, proc = nullptr;
        std::vector<float> queue;   // interleaved stereo, not mixed yet (several apps only)
        double lastData = 0;        // ms, last packet with frames
        bool active() const noexcept { return cc != nullptr; }
    };

    int numActive() const { return (int) std::count_if (clients.begin(), clients.end(), [] (auto& c) { return c->active(); }); }

    void run() override
    {
        CoInitializeEx (nullptr, COINIT_MULTITHREADED);
        DWORD taskIndex = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW (L"Pro Audio", &taskIndex);
        for (auto& c : clients) c->queue.reserve (48000);
        mix.reserve (48000);

        double lastLook = -1.0e9;
        while (! threadShouldExit())
        {
            double now = juce::Time::getMillisecondCounterHiRes();
            // Look for apps that are not captured yet (started later, restarted).
            if (numActive() < (int) clients.size() && now - lastLook > 1000.0)
            {
                lastLook = now;
                for (auto& c : clients)
                    if (! c->active())
                        if (const DWORD pid = findRootProcess (c->exe); pid != 0) activate (*c, pid);
            }
            idle = numActive() == 0;

            HANDLE events[MAXIMUM_WAIT_OBJECTS];
            DWORD n = 0;
            events[n++] = wake;
            for (auto& c : clients) if (c->active() && n < MAXIMUM_WAIT_OBJECTS) events[n++] = c->event;
            WaitForMultipleObjects (n, events, FALSE, n == 1 ? 500 : 20);
            if (threadShouldExit()) break;

            now = juce::Time::getMillisecondCounterHiRes();
            const bool single = numActive() == 1;
            for (auto& c : clients)
            {
                if (! c->active()) continue;
                if (c->proc != nullptr && WaitForSingleObject (c->proc, 0) == WAIT_OBJECT_0) { close (*c); continue; }   // app closed -> look for it again
                drain (*c, single, now);
            }
            if (! single) mixQueues (now);
        }

        for (auto& c : clients) close (*c);
        if (mmcss) AvRevertMmThreadCharacteristics (mmcss);
        CoUninitialize();
    }

    void activate (Client& c, DWORD pid)
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
        if (SUCCEEDED (hr)) hr = client->SetEventHandle (c.event);
        if (SUCCEEDED (hr)) hr = client->GetService (__uuidof (IAudioCaptureClient), (void**) &cc);
        if (FAILED (hr) || cc == nullptr)
        {
            error = "Process loopback init failed";
            client->Release();
            return;
        }
        c.client = client;
        c.cc = cc;
        c.proc = OpenProcess (SYNCHRONIZE, FALSE, pid);
        c.queue.clear();
        c.lastData = juce::Time::getMillisecondCounterHiRes();
        client->Start();
        error.clear();
    }

    static void close (Client& c)
    {
        if (c.client != nullptr) c.client->Stop();
        if (c.cc != nullptr) c.cc->Release();
        if (c.client != nullptr) c.client->Release();
        if (c.proc != nullptr) CloseHandle (c.proc);
        c.cc = nullptr; c.client = nullptr; c.proc = nullptr;
        c.queue.clear();
    }

    /** Reads the app's packets: straight into the ring (single app) or into its queue. */
    void drain (Client& c, bool single, double now)
    {
        UINT32 packet = 0;
        while (SUCCEEDED (c.cc->GetNextPacketSize (&packet)) && packet > 0)
        {
            BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
            if (FAILED (c.cc->GetBuffer (&data, &frames, &flags, nullptr, nullptr))) break;
            if (frames > 0) c.lastData = now;
            if (capturing.load (std::memory_order_relaxed))
            {
                const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr;
                const auto* f = reinterpret_cast<const float*> (data);
                if (single)
                {
                    if (silent)
                    {
                        silence.assign ((size_t) frames * 2, 0.0f);
                        produceInterleaved (silence.data(), 2, (int) frames);
                    }
                    else
                        produceInterleaved (f, 2, (int) frames);
                }
                else if (c.queue.size() < 48000 * 2)   // at most 0.5 s (only if nobody mixes: a bug, not normal)
                {
                    if (silent) c.queue.insert (c.queue.end(), (size_t) frames * 2, 0.0f);
                    else        c.queue.insert (c.queue.end(), f, f + (size_t) frames * 2);
                }
            }
            c.cc->ReleaseBuffer (frames);
        }
    }

    /** Several apps: mixes what all of them have delivered. An app without packets for 30 ms (or 60 ms behind)
        gets silence instead, so it does not hold the others back. */
    void mixQueues (double now)
    {
        size_t most = 0;
        for (auto& c : clients) if (c->active()) most = juce::jmax (most, c->queue.size());
        if (most == 0) return;
        for (auto& c : clients)
            if (c->active() && c->queue.size() < most && (now - c->lastData > 30.0 || most - c->queue.size() > 2880 * 2))
                c->queue.resize (most, 0.0f);
        size_t n = most;
        for (auto& c : clients) if (c->active()) n = juce::jmin (n, c->queue.size());
        n -= n % 2;
        if (n == 0) return;
        mix.assign (n, 0.0f);
        for (auto& c : clients)
            if (c->active())
            {
                juce::FloatVectorOperations::add (mix.data(), c->queue.data(), (int) n);
                c->queue.erase (c->queue.begin(), c->queue.begin() + (std::ptrdiff_t) n);
            }
        produceInterleaved (mix.data(), 2, (int) (n / 2));
    }

    std::vector<std::unique_ptr<Client>> clients;
    std::atomic<bool> capturing { false };
    HANDLE wake = nullptr;
    std::vector<float> silence, mix;
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
        // Virtual Audio Cable: straight to the driver (Kernel Streaming), WASAPI only as fallback.
        if (t.typeName.startsWith ("Windows Audio"))
        {
            juce::String why;
            if (auto v = openVacInput (t, engineRate, engineBlock, why)) return v;
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
        if (! s->open (t.appNames(), engineRate, engineBlock)) { error = s->error; return nullptr; }
        s->target = t;
        return s;
    }

    if (t.kind == IoTarget::Kind::Network)
        return net::Hub::get().openInput (t, engineRate, engineBlock, error);

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
        if (t.typeName.startsWith ("Windows Audio"))
        {
            juce::String why;
            if (auto v = openVacOutput (t, engineRate, engineBlock, why)) return v;
        }
        auto s = std::make_shared<DeviceOutput>();
        if (! s->open (*type, t.deviceName, engineRate, engineBlock)) { error = s->error; return nullptr; }
        s->target = t;
        return s;
    }

    if (t.kind == IoTarget::Kind::Network)
        return net::Hub::get().openOutput (t, engineRate, engineBlock, error);

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

//==============================================================================
StreamOpener::StreamOpener (juce::OwnedArray<juce::AudioIODeviceType>& deviceTypes)
    : juce::Thread ("Hydra device opener"), types (deviceTypes)
{
    startThread();
}

StreamOpener::~StreamOpener()
{
    signalThreadShouldExit();
    notify();
    stopThread (15000);   // a device that hangs while opening must not keep the app from closing forever
}

void StreamOpener::request (const IoTarget& t, bool input, double rate, int block)
{
    const juce::ScopedLock sl (lock);
    for (auto& j : jobs)
        if (j.matches (t, input, rate, block)) return;
    jobs.push_back ({ t, input, rate, block, Job::State::queued, {}, {}, {} });
    notify();
}

bool StreamOpener::isPending (const IoTarget& t, bool input) const
{
    const juce::ScopedLock sl (lock);
    for (auto& j : jobs)
        if (j.input == input && j.target.sameStream (t) && j.state != Job::State::done) return true;
    return false;
}

bool StreamOpener::takeInput (const IoTarget& t, double rate, int block, std::shared_ptr<AsyncInput>& stream, juce::String& error)
{
    const juce::ScopedLock sl (lock);
    for (auto it = jobs.begin(); it != jobs.end(); ++it)
        if (it->state == Job::State::done && it->matches (t, true, rate, block))
        {
            stream = std::move (it->in);
            error = it->error;
            jobs.erase (it);
            --numFinished;
            return true;
        }
    return false;
}

bool StreamOpener::takeOutput (const IoTarget& t, double rate, int block, std::shared_ptr<AsyncOutput>& stream, juce::String& error)
{
    const juce::ScopedLock sl (lock);
    for (auto it = jobs.begin(); it != jobs.end(); ++it)
        if (it->state == Job::State::done && it->matches (t, false, rate, block))
        {
            stream = std::move (it->out);
            error = it->error;
            jobs.erase (it);
            --numFinished;
            return true;
        }
    return false;
}

void StreamOpener::discardFinishedExcept (const std::vector<IoTarget>& wantedIn, const std::vector<IoTarget>& wantedOut, double rate, int block)
{
    const juce::ScopedLock sl (lock);
    for (auto it = jobs.begin(); it != jobs.end();)
    {
        const auto& wanted = it->input ? wantedIn : wantedOut;
        const bool keep = it->state != Job::State::done
                       || (it->rate == rate && it->block == block
                           && std::any_of (wanted.begin(), wanted.end(), [&] (const IoTarget& w) { return w.sameStream (it->target); }));
        if (keep) { ++it; continue; }
        if (it->in != nullptr)  trash.push_back (std::move (it->in));
        if (it->out != nullptr) trash.push_back (std::move (it->out));
        it = jobs.erase (it);
        --numFinished;
        notify();
    }
}

void StreamOpener::dispose (std::shared_ptr<void> stream)
{
    const juce::ScopedLock sl (lock);
    trash.push_back (std::move (stream));
    notify();
}

void StreamOpener::run()
{
    CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    while (! threadShouldExit())
    {
        std::vector<std::shared_ptr<void>> closing;
        Job* job = nullptr;
        {
            const juce::ScopedLock sl (lock);
            closing.swap (trash);
            for (auto& j : jobs)
                if (j.state == Job::State::queued) { j.state = Job::State::opening; job = &j; break; }
        }
        closing.clear();   // closes the devices (in the order they were given up, before anything new opens)

        if (job == nullptr)
        {
            wait (-1);
            continue;
        }

        // The list never loses an element while it is being opened (only finished jobs are removed), so job stays valid.
        juce::String error;
        std::shared_ptr<AsyncInput> in;
        std::shared_ptr<AsyncOutput> out;
        if (job->input) in  = openAsyncInput  (job->target, types, job->rate, job->block, error, {});
        else            out = openAsyncOutput (job->target, types, job->rate, job->block, error, {});

        const juce::ScopedLock sl (lock);
        job->in = std::move (in);
        job->out = std::move (out);
        job->error = job->in != nullptr || job->out != nullptr ? juce::String()
                   : error.isNotEmpty() ? error : juce::String ("could not be opened");
        job->state = Job::State::done;
        ++numFinished;
    }
    const juce::ScopedLock sl (lock);
    trash.clear();
    for (auto& j : jobs) { j.in.reset(); j.out.reset(); }
    jobs.clear();
    CoUninitialize();
}

//==============================================================================
void listAudioAppsAsync (std::function<void (juce::StringArray)> done)
{
    juce::Thread::launch ([done = std::move (done)]
    {
        CoInitializeEx (nullptr, COINIT_MULTITHREADED);
        auto names = listAudioApps();
        CoUninitialize();
        juce::MessageManager::callAsync ([done, names = std::move (names)] { done (names); });
    });
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
    auto* type = findType (types, "Windows Audio (Low Latency Mode)");
    if (type == nullptr) type = findType (types, "Windows Audio");
    if (type == nullptr) return {};

    return findVirtualCables (type->getDeviceNames (false), type->getDeviceNames (true));
}

std::vector<VirtualCable> findVirtualCables (const juce::StringArray& outs, const juce::StringArray& ins)
{
    std::vector<VirtualCable> cables;
    auto isCandidate = [] (const juce::String& n)
    {
        // VB-Cable / Hi-Fi Cable / VB-Audio Point, Virtual Audio Cable, other virtual drivers with both sides.
        // Voicemeeter is not a cable here (it is reached with Loopback).
        return ! n.containsIgnoreCase ("Voicemeeter")
            && (n.containsIgnoreCase ("Cable") || n.containsIgnoreCase ("Virtual") || n.containsIgnoreCase ("VB-Audio"));
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
        // Any other pair of software endpoints with the same driver name in brackets, e.g. a renamed VB-Cable
        // ("Game (VB-Audio Virtual Cable)" <-> "CABLE Output (VB-Audio Virtual Cable)"): only when the pair is unique.
        if (rec.isEmpty() && tail.length() > 2)
        {
            int outsWithTail = 0, found = 0;
            for (auto& o : outs) if (o.endsWithIgnoreCase (tail)) ++outsWithTail;
            for (auto& i : ins) if (i.endsWithIgnoreCase (tail)) { rec = i; ++found; }
            if (outsWithTail != 1 || found != 1) rec = {};
        }
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

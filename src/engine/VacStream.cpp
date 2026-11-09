#include "VacStream.h"
#include "state/VacControl.h"

#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <winioctl.h>
#include <ks.h>
#include <ksmedia.h>
#include <avrt.h>
#include <mutex>
#include <set>
#include <thread>

#pragma comment (lib, "ksuser.lib")
#pragma comment (lib, "avrt.lib")
#pragma comment (lib, "winmm.lib")

namespace hydra
{
namespace
{
std::atomic<bool> directEnabled { true };
std::atomic<int> lateWrites { 0 };   // the cable played past what Hydra wrote (all streams; diagnostics)
std::atomic<double> lateGapMs { 0 }, lateJumpMs { 0 }, maxGapMs { 0 };   // diagnostics of the last late write

//==============================================================================
/** Cables whose clock a Hydra stream steers (one stream per cable). */
std::mutex steerLock;
std::set<int> steered;

bool claimSteering (int cable)
{
    std::lock_guard<std::mutex> l (steerLock);
    return steered.insert (cable).second;
}

void releaseSteering (int cable)
{
    std::lock_guard<std::mutex> l (steerLock);
    steered.erase (cable);
}

//==============================================================================
bool ksProperty (HANDLE h, const GUID& set, ULONG id, ULONG flags, void* in, ULONG inSize, void* out, ULONG outSize, ULONG* got = nullptr)
{
    DWORD n = 0;
    KSPROPERTY* p = static_cast<KSPROPERTY*> (in);
    p->Set = set;
    p->Id = id;
    p->Flags = flags;
    const BOOL ok = DeviceIoControl (h, IOCTL_KS_PROPERTY, in, inSize, out, outSize, &n, nullptr);
    if (got != nullptr) *got = n;
    return ok != FALSE;
}

/** One WaveRT pin of a VAC wave filter: the driver's cyclic buffer, its position and the notification event. */
class KsPin
{
public:
    ~KsPin() { close(); }

    /** capture = Hydra records (the cable's recording side). Tries float32, then 32/24-bit and 16-bit PCM. */
    bool open (const juce::String& filterPath, bool captureSide, int rate, int channelCount, int periodFrames, juce::String& error)
    {
        capture = captureSide;
        filter = CreateFileW (filterPath.toWideCharPointer(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (filter == INVALID_HANDLE_VALUE)
        {
            error = "cannot open the cable's KS filter " + filterPath + " (error " + juce::String ((int) GetLastError()) + ")";
            return false;
        }

        const int pinId = findStreamingPin();
        if (pinId < 0) { error = "no streaming pin on the cable's KS filter"; return false; }

        struct Format { bool isFloat; int bits, valid; };
        for (auto f : { Format { true, 32, 32 }, Format { false, 32, 24 }, Format { false, 32, 32 }, Format { false, 16, 16 } })
            if (createPin (pinId, rate, channelCount, f.isFloat, f.bits, f.valid))
            {
                isFloat = f.isFloat; bits = f.bits; channels = channelCount; this->rate = rate;
                blockAlign = channelCount * bits / 8;
                break;
            }
        if (pin == INVALID_HANDLE_VALUE) { error = "the cable accepts none of Hydra's formats at " + juce::String (rate) + " Hz"; return false; }

        // ~40 ms of cyclic buffer (at least 8 cable periods): a late wake-up of Hydra's cable thread costs nothing
        // as long as it stays below that (latency comes from Hydra's own sync buffer, not from this size).
        const int periods = juce::jmax (8, (int) std::ceil (0.040 * rate / juce::jmax (1, periodFrames)));
        if (! allocateBuffer (periodFrames * periods)) { error = "the cable did not give a WaveRT buffer"; return false; }
        if (! setState (KSSTATE_ACQUIRE) || ! setState (KSSTATE_PAUSE)) { error = "the cable pin did not start"; return false; }
        return true;
    }

    void close()
    {
        if (pin != INVALID_HANDLE_VALUE)
        {
            setState (KSSTATE_PAUSE);
            setState (KSSTATE_ACQUIRE);
            setState (KSSTATE_STOP);
            if (event != nullptr && notifications)
            {
                KSRTAUDIO_NOTIFICATION_EVENT_PROPERTY p {};
                p.NotificationEvent = event;
                ksProperty (pin, KSPROPSETID_RtAudio, KSPROPERTY_RTAUDIO_UNREGISTER_NOTIFICATION_EVENT, KSPROPERTY_TYPE_GET, &p, sizeof (p), nullptr, 0);
            }
            CloseHandle (pin);
            pin = INVALID_HANDLE_VALUE;
        }
        if (filter != INVALID_HANDLE_VALUE) { CloseHandle (filter); filter = INVALID_HANDLE_VALUE; }
        if (event != nullptr) { CloseHandle (event); event = nullptr; }
    }

    bool run() { return setState (KSSTATE_RUN); }

    /** Byte offset of the hardware inside the cyclic buffer (capture: written up to; render: playing). */
    bool position (ULONG& offset) const
    {
        if (posRegister != nullptr) { offset = *posRegister % bufferBytes; return true; }
        KSPROPERTY p {};
        KSAUDIO_POSITION pos {};
        if (! ksProperty (pin, KSPROPSETID_Audio, KSPROPERTY_AUDIO_POSITION, KSPROPERTY_TYPE_GET, &p, sizeof (p), &pos, sizeof (pos)))
            return false;
        offset = (ULONG) (pos.PlayOffset % bufferBytes);
        return true;
    }

    /** Waits for the next notification (or at most timeoutMs). */
    void wait (DWORD timeoutMs) const
    {
        if (event != nullptr && notifications) WaitForSingleObject (event, timeoutMs);
        else Sleep (1);
    }

    // Sample conversion between the cyclic buffer and planar float.
    void read (ULONG byteOffset, int frames, float* const* dst) const
    {
        for (int i = 0; i < frames; ++i)
        {
            const BYTE* s = buffer + (byteOffset + (ULONG) i * (ULONG) blockAlign) % bufferBytes;
            for (int c = 0; c < channels; ++c) dst[c][i] = sample (s, c);
        }
    }

    void write (ULONG byteOffset, int frames, const float* const* src) const
    {
        for (int i = 0; i < frames; ++i)
        {
            BYTE* d = buffer + (byteOffset + (ULONG) i * (ULONG) blockAlign) % bufferBytes;
            for (int c = 0; c < channels; ++c) setSample (d, c, src[c] != nullptr ? src[c][i] : 0.0f);
        }
    }

    void clear() const { if (buffer != nullptr) std::memset (buffer, 0, bufferBytes); }

    juce::String formatName() const { return isFloat ? juce::String ("32-bit float") : juce::String (bits == 32 ? 32 : bits) + "-bit"; }

    ULONG bufferBytes = 0;
    int blockAlign = 8, channels = 2, rate = 48000;
    bool capture = false;

private:
    int findStreamingPin() const
    {
        KSPROPERTY p {};
        ULONG count = 0;
        if (! ksProperty (filter, KSPROPSETID_Pin, KSPROPERTY_PIN_CTYPES, KSPROPERTY_TYPE_GET, &p, sizeof (p), &count, sizeof (count)))
            return -1;
        for (ULONG id = 0; id < count; ++id)
        {
            KSP_PIN pp {};
            pp.PinId = id;
            KSPIN_DATAFLOW flow {};
            KSPIN_COMMUNICATION comm {};
            if (! ksProperty (filter, KSPROPSETID_Pin, KSPROPERTY_PIN_DATAFLOW, KSPROPERTY_TYPE_GET, &pp, sizeof (pp), &flow, sizeof (flow))) continue;
            pp = {};
            pp.PinId = id;
            if (! ksProperty (filter, KSPROPSETID_Pin, KSPROPERTY_PIN_COMMUNICATION, KSPROPERTY_TYPE_GET, &pp, sizeof (pp), &comm, sizeof (comm))) continue;
            // The streaming pin: an app-facing sink. Data leaves a capture filter and enters a render filter.
            const bool sink = comm == KSPIN_COMMUNICATION_SINK || comm == KSPIN_COMMUNICATION_BOTH;
            if (sink && flow == (capture ? KSPIN_DATAFLOW_OUT : KSPIN_DATAFLOW_IN)) return (int) id;
        }
        return -1;
    }

    bool createPin (int pinId, int sampleRate, int numCh, bool useFloat, int containerBits, int validBits)
    {
        struct
        {
            KSPIN_CONNECT connect;
            KSDATAFORMAT_WAVEFORMATEXTENSIBLE format;
        } req {};
        req.connect.Interface.Set = KSINTERFACESETID_Standard;
        req.connect.Interface.Id = KSINTERFACE_STANDARD_LOOPED_STREAMING;
        req.connect.Medium.Set = KSMEDIUMSETID_Standard;
        req.connect.Medium.Id = KSMEDIUM_TYPE_ANYINSTANCE;
        req.connect.PinId = (ULONG) pinId;
        req.connect.PinToHandle = nullptr;
        req.connect.Priority.PriorityClass = KSPRIORITY_NORMAL;
        req.connect.Priority.PrioritySubClass = 1;

        auto& df = req.format.DataFormat;
        df.FormatSize = sizeof (KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
        df.SampleSize = (ULONG) (numCh * containerBits / 8);
        df.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
        df.SubFormat = useFloat ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
        df.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;

        auto& wf = req.format.WaveFormatExt;
        wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wf.Format.nChannels = (WORD) numCh;
        wf.Format.nSamplesPerSec = (DWORD) sampleRate;
        wf.Format.wBitsPerSample = (WORD) containerBits;
        wf.Format.nBlockAlign = (WORD) (numCh * containerBits / 8);
        wf.Format.nAvgBytesPerSec = wf.Format.nBlockAlign * (DWORD) sampleRate;
        wf.Format.cbSize = sizeof (WAVEFORMATEXTENSIBLE) - sizeof (WAVEFORMATEX);
        wf.Samples.wValidBitsPerSample = (WORD) validBits;
        wf.dwChannelMask = numCh == 1 ? SPEAKER_FRONT_CENTER : KSAUDIO_SPEAKER_STEREO;
        wf.SubFormat = df.SubFormat;

        HANDLE h = nullptr;
        if (KsCreatePin (filter, &req.connect, GENERIC_READ | GENERIC_WRITE, &h) != ERROR_SUCCESS || h == nullptr) return false;
        pin = h;
        return true;
    }

    bool allocateBuffer (int frames)
    {
        event = CreateEventW (nullptr, FALSE, FALSE, nullptr);
        KSRTAUDIO_BUFFER_PROPERTY_WITH_NOTIFICATION req {};
        req.BaseAddress = nullptr;
        req.RequestedBufferSize = (ULONG) (frames * blockAlign);
        req.NotificationCount = 2;
        KSRTAUDIO_BUFFER result {};
        notifications = ksProperty (pin, KSPROPSETID_RtAudio, KSPROPERTY_RTAUDIO_BUFFER_WITH_NOTIFICATION, KSPROPERTY_TYPE_GET,
                                    &req, sizeof (req), &result, sizeof (result));
        if (notifications)
        {
            KSRTAUDIO_NOTIFICATION_EVENT_PROPERTY ev {};
            ev.NotificationEvent = event;
            notifications = ksProperty (pin, KSPROPSETID_RtAudio, KSPROPERTY_RTAUDIO_REGISTER_NOTIFICATION_EVENT, KSPROPERTY_TYPE_GET,
                                        &ev, sizeof (ev), nullptr, 0);
        }
        else
        {
            KSRTAUDIO_BUFFER_PROPERTY plain {};
            plain.BaseAddress = nullptr;
            plain.RequestedBufferSize = (ULONG) (frames * blockAlign);
            if (! ksProperty (pin, KSPROPSETID_RtAudio, KSPROPERTY_RTAUDIO_BUFFER, KSPROPERTY_TYPE_GET, &plain, sizeof (plain), &result, sizeof (result)))
                return false;
        }
        buffer = static_cast<BYTE*> (result.BufferAddress);
        bufferBytes = result.ActualBufferSize - result.ActualBufferSize % (ULONG) blockAlign;
        if (buffer == nullptr || bufferBytes == 0) return false;

        // A position register (mapped into Hydra) saves one driver call per wake-up.
        KSRTAUDIO_HWREGISTER_PROPERTY rp {};
        rp.BaseAddress = nullptr;
        KSRTAUDIO_HWREGISTER reg {};
        if (ksProperty (pin, KSPROPSETID_RtAudio, KSPROPERTY_RTAUDIO_POSITIONREGISTER, KSPROPERTY_TYPE_GET, &rp, sizeof (rp), &reg, sizeof (reg))
            && reg.Register != nullptr && reg.Width == 32)
            posRegister = static_cast<volatile ULONG*> (reg.Register);
        return true;
    }

    bool setState (KSSTATE s)
    {
        KSPROPERTY p {};
        return ksProperty (pin, KSPROPSETID_Connection, KSPROPERTY_CONNECTION_STATE, KSPROPERTY_TYPE_SET, &p, sizeof (p), &s, sizeof (s));
    }

    float sample (const BYTE* frame, int c) const noexcept
    {
        if (isFloat) return reinterpret_cast<const float*> (frame)[c];
        if (bits == 32) return (float) (reinterpret_cast<const int32_t*> (frame)[c] / 2147483648.0);
        return reinterpret_cast<const int16_t*> (frame)[c] / 32768.0f;
    }

    void setSample (BYTE* frame, int c, float v) const noexcept
    {
        v = juce::jlimit (-1.0f, 1.0f, v);
        if (isFloat) reinterpret_cast<float*> (frame)[c] = v;
        else if (bits == 32) reinterpret_cast<int32_t*> (frame)[c] = (int32_t) juce::jlimit (-2147483648.0, 2147483647.0, (double) v * 2147483648.0);
        else reinterpret_cast<int16_t*> (frame)[c] = (int16_t) juce::jlimit (-32768.0f, 32767.0f, v * 32768.0f);
    }

    HANDLE filter = INVALID_HANDLE_VALUE, pin = INVALID_HANDLE_VALUE, event = nullptr;
    BYTE* buffer = nullptr;
    volatile ULONG* posRegister = nullptr;
    bool notifications = false, isFloat = true;
    int bits = 32;
};

//==============================================================================
/** What both directions share: endpoint lookup, the device thread and the clock steering. */
struct VacCore
{
    KsPin ks;
    int cable = 0, periodFrames = 144, periodMs = 3;
    bool steering = false;
    double baseClock = 1.0, applied = 1.0;
    juce::String desc;

    bool open (const IoTarget& t, bool capture, double engineRate, juce::String& error, bool& direct)
    {
        if (! directEnabled.load()) { error = "direct streaming is off"; return false; }
        const vac::Endpoint* ep = nullptr;
        const auto eps = vac::endpoints();
        for (auto& e : eps) if (e.friendly == t.deviceName && e.capture == capture && e.filterPath.isNotEmpty()) ep = &e;
        if (ep == nullptr) { error = "not a Virtual Audio Cable endpoint"; return false; }
        cable = ep->cable;

        // The cable's timing period is the granularity it moves audio in.
        int ms = 3;
        auto cables = vac::cables();
        vac::FormatRange range;
        if ((size_t) cable <= cables.size()) { ms = juce::jmax (1, cables[(size_t) cable - 1].msPerInt); range = cables[(size_t) cable - 1].range; }

        // At the engine rate the cable needs no resampling; otherwise 48 kHz (or the nearest it allows).
        int rate = (int) engineRate;
        const bool rateOk = range.maxRate == 0 || (rate >= range.minRate && rate <= range.maxRate);
        if (! rateOk) rate = juce::jlimit (range.minRate, range.maxRate, 48000);
        periodFrames = juce::jmax (32, rate * ms / 1000);
        periodMs = ms;

        // Audio quality: the cable's format range decides what a new stream may use. Hydra's own cables are
        // opened up to 32 bit, so its stream runs in 32-bit float instead of 16 bit (applies to new streams only).
        if (range.maxBits > 0 && range.maxBits < 32)
        {
            auto wider = range;
            wider.maxBits = 32;
            if (vac::setFormatRange (cable, wider)) range = wider;
        }
        if (! ks.open (ep->filterPath, capture, rate, 2, periodFrames, error)) return false;

        direct = rate == (int) engineRate && claimSteering (cable);
        steering = direct;
        if (steering) baseClock = applied = vac::getClockCorrection (cable);
        desc = "Kernel Streaming, cable " + juce::String (cable) + ", " + ks.formatName()
             + (steering ? ", clock synced to the master" : ", resampled");
        return true;
    }

    void close()
    {
        ks.close();
        if (steering)
        {
            vac::setClockCorrection (cable, baseClock);
            releaseSteering (cable);
            steering = false;
        }
    }

    /** Message thread, 10 Hz: hands the drift controller's correction to the cable clock. */
    void steer (double ratio)
    {
        if (! steering) return;
        const double want = baseClock * juce::jlimit (0.995, 1.005, ratio);
        if (std::abs (want - applied) < 2.0e-8) return;   // 0.02 ppm
        if (vac::setClockCorrection (cable, want)) applied = want;
    }
};

//==============================================================================
/** One thread moves the audio of all of Hydra's cable streams (instead of one thread per stream that woke
    every 1-2 ms with the system timer turned up). It wakes once per cable period on a high-resolution
    waitable timer (no timeBeginPeriod), runs with MMCSS "Pro Audio" priority and sleeps while no stream runs.
    Streams are serviced under a mutex; removing a stream waits until its last service call has returned. */
class VacPump
{
public:
    struct Client
    {
        virtual ~Client() = default;
        virtual void service() noexcept = 0;   // move what the cable has / needs now
        virtual int periodMs() const noexcept = 0;
    };

    static VacPump& get() { static VacPump pump; return pump; }

    void add (Client* c)
    {
        {
            std::lock_guard<std::mutex> l (lock);
            clients.push_back (c);
            if (! worker.joinable()) worker = std::thread ([this] { run(); });
        }
        SetEvent (wake);
    }

    void remove (Client* c)
    {
        std::lock_guard<std::mutex> l (lock);   // waits for a running service pass
        clients.erase (std::remove (clients.begin(), clients.end(), c), clients.end());
    }

private:
    VacPump() : wake (CreateEventW (nullptr, FALSE, FALSE, nullptr)) {}
    ~VacPump()
    {
        stopping = true;
        SetEvent (wake);
        if (worker.joinable()) worker.join();
        CloseHandle (wake);
    }

    void run()
    {
        DWORD task = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW (L"Pro Audio", &task);
        // High-resolution timer (Windows 10 1803+): exact 1-3 ms waits without raising the system timer rate.
        HANDLE timer = CreateWaitableTimerExW (nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
        const bool coarse = timer == nullptr;
        if (coarse) timeBeginPeriod (1);

        while (! stopping)
        {
            int period = 0;
            {
                std::lock_guard<std::mutex> l (lock);
                for (auto* c : clients) period = period == 0 ? c->periodMs() : juce::jmin (period, c->periodMs());
            }
            if (period == 0) { WaitForSingleObject (wake, INFINITE); continue; }   // no stream: sleep

            if (! coarse)
            {
                LARGE_INTEGER due;
                due.QuadPart = -(LONGLONG) juce::jlimit (1, 50, period) * 10000;   // relative, 100 ns units
                SetWaitableTimer (timer, &due, 0, nullptr, nullptr, FALSE);
                HANDLE both[2] = { timer, wake };
                WaitForMultipleObjects (2, both, FALSE, INFINITE);
            }
            else
                WaitForSingleObject (wake, (DWORD) juce::jlimit (1, 50, period));

            std::lock_guard<std::mutex> l (lock);
            for (auto* c : clients) c->service();
        }
        if (coarse) timeEndPeriod (1);
        if (timer != nullptr) CloseHandle (timer);
        if (mmcss) AvRevertMmThreadCharacteristics (mmcss);
    }

    std::mutex lock;
    std::vector<Client*> clients;
    std::thread worker;
    HANDLE wake;
    std::atomic<bool> stopping { false };
};

//==============================================================================
class VacInput final : public AsyncInput, private VacPump::Client
{
public:
    ~VacInput() override { stop(); core.close(); }

    bool open (const IoTarget& t, double engRate, int engBlock, juce::String& err)
    {
        bool isDirect = false;
        if (! core.open (t, true, engRate, err, isDirect)) return false;
        direct = isDirect;
        preferStable = true;   // virtual cables: stability before latency
        setup (core.ks.rate, core.periodFrames, 2, engRate, engBlock);
        deviceLatencyFrames = core.periodFrames;   // data arrives in cable periods
        transport = core.desc;
        capacity = (int) (core.ks.bufferBytes / (ULONG) core.ks.blockAlign);
        for (auto& v : tmp) v.assign ((size_t) capacity, 0.0f);
        return true;
    }

    void start() override
    {
        if (running) return;
        if (! core.ks.run()) { failed = true; return; }
        core.ks.position (last);
        lastAdvance = juce::Time::getMillisecondCounter();
        running = true;
        VacPump::get().add (this);
    }
    void stop() override
    {
        if (! running) return;
        VacPump::get().remove (this);
        running = false;
    }
    void poll() override
    {
        core.steer (clockRatio.load (std::memory_order_relaxed));
        const auto t = lastAdvance.load();
        if (running && t != 0 && juce::Time::getMillisecondCounter() - t > 2000) failed = true;   // cable stopped
    }

private:
    int periodMs() const noexcept override { return core.periodMs; }

    void service() noexcept override
    {
        ULONG pos = 0;
        if (! core.ks.position (pos)) return;
        const ULONG size = core.ks.bufferBytes;
        const int frames = (int) (((pos + size - last) % size) / (ULONG) core.ks.blockAlign);
        if (frames <= 0) return;
        // Nearly a whole buffer since the last pass: the cable may have overwritten unread audio.
        if (frames >= capacity - core.periodFrames) underruns.fetch_add (1, std::memory_order_relaxed);
        float* planar[2] = { tmp[0].data(), tmp[1].data() };
        core.ks.read (last, frames, planar);
        last = (last + (ULONG) frames * (ULONG) core.ks.blockAlign) % size;
        produce (planar, 2, frames);
        lastAdvance = juce::Time::getMillisecondCounter();
    }

    VacCore core;
    std::vector<float> tmp[2];
    int capacity = 0;
    ULONG last = 0;
    bool running = false;
    std::atomic<juce::uint32> lastAdvance { 0 };
};

class VacOutput final : public AsyncOutput, private VacPump::Client
{
public:
    ~VacOutput() override { stop(); core.close(); }

    bool open (const IoTarget& t, double engRate, int engBlock, juce::String& err)
    {
        bool isDirect = false;
        if (! core.open (t, false, engRate, err, isDirect)) return false;
        direct = isDirect;
        // Hydra keeps this much written ahead of the cable's play position: two cable periods (VAC takes data in
        // periods, and the shared cable thread wakes once per period) plus 2 ms for its wake-up jitter.
        lead = core.periodFrames * 2 + core.ks.rate / 1000 * 2;
        preferStable = true;
        setup (core.ks.rate, core.periodFrames, 2, engRate, engBlock);
        deviceLatencyFrames = lead;
        transport = core.desc;
        capacity = (int) (core.ks.bufferBytes / (ULONG) core.ks.blockAlign);
        lead = juce::jmin (lead, capacity / 2);
        for (auto& v : tmp) v.assign ((size_t) capacity, 0.0f);
        return true;
    }

    void start() override
    {
        if (running) return;
        core.ks.clear();
        if (! core.ks.run()) { failed = true; return; }
        ULONG pos = 0;
        core.ks.position (pos);
        lastPos = pos;
        origin = (int64_t) (pos / (ULONG) core.ks.blockAlign);   // frame index in the buffer where counting starts
        played = written = 0;
        lastAdvance = juce::Time::getMillisecondCounter();
        running = true;
        VacPump::get().add (this);
    }
    void stop() override
    {
        if (! running) return;
        VacPump::get().remove (this);
        running = false;
    }
    void poll() override
    {
        core.steer (clockRatio.load (std::memory_order_relaxed));
        const auto t = lastAdvance.load();
        if (running && t != 0 && juce::Time::getMillisecondCounter() - t > 2000) failed = true;
    }

private:
    int periodMs() const noexcept override { return core.periodMs; }

    void service() noexcept override
    {
        ULONG pos = 0;
        if (! core.ks.position (pos)) return;
        const ULONG size = core.ks.bufferBytes;
        const ULONG align = (ULONG) core.ks.blockAlign;
        const ULONG moved = (pos + size - lastPos) % size;
        if (moved > 0) lastAdvance = juce::Time::getMillisecondCounter();
        played += moved / align;
        lastPos = pos;
        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        const double gap = lastService > 0 ? nowMs - lastService : 0.0;
        lastService = nowMs;
        if (gap > maxGapMs.load()) maxGapMs = gap;

        if (written < played)
        {
            // The cable played past what Hydra wrote (it played old data or silence): a dropout, start again here.
            if (written > 0)
            {
                underruns.fetch_add (1, std::memory_order_relaxed);
                lateWrites.fetch_add (1, std::memory_order_relaxed);
                lateGapMs = gap;
                lateJumpMs = (double) (moved / align) * 1000.0 / core.ks.rate;
                // Stability before latency: write one cable period further ahead from now on (up to half the buffer).
                lead = juce::jmin (lead + core.periodFrames, capacity / 2);
                deviceLatencyFrames = lead;
            }
            written = played;
        }
        const int need = (int) juce::jmin<int64_t> (capacity - 16, played + lead - written);
        if (need <= 0) return;
        float* planar[2] = { tmp[0].data(), tmp[1].data() };
        consume (planar, 2, need);
        core.ks.write ((ULONG) (((origin + written) % capacity) * align), need, planar);
        written += need;
    }

    VacCore core;
    std::vector<float> tmp[2];
    int capacity = 0, lead = 256;
    ULONG lastPos = 0;
    int64_t origin = 0, played = 0, written = 0;
    double lastService = 0;
    bool running = false;
    std::atomic<juce::uint32> lastAdvance { 0 };
};
} // namespace

//==============================================================================
void setVacDirectEnabled (bool on) noexcept { directEnabled = on; }
bool isVacDirectEnabled() noexcept          { return directEnabled.load(); }
int vacLateWriteCount() noexcept            { return lateWrites.load(); }
juce::String vacLateWriteInfo()              { return "last late write: pump gap " + juce::String (lateGapMs.load(), 2) + " ms, cable moved " + juce::String (lateJumpMs.load(), 2) + " ms; max pump gap " + juce::String (maxGapMs.load(), 2) + " ms"; }

std::shared_ptr<AsyncInput> openVacInput (const IoTarget& t, double engineRate, int engineBlock, juce::String& error)
{
    if (! vac::isCableEndpoint (t.deviceName)) { error = "not a Virtual Audio Cable endpoint"; return nullptr; }
    auto s = std::make_shared<VacInput>();
    if (! s->open (t, engineRate, engineBlock, error)) return nullptr;
    s->target = t;
    return s;
}

std::shared_ptr<AsyncOutput> openVacOutput (const IoTarget& t, double engineRate, int engineBlock, juce::String& error)
{
    if (! vac::isCableEndpoint (t.deviceName)) { error = "not a Virtual Audio Cable endpoint"; return nullptr; }
    auto s = std::make_shared<VacOutput>();
    if (! s->open (t, engineRate, engineBlock, error)) return nullptr;
    s->target = t;
    return s;
}
} // namespace hydra

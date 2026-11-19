#include "PhoneCamera.h"
#include "camera/CameraShared.h"
#include "camera/VideoCodec.h"
#include "Common.h"

#include <shellapi.h>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace hydra::cam
{
namespace
{
// Packet layout: see the comment at the top of engine/NetLink.cpp (Video / VideoInfo / VideoCtl).
constexpr int kVideoHeader = 24;
constexpr int kVideoPayload = 1136;          // = kMaxPacket - header - video header - tag (as src/linkcore/LinkLite.cpp)
constexpr int kMaxFrameBytes = 12 << 20;        // a 4K key frame at a high bit rate fits
constexpr int kQueueFrames = 3;
constexpr double kOfferTimeoutMs = 3000.0;

double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }

struct In
{
    const uint8_t* p;
    int n, pos = 0;
    bool ok = true;
    uint32_t u8()  { if (pos >= n) { ok = false; return 0; } return p[pos++]; }
    uint32_t u16() { const uint32_t a = u8(); return a | (u8() << 8); }
    uint32_t u32() { const uint32_t a = u16(); return a | (u16() << 16); }
    float f32() { const uint32_t v = u32(); float f; std::memcpy (&f, &v, 4); return std::isfinite (f) ? f : 0.0f; }
    juce::String str()
    {
        const int len = (int) u8();
        if (! ok || pos + len > n) { ok = false; return {}; }
        auto s = juce::String::fromUTF8 ((const char*) p + pos, len);
        pos += len;
        return s;
    }
};

/** The control list of CamCaps (all parts put together); false if it does not parse. */
bool parseControls (const uint8_t* p, int n, std::vector<Control>& out)
{
    In b { p, n };
    const int count = (int) b.u8();
    std::vector<Control> list;
    for (int i = 0; i < count && b.ok; ++i)
    {
        Control c;
        c.id = (int) b.u8();
        c.kind = (int) b.u8();
        c.flags = (int) b.u8();
        c.group = (int) b.u8();
        c.label = b.str();
        c.min = b.f32();
        c.max = b.f32();
        c.step = b.f32();
        c.value = b.f32();
        c.unit = b.str();
        const int options = (int) b.u8();
        for (int k = 0; k < options && b.ok; ++k) c.options.add (b.str());
        if (c.kind > Control::point) continue;   // a newer phone: not drawn here
        if (c.max < c.min) std::swap (c.min, c.max);
        list.push_back (std::move (c));
    }
    if (! b.ok) return false;
    out = std::move (list);
    return true;
}

struct Frame
{
    std::vector<uint8_t> data;
    int width = 0, height = 0, rotation = 0;
    bool key = false;
    uint32_t time = 0;
};

struct Offer
{
    juce::String name, stream;
    uint32_t id = 0;
    int width = 0, height = 0, fps = 0;
    bool sending = false;
    double lastInfo = 0.0;
};

/** The shared memory of the camera source (CameraShared.h), opened when the source has created it. */
struct Shared
{
    HANDLE mapping = nullptr, event = nullptr;
    void* view = nullptr;

    bool open (bool test)
    {
        if (view != nullptr) return true;
        mapping = OpenFileMappingW (FILE_MAP_ALL_ACCESS, FALSE, camshare::objectName (camshare::kMappingName, test).c_str());
        if (mapping != nullptr)
            event = OpenEventW (EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, camshare::objectName (camshare::kEventName, test).c_str());
        if (mapping == nullptr) return false;
        view = MapViewOfFile (mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (view == nullptr || header()->magic != camshare::kMagic || header()->version != camshare::kVersion) { close(); return false; }
        return true;
    }

    void close()
    {
        if (view != nullptr) { UnmapViewOfFile (view); view = nullptr; }
        if (mapping != nullptr) { CloseHandle (mapping); mapping = nullptr; }
        if (event != nullptr) { CloseHandle (event); event = nullptr; }
    }

    camshare::Header* header() const noexcept { return (camshare::Header*) view; }
};
} // namespace

//==============================================================================
struct PhoneCamera::Impl
{
    PhoneCamera& owner;
    explicit Impl (PhoneCamera& o) : owner (o) {}

    net::Hub* hub = nullptr;
    mutable std::mutex lock;   // network thread, camera thread, UI
    std::condition_variable wakeCamera;
    bool enabled = false, preview = false, threadRunning = false;
    juce::String wantPhone;
    std::map<uint64_t, Offer> offers;
    uint64_t source = 0;       // device whose camera is used
    uint32_t sourceStream = 0;

    // Reassembly (network thread)
    bool curActive = false, curKey = false, haveDone = false;
    uint32_t curId = 0, curTime = 0, lastDone = 0;   // lastDone: the newest frame finished or given up
    int curParts = 0, curGot = 0, curLastLen = 0, curW = 0, curH = 0, curRot = 0;
    std::vector<uint8_t> curBuf, curHave;
    bool needKey = true, keyRequest = false;
    std::deque<Frame> queue;

    // Statistics
    uint32_t framesIn = 0, framesLost = 0, fbFrames = 0, fbLost = 0;
    uint64_t bytesIn = 0;
    double lastFrameAt = -1.0e9;
    Status status;
    juce::Image previewImage;
    uint32_t previewVersion = 0;
    int previewUsers = 0, previewWidths[2] = { 320, 320 };

    bool demo = false;
    Status demoStatus;

    // Camera controls (CamCaps / CamSet, engine/NetLink.cpp)
    struct CapsParts { uint16_t revision = 0; int parts = 0, got = 0; std::vector<std::vector<uint8_t>> data; };
    CapsParts capsIn;
    std::vector<Control> controls;
    uint32_t controlsVersion = 0;
    struct Pending { uint32_t revision = 0; std::vector<float> values; };
    std::map<int, Pending> pending;        // changed here, not yet confirmed by the phone
    uint32_t revision = 0;                 // of the newest change
    std::deque<uint32_t> sentRevisions;    // what went out (the phone confirms one of them)
    bool setDue = false;
    float pressCount = 0;                  // buttons / points count their presses (the phone acts on a new count)

    std::thread thread;
    std::atomic<bool> quit { false };

    //--------------------------------------------------------------------------
    void resetReassembly()
    {
        curActive = false;
        haveDone = false;
        needKey = true;
        queue.clear();
    }

    void onInfo (uint64_t device, const juce::String& sender, const uint8_t* body, int bytes, double now)
    {
        In b { body, bytes };
        Offer o;
        o.id = b.u32();
        o.width = (int) b.u16();
        o.height = (int) b.u16();
        o.fps = (int) b.u8();
        o.sending = b.u8() != 0;
        o.stream = b.str();
        b.str();   // the sender's name as it sees itself (we use the announced one)
        if (! b.ok) return;
        o.name = sender;
        o.lastInfo = now;
        offers[device] = o;
    }

    void clearControls()
    {
        capsIn = {};
        controls.clear();
        pending.clear();
        sentRevisions.clear();
        ++controlsVersion;
    }

    void onCaps (uint64_t device, const uint8_t* body, int bytes)
    {
        if (device != source) return;
        In b { body, bytes };
        b.u32();   // stream id (the phone's camera, whichever stream)
        const uint32_t applied = b.u32();
        const auto rev = (uint16_t) b.u16();
        const int part = (int) b.u8(), parts = (int) b.u8();
        if (! b.ok || parts <= 0 || part >= parts) return;

        // The phone took this revision: what was set up to it is confirmed (only revisions sent by this Hydra count).
        if (std::find (sentRevisions.begin(), sentRevisions.end(), applied) != sentRevisions.end())
        {
            for (auto it = pending.begin(); it != pending.end();)
                it = it->second.revision <= applied ? pending.erase (it) : std::next (it);
            ++controlsVersion;
        }

        if (capsIn.revision != rev || capsIn.parts != parts) { capsIn = {}; capsIn.revision = rev; capsIn.parts = parts; capsIn.data.resize ((size_t) parts); }
        auto& d = capsIn.data[(size_t) part];
        const bool fresh = d.empty();
        d.assign (body + b.pos, body + bytes);
        if (d.empty()) d.push_back (0);   // an empty list: a single "count 0"
        if (fresh) ++capsIn.got;
        if (capsIn.got < capsIn.parts) return;
        std::vector<uint8_t> all;
        for (auto& x : capsIn.data) all.insert (all.end(), x.begin(), x.end());
        std::vector<Control> list;
        if (! parseControls (all.data(), (int) all.size(), list)) return;
        controls = std::move (list);
        ++controlsVersion;
        // The next update (2 s later, or after a change on the phone) starts a new collection.
        capsIn.got = 0;
        for (auto& x : capsIn.data) x.clear();
    }

    void change (int id, std::vector<float> values)
    {
        auto& p = pending[id];
        p.revision = ++revision;
        p.values = std::move (values);
        setDue = true;
        ++controlsVersion;
        wakeCamera.notify_one();
    }

    /** The CamSet body with everything not confirmed yet ("" when nothing waits). */
    std::vector<uint8_t> setBody (uint32_t stream)
    {
        std::vector<uint8_t> out;
        if (pending.empty()) return out;
        auto u8 = [&] (uint32_t v) { out.push_back ((uint8_t) v); };
        auto u32 = [&] (uint32_t v) { for (int i = 0; i < 4; ++i) u8 (v >> (8 * i)); };
        u32 (stream);
        u32 (revision);
        const size_t countAt = out.size();
        u8 (0);
        int count = 0;
        for (auto& [id, p] : pending)
        {
            if (out.size() + 2 + 4 * p.values.size() > 1100) break;
            u8 ((uint32_t) id);
            u8 ((uint32_t) p.values.size());
            for (float f : p.values) { uint32_t v; std::memcpy (&v, &f, 4); u32 (v); }
            ++count;
        }
        out[countAt] = (uint8_t) count;
        sentRevisions.push_back (revision);
        while (sentRevisions.size() > 64) sentRevisions.pop_front();
        return out;
    }

    void onVideo (uint64_t device, const uint8_t* body, int bytes, double now)
    {
        if (! enabled || device != source || bytes < kVideoHeader) return;
        In b { body, bytes };
        const uint32_t stream = b.u32(), id = b.u32();
        const int part = (int) b.u16(), parts = (int) b.u16();
        const uint32_t flags = b.u8();
        const uint32_t codec = b.u8();
        const int w = (int) b.u16(), h = (int) b.u16(), rot = (int) (b.u8() & 3);
        b.u8();
        const uint32_t time = b.u32();
        const uint8_t* payload = body + kVideoHeader;
        const int len = bytes - kVideoHeader;
        if (! b.ok || codec != 1 || parts <= 0 || part >= parts || len <= 0 || len > kVideoPayload) return;
        if ((size_t) parts * kVideoPayload > (size_t) kMaxFrameBytes || w <= 0 || h <= 0 || w > 4096 || h > 4096) return;
        bytesIn += (uint64_t) bytes;
        lastFrameAt = now;

        if (stream != sourceStream) { sourceStream = stream; resetReassembly(); }
        if (curActive && id != curId)
        {
            if ((int32_t) (id - curId) < 0) return;   // a late part of a frame given up already
            ++framesLost;                              // the frame being assembled misses parts
            needKey = true;
            keyRequest = true;
            curActive = false;
            haveDone = true;
            lastDone = curId;
        }
        if (! curActive)
        {
            if (haveDone && (int32_t) (id - lastDone) <= 0) return;   // finished or given up already
            if (haveDone && id != lastDone + 1)
            {
                // Whole frames missing.
                framesLost += juce::jmin<uint32_t> (id - lastDone - 1, 1000);
                needKey = true;
                keyRequest = true;
            }
            curActive = true;
            curId = id;
            curParts = parts;
            curGot = 0;
            curLastLen = 0;
            curHave.assign ((size_t) parts, 0);
            if (curBuf.size() < (size_t) parts * kVideoPayload) curBuf.resize ((size_t) parts * kVideoPayload);
            curKey = (flags & 1) != 0;
            curW = w;
            curH = h;
            curRot = rot;
            curTime = time;
        }
        if (parts != curParts || curHave[(size_t) part] != 0) return;
        if (part < parts - 1 && len != kVideoPayload) { curActive = false; return; }   // not our packet layout
        std::memcpy (curBuf.data() + (size_t) part * kVideoPayload, payload, (size_t) len);
        curHave[(size_t) part] = 1;
        if (part == parts - 1) curLastLen = len;
        if (++curGot < curParts) return;

        curActive = false;
        haveDone = true;
        lastDone = curId;
        ++framesIn;
        if (needKey && ! curKey) { keyRequest = true; wakeCamera.notify_one(); return; }
        needKey = false;
        Frame f;
        f.data.assign (curBuf.begin(), curBuf.begin() + (ptrdiff_t) ((size_t) (curParts - 1) * kVideoPayload + (size_t) curLastLen));
        f.key = curKey;
        f.width = curW;
        f.height = curH;
        f.rotation = curRot;
        f.time = curTime;
        queue.push_back (std::move (f));
        if ((int) queue.size() > kQueueFrames)
        {
            // The decoder cannot keep up: start again at a key frame.
            queue.clear();
            needKey = true;
            keyRequest = true;
        }
        wakeCamera.notify_one();
    }

    /** Which phone's camera: the one named in the settings, else the most recent offer. */
    void chooseSource (double now)
    {
        uint64_t best = 0;
        double bestAt = -1.0e18;
        for (auto& [dev, o] : offers)
        {
            if (now - o.lastInfo > kOfferTimeoutMs) continue;
            if (wantPhone.isNotEmpty() && ! o.name.equalsIgnoreCase (wantPhone)) continue;
            if (o.lastInfo > bestAt) { best = dev; bestAt = o.lastInfo; }
        }
        // Keep the current one while it is there (no flipping between two phones).
        if (source != 0 && best != source)
            if (auto it = offers.find (source); it != offers.end() && now - it->second.lastInfo <= kOfferTimeoutMs
                && (wantPhone.isEmpty() || it->second.name.equalsIgnoreCase (wantPhone)))
                best = source;
        if (best != source)
        {
            source = best;
            sourceStream = 0;
            resetReassembly();
            clearControls();
        }
    }

    //--------------------------------------------------------------------------
    void run()
    {
        comInit();
        H264Decoder decoder;
        bool decoderOpen = false;
        VirtualCamera vcam;
        Shared shared;
        std::vector<uint8_t> out;
        double nextCtl = 0.0, nextSet = 0.0, nextShared = 0.0, nextVcam = 0.0, lastKeyAsk = -1.0e9, lastPreview = -1.0e9, rateMark = 0.0;
        bool wasWanted = false, wasWantedDecoding = false;
        uint64_t sentTo = 0;
        int decoded = 0, lastW = 0, lastH = 0, lastRot = 0;
        uint64_t bytesMark = 0;

        for (;;)
        {
            Frame frame;
            bool haveFrame = false, en, prev, key;
            uint64_t dev;
            uint32_t streamId, rec, lost;
            juce::String err;
            std::vector<uint8_t> settings;
            int previewWidth = 320;
            {
                std::unique_lock<std::mutex> l (lock);
                wakeCamera.wait_for (l, std::chrono::milliseconds (50), [this] { return quit.load() || ! queue.empty() || keyRequest || setDue; });
                if (quit.load()) break;
                const double now = nowMs();
                chooseSource (now);
                en = enabled;
                prev = preview;
                dev = source;
                streamId = sourceStream;
                if (! queue.empty()) { frame = std::move (queue.front()); queue.pop_front(); haveFrame = true; }
                key = keyRequest && now - lastKeyAsk > 300.0;
                if (key) keyRequest = false;
                rec = framesIn - fbFrames;
                lost = framesLost - fbLost;
                previewWidth = juce::jmax (previewUsers & 1 ? previewWidths[0] : 0, previewUsers & 2 ? previewWidths[1] : 0, 64);
                // Camera settings: at once after a change, then every 0.3 s until the phone confirms them.
                if (dev != 0 && ! pending.empty() && (setDue || now >= nextSet))
                {
                    settings = setBody (streamId);
                    setDue = false;
                    nextSet = now + 300.0;
                }
            }
            if (hub != nullptr && ! settings.empty()) hub->sendVideoControl (dev, settings.data(), (int) settings.size(), true);
            const double now = nowMs();

            // "Hydra Camera" for the apps
            if (en && ! owner.noVirtualCamera.load() && ! vcam.isStarted() && now >= nextVcam)
            {
                nextVcam = now + 5000.0;
                if (setupState() == SetupState::ok)
                {
                    std::string e;
                    if (vcam.start (e)) logLine ("camera: Hydra Camera is on");
                    else { err = e; logLine ("camera: " + juce::String (e)); }
                }
            }
            if (! en && vcam.isStarted()) { vcam.stop(); logLine ("camera: Hydra Camera is off"); }

            if (en && shared.view == nullptr && now >= nextShared) { nextShared = now + 1000.0; shared.open (owner.testNames.load()); }
            if (! en && shared.view != nullptr) shared.close();

            // Who watches, in which size
            int outW = juce::jlimit (16, camshare::kMaxWidth, owner.testWidth.load() & ~1);
            int outH = juce::jlimit (16, camshare::kMaxHeight, owner.testHeight.load() & ~1);
            bool consumer = false;
            if (auto* hd = shared.header())
            {
                const int ww = (int) hd->wantWidth, wh = (int) hd->wantHeight;
                consumer = ww > 0 && wh > 0 && (int64_t) GetTickCount64() - (int64_t) hd->consumerTick < 2000;
                if (consumer) { outW = juce::jmin (ww, camshare::kMaxWidth); outH = juce::jmin (wh, camshare::kMaxHeight); }
            }
            const bool wanted = en && dev != 0 && (consumer || prev || owner.forceWanted.load());

            // Tell the phone (once a second, at once on a change or a key frame request)
            if (hub != nullptr && dev != 0 && (now >= nextCtl || key || wanted != wasWanted || dev != sentTo))
            {
                uint8_t body[16];
                auto put32 = [&] (int at, uint32_t v) { body[at] = (uint8_t) v; body[at + 1] = (uint8_t) (v >> 8); body[at + 2] = (uint8_t) (v >> 16); body[at + 3] = (uint8_t) (v >> 24); };
                put32 (0, streamId);
                body[4] = wanted ? 1 : 0;
                body[5] = key ? 1 : 0;
                body[6] = (uint8_t) outH;
                body[7] = (uint8_t) (outH >> 8);
                put32 (8, rec);
                put32 (12, lost);
                hub->sendVideoControl (dev, body, 16);
                if (key) lastKeyAsk = now;
                nextCtl = now + 1000.0;
                wasWanted = wanted;
                sentTo = dev;
                std::lock_guard<std::mutex> l (lock);
                fbFrames += rec;
                fbLost += lost;
            }

            if (haveFrame && wanted)
            {
                if (! decoderOpen)
                {
                    decoderOpen = decoder.open();
                    if (! decoderOpen) err = juce::String (decoder.error());
                }
                if (decoderOpen)
                {
                    out.resize ((size_t) outW * outH * 3 / 2);
                    const bool ok = decoder.decode (frame.data.data(), (int) frame.data.size(), (int64_t) frame.time * 10, [&] (const Picture& p)
                    {
                        const int w = juce::jmin (frame.width, p.width) & ~1, h = juce::jmin (frame.height, p.height) & ~1;
                        fitNv12 (p.y, p.uv, p.pitch, w, h, frame.rotation, out.data(), outW, outH);
                        ++decoded;
                        lastW = w;
                        lastH = h;
                        lastRot = frame.rotation;
                        if (shared.view != nullptr && consumer)
                        {
                            camshare::writeFrame (shared.view, out.data(), outW, outH);
                            if (shared.event != nullptr) SetEvent (shared.event);
                        }
                        if (owner.onFrame) owner.onFrame (out.data(), outW, outH);
                        if (prev && now - lastPreview >= 100.0)
                        {
                            lastPreview = now;
                            makePreview (out.data(), outW, outH, previewWidth);
                        }
                    });
                    if (! ok)
                    {
                        std::lock_guard<std::mutex> l (lock);
                        needKey = true;
                        keyRequest = true;
                    }
                }
            }
            if (! wanted && wasWantedDecoding)
            {
                // Nobody watches: the phone stops; the next picture starts at a key frame.
                if (decoderOpen) decoder.flush();
                std::lock_guard<std::mutex> l (lock);
                needKey = true;
                queue.clear();
            }
            wasWantedDecoding = wanted;

            // Status (twice a second)
            if (now - rateMark >= 500.0)
            {
                std::lock_guard<std::mutex> l (lock);
                const double secs = rateMark > 0.0 ? (now - rateMark) / 1000.0 : 1.0;
                auto& s = status;
                s.fps = decoded / secs;
                s.kbps = (double) (bytesIn - bytesMark) * 8.0 / 1000.0 / secs;
                decoded = 0;
                bytesMark = bytesIn;
                rateMark = now;
                s.enabled = en;
                s.cameraOn = vcam.isStarted();
                s.inUse = wanted;
                s.receiving = now - lastFrameAt < 1000.0;
                s.outWidth = consumer || prev ? outW : 0;
                s.outHeight = consumer || prev ? outH : 0;
                s.lostFrames = framesLost;
                s.frameWidth = lastW;
                s.frameHeight = lastH;
                s.rotation = lastRot;
                s.phones.clear();
                s.phone.clear();
                s.width = s.height = 0;
                for (auto& [d, o] : offers)
                    if (now - o.lastInfo <= kOfferTimeoutMs)
                    {
                        s.phones.addIfNotAlreadyThere (o.name);
                        if (d == source) { s.phone = o.name; s.width = o.width; s.height = o.height; }
                    }
                if (err.isNotEmpty()) s.error = err;
                else if (s.receiving) s.error.clear();
            }
        }
        vcam.stop();
        shared.close();
        comShutdown();
    }

    void makePreview (const uint8_t* nv12, int w, int h, int width)
    {
        const int pw = juce::jlimit (64, w, width), ph = juce::jmax (2, pw * h / juce::jmax (1, w));
        juce::Image img (juce::Image::RGB, pw, ph, false);
        {
            juce::Image::BitmapData d (img, juce::Image::BitmapData::writeOnly);
            const uint8_t* uvp = nv12 + (size_t) w * h;
            for (int y = 0; y < ph; ++y)
            {
                const int sy = y * h / ph;
                auto* row = d.getLinePointer (y);
                for (int x = 0; x < pw; ++x)
                {
                    const int sx = x * w / pw;
                    const int Y = nv12[(size_t) sy * w + sx] - 16;
                    const uint8_t* c = uvp + (size_t) (sy / 2) * w + (sx & ~1);
                    const int U = c[0] - 128, V = c[1] - 128;
                    const int r = (298 * Y + 409 * V + 128) >> 8, g = (298 * Y - 100 * U - 208 * V + 128) >> 8, bl = (298 * Y + 516 * U + 128) >> 8;
                    auto* px = row + x * d.pixelStride;
                    px[0] = (uint8_t) juce::jlimit (0, 255, bl);   // JUCE's RGB images are BGR in memory on Windows
                    px[1] = (uint8_t) juce::jlimit (0, 255, g);
                    px[2] = (uint8_t) juce::jlimit (0, 255, r);
                }
            }
        }
        std::lock_guard<std::mutex> l (lock);
        previewImage = img;
        ++previewVersion;
    }

    void startThread()
    {
        if (threadRunning) return;
        quit = false;
        thread = std::thread ([this] { run(); });
        threadRunning = true;
    }

    void stopThread()
    {
        if (! threadRunning) return;
        {
            std::lock_guard<std::mutex> l (lock);
            quit = true;
        }
        wakeCamera.notify_all();
        thread.join();
        threadRunning = false;
    }
};

//==============================================================================
PhoneCamera::PhoneCamera() : impl (std::make_unique<Impl> (*this)) {}

PhoneCamera::~PhoneCamera()
{
    if (impl->hub != nullptr) impl->hub->setVideoHandler (nullptr);
    impl->stopThread();
}

PhoneCamera& PhoneCamera::get()
{
    static net::Hub& hub = net::Hub::get();   // constructed first: destroyed after the camera, which detaches from it
    static PhoneCamera camera;
    juce::ignoreUnused (hub);
    if (camera.impl->hub == nullptr) camera.attach (net::Hub::get());
    return camera;
}

void PhoneCamera::attach (net::Hub& h)
{
    if (impl->hub != nullptr) impl->hub->setVideoHandler (nullptr);
    impl->hub = &h;
    h.setVideoHandler (this);
}

void PhoneCamera::videoPacket (uint64_t device, const juce::String& sender, net::Hub::VideoPacket type, const uint8_t* body, int bytes, double now)
{
    std::lock_guard<std::mutex> l (impl->lock);
    switch (type)
    {
        case net::Hub::VideoPacket::info:  impl->onInfo (device, sender, body, bytes, now); break;
        case net::Hub::VideoPacket::caps:  impl->onCaps (device, body, bytes); break;
        case net::Hub::VideoPacket::frame: impl->onVideo (device, body, bytes, now); break;
    }
}

std::vector<Control> PhoneCamera::getControls (uint32_t& version) const
{
    std::lock_guard<std::mutex> l (impl->lock);
    version = impl->controlsVersion;
    auto list = impl->controls;
    for (auto& c : list)   // what was set here shows until the phone confirms it
        if (auto it = impl->pending.find (c.id); it != impl->pending.end() && ! it->second.values.empty()
            && (c.kind == Control::toggle || c.kind == Control::range || c.kind == Control::choice))
            c.value = it->second.values[0];
    return list;
}

void PhoneCamera::setControl (int id, float value)
{
    std::lock_guard<std::mutex> l (impl->lock);
    if (impl->source == 0) return;
    impl->change (id, { value });
}

void PhoneCamera::pressButton (int id)
{
    std::lock_guard<std::mutex> l (impl->lock);
    if (impl->source == 0) return;
    impl->change (id, { ++impl->pressCount });
}

void PhoneCamera::setPoint (int id, float x, float y)
{
    std::lock_guard<std::mutex> l (impl->lock);
    if (impl->source == 0) return;
    impl->change (id, { juce::jlimit (0.0f, 1.0f, x), juce::jlimit (0.0f, 1.0f, y), ++impl->pressCount });
}

juce::String Control::format (float v) const
{
    if (kind == choice) return options[juce::jlimit (0, juce::jmax (0, options.size() - 1), (int) std::lround (v))];
    if (kind == toggle) return v >= 0.5f ? "On" : "Off";
    if ((flags & seconds) != 0)
    {
        if (v <= 0.0f) return "-";
        if (v < 0.5f) return "1/" + juce::String (juce::roundToInt (1.0 / v)) + " s";
        return juce::String (v, 1) + " s";
    }
    if ((flags & diopters) != 0)
    {
        if (v <= 0.001f) return juce::String::fromUTF8 ("\xe2\x88\x9e");   // infinity
        const float m = 1.0f / v;
        return m < 1.0f ? juce::String (juce::roundToInt (m * 100.0f)) + " cm" : juce::String (m, m < 10.0f ? 1 : 0) + " m";
    }
    const int decimals = step <= 0.0f ? 2 : step >= 1.0f ? 0 : step >= 0.1f ? 1 : 2;
    juce::String t = juce::String (v, decimals);
    if (min < 0.0f && v > 0.0f) t = "+" + t;
    return unit.isEmpty() ? t : t + " " + unit;
}

void PhoneCamera::setEnabled (bool on, const juce::String& phone)
{
    {
        std::lock_guard<std::mutex> l (impl->lock);
        if (on == impl->enabled && phone == impl->wantPhone && (! on || impl->threadRunning)) return;
        impl->enabled = on;
        impl->wantPhone = phone.trim();
        impl->status.enabled = on;
    }
    // The thread stays once started: switching off tells the phone (want = 0) and removes the camera.
    if (on) impl->startThread();
    impl->wakeCamera.notify_all();
}

void PhoneCamera::setDemo (const Status& s, std::vector<Control> list, const juce::Image& preview)
{
    std::lock_guard<std::mutex> l (impl->lock);
    impl->demo = true;
    impl->demoStatus = s;
    impl->controls = std::move (list);
    ++impl->controlsVersion;
    impl->previewImage = preview;
    ++impl->previewVersion;
}

Status PhoneCamera::getStatus() const
{
    Status s;
    {
        std::lock_guard<std::mutex> l (impl->lock);
        if (impl->demo) return impl->demoStatus;
        s = impl->status;
        s.enabled = impl->enabled;
    }
    s.supported = virtualCameraSupported();
    const auto state = s.supported ? setupState() : SetupState::missing;
    s.setUp = state == SetupState::ok;
    s.setUpOld = state == SetupState::outdated;
    return s;
}

void PhoneCamera::setPreview (bool on, int user, int width)
{
    std::lock_guard<std::mutex> l (impl->lock);
    user = juce::jlimit (0, 1, user);
    impl->previewWidths[user] = juce::jlimit (64, 1920, width);
    impl->previewUsers = on ? (impl->previewUsers | (1 << user)) : (impl->previewUsers & ~(1 << user));
    impl->preview = impl->previewUsers != 0;
    if (! impl->preview && ! impl->demo) impl->previewImage = {};
}

juce::Image PhoneCamera::getPreview (uint32_t& version) const
{
    std::lock_guard<std::mutex> l (impl->lock);
    version = impl->previewVersion;
    return impl->previewImage;
}

bool PhoneCamera::requestSetup (bool install)
{
    const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName();
    const juce::String args = juce::String (kSetupArgument) + (install ? " install" : " remove");
    SHELLEXECUTEINFOW sei {};
    sei.cbSize = sizeof (sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.toWideCharPointer();
    sei.lpParameters = args.toWideCharPointer();
    sei.nShow = SW_HIDE;
    int code = -1;
    if (ShellExecuteExW (&sei) && sei.hProcess != nullptr)
    {
        if (WaitForSingleObject (sei.hProcess, 30000) == WAIT_OBJECT_0)
        {
            DWORD c = 0;
            if (GetExitCodeProcess (sei.hProcess, &c)) code = (int) c;
        }
        CloseHandle (sei.hProcess);
    }
    logLine (juce::String ("camera setup ") + (install ? "install" : "remove") + (code == 0 ? ": done" : code < 0 ? ": declined / failed" : ": error " + juce::String (code)));
    return code == 0;
}

int PhoneCamera::runSetupJob (const juce::String& commandLine)
{
    const bool install = ! commandLine.fromFirstOccurrenceOf (kSetupArgument, false, false).trim().startsWith ("remove");
    return setupMachine (install);
}

//==============================================================================
namespace
{
/** Upright (no turn), any scale: separable bilinear for luma (each source row scaled across once, then rows
    blended; 8-bit weights), nearest for chroma, both from index tables. ~10x faster than the general path. */
void fitUpright (const uint8_t* y, const uint8_t* uv, int pitch, int w, int h, uint8_t* oy, uint8_t* ouv, int W, int ox, int oyy, int fw, int fh)
{
    thread_local std::vector<int> xi, yi, cx, cy;
    thread_local std::vector<uint8_t> xa, ya;
    thread_local std::vector<uint16_t> rowA, rowB;
    xi.resize ((size_t) fw); xa.resize ((size_t) fw); yi.resize ((size_t) fh); ya.resize ((size_t) fh);
    const double kx = (double) w / fw, ky = (double) h / fh;
    for (int x = 0; x < fw; ++x)
    {
        const double sx = juce::jlimit (0.0, (double) (w - 1), (x + 0.5) * kx - 0.5);
        xi[(size_t) x] = juce::jmin ((int) sx, w - 2 < 0 ? 0 : w - 2);
        xa[(size_t) x] = (uint8_t) juce::jlimit (0, 255, (int) std::lround ((sx - xi[(size_t) x]) * 256.0));
    }
    for (int r = 0; r < fh; ++r)
    {
        const double sy = juce::jlimit (0.0, (double) (h - 1), (r + 0.5) * ky - 0.5);
        yi[(size_t) r] = juce::jmin ((int) sy, h - 2 < 0 ? 0 : h - 2);
        ya[(size_t) r] = (uint8_t) juce::jlimit (0, 255, (int) std::lround ((sy - yi[(size_t) r]) * 256.0));
    }
    rowA.resize ((size_t) fw); rowB.resize ((size_t) fw);
    int haveA = -1, haveB = -1;
    auto across = [&] (int sr, std::vector<uint16_t>& out)
    {
        const uint8_t* src = y + (size_t) sr * pitch;
        const int x1max = w - 1;
        for (int x = 0; x < fw; ++x)
        {
            const int i = xi[(size_t) x], a = xa[(size_t) x];
            out[(size_t) x] = (uint16_t) (src[i] * (256 - a) + src[juce::jmin (i + 1, x1max)] * a);   // value * 256
        }
    };
    for (int r = 0; r < fh; ++r)
    {
        const int r0 = yi[(size_t) r], r1 = juce::jmin (r0 + 1, h - 1);
        if (haveA != r0) { if (haveB == r0) { std::swap (rowA, rowB); std::swap (haveA, haveB); } else { across (r0, rowA); haveA = r0; } }
        if (haveB != r1) { across (r1, rowB); haveB = r1; }
        const uint32_t b = ya[(size_t) r], a = 256 - b;
        uint8_t* dst = oy + (size_t) (oyy + r) * W + ox;
        const uint16_t* A = rowA.data();
        const uint16_t* B = rowB.data();
        for (int x = 0; x < fw; ++x) dst[x] = (uint8_t) ((A[x] * a + B[x] * b + 32768u) >> 16);   // vectorises
    }
    // Chroma: nearest on the half-size grid.
    const int cw = w / 2, ch = h / 2, fcw = fw / 2, fch = fh / 2;
    cx.resize ((size_t) fcw); cy.resize ((size_t) fch);
    for (int x = 0; x < fcw; ++x) cx[(size_t) x] = juce::jlimit (0, cw - 1, (int) ((x + 0.5) * cw / fcw)) * 2;
    for (int r = 0; r < fch; ++r) cy[(size_t) r] = juce::jlimit (0, ch - 1, (int) ((r + 0.5) * ch / fch));
    for (int r = 0; r < fch; ++r)
    {
        const uint8_t* src = uv + (size_t) cy[(size_t) r] * pitch;
        auto* dst = (uint16_t*) (ouv + (size_t) (oyy / 2 + r) * W + ox);
        for (int x = 0; x < fcw; ++x) { uint16_t v; std::memcpy (&v, src + cx[(size_t) x], 2); std::memcpy (dst + x, &v, 2); }
    }
}
} // namespace

void fitNv12 (const uint8_t* y, const uint8_t* uv, int pitch, int w, int h, int rotation, uint8_t* out, int W, int H)
{
    rotation &= 3;
    uint8_t* oy = out;
    uint8_t* ouv = out + (size_t) W * H;
    if (rotation == 0 && w == W && h == H)
    {
        for (int r = 0; r < H; ++r) std::memcpy (oy + (size_t) r * W, y + (size_t) r * pitch, (size_t) W);
        for (int r = 0; r < H / 2; ++r) std::memcpy (ouv + (size_t) r * W, uv + (size_t) r * pitch, (size_t) W);
        return;
    }
    const int dw = (rotation & 1) ? h : w, dh = (rotation & 1) ? w : h;   // upright size
    const double s = std::min ((double) W / dw, (double) H / dh);
    const int fw = juce::jlimit (2, W, (int) std::lround (dw * s) & ~1), fh = juce::jlimit (2, H, (int) std::lround (dh * s) & ~1);
    const int ox = ((W - fw) / 2) & ~1, oyy = ((H - fh) / 2) & ~1;
    if (fw != W || fh != H)
    {
        std::memset (oy, 16, (size_t) W * H);
        std::memset (ouv, 128, (size_t) W * H / 2);
    }

    if (rotation == 0 && w >= 2 && h >= 2) { fitUpright (y, uv, pitch, w, h, oy, ouv, W, ox, oyy, fw, fh); return; }

    // Upright (u, v) -> source (x, y) of a w x h picture turned `rotation` quarter turns.
    auto toSource = [rotation] (double u, double v, int sw, int sh, double& sx, double& sy)
    {
        switch (rotation)
        {
            case 1:  sx = v;            sy = sh - 1 - u; break;
            case 2:  sx = sw - 1 - u;   sy = sh - 1 - v; break;
            case 3:  sx = sw - 1 - v;   sy = u;          break;
            default: sx = u;            sy = v;          break;
        }
    };

    // Luma: bilinear, 16.16 fixed point, one affine step per row.
    {
        const double kx = (double) dw / fw, ky = (double) dh / fh;
        for (int ty = 0; ty < fh; ++ty)
        {
            const double v = (ty + 0.5) * ky - 0.5;
            double x0, y0, x1, y1;
            toSource (0.5 * kx - 0.5, v, w, h, x0, y0);
            toSource (1.5 * kx - 0.5, v, w, h, x1, y1);
            int64_t fx = (int64_t) std::llround (x0 * 65536.0), fy = (int64_t) std::llround (y0 * 65536.0);
            const int64_t sx = (int64_t) std::llround ((x1 - x0) * 65536.0), sy = (int64_t) std::llround ((y1 - y0) * 65536.0);
            uint8_t* dst = oy + (size_t) (oyy + ty) * W + ox;
            const int64_t maxX = (int64_t) (w - 1) << 16, maxY = (int64_t) (h - 1) << 16;
            for (int tx = 0; tx < fw; ++tx, fx += sx, fy += sy)
            {
                const int64_t cx = fx < 0 ? 0 : fx > maxX ? maxX : fx, cy = fy < 0 ? 0 : fy > maxY ? maxY : fy;
                const int ix = (int) (cx >> 16), iy = (int) (cy >> 16);
                const int ax = (int) ((cx >> 8) & 255), ay = (int) ((cy >> 8) & 255);
                const int ix1 = ix + (ix < w - 1 ? 1 : 0);
                const uint8_t* r0 = y + (size_t) iy * pitch;
                const uint8_t* r1 = y + (size_t) (iy + (iy < h - 1 ? 1 : 0)) * pitch;
                const int top = r0[ix] * (256 - ax) + r0[ix1] * ax, bottom = r1[ix] * (256 - ax) + r1[ix1] * ax;
                dst[tx] = (uint8_t) ((top * (256 - ay) + bottom * ay + 32768) >> 16);
            }
        }
    }
    // Chroma: nearest, on the half-size grid.
    {
        const int cw = w / 2, ch = h / 2, fcw = fw / 2, fch = fh / 2;
        const int dcw = (rotation & 1) ? ch : cw, dch = (rotation & 1) ? cw : ch;
        const double kx = (double) dcw / fcw, ky = (double) dch / fch;
        for (int ty = 0; ty < fch; ++ty)
        {
            uint8_t* dst = ouv + (size_t) (oyy / 2 + ty) * W + ox;
            const double v = (ty + 0.5) * ky - 0.5;
            for (int tx = 0; tx < fcw; ++tx)
            {
                double sxd, syd;
                toSource ((tx + 0.5) * kx - 0.5, v, cw, ch, sxd, syd);
                const int sx = juce::jlimit (0, cw - 1, (int) std::lround (sxd)), sy = juce::jlimit (0, ch - 1, (int) std::lround (syd));
                const uint8_t* c = uv + (size_t) sy * pitch + (size_t) sx * 2;
                dst[tx * 2] = c[0];
                dst[tx * 2 + 1] = c[1];
            }
        }
    }
}
} // namespace hydra::cam

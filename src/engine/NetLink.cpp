#include "NetLink.h"
#include "core/NetCrypto.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <mstcpip.h>
#include <windows.h>
#include <avrt.h>
#include <map>
#include <mutex>
#include <thread>

#pragma comment (lib, "ws2_32.lib")
#pragma comment (lib, "iphlpapi.lib")
#pragma comment (lib, "avrt.lib")

#ifndef SIO_UDP_CONNRESET
 #define SIO_UDP_CONNRESET _WSAIOW (IOC_VENDOR, 12)
#endif

namespace hydra::net
{
namespace
{
/* Packet format, version 1 (little-endian). Every packet starts with the same 24 bytes:
       0  "HYLK"
       4  u8  version (1)
       5  u8  type (PacketType)
       6  u8  flags (bit 0: the rest is encrypted, 16-byte tag at the end)
       7  u8  0
       8  u64 instance  (random per start of the sending Hydra)
      16  u64 counter   (the sender's packet number: nonce of the encryption, replay window)
   Audio:     u32 stream id, u32 sequence, u32 sample position, u8 codec, u8 channels, u16 frames, payload
   Info:      u32 stream id, u8 codec, u8 channels, u16 frames, u32 rate, u16 kbps, str stream, str sender
   Announce:  u16 port, str name, u16 remote port (Hydra Remote, 0 = off; since 9.11., may be missing)
   Feedback:  u32 stream id, u32 received, u32 lost, u32 late, u32 jitter (us), u32 buffer (us)   (deltas since the last one)
   Bye:       u32 stream id
   str = u8 length + UTF-8 bytes (at most 63). */
constexpr uint8_t kVersion = 1;
enum PacketType : uint8_t { tAudio = 1, tInfo = 2, tAnnounce = 3, tFeedback = 4, tBye = 5 };
constexpr int kHeader = 24;
constexpr int kAudioHeader = 16;
constexpr int kMaxPacket = 1200;   // stays below the MTU of any network, VPNs included
constexpr int kMaxPayload = kMaxPacket - kHeader - kAudioHeader - KeyRing::kTagBytes;
constexpr int kMaxName = 63;
constexpr int kHold = 4;            // reorder window (packets)
constexpr int kMaxRemotes = 64, kMaxStreamsPerRemote = 32;

double nowMs() noexcept { return juce::Time::getMillisecondCounterHiRes(); }

struct Writer
{
    uint8_t* p;
    int cap, n = 0;
    void u8 (uint32_t v) noexcept  { if (n < cap) p[n] = (uint8_t) v; ++n; }
    void u16 (uint32_t v) noexcept { u8 (v); u8 (v >> 8); }
    void u32 (uint32_t v) noexcept { u16 (v); u16 (v >> 16); }
    void u64 (uint64_t v) noexcept { u32 ((uint32_t) v); u32 ((uint32_t) (v >> 32)); }
    void str (const juce::String& s)
    {
        const char* t = s.toRawUTF8();
        const int len = juce::jmin (kMaxName, (int) std::strlen (t));
        u8 ((uint32_t) len);
        for (int i = 0; i < len; ++i) u8 ((uint8_t) t[i]);
    }
    bool ok() const noexcept { return n <= cap; }
};

struct Reader
{
    const uint8_t* p;
    int n, pos = 0;
    bool ok = true;
    uint32_t u8() noexcept       { if (pos >= n) { ok = false; return 0; } return p[pos++]; }
    uint32_t u16() noexcept      { const uint32_t a = u8(); return a | (u8() << 8); }
    uint32_t u32() noexcept      { const uint32_t a = u16(); return a | (u16() << 16); }
    uint64_t u64() noexcept      { const uint64_t a = u32(); return a | ((uint64_t) u32() << 32); }
    juce::String str()
    {
        const int len = (int) u8();
        if (! ok || pos + len > n) { ok = false; return {}; }
        auto s = juce::String::fromUTF8 ((const char*) p + pos, len);
        pos += len;
        return s;
    }
    const uint8_t* rest() const noexcept { return p + pos; }
    int restBytes() const noexcept       { return n - pos; }
};

uint32_t ipOf (const sockaddr_in& a) noexcept { return ntohl (a.sin_addr.s_addr); }

bool isLanIp (uint32_t ip) noexcept
{
    return (ip >> 24) == 10 || (ip >> 24) == 127
        || (ip & 0xFFF00000u) == 0xAC100000u    // 172.16/12
        || (ip & 0xFFFF0000u) == 0xC0A80000u    // 192.168/16
        || (ip & 0xFFFF0000u) == 0xA9FE0000u;   // 169.254/16 (link-local)
}

juce::String ipText (uint32_t ip)
{
    return juce::String ((ip >> 24) & 255) + "." + juce::String ((ip >> 16) & 255) + "." + juce::String ((ip >> 8) & 255) + "." + juce::String (ip & 255);
}

sockaddr_in makeAddr (uint32_t ip, int port) noexcept
{
    sockaddr_in a {};
    a.sin_family = AF_INET;
    a.sin_port = htons ((u_short) port);
    a.sin_addr.s_addr = htonl (ip);
    return a;
}

bool parseIp (const juce::String& s, uint32_t& ip) noexcept
{
    in_addr a {};
    if (inet_pton (AF_INET, s.trim().toRawUTF8(), &a) != 1) return false;
    ip = ntohl (a.s_addr);
    return true;
}

/** IPv4 address of a host name or literal (blocks for DNS: never on the network thread). */
bool resolveHost (const juce::String& host, uint32_t& ip)
{
    if (parseIp (host, ip)) return true;
    addrinfo hints {}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo (host.trim().toRawUTF8(), nullptr, &hints, &res) != 0 || res == nullptr) return false;
    ip = ntohl (reinterpret_cast<sockaddr_in*> (res->ai_addr)->sin_addr.s_addr);
    freeaddrinfo (res);
    return true;
}

/** "host[:port]" */
void splitHostPort (const juce::String& s, juce::String& host, int& port)
{
    host = s.trim();
    port = kDefaultPort;
    if (const int colon = host.lastIndexOfChar (':'); colon > 0 && host.indexOfChar (':') == colon)
    {
        port = juce::jlimit (1, 65535, host.substring (colon + 1).getIntValue());
        host = host.substring (0, colon).trim();
    }
}

/** Frames per PCM packet: the engine block, split into equal parts until a part fits into one packet. */
int pcmPacketFrames (int engineBlock, Codec codec, int channels)
{
    const int bytesPerFrame = pcmBytes (codec) * channels;
    const int fit = kMaxPayload / juce::jmax (1, bytesPerFrame);
    const int block = juce::jmax (16, engineBlock);
    for (int k = 1; k < 64; ++k)
        if ((block + k - 1) / k <= fit) return juce::jmax (16, (block + k - 1) / k);
    return fit;
}
} // namespace

//==============================================================================
juce::String cleanName (const juce::String& s)
{
    auto t = s.removeCharacters ("@/?&\r\n\t").trim();
    while (t.isNotEmpty() && (int) std::strlen (t.toRawUTF8()) > kMaxName) t = t.dropLastCharacters (1);
    return t;
}

juce::String Destination::toDeviceName() const
{
    juce::String s ("hydralink://");
    s << host << ":" << port << "/" << juce::URL::addEscapeChars (stream, false);
    s << "?codec=" << codecId (codec);
    if (codec == Codec::opus || codec == Codec::automatic) s << "&kbps=" << kbps;
    return s;
}

Destination Destination::parse (const juce::String& deviceName)
{
    Destination d;
    auto s = deviceName.fromFirstOccurrenceOf ("hydralink://", false, true);
    if (s.isEmpty()) s = deviceName;
    const auto query = s.fromFirstOccurrenceOf ("?", false, false);
    s = s.upToFirstOccurrenceOf ("?", false, false);
    const auto hostPort = s.upToFirstOccurrenceOf ("/", false, false);
    d.stream = juce::URL::removeEscapeChars (s.fromFirstOccurrenceOf ("/", false, false));
    splitHostPort (hostPort, d.host, d.port);
    for (auto& kv : juce::StringArray::fromTokens (query, "&", {}))
    {
        const auto key = kv.upToFirstOccurrenceOf ("=", false, false), value = kv.fromFirstOccurrenceOf ("=", false, false);
        if (key == "codec") d.codec = codecFromId (value);
        else if (key == "kbps") d.kbps = juce::jlimit (16, 510, value.getIntValue());
    }
    return d;
}

IoTarget Destination::target() const
{
    IoTarget t;
    t.kind = IoTarget::Kind::Network;
    t.deviceName = toDeviceName();
    return t;
}

Source Source::parse (const juce::String& deviceName)
{
    return { deviceName.upToLastOccurrenceOf ("@", false, false), deviceName.fromLastOccurrenceOf ("@", false, false) };
}

IoTarget Source::target() const
{
    IoTarget t;
    t.kind = IoTarget::Kind::Network;
    t.deviceName = toDeviceName();
    return t;
}

//==============================================================================
/** One stream of one remote sender, as this Hydra receives it. */
struct Rx
{
    uint32_t id = 0;
    juce::String stream;
    Codec codec = Codec::pcm24;
    int channels = 2, frames = 0, kbps = 0;
    double rate = 0.0;
    bool hasInfo = false;
    double lastPacket = 0.0, lastInfo = 0.0;
    class NetInput* sub = nullptr;

    // Sequence (only while an input listens).
    bool started = false;
    uint32_t expect = 0;
    struct Held { bool used = false; int bytes = 0; double at = 0.0; std::array<uint8_t, kMaxPacket> data {}; };
    std::array<Held, kHold> hold {};
    std::array<std::vector<float>, 2> out, last;
    bool lastValid = false, fadeIn = false;
    int lastFrames = 0;   // PCM: length of the last packet played (packets carry what the sender's engine wrote)
    OpusDec dec;
    bool decReady = false;

    // Statistics.
    uint32_t received = 0, lost = 0, late = 0;
    uint32_t fbReceived = 0, fbLost = 0, fbLate = 0;   // values at the last feedback
    double jitter = 0.0, prevTransit = 0.0;            // seconds
    bool haveTransit = false;
    double lastFeedback = 0.0;

    void reset()
    {
        started = false;
        for (auto& h : hold) h.used = false;
        lastValid = fadeIn = false;
        lastFrames = frames;
        haveTransit = false;
        for (int c = 0; c < 2; ++c) { out[(size_t) c].assign ((size_t) juce::jmax (1, frames), 0.0f); last[(size_t) c].assign ((size_t) juce::jmax (1, frames), 0.0f); }
        decReady = codec == Codec::opus && dec.prepare (channels, frames);
    }
    int heldCount() const noexcept { int n = 0; for (auto& h : hold) n += h.used ? 1 : 0; return n; }
};

/** Another Hydra (one start of it: the instance id is new every time it starts). */
struct Remote
{
    uint64_t instance = 0;
    juce::String name;
    sockaddr_in from {};
    double lastSeen = 0.0;
    int port = 0;
    bool announced = false;
    uint64_t maxCounter = 0, window = 0;   // replay window (encrypted packets only)
    std::map<uint32_t, std::unique_ptr<Rx>> rx;

    /** Each packet number only once, at most 64 numbers back. */
    bool fresh (uint64_t c) noexcept
    {
        if (c > maxCounter)
        {
            const uint64_t shift = c - maxCounter;
            window = shift >= 64 ? 0 : window << shift;
            window |= 1;
            maxCounter = c;
            return true;
        }
        const uint64_t back = maxCounter - c;
        if (back >= 64) return false;
        const uint64_t bit = 1ull << back;
        if (window & bit) return false;
        window |= bit;
        return true;
    }
};

//==============================================================================
class NetInput final : public AsyncInput
{
public:
    NetInput (Hub::Impl& h, Source s) : hub (h), source (std::move (s)) {}
    ~NetInput() override;

    void prepare (double senderRate, int packetFrames, bool lan, bool opus, double engRate, int engBlock)
    {
        // Software over a network: stability before latency (the buffer never shrinks back into a dropout).
        preferStable = true;
        rate = senderRate;
        // Packets come in bursts of one packet, plus the network's jitter: ~3 ms in a local network, 10 ms beyond.
        const int jitter = (int) std::ceil (senderRate * (lan ? 0.003 : 0.010));
        // Never tighter than one packet + 3 ms (10 ms beyond the local network): the network's (and the two
        // computers') timing has rare spikes the drift controller would only learn from a dropout.
        jitterFloor = packetFrames + senderRate * (lan ? 0.003 : 0.010);
        // Packets come in bursts and gaps (packet size against block size, Wi-Fi): the fill minimum the drift
        // controller holds at its target must span such gaps, not only a few blocks.
        minWindowSeconds = 0.5;
        setup (senderRate, juce::jmax (packetFrames, jitter), 2, engRate, engBlock);
        deviceLatencyFrames = packetFrames + (opus ? OpusDec::kLookahead : 0);
        idle = true;
        transport = "Hydra Link, waiting for \"" + source.stream + "\" from " + source.sender;
    }

    void start() override { running = true; }
    void stop() override  { running = false; }
    void poll() override;

    /** Network thread. */
    void deliver (const float* const* data, int numCh, int n) noexcept
    {
        if (running.load (std::memory_order_relaxed)) produce (data, numCh, n);
    }
    void fail (const juce::String& why)
    {
        if (failed.load()) return;
        error = why;
        failed = true;
    }

    Hub::Impl& hub;
    const Source source;
    double rate = 48000.0;
    Rx* bound = nullptr;   // network thread (under the hub's lock)
    std::atomic<bool> running { false };

    // For poll() (written by the network thread).
    std::atomic<bool> statBound { false };
    std::atomic<uint32_t> statReceived { 0 }, statLost { 0 }, statLate { 0 }, statIp { 0 };
    std::atomic<float> statJitterMs { 0.0f };
    std::atomic<int> statCodec { 0 }, statKbps { 0 };
};

class NetOutput final : public AsyncOutput
{
public:
    explicit NetOutput (Hub::Impl& h) : hub (h) {}
    ~NetOutput() override;

    bool prepare (const Destination& d, uint32_t ip, int port, bool viaName, bool isLan, double engRate, int engBlock, juce::String& err)
    {
        dest = d;
        to = makeAddr (ip, port);
        byName = viaName;
        lan = isLan;
        streamId = (uint32_t) randomU64();
        if (streamId == 0) streamId = 1;
        codec = d.codec == Codec::automatic ? (lan ? Codec::pcm24 : Codec::opus) : d.codec;
        kbps = juce::jlimit (16, 510, d.kbps);
        direct = true;   // the engine's clock is the stream's clock: no resampler, no drift control
        if (codec == Codec::opus)
        {
            frames = lan ? 240 : 480;   // 5 ms in the local network, 10 ms beyond (half the packets, more robust)
            if (! enc.prepare (2, frames, kbps, lan ? 0 : 5)) { err = "Opus encoder failed"; return false; }
            // Opus runs at 48 kHz: other engine rates are resampled on the network thread.
            resample = std::abs (engRate - 48000.0) > 0.5;
            if (resample)
            {
                rs.prepare (2, engRate / 48000.0, 4096);
                ring48.allocate (2, 16384);
            }
            sendRate = 48000.0;
        }
        else
        {
            frames = pcmPacketFrames (engBlock, codec, 2);
            sendRate = engRate;
        }
        setup (engRate, juce::jmax (frames, engBlock), 2, engRate, engBlock);
        for (auto& v : planar) v.assign ((size_t) juce::jmax (frames, 4096), 0.0f);
        deviceLatencyFrames = codec == Codec::opus ? frames + OpusDec::kLookahead : frames;
        transport = "Hydra Link";
        return true;
    }

    void start() override { running = true; }
    void stop() override  { running = false; }
    void poll() override;
    void service (double now) noexcept;   // network thread, under the hub's lock
    void sendInfo();

    Hub::Impl& hub;
    Destination dest;
    sockaddr_in to {};
    bool byName = false, lan = true, resample = false;
    uint32_t streamId = 1, seq = 0, position = 0;
    Codec codec = Codec::pcm24;
    int frames = 128, kbps = 128;
    double sendRate = 48000.0;
    OpusEnc enc;
    VarResampler rs;
    SpscAudioRing ring48;
    std::array<std::vector<float>, 2> planar;
    double lastInfo = -1.0e9;
    int infoCount = 0;
    std::atomic<bool> running { false };

    // Feedback from the receiver (network thread writes, poll() reads).
    std::atomic<double> fbAt { 0.0 };
    std::atomic<float> fbLossPercent { 0.0f }, fbJitterMs { 0.0f }, fbBufferMs { 0.0f };
};

//==============================================================================
struct Hub::Impl
{
    explicit Impl (Hub& h) : owner (h)
    {
        WSADATA wsa {};
        wsaOk = WSAStartup (MAKEWORD (2, 2), &wsa) == 0;
        wake = CreateEventW (nullptr, FALSE, FALSE, nullptr);
    }

    ~Impl()
    {
        stopSocket();
        if (resolver.joinable()) resolver.join();
        if (wake) CloseHandle (wake);
        if (wsaOk) WSACleanup();
    }

    //==========================================================================
    bool start (const Config& c, juce::String& err)
    {
        Config want = c;
        want.port = juce::jlimit (1, 65535, want.port);
        want.name = cleanName (want.name.isNotEmpty() ? want.name : computerName());
        if (want.name.isEmpty()) want.name = "Hydra";

        std::unique_lock<std::mutex> l (lock);
        const bool restart = ! running || want.port != cfg.port || want.password != cfg.password || want.broadcast != cfg.broadcast
                           || want.loopbackOnly != cfg.loopbackOnly;
        const bool peersChanged = want.peers != cfg.peers;
        cfg = want;
        name = want.name;
        l.unlock();

        if (peersChanged || restart) resolvePeers();
        if (! restart) { wakeUp(); return true; }

        stopSocket();
        if (want.password != keyPassword)
        {
            groupKey = KeyRing::deriveGroupKey (want.password);
            keyPassword = want.password;
        }
        {
            std::lock_guard<std::mutex> g (lock);   // the network thread is stopped; openOutput reads isOn()
            keys.setGroupKey (groupKey, want.password.isNotEmpty());
        }

        if (! wsaOk) { err = lastError = "Windows networking is not available"; return false; }
        sock = socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET) { err = lastError = "Cannot create a network socket"; return false; }
        BOOL yes = TRUE, no = FALSE;
        setsockopt (sock, SOL_SOCKET, SO_BROADCAST, (const char*) &yes, sizeof (yes));
        setsockopt (sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*) &yes, sizeof (yes));
        int bufBytes = 1 << 20;
        setsockopt (sock, SOL_SOCKET, SO_RCVBUF, (const char*) &bufBytes, sizeof (bufBytes));
        setsockopt (sock, SOL_SOCKET, SO_SNDBUF, (const char*) &bufBytes, sizeof (bufBytes));
        // UDP on Windows: an ICMP "port unreachable" (nobody listens at a destination) would make the next
        // recvfrom fail with WSAECONNRESET. Turn that off.
        DWORD ret = 0;
        WSAIoctl (sock, SIO_UDP_CONNRESET, &no, sizeof (no), nullptr, 0, &ret, nullptr, nullptr);

        auto local = makeAddr (want.loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY, want.port);
        if (bind (sock, (const sockaddr*) &local, sizeof (local)) != 0)
        {
            const int e = WSAGetLastError();
            closesocket (sock);
            sock = INVALID_SOCKET;
            err = lastError = (e == WSAEADDRINUSE || e == WSAEACCES)
                ? "Port " + juce::String (want.port) + " is in use (is another Hydra running?)"
                : "Cannot open port " + juce::String (want.port) + " (error " + juce::String (e) + ")";
            return false;
        }
        sockEvent = WSACreateEvent();
        WSAEventSelect (sock, sockEvent, FD_READ);

        instance = randomU64();
        counter = 0;
        quit = false;
        {
            std::lock_guard<std::mutex> g (lock);
            remotes.clear();
            for (auto* in : ins) { in->bound = nullptr; in->statBound = false; }
            for (auto* o : outs) { o->lastInfo = -1.0e9; o->infoCount = 0; }
            running = true;
            lastError.clear();
            nextAnnounce = 0.0;
            lastIfScan = -1.0e9;
        }
        worker = std::thread ([this] { run(); });
        changes.fetch_add (1);
        return true;
    }

    /** Closes the socket and the thread. Streams stay registered (a restart with another port / password). */
    void stopSocket()
    {
        if (worker.joinable())
        {
            quit = true;
            SetEvent (wake);
            worker.join();
        }
        if (sock != INVALID_SOCKET) { closesocket (sock); sock = INVALID_SOCKET; }
        if (sockEvent != nullptr) { WSACloseEvent (sockEvent); sockEvent = nullptr; }
        std::lock_guard<std::mutex> l (lock);
        running = false;
    }

    void stop()
    {
        stopSocket();
        std::lock_guard<std::mutex> l (lock);
        for (auto* in : ins) { in->fail ("Hydra Link is off (Settings > Network)"); in->bound = nullptr; in->statBound = false; }
        for (auto* o : outs)
            if (! o->failed.load()) { o->error = "Hydra Link is off (Settings > Network)"; o->failed = true; }
        remotes.clear();
        byes.clear();
        changes.fetch_add (1);
    }

    void wakeUp() { SetEvent (wake); }

    /** Manual peer addresses: resolved in the background (DNS may take seconds). */
    void resolvePeers()
    {
        if (resolver.joinable()) resolver.join();
        juce::StringArray list;
        {
            std::lock_guard<std::mutex> l (lock);
            list = cfg.peers;
        }
        resolver = std::thread ([this, list]
        {
            std::vector<std::pair<juce::String, sockaddr_in>> found;
            for (auto& p : list)
            {
                juce::String host;
                int port = kDefaultPort;
                splitHostPort (p, host, port);
                uint32_t ip = 0;
                if (host.isNotEmpty() && resolveHost (host, ip)) found.emplace_back (p.trim(), makeAddr (ip, port));
                else found.emplace_back (p.trim(), sockaddr_in {});
            }
            std::lock_guard<std::mutex> l (lock);
            manualPeers = std::move (found);
            changes.fetch_add (1);
        });
    }

    //==========================================================================
    void run()
    {
        DWORD task = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW (L"Pro Audio", &task);
        HANDLE timer = CreateWaitableTimerExW (nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
        if (timer == nullptr) timer = CreateWaitableTimerW (nullptr, FALSE, nullptr);

        double nextHousekeep = 0.0;
        while (! quit.load())
        {
            double now = nowMs();
            double waitMs;
            {
                std::lock_guard<std::mutex> l (lock);
                waitMs = juce::jmax (0.0, nextHousekeep - now);
                bool sending = false;
                for (auto* o : outs) sending |= o->running.load (std::memory_order_relaxed);
                // Sending: every 1 ms (a packet leaves at most 1 ms after the engine wrote it).
                if (sending) waitMs = juce::jmin (waitMs, 1.0);
                if (heldUntil > 0.0) waitMs = juce::jmin (waitMs, juce::jmax (0.0, heldUntil - now));
            }
            LARGE_INTEGER due;
            due.QuadPart = -juce::jmax ((LONGLONG) 1, (LONGLONG) (waitMs * 10000.0));   // relative, 100 ns
            SetWaitableTimer (timer, &due, 0, nullptr, nullptr, FALSE);
            HANDLE events[3] = { sockEvent, wake, timer };
            WaitForMultipleObjects (3, events, FALSE, INFINITE);
            if (quit.load()) break;

            WSANETWORKEVENTS ne {};
            WSAEnumNetworkEvents (sock, sockEvent, &ne);   // resets the socket event

            now = nowMs();
            std::lock_guard<std::mutex> l (lock);
            receiveAll (now);
            for (auto* o : outs) o->service (now);
            flushHeld (now);
            if (now >= nextHousekeep)
            {
                housekeeping (now);
                nextHousekeep = now + 100.0;
            }
        }

        CancelWaitableTimer (timer);
        CloseHandle (timer);
        if (mmcss) AvRevertMmThreadCharacteristics (mmcss);
    }

    //==========================================================================
    // Sending (network thread, under the lock)

    /** Header + body (written by fill), encrypted if there is a password, sent to `to`. */
    template <typename Fill>
    bool send (PacketType type, const sockaddr_in& to, Fill&& fill) noexcept
    {
        if (sock == INVALID_SOCKET || to.sin_family != AF_INET) return false;
        uint8_t buf[kMaxPacket + 64];
        Writer w { buf, kMaxPacket - KeyRing::kTagBytes };
        const uint64_t c = ++counter;
        w.u8 ('H'); w.u8 ('Y'); w.u8 ('L'); w.u8 ('K');
        w.u8 (kVersion); w.u8 (type); w.u8 (keys.isOn() ? 1 : 0); w.u8 (0);
        w.u64 (instance);
        w.u64 (c);
        fill (w);
        if (! w.ok()) return false;
        int len = w.n;
        if (keys.isOn())
        {
            if (! keys.seal (instance, c, buf, kHeader, buf + kHeader, len - kHeader, buf + len)) return false;
            len += KeyRing::kTagBytes;
        }
        return sendto (sock, (const char*) buf, len, 0, (const sockaddr*) &to, sizeof (to)) == len;
    }

    void announce (double now)
    {
        if (now - lastIfScan > 30000.0) { scanInterfaces(); lastIfScan = now; }
        // Since 9.11. a third field: Hydra Remote's port (0 = off). Older receivers stop reading after the name.
        auto fill = [this] (Writer& w) { w.u16 ((uint32_t) cfg.port); w.str (name); w.u16 ((uint32_t) cfg.remotePort); };
        if (cfg.broadcast)
            for (auto& b : broadcasts)
            {
                send (tAnnounce, makeAddr (ipOf (b), cfg.port), fill);
                if (cfg.port != kDefaultPort) send (tAnnounce, makeAddr (ipOf (b), kDefaultPort), fill);
            }
        for (auto& p : manualPeers)
            if (p.second.sin_family == AF_INET) send (tAnnounce, p.second, fill);
    }

    /** Directed broadcast address of every IPv4 network this computer is in. */
    void scanInterfaces()
    {
        broadcasts.clear();
        for (auto& a : interfaceAddresses())
        {
            const uint32_t mask = a.second >= 32 ? 0xFFFFFFFFu : a.second == 0 ? 0u : ~(0xFFFFFFFFu >> a.second);
            broadcasts.push_back (makeAddr ((a.first & mask) | ~mask, cfg.port));
        }
        if (broadcasts.empty()) broadcasts.push_back (makeAddr (INADDR_BROADCAST, cfg.port));
    }

    /** IPv4 addresses (host order) with their prefix length, of the interfaces that are up (no loopback). */
    static std::vector<std::pair<uint32_t, int>> interfaceAddresses()
    {
        std::vector<std::pair<uint32_t, int>> list;
        ULONG size = 16384;
        std::vector<uint8_t> mem (size);
        auto* first = reinterpret_cast<IP_ADAPTER_ADDRESSES*> (mem.data());
        const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
        ULONG r = GetAdaptersAddresses (AF_INET, flags, nullptr, first, &size);
        if (r == ERROR_BUFFER_OVERFLOW)
        {
            mem.resize (size);
            first = reinterpret_cast<IP_ADAPTER_ADDRESSES*> (mem.data());
            r = GetAdaptersAddresses (AF_INET, flags, nullptr, first, &size);
        }
        if (r != NO_ERROR) return list;
        for (auto* a = first; a != nullptr; a = a->Next)
        {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (auto* u = a->FirstUnicastAddress; u != nullptr; u = u->Next)
                if (u->Address.lpSockaddr != nullptr && u->Address.lpSockaddr->sa_family == AF_INET)
                    list.emplace_back (ipOf (*reinterpret_cast<sockaddr_in*> (u->Address.lpSockaddr)), (int) u->OnLinkPrefixLength);
        }
        return list;
    }

    //==========================================================================
    // Receiving (network thread, under the lock)

    void receiveAll (double now)
    {
        uint8_t buf[2048];
        for (int i = 0; i < 512; ++i)
        {
            sockaddr_in from {};
            int fromLen = sizeof (from);
            const int n = recvfrom (sock, (char*) buf, sizeof (buf), 0, (sockaddr*) &from, &fromLen);
            if (n < 0)
            {
                if (WSAGetLastError() == WSAECONNRESET) continue;
                break;   // WSAEWOULDBLOCK: nothing more
            }
            if (applyFaults (buf, n, from, now)) continue;
            handle (buf, n, from, now);
        }
    }

    /** Tests only: true if the packet was taken (dropped or held back). */
    bool applyFaults (uint8_t* buf, int n, const sockaddr_in& from, double now)
    {
        auto& f = owner.faults;
        const int drop = f.dropPercent.load (std::memory_order_relaxed), swap = f.swapEvery.load (std::memory_order_relaxed);
        const int replay = f.replayEvery.load (std::memory_order_relaxed), corrupt = f.corruptEvery.load (std::memory_order_relaxed);
        if ((drop | swap | replay | corrupt) == 0 || n < kHeader || buf[5] != tAudio) return false;
        ++faultCount;
        if (drop > 0)
        {
            faultRng = faultRng * 1664525u + 1013904223u;
            if ((int) ((faultRng >> 8) % 100) < drop) return true;
        }
        if (corrupt > 0 && faultCount % (uint32_t) corrupt == 0) buf[n - 1] ^= 0x5a;
        if (replay > 0 && faultCount % (uint32_t) replay == 0)
        {
            std::vector<uint8_t> copy (buf, buf + n);
            handle (buf, n, from, now);
            handle (copy.data(), n, from, now);
            return true;
        }
        if (swap > 0)
        {
            if (swapped.empty() && faultCount % (uint32_t) swap == 0) { swapped.assign (buf, buf + n); return true; }
            if (! swapped.empty())
            {
                handle (buf, n, from, now);
                auto held = std::move (swapped);
                swapped.clear();
                handle (held.data(), (int) held.size(), from, now);
                return true;
            }
        }
        return false;
    }

    void reject (const sockaddr_in& from, const juce::String& why, double now)
    {
        rejected.fetch_add (1, std::memory_order_relaxed);
        if (now - rejectAt > 1000.0)   // the text is only for the settings page: build it at most once per second
        {
            rejectHint = ipText (ipOf (from)) + ": " + why;
            rejectAt = now;
        }
    }

    void handle (uint8_t* buf, int n, const sockaddr_in& from, double now)
    {
        if (n < kHeader + 2 || buf[0] != 'H' || buf[1] != 'Y' || buf[2] != 'L' || buf[3] != 'K') return;
        if (buf[4] != kVersion) { reject (from, "uses another version of Hydra Link (update both Hydras)", now); return; }
        Reader h { buf, kHeader };
        h.pos = 5;
        const uint8_t type = (uint8_t) h.u8(), flags = (uint8_t) h.u8();
        h.u8();
        const uint64_t inst = h.u64(), c = h.u64();
        if (inst == instance) return;   // our own broadcast
        const bool encrypted = (flags & 1) != 0;
        int len = n;
        if (keys.isOn())
        {
            if (! encrypted) { reject (from, "sends without a password (set the same network password on both computers)", now); return; }
            if (n < kHeader + KeyRing::kTagBytes + 2) return;
            len = n - KeyRing::kTagBytes;
            if (! keys.open (inst, c, buf, kHeader, buf + kHeader, len - kHeader, buf + len))
            {
                reject (from, "sends with another network password", now);
                return;
            }
        }
        else
        {
            if (encrypted) { reject (from, "uses a network password (set the same one here)", now); return; }
            if (! isLanIp (ipOf (from))) { reject (from, "is outside the local network: this needs a network password", now); return; }
        }

        auto* r = remoteFor (inst, from, now);
        if (r == nullptr) return;
        if (keys.isOn() && ! r->fresh (c)) { reject (from, "repeated an old packet", now); return; }
        r->from = from;
        r->lastSeen = now;

        Reader b { buf + kHeader, len - kHeader };
        switch (type)
        {
            case tAudio:    onAudio (*r, b, now); break;
            case tInfo:     onInfo (*r, b, now); break;
            case tAnnounce: onAnnounce (*r, b); break;
            case tFeedback: onFeedback (b); break;
            case tBye:      onBye (*r, b); break;
            default: break;
        }
    }

    Remote* remoteFor (uint64_t inst, const sockaddr_in& from, double now)
    {
        if (auto it = remotes.find (inst); it != remotes.end()) return it->second.get();
        if ((int) remotes.size() >= kMaxRemotes)
        {
            // Full (a lot of restarts or junk): forget the one heard from longest ago, if it is quiet.
            auto oldest = std::min_element (remotes.begin(), remotes.end(), [] (auto& a, auto& b) { return a.second->lastSeen < b.second->lastSeen; });
            if (now - oldest->second->lastSeen < 10000.0) return nullptr;
            dropRemote (oldest);
        }
        auto r = std::make_unique<Remote>();
        r->instance = inst;
        r->from = from;
        r->lastSeen = now;
        auto* p = r.get();
        remotes[inst] = std::move (r);
        return p;
    }

    void dropRemote (std::map<uint64_t, std::unique_ptr<Remote>>::iterator it)
    {
        for (auto& [id, rx] : it->second->rx) unbind (*rx);
        remotes.erase (it);
        changes.fetch_add (1);
    }

    void onAnnounce (Remote& r, Reader& b)
    {
        const int port = (int) b.u16();
        const auto n = cleanName (b.str());
        if (! b.ok || n.isEmpty()) return;
        if (! r.announced || r.name != n || r.port != port) changes.fetch_add (1);
        r.announced = true;
        r.name = n;
        r.port = port;
    }

    void onInfo (Remote& r, Reader& b, double now)
    {
        const uint32_t id = b.u32();
        const auto codec = (Codec) b.u8();
        const int ch = (int) b.u8(), frames = (int) b.u16();
        const double rate = (double) b.u32();
        const int kbps = (int) b.u16();
        const auto stream = cleanName (b.str()), sender = cleanName (b.str());
        if (! b.ok || stream.isEmpty() || sender.isEmpty() || ch < 1 || ch > 2 || rate < 8000.0 || rate > 384000.0) return;
        if (codec == Codec::opus ? (frames != 120 && frames != 240 && frames != 480 && frames != 960)
                                 : (pcmBytes (codec) == 0 || frames < 16 || frames * ch * pcmBytes (codec) > kMaxPayload)) return;

        auto it = r.rx.find (id);
        if (it == r.rx.end())
        {
            if ((int) r.rx.size() >= kMaxStreamsPerRemote) return;
            it = r.rx.emplace (id, std::make_unique<Rx>()).first;
            it->second->id = id;
            changes.fetch_add (1);
        }
        auto& x = *it->second;
        const bool formatChanged = ! x.hasInfo || x.codec != codec || x.channels != ch || x.frames != frames || x.rate != rate;
        if (x.stream != stream || r.name != sender) changes.fetch_add (1);
        x.stream = stream;
        x.codec = codec;
        x.channels = ch;
        x.frames = frames;
        x.rate = rate;
        x.kbps = kbps;
        x.hasInfo = true;
        x.lastInfo = now;
        if (x.lastPacket == 0.0) x.lastPacket = now;
        r.name = sender;
        if (formatChanged)
        {
            if (x.sub != nullptr && x.sub->rate != rate)
            {
                x.sub->fail ("The sender changed its sample rate");
                unbind (x);
            }
            x.reset();
        }
        bindInputs (now);
    }

    void onAudio (Remote& r, Reader& b, double now)
    {
        const uint32_t id = b.u32(), seq = b.u32(), pos = b.u32();
        const auto codec = (Codec) b.u8();
        const int ch = (int) b.u8(), frames = (int) b.u16();
        if (! b.ok) return;
        auto it = r.rx.find (id);
        if (it == r.rx.end())
        {
            // Not described yet (the description comes once per second): remember the stream, wait for it.
            if ((int) r.rx.size() < kMaxStreamsPerRemote) { auto x = std::make_unique<Rx>(); x->id = id; x->lastPacket = now; r.rx.emplace (id, std::move (x)); }
            return;
        }
        auto& x = *it->second;
        x.lastPacket = now;
        // PCM packets carry what the sender's engine wrote (up to the described length), Opus packets a fixed frame.
        if (! x.hasInfo || codec != x.codec || ch != x.channels || (codec == Codec::opus ? frames != x.frames : (frames < 1 || frames > x.frames))) return;
        ++x.received;

        // Interarrival jitter (RFC 3550): how much the transit time varies.
        const double transit = now * 0.001 - (double) pos / x.rate;
        if (x.haveTransit)
        {
            double d = std::abs (transit - x.prevTransit);
            if (d > 1.0) d = 0.0;   // sample counter wrapped / sender restarted
            x.jitter += (d - x.jitter) / 16.0;
        }
        x.prevTransit = transit;
        x.haveTransit = true;

        if (x.sub == nullptr) return;
        place (x, seq, b.rest(), b.restBytes(), now);
    }

    void onFeedback (Reader& b)
    {
        const uint32_t id = b.u32(), received = b.u32(), lost = b.u32(), late = b.u32(), jitterUs = b.u32(), bufferUs = b.u32();
        if (! b.ok) return;
        juce::ignoreUnused (late);
        for (auto* o : outs)
            if (o->streamId == id)
            {
                const double total = (double) received + (double) lost;
                o->fbLossPercent = total > 0.0 ? (float) (100.0 * lost / total) : 0.0f;
                o->fbJitterMs = (float) (jitterUs / 1000.0);
                o->fbBufferMs = (float) (bufferUs / 1000.0);
                o->fbAt = nowMs();
            }
    }

    void onBye (Remote& r, Reader& b)
    {
        const uint32_t id = b.u32();
        if (! b.ok) return;
        if (auto it = r.rx.find (id); it != r.rx.end())
        {
            unbind (*it->second);
            r.rx.erase (it);
            changes.fetch_add (1);
        }
    }

    //==========================================================================
    // Sequencing and concealment of the stream an input listens to

    /** hold[k] holds the packet with sequence expect + k; hold[0] is played as soon as it is there. */
    void place (Rx& x, uint32_t seq, const uint8_t* data, int bytes, double now)
    {
        if (bytes > kMaxPacket) return;
        if (! x.started) { x.started = true; x.expect = seq; }
        int32_t d = (int32_t) (seq - x.expect);
        if (d < 0) { ++x.late; return; }   // too late (already concealed) or a duplicate
        if (d > 32)
        {
            // A long gap (sender paused, network down): start again at this packet without filling the gap.
            x.lost += (uint32_t) d;
            for (auto& h : x.hold) h.used = false;
            x.expect = seq;
            x.lastValid = false;
            x.fadeIn = true;
            d = 0;
        }
        while (d >= kHold) { takeFirst (x); --d; }
        auto& slot = x.hold[(size_t) d];
        if (slot.used) { ++x.late; return; }   // duplicate
        slot.used = true;
        slot.bytes = bytes;
        slot.at = now;
        std::memcpy (slot.data.data(), data, (size_t) bytes);
        drain (x);
        // A packet is missing and later ones are here: lost after two later packets (three with Opus), or after
        // half a packet length (flushHeld). A packet that only came one place late (reordered) still plays.
        if (! x.hold[0].used && x.heldCount() >= (x.codec == Codec::opus ? 3 : 2))
        {
            takeFirst (x);
            drain (x);
        }
        updateHeldDue();
    }

    void advance (Rx& x) noexcept
    {
        ++x.expect;
        for (int k = 0; k + 1 < kHold; ++k) std::swap (x.hold[(size_t) k], x.hold[(size_t) k + 1]);
        x.hold[kHold - 1].used = false;
    }

    void drain (Rx& x) noexcept
    {
        while (x.hold[0].used)
        {
            play (x, x.hold[0].data.data(), x.hold[0].bytes);
            x.hold[0].used = false;
            advance (x);
        }
    }

    /** The expected packet: played if it is there, otherwise concealed. */
    void takeFirst (Rx& x) noexcept
    {
        if (x.hold[0].used) { play (x, x.hold[0].data.data(), x.hold[0].bytes); x.hold[0].used = false; }
        else conceal (x);
        advance (x);
    }

    /** A missing packet is given up half a packet length after a later one came (the ring holds more than that). */
    void flushHeld (double now) noexcept
    {
        if (heldUntil <= 0.0 || now < heldUntil) return;
        for (auto& [inst, r] : remotes)
            for (auto& [id, rx] : r->rx)
            {
                auto& x = *rx;
                if (x.sub == nullptr || x.hold[0].used || x.heldCount() == 0) continue;
                double oldest = 1.0e18;
                for (auto& h : x.hold) if (h.used) oldest = juce::jmin (oldest, h.at);
                if (now - oldest >= packetMs (x) * 0.5)
                {
                    while (x.heldCount() > 0 && ! x.hold[0].used) { takeFirst (x); drain (x); }
                }
            }
        updateHeldDue();
    }

    static double packetMs (const Rx& x) noexcept { return 1000.0 * x.frames / juce::jmax (8000.0, x.rate); }

    void updateHeldDue() noexcept
    {
        heldUntil = 0.0;
        for (auto& [inst, r] : remotes)
            for (auto& [id, rx] : r->rx)
                for (auto& h : rx->hold)
                    if (h.used && rx->sub != nullptr)
                    {
                        const double due = h.at + packetMs (*rx) * 0.5;
                        heldUntil = heldUntil <= 0.0 ? due : juce::jmin (heldUntil, due);
                    }
    }

    void play (Rx& x, const uint8_t* data, int bytes) noexcept
    {
        float* o[2] = { x.out[0].data(), x.out[1].data() };
        bool ok = false;
        int n = x.frames;
        if (x.codec == Codec::opus)
            ok = x.decReady && x.dec.decode (data, bytes, o, false);
        else if (const int bpf = x.channels * pcmBytes (x.codec); bpf > 0 && bytes > 0 && bytes % bpf == 0 && bytes / bpf <= x.frames)
        {
            n = bytes / bpf;
            decodePcm (x.codec, data, x.channels, n, o);
            ok = true;
        }
        if (! ok) { conceal (x); return; }

        if (x.fadeIn && x.codec != Codec::opus)
        {
            // After a concealed gap: a short fade-in instead of a click.
            const int fade = juce::jmin (64, n);
            for (int c = 0; c < x.channels; ++c)
                for (int i = 0; i < fade; ++i) o[c][i] *= (float) i / (float) fade;
        }
        x.fadeIn = false;
        if (x.codec != Codec::opus)
        {
            for (int c = 0; c < x.channels; ++c) std::memcpy (x.last[(size_t) c].data(), o[c], (size_t) n * sizeof (float));
            x.lastValid = true;
            x.lastFrames = n;
        }
        x.sub->deliver (o, x.channels, n);
    }

    void conceal (Rx& x) noexcept
    {
        ++x.lost;
        float* o[2] = { x.out[0].data(), x.out[1].data() };
        int n = x.frames;
        if (x.codec == Codec::opus)
        {
            // Redundancy of the next packet if it is there (in-band FEC), otherwise Opus' own concealment.
            if (x.hold[1].used) x.dec.decode (x.hold[1].data.data(), x.hold[1].bytes, o, true);
            else                x.dec.decode (nullptr, 0, o, false);
        }
        else
        {
            // PCM: the last packet once more, fading out (no hard cut), then silence.
            // As long as the last packet (the lost one most likely had the same length).
            n = juce::jlimit (1, x.frames, x.lastFrames > 0 ? x.lastFrames : x.frames);
            for (int c = 0; c < x.channels; ++c)
            {
                if (x.lastValid)
                    for (int i = 0; i < n; ++i) o[c][i] = x.last[(size_t) c][(size_t) i] * (1.0f - (float) (i + 1) / (float) n);
                else
                    std::memset (o[c], 0, (size_t) n * sizeof (float));
            }
            x.lastValid = false;
            x.fadeIn = true;
        }
        x.sub->deliver (o, x.channels, n);
    }

    //==========================================================================
    void unbind (Rx& x) noexcept
    {
        if (x.sub != nullptr)
        {
            x.sub->bound = nullptr;
            x.sub->statBound = false;
            x.sub->idle = true;
        }
        x.sub = nullptr;
    }

    /** Inputs without a (live) stream take the most recent matching one: by stream name and sender name, so a
        sender that restarted (new instance) or got another address is found again. */
    void bindInputs (double now)
    {
        for (auto* in : ins)
        {
            if (in->failed.load()) continue;
            if (in->bound != nullptr && now - in->bound->lastPacket < 1000.0) continue;
            Rx* best = nullptr;
            Remote* bestRemote = nullptr;
            for (auto& [inst, r] : remotes)
            {
                if (! r->name.equalsIgnoreCase (in->source.sender)) continue;
                for (auto& [id, rx] : r->rx)
                    if (rx->hasInfo && rx->stream.equalsIgnoreCase (in->source.stream) && now - rx->lastPacket < 1000.0
                        && (rx->sub == nullptr || rx->sub == in) && (best == nullptr || rx->lastPacket > best->lastPacket))
                    {
                        best = rx.get();
                        bestRemote = r.get();
                    }
            }
            if (best == nullptr || best == in->bound) continue;
            if (best->rate != in->rate)
            {
                in->fail ("The sender's sample rate is " + juce::String (best->rate / 1000.0, 1) + " kHz now");
                continue;
            }
            if (in->bound != nullptr) unbind (*in->bound);
            best->sub = in;
            best->reset();
            in->bound = best;
            in->statBound = true;
            in->statIp = ipOf (bestRemote->from);
            in->statCodec = (int) best->codec;
            in->statKbps = best->kbps;
            changes.fetch_add (1);
        }
    }

    void housekeeping (double now)
    {
        // Byes of streams that were closed.
        for (auto& b : byes) send (tBye, b.first, [id = b.second] (Writer& w) { w.u32 (id); });
        byes.clear();

        if (now >= nextAnnounce) { announce (now); nextAnnounce = now + 2000.0; }

        for (auto* o : outs)
        {
            if (! o->running.load (std::memory_order_relaxed)) continue;
            // Sending to a Hydra by its name: follow it to its current address.
            if (o->byName && now - o->lastInfo >= 900.0)
                for (auto& [inst, r] : remotes)
                    if (r->announced && r->name.equalsIgnoreCase (o->dest.host) && now - r->lastSeen < 6000.0)
                        o->to = makeAddr (ipOf (r->from), r->port > 0 ? r->port : o->dest.port);
            // Describe the stream: 5x per second at first (the receiver can start right away), then once per second.
            if (now - o->lastInfo >= (o->infoCount < 5 ? 200.0 : 1000.0)) { o->sendInfo(); o->lastInfo = now; ++o->infoCount; }
        }

        bindInputs (now);
        for (auto* in : ins)
        {
            const bool live = in->bound != nullptr && now - in->bound->lastPacket < 500.0;
            in->idle = ! live;
            if (auto* x = in->bound)
            {
                in->statReceived = x->received;
                in->statLost = x->lost;
                in->statLate = x->late;
                in->statJitterMs = (float) (x->jitter * 1000.0);
            }
        }

        // Feedback for the senders (once per second per stream that is listened to).
        for (auto& [inst, r] : remotes)
            for (auto& [id, rx] : r->rx)
            {
                auto& x = *rx;
                if (x.sub == nullptr || now - x.lastFeedback < 1000.0) continue;
                x.lastFeedback = now;
                const uint32_t dr = x.received - x.fbReceived, dl = x.lost - x.fbLost, dlate = x.late - x.fbLate;
                x.fbReceived = x.received; x.fbLost = x.lost; x.fbLate = x.late;
                const auto bufferUs = (uint32_t) (1.0e6 * x.sub->latencyFrames.load (std::memory_order_relaxed) / juce::jmax (8000.0, x.rate));
                const auto jitterUs = (uint32_t) (x.jitter * 1.0e6);
                send (tFeedback, r->from, [&] (Writer& w) { w.u32 (x.id); w.u32 (dr); w.u32 (dl); w.u32 (dlate); w.u32 (jitterUs); w.u32 (bufferUs); });
            }

        // Forget streams and Hydras that went quiet.
        for (auto it = remotes.begin(); it != remotes.end();)
        {
            auto& r = *it->second;
            for (auto s = r.rx.begin(); s != r.rx.end();)
            {
                if (now - s->second->lastPacket > 10000.0 && now - s->second->lastInfo > 10000.0)
                {
                    unbind (*s->second);
                    s = r.rx.erase (s);
                    changes.fetch_add (1);
                }
                else ++s;
            }
            if (r.rx.empty() && now - r.lastSeen > 15000.0) it = (dropRemote (it), remotes.begin());
            else ++it;
        }
    }

    //==========================================================================
    Hub& owner;
    mutable std::mutex lock;
    Config cfg;
    juce::String name, lastError;
    bool running = false, wsaOk = false;
    SOCKET sock = INVALID_SOCKET;
    HANDLE sockEvent = nullptr, wake = nullptr;
    std::thread worker, resolver;
    std::atomic<bool> quit { false };
    uint64_t instance = 0, counter = 0;
    KeyRing keys;
    juce::String keyPassword;
    std::array<uint8_t, 32> groupKey {};
    std::vector<NetInput*> ins;
    std::vector<NetOutput*> outs;
    std::map<uint64_t, std::unique_ptr<Remote>> remotes;
    std::vector<sockaddr_in> broadcasts;
    std::vector<std::pair<juce::String, sockaddr_in>> manualPeers;
    std::vector<std::pair<sockaddr_in, uint32_t>> byes;
    double lastIfScan = -1.0e9, nextAnnounce = 0.0, heldUntil = 0.0;
    std::atomic<uint32_t> changes { 0 }, rejected { 0 };
    juce::String rejectHint;
    double rejectAt = -1.0e9;
    uint32_t faultCount = 0, faultRng = 12345;
    std::vector<uint8_t> swapped;
};

//==============================================================================
NetInput::~NetInput()
{
    std::lock_guard<std::mutex> l (hub.lock);
    if (bound != nullptr) hub.unbind (*bound);
    hub.ins.erase (std::remove (hub.ins.begin(), hub.ins.end(), this), hub.ins.end());
}

void NetInput::poll()
{
    if (! statBound.load())
    {
        transport = "Hydra Link, waiting for \"" + source.stream + "\" from " + source.sender;
        return;
    }
    const auto codec = (Codec) statCodec.load();
    const double got = statReceived.load(), lost = statLost.load();
    juce::String t ("Hydra Link, ");
    t << codecName (codec);
    if (codec == Codec::opus) t << " " << statKbps.load() << " kbit/s";
    t << " from " << source.sender << " (" << ipText (statIp.load()) << ")";
    t << ", " << juce::String (got + lost > 0 ? 100.0 * lost / (got + lost) : 0.0, 1) << " % lost";
    t << ", jitter " << juce::String (statJitterMs.load(), 1) << " ms";
    transport = t;
}

NetOutput::~NetOutput()
{
    std::lock_guard<std::mutex> l (hub.lock);
    hub.outs.erase (std::remove (hub.outs.begin(), hub.outs.end(), this), hub.outs.end());
    hub.byes.emplace_back (to, streamId);   // tell the receiver right away (sent by the network thread)
    hub.wakeUp();
}

void NetOutput::poll()
{
    juce::String t ("Hydra Link, ");
    t << codecName (codec);
    if (codec == Codec::opus) t << " " << kbps << " kbit/s";
    t << " to " << dest.host;
    if (byName || ! dest.host.containsOnly ("0123456789.")) t << " (" << ipText (ipOf (to)) << ")";
    if (nowMs() - fbAt.load() < 3000.0)
        t << ", received with " << juce::String (fbLossPercent.load(), 1) << " % lost, " << juce::String (fbBufferMs.load(), 1) << " ms buffer";
    else
        t << ", nobody listening yet";
    transport = t;
}

void NetOutput::sendInfo()
{
    hub.send (tInfo, to, [this] (Writer& w)
    {
        w.u32 (streamId);
        w.u8 ((uint8_t) codec);
        w.u8 (2);
        w.u16 ((uint32_t) frames);
        w.u32 ((uint32_t) std::lround (sendRate));
        w.u16 ((uint32_t) (codec == Codec::opus ? kbps : 0));
        w.str (cleanName (dest.stream).isNotEmpty() ? cleanName (dest.stream) : juce::String ("Hydra"));
        w.str (hub.name);
    });
}

void NetOutput::service (double) noexcept
{
    if (! running.load (std::memory_order_relaxed)) return;
    deviceCallbacks.fetch_add (1, std::memory_order_relaxed);
    float* p[2] = { planar[0].data(), planar[1].data() };
    uint8_t payload[kMaxPayload];

    auto sendAudio = [this] (const uint8_t* data, int bytes, int n)
    {
        hub.send (tAudio, to, [&] (Writer& w)
        {
            w.u32 (streamId); w.u32 (seq); w.u32 (position);
            w.u8 ((uint8_t) codec); w.u8 (2); w.u16 ((uint32_t) n);
            for (int i = 0; i < bytes; ++i) w.u8 (data[i]);
        });
        ++seq;
        position += (uint32_t) n;
    };

    if (codec != Codec::opus)
    {
        // Whatever the engine wrote, at once (no waiting for a full packet: less latency, and a sender whose clock is
        // slower does not leave a regular gap of one packet). Longer blocks are split to fit into packets.
        while (const int avail = ring.available())
        {
            const int n = juce::jmin (avail, frames);
            ring.read (p, 2, n);
            encodePcm (codec, p, 2, n, payload);
            sendAudio (payload, n * 2 * pcmBytes (codec), n);
        }
        return;
    }

    SpscAudioRing* src = &ring;
    if (resample)
    {
        // Engine rate -> 48 kHz for Opus (fixed ratio).
        while (ring.available() > 0)
        {
            const int n = juce::jmin (ring.available(), 1024);
            ring.read (p, 2, n);
            const float* in[2] = { p[0], p[1] };
            rs.push (in, n, ring48);
        }
        src = &ring48;
    }
    while (src->available() >= frames)
    {
        src->read (p, 2, frames);
        const int bytes = enc.encode (p, payload, kMaxPayload);
        if (bytes > 0) sendAudio (payload, bytes, frames);
    }
}

//==============================================================================
Hub::Hub() : impl (std::make_unique<Impl> (*this)) {}
Hub::~Hub() { impl->stopSocket(); }

Hub& Hub::get()
{
    static Hub hub;
    return hub;
}

bool Hub::start (const Config& c, juce::String& error) { return impl->start (c, error); }
void Hub::stop() { impl->stop(); }

bool Hub::isRunning() const noexcept
{
    std::lock_guard<std::mutex> l (impl->lock);
    return impl->running;
}

Config Hub::getConfig() const
{
    std::lock_guard<std::mutex> l (impl->lock);
    return impl->cfg;
}

juce::String Hub::getName() const
{
    std::lock_guard<std::mutex> l (impl->lock);
    return impl->name;
}

juce::String Hub::getLastError() const
{
    std::lock_guard<std::mutex> l (impl->lock);
    return impl->lastError;
}

uint32_t Hub::getChangeCount() const noexcept { return impl->changes.load(); }
uint32_t Hub::getRejectedCount() const noexcept { return impl->rejected.load(); }

juce::String Hub::getRejectHint() const
{
    std::lock_guard<std::mutex> l (impl->lock);
    return nowMs() - impl->rejectAt < 10000.0 ? impl->rejectHint : juce::String();
}

std::vector<PeerInfo> Hub::getPeers() const
{
    std::vector<PeerInfo> list;
    std::lock_guard<std::mutex> l (impl->lock);
    const double now = nowMs();
    for (auto& [inst, r] : impl->remotes)
    {
        if (! r->announced || now - r->lastSeen > 10000.0) continue;
        // A Hydra that restarted is heard under a new instance: list each name once (the newest).
        auto same = std::find_if (list.begin(), list.end(), [&] (const PeerInfo& p) { return p.name.equalsIgnoreCase (r->name); });
        PeerInfo p { r->name, ipText (ipOf (r->from)), r->port, isLanIp (ipOf (r->from)), false, (now - r->lastSeen) / 1000.0 };
        if (same == list.end()) list.push_back (p);
        else if (p.secondsAgo < same->secondsAgo) *same = p;
    }
    for (auto& [text, addr] : impl->manualPeers)
    {
        const bool heard = std::any_of (list.begin(), list.end(), [&] (const PeerInfo& p) { return addr.sin_family == AF_INET && p.address == ipText (ipOf (addr)); });
        for (auto& p : list) if (addr.sin_family == AF_INET && p.address == ipText (ipOf (addr))) p.manual = true;
        if (! heard)
        {
            PeerInfo p;
            p.address = text;
            p.port = addr.sin_family == AF_INET ? (int) ntohs (addr.sin_port) : kDefaultPort;
            p.lan = addr.sin_family == AF_INET && isLanIp (ipOf (addr));
            p.manual = true;
            list.push_back (p);
        }
    }
    std::sort (list.begin(), list.end(), [] (const PeerInfo& a, const PeerInfo& b) { return a.name.compareNatural (b.name) < 0; });
    return list;
}

std::vector<StreamInfo> Hub::getStreams() const
{
    std::vector<StreamInfo> list;
    std::lock_guard<std::mutex> l (impl->lock);
    const double now = nowMs();
    for (auto& [inst, r] : impl->remotes)
        for (auto& [id, rx] : r->rx)
        {
            if (! rx->hasInfo || r->name.isEmpty() || now - rx->lastPacket > 5000.0) continue;
            StreamInfo s;
            s.stream = rx->stream;
            s.sender = r->name;
            s.address = ipText (ipOf (r->from));
            s.codec = rx->codec;
            s.channels = rx->channels;
            s.packetFrames = rx->frames;
            s.kbps = rx->kbps;
            s.rate = rx->rate;
            s.used = rx->sub != nullptr;
            s.lan = isLanIp (ipOf (r->from));
            s.secondsAgo = (now - rx->lastPacket) / 1000.0;
            auto same = std::find_if (list.begin(), list.end(), [&] (const StreamInfo& o) { return o.source().toDeviceName().equalsIgnoreCase (s.source().toDeviceName()); });
            if (same == list.end()) list.push_back (s);
            else if (s.secondsAgo < same->secondsAgo || s.used) *same = s;
        }
    std::sort (list.begin(), list.end(), [] (const StreamInfo& a, const StreamInfo& b)
    {
        const int c = a.sender.compareNatural (b.sender);
        return c != 0 ? c < 0 : a.stream.compareNatural (b.stream) < 0;
    });
    return list;
}

StreamStats Hub::getStats (const Source& s) const
{
    StreamStats st;
    std::lock_guard<std::mutex> l (impl->lock);
    for (auto* in : impl->ins)
        if (in->source.stream.equalsIgnoreCase (s.stream) && in->source.sender.equalsIgnoreCase (s.sender) && in->bound != nullptr)
        {
            st.received = in->bound->received;
            st.lost = in->bound->lost;
            st.late = in->bound->late;
            st.jitterMs = in->bound->jitter * 1000.0;
        }
    return st;
}

juce::StringArray Hub::localAddresses()
{
    juce::StringArray list;
    for (auto& a : Impl::interfaceAddresses()) list.addIfNotAlreadyThere (ipText (a.first));
    return list;
}

juce::String Hub::computerName()
{
    wchar_t buf[256] {};
    DWORD n = 256;
    if (GetComputerNameExW (ComputerNamePhysicalDnsHostname, buf, &n) && n > 0) return juce::String (buf);
    return juce::SystemStats::getComputerName();
}

bool Hub::isLocalAddress (const juce::String& ipv4)
{
    uint32_t ip = 0;
    return parseIp (ipv4, ip) && isLanIp (ip);
}

std::shared_ptr<AsyncInput> Hub::openInput (const IoTarget& t, double engineRate, int engineBlock, juce::String& error)
{
    const auto src = Source::parse (t.deviceName);
    if (src.stream.isEmpty() || src.sender.isEmpty()) { error = "No network stream chosen"; return nullptr; }
    std::lock_guard<std::mutex> l (impl->lock);
    if (! impl->running) { error = "Hydra Link is off (Settings > Network)"; return nullptr; }

    // The stream's format if it is heard right now (otherwise assume the engine's rate; a different one later
    // makes the input reopen with it).
    double rate = engineRate;
    int frames = 256;
    bool lan = true, opus = false;
    double newest = -1.0;
    for (auto& [inst, r] : impl->remotes)
        if (r->name.equalsIgnoreCase (src.sender))
            for (auto& [id, rx] : r->rx)
                if (rx->hasInfo && rx->stream.equalsIgnoreCase (src.stream) && rx->lastPacket > newest)
                {
                    newest = rx->lastPacket;
                    rate = rx->rate;
                    frames = rx->frames;
                    lan = isLanIp (ipOf (r->from));
                    opus = rx->codec == Codec::opus;
                }

    auto in = std::make_shared<NetInput> (*impl, src);
    in->prepare (rate, frames, lan, opus, engineRate, engineBlock);
    in->target = t;
    impl->ins.push_back (in.get());
    impl->bindInputs (nowMs());
    return in;
}

std::shared_ptr<AsyncOutput> Hub::openOutput (const IoTarget& t, double engineRate, int engineBlock, juce::String& error)
{
    const auto d = Destination::parse (t.deviceName);
    if (d.host.isEmpty()) { error = "No destination chosen"; return nullptr; }

    uint32_t ip = 0;
    int port = d.port;
    bool byName = false, encrypted = false;
    {
        std::lock_guard<std::mutex> l (impl->lock);
        if (! impl->running) { error = "Hydra Link is off (Settings > Network)"; return nullptr; }
        encrypted = impl->keys.isOn();
        // A Hydra by its name (as it announces itself): its current address.
        double newest = -1.0;
        for (auto& [inst, r] : impl->remotes)
            if (r->announced && r->name.equalsIgnoreCase (d.host) && r->lastSeen > newest)
            {
                newest = r->lastSeen;
                ip = ipOf (r->from);
                port = r->port > 0 ? r->port : d.port;
                byName = true;
            }
    }
    if (! byName && ! resolveHost (d.host, ip))
    {
        error = d.host + " is not on the network right now";
        return nullptr;
    }
    const bool lan = isLanIp (ip);
    if (! lan && ! encrypted)
    {
        error = "Outside the local network Hydra Link needs a network password (Settings > Network) on both computers";
        return nullptr;
    }

    auto out = std::make_shared<NetOutput> (*impl);
    if (! out->prepare (d, ip, port, byName, lan, engineRate, engineBlock, error)) return nullptr;
    out->target = t;
    std::lock_guard<std::mutex> l (impl->lock);
    impl->outs.push_back (out.get());
    impl->wakeUp();
    return out;
}
} // namespace hydra::net

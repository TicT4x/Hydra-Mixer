#include "LinkLite.h"
#include "core/Resampler.h"
#include <opus.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <thread>

#ifdef _WIN32
 #include <winsock2.h>
 #include <ws2tcpip.h>
 using SocketHandle = SOCKET;
 static constexpr SocketHandle kNoSocket = INVALID_SOCKET;
 static int closeSocket (SocketHandle s) { return closesocket (s); }
 static int pollSockets (WSAPOLLFD* f, int n, int ms) { return WSAPoll (f, (ULONG) n, ms); }
 using PollFd = WSAPOLLFD;
#else
 #include <arpa/inet.h>
 #include <fcntl.h>
 #include <ifaddrs.h>
 #include <net/if.h>
 #include <netinet/in.h>
 #include <poll.h>
 #include <sys/socket.h>
 #include <unistd.h>
 using SocketHandle = int;
 static constexpr SocketHandle kNoSocket = -1;
 static int closeSocket (SocketHandle s) { return close (s); }
 static int pollSockets (pollfd* f, int n, int ms) { return poll (f, (nfds_t) n, ms); }
 using PollFd = pollfd;
#endif

namespace hydra::lite
{
namespace
{
// Packet format v1: see the comment at the top of engine/NetLink.cpp (must stay identical).
constexpr uint8_t kVersion = 1;
enum PacketType : uint8_t { tAudio = 1, tInfo = 2, tAnnounce = 3, tFeedback = 4, tBye = 5, tPair = 6 };
enum PairOp : uint8_t { opRequest = 1, opAnswer = 2, opUnpair = 3 };
enum PairResult : uint8_t { resDenied = 0, resAllowed = 1, resWaiting = 2 };
constexpr int kHeader = 24, kAudioHeader = 16, kTagBytes = 16;
constexpr int kMaxPacket = 1200;
constexpr int kMaxPayload = kMaxPacket - kHeader - kAudioHeader - kTagBytes;
constexpr int kMaxName = 63;
constexpr int kHold = 4;
constexpr int kOpusFrames = 240;   // 5 ms
constexpr int kMaxChunk = 4096;

double nowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli> (steady_clock::now().time_since_epoch()).count();
}

uint64_t random64()
{
    static std::mt19937_64 gen { std::random_device{}() ^ (uint64_t) std::chrono::high_resolution_clock::now().time_since_epoch().count() };
    static std::mutex m;
    std::lock_guard<std::mutex> l (m);
    return gen();
}

std::string cleanName (std::string s)
{
    std::string t;
    for (char c : s) if (std::strchr ("@/?&\r\n\t", c) == nullptr) t += c;
    while (! t.empty() && (t.front() == ' ')) t.erase (t.begin());
    while (! t.empty() && (t.back() == ' ')) t.pop_back();
    while ((int) t.size() > kMaxName)
    {
        t.pop_back();
        while (! t.empty() && ((uint8_t) t.back() & 0xC0) == 0x80) t.pop_back();   // never cut a UTF-8 sequence
        if (! t.empty() && ((uint8_t) t.back() & 0x80) != 0) t.pop_back();
    }
    return t;
}

bool sameName (const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower ((unsigned char) a[i]) != std::tolower ((unsigned char) b[i])) return false;
    return true;
}

bool isLanIp (uint32_t ip)
{
    return (ip >> 24) == 10 || (ip >> 24) == 127
        || (ip & 0xFFF00000u) == 0xAC100000u
        || (ip & 0xFFFF0000u) == 0xC0A80000u
        || (ip & 0xFFFF0000u) == 0xA9FE0000u;
}

std::string ipText (uint32_t ip)
{
    return std::to_string ((ip >> 24) & 255) + "." + std::to_string ((ip >> 16) & 255) + "." + std::to_string ((ip >> 8) & 255) + "." + std::to_string (ip & 255);
}

bool parseIp (const std::string& s, uint32_t& ip)
{
    in_addr a {};
    if (inet_pton (AF_INET, s.c_str(), &a) != 1) return false;
    ip = ntohl (a.s_addr);
    return true;
}

sockaddr_in makeAddr (uint32_t ip, int port)
{
    sockaddr_in a {};
    a.sin_family = AF_INET;
    a.sin_port = htons ((uint16_t) port);
    a.sin_addr.s_addr = htonl (ip);
    return a;
}

void splitHostPort (const std::string& s, std::string& host, int& port)
{
    host = s;
    port = kDefaultPort;
    const auto colon = s.rfind (':');
    if (colon != std::string::npos && colon > 0 && s.find (':') == colon)
    {
        port = std::max (1, std::min (65535, std::atoi (s.c_str() + colon + 1)));
        host = s.substr (0, colon);
    }
}

int pcmBytes (Codec c) { return c == Codec::pcm16 ? 2 : c == Codec::pcm24 ? 3 : c == Codec::float32 ? 4 : 0; }

struct Writer
{
    uint8_t* p;
    int cap, n = 0;
    void u8 (uint32_t v)  { if (n < cap) p[n] = (uint8_t) v; ++n; }
    void u16 (uint32_t v) { u8 (v); u8 (v >> 8); }
    void u32 (uint32_t v) { u16 (v); u16 (v >> 16); }
    void u64 (uint64_t v) { u32 ((uint32_t) v); u32 ((uint32_t) (v >> 32)); }
    void str (const std::string& s)
    {
        const int len = std::min (kMaxName, (int) s.size());
        u8 ((uint32_t) len);
        for (int i = 0; i < len; ++i) u8 ((uint8_t) s[(size_t) i]);
    }
    bool ok() const { return n <= cap; }
};

struct Reader
{
    const uint8_t* p;
    int n, pos = 0;
    bool ok = true;
    uint32_t u8()  { if (pos >= n) { ok = false; return 0; } return p[pos++]; }
    uint32_t u16() { const uint32_t a = u8(); return a | (u8() << 8); }
    uint32_t u32() { const uint32_t a = u16(); return a | (u16() << 16); }
    uint64_t u64() { const uint64_t a = u32(); return a | ((uint64_t) u32() << 32); }
    std::string str()
    {
        const int len = (int) u8();
        if (! ok || pos + len > n) { ok = false; return {}; }
        std::string s ((const char*) p + pos, (size_t) len);
        pos += len;
        return s;
    }
    const uint8_t* rest() const { return p + pos; }
    int restBytes() const       { return n - pos; }
};

// PCM exactly as core/NetCodec.cpp.
void encodePcm (Codec codec, const float* const* in, int channels, int frames, uint8_t* out)
{
    static uint32_t seed = 0x1234567u;
    auto rnd = [] { seed = seed * 1664525u + 1013904223u; return (float) (seed >> 8) * (1.0f / 16777216.0f); };
    for (int i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c)
        {
            const float x = in[c][i];
            if (codec == Codec::pcm16)
            {
                const float d = rnd() - rnd();
                const int v = std::max (-32768, std::min (32767, (int) std::lrint (x * 32767.0f + d)));
                *out++ = (uint8_t) (v & 0xff);
                *out++ = (uint8_t) ((v >> 8) & 0xff);
            }
            else if (codec == Codec::pcm24)
            {
                const int v = std::max (-8388608, std::min (8388607, (int) std::lrint ((double) x * 8388607.0)));
                *out++ = (uint8_t) (v & 0xff);
                *out++ = (uint8_t) ((v >> 8) & 0xff);
                *out++ = (uint8_t) ((v >> 16) & 0xff);
            }
            else
            {
                std::memcpy (out, &x, 4);   // ARM and x86: little-endian
                out += 4;
            }
        }
}

void decodePcm (Codec codec, const uint8_t* in, int channels, int frames, float* const* out)
{
    for (int i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c)
        {
            float v = 0.0f;
            if (codec == Codec::pcm16)
            {
                v = (float) (int16_t) (uint16_t) (in[0] | (in[1] << 8)) * (1.0f / 32767.0f);
                in += 2;
            }
            else if (codec == Codec::pcm24)
            {
                int32_t s = (int32_t) ((uint32_t) in[0] | ((uint32_t) in[1] << 8) | ((uint32_t) in[2] << 16));
                if (s & 0x800000) s -= 0x1000000;
                v = (float) ((double) s * (1.0 / 8388607.0));
                in += 3;
            }
            else
            {
                std::memcpy (&v, in, 4);
                if (! std::isfinite (v)) v = 0.0f;
                in += 4;
            }
            out[c][i] = v;
        }
}

void maxInto (std::atomic<float>& a, float v)
{
    float cur = a.load (std::memory_order_relaxed);
    while (v > cur && ! a.compare_exchange_weak (cur, v, std::memory_order_relaxed)) {}
}

//==============================================================================
/** What the output callback plays from: ring at the sender's rate + resampler + drift controller. Built by the
    network thread, published to the audio thread (see Impl::publishRx). */
struct RxPath
{
    SpscAudioRing ring;
    VarResampler rs;
    DriftController drift;
    double senderRate = 48000, deviceRate = 48000, slip = 0;
    int burst = 192;
    bool primed = false;
    std::atomic<int> underruns { 0 }, latency { 0 };
    std::atomic<float> peak { 0 };
    std::vector<float> l, r;

    void prepare (double sRate, int packetFrames, double devRate, int devBurst)
    {
        senderRate = sRate;
        deviceRate = devRate;
        burst = std::max (16, std::min (kMaxChunk, devBurst));
        const double ratio = senderRate / deviceRate;   // sender frames per output frame
        const double perBlock = burst * ratio;
        // Wi-Fi: packets come in bursts, with more jitter than a cable (~6 ms; more after a dropout, see holdFloor).
        const double jitterFloor = packetFrames + senderRate * 0.006;
        const int devBlock = std::max (packetFrames, (int) std::ceil (senderRate * 0.006));
        ring.allocate (2, (int) (perBlock * 4 + devBlock * 8 + 16384 + senderRate * 0.5));
        rs.prepare (2, ratio, kMaxChunk);
        const double larger = std::max (perBlock, (double) devBlock), smaller = std::min (perBlock, (double) devBlock);
        const double frac = larger / smaller - std::floor (larger / smaller);
        slip = (frac < 0.25 || frac > 0.75) ? smaller : 0.0;
        drift.setHoldFloor (true);   // stability before latency (software over a network, as in Hydra)
        drift.prepare (std::max (perBlock + devBlock + 32, perBlock + slip + 32), std::max (perBlock + slip + 16, perBlock + jitterFloor),
                       perBlock + devBlock * 6 + 4096 + senderRate * 0.15, std::max (16.0, devBlock * 0.25),
                       deviceRate, burst, senderRate, std::max (4, (int) std::ceil (0.5 * deviceRate / burst)));
        l.assign (kMaxChunk, 0.0f);
        r.assign (kMaxChunk, 0.0f);
        primed = false;
    }

    /** As AsyncInput::pullBlock (engine/IoStreams.cpp), resampled path. */
    void pullChunk (float* out, int n, float gain) noexcept
    {
        float* p[2] = { l.data(), r.data() };
        const int avail = ring.available();
        const int need = rs.inputNeededFor (n);
        bool silent = false;
        if (! primed)
        {
            if (avail >= (int) drift.getTarget()) primed = true;
            else silent = true;
        }
        if (! silent)
        {
            if (avail > (int) drift.getTarget() + burst * 8 + 2048 + (int) (senderRate * 0.1))
            {
                ring.skip (avail - (int) drift.getTarget());
                underruns.fetch_add (1, std::memory_order_relaxed);
            }
            rs.setCorrection (drift.update ((double) ring.available(), need + slip, false));
            if (! rs.pull (ring, p, n))
            {
                underruns.fetch_add (1, std::memory_order_relaxed);
                drift.update ((double) ring.available(), need + slip, true);
                primed = false;
            }
            latency.store (ring.available() + rs.getLatencyFrames(), std::memory_order_relaxed);
        }
        float pk = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float a = silent ? 0.0f : l[(size_t) i] * gain, b = silent ? 0.0f : r[(size_t) i] * gain;
            out[2 * i] = a;
            out[2 * i + 1] = b;
            pk = std::max (pk, std::max (std::abs (a), std::abs (b)));
        }
        maxInto (peak, pk);
    }
};

/** What the microphone callback writes to. */
struct TxPath
{
    SpscAudioRing ring;
    int channels = 1;
    std::atomic<float> peak { 0 };
    std::vector<float> l, r;
    std::atomic<int> overflows { 0 };

    void prepare (int ch, double rate)
    {
        channels = std::max (1, std::min (2, ch));
        ring.allocate (channels, (int) (rate * 0.5));
        l.assign (kMaxChunk, 0.0f);
        r.assign (kMaxChunk, 0.0f);
    }
};

/** One stream of one Hydra. */
struct Rx
{
    uint32_t id = 0;
    std::string stream;
    Codec codec = Codec::pcm24;
    int channels = 2, frames = 0, kbps = 0;
    double rate = 0;
    bool hasInfo = false;
    double lastPacket = 0, lastInfo = 0;
};

struct Remote
{
    uint64_t instance = 0;
    std::string name;
    sockaddr_in from {};
    int port = 0, remotePort = 0;
    bool announced = false;
    double lastSeen = 0;
    uint64_t device = 0;
    std::map<uint32_t, Rx> rx;
};

/** Sequencing / concealment of the stream being listened to (as Hub::Impl::place ... conceal). */
struct Playback
{
    uint64_t instance = 0;
    uint32_t id = 0;
    Codec codec = Codec::pcm24;
    int channels = 2, frames = 0;
    double rate = 0;
    bool started = false;
    uint32_t expect = 0;
    struct Held { bool used = false; int bytes = 0; double at = 0; std::array<uint8_t, kMaxPacket> data {}; };
    std::array<Held, kHold> hold {};
    std::array<std::vector<float>, 2> out, last;
    bool lastValid = false, fadeIn = false;
    int lastFrames = 0;
    OpusDecoder* dec = nullptr;
    std::vector<float> inter;
    uint32_t received = 0, lost = 0, late = 0, fbReceived = 0, fbLost = 0, fbLate = 0;
    double jitter = 0, prevTransit = 0, lastFeedback = 0;
    bool haveTransit = false;

    ~Playback() { if (dec != nullptr) opus_decoder_destroy (dec); }

    void reset (const Rx& x, uint64_t inst)
    {
        instance = inst;
        id = x.id;
        codec = x.codec;
        channels = x.channels;
        frames = x.frames;
        rate = x.rate;
        started = false;
        for (auto& h : hold) h.used = false;
        lastValid = fadeIn = false;
        lastFrames = frames;
        haveTransit = false;
        for (int c = 0; c < 2; ++c) { out[(size_t) c].assign ((size_t) std::max (1, frames), 0.0f); last[(size_t) c].assign ((size_t) std::max (1, frames), 0.0f); }
        if (dec != nullptr) { opus_decoder_destroy (dec); dec = nullptr; }
        if (codec == Codec::opus)
        {
            int err = 0;
            dec = opus_decoder_create (48000, channels, &err);
            if (err != OPUS_OK) dec = nullptr;
            inter.assign ((size_t) (frames * channels), 0.0f);
        }
    }
    int heldCount() const { int n = 0; for (auto& h : hold) n += h.used ? 1 : 0; return n; }
    double packetMs() const { return 1000.0 * frames / std::max (8000.0, rate); }

    bool opusDecode (const uint8_t* data, int bytes, bool fec)
    {
        const int got = dec != nullptr ? opus_decode_float (dec, data, data != nullptr ? bytes : 0, inter.data(), frames, fec ? 1 : 0) : -1;
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < frames; ++i)
                out[(size_t) c][(size_t) i] = got == frames ? inter[(size_t) (i * channels + c)] : 0.0f;
        return got == frames;
    }
};
} // namespace

const char* codecName (Codec c)
{
    switch (c)
    {
        case Codec::pcm16:   return "PCM 16";
        case Codec::pcm24:   return "PCM 24";
        case Codec::float32: return "PCM 32f";
        case Codec::opus:    return "Opus";
        default:             return "Auto";
    }
}

//==============================================================================
struct Endpoint::Impl
{
    Endpoint& owner;
    explicit Impl (Endpoint& o) : owner (o) {}

    mutable std::mutex lock;   // everything below except the audio-thread atomics (network thread + control calls)
    Config cfg;
    std::string name;
    SocketHandle sock = kNoSocket;
    std::thread thread;
    std::atomic<bool> quit { false }, running { false };
    uint64_t instance = 0, counter = 0;
    std::map<uint64_t, Remote> remotes;
    std::vector<sockaddr_in> broadcasts, manualPeers;
    std::atomic<uint32_t> changes { 0 };
    double nextAnnounce = 0, lastIfScan = -1.0e9;
    std::string hint;
    bool wsa = false;

    // Pairing (as engine/NetLink.cpp)
    uint64_t myDevice = random64() | 1;
    struct Incoming { std::string name; sockaddr_in from {}; double lastAsked = 0; };
    struct Outgoing { std::string name; double started = 0, lastSent = -1.0e9; PairState state = PairState::none; };
    struct Code { int port = 0; std::string code; };
    std::map<uint64_t, std::string> trusted;
    std::map<uint64_t, Incoming> incoming;
    std::map<uint64_t, Outgoing> outgoing;
    std::map<uint64_t, double> denied;
    std::map<uint64_t, Code> codes;
    std::vector<std::pair<sockaddr_in, uint64_t>> unpairs;
    std::atomic<uint32_t> trustVersion { 0 };

    // Receive
    std::string wantStream, wantSender;
    std::unique_ptr<Playback> play;   // bound stream (network thread)
    std::atomic<RxPath*> rxLive { nullptr };
    std::atomic<bool> inPull { false };
    std::atomic<uint64_t> pullEpoch { 0 };
    std::vector<std::pair<RxPath*, uint64_t>> rxRetired;
    std::atomic<double> outRate { 48000.0 };
    std::atomic<int> outBurst { 192 };
    std::vector<float> scratchL, scratchR;

    // Send
    SendSettings send;
    bool sending = false;
    std::atomic<TxPath*> txLive { nullptr };
    std::atomic<bool> inPush { false };
    std::atomic<uint64_t> pushEpoch { 0 };
    std::vector<std::pair<TxPath*, uint64_t>> txRetired;
    sockaddr_in to {};
    bool toValid = false, byName = false;
    uint32_t streamId = 1, seq = 0, position = 0;
    Codec txCodec = Codec::pcm24;
    int txFrames = 240;
    double txRate = 48000, lastInfo = -1.0e9;
    int infoCount = 0;
    OpusEncoder* enc = nullptr;
    std::vector<float> encInter, txL, txR;
    double fbLoss = -1, fbBuffer = -1, fbAt = -1.0e9;

    ~Impl()
    {
        if (enc != nullptr) opus_encoder_destroy (enc);
    }

    //--------------------------------------------------------------------------
    bool start (const Config& c, std::string& error)
    {
       #ifdef _WIN32
        if (! wsa) { WSADATA d; wsa = WSAStartup (MAKEWORD (2, 2), &d) == 0; }
       #endif
        sock = socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == kNoSocket) { error = "Cannot create a network socket"; return false; }
        int yes = 1;
        setsockopt (sock, SOL_SOCKET, SO_BROADCAST, (const char*) &yes, sizeof (yes));
       #ifndef _WIN32
        setsockopt (sock, SOL_SOCKET, SO_REUSEADDR, (const char*) &yes, sizeof (yes));
        fcntl (sock, F_SETFL, fcntl (sock, F_GETFL, 0) | O_NONBLOCK);
       #else
        u_long nb = 1;
        ioctlsocket (sock, FIONBIO, &nb);
       #endif
        int buf = 1 << 20;
        setsockopt (sock, SOL_SOCKET, SO_RCVBUF, (const char*) &buf, sizeof (buf));
        auto a = makeAddr (c.loopbackOnly ? 0x7F000001u : 0u, c.port);
        if (bind (sock, (const sockaddr*) &a, sizeof (a)) != 0)
        {
            error = "Port " + std::to_string (c.port) + " is in use";
            closeSocket (sock);
            sock = kNoSocket;
            return false;
        }
        cfg = c;
        name = cleanName (c.name.empty() ? std::string ("Phone") : c.name);
        instance = random64();
        if (c.deviceId != 0) myDevice = c.deviceId;
        counter = 0;
        manualPeers.clear();
        for (auto& p : c.peers)
        {
            std::string host;
            int port = kDefaultPort;
            splitHostPort (p, host, port);
            uint32_t ip = 0;
            if (parseIp (host, ip)) manualPeers.push_back (makeAddr (ip, port));
        }
        nextAnnounce = 0;
        quit = false;
        running = true;
        thread = std::thread ([this] { run(); });
        return true;
    }

    void stop()
    {
        if (! running.load()) return;
        {
            std::lock_guard<std::mutex> l (lock);
            if (sending) sendBye();
        }
        quit = true;
        if (thread.joinable()) thread.join();
        running = false;
        closeSocket (sock);
        sock = kNoSocket;
        std::lock_guard<std::mutex> l (lock);
        remotes.clear();
        play.reset();
        retireRx();
        retireTx();
        freeRetired (true);
        ++changes;
    }

    //--------------------------------------------------------------------------
    template <typename Fill>
    void sendPacket (PacketType type, const sockaddr_in& dest, Fill&& fill)
    {
        if (sock == kNoSocket || dest.sin_family != AF_INET) return;
        uint8_t buf[kMaxPacket + 16];
        Writer w { buf, kMaxPacket - kTagBytes };
        w.u8 ('H'); w.u8 ('Y'); w.u8 ('L'); w.u8 ('K');
        w.u8 (kVersion); w.u8 (type); w.u8 (0); w.u8 (0);
        w.u64 (instance);
        w.u64 (++counter);
        fill (w);
        if (! w.ok()) return;
        sendto (sock, (const char*) buf, w.n, 0, (const sockaddr*) &dest, sizeof (dest));
    }

    void scanInterfaces()
    {
        broadcasts.clear();
       #ifndef _WIN32
        ifaddrs* list = nullptr;
        if (getifaddrs (&list) != 0) return;
        for (auto* i = list; i != nullptr; i = i->ifa_next)
        {
            if (i->ifa_addr == nullptr || i->ifa_addr->sa_family != AF_INET) continue;
            if ((i->ifa_flags & IFF_UP) == 0 || (i->ifa_flags & IFF_LOOPBACK) != 0) continue;
            const uint32_t ip = ntohl (((sockaddr_in*) i->ifa_addr)->sin_addr.s_addr);
            const uint32_t mask = i->ifa_netmask != nullptr ? ntohl (((sockaddr_in*) i->ifa_netmask)->sin_addr.s_addr) : 0xFFFFFF00u;
            if (! isLanIp (ip)) continue;   // mobile data: never
            broadcasts.push_back (makeAddr ((ip & mask) | ~mask, cfg.port));
        }
        freeifaddrs (list);
       #endif
    }

    void announce (double now)
    {
        if (now - lastIfScan > 10000.0) { scanInterfaces(); lastIfScan = now; }
        // remote port 0 (no web control on a phone), device id, kind 1 = phone
        auto fill = [this] (Writer& w) { w.u16 ((uint32_t) cfg.port); w.str (name); w.u16 (0); w.u64 (myDevice); w.u8 (1); };
        if (cfg.broadcast)
            for (auto& b : broadcasts)
            {
                sendPacket (tAnnounce, b, fill);
                if (cfg.port != kDefaultPort) sendPacket (tAnnounce, makeAddr (ntohl (b.sin_addr.s_addr), kDefaultPort), fill);
            }
        for (auto& p : manualPeers) sendPacket (tAnnounce, p, fill);
    }

    //--------------------------------------------------------------------------
    void run()
    {
        std::vector<uint8_t> buf (2048);
        while (! quit.load())
        {
            PollFd f {};
            f.fd = sock;
            f.events = POLLIN;
            int timeout = sending ? 1 : 20;
            pollSockets (&f, 1, timeout);
            std::lock_guard<std::mutex> l (lock);
            const double now = nowMs();
            for (;;)
            {
                sockaddr_in from {};
                socklen_t len = sizeof (from);
                const int n = (int) recvfrom (sock, (char*) buf.data(), (int) buf.size(), 0, (sockaddr*) &from, &len);
                if (n <= 0) break;
                handle (buf.data(), n, from, now);
            }
            flushHeld (now);
            housekeeping (now);
            serviceTx();
        }
    }

    void handle (uint8_t* b, int n, const sockaddr_in& from, double now)
    {
        if (n < kHeader + 2 || b[0] != 'H' || b[1] != 'Y' || b[2] != 'L' || b[3] != 'K') return;
        if (b[4] != kVersion) { hint = ipText (ntohl (from.sin_addr.s_addr)) + " uses another version of Hydra Link"; return; }
        Reader h { b, kHeader };
        h.pos = 5;
        const uint8_t type = (uint8_t) h.u8(), flags = (uint8_t) h.u8();
        h.u8();
        const uint64_t inst = h.u64();
        h.u64();
        if (inst == instance) return;
        if ((flags & 1) != 0) { hint = "Hydra uses a network password: the app does not support passwords yet (remove it in Hydra under Settings > Network)"; return; }
        if (! isLanIp (ntohl (from.sin_addr.s_addr))) return;

        auto& r = remotes[inst];
        if (r.instance == 0) { r.instance = inst; ++changes; }
        r.from = from;
        r.lastSeen = now;
        Reader p { b + kHeader, n - kHeader };
        switch (type)
        {
            case tAnnounce:
            {
                const int port = (int) p.u16();
                const auto nm = cleanName (p.str());
                if (! p.ok || nm.empty()) return;
                const int remote = p.restBytes() >= 2 ? (int) p.u16() : 0;   // Hydra Remote's port (since 9.11.)
                const uint64_t dev = p.restBytes() >= 9 ? p.u64() : 0;      // device id (since 10.11.)
                if (! r.announced || r.name != nm || r.port != port || r.remotePort != remote || r.device != dev) ++changes;
                r.announced = true;
                r.name = nm;
                r.port = port;
                r.remotePort = remote;
                r.device = dev;
                break;
            }
            case tPair:     onPair (r, p, now); break;
            case tInfo:     onInfo (r, p, now); break;
            case tAudio:    onAudio (r, p, now); break;
            case tFeedback: onFeedback (p); break;
            case tBye:
            {
                const uint32_t id = p.u32();
                if (! p.ok) return;
                if (play != nullptr && play->instance == inst && play->id == id) unbind();
                if (r.rx.erase (id) > 0) ++changes;
                break;
            }
            default: break;
        }
    }

    //--------------------------------------------------------------------------
    // Pairing
    bool trusts (const Remote& r) const { return ! cfg.pairing || (r.device != 0 && trusted.count (r.device) > 0); }

    template <typename Extra>
    void sendPair (const sockaddr_in& dest, PairOp op, uint64_t toDevice, Extra&& extra)
    {
        sendPacket (tPair, dest, [&] (Writer& w) { w.u8 (op); w.u64 (myDevice); w.u64 (toDevice); w.u8 (1); w.str (name); extra (w); });
    }

    void onPair (Remote& r, Reader& b, double now)
    {
        const auto op = (uint8_t) b.u8();
        const uint64_t fromDev = b.u64(), toDev = b.u64();
        b.u8();   // kind
        const auto who = cleanName (b.str());
        if (! b.ok || fromDev == 0 || toDev != myDevice) return;
        if (r.device != fromDev) { r.device = fromDev; ++changes; }
        if (op == opRequest)
        {
            uint8_t result = resWaiting;
            if (! cfg.pairing) result = resAllowed;
            else if (auto t = trusted.find (fromDev); t != trusted.end())
            {
                result = resAllowed;
                if (! who.empty() && t->second != who) { t->second = who; ++trustVersion; }
            }
            else if (auto d = denied.find (fromDev); d != denied.end() && now < d->second) result = resDenied;
            else
            {
                auto& in = incoming[fromDev];
                if (in.lastAsked == 0 || in.name != who) ++changes;
                in.name = who.empty() ? std::string ("Hydra") : who;
                in.from = r.from;
                in.lastAsked = now;
            }
            sendPair (r.from, opAnswer, fromDev, [&] (Writer& w) { w.u8 (result); w.u16 (0); w.str (std::string()); });
        }
        else if (op == opAnswer)
        {
            const auto result = (uint8_t) b.u8();
            const int rport = (int) b.u16();
            const auto code = b.str();
            if (! b.ok) return;
            auto it = outgoing.find (fromDev);
            if (it == outgoing.end() || it->second.state != PairState::asking) return;
            if (result == resAllowed)
            {
                if (trusted.count (fromDev) == 0 || trusted[fromDev] != who) ++trustVersion;
                trusted[fromDev] = who.empty() ? it->second.name : who;
                it->second.state = PairState::allowed;
                if (rport > 0 && ! code.empty()) codes[fromDev] = { rport, code };
                bindIfWanted (now);
            }
            else if (result == resDenied) it->second.state = PairState::denied;
            ++changes;
        }
        else if (op == opUnpair)
        {
            if (trusted.erase (fromDev) > 0) { ++trustVersion; dropUntrusted(); }
            outgoing.erase (fromDev);
            codes.erase (fromDev);
            ++changes;
        }
    }

    void dropUntrusted()
    {
        if (play == nullptr) return;
        auto rit = remotes.find (play->instance);
        if (rit == remotes.end() || ! trusts (rit->second)) unbind();
    }

    Remote* remoteByName (const std::string& n, double now)
    {
        Remote* best = nullptr;
        for (auto& [inst, r] : remotes)
            if (r.announced && r.device != 0 && sameName (r.name, n) && now - r.lastSeen < 10000.0 && (best == nullptr || r.lastSeen > best->lastSeen)) best = &r;
        return best;
    }

    Remote* remoteByDevice (uint64_t dev, double now)
    {
        Remote* best = nullptr;
        for (auto& [inst, r] : remotes)
            if (r.device == dev && now - r.lastSeen < 10000.0 && (best == nullptr || r.lastSeen > best->lastSeen)) best = &r;
        return best;
    }

    void startRequest (const Remote& r, double now)
    {
        auto& o = outgoing[r.device];
        o.name = r.name;
        o.started = now;
        o.lastSent = -1.0e9;
        o.state = PairState::asking;
        ++changes;
    }

    void pairingHousekeeping (double now)
    {
        for (auto& [dest, dev] : unpairs) sendPair (dest, opUnpair, dev, [] (Writer&) {});
        unpairs.clear();
        if (! cfg.pairing) return;
        // Sending to a Hydra that is not paired: ask it.
        if (sending)
        {
            Remote* r = byName ? remoteByName (send.host, now) : nullptr;
            if (! byName && toValid)
                for (auto& [inst, x] : remotes)
                    if (x.announced && x.device != 0 && x.from.sin_addr.s_addr == to.sin_addr.s_addr && now - x.lastSeen < 10000.0) r = &x;
            if (r != nullptr && ! trusts (*r))
            {
                auto it = outgoing.find (r->device);
                if (it == outgoing.end() || (it->second.state != PairState::asking && now - it->second.started > 600000.0)) startRequest (*r, now);
            }
        }
        for (auto& [dev, o] : outgoing)
        {
            if (o.state != PairState::asking) continue;
            if (now - o.started > 120000.0) { o.state = PairState::noAnswer; ++changes; continue; }
            if (now - o.lastSent < 1000.0) continue;
            if (auto* r = remoteByDevice (dev, now)) { o.lastSent = now; sendPair (r->from, opRequest, dev, [] (Writer&) {}); }
        }
        for (auto it = incoming.begin(); it != incoming.end();)
            if (now - it->second.lastAsked > 10000.0) { it = incoming.erase (it); ++changes; } else ++it;
        for (auto it = denied.begin(); it != denied.end();)
            if (now > it->second) it = denied.erase (it); else ++it;
    }

    void onInfo (Remote& r, Reader& b, double now)
    {
        const uint32_t id = b.u32();
        const auto codec = (Codec) b.u8();
        const int ch = (int) b.u8(), frames = (int) b.u16();
        const double rate = (double) b.u32();
        const int kbps = (int) b.u16();
        const auto stream = cleanName (b.str()), sender = cleanName (b.str());
        if (! b.ok || stream.empty() || sender.empty() || ch < 1 || ch > 2 || rate < 8000 || rate > 384000) return;
        if (codec == Codec::opus ? (frames != 120 && frames != 240 && frames != 480 && frames != 960)
                                 : (pcmBytes (codec) == 0 || frames < 16 || frames * ch * pcmBytes (codec) > kMaxPayload)) return;
        if (r.rx.size() >= 32 && r.rx.count (id) == 0) return;
        auto& x = r.rx[id];
        const bool fresh = ! x.hasInfo;
        const bool formatChanged = ! x.hasInfo || x.codec != codec || x.channels != ch || x.frames != frames || x.rate != rate;
        if (fresh || x.stream != stream || r.name != sender) ++changes;
        x.id = id;
        x.stream = stream;
        x.codec = codec;
        x.channels = ch;
        x.frames = frames;
        x.rate = rate;
        x.kbps = kbps;
        x.hasInfo = true;
        x.lastInfo = now;
        if (x.lastPacket == 0) x.lastPacket = now;
        r.name = sender;

        if (play != nullptr && play->instance == r.instance && play->id == id && formatChanged) bindTo (r, x);
        bindIfWanted (now);
    }

    void onAudio (Remote& r, Reader& b, double now)
    {
        const uint32_t id = b.u32(), sq = b.u32(), pos = b.u32();
        const auto codec = (Codec) b.u8();
        const int ch = (int) b.u8(), frames = (int) b.u16();
        if (! b.ok) return;
        auto it = r.rx.find (id);
        if (it == r.rx.end()) { if (r.rx.size() < 32) { r.rx[id].id = id; r.rx[id].lastPacket = now; } return; }
        auto& x = it->second;
        x.lastPacket = now;
        if (play == nullptr || play->instance != r.instance || play->id != id) return;
        auto& pb = *play;
        if (! x.hasInfo || codec != pb.codec || ch != pb.channels || (codec == Codec::opus ? frames != pb.frames : (frames < 1 || frames > pb.frames))) return;
        ++pb.received;
        const double transit = now * 0.001 - (double) pos / pb.rate;
        if (pb.haveTransit)
        {
            double d = std::abs (transit - pb.prevTransit);
            if (d > 1.0) d = 0.0;
            pb.jitter += (d - pb.jitter) / 16.0;
        }
        pb.prevTransit = transit;
        pb.haveTransit = true;
        place (pb, sq, b.rest(), b.restBytes(), now);
    }

    void onFeedback (Reader& b)
    {
        const uint32_t id = b.u32(), received = b.u32(), lost = b.u32(), late = b.u32(), jitterUs = b.u32(), bufferUs = b.u32();
        (void) late; (void) jitterUs;
        if (! b.ok || ! sending || id != streamId) return;
        const double total = (double) received + (double) lost;
        fbLoss = total > 0 ? 100.0 * lost / total : 0.0;
        fbBuffer = bufferUs / 1000.0;
        fbAt = nowMs();
    }

    //--------------------------------------------------------------------------
    // Receive: binding by name, sequencing, concealment (as engine/NetLink.cpp)
    void bindIfWanted (double now)
    {
        if (wantStream.empty()) return;
        if (play != nullptr)
        {
            auto rit = remotes.find (play->instance);
            if (rit != remotes.end())
                if (auto x = rit->second.rx.find (play->id); x != rit->second.rx.end() && now - x->second.lastPacket < 1000.0) return;
        }
        Remote* bestR = nullptr;
        Rx* best = nullptr;
        for (auto& [inst, r] : remotes)
            for (auto& [id, x] : r.rx)
                if (x.hasInfo && (wantStream == "*" || sameName (x.stream, wantStream)) && sameName (r.name, wantSender) && trusts (r) && now - x.lastPacket < 3000.0)
                    if (best == nullptr || x.lastPacket > best->lastPacket) { best = &x; bestR = &r; }
        if (best != nullptr && (play == nullptr || play->instance != bestR->instance || play->id != best->id)) bindTo (*bestR, *best);
    }

    void bindTo (Remote& r, const Rx& x)
    {
        if (play == nullptr) play = std::make_unique<Playback>();
        play->reset (x, r.instance);
        auto* p = new RxPath();
        p->prepare (x.rate, x.frames, outRate.load(), outBurst.load());
        publishRx (p);
        ++changes;
    }

    void unbind()
    {
        play.reset();
        publishRx (nullptr);
        ++changes;
    }

    void publishRx (RxPath* p)
    {
        auto* old = rxLive.exchange (p, std::memory_order_seq_cst);
        if (old != nullptr) rxRetired.push_back ({ old, pullEpoch.load() });
    }
    void retireRx() { publishRx (nullptr); }
    void retireTx()
    {
        auto* old = txLive.exchange (nullptr, std::memory_order_seq_cst);
        if (old != nullptr) txRetired.push_back ({ old, pushEpoch.load() });
    }

    /** Frees retired paths once the audio thread can no longer hold them. */
    void freeRetired (bool all)
    {
        for (auto it = rxRetired.begin(); it != rxRetired.end();)
            if (all || ! inPull.load() || pullEpoch.load() != it->second) { delete it->first; it = rxRetired.erase (it); }
            else ++it;
        for (auto it = txRetired.begin(); it != txRetired.end();)
            if (all || ! inPush.load() || pushEpoch.load() != it->second) { delete it->first; it = txRetired.erase (it); }
            else ++it;
    }

    void deliver (const float* const* o, int ch, int n)
    {
        if (auto* p = rxLive.load (std::memory_order_acquire)) p->ring.write (o, ch, n);
    }

    void place (Playback& x, uint32_t sq, const uint8_t* data, int bytes, double now)
    {
        if (bytes > kMaxPacket) return;
        if (! x.started) { x.started = true; x.expect = sq; }
        int32_t d = (int32_t) (sq - x.expect);
        if (d < 0) { ++x.late; return; }
        if (d > 32)
        {
            x.lost += (uint32_t) d;
            for (auto& h : x.hold) h.used = false;
            x.expect = sq;
            x.lastValid = false;
            x.fadeIn = true;
            d = 0;
        }
        while (d >= kHold) { takeFirst (x); --d; }
        auto& slot = x.hold[(size_t) d];
        if (slot.used) { ++x.late; return; }
        slot.used = true;
        slot.bytes = bytes;
        slot.at = now;
        std::memcpy (slot.data.data(), data, (size_t) bytes);
        drain (x);
        if (! x.hold[0].used && x.heldCount() >= (x.codec == Codec::opus ? 3 : 2)) { takeFirst (x); drain (x); }
    }

    void advance (Playback& x)
    {
        ++x.expect;
        for (int k = 0; k + 1 < kHold; ++k) std::swap (x.hold[(size_t) k], x.hold[(size_t) k + 1]);
        x.hold[kHold - 1].used = false;
    }
    void drain (Playback& x)
    {
        while (x.hold[0].used) { playPacket (x, x.hold[0].data.data(), x.hold[0].bytes); x.hold[0].used = false; advance (x); }
    }
    void takeFirst (Playback& x)
    {
        if (x.hold[0].used) { playPacket (x, x.hold[0].data.data(), x.hold[0].bytes); x.hold[0].used = false; }
        else conceal (x);
        advance (x);
    }
    void flushHeld (double now)
    {
        if (play == nullptr) return;
        auto& x = *play;
        if (x.hold[0].used || x.heldCount() == 0) return;
        double oldest = 1.0e18;
        for (auto& h : x.hold) if (h.used) oldest = std::min (oldest, h.at);
        if (now - oldest >= x.packetMs() * 0.5)
            while (x.heldCount() > 0 && ! x.hold[0].used) { takeFirst (x); drain (x); }
    }

    void playPacket (Playback& x, const uint8_t* data, int bytes)
    {
        float* o[2] = { x.out[0].data(), x.out[1].data() };
        bool ok = false;
        int n = x.frames;
        if (x.codec == Codec::opus) ok = x.opusDecode (data, bytes, false);
        else if (const int bpf = x.channels * pcmBytes (x.codec); bpf > 0 && bytes > 0 && bytes % bpf == 0 && bytes / bpf <= x.frames)
        {
            n = bytes / bpf;
            decodePcm (x.codec, data, x.channels, n, o);
            ok = true;
        }
        if (! ok) { conceal (x); return; }
        if (x.fadeIn && x.codec != Codec::opus)
        {
            const int fade = std::min (64, n);
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
        deliver (o, x.channels, n);
    }

    void conceal (Playback& x)
    {
        ++x.lost;
        float* o[2] = { x.out[0].data(), x.out[1].data() };
        int n = x.frames;
        if (x.codec == Codec::opus)
        {
            if (x.hold[1].used) x.opusDecode (x.hold[1].data.data(), x.hold[1].bytes, true);
            else                x.opusDecode (nullptr, 0, false);
        }
        else
        {
            n = std::max (1, std::min (x.frames, x.lastFrames > 0 ? x.lastFrames : x.frames));
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
        deliver (o, x.channels, n);
    }

    //--------------------------------------------------------------------------
    void housekeeping (double now)
    {
        if (now >= nextAnnounce) { announce (now); nextAnnounce = now + 2000.0; }
        pairingHousekeeping (now);

        // Hydras / streams not heard for a while are gone.
        for (auto it = remotes.begin(); it != remotes.end();)
        {
            auto& r = it->second;
            for (auto x = r.rx.begin(); x != r.rx.end();)
                if (now - x->second.lastPacket > 10000.0 && now - x->second.lastInfo > 10000.0) { x = r.rx.erase (x); ++changes; }
                else ++x;
            if (now - r.lastSeen > 15000.0)
            {
                if (play != nullptr && play->instance == r.instance) unbind();
                it = remotes.erase (it);
                ++changes;
            }
            else ++it;
        }
        bindIfWanted (now);

        // Feedback to the sender of the stream we play, once a second.
        if (play != nullptr && now - play->lastFeedback >= 1000.0)
        {
            auto& x = *play;
            x.lastFeedback = now;
            auto rit = remotes.find (x.instance);
            if (rit != remotes.end())
            {
                const uint32_t dr = x.received - x.fbReceived, dl = x.lost - x.fbLost, dlate = x.late - x.fbLate;
                x.fbReceived = x.received; x.fbLost = x.lost; x.fbLate = x.late;
                uint32_t bufferUs = 0;
                if (auto* p = rxLive.load()) bufferUs = (uint32_t) (1.0e6 * p->latency.load() / std::max (8000.0, p->senderRate));
                const uint32_t jitterUs = (uint32_t) (x.jitter * 1.0e6);
                sendPacket (tFeedback, rit->second.from, [&] (Writer& w) { w.u32 (x.id); w.u32 (dr); w.u32 (dl); w.u32 (dlate); w.u32 (jitterUs); w.u32 (bufferUs); });
            }
        }

        if (sending)
        {
            // Sending to a Hydra by its name: follow it to its current address.
            if (byName)
                for (auto& [inst, r] : remotes)
                    if (r.announced && sameName (r.name, send.host) && now - r.lastSeen < 6000.0)
                    {
                        to = makeAddr (ntohl (r.from.sin_addr.s_addr), r.port > 0 ? r.port : send.port);
                        toValid = true;
                    }
            if (toValid && now - lastInfo >= (infoCount < 5 ? 200.0 : 1000.0))
            {
                lastInfo = now;
                ++infoCount;
                sendPacket (tInfo, to, [this] (Writer& w)
                {
                    w.u32 (streamId);
                    w.u8 ((uint8_t) txCodec);
                    w.u8 ((uint32_t) send.channels);
                    w.u16 ((uint32_t) txFrames);
                    w.u32 ((uint32_t) std::lround (txRate));
                    w.u16 ((uint32_t) (txCodec == Codec::opus ? send.kbps : 0));
                    w.str (send.stream);
                    w.str (name);
                });
            }
        }
        freeRetired (false);
    }

    void serviceTx()
    {
        auto* p = txLive.load (std::memory_order_acquire);
        if (! sending || p == nullptr) return;
        if (! toValid) { p->ring.skip (p->ring.available()); return; }   // nobody to send to yet: do not pile up
        float* planar[2] = { txL.data(), txR.data() };
        uint8_t payload[kMaxPayload];
        auto sendAudio = [&] (const uint8_t* data, int bytes, int n)
        {
            sendPacket (tAudio, to, [&] (Writer& w)
            {
                w.u32 (streamId); w.u32 (seq); w.u32 (position);
                w.u8 ((uint8_t) txCodec); w.u8 ((uint32_t) send.channels); w.u16 ((uint32_t) n);
                for (int i = 0; i < bytes; ++i) w.u8 (data[i]);
            });
            ++seq;
            position += (uint32_t) n;
        };
        if (txCodec != Codec::opus)
        {
            while (const int avail = p->ring.available())
            {
                const int n = std::min (avail, txFrames);
                p->ring.read (planar, send.channels, n);
                encodePcm (txCodec, planar, send.channels, n, payload);
                sendAudio (payload, n * send.channels * pcmBytes (txCodec), n);
            }
            return;
        }
        while (p->ring.available() >= txFrames)
        {
            p->ring.read (planar, send.channels, txFrames);
            for (int i = 0; i < txFrames; ++i)
                for (int c = 0; c < send.channels; ++c) encInter[(size_t) (i * send.channels + c)] = planar[c][i];
            const int bytes = enc != nullptr ? opus_encode_float (enc, encInter.data(), txFrames, payload, kMaxPayload) : -1;
            if (bytes > 0) sendAudio (payload, bytes, txFrames);
        }
    }

    void sendBye()
    {
        if (toValid) sendPacket (tBye, to, [this] (Writer& w) { w.u32 (streamId); });
    }
};

//==============================================================================
Endpoint::Endpoint() : impl (std::make_unique<Impl> (*this)) {}
Endpoint::~Endpoint() { stop(); }

bool Endpoint::start (const Config& c, std::string& error)
{
    stop();
    return impl->start (c, error);
}
void Endpoint::stop() { impl->stop(); }
bool Endpoint::isRunning() const noexcept { return impl->running.load(); }
std::string Endpoint::getName() const { std::lock_guard<std::mutex> l (impl->lock); return impl->name; }
uint32_t Endpoint::getChangeCount() const noexcept { return impl->changes.load(); }

std::vector<Peer> Endpoint::getPeers() const
{
    std::lock_guard<std::mutex> l (impl->lock);
    std::vector<Peer> out;
    const double now = nowMs();
    for (auto& [inst, r] : impl->remotes)
        if (r.announced)
        {
            // A Hydra that restarted is heard under a new instance: list each name once (the newest).
            auto same = std::find_if (out.begin(), out.end(), [&] (const Peer& p) { return sameName (p.name, r.name); });
            Peer p { r.name, ipText (ntohl (r.from.sin_addr.s_addr)), r.port, r.remotePort, (now - r.lastSeen) / 1000.0 };
            p.deviceId = r.device;
            p.trusted = impl->trusts (r);
            if (auto o = impl->outgoing.find (r.device); o != impl->outgoing.end()) p.pairing = o->second.state;
            if (auto c = impl->codes.find (r.device); c != impl->codes.end() && c->second.port == r.remotePort) p.remoteCode = c->second.code;
            if (same == out.end()) out.push_back (p);
            else if (p.secondsAgo < same->secondsAgo) *same = p;
        }
    return out;
}

void Endpoint::setTrusted (const std::vector<Trusted>& list)
{
    std::lock_guard<std::mutex> l (impl->lock);
    impl->trusted.clear();
    for (auto& t : list) if (t.id != 0) impl->trusted[t.id] = t.name;
    ++impl->changes;
}

std::vector<Trusted> Endpoint::getTrusted() const
{
    std::lock_guard<std::mutex> l (impl->lock);
    std::vector<Trusted> out;
    for (auto& [id, n] : impl->trusted) out.push_back ({ id, n });
    return out;
}

uint32_t Endpoint::getTrustVersion() const noexcept { return impl->trustVersion.load(); }

std::vector<PairRequest> Endpoint::getRequests() const
{
    std::lock_guard<std::mutex> l (impl->lock);
    std::vector<PairRequest> out;
    for (auto& [id, in] : impl->incoming) out.push_back ({ id, in.name, ipText (ntohl (in.from.sin_addr.s_addr)) });
    return out;
}

void Endpoint::answer (uint64_t id, bool allow)
{
    std::lock_guard<std::mutex> l (impl->lock);
    auto it = impl->incoming.find (id);
    if (it == impl->incoming.end()) return;
    if (allow)
    {
        impl->trusted[id] = it->second.name;
        ++impl->trustVersion;
        impl->denied.erase (id);
    }
    else impl->denied[id] = nowMs() + 300000.0;
    impl->incoming.erase (it);
    ++impl->changes;
}

void Endpoint::requestPairing (const std::string& hydraName)
{
    std::lock_guard<std::mutex> l (impl->lock);
    const double now = nowMs();
    if (auto* r = impl->remoteByName (hydraName, now)) impl->startRequest (*r, now);
}

void Endpoint::forget (uint64_t id)
{
    std::lock_guard<std::mutex> l (impl->lock);
    if (impl->trusted.erase (id) > 0) ++impl->trustVersion;
    impl->outgoing.erase (id);
    impl->codes.erase (id);
    if (auto* r = impl->remoteByDevice (id, nowMs())) impl->unpairs.push_back ({ r->from, id });
    impl->dropUntrusted();
    ++impl->changes;
}

std::vector<Stream> Endpoint::getStreams() const
{
    std::lock_guard<std::mutex> l (impl->lock);
    std::vector<Stream> out;
    const double now = nowMs();
    for (auto& [inst, r] : impl->remotes)
        for (auto& [id, x] : r.rx)
            if (x.hasInfo && now - x.lastInfo < 5000.0 && impl->trusts (r))
                out.push_back ({ x.stream, r.name, ipText (ntohl (r.from.sin_addr.s_addr)), x.codec, x.channels, x.frames, x.kbps, x.rate, (now - x.lastPacket) / 1000.0 });
    return out;
}

void Endpoint::listen (const std::string& stream, const std::string& sender)
{
    std::lock_guard<std::mutex> l (impl->lock);
    impl->wantStream = cleanName (stream);
    impl->wantSender = cleanName (sender);
    if (impl->play != nullptr) impl->unbind();
    impl->bindIfWanted (nowMs());
}

void Endpoint::setOutputFormat (double rate, int burst)
{
    std::lock_guard<std::mutex> l (impl->lock);
    if (rate == impl->outRate.load() && burst == impl->outBurst.load()) return;
    impl->outRate = rate;
    impl->outBurst = burst;
    if (impl->play != nullptr)
    {
        auto rit = impl->remotes.find (impl->play->instance);
        if (rit != impl->remotes.end())
            if (auto x = rit->second.rx.find (impl->play->id); x != rit->second.rx.end()) impl->bindTo (rit->second, x->second);
    }
}

void Endpoint::pull (float* out, int frames) noexcept
{
    impl->inPull.store (true, std::memory_order_seq_cst);
    auto* p = impl->rxLive.load (std::memory_order_seq_cst);
    const float g = playMuted.load (std::memory_order_relaxed) ? 0.0f : playGain.load (std::memory_order_relaxed);
    for (int done = 0; done < frames;)
    {
        const int n = std::min (frames - done, kMaxChunk);
        if (p != nullptr) p->pullChunk (out + 2 * done, n, g);
        else std::memset (out + 2 * done, 0, (size_t) n * 2 * sizeof (float));
        done += n;
    }
    impl->pullEpoch.fetch_add (1, std::memory_order_seq_cst);
    impl->inPull.store (false, std::memory_order_seq_cst);
}

RxStats Endpoint::getRxStats()
{
    std::lock_guard<std::mutex> l (impl->lock);
    RxStats s;
    s.listening = ! impl->wantStream.empty();
    s.stream = impl->wantStream;
    s.sender = impl->wantSender;
    s.hint = impl->hint;
    if (auto* pb = impl->play.get())
    {
        auto rit = impl->remotes.find (pb->instance);
        const double now = nowMs();
        if (rit != impl->remotes.end())
            if (auto x = rit->second.rx.find (pb->id); x != rit->second.rx.end()) s.receiving = now - x->second.lastPacket < 1000.0;
        if (rit != impl->remotes.end())
            if (auto x = rit->second.rx.find (pb->id); x != rit->second.rx.end()) s.stream = x->second.stream;
        s.codec = pb->codec;
        s.rate = pb->rate;
        s.received = pb->received;
        s.lost = pb->lost;
        const double total = (double) pb->received + (double) pb->lost;
        s.lossPercent = total > 0 ? 100.0 * pb->lost / total : 0.0;
        s.jitterMs = pb->jitter * 1000.0;
    }
    if (auto* p = impl->rxLive.load())
    {
        s.bufferMs = 1000.0 * p->latency.load() / std::max (8000.0, p->senderRate);
        s.underruns = p->underruns.load();
        s.peak = p->peak.exchange (0.0f);
    }
    return s;
}

bool Endpoint::startSending (const SendSettings& settings, double inputRate, std::string& error)
{
    stopSending();
    std::lock_guard<std::mutex> l (impl->lock);
    auto& m = *impl;
    if (! m.running.load()) { error = "Hydra Link is not running"; return false; }
    m.send = settings;
    m.send.channels = std::max (1, std::min (2, settings.channels));
    m.send.stream = cleanName (settings.stream.empty() ? std::string ("Phone") : settings.stream);
    m.txCodec = settings.codec == Codec::automatic ? Codec::pcm24 : settings.codec;
    m.txRate = inputRate;
    if (m.txCodec == Codec::opus)
    {
        if (inputRate != 48000.0) { error = "Opus needs 48 kHz"; return false; }
        int err = 0;
        if (m.enc != nullptr) opus_encoder_destroy (m.enc);
        m.enc = opus_encoder_create (48000, m.send.channels, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
        if (m.enc == nullptr || err != OPUS_OK) { m.enc = nullptr; error = "Opus could not start"; return false; }
        opus_encoder_ctl (m.enc, OPUS_SET_BITRATE (std::max (16, std::min (510, settings.kbps)) * 1000));
        opus_encoder_ctl (m.enc, OPUS_SET_VBR (1));
        opus_encoder_ctl (m.enc, OPUS_SET_VBR_CONSTRAINT (1));
        opus_encoder_ctl (m.enc, OPUS_SET_COMPLEXITY (5));
        opus_encoder_ctl (m.enc, OPUS_SET_INBAND_FEC (1));
        opus_encoder_ctl (m.enc, OPUS_SET_PACKET_LOSS_PERC (5));   // Wi-Fi
        opus_encoder_ctl (m.enc, OPUS_SET_SIGNAL (OPUS_SIGNAL_VOICE));
        m.txFrames = kOpusFrames;
        m.encInter.assign ((size_t) (kOpusFrames * m.send.channels), 0.0f);
    }
    else
    {
        // At most one packet; ~2.5 ms of audio keeps Hydra's buffer (one packet + 3 ms) small.
        const int fit = kMaxPayload / (pcmBytes (m.txCodec) * m.send.channels);
        m.txFrames = std::max (16, std::min (fit, (int) (inputRate * 0.0025)));
    }
    m.txL.assign (kMaxChunk, 0.0f);
    m.txR.assign (kMaxChunk, 0.0f);

    // Destination: a Hydra by its announced name (followed to its current address) or an IP address.
    uint32_t ip = 0;
    m.byName = ! parseIp (settings.host, ip);
    m.toValid = false;
    if (! m.byName)
    {
        if (! isLanIp (ip)) { error = "Only Hydras in the local network (no network password yet)"; return false; }
        m.to = makeAddr (ip, settings.port);
        m.toValid = true;
    }
    else
        for (auto& [inst, r] : m.remotes)
            if (r.announced && sameName (r.name, settings.host))
            {
                m.to = makeAddr (ntohl (r.from.sin_addr.s_addr), r.port > 0 ? r.port : settings.port);
                m.toValid = true;
            }

    m.streamId = (uint32_t) random64();
    if (m.streamId == 0) m.streamId = 1;
    m.seq = 0;
    m.position = 0;
    m.lastInfo = -1.0e9;
    m.infoCount = 0;
    m.fbLoss = m.fbBuffer = -1;
    m.fbAt = -1.0e9;
    auto* p = new TxPath();
    p->prepare (m.send.channels, inputRate);
    m.txLive.store (p, std::memory_order_seq_cst);
    m.sending = true;
    return true;
}

void Endpoint::stopSending()
{
    std::lock_guard<std::mutex> l (impl->lock);
    if (! impl->sending) return;
    impl->sendBye();
    impl->sending = false;
    impl->retireTx();
}

void Endpoint::push (const float* in, int channels, int frames) noexcept
{
    impl->inPush.store (true, std::memory_order_seq_cst);
    if (auto* p = impl->txLive.load (std::memory_order_seq_cst))
    {
        const float g = micGain.load (std::memory_order_relaxed);
        const float send = micMuted.load (std::memory_order_relaxed) ? 0.0f : 1.0f;   // muted: silence, the meter still shows the mic
        float pk = 0.0f;
        for (int done = 0; done < frames;)
        {
            const int n = std::min (frames - done, kMaxChunk);
            for (int i = 0; i < n; ++i)
            {
                const float* f = in + (size_t) (done + i) * (size_t) channels;
                float a = f[0] * g, b = (channels > 1 ? f[1] : f[0]) * g;
                if (p->channels == 1 && channels > 1) a = 0.5f * (a + b);
                pk = std::max (pk, std::max (std::abs (a), p->channels > 1 ? std::abs (b) : 0.0f));
                p->l[(size_t) i] = a * send;
                p->r[(size_t) i] = b * send;
            }
            const float* src[2] = { p->l.data(), p->r.data() };
            if (p->ring.write (src, p->channels, n) < n) p->overflows.fetch_add (1, std::memory_order_relaxed);
            done += n;
        }
        maxInto (p->peak, pk);
    }
    impl->pushEpoch.fetch_add (1, std::memory_order_seq_cst);
    impl->inPush.store (false, std::memory_order_seq_cst);
}

TxStats Endpoint::getTxStats()
{
    std::lock_guard<std::mutex> l (impl->lock);
    TxStats s;
    auto& m = *impl;
    s.sending = m.sending;
    s.to = m.send.host;
    s.codec = m.txCodec;
    s.reachable = m.toValid;
    if (m.toValid) s.address = ipText (ntohl (m.to.sin_addr.s_addr));
    s.heard = nowMs() - m.fbAt < 3000.0;
    s.lossPercent = s.heard ? m.fbLoss : -1;
    s.bufferMs = s.heard ? m.fbBuffer : -1;
    if (auto* p = m.txLive.load()) s.peak = p->peak.exchange (0.0f);
    return s;
}
} // namespace hydra::lite

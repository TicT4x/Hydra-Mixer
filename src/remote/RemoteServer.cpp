#include "RemoteServer.h"
#include "core/NetCrypto.h"
#include "engine/NetLink.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <map>
#include <thread>

#pragma comment (lib, "ws2_32.lib")
#pragma comment (lib, "bcrypt.lib")

namespace hydra::remote
{
namespace
{
constexpr int kMaxClients = 8;              // joined WebSocket clients
constexpr int kMaxConnections = 24;         // incl. plain HTTP requests and clients still entering the code
constexpr size_t kMaxRequest = 16 * 1024;   // HTTP request head
constexpr size_t kMaxMessage = 64 * 1024;   // one WebSocket message
constexpr size_t kMaxQueued = 4 * 1024 * 1024;   // unsent bytes to one client before it is dropped (stalled phone)
constexpr double kHttpTimeoutMs = 10000, kHelloTimeoutMs = 30000, kIdleTimeoutMs = 30000, kPingMs = 10000;
constexpr int kMaxFailures = 5;
constexpr double kBlockMs = 60000;
const char* const kCodeAlphabet = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";   // no 0 / O, 1 / I / L

std::string frame (int opcode, const void* data, size_t bytes)
{
    std::string f;
    f.reserve (bytes + 10);
    f.push_back ((char) (0x80 | opcode));
    if (bytes < 126) f.push_back ((char) bytes);
    else if (bytes < 65536) { f.push_back ((char) 126); f.push_back ((char) (bytes >> 8)); f.push_back ((char) bytes); }
    else
    {
        f.push_back ((char) 127);
        for (int i = 7; i >= 0; --i) f.push_back ((char) ((uint64_t) bytes >> (8 * i)));
    }
    f.append ((const char*) data, bytes);
    return f;
}

std::string textFrame (const juce::String& s)
{
    const auto utf8 = s.toStdString();
    return frame (1, utf8.data(), utf8.size());
}

juce::String deviceFromAgent (const juce::String& ua)
{
    if (ua.contains ("iPad")) return "iPad";
    if (ua.contains ("iPhone")) return "iPhone";
    if (ua.contains ("Android")) return ua.contains ("Mobile") ? "Android phone" : "Android tablet";
    if (ua.contains ("Windows")) return "Windows PC";
    if (ua.contains ("Macintosh")) return "Mac";
    if (ua.contains ("Linux")) return "Linux";
    return "Browser";
}

double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }
} // namespace

struct Server::Impl
{
    struct Conn
    {
        SOCKET s = INVALID_SOCKET;
        WSAEVENT ev = WSA_INVALID_EVENT;
        int id = 0;
        uint32_t ip = 0;
        juce::String address, device;
        std::string in, out;
        bool ws = false, joined = false, closing = false, dead = false;
        double opened = 0, lastRx = 0, lastPing = 0;
        std::string msg;
        int msgOp = 0;
    };
    struct Failures { int count = 0; double first = 0, blockedUntil = 0; };
    struct Snapshot { int id; juce::String address, device; double opened; };

    // Shared (lock)
    mutable std::mutex lock;
    Config cfg;
    bool running = false;
    int port = 0;
    juce::String lastError;
    std::vector<Event> events;
    std::vector<std::pair<int, std::string>> outbox;
    std::vector<Snapshot> snapshot;
    bool kickAll = false;
    std::atomic<uint32_t> changes { 0 };

    // Set before start, read by the thread.
    std::map<juce::String, File> files;

    // Thread
    std::thread thread;
    std::atomic<bool> quit { false };
    HANDLE wake = nullptr;
    SOCKET listener = INVALID_SOCKET;
    WSAEVENT listenEv = WSA_INVALID_EVENT;
    std::vector<std::unique_ptr<Conn>> conns;
    std::map<uint32_t, Failures> failures;
    int nextId = 1;
    bool wsaOk = false;

    Impl()
    {
        WSADATA d;
        wsaOk = WSAStartup (MAKEWORD (2, 2), &d) == 0;
        wake = CreateEventW (nullptr, FALSE, FALSE, nullptr);
    }
    ~Impl()
    {
        stopThread();
        if (wake != nullptr) CloseHandle (wake);
        if (wsaOk) WSACleanup();
    }

    bool openSocket (const Config& c, juce::String& error)
    {
        listener = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET) { error = "Cannot create a network socket"; return false; }
        BOOL excl = TRUE;
        setsockopt (listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*) &excl, sizeof (excl));
        sockaddr_in a {};
        a.sin_family = AF_INET;
        a.sin_port = htons ((u_short) c.port);
        a.sin_addr.s_addr = htonl (c.loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
        if (bind (listener, (sockaddr*) &a, sizeof (a)) != 0 || listen (listener, 16) != 0)
        {
            error = "Port " + juce::String (c.port) + " is in use by another program";
            closesocket (listener);
            listener = INVALID_SOCKET;
            return false;
        }
        listenEv = WSACreateEvent();
        WSAEventSelect (listener, listenEv, FD_ACCEPT);
        return true;
    }

    void stopThread()
    {
        if (thread.joinable())
        {
            quit = true;
            SetEvent (wake);
            thread.join();
        }
        quit = false;
        for (auto& c : conns) closeConn (*c);
        conns.clear();
        if (listener != INVALID_SOCKET) { closesocket (listener); listener = INVALID_SOCKET; }
        if (listenEv != WSA_INVALID_EVENT) { WSACloseEvent (listenEv); listenEv = WSA_INVALID_EVENT; }
        std::lock_guard<std::mutex> l (lock);
        outbox.clear();
        if (! snapshot.empty()) { snapshot.clear(); ++changes; }
    }

    void closeConn (Conn& c)
    {
        if (c.s != INVALID_SOCKET) { closesocket (c.s); c.s = INVALID_SOCKET; }
        if (c.ev != WSA_INVALID_EVENT) { WSACloseEvent (c.ev); c.ev = WSA_INVALID_EVENT; }
    }

    //--------------------------------------------------------------------------
    void run()
    {
        while (! quit.load())
        {
            std::vector<WSAEVENT> evs { wake, listenEv };
            for (auto& c : conns) evs.push_back (c->ev);
            WSAWaitForMultipleEvents ((DWORD) evs.size(), evs.data(), FALSE, 5000, FALSE);
            if (quit.load()) break;

            WSANETWORKEVENTS ne {};
            if (WSAEnumNetworkEvents (listener, listenEv, &ne) == 0 && (ne.lNetworkEvents & FD_ACCEPT) != 0)
                acceptAll();
            for (auto& c : conns)
                if (WSAEnumNetworkEvents (c->s, c->ev, &ne) == 0 && (ne.lNetworkEvents & (FD_READ | FD_CLOSE)) != 0)
                    readAll (*c, (ne.lNetworkEvents & FD_CLOSE) != 0);

            takeOutbox();
            housekeeping();
            for (auto& c : conns) flush (*c);
            reap();
        }
    }

    void acceptAll()
    {
        for (;;)
        {
            sockaddr_in a {};
            int len = sizeof (a);
            SOCKET s = accept (listener, (sockaddr*) &a, &len);
            if (s == INVALID_SOCKET) return;
            char buf[INET_ADDRSTRLEN] = {};
            inet_ntop (AF_INET, &a.sin_addr, buf, sizeof (buf));
            const juce::String address (buf);
            if (! net::Hub::isLocalAddress (address) || (int) conns.size() >= kMaxConnections)
            {
                closesocket (s);   // only the local network, and not too many at once
                continue;
            }
            BOOL nodelay = TRUE;
            setsockopt (s, IPPROTO_TCP, TCP_NODELAY, (const char*) &nodelay, sizeof (nodelay));
            auto c = std::make_unique<Conn>();
            c->s = s;
            c->ev = WSACreateEvent();
            WSAEventSelect (s, c->ev, FD_READ | FD_WRITE | FD_CLOSE);
            c->id = nextId++;
            c->ip = ntohl (a.sin_addr.s_addr);
            c->address = address;
            c->opened = c->lastRx = c->lastPing = nowMs();
            conns.push_back (std::move (c));
        }
    }

    void readAll (Conn& c, bool closed)
    {
        char buf[8192];
        for (;;)
        {
            const int r = recv (c.s, buf, sizeof (buf), 0);
            if (r > 0)
            {
                c.in.append (buf, (size_t) r);
                c.lastRx = nowMs();
                if (c.in.size() > kMaxMessage * 4) { c.dead = true; return; }
                continue;
            }
            if (r == 0 || WSAGetLastError() != WSAEWOULDBLOCK) closed = true;
            break;
        }
        if (c.ws) parseFrames (c);
        else      parseHttp (c);
        if (closed) c.dead = true;
    }

    //--------------------------------------------------------------------------
    void parseHttp (Conn& c)
    {
        const auto end = c.in.find ("\r\n\r\n");
        if (end == std::string::npos)
        {
            if (c.in.size() > kMaxRequest) c.dead = true;
            return;
        }
        const juce::String head = juce::String::fromUTF8 (c.in.data(), (int) end);
        c.in.erase (0, end + 4);
        auto lines = juce::StringArray::fromLines (head);
        const auto request = juce::StringArray::fromTokens (lines[0], " ", {});
        auto header = [&lines] (const char* name)
        {
            for (int i = 1; i < lines.size(); ++i)
                if (lines[i].upToFirstOccurrenceOf (":", false, false).trim().equalsIgnoreCase (name))
                    return lines[i].fromFirstOccurrenceOf (":", false, false).trim();
            return juce::String();
        };
        c.device = deviceFromAgent (header ("User-Agent"));

        if (request.size() < 2 || request[0] != "GET")
        {
            respond (c, "405 Method Not Allowed", "text/plain", "Method not allowed", 18);
            return;
        }
        auto path = request[1].upToFirstOccurrenceOf ("?", false, false).upToFirstOccurrenceOf ("#", false, false);
        if (path == "/ws")
        {
            const auto key = header ("Sec-WebSocket-Key");
            if (! header ("Upgrade").equalsIgnoreCase ("websocket") || key.isEmpty())
            {
                respond (c, "400 Bad Request", "text/plain", "WebSocket expected", 18);
                return;
            }
            c.out += "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: "
                   + acceptKey (key).toStdString() + "\r\n\r\n";
            c.ws = true;
            c.opened = c.lastRx = c.lastPing = nowMs();
            parseFrames (c);   // anything that came right after the head
            return;
        }
        if (path == "/") path = "/index.html";
        auto it = files.find (path);
        if (it == files.end())
        {
            respond (c, "404 Not Found", "text/plain", "Not found", 9);
            return;
        }
        if (it->second.data == nullptr)
        {
            juce::MemoryBlock mb;
            if (! it->second.disk.loadFileAsData (mb))
            {
                respond (c, "404 Not Found", "text/plain", "Not found", 9);
                return;
            }
            respond (c, "200 OK", it->second.mime, mb.getData(), mb.getSize());
            return;
        }
        respond (c, "200 OK", it->second.mime, it->second.data, it->second.size);
    }

    void respond (Conn& c, const char* status, const juce::String& mime, const void* body, size_t bytes)
    {
        std::string h = "HTTP/1.1 ";
        h += status;
        h += "\r\nContent-Type: " + mime.toStdString()
           + "\r\nContent-Length: " + std::to_string (bytes)
           + "\r\nCache-Control: no-cache\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n";
        c.out += h;
        c.out.append ((const char*) body, bytes);
        c.closing = true;   // closed once sent
    }

    //--------------------------------------------------------------------------
    void parseFrames (Conn& c)
    {
        for (;;)
        {
            if (c.dead || c.closing || c.in.size() < 2) return;
            const auto* p = (const uint8_t*) c.in.data();
            const bool fin = (p[0] & 0x80) != 0;
            const int op = p[0] & 0x0F;
            const bool masked = (p[1] & 0x80) != 0;
            uint64_t len = p[1] & 0x7F;
            size_t pos = 2;
            if (len == 126)
            {
                if (c.in.size() < 4) return;
                len = ((uint64_t) p[2] << 8) | p[3];
                pos = 4;
            }
            else if (len == 127)
            {
                if (c.in.size() < 10) return;
                len = 0;
                for (int i = 0; i < 8; ++i) len = (len << 8) | p[2 + i];
                pos = 10;
            }
            if (! masked || len > kMaxMessage) { c.dead = true; return; }   // clients must mask (RFC 6455)
            if (c.in.size() < pos + 4 + len) return;
            const uint8_t* mask = p + pos;
            std::string payload (c.in.data() + pos + 4, (size_t) len);
            for (size_t i = 0; i < payload.size(); ++i) payload[i] = (char) (payload[i] ^ mask[i & 3]);
            c.in.erase (0, pos + 4 + (size_t) len);

            switch (op)
            {
                case 8:   // close: answer and close
                    c.out += frame (8, payload.data(), std::min<size_t> (payload.size(), 2));
                    c.closing = true;
                    return;
                case 9:  c.out += frame (10, payload.data(), payload.size()); break;   // ping -> pong
                case 10: break;   // pong (lastRx already updated)
                case 0: case 1: case 2:
                    if (op != 0) { c.msg.clear(); c.msgOp = op; }
                    c.msg += payload;
                    if (c.msg.size() > kMaxMessage) { c.dead = true; return; }
                    if (fin && c.msgOp == 1) onText (c, juce::String::fromUTF8 (c.msg.data(), (int) c.msg.size()));
                    if (fin) c.msg.clear();
                    break;
                default: c.dead = true; return;
            }
        }
    }

    void onText (Conn& c, const juce::String& text)
    {
        if (c.joined)
        {
            std::lock_guard<std::mutex> l (lock);
            if (events.size() < 4096) events.push_back ({ Event::Type::message, c.id, text });
            return;
        }
        const auto v = juce::JSON::parse (text);
        if (v.getProperty ("t", {}).toString() != "hello") return;   // nothing else before the code

        const double now = nowMs();
        auto& f = failures[c.ip];
        if (now < f.blockedUntil)
        {
            c.out += textFrame (R"({"t":"auth","ok":false,"why":"blocked"})");
            return;
        }
        juce::String expected;
        int joinedCount = 0;
        {
            std::lock_guard<std::mutex> l (lock);
            expected = normaliseCode (cfg.code);
        }
        for (auto& o : conns) if (o->joined && ! o->dead) ++joinedCount;
        const auto given = normaliseCode (v.getProperty ("code", {}).toString());

        // Constant time over the expected length.
        bool same = expected.isNotEmpty() && given.length() == expected.length();
        int diff = 0;
        for (int i = 0; i < expected.length(); ++i) diff |= (int) (expected[i] ^ (i < given.length() ? given[i] : 0));
        same = same && diff == 0;

        if (! same)
        {
            if (now - f.first > kBlockMs) { f.count = 0; f.first = now; }
            if (++f.count >= kMaxFailures) { f.blockedUntil = now + kBlockMs; f.count = 0; }
            c.out += textFrame (R"({"t":"auth","ok":false,"why":"code"})");
            return;
        }
        if (joinedCount >= kMaxClients)
        {
            c.out += textFrame (R"({"t":"auth","ok":false,"why":"full"})");
            c.closing = true;
            return;
        }
        failures.erase (c.ip);
        c.joined = true;
        c.out += textFrame (R"({"t":"auth","ok":true})");
        std::lock_guard<std::mutex> l (lock);
        events.push_back ({ Event::Type::joined, c.id, {} });
        updateSnapshot();
    }

    //--------------------------------------------------------------------------
    void takeOutbox()
    {
        std::vector<std::pair<int, std::string>> out;
        bool kick = false;
        {
            std::lock_guard<std::mutex> l (lock);
            out.swap (outbox);
            kick = std::exchange (kickAll, false);
        }
        if (kick)
            for (auto& c : conns)
                if (c->ws && c->joined)
                {
                    const uint8_t code[2] = { 0x0F, 0xA0 };   // 4000: the access code was changed
                    c->out += frame (8, code, 2);
                    c->closing = true;
                }
        for (auto& [id, bytes] : out)
            for (auto& c : conns)
                if (c->joined && ! c->closing && (id < 0 || c->id == id))
                {
                    if (c->out.size() > kMaxQueued) { c->dead = true; continue; }
                    c->out += bytes;
                }
    }

    void housekeeping()
    {
        const double now = nowMs();
        for (auto& c : conns)
        {
            if (! c->ws) { if (now - c->opened > kHttpTimeoutMs) c->dead = true; continue; }
            if (! c->joined && now - c->opened > kHelloTimeoutMs) c->dead = true;
            if (now - c->lastRx > kIdleTimeoutMs) c->dead = true;   // phone asleep / out of range
            else if (now - c->lastPing > kPingMs && now - c->lastRx > kPingMs)
            {
                c->out += frame (9, nullptr, 0);
                c->lastPing = now;
            }
        }
        for (auto it = failures.begin(); it != failures.end();)
            it = (now > it->second.blockedUntil && now - it->second.first > kBlockMs) ? failures.erase (it) : std::next (it);
    }

    void flush (Conn& c)
    {
        while (! c.dead && ! c.out.empty())
        {
            const int r = ::send (c.s, c.out.data(), (int) std::min<size_t> (c.out.size(), 1 << 16), 0);
            if (r > 0) { c.out.erase (0, (size_t) r); continue; }
            if (WSAGetLastError() != WSAEWOULDBLOCK) c.dead = true;
            break;   // FD_WRITE wakes us when there is room again
        }
        if (c.closing && c.out.empty())
        {
            shutdown (c.s, SD_SEND);
            c.dead = true;
        }
    }

    void reap()
    {
        bool changed = false;
        for (auto it = conns.begin(); it != conns.end();)
        {
            auto& c = **it;
            if (! c.dead) { ++it; continue; }
            if (c.joined)
            {
                std::lock_guard<std::mutex> l (lock);
                events.push_back ({ Event::Type::left, c.id, {} });
                changed = true;
            }
            closeConn (c);
            it = conns.erase (it);
        }
        if (changed)
        {
            std::lock_guard<std::mutex> l (lock);
            updateSnapshot();
        }
    }

    /** Lock held. */
    void updateSnapshot()
    {
        snapshot.clear();
        for (auto& c : conns)
            if (c->joined && ! c->dead) snapshot.push_back ({ c->id, c->address, c->device, c->opened });
        ++changes;
    }
};

//==============================================================================
Server::Server() : impl (std::make_unique<Impl>()) {}
Server::~Server() { stop(); }

void Server::setFiles (std::vector<File> list)
{
    impl->files.clear();
    for (auto& f : list) impl->files[f.path] = f;
}

bool Server::start (const Config& c, juce::String& error)
{
    {
        std::lock_guard<std::mutex> l (impl->lock);
        if (impl->running && impl->cfg.port == c.port && impl->cfg.loopbackOnly == c.loopbackOnly)
        {
            if (normaliseCode (impl->cfg.code) != normaliseCode (c.code)) impl->kickAll = true;   // new code: everyone pairs again
            impl->cfg = c;
            SetEvent (impl->wake);
            return true;
        }
    }
    stop();
    if (! impl->wsaOk) { error = "Windows networking is not available"; return false; }
    if (! impl->openSocket (c, error))
    {
        std::lock_guard<std::mutex> l (impl->lock);
        impl->lastError = error;
        return false;
    }
    {
        std::lock_guard<std::mutex> l (impl->lock);
        impl->cfg = c;
        impl->running = true;
        impl->port = c.port;
        impl->lastError = {};
        impl->events.clear();
    }
    impl->thread = std::thread ([this] { impl->run(); });
    return true;
}

void Server::stop()
{
    impl->stopThread();
    std::lock_guard<std::mutex> l (impl->lock);
    if (impl->running) impl->events.clear();
    impl->running = false;
    impl->port = 0;
}

bool Server::isRunning() const noexcept   { std::lock_guard<std::mutex> l (impl->lock); return impl->running; }
Server::Config Server::getConfig() const  { std::lock_guard<std::mutex> l (impl->lock); return impl->cfg; }
int Server::getPort() const noexcept      { std::lock_guard<std::mutex> l (impl->lock); return impl->port; }
juce::String Server::getLastError() const { std::lock_guard<std::mutex> l (impl->lock); return impl->lastError; }
uint32_t Server::getChangeCount() const noexcept { return impl->changes.load(); }

std::vector<Server::Event> Server::takeEvents()
{
    std::vector<Event> e;
    std::lock_guard<std::mutex> l (impl->lock);
    e.swap (impl->events);
    return e;
}

void Server::send (int client, const juce::String& text)
{
    auto f = textFrame (text);
    std::lock_guard<std::mutex> l (impl->lock);
    if (! impl->running) return;
    impl->outbox.emplace_back (client, std::move (f));
    SetEvent (impl->wake);
}

void Server::sendBinary (int client, const void* data, size_t bytes)
{
    auto f = frame (2, data, bytes);
    std::lock_guard<std::mutex> l (impl->lock);
    if (! impl->running) return;
    impl->outbox.emplace_back (client, std::move (f));
    SetEvent (impl->wake);
}

std::vector<Server::ClientInfo> Server::getClients() const
{
    std::vector<ClientInfo> out;
    const double now = nowMs();
    std::lock_guard<std::mutex> l (impl->lock);
    for (auto& s : impl->snapshot) out.push_back ({ s.id, s.address, s.device, (now - s.opened) / 1000.0 });
    return out;
}

juce::String Server::normaliseCode (const juce::String& s)
{
    juce::String out;
    for (auto c : s.toUpperCase())
        if (juce::CharacterFunctions::isLetterOrDigit (c)) out += c;
    return out;
}

juce::String Server::newCode()
{
    const int n = (int) strlen (kCodeAlphabet);
    juce::String code;
    for (int i = 0; i < 8; ++i)
    {
        uint64_t r;
        do r = net::randomU64(); while (r >= UINT64_MAX - UINT64_MAX % (uint64_t) n);   // no modulo bias
        code += kCodeAlphabet[r % (uint64_t) n];
    }
    return code;
}

juce::String Server::formatCode (const juce::String& s)
{
    const auto c = normaliseCode (s);
    return c.length() == 8 ? c.substring (0, 4) + "-" + c.substring (4) : c;
}

juce::String Server::acceptKey (const juce::String& key)
{
    const auto text = (key.trim() + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").toStdString();
    uint8_t digest[20] = {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider (&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) == 0)
    {
        BCryptHash (alg, nullptr, 0, (PUCHAR) text.data(), (ULONG) text.size(), digest, sizeof (digest));
        BCryptCloseAlgorithmProvider (alg, 0);
    }
    return juce::Base64::toBase64 (digest, sizeof (digest));
}
} // namespace hydra::remote

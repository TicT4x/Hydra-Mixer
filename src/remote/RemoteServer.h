#pragma once
#include "Common.h"
#include <mutex>

/** Hydra Remote, transport: a small HTTP + WebSocket server for phones and tablets (no library, Winsock).

    One thread (normal priority, never touches audio) serves the web app (static files compiled into the exe)
    and keeps the WebSocket connections. It sleeps in WSAWaitForMultipleEvents and only wakes for network
    traffic, outgoing messages or a 5 s housekeeping tick (pings, timeouts).

    Security:
      - Only private / local addresses are accepted (10/8, 172.16/12, 192.168/16, 169.254/16, 127/8, see
        net::Hub::isLocalAddress); others are disconnected right after accept. CGNAT / Tailscale (100.64/10)
        is not local.
      - Every WebSocket must first send {"t":"hello","code":"..."} with the access code shown in Hydra
        (Settings > Remote). Until then nothing else is accepted or sent. After 5 wrong codes from one address
        within a minute that address is refused for 60 s.
      - HTTP is not encrypted (a local network tool): anyone who can sniff the local network can read the code.
        "New code" in the settings disconnects every phone.
    The message thread side (what is sent, what commands do) is RemoteControl. */
namespace hydra::remote
{
constexpr int kDefaultPort = 6981;

class Server
{
public:
    struct Config
    {
        int port = kDefaultPort;
        juce::String code;          // access code (compared after normaliseCode)
        bool loopbackOnly = false;  // tests: listen on 127.0.0.1 only
        bool operator== (const Config&) const = default;
    };

    /** A file of the web app. */
    struct File
    {
        juce::String path, mime;    // "/index.html", "text/html; charset=utf-8"
        const void* data = nullptr;
        size_t size = 0;
        juce::File disk;            // data == nullptr: read from this file at each request (large, optional files)
    };

    struct Event
    {
        enum class Type { joined, left, message };
        Type type = Type::message;
        int client = 0;
        juce::String text;   // message: the JSON text
    };

    struct ClientInfo
    {
        int id = 0;
        juce::String address, device;   // device: short description from the browser's User-Agent
        double seconds = 0;             // connected for
    };

    Server();
    ~Server();

    /** Files served over HTTP ("/" = "/index.html"). Call before start. */
    void setFiles (std::vector<File>);

    /** Opens the port and starts the thread (or applies a changed configuration: a new port reopens, a new code
        disconnects every client). False + error if the port cannot be opened. */
    bool start (const Config&, juce::String& error);
    void stop();
    bool isRunning() const noexcept;
    Config getConfig() const;
    int getPort() const noexcept;   // the port actually open (0 if not running)
    juce::String getLastError() const;

    /** Events since the last call (joined after a correct code, left, messages of joined clients). */
    std::vector<Event> takeEvents();
    /** Queues a text / binary WebSocket message to a joined client (client < 0: to all joined clients). */
    void send (int client, const juce::String& text);
    void sendBinary (int client, const void* data, size_t bytes);

    std::vector<ClientInfo> getClients() const;
    /** Grows whenever a client joins or leaves. */
    uint32_t getChangeCount() const noexcept;

    /** Upper case, without separators and look-alikes (0 -> O, 1 -> I ...). */
    static juce::String normaliseCode (const juce::String&);
    /** A new random access code: 8 characters, shown as "K7QF-3MXA". */
    static juce::String newCode();
    static juce::String formatCode (const juce::String&);

    /** The accept key of a WebSocket handshake (base64 of SHA-1 (key + GUID)); public for the tests. */
    static juce::String acceptKey (const juce::String& key);

    struct Impl;

private:
    std::unique_ptr<Impl> impl;
    JUCE_DECLARE_NON_COPYABLE (Server)
};
} // namespace hydra::remote

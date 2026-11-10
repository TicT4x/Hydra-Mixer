#pragma once
#include "IoStreams.h"
#include "core/NetCodec.h"

/** Hydra Link: audio between Hydras over the network (own protocol over UDP, not VBAN).

    One Hub per process owns one UDP socket (default port 6980) and one network thread (MMCSS "Pro Audio",
    woken by the socket and, while something is sent, by a high-resolution timer every 1 ms). Streams are
    ordinary asynchronous streams for the engine:
      - NetInput (Kind::Network input): packets of one sender's stream go into the stream's ring; the drift
        controller and resampler bind the sender's clock to the master like any other secondary device.
      - NetOutput (Kind::Network output): the engine writes the bus into the ring at the master clock, the
        network thread cuts it into packets and sends them. No resampler on the sending side.
    The engine thread only touches the rings: sockets, coding and encryption all run on the network thread.

    Discovery: every Hydra announces itself every 2 s by broadcast in each local network (and directly to the
    manually added addresses). A sender describes each stream (name, codec, rate) once per second; receivers list
    all streams they hear, and an input picks one by "<stream>@<sender>" (by name, not by IP address).

    Security: without a password only private addresses (local network) are accepted and sent to. With a
    password every packet is AES-256-GCM encrypted (core/NetCrypto.h) and only Hydras with the same password
    are heard; this is required for anything outside the local network.

    Pairing (without a password, since 10.11.): every Hydra / phone has a persistent device id (announced). Audio
    is only taken from paired devices. A device asks the other one ("Pair" request, repeated each second); there
    the user allows or denies once (Hub::getRequests / answer), both sides then store each other (Hub::getTrusted,
    persisted by the app). An output to a Hydra that is not paired asks it by itself. A phone asking a Hydra that
    allows it gets Hydra Remote's port + access code in the answer (control without typing the code). With a
    password the password is the proof: every Hydra with it counts as paired. */
namespace hydra::net
{
constexpr int kDefaultPort = 6980;

/** Where a bus sends to (deviceName of a Kind::Network output target):
    "hydralink://<host>:<port>/<stream>?codec=<id>&kbps=<n>". host = a Hydra's name as it announces itself,
    an IP address or a DNS name. */
struct Destination
{
    juce::String host;
    int port = kDefaultPort;
    juce::String stream;
    Codec codec = Codec::automatic;
    int kbps = 128;   // Opus

    juce::String toDeviceName() const;
    static Destination parse (const juce::String& deviceName);
    IoTarget target() const;
};

/** What an input listens to (deviceName of a Kind::Network input target): "<stream>@<sender>". */
struct Source
{
    juce::String stream, sender;

    juce::String toDeviceName() const { return stream + "@" + sender; }
    static Source parse (const juce::String& deviceName);
    IoTarget target() const;
};

/** Stream names: what a channel is called, shortened to what the packet format carries. */
juce::String cleanName (const juce::String&);

enum class DeviceKind : uint8_t { computer = 0, phone = 1 };

/** A device this one trusts (paired). */
struct TrustedDevice
{
    uint64_t id = 0;
    juce::String name;
    DeviceKind kind = DeviceKind::computer;
    bool operator== (const TrustedDevice&) const = default;
};

/** "1a2b...": device ids as text (sessions, the phone app). */
juce::String deviceIdText (uint64_t);
uint64_t parseDeviceId (const juce::String&);

/** Another device that asks to be paired with this one (waiting for the user's answer). */
struct PairRequest
{
    uint64_t id = 0;
    juce::String name, address;
    DeviceKind kind = DeviceKind::computer;
};

/** Where pairing with another device stands (from this side's view). */
enum class PairState { none, asking, allowed, denied, noAnswer };

struct Config
{
    int port = kDefaultPort;
    juce::String name;         // this computer as the others see it (empty = Windows computer name)
    juce::String password;     // empty = no encryption, local network only
    juce::StringArray peers;   // more addresses to announce to: "host[:port]" (VPN, internet)
    bool broadcast = true;     // announce in the local networks (off in the tests: they must not show up elsewhere)
    bool loopbackOnly = false; // tests: listen on 127.0.0.1 only (no firewall question, nothing reachable from outside)
    int remotePort = 0;        // Hydra Remote's web app (TCP), announced so the phone app finds it; 0 = off
    juce::String remoteCode;   // Hydra Remote's access code: given to phones this Hydra pairs with
    uint64_t deviceId = 0;     // persistent id of this Hydra (0 = a random one per start: tests)
    bool pairing = true;       // without a password: audio only from paired devices (false: from every Hydra in the LAN)
    bool operator== (const Config&) const = default;
};

/** Another Hydra this one hears (announce) or was told about (Config::peers). */
struct PeerInfo
{
    juce::String name;      // as it announces itself (empty: a manual address that has not answered)
    juce::String address;   // "192.168.1.20"
    int port = kDefaultPort;
    bool lan = true, manual = false;
    double secondsAgo = 1.0e9;
    uint64_t deviceId = 0;                  // 0: an older Hydra (cannot pair) or not heard yet
    DeviceKind kind = DeviceKind::computer;
    bool trusted = false;                   // paired (or no pairing needed: password / pairing off)
    PairState pairing = PairState::none;    // this side's request to it
};

/** A stream this Hydra receives (or could receive). */
struct StreamInfo
{
    juce::String stream, sender, address;
    Codec codec = Codec::pcm24;
    int channels = 2, packetFrames = 0, kbps = 0;
    double rate = 48000.0;
    bool used = false, lan = true;
    double secondsAgo = 0.0;
    Source source() const { return { stream, sender }; }
};

/** Counters of one stream, for the tests and the status texts. */
struct StreamStats
{
    uint32_t received = 0, lost = 0, late = 0;
    double jitterMs = 0.0;
};

class Hub
{
public:
    Hub();
    ~Hub();

    /** The app's hub (Kind::Network targets in openAsyncInput / openAsyncOutput open here). */
    static Hub& get();

    /** Opens the socket and starts the network thread (or applies a changed configuration). False + error if the
        port cannot be opened. Changing port or password restarts the socket; open streams stay and continue. */
    bool start (const Config&, juce::String& error);
    /** Closes the socket. Open streams fail ("Hydra Link is off"). */
    void stop();
    bool isRunning() const noexcept;
    Config getConfig() const;
    juce::String getName() const;   // as announced
    juce::String getLastError() const;

    std::vector<PeerInfo> getPeers() const;
    std::vector<StreamInfo> getStreams() const;
    /** Grows whenever the lists above change (the UI refreshes then). */
    uint32_t getChangeCount() const noexcept;
    /** Why packets were turned away lately (wrong password, unencrypted from the internet ...), empty if none. */
    juce::String getRejectHint() const;
    /** Packets turned away so far (wrong password, changed, repeated, not allowed). */
    uint32_t getRejectedCount() const noexcept;

    //--------------------------------------------------------------------------
    // Pairing
    /** The paired devices (the app loads them once at start; changes by pairing raise getTrustVersion()). */
    void setTrusted (std::vector<TrustedDevice>);
    std::vector<TrustedDevice> getTrusted() const;
    uint32_t getTrustVersion() const noexcept;
    /** Devices asking to be paired right now (they ask again every second; gone after 10 s without asking). */
    std::vector<PairRequest> getRequests() const;
    /** The user's answer. Allowed: stored on both sides. Denied: that device is not asked about for 5 minutes. */
    void answer (uint64_t deviceId, bool allow);
    /** Asks a device (by its announced name) to pair: repeated each second until it answers (2 min at most). */
    void requestPairing (const juce::String& peerName);
    /** Unpairs (and tells the other side, which unpairs as well). */
    void forget (uint64_t deviceId);
    bool isTrusted (uint64_t deviceId) const;

    static juce::StringArray localAddresses();
    static juce::String computerName();
    /** Private / local address (10/8, 172.16/12, 192.168/16, 169.254/16, 127/8). Hosts by name: false. */
    static bool isLocalAddress (const juce::String& ipv4);

    std::shared_ptr<AsyncInput>  openInput  (const IoTarget&, double engineRate, int engineBlock, juce::String& error);
    std::shared_ptr<AsyncOutput> openOutput (const IoTarget&, double engineRate, int engineBlock, juce::String& error);

    /** Counters of the stream an input of this hub receives (tests). */
    StreamStats getStats (const Source&) const;

    /** Tests: faults on received audio packets (applied before anything else). */
    struct Faults
    {
        std::atomic<int> dropPercent { 0 };   // lose this share of the audio packets
        std::atomic<int> swapEvery { 0 };     // hold every n-th audio packet back and deliver it after the next one
        std::atomic<int> replayEvery { 0 };   // deliver every n-th packet twice (raw, as received)
        std::atomic<int> corruptEvery { 0 };  // flip a byte in every n-th packet
    };
    Faults faults;

    struct Impl;

private:
    std::unique_ptr<Impl> impl;
    JUCE_DECLARE_NON_COPYABLE (Hub)
};
} // namespace hydra::net

#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/** Hydra Link Lite: a portable Hydra Link endpoint for the phone app (Android) without JUCE.

    Speaks packet format v1 of engine/NetLink.cpp (same header, Audio / Info / Announce / Feedback / Bye), without
    encryption: local network only, like Hydra Link without a network password. Encrypted packets and packets from
    outside the local network are ignored. tests/LinkLiteTest.cpp checks it against Hydra's own Hub over 127.0.0.1.

    One UDP socket (default port 6980) and one network thread (poll: every 1 ms while sending, else 50 ms). The
    audio threads only touch wait-free rings:
      - receive: one stream at a time (the one Hydra sends to this phone). The network thread reorders, conceals
        and decodes packets into a ring at the sender's rate; the output callback pulls through Hydra's own
        VarResampler + DriftController (core/Resampler.h), which binds the sender's clock to the phone's.
      - send: the microphone callback pushes into a ring; the network thread cuts it into packets (PCM: what is
        there, up to one packet; Opus: 5 ms frames, needs 48 kHz) and sends them to the chosen Hydra.
    The phone announces itself every 2 s (broadcast), so Hydra lists it as a network destination.

    Pairing (as engine/NetLink.h): audio is only taken from paired Hydras. The phone asks a Hydra (requestPairing,
    or by itself when it sends to one that is not paired); a Hydra that allows it answers with Hydra Remote's port
    and access code (Peer::remoteCode), so the app controls it without typing the code. A Hydra that wants to send
    to the phone asks it the same way (getRequests / answer). */
namespace hydra::lite
{
constexpr int kDefaultPort = 6980;

enum class Codec : uint8_t { automatic = 0, pcm16 = 1, pcm24 = 2, float32 = 3, opus = 4 };   // as net::Codec
const char* codecName (Codec);

struct Trusted
{
    uint64_t id = 0;
    std::string name;
};

enum class PairState : int { none = 0, asking = 1, allowed = 2, denied = 3, noAnswer = 4 };   // as net::PairState

struct PairRequest
{
    uint64_t id = 0;
    std::string name, address;
};

struct Config
{
    int port = kDefaultPort;
    uint64_t deviceId = 0;            // persistent id of this phone (0 = random per start: tests)
    bool pairing = true;              // audio only from paired Hydras
    std::string name;                 // how Hydra lists this phone
    std::vector<std::string> peers;   // more addresses to announce to: "ip[:port]"
    bool broadcast = true;            // announce in the local networks
    bool loopbackOnly = false;        // tests: 127.0.0.1 only
};

struct Peer
{
    std::string name, address;
    int port = kDefaultPort;
    int remotePort = 0;   // Hydra Remote (web control) port, 0 = off or an older Hydra
    double secondsAgo = 0;
    uint64_t deviceId = 0;
    bool trusted = false;
    PairState pairing = PairState::none;
    std::string remoteCode;   // Hydra Remote's access code, given by the Hydra when it allowed this phone
};

struct Stream
{
    std::string stream, sender, address;
    Codec codec = Codec::pcm24;
    int channels = 2, frames = 0, kbps = 0;
    double rate = 48000;
    double secondsAgo = 0;
};

struct RxStats
{
    bool listening = false, receiving = false;   // a stream is chosen / its packets arrive
    std::string stream, sender;
    Codec codec = Codec::pcm24;
    double rate = 0, bufferMs = 0, jitterMs = 0, lossPercent = 0;
    uint32_t received = 0, lost = 0;
    int underruns = 0;
    float peak = 0;   // since the last call
    std::string hint; // why packets were ignored lately (network password, other version), empty if none
};

struct TxStats
{
    bool sending = false, reachable = false;   // reachable: the Hydra announces itself / answers
    std::string to, address;
    Codec codec = Codec::pcm24;
    double lossPercent = 0, bufferMs = 0;      // what Hydra reports (Feedback), < 0 if nothing heard
    bool heard = false;
    float peak = 0;   // since the last call
};

struct SendSettings
{
    std::string host;          // a Hydra's announced name or an IPv4 address
    int port = kDefaultPort;
    std::string stream = "Phone";
    Codec codec = Codec::automatic;   // automatic = PCM 24
    int kbps = 128;
    int channels = 1;          // 1 = mono microphone
};

class Endpoint
{
public:
    Endpoint();
    ~Endpoint();

    bool start (const Config&, std::string& error);
    void stop();
    bool isRunning() const noexcept;
    std::string getName() const;

    std::vector<Peer> getPeers() const;
    std::vector<Stream> getStreams() const;
    /** Grows when the lists above change. */
    uint32_t getChangeCount() const noexcept;

    //--------------------------------------------------------------------------
    void setTrusted (const std::vector<Trusted>&);
    std::vector<Trusted> getTrusted() const;
    uint32_t getTrustVersion() const noexcept;
    std::vector<PairRequest> getRequests() const;
    void answer (uint64_t deviceId, bool allow);
    /** Asks a Hydra (by its announced name) to pair, once per second until it answers (2 min at most).
        Asking a Hydra that is paired already returns at once (and gives the current remote code). */
    void requestPairing (const std::string& hydraName);
    void forget (uint64_t deviceId);

    //--------------------------------------------------------------------------
    /** Listen to "<stream>@<sender>" (bound by name: follows a restart of Hydra). Empty stream = stop.
        stream "*": whatever stream that sender sends to this phone (the most recent one). */
    void listen (const std::string& stream, const std::string& sender);
    /** The output device: its rate and burst size (frames per callback). Before pulling; again when it changes. */
    void setOutputFormat (double rate, int burst);
    /** Output callback: interleaved stereo, frames frames (silence while nothing plays). Wait-free. */
    void pull (float* interleaved, int frames) noexcept;
    RxStats getRxStats();
    std::atomic<float> playGain { 1.0f };
    std::atomic<bool> playMuted { false };

    //--------------------------------------------------------------------------
    /** Starts sending to a Hydra. inputRate: the microphone's rate (Opus needs 48000). */
    bool startSending (const SendSettings&, double inputRate, std::string& error);
    void stopSending();
    /** Microphone callback: interleaved, `channels` channels. Wait-free. */
    void push (const float* interleaved, int channels, int frames) noexcept;
    TxStats getTxStats();
    std::atomic<float> micGain { 1.0f };
    /** Sends silence (the stream stays, Hydra's input keeps running); the level meter still shows the microphone. */
    std::atomic<bool> micMuted { false };

    struct Impl;

private:
    std::unique_ptr<Impl> impl;
};
} // namespace hydra::lite

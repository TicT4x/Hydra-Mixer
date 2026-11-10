// JNI bridge for de.hydra.link.NativeLink: one Hydra Link endpoint + the phone's audio for the whole process.
#include "audio.h"
#include <jni.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>

using namespace hydra;

namespace
{
std::mutex lock;
lite::Endpoint endpoint;
app::Audio audio { endpoint };

std::string str (JNIEnv* env, jstring s)
{
    if (s == nullptr) return {};
    const char* c = env->GetStringUTFChars (s, nullptr);
    std::string out (c != nullptr ? c : "");
    if (c != nullptr) env->ReleaseStringUTFChars (s, c);
    return out;
}

jstring jstr (JNIEnv* env, const std::string& s) { return env->NewStringUTF (s.c_str()); }

std::string esc (const std::string& s)
{
    std::string o = "\"";
    for (unsigned char c : s)
    {
        if (c == '"' || c == '\\') { o += '\\'; o += (char) c; }
        else if (c < 0x20) { char b[8]; std::snprintf (b, sizeof (b), "\\u%04x", c); o += b; }
        else o += (char) c;
    }
    return o + "\"";
}

std::string num (double v)
{
    char b[32];
    std::snprintf (b, sizeof (b), "%.3f", v);
    return b;
}

/** Device ids as 16 hex digits (as Hydra's net::deviceIdText). */
std::string hex (uint64_t id)
{
    char b[24];
    std::snprintf (b, sizeof (b), "%016llx", (unsigned long long) id);
    return b;
}

uint64_t parseHex (const std::string& s) { return (uint64_t) std::strtoull (s.c_str(), nullptr, 16); }

const char* b (bool v) { return v ? "true" : "false"; }
} // namespace

extern "C"
{
/** Starts the endpoint (announces the phone). deviceId: 16 hex digits (persistent, made by the app).
    trusted: the paired Hydras, one "id|name" per line. Returns an error text or "". */
JNIEXPORT jstring JNICALL Java_de_hydra_link_NativeLink_start (JNIEnv* env, jclass, jstring name, jint port, jstring deviceId, jstring trusted)
{
    std::lock_guard<std::mutex> l (lock);
    lite::Config c;
    c.name = str (env, name);
    c.port = port;
    c.deviceId = parseHex (str (env, deviceId));
    std::string err;
    if (endpoint.isRunning() && endpoint.getName() == c.name) return jstr (env, "");
    audio.stopOutput();
    audio.stopInput();
    endpoint.start (c, err);
    std::vector<lite::Trusted> list;
    const auto t = str (env, trusted);
    size_t pos = 0;
    while (pos < t.size())
    {
        auto end = t.find ('\n', pos);
        if (end == std::string::npos) end = t.size();
        const auto line = t.substr (pos, end - pos);
        if (const auto bar = line.find ('|'); bar != std::string::npos)
            list.push_back ({ parseHex (line.substr (0, bar)), line.substr (bar + 1) });
        pos = end + 1;
    }
    endpoint.setTrusted (list);
    return jstr (env, err);
}

JNIEXPORT void JNICALL Java_de_hydra_link_NativeLink_stop (JNIEnv*, jclass)
{
    std::lock_guard<std::mutex> l (lock);
    audio.stopOutput();
    audio.stopInput();
    endpoint.stop();
}

/** Listen to a stream (starts the audio output); "*" = whatever this sender sends to the phone; empty = stop. */
JNIEXPORT jstring JNICALL Java_de_hydra_link_NativeLink_listen (JNIEnv* env, jclass, jstring stream, jstring sender)
{
    std::lock_guard<std::mutex> l (lock);
    const auto s = str (env, stream);
    std::string err;
    if (s.empty())
    {
        endpoint.listen ({}, {});
        audio.stopOutput();
        return jstr (env, "");
    }
    if (! audio.startOutput (err)) return jstr (env, err);
    endpoint.listen (s, str (env, sender));
    return jstr (env, "");
}

/** Start sending the microphone. codec: 0 auto (PCM 24), 1 PCM 16, 2 PCM 24, 4 Opus. */
JNIEXPORT jstring JNICALL Java_de_hydra_link_NativeLink_send (JNIEnv* env, jclass, jstring host, jint port, jstring stream,
                                                              jint codec, jint kbps, jint channels, jint preset)
{
    std::lock_guard<std::mutex> l (lock);
    std::string err;
    if (! audio.startInput (channels, preset, err)) return jstr (env, err);
    lite::SendSettings s;
    s.host = str (env, host);
    s.port = port;
    s.stream = str (env, stream);
    s.codec = (lite::Codec) codec;
    s.kbps = kbps;
    s.channels = channels;
    if (! endpoint.startSending (s, audio.inRate, err))
    {
        audio.stopInput();
        return jstr (env, err);
    }
    return jstr (env, "");
}

JNIEXPORT void JNICALL Java_de_hydra_link_NativeLink_stopSending (JNIEnv*, jclass)
{
    std::lock_guard<std::mutex> l (lock);
    endpoint.stopSending();
    audio.stopInput();
}

JNIEXPORT void JNICALL Java_de_hydra_link_NativeLink_setGains (JNIEnv*, jclass, jfloat play, jfloat mic)
{
    endpoint.playGain.store (play);
    endpoint.micGain.store (mic);
}

/** Muted playback = silence out (the stream stays bound); muted microphone = Hydra gets silence (its input keeps running). */
JNIEXPORT void JNICALL Java_de_hydra_link_NativeLink_setMuted (JNIEnv*, jclass, jboolean play, jboolean mic)
{
    endpoint.playMuted.store (play);
    endpoint.micMuted.store (mic);
}

/** Pairing: ask a Hydra (by name), answer a Hydra that asks, unpair. */
JNIEXPORT void JNICALL Java_de_hydra_link_NativeLink_connect (JNIEnv* env, jclass, jstring hydra)
{
    endpoint.requestPairing (str (env, hydra));
}

JNIEXPORT void JNICALL Java_de_hydra_link_NativeLink_answer (JNIEnv* env, jclass, jstring id, jboolean allow)
{
    endpoint.answer (parseHex (str (env, id)), allow);
}

JNIEXPORT void JNICALL Java_de_hydra_link_NativeLink_forget (JNIEnv* env, jclass, jstring id)
{
    endpoint.forget (parseHex (str (env, id)));
}

/** Everything the UI shows, as JSON (polled ~10x per second). */
JNIEXPORT jstring JNICALL Java_de_hydra_link_NativeLink_state (JNIEnv* env, jclass)
{
    std::lock_guard<std::mutex> l (lock);
    std::string j = "{\"running\":";
    j += b (endpoint.isRunning());
    j += ",\"name\":" + esc (endpoint.getName());
    j += ",\"changes\":" + std::to_string (endpoint.getChangeCount());
    j += ",\"trustVersion\":" + std::to_string (endpoint.getTrustVersion());
    j += ",\"peers\":[";
    bool first = true;
    for (auto& p : endpoint.getPeers())
    {
        if (! first) j += ",";
        first = false;
        j += "{\"name\":" + esc (p.name) + ",\"address\":" + esc (p.address) + ",\"port\":" + std::to_string (p.port)
           + ",\"remote\":" + std::to_string (p.remotePort) + ",\"age\":" + num (p.secondsAgo)
           + ",\"id\":" + esc (p.deviceId != 0 ? hex (p.deviceId) : std::string()) + ",\"trusted\":" + b (p.trusted)
           + ",\"pair\":" + std::to_string ((int) p.pairing) + ",\"code\":" + esc (p.remoteCode) + "}";
    }
    j += "],\"trusted\":[";
    first = true;
    for (auto& t : endpoint.getTrusted())
    {
        if (! first) j += ",";
        first = false;
        j += "{\"id\":" + esc (hex (t.id)) + ",\"name\":" + esc (t.name) + "}";
    }
    j += "],\"requests\":[";
    first = true;
    for (auto& r : endpoint.getRequests())
    {
        if (! first) j += ",";
        first = false;
        j += "{\"id\":" + esc (hex (r.id)) + ",\"name\":" + esc (r.name) + ",\"address\":" + esc (r.address) + "}";
    }
    j += "],\"streams\":[";
    first = true;
    for (auto& s : endpoint.getStreams())
    {
        if (! first) j += ",";
        first = false;
        j += "{\"stream\":" + esc (s.stream) + ",\"sender\":" + esc (s.sender) + ",\"address\":" + esc (s.address)
           + ",\"codec\":" + esc (lite::codecName (s.codec)) + ",\"channels\":" + std::to_string (s.channels)
           + ",\"kbps\":" + std::to_string (s.kbps) + ",\"rate\":" + num (s.rate) + ",\"age\":" + num (s.secondsAgo) + "}";
    }
    const auto rx = endpoint.getRxStats();
    j += "],\"rx\":{\"listening\":" + std::string (b (rx.listening))
       + ",\"receiving\":" + b (rx.receiving)
       + ",\"stream\":" + esc (rx.stream) + ",\"sender\":" + esc (rx.sender)
       + ",\"codec\":" + esc (lite::codecName (rx.codec)) + ",\"rate\":" + num (rx.rate)
       + ",\"buffer\":" + num (rx.bufferMs) + ",\"jitter\":" + num (rx.jitterMs) + ",\"loss\":" + num (rx.lossPercent)
       + ",\"underruns\":" + std::to_string (rx.underruns) + ",\"peak\":" + num (rx.peak) + ",\"hint\":" + esc (rx.hint)
       + ",\"device\":" + num (audio.isOutputRunning() ? 1000.0 * audio.outBufferFrames / std::max (8000, audio.outRate) : 0.0)
       + ",\"deviceId\":" + std::to_string (audio.isOutputRunning() ? audio.outDevice : 0)
       + ",\"muted\":" + b (endpoint.playMuted.load())
       + ",\"exclusive\":" + b (audio.outExclusive) + "}";
    const auto tx = endpoint.getTxStats();
    j += ",\"tx\":{\"sending\":" + std::string (b (tx.sending))
       + ",\"to\":" + esc (tx.to) + ",\"address\":" + esc (tx.address) + ",\"reachable\":" + b (tx.reachable)
       + ",\"codec\":" + esc (lite::codecName (tx.codec)) + ",\"heard\":" + b (tx.heard)
       + ",\"loss\":" + num (tx.lossPercent) + ",\"buffer\":" + num (tx.bufferMs) + ",\"peak\":" + num (tx.peak)
       + ",\"device\":" + num (audio.isInputRunning() ? 1000.0 * audio.inBurst / std::max (8000, audio.inRate) : 0.0)
       + ",\"deviceId\":" + std::to_string (audio.isInputRunning() ? audio.inDevice : 0)
       + ",\"muted\":" + b (endpoint.micMuted.load())
       + ",\"exclusive\":" + b (audio.inExclusive) + "}}";
    return jstr (env, j);
}
} // extern "C"

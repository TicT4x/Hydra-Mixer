// JNI bridge for de.hydra.link.NativeLink: one Hydra Link endpoint + the phone's audio for the whole process.
#include "audio.h"
#include <jni.h>
#include <algorithm>
#include <cstdio>
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
} // namespace

extern "C"
{
JNIEXPORT jstring JNICALL Java_de_hydra_link_NativeLink_start (JNIEnv* env, jclass, jstring name, jint port)
{
    std::lock_guard<std::mutex> l (lock);
    lite::Config c;
    c.name = str (env, name);
    c.port = port;
    std::string err;
    if (endpoint.isRunning() && endpoint.getName() == c.name) return jstr (env, "");
    audio.stopOutput();
    audio.stopInput();
    endpoint.start (c, err);
    return jstr (env, err);
}

JNIEXPORT void JNICALL Java_de_hydra_link_NativeLink_stop (JNIEnv*, jclass)
{
    std::lock_guard<std::mutex> l (lock);
    audio.stopOutput();
    audio.stopInput();
    endpoint.stop();
}

/** Listen to a stream (starts the audio output); empty stream = stop listening. Returns an error or "". */
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

/** Everything the UI shows, as JSON (polled ~10x per second). */
JNIEXPORT jstring JNICALL Java_de_hydra_link_NativeLink_state (JNIEnv* env, jclass)
{
    std::lock_guard<std::mutex> l (lock);
    std::string j = "{\"running\":";
    j += endpoint.isRunning() ? "true" : "false";
    j += ",\"name\":" + esc (endpoint.getName());
    j += ",\"changes\":" + std::to_string (endpoint.getChangeCount());
    j += ",\"peers\":[";
    bool first = true;
    for (auto& p : endpoint.getPeers())
    {
        if (! first) j += ",";
        first = false;
        j += "{\"name\":" + esc (p.name) + ",\"address\":" + esc (p.address) + ",\"port\":" + std::to_string (p.port) + ",\"remote\":" + std::to_string (p.remotePort) + ",\"age\":" + num (p.secondsAgo) + "}";
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
    j += "],\"rx\":{\"listening\":" + std::string (rx.listening ? "true" : "false")
       + ",\"receiving\":" + (rx.receiving ? "true" : "false")
       + ",\"stream\":" + esc (rx.stream) + ",\"sender\":" + esc (rx.sender)
       + ",\"codec\":" + esc (lite::codecName (rx.codec)) + ",\"rate\":" + num (rx.rate)
       + ",\"buffer\":" + num (rx.bufferMs) + ",\"jitter\":" + num (rx.jitterMs) + ",\"loss\":" + num (rx.lossPercent)
       + ",\"underruns\":" + std::to_string (rx.underruns) + ",\"peak\":" + num (rx.peak) + ",\"hint\":" + esc (rx.hint)
       + ",\"device\":" + num (audio.isOutputRunning() ? 1000.0 * audio.outBufferFrames / std::max (8000, audio.outRate) : 0.0)
       + ",\"exclusive\":" + (audio.outExclusive ? "true" : "false") + "}";
    const auto tx = endpoint.getTxStats();
    j += ",\"tx\":{\"sending\":" + std::string (tx.sending ? "true" : "false")
       + ",\"to\":" + esc (tx.to) + ",\"address\":" + esc (tx.address) + ",\"reachable\":" + (tx.reachable ? "true" : "false")
       + ",\"codec\":" + esc (lite::codecName (tx.codec)) + ",\"heard\":" + (tx.heard ? "true" : "false")
       + ",\"loss\":" + num (tx.lossPercent) + ",\"buffer\":" + num (tx.bufferMs) + ",\"peak\":" + num (tx.peak)
       + ",\"device\":" + num (audio.isInputRunning() ? 1000.0 * audio.inBurst / std::max (8000, audio.inRate) : 0.0)
       + ",\"exclusive\":" + (audio.inExclusive ? "true" : "false") + "}}";
    return jstr (env, j);
}
} // extern "C"

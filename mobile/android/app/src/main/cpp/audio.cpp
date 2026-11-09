#include "audio.h"
#include <android/log.h>
#include <thread>

#define LOGI(...) __android_log_print (ANDROID_LOG_INFO, "HydraLink", __VA_ARGS__)

namespace hydra::app
{
AAudioStream* Audio::open (bool output, int channels, int preset, std::string& error, bool& exclusive)
{
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        AAudioStreamBuilder* b = nullptr;
        if (AAudio_createStreamBuilder (&b) != AAUDIO_OK) { error = "AAudio is not available"; return nullptr; }
        AAudioStreamBuilder_setDirection (b, output ? AAUDIO_DIRECTION_OUTPUT : AAUDIO_DIRECTION_INPUT);
        AAudioStreamBuilder_setPerformanceMode (b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        // Exclusive first (lowest latency, MMAP path); some phones refuse it: then shared.
        AAudioStreamBuilder_setSharingMode (b, attempt == 0 ? AAUDIO_SHARING_MODE_EXCLUSIVE : AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setFormat (b, AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setChannelCount (b, channels);
        AAudioStreamBuilder_setSampleRate (b, 48000);   // Opus and Hydra's default; AAudio converts if the hardware differs
        if (output)
        {
            AAudioStreamBuilder_setUsage (b, AAUDIO_USAGE_MEDIA);
            AAudioStreamBuilder_setContentType (b, AAUDIO_CONTENT_TYPE_MUSIC);
            AAudioStreamBuilder_setDataCallback (b, outCallback, this);
        }
        else
        {
            AAudioStreamBuilder_setInputPreset (b, preset);
            AAudioStreamBuilder_setDataCallback (b, inCallback, this);
        }
        AAudioStreamBuilder_setErrorCallback (b, errorCallback, this);
        AAudioStream* s = nullptr;
        const auto r = AAudioStreamBuilder_openStream (b, &s);
        AAudioStreamBuilder_delete (b);
        if (r == AAUDIO_OK && s != nullptr)
        {
            exclusive = AAudioStream_getSharingMode (s) == AAUDIO_SHARING_MODE_EXCLUSIVE;
            return s;
        }
        error = std::string ("Audio could not be opened: ") + AAudio_convertResultToText (r);
    }
    return nullptr;
}

bool Audio::startOutput (std::string& error)
{
    std::lock_guard<std::recursive_mutex> l (lock);
    if (out != nullptr) return true;
    out = open (true, 2, 0, error, outExclusive);
    if (out == nullptr) return false;
    outRate = AAudioStream_getSampleRate (out);
    outBurst = AAudioStream_getFramesPerBurst (out);
    // Two bursts: the usual low-latency setting; grows by one burst after each underrun (see outCallback).
    outBufferFrames = AAudioStream_setBufferSizeInFrames (out, outBurst * 2);
    endpoint.setOutputFormat (outRate, outBurst);
    if (AAudioStream_requestStart (out) != AAUDIO_OK)
    {
        AAudioStream_close (out);
        out = nullptr;
        error = "Audio output could not start";
        return false;
    }
    LOGI ("output %d Hz, burst %d, buffer %d, %s", outRate, outBurst, outBufferFrames, outExclusive ? "exclusive" : "shared");
    return true;
}

void Audio::stopOutput()
{
    std::lock_guard<std::recursive_mutex> l (lock);
    if (out == nullptr) return;
    AAudioStream_requestStop (out);
    AAudioStream_close (out);
    out = nullptr;
}

bool Audio::startInput (int channels, int preset, std::string& error)
{
    std::lock_guard<std::recursive_mutex> l (lock);
    if (in != nullptr) return true;
    inChannels = channels;
    inPreset = preset;
    in = open (false, channels, preset, error, inExclusive);
    if (in == nullptr) return false;
    inRate = AAudioStream_getSampleRate (in);
    inBurst = AAudioStream_getFramesPerBurst (in);
    inChannels = AAudioStream_getChannelCount (in);
    if (AAudioStream_requestStart (in) != AAUDIO_OK)
    {
        AAudioStream_close (in);
        in = nullptr;
        error = "The microphone could not start";
        return false;
    }
    LOGI ("input %d Hz, burst %d, %d ch, %s", inRate, inBurst, inChannels, inExclusive ? "exclusive" : "shared");
    return true;
}

void Audio::stopInput()
{
    std::lock_guard<std::recursive_mutex> l (lock);
    if (in == nullptr) return;
    AAudioStream_requestStop (in);
    AAudioStream_close (in);
    in = nullptr;
}

aaudio_data_callback_result_t Audio::outCallback (AAudioStream* s, void* user, void* data, int32_t frames)
{
    auto* a = static_cast<Audio*> (user);
    a->endpoint.pull (static_cast<float*> (data), frames);
    // Underruns of the phone's own audio path: one burst more buffer each time (at most 8 bursts).
    const int x = AAudioStream_getXRunCount (s);
    if (x > a->outXruns.load (std::memory_order_relaxed))
    {
        a->outXruns.store (x, std::memory_order_relaxed);
        const int size = AAudioStream_getBufferSizeInFrames (s);
        if (size < a->outBurst * 8) a->outBufferFrames = AAudioStream_setBufferSizeInFrames (s, size + a->outBurst);
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

aaudio_data_callback_result_t Audio::inCallback (AAudioStream*, void* user, void* data, int32_t frames)
{
    auto* a = static_cast<Audio*> (user);
    a->endpoint.push (static_cast<const float*> (data), a->inChannels, frames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void Audio::errorCallback (AAudioStream* s, void* user, aaudio_result_t error)
{
    if (error != AAUDIO_ERROR_DISCONNECTED) return;
    auto* a = static_cast<Audio*> (user);
    // Reopen on another thread (the device changed: headphones, Bluetooth).
    std::thread ([a, s]
    {
        std::lock_guard<std::recursive_mutex> l (a->lock);
        std::string err;
        if (s == a->out) { a->stopOutput(); a->startOutput (err); }
        else if (s == a->in) { const int ch = a->inChannels, p = a->inPreset; a->stopInput(); a->startInput (ch, p, err); }
        if (! err.empty()) LOGI ("reopen failed: %s", err.c_str());
    }).detach();
}
} // namespace hydra::app

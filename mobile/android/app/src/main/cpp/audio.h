#pragma once
#include "linkcore/LinkLite.h"
#include <aaudio/AAudio.h>
#include <atomic>
#include <mutex>
#include <string>

/** Phone audio over AAudio (low-latency mode, float, 48 kHz): the output plays what Endpoint::pull gives, the
    input feeds Endpoint::push. A stream that gets disconnected (headphones unplugged, Bluetooth switched) is
    reopened from a helper thread (AAudio does not allow that inside its callbacks). */
namespace hydra::app
{
class Audio
{
public:
    explicit Audio (lite::Endpoint& e) : endpoint (e) {}
    ~Audio() { stopOutput(); stopInput(); }

    bool startOutput (std::string& error);
    void stopOutput();
    bool isOutputRunning() const { return out != nullptr; }

    /** preset: AAUDIO_INPUT_PRESET_* (VOICE_PERFORMANCE: no processing, low latency). */
    bool startInput (int channels, int preset, std::string& error);
    void stopInput();
    bool isInputRunning() const { return in != nullptr; }

    int outRate = 48000, outBurst = 192, outBufferFrames = 0, inRate = 48000, inBurst = 192;
    bool outExclusive = false, inExclusive = false;
    std::atomic<int> outXruns { 0 };

private:
    static aaudio_data_callback_result_t outCallback (AAudioStream*, void* user, void* data, int32_t frames);
    static aaudio_data_callback_result_t inCallback (AAudioStream*, void* user, void* data, int32_t frames);
    static void errorCallback (AAudioStream*, void* user, aaudio_result_t error);
    AAudioStream* open (bool output, int channels, int preset, std::string& error, bool& exclusive);

    lite::Endpoint& endpoint;
    std::recursive_mutex lock;
    AAudioStream* out = nullptr;
    AAudioStream* in = nullptr;
    int inChannels = 1, inPreset = 0;
};
} // namespace hydra::app

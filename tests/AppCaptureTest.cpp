// Per-app capture: this test process plays a -60 dBFS tone and captures itself via process loopback.
#include "engine/IoStreams.h"
#include <cstdio>

namespace
{
struct QuietTone : juce::AudioIODeviceCallback
{
    double phase = 0.0, inc = 0.0;
    void audioDeviceAboutToStart (juce::AudioIODevice* d) override { inc = juce::MathConstants<double>::twoPi * 440.0 / d->getCurrentSampleRate(); }
    void audioDeviceStopped() override {}
    void audioDeviceIOCallbackWithContext (const float* const*, int, float* const* out, int numOut, int n,
                                           const juce::AudioIODeviceCallbackContext&) override
    {
        for (int i = 0; i < n; ++i)
        {
            const float v = 0.001f * (float) std::sin (phase); // -60 dBFS
            phase += inc;
            for (int c = 0; c < numOut; ++c) if (out[c]) out[c][i] = v;
        }
    }
};
} // namespace

int runAppCaptureTest()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    int fails = 0;
    auto check = [&] (bool c, const char* what) { std::printf ("%s %s\n", c ? "ok:  " : "FAIL:", what); if (! c) ++fails; };

    juce::OwnedArray<juce::AudioIODeviceType> types;
    types.add (juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (juce::WASAPIDeviceMode::shared));
    types.add (juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (juce::WASAPIDeviceMode::sharedLowLatency));
    for (auto* t : types) t->scanForDevices();

    const auto cables = hydra::findVirtualCables (types);
    std::printf ("info: %d virtual cable(s) detected\n", (int) cables.size());
    for (auto& c : cables) std::printf ("info:   %s  [%s -> %s]\n", c.label.toRawUTF8(), c.playbackName.toRawUTF8(), c.recordingName.toRawUTF8());

    auto* shared = types[0];
    const auto outName = shared->getDeviceNames (false)[shared->getDefaultDeviceIndex (false)];
    std::unique_ptr<juce::AudioIODevice> out (shared->createDevice (outName, {}));
    QuietTone tone;
    juce::BigInteger chans; chans.setRange (0, 2, true);
    if (out == nullptr || out->open ({}, chans, 48000.0, out->getDefaultBufferSize()).isNotEmpty())
    {
        std::printf ("skip: cannot open default output for app-capture test\n");
        return 0;
    }
    out->start (&tone);

    hydra::IoTarget t;
    t.kind = hydra::IoTarget::Kind::App;
    t.deviceName = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFileName();
    juce::String err;
    auto in = hydra::openAsyncInput (t, types, 48000.0, 480, err);
    check (in != nullptr, "per-app capture stream opened");
    if (in == nullptr) { out->stop(); return fails; }
    in->start();

    const auto apps = hydra::listAudioApps();
    std::printf ("info: apps with audio sessions: %s\n", apps.joinIntoString (", ").toRawUTF8());
    check (! apps.contains (t.deviceName, true), "own process is excluded from the app list");

    double energy = 0.0;
    int frames = 0;
    for (int block = 0; block < 300; ++block) // 3 s of 10 ms engine blocks
    {
        juce::Thread::sleep (10);
        in->pullBlock (480);
        if (block < 100) continue; // allow activation + priming
        const float* l = in->channel (0);
        for (int i = 0; i < 480; ++i) energy += (double) l[i] * l[i];
        frames += 480;
    }
    out->stop();
    const double rmsDb = 10.0 * std::log10 (energy / juce::jmax (1, frames) + 1e-20);
    std::printf ("info: captured level %.1f dBFS (played -63 dBFS RMS), idle=%d, underruns=%d\n", rmsDb, (int) in->idle.load(), in->underruns.load());
    check (rmsDb > -75.0 && rmsDb < -50.0, "captured the app's own tone at the right level");
    in->stop();
    return fails;
}

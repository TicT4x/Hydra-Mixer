// Secondary ASIO drivers (AsioSecondary.cpp) against a simulated driver: own clock (+100 ppm),
// 64-frame blocks, 6 in / 6 out. Checks: one device per driver shared by input and output streams,
// channel mapping, drift without dropouts (simulated time, deterministic), attach/detach while a
// real driver thread runs, watchdog for a driver that stops calling back, device closed with its
// last stream. Then a matrix of block sizes / rates / drift: no dropouts once settled.
#include "engine/IoStreams.h"
#include <cstdio>
#include <thread>

namespace
{
struct FakeStats
{
    std::atomic<int> created { 0 }, alive { 0 };
};

class FakeAsioDevice final : public juce::AudioIODevice, private juce::Thread
{
public:
    /** threaded = own driver thread in real time; otherwise the test calls tick(). */
    FakeAsioDevice (const juce::String& n, FakeStats& st, double ppm, bool ownThread, int blockSize, double onlyRate)
        : AudioIODevice (n, "FakeASIO"), Thread ("Fake ASIO"), kBlock (blockSize), stats (st), clockPpm (ppm),
          threaded (ownThread), fixedRate (onlyRate)
    {
        ++stats.created;
        ++stats.alive;
    }
    ~FakeAsioDevice() override { close(); --stats.alive; }

    juce::StringArray getOutputChannelNames() override { return names ("Out"); }
    juce::StringArray getInputChannelNames() override  { return names ("In"); }
    juce::Array<double> getAvailableSampleRates() override
    {
        return fixedRate > 0 ? juce::Array<double> { fixedRate } : juce::Array<double> { 44100.0, 48000.0 };
    }
    juce::Array<int> getAvailableBufferSizes() override { return { kBlock }; }
    int getDefaultBufferSize() override { return kBlock; }

    juce::String open (const juce::BigInteger& ins, const juce::BigInteger& outs, double sr, int) override
    {
        activeIns = ins; activeOuts = outs; rate = fixedRate > 0 ? fixedRate : sr; opened = true;
        return {};
    }
    void close() override { stop(); opened = false; }
    bool isOpen() override { return opened; }
    void start (juce::AudioIODeviceCallback* cb) override
    {
        cb->audioDeviceAboutToStart (this);
        callback = cb;
        if (threaded) startThread (juce::Thread::Priority::highest);
    }
    void stop() override
    {
        stopThread (2000);
        if (auto* cb = std::exchange (callback, nullptr)) cb->audioDeviceStopped();
    }
    bool isPlaying() override { return callback != nullptr; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return kBlock; }
    double getCurrentSampleRate() override { return rate; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return activeOuts; }
    juce::BigInteger getActiveInputChannels() const override { return activeIns; }
    int getOutputLatencyInSamples() override { return 64; }
    int getInputLatencyInSamples() override { return 64; }

    std::atomic<bool> paused { false };
    std::array<std::atomic<float>, 6> lastOut {};
    const int kBlock;
    double periodMs() const { return 1000.0 * kBlock / (rate * (1.0 + clockPpm * 1.0e-6)); }

    /** One driver callback. */
    void tick()
    {
        if (callback == nullptr || paused.load()) return;
        if (in[0].empty())
            for (int c = 0; c < 6; ++c)
            {
                in[(size_t) c].assign (kBlock, 0.1f * (float) (c + 1)); // DC per channel: 0.1, 0.2, ...
                out[(size_t) c].assign (kBlock, 0.0f);
                inPtrs[c] = in[(size_t) c].data();
                outPtrs[c] = out[(size_t) c].data();
            }
        callback->audioDeviceIOCallbackWithContext (inPtrs, 6, outPtrs, 6, kBlock, {});
        for (int c = 0; c < 6; ++c) lastOut[(size_t) c].store (out[(size_t) c][kBlock - 1]);
    }

private:
    static juce::StringArray names (const char* prefix)
    {
        juce::StringArray s;
        for (int c = 0; c < 6; ++c) s.add (juce::String (prefix) + " " + juce::String (c + 1));
        return s;
    }

    void run() override
    {
        double next = juce::Time::getMillisecondCounterHiRes();
        while (! threadShouldExit())
        {
            if (juce::Time::getMillisecondCounterHiRes() < next) { juce::Thread::sleep (1); continue; }
            next += periodMs();
            tick();
        }
    }

    FakeStats& stats;
    const double clockPpm;
    const bool threaded;
    const double fixedRate;
    std::array<std::vector<float>, 6> in, out;
    const float* inPtrs[6] {};
    float* outPtrs[6] {};
    juce::AudioIODeviceCallback* callback = nullptr;
    juce::BigInteger activeIns, activeOuts;
    double rate = 48000.0;
    bool opened = false;
};

class FakeAsioType final : public juce::AudioIODeviceType
{
public:
    explicit FakeAsioType (FakeStats& s) : AudioIODeviceType ("FakeASIO"), stats (s) {}
    void scanForDevices() override {}
    juce::StringArray getDeviceNames (bool) const override { return { "Fake A", "Fake B", "Fake C" }; }
    int getDefaultDeviceIndex (bool) const override { return 0; }
    int getIndexOfDevice (juce::AudioIODevice*, bool) const override { return 0; }
    bool hasSeparateInputsAndOutputs() const override { return false; }
    juce::AudioIODevice* createDevice (const juce::String& out, const juce::String& in) override
    {
        const auto name = out.isNotEmpty() ? out : in;
        last = new FakeAsioDevice (name, stats, ppm, name == "Fake B", block, rate);
        return last;
    }
    FakeAsioDevice* last = nullptr;
    int block = 64;      // next device's buffer size
    double rate = 0.0;   // next device's only sample rate (0 = 44.1k or 48k)
    double ppm = 100.0;  // next device's clock error

private:
    FakeStats& stats;
};
} // namespace

int runAsioSecondaryTest()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    int fails = 0;
    auto check = [&] (bool c, const char* what) { std::printf ("%s %s\n", c ? "ok:  " : "FAIL:", what); if (! c) ++fails; };
    std::printf ("\n--- secondary ASIO driver ---\n");

    FakeStats stats;
    FakeAsioType type (stats);
    constexpr int block = 128;
    constexpr double rate = 48000.0;
    juce::String err;

    auto in1 = hydra::openAsioInput (type, "Fake A", rate, block, { 2, 3 }, err);
    auto out1 = hydra::openAsioOutput (type, "Fake A", rate, block, { 4, 1, 1 }, err);
    check (in1 != nullptr && out1 != nullptr && stats.created == 1, "input and output of one driver share one device");
    if (in1 == nullptr || out1 == nullptr) return fails + 1;
    auto* device = type.last;
    check (out1->deviceChannels == std::vector<int> ({ 1, 4 }) && out1->getNumChannels() == 2,
           "a stream carries only the routed channels (sorted, unique)");
    check (in1->localChannel (3) == 1 && in1->localChannel (0) == -1, "device channel -> stream channel mapping");
    check (in1->deviceLatencyFrames == 64, "driver latency is reported");
    in1->start();
    out1->start();

    // Simulated time: the master engine runs 128-frame blocks at exactly 48 kHz, the driver its
    // 64-frame blocks on a clock 100 ppm faster; callbacks interleave in time order.
    std::shared_ptr<hydra::AsyncInput> in2;
    float v2 = 0, v3 = 0, v0 = 0;
    double now = 0.0, nextDevice = 0.37, nextEngine = 0.0;
    auto runEngine = [&] (double ms)
    {
        const double end = now + ms;
        while (now < end)
        {
            if (nextDevice <= nextEngine)
            {
                now = nextDevice;
                nextDevice += device->periodMs();
                device->tick();
                continue;
            }
            now = nextEngine;
            nextEngine += 1000.0 * block / rate;
            if (in1 != nullptr)
            {
                in1->pullBlock (block);
                v2 = in1->channel (in1->localChannel (2))[block - 1];
                v3 = in1->channel (in1->localChannel (3))[block - 1];
            }
            if (in2 != nullptr)
            {
                in2->pullBlock (block);
                v0 = in2->channel (in2->localChannel (0))[block - 1];
            }
            out1->clearMix (block);
            juce::FloatVectorOperations::fill (out1->mixChannel (out1->localChannel (1)), 0.3f, block);
            juce::FloatVectorOperations::fill (out1->mixChannel (out1->localChannel (4)), 0.6f, block);
            out1->pushBlock (block);
        }
    };

    runEngine (2000.0);
    const int inU = in1->underruns.load(), outU = out1->underruns.load();
    runEngine (20000.0);
    std::printf ("info: underruns in %d (+%d), out %d (+%d); buffered in %.2f ms, out %.2f ms\n",
                 inU, in1->underruns.load() - inU, outU, out1->underruns.load() - outU,
                 1000.0 * in1->latencyFrames.load() / rate, 1000.0 * out1->latencyFrames.load() / rate);
    check (in1->underruns.load() == inU && out1->underruns.load() == outU, "+100 ppm clock drift: no dropouts in 20 s once settled");
    check (std::abs (v2 - 0.3f) < 1.0e-3f && std::abs (v3 - 0.4f) < 1.0e-3f, "inputs read the right device channels");
    check (std::abs (device->lastOut[1].load() - 0.3f) < 1.0e-3f && std::abs (device->lastOut[4].load() - 0.6f) < 1.0e-3f
               && device->lastOut[0].load() == 0.0f && device->lastOut[2].load() == 0.0f,
           "outputs land on the right device channels, others silent");

    // A second input stream (other channel set) joins the running driver, then the first leaves.
    in2 = hydra::openAsioInput (type, "Fake A", rate, block, { 0, 3 }, err);
    check (in2 != nullptr && stats.created == 1, "further streams reuse the open driver");
    if (in2 == nullptr) return fails + 1;
    check (in2->carries ({ 3, 0 }) && ! in2->carries ({ 5 }) && in2->carries ({ 40 }), "carries(): routed vs. missing channels");
    in2->start();
    runEngine (600.0);
    in1.reset(); // detaches while the driver keeps running
    runEngine (400.0);
    check (std::abs (v0 - 0.1f) < 1.0e-3f, "joined stream reads its channel");

    check (hydra::asioChannelNames (type, "Fake A", true).size() == 6 && stats.created == 1, "channel names of an open driver come from it");
    check (hydra::asioChannelNames (type, "Fake C", false).size() == 6 && stats.alive == 1, "other drivers are probed and released");

    {   // A real driver thread: streams attach and detach while it calls back.
        auto keep = hydra::openAsioInput (type, "Fake B", rate, block, { 0, 1 }, err);
        check (keep != nullptr && stats.alive == 2, "a second ASIO driver runs next to the first");
        if (keep == nullptr) return fails + 1;
        keep->start();
        auto* threaded = type.last;
        for (int i = 0; i < 200; ++i)
        {
            auto s = hydra::openAsioOutput (type, "Fake B", rate, block, { i % 6 }, err);
            if (s == nullptr) { ++fails; break; }
            s->start();
            if (i % 20 == 0) juce::Thread::sleep (3);
            keep->pullBlock (block);
        }
        juce::Thread::sleep (20);
        check (stats.alive == 2 && threaded->isPlaying(), "200 x attach/detach during callbacks (no crash, driver keeps running)");
    }
    check (stats.alive == 1, "second driver closed with its last stream");

    auto bad = hydra::openAsioInput (type, "Fake A", rate, block, { 40 }, err);
    check (bad == nullptr && err.isNotEmpty(), "a channel the driver does not have is refused");

    // Driver stops calling back (unplugged): the watchdog fails the streams.
    device->paused = true;
    for (int i = 0; i < 25 && ! in2->failed.load(); ++i)
    {
        juce::Thread::sleep (100);
        in2->poll();
    }
    check (in2->failed.load() && out1->failed.load(), "watchdog fails all streams of a silent driver");
    err.clear();
    check (hydra::openAsioInput (type, "Fake A", rate, block, { 0 }, err) == nullptr && err.contains ("does not respond"),
           "a driver that died right after opening is not reloaded in a loop");

    in2.reset();
    out1.reset();
    check (stats.alive == 0, "driver closed with its last stream");

    // Drift matrix: device block / engine block / device rate / clock error, 20 s simulated each.
    // Includes the hard cases: whole-number block ratios (clock slips are rare there) and device
    // blocks shorter than the engine's.
    int badConfigs = 0, configs = 0;
    double worstIn = 0, worstOut = 0;
    for (int devBlock : { 32, 64, 128, 480 })
        for (int engBlock : { 64, 128, 512 })
            for (double devRate : { 48000.0, 44100.0 })
                for (double ppm : { -100.0, 100.0 })
                {
                    type.block = devBlock; type.rate = devRate; type.ppm = ppm;
                    const auto name = "Matrix " + juce::String (++configs);
                    auto in = hydra::openAsioInput (type, name, rate, engBlock, { 0, 1 }, err);
                    auto out = hydra::openAsioOutput (type, name, rate, engBlock, { 0, 1 }, err);
                    if (in == nullptr || out == nullptr) { ++badConfigs; continue; }
                    in->start();
                    out->start();
                    auto* dev = type.last;
                    double t = 0.0, nextDev = 0.123, nextEng = 0.0;
                    int inU = 0, outU = 0;
                    bool settled = false;
                    while (t < 20000.0)
                    {
                        if (nextDev <= nextEng) { t = nextDev; nextDev += dev->periodMs(); dev->tick(); continue; }
                        t = nextEng;
                        nextEng += 1000.0 * engBlock / rate;
                        in->pullBlock (engBlock);
                        out->clearMix (engBlock);
                        out->pushBlock (engBlock);
                        if (! settled && t > 2000.0) { settled = true; inU = in->underruns.load(); outU = out->underruns.load(); }
                        if (settled)
                        {
                            worstIn = juce::jmax (worstIn, 1000.0 * in->latencyFrames.load() / devRate);
                            worstOut = juce::jmax (worstOut, 1000.0 * out->latencyFrames.load() / devRate);
                        }
                    }
                    if (in->underruns.load() != inU || out->underruns.load() != outU)
                    {
                        ++badConfigs;
                        std::printf ("info: dropouts with device %d @ %.0f Hz, engine %d, %+.0f ppm: in %d, out %d\n",
                                     devBlock, devRate, engBlock, ppm, in->underruns.load() - inU, out->underruns.load() - outU);
                    }
                }
    type.ppm = 100.0; type.block = 64; type.rate = 0.0;
    std::printf ("info: %d configurations, worst buffered latency in %.1f ms / out %.1f ms\n", configs, worstIn, worstOut);
    check (badConfigs == 0, "drift matrix (block sizes 32..480, 44.1k/48k, +-100 ppm): no dropouts once settled");
    return fails;
}

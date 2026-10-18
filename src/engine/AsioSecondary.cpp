#include "IoStreams.h"

#include <map>
#include <thread>

namespace hydra
{
namespace
{
class AsioInput;
class AsioOutput;

double pickAsioRate (juce::AudioIODevice& d, double preferred)
{
    auto rates = d.getAvailableSampleRates();
    if (rates.contains (preferred)) return preferred;
    const double cur = d.getCurrentSampleRate();
    if (cur > 0) return cur;
    return rates.isEmpty() ? preferred : rates.getLast();
}

/** Sorted, unique channels that exist on the device (at most kMaxStreamChannels). */
std::vector<int> existingChannels (std::vector<int> wanted, int deviceChannels)
{
    std::sort (wanted.begin(), wanted.end());
    wanted.erase (std::unique (wanted.begin(), wanted.end()), wanted.end());
    std::vector<int> result;
    for (int c : wanted)
        if (c >= 0 && c < deviceChannels && (int) result.size() < kMaxStreamChannels)
            result.push_back (c);
    return result;
}

//==============================================================================
/** One secondary ASIO driver. ASIO has a single device object for inputs and outputs and a
    driver can only be loaded once per process, so the driver is opened once with all of its
    channels and the engine's input/output streams for it attach here. The device runs on its
    own clock; each attached stream has its own ring + resampler + drift controller.

    Attach/detach never blocks the device thread: a slot is cleared, then the message thread
    waits until a callback that may still be using it has returned. */
class AsioDuplex final : private juce::AudioIODeviceCallback
{
public:
    static constexpr int kSlots = 8; // streams per direction (old + new graph during a rebuild)

    explicit AsioDuplex (juce::String driverName) : name (std::move (driverName)) {}
    ~AsioDuplex() override { close(); }

    bool open (juce::AudioIODeviceType& type, double engRate, juce::String& error)
    {
        close();
        device.reset (type.createDevice (name, name));
        if (device == nullptr) { error = "Cannot load ASIO driver " + name; return false; }

        numIns  = juce::jmin (kMaxDeviceChannels, device->getInputChannelNames().size());
        numOuts = juce::jmin (kMaxDeviceChannels, device->getOutputChannelNames().size());
        if (numIns + numOuts == 0)
        {
            device.reset();
            error = name + " reports no channels (in use by another program?)";
            return false;
        }

        // All channels stay open, so adding a route later never restarts the driver.
        // Buffer size = the one set in the driver's own control panel.
        juce::BigInteger ins, outs;
        ins.setRange (0, numIns, true);
        outs.setRange (0, numOuts, true);
        error = device->open (ins, outs, pickAsioRate (*device, engRate), device->getDefaultBufferSize());
        if (error.isNotEmpty()) { device.reset(); return false; }

        rate = device->getCurrentSampleRate();
        block = juce::jmax (16, device->getCurrentBufferSizeSamples());
        inLatency = device->getInputLatencyInSamples();
        outLatency = device->getOutputLatencyInSamples();
        inNames = device->getInputChannelNames();
        outNames = device->getOutputChannelNames();
        engineRate = engRate;

        scratchSize = juce::jmax (1024, block);
        for (int c = 0; c < kMaxStreamChannels; ++c)
        {
            scratch[(size_t) c].assign ((size_t) scratchSize, 0.0f);
            scratchPtrs[c] = scratch[(size_t) c].data();
        }

        dead.store (false);
        lastCount = callbacks.load();
        lastProgress = openedAt = juce::Time::getMillisecondCounter();
        device->start (this);
        return true;
    }

    void close()
    {
        if (device == nullptr) return;
        device->stop(); // no callback runs after this returns
        device->close();
        device.reset();
    }

    bool isUsable (double engRate) const noexcept { return device != nullptr && ! dead.load() && engRate == engineRate; }
    bool isOpen() const noexcept { return device != nullptr; }
    /** A driver that died again within 10 s of being opened is not reopened automatically
        (it loads but never calls back): avoids reloading it every few seconds. */
    bool mayReopen() const noexcept
    {
        return device == nullptr || ! dead.load() || juce::Time::getMillisecondCounter() - openedAt > 10000;
    }

    double getRate() const noexcept     { return rate; }
    int getBlock() const noexcept       { return block; }
    int getNumInputs() const noexcept   { return numIns; }
    int getNumOutputs() const noexcept  { return numOuts; }
    int getInputLatency() const noexcept  { return inLatency; }
    int getOutputLatency() const noexcept { return outLatency; }
    const juce::StringArray& getNames (bool inputs) const noexcept { return inputs ? inNames : outNames; }

    bool attach (AsioInput* s)  { return attachTo (inSlots, s); }
    bool attach (AsioOutput* s) { return attachTo (outSlots, s); }
    void detach (AsioInput* s)  { detachFrom (inSlots, s); }
    void detach (AsioOutput* s) { detachFrom (outSlots, s); }

    /** Message thread: detects a driver that stopped calling back (unplugged) and fails the
        attached streams, so the engine's recovery reopens the driver. */
    void watchdog();
    void failAttached();

private:
    template <typename T>
    static bool attachTo (std::array<std::atomic<T*>, kSlots>& slots, T* s)
    {
        for (auto& p : slots) if (p.load() == s) return true;
        for (auto& p : slots) if (p.load() == nullptr) { p.store (s); return true; }
        return false;
    }

    template <typename T>
    void detachFrom (std::array<std::atomic<T*>, kSlots>& slots, T* s)
    {
        bool found = false;
        for (auto& p : slots)
            if (p.load() == s) { p.store (nullptr); found = true; }
        // seq_cst pairs with the callback (busy = true, then load slots): either it saw the
        // cleared slot, or we see it busy and wait until it returned.
        if (found)
            while (busy.load())
                std::this_thread::yield();
    }

    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                           int n, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* d) override
    {
        // The driver restarted itself (reset request from its control panel). A new rate or
        // buffer size invalidates the streams' rings: let the engine rebuild them.
        if (d->getCurrentSampleRate() != rate || juce::jmax (16, d->getCurrentBufferSizeSamples()) != block)
            dead.store (true);
    }
    void audioDeviceStopped() override {}
    void audioDeviceError (const juce::String&) override { dead.store (true); }

    const juce::String name;
    std::unique_ptr<juce::AudioIODevice> device;
    double rate = 48000.0, engineRate = 48000.0;
    int block = 256, numIns = 0, numOuts = 0, inLatency = 0, outLatency = 0;
    juce::StringArray inNames, outNames;

    std::array<std::atomic<AsioInput*>, kSlots> inSlots {};
    std::array<std::atomic<AsioOutput*>, kSlots> outSlots {};
    std::atomic<bool> busy { false }, dead { false };
    std::atomic<uint64_t> callbacks { 0 };
    uint64_t lastCount = 0;
    juce::uint32 lastProgress = 0, openedAt = 0;

    std::array<std::vector<float>, kMaxStreamChannels> scratch;
    float* scratchPtrs[kMaxStreamChannels] {};
    int scratchSize = 0;
};

//==============================================================================
class AsioInput final : public AsyncInput
{
public:
    AsioInput (std::shared_ptr<AsioDuplex> d, std::vector<int> chans, double engRate, int engBlock)
        : duplex (std::move (d))
    {
        deviceChannels = std::move (chans);
        deviceChannelCount = duplex->getNumInputs();
        deviceLatencyFrames = duplex->getInputLatency();
        setup (duplex->getRate(), duplex->getBlock(), (int) deviceChannels.size(), engRate, engBlock);
    }

    ~AsioInput() override { stop(); }

    void start() override
    {
        if (! duplex->attach (this)) { error = "Too many streams on one ASIO driver"; failed = true; }
    }
    void stop() override { duplex->detach (this); }
    void poll() override { duplex->watchdog(); }

    /** Device thread. */
    void fromDevice (const float* const* in, int numIn, int n) noexcept
    {
        const float* src[kMaxStreamChannels] {};
        for (int c = 0; c < channels; ++c)
        {
            const int d = deviceChannels[(size_t) c];
            src[c] = d < numIn ? in[d] : nullptr;
        }
        produce (src, channels, n);
    }

private:
    std::shared_ptr<AsioDuplex> duplex;
};

class AsioOutput final : public AsyncOutput
{
public:
    AsioOutput (std::shared_ptr<AsioDuplex> d, std::vector<int> chans, double engRate, int engBlock)
        : duplex (std::move (d))
    {
        deviceChannels = std::move (chans);
        deviceChannelCount = duplex->getNumOutputs();
        deviceLatencyFrames = duplex->getOutputLatency();
        setup (duplex->getRate(), duplex->getBlock(), (int) deviceChannels.size(), engRate, engBlock);
    }

    ~AsioOutput() override { stop(); }

    void start() override
    {
        if (! duplex->attach (this)) { error = "Too many streams on one ASIO driver"; failed = true; }
    }
    void stop() override { duplex->detach (this); }
    void poll() override { duplex->watchdog(); }

    /** Device thread: adds this stream into the driver's (already cleared) output buffers. */
    void toDevice (float* const* out, int numOut, int n, float* const* scratch, int scratchSize) noexcept
    {
        for (int done = 0; done < n;)
        {
            const int k = juce::jmin (n - done, scratchSize);
            consume (scratch, channels, k);
            for (int c = 0; c < channels; ++c)
            {
                const int d = deviceChannels[(size_t) c];
                if (d < numOut && out[d] != nullptr)
                    juce::FloatVectorOperations::add (out[d] + done, scratch[c], k);
            }
            done += k;
        }
    }

private:
    std::shared_ptr<AsioDuplex> duplex;
};

//==============================================================================
void AsioDuplex::audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                                   int n, const juce::AudioIODeviceCallbackContext&)
{
    const juce::ScopedNoDenormals noDenormals;
    busy.store (true);

    for (int c = 0; c < numOut; ++c)
        if (out[c] != nullptr) std::memset (out[c], 0, (size_t) n * sizeof (float));

    for (auto& p : inSlots)
        if (auto* s = p.load()) s->fromDevice (in, numIn, n);
    for (auto& p : outSlots)
        if (auto* s = p.load()) s->toDevice (out, numOut, n, scratchPtrs, scratchSize);

    callbacks.fetch_add (1, std::memory_order_relaxed);
    busy.store (false);
}

void AsioDuplex::watchdog()
{
    if (device == nullptr) return;
    const auto now = juce::Time::getMillisecondCounter();
    const auto count = callbacks.load (std::memory_order_relaxed);
    if (count != lastCount) { lastCount = count; lastProgress = now; }
    else if (now - lastProgress > 2000) dead.store (true);

    if (dead.load()) failAttached();
}

void AsioDuplex::failAttached()
{
    for (auto& p : inSlots)  if (auto* s = p.load()) s->failed.store (true);
    for (auto& p : outSlots) if (auto* s = p.load()) s->failed.store (true);
}

//==============================================================================
/** Open secondary drivers by name (message thread only). */
std::map<juce::String, std::weak_ptr<AsioDuplex>>& openDrivers()
{
    static std::map<juce::String, std::weak_ptr<AsioDuplex>> drivers;
    return drivers;
}

std::shared_ptr<AsioDuplex> acquireDriver (juce::AudioIODeviceType& type, const juce::String& driver,
                                           double engRate, juce::String& error)
{
    auto& slot = openDrivers()[driver];
    if (auto d = slot.lock())
    {
        if (d->isUsable (engRate)) return d;
        // Died (unplugged, new format from its control panel) or the engine rate changed: reopen
        // in place. Streams still attached to it are failed and replaced by the engine.
        d->failAttached();
        if (! d->mayReopen()) { error = driver + " does not respond (select it again in a few seconds to retry)"; return nullptr; }
        return d->open (type, engRate, error) ? d : nullptr;
    }

    auto d = std::make_shared<AsioDuplex> (driver);
    if (! d->open (type, engRate, error)) return nullptr;
    slot = d;
    return d;
}
} // namespace

//==============================================================================
std::shared_ptr<AsyncInput> openAsioInput (juce::AudioIODeviceType& type, const juce::String& driver, double engineRate,
                                           int engineBlock, const std::vector<int>& channels, juce::String& error)
{
    auto d = acquireDriver (type, driver, engineRate, error);
    if (d == nullptr) return nullptr;
    auto chans = existingChannels (channels, d->getNumInputs());
    if (chans.empty()) { error = driver + " has no such input channel"; return nullptr; }
    return std::make_shared<AsioInput> (d, std::move (chans), engineRate, engineBlock);
}

std::shared_ptr<AsyncOutput> openAsioOutput (juce::AudioIODeviceType& type, const juce::String& driver, double engineRate,
                                             int engineBlock, const std::vector<int>& channels, juce::String& error)
{
    auto d = acquireDriver (type, driver, engineRate, error);
    if (d == nullptr) return nullptr;
    auto chans = existingChannels (channels, d->getNumOutputs());
    if (chans.empty()) { error = driver + " has no such output channel"; return nullptr; }
    return std::make_shared<AsioOutput> (d, std::move (chans), engineRate, engineBlock);
}

juce::StringArray asioChannelNames (juce::AudioIODeviceType& type, const juce::String& driver, bool inputs)
{
    static std::map<juce::String, std::pair<juce::StringArray, juce::StringArray>> cache;

    auto& drivers = openDrivers();
    if (auto it = drivers.find (driver); it != drivers.end())
        if (auto d = it->second.lock(); d != nullptr && d->isOpen())
        {
            cache[driver] = { d->getNames (true), d->getNames (false) };
            return d->getNames (inputs);
        }

    auto it = cache.find (driver);
    if (it == cache.end())
    {
        // Loads the driver briefly (like any ASIO device list). Failures are not cached, so a
        // driver that was busy is probed again next time.
        std::unique_ptr<juce::AudioIODevice> probe (type.createDevice (driver, driver));
        if (probe == nullptr) return {};
        auto ins = probe->getInputChannelNames();
        auto outs = probe->getOutputChannelNames();
        probe.reset();
        if (ins.isEmpty() && outs.isEmpty()) return {};
        it = cache.emplace (driver, std::make_pair (ins, outs)).first;
    }
    return inputs ? it->second.first : it->second.second;
}
} // namespace hydra

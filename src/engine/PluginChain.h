#pragma once
#include "Common.h"

namespace hydra
{
/** Global plugin host: formats, known-plugin list and scanning. Message thread only. */
class PluginHost
{
public:
    PluginHost();
    ~PluginHost();

    juce::AudioPluginFormatManager formats;
    juce::KnownPluginList known;

    juce::StringArray getSearchPaths() const;
    void setSearchPaths (const juce::StringArray& paths);

    /** Blocking scan (run from a background thread). progress in 0..1. */
    void scan (std::function<void (float, const juce::String&)> progress, std::atomic<bool>& cancel);

    std::unique_ptr<juce::AudioPluginInstance> create (const juce::PluginDescription& d,
                                                       double sr, int block, juce::String& error);

    void saveList();

private:
    juce::File listFile() const     { return appDataDir().getChildFile ("plugins.xml"); }
    juce::File pedalFile() const    { return appDataDir().getChildFile ("scan-crash.txt"); }
    juce::StringArray searchPaths;
    juce::CriticalSection listLock;
};

/** Built-in effects that sit in a channel's effect chain next to the plugins. A channel can have each of them
    up to kFxInstances times (correction once); every instance has its own parameters (Engine).
    Inputs offer denoiser, EQ, gate, compressor, phase; buses EQ, limiter and (hardware outputs) correction. */
enum class Fx : int { plugin = -1, denoiser = 0, eq, gate, comp, correction, limiter, phase, numBuiltins };
constexpr int kFxInstances = 4;

/** Id in session files ("eq", "gate", ...) and display name ("Equalizer", ...). */
juce::String fxId (Fx);
juce::String fxName (Fx);
Fx fxFromId (const juce::String&);

struct PluginSlot
{
    Fx builtin = Fx::plugin;   // a built-in effect (instance == nullptr) or a plugin
    int fxInstance = 0;        // which instance of the built-in effect (0..kFxInstances-1)
    bool isPlugin() const noexcept { return builtin == Fx::plugin; }
    std::unique_ptr<juce::AudioPluginInstance> instance;
    juce::PluginDescription description;
    std::atomic<bool> bypassed { false };
    std::atomic<uint64_t> dspTicks { 0 };               // processing time while profiling (DSP view)
    std::unique_ptr<juce::DocumentWindow> editorWindow; // UI only
};

/** Ordered effect chain of a channel: plugins and built-in effects (Fx) in any order. Audio thread uses a
    try-lock and simply passes audio through for the one block in which the message thread edits the chain
    (never blocks). */
class PluginChain
{
public:
    void prepare (double sampleRate, int maxBlock);
    void release();

    /** Audio thread. In-place stereo processing in chain order. Plugins are processed here (timed: their
        processing time is added to dspTicks), a built-in effect calls builtin (Fx, instance) at its position.
        Returns false if nothing was processed (chain being edited). */
    template <typename BuiltinFn>
    bool process (float* l, float* r, int n, bool timed, BuiltinFn&& builtin) noexcept
    {
        const juce::SpinLock::ScopedTryLockType tl (lock);
        if (! tl.isLocked() || ! prepared) return false;
        for (auto& s : slots)
        {
            if (s->isPlugin()) processPlugin (*s, l, r, n, timed);
            else               builtin (s->builtin, s->fxInstance);
        }
        return true;
    }

    // Message thread
    /** Adds a plugin at index (-1 = at the end). */
    void add (std::unique_ptr<juce::AudioPluginInstance> p, const juce::PluginDescription& d, int index = -1);
    /** Inserts instance k of a built-in effect at index (-1 = at the end); nothing if the chain already has it. */
    void addBuiltin (Fx, int k, int index = -1);
    /** Any instance of f in the chain (any thread). */
    bool hasBuiltin (Fx f) const noexcept { return (builtinMask.load (std::memory_order_relaxed) >> ((int) f * kFxInstances)) & 0xfu; }
    bool hasBuiltin (Fx f, int k) const noexcept { return (builtinMask.load (std::memory_order_relaxed) >> ((int) f * kFxInstances + k)) & 1u; }
    int indexOf (Fx, int k) const noexcept;
    int numPlugins() const noexcept;
    void remove (int index);
    void move (int from, int to);
    void clear();
    /** Changes with every edit (add / remove / move / load), for views showing the chain. */
    uint32_t getVersion() const noexcept { return version.load (std::memory_order_relaxed); }
    /** Moves all plugins out of / into the chain (reordering channels). The plugins stay prepared;
        putAll expects the chain to be empty. */
    std::vector<std::unique_ptr<PluginSlot>> takeAll();
    void putAll (std::vector<std::unique_ptr<PluginSlot>> plugins);
    int size() const noexcept                 { return (int) slots.size(); }
    PluginSlot* getSlot (int i) const noexcept { return juce::isPositiveAndBelow (i, (int) slots.size()) ? slots[(size_t) i].get() : nullptr; }
    int getTotalLatency() const;

    std::unique_ptr<juce::XmlElement> toXml() const;
    void fromXml (const juce::XmlElement& xml, PluginHost& host);
    /** One slot as XML (SLOT: built-in effect or plugin with its state). */
    std::unique_ptr<juce::XmlElement> slotToXml (int index) const;
    /** Creates the plugin of a SLOT element at index (-1 = end). Built-in slots are not handled here (Engine).
        Returns false if the plugin cannot be loaded. */
    bool insertPluginFromXml (const juce::XmlElement& slot, PluginHost& host, int index = -1);

private:
    void preparePlugin (juce::AudioPluginInstance& p);
    void processPlugin (PluginSlot&, float* l, float* r, int n, bool timed) noexcept;
    void changed() noexcept;   // under the lock: updates builtinMask, bumps version

    juce::SpinLock lock;
    std::atomic<uint32_t> builtinMask { 0 }, version { 0 };
    std::vector<std::unique_ptr<PluginSlot>> slots;
    juce::AudioBuffer<float> work;
    juce::MidiBuffer midi;
    double sr = 48000.0;
    int block = 512;
    bool prepared = false;
};
} // namespace hydra

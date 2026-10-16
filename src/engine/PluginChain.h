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

struct PluginSlot
{
    std::unique_ptr<juce::AudioPluginInstance> instance;
    juce::PluginDescription description;
    std::atomic<bool> bypassed { false };
    std::atomic<uint64_t> dspTicks { 0 };               // processing time while profiling (DSP view)
    std::unique_ptr<juce::DocumentWindow> editorWindow; // UI only
};

/** Ordered insert chain. Audio thread uses a try-lock and simply passes audio through for
    the one block in which the message thread edits the chain (never blocks). */
class PluginChain
{
public:
    void prepare (double sampleRate, int maxBlock);
    void release();

    /** Audio thread. In-place stereo processing. timed: add each plugin's processing time to its dspTicks. */
    void process (float* l, float* r, int n, bool timed = false) noexcept;

    // Message thread
    void add (std::unique_ptr<juce::AudioPluginInstance> p, const juce::PluginDescription& d);
    void remove (int index);
    void move (int from, int to);
    void clear();
    /** Moves all plugins out of / into the chain (reordering channels). The plugins stay prepared;
        putAll expects the chain to be empty. */
    std::vector<std::unique_ptr<PluginSlot>> takeAll();
    void putAll (std::vector<std::unique_ptr<PluginSlot>> plugins);
    int size() const noexcept                 { return (int) slots.size(); }
    PluginSlot* getSlot (int i) const noexcept { return juce::isPositiveAndBelow (i, (int) slots.size()) ? slots[(size_t) i].get() : nullptr; }
    int getTotalLatency() const;

    std::unique_ptr<juce::XmlElement> toXml() const;
    void fromXml (const juce::XmlElement& xml, PluginHost& host);

private:
    void preparePlugin (juce::AudioPluginInstance& p);

    juce::SpinLock lock;
    std::vector<std::unique_ptr<PluginSlot>> slots;
    juce::AudioBuffer<float> work;
    juce::MidiBuffer midi;
    double sr = 48000.0;
    int block = 512;
    bool prepared = false;
};
} // namespace hydra

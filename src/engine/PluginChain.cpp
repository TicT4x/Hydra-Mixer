#include "PluginChain.h"

namespace hydra
{
//==============================================================================
PluginHost::PluginHost()
{
    juce::addDefaultFormatsToManager (formats);

    if (auto xml = juce::XmlDocument::parse (listFile()))
    {
        if (auto* list = xml->getChildByName ("KNOWNPLUGINS"))
            known.recreateFromXml (*list);
        if (auto* paths = xml->getChildByName ("PATHS"))
            searchPaths.addTokens (paths->getAllSubText(), ";", {});
    }

    if (searchPaths.isEmpty())
        searchPaths.add (juce::File::getSpecialLocation (juce::File::globalApplicationsDirectory)
                             .getChildFile ("Common Files").getChildFile ("VST3").getFullPathName());
    searchPaths.removeEmptyStrings();
}

PluginHost::~PluginHost() { saveList(); }

juce::StringArray PluginHost::getSearchPaths() const { return searchPaths; }

void PluginHost::setSearchPaths (const juce::StringArray& paths)
{
    searchPaths = paths;
    searchPaths.removeEmptyStrings();
    searchPaths.removeDuplicates (true);
    saveList();
}

void PluginHost::saveList()
{
    juce::XmlElement root ("HYDRAPLUGINS");
    {
        const juce::ScopedLock sl (listLock);
        if (auto list = known.createXml())
        {
            list->setTagName ("KNOWNPLUGINS");
            root.addChildElement (list.release());
        }
    }
    root.createNewChildElement ("PATHS")->addTextElement (searchPaths.joinIntoString (";"));
    root.writeTo (listFile());
}

void PluginHost::scan (std::function<void (float, const juce::String&)> progress, std::atomic<bool>& cancel)
{
    juce::FileSearchPath path;
    for (auto& p : searchPaths)
        path.add (juce::File (p));

    for (int f = 0; f < formats.getNumFormats(); ++f)
    {
        auto* format = formats.getFormat (f);
        juce::PluginDirectoryScanner scanner (known, *format, path, true, pedalFile(), false);
        juce::String name;
        for (;;)
        {
            if (cancel.load()) return;
            progress (scanner.getProgress(), scanner.getNextPluginFileThatWillBeScanned());
            const juce::ScopedLock sl (listLock);
            if (! scanner.scanNextFile (true, name))
                break;
        }
    }
    progress (1.0f, {});
    saveList();
}

std::unique_ptr<juce::AudioPluginInstance> PluginHost::create (const juce::PluginDescription& d,
                                                               double sr, int block, juce::String& error)
{
    return formats.createPluginInstance (d, sr, block, error);
}

//==============================================================================
void PluginChain::preparePlugin (juce::AudioPluginInstance& p)
{
    // Prefer a plain stereo in / stereo out layout, disable sidechains.
    juce::AudioProcessor::BusesLayout stereo;
    stereo.inputBuses.add (juce::AudioChannelSet::stereo());
    stereo.outputBuses.add (juce::AudioChannelSet::stereo());
    for (int i = 1; i < p.getBusCount (true); ++i)  stereo.inputBuses.add (juce::AudioChannelSet::disabled());
    for (int i = 1; i < p.getBusCount (false); ++i) stereo.outputBuses.add (juce::AudioChannelSet::disabled());
    if (p.checkBusesLayoutSupported (stereo))
        p.setBusesLayout (stereo);

    p.setNonRealtime (false);
    p.setProcessingPrecision (juce::AudioProcessor::singlePrecision);
    p.prepareToPlay (sr, block);
}

void PluginChain::prepare (double sampleRate, int maxBlock)
{
    const juce::SpinLock::ScopedLockType sl (lock);
    sr = sampleRate;
    block = maxBlock;

    int maxCh = 2;
    for (auto& s : slots)
    {
        if (prepared) s->instance->releaseResources();
        preparePlugin (*s->instance);
        maxCh = juce::jmax (maxCh, s->instance->getTotalNumInputChannels(), s->instance->getTotalNumOutputChannels());
    }

    work.setSize (juce::jmax (8, maxCh), maxBlock, false, true, false);
    midi.ensureSize (256);
    prepared = true;
}

void PluginChain::release()
{
    const juce::SpinLock::ScopedLockType sl (lock);
    if (prepared)
        for (auto& s : slots)
            s->instance->releaseResources();
    prepared = false;
}

void PluginChain::process (float* l, float* r, int n, bool timed) noexcept
{
    const juce::SpinLock::ScopedTryLockType tl (lock);
    if (! tl.isLocked() || ! prepared || slots.empty())
        return;

    for (auto& s : slots)
    {
        if (s->bypassed.load (std::memory_order_relaxed))
            continue;

        auto& p = *s->instance;
        const int ins = p.getTotalNumInputChannels();
        const int outs = p.getTotalNumOutputChannels();
        const int chans = juce::jmax (ins, outs, 1);
        if (chans > work.getNumChannels() || n > work.getNumSamples())
            continue; // layout changed behind our back; skip rather than allocate

        juce::AudioBuffer<float> buf (work.getArrayOfWritePointers(), chans, n);
        if (ins == 1)
        {
            float* d = buf.getWritePointer (0);
            for (int i = 0; i < n; ++i) d[i] = 0.5f * (l[i] + r[i]);
        }
        else
        {
            buf.copyFrom (0, 0, l, n);
            if (chans > 1) buf.copyFrom (1, 0, r, n);
        }
        for (int c = juce::jmax (2, ins); c < chans; ++c)
            buf.clear (c, 0, n);

        midi.clear();
        const auto t0 = timed ? juce::Time::getHighResolutionTicks() : 0;
        p.processBlock (buf, midi);
        if (timed)
            s->dspTicks.store (s->dspTicks.load (std::memory_order_relaxed) + (uint64_t) juce::jmax ((juce::int64) 0, juce::Time::getHighResolutionTicks() - t0),
                               std::memory_order_relaxed);

        std::memcpy (l, buf.getReadPointer (0), (size_t) n * sizeof (float));
        std::memcpy (r, buf.getReadPointer (outs > 1 ? 1 : 0), (size_t) n * sizeof (float));
    }
}

void PluginChain::add (std::unique_ptr<juce::AudioPluginInstance> p, const juce::PluginDescription& d)
{
    auto slot = std::make_unique<PluginSlot>();
    preparePlugin (*p);
    slot->instance = std::move (p);
    slot->description = d;

    const int chans = juce::jmax (8, slot->instance->getTotalNumInputChannels(), slot->instance->getTotalNumOutputChannels());
    juce::AudioBuffer<float> bigger;
    if (chans > work.getNumChannels() || work.getNumSamples() < block)
        bigger.setSize (chans, block, false, true, false);

    const juce::SpinLock::ScopedLockType sl (lock);
    if (bigger.getNumChannels() > 0)
        std::swap (work, bigger);
    slots.push_back (std::move (slot));
}

void PluginChain::remove (int index)
{
    std::unique_ptr<PluginSlot> dead;
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        if (! juce::isPositiveAndBelow (index, (int) slots.size())) return;
        dead = std::move (slots[(size_t) index]);
        slots.erase (slots.begin() + index);
    }
    dead->editorWindow.reset();
    dead->instance->releaseResources();
}

void PluginChain::move (int from, int to)
{
    const juce::SpinLock::ScopedLockType sl (lock);
    if (! juce::isPositiveAndBelow (from, (int) slots.size()) || ! juce::isPositiveAndBelow (to, (int) slots.size()))
        return;
    auto s = std::move (slots[(size_t) from]);
    slots.erase (slots.begin() + from);
    slots.insert (slots.begin() + to, std::move (s));
}

std::vector<std::unique_ptr<PluginSlot>> PluginChain::takeAll()
{
    std::vector<std::unique_ptr<PluginSlot>> out;
    const juce::SpinLock::ScopedLockType sl (lock);
    std::swap (out, slots);
    return out;
}

void PluginChain::putAll (std::vector<std::unique_ptr<PluginSlot>> plugins)
{
    int chans = 8;
    for (auto& s : plugins)
        chans = juce::jmax (chans, s->instance->getTotalNumInputChannels(), s->instance->getTotalNumOutputChannels());
    juce::AudioBuffer<float> bigger;
    if (chans > work.getNumChannels() || work.getNumSamples() < block)
        bigger.setSize (chans, block, false, true, false);

    const juce::SpinLock::ScopedLockType sl (lock);
    if (bigger.getNumChannels() > 0)
        std::swap (work, bigger);
    for (auto& s : plugins)
        slots.push_back (std::move (s));
}

void PluginChain::clear()
{
    while (! slots.empty())
        remove ((int) slots.size() - 1);
}

int PluginChain::getTotalLatency() const
{
    int total = 0;
    for (auto& s : slots)
        if (! s->bypassed) total += s->instance->getLatencySamples();
    return total;
}

std::unique_ptr<juce::XmlElement> PluginChain::toXml() const
{
    auto xml = std::make_unique<juce::XmlElement> ("CHAIN");
    for (auto& s : slots)
    {
        auto* e = xml->createNewChildElement ("SLOT");
        e->setAttribute ("bypass", s->bypassed.load());
        if (auto d = s->description.createXml())
            e->addChildElement (d.release());
        juce::MemoryBlock state;
        s->instance->getStateInformation (state);
        e->createNewChildElement ("STATE")->addTextElement (state.toBase64Encoding());
    }
    return xml;
}

void PluginChain::fromXml (const juce::XmlElement& xml, PluginHost& host)
{
    clear();
    for (auto* e : xml.getChildWithTagNameIterator ("SLOT"))
    {
        juce::PluginDescription d;
        auto* de = e->getChildByName ("PLUGIN");
        if (de == nullptr || ! d.loadFromXml (*de))
            continue;

        juce::String err;
        auto inst = host.create (d, sr, block, err);
        if (inst == nullptr)
            continue;

        if (auto* st = e->getChildByName ("STATE"))
        {
            juce::MemoryBlock state;
            if (state.fromBase64Encoding (st->getAllSubText()))
                inst->setStateInformation (state.getData(), (int) state.getSize());
        }

        add (std::move (inst), d);
        slots.back()->bypassed = e->getBoolAttribute ("bypass");
    }
}
} // namespace hydra

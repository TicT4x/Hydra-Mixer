#include "RecorderProject.h"
#include "ChannelClipboard.h"

namespace hydra::recproject
{
juce::StringArray stripSuffixes()
{
    return { "gain", "trim", "pan", "mute", "solo", "mono", "fx.bypass", "fx.post" };
}

namespace
{
void resetTrack (Engine& e, int t)
{
    const auto prefix = Engine::channelPrefix (recTrackChannel (t));
    for (auto& p : e.params.all())
        if (p->project && p->id.startsWith (prefix)) p->reset();
    auto& rt = e.recTracks[(size_t) t];
    rt.plugins.clear();
    for (int k = 0; k < kFxInstances; ++k) rt.correctionAt (k).on->set (0.0f);
    e.setCorrection (recTrackChannel (t), {}, 0);
    for (int k = 0; k < kFxInstances; ++k) e.setHeadphone (recTrackChannel (t), k, {}, {});
}
} // namespace

void attach (Engine& e)
{
    auto& rec = e.recorder;
    rec.hooks.saveTracks = [&e] (juce::XmlElement& root, int numTracks)
    {
        for (int t = 0; t < numTracks; ++t)
        {
            const int ch = recTrackChannel (t);
            const auto prefix = Engine::channelPrefix (ch);
            auto* x = root.createNewChildElement ("TRACKSETTINGS");
            x->setAttribute ("index", t);
            for (auto& s : stripSuffixes())
                if (auto* p = e.params.find (prefix + s); p != nullptr && p->get() != p->defaultValue)
                {
                    auto* px = x->createNewChildElement ("P");
                    px->setAttribute ("id", s);
                    px->setAttribute ("v", (double) p->get());
                }
            if (auto chain = clip::copyChain (e, ch)) x->addChildElement (chain.release());
        }
    };
    rec.hooks.loadTracks = [&e] (const juce::XmlElement* root, int numTracks)
    {
        const auto& project = e.recorder.getProject();
        for (int t = 0; t < kMaxRecTracks; ++t)
        {
            resetTrack (e, t);
            auto& rt = e.recTracks[(size_t) t];
            rt.name = t < numTracks && t < (int) project.tracks.size() ? project.tracks[(size_t) t].name : "Track " + juce::String (t + 1);
        }
        if (root != nullptr)
            for (auto* x : root->getChildWithTagNameIterator ("TRACKSETTINGS"))
            {
                const int t = x->getIntAttribute ("index", -1);
                if (! juce::isPositiveAndBelow (t, numTracks)) continue;
                const int ch = recTrackChannel (t);
                if (auto* chain = x->getChildByName ("HYDRACLIP")) clip::pasteChain (e, ch, *chain);
                const auto prefix = Engine::channelPrefix (ch);
                for (auto* px : x->getChildWithTagNameIterator ("P"))
                    if (auto* p = e.params.find (prefix + px->getStringAttribute ("id")); p != nullptr && p->project)
                        p->set ((float) px->getDoubleAttribute ("v", p->defaultValue));
            }
        e.ensureBuiltins();
        e.sendChangeMessage();
    };
    rec.hooks.inputChain = [&e] (int input) -> std::unique_ptr<juce::XmlElement>
    {
        if (! juce::isPositiveAndBelow (input, kMaxInputs) || e.chainFor (input).size() == 0) return nullptr;
        return clip::copyChain (e, input);
    };
    rec.hooks.inputMono = [&e] (int input) { return juce::isPositiveAndBelow (input, kMaxInputs) && e.inputs[(size_t) input].mono->isOn(); };
    rec.hooks.tracksVersion = [&e]
    {
        uint64_t v = 0;
        const int n = e.recorder.getNumTracks();
        for (int t = 0; t < n; ++t) v += e.recTracks[(size_t) t].plugins.getVersion();
        // Parameter versions: only the tracks in use (a few hundred values each, polled at 20 Hz).
        static thread_local std::vector<Param*> cache;
        static thread_local int cachedFor = -1;
        if (cachedFor != n)
        {
            cache.clear();
            for (auto& p : e.params.all())
                if (p->project)
                {
                    const int t = p->id.fromFirstOccurrenceOf ("rt", false, false).getIntValue() - 1;
                    if (t >= 0 && t < n) cache.push_back (p.get());
                }
            cachedFor = n;
        }
        for (auto* p : cache) v += p->getVersion();
        return v;
    };
}
} // namespace hydra::recproject

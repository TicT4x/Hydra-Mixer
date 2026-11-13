#include "RecProject.h"

namespace hydra
{
std::unique_ptr<juce::XmlElement> RecProjectData::toXml() const
{
    auto xml = std::make_unique<juce::XmlElement> ("PROJECTDATA");
    xml->setAttribute ("rate", sampleRate);
    if (hasLoop())
    {
        auto* l = xml->createNewChildElement ("LOOP");
        l->setAttribute ("start", loopStart);
        l->setAttribute ("end", loopEnd);
    }
    auto* ts = xml->createNewChildElement ("TRACKS");
    for (auto& t : tracks)
    {
        auto* x = ts->createNewChildElement ("TRACK");
        x->setAttribute ("name", t.name);
        x->setAttribute ("file", t.file);
        x->setAttribute ("channels", t.channels);
        x->setAttribute ("input", t.sourceInput);
        x->setAttribute ("seconds", t.fileSeconds);
        for (auto& c : t.clips)
        {
            auto* cx = x->createNewChildElement ("CLIP");
            cx->setAttribute ("start", c.start);
            cx->setAttribute ("offset", c.offset);
            cx->setAttribute ("length", c.length);
            if (c.fadeIn > 0.0) cx->setAttribute ("fadeIn", c.fadeIn);
            if (c.fadeOut > 0.0) cx->setAttribute ("fadeOut", c.fadeOut);
            if (c.gainDb != 0.0f) cx->setAttribute ("gain", (double) c.gainDb);
        }
    }
    return xml;
}

void RecProjectData::fromXml (const juce::XmlElement& xml)
{
    *this = {};
    sampleRate = xml.getDoubleAttribute ("rate", 48000.0);
    if (auto* l = xml.getChildByName ("LOOP"))
    {
        loopStart = l->getDoubleAttribute ("start");
        loopEnd = l->getDoubleAttribute ("end");
    }
    if (auto* ts = xml.getChildByName ("TRACKS"))
        for (auto* x : ts->getChildWithTagNameIterator ("TRACK"))
        {
            if ((int) tracks.size() >= kMaxRecTracks) break;
            RecTrackInfo t;
            t.name = x->getStringAttribute ("name");
            t.file = x->getStringAttribute ("file");
            t.channels = juce::jlimit (1, 2, x->getIntAttribute ("channels", 2));
            t.sourceInput = x->getIntAttribute ("input", -1);
            t.fileSeconds = x->getDoubleAttribute ("seconds");
            for (auto* cx : x->getChildWithTagNameIterator ("CLIP"))
            {
                RecClip c;
                c.start = juce::jmax (0.0, cx->getDoubleAttribute ("start"));
                c.offset = juce::jmax (0.0, cx->getDoubleAttribute ("offset"));
                c.length = juce::jmax (0.0, cx->getDoubleAttribute ("length"));
                c.fadeIn = juce::jmax (0.0, cx->getDoubleAttribute ("fadeIn"));
                c.fadeOut = juce::jmax (0.0, cx->getDoubleAttribute ("fadeOut"));
                c.gainDb = (float) cx->getDoubleAttribute ("gain");
                if (c.length >= kMinClip) t.clips.push_back (c);
            }
            tracks.push_back (std::move (t));
        }
    for (int i = 0; i < (int) tracks.size(); ++i) tidy (i);
}

void RecProjectData::tidy (int track)
{
    auto& clips = tracks[(size_t) track].clips;
    std::stable_sort (clips.begin(), clips.end(), [] (const RecClip& a, const RecClip& b) { return a.start < b.start; });
    std::vector<RecClip> out;
    for (auto c : clips)
    {
        // Earlier clips that reach into this one end where it begins.
        for (auto& o : out)
            if (o.end() > c.start)
            {
                if (o.start >= c.start) o.length = 0.0;
                else o.length = c.start - o.start;
                o.fadeOut = juce::jmin (o.fadeOut, o.length);
            }
        out.erase (std::remove_if (out.begin(), out.end(), [] (const RecClip& o) { return o.length < kMinClip; }), out.end());
        c.fadeIn = juce::jmin (c.fadeIn, c.length);
        c.fadeOut = juce::jmin (c.fadeOut, c.length - c.fadeIn);
        if (c.length >= kMinClip) out.push_back (c);
    }
    clips = std::move (out);
}

bool RecProjectData::split (int track, double t)
{
    bool any = false;
    for (int i = 0; i < (int) tracks.size(); ++i)
    {
        if (track >= 0 && i != track) continue;
        auto& clips = tracks[(size_t) i].clips;
        for (size_t k = 0; k < clips.size(); ++k)
        {
            auto& c = clips[k];
            if (t <= c.start + kMinClip || t >= c.end() - kMinClip) continue;
            RecClip right = c;
            right.start = t;
            right.offset = c.offset + (t - c.start);
            right.length = c.end() - t;
            right.fadeIn = 0.0;
            c.length = t - c.start;
            c.fadeOut = 0.0;
            c.fadeIn = juce::jmin (c.fadeIn, c.length);
            right.fadeOut = juce::jmin (right.fadeOut, right.length);
            clips.insert (clips.begin() + (std::ptrdiff_t) k + 1, right);
            any = true;
            break;
        }
    }
    return any;
}

bool RecProjectData::erase (int track, double from, double to)
{
    if (to <= from) return false;
    bool any = false;
    for (int i = 0; i < (int) tracks.size(); ++i)
    {
        if (track >= 0 && i != track) continue;
        split (i, from);
        split (i, to);
        auto& clips = tracks[(size_t) i].clips;
        const auto before = clips.size();
        clips.erase (std::remove_if (clips.begin(), clips.end(),
                                     [&] (const RecClip& c) { return c.start >= from - 1.0e-9 && c.end() <= to + 1.0e-9; }),
                     clips.end());
        any |= clips.size() != before;
    }
    return any;
}

bool RecProjectData::crop (double from, double to)
{
    if (to <= from) return false;
    const double len = length();
    bool any = erase (-1, to, juce::jmax (to, len) + 1.0);
    any |= erase (-1, -1.0, from);
    if (from > 0.0)
        for (auto& t : tracks)
            for (auto& c : t.clips) c.start = juce::jmax (0.0, c.start - from);
    if (hasLoop())
    {
        loopStart = juce::jmax (0.0, loopStart - from);
        loopEnd = juce::jmax (0.0, loopEnd - from);
        if (! hasLoop()) loopStart = loopEnd = 0.0;
    }
    return any || from > 0.0;
}
} // namespace hydra

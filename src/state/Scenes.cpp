#include "Scenes.h"

namespace hydra
{
namespace
{
juce::String keyOf (const juce::String& id) { return id.replaceCharacter ('.', '_'); }
bool same (float a, float b) noexcept { return std::abs (a - b) <= 1.0e-4f * juce::jmax (1.0f, std::abs (a)); }
} // namespace

bool SceneBank::captures (const Param& p)
{
    // Whether a channel exists is the layout, not a scene setting.
    return p.persistent && ! p.id.endsWith (".active");
}

SceneBank::SceneBank (Engine& e) : engine (e)
{
    for (auto& p : engine.params.all())
        if (captures (*p)) captured.push_back (p.get());
    startTimerHz (50);   // MIDI / shortcut triggers
}

SceneBank::~SceneBank() { stopTimer(); }

SceneBank::Scene SceneBank::snapshot (const juce::String& name) const
{
    Scene s;
    s.name = name;
    for (auto* p : captured)
        if (! same (p->get(), p->defaultValue))
            s.values[p->id] = p->get();
    return s;
}

int SceneBank::addFromMixer (const juce::String& name)
{
    if (size() >= kMaxScenes) return -1;
    scenes.push_back (snapshot (name.isNotEmpty() ? name : "Scene " + juce::String (size() + 1)));
    current = size() - 1;
    ++version;
    return current;
}

int SceneBank::duplicate (int i)
{
    if (! juce::isPositiveAndBelow (i, size()) || size() >= kMaxScenes) return -1;
    // Appended at the end, so no other scene changes its place (and its MIDI / shortcut trigger).
    auto copy = scenes[(size_t) i];
    copy.name << " (copy)";
    scenes.push_back (std::move (copy));
    ++version;
    return size() - 1;
}

void SceneBank::store (int i)
{
    if (! juce::isPositiveAndBelow (i, size())) return;
    scenes[(size_t) i] = snapshot (scenes[(size_t) i].name);
    current = i;
    ++version;
}

void SceneBank::recall (int i)
{
    if (! juce::isPositiveAndBelow (i, size())) return;
    const auto& s = scenes[(size_t) i];
    for (auto* p : captured)
    {
        const auto it = s.values.find (p->id);
        p->set (it != s.values.end() ? it->second : p->defaultValue);
    }
    current = i;
    ++version;
    if (onRecalled) onRecalled (i);
}

void SceneBank::rename (int i, const juce::String& name)
{
    if (! juce::isPositiveAndBelow (i, size()) || name.trim().isEmpty()) return;
    scenes[(size_t) i].name = name.trim();
    ++version;
}

std::vector<std::pair<Param*, Param*>> SceneBank::remove (int i, Param*& removedTrigger)
{
    removedTrigger = nullptr;
    std::vector<std::pair<Param*, Param*>> moves;
    if (! juce::isPositiveAndBelow (i, size())) return moves;
    removedTrigger = trigger (i);
    for (int k = i + 1; k < size(); ++k)
        moves.emplace_back (trigger (k), trigger (k - 1));
    scenes.erase (scenes.begin() + i);
    if (current == i) current = -1;
    else if (current > i) --current;
    ++version;
    return moves;
}

std::vector<std::pair<Param*, Param*>> SceneBank::move (int from, int to)
{
    std::vector<std::pair<Param*, Param*>> moves;
    if (! juce::isPositiveAndBelow (from, size()) || ! juce::isPositiveAndBelow (to, size()) || from == to) return moves;
    // New place of every scene between the two positions.
    std::vector<int> newPos ((size_t) size());
    for (int k = 0; k < size(); ++k)
    {
        int n = k;
        if (k == from)                          n = to;
        else if (from < to && k > from && k <= to) n = k - 1;
        else if (to < from && k >= to && k < from) n = k + 1;
        newPos[(size_t) k] = n;
        if (n != k) moves.emplace_back (trigger (k), trigger (n));
    }
    auto s = std::move (scenes[(size_t) from]);
    scenes.erase (scenes.begin() + from);
    scenes.insert (scenes.begin() + to, std::move (s));
    if (current >= 0) current = newPos[(size_t) current];
    ++version;
    return moves;
}

void SceneBank::clear()
{
    scenes.clear();
    current = -1;
    ++version;
}

int SceneBank::countDifferences (int i) const
{
    if (! juce::isPositiveAndBelow (i, size())) return 0;
    const auto& s = scenes[(size_t) i];
    int n = 0;
    for (auto* p : captured)
    {
        const auto it = s.values.find (p->id);
        n += same (p->get(), it != s.values.end() ? it->second : p->defaultValue) ? 0 : 1;
    }
    return n;
}

bool SceneBank::differsFromMixer (int i) const { return countDifferences (i) > 0; }

void SceneBank::timerCallback()
{
    int wanted = -1;
    for (int k = 0; k < kMaxScenes; ++k)
        if (auto* p = engine.sceneRecall[(size_t) k]; p->isOn())
        {
            p->set (0.0f, true);   // trigger handled: ready for the next press
            if (k < size()) wanted = k;
        }
    if (engine.sceneNext->isOn())
    {
        engine.sceneNext->set (0.0f, true);
        if (size() > 0) wanted = current < 0 ? 0 : (current + 1) % size();
    }
    if (engine.scenePrev->isOn())
    {
        engine.scenePrev->set (0.0f, true);
        if (size() > 0) wanted = current <= 0 ? size() - 1 : current - 1;
    }
    if (wanted >= 0) recall (wanted);
}

std::unique_ptr<juce::XmlElement> SceneBank::toXml (const std::vector<int>& only) const
{
    auto xml = std::make_unique<juce::XmlElement> ("SCENES");
    if (only.empty()) xml->setAttribute ("current", current);
    for (int i = 0; i < size(); ++i)
    {
        if (! only.empty() && std::find (only.begin(), only.end(), i) == only.end()) continue;
        const auto& s = scenes[(size_t) i];
        auto* e = xml->createNewChildElement ("SCENE");
        e->setAttribute ("name", s.name);
        auto* ps = e->createNewChildElement ("PARAMS");
        for (auto& [id, v] : s.values)
            ps->setAttribute (keyOf (id), v);
    }
    return xml;
}

int SceneBank::fromXml (const juce::XmlElement& xml, bool append)
{
    if (! append) { scenes.clear(); current = -1; }
    int read = 0;
    for (auto* e : xml.getChildWithTagNameIterator ("SCENE"))
    {
        if (size() >= kMaxScenes) break;
        Scene s;
        s.name = e->getStringAttribute ("name", "Scene " + juce::String (size() + 1));
        if (auto* ps = e->getChildByName ("PARAMS"))
            for (auto* p : captured)
                if (const auto key = keyOf (p->id); ps->hasAttribute (key))
                    s.values[p->id] = juce::jlimit (p->range.start, p->range.end, (float) ps->getDoubleAttribute (key));
        scenes.push_back (std::move (s));
        ++read;
    }
    if (! append)
        current = juce::jlimit (-1, size() - 1, xml.getIntAttribute ("current", -1));
    ++version;
    return read;
}

bool SceneBank::exportTo (const juce::File& f, const std::vector<int>& only) const
{
    auto xml = toXml (only);
    xml->setTagName ("HYDRASCENES");
    xml->removeAttribute ("current");
    xml->setAttribute ("version", 1);
    return xml->writeTo (f);
}

int SceneBank::importFrom (const juce::File& f, juce::String& error)
{
    auto xml = juce::XmlDocument::parse (f);
    if (xml == nullptr || ! (xml->hasTagName ("HYDRASCENES") || xml->hasTagName ("SCENES")))
    {
        error = f.getFileName() + " is not a Hydra scene file.";
        return 0;
    }
    if (size() >= kMaxScenes)
    {
        error = "All " + juce::String (kMaxScenes) + " scene slots are in use.";
        return 0;
    }
    const int n = fromXml (*xml, true);
    if (n == 0) error = f.getFileName() + " contains no scenes.";
    return n;
}
} // namespace hydra

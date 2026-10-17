#pragma once
#include "engine/Engine.h"
#include <map>

namespace hydra
{
/** Scenes: named snapshots of every mixer parameter (faders, routing, EQ, dynamics, ...), recalled at once.

    - A scene holds all stored parameters except whether a channel slot is in use (*.active): recalling
      never adds or removes channels. Devices, names and plugins are not part of a scene.
    - Recall applies the values immediately and without asking, also while the mixer has unsaved edits
      (live use). Editing the mixer does not change the scene; store() saves the current state into it.
    - Scene k is recalled by its trigger parameter Engine::sceneRecall[k] (MIDI learn / keyboard shortcut
      like any button), Engine::sceneNext / scenePrev step through the list. Polled on the message thread.
    - Stored with the session; scenes can also be exported to / imported from a file of their own. */
class SceneBank : private juce::Timer
{
public:
    struct Scene
    {
        juce::String name;
        std::map<juce::String, float> values;   // parameter id -> value; missing = the parameter's default
    };

    explicit SceneBank (Engine& e);
    ~SceneBank() override;

    int size() const noexcept                 { return (int) scenes.size(); }
    const Scene& get (int i) const            { return scenes[(size_t) i]; }
    /** The scene last recalled or saved (the one being edited), -1 if none. */
    int getCurrent() const noexcept           { return current; }
    void setCurrent (int i)                   { current = juce::isPositiveAndBelow (i, size()) ? i : -1; ++version; }

    /** Adds a scene holding the current mixer state; returns its index or -1 if all kMaxScenes are used. */
    int addFromMixer (const juce::String& name);
    int duplicate (int i);
    /** Saves the current mixer state into scene i (it becomes the current scene). */
    void store (int i);
    /** Applies scene i to the mixer (it becomes the current scene). */
    void recall (int i);
    void rename (int i, const juce::String& name);

    /** Removes scene i. The scenes after it move up one place, and so do their trigger parameters:
        the returned pairs (old trigger, new trigger) let MIDI mappings / shortcuts follow their scene;
        removedTrigger receives the trigger of scene i (its assignments should be dropped). */
    std::vector<std::pair<Param*, Param*>> remove (int i, Param*& removedTrigger);
    /** Moves scene from to position to; returns the trigger moves (see remove). */
    std::vector<std::pair<Param*, Param*>> move (int from, int to);
    void clear();

    /** True if the mixer differs from scene i (unsaved edits). */
    bool differsFromMixer (int i) const;
    /** Number of parameters whose value differs between the mixer and scene i. */
    int countDifferences (int i) const;
    Param* trigger (int i) const { return juce::isPositiveAndBelow (i, kMaxScenes) ? engine.sceneRecall[(size_t) i] : nullptr; }

    /** Session storage (all scenes) and export / import (the given scenes; empty = all). */
    std::unique_ptr<juce::XmlElement> toXml (const std::vector<int>& only = {}) const;
    /** Reads scenes; append = add them after the existing ones (import), otherwise replace all.
        Returns the number of scenes read. */
    int fromXml (const juce::XmlElement& xml, bool append);
    bool exportTo (const juce::File& f, const std::vector<int>& only) const;
    /** Appends the scenes of an exported file; returns how many were added (error set if none). */
    int importFrom (const juce::File& f, juce::String& error);

    /** Bumped on every change of the scene list (autosave, UI refresh). */
    std::atomic<uint32_t> version { 0 };
    /** Called on the message thread after a scene was recalled (also by MIDI / shortcut). */
    std::function<void (int)> onRecalled;

    /** Parameters that are part of a scene. */
    static bool captures (const Param& p);

private:
    void timerCallback() override;
    Scene snapshot (const juce::String& name) const;

    Engine& engine;
    std::vector<Param*> captured;
    std::vector<Scene> scenes;
    int current = -1;
};
} // namespace hydra

#pragma once
#include "Common.h"
#include <functional>
#include <unordered_map>

namespace hydra
{
enum class ParamType { Continuous, Toggle };

/** A single lock-free parameter. Written from UI / MIDI threads, read by the audio thread.
    Every change bumps a version counter so the UI and MIDI feedback can poll cheaply. */
class Param
{
public:
    Param (juce::String paramId, juce::String displayName, ParamType t,
           juce::NormalisableRange<float> r, float def, juce::String unitSuffix = {})
        : id (std::move (paramId)), name (std::move (displayName)), type (t),
          range (r), defaultValue (def), unit (std::move (unitSuffix)), value (def) {}

    const juce::String id, name;
    const ParamType type;
    const juce::NormalisableRange<float> range;
    const float defaultValue;
    const juce::String unit;
    juce::StringArray choices;   // non-empty: discrete choice parameter (value = index)
    bool persistent = true;      // false: not stored in the session (e.g. transport buttons)

    float get() const noexcept            { return value.load (std::memory_order_relaxed); }
    bool isOn() const noexcept            { return get() >= 0.5f; }
    float getNormalised() const noexcept  { return range.convertTo0to1 (get()); }
    uint32_t getVersion() const noexcept  { return version.load (std::memory_order_acquire); }

    /** external = set by a MIDI controller: not recorded as an undo step (see UndoHistory). */
    void set (float v, bool external = false) noexcept
    {
        v = juce::jlimit (range.start, range.end, v);
        if (type == ParamType::Toggle)
            v = v >= 0.5f ? 1.0f : 0.0f;
        else if (! choices.isEmpty())
            v = (float) juce::roundToInt (v);

        if (value.exchange (v, std::memory_order_relaxed) != v)
        {
            // Stored before the version is published, so a reader that sees the new version sees the mark too.
            externalVersion.store (external ? version.load (std::memory_order_relaxed) + 1 : 0, std::memory_order_relaxed);
            version.fetch_add (1, std::memory_order_release);
        }
    }

    void setNormalised (float n, bool external = false) noexcept { set (range.convertFrom0to1 (juce::jlimit (0.0f, 1.0f, n)), external); }
    void toggle (bool external = false) noexcept                 { set (isOn() ? 0.0f : 1.0f, external); }
    /** True if the current value was set by a MIDI controller. */
    bool isExternalChange() const noexcept { return externalVersion.load (std::memory_order_relaxed) == getVersion(); }
    void reset() noexcept                 { set (defaultValue); }

    juce::String toText() const
    {
        if (type == ParamType::Toggle)
            return isOn() ? "On" : "Off";

        const auto v = get();
        if (! choices.isEmpty())
            return choices[juce::jlimit (0, choices.size() - 1, juce::roundToInt (v))];
        if (unit == " ms" && v >= 1000.0f)
            return juce::String (v / 1000.0f, 2) + " s";
        if (unit == " dB" && v <= range.start + 0.001f && range.start <= -60.0f)
            return "-inf dB";

        const int decimals = std::abs (v) >= 100.0f ? 0 : 1;
        if (unit == " Hz" && v >= 1000.0f)
            return juce::String (v / 1000.0f, 2) + " kHz";
        return juce::String (v, decimals) + unit;
    }

private:
    std::atomic<float> value;
    std::atomic<uint32_t> version { 1 };
    std::atomic<uint32_t> externalVersion { 0 };
};

class ParamRegistry
{
public:
    Param& add (const juce::String& id, const juce::String& name, ParamType t,
                juce::NormalisableRange<float> r, float def, const juce::String& unit = {})
    {
        jassert (find (id) == nullptr);
        params.push_back (std::make_unique<Param> (id, name, t, r, def, unit));
        auto& p = *params.back();
        lookup[id.toStdString()] = &p;
        return p;
    }

    Param& addChoice (const juce::String& id, const juce::String& name, const juce::StringArray& options, int def = 0)
    {
        auto& p = add (id, name, ParamType::Continuous, { 0.0f, (float) (options.size() - 1), 1.0f }, (float) def);
        p.choices = options;
        return p;
    }

    Param& addToggle (const juce::String& id, const juce::String& name, bool def = false)
    {
        return add (id, name, ParamType::Toggle, { 0.0f, 1.0f, 1.0f }, def ? 1.0f : 0.0f);
    }

    Param* find (const juce::String& id) const
    {
        auto it = lookup.find (id.toStdString());
        return it != lookup.end() ? it->second : nullptr;
    }

    Param& get (const juce::String& id) const
    {
        auto* p = find (id);
        jassert (p != nullptr);
        return *p;
    }

    const std::vector<std::unique_ptr<Param>>& all() const noexcept { return params; }

    /** Sum of all versions — cheap way to detect "something changed" for autosave. */
    uint64_t versionSum() const noexcept
    {
        uint64_t s = 0;
        for (auto& p : params) s += p->getVersion();
        return s;
    }

private:
    std::vector<std::unique_ptr<Param>> params;
    std::unordered_map<std::string, Param*> lookup;
};

/** Message-thread poller: anything that displays parameter state registers here and
    gets a callback only when its parameter actually changed (one atomic load per tick). */
class ParamPoller : private juce::Timer
{
public:
    struct Client
    {
        virtual ~Client() = default;
        virtual void pollParams() = 0;
    };

    /** Owned by the application; exists for the lifetime of the UI. */
    ParamPoller()  { current() = this; startTimerHz (60); }
    ~ParamPoller() override { stopTimer(); current() = nullptr; }

    static ParamPoller* get() noexcept { return current(); }

    static void attach (Client* c) { if (auto* p = get()) p->clients.addIfNotAlreadyThere (c); }
    static void detach (Client* c) { if (auto* p = get()) p->clients.removeFirstMatchingValue (c); }

private:
    static ParamPoller*& current() { static ParamPoller* p = nullptr; return p; }
    void timerCallback() override
    {
        for (int i = clients.size(); --i >= 0;)
            if (auto* c = clients[i])
                c->pollParams();
    }

    juce::Array<Client*> clients;
};
} // namespace hydra

#pragma once
#include "core/Params.h"

namespace hydra
{
struct MidiMapping
{
    enum class Source { CC = 0, Note, PitchBend };
    enum class Mode { Absolute = 0, Toggle, Momentary, Relative };

    juce::String paramId;
    Param* param = nullptr;
    juce::String device;      // input device identifier ("" = any)
    int channel = 0;          // 1..16, 0 = any
    Source source = Source::CC;
    int number = 0;           // CC / note number
    Mode mode = Mode::Absolute;
    bool invert = false;
    float minN = 0.0f;        // range the MIDI control sweeps, as normalised parameter positions
    float maxN = 1.0f;        //   (e.g. fader -inf..0 dB instead of -inf..+12 dB)
    int lastSent = -1;        // feedback de-duplication (MIDI value)
    Param* lastTarget = nullptr; // feedback: parameter the value was last sent for (see MidiMapper::redirect)

    bool isSwitch() const noexcept { return mode == Mode::Toggle || mode == Mode::Momentary; }
    bool hasCustomRange() const noexcept { return minN != 0.0f || maxN != 1.0f; }
    /** Maps a 0..1 controller position into the configured range. */
    float toParam (float n) const noexcept { return minN + n * (maxN - minN); }
    /** Inverse of toParam (for feedback), clamped to 0..1. */
    float fromParam (float p) const noexcept
    {
        const float span = maxN - minN;
        return std::abs (span) < 1.0e-6f ? 0.0f : juce::jlimit (0.0f, 1.0f, (p - minN) / span);
    }
    juce::String rangeText() const;

    juce::String describe() const;
    static juce::String modeName (Mode m);
};

/** LED feedback for pad grids (velocity = colour), e.g. Akai APC mini.
      bound + active   -> velOn      (green)
      bound + inactive -> velOff     (yellow)
      unbound          -> velUnbound (off)
      unbound, and the channel that owns this row/column is muted -> velMuted (red)
    Rows vs. columns is detected from the mappings (one channel per row or one per column). */
struct LedProfile
{
    juce::String name = "Generic (127 / 0)";
    bool grid = false;
    int firstNote = 0, lastNote = 63, rowSize = 8, channel = 1;
    int velOn = 127, velOff = 0, velUnbound = 0, velMuted = 0;

    static std::vector<LedProfile> presets()
    {
        LedProfile generic;
        LedProfile apc;
        apc.name = "Akai APC mini (mk1)";
        apc.grid = true;
        apc.velOn = 1; apc.velOff = 5; apc.velUnbound = 0; apc.velMuted = 3;   // green / yellow / off / red
        LedProfile mk2 = apc;
        mk2.name = "Akai APC mini mk2";
        mk2.channel = 7;                                                      // channel 7 = full brightness
        mk2.velOn = 21; mk2.velOff = 13; mk2.velUnbound = 0; mk2.velMuted = 5; // palette green / yellow / off / red
        return { generic, apc, mk2 };
    }

    bool contains (int note) const noexcept { return grid && note >= firstNote && note <= lastNote; }
};

/** Maps any MIDI CC / Note / Pitch-bend directly to any mixer parameter.
    - MIDI learn: armParam(), then move/press a control.
    - Feedback: current values are sent back to an output device (LEDs, motor faders). */
class MidiMapper : private juce::MidiInputCallback,
                   private juce::Timer
{
public:
    explicit MidiMapper (ParamRegistry& registry);
    ~MidiMapper() override;

    // Devices
    juce::Array<juce::MidiDeviceInfo> availableInputs() const  { return juce::MidiInput::getAvailableDevices(); }
    juce::Array<juce::MidiDeviceInfo> availableOutputs() const { return juce::MidiOutput::getAvailableDevices(); }
    void setInputEnabled (const juce::String& identifier, bool enabled);
    bool isInputEnabled (const juce::String& identifier) const;
    void setFeedbackOutput (const juce::String& identifier);
    void setLedProfile (const LedProfile& p);
    LedProfile getLedProfile() const { const juce::ScopedLock sl (lock); return led; }
    juce::String getFeedbackOutput() const { return feedbackId; }

    // Learning
    void armParam (Param* p);
    Param* getArmedParam() const noexcept { return armed.load(); }
    void cancelLearn() { armed.store (nullptr); }
    std::function<void (Param*)> onLearned; // message thread

    /** Optional: maps a mapping's parameter to the one it should drive right now (bus SEL makes the
        input faders drive their sends). Called on the MIDI thread; set before devices are opened. */
    std::function<Param* (Param*)> redirect;

    // Mappings (message thread)
    std::vector<MidiMapping> getMappings() const;
    std::vector<MidiMapping> getMappingsFor (const Param* p) const;
    void removeMappingsFor (const Param* p);
    void removeMapping (int index);
    void setMappingMode (int index, MidiMapping::Mode m);
    void setMappingInvert (int index, bool inv);
    void setMappingRange (int index, float minN, float maxN);
    /** Changes the range of every mapping of a parameter. Pass a negative value to keep that end. */
    void setRangeForParam (const Param* p, float minN, float maxN);
    void clearAll();

    /** Hardware faders on the channel / send faders reach at most 0 dB instead of +12 dB. Switching it on
        sets the top of every fader mapping that still reaches above 0 dB to 0 dB (switching it off returns
        those to +12 dB); faders learned while it is on get it too. Stored with the MIDI settings. */
    void setFaderTopAt0dB (bool on);
    bool getFaderTopAt0dB() const noexcept { return faderTop0dB; }
    /** Channel / bus fader or a send level (the parameters the option above applies to). */
    static bool isFaderParam (const Param& p);
    /** Channels were reordered: every mapping of a parameter in moves (first) now drives the parameter
        that holds its value (second), so the controller keeps controlling the same channel. */
    void remapParams (const std::vector<std::pair<Param*, Param*>>& moves);
    /** Number of mappings on the given parameters (first of each pair). */
    int countMappings (const std::vector<std::pair<Param*, Param*>>& moves) const;

    std::unique_ptr<juce::XmlElement> toXml() const;
    void fromXml (const juce::XmlElement& xml);

    /** Last raw MIDI event seen (for the UI's activity indicator). */
    std::atomic<uint32_t> activity { 0 };
    /** Bumped whenever the mapping table changes (UI refresh). */
    std::atomic<uint32_t> mapVersion { 0 };
    bool hasMapping (const Param* p) const;

private:
    void handleIncomingMidiMessage (juce::MidiInput* source, const juce::MidiMessage& m) override;
    void timerCallback() override;
    void sendFeedback (MidiMapping& m, bool force);
    Param* targetOf (const MidiMapping& m) const { return redirect ? redirect (m.param) : m.param; }
    void updateGridLeds (bool force);
public:
    /** Desired velocity per note for the current state (-1 = not part of the grid). For tests/UI. */
    std::array<int, 128> computeGridLeds() const;
private:
    void clearGridLeds();
    /** Switches off every LED Hydra may have lit: the pad grid, every mapped note (e.g. mute buttons outside
        the grid) and, for pad-grid controllers, the remaining button rows. Called when Hydra closes. */
    void switchOffLeds();
    /** Asks controllers that support it for the current position of their faders (Akai APC mini: SysEx
        introduction message, the controller answers with its fader positions). Sent ~0.5 s after an input
        opened; the answer drives the mapped parameters like a moved fader. */
    void requestControllerState();
public:
    /** Answer to requestControllerState (APC mini SysEx with the fader positions); public for tests. */
    bool handleStateReply (juce::MidiInput* source, const juce::MidiMessage& m);
private:
    double stateRequestAt = 0.0;   // ms (Time::getMillisecondCounterHiRes), 0 = nothing pending

    ParamRegistry& params;
    mutable juce::CriticalSection lock;
    std::vector<MidiMapping> mappings;
    std::atomic<Param*> armed { nullptr };

    std::vector<std::unique_ptr<juce::MidiInput>> openInputs;
    juce::StringArray enabledInputs;
    std::unique_ptr<juce::MidiOutput> feedback;
    juce::String feedbackId;
    std::unordered_map<Param*, uint32_t> lastVersions;
    LedProfile led;
    std::array<int, 128> ledSent {};
    bool faderTop0dB = false;
};
} // namespace hydra

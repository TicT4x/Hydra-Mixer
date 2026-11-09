#pragma once
#include "state/CorrectionPool.h"

namespace hydra
{
/** "Hydra Room Correction" (VST3): the room correction of Hydra Mixer's hardware outputs for a DAW's master bus.

    It uses the same data as the mixer: the corrections of the shared pool (%APPDATA%/HydraMixer/Corrections) and the
    measurements of the library (%APPDATA%/HydraMixer/Measurements). The plugin's correction is linked to its pool
    entry (pool::Link): a change made here, in the mixer or in another instance shows up everywhere it is used. The
    project keeps a copy, so it sounds the same on a computer without the pool (the entry is written back there).
    Measuring stays in the mixer (the calibration wizard); here a correction is chosen, designed from a measurement
    (Edit page) and its level set. Zero latency (biquads), mono or stereo. */
class RoomCorrectionProcessor final : public juce::AudioProcessor, public CorrectionTarget, private juce::Timer
{
public:
    RoomCorrectionProcessor();
    ~RoomCorrectionProcessor() override;

    // AudioProcessor
    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void* data, int size) override;

    // CorrectionTarget (message thread)
    const CorrectionProfile& getProfile() const override { return correction.getProfile(); }
    uint32_t getVersion() const override { return correction.getVersion(); }
    void setProfile (CorrectionProfile) override;
    bool isOn() const override { return onParam->get(); }
    void setOn (bool) override;
    double getSampleRate() const override { return rate.load(); }
    juce::String label() const override { return "this plugin"; }
    juce::Colour colour() const override;

    const pool::Link& getLink() const noexcept { return link; }
    juce::Point<int> editorSize { 1240, 820 };

private:
    void timerCallback() override;

    juce::AudioParameterBool* onParam;
    Param on { "on", "On", ParamType::Toggle, { 0.0f, 1.0f, 1.0f }, 1.0f };   // what Correction reads
    Correction correction;
    pool::Link link { pool::kPlugin };

    std::atomic<double> rate { 0.0 };
    std::atomic<uint64_t> blocks { 0 };
    std::atomic<bool> inProcess { false };
    std::vector<float> scratch;   // the second channel of a mono bus

    // State loaded on another thread than the message thread: applied by the timer.
    juce::CriticalSection pendingLock;
    std::optional<CorrectionProfile> pending;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RoomCorrectionProcessor)
};
} // namespace hydra

#pragma once
#include "Charts.h"
#include "state/RoomMeasurements.h"

namespace hydra
{
/** Room measurement (Tools > Room Measurement, own window).

    Left: the setup (microphone = an input, speaker = a bus, sweep length / level / repeats) with the
    start button, and the library of saved measurements (rename by double click, import / export, delete).
    Right: the selected measurement as
    - Response: frequency response (smoothing selectable, a second measurement for comparison), the
      resonances found, and the designed EQ with the response it would give,
    - Waterfall: 3D cumulative spectral decay (drag to tilt), shows which frequencies keep ringing,
    - Decay: reverberation time per octave band (T20 / EDT),
    - Impulse: energy time curve (direct sound, reflections, decay into the noise).
    Below: the resonance list and the automatic EQ (range, bands, limits), applied to any channel's EQ
    with one click (one undo step).

    Measuring is raw: the sweep replaces the output bus's signal and chain, the microphone is taken from
    the input's source before any processing (see MeasureRun). */
class RoomView final : public juce::Component, private juce::Timer, private juce::ListBoxModel
{
public:
    explicit RoomView (UiContext& c);
    ~RoomView() override;
    void paint (juce::Graphics&) override;
    void resized() override;

    enum class Mode { response, waterfall, decay, impulse };

private:
    class Chart;

    void timerCallback() override;
    int getNumRows() override { return (int) library.size(); }
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    void selectedRowsChanged (int row) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override { startRename (row); }
    void deleteKeyPressed (int) override { deleteSelected(); }

    void refreshChannelBoxes();
    void startOrStop();
    void measurementFinished (std::vector<float> capture, room::SweepSpec spec, juce::String mic, juce::String out, bool throughEq);
    void select (int index);
    void analyseIfNeeded (RoomMeasurement* m);
    void updateEq();
    void updateApplyButton();
    void applyEq();
    void setMode (Mode);
    void updateModeControls();
    void showExportMenu();
    void importFile();
    void deleteSelected();
    void startRename (int row);
    void finishRename (bool apply);
    void exportTo (const juce::String& title, const juce::String& pattern, std::function<bool (const juce::File&)> write);

    RoomMeasurement* current() const { return juce::isPositiveAndBelow (currentIndex, (int) library.size()) ? library[(size_t) currentIndex].get() : nullptr; }
    RoomMeasurement* compared() const;
    int targetChannel() const;   // selection index of the EQ target, -1 if none
    double responseSmoothing() const;
    double waterfallMaxHz() const;
    double impulseSpanMs() const;
    juce::String quality (const RoomMeasurement&) const;

    UiContext& ctx;

    // Setup
    juce::ComboBox micBox, micChannelBox, busBox, speakerBox, lengthBox, repeatsBox, levelBox;
    juce::ToggleButton muteOthers { "Mute all other outputs" }, throughEq { "Through the output's EQ (check a correction)" };
    juce::TextButton startButton { "Start measurement" };
    juce::String status;
    juce::Colour statusColour = theme::textDim;
    float micLevel = 0.0f, micHold = 0.0f;
    juce::Rectangle<int> setupArea, progressArea, statusArea;
    juce::String shownChannels;   // names / devices the channel boxes were built for
    int layoutTick = 0, stopTicks = 0;
    bool targetChosen = false;   // the EQ target no longer follows the measured bus
    juce::String runMic, runOut;
    bool runThroughEq = false;
    room::SweepSpec runSpec;

    // Library
    std::vector<std::unique_ptr<RoomMeasurement>> library;
    int currentIndex = -1;
    RoomMeasurement* comparePtr = nullptr;            // shown for comparison in the response chart
    std::vector<const RoomMeasurement*> analysing;     // analysis running in the background
    juce::ListBox list { "Measurements", this };
    juce::TextButton importButton { "Import..." }, exportButton { "Export..." }, deleteButton { "Delete" };
    std::unique_ptr<juce::TextEditor> renameEditor;
    int renameRow = -1;
    juce::Rectangle<int> libraryArea;

    // Charts
    Mode mode = Mode::response;
    Chip tabResponse { "Response" }, tabWaterfall { "Waterfall 3D" }, tabDecay { "Decay (RT60)" }, tabImpulse { "Impulse" };
    juce::ComboBox viewOption, compareBox;
    std::unique_ptr<Chart> chart;
    juce::Rectangle<int> infoArea;

    // Resonances + Auto EQ
    juce::ComboBox eqLowBox, eqHighBox, eqBandsBox, eqCutBox, eqBoostBox, eqSmoothBox, targetBox;
    juce::TextButton applyButton { "Apply EQ" }, exportEqButton { "Export EQ..." };
    room::AutoEqResult eq;
    juce::Rectangle<int> resonanceArea, eqArea, bandsArea;
    std::vector<std::pair<juce::Component*, juce::String>> captions;   // label drawn above each control

    std::unique_ptr<juce::FileChooser> chooser;
    friend class Chart;
};
} // namespace hydra

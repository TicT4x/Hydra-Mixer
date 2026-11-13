#pragma once
#include "RoomPages.h"
#include "Charts.h"
#include "DevicePicker.h"

namespace hydra
{
/** Guided speaker calibration (Room Correction window, page "Calibrate"): one step at a time, each with a picture
    and a short explanation, in the spirit of SoundID Reference.

    Steps: Start (what is needed) -> Speakers (output, left/right separately or together) -> Microphone (input,
    channel, calibration file) -> Placement (where the microphone goes, how many positions) -> Level (pink noise
    loop, big gauge, advice, warns when the microphone / room is too noisy) -> Near field (optional: the microphone
    1-2 cm in front of each speaker, a quiet sweep with automatic level, then the other speaker from there: the
    speaker's own response, the system latency and the distance between the speakers; then back to the listening
    position) -> Measure (per position a sweep from each speaker, the picture shows where the
    microphone goes next) -> Result (before / after, strength and range as presets) -> Done (A/B switch).

    The measurements end up in the library (RoomLibrary, list on the Correction page); Analysis shows them in
    detail, Edit fine-tunes the correction with every setting. The wizard uses sensible defaults.

    Validate (startValidation, "Validate / Refine" on the Correction page): Microphone -> Placement -> Level -> Measure
    -> Check. The sweeps play through the output's Correction (MeasureRun::busCorrection); the result is compared with
    the measurement the correction came from, and "Refine" adds up to 8 small bands (bass range) for what is left.
    Validation measurements are not kept in the library. */
class CorrectionWizard final : public juce::Component, private juce::Timer
{
public:
    explicit CorrectionWizard (UiContext& c);
    ~CorrectionWizard() override;
    void paint (juce::Graphics&) override;
    void resized() override;

    /** The hardware output (selection index) that is measured and corrected. */
    void setOutput (int selectIndex);
    /** True from the level check on: the window must keep its output (results belong to it). */
    bool holdsOutput() const;
    /** True while this wizard plays something (level check or a sweep). */
    bool isRunning() const;

    std::function<void()> onShowCorrection;                      // "Show correction" on the last step
    std::function<void()> onOpenAnalysis;                        // "Detailed analysis"
    std::function<void()> onFineTune;                            // "Fine-tune" (the correction is on the output by then)
    std::function<void (const RoomMeasurement&)> onMeasured;     // a finished measurement (copy for the library)
    /** File name of the measurement of the result step, empty if there is none. */
    juce::String measurementFile() const { return results[0] != nullptr ? results[0]->file.getFileName() : juce::String(); }

    enum class Step { welcome, speakers, microphone, placement, level, nearField, measure, result, done };
    enum class Mode { calibrate, validate };

    /** Validates the output's correction made from `original` (the loaded measurement). */
    void startValidation (const RoomMeasurement& original);
    /** Back to calibrating, if the wizard is validating (a calibration in progress stays). */
    void startCalibration();

private:
    class ResultChart;

    void timerCallback() override;
    void showStep (Step);
    std::vector<Step> stepList() const;   // the steps of the mode, in order (Done not included)
    void next();
    void back();
    void restart();
    void updateButtons();
    bool canContinue() const;

    // Microphone
    void refreshMicBoxes();
    void micChosen();
    void chooseMicDevice();
    void closePicker();
    void refreshCalibrations (const juce::String& select);
    void importCalibration();
    int micInput() const;                 // input slot, -1 = none
    juce::String micDescription() const;
    juce::String speakerDescription() const;

    // Level check
    void startLevelCheck();
    void stopRun();
    void updateLevelCheck (float peak, const MeasureRun&);
    float levelDb() const;
    /** Level step: test signal over the noise in the pauses (dB), 99 while the noise is not measured yet. */
    float snrDb() const noexcept { return signalRms > 0.0f && noiseRms > 0.0f ? juce::Decibels::gainToDecibels (signalRms / noiseRms) : 99.0f; }

    // Near field: per speaker a quiet sweep at 1-2 cm (level found automatically), then the other speaker from there.
    void startNear (int speaker);
    void startNearRun();
    void nearFinished (std::vector<float> capture, bool stopped, bool noInput);
    juce::String nearKey() const;          // setup the near-field data belongs to
    void clearNear();
    bool nearMeasured() const noexcept { return nearDone[0] || nearDone[1]; }
    /** Clock-sync buffering of the microphone and the speakers now (ms): moves a little, taken out of every arrival time. */
    double syncBufferMs() const;
    /** Where the speakers and the microphone positions are (needs the near field). */
    room::Layout layout() const { return room::locate (geometry); }

    // Measurement
    std::unique_ptr<MeasureRun> makeRun (std::vector<float> stimulus, int outChannels);
    void startPosition (bool repeat);
    void startPass();                     // the next sweep of the current position
    void passFinished (std::vector<float> capture, bool stopped, bool noInput);
    /** Arrival time of one sweep (where the microphone was), in the background; then the position is checked. */
    void evaluatePosition (int side, int index, std::vector<float> capture, double syncMs);
    void checkPosition (int index);
    /** The measuring picture to scale (Measure step): speakers, planned marks, measured positions. */
    LayoutPicture measurePicture (const room::Layout&) const;
    void analyseAll();
    int sides() const noexcept { return separate ? 2 : 1; }
    int positionsDone() const noexcept { return (int) captures[sides() - 1].size(); }   // complete positions
    juce::String configKey() const;

    // Result
    void designEq();
    void apply();
    void refine();                        // validating: adds the residual bands to the output's correction

    UiContext& ctx;
    int output = -1;
    Step step = Step::welcome;
    Mode mode = Mode::calibrate;
    std::shared_ptr<RoomMeasurement> original;   // validating: the measurement the correction came from
    double devBefore[2] {}, devNow[2] {};        // validating: deviation from flat (rms) before calibrating / now
    bool refined = false;

    // Choices
    bool separate = true;
    int positions = 5;
    int strength = 1, range = 0;          // presets of the result step
    std::unique_ptr<ChoiceCard> separateCard, togetherCard, posCards[3], strengthCards[3], rangeCards[3];

    juce::ComboBox micBox, micChannelBox, calBox, levelBox;
    std::vector<room::Calibration> calList;
    int shownCal = 1;
    juce::TextButton backButton { "Back" }, nextButton { "Next" }, deviceButton { "Change device..." },
                     checkButton { "Stop test noise" }, measureButton { "Measure" }, redoButton { "Repeat last position" },
                     analysisButton { "Detailed analysis..." }, fineTuneButton { "Fine-tune..." },
                     showCorrButton { "Show correction" }, restartButton { "Start over" },
                     keepButton { "Keep it as it is" },
                     abButton;
    juce::ToggleButton delayToggle { "Wait 5 seconds before each position (time to step back)" },
                       boostToggle { "Allow small boosts (up to +3 dB)" };
    struct Backdrop final : juce::Component
    {
        std::function<void()> onClick;
        void paint (juce::Graphics& g) override { g.fillAll (juce::Colours::black.withAlpha (0.55f)); }
        void mouseUp (const juce::MouseEvent&) override { if (onClick) onClick(); }
    };
    std::unique_ptr<Backdrop> pickerBackdrop;
    std::unique_ptr<DevicePicker> picker;
    juce::String shownChannels;
    int layoutTick = 0;

    // Running: the level check or one sweep (ownRun is only compared, never dereferenced after it finished).
    const MeasureRun* ownRun = nullptr;
    bool checking = false;
    int checkPattern = 0, checkBurst = 0, stopTicks = 0;
    float signalRms = 0.0f, noiseRms = 0.0f, clipPeak = 0.0f, micLevel = 0.0f;
    float micPreview = 0.0f;               // Microphone step: live level of the chosen input and channel
    int greenTicks = 0;                    // timer ticks the level has been in the green range
    bool levelSeen = false, levelGood = false;

    // Measurement: captures[side][position]
    room::SweepSpec spec;
    room::Calibration runCal;
    std::vector<std::vector<float>> captures[2];
    std::vector<double> captureSync[2];    // clock-sync buffering during each capture (ms)
    int redoIndex = -1;                    // a position measured again (click in the picture / "Repeat"), -1 = none
    std::vector<float> redoCapture[2];     // its sweeps until both speakers are done
    double redoSync[2] {};
    int pass = 0;                          // sweeps done of the current position (0 .. sides())
    bool counting = false;
    double countdownEnd = 0.0;
    juce::String passWarning, measuredKey;
    bool analysing = false;

    // Result: one measurement per side (side 0 = left or both)
    std::unique_ptr<RoomMeasurement> results[2];
    room::AutoEqResult eqs[2];
    int eqGeneration = 0, eqPending = 0, viewSide = 0;
    Chip sideChips[2] { Chip ("Left speaker"), Chip ("Right speaker") };
    std::unique_ptr<ResultChart> chart;

    // Near field
    int nearSpeaker = -1;                  // measuring now (-1 = not)
    int nearPhase = 0;                     // 0 = the near sweep, 1 = the other speaker
    int nearTries = 0;
    double nearLevelDb = -40.0;
    room::SweepSpec nearSpec;
    room::ImpulseResponse nearIr[2];
    bool nearDone[2] {};
    juce::String nearMeasuredKey;
    juce::TextButton nearButtons[2] { juce::TextButton ("Measure left speaker"), juce::TextButton ("Measure right speaker") };
    // Arrival times: near field, cross, every listening position (live: each sweep is evaluated right away).
    room::Geometry geometry;
    int posGeneration = 0;                 // a new series: evaluations still running are dropped
    double syncSum = 0.0, lastSyncMs = 0.0;
    int syncCount = 0;
    bool rangeChosen = false;              // the user picked a range (else Full range with a near field)

    juce::String status;                   // what just happened (measure / result steps)
    juce::Colour statusColour = theme::textDim;

    void mouseUp (const juce::MouseEvent&) override;   // click on a finished step in the step bar
    juce::AttributedString bodyText() const;
    juce::TextLayout bodyLayout;
    juce::Rectangle<int> stepsArea, artArea, textArea, footerArea, gaugeArea, statusArea, statsArea, micMeterArea;
    std::unique_ptr<juce::FileChooser> chooser;
};
} // namespace hydra

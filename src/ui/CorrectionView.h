#pragma once
#include "CorrectionWizard.h"
#include "state/CorrectionSync.h"

namespace hydra
{
//==============================================================================
/** Room correction of the hardware outputs in one window (inspector's expand button, Tools > Room Correction).

    Top: the output (H bus) it is about, its ON switch and four pages:
    - Correction: on the left the saved corrections (PoolList, shared with the Room Correction plugin: "Use" links one
      to the output) and the saved measurements (MeasurementList; "Load" makes the output's profile from one), the
      output's profile on the right (CorrectionPage),
    - Calibrate: the guided calibration (CorrectionWizard), the way to measure and make a correction,
    - Analysis: the loaded measurement in detail (AnalysisPage),
    - Edit: the correction designed from the loaded measurement with every setting, applied to this output (EditPage).
    The window follows the selected hardware output like the EQ window (not while the wizard holds its results). */
class CorrectionView final : public juce::Component, private juce::Timer
{
public:
    explicit CorrectionView (UiContext& c);
    void paint (juce::Graphics&) override;
    void resized() override;

    enum class Page { correction, wizard, analysis, edit };
    /** Shows a hardware output (selection index); others are ignored. Rebuilds the Correction page. */
    void showOutput (int selectIndex);
    void showPage (Page);
    int getOutput() const noexcept { return output; }
    /** True while a measurement runs or the wizard holds its results (the window then keeps its output). */
    bool isBusy() const;

private:
    void timerCallback() override;
    void refreshOutputs();
    void loadSelected();   // "Load" in the list: the output's correction from the selected measurement
    void syncLoaded();     // the loaded measurement follows the output's profile

    UiContext& ctx;
    int output = -1;
    Page page = Page::correction;
    juce::ComboBox outputBox;
    std::unique_ptr<Toggle> onToggle;
    Chip tabs[4] { Chip ("Correction"), Chip ("Calibrate"), Chip ("Analysis"), Chip ("Edit") };
    RoomLibrary library;
    std::unique_ptr<BusCorrectionTarget> target;   // the shown output
    std::unique_ptr<CorrectionPage> correction;
    PoolList pools { pool::kMixer };
    MeasurementList list;
    CorrectionWizard wizard;
    AnalysisPage analysis;
    EditPage edit;
    juce::Rectangle<int> barArea;
    juce::String shownOutputs;
    uint32_t shownVersion = 0;
    bool previewLoaded = false;   // Analysis shows the wizard's measurement before it is applied
};
} // namespace hydra

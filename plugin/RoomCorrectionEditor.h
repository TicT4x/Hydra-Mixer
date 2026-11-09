#pragma once
#include "RoomCorrectionPlugin.h"
#include "ui/RoomPages.h"

namespace hydra
{
/** The plugin's window, laid out like the mixer's Room Correction window without the calibration wizard:
    a bar with ON and what the correction is linked to, and three pages:
    - Correction: the saved corrections (shared pool) and the measurements on the left, the plugin's correction
      on the right (curve, level, bands),
    - Analysis: the loaded measurement in detail,
    - Edit: the correction designed from the loaded measurement, every change goes to the plugin (and the pool). */
class RoomCorrectionEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit RoomCorrectionEditor (RoomCorrectionProcessor&);
    ~RoomCorrectionEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    enum class Page { correction, analysis, edit };
    void timerCallback() override;
    void showPage (Page);
    void loadSelected();
    void syncLoaded();
    juce::String linkText() const;

    /** The app's look while a plugin window is open (shared by all instances in the host). */
    struct Look
    {
        Look()  { juce::LookAndFeel::setDefaultLookAndFeel (&lnf); }
        ~Look() { juce::LookAndFeel::setDefaultLookAndFeel (nullptr); }
        theme::LookAndFeel lnf;
    };
    juce::SharedResourcePointer<Look> look;
    theme::TooltipWindow tooltips;

    RoomCorrectionProcessor& proc;
    Page page = Page::correction;
    juce::ToggleButton onButton { "ON" };
    Chip tabs[3] { Chip ("Correction"), Chip ("Analysis"), Chip ("Edit") };
    RoomLibrary library;
    CorrectionPage correction { proc };
    PoolList pools { pool::kPlugin };
    MeasurementList list { library };
    AnalysisPage analysis { library };
    EditPage edit { library };
    juce::Rectangle<int> barArea, linkArea;
    uint32_t shownVersion = ~0u;
    juce::String shownLink;
};
} // namespace hydra

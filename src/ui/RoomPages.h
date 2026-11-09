#pragma once
#include "Controls.h"
#include "state/RoomMeasurements.h"
#include "state/CorrectionPool.h"

namespace hydra
{
/** The saved room measurements (%APPDATA%\HydraMixer\Measurements), shared by the pages of the Room Correction
    window. The list on the Correction page selects one (current) and loads it; the loaded one is the measurement the
    output's correction comes from (CorrectionProfile::source), Analysis and Edit show it. Measurements are analysed
    in the background (both speakers of a left / right one), with the waterfall length the Analysis page asks for.
    Message thread only. */
class RoomLibrary
{
public:
    RoomLibrary();

    struct Listener
    {
        virtual ~Listener() = default;
        /** The list, the selection or an analysis changed. */
        virtual void libraryChanged() = 0;
    };
    void addListener (Listener* l)    { listeners.add (l); }
    void removeListener (Listener* l) { listeners.remove (l); }

    const std::vector<std::unique_ptr<RoomMeasurement>>& items() const noexcept { return list; }
    int size() const noexcept { return (int) list.size(); }
    int currentIndex() const noexcept { return selected; }
    RoomMeasurement* current() const;
    int indexOf (const RoomMeasurement*) const;
    void select (int index);
    /** A new measurement (already saved in the library folder): first in the list and selected. */
    void add (std::unique_ptr<RoomMeasurement>);
    /** Removes it from the list and deletes its file. */
    void remove (RoomMeasurement*);
    void rename (RoomMeasurement*, const juce::String& name);
    /** Another microphone calibration: saved, analysed again. */
    void setCalibration (RoomMeasurement*, const room::Calibration&);
    /** The measurement the output's correction is made from (Analysis, Edit), nullptr if none. */
    RoomMeasurement* loaded() const noexcept { return loadedPtr; }
    void setLoaded (RoomMeasurement*);
    /** The measurement saved as this file name (CorrectionProfile::source), nullptr if there is none. */
    RoomMeasurement* findByFile (const juce::String& fileName) const;

    /** Inside a DAW (the plugin): positions are analysed one after another, and waterfall / decay / impulse of the loaded
        measurement only once something shows them (allowFull), so the analysis does not take the cores from the audio. */
    void setLightweight (bool);
    /** The full analysis of the loaded measurement is wanted now (the Analysis page is shown). */
    void allowFull();

    /** Waterfall length (ms) of the analyses; a new one analyses the selected measurement again. */
    double getWaterfallMs() const noexcept { return waterfallMs; }
    void setWaterfallMs (double ms);
    /** Analyses m (both speakers) in the background: first quickly the response (enough for a correction, m->analysed),
        then, if full, waterfall / decay / impulse with the current waterfall length. */
    void analyseIfNeeded (RoomMeasurement* m, bool full = true);
    /** The full analysis (waterfall...) of m is there. */
    static bool isComplete (const RoomMeasurement& m) { return m.analysed && ! m.analysis.waterfall.empty(); }
    bool isAnalysing (const RoomMeasurement* m) const;

private:
    void changed() { listeners.call (&Listener::libraryChanged); }
    void analyseSide (RoomMeasurement*, bool full);
    bool wantsFull (const RoomMeasurement*) const;   // the loaded one (or its right speaker)

    std::vector<std::unique_ptr<RoomMeasurement>> list;
    int selected = -1;
    RoomMeasurement* loadedPtr = nullptr;
    double waterfallMs = 300.0;
    bool parallel = true, deferFull = false;
    std::vector<const RoomMeasurement*> analysing;
    juce::ListenerList<Listener> listeners;
    JUCE_DECLARE_WEAK_REFERENCEABLE (RoomLibrary)
};

//==============================================================================
/** Speakers and microphone positions seen from above, to scale (room::Layout): the speakers' distance, the distance
    and angle from the listening position (the first spot) to each speaker, the ideal stereo triangle (dashed), the
    other measured spots, and optionally planned spots (rings) with the next one marked. */
struct LayoutPicture
{
    const room::Layout* layout = nullptr;
    int highlight = -1;                           // a measured spot drawn bigger (the latest)
    std::vector<juce::Point<double>> planned;     // metres, same coordinates as the layout
    int nextPlanned = -1;
    int playing = 0;                              // bit 0 = left, bit 1 = right speaker plays
    bool summary = true;                          // the line with the numbers under the picture
    double toleranceM = 0.0;                      // grey area around each planned spot; a measured spot outside its own turns yellow
};
void drawSpeakerLayout (juce::Graphics&, juce::Rectangle<float> area, const LayoutPicture&);
/** The measured spot (or else the planned ring) under a point of the picture, -1 if none. */
int hitLayoutSpot (juce::Rectangle<float> area, const LayoutPicture&, juce::Point<float> at);

//==============================================================================
/** A choice drawn as a card: picture, title, short explanation, optional "Recommended" badge, radio mark
    (the calibration wizard and the Edit page). */
class ChoiceCard final : public juce::Component
{
public:
    using Painter = std::function<void (juce::Graphics&, juce::Rectangle<float>, bool selected)>;
    ChoiceCard (juce::String t, juce::String d, Painter p = {}, juce::String b = {})
        : title (std::move (t)), detail (std::move (d)), badge (std::move (b)), painter (std::move (p)) {}

    std::function<void()> onClick;
    bool selected = false, iconTop = false;

    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { hover = false; repaint(); }
    void mouseUp (const juce::MouseEvent& e) override  { if (e.mouseWasClicked() && isEnabled() && onClick) onClick(); }

private:
    juce::String title, detail, badge;
    Painter painter;
    bool hover = false;
};

//==============================================================================
/** An accent button with a "+" symbol in front of its text. */
class AddButton final : public juce::TextButton
{
public:
    using juce::TextButton::TextButton;
    void paintButton (juce::Graphics&, bool over, bool down) override;
};

//==============================================================================
/** The measurement library on the left of the Correction page: select, rename (double click), import, export,
    delete, microphone calibration (right click). "Load" makes the output's correction from the selected measurement;
    the loaded one is marked, Analysis and Edit show it. */
class MeasurementList final : public juce::Component, private juce::ListBoxModel, private RoomLibrary::Listener
{
public:
    explicit MeasurementList (RoomLibrary&);
    ~MeasurementList() override;
    void paint (juce::Graphics&) override;
    void resized() override;

    std::function<void()> onLoad, onCalibrate;

private:
    void libraryChanged() override;
    int getNumRows() override { return lib.size(); }
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    void selectedRowsChanged (int row) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override { startRename (row); }
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    void deleteKeyPressed (int) override { deleteSelected(); }

    void importFile();
    void showExportMenu();
    void deleteSelected();
    void startRename (int row);
    void finishRename (bool apply);

    RoomLibrary& lib;
    juce::ListBox list { "Measurements", this };
    juce::TextButton importButton { "Import..." }, exportButton { "Export..." }, deleteButton { "Delete" }, loadButton { "Load" };
    AddButton calibrateButton { "Calibrate speakers" };
    std::unique_ptr<juce::TextEditor> renameEditor;
    int renameRow = -1;
    juce::Rectangle<int> emptyArea;
    std::unique_ptr<juce::FileChooser> chooser;
};

//==============================================================================
/** The shared correction pool (pool::list): every correction made in the mixer or in the Room Correction plugin.
    "Use" puts the selected one on the target, linked (later changes of it, in either app, follow). Double click
    renames, Delete removes it from the pool (targets using it keep their correction, unlinked). The list follows
    the folder (checked once a second). */
class PoolList final : public juce::Component, private juce::ListBoxModel, private juce::Timer
{
public:
    explicit PoolList (juce::String madeInApp);
    ~PoolList() override;
    void paint (juce::Graphics&) override;
    void resized() override;

    void setTarget (CorrectionTarget*);
    /** After "Use". */
    std::function<void()> onUse;

private:
    void timerCallback() override;
    void refresh (bool force);
    int getNumRows() override { return (int) entries.size(); }
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    void selectedRowsChanged (int) override { updateButtons(); }
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override { startRename (row); }
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    void deleteKeyPressed (int) override { deleteSelected(); }
    void updateButtons();
    void useSelected();
    void deleteSelected();
    void startRename (int row);
    void finishRename (bool apply);
    const pool::Entry* selectedEntry() const;

    juce::String madeIn;
    CorrectionTarget* target = nullptr;
    std::vector<pool::Entry> entries;
    juce::String signature, shownLinked;
    juce::ListBox list { "Corrections", this };
    juce::TextButton useButton { "Use" }, deleteButton { "Delete" };
    std::unique_ptr<juce::TextEditor> renameEditor;
    int renameRow = -1;
    juce::Rectangle<int> emptyArea;
};

//==============================================================================
/** Analysis page: the loaded measurement in detail, nothing to set up. Key numbers on top, the chart (response,
    waterfall 3D, decay, impulse) with a short explanation of what it shows, and the room's resonances. A left / right
    measurement shows one speaker at a time (chips). */
class AnalysisPage final : public juce::Component, private RoomLibrary::Listener
{
public:
    explicit AnalysisPage (RoomLibrary&);
    ~AnalysisPage() override;
    void paint (juce::Graphics&) override;
    void resized() override;

    std::function<void()> onCalibrate;
    enum class Mode { response, waterfall, decay, impulse, positions };

private:
    class Chart;

    void libraryChanged() override;
    void setMode (Mode);
    void refreshCompareBox();
    RoomMeasurement* shown() const;   // the loaded measurement's shown speaker
    void waterfallSettingChanged (juce::ComboBox&);
    void showExportMenu();
    juce::AttributedString explanation() const;

    RoomLibrary& lib;
    Mode mode = Mode::response;
    juce::ComboBox viewOption, compareBox, wfLowBox, wfHighBox, wfLengthBox;
    Chip tabs[5] { Chip ("Response"), Chip ("Waterfall 3D"), Chip ("Decay (RT60)"), Chip ("Impulse"), Chip ("Positions") };
    Chip sideChips[2] { Chip ("Left speaker"), Chip ("Right speaker") };
    int viewSide = 0;
    juce::TextButton exportButton { "Export..." }, calibrateButton { "Calibrate speakers..." };
    double wfLo = 20.0, wfHi = 500.0;
    const RoomMeasurement* comparePtr = nullptr;
    std::unique_ptr<Chart> chart;
    juce::Rectangle<int> headArea, tilesArea, sideArea, explainArea, resonanceArea, emptyArea;
    juce::TextLayout explainLayout;
    std::unique_ptr<juce::FileChooser> chooser;
    friend class Chart;
};

//==============================================================================
/** Edit page: the correction of the window's output, designed from the loaded measurement (both speakers of a left /
    right one), with every setting of the automatic design, shown before / after. Every change goes to the output
    right away; the settings are kept in the profile (CorrectionProfile::design) and shown again when it is loaded. */
class EditPage final : public juce::Component, private RoomLibrary::Listener, private juce::Timer
{
public:
    explicit EditPage (RoomLibrary&);
    ~EditPage() override;
    void paint (juce::Graphics&) override;
    void resized() override;

    /** Where the correction goes (an output of the mixer, the plugin), nullptr: nowhere. */
    void setTarget (CorrectionTarget*);
    /** Loads m: designs its correction with the current settings and puts it on the output (no question). */
    void load (RoomMeasurement* m);
    std::function<void()> onCalibrate;

private:
    class Chart;

    void libraryChanged() override;
    void timerCallback() override;
    void takeSelection();          // the measurements to edit: the loaded one's speakers
    void design();
    void choosePreset (int);
    int matchingPreset() const;
    void updateControls();
    void userChanged();              // a setting changed: design again and put it on the output
    void takeDesign();               // the controls from the output's profile (if it came from the loaded measurement)
    void applyNow();
    void refreshCalBox();            // the loaded measurement's microphone calibration
    void calChosen();
    void importCalibration();
    CorrectionProfile profile() const;   // what Apply would set (the parts that are designed)
    int sides() const noexcept { return measured[1] != nullptr ? 2 : 1; }
    /** Every speaker of the loaded measurement is analysed (a design then covers all of them). */
    bool ready() const noexcept { return measured[0] != nullptr && measured[0]->analysed && (measured[1] == nullptr || measured[1]->analysed); }

    RoomLibrary& lib;
    CorrectionTarget* target = nullptr;
    RoomMeasurement* measured[2] {};    // side 0 = left (or the only one), 1 = right of a pair
    room::AutoEqResult eqs[2];
    int generation = 0, pending = 0, viewSide = 0, dirtyTicks = 0;
    bool applyWhenDesigned = false;   // Load: puts the design on the output as soon as it is ready

    juce::ComboBox lowBox, highBox, calBox;
    std::vector<room::Calibration> calList;
    int shownCalId = 1;
    juce::String shownCalName;
    const RoomMeasurement* shownCalFor = nullptr;   // only compared
    juce::Slider cutSlider, boostSlider, bandsSlider, detailSlider;
    juce::ToggleButton nearToggle { "Use the speakers' near field above 400 Hz" },
                       alignToggle { "Time-align left and right" };
    std::unique_ptr<ChoiceCard> presetCards[3], levelCards[2];
    bool keepLoudness = true;
    Chip sideChips[2] { Chip ("Left speaker"), Chip ("Right speaker") };
    juce::TextButton calibrateButton { "Calibrate speakers..." };
    std::unique_ptr<Chart> chart;
    juce::String status;
    juce::Colour statusColour = theme::textDim;
    juce::Rectangle<int> headArea, settingsArea, statsArea, footerArea, statusArea, emptyArea;
    std::vector<std::pair<juce::Component*, juce::String>> captions;
    std::unique_ptr<juce::FileChooser> chooser;
    friend class Chart;
};
//==============================================================================
/** The Correction page of a target (an output of the mixer, the plugin): the profile's curve with its bands, the level
    (preamp), clear, and (mixer only) validating it. */
class CorrectionPage final : public juce::Component, private juce::Timer
{
public:
    explicit CorrectionPage (CorrectionTarget&);
    ~CorrectionPage() override;
    void paint (juce::Graphics&) override;
    void resized() override;

    /** Validate / Refine (shown only if set). */
    std::function<void()> onValidate;
    /** Shown instead of the curve while there is no profile. */
    juce::String emptyHint { "No correction profile yet.\n\n\"Calibrate speakers\" guides you step by step through measuring your "
                             "speakers in the room (the button at the top of the measurements)." };
    /** Validating needs a profile made from a measurement of the library (the loaded one). */
    void setCanValidate (bool);

private:
    class Curve;

    void timerCallback() override;
    void profileChanged();
    void setPreamp (float db);
    void clear();
    void exportApo();

    CorrectionTarget& target;
    juce::Colour colour;
    juce::Slider level;
    juce::TextButton validateButton { "Validate / Refine..." }, exportButton { "Export APO..." }, clearButton { "Clear" };
    std::unique_ptr<Curve> curve;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Rectangle<int> infoArea, levelArea, levelInfoArea, bandsArea;
    uint32_t shownVersion = ~0u;
    bool shownOn = false;
};

} // namespace hydra

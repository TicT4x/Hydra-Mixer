#pragma once
#include "Widgets.h"

namespace hydra
{
/** The recorder's full view (the expand button of the recorder in the inspector, whose compact view shows the
    same recorder; 13.11., wish of the user, modelled on Ableton's arrangement view):
    - left: what is recorded (a V bus, or inputs with arm buttons; separate tracks, pre / post effects, folder,
      format),
    - middle: the tracks of the open recording one below the other with their clips (waveforms) on a timeline:
      move, trim, split, delete, fades, loop range; non-destructive, with its own undo (Ctrl+Z while the timeline
      has the focus),
    - right of the lanes: the strip of every track (mute, solo, mono, gain, pan, fader, meter),
    - right: where playback goes (the recorder's Play To buses) and its level,
    - bottom: the effect chain of the selected track (the same blocks as the inspector),
    - top: transport, the recordings in the folder, export (stems / mixdown).
    Nothing of it runs while the window is closed (no timers, no repaints). */
class RecorderWindow final : public juce::Component, private juce::Timer
{
public:
    explicit RecorderWindow (UiContext&);
    ~RecorderWindow() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    void visibilityChanged() override;
    void parentHierarchyChanged() override;

    std::function<void()> onScanRequested;

    class Arrange;
    class TrackHeader;
    class ExportPanel;

private:
    void timerCallback() override;
    void frame (double timestamp);
    void rebuild();               // tracks / sources changed
    void rebuildSources();
    void layoutSources (juce::Rectangle<int>);
    void selectTrack (int t);
    void showProjectsMenu();
    /** Right-click on a track (lane or strip): rename, add an audio file as a track, delete the track. */
    void showTrackMenu (int track, double atSeconds);
    /** Asks for an audio file and adds it as a new track at the play position (a single file first becomes a
        multitrack recording). */
    void importTrack();
    void showExport();
    void updateTransportText();
    bool running() const;         // something moves (playing / recording / exporting): animate

    UiContext& ctx;
    Recorder& rec;
    std::unique_ptr<Arrange> arrange;
    std::vector<std::unique_ptr<TrackHeader>> headers;
    std::unique_ptr<juce::Component> fxRack;
    std::unique_ptr<Toggle> rackBypass, rackPost;   // the selected track's chain: BYPASS, PRE / POST
    void makeRack();
    std::unique_ptr<ExportPanel> exportPanel;
    int selectedTrack = 0;
    int shownTracks = -1, shownTakeTracks = -1;
    uint32_t shownState = 0, shownLayout = 0;
    bool shownModeInputs = false, fitted = false;
    uint64_t shownAudible = 0;   // mute / solo of the tracks: lanes and strips grey out
    juce::File shownProject;

    // transport (top)
    Toggle recBtn, playBtn, stopBtn, loopBtn;
    Chip projectChip, exportChip, splitChip, deleteChip, cropChip, undoChip, redoChip, closeChip;
    juce::String timeText, statusText;

    // record source (left)
    ChoiceSegments modeSeg;
    std::vector<std::unique_ptr<Toggle>> busSources;
    struct ArmRow
    {
        int input = 0;
        std::unique_ptr<Toggle> arm;
        std::unique_ptr<MeterView> meter;
    };
    std::vector<ArmRow> armRows;
    Switch separateSwitch;
    ChoiceSegments tapSeg;
    Chip folderChip, formatChip;
    juce::Rectangle<int> sourceArea, outputArea, rackArea, headerArea, rackTitle;

    // outputs (right)
    std::vector<std::unique_ptr<Toggle>> outs;
    Knob level;

    double lastFrame = 0.0;
    juce::VBlankAttachment vblank { this, [this] (double t) { frame (t); } };
    static constexpr int kSourceWidth = 230, kOutputWidth = 150, kHeaderWidth = 200, kTopHeight = 52;
};
} // namespace hydra

#pragma once
#include "Widgets.h"
#include "Dialog.h"

namespace hydra
{
/** Guided tour over the real mixer (Tools > Guided tour, offered after the welcome wizard).

    An overlay on the main window dims everything except the part a step explains (the "spotlight"), a card
    next to it says what it is in a sentence or two. Routing is shown with animated paths: dots flow from an
    input's routing button to the strip of the output it reaches. Many steps let the user do it (click H1,
    move the fader, open the source); the tour notices and moves on. Clicks reach the mixer only inside the
    spotlight. Closing (x, Esc) pauses: the step is kept in the session (prefs.tour = "full:5"), and a small card
    offers Continue / Restart, now or after the next start.

    The tour finds what it explains by component ids (Strips.cpp: "in0", "in0.route.8", "bus3.output", ...;
    MainComponent: "inspector", "top.tools", ...), so it follows the layout, scaling and scrolling. Animation runs
    only while the tour is open and repaints only the moving parts. */
class Tour final : public juce::Component
{
public:
    enum class Kind { full, voicemeeter };

    /** What the tour needs from the main window. */
    struct Host
    {
        std::function<void (int selectIndex)> reveal;          // scroll a channel into view
        std::function<bool (const juce::String&)> windowOpen;  // "device" (device picker), "settings"
        std::function<void (juce::Rectangle<int>)> moveDeviceWindowAside;   // keep it off this area (screen coordinates)
    };

    Tour (UiContext&, juce::Component& root, Host, Kind, int step);
    ~Tour() override;

    /** Closed: finished = true when the last step was done (or the tour was ended), else paused at `step`. */
    std::function<void (bool finished, int step)> onClose;

    Kind getKind() const noexcept { return kind; }
    int getStep() const noexcept { return current; }
    int getNumSteps() const noexcept;
    /** Esc from the main window while the user works inside the spotlight. */
    void pause();
    /** Screenshot tool (tools/Shots.cpp) only: show a step at an animation time. */
    void demoShow (int step, double t);

    static juce::String kindId (Kind k) { return k == Kind::voicemeeter ? "vm" : "full"; }
    /** prefs.tour "full:3" / "vm:1" -> kind and step; false for "" / "done". */
    static bool parseState (const juce::String&, Kind&, int& step);

    void paint (juce::Graphics&) override;
    void resized() override;
    bool hitTest (int x, int y) override;
    bool keyPressed (const juce::KeyPress&) override;
    void parentSizeChanged() override;

private:
    struct Step;
    class Card;

    void buildFull();
    void buildVoicemeeter();
    void show (int step);
    void next();
    void back();
    void finish();
    void frame (double t);
    void updateSpots();
    void placeCard();

    juce::Component* find (const juce::String& id) const;
    mutable std::map<juce::String, juce::Component::SafePointer<juce::Component>> found;
    juce::Rectangle<int> rectOf (const juce::String& id) const;     // empty if not shown
    juce::Rectangle<int> groupRect (int group) const;               // 0 inputs, 1 H outputs, 2 V outputs
    juce::Point<float> routeStart (int input, int bus) const;
    juce::Point<float> busEnd (int bus) const;
    void paintPaths (juce::Graphics&, float t);
    uint32_t chainVersion() const;   // effect chain of the selected channel

    // State when a step began, to notice the user did the task.
    bool routeWas = false, sawOff = false, sawAway = false, pickerSeen = false;
    uint32_t gainVersion = 0, chainWas = 0;
    int selectedWas = -1;

    UiContext& ctx;
    juce::Component& root;
    Host host;
    Kind kind;
    std::vector<Step> steps;
    int current = 0;
    int in = 0, hw = 0, virt = -1;   // channels the tour uses: first input, first H and V output (-1 = none)

    std::vector<juce::Rectangle<int>> spots;   // in this component; [0..interactiveCount) take clicks
    int interactiveCount = 0;
    juce::Rectangle<int> animArea;             // repainted every frame
    std::unique_ptr<Card> card;
    bool taskDone = false;
    double doneAt = 0.0, startTime = 0.0, lastT = 0.0;
    juce::VBlankAttachment vblank { this, [this] (double t) { frame (t); } };
};

/** "Tour paused at step 5 of 14 - Continue / Restart / x" at the bottom right of the main window. */
class TourResumeCard final : public juce::Component
{
public:
    TourResumeCard (juce::String text);
    std::function<void (int)> onChoice;   // 0 continue, 1 restart, 2 dismiss
    void paint (juce::Graphics&) override;
    void resized() override;
    static constexpr int kWidth = 380, kHeight = 92;

private:
    juce::String text;
    dialog::ActionButton cont { { "Continue", dialog::Button::Role::primary }, theme::accentIn },
                         restart { { "Restart" }, theme::accentIn };
    juce::Rectangle<int> closeArea;
    void mouseUp (const juce::MouseEvent&) override;
};
} // namespace hydra

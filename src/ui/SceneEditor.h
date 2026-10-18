#pragma once
#include "Widgets.h"
#include "state/Scenes.h"

namespace hydra
{
/** Scene Editor: sidebar on the left of the mixer (Tools > Scene Editor).

    Lists the scenes; a click on a scene recalls it at once (no questions, also with unsaved edits, so it
    can be used live) and makes it the scene being edited. Changes in the mixer are only kept in the scene
    with "Save" (the row shows "edited" until then). Every scene has a recall button that can be MIDI-learned
    or get a keyboard shortcut (right-click, or the buttons below the list); Prev / Next step through the
    list. New, rename, duplicate, move, delete, export and import are in the header, the footer and the
    right-click menu of a row. */
class SceneSidebar final : public juce::Component, private juce::Timer
{
public:
    SceneSidebar (UiContext& c, SceneBank& bank);
    ~SceneSidebar() override;

    std::function<void()> onClose;
    std::function<void (const juce::String&)> onMessage;   // short feedback (toast)

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int kWidth = 290;

private:
    class Row;
    void timerCallback() override;
    void rebuild();
    void refreshState();
    void select (int i);   // recall + edit
    void addScene();
    void saveCurrent();
    void deleteScene (int i);
    void moveScene (int from, int to);
    void renameScene (int i);
    void showRowMenu (int i, juce::Component& target);
    void exportScenes (bool onlySelected);
    void importScenes();
    void applyTriggerMoves (const std::vector<std::pair<Param*, Param*>>& moves, Param* removed);
    juce::String assignmentText (int i) const;

    UiContext& ctx;
    SceneBank& bank;
    Chip closeChip { "X", theme::text }, addChip { "+ New", theme::good }, importChip { "Import...", theme::text },
         exportChip { "Export...", theme::text };
    Chip saveChip { "Save", theme::text }, revertChip { "Revert", theme::text }, learnChip { "MIDI Learn", theme::text },
         keyChip { "Shortcut...", theme::text }, moreChip { "More", theme::text };
    std::unique_ptr<Toggle> prevBtn, nextBtn;
    juce::Viewport viewport;
    juce::Component list;
    std::vector<std::unique_ptr<Row>> rows;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Rectangle<int> footerArea, stepArea, listArea;
    uint32_t seenBank = ~0u, seenMap = ~0u, seenKeys = ~0u;
    int differences = 0, shownCurrent = -2;
};
} // namespace hydra

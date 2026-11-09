#pragma once
#include "Widgets.h"

namespace hydra
{
/** Optional panel right of the strips (Settings > Display > Side panel). It only appears while the
    strips leave enough room (MainComponent::resized). Content for now: the routing matrix, every input
    against every bus in use. Each cell is the input's routing button (a Toggle, so MIDI Learn and keyboard
    shortcuts work like on the strips). Click a row name to select the input. */
class SidePanel final : public juce::Component, private juce::Timer
{
public:
    explicit SidePanel (UiContext& c);
    ~SidePanel() override;

    /** Recreates the matrix for the channels in use. */
    void rebuild();
    /** Channel names changed. */
    void refresh();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;

    static constexpr int kMinWidth = 300, kMaxWidth = 420;

private:
    void timerCallback() override;
    int rowAt (int y) const;

    UiContext& ctx;
    std::vector<int> inputs, buses;                       // shown channels (slot indices)
    std::vector<std::unique_ptr<Toggle>> cells;           // inputs.size() x buses.size(), row by row
    juce::Rectangle<int> grid;                            // cells area (without names / codes)
    int nameWidth = 0, rowH = 0, colW = 0, shownSelected = -1;
    juce::StringArray shownNames;
};
} // namespace hydra

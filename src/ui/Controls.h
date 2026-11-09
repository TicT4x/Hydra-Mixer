#pragma once
#include "Theme.h"
#include "Dialog.h"

namespace hydra
{
/* Controls without parameters or mixer state (no Engine): used by the mixer and by the Room Correction plugin. */

/** The main window's content (set by MainComponent): other windows open on the screen that shows it. */
inline juce::Component* appComponent = nullptr;
/** Moves a top-level window to the centre of the screen the main window is on: always, or only if it is
    not on that screen yet (a window that was opened before on another screen comes back to the app). */
void placeOnAppDisplay (juce::Component& window, bool always);

/** Removes the thin light frame Windows 11 draws around a top-level window (no effect on older Windows). */
void removeNativeWindowBorder (juce::Component& window);

/** Lets components glide to new bounds instead of jumping (blocks and strips making room, rearranged layouts).
    Only runs while something moves; while its owner is not on screen, moves are applied at once. */
class LayoutAnimator
{
public:
    explicit LayoutAnimator (juce::Component& ownerComponent, double timeConstantSeconds = 0.05)
        : owner (ownerComponent), timeConstant (timeConstantSeconds) {}

    /** Glides c from where it is now to target. snapSize: only the position glides, the size is taken at once
        (moving a component without resizing it costs next to nothing, its children are not laid out again). */
    void moveTo (juce::Component& c, juce::Rectangle<int> target, bool snapSize = false);
    /** Moves c sideways now, also during a glide (it keeps its target): c's parent moved and c should stay where it is on screen. */
    void shift (juce::Component& c, int dx);
    /** Puts c at target at once (a glide in progress ends). */
    void place (juce::Component& c, juce::Rectangle<int> target);
    /** Forget a component: it is about to be moved by hand or deleted. */
    void forget (const juce::Component& c);
    void clear() { items.clear(); }
    bool isMoving() const noexcept { return ! items.empty(); }

    std::function<void()> onFrame;   // after each step (e.g. what is drawn depends on the neighbours)

private:
    struct Item { juce::Component::SafePointer<juce::Component> comp; juce::Rectangle<float> pos; juce::Rectangle<int> target; };
    void frame (double t);

    juce::Component& owner;
    double timeConstant, lastFrame = 0.0;
    std::vector<Item> items;
    juce::VBlankAttachment vblank { &owner, [this] (double t) { frame (t); } };
};

/** Legend for a tool window header: colour swatches with names, right-aligned in area. */
void drawLegend (juce::Graphics&, juce::Rectangle<int> area, const std::vector<std::pair<juce::String, juce::Colour>>& items);

//==============================================================================
/** Clickable text chip (source/output selectors, header buttons). */
class Chip : public juce::Component, public juce::SettableTooltipClient
{
public:
    Chip (juce::String t = {}, juce::Colour c = theme::textDim) : text (std::move (t)), colour (c) {}
    std::function<void()> onClick;
    juce::String text;
    juce::Colour colour;
    bool active = false, showArrow = true, centred = false;
    bool flat = false;   // Deep theme: only a line below the text instead of a box (device of a strip)

    /** Symbol drawn left of the text (or alone, centred, when the text is empty). */
    enum class Icon { none, save, revert };
    Icon icon = Icon::none;
    static void drawIcon (juce::Graphics&, Icon, juce::Rectangle<float> area, juce::Colour);

    /** Status dots left of the name (device problems): red = offline, yellow = dropouts or something that
        needs attention. Problems of the same colour share one dot. The chip's tooltip starts with what the
        dots mean (dropouts: how many and when). */
    enum class Marker { attention, offline, dropouts, connected };   // connected: green dot (Hydra Link)
    struct Status { Marker kind; juce::String tip; };
    /** Replaces the status dots; repaints only if they changed. */
    void setStatus (std::vector<Status>);
    const std::vector<Status>& getStatus() const noexcept { return status; }
    static void drawMarker (juce::Graphics&, Marker, juce::Rectangle<float> area);

    void paint (juce::Graphics&) override;
    juce::String getTooltip() override;
    void mouseUp (const juce::MouseEvent& e) override { if (e.mouseWasClicked() && onClick) onClick(); }
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { hover = false; repaint(); }

protected:
    bool hover = false;

private:
    std::vector<Status> status;
    std::vector<juce::Rectangle<float>> markerAreas;   // where each status symbol was drawn
};

/** Chip with a trash-can icon (remove a plugin, delete a recording); turns red on hover, dimmed when disabled. */
class TrashButton final : public Chip
{
public:
    TrashButton() { setMouseCursor (juce::MouseCursor::PointingHandCursor); }
    void paint (juce::Graphics&) override;
    static void drawIcon (juce::Graphics&, juce::Rectangle<float> area, juce::Colour);
};

//==============================================================================
/** Small "i" in a circle: hovering or clicking it shows an explanation (in the tooltip style) at once.
    Replaces descriptive text that would otherwise sit in the layout. */
class InfoButton final : public juce::Component, public juce::TooltipClient
{
public:
    explicit InfoButton (juce::String t = {}) : text (std::move (t)) { setMouseCursor (juce::MouseCursor::PointingHandCursor); }
    void setText (juce::String t) { text = std::move (t); }
    juce::String getTooltip() override { return text; }
    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override { hover = true; pinned = false; repaint(); show(); }
    void mouseExit (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override    { pinned = true; show(); }   // stays until the next click
    static constexpr int kSize = 16;

private:
    void show();
    juce::String text;
    bool hover = false, pinned = false;
};

//==============================================================================
/** Round-cornered "+" button (add a channel, add a plugin). */
class PlusButton final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit PlusButton (juce::Colour c = theme::textDim) : colour (c) { setMouseCursor (juce::MouseCursor::PointingHandCursor); }
    std::function<void()> onClick;
    juce::Colour colour;
    bool card = false;   // Deep theme: a card like the effect blocks with the plus in a round badge

    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent& e) override { if (e.mouseWasClicked() && onClick) onClick(); }
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { hover = false; repaint(); }

private:
    bool hover = false;
};
} // namespace hydra

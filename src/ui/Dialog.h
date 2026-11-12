#pragma once
#include "Theme.h"

namespace hydra
{
/** Confirmation and message dialogs in the app style (replaces juce::AlertWindow).

    The dialog is a card in the middle of the window that contains `near` (the main window or a tool window),
    above a dimmed backdrop. It is modal: the rest of the app takes no clicks until it is answered. Enter =
    the primary button, Esc / a click on the backdrop = cancel. Global hotkeys and MIDI keep working. */
namespace dialog
{
enum class Kind { question, warning, danger, info };

struct Button
{
    juce::String text;
    enum class Role { normal, primary, destructive, cancel } role = Role::normal;
};

/** Rounded button of the dialogs (also used by the welcome wizard and the tour). */
class ActionButton final : public juce::Component
{
public:
    ActionButton (Button b, juce::Colour accent) : spec (std::move (b)), accent (accent)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }
    std::function<void()> onClick;
    Button spec;
    void setText (juce::String t) { spec.text = std::move (t); repaint(); }

    int idealWidth() const
    {
        return juce::jmax (96, juce::GlyphArrangement::getStringWidthInt (theme::font (13.5f, true), spec.text) + 36);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        using Role = Button::Role;
        const bool filled = spec.role == Role::primary || spec.role == Role::destructive;
        auto fill = spec.role == Role::destructive ? theme::mute : spec.role == Role::primary ? accent : theme::raised;
        if (down)       fill = fill.brighter (0.18f);
        else if (hover) fill = fill.brighter (0.09f);
        g.setColour (fill);
        g.fillRoundedRectangle (r, 8.0f);
        if (! filled)
        {
            g.setColour (hover ? theme::textFaint : theme::outline);
            g.drawRoundedRectangle (r, 8.0f, 1.0f);
        }
        g.setColour (filled ? (spec.role == Role::destructive ? juce::Colours::white : theme::bg) : theme::text);
        g.setFont (theme::font (13.5f, true));
        g.drawText (spec.text, getLocalBounds(), juce::Justification::centred, true);
    }
    void enablementChanged() override { setAlpha (isEnabled() ? 1.0f : 0.4f); }
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { hover = false; repaint(); }
    void mouseDown (const juce::MouseEvent&) override  { down = true; repaint(); }
    void mouseUp (const juce::MouseEvent& e) override
    {
        down = false;
        repaint();
        if (e.mouseWasClicked() && contains (e.getPosition()) && onClick) onClick();
    }

private:
    juce::Colour accent;
    bool hover = false, down = false;
};


/** Shows a dialog. done (index): the clicked button (0-based, in the given order), -1 = cancelled
    (Esc, backdrop, window closed). Buttons are drawn right-aligned in the given order. */
void show (juce::Component* near, Kind, juce::String title, juce::String message,
           std::vector<Button> buttons, std::function<void (int)> done);

/** Two buttons: okText (primary, red when destructive) and Cancel. onOk runs only when confirmed. */
void confirm (juce::Component* near, Kind, juce::String title, juce::String message,
              juce::String okText, std::function<void()> onOk);

/** A message with a single OK button (errors, warnings). */
void message (juce::Component* near, Kind, juce::String title, juce::String text);

/** Asks for a line of text (a name): text field with `initial` selected, okText (primary) and Cancel.
    onOk gets the trimmed text; it is not called for an empty text or when cancelled. */
void askText (juce::Component* near, juce::String title, juce::String message, juce::String initial,
              juce::String okText, std::function<void (juce::String)> onOk);
} // namespace dialog
} // namespace hydra

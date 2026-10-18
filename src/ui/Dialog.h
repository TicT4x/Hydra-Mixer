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

/** Shows a dialog. done (index): the clicked button (0-based, in the given order), -1 = cancelled
    (Esc, backdrop, window closed). Buttons are drawn right-aligned in the given order. */
void show (juce::Component* near, Kind, juce::String title, juce::String message,
           std::vector<Button> buttons, std::function<void (int)> done);

/** Two buttons: okText (primary, red when destructive) and Cancel. onOk runs only when confirmed. */
void confirm (juce::Component* near, Kind, juce::String title, juce::String message,
              juce::String okText, std::function<void()> onOk);

/** A message with a single OK button (errors, warnings). */
void message (juce::Component* near, Kind, juce::String title, juce::String text);
} // namespace dialog
} // namespace hydra

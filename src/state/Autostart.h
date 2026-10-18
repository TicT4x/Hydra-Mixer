#pragma once
#include "Common.h"

namespace hydra
{
/** Start Hydra Mixer when the user logs on to Windows.

    Registered as a Task Scheduler task (logon trigger, no delay, high process priority, no time limit),
    which starts earlier than the "Run" registry key: Windows delays Run entries and starts them with
    normal priority after the desktop is up. If the task cannot be created, the Run key is used instead.
    The app is started with kArgument so it knows it was started with Windows. Message thread. */
namespace Autostart
{
    constexpr const char* kArgument = "--autostart";

    /** True if a task or Run entry for Hydra Mixer exists. */
    bool isEnabled();
    /** Creates / removes the entry. Returns an error text (empty on success). */
    juce::String setEnabled (bool enable);
    /** If enabled, re-creates the entry when it points to another exe (moved / rebuilt elsewhere). */
    void refreshIfMoved();
}
} // namespace hydra

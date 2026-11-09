#pragma once
#include "Widgets.h"
#include "state/VacCables.h"

namespace hydra
{
/** Cable Manager for Virtual Audio Cable (Tools > Cable Manager, "Manage Cables" in the device picker,
    Settings > Virtual Cables). One row per cable:
      # | name in Windows (double-click to rename both sides) | used by (Hydra channels, other streams)
        | format now | timing (ms per interrupt) | max. channels | health (VAC's own over-/underflows,
        measured event period) | remove (only the last cable, if nobody uses it)
    Above: driver version / edition / number of cables, "+ Input cable" / "+ Output cable" (a new channel with
    a cable of its own); below: the status of the running
    change, "Reset counters" and "Apply" when the driver waits for a restart.
    Refreshes twice a second while shown; rows are rebuilt only when the number of cables changes. */
class CableManager final : public juce::Component, private juce::Timer, private juce::ChangeListener
{
public:
    explicit CableManager (UiContext&);
    ~CableManager() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;

    std::function<void()> onClose;

private:
    class Row;
    class Group;

    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void reload();     // asks the driver again, updates the rows
    void report (bool ok, const juce::String& msg);
    VacCables* vac() const;
    /** New input / virtual output with a cable of its own (a free one, or one more). */
    void addChannelWithCable (bool input);

    UiContext& ctx;
    std::vector<vac::Cable> cables;
    std::vector<vac::Stream> streams;
    juce::OwnedArray<Row> rows;
    juce::OwnedArray<Group> groups;
    std::vector<int> rowOrder;      // row -> index into cables
    int groupSizes[3] {};
    juce::String layoutKey;
    juce::Viewport view;
    juce::Component rowHolder;
    InfoButton help;
    juce::TextButton addIn { "+ Input cable" }, addOut { "+ Output cable" }, reset { "Reset counters" }, apply { "Apply (restart driver)" }, done { "Done" };
    juce::String message;
    juce::Rectangle<int> headerArea, columnArea, footerText;
    int tick = 0;
};
} // namespace hydra

#pragma once
#include "Widgets.h"

namespace hydra
{
/** Device selection of a channel in its own window (click on a strip's source / output chip).

    Three columns from left to right: the kind of connection (master hardware, other ASIO interfaces, virtual
    cables, Windows devices, per-app capture, loopback), the devices of that kind and their channels (stereo pairs,
    mono channels). Only what fits the channel is offered: hardware outputs (A) get hardware, virtual outputs (B)
    software, Hydra's own cables never show up as Windows devices; Windows devices in one mode only (Low Latency)
    unless the expert setting shows all three. "Show all devices" lifts these filters. Devices and channels other
    channels use are marked with their codes. Inputs can capture several apps at once (tick them). Below them a short explanation of the chosen kind (latency, exclusivity).
    A click on a channel applies it right away and the window stays open (to try another one); a double-click
    applies and closes it. Columns that lead to the current choice carry a dot, the current channel a tick.
    The window follows the strip that opened it last (one window for all channels). */
class DevicePicker final : public juce::Component, private juce::Timer, private juce::ChangeListener
{
public:
    /** channel: input 0..kMaxInputs-1 or busSelectIndex (bus). */
    DevicePicker (UiContext& c, int channel);
    ~DevicePicker() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    int getChannel() const noexcept { return channel; }
    static juce::String windowTitle (const UiContext&, int channel);

    std::function<void()> onChanged;   // a new source / output was applied
    std::function<void()> onClose;

private:
    struct Option   { juce::String text, detail; IoTarget target; bool enabled = true; juce::String used; };
    struct Device   { juce::String name, detail, used; IoTarget stream; bool lazyAsio = false, loaded = true, createVac = false; std::vector<Option> options; };   // used: codes of other channels on it   // createVac: "+ New cable" (VacCables)
    struct Category { juce::String section, name, hint, empty, notice; bool isNone = false, autoVac = false, apps = false; std::vector<Device> devices; };   // autoVac: choosing it sets up a cable (VacCables)   // notice: always shown, in the footer
    class List;

    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;   // VacCables: cables added / removed / renamed
    bool showsVacCables() const;
    bool isCurrentDevice (const Device&, const IoTarget& cur) const;   // apps: one of the channel's apps
    /** Codes of the other active channels on this stream ("In 2, A1"); exact: only those whose channels overlap t's. */
    juce::String usedBy (const IoTarget& t, bool exact) const;
    void toggleApp (const juce::String& exe, bool onlyAdd);   // inputs capture a set of apps
    void buildModel();
    void loadOptions (Device&);
    IoTarget current() const;
    void apply (const IoTarget&);
    void selectCategory (int index);
    void selectDevice (int index);
    void showLists();
    void refreshDevices();   // re-reads the device lists (Refresh button, apps every 2 s)
    void refreshApps();      // asks in the background which apps play audio (updates the lists when they changed)
    void chooseFirstChannel();   // a device was picked: its top channel, unless it is the current device already
    void createCable();          // "+ New cable": Virtual Audio Cable gets one more, this channel takes it

    UiContext& ctx;
    const int channel;
    const bool isInput;
    std::vector<Category> categories;
    juce::StringArray appNames;
    bool appsLoading = false;
    juce::Rectangle<int> noticeArea;
    juce::String cableProgress;   // "+ New cable": what is happening / the result (footer)
    int category = -1, device = -1;
    IoTarget shown;
    int tick = 0;
    int prefsKey = -1;   // expert modes / show all as last seen (timer)
    uint32_t netChanges = 0;   // Hydra Link: change count of the hub's lists as last shown

    std::unique_ptr<List> catList, devList, optList;
    juce::Viewport catView, devView, optView;
    InfoButton hint;   // what the chosen kind of connection means (latency, exclusivity)
    juce::TextButton refresh { "Refresh" }, done { "Done" };
    juce::ToggleButton showAll { "Show all devices" };   // lifts the filters (prefs.pickerShowAll)
    juce::Rectangle<int> headerArea, columnTitles[3];
};
} // namespace hydra

#pragma once
#include "Widgets.h"

namespace hydra
{
/** Device selection of a channel in its own window (click on a strip's source / output chip).

    Three columns from left to right: the kind of connection (master hardware, other ASIO interfaces, the three
    Windows modes, per-app capture, virtual cables, loopback), the devices of that kind and their channels
    (stereo pairs, mono channels). Below them a short explanation of the chosen kind (latency, exclusivity).
    A click on a channel applies it right away and the window stays open (to try another one); a double-click
    applies and closes it. Columns that lead to the current choice carry a dot, the current channel a tick.
    The window follows the strip that opened it last (one window for all channels). */
class DevicePicker final : public juce::Component, private juce::Timer
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
    struct Option   { juce::String text, detail; IoTarget target; bool enabled = true; };
    struct Device   { juce::String name, detail; IoTarget stream; bool lazyAsio = false, loaded = true; std::vector<Option> options; };
    struct Category { juce::String section, name, hint, empty; bool isNone = false; std::vector<Device> devices; };
    class List;

    void timerCallback() override;
    void buildModel();
    void loadOptions (Device&);
    IoTarget current() const;
    void apply (const IoTarget&);
    void selectCategory (int index);
    void selectDevice (int index);
    void showLists();
    void refreshDevices();   // re-reads the device lists (Refresh button, apps every 2 s)

    UiContext& ctx;
    const int channel;
    const bool isInput;
    std::vector<Category> categories;
    int category = -1, device = -1;
    IoTarget shown;
    int tick = 0;

    std::unique_ptr<List> catList, devList, optList;
    juce::Viewport catView, devView, optView;
    juce::Label hint;
    juce::TextButton refresh { "Refresh" }, done { "Done" };
    juce::Rectangle<int> headerArea, columnTitles[3];
};
} // namespace hydra

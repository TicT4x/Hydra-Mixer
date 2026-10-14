#pragma once
#include "Widgets.h"

namespace hydra
{
/** Popup menus for choosing a strip's source / a bus's destination. */
namespace IoMenus
{
    void showInputMenu (juce::Component& target, UiContext& ctx, int strip, std::function<void()> changed);
    void showOutputMenu (juce::Component& target, UiContext& ctx, int bus, std::function<void()> changed);
}

//==============================================================================
/** Fader value readout under a strip; click to type a value. */
void makeEditableDbLabel (juce::Label& label, Param& gain);

/** Editable channel name + numbered badge; click selects the channel. */
class StripHeader : public juce::Component
{
public:
    StripHeader (UiContext& c, int selectIndex, juce::String badge, juce::Colour colour,
                 std::function<juce::String()> getName, std::function<void (juce::String)> setName);
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;
    void refresh() { name.setText (getName(), juce::dontSendNotification); }

private:
    UiContext& ctx;
    int selectIndex;
    juce::String badge;
    juce::Colour colour;
    std::function<juce::String()> getName;
    juce::Label name;
};

//==============================================================================
class InputStripView final : public juce::Component
{
public:
    InputStripView (UiContext& c, int index);
    ~InputStripView() override { ParamPoller::detach (&dbPoll); }
    void paint (juce::Graphics&) override;
    void resized() override;
    /** Any click inside the strip (also on its controls) selects it for the inspector. */
    void mouseDown (const juce::MouseEvent&) override { ctx.select (index); }
    void updateMeters (float dt);
    void refresh();
    /** -1: the fader controls the strip level. 0..7: it controls the send level to that bus (bus SEL). */
    void setFaderTarget (int bus);
    void paintOverChildren (juce::Graphics&) override;

private:
    UiContext& ctx;
    int index;
    InputStrip& strip;
    int faderBus = -1;
    Param* faderParam = nullptr;
    bool faderRouted = true;
    juce::Rectangle<float> hwGroup, virtGroup;

    StripHeader header;
    Chip source;
    juce::Label sourceStatus;
    Knob trim, pan;
    Toggle eq, gate, comp, denoise;
    std::vector<std::unique_ptr<Toggle>> routes;
    Toggle mono, solo, mute;
    std::unique_ptr<Fader> fader;
    MeterView meter;
    juce::Label dbLabel;
    struct DbPoll : ParamPoller::Client { InputStripView* v; uint32_t ver = 0; void pollParams() override; } dbPoll;
};

//==============================================================================
class BusStripView final : public juce::Component
{
public:
    BusStripView (UiContext& c, int index);
    ~BusStripView() override { ParamPoller::detach (&dbPoll); }
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override { ctx.select (kNumInputs + index); }
    void updateMeters (float dt);
    void refresh();

private:
    UiContext& ctx;
    int index;
    Bus& bus;

    StripHeader header;
    Chip output;
    juce::Label outputStatus;
    Toggle eq, lim, mono, sel, mute;
    bool monitorShown = false;
    int tick = 0;
    Fader fader;
    MeterView meter;
    juce::Label dbLabel;
    struct DbPoll : ParamPoller::Client { BusStripView* v; uint32_t ver = 0; void pollParams() override; } dbPoll;
};
} // namespace hydra

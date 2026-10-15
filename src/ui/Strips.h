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

/** Right-click menu of a strip: rename, remove the channel. */
void showChannelMenu (UiContext& ctx, int selectIndex, juce::Component& target, std::function<void()> rename);

/** Editable channel name + numbered badge; click selects the channel, right-click opens the channel menu. */
class StripHeader : public juce::Component
{
public:
    StripHeader (UiContext& c, int selectIndex, juce::String badge, juce::Colour colour,
                 std::function<juce::String()> getName, std::function<void (juce::String)> setName);
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void refresh() { name.setText (getName(), juce::dontSendNotification); }
    void showMenu();

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
    /** Any click inside the strip (also on its controls) selects it for the inspector;
        a right-click on the strip's background opens the channel menu. */
    void mouseDown (const juce::MouseEvent&) override;
    void updateMeters (float dt);
    void refresh();
    int getSelectIndex() const noexcept { return index; }
    /** Height the strip needs with the given number of A / B routing buttons (fader at its minimum). */
    static int minimumHeight (int numHwRoutes, int numVirtRoutes);
    /** -1: the fader controls the strip level. 0..7: it controls the send level to that bus (bus SEL). */
    void setFaderTarget (int bus);
    void paintOverChildren (juce::Graphics&) override;

private:
    UiContext& ctx;
    int index;
    InputStrip& strip;
    int faderBus = -1, statusTick = 0;
    Param* faderParam = nullptr;
    bool faderRouted = true;
    juce::Rectangle<float> hwGroup, virtGroup;

    StripHeader header;
    Chip source;
    juce::Label sourceStatus;
    Knob trim, pan;
    // Routing buttons of the buses in use (EQ / gate / comp / denoiser are switched in the inspector).
    std::vector<std::unique_ptr<Toggle>> hwRoutes, virtRoutes;
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
    void mouseDown (const juce::MouseEvent&) override;
    void updateMeters (float dt);
    void refresh();
    int getSelectIndex() const noexcept { return busSelectIndex (index); }

private:
    UiContext& ctx;
    int index;
    Bus& bus;

    StripHeader header;
    Chip output;
    juce::Label outputStatus;
    Toggle lim, mono, sel, mute;
    bool monitorShown = false;
    int tick = 0;
    Fader fader;
    MeterView meter;
    juce::Label dbLabel;
    struct DbPoll : ParamPoller::Client { BusStripView* v; uint32_t ver = 0; void pollParams() override; } dbPoll;
};

//==============================================================================
/** All strips of the channels in use, with the section captions (Inputs / Hardware Outputs /
    Virtual Outputs) and their "+" buttons. Lives in a horizontally scrolling viewport: strips
    stretch to fill the visible width, and below kMinStripWidth the area becomes wider than the view. */
class StripArea final : public juce::Component
{
public:
    explicit StripArea (UiContext& c);

    /** Recreates the strips for the current channel layout. */
    void rebuild();
    /** Sets this component's size for a viewport of the given visible size (scrolls sideways when the
        strips do not fit, and vertically when the window is too low for the faders). */
    void layoutFor (int viewWidth, int viewHeight, int scrollBarThickness);
    /** Area of a channel's strip (selection index), or empty if it is not shown. */
    juce::Rectangle<int> stripBounds (int selectIndex) const;

    void paint (juce::Graphics&) override;
    void resized() override;
    void updateMeters (float dt);
    void refreshStrips();
    void repaintStrips();
    void setFaderTarget (int bus);
    /** Bus selected with SEL / monitored bus, shown next to the Inputs caption. */
    void setSelInfo (int selBus, int monBus);

    static constexpr int kMinStripWidth = 92, kMaxStripWidth = 140, kCaptionHeight = 34;

    std::vector<std::unique_ptr<InputStripView>> inStrips;
    std::vector<std::unique_ptr<BusStripView>> busStrips;   // A buses first, then B buses

private:
    struct Section
    {
        juce::String title;
        juce::Colour colour;
        juce::Rectangle<int> area;    // caption + strips
        int captionWidth = 0;
    };
    int sectionWidth (const Section&, int strips, int stripWidth) const;
    int fitStripWidth (int viewWidth) const;
    int totalWidth (int stripWidth) const;

    UiContext& ctx;
    Section inSec, hwSec, virtSec;
    int numHw = 0, numVirt = 0;
    int selBus = -1, monBus = -1;
    PlusButton addIn { theme::accentIn }, addHw { theme::accentHw }, addVirt { theme::accentVirt };
    int stripWidth = kMinStripWidth;
};
} // namespace hydra

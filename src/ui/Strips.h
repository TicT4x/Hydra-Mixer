#pragma once
#include "Widgets.h"

namespace hydra
{
//==============================================================================
/** Fader value readout under a strip; click to type a value. */
void makeEditableDbLabel (juce::Label& label, Param& gain);

/** Right-click menu of a strip: rename, change order, remove / add a channel. */
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
    Chip source;   // status symbols when the source has a problem (offline, dropouts, ...)
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
    Chip output;               // status symbols like the input source chip
    juce::Label outputStatus;  // "monitoring Bx" 
    Toggle lim, mono, sel, mute;
    bool monitorShown = false;
    int tick = 0;
    Fader fader;
    MeterView meter;
    juce::Label dbLabel;
    struct DbPoll : ParamPoller::Client { BusStripView* v; uint32_t ver = 0; void pollParams() override; } dbPoll;
};

//==============================================================================
/** Horizontally scrolling row of strips (one per category: inputs, outputs). Never scrolls vertically:
    when the pane is too low for the strips, they are drawn smaller instead. The strips are children of
    getContent(), which is laid out in logical units and drawn scaled. */
class StripPane final : public juce::Component
{
public:
    StripPane();
    juce::Component& getContent() noexcept { return content; }
    /** While dragging near the left / right edge: scrolls that way (position in this component). */
    void autoScroll (juce::Point<int> position) { view.autoScroll (position.x, position.y, 40, 16); }
    /** Logical size of the strips and the factor they are drawn with; scrolls sideways when wider than the pane. */
    void setContentSize (int logicalWidth, int logicalHeight, float scale);
    /** Scroll position in logical units (0 = start). */
    float getScrollX() const;
    float getScale() const noexcept { return scale; }
    /** Scrolls so that the area (content coordinates) is visible. */
    void scrollToShow (juce::Rectangle<int> area);
    void resized() override;

    std::function<void()> onScroll;
    static constexpr int kScrollBar = 10;

private:
    struct View final : juce::Viewport
    {
        std::function<void()> moved;
        void visibleAreaChanged (const juce::Rectangle<int>&) override { if (moved) moved(); }
    };
    View view;
    juce::Component holder, content;
    float scale = 1.0f;
};

/** Vertical divider between the input and the output pane: drag sideways to give one side more room,
    double-click to return to the default position. */
class PaneDivider final : public juce::Component, public juce::SettableTooltipClient
{
public:
    PaneDivider();
    std::function<void (int x)> onDrag;   // wanted centre position in the parent
    std::function<void()> onReset;
    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { hover = false; repaint(); }
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override    { dragging = false; repaint(); }
    void mouseDoubleClick (const juce::MouseEvent&) override { if (onReset) onReset(); }

private:
    int startCentre = 0;
    bool hover = false, dragging = false;
};

/** All strips of the channels in use: the inputs on the left, the outputs (hardware, then virtual) on the
    right, each in its own pane that scrolls sideways on its own. Section captions (Inputs / Hardware
    Outputs / Virtual Outputs) with their "+" buttons stay in view above the panes. */
class StripArea final : public juce::Component
{
public:
    explicit StripArea (UiContext& c);
    ~StripArea() override;

    /** Recreates the strips for the current channel layout. */
    void rebuild();
    /** Width at which every strip fits side by side at the current height: no scroll bar and no gap
        between inputs and outputs (with the default divider position). */
    int idealWidth() const;
    /** Scrolls the pane of a channel (selection index) so that its strip is visible. */
    void scrollToChannel (int selectIndex);

    void paint (juce::Graphics&) override;
    void resized() override;
    void updateMeters (float dt);
    void refreshStrips();
    void repaintStrips();
    void setFaderTarget (int bus);
    /** Bus selected with SEL / monitored bus, shown next to the Inputs caption. */
    void setSelInfo (int selBus, int monBus);

    /** Change Order mode: the strips can be dragged to a new place within their group (inputs, hardware
        outputs, virtual outputs); a bar with Done / Discard appears above them. The engine is not touched
        until onReorderDone applies getShownOrder(); rebuild() leaves the mode (and shows the real order). */
    void beginReorder();
    void endReorder();
    bool isReordering() const noexcept { return reordering; }
    /** Shown order of each group as slot indices: [0] inputs, [1] A buses, [2] B buses. */
    std::array<std::vector<int>, 3> getShownOrder() const;
    std::function<void()> onReorderDone, onReorderDiscard;

    /** Strips always have this width (never stretched or squeezed); panes scroll when they do not fit.
        108 px: the default layout (8 inputs, A1-A5, B1-B3) fits side by side on a 1920 px screen at 100 %. */
    static constexpr int kStripWidth = 108, kCaptionHeight = 34, kDividerWidth = 16, kBannerHeight = 46;

    std::vector<std::unique_ptr<InputStripView>> inStrips;
    std::vector<std::unique_ptr<BusStripView>> busStrips;   // A buses first, then B buses

private:
    struct Section
    {
        juce::String title;
        juce::Colour colour;
        int captionWidth = 0;
        int x = 0, width = 0;          // group of strips in its pane's content (logical units)
        juce::Rectangle<int> caption;  // shown caption in this component (sticky while scrolling)
    };
    int groupWidth (const Section&, int strips, int stripWidth) const;
    int inputsWidth (int stripWidth) const;    // logical width of a pane's content
    int outputsWidth (int stripWidth) const;
    float scaleFor (int paneHeight) const;     // < 1 when the pane is too low for the strips
    int defaultSplit() const;                  // divider position (centre) for equally wide strips
    void layoutPane (StripPane&, bool outputs, int paneWidth, int paneHeight);
    void layoutCaptions();
    void setSplit (int centreX);
    /** Puts the strips of a pane at the places of their current order (except the one being dragged). */
    void placeStrips (bool outputs, juce::Component* except = nullptr);
    int captionTop() const noexcept { return reordering ? kBannerHeight : 0; }

    class DragLayer;
    class ReorderBar;
    std::unique_ptr<DragLayer> inLayer, outLayer;
    std::unique_ptr<ReorderBar> reorderBar;
    bool reordering = false;
    int stripWidth[2] {}, stripHeight[2] {};   // per pane (inputs, outputs), logical units

    UiContext& ctx;
    StripPane inPane, outPane;
    PaneDivider divider;
    Section inSec, hwSec, virtSec;
    int numHw = 0, numVirt = 0;
    int selBus = -1, monBus = -1;
    PlusButton addIn { theme::accentIn }, addHw { theme::accentHw }, addVirt { theme::accentVirt };
};
} // namespace hydra

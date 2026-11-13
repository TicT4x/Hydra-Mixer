#pragma once
#include "Widgets.h"
#include "EffectBrowser.h"

namespace hydra
{
//==============================================================================
/** Fader value readout under a strip; click to type a value. */
void makeEditableDbLabel (juce::Label& label, Param& gain);

/** Right-click menu of a strip: rename, change order, remove / add a channel. */
void showChannelMenu (UiContext& ctx, int selectIndex, juce::Component& target, std::function<void()> rename);
/** Right-click on the empty space of a group of strips (0 inputs, 1 hardware, 2 virtual outputs): paste as new, add. */
void showGroupMenu (UiContext& ctx, int group);

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
    // Label::setText closes an open editor: never while the user is typing a name (background refreshes).
    void refresh() { if (! name.isBeingEdited()) name.setText (getName(), juce::dontSendNotification); }
    void showMenu();
    /** Optional icon (ui/ChannelIcons.h id) drawn in the badge instead of its text. */
    std::function<juce::String()> getIcon;

private:
    UiContext& ctx;
    int selectIndex;
    juce::String badge;
    juce::Colour colour;
    std::function<juce::String()> getName;
    juce::Label name;
};

//==============================================================================
class InputStripView final : public juce::Component, public juce::DragAndDropTarget
{
public:
    InputStripView (UiContext& c, int index);
    ~InputStripView() override { ParamPoller::detach (&dbPoll); }
    void paint (juce::Graphics&) override;
    void resized() override;
    /** Any click inside the strip (also on its controls) selects it for the inspector;
        a right-click on the strip's background opens the channel menu. */
    void mouseDown (const juce::MouseEvent&) override;
    /** Hovering an A / B routing button lights up the strip of that bus (StripArea::setHoverBus). */
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void updateMeters (float dt);
    void refresh();
    int getSelectIndex() const noexcept { return index; }
    /** Height the strip needs with the given number of A / B routing buttons (fader at its minimum). */
    static int minimumHeight (int numHwRoutes, int numVirtRoutes);
    /** -1: the fader controls the strip level. 0..7: it controls the send level to that bus (bus SEL). */
    void setFaderTarget (int bus);
    void paintOverChildren (juce::Graphics&) override;
    // Effects dragged from the effect browser (FxDropZone): appended to the chain, held still = open the chain.
    bool isInterestedInDragSource (const SourceDetails& d) override { return drop.interested (d.description); }
    void itemDragEnter (const SourceDetails& d) override { drop.enter (d.description); }
    void itemDragExit (const SourceDetails&) override    { drop.exit(); }
    void itemDropped (const SourceDetails& d) override   { drop.dropped (d.description); }

    /** Horizontal channel rows: name, knobs and routing at the left, MONO / SOLO / MUTE at the right,
        the fader sideways in between (it takes all the room that is left, like in the vertical strips). */
    void setHorizontal (bool rows);
    /** Width a row needs besides its fader with the given number of A / B routing buttons. */
    static int rowControlsWidth (int numHwRoutes, int numVirtRoutes);

private:
    UiContext& ctx;
    int index;
    InputStrip& strip;
    int faderBus = -1, statusTick = 0;
    Param* faderParam = nullptr;
    bool faderRouted = true;
    bool rows = false;
    juce::Rectangle<float> hwGroup, virtGroup;
    int routeBusOf (const juce::Component*) const;   // bus of a routing button, -1: not one

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
    FxDropZone drop { ctx, *this, index };
};

//==============================================================================
class BusStripView final : public juce::Component, public juce::DragAndDropTarget
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
    int getBusIndex() const noexcept { return index; }
    void setHorizontal (bool rows);
    static int rowControlsWidth();
    /** Lit while the mouse is over this bus's routing button in an input strip. */
    void setHoverLit (bool);
    void paintOverChildren (juce::Graphics& g) override { drop.paintOver (g, getLocalBounds().toFloat()); }
    // Effects dragged from the effect browser (FxDropZone): appended to the chain, held still = open the chain.
    bool isInterestedInDragSource (const SourceDetails& d) override { return drop.interested (d.description); }
    void itemDragEnter (const SourceDetails& d) override { drop.enter (d.description); }
    void itemDragExit (const SourceDetails&) override    { drop.exit(); }
    void itemDropped (const SourceDetails& d) override   { drop.dropped (d.description); }


private:
    UiContext& ctx;
    int index;
    Bus& bus;

    StripHeader header;
    Chip output;               // status symbols like the input source chip
    juce::Label outputStatus;  // "monitoring Bx" 
    Toggle mono, sel, mute;   // no LIM: a limiter is a block of the effect chain like any other (9.11.)
    bool monitorShown = false, rows = false, hoverLit = false;
    int tick = 0;
    Fader fader;
    MeterView meter;
    juce::Label dbLabel;
    struct DbPoll : ParamPoller::Client { BusStripView* v; uint32_t ver = 0; void pollParams() override; } dbPoll;
    FxDropZone drop { ctx, *this, busSelectIndex (index) };
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
    /** Horizontal channel rows: scrolls up / down (and sideways only when the rows do not fit). */
    void setVertical (bool);
    /** Paints on the content below the strips (captions of the horizontal rows). */
    std::function<void (juce::Graphics&)> paintContent;

    std::function<void()> onScroll;
    /** Right-click on the empty space between / next to the strips (position in getContent()). */
    std::function<void (juce::Point<int>)> onBackgroundMenu;
    /** Left click on the empty space (position in getContent()): captions of the horizontal rows. */
    std::function<void (juce::Point<int>)> onBackgroundClick;
    static constexpr int kScrollBar = 10;

private:
    struct View final : juce::Viewport
    {
        std::function<void()> moved;
        void visibleAreaChanged (const juce::Rectangle<int>&) override { if (moved) moved(); }
        /** A mouse wheel notch glides to its place (~0.1 s) instead of jumping; touchpads (smooth wheels) scroll directly. */
        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;
        void glideFrame (double timestamp);
        void stopGlide() { gliding = false; vblank.reset(); }   // not from inside glideFrame
        bool gliding = false;
        juce::Point<float> target, shown;
        juce::Point<int> lastSet;
        double lastFrame = 0.0;
        std::unique_ptr<juce::VBlankAttachment> vblank;   // only while gliding
    };
    struct Content final : juce::Component
    {
        std::function<void (juce::Point<int>)> onMenu, onClick;
        std::function<void (juce::Graphics&)> onPaint;
        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu()) { if (onMenu) onMenu (e.getPosition()); }
            else if (onClick) onClick (e.getPosition());
        }
        void paint (juce::Graphics& g) override { if (onPaint) onPaint (g); }
    };
    View view;
    juce::Component holder;
    Content content;
    float scale = 1.0f;
    bool vertical = false;
};

/** Vertical divider between two panes of strips: drag sideways to give one side more room,
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

/** All strips of the channels in use: inputs, hardware outputs and virtual outputs side by side, each group
    in its own pane that scrolls sideways on its own. While they do not all fit, draggable dividers appear
    between the panes. With the dividers switched off (Settings > Display) all strips share one pane with one
    scroll bar. When the layout changes (window resized) the strips glide to their new places. Section captions (Inputs / Hardware Outputs / Virtual Outputs) with their "+" buttons stay in view above the panes. */
class StripArea final : public juce::Component
{
public:
    explicit StripArea (UiContext& c);
    ~StripArea() override;

    /** Recreates the strips for the current channel layout. */
    void rebuild();
    /** Width at which every strip fits side by side at the current height (or areaHeight, the height
        of the area): no scroll bar and no gap between the groups (with the default divider positions).
        Horizontal rows: the width at which the faders have a comfortable length. */
    int idealWidth() const;
    int idealWidthFor (int areaHeight) const;
    /** Size (logical units) the channels need without scrolling: vertical strips with a comfortable fader length,
        or all horizontal rows. Used when the window always takes its fitting size (Settings > Display). */
    juce::Point<int> fittingSize() const;
    /** Scrolls the pane of a channel (selection index) so that its strip is visible. */
    void scrollToChannel (int selectIndex);

    void paint (juce::Graphics&) override;
    void resized() override;
    void updateMeters (float dt);
    void refreshStrips();
    void repaintStrips();
    void setFaderTarget (int bus);
    /** Lights up the strip of a bus (-1: none) while the mouse is over its routing button in an input strip. */
    void setHoverBus (int bus);
    /** Bus selected with SEL / monitored bus, shown next to the Inputs caption. */
    void setSelInfo (int selBus, int monBus);

    /** A click on a group caption (Inputs / Hardware Outputs / Virtual Outputs) selects it: two symbols appear
        beside it, collapse (the group shrinks to a narrow summary: level, mute, solo of each channel) and pin
        (the group is always shown in full, the others scroll). Esc, a click elsewhere or on the caption again
        ends it. Both are stored (<APP stripCollapsed / stripPinned>, bits 0..2), default off. */
    bool hasCaptionSelection() const noexcept { return captionSel >= 0; }
    void clearCaptionSelection();
    void toggleCollapsed (int group);
    void togglePinned (int group);
    void selectCaption (int group);   // -1: none
    void mouseDown (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void focusLost (FocusChangeType) override { clearCaptionSelection(); }
    void paintOverChildren (juce::Graphics&) override;

    /** Change Order mode: the strips can be dragged to a new place within their group (inputs, hardware
        outputs, virtual outputs); a bar with Done / Discard appears above them. The engine is not touched
        until onReorderDone applies getShownOrder(); rebuild() leaves the mode (and shows the real order). */
    void beginReorder();
    void endReorder();
    bool isReordering() const noexcept { return reordering; }
    /** Shown order of each group as slot indices: [0] inputs, [1] H buses, [2] V buses. */
    std::array<std::vector<int>, 3> getShownOrder() const;
    /** Channels (selection indices) deleted with the trash can in Change Order mode; they are removed on Done. */
    const std::vector<int>& getRemovedChannels() const noexcept { return removedChannels; }
    void clearRemovedChannels() { removedChannels.clear(); }
    std::function<void()> onReorderDone, onReorderDiscard;

    /** Strips always have this width (never stretched or squeezed); panes scroll when they do not fit.
        108 px: the default layout (8 inputs, A1-A5, B1-B3) fits side by side on a 1920 px screen at 100 %. */
    static constexpr int kStripWidth = 108, kCaptionHeight = 34, kDividerWidth = 16, kBannerHeight = 46;
    /** Adaptive strips (Settings > Display): with room to spare the groups stay compact and centred, the strips
        are drawn larger as a whole (up to kMaxGrow, as long as the fader keeps kComfortFader px of extra height),
        the rest of the room goes between the groups (up to kMaxGroupGap each) and to the sides. */
    static constexpr float kMaxGrow = 1.35f;
    static constexpr int kComfortFader = 120, kMaxGroupGap = 90;
    /** Horizontal rows (Settings > Display): height of a row, of a group caption, comfortable / minimum fader length
        (of the input rows, which have the most controls), room for the value right of the fader. */
    static constexpr int kRowHeight = 62, kRowCaption = 30, kRowGroupGap = 10, kRowFader = 340, kMinRowFader = 160, kRowValue = 54;
    /** Collapsed group: width of a channel's column (vertical strips); horizontal rows: a channel's cell and a line of cells. */
    static constexpr int kMiniWidth = 26, kMiniCell = 104, kMiniLine = 34, kToolSize = 22;

    std::vector<std::unique_ptr<InputStripView>> inStrips;
    std::vector<std::unique_ptr<BusStripView>> busStrips;   // H buses first, then V buses

private:
    struct Section
    {
        juce::String title, shortTitle;   // shortTitle: collapsed (vertical strips)
        juce::Colour colour;
        int captionWidth = 0, shortWidth = 0;
        int x = 0, y = 0, width = 0;   // group of strips in its pane's content (logical units)
        juce::Rectangle<int> caption;  // shown caption in this component (sticky while scrolling); rows: in the content
    };
    static constexpr int kGroups = 3;          // panes, in this order: inputs, hardware outputs, virtual outputs
    int groupWidth (int group, int strips, int stripWidth) const;
    int stripCount (int group) const;
    juce::Component* stripAt (int group, int k) const;
    int paneWidthFor (int group, int stripWidth) const;   // logical width of a pane's content
    float scaleFor (int paneHeight) const;     // < 1 when the pane is too low for the strips
    std::array<int, 2> defaultSplits() const;  // divider positions (centres) for equally wide strips
    bool adaptive() const;                     // Settings > Display > Adaptive strips
    bool dividersShown() const;                // the channels do not fit and Settings > Display allows the dividers
    /** Adaptive layout while everything fits: scale of the strips and where each group starts
        (in this component, pixels). False: the normal layout applies. */
    struct Fit { float scale = 1.0f, margin = 0.0f, gap = 0.0f; int splits[2] {}; };
    bool computeFit (int paneHeight, Fit&);
    bool joined() const;                       // dividers switched off: all strips in panes[0]
    bool rowsMode() const;                     // Settings > Display: horizontal channels (all in panes[0], one row each)
    StripPane& paneOf (int group) noexcept { return panes[paneIndex (group)]; }
    const StripPane& paneOf (int group) const noexcept { return panes[paneIndex (group)]; }
    /** Pane a group is in: its own; joined (one scroll bar): shared by neighbours that are not pinned;
        rows: one pane for the pinned groups (on top, not scrolling), one for the others. */
    int paneIndex (int group) const noexcept;
    bool collapsed (int group) const noexcept;   // folded to the summary (never while reordering)
    bool pinned (int group) const noexcept { return ((shownPinned >> group) & 1) != 0; }
    int pinMask() const noexcept;
    int captionTitleWidth (int group) const;     // short title while collapsed (vertical strips)
    int toolsWidth (int group) const;            // collapse / pin symbols beside the caption
    void applyCollapsed();                       // strips hidden / summary shown per group
    void layoutUnits (int paneY, int paneH, bool animated, const Fit* fit);
    int rowsGroupHeight (int group, int width) const;
    int rowsPaneHeight (int pane, int width) const;
    void placeTools (int group, juce::Rectangle<int> captionArea, int titleWidth, bool animated);
    void placeRowCaption (int group, bool animated);   // horizontal rows: title, symbols and "+" of a group
    void placeCaptions();                              // captions only, at the current selAnim
    void startCaptionGlide();                          // the selection box opens / closes (selAnim)
    void captionFrame (double timestamp);
    void attachStrips();                       // puts strips and drag layers into the pane of their group
    void attachPlusButtons();                  // rows: into the content (they scroll with the captions), else here
    void layoutRows (bool animated);
    void paintRowCaptions (juce::Graphics&, int pane);
    int groupAt (int pane, juce::Point<int> contentPos) const;   // group whose part of a pane's content that is
    int rowsContentHeight() const;
    int rowControlsWidth() const;              // widest row without its fader
    /** Change Order: the trash can of a strip hides it; it is removed when the new order is applied. */
    bool canRemoveInReorder (int group) const;
    void removeInReorder (int group, int k);
    void layout (bool animated);
    void layoutPane (int group, int paneHeight, bool animated, const Fit* fit = nullptr);
    void layoutJoined (int paneHeight, bool animated, const Fit* fit = nullptr);
    void layoutCaptions (bool animated = false);
    void setSplit (int dividerIndex, int centreX);
    /** Puts the strips of a pane at the places of their current order (except the one being dragged). */
    void placeStrips (int group, juce::Component* except = nullptr, bool animated = false);
    int captionTop() const noexcept { return reordering ? kBannerHeight : 0; }

    class DragLayer;
    class ReorderBar;
    class Summary;
    class CaptionTool;
    std::unique_ptr<Summary> summaries[kGroups];
    std::unique_ptr<CaptionTool> collapseTools[kGroups], pinTools[kGroups];
    int captionSel = -1, shownPinned = 0;
    float selAnim[kGroups] {};   // 0..1: how far a group's caption selection is open (glides, captionFrame)
    bool captionGliding = false;
    double captionLastFrame = 0.0;
    std::unique_ptr<juce::VBlankAttachment> captionVBlank;   // only while the selection glides
    bool captionClick = false;   // this mouse-down selected a caption (the click-elsewhere listener ignores it)
    struct ClickElsewhere final : juce::MouseListener
    {
        StripArea& owner;
        explicit ClickElsewhere (StripArea& o) : owner (o) {}
        void mouseDown (const juce::MouseEvent&) override;
    } clickElsewhere { *this };
    std::unique_ptr<DragLayer> layers[kGroups];
    std::unique_ptr<ReorderBar> reorderBar;
    bool reordering = false;
    bool shownJoined = false, shownRows = false, layingOut = false;
    bool resizeGlide = false;   // the strips glide after a size change: all alike, so their corners stay as they are
    int stripWidth[kGroups] {}, stripHeight[kGroups] {};   // per pane, logical units; width = slot incl. spacing
    int routesHw = 0, routesVirt = 0;   // routing buttons in the input strips (counted at rebuild)
    std::vector<std::unique_ptr<juce::Component>> removedStrips;   // Change Order: deleted, hidden until rebuild
    std::vector<int> removedChannels;

    UiContext& ctx;
    StripPane panes[kGroups];
    PaneDivider dividers[kGroups - 1];
    Section sections[kGroups];
    int numHw = 0, numVirt = 0;
    int selBus = -1, monBus = -1, hoverBus = -1;
    PlusButton addIn { theme::accentIn }, addHw { theme::accentHw }, addVirt { theme::accentVirt };
    PlusButton* const addButtons[kGroups] { &addIn, &addHw, &addVirt };
    LayoutAnimator animator { *this, 0.06 };   // strips: window resized, making room in Change Order, channel added or removed
};
} // namespace hydra

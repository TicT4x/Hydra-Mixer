#include "Inspector.h"
#include "state/ChannelClipboard.h"
#include "state/Presets.h"
#include "state/CorrectionPool.h"
#include "EffectBrowser.h"
#include "state/Session.h"
#include <juce_dsp/juce_dsp.h>
#include <deque>

namespace hydra
{
//==============================================================================
namespace
{
/** Selection index of a channel's effect side (-1 if none). */
int channelOf (const Engine& e, const ChannelFx& c)
{
    for (int ch = 0; ch < kMaxInputs + kMaxBuses; ++ch)
        if (&e.fxOf (ch) == &c) return ch;
    return -1;
}

juce::String nameOf (const Engine& e, const ChannelFx& c)
{
    const int ch = channelOf (e, c);
    if (ch < 0) return {};
    return ch < kMaxInputs ? e.inputs[(size_t) ch].name : e.buses[(size_t) (ch - kMaxInputs)].name;
}

class PluginWindow final : public juce::DocumentWindow
{
public:
    explicit PluginWindow (PluginSlot& s)
        : juce::DocumentWindow (s.instance->getName(), theme::panel, juce::DocumentWindow::closeButton), slot (s)
    {
        setUsingNativeTitleBar (true);
        juce::AudioProcessorEditor* ed = s.instance->hasEditor() ? s.instance->createEditorAndMakeActive() : nullptr;
        if (ed == nullptr)
            ed = new juce::GenericAudioProcessorEditor (*s.instance);
        setContentOwned (ed, true);
        setResizable (ed->isResizable(), false);
        centreWithSize (getWidth(), getHeight());
        placeOnAppDisplay (*this, true);
        setVisible (true);
        toFront (true);
    }

    void closeButtonPressed() override
    {
        // Destroy the editor (not just hide it) so plugin GUIs stop consuming CPU.
        juce::Component::SafePointer<PluginWindow> self (this);
        auto* s = &slot;
        juce::MessageManager::callAsync ([self, s] { if (self != nullptr) s->editorWindow.reset(); });
    }

private:
    PluginSlot& slot;
};

/** Six dots: "drag here". */
void drawGrip (juce::Graphics& g, juce::Rectangle<float> r)
{
    g.setColour (theme::textFaint);
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 2; ++col)
            g.fillEllipse (r.getX() + (float) col * 5.0f, r.getY() + (float) row * 5.0f, 2.2f, 2.2f);
}

/** Transparent handle over a section's title: dragging it moves the effect within the chain. */
struct DragGrip final : juce::Component, juce::SettableTooltipClient
{
    DragGrip()
    {
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        setTooltip ("Drag to move this effect in the chain");
    }
    std::function<void (const juce::MouseEvent&)> onDown, onDrag;
    std::function<void()> onDrop, onMenu;
    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = false;
        if (e.mods.isPopupMenu()) { if (onMenu) onMenu(); return; }
        if (onDown) onDown (e);
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) return;
        if (! dragging && e.getDistanceFromDragStart() < 6) return;
        if (! dragging)
            if (auto* unit = getParentComponent() != nullptr ? getParentComponent()->getParentComponent() : nullptr) unit->toFront (false);
        dragging = true;
        if (onDrag) onDrag (e);
    }
    void mouseUp (const juce::MouseEvent&) override { if (std::exchange (dragging, false) && onDrop) onDrop(); }
    bool dragging = false;
};

/** Opens a larger window: the expand symbol (EQ, Correction; the settings window of the other effects). */
class ExpandButton final : public juce::Component, public juce::SettableTooltipClient
{
public:
    std::function<void()> onClick;

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (hover ? theme::raised.brighter (0.1f) : theme::raised);
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour (hover ? theme::text : theme::textDim);
        const auto c = r.getCentre();
        const float s = 4.5f;
        juce::Path p;
        p.startNewSubPath (c.x - s, c.y + s); p.lineTo (c.x + s, c.y - s);       // diagonal
        p.startNewSubPath (c.x + s - 3.5f, c.y - s); p.lineTo (c.x + s, c.y - s); p.lineTo (c.x + s, c.y - s + 3.5f);
        p.startNewSubPath (c.x - s, c.y + s - 3.5f); p.lineTo (c.x - s, c.y + s); p.lineTo (c.x - s + 3.5f, c.y + s);
        g.strokePath (p, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    void mouseUp (const juce::MouseEvent& e) override { if (e.mouseWasClicked() && onClick) onClick(); }
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { hover = false; repaint(); }

private:
    bool hover = false;
};

/** Selected block in an effect chain: a soft glow in the block's colour around the card (drawn by the row
    behind the blocks), the card a touch lighter, its edge in the colour and the header washed with it. */
void drawSelectedCard (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c, float corner)
{
    g.setColour (c.withAlpha (0.07f));
    g.fillRoundedRectangle (r, corner);
    juce::Path shape;
    shape.addRoundedRectangle (r, corner);
    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (shape);
        g.setGradientFill (juce::ColourGradient (c.withAlpha (0.20f), 0.0f, r.getY(), c.withAlpha (0.0f), 0.0f, r.getY() + 34.0f, false));
        g.fillRect (r.withHeight (34.0f));
        g.setColour (c);
        g.fillRect (r.withHeight (2.0f));
    }
    g.setColour (c.withAlpha (0.75f));
    g.drawRoundedRectangle (r.reduced (0.5f), corner, 1.2f);
}

/** Rounded section with title, optional ON toggle (headerSlot) and optional settings gear. In an effect
    chain it also has a drag grip on its title and a remove button (enableChainControls). */
struct Section : juce::Component
{
    Section (juce::String t, juce::Colour c) : title (std::move (t)), colour (c)
    {
        gear.setTooltip ("All settings in a window");
    }

    void enableGear (std::function<void()> onClick)
    {
        gear.onClick = std::move (onClick);
        addAndMakeVisible (gear);
    }

    void enableChainControls (std::function<void()> onRemove)
    {
        chained = true;
        remove.onClick = std::move (onRemove);
        remove.setTooltip ("Remove " + title + " from the chain (its settings go back to the defaults; Ctrl+Z brings them back)");
        addAndMakeVisible (remove);
        addAndMakeVisible (grip);
        grip.onMenu = [this] { if (onMenu) onMenu(); };
        resized();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (3.0f);
        if (theme::deep())
            theme::drawCard (g, r, theme::panel2, theme::cardLine, colour);
        else
        {
            g.setColour (theme::panel2);
            g.fillRoundedRectangle (r, 8.0f);
            g.setColour (theme::cardLine);
            g.drawRoundedRectangle (r, 8.0f, 1.0f);
        }
        if (selected) drawSelectedCard (g, r, colour, theme::deep() ? 4.0f : 8.0f);
        if (! subCard.isEmpty() && ! theme::deep())
        {
            g.setColour (theme::bg.withAlpha (0.45f));
            g.fillRoundedRectangle (subCard.toFloat(), 6.0f);
            g.setColour (subCardColour);
            g.drawRoundedRectangle (subCard.toFloat().reduced (0.5f), 6.0f, 1.0f);
        }
        auto titleArea = r.reduced (10.0f, 6.0f).removeFromTop (16.0f);
        if (chained)
        {
            drawGrip (g, titleArea.removeFromLeft (12.0f).withSizeKeepingCentre (8.0f, 13.0f).translated (0.0f, 1.0f));
            titleArea.removeFromLeft (4.0f);
        }
        titleArea.setRight ((float) titleRight());
        g.setColour (colour);
        g.setFont (titleFont());
        if (beta)
        {
            const float tw = juce::GlyphArrangement::getStringWidth (titleFont(), title.toUpperCase());
            const float textW = juce::jmin (tw, titleArea.getWidth() - kBetaWidth - 6.0f);
            g.drawFittedText (title.toUpperCase(), titleArea.withWidth (textW).toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
            drawBeta (g, juce::Rectangle<float> (titleArea.getX() + textW + 6.0f, titleArea.getCentreY() - 7.0f, kBetaWidth, 14.0f), colour);
            return;
        }
        g.drawFittedText (title.toUpperCase(), titleArea.toNearestInt(), juce::Justification::centredLeft, 1, 1.0f);
    }

    static constexpr float kBetaWidth = 34.0f;
    static void drawBeta (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c)
    {
        g.setColour (c.withAlpha (0.16f));
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (c.withAlpha (0.7f));
        g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
        g.setColour (c);
        g.setFont (theme::font (9.0f, true));
        g.drawText ("BETA", r, juce::Justification::centred);
    }

    /** Narrowest width at which the title fits next to the header controls. */
    int minimumWidth() const
    {
        const int text = juce::GlyphArrangement::getStringWidthInt (titleFont(), title.toUpperCase());
        return 13 + (chained ? 16 : 0) + text + (beta ? (int) kBetaWidth + 6 : 0) + 4 + (gear.isVisible() ? 28 : 0) + headerExtra + 44 + (chained ? 26 : 0) + 10 + 4;
    }

    /** Right-click on the section (not on a control): the block menu. */
    void mouseDown (const juce::MouseEvent& e) override { if (e.mods.isPopupMenu() && onMenu) onMenu(); }

    void resized() override
    {
        gear.setBounds (headerSlot().withRight (beforeSwitch()).withLeft (beforeSwitch() - 26));
        if (chained)
        {
            remove.setBounds (getLocalBounds().reduced (8, 7).removeFromTop (20).removeFromRight (22));
            grip.setBounds (juce::Rectangle<int> (8, 4, juce::jmax (20, titleRight() - 8), 24));
        }
    }

    juce::Rectangle<int> content() const    { return getLocalBounds().reduced (12, 8).withTrimmedTop (24); }
    juce::Rectangle<int> headerSlot() const
    {
        return getLocalBounds().reduced (10, 7).removeFromTop (20).withTrimmedRight (chained ? 26 : 0).removeFromRight (44);
    }
    /** Right edge for the next control left of the ON switch: the same distance as between the switch and the
        remove button on its right. (Default theme: the switch only takes the right end of its slot.) */
    int beforeSwitch() const
    {
        const auto slot = headerSlot();
        return (theme::deep() ? slot.getRight() - 33 : slot.getX()) - 6;
    }
    /** Right end of the title text: left of the header controls. Sections place extra header buttons in
        front of the ON toggle and say how wide they are with headerExtra. */
    int titleRight() const { return beforeSwitch() - (gear.isVisible() ? 28 : 0) - headerExtra - 2; }

    static juce::Font titleFont() { return theme::font (12.0f, true); }

    juce::String title;
    juce::Colour colour;
    bool beta = false;   // a small "BETA" tag after the title
    juce::Rectangle<int> subCard;   // optional framed area behind some controls
    juce::Colour subCardColour;
    ExpandButton gear;
    TrashButton remove;
    DragGrip grip;
    std::function<void()> onMenu;   // block menu (copy, cut, paste ...)
    bool chained = false;
    bool selected = false;   // selected in the chain (FxRack): drawSelectedCard
    int headerExtra = 0;
};

void rowLayout (juce::Rectangle<int> r, const std::vector<juce::Component*>& comps, int gap = 4)
{
    const int n = (int) comps.size();
    if (n == 0) return;
    const int w = (r.getWidth() - gap * (n - 1)) / n;
    for (auto* c : comps) { c->setBounds (r.removeFromLeft (w)); r.removeFromLeft (gap); }
}

/** Small live bar (vertical or horizontal) fed by a lambda; repaints only on change. */
class LiveBar final : public juce::Component
{
public:
    LiveBar (std::function<float()> src, juce::Colour c, juce::String cap = {}, float markerValue = -1.0f)
        : source (std::move (src)), colour (c), caption (std::move (cap)), marker (markerValue)
    {
        setInterceptsMouseClicks (false, false);
    }

    /** How fast the bar follows its source (seconds, rising / falling). Noisy sources get longer times. */
    void setBallistics (float riseSeconds, float fallSeconds) { rise = riseSeconds; fall = fallSeconds; }

    void setMarker (float m) { if (m != marker) { marker = m; repaint(); } }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        const bool vertical = r.getHeight() > r.getWidth();
        auto bar = r;
        if (caption.isNotEmpty())
        {
            g.setColour (theme::textFaint);
            g.setFont (theme::font (9.5f));
            if (vertical) g.drawText (caption, bar.removeFromBottom (12.0f), juce::Justification::centred);
            else          g.drawText (caption, bar.removeFromLeft (44.0f), juce::Justification::centredLeft);
        }
        if (vertical) bar = bar.withSizeKeepingCentre (juce::jmin (10.0f, bar.getWidth()), bar.getHeight());
        g.setColour (theme::bg);
        g.fillRoundedRectangle (bar, 3.0f);
        g.setColour (colour);
        const float v = juce::jmax (0.0f, shown);
        if (vertical) g.fillRoundedRectangle (bar.withTop (bar.getBottom() - bar.getHeight() * v), 3.0f);
        else          g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * v), 3.0f);
        if (marker >= 0.0f)
        {
            g.setColour (theme::text);
            if (vertical) g.fillRect (bar.getX() - 2.0f, bar.getBottom() - bar.getHeight() * marker - 0.75f, bar.getWidth() + 4.0f, 1.5f);
            else          g.fillRect (bar.getX() + bar.getWidth() * marker - 0.75f, bar.getY() - 2.0f, 1.5f, bar.getHeight() + 4.0f);
        }
    }

private:
    void frame (double t)
    {
        const float dt = lastFrame > 0.0 ? (float) juce::jlimit (0.0, 0.1, t - lastFrame) : 1.0f / 60.0f;
        lastFrame = t;
        const float target = juce::jlimit (0.0f, 1.0f, source());
        smooth += (target - smooth) * (1.0f - std::exp (-dt / (target > smooth ? rise : fall)));
        // Repaint for a visible step only (about a pixel at the bar's length).
        const float step = 0.6f / (float) juce::jmax (20, juce::jmax (getWidth(), getHeight()));
        if (std::abs (smooth - shown) >= step || (target == 0.0f && shown != 0.0f && smooth < step))
        {
            shown = smooth < step ? 0.0f : smooth;
            repaint();
        }
    }

    std::function<float()> source;
    juce::Colour colour;
    juce::String caption;
    float marker = -1.0f, shown = 0.0f, smooth = 0.0f, rise = 0.03f, fall = 0.18f;
    double lastFrame = 0.0;
    juce::VBlankAttachment vblank { this, [this] (double t) { frame (t); } };
};

// RNNoise's speech probability jumps from frame to frame: a bar showing it follows slowly.
constexpr float kVoiceRise = 0.12f, kVoiceFall = 0.4f;

/** Base for floating advanced panels: controls grouped on captioned cards (knobs with label and value,
    each group's switch in the caption). Polls its live displays at 30 Hz while open. */
class AdvancedPanel : public juce::Component, private juce::Timer
{
public:
    AdvancedPanel() { startTimerHz (30); }

    void paint (juce::Graphics& g) override
    {
        for (auto& c : cards)
        {
            const auto r = c.area.toFloat();
            const float corner = theme::deep() ? 4.0f : 8.0f;
            g.setColour (theme::panel2);
            g.fillRoundedRectangle (r, corner);
            g.setColour (theme::cardLine);
            g.drawRoundedRectangle (r.reduced (0.5f), corner, 1.0f);
            g.setColour (theme::textDim);
            g.setFont (theme::font (11.5f, true));
            g.drawText (c.title, c.area.withHeight (kCaption + 2).withTrimmedLeft (12).withTrimmedRight (8), juce::Justification::centredLeft);
        }
    }

protected:
    static constexpr int kCaption = 26, kKnobs = 96, kPad = 14, kGap = 8;
    static constexpr int kCardHeight = kCaption + kKnobs + 8;

    /** Adds a card (forget the old ones in resized first) and returns the area for its controls. */
    juce::Rectangle<int> card (juce::Rectangle<int> area, const juce::String& title)
    {
        cards.push_back ({ area, title });
        return area.reduced (10, 0).withTrimmedTop (kCaption).withTrimmedBottom (8);
    }

    /** A control in the caption of a card; offset counts from the right edge. */
    static juce::Rectangle<int> captionSlot (juce::Rectangle<int> cardArea, int width, int offset = 0)
    {
        return { cardArea.getRight() - 8 - offset - width, cardArea.getY() + 4, width, 20 };
    }

    static void showValues (std::initializer_list<Knob*> knobs) { for (auto* k : knobs) k->alwaysShowValue = true; }

    struct Card { juce::Rectangle<int> area; juce::String title; };
    std::vector<Card> cards;
    virtual void tick() {}

private:
    void timerCallback() override { tick(); }
};

//==============================================================================
/** Horizontal device rack (like Ableton's device view): all sections of a channel side by side at their
    own width in one row that scrolls sideways as a whole when it does not fit. The last section takes
    any room that is left. */
class EffectRack final : public juce::Component
{
public:
    struct Row final : juce::Component
    {
        std::function<void (juce::Graphics&)> over;   // drawn above the sections (row coordinates)
        void paintOverChildren (juce::Graphics& g) override { if (over) over (g); }
    };

    EffectRack()
    {
        view.setViewedComponent (&row, false);
        view.setScrollBarsShown (false, true);
        view.setScrollBarThickness (8);
        view.setSingleStepSizes (40, 40);
        addAndMakeVisible (view);
    }

    Row& content() noexcept { return row; }

    /** Lays the sections out left to right; widths[i] is the width of sections[i], the last one gets at
        least lastMin and otherwise everything that is left. */
    void layout (const std::vector<juce::Component*>& sections, std::vector<int> widths, int lastMin)
    {
        view.setBounds (getLocalBounds());
        int fixed = 0;
        for (size_t i = 0; i + 1 < widths.size(); ++i) fixed += widths[i];
        const bool scrolls = fixed + lastMin > getWidth();
        widths.back() = juce::jmax (lastMin, getWidth() - fixed);
        const int h = getHeight() - (scrolls ? view.getScrollBarThickness() + 2 : 0);
        row.setSize (fixed + widths.back(), juce::jmax (40, h));
        int x = 0;
        for (size_t i = 0; i < sections.size(); ++i)
        {
            sections[i]->setBounds (x, 0, widths[i], row.getHeight());
            x += widths[i];
        }
    }

private:
    juce::Viewport view;
    Row row;
};

//==============================================================================
/** Small icon button: two diagonal arrows ("open larger"). */
//==============================================================================
/** Import / export / clear of one EQ (instance k) of a channel. Copy / paste: right-click the block. */
void showEqMenu (UiContext& ctx, int channel, int k, juce::Component& target)
{
    auto* e = &ctx.engine;
    juce::Component::SafePointer<juce::Component> near (&target);
    juce::PopupMenu m;
    m.addSectionHeader ("EQ" + (k > 0 ? " " + juce::String (k + 1) : juce::String()) + " of " + channelCode (channel));
    static std::unique_ptr<juce::FileChooser> chooser;
    const auto dir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
    m.addItem ("Export EQ...", [e, channel, k, dir]
    {
        const auto name = channel < kMaxInputs ? e->inputs[(size_t) channel].name : e->buses[(size_t) (channel - kMaxInputs)].name;
        chooser = std::make_unique<juce::FileChooser> ("Export EQ", dir.getChildFile (juce::File::createLegalFileName (name + " EQ") + clip::kEqExtension),
                                                       juce::String ("*") + clip::kEqExtension);
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [e, channel, k] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult();
            if (f == juce::File()) return;
            if (auto xml = clip::copyBuiltin (*e, channel, Fx::eq, k)) xml->writeTo (f.withFileExtension (clip::kEqExtension));
        });
    });
    m.addItem ("Import EQ...", [e, channel, k, dir, near]
    {
        chooser = std::make_unique<juce::FileChooser> ("Import EQ", dir, juce::String ("*") + clip::kEqExtension);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [e, channel, k, near] (const juce::FileChooser& fc)
        {
            const auto f = fc.getResult();
            if (f == juce::File()) return;
            auto xml = clip::parse (f.loadFileAsString());
            if (xml == nullptr || ! clip::pasteBuiltinSettings (*e, channel, Fx::eq, k, *xml))
                dialog::message (near.getComponent(), dialog::Kind::warning, "EQ could not be imported",
                                 f.getFileName() + " is not an EQ exported from Hydra Mixer.");
        });
    });
    m.addSeparator();
    m.addItem ("Remove all bands", [&ctx, channel, k]
    {
        auto* eq = channel < kMaxInputs ? &ctx.engine.inputs[(size_t) channel].eqs[(size_t) k] : &ctx.engine.buses[(size_t) (channel - kMaxInputs)].eqs[(size_t) k];
        for (int b = 0; b < ParamEq::kBands; ++b)
            for (auto* p : { eq->bandOn[(size_t) b], eq->enabled[(size_t) b], eq->type[(size_t) b], eq->gain[(size_t) b], eq->freq[(size_t) b], eq->q[(size_t) b] })
                p->reset();
    });
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target));
}

//==============================================================================
/** EQ block: type picker | curve | selected band controls. Shared by inputs and buses. */
class EqSection final : public juce::Component, private ParamPoller::Client
{
public:
    EqSection (UiContext& c, ParamEq& e, juce::Colour col, int selectIndex, int instance, const juce::String& title)
        : ctx (c), eq (e), colour (col), section (title, theme::fxEq), onToggle (c, *e.on, "ON", theme::fxEq), graph (c, e, col)
    {
        addAndMakeVisible (section);
        section.addAndMakeVisible (onToggle);
        section.addAndMakeVisible (graph);
        section.addAndMakeVisible (expand);
        section.addAndMakeVisible (menu);
        section.headerExtra = 62;   // expand + menu in front of the ON toggle
        onToggle.setTooltip ("Switch the whole EQ on or off");
        expand.setTooltip ("Open the EQ in a large window");
        expand.onClick = [this, selectIndex, instance] { if (ctx.openEqWindow) ctx.openEqWindow (selectIndex, instance); };
        menu.showArrow = false;
        menu.centred = true;
        menu.setTooltip ("Import or export this EQ, remove all bands");
        menu.onClick = [this, selectIndex, instance] { showEqMenu (ctx, selectIndex, instance, menu); };
        graph.onBandSelected = [this] (int b) { bindBand (b); };
        bindBand (0);
        for (int k = 0; k < ParamEq::kBands; ++k) // show the first band in use
            if (eq.bandOn[(size_t) k]->isOn()) { graph.setSelectedBand (k); break; }
        ParamPoller::attach (this);
    }

    ~EqSection() override { ParamPoller::detach (this); }

    Section& getSection() noexcept { return section; }

    void resized() override
    {
        section.setBounds (getLocalBounds());
        onToggle.setBounds (section.headerSlot());
        const auto header = section.headerSlot();
        expand.setBounds (header.withRight (section.beforeSwitch()).withLeft (section.beforeSwitch() - 26));
        menu.setBounds (header.withRight (expand.getX() - 4).withLeft (expand.getX() - 30));
        auto r = section.content();
        typeBar->setBounds (r.removeFromLeft (34));
        r.removeFromLeft (6);
        const auto card = r.removeFromRight (172);
        r.removeFromRight (8);
        graph.setBounds (r);

        // The selected band has its own framed card, so its switch is not mistaken for the EQ's.
        // Knobs and switch sit together in the middle of it.
        section.subCard = card;
        auto col = card.reduced (8, 6);
        constexpr int knobsH = 70, gap = 8, footH = 22;
        auto group = col.withSizeKeepingCentre (col.getWidth(), juce::jmin (col.getHeight(), knobsH + gap + footH));
        auto foot = group.removeFromBottom (footH).withSizeKeepingCentre (juce::jmin (col.getWidth(), 138), footH);
        group.removeFromBottom (gap);
        bandRemove.setBounds (foot.removeFromRight (26));
        foot.removeFromRight (4);
        bandToggle->setBounds (foot);
        rowLayout (group, { gainKnob.get(), freqKnob.get(), qKnob.get() }, 2);
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        if (eq.bandOn[(size_t) graph.getSelectedBand()]->isOn()) return;
        g.setColour (theme::textDim);
        g.setFont (theme::font (11.5f));
        g.drawFittedText ("Click the curve\nto add a band", section.subCard, juce::Justification::centred, 2);
    }

private:
    void bindBand (int b)
    {
        const auto k = (size_t) b;
        const auto bc = theme::bandColour (b);
        section.subCardColour = bc.withAlpha (0.35f);
        typeBar = std::make_unique<EqTypeBar> (ctx, *eq.type[k], bc);
        bandToggle = std::make_unique<Toggle> (ctx, *eq.enabled[k], "BAND " + juce::String (b + 1) + " ON", bc);
        bandToggle->setTooltip ("Switch this band on or off (off keeps its settings). Click an empty spot in the curve to add a band.");
        bandRemove.setTooltip ("Remove this band");
        bandRemove.onClick = [this, b]
        {
            juce::Component::SafePointer<EqSection> safe (this);
            juce::MessageManager::callAsync ([safe, b] { if (safe != nullptr) safe->graph.removeBand (b); });
        };
        section.addAndMakeVisible (bandRemove);
        gainKnob = std::make_unique<Knob> (ctx, *eq.gain[k], "GAIN", bc);
        freqKnob = std::make_unique<Knob> (ctx, *eq.freq[k], "FREQ", bc);
        qKnob = std::make_unique<Knob> (ctx, *eq.q[k], "Q", bc);
        for (juce::Component* c : std::initializer_list<juce::Component*> { typeBar.get(), (juce::Component*) bandToggle.get(),
                                    (juce::Component*) gainKnob.get(), (juce::Component*) freqKnob.get(), (juce::Component*) qKnob.get() })
            section.addAndMakeVisible (c);
        typeVersion = 0;
        shownUsed = -1;
        pollParams();
        resized();
        section.repaint();
        repaint();
    }

    void pollParams() override
    {
        const auto* t = eq.type[(size_t) graph.getSelectedBand()];
        if (t->getVersion() != typeVersion)
        {
            typeVersion = t->getVersion();
            gainKnob->setAlpha (ParamEq::typeUsesGain (juce::roundToInt (t->get())) ? 1.0f : 0.35f);
        }
        const bool used = eq.bandOn[(size_t) graph.getSelectedBand()]->isOn();
        if ((used ? 1 : 0) == shownUsed) return;
        shownUsed = used ? 1 : 0;
        for (juce::Component* c : std::initializer_list<juce::Component*> { typeBar.get(), (juce::Component*) bandToggle.get(), &bandRemove,
                                    (juce::Component*) gainKnob.get(), (juce::Component*) freqKnob.get(), (juce::Component*) qKnob.get() })
            c->setVisible (used);
        repaint();
    }

    UiContext& ctx;
    ParamEq& eq;
    juce::Colour colour;
    Section section;
    Toggle onToggle;
    EqGraph graph;
    ExpandButton expand;
    Chip menu { "...", theme::text };
    std::unique_ptr<EqTypeBar> typeBar;
    std::unique_ptr<Toggle> bandToggle;
    TrashButton bandRemove;
    std::unique_ptr<Knob> gainKnob, freqKnob, qKnob;
    uint32_t typeVersion = 0;
    int shownUsed = -1;
};

//==============================================================================
class GatePanel final : public AdvancedPanel
{
public:
    GatePanel (UiContext& c, ChannelFx& s, GateFx& g)
        : strip (s), fx (g),
          thr (c, *g.thr, "THRESHOLD", theme::fxGate), range (c, *g.range, "RANGE", theme::fxGate),
          attack (c, *g.attack, "ATTACK", theme::fxGate), hold (c, *g.hold, "HOLD", theme::fxGate),
          release (c, *g.release, "RELEASE", theme::fxGate),
          sc (c, *g.scOn, "ON", theme::fxGate), scFreq (c, *g.scFreq, "FREQUENCY", theme::fxGate),
          on (c, *g.on, "ON", theme::fxGate),
          voice (c, *g.voice, "ON", theme::fxGate), voiceThr (c, *g.voiceThr, "VOICE THR", theme::fxGate),
          state ([&g] { return g.on->isOn() ? juce::jmap (gainToDb (g.dsp.shownGain.load()), -80.0f, 0.0f, 0.0f, 1.0f) : 0.0f; }, theme::fxGate, "OPEN"),
          vad ([&s] { return juce::jmax (0.0f, s.voiceProbability()); }, theme::fxGate, "VOICE")
    {
        vad.setBallistics (kVoiceRise, kVoiceFall);
        showValues ({ &thr, &range, &attack, &hold, &release, &scFreq, &voiceThr });
        for (juce::Component* k : std::initializer_list<juce::Component*> { &thr, &range, &attack, &hold, &release, &sc, &scFreq,
                                                                           &on, &voice, &voiceThr, &state, &vad, &info })
            addAndMakeVisible (k);
        on.setTooltip ("Switch the gate on or off");
        sc.setTooltip ("The gate listens only to this frequency region (e.g. to ignore rumble)");
        voice.setTooltip ("Voice Key: the gate only opens while the denoiser detects speech (needs the denoiser on)");
        info.setText ("Gate\nSidechain filter: the gate listens only to this frequency region (e.g. ignore rumble).\n\n"
                      "Voice Key: it only opens while the denoiser detects speech, so keyboard and background noise stay out. "
                      "Needs the denoiser in the chain and switched on.");
        setSize (660, 2 * kPad + 2 * kCardHeight + kGap);
    }

    void resized() override
    {
        cards.clear();
        auto r = getLocalBounds().reduced (kPad);
        const auto top = r.removeFromTop (kCardHeight);
        rowLayout (card (top, "Gate"), { &thr, &range, &attack, &hold, &release });
        on.setBounds (captionSlot (top, 44));
        info.setBounds (captionSlot (top, 20, 52).withSizeKeepingCentre (InfoButton::kSize, InfoButton::kSize));
        r.removeFromTop (kGap);

        const auto filter = r.removeFromLeft (160);
        scFreq.setBounds (card (filter, "Sidechain filter").withSizeKeepingCentre (84, kKnobs));
        sc.setBounds (captionSlot (filter, 44));
        r.removeFromLeft (kGap);

        const auto key = r.removeFromLeft (160);
        voiceThr.setBounds (card (key, "Voice Key").withSizeKeepingCentre (84, kKnobs));
        voice.setBounds (captionSlot (key, 44));
        r.removeFromLeft (kGap);

        auto live = card (r, "Now");
        auto barsArea = live.removeFromTop (46).withTrimmedTop (4);
        state.setBounds (barsArea.removeFromTop (14));
        barsArea.removeFromTop (10);
        vad.setBounds (barsArea.removeFromTop (14));
        hint = live;
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        // Only a warning stays as text: everything else is behind the "i".
        if (! needsDenoiser()) return;
        g.setColour (theme::warn);
        g.setFont (theme::font (11.0f));
        g.drawFittedText ("Voice Key needs the denoiser on. Until then the gate works on level only.", hint, juce::Justification::centredLeft, 3, 1.0f);
    }

private:
    bool needsDenoiser() const { return fx.voice->isOn() && ! strip.denoiserActive(); }

    void tick() override
    {
        vad.setMarker (fx.voice->isOn() ? fx.voiceThr->get() * 0.01f : -1.0f);
        if (needsDenoiser() != shownHint) { shownHint = ! shownHint; repaint (hint); }
    }

    ChannelFx& strip;
    GateFx& fx;
    Knob thr, range, attack, hold, release;
    Toggle sc;
    Knob scFreq;
    Toggle on, voice;
    Knob voiceThr;
    LiveBar state, vad;
    InfoButton info;
    juce::Rectangle<int> hint;
    bool shownHint = false;
};

//==============================================================================
class DenoiserPanel final : public AdvancedPanel
{
public:
    DenoiserPanel (UiContext& c, DenoiserFx& d)
        : fx (d),
          mode (c, *d.mode, theme::good), amount (c, *d.amount, "AMOUNT", theme::good),
          on (c, *d.on, "ON", theme::good),
          vad ([&d] { auto* x = d.live.load(); return x != nullptr && d.on->isOn() ? x->getVad() : 0.0f; }, theme::good, "VOICE")
    {
        vad.setBallistics (kVoiceRise, kVoiceFall);
        showValues ({ &amount });
        for (juce::Component* k : std::initializer_list<juce::Component*> { &mode, &amount, &on, &vad, &info })
            addAndMakeVisible (k);
        on.setTooltip ("Switch the denoiser on or off");
        info.setText ("Latency modes\nUltra Low / Low / Aligned apply RNNoise's band gains with a zero-latency filterbank: "
                      "less latency = the gains react a little later at word onsets. Native HQ = original RNNoise synthesis (30 ms).\n\n"
                      "RNNoise itself works at 48 kHz. At other sample rates it listens to a resampled copy: the first three modes "
                      "keep their latency, Native HQ takes a few ms more.\n\n"
                      "To mute between words, use the Gate with \"Voice Key\".");
        setSize (620, 2 * kPad + kModeCard + kGap + kCardHeight);
    }

    void resized() override
    {
        cards.clear();
        auto r = getLocalBounds().reduced (kPad);
        const auto top = r.removeFromTop (kModeCard);
        auto modeArea = card (top, "Latency mode");
        on.setBounds (captionSlot (top, 44));
        info.setBounds (captionSlot (top, 20, 52).withSizeKeepingCentre (InfoButton::kSize, InfoButton::kSize));
        mode.setBounds (modeArea.removeFromTop (30));
        latency = modeArea.withTrimmedTop (4);
        r.removeFromTop (kGap);

        const auto mix = r.removeFromLeft (160);
        amount.setBounds (card (mix, "Mix").withSizeKeepingCentre (84, kKnobs));
        r.removeFromLeft (kGap);
        auto live = card (r, "Voice detection");
        vad.setBounds (live.withSizeKeepingCentre (live.getWidth(), 14));
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        auto* d = fx.live.load();
        const int m = juce::roundToInt (fx.mode->get());
        const double ms = d != nullptr ? d->latencyMsForMode (m) : dsp::Denoiser::latencyForMode (m) / 48.0;
        g.setColour (theme::textDim);
        g.setFont (theme::font (11.5f));
        g.drawText ("Latency while on: " + juce::String (ms, ms == std::floor (ms) ? 0 : 1) + " ms", latency, juce::Justification::centredLeft);
    }

private:
    static constexpr int kModeCard = kCaption + 30 + 4 + 18 + 8;

    void tick() override
    {
        if (const auto v = fx.mode->getVersion(); v != modeVersion) { modeVersion = v; repaint (latency); }
    }

    DenoiserFx& fx;
    ChoiceSegments mode;
    Knob amount;
    Toggle on;
    LiveBar vad;
    InfoButton info;
    juce::Rectangle<int> latency;
    uint32_t modeVersion = 0;
};

//==============================================================================
/** Static compressor curve with the live gain reduction and a point where the signal is now (wish of the user
    11.11.): x = level entering the compressor, y = level leaving it (with the gain reduction it applies right
    now, so attack / release show as the point lagging below / above the curve). A short trail follows it. */
class CompCurve final : public juce::Component
{
public:
    explicit CompCurve (CompFx& c) : fx (c)
    {
        setInterceptsMouseClicks (false, false);
        fx.views.fetch_add (1);
    }
    ~CompCurve() override { fx.views.fetch_sub (1); }

    void update()
    {
        // Called at 30 Hz: the number follows smoothly and changes at most ~6 x per second, so it can be read.
        const float gr = fx.gr.load();
        smoothGr += (gr - smoothGr) * (gr > smoothGr ? 0.5f : 0.15f);
        uint64_t key = 0;
        for (auto* p : { fx.input, fx.thr, fx.ratio, fx.knee, fx.makeup, fx.autoGain })
            key += p->getVersion();
        ++ticks;
        bool dirty = key != shownKey;
        if (std::abs (smoothGr - shownGr) > 0.1f && ticks >= 5) dirty = true;

        // The live point: peaks rise at once, fall over ~0.3 s (like a meter), so it can be followed by eye.
        const bool on = fx.on->isOn() && gr >= 0.0f;
        const float inDb = juce::Decibels::gainToDecibels (fx.inPeak.load(), -100.0f);
        levelDb = inDb > levelDb ? inDb : juce::jmax (inDb, levelDb - 3.0f);
        const bool visible = on && levelDb > -60.0f;
        trail[(size_t) (trailAt++ % trail.size())] = visible ? juce::Point<float> (levelDb, gr) : juce::Point<float> (-200.0f, 0.0f);
        if (visible || pointShown) dirty = true;
        pointShown = visible;

        if (dirty)
        {
            if (std::abs (smoothGr - shownGr) > 0.1f && ticks >= 5) { shownGr = smoothGr; ticks = 0; }
            shownKey = key;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (theme::bg);
        g.fillRoundedRectangle (r, 6.0f);
        auto map = [r] (float inDb, float outDb)
        {
            return juce::Point<float> (r.getX() + r.getWidth() * (inDb + 60.0f) / 60.0f,
                                       r.getBottom() - r.getHeight() * (juce::jlimit (-60.0f, 0.0f, outDb) + 60.0f) / 60.0f);
        };
        g.setColour (theme::outline);
        g.drawLine ({ map (-60, -60), map (0, 0) }, 1.0f);
        const auto s = fx.settings();
        juce::Path p;
        for (float in = -60.0f; in <= 0.0f; in += 0.5f)
        {
            const float x = in + s.inputDb;
            const auto pt = map (in, x + s.staticGainDb (x) + s.totalMakeupDb());
            if (in == -60.0f) p.startNewSubPath (pt); else p.lineTo (pt);
        }
        g.setColour (theme::fxComp);
        g.strokePath (p, juce::PathStrokeType (2.0f));
        const auto t = map (s.thresholdDb, s.thresholdDb);
        g.setColour (theme::textFaint);
        g.fillRect (t.x, r.getY(), 1.0f, r.getHeight());

        // Live point and its trail (oldest faintest). Output = input + input gain - live reduction + makeup.
        auto outOf = [&s] (juce::Point<float> v) { return v.x + s.inputDb - v.y + s.totalMakeupDb(); };
        const int count = (int) trail.size();
        for (int k = count - 1; k >= 0; --k)   // k = age in ticks
        {
            const auto v = trail[(size_t) ((trailAt - 1 - k + 4 * count) % count)];
            if (v.x < -60.0f) continue;
            const auto pt = map (juce::jmin (0.0f, v.x), outOf (v));
            const float fade = 1.0f - (float) k / (float) count;
            const float rad = k == 0 ? 4.5f : 1.5f + 2.0f * fade;
            if (k == 0)
            {
                g.setColour (theme::fxComp.withAlpha (0.25f));
                g.fillEllipse (pt.x - 9.0f, pt.y - 9.0f, 18.0f, 18.0f);
                g.setColour (juce::Colours::white);
            }
            else
                g.setColour (theme::fxComp.withAlpha (0.12f + 0.4f * fade));
            g.fillEllipse (pt.x - rad, pt.y - rad, 2.0f * rad, 2.0f * rad);
        }

        g.setColour (theme::text);
        g.setFont (theme::font (11.0f, true));
        g.drawText ("GR " + juce::String (juce::jmax (0.0f, shownGr), 1) + " dB", r.reduced (8.0f, 6.0f), juce::Justification::topLeft);
    }

private:
    CompFx& fx;
    float shownGr = -1.0f, smoothGr = 0.0f, levelDb = -100.0f;
    int ticks = 0;
    uint64_t shownKey = 0;
    std::array<juce::Point<float>, 8> trail { [] { std::array<juce::Point<float>, 8> a; a.fill ({ -200.0f, 0.0f }); return a; }() };
    int trailAt = 0;
    bool pointShown = false;
};

class CompPanel final : public AdvancedPanel
{
public:
    CompPanel (UiContext& c, CompFx& s)
        : input (c, *s.input, "INPUT", theme::fxComp), thr (c, *s.thr, "THRESHOLD", theme::fxComp),
          ratio (c, *s.ratio, "RATIO", theme::fxComp), knee (c, *s.knee, "KNEE", theme::fxComp),
          attack (c, *s.attack, "ATTACK", theme::fxComp), release (c, *s.release, "RELEASE", theme::fxComp),
          makeup (c, *s.makeup, "MAKEUP", theme::fxComp), autoMakeup (c, *s.autoGain, "AUTO GAIN", theme::fxComp),
          on (c, *s.on, "ON", theme::fxComp), curve (s),
          gr ([&s] { return s.gr.load() / 24.0f; }, theme::fxComp, "GR")
    {
        showValues ({ &input, &thr, &ratio, &knee, &attack, &release, &makeup });
        for (juce::Component* k : std::initializer_list<juce::Component*> { &input, &thr, &ratio, &knee, &attack, &release, &makeup, &autoMakeup, &on, &curve, &gr })
            addAndMakeVisible (k);
        on.setTooltip ("Switch the compressor on or off");
        autoMakeup.setTooltip ("Makes up for the reduction at 0 dBFS automatically");
        setSize (660, 2 * kPad + 2 * kCardHeight + kGap);
    }

    void resized() override
    {
        cards.clear();
        auto r = getLocalBounds().reduced (kPad);
        {
            auto c = card (r.removeFromRight (210), "Curve");
            gr.setBounds (c.removeFromBottom (12));
            c.removeFromBottom (6);
            curve.setBounds (c);
        }
        r.removeFromRight (kGap);

        const auto top = r.removeFromTop (kCardHeight);
        rowLayout (card (top, "Compressor"), { &input, &thr, &ratio, &knee });
        on.setBounds (captionSlot (top, 44));
        r.removeFromTop (kGap);

        const auto bottom = r.removeFromTop (kCardHeight);
        rowLayout (card (bottom, "Timing and output"), { &attack, &release, &makeup });
        autoMakeup.setBounds (captionSlot (bottom, 84));
    }

private:
    void tick() override { curve.update(); }

    Knob input, thr, ratio, knee, attack, release, makeup;
    Toggle autoMakeup, on;
    CompCurve curve;
    LiveBar gr;
};

//==============================================================================
/** Correction stage of a hardware output (room + speakers): ON, the profile's curve, name and size.
    The expand button (or a click on the curve) opens the Correction window with everything else. */
class CorrectionSection final : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    CorrectionSection (UiContext& c, int channel, juce::Colour col)
        : ctx (c), index (channel), corr (*c.engine.correctionFor (channel)), colour (col),
          section ("Correction", theme::fxCorrection), onToggle (c, *corr.on, "ON", theme::fxCorrection)
    {
        addAndMakeVisible (section);
        section.addAndMakeVisible (onToggle);
        section.addAndMakeVisible (expand);
        section.headerExtra = 32;
        expand.setTooltip ("Open the correction in a large window (measure, import, level, bands)");
        expand.onClick = [this] { open(); };
        onToggle.setTooltip ("Room correction on or off (zero latency, before the EQ)");
        section.setInterceptsMouseClicks (false, true);
        setTooltip ("Room correction of the speakers. Click to open it: calibrate the speakers step by step "
                    "or import an Equalizer APO file.");
        startTimerHz (5);
    }

    Section& getSection() noexcept { return section; }

    void resized() override
    {
        section.setBounds (getLocalBounds());
        onToggle.setBounds (section.headerSlot());
        expand.setBounds (section.headerSlot().withRight (section.beforeSwitch()).withLeft (section.beforeSwitch() - 26));
    }

    void mouseDown (const juce::MouseEvent& e) override { if (e.mods.isPopupMenu() && section.onMenu) section.onMenu(); }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mouseWasClicked() && ! e.mods.isPopupMenu() && section.content().contains (e.getPosition())) open();
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        auto r = section.content();
        const auto& p = corr.getProfile();
        auto text = r.removeFromBottom (30);
        g.setFont (theme::font (11.5f, true));
        g.setColour (p.isEmpty() ? theme::textFaint : theme::text);
        g.drawText (p.isEmpty() ? juce::String ("No profile") : p.name, text.removeFromTop (15), juce::Justification::centredLeft, true);
        g.setFont (theme::font (10.5f));
        g.setColour (theme::textDim);
        g.drawText (p.isEmpty() ? juce::String (Engine::isHwOutput (index) ? "Click to measure or import" : "Click to use a saved correction") : p.describe(),
                    text, juce::Justification::centredLeft, true);

        auto plot = r.reduced (0, 2).toFloat();
        g.setColour (theme::bg.withAlpha (0.6f));
        g.fillRoundedRectangle (plot, 5.0f);
        if (p.isEmpty()) return;
        double range = 12.0;
        const int channels = p.separate ? 2 : 1;   // left / right corrected separately: both curves
        std::vector<double> db[2];
        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i <= 120; ++i)
            {
                db[ch].push_back (p.magnitudeDb (20.0 * std::pow (1000.0, i / 120.0), 48000.0, ch));
                range = juce::jmax (range, std::abs (db[ch].back()) * 1.1);
            }
        range = juce::jmin (range, 30.0);
        auto fy = [&] (double v) { return plot.getCentreY() - (float) (juce::jlimit (-range, range, v) / range) * plot.getHeight() * 0.45f; };
        g.setColour (theme::outline);
        g.drawHorizontalLine (juce::roundToInt (plot.getCentreY()), plot.getX() + 4.0f, plot.getRight() - 4.0f);
        for (int ch = 0; ch < channels; ++ch)
        {
            juce::Path path;
            for (int i = 0; i <= 120; ++i)
            {
                const float x = plot.getX() + 4.0f + (plot.getWidth() - 8.0f) * (float) i / 120.0f;
                if (i == 0) path.startNewSubPath (x, fy (db[ch][0]));
                else        path.lineTo (x, fy (db[ch][(size_t) i]));
            }
            g.setColour ((ch == 1 ? theme::accentVirt : colour).withAlpha (corr.on->isOn() ? 1.0f : 0.35f));
            g.strokePath (path, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        g.setColour (theme::textFaint);
        g.setFont (theme::font (9.5f));
        g.drawText ("+-" + juce::String (juce::roundToInt (range)) + " dB", plot.reduced (5.0f, 2.0f), juce::Justification::topRight);
    }

private:
    void open()
    {
        if (Engine::isHwOutput (index)) { if (ctx.openCorrectionWindow) ctx.openCorrectionWindow (index); return; }
        // Not a hardware output: no measuring here, but any saved correction (pool) can be used.
        juce::PopupMenu m;
        m.addSectionHeader ("Saved corrections");
        const auto entries = pool::list();
        for (auto& en : entries)
            m.addItem (en.profile.name + "   (" + en.profile.describe() + ")", true, en.id == corr.getProfile().poolId, [this, p = en.profile]
            {
                ctx.engine.setCorrection (index, p);
                corr.on->set (1.0f);
            });
        if (entries.empty()) m.addItem ("(none yet: calibrate a hardware output first)", false, false, nullptr);
        m.addSeparator();
        m.addItem ("No correction", ! corr.getProfile().isEmpty(), false, [this] { ctx.engine.setCorrection (index, {}); });
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this));
    }

    void timerCallback() override
    {
        const auto v = corr.getVersion() * 2u + (corr.on->isOn() ? 1u : 0u);
        if (v != shown) { shown = v; repaint(); }
    }

    UiContext& ctx;
    int index;
    Correction& corr;
    juce::Colour colour;
    Section section;
    Toggle onToggle;
    ExpandButton expand;
    uint32_t shown = ~0u;
};

//==============================================================================
/** Small built-in effects of the chain: a section with the main controls, the rest in the "..." panel. */
class GateUnit final : public juce::Component
{
public:
    GateUnit (UiContext& c, ChannelFx& s, int k, const juce::String& title)
        : ctx (c), strip (s), fx (s.gates[(size_t) k]), section (title, theme::fxGate),
          on (c, *fx.on, "ON", theme::fxGate), thr (c, *fx.thr, "THRESH", theme::fxGate),
          state ([this] { return fx.on->isOn() ? juce::jmap (gainToDb (fx.dsp.shownGain.load()), -80.0f, 0.0f, 0.0f, 1.0f) : 0.0f; }, theme::fxGate, "OPEN")
    {
        addAndMakeVisible (section);
        for (juce::Component* k2 : std::initializer_list<juce::Component*> { &on, &thr, &state }) section.addAndMakeVisible (k2);
        section.enableGear ([this, title] { ctx.openPanel (std::make_unique<GatePanel> (ctx, strip, fx), title + "  -  " + nameOf (ctx.engine, strip)); });
    }
    void resized() override
    {
        section.setBounds (getLocalBounds());
        on.setBounds (section.headerSlot());
        auto gc = section.content();
        state.setBounds (gc.removeFromRight (26));
        thr.setBounds (gc.withSizeKeepingCentre (gc.getWidth(), 70));
    }

    UiContext& ctx;
    ChannelFx& strip;
    GateFx& fx;
    Section section;
    Toggle on;
    Knob thr;
    LiveBar state;
};

class DenoiserUnit final : public juce::Component
{
public:
    DenoiserUnit (UiContext& c, ChannelFx& s, int k, const juce::String& title)
        : ctx (c), strip (s), fx (s.denoisers[(size_t) k]), section (title, theme::good),
          on (c, *fx.on, "ON", theme::good), amount (c, *fx.amount, "AMOUNT", theme::good)
    {
        addAndMakeVisible (section);
        section.addAndMakeVisible (on);
        section.addAndMakeVisible (amount);
        section.enableGear ([this, title] { ctx.openPanel (std::make_unique<DenoiserPanel> (ctx, fx), title + " (RNNoise)  -  " + nameOf (ctx.engine, strip)); });
    }
    void resized() override
    {
        section.setBounds (getLocalBounds());
        on.setBounds (section.headerSlot());
        auto dc = section.content();
        modeArea = dc.removeFromBottom (16);
        amount.setBounds (dc.withSizeKeepingCentre (dc.getWidth(), 70));
    }
    void paintOverChildren (juce::Graphics& g) override
    {
        g.setColour (theme::textFaint);
        g.setFont (theme::font (10.0f));
        g.drawText (fx.mode->toText(), modeArea, juce::Justification::centred);
    }
    void tick() { if (const auto v = fx.mode->getVersion(); v != modeVersion) { modeVersion = v; repaint (modeArea); } }

    UiContext& ctx;
    ChannelFx& strip;
    DenoiserFx& fx;
    Section section;
    Toggle on;
    Knob amount;
    juce::Rectangle<int> modeArea;
    uint32_t modeVersion = 0;
};

//==============================================================================
/** Channels (selection index scheme) whose chain has the Stems effect. */
std::vector<int> stemsChannels (const Engine& e)
{
    std::vector<int> v;
    for (int i = 0; i < kMaxInputs + kMaxBuses; ++i)
        if ((i < kMaxInputs ? e.isInputActive (i) : e.isBusActive (i - kMaxInputs)) && e.fxOf (i).plugins.hasBuiltin (Fx::stems)) v.push_back (i);
    return v;
}


/** Why the model of a mode does not run (empty while it runs). */
juce::String stemsProblem (const stems::Service::Status& st, stems::Model m)
{
    using S = stems::Service::State;
    switch (st.state)
    {
        case S::unavailable: return st.message.isNotEmpty() ? st.message : juce::String ("Not available in this build");
        case S::noModel:     return stems::Service::modelFile (m).existsAsFile() ? "Starting..." : "Model not downloaded yet: click here";
        case S::downloading: return "Downloading " + juce::String (juce::roundToInt (st.progress * 100.0f)) + " %";
        case S::preparing:   return "Preparing the model";
        case S::loading:     return "Loading onto the GPU";
        case S::failed:      return "Error: " + st.message;
        case S::ready:       break;
    }
    return {};
}

/** GPU load of the separation (like the DSP meter in the top bar): label, slim bar, percentage. */
class GpuBar final : public juce::Component, public juce::SettableTooltipClient
{
public:
    /** Target; the bar glides there (~0.25 s, VBlank, only while moving), the number changes once a second (mean of
        the last second), like the other live displays. */
    void setLoad (float l) { target = juce::jmax (0.0f, l); }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        g.setColour (theme::textDim);
        g.setFont (theme::font (11.0f));
        g.drawText ("GPU", r.removeFromLeft (28), juce::Justification::centredLeft);
        auto text = r.removeFromRight (38);
        auto bar = r.withSizeKeepingCentre (r.getWidth(), 6).toFloat();
        g.setColour (theme::bg);
        g.fillRoundedRectangle (bar, 3.0f);
        g.setColour (load < 0.5f ? theme::good : load < 0.8f ? theme::solo : theme::mute);
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, load)), 3.0f);
        g.setColour (theme::text);
        g.drawText (juce::String (juce::roundToInt (number * 100.0f)) + " %", text, juce::Justification::centredRight);
    }

private:
    void frame (double t)
    {
        const float dt = last > 0.0 ? (float) juce::jlimit (0.0, 0.1, t - last) : 1.0f / 60.0f;
        if (last <= 0.0) { load = number = target; repaint(); }   // first frame: no glide from 0
        last = t;
        if (secondStart <= 0.0) secondStart = t;
        sum += target * dt;
        sumT += dt;
        bool changed = false;
        if (t - secondStart >= 1.0)
        {
            const float mean = sum / juce::jmax (1.0e-3f, sumT);
            sum = sumT = 0.0f;
            secondStart = t;
            if (std::abs (mean - number) >= 0.005f) { number = mean; changed = true; }
        }
        const float before = load;
        load += (target - load) * (1.0f - std::exp (-dt / 0.25f));
        if (std::abs (target - load) < 0.002f) load = target;
        if (changed || std::abs (load - before) > 0.0005f) repaint();
    }
    float load = 0.0f, target = 0.0f, number = 0.0f, sum = 0.0f, sumT = 0.0f;
    double last = 0.0, secondStart = 0.0;
    juce::VBlankAttachment vblank { this, [this] (double t) { if (isShowing()) frame (t); } };
};

/** The latency of the separation, large, with a line below (mode, or a warning). */
class StemsLatency final : public juce::Component, public juce::SettableTooltipClient
{
public:
    void set (double seconds, const juce::String& line, juce::Colour lineColour)
    {
        if (seconds == shownSeconds && line == shownLine && lineColour == colour) return;
        shownSeconds = seconds; shownLine = line; colour = lineColour;
        repaint();
    }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        const bool twoLines = r.getHeight() >= 30;
        auto big = twoLines ? r.removeFromTop (r.getHeight() * 3 / 5) : r.removeFromLeft (52);
        g.setColour (theme::text);
        g.setFont (theme::font (twoLines ? 17.0f : 15.0f, true));
        g.drawText (juce::String (shownSeconds, 1) + " s", big, juce::Justification::bottomLeft, false);
        g.setColour (colour);
        g.setFont (theme::font (10.5f));
        g.drawFittedText (shownLine, r, twoLines ? juce::Justification::topLeft : juce::Justification::centredLeft, 1, 0.85f);
    }

private:
    double shownSeconds = -1.0;
    juce::String shownLine;
    juce::Colour colour = theme::textDim;
};

/** How much of the last 30 s played unseparated (the GPU was late): differences of the instance's running count. */
struct MissWindow
{
    double seconds (const stems::Instance* inst)
    {
        if (inst == nullptr) { history.clear(); return 0.0; }
        const double now = juce::Time::getMillisecondCounterHiRes();
        const auto count = inst->getMissedSamples();
        if (! history.empty() && count < history.back().second) history.clear();   // a new instance
        history.emplace_back (now, count);
        while (history.size() > 1 && now - history.front().first > 30000.0) history.pop_front();
        return (double) (count - history.front().second) / juce::jmax (1.0, inst->getSampleRate());
    }
    std::deque<std::pair<double, uint64_t>> history;
};

/** Latency the channel has (or will have) in its mode with the GPU setting of the mode's model. */
double stemsLatencySeconds (const Engine& e, const ChannelFx& s)
{
    const int mode = juce::roundToInt (s.stems.mode->get());
    if (const auto* inst = s.stems.live.load(); inst != nullptr && inst->getMode() == mode && inst->getLatencySamples() > 0)
        return inst->getLatencySamples() / juce::jmax (1.0, inst->getSampleRate());
    const auto m = stems::modelFor (mode);
    return stems::timingFor (mode, e.stemsTuning (m), stems::Service::referenceMs (m)).latency();
}

/** Shared by block and window: latency line, GPU bar and their tooltips from the current state. */
void updateStemsInfo (const Engine& e, const ChannelFx& s, StemsLatency& latency, GpuBar& gpu, MissWindow& misses)
{
    auto& svc = stems::Service::get();
    const int mode = juce::roundToInt (s.stems.mode->get());
    const auto model = stems::modelFor (mode);
    const auto tuning = e.stemsTuning (model);
    const auto others = stemsChannels (e);
    juce::String line = s.stems.mode->choices[mode] + (stems::hasGuitar (mode) ? juce::String() : " (no guitar)")
                      + (tuning.automatic ? juce::String() : ", GPU " + juce::String (juce::roundToInt (tuning.share * 100.0f)) + " %");
    auto colour = theme::textDim;
    if (others.size() > 1)
    {
        juce::StringArray names;
        for (int i : others) if (i != channelOf (e, s)) names.add (channelCode (i));
        line = "Also on " + names.joinIntoString (", ") + ": shares the GPU";
        colour = theme::warn;
    }
    const auto* inst = s.stems.live.load();
    if (const double missed = misses.seconds (inst); missed >= 0.05)
    {
        line = juce::String (missed, 1) + " s unseparated in the last 30 s";
        colour = theme::warn;
    }
    latency.set (stemsLatencySeconds (e, s), line, colour);
    latency.setTooltip ("The channel plays this much later while Stems is on: the model listens a little ahead.\n"
                        "Auto: Live ~1.3 s (HT-Demucs, no guitar), Fast ~2.2 s, Balanced ~2.8 s, Best ~3.6 s (BS-RoFormer).\n"
                        "More GPU for a model (Stems window) = less latency.\n\n"
                        "\"Unseparated\": when the GPU is late (other programs, several channels), that part plays the\n"
                        "original instead of a dropout. Counted over the last 30 s.");
    const double total = svc.getLoad(), own = inst != nullptr ? svc.loadOf (*inst) : svc.loadOf (mode, tuning);
    gpu.setLoad ((float) total);
    gpu.setTooltip ("GPU time the separation needs, all channels with Stems together (this channel: about "
                    + juce::String (juce::roundToInt (own * 100.0)) + " %). Over 100 % the GPU cannot keep up and parts play unseparated.");
}

/** Mute + solo of one stem (small, side by side). */
struct StemSwitches
{
    StemSwitches (UiContext& c, StemsFx& fx, int k)
        : mute (c, *fx.mute[(size_t) k], "M", theme::mute), solo (c, *fx.solo[(size_t) k], "S", theme::solo)
    {
        mute.setTooltip ("Mute " + stems::stemName (k).toLowerCase());
        solo.setTooltip ("Solo " + stems::stemName (k).toLowerCase() + " (only soloed stems play; Shift + click: several at once)");
        solo.exclusiveGroup = [&fx] { return std::vector<Param*> (fx.solo.begin(), fx.solo.end()); };
    }
    void addTo (juce::Component& parent) { parent.addAndMakeVisible (mute); parent.addAndMakeVisible (solo); }
    void setBounds (juce::Rectangle<int> r)
    {
        mute.setBounds (r.removeFromLeft (r.getWidth() / 2).reduced (1, 0));
        solo.setBounds (r.reduced (1, 0));
    }
    void setAlpha (float a) { mute.setAlpha (a); solo.setAlpha (a); }
    Toggle mute, solo;
};

constexpr const char* kStemLabels[] = { "VOCALS", "DRUMS", "BASS", "GUITAR", "REST" };
constexpr const char* kNoGuitarTip = "Live mode does not separate guitar: it is part of the rest";

/** Level of one stem after its gain, in the scale of its fader (grey while it does not play). */
class StemMeter final : public juce::Component
{
public:
    StemMeter (StemsFx& f, int k) : fx (f), stem (k) { setInterceptsMouseClicks (false, false); }
    void paint (juce::Graphics& g) override
    {
        auto bar = getLocalBounds().toFloat();
        g.setColour (theme::bg);
        g.fillRoundedRectangle (bar, 2.0f);
        if (shown <= 0.0f) return;
        float gains[stems::numStems];
        fx.gains (gains);
        g.setColour (gains[stem] > 0.0f ? theme::stemColour (stem) : theme::textFaint);
        g.fillRoundedRectangle (bar.withTop (bar.getBottom() - bar.getHeight() * shown), 2.0f);
    }

private:
    void frame (double t)
    {
        const float dt = last > 0.0 ? (float) juce::jlimit (0.0, 0.1, t - last) : 1.0f / 60.0f;
        last = t;
        const float pk = fx.takePeak (stem) * dbToGain (fx.gain[(size_t) stem]->get());
        const auto& range = fx.gain[(size_t) stem]->range;
        const float target = pk > 1.0e-4f ? range.convertTo0to1 (juce::jlimit (range.start, range.end, gainToDb (pk))) : 0.0f;
        level = target > level ? target : level + (target - level) * (1.0f - std::exp (-dt / 0.35f));
        if (std::abs (level - shown) > 0.004f || (level < 0.004f && shown != 0.0f))
        {
            shown = level < 0.004f ? 0.0f : level;
            repaint();
        }
    }
    StemsFx& fx;
    int stem;
    float level = 0.0f, shown = 0.0f;
    double last = 0.0;
    juce::VBlankAttachment vblank { this, [this] (double t) { frame (t); } };
};

/** Horizontal slider for a GPU share (%): track, fill, thumb, value. Dragging it while Auto is on switches Auto off. */
class ShareSlider final : public ParamControl
{
public:
    ShareSlider (UiContext& c, Param& p, Param& autoParam) : ParamControl (c, p), autoMode (autoParam)
    {
        setTooltip ("GPU share per channel: more = the model runs more often = less latency (and more GPU load)");
    }
    void paint (juce::Graphics& g) override
    {
        const bool manual = ! autoMode.isOn();
        auto r = getLocalBounds().toFloat();
        auto text = r.removeFromRight (44.0f);
        auto t = r.reduced (8.0f, 0.0f).withSizeKeepingCentre (r.getWidth() - 16.0f, 6.0f);
        g.setColour (theme::bg);
        g.fillRoundedRectangle (t, 3.0f);
        const float x = t.getX() + param.getNormalised() * t.getWidth();
        const auto c = theme::fxStems.withAlpha (manual ? 1.0f : 0.35f);
        g.setColour (c);
        g.fillRoundedRectangle (t.withRight (x), 3.0f);
        g.setColour (manual ? theme::text : theme::textFaint);
        g.fillEllipse (juce::Rectangle<float> (12.0f, 12.0f).withCentre ({ x, t.getCentreY() }));
        g.setFont (theme::font (11.5f));
        g.drawText (manual ? param.toText().replace (".0 %", " %") : juce::String ("Auto"), text, juce::Justification::centredRight);
    }
    void pollParams() override
    {
        ParamControl::pollParams();
        if (const auto v = param.getVersion() + autoMode.getVersion(); v != shown) { shown = v; repaint(); }
    }

private:
    void press (const juce::MouseEvent& e) override { setFrom (e); }
    void drag (const juce::MouseEvent& e) override  { setFrom (e); }
    void setFrom (const juce::MouseEvent& e)
    {
        if (autoMode.isOn()) autoMode.set (0.0f);
        const float w = (float) getWidth() - 44.0f - 16.0f;
        param.setNormalised (juce::jlimit (0.0f, 1.0f, ((float) e.x - 8.0f) / juce::jmax (1.0f, w)));
    }
    Param& autoMode;
    uint32_t shown = 0;
};

/** Blurs an image (component effect): scaled down, box-blurred, scaled up. Cheap enough for a few repaints a second. */
class BlurEffect final : public juce::ImageEffectFilter
{
public:
    void applyEffect (juce::Image& src, juce::Graphics& dest, float scale, float alpha) override
    {
        const int w = src.getWidth(), h = src.getHeight();
        if (w <= 0 || h <= 0) return;
        constexpr int kDown = 4;
        const int sw = juce::jmax (1, w / kDown), sh = juce::jmax (1, h / kDown);
        juce::Image small (juce::Image::ARGB, sw, sh, true, juce::SoftwareImageType());
        {
            juce::Graphics sg (small);
            sg.setImageResamplingQuality (juce::Graphics::mediumResamplingQuality);
            sg.drawImage (src, 0, 0, sw, sh, 0, 0, w, h);
        }
        boxBlur (small, juce::jmax (1, juce::roundToInt (2.0f * scale)));
        dest.setOpacity (alpha);
        dest.setImageResamplingQuality (juce::Graphics::mediumResamplingQuality);
        dest.drawImage (small, 0, 0, w, h, 0, 0, sw, sh);
    }

private:
    /** Three passes of a horizontal + vertical box blur (~ Gaussian). Premultiplied ARGB. */
    static void boxBlur (juce::Image& img, int radius)
    {
        juce::Image::BitmapData d (img, juce::Image::BitmapData::readWrite);
        const int w = d.width, h = d.height;
        std::vector<uint32_t> line ((size_t) juce::jmax (w, h)), out (line.size());
        auto pass = [&] (int count, auto&& at)
        {
            const int div = 2 * radius + 1;
            for (int i = 0; i < count; ++i)
                line[(size_t) i] = *reinterpret_cast<const uint32_t*> (at (i));
            for (int i = 0; i < count; ++i)
            {
                uint32_t sum[4] {};
                for (int k = -radius; k <= radius; ++k)
                {
                    const uint32_t px = line[(size_t) juce::jlimit (0, count - 1, i + k)];
                    for (int c = 0; c < 4; ++c) sum[c] += (px >> (8 * c)) & 0xffu;
                }
                uint32_t v = 0;
                for (int c = 0; c < 4; ++c) v |= (sum[c] / (uint32_t) div) << (8 * c);
                out[(size_t) i] = v;
            }
            for (int i = 0; i < count; ++i) *reinterpret_cast<uint32_t*> (at (i)) = out[(size_t) i];
        };
        for (int it = 0; it < 3; ++it)
        {
            for (int y = 0; y < h; ++y) pass (w, [&] (int x) { return d.getPixelPointer (x, y); });
            for (int x = 0; x < w; ++x) pass (h, [&] (int y) { return d.getPixelPointer (x, y); });
        }
    }
};

/** A container whose content can be blurred (behind the loading overlay). */
class BlurBox final : public juce::Component
{
public:
    BlurBox() { setInterceptsMouseClicks (false, true); }
    void setBlurred (bool b)
    {
        if (b == (getComponentEffect() != nullptr)) return;
        setComponentEffect (b ? &effect : nullptr);
        repaint();
    }
    std::function<void (juce::Graphics&)> onPaint;
    void paint (juce::Graphics& g) override { if (onPaint) onPaint (g); }

private:
    BlurEffect effect;
};

/** Over the blurred stem controls while the model loads (or the stream starts): a veil, a spinner and a line. Takes
    the clicks so nothing behind it moves meanwhile. The spinner animates only while visible. */
class StemsLoading final : public juce::Component
{
public:
    void set (bool spin, const juce::String& line)
    {
        if (spin == spinning && line == text) return;
        spinning = spin;
        text = line;
        repaint();
    }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (theme::panel2.withAlpha (0.45f));
        g.fillRoundedRectangle (r, 4.0f);
        const bool compact = r.getHeight() < 110.0f;
        const float size = compact ? 18.0f : 30.0f;
        auto textArea = compact ? r.withTrimmedLeft (size + 10.0f) : r.withTop (r.getCentreY() + size * 0.5f + 6.0f).withHeight (18.0f);
        g.setFont (theme::font (compact ? 11.0f : 12.5f, true));
        const float tw = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), text);
        const auto c = compact ? juce::Point<float> (r.getCentreX() - (tw + size + 10.0f) * 0.5f + size * 0.5f, r.getCentreY())
                               : r.getCentre().translated (0.0f, -6.0f);
        if (compact) textArea = juce::Rectangle<float> (c.x + size * 0.5f + 10.0f, r.getY(), tw + 4.0f, r.getHeight());
        if (spinning)
        {
            const float rad = size * 0.5f - 2.0f;
            juce::Path track, arc;
            track.addCentredArc (c.x, c.y, rad, rad, 0.0f, 0.0f, juce::MathConstants<float>::twoPi, true);
            g.setColour (theme::fxStems.withAlpha (0.2f));
            g.strokePath (track, juce::PathStrokeType (3.0f));
            const float a = (float) phase * juce::MathConstants<float>::twoPi;
            arc.addCentredArc (c.x, c.y, rad, rad, 0.0f, a, a + 1.9f, true);
            g.setColour (theme::fxStems);
            g.strokePath (arc, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        else if (compact) textArea = r;
        g.setColour (theme::text);
        g.drawText (text, textArea.toNearestInt(), compact && spinning ? juce::Justification::centredLeft : juce::Justification::centred, true);
    }

private:
    void frame (double t)
    {
        if (! spinning || ! isShowing()) { last = 0.0; return; }
        if (last > 0.0) phase = std::fmod (phase + (t - last) * 0.9, 1.0);
        last = t;
        repaint();
    }
    bool spinning = false;
    juce::String text;
    double phase = 0.0, last = 0.0;
    juce::VBlankAttachment vblank { this, [this] (double t) { frame (t); } };
};

/** What the stem controls show over themselves: nothing while the separation runs (or the effect is off), else a
    line (spin: something is in progress: loading, downloading, the first window). */
struct StemsCover
{
    bool show = false, spin = false;
    juce::String line;
};
StemsCover stemsCover (const ChannelFx& s)
{
    StemsCover c;
    const auto& fx = s.stems;
    if (! fx.on->isOn() || s.fxBypass->isOn()) return c;
    const int mode = juce::roundToInt (fx.mode->get());
    const auto model = stems::modelFor (mode);
    const auto st = stems::Service::get().getStatus (model);
    const juce::String name = juce::String (stems::modelInfo (model).name).upToFirstOccurrenceOf (",", false, false);
    using S = stems::Service::State;
    c.show = true;
    c.spin = true;
    switch (st.state)
    {
        case S::unavailable: c.spin = false; c.line = st.message.isNotEmpty() ? st.message : juce::String ("Not available in this build"); break;
        case S::failed:      c.spin = false; c.line = "Error: see the model below"; break;
        case S::downloading: c.line = "Downloading " + name + "  " + juce::String (juce::roundToInt (st.progress * 100.0f)) + " %"; break;
        case S::preparing:   c.line = "Preparing " + name; break;
        case S::loading:     c.line = "Loading " + name + " onto the GPU"; break;
        case S::noModel:
            if (stems::Service::modelFile (model).existsAsFile()) c.line = "Loading " + name + " onto the GPU";
            else { c.spin = false; c.line = "Download " + name + " below to separate"; }
            break;
        case S::ready:
        {
            const auto* inst = fx.live.load();
            if (inst != nullptr && inst->getMode() == mode && inst->isStarting())
                c.line = "Separation starts in " + juce::String (inst->secondsUntilWet(), 1) + " s";
            else c.show = false;
            break;
        }
    }
    return c;
}

/** Window of the Stems effect: latency mode, a fader + level + mute / solo per stem, the models. */
class StemsPanel final : public AdvancedPanel
{
public:
    StemsPanel (UiContext& c, ChannelFx& s)
        : ctx (c), strip (s), fx (s.stems), mode (c, *fx.mode, theme::fxStems, { "Live", "Fast", "Balanced", "Best" }, stems::modeOrder()),
          on (c, *fx.on, "ON", theme::fxStems)
    {
        for (juce::Component* k : std::initializer_list<juce::Component*> { &mode, &on, &info, &latency, &gpu, &mixBox })
            addAndMakeVisible (k);
        addChildComponent (loading);
        mixBox.onPaint = [this] (juce::Graphics& g) { paintNames (g); };
        for (int k = 0; k < stems::numStems; ++k)
        {
            auto& f = faders.emplace_back (std::make_unique<Fader> (c, *fx.gain[(size_t) k], theme::stemColour (k)));
            auto& m = meters.emplace_back (std::make_unique<StemMeter> (fx, k));
            auto& sw = switches.emplace_back (std::make_unique<StemSwitches> (c, fx, k));
            mixBox.addAndMakeVisible (*f);
            mixBox.addAndMakeVisible (*m);
            sw->addTo (mixBox);
        }
        for (int m = 0; m < stems::kNumModels; ++m)
        {
            auto& b = modelButtons.emplace_back (std::make_unique<Chip> ("", theme::fxStems));
            b->showArrow = false;
            b->centred = true;
            auto& d = deleteButtons.emplace_back (std::make_unique<TrashButton>());
            const auto model = (stems::Model) m;
            b->onClick = [model]
            {
                auto& svc = stems::Service::get();
                const auto st = svc.getStatus (model).state;
                if (st == stems::Service::State::downloading || st == stems::Service::State::preparing) svc.cancelDownload (model);
                else svc.startDownload (model);
            };
            d->setTooltip ("Delete this model");
            d->onClick = [this, model]
            {
                dialog::confirm (this, dialog::Kind::danger, "Delete the model?",
                                 juce::String (stems::modelInfo (model).name) + " is deleted. Channels using it then play unseparated "
                                 "until it is downloaded again.", "Delete", [model] { stems::Service::get().removeModel (model); });
            };
            addAndMakeVisible (*b);
            addChildComponent (*d);
            auto& au = autoToggles.emplace_back (std::make_unique<Toggle> (c, *c.engine.stemsAuto[(size_t) m], "AUTO", theme::fxStems));
            au->setTooltip ("Auto: the usual latency of each mode (Live 1.3 s ... Best 3.6 s). Off: set the GPU share yourself");
            auto& sh = shareSliders.emplace_back (std::make_unique<ShareSlider> (c, *c.engine.stemsGpu[(size_t) m], *c.engine.stemsAuto[(size_t) m]));
            addAndMakeVisible (*au);
            addAndMakeVisible (*sh);
        }
        on.setTooltip ("Switch the separation on or off");
        info.setText ("The Stem Separator (beta) splits the channel into vocals, drums, bass, guitar and the rest. The rest is what remains "
                      "after the others, so nothing is lost: all at 0 dB = the original.\n\n"
                      "Live: HT-Demucs (4 stems, guitar stays in the rest), ~1.3 s late, much lighter on the GPU.\n"
                      "Fast / Balanced / Best: BS-RoFormer SW (6 stems, piano and other instruments are part of the rest), "
                      "2.2 / 2.8 / 3.6 s late; Best is the cleanest.\n\n"
                      "The channel plays late so the model can hear what comes next: meant for playback you play or sing along to. "
                      "If the GPU is late, that part plays unseparated instead of a dropout.");
        fx.meterUsers.fetch_add (1);
        setSize (720, 2 * kPad + kModeCard + kGap + kStemsCard + kGap + kModelsCard);
        tick();
    }
    ~StemsPanel() override { fx.meterUsers.fetch_sub (1); }

    void resized() override
    {
        cards.clear();
        auto r = getLocalBounds().reduced (kPad);
        const auto top = r.removeFromTop (kModeCard);
        auto modeArea = card (top, "Latency mode");
        on.setBounds (captionSlot (top, 44));
        info.setBounds (captionSlot (top, 20, 52).withSizeKeepingCentre (InfoButton::kSize, InfoButton::kSize));
        mode.setBounds (modeArea.removeFromTop (30));
        modeArea.removeFromTop (6);
        gpu.setBounds (modeArea.removeFromRight (240).withSizeKeepingCentre (240, 18));
        latency.setBounds (modeArea);
        r.removeFromTop (kGap);

        auto mix = card (r.removeFromTop (kStemsCard), "Stems");
        mixBox.setBounds (mix);
        loading.setBounds (mix);
        mix = mixBox.getLocalBounds();
        mix.removeFromTop (16);   // names (painted)
        const int n = stems::numStems, colW = mix.getWidth() / n;
        names.clear();
        for (int k = 0; k < n; ++k)
        {
            auto col = juce::Rectangle<int> (mix.getX() + k * colW, mix.getY(), colW, mix.getHeight()).reduced (8, 0);
            names.push_back (col.withY (col.getY() - 16).withHeight (16));
            switches[(size_t) k]->setBounds (col.removeFromBottom (22).withSizeKeepingCentre (juce::jmin (col.getWidth(), 64), 22));
            col.removeFromBottom (6);
            auto f = col.withSizeKeepingCentre (juce::jmin (col.getWidth(), 70), col.getHeight());
            meters[(size_t) k]->setBounds (f.removeFromRight (6).reduced (0, 8));
            faders[(size_t) k]->setBounds (f);
        }
        r.removeFromTop (kGap);

        auto models = card (r, "Models");
        modelRows.clear();
        gpuRows.clear();
        for (int m = 0; m < stems::kNumModels; ++m)
        {
            auto row = models.removeFromTop (kModelRow);
            auto gpuRow = row.removeFromBottom (26);
            auto buttons = row.removeFromRight (190);
            deleteButtons[(size_t) m]->setBounds (buttons.removeFromRight (28).withSizeKeepingCentre (28, 26));
            buttons.removeFromRight (6);
            modelButtons[(size_t) m]->setBounds (buttons.withSizeKeepingCentre (buttons.getWidth(), 26));
            modelRows.push_back (row);
            gpuRow.removeFromLeft (34);   // "GPU" (painted)
            autoToggles[(size_t) m]->setBounds (gpuRow.removeFromLeft (52).withSizeKeepingCentre (52, 22));
            gpuRow.removeFromLeft (8);
            shareSliders[(size_t) m]->setBounds (gpuRow.removeFromLeft (300));
            gpuRows.push_back (gpuRow.withTrimmedLeft (12));   // resulting latency (painted)
        }
    }

    void paintNames (juce::Graphics& g)
    {
        g.setFont (theme::font (11.0f, true));
        const bool guitar = stems::hasGuitar (juce::roundToInt (fx.mode->get()));
        for (int k = 0; k < (int) names.size(); ++k)
        {
            g.setColour (theme::stemColour (k).withAlpha (k == stems::guitar && ! guitar ? 0.35f : 1.0f));
            g.drawText (kStemLabels[k], names[(size_t) k], juce::Justification::centred);
        }
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        for (int m = 0; m < (int) modelRows.size(); ++m)
        {
            const auto model = (stems::Model) m;
            const auto& inf = stems::modelInfo (model);
            const auto& st = status[(size_t) m];
            auto r = modelRows[(size_t) m].reduced (0, 4);
            g.setColour (theme::text);
            g.setFont (theme::font (12.0f, true));
            g.drawText (juce::String (inf.name) + (m == (int) stems::Model::demucs ? "  -  Live" : "  -  Fast, Balanced, Best")
                        + "   (" + juce::String (inf.bytes / 1000000) + " MB)", r.removeFromTop (17), juce::Justification::centredLeft);
            g.setFont (theme::font (11.0f));
            g.setColour (theme::textFaint);
            g.drawFittedText (inf.license, r.removeFromTop (15), juce::Justification::centredLeft, 1, 0.9f);
            auto problem = stemsProblem (st, model);
            if (st.state == stems::Service::State::noModel)
                problem = stems::Service::modelFile (model).existsAsFile() ? "Downloaded: loads onto the GPU when a channel uses this mode"
                                                                           : "Not downloaded yet";
            g.setColour (st.state == stems::Service::State::failed ? theme::warn : theme::textDim);
            g.drawFittedText (problem.isNotEmpty() ? problem.replace (": click here", "")
                                                   : (st.device.isNotEmpty() ? st.device : juce::String ("GPU")) + ": "
                                                     + juce::String (juce::roundToInt (st.inferenceMs)) + " ms per "
                                                     + juce::String (inf.samples / 44100.0, 1) + "-s piece",
                              r.removeFromTop (15), juce::Justification::centredLeft, 1, 0.9f);
            // GPU setting: what latency it gives in the model's modes.
            if (juce::isPositiveAndBelow (m, (int) gpuRows.size()))
            {
                const auto gr = gpuRows[(size_t) m];
                g.setColour (theme::textDim);
                g.drawText ("GPU", gr.withX (modelRows[(size_t) m].getX()).withWidth (34), juce::Justification::centredLeft);
                const auto tuning = ctx.engine.stemsTuning (model);
                const double ref = stems::Service::referenceMs (model);
                juce::StringArray lat;
                for (int mode : stems::modeOrder())
                    if (stems::modelFor (mode) == model)
                        lat.add (stems::modeNames()[mode] + " " + juce::String (stems::timingFor (mode, tuning, ref).latency(), 2) + " s");
                g.setColour (tuning.automatic ? theme::textFaint : theme::text);
                g.drawFittedText ("Latency: " + lat.joinIntoString (", "), gr, juce::Justification::centredLeft, 1, 0.85f);
            }
        }
    }

private:
    static constexpr int kModeCard = kCaption + 30 + 6 + 40 + 8;
    static constexpr int kStemsCard = kCaption + 16 + 230 + 8;
    static constexpr int kModelRow = 54 + 30;
    static constexpr int kModelsCard = kCaption + 2 * kModelRow + 8;

    void tick() override
    {
        updateStemsInfo (ctx.engine, strip, latency, gpu, misses);
        auto& svc = stems::Service::get();
        using S = stems::Service::State;
        juce::String key;
        for (int m = 0; m < stems::kNumModels; ++m)
        {
            const auto model = (stems::Model) m;
            status[(size_t) m] = svc.getStatus (model);
            const auto& st = status[(size_t) m];
            const bool busy = st.state == S::downloading || st.state == S::preparing;
            const bool have = stems::Service::modelFile (model).existsAsFile();
            auto& b = *modelButtons[(size_t) m];
            const bool failed = st.state == S::failed;
            const juce::String text = busy ? "Cancel download" : have ? "Download again" : "Download";
            if (b.text != text) { b.text = text; b.repaint(); }
            b.setVisible (st.state != S::unavailable && (busy || ! have || failed));
            deleteButtons[(size_t) m]->setVisible (have && ! busy);
            key << stemsProblem (st, model) << st.device << juce::roundToInt (st.inferenceMs) << (int) have
                << ctx.engine.stemsGpu[(size_t) m]->getVersion() << ctx.engine.stemsAuto[(size_t) m]->getVersion();
        }
        const bool guitar = stems::hasGuitar (juce::roundToInt (fx.mode->get()));
        const float a = guitar ? 1.0f : 0.35f;
        faders[stems::guitar]->setAlpha (a);
        switches[stems::guitar]->setAlpha (a);
        faders[stems::guitar]->setTooltip (guitar ? juce::String() : kNoGuitarTip);
        key << (int) guitar;
        if (key != shownKey) { shownKey = key; repaint(); mixBox.repaint(); }
        const auto cover = stemsCover (strip);
        loading.set (cover.spin, cover.line);
        loading.setVisible (cover.show);
        mixBox.setBlurred (cover.show);
    }

    UiContext& ctx;
    ChannelFx& strip;
    StemsFx& fx;
    ChoiceSegments mode;
    Toggle on;
    InfoButton info;
    StemsLatency latency;
    GpuBar gpu;
    BlurBox mixBox;
    StemsLoading loading;
    std::vector<std::unique_ptr<Fader>> faders;
    std::vector<std::unique_ptr<StemMeter>> meters;
    std::vector<std::unique_ptr<StemSwitches>> switches;
    std::vector<std::unique_ptr<Chip>> modelButtons;
    std::vector<std::unique_ptr<TrashButton>> deleteButtons;
    std::vector<std::unique_ptr<Toggle>> autoToggles;
    std::vector<std::unique_ptr<ShareSlider>> shareSliders;
    std::vector<juce::Rectangle<int>> names, modelRows, gpuRows;
    MissWindow misses;
    std::array<stems::Service::Status, stems::kNumModels> status;
    juce::String shownKey;
};

/** Lights up the arc of each knob of the block in its stem's colour with the stem's level (after gain, mute and solo).
    Smooth: fast rise (~25 ms), slow fall (~0.3 s); a knob repaints only when its glow changed. No painting itself. */
class StemGlow final : public juce::Component
{
public:
    explicit StemGlow (StemsFx& f) : fx (f)
    {
        setInterceptsMouseClicks (false, false);
        fx.meterUsers.fetch_add (1);
    }
    ~StemGlow() override { fx.meterUsers.fetch_sub (1); }
    void setKnobs (std::vector<Knob*> k) { knobs = std::move (k); for (auto* kn : knobs) kn->setGlow (0.0f); }

private:
    void frame (double t)
    {
        const float dt = last > 0.0 ? (float) juce::jlimit (0.0, 0.1, t - last) : 1.0f / 60.0f;
        last = t;
        float gains[stems::numStems];
        fx.gains (gains);
        for (int k = 0; k < stems::numStems && k < (int) knobs.size(); ++k)
        {
            const float pk = fx.takePeak (k, StemsFx::block) * gains[k];
            // Brightness over -42 .. -6 dB (music sits there), a little curved so quiet parts still show.
            const float target = pk > 1.0e-5f ? std::sqrt (juce::jlimit (0.0f, 1.0f, (gainToDb (pk) + 42.0f) / 36.0f)) : 0.0f;
            float& v = level[(size_t) k];
            v += (target - v) * (1.0f - std::exp (-dt / (target > v ? 0.025f : 0.3f)));
            knobs[(size_t) k]->setGlow (v < 0.01f ? 0.0f : v);
        }
    }
    StemsFx& fx;
    std::vector<Knob*> knobs;
    std::array<float, stems::numStems> level {};
    double last = 0.0;
    juce::VBlankAttachment vblank { this, [this] (double t) { frame (t); } };
};

/** The Stems block: a level and mute / solo per stem, the latency (large) and the GPU load. */
class StemsUnit final : public juce::Component
{
public:
    StemsUnit (UiContext& c, ChannelFx& s, const juce::String& title)
        : ctx (c), strip (s), fx (s.stems), section (title, theme::fxStems), on (c, *fx.on, "ON", theme::fxStems), glow (s.stems)
    {
        addAndMakeVisible (section);
        section.addAndMakeVisible (glow);
        section.addAndMakeVisible (on);
        section.addAndMakeVisible (knobBox);
        section.addChildComponent (loading);
        std::vector<Knob*> lit;
        for (int k = 0; k < stems::numStems; ++k)
        {
            auto& knob = knobs.emplace_back (std::make_unique<Knob> (c, *fx.gain[(size_t) k], kStemLabels[k], theme::stemColour (k)));
            auto& sw = switches.emplace_back (std::make_unique<StemSwitches> (c, fx, k));
            knobBox.addAndMakeVisible (*knob);
            sw->addTo (knobBox);
            lit.push_back (knob.get());
        }
        glow.setKnobs (std::move (lit));
        for (juce::Component* k : std::initializer_list<juce::Component*> { &latency, &gpu, &statusChip }) section.addChildComponent (k);
        latency.setInterceptsMouseClicks (true, false);
        statusChip.showArrow = false;
        statusChip.onClick = [this] { openPanel(); };
        statusChip.setTooltip ("Models and latency mode");
        section.enableGear ([this] { openPanel(); });
        tick (true);
    }
    Section& getSection() noexcept { return section; }

    void resized() override
    {
        section.setBounds (getLocalBounds());
        on.setBounds (section.headerSlot());
        auto cc = section.content();
        // Knobs + switches take what they need (~100 px); the rest goes to the latency / GPU line (up to two lines).
        auto info = cc.removeFromBottom (juce::jlimit (22, 40, cc.getHeight() - 100));
        statusChip.setBounds (info.withSizeKeepingCentre (info.getWidth(), juce::jmin (info.getHeight(), 26)));
        // Top: latency (large) on the left, the GPU bar fills the rest; below: the mode / warning line, full width.
        latency.setBounds (info);
        const int big = info.getHeight() >= 30 ? info.getHeight() * 3 / 5 : info.getHeight();
        gpu.setBounds (info.withHeight (big).withTrimmedLeft (70).withSizeKeepingCentre (info.getWidth() - 70, 18));
        cc.removeFromBottom (4);
        const int n = stems::numStems, gap = 4;
        const int w = (cc.getWidth() - gap * (n - 1)) / n;
        auto row = cc.removeFromBottom (18);
        cc.removeFromBottom (2);
        const int knobH = juce::jmin (cc.getHeight(), 76);
        const auto box = cc.withTop (cc.getBottom() - knobH).getUnion (row);
        knobBox.setBounds (box);
        loading.setBounds (box);
        for (int k = 0; k < n; ++k)
        {
            const int x = cc.getX() + k * (w + gap) - box.getX();
            knobs[(size_t) k]->setBounds ({ x, cc.getBottom() - knobH - box.getY(), w, knobH });
            switches[(size_t) k]->setBounds ({ x, row.getY() - box.getY(), w, row.getHeight() });
        }
        glow.setBounds ({});
    }

    /** Called with the rack's meters (~60 Hz); 6 Hz is enough here. */
    void tick (bool now = false)
    {
        if (! now && ++ticks % 10 != 0) return;
        const int mode = juce::roundToInt (fx.mode->get());
        const auto model = stems::modelFor (mode);
        const auto st = stems::Service::get().getStatus (model);
        const auto problem = stemsProblem (st, model);
        const bool running = problem.isEmpty();
        statusChip.setVisible (! running);
        latency.setVisible (running);
        gpu.setVisible (running);
        if (! running)
        {
            const auto colour = st.state == stems::Service::State::failed ? theme::warn : theme::fxStems;
            const auto text = (mode == stems::kLiveMode ? "Live: " : "") + problem;
            if (text != statusChip.text || colour != statusChip.colour)
            {
                statusChip.text = text;
                statusChip.colour = colour;
                statusChip.repaint();
            }
        }
        else updateStemsInfo (ctx.engine, strip, latency, gpu, misses);
        const auto cover = stemsCover (strip);
        loading.set (cover.spin, running ? cover.line : juce::String());
        loading.setVisible (cover.show);
        knobBox.setBlurred (cover.show);
        const bool guitar = stems::hasGuitar (mode);
        const float a = guitar ? 1.0f : 0.35f;
        if (knobs[stems::guitar]->getAlpha() != a)
        {
            knobs[stems::guitar]->setAlpha (a);
            switches[stems::guitar]->setAlpha (a);
            knobs[stems::guitar]->setTooltip (guitar ? juce::String() : kNoGuitarTip);
        }
    }

private:
    void openPanel() { ctx.openPanel (std::make_unique<StemsPanel> (ctx, strip), section.title + "  -  " + nameOf (ctx.engine, strip)); }

    UiContext& ctx;
    ChannelFx& strip;
    StemsFx& fx;
    Section section;
    Toggle on;
    StemGlow glow;
    BlurBox knobBox;
    StemsLoading loading;
    std::vector<std::unique_ptr<Knob>> knobs;
    std::vector<std::unique_ptr<StemSwitches>> switches;
    StemsLatency latency;
    GpuBar gpu;
    Chip statusChip { "", theme::fxStems };
    MissWindow misses;
    int ticks = 0;
};

class CompUnit final : public juce::Component
{
public:
    /** channelName: the input's or output's name (it can be renamed), for the settings window's title. */
    CompUnit (UiContext& c, CompFx& f, std::function<juce::String()> channelName, const juce::String& title)
        : ctx (c), fx (f), section (title, theme::fxComp),
          on (c, *fx.on, "ON", theme::fxComp), thr (c, *fx.thr, "THRESH", theme::fxComp),
          ratio (c, *fx.ratio, "RATIO", theme::fxComp),
          gr ([this] { return fx.gr.load() / 24.0f; }, theme::fxComp, "GR")
    {
        addAndMakeVisible (section);
        for (juce::Component* k2 : std::initializer_list<juce::Component*> { &on, &thr, &ratio, &gr }) section.addAndMakeVisible (k2);
        section.enableGear ([this, title, channelName] { ctx.openPanel (std::make_unique<CompPanel> (ctx, fx), title + "  -  " + channelName()); });
    }
    void resized() override
    {
        section.setBounds (getLocalBounds());
        on.setBounds (section.headerSlot());
        auto cc = section.content();
        gr.setBounds (cc.removeFromBottom (12));
        rowLayout (cc.withSizeKeepingCentre (cc.getWidth(), 70), { &thr, &ratio });
    }

    UiContext& ctx;
    CompFx& fx;
    Section section;
    Toggle on;
    Knob thr, ratio;
    LiveBar gr;
};

class LimiterUnit final : public juce::Component
{
public:
    LimiterUnit (UiContext& c, LimiterFx& l, const juce::String& title)
        : section (title, theme::fxLimiter), on (c, *l.on, "ON", theme::fxLimiter), ceiling (c, *l.ceiling, "CEILING", theme::fxLimiter)
    {
        addAndMakeVisible (section);
        section.addAndMakeVisible (on);
        section.addAndMakeVisible (ceiling);
    }
    void resized() override
    {
        section.setBounds (getLocalBounds());
        on.setBounds (section.headerSlot());
        ceiling.setBounds (section.content().withSizeKeepingCentre (76, 70));
    }

    Section section;
    Toggle on;
    Knob ceiling;
};

constexpr const char* kPhaseTip = "Phase: flips the polarity of the channel (both sides together)";

/** All Utility settings (like Ableton's Utility). The on switch (top right) is for the whole effect. */
class UtilityPanel final : public AdvancedPanel
{
public:
    UtilityPanel (UiContext& c, UtilityFx& u)
        : phase (c, *u.phase, {}, theme::fxUtility, Toggle::Icon::phase),
          channel (c, *u.channel, theme::fxUtility),
          width (c, *u.width, "WIDTH", theme::fxUtility), mono (c, *u.mono, "MONO", theme::fxUtility),
          bassFreq (c, *u.bassFreq, "FREQUENCY", theme::fxUtility), bass (c, *u.bass, "ON", theme::fxUtility),
          balance (c, *u.balance, "BALANCE", theme::fxUtility), gain (c, *u.gain, "GAIN", theme::fxUtility),
          dc (c, *u.dc, "DC", theme::fxUtility), on (c, *u.on, "ON", theme::fxUtility)
    {
        showValues ({ &width, &bassFreq, &balance, &gain });
        for (juce::Component* k : std::initializer_list<juce::Component*> { &phase, &channel, &width, &mono, &bassFreq, &bass,
                                                                           &balance, &gain, &dc, &on, &info })
            addAndMakeVisible (k);
        phase.setTooltip (kPhaseTip);
        channel.setTooltip ("Left / Right: only that channel, on both sides. Swap: left and right change places");
        mono.setTooltip ("Both channels summed to mono (width 0 %)");
        bass.setTooltip ("Bass Mono: everything below the frequency is mono, the highs keep their width");
        dc.setTooltip ("Removes DC offset (a high pass at 5 Hz)");
        on.setTooltip ("Switch the whole Utility on or off");
        info.setText ("Utility\nPhase, channel, width and level of the channel, like Ableton's Utility. No latency, "
                      "and every change is smoothed (no clicks).\n\n"
                      "Width works on mid (what both sides share) and side (the difference): 0 % = mono, 100 % = unchanged, "
                      "up to 400 % = wider. Bass Mono keeps the low end in the middle (e.g. for vinyl or club playback).");
        setSize (700, 2 * kPad + 2 * kCardHeight + kGap);
    }

    void resized() override
    {
        cards.clear();
        auto r = getLocalBounds().reduced (kPad);
        auto top = r.removeFromTop (kCardHeight);
        const auto pc = top.removeFromLeft (390);
        {
            auto a = card (pc, "Phase and channel").withSizeKeepingCentre (pc.getWidth() - 20, 30);
            phase.setBounds (a.removeFromLeft (56));
            a.removeFromLeft (10);
            channel.setBounds (a);
        }
        top.removeFromLeft (kGap);
        width.setBounds (card (top, "Width").withSizeKeepingCentre (84, kKnobs));
        // Top right corner: the switch of the whole effect, the info left of it.
        on.setBounds (captionSlot (top, 44));
        info.setBounds (captionSlot (top, 20, 52).withSizeKeepingCentre (InfoButton::kSize, InfoButton::kSize));
        mono.setBounds (captionSlot (top, 52, 80));
        r.removeFromTop (kGap);

        const auto bm = r.removeFromLeft (200);
        bassFreq.setBounds (card (bm, "Bass Mono").withSizeKeepingCentre (84, kKnobs));
        bass.setBounds (captionSlot (bm, 44));
        r.removeFromLeft (kGap);
        const auto out = r;
        rowLayout (card (out, "Output").withSizeKeepingCentre (200, kKnobs), { &balance, &gain });
        dc.setBounds (captionSlot (out, 44));
    }

private:
    Toggle phase;
    ChoiceSegments channel;
    Knob width;
    Toggle mono;
    Knob bassFreq;
    Toggle bass;
    Knob balance, gain;
    Toggle dc, on;
    InfoButton info;
};

/** Utility block: phase, mono, channel mode, width and gain; everything else in its window. */
class UtilityUnit final : public juce::Component
{
public:
    UtilityUnit (UiContext& c, UtilityFx& u, std::function<juce::String()> channelName, const juce::String& title)
        : ctx (c), fx (u), section (title, theme::fxUtility), on (c, *u.on, "ON", theme::fxUtility),
          phase (c, *u.phase, {}, theme::fxUtility, Toggle::Icon::phase), mono (c, *u.mono, "MONO", theme::fxUtility),
          channel (c, *u.channel, theme::fxUtility, { "L", "ST", "R", "SWAP" }),
          width (c, *u.width, "WIDTH", theme::fxUtility), gain (c, *u.gain, "GAIN", theme::fxUtility)
    {
        addAndMakeVisible (section);
        for (juce::Component* k : std::initializer_list<juce::Component*> { &on, &phase, &mono, &channel, &width, &gain })
            section.addAndMakeVisible (k);
        phase.setTooltip (kPhaseTip);
        mono.setTooltip ("Sum to mono");
        channel.setTooltip ("Left only / stereo / right only / left and right swapped (L and R: that channel on both sides)");
        section.enableGear ([this, title, channelName] { ctx.openPanel (std::make_unique<UtilityPanel> (ctx, fx), title + "  -  " + channelName()); });
    }
    Section& getSection() noexcept { return section; }

    void resized() override
    {
        section.setBounds (getLocalBounds());
        on.setBounds (section.headerSlot());
        auto cc = section.content();
        auto row = cc.removeFromTop (20);
        phase.setBounds (row.removeFromLeft (row.getWidth() / 2 - 2));
        row.removeFromLeft (4);
        mono.setBounds (row);
        cc.removeFromTop (5);
        channel.setBounds (cc.removeFromTop (20));
        cc.removeFromTop (2);
        rowLayout (cc.withSizeKeepingCentre (cc.getWidth(), juce::jmin (cc.getHeight(), 70)), { &width, &gain });
    }

private:
    UiContext& ctx;
    UtilityFx& fx;
    Section section;
    Toggle on, phase, mono;
    ChoiceSegments channel;
    Knob width, gain;
};

//==============================================================================
/** What Transpose's listener is doing / found, for the block and the window. */
struct TransposeStatus
{
    juce::String text;
    juce::Colour colour;
    bool busy = false;
};

TransposeStatus transposeStatus (const TransposeFx& t, bool longText)
{
    using S = TransposeFx::State;
    const auto st = t.getState();
    if (st == S::listening)
    {
        const int pct = juce::roundToInt (t.getProgress() * 100.0f);
        const juce::String what = t.autoTune->isOn() ? "Auto Tune: listening " : t.isAddingPart() ? "Adding a part: listening " : "Listening ";
        return { what + juce::String (pct) + " %", theme::fxTranspose, true };
    }
    if (st == S::analysing) return { "Analysing...", theme::fxTranspose, true };
    if (st == S::noSignal)
        return { longText ? "Heard too little music: play the song and try again" : "No music heard", theme::warn, false };
    const int key = juce::roundToInt (t.key->get()) - 1;
    if (key < 0) return { longText ? "Key not detected yet: press Find Key while the song plays" : "Key: -", theme::textFaint, false };
    const int semi = juce::roundToInt (t.semi->get());
    const float found = t.cents->get();
    auto text = "Key: " + dsp::keyName (key, semi) + "  " + juce::String (juce::roundToInt (t.prob->get() * 100.0f)) + " %";
    if (! longText) text << "  " << (found >= 0.0f ? "+" : "-") << juce::String (std::abs (found), 0) << " ct";
    return { text, theme::textDim, false };
}

/** The key found by Find Key / Add Part / Auto Tune: a box with the most likely key (after the semitone shift) and
    its probability, a box with how far the song is off A = 440 Hz, and the notes of the first and the second
    choice with their probabilities; notes heard while listening are filled, notes heard outside the first choice
    are listed below. Repaints only when something it shows changed. */
class KeyDisplay final : public juce::Component
{
public:
    explicit KeyDisplay (TransposeFx& t) : fx (t) {}

    /** Called ~30 Hz by the window. */
    void update()
    {
        juce::String sig;
        sig << fx.key->get() << "|" << fx.key2->get() << "|" << fx.prob->get() << "|" << fx.prob2->get() << "|" << fx.parts->get()
            << "|" << fx.semi->get() << "|" << fx.cents->get() << "|" << fx.fine->get() << "|" << (int) fx.on->isOn();
        for (auto* h : fx.heard) sig << "|" << h->get();
        if (sig != shownSig) { shownSig = sig; repaint(); }
    }

    void paint (juce::Graphics& g) override
    {
        const auto accent = theme::fxTranspose;
        const int key = juce::roundToInt (fx.key->get()) - 1;
        const int key2 = juce::roundToInt (fx.key2->get()) - 1;
        const int semi = juce::roundToInt (fx.semi->get());
        const bool known = key >= 0;
        auto r = getLocalBounds().toFloat();
        const float corner = theme::deep() ? 4.0f : 8.0f;
        auto box = [&] (juce::Rectangle<float> b, bool lit)
        {
            g.setColour (lit ? theme::bg.interpolatedWith (accent, 0.10f) : theme::bg.withAlpha (0.6f));
            g.fillRoundedRectangle (b, corner);
            g.setColour (lit ? accent.withAlpha (0.55f) : theme::cardLine);
            g.drawRoundedRectangle (b.reduced (0.5f), corner, 1.0f);
        };

        // Key box: the most likely key and how likely it is.
        auto keyBox = r.removeFromLeft (r.getHeight() * 0.95f);
        box (keyBox, known);
        {
            auto k = keyBox.reduced (6.0f, 8.0f);
            g.setColour (known ? theme::text : theme::textFaint);
            g.setFont (theme::font (k.getHeight() * 0.36f, true));
            g.drawText (known ? juce::String (noteName ((key % 12) + semi)) : juce::String ("?"), k.removeFromTop (k.getHeight() * 0.52f), juce::Justification::centredBottom);
            g.setColour (known ? accent : theme::textFaint);
            g.setFont (theme::font (12.0f, true));
            g.drawText (known ? (key >= 12 ? "MINOR" : "MAJOR") : "KEY", k.removeFromTop (17.0f), juce::Justification::centred);
            if (known)
            {
                g.setColour (theme::text);
                g.setFont (theme::font (13.0f, true));
                g.drawText (percent (fx.prob->get()), k.removeFromTop (18.0f), juce::Justification::centred);
                if (semi != 0)
                {
                    g.setColour (theme::textFaint);
                    g.setFont (theme::font (10.0f));
                    g.drawText ("song: " + dsp::keyName (key), k, juce::Justification::centred);
                }
            }
        }
        r.removeFromLeft (8.0f);

        // Offset box: how far the song is off A = 440 Hz, whether Fine corrects it; the Auto Finetune button at
        // its bottom belongs to the window (autoTuneArea).
        auto off = r.removeFromLeft (offsetWidth());
        box (off, false);
        {
            auto o = off.reduced (6.0f, 8.0f);
            o.removeFromBottom ((float) kButtonHeight + 4.0f);
            const float c = fx.cents->get();
            const bool tuned = known && fx.on->isOn() && std::abs (fx.fine->get() + c) < 0.5f && std::abs (c) >= 0.5f;
            g.setColour (known ? theme::text : theme::textFaint);
            g.setFont (theme::font (o.getHeight() * 0.36f, true));
            g.drawText (known ? (c >= 0.0f ? "+" : "-") + juce::String (std::abs (c), std::abs (c) < 10.0f ? 1 : 0) : juce::String ("-"),
                        o.removeFromTop (o.getHeight() * 0.55f), juce::Justification::centredBottom);
            g.setColour (tuned ? theme::good : theme::textFaint);
            g.setFont (theme::font (11.0f, tuned));
            g.drawText (tuned ? "ct, TUNED" : known ? (std::abs (c) < 0.5f ? "ct, in tune" : c > 0.0f ? "ct sharp" : "ct flat") : "ct",
                        o.removeFromTop (16.0f), juce::Justification::centred);
        }
        r.removeFromLeft (14.0f);

        if (! known)
        {
            g.setColour (theme::textFaint);
            g.setFont (theme::font (12.0f));
            g.drawFittedText ("Play the song and press Find Key: the key, how sure it is, a second choice and how far the song is off "
                              "A = 440 Hz show here.", r.toNearestInt(), juce::Justification::centredLeft, 3, 1.0f);
            return;
        }

        // First and second choice: name + probability, then their 7 notes (filled = heard).
        const float rowH = juce::jmin (36.0f, (r.getHeight() - 16.0f) / 2.0f - 4.0f);
        auto row1 = r.removeFromTop (rowH);
        r.removeFromTop (8.0f);
        auto row2 = r.removeFromTop (rowH);
        drawChoice (g, row1, key, fx.prob->get(), semi, true);
        if (key2 >= 0) drawChoice (g, row2, key2, fx.prob2->get(), semi, false);

        // Heard outside the first choice, and how many parts of the song were heard.
        bool inKey[12] {};
        for (int pc : dsp::scaleOf (key)) inKey[pc] = true;
        juce::StringArray outside;
        for (int pc = 0; pc < 12; ++pc)
            if (! inKey[pc] && fx.heard[(size_t) pc]->get() >= kHeardMin) outside.add (noteName (pc + semi));
        const int parts = juce::roundToInt (fx.parts->get());
        juce::String line = "Filled = heard";
        if (outside.size() > 0) line << "   ·   also heard: " << outside.joinIntoString (", ");
        if (parts > 0) line << "   ·   " << parts << (parts == 1 ? " part of the song" : " parts of the song");
        g.setColour (theme::textFaint);
        g.setFont (theme::font (11.0f));
        g.drawFittedText (line, r.toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
    }

    /** Where the window puts its Auto Finetune button: the bottom of the offset box. */
    juce::Rectangle<int> autoTuneArea() const
    {
        const auto r = getLocalBounds();
        const int x = (int) (r.getHeight() * 0.95f) + 8;
        return juce::Rectangle<int> (x, 0, offsetWidth(), r.getHeight()).reduced (7, 8).removeFromBottom (kButtonHeight);
    }

private:
    static constexpr float kHeardMin = 0.25f;   // share of the loudest pitch class that counts as heard
    static constexpr int kButtonHeight = 26;
    float offsetWidth() const { return juce::jmax ((float) getHeight() * 0.8f, 128.0f); }

    static const char* noteName (int pc)
    {
        static const char* names[] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
        return names[(pc % 12 + 12) % 12];
    }
    static juce::String percent (float p) { return juce::String (juce::roundToInt (juce::jlimit (0.0f, 1.0f, p) * 100.0f)) + " %"; }

    void drawChoice (juce::Graphics& g, juce::Rectangle<float> row, int key, float p, int semi, bool first) const
    {
        const auto accent = theme::fxTranspose;
        auto label = row.removeFromLeft (118.0f);
        g.setColour (first ? theme::text : theme::textDim);
        g.setFont (theme::font (12.0f, true));
        g.drawText (dsp::keyName (key, semi), label.removeFromTop (label.getHeight() * 0.55f), juce::Justification::bottomLeft);
        g.setColour (first ? accent : theme::textFaint);
        g.setFont (theme::font (11.0f));
        g.drawText ((first ? "1st  " : "2nd  ") + percent (p), label, juce::Justification::topLeft);
        const float gap = 5.0f, w = juce::jmin (40.0f, (row.getWidth() - 6 * gap) / 7.0f);
        const auto scale = dsp::scaleOf (key);
        for (int i = 0; i < 7; ++i)
        {
            const int pc = scale[(size_t) i];
            const float h = fx.heard[(size_t) pc]->get();
            const bool found = h >= kHeardMin;
            auto chip = juce::Rectangle<float> (row.getX() + i * (w + gap), row.getY(), w, row.getHeight());
            const float strength = first ? 1.0f : 0.6f;
            g.setColour (found ? accent.withAlpha ((0.2f + 0.6f * juce::jlimit (0.0f, 1.0f, h)) * strength) : theme::bg.withAlpha (0.6f));
            g.fillRoundedRectangle (chip, 5.0f);
            g.setColour (found ? accent.withAlpha (strength) : theme::cardLine);
            g.drawRoundedRectangle (chip.reduced (0.5f), 5.0f, 1.0f);
            g.setColour (found ? theme::text : theme::textFaint);
            g.setFont (theme::font (12.0f, i == 0));
            g.drawText (noteName (pc + semi), chip, juce::Justification::centred);
        }
    }

    TransposeFx& fx;
    juce::String shownSig;
};

/** All Transpose settings: pitch, quality (with the on switch top right) and the key found. */
class TransposePanel final : public AdvancedPanel
{
public:
    TransposePanel (UiContext& c, TransposeFx& t)
        : fx (t), rate (c.engine.getSampleRate()), semi (c, *t.semi, "SEMITONES", theme::fxTranspose), fine (c, *t.fine, "FINE", theme::fxTranspose),
          on (c, *t.on, "ON", theme::fxTranspose),
          findKey (c, *t.findKey, "FIND KEY", theme::fxTranspose), addPart (c, *t.addPart, "ADD PART", theme::fxTranspose),
          autoTune (c, *t.autoTune, "AUTO FINETUNE", theme::fxTranspose),
          quality (c, *t.quality, theme::fxTranspose), timbre (c, *t.timbre, "KEEP TIMBRE", theme::fxTranspose), keys (t)
    {
        showValues ({ &semi, &fine });
        for (juce::Component* k : std::initializer_list<juce::Component*> { &semi, &fine, &on, &findKey, &addPart, &quality, &timbre, &keys, &autoTune, &info })
            addAndMakeVisible (k);   // the Auto Finetune button lies on the key display
        on.setTooltip ("Switch Transpose on or off");
        findKey.setTooltip ("Listens to the song for about 10 s: its key, a second choice, how sure it is and how far it is off A = 440 Hz. Fine stays as it is");
        addPart.setTooltip ("Listens to another part of the song (e.g. the chorus) and combines it with what was heard: a more reliable key");
        autoTune.setTooltip ("Sets Fine so the song is in tune with A = 440 Hz (uses what Find Key / Add Part heard)");
        updateButtons();
        quality.setTooltip ("Shorter = less latency, longer = cleaner for chords and full mixes (the latency of each is shown below). "
                            "Latency only while shifting");
        timbre.setTooltip ("Keeps the tone colour of the original (its spectral envelope) while the pitch moves: up no longer sounds thin, "
                           "down no longer muffled. Works best with Normal and High (Fast: only for small shifts). A little more CPU");
        info.setText ("Transpose\nShifts the pitch in semitones (+-24) and cents (+-50) without changing the tempo. "
                      "Voices sound a little brighter / darker, like on a sampler; Keep timbre keeps the tone colour of the original.\n\n"
                      "Pitch shifting by Signalsmith Stretch (MIT licence, Signalsmith Audio).\n\n"
                      "At 0 st and 0 ct the sound passes straight through: no latency. While it shifts, the latency of the quality applies.\n\n"
                      "Find Key listens once (about 10 s of music, pauses do not count) and then stops. Add Part listens to another part "
                      "and combines both. The percentages say how sure it is, based on the notes heard. Auto Tune uses the key found. "
                      "The keys shown already include the semitone shift.");
        setSize (720, 2 * kPad + kCardHeight + kGap + kKeyCard);
    }

    void resized() override
    {
        cards.clear();
        auto r = getLocalBounds().reduced (kPad);
        auto top = r.removeFromTop (kCardHeight);
        const auto pitch = top.removeFromLeft (230);
        rowLayout (card (pitch, "Pitch").withSizeKeepingCentre (200, kKnobs), { &semi, &fine });
        top.removeFromLeft (kGap);
        auto q = card (top, "Quality");
        on.setBounds (captionSlot (top, 44));   // top right corner: the whole effect
        info.setBounds (captionSlot (top, 20, 52).withSizeKeepingCentre (InfoButton::kSize, InfoButton::kSize));
        quality.setBounds (q.removeFromTop (30));
        q.removeFromTop (8);
        timbre.setBounds (q.removeFromTop (26).withWidth (130));
        latencyArea = q;
        r.removeFromTop (kGap);

        auto k = card (r, "Key");
        auto buttons = k.removeFromLeft (130);
        auto pair = buttons.withSizeKeepingCentre (buttons.getWidth(), 2 * 28 + 8);   // vertically centred
        statusArea = buttons.withTop (pair.getBottom() + 4);                          // "Listening 40 %" below them
        findKey.setBounds (pair.removeFromTop (28));
        pair.removeFromTop (8);
        addPart.setBounds (pair.removeFromTop (28));
        k.removeFromLeft (14);
        keys.setBounds (k);
        autoTune.setBounds (keys.autoTuneArea() + keys.getPosition());
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        g.setColour (theme::textDim);
        g.setFont (theme::font (11.5f));
        const int qn = juce::roundToInt (fx.quality->get());
        const auto* ps = fx.live.load();
        const int samples = ps != nullptr ? ps->latencyFor (qn) : dsp::PitchShifter::latencyAt (rate, qn);
        const bool shifting = fx.on->isOn() && fx.ratio() != 1.0;
        g.drawFittedText ("Latency while shifting: " + juce::String (juce::roundToInt (1000.0 * samples / rate)) + " ms"
                              + (shifting ? juce::String() : "\nNow: 0 ms (no shift)"),
                          latencyArea, juce::Justification::centredLeft, 2, 1.0f);
        const auto st = transposeStatus (fx, false);
        if (st.busy || fx.getState() == TransposeFx::State::noSignal)
        {
            g.setColour (st.colour);
            g.setFont (theme::font (11.0f));
            g.drawFittedText (st.text, statusArea, juce::Justification::topLeft, 3, 1.0f);
        }
    }

private:
    static constexpr int kKeyCard = kCaption + 134 + 8;

    /** Add Part and Auto Finetune need a key found; Find Key then finds a new one. */
    void updateButtons()
    {
        const bool known = juce::roundToInt (fx.key->get()) > 0;
        for (auto* b : { &addPart, &autoTune })
            if (b->isEnabled() != known) { b->setEnabled (known); b->setAlpha (known ? 1.0f : 0.35f); }
        const juce::String label = known ? "FIND NEW KEY" : "FIND KEY";
        if (findKey.text != label) { findKey.text = label; findKey.repaint(); }
    }

    void tick() override
    {
        keys.update();
        updateButtons();
        juce::String sig;
        sig << transposeStatus (fx, false).text << "|" << fx.quality->get() << "|" << (int) (fx.on->isOn() && fx.ratio() != 1.0);
        if (sig != shown) { shown = sig; repaint(); }
    }

    TransposeFx& fx;
    double rate;
    Knob semi, fine;
    Toggle on, findKey, addPart, autoTune;
    ChoiceSegments quality;
    Toggle timbre;
    KeyDisplay keys;
    InfoButton info;
    juce::Rectangle<int> statusArea, latencyArea;
    juce::String shown;
};

/** Transpose block: semitones, fine, Auto Tune / Find Key and the key found. */
class TransposeUnit final : public juce::Component
{
public:
    TransposeUnit (UiContext& c, ChannelFx& s, const juce::String& title)
        : ctx (c), strip (s), fx (s.transpose), section (title, theme::fxTranspose), on (c, *fx.on, "ON", theme::fxTranspose),
          semi (c, *fx.semi, "SEMI", theme::fxTranspose), fine (c, *fx.fine, "FINE", theme::fxTranspose),
          autoTune (c, *fx.autoTune, "AUTO TUNE", theme::fxTranspose), findKey (c, *fx.findKey, "FIND KEY", theme::fxTranspose)
    {
        addAndMakeVisible (section);
        for (juce::Component* k : std::initializer_list<juce::Component*> { &on, &semi, &fine, &autoTune, &findKey })
            section.addAndMakeVisible (k);
        semi.alwaysShowValue = fine.alwaysShowValue = true;
        autoTune.setTooltip ("Sets Fine so the song is in tune (needs a key found with Find Key)");
        findKey.setTooltip ("Listens once (~10 s) and shows the key and tuning (Fine stays)");
        section.enableGear ([this, title] { ctx.openPanel (std::make_unique<TransposePanel> (ctx, fx), title + "  -  " + nameOf (ctx.engine, strip)); });
    }
    Section& getSection() noexcept { return section; }

    void resized() override
    {
        section.setBounds (getLocalBounds());
        on.setBounds (section.headerSlot());
        auto cc = section.content();
        statusArea = cc.removeFromBottom (14);
        cc.removeFromBottom (4);
        auto row = cc.removeFromBottom (20);
        autoTune.setBounds (row.removeFromLeft (row.getWidth() / 2 - 2));
        row.removeFromLeft (4);
        findKey.setBounds (row);
        cc.removeFromBottom (2);
        rowLayout (cc.withSizeKeepingCentre (cc.getWidth(), juce::jmin (cc.getHeight(), 70)), { &semi, &fine });
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        const auto st = transposeStatus (fx, false);
        g.setColour (st.colour);
        g.setFont (theme::font (10.5f));
        g.drawFittedText (st.text, statusArea, juce::Justification::centred, 1, 0.8f);
    }

    /** With the rack's meters (~60 Hz); the line is checked at 10 Hz. */
    void tick()
    {
        if (++ticks % 6 != 0) return;
        const bool known = juce::roundToInt (fx.key->get()) > 0;
        if (autoTune.isEnabled() != known) { autoTune.setEnabled (known); autoTune.setAlpha (known ? 1.0f : 0.35f); }
        if (const juce::String label = known ? "NEW KEY" : "FIND KEY"; findKey.text != label) { findKey.text = label; findKey.repaint(); }
        const auto st = transposeStatus (fx, false);
        if (st.text != shown) { shown = st.text; repaint (statusArea); }
    }

private:
    UiContext& ctx;
    ChannelFx& strip;
    TransposeFx& fx;
    Section section;
    Toggle on;
    Knob semi, fine;
    Toggle autoTune, findKey;
    juce::Rectangle<int> statusArea;
    juce::String shown;
    int ticks = 0;
};

//==============================================================================
/** Height of the blocks in the inspector's chain the last time it was laid out (the effect browser renders the
    block it drags at this height). */
int gRackHeight = 186;

/** A built-in effect's block in an effect chain (also rendered as the picture dragged out of the effect browser). */
struct BuiltUnit
{
    std::unique_ptr<juce::Component> comp;
    Section* section = nullptr;
    int width = 0;
    std::function<void()> tick;
};

std::unique_ptr<BuiltUnit> makeBuiltinUnit (UiContext& ctx, int channel, Fx f, int k, juce::Colour colour, const juce::String& title)
{
    auto made = std::make_unique<BuiltUnit>();
    auto& u = *made;
    auto& e = ctx.engine;
    const bool isInput = channel < kMaxInputs;
    Section* sec = nullptr;
        switch (f)
        {
            case Fx::eq:
            {
                auto& eq = isInput ? e.inputs[(size_t) channel].eqs[(size_t) k] : e.buses[(size_t) (channel - kMaxInputs)].eqs[(size_t) k];
                auto unit = std::make_unique<EqSection> (ctx, eq, colour, channel, k, title);
                sec = &unit->getSection();
                u.width = isInput ? 520 : 560;
                u.comp = std::move (unit);
                break;
            }
            case Fx::gate:
            {
                auto g = std::make_unique<GateUnit> (ctx, e.fxOf (channel), k, title);
                sec = &g->section;
                u.width = 150;
                u.comp = std::move (g);
                break;
            }
            case Fx::denoiser:
            {
                auto d = std::make_unique<DenoiserUnit> (ctx, e.fxOf (channel), k, title);
                sec = &d->section;
                u.tick = [p = d.get()] { p->tick(); };
                u.width = 140;
                u.comp = std::move (d);
                break;
            }
            case Fx::comp:
            {
                auto channelName = [&ctx, channel]
                {
                    return channel < kMaxInputs ? ctx.engine.inputs[(size_t) channel].name : ctx.engine.buses[(size_t) (channel - kMaxInputs)].name;
                };
                auto& comp = isInput ? e.inputs[(size_t) channel].comps[(size_t) k] : e.buses[(size_t) (channel - kMaxInputs)].comps[(size_t) k];
                auto c = std::make_unique<CompUnit> (ctx, comp, channelName, title);
                sec = &c->section;
                u.width = 200;
                u.comp = std::move (c);
                break;
            }
            case Fx::limiter:
            {
                auto& lim = isInput ? e.inputs[(size_t) channel].limiters[(size_t) k] : e.buses[(size_t) (channel - kMaxInputs)].limiters[(size_t) k];
                auto l = std::make_unique<LimiterUnit> (ctx, lim, title);
                sec = &l->section;
                u.width = 130;
                u.comp = std::move (l);
                break;
            }
            case Fx::utility:
            {
                auto channelName = [&ctx, channel]
                {
                    return channel < kMaxInputs ? ctx.engine.inputs[(size_t) channel].name : ctx.engine.buses[(size_t) (channel - kMaxInputs)].name;
                };
                auto& ut = isInput ? e.inputs[(size_t) channel].utilities[(size_t) k] : e.buses[(size_t) (channel - kMaxInputs)].utilities[(size_t) k];
                auto p = std::make_unique<UtilityUnit> (ctx, ut, channelName, title);
                sec = &p->getSection();
                u.width = 190;
                u.comp = std::move (p);
                break;
            }
            case Fx::transpose:
            {
                auto t = std::make_unique<TransposeUnit> (ctx, e.fxOf (channel), title);
                sec = &t->getSection();
                u.tick = [p = t.get()] { p->tick(); };
                u.width = 190;
                u.comp = std::move (t);
                break;
            }
            case Fx::stems:
            {
                auto st = std::make_unique<StemsUnit> (ctx, e.fxOf (channel), title);
                sec = &st->getSection();
                sec->beta = true;
                u.tick = [p = st.get()] { p->tick(); };
                u.width = 300;
                u.comp = std::move (st);
                break;
            }
            case Fx::correction:
            {
                auto c = std::make_unique<CorrectionSection> (ctx, channel, colour);
                sec = &c->getSection();
                u.width = 220;
                u.comp = std::move (c);
                break;
            }
            default: break;
        }
    if (u.comp == nullptr) return nullptr;
    u.section = sec;
    return made;
}

/** A plugin in the effect chain: name, vendor, bypass / edit / remove; drag the card to move it. */
class PluginCard final : public juce::Component, public juce::SettableTooltipClient
{
public:
    PluginCard (PluginSlot& s, juce::Colour a);
    void refresh();
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent&) override { openEditor(); }
    void openEditor();

    std::function<void (const juce::MouseEvent&)> onDrag;
    std::function<void()> onDrop, onRemove, onMenu;
    bool dragging = false, selected = false;

private:
    PluginSlot& slot;
    juce::Colour accent;
    juce::String name, vendor;
    Chip bypass { "ON" }, edit { "EDIT" };
    TrashButton remove;
};

/** The effect chain of a channel (inspector): plugins and built-in effects side by side in processing order.
    Every block can be moved (drag its header / card); right-click a block: copy, cut, paste, duplicate,
    delete; right-click the empty space: copy / paste the whole chain, add effects. The row scrolls sideways.
    While the chain is bypassed (inspector header) the blocks are drawn dimmed. */
class FxRack final : public juce::Component, public juce::DragAndDropTarget, private juce::Timer
{
public:
    FxRack (UiContext& c, int ch, std::function<void()> scan)
        : ctx (c), channel (ch), chain (c.engine.chainFor (ch)),
          colour (ch < kMaxInputs ? theme::accentIn : theme::busColour (ch - kMaxInputs)),
          bypass (ch < kMaxInputs ? *c.engine.inputs[(size_t) ch].fxBypass : *c.engine.buses[(size_t) (ch - kMaxInputs)].fxBypass),
          onScan (std::move (scan))
    {
        view.setViewedComponent (&row, false);
        view.setScrollBarsShown (false, true);
        view.setScrollBarThickness (8);
        view.setSingleStepSizes (40, 40);
        addAndMakeVisible (view);
        row.onMenu = [this] { showChainMenu(); };
        plus.colour = colour;
        plus.card = true;
        plus.setTooltip ("Add an effect or a VST3 plugin at the end of the chain");
        plus.onClick = [this] { showAddMenu (plus); };
        plus.setComponentID ("fx.add");
        row.addAndMakeVisible (plus);
        addChip.showArrow = false;
        addChip.centred = true;
        addChip.onClick = [this] { showAddMenu (addChip); };
        row.addAndMakeVisible (addChip);
        row.addAndMakeVisible (emptyInfo);
        emptyInfo.setText ("Effect chain\nEverything here runs left to right, with no added buffering, before the fader (PRE) "
                           "or after it (POST, next to BYPASS). "
                           "Add built-in effects (EQ, gate, compressor, utility, ...) as often as you like and VST3 plugins in any order; "
                           "drag a header or a card to move it, or drag effects and presets in from the effect browser (right edge).\n\n"
                           "Click a block to select it, Ctrl+click / Shift+click for several: Ctrl+C, Ctrl+X, Ctrl+V, Ctrl+D, Delete. "
                           "Right-click a block: copy, cut, paste, duplicate, delete, presets. Right-click the empty space: the whole chain, chain presets.");
        row.paintOver = [this] (juce::Graphics& g) { paintSelection (g); };
        row.paintBehind = [this] (juce::Graphics& g) { paintGlow (g); };
        row.addMouseListener (this, true);
        setWantsKeyboardFocus (true);
        rebuild();
        startTimerHz (20);
    }

    ~FxRack() override { stopTimer(); row.removeMouseListener (this); }

    //--------------------------------------------------------------------------
    // Selection: click a block, Ctrl+click adds / removes one, Shift+click the range from the last clicked one.
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) return;   // the block menu selects (showBlockMenu)
        const int i = unitAt (e);
        if (i < 0)
        {
            if (e.eventComponent == &row && ! selection.empty()) { selection.clear(); showSelection(); }
        }
        else if (e.mods.isCommandDown())
        {
            if (auto it = std::find (selection.begin(), selection.end(), i); it != selection.end()) selection.erase (it);
            else selection.push_back (i);
            anchor = i;
        }
        else if (e.mods.isShiftDown() && anchor >= 0)
        {
            selection.clear();
            for (int k = juce::jmin (anchor, i); k <= juce::jmax (anchor, i); ++k) selection.push_back (k);
        }
        else
        {
            selection = { i };
            anchor = i;
        }
        showSelection();
        // Keys (Ctrl+C ...) go to the chain, unless the click went to something that types (a value editor).
        if (e.eventComponent != nullptr && ! e.eventComponent->getWantsKeyboardFocus()) grabKeyboardFocus();
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        const bool cmd = k.getModifiers().isCommandDown() && ! k.getModifiers().isAltDown();
        const int code = k.getKeyCode();
        if (cmd && (code == 'A' || code == 'a'))
        {
            selection.clear();
            for (int i = 0; i < chain.size(); ++i) selection.push_back (i);
            anchor = 0;
            showSelection();
            return true;
        }
        if (cmd && (code == 'V' || code == 'v')) { pasteAfterSelection(); return true; }
        if (selection.empty())
            return false;
        if (cmd && (code == 'C' || code == 'c')) { copySelection(); return true; }
        if (cmd && (code == 'X' || code == 'x')) { copySelection(); deleteSelection(); return true; }
        if (cmd && (code == 'D' || code == 'd')) { duplicateSelection(); return true; }
        if (! cmd && (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey)) { deleteSelection(); return true; }
        if (k == juce::KeyPress::escapeKey) { selection.clear(); showSelection(); return true; }
        return false;
    }

    //--------------------------------------------------------------------------
    // Dropping effects / presets from the effect browser: a line shows where it lands.
    bool isInterestedInDragSource (const SourceDetails& d) override { return fxdrag::isFxDrag (d.description); }
    void itemDragEnter (const SourceDetails& d) override { itemDragMove (d); }
    void itemDragMove (const SourceDetails& d) override
    {
        // Like moving a block in the chain: the blocks make room (animated) where the dragged one's left edge is
        // nearest to a place. The picture hangs at the mouse by its header (fxdrag::kGrab).
        const int left = d.localPosition.x - view.getX() + view.getViewPositionX() - fxdrag::kGrabX;
        int target = 0, cx = 0, best = std::abs (left);
        for (int i = 0; i < (int) units.size(); ++i)
        {
            cx += units[(size_t) i].width + kGap;
            if (std::abs (left - cx) < best) { best = std::abs (left - cx); target = i + 1; }
        }
        dropAllowed = fxdrag::canAdd (ctx.engine, channel, d.description);
        const int w = juce::jmax (60, (int) d.description.getProperty ("width", 160));
        const int want = dropAllowed ? target : -2;   // -2: not allowed here, no room is made (grey line instead)
        if (want != externalDrop || w != externalWidth)
        {
            externalDrop = want;
            externalWidth = w;
            layoutUnits (true);
            row.repaint();
        }
        dropLeft = left;
        view.autoScroll (d.localPosition.x - view.getX(), d.localPosition.y - view.getY(), 40, 10);
    }
    void itemDragExit (const SourceDetails&) override
    {
        externalDrop = -1;
        layoutUnits (true);
        row.repaint();
    }
    void itemDropped (const SourceDetails& d) override
    {
        const int place = externalDrop;
        externalDrop = -1;
        if (place < 0) { layoutUnits (true); row.repaint(); return; }
        const int at = juce::isPositiveAndBelow (place, (int) units.size()) ? units[(size_t) place].slot : chain.size();
        const int from = juce::jmax (0, dropLeft);
        juce::Component::SafePointer<FxRack> safe (this);
        // Keeps the gap open until the new block is there; it then glides from where it was let go into it.
        externalDrop = place;
        fxdrag::add (ctx, channel, d.description, at, this, [safe, from] (int first, int n)
        {
            if (safe == nullptr) return;
            safe->externalDrop = -1;
            safe->selection.clear();
            for (int k = 0; k < n; ++k) safe->selection.push_back (first + k);
            safe->anchor = first;
            safe->landAt = first;
            safe->landFromX = from;
            safe->rebuild();
        });
        // Asked first (a note) or nothing was added: the timer closes the gap once nothing is open any more.
    }

    void resized() override
    {
        view.setBounds (getLocalBounds());
        layoutUnits();
    }

    void updateMeters() { for (auto& u : units) if (u.tick) u.tick(); }

    void unitDragged (int index, int x)
    {
        auto& u = units[(size_t) index];
        const int total = contentWidth (false);
        animator.forget (*u.comp);
        u.comp->setTopLeftPosition (juce::jlimit (0, juce::jmax (0, total - u.width), x), 0);
        // Drop slot: the place whose position is nearest to the dragged block's left edge. (Comparing centres
        // made the first place unreachable for a block wider than its neighbour: its centre can't get past it.)
        const int left = u.comp->getX();
        int target = 0, cx = 0, best = std::abs (left);
        int slot = 0;
        for (int i = 0; i < (int) units.size(); ++i)
        {
            if (i == index) continue;
            cx += units[(size_t) i].width + kGap;
            ++slot;
            if (std::abs (left - cx) < best) { best = std::abs (left - cx); target = slot; }
        }
        if (dragIndex != index || dropIndex != target)
        {
            dragIndex = index;
            dropIndex = target;
            layoutUnits (true);
        }
        view.autoScroll (u.comp->getX() - view.getViewPositionX() + u.width / 2, 10, 30, 8);
    }

    void unitDropped (int index)
    {
        const int to = dropIndex;
        const int droppedAt = juce::isPositiveAndBelow (index, (int) units.size()) ? units[(size_t) index].comp->getX() : 0;
        dragIndex = dropIndex = -1;
        if (to < 0 || to == index)
        {
            layoutUnits (true);
            return;
        }
        // Rebuilding destroys the unit whose mouse handler called us: do it afterwards. The moved block
        // then glides from where it was let go into its place.
        later ([index, to, droppedAt] (FxRack& r) { r.chain.move (index, to); r.landAt = to; r.landFromX = droppedAt; r.selection = { to }; r.anchor = to; });
    }

private:
    struct Unit
    {
        std::unique_ptr<juce::Component> comp;
        int width = 0;
        int slot = 0;           // index in the chain
        float alpha = 1.0f;     // dimmed while the block is off or the chain is bypassed
        std::function<void()> tick;
        Section* section = nullptr;     // built-in effect: its card (drawn selected); plugins: card below
    };

    /** The row behind the blocks: a right-click on empty space opens the chain menu. Draws the selection
        and the drop line over the blocks. */
    struct Row final : juce::Component
    {
        std::function<void()> onMenu;
        std::function<void (juce::Graphics&)> paintBehind, paintOver;
        void mouseDown (const juce::MouseEvent& e) override { if (e.mods.isPopupMenu() && onMenu) onMenu(); }
        void paint (juce::Graphics& g) override { if (paintBehind) paintBehind (g); }
        void paintOverChildren (juce::Graphics& g) override { if (paintOver) paintOver (g); }
    };

    int unitAt (const juce::MouseEvent& e)
    {
        const auto p = e.getEventRelativeTo (&row).getPosition();
        for (auto& u : units)
            if (u.comp->getBounds().contains (p)) return u.slot;
        return -1;
    }

    bool isSelected (int slot) const { return std::find (selection.begin(), selection.end(), slot) != selection.end(); }

    std::vector<int> sortedSelection() const
    {
        auto v = selection;
        std::sort (v.begin(), v.end());
        return v;
    }

    /** Soft glow behind the selected blocks (their cards draw the rest, drawSelectedCard). */
    void paintGlow (juce::Graphics& g)
    {
        for (auto& u : units)
            if (isSelected (u.slot) && u.comp->isVisible())
            {
                const auto c = u.section != nullptr ? u.section->colour : colour;
                juce::Path p;
                p.addRoundedRectangle (u.comp->getBounds().toFloat().reduced (3.0f), 6.0f);
                juce::DropShadow (c.withAlpha (0.45f), 16, {}).drawForPath (g, p);
            }
    }

    /** Pushes the selection into the cards and repaints. */
    void showSelection()
    {
        for (auto& u : units)
        {
            const bool sel = isSelected (u.slot);
            if (u.section != nullptr && u.section->selected != sel) { u.section->selected = sel; u.section->repaint(); }
            if (auto* card = dynamic_cast<PluginCard*> (u.comp.get()); card != nullptr && card->selected != sel) { card->selected = sel; card->repaint(); }
        }
        row.repaint();
    }

    void paintSelection (juce::Graphics& g)
    {
        if (externalDrop == -2)   // the dragged effect cannot go into this chain (the most instances reached)
        {
            const auto r = juce::Rectangle<int> (juce::jmax (0, dropLeft), 6, juce::jmax (120, externalWidth), row.getHeight() - 12).toFloat();
            g.setColour (theme::bg.withAlpha (0.6f));
            g.fillRoundedRectangle (r, 8.0f);
            g.setColour (theme::textFaint);
            g.drawRoundedRectangle (r.reduced (1.0f), 8.0f, 1.5f);
            g.setFont (theme::font (11.5f, true));
            g.drawFittedText ("Already the most\nof this effect", r.reduced (8.0f).toNearestInt(), juce::Justification::centred, 2);
        }
    }

    /** One block: an fx clip; several: a chain clip of just them. */
    std::unique_ptr<juce::XmlElement> selectionClip() const
    {
        const auto v = sortedSelection();
        if (v.size() == 1) return clip::copyBlock (ctx.engine, channel, v[0]);
        return clip::copyBlocks (ctx.engine, channel, v);
    }

    void copySelection()
    {
        if (auto x = selectionClip()) clip::toClipboard (*x);
    }

    void deleteSelection()
    {
        auto v = sortedSelection();
        selection.clear();
        later ([v] (FxRack& r)
        {
            for (auto it = v.rbegin(); it != v.rend(); ++it) clip::deleteBlock (r.ctx.engine, r.channel, *it);
        });
    }

    /** Inserts a clip's blocks (one block, a chain, a channel's chain) at index and selects them. */
    void insertClip (const juce::XmlElement& x, int index)
    {
        auto copy = std::make_shared<juce::XmlElement> (x);
        later ([copy, index] (FxRack& r)
        {
            const int at = index < 0 || index > r.chain.size() ? r.chain.size() : index;
            const int n = clip::insertBlocks (r.ctx.engine, r.channel, *copy, at);
            r.selection.clear();
            for (int k = 0; k < n; ++k) r.selection.push_back (at + k);
            if (n < clip::numBlocks (copy.get()))
                dialog::message (&r, dialog::Kind::info, n == 0 ? "Nothing was added" : "Some effects were left out",
                                 "This channel cannot have all of them (an effect it cannot have, the most instances reached or a plugin that does not load).");
        });
    }

    void pasteAfterSelection()
    {
        auto x = clip::fromClipboard();
        if (x == nullptr || clip::numBlocks (x.get()) == 0) return;
        const auto v = sortedSelection();
        insertClip (*x, v.empty() ? -1 : v.back() + 1);
    }

    void duplicateSelection()
    {
        const auto v = sortedSelection();
        if (auto x = selectionClip()) insertClip (*x, v.back() + 1);
    }

    //--------------------------------------------------------------------------
    // Presets (state/Presets.h)
    juce::String channelName() const
    {
        return channel < kMaxInputs ? ctx.engine.inputs[(size_t) channel].name : ctx.engine.buses[(size_t) (channel - kMaxInputs)].name;
    }

    /** Asks for a name, then saves the clip as a preset (asks before replacing one of the same name). */
    void savePreset (std::unique_ptr<juce::XmlElement> clipXml, const juce::String& suggested, const juce::String& what)
    {
        if (clipXml == nullptr) return;
        std::shared_ptr<juce::XmlElement> x (clipXml.release());
        juce::Component::SafePointer<FxRack> safe (this);
        dialog::askText (this, "Save " + what + " as preset", "The preset appears in the add menu and in the effect browser.",
                         suggested, "Save", [safe, x] (juce::String name)
        {
            auto write = [safe, x, name]
            {
                juce::String err;
                if (! presets::save (*x, name, err) && safe != nullptr)
                    dialog::message (safe.getComponent(), dialog::Kind::warning, "Preset not saved", err);
            };
            if (presets::fileFor (*x, name).existsAsFile())
                dialog::confirm (safe.getComponent(), dialog::Kind::question, "Replace the preset \"" + name + "\"?",
                                 "A preset with this name already exists.", "Replace", write);
            else
                write();
        });
    }

    /** Presets that can replace the settings of block i (same built-in effect / same plugin). */
    std::vector<presets::Preset> presetsForBlock (int i) const
    {
        auto* s = chain.getSlot (i);
        if (s == nullptr) return {};
        return s->isPlugin() ? presets::forPlugin (s->description) : presets::forFx (s->builtin);
    }

    static constexpr int kGap = 0, kCardWidth = 190, kPlusWidth = 40;

    /** Hooks a section header up to dragging / removing / the block menu of unit index i. */
    void chainControls (Section& s, int i)
    {
        s.enableChainControls ([this, i] { later ([i] (FxRack& r) { clip::deleteBlock (r.ctx.engine, r.channel, i); }); });
        s.onMenu = [this, i] { showBlockMenu (i); };
        s.grip.onDown = [this, &s] (const juce::MouseEvent& e) { grabX = e.getEventRelativeTo (s.getParentComponent()).getMouseDownX(); };
        s.grip.onDrag = [this, i] (const juce::MouseEvent& e) { unitDragged (i, e.getEventRelativeTo (&row).x - grabX); };
        s.grip.onDrop = [this, i] { unitDropped (i); };
    }

    /** Runs an edit of the chain after the current mouse / menu handler and rebuilds the blocks. */
    void later (std::function<void (FxRack&)> what)
    {
        juce::Component::SafePointer<FxRack> safe (this);
        juce::MessageManager::callAsync ([safe, what] { if (safe != nullptr) { what (*safe); safe->rebuild(); } });
    }

    juce::String blockName (int i) const
    {
        auto* s = chain.getSlot (i);
        if (s == nullptr) return {};
        if (s->isPlugin()) return s->instance->getName();
        return fxName (s->builtin) + (s->fxInstance > 0 ? " " + juce::String (s->fxInstance + 1) : juce::String());
    }

    void rebuild()
    {
        animator.clear();
        units.clear();
        dragIndex = dropIndex = -1;
        selection.erase (std::remove_if (selection.begin(), selection.end(), [this] (int i) { return i < 0 || i >= chain.size(); }), selection.end());
        seenVersion = chain.getVersion();
        auto& e = ctx.engine;
        const bool isInput = channel < kMaxInputs;
        for (int i = 0; i < chain.size(); ++i)
        {
            auto* slot = chain.getSlot (i);
            Unit u;
            if (slot->isPlugin())
            {
                auto card = std::make_unique<PluginCard> (*slot, colour);
                card->onDrag = [this, i] (const juce::MouseEvent& ev) { unitDragged (i, ev.getEventRelativeTo (&row).x - ev.getMouseDownX()); };
                card->onDrop = [this, i] { unitDropped (i); };
                card->onRemove = [this, i] { later ([i] (FxRack& r) { r.chain.remove (i); }); };
                card->onMenu = [this, i] { showBlockMenu (i); };
                u.width = kCardWidth;
                u.comp = std::move (card);
            }
            else
            {
                auto made = makeBuiltinUnit (ctx, channel, slot->builtin, slot->fxInstance, colour, blockName (i));
                if (made == nullptr) continue;
                u.comp = std::move (made->comp);
                u.width = made->width;
                u.tick = made->tick;
                Section* sec = made->section;
                u.section = sec;
                if (u.comp == nullptr) continue;
                chainControls (*sec, i);
                u.width = juce::jmax (u.width, sec->minimumWidth());
            }
            u.slot = i;
            row.addAndMakeVisible (*u.comp);
            units.push_back (std::move (u));
        }
        const bool empty = units.empty();
        plus.setVisible (! empty || theme::deep());   // Deep theme: the same button with and without blocks
        addChip.setVisible (empty && ! theme::deep());
        emptyInfo.setVisible (empty);
        for (auto& u : units)
        {
            u.alpha = targetAlpha (u);
            u.comp->setAlpha (u.alpha);
        }
        layoutUnits();
        const int land = std::exchange (landAt, -1);
        for (auto& u : units)
            if (u.slot == land)
            {
                const auto target = u.comp->getBounds();
                u.comp->setTopLeftPosition (landFromX, 0);
                u.comp->toFront (false);
                animator.moveTo (*u.comp, target);
            }
        showSelection();
    }

    int contentWidth (bool withPlus) const
    {
        int w = 0;
        for (auto& u : units) w += u.width + kGap;
        return w + (withPlus ? kPlusWidth + 6 : 0);
    }

    /** animated: blocks glide to their places (making room while one is dragged); otherwise they are simply there. */
    void layoutUnits (bool animated = false)
    {
        auto put = [this, animated] (juce::Component& c, juce::Rectangle<int> target)
        {
            if (animated) animator.moveTo (c, target);
            else          animator.place (c, target);
        };
        // At least as wide as the view, so the empty space right of the blocks takes right-clicks too.
        const int needed = (units.empty() && externalDrop < 0 ? 420 : contentWidth (true)) + (externalDrop >= 0 ? externalWidth + kGap : 0);
        const bool scrolls = needed > view.getWidth();
        row.setSize (juce::jmax (needed, view.getWidth()), juce::jmax (40, view.getHeight() - (scrolls ? view.getScrollBarThickness() + 2 : 0)));
        const int h = row.getHeight();
        gRackHeight = h;
        if (units.empty() && externalDrop < 0)
        {
            if (theme::deep())
            {
                plus.setBounds (3, 3, kPlusWidth, h - 6);
                emptyInfo.setBounds (kPlusWidth + 11, h / 2 - 8, 16, 16);
                return;
            }
            addChip.setBounds (6, h / 2 - 16, 150, 32);
            emptyInfo.setBounds (162, h / 2 - 8, 16, 16);
            return;
        }
        plus.setVisible (! units.empty() || theme::deep());
        addChip.setVisible (units.empty() && ! theme::deep() && externalDrop < 0);
        emptyInfo.setVisible (units.empty() && externalDrop < 0);
        int x = 0, k = 0;
        for (int i = 0; i < (int) units.size(); ++i)
        {
            if (i == dragIndex) continue;   // follows the mouse
            if (k == dropIndex) x += units[(size_t) dragIndex].width + kGap;   // gap where it would land
            if (i == externalDrop) x += externalWidth + kGap;                  // a block from the effect browser
            put (*units[(size_t) i].comp, { x, 0, units[(size_t) i].width, h });
            x += units[(size_t) i].width + kGap;
            ++k;
        }
        if (dragIndex >= 0 && dropIndex >= k) x += units[(size_t) dragIndex].width + kGap;
        if (externalDrop >= (int) units.size()) x += externalWidth + kGap;
        // Deep theme: as high as the blocks (their cards are inset by 3 px).
        if (theme::deep()) put (plus, { x + 3, 3, kPlusWidth, h - 6 });
        else               put (plus, { x + 6, 6, kPlusWidth, h - 12 });
    }

    /** A block that is off, or any block of a bypassed chain, is dimmed (it stays usable). */
    float targetAlpha (const Unit& u) const
    {
        bool off = bypass.isOn();
        if (const auto* s = chain.getSlot (u.slot); s != nullptr && ! off)
        {
            if (s->isPlugin()) off = s->bypassed.load();
            else if (auto* on = ctx.engine.fxOnParam (channel, s->builtin, s->fxInstance)) off = ! on->isOn();
        }
        return off ? 0.4f : 1.0f;
    }

    void updateDimming()
    {
        for (auto& u : units)
            if (targetAlpha (u) != u.alpha) { fading = true; return; }
    }

    void fadeFrame (double)
    {
        if (! fading) return;
        fading = false;
        for (auto& u : units)
        {
            const float target = targetAlpha (u);
            u.alpha += (target - u.alpha) * 0.25f;
            if (std::abs (target - u.alpha) < 0.01f) u.alpha = target;
            else fading = true;
            u.comp->setAlpha (u.alpha);
        }
    }

    //--------------------------------------------------------------------------
    /** Built-in effects and VST3 plugins (each with its presets in a submenu, if it has any) and chain presets;
        chosen ones are inserted at index (-1 = end). */
    juce::PopupMenu addMenu (int index)
    {
        juce::PopupMenu m;
        juce::Component::SafePointer<FxRack> safe (this);
        auto insertPreset = [safe, index] (const presets::Preset& p)
        {
            return [safe, index, p]
            {
                if (safe == nullptr) return;
                if (auto x = presets::load (p)) safe->insertClip (*x, index);
                else dialog::message (safe.getComponent(), dialog::Kind::warning, "Preset not found", "The preset file is gone or cannot be read.");
            };
        };
        if (const auto cs = presets::chains(); ! cs.empty())
        {
            juce::PopupMenu sub;
            for (auto& p : cs) sub.addItem (p.name + "   (" + juce::String (p.blocks) + ")", insertPreset (p));
            m.addSubMenu ("Chain presets", sub);
            m.addSeparator();
        }
        m.addSectionHeader ("Built-in effects");
        for (auto f : ctx.engine.typicalFx (channel))   // the effects made for this kind of channel (the browser has all)
        {
            int used = 0;
            for (int k = 0; k < Engine::maxInstances (f); ++k) used += chain.hasBuiltin (f, k) ? 1 : 0;
            const bool full = used >= Engine::maxInstances (f);
            const auto label = fxName (f) + (f == Fx::stems ? "  (beta)" : "") + (full ? "   (" + juce::String (used) + " in the chain, the most possible)" : juce::String());
            auto add = [safe, f, index]
            {
                if (safe == nullptr) return;
                fxdrag::add (safe->ctx, safe->channel, fxdrag::builtin (f), index, safe.getComponent(), [safe, index] (int at, int)
                {
                    if (safe == nullptr) return;
                    safe->selection = { at };
                    safe->anchor = at;
                    safe->rebuild();
                    if (index < 0) safe->view.setViewPosition (safe->row.getWidth(), 0);
                });
            };
            const auto ps = presets::forFx (f);
            if (ps.empty() || full)
            {
                m.addItem (label, ! full, false, add);
                continue;
            }
            juce::PopupMenu sub;
            sub.addItem ("Default", add);
            sub.addSeparator();
            for (auto& p : ps) sub.addItem (p.name, insertPreset (p));
            m.addSubMenu (label, sub);
        }
        m.addSeparator();
        m.addSectionHeader ("VST3 plugins");
        std::map<juce::String, juce::PopupMenu> byVendor;
        for (auto& d : ctx.engine.pluginHost.known.getTypes())
        {
            if (d.isInstrument) continue;
            auto create = [safe, d, index]
            {
                if (safe == nullptr) return;
                auto& eng = safe->ctx.engine;
                juce::String err;
                auto inst = eng.pluginHost.create (d, eng.getSampleRate(), eng.getBlockSize(), err);
                if (inst == nullptr)
                {
                    dialog::message (safe.getComponent(), dialog::Kind::warning, "Plugin could not be loaded", d.name + "\n" + err);
                    return;
                }
                const int at = index < 0 ? safe->chain.size() : index;
                safe->chain.add (std::move (inst), d, at);
                safe->selection = { at };
                safe->anchor = at;
                safe->rebuild();
                if (index < 0) safe->view.setViewPosition (safe->row.getWidth(), 0);
                if (juce::isPositiveAndBelow (at, (int) safe->units.size()))
                    if (auto* card = dynamic_cast<PluginCard*> (safe->units[(size_t) at].comp.get())) card->openEditor();
            };
            auto& vendorMenu = byVendor[d.manufacturerName.isEmpty() ? juce::String ("Unknown") : d.manufacturerName];
            const auto ps = presets::forPlugin (d);
            if (ps.empty())
            {
                vendorMenu.addItem (d.name, create);
                continue;
            }
            juce::PopupMenu sub;
            sub.addItem ("Default", create);
            sub.addSeparator();
            for (auto& p : ps) sub.addItem (p.name, insertPreset (p));
            vendorMenu.addSubMenu (d.name, sub);
        }
        for (auto& [vendor, sub] : byVendor)
            m.addSubMenu (vendor, sub);
        if (byVendor.empty())
            m.addItem ("(no plugins found yet)", false, false, nullptr);
        m.addItem ("Scan for VST3 plugins...", [safe] { if (safe != nullptr && safe->onScan) safe->onScan(); });
        return m;
    }

    void showAddMenu (juce::Component& target)
    {
        addMenu (-1).showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target));
    }

    /** Right-click on a block: copy, cut, paste after it, paste its settings, duplicate, delete, on / off, presets.
        With several blocks selected (the clicked one among them): copy / cut / duplicate / delete / save them all. */
    void showBlockMenu (int i)
    {
        auto* slot = chain.getSlot (i);
        if (slot == nullptr) return;
        if (! isSelected (i)) { selection = { i }; anchor = i; showSelection(); }
        grabKeyboardFocus();
        auto& e = ctx.engine;
        const auto held = clip::fromClipboard();
        const auto heldName = clip::describe (held.get());
        const int heldBlocks = clip::numBlocks (held.get());
        const auto sel = sortedSelection();
        juce::PopupMenu m;
        if (sel.size() > 1)
        {
            const auto n = juce::String ((int) sel.size()) + " effects";
            m.addSectionHeader (n + " selected");
            m.addItem ("Copy " + n + "   (Ctrl+C)", [this] { copySelection(); });
            m.addItem ("Cut " + n + "   (Ctrl+X)", [this] { copySelection(); deleteSelection(); });
            m.addItem ("Paste" + (heldBlocks > 0 ? "  " + (heldBlocks > 1 ? juce::String (heldBlocks) + " effects" : heldName) + "  (after them)" : juce::String()),
                       heldBlocks > 0, false, [this] { pasteAfterSelection(); });
            m.addItem ("Duplicate " + n + "   (Ctrl+D)", [this] { duplicateSelection(); });
            m.addItem ("Delete " + n + "   (Del)", [this] { deleteSelection(); });
            m.addSeparator();
            m.addItem ("Save " + n + " as chain preset...", [this]
            {
                savePreset (selectionClip(), channelName() + " effects", "the selected effects");
            });
            m.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
            return;
        }
        const auto self = clip::copyBlock (e, channel, i);
        const bool canDuplicate = slot->isPlugin() || clip::canInsertBlock (e, self.get(), channel);
        const bool isOn = slot->isPlugin() ? ! slot->bypassed.load() : (e.fxOnParam (channel, slot->builtin, slot->fxInstance) != nullptr
                                                                         && e.fxOnParam (channel, slot->builtin, slot->fxInstance)->isOn());
        m.addSectionHeader (blockName (i));
        m.addItem ("Copy   (Ctrl+C)", [this, i] { if (auto x = clip::copyBlock (ctx.engine, channel, i)) clip::toClipboard (*x); });
        m.addItem ("Cut   (Ctrl+X)", [this, i]
        {
            if (auto x = clip::copyBlock (ctx.engine, channel, i)) clip::toClipboard (*x);
            later ([i] (FxRack& r) { clip::deleteBlock (r.ctx.engine, r.channel, i); });
        });
        m.addItem ("Paste" + (heldBlocks > 0 ? "  " + (heldBlocks > 1 ? juce::String (heldBlocks) + " effects" : heldName) + "  (after this)" : juce::String()),
                   heldBlocks > 0 && (clip::whatOf (held.get()) != clip::What::fx || clip::canInsertBlock (e, held.get(), channel)), false,
                   [this] { pasteAfterSelection(); });
        if (clip::canPasteSettings (e, held.get(), channel, i))
            m.addItem ("Paste settings from the copied " + heldName,
                       [this, i] { later ([i] (FxRack& r) { if (auto x = clip::fromClipboard()) clip::pasteSettings (r.ctx.engine, r.channel, i, *x); }); });
        m.addItem ("Duplicate   (Ctrl+D)", canDuplicate, false, [this, i] { later ([i] (FxRack& r) { clip::duplicateBlock (r.ctx.engine, r.channel, i); r.selection = { i + 1 }; }); });
        m.addItem ("Delete   (Del)", [this, i] { later ([i] (FxRack& r) { clip::deleteBlock (r.ctx.engine, r.channel, i); }); });
        m.addSeparator();
        m.addItem (isOn ? "Switch off (bypass)" : "Switch on", [this, i, isOn]
        {
            auto* s = chain.getSlot (i);
            if (s == nullptr) return;
            if (s->isPlugin()) { s->bypassed = isOn; rebuild(); }
            else if (auto* on = ctx.engine.fxOnParam (channel, s->builtin, s->fxInstance)) on->set (isOn ? 0.0f : 1.0f);
        });
        m.addSubMenu ("Insert after this", addMenu (i + 1));
        m.addSeparator();
        const auto name = blockName (i);
        m.addItem ("Save as preset...", [this, i, name]
        {
            const auto* s = chain.getSlot (i);
            savePreset (clip::copyBlock (ctx.engine, channel, i), s != nullptr && s->isPlugin() ? name + " " + channelName() : channelName() + " " + name, name);
        });
        const auto ps = presetsForBlock (i);
        juce::PopupMenu load;
        for (auto& p : ps)
            load.addItem (p.name, [this, i, p]
            {
                later ([i, p] (FxRack& r)
                {
                    if (auto x = presets::load (p)) clip::pasteSettings (r.ctx.engine, r.channel, i, *x);
                });
            });
        m.addSubMenu ("Load preset", load, ! ps.empty());
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
    }

    /** Right-click on the empty space: the whole chain. */
    void showChainMenu()
    {
        auto& e = ctx.engine;
        const auto held = clip::fromClipboard();
        const auto w = clip::whatOf (held.get());
        juce::ignoreUnused (e);
        juce::PopupMenu m;
        m.addSectionHeader ("Effect chain of " + channelCode (channel));
        m.addItem ("Copy effect chain", chain.size() > 0, false, [this] { clip::toClipboard (*clip::copyChain (ctx.engine, channel)); });
        m.addItem (w == clip::What::channel ? "Paste effect chain (from the copied channel)" : "Paste effect chain (replaces this one)",
                   w == clip::What::chain || w == clip::What::channel, false,
                   [this] { later ([] (FxRack& r) { if (auto x = clip::fromClipboard()) clip::pasteChain (r.ctx.engine, r.channel, *x); }); });
        if (clip::numBlocks (held.get()) > 0)
            m.addItem ("Paste  " + (w == clip::What::fx ? clip::describe (held.get()) : juce::String (clip::numBlocks (held.get())) + " effects") + "  (at the end)",
                       w != clip::What::fx || clip::canInsertBlock (ctx.engine, held.get(), channel), false,
                       [this] { if (auto x = clip::fromClipboard()) insertClip (*x, -1); });
        m.addSeparator();
        m.addSubMenu ("Add effect", addMenu (-1));
        m.addItem ("Select all effects   (Ctrl+A)", chain.size() > 0, false, [this]
        {
            selection.clear();
            for (int i = 0; i < chain.size(); ++i) selection.push_back (i);
            showSelection();
            grabKeyboardFocus();
        });
        m.addItem ("Delete all effects", chain.size() > 0, false, [this]
        {
            later ([] (FxRack& r) { while (r.chain.size() > 0) clip::deleteBlock (r.ctx.engine, r.channel, r.chain.size() - 1); });
        });
        m.addSeparator();
        m.addItem ("Save chain as preset...", chain.size() > 0, false, [this]
        {
            savePreset (clip::copyChain (ctx.engine, channel), channelName() + " chain", "the effect chain");
        });
        juce::PopupMenu load;
        for (auto& p : presets::chains())
            load.addItem (p.name + "   (" + juce::String (p.blocks) + ")", [this, p]
            {
                later ([p] (FxRack& r) { if (auto x = presets::load (p)) clip::pasteChain (r.ctx.engine, r.channel, *x); });
            });
        m.addSubMenu ("Replace with chain preset", load, load.getNumItems() > 0);
        m.addItem ("Export effect chain...", chain.size() > 0, false, [this] { exportChain(); });
        m.addItem ("Import effect chain...", [this] { importChain(); });
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
    }

    /** Chain as a preset file anywhere (to pass on); import replaces this chain with one from such a file. */
    void exportChain()
    {
        auto x = clip::copyChain (ctx.engine, channel);
        x->setAttribute ("preset", channelName() + " chain");
        std::shared_ptr<juce::XmlElement> xml (x.release());
        juce::Component::SafePointer<FxRack> safe (this);
        chooser = std::make_unique<juce::FileChooser> ("Export effect chain",
            juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile (juce::File::createLegalFileName (channelName() + " chain") + presets::kExtension),
            juce::String ("*") + presets::kExtension);
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [safe, xml] (const juce::FileChooser& fc)
        {
            const auto f = fc.getResult();
            if (f == juce::File() || safe == nullptr) return;
            if (! xml->writeTo (f.withFileExtension (presets::kExtension)))
                dialog::message (safe.getComponent(), dialog::Kind::warning, "Export failed", "Could not write " + f.getFullPathName());
        });
    }

    void importChain()
    {
        juce::Component::SafePointer<FxRack> safe (this);
        chooser = std::make_unique<juce::FileChooser> ("Import effect chain", juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
                                                       juce::String ("*") + presets::kExtension);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc)
        {
            const auto f = fc.getResult();
            if (f == juce::File() || safe == nullptr) return;
            auto x = clip::parse (f.loadFileAsString());
            if (x == nullptr || clip::numBlocks (x.get()) == 0)
            {
                dialog::message (safe.getComponent(), dialog::Kind::warning, "Not an effect chain", f.getFileName() + " is not a Hydra preset.");
                return;
            }
            std::shared_ptr<juce::XmlElement> xml (x.release());
            if (clip::whatOf (xml.get()) == clip::What::fx) { safe->insertClip (*xml, -1); return; }
            safe->later ([xml] (FxRack& r) { clip::pasteChain (r.ctx.engine, r.channel, *xml); });
        });
    }

    void timerCallback() override
    {
        if (chain.getVersion() != seenVersion && dragIndex < 0) rebuild();   // e.g. pasted, undone, added by a scene
        // A gap kept open for a dropped effect whose adding was cancelled (or failed): close it.
        if (externalDrop != -1 && juce::Component::getCurrentlyModalComponent() == nullptr)
            if (auto* c = juce::DragAndDropContainer::findParentDragContainerFor (this); c == nullptr || ! c->isDragAndDropActive())
            {
                externalDrop = -1;
                layoutUnits (true);
                row.repaint();
            }
        updateDimming();
    }

    UiContext& ctx;
    int channel;
    PluginChain& chain;
    juce::Colour colour;
    Param& bypass;
    std::function<void()> onScan;
    juce::Viewport view;
    Row row;
    std::vector<Unit> units;
    PlusButton plus;
    Chip addChip { "+  Add effect", theme::text };
    InfoButton emptyInfo;
    int dragIndex = -1, dropIndex = -1, grabX = 0;
    int landAt = -1, landFromX = 0;   // block that was just dropped: glides from there to its place after the rebuild
    std::vector<int> selection;        // selected blocks (chain indices)
    int anchor = -1;                   // last clicked block (Shift+click selects the range from it)
    int externalDrop = -1;             // drag from the effect browser: unit index it would land before (-2: cannot go here)
    int externalWidth = 160, dropLeft = 0;
    bool dropAllowed = true;
    std::unique_ptr<juce::FileChooser> chooser;
    bool fading = false;
    uint32_t seenVersion = ~0u;
    LayoutAnimator animator { *this };
    juce::VBlankAttachment fadeVBlank { this, [this] (double t) { fadeFrame (t); } };
};

PluginCard::PluginCard (PluginSlot& s, juce::Colour a) : slot (s), accent (a)
{
    for (auto* b : { &bypass, &edit, (Chip*) &remove })
    {
        addAndMakeVisible (b);
        b->showArrow = false;
        b->centred = true;
    }
    bypass.onClick = [this] { slot.bypassed = ! slot.bypassed.load(); refresh(); };
    edit.onClick   = [this] { openEditor(); };
    remove.onClick = [this] { if (onRemove) onRemove(); };
    bypass.setTooltip ("Bypass this plugin");
    edit.setTooltip ("Open plugin editor");
    remove.setTooltip ("Remove plugin");
    setTooltip ("Drag to move it in the chain. Double-click: open the editor. Right-click: copy, cut, paste, duplicate, delete.");
    setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    refresh();
}

void PluginCard::refresh()
{
    name = slot.instance->getName();
    vendor = slot.description.manufacturerName;
    bypass.text = slot.bypassed ? "OFF" : "ON";
    bypass.active = ! slot.bypassed;
    bypass.colour = slot.bypassed ? theme::textDim : accent;
    repaint();
}

void PluginCard::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (3.0f);
    if (dragging)
        juce::DropShadow (juce::Colours::black.withAlpha (0.5f), 12, { 0, 3 }).drawForRectangle (g, r.toNearestInt());
    const auto fill = dragging ? theme::raised.brighter (0.08f) : theme::panel2;
    const auto line = dragging ? accent : bypass.active ? accent.withAlpha (0.45f) : theme::cardLine;
    if (theme::deep())
        theme::drawCard (g, r, fill, line, accent);
    else
    {
        g.setColour (fill);
        g.fillRoundedRectangle (r, 8.0f);
        g.setColour (line);
        g.drawRoundedRectangle (r, 8.0f, dragging ? 1.5f : 1.0f);
    }

    if (selected && ! dragging) drawSelectedCard (g, r, accent, theme::deep() ? 4.0f : 8.0f);
    drawGrip (g, { r.getX() + 9.0f, r.getY() + 9.0f, 8.0f, 14.0f });
    auto t = r.reduced (10.0f, 7.0f).withTrimmedLeft (14.0f);
    g.setColour (theme::textFaint);
    g.setFont (theme::font (11.0f, true));
    g.drawText ("VST3", t.removeFromTop (16.0f), juce::Justification::centredLeft);
    t.removeFromTop (4.0f);
    g.setColour (theme::text);
    g.setFont (theme::font (13.0f, true));
    g.drawFittedText (name, t.removeFromTop (18.0f).toNearestInt(), juce::Justification::centredLeft, 1);
    g.setColour (theme::textDim);
    g.setFont (theme::font (11.0f));
    g.drawFittedText (vendor, t.removeFromTop (15.0f).toNearestInt(), juce::Justification::centredLeft, 1);
}

void PluginCard::resized()
{
    auto r = getLocalBounds().reduced (12, 10).removeFromBottom (22);
    remove.setBounds (r.removeFromRight (30));
    r.removeFromRight (3);
    rowLayout (r, { &bypass, &edit }, 3);
}

void PluginCard::mouseDown (const juce::MouseEvent& e)
{
    dragging = false;
    if (e.mods.isPopupMenu() && onMenu) onMenu();
}

void PluginCard::mouseDrag (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) return;
    if (! dragging)
    {
        if (e.getDistanceFromDragStart() < 6) return;
        dragging = true;
        toFront (false);
        repaint();
    }
    if (onDrag) onDrag (e);
}

void PluginCard::mouseUp (const juce::MouseEvent&)
{
    if (! dragging) return;
    dragging = false;
    repaint();
    if (onDrop) onDrop();
}

void PluginCard::openEditor()
{
    if (slot.editorWindow == nullptr)
        slot.editorWindow = std::make_unique<PluginWindow> (slot);
    else
        slot.editorWindow->toFront (true);
}

//==============================================================================
juce::String formatTime (double seconds)
{
    const auto t = (int) seconds;
    const auto hms = t >= 3600 ? juce::String (t / 3600) + ":" + juce::String ((t / 60) % 60).paddedLeft ('0', 2)
                               : juce::String (t / 60).paddedLeft ('0', 2);
    return hms + ":" + juce::String (t % 60).paddedLeft ('0', 2) + "." + juce::String ((int) (seconds * 10.0) % 10);
}

/** Waveform of the take being recorded, or of the loaded file with the play position. Click or drag
    to move the play position (like a DAW timeline); while stopped it sets where PLAY starts.
    Trim like a clip in a video editor: drag the handle at the left / right edge inwards; the parts outside
    are greyed out and not played (Recorder::setTrim). "Cut" in the recorder writes the trimmed file. */
class WaveformView final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit WaveformView (Recorder& r) : rec (r)
    {
        setTooltip ("Click / drag: jump to that position.  Drag the handles at the edges to trim the start / end.");
        setMouseCursor (juce::MouseCursor::IBeamCursor);
    }

    /** Called ~30x per second: repaints only if the waveform or the position changed. */
    void update()
    {
        const bool recording = rec.isRecording();
        const auto& wave = recording ? rec.getRecordWaveform() : rec.getPlayWaveform();
        const double pos = recording ? rec.getRecordSeconds() : rec.isPlaying() ? rec.getPlaySeconds() : rec.getStartSeconds();
        const auto v = wave.getVersion();
        const int px = juce::roundToInt (pos / juce::jmax (0.001, length()) * getWidth());
        const auto trim = std::make_pair (rec.getTrimIn(), rec.getTrimOut());
        if (v != shownVersion || recording != shownRecording || px != shownPx || rec.getLoadedFile() != shownFile || trim != shownTrim)
        {
            shownTrim = trim;
            shownVersion = v;
            shownRecording = recording;
            shownPx = px;
            shownFile = rec.getLoadedFile();
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (theme::bg);
        g.fillRoundedRectangle (r, 6.0f);
        auto plot = r.reduced (6.0f, 4.0f).withTrimmedBottom (12.0f);

        const bool recording = rec.isRecording();
        const auto& wave = recording ? rec.getRecordWaveform() : rec.getPlayWaveform();
        const double len = length();
        const auto colour = recording ? theme::mute : theme::good;

        // Time ruler
        g.setFont (theme::font (9.5f));
        const double step = len <= 10 ? 1 : len <= 60 ? 5 : len <= 300 ? 30 : len <= 1200 ? 60 : len <= 3600 ? 300 : 600;
        for (double s = 0; s <= len && len > 0.0; s += step)
        {
            const float x = plot.getX() + (float) (s / len) * plot.getWidth();
            g.setColour (theme::outline.withAlpha (0.5f));
            g.fillRect (x, plot.getY(), 1.0f, plot.getHeight());
            g.setColour (theme::textFaint);
            const int t = (int) s;
            g.drawText (juce::String (t / 60) + ":" + juce::String (t % 60).paddedLeft ('0', 2),
                        juce::Rectangle<float> (x + 2.0f, plot.getBottom(), 40.0f, 12.0f), juce::Justification::centredLeft, false);
        }

        if (wave.getSeconds() <= 0.0 || len <= 0.0)
        {
            g.setColour (theme::textFaint);
            g.setFont (theme::font (12.0f));
            g.drawText (recording ? "Recording..." : rec.getLoadedFile() == juce::File() ? "Record something or open a file - its waveform appears here"
                                                                                          : "Reading the file...",
                        plot, juce::Justification::centred);
            return;
        }

        // Waveform: one column per pixel, mirrored around the centre line (level in dB-ish scale).
        const int cols = juce::jmax (1, (int) plot.getWidth());
        columns.resize ((size_t) cols);
        wave.getColumns (0.0, len, columns.data(), cols);
        const float mid = plot.getCentreY(), half = plot.getHeight() * 0.5f;
        juce::Path p;
        p.startNewSubPath (plot.getX(), mid);
        auto toH = [half] (float v) { return half * juce::jlimit (0.0f, 1.0f, std::sqrt (v)); };
        for (int c = 0; c < cols; ++c) p.lineTo (plot.getX() + (float) c, mid - toH (columns[(size_t) c]));
        for (int c = cols; --c >= 0;) p.lineTo (plot.getX() + (float) c, mid + toH (columns[(size_t) c]));
        p.closeSubPath();
        g.setColour (colour.withAlpha (0.55f));
        g.fillPath (p);
        g.setColour (colour.withAlpha (0.25f));
        g.fillRect (plot.getX(), mid - 0.5f, plot.getWidth(), 1.0f);

        if (recording) return;

        // Trim: greyed out outside [in, out], a handle at each end
        const float inX = xOf (rec.getTrimIn()), outX = xOf (rec.getTrimOut());
        g.setColour (theme::bg.withAlpha (0.72f));
        g.fillRect (juce::Rectangle<float> (plot.getX(), plot.getY(), inX - plot.getX(), plot.getHeight()));
        g.fillRect (juce::Rectangle<float> (outX, plot.getY(), plot.getRight() - outX, plot.getHeight()));
        for (int k = 0; k < 2; ++k)
        {
            const float x = k == 0 ? inX : outX;
            const bool active = dragHandle == k || hoverHandle == k;
            g.setColour (active ? theme::solo : theme::solo.withAlpha (0.75f));
            g.fillRect (x - 1.0f, plot.getY(), 2.0f, plot.getHeight());
            // Grip tab pointing into the kept part
            auto tab = juce::Rectangle<float> (k == 0 ? x : x - 9.0f, plot.getCentreY() - 14.0f, 9.0f, 28.0f);
            g.fillRoundedRectangle (tab, 3.0f);
            g.setColour (theme::bg.withAlpha (0.8f));
            for (int j = -1; j <= 1; ++j)
                g.fillRect (tab.getCentreX() - 2.0f, tab.getCentreY() + (float) j * 5.0f - 0.5f, 4.0f, 1.0f);
        }

        // Start cursor (where PLAY begins) and play head
        const float startX = plot.getX() + (float) (rec.getStartSeconds() / len) * plot.getWidth();
        g.setColour (theme::text.withAlpha (0.5f));
        g.fillRect (startX - 0.5f, plot.getY(), 1.0f, plot.getHeight());
        juce::Path flag;
        flag.addTriangle (startX - 4.0f, plot.getY(), startX + 4.0f, plot.getY(), startX, plot.getY() + 6.0f);
        g.fillPath (flag);
        if (rec.isPlaying())
        {
            const float x = plot.getX() + (float) (rec.getPlaySeconds() / len) * plot.getWidth();
            g.setColour (theme::text);
            g.fillRect (x - 1.0f, plot.getY() - 2.0f, 2.0f, plot.getHeight() + 4.0f);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragHandle = handleAt (e.x);
        if (dragHandle < 0) seekTo (e.x);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragHandle < 0) { seekTo (e.x); return; }
        const double t = secondsAt (e.x);
        if (dragHandle == 0) rec.setTrim (t, rec.getTrimOut());
        else                 rec.setTrim (rec.getTrimIn(), t);
        update();
    }

    void mouseUp (const juce::MouseEvent&) override { dragHandle = -1; repaint(); }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int h = handleAt (e.x);
        if (h != hoverHandle) { hoverHandle = h; repaint(); }
        setMouseCursor (h >= 0 ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::IBeamCursor);
    }

    void mouseExit (const juce::MouseEvent&) override { if (hoverHandle >= 0) { hoverHandle = -1; repaint(); } }

private:
    static constexpr float kLeft = 6.0f;
    float plotWidth() const { return (float) getWidth() - 2.0f * kLeft; }
    float xOf (double seconds) const
    {
        const double len = rec.getPlayLengthSeconds();
        return kLeft + (len > 0.0 ? (float) (seconds / len) : 0.0f) * plotWidth();
    }
    double secondsAt (int x) const
    {
        return juce::jlimit (0.0, 1.0, (double) ((float) x - kLeft) / plotWidth()) * rec.getPlayLengthSeconds();
    }
    /** 0 = in handle, 1 = out handle, -1 = none (grab zone of 8 px; the handles also catch the outer padding). */
    int handleAt (int x) const
    {
        if (rec.isRecording() || rec.getLoadedFile() == juce::File() || rec.getPlayLengthSeconds() <= 0.0) return -1;
        const float fx = (float) x, inX = xOf (rec.getTrimIn()), outX = xOf (rec.getTrimOut());
        const float dIn = std::abs (fx - inX) - (fx < inX ? 6.0f : 0.0f), dOut = std::abs (fx - outX) - (fx > outX ? 6.0f : 0.0f);
        if (dIn <= 8.0f && dIn <= dOut) return 0;
        if (dOut <= 8.0f) return 1;
        return -1;
    }

    double length() const
    {
        if (rec.isRecording()) return juce::jmax (1.0, rec.getRecordSeconds());
        return rec.getPlayLengthSeconds();
    }

    void seekTo (int x)
    {
        if (rec.isRecording() || rec.getLoadedFile() == juce::File()) return;
        rec.seek (secondsAt (x));
        update();
    }

    Recorder& rec;
    std::vector<float> columns;
    uint32_t shownVersion = 0;
    bool shownRecording = false;
    int shownPx = -1;
    juce::File shownFile;
    std::pair<double, double> shownTrim;
    int dragHandle = -1, hoverHandle = -1;
};

/** Recorder: left what is recorded (a V bus), in the middle the recording itself (transport, waveform,
    file), on the right where playback goes (any A / V buses). */
class RecorderDetail final : public juce::Component
{
public:
    explicit RecorderDetail (UiContext& c)
        : ctx (c), rec (c.engine.recorder),
          sourceSec ("Record Source", theme::accentVirt), takeSec ("Recording", theme::mute), outSec ("Play To", theme::accentHw),
          recBtn (c, *rec.record, "REC", theme::mute, Toggle::Icon::record),
          playBtn (c, *rec.play, "PLAY", theme::good, Toggle::Icon::playPause),
          stopBtn (c, *rec.stop, "STOP", theme::text, Toggle::Icon::stop),
          loopBtn (c, *rec.loop, "LOOP", theme::good, Toggle::Icon::loop),
          level (c, *rec.level, "LEVEL", theme::good),
          wave (rec)
    {
        for (juce::Component* s : std::initializer_list<juce::Component*> { &sourceSec, &takeSec, &outSec }) addAndMakeVisible (s);
        for (juce::Component* k : std::initializer_list<juce::Component*> { &folderChip, &formatChip }) sourceSec.addAndMakeVisible (k);
        for (juce::Component* k : std::initializer_list<juce::Component*> { &recBtn, &playBtn, &stopBtn, &loopBtn, &deleteBtn, &wave, &openChip, &lastChip, &showChip,
                                                                           &cutChip, &resetTrimChip })
            takeSec.addAndMakeVisible (k);
        outSec.addAndMakeVisible (level);
        for (int b = 0; b < kMaxBuses; ++b)
        {
            if (! c.engine.isBusActive (b)) continue;
            (isHwBus (b) ? hwOuts : virtOuts).push_back (std::make_unique<Toggle> (c, *rec.out[(size_t) b], busLabel (b), theme::busColour (b)));
            outSec.addAndMakeVisible (*(isHwBus (b) ? hwOuts : virtOuts).back());
        }
        // Record source: the V buses as the same kind of buttons as Play To (one of them is lit).
        for (int k = 0; k < kMaxVirtBuses; ++k)
        {
            const int b = virtBusIndex (k);
            if (! c.engine.isBusActive (b)) continue;
            sources.push_back (std::make_unique<Toggle> (c, *rec.source, busLabel (b), theme::busColour (b)));
            sources.back()->choiceValue = k;
            sources.back()->setTooltip ("Record " + busLabel (b) + " (" + c.engine.buses[(size_t) b].name + ")");
            sourceSec.addAndMakeVisible (*sources.back());
        }
        for (auto* chip : { &folderChip, &formatChip, &openChip, &lastChip, &showChip, &cutChip, &resetTrimChip })
        {
            chip->showArrow = false;
            chip->centred = true;
        }
        formatChip.showArrow = true;

        recBtn.setTooltip ("Record the selected V bus to a WAV file (stop with a second click)");
        playBtn.setTooltip ("Play / pause the loaded file on the buses selected under Play To (starts at the marker in the waveform)");
        stopBtn.setTooltip ("Stop recording / playback and go back to the start");
        loopBtn.setTooltip ("Repeat the file");
        deleteBtn.setTooltip ("Delete this recording: unloads it and moves the file to the Recycle Bin");
        deleteBtn.onClick = [this] { confirmDelete(); };
        cutChip.setTooltip ("Cut the file to the part between the trim handles (the original goes to the Recycle Bin)");
        resetTrimChip.setTooltip ("Move the trim handles back to the start and end");
        cutChip.onClick = [this] { confirmCut(); };
        resetTrimChip.onClick = [this] { rec.resetTrim(); };
        folderChip.setTooltip ("Folder for new recordings");
        openChip.setTooltip ("Load an audio file for playback (WAV, AIFF, FLAC, Ogg, MP3)");
        lastChip.setTooltip ("Load the most recent recording");
        showChip.setTooltip ("Show the loaded file in Explorer");
        level.setTooltip ("Playback level");

        folderChip.onClick = [this] { chooseFolder(); };
        formatChip.onClick = [this] { chooseFormat(); };
        openChip.onClick = [this] { openFile(); };
        lastChip.onClick = [this]
        {
            juce::String err;
            const auto f = rec.getLastRecording();
            if (f.existsAsFile()) rec.loadFile (f, err);
            refreshTexts();
        };
        showChip.onClick = [this]
        {
            const auto f = rec.getLoadedFile();
            if (f.existsAsFile()) f.revealToUser();
            else rec.getFolder().revealToUser();
        };
        refreshTexts();
    }

    void resized() override
    {
        // Source and outputs take what they need, the recording gets the rest.
        auto r = getLocalBounds();
        const int most = (int) juce::jmax (hwOuts.size(), virtOuts.size());
        const int perRow = most == 5 ? 5 : 4;   // 5 H buses in one row, otherwise rows of four
        sourceSec.setBounds (r.removeFromLeft (juce::jmax (210, 24 + perRow * 40 + 8)));
        outSec.setBounds (r.removeFromRight (24 + perRow * 40 + 8 + 70));
        takeSec.setBounds (r);

        // Rows of bus buttons under a small caption (record source and Play To look the same).
        auto group = [perRow] (juce::Rectangle<int>& area, std::vector<std::unique_ptr<Toggle>>& list, juce::Rectangle<int>& label,
                               juce::Point<int> origin)
        {
            label = {};
            if (list.empty()) return;
            label = area.removeFromTop (14) + origin;
            for (size_t k = 0; k < list.size(); k += (size_t) perRow)
            {
                auto row = area.removeFromTop (22);
                area.removeFromTop (4);
                for (size_t j = k; j < juce::jmin (list.size(), k + (size_t) perRow); ++j)
                {
                    list[j]->setBounds (row.removeFromLeft (36));
                    row.removeFromLeft (4);
                }
            }
            area.removeFromTop (4);
        };

        // Source: V bus, what it records, folder / format
        auto s = sourceSec.content();
        group (s, sources, sourceLabel, sourceSec.getPosition());
        s.removeFromTop (2);
        sourceInfo = s.removeFromTop (30).translated (sourceSec.getX(), sourceSec.getY());
        s.removeFromTop (6);
        auto chips = s.removeFromTop (26);
        folderChip.setBounds (chips.removeFromLeft (chips.getWidth() / 2 - 3));
        chips.removeFromLeft (6);
        formatChip.setBounds (chips);

        // Recording: transport + time, waveform, file
        auto t = takeSec.content();
        auto transport = t.removeFromTop (28);
        for (juce::Component* b : std::initializer_list<juce::Component*> { &recBtn, &playBtn, &stopBtn })
        {
            b->setBounds (transport.removeFromLeft (38));
            transport.removeFromLeft (4);
        }
        transport.removeFromLeft (6);
        loopBtn.setBounds (transport.removeFromLeft (38));
        transport.removeFromLeft (10);
        deleteBtn.setBounds (transport.removeFromLeft (34));
        transport.removeFromLeft (10);
        cutChip.setBounds (transport.removeFromLeft (64));
        transport.removeFromLeft (4);
        resetTrimChip.setBounds (transport.removeFromLeft (58));
        transport.removeFromLeft (14);
        timeArea = transport.translated (takeSec.getX(), takeSec.getY());
        t.removeFromTop (6);
        auto fileRow = t.removeFromBottom (24);
        t.removeFromBottom (6);
        wave.setBounds (t);
        showChip.setBounds (fileRow.removeFromRight (64));
        fileRow.removeFromRight (5);
        lastChip.setBounds (fileRow.removeFromRight (84));
        fileRow.removeFromRight (5);
        openChip.setBounds (fileRow.removeFromRight (92));
        fileRow.removeFromRight (10);
        fileArea = fileRow.translated (takeSec.getX(), takeSec.getY());

        // Play to: H buses, then V buses (own rows), level knob on the right
        auto o = outSec.content();
        level.setBounds (o.removeFromRight (64).withSizeKeepingCentre (64, 74));
        o.removeFromRight (8);
        group (o, hwOuts, hwLabel, outSec.getPosition());
        group (o, virtOuts, virtLabel, outSec.getPosition());
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        const bool recording = rec.isRecording(), playing = rec.isPlaying();
        auto t = timeArea;
        g.setFont (theme::mono (20.0f));
        g.setColour (recording ? theme::mute : playing ? theme::good : theme::textDim);
        const int tw = juce::jmin (t.getWidth(), juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), shownTime) + 16);
        g.drawText (shownTime, t.removeFromLeft (tw), juce::Justification::centredLeft);
        g.setFont (theme::font (11.5f));
        g.setColour (rec.getLastError().isNotEmpty() && ! recording && ! playing ? theme::warn : theme::textDim);
        g.drawFittedText (stateText, t, juce::Justification::centredRight, 2, 0.85f);

        g.setColour (theme::textFaint);
        g.setFont (theme::font (11.0f));
        g.drawFittedText (sourceText, sourceInfo, juce::Justification::topLeft, 2, 0.85f);

        g.setFont (theme::font (10.5f, true));
        if (! hwLabel.isEmpty())   { g.setColour (theme::accentHw.withAlpha (0.8f));   g.drawText ("HARDWARE (A)", hwLabel, juce::Justification::centredLeft); }
        if (! virtLabel.isEmpty()) { g.setColour (theme::accentVirt.withAlpha (0.8f)); g.drawText ("VIRTUAL (B)", virtLabel, juce::Justification::centredLeft); }
        if (! sourceLabel.isEmpty()) { g.setColour (theme::accentVirt.withAlpha (0.8f)); g.drawText ("VIRTUAL (B)", sourceLabel, juce::Justification::centredLeft); }

        auto f = fileArea;
        g.setColour (rec.getLoadedFile() != juce::File() ? theme::text : theme::textFaint);
        g.setFont (theme::font (12.5f, true));
        const int fw = juce::jmin (f.getWidth(), juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), fileTitle) + 12);
        g.drawFittedText (fileTitle, f.removeFromLeft (fw), juce::Justification::centredLeft, 1, 0.8f);
        g.setColour (theme::textDim);
        g.setFont (theme::font (11.0f));
        g.drawFittedText (fileInfo, f, juce::Justification::centredLeft, 1, 0.8f);
    }

    void updateMeters()
    {
        const bool recording = rec.isRecording(), playing = rec.isPlaying();
        juce::String time;
        if (recording)    time = formatTime (rec.getRecordSeconds());
        else if (playing) time = formatTime (rec.getPlaySeconds()) + " / " + formatTime (rec.getPlayLengthSeconds());
        else if (rec.getLoadedFile() != juce::File())
            time = formatTime (rec.getStartSeconds()) + " / " + formatTime (rec.getPlayLengthSeconds());
        else              time = formatTime (0.0);
        const auto v = rec.stateVersion.load() + rec.source->getVersion();
        if (v != seenVersion) { seenVersion = v; refreshTexts(); }
        if (time != shownTime) { shownTime = time; repaint (timeArea); }
        wave.update();
    }

private:
    void refreshTexts()
    {
        const bool recording = rec.isRecording(), playing = rec.isPlaying();
        if (recording)    stateText = "Recording " + busLabel (rec.getRecordSourceBus()) + "  ->  " + rec.getRecordingFile().getFileName();
        else if (playing) stateText = "Playing";
        else if (rec.getLastError().isNotEmpty()) stateText = rec.getLastError();
        else              stateText = "Stopped";

        sourceText = sources.empty() ? juce::String ("No virtual output (V bus) in use - add one to record.")
                                     : "Records " + busLabel (rec.getRecordSourceBus()) + " after its fader and limiter into  "
                                           + rec.getFolder().getFileName();
        folderChip.text = "Folder...";
        formatChip.text = rec.bitDepth >= 32 ? "WAV 32f" : "WAV " + juce::String (rec.bitDepth);

        const auto file = rec.getLoadedFile();
        deleteBtn.setEnabled (file != juce::File() && ! recording);
        const bool trimmed = rec.isTrimmed() && ! recording;
        for (auto* chip : { &cutChip, &resetTrimChip })
        {
            chip->setEnabled (trimmed);
            chip->setAlpha (trimmed ? 1.0f : 0.4f);
        }
        cutChip.active = trimmed;
        fileTitle = file != juce::File() ? file.getFileName() : juce::String ("No file loaded");
        fileInfo = file != juce::File() ? file.getParentDirectory().getFullPathName() : juce::String();
        if (rec.isTrimmed())
            fileInfo = "keeps " + formatTime (rec.getTrimIn()) + " - " + formatTime (rec.getTrimOut()) + "  ("
                     + formatTime (rec.getTrimOut() - rec.getTrimIn()) + ")  -  Cut writes it";
        for (auto* chip : { &folderChip, &formatChip }) chip->repaint();
        repaint();
    }

    void confirmDelete()
    {
        const auto file = rec.getLoadedFile();
        if (file == juce::File() || rec.isRecording()) return;
        juce::Component::SafePointer<RecorderDetail> safe (this);
        dialog::confirm (this, dialog::Kind::danger, "Delete this recording?",
                         file.getFileName() + "\n\nThe track is removed from the recorder and the file is moved to the Recycle Bin.",
                         "Delete", [safe, file]
                         {
                             if (safe == nullptr || safe->rec.getLoadedFile() != file) return;
                             juce::String err;
                             if (! safe->rec.deleteLoadedFile (err))
                                 dialog::message (safe.getComponent(), dialog::Kind::warning, "Recorder", err);
                             safe->refreshTexts();
                         });
    }

    void confirmCut()
    {
        const auto file = rec.getLoadedFile();
        if (file == juce::File() || ! rec.isTrimmed()) return;
        const bool wav = file.hasFileExtension ("wav");
        juce::Component::SafePointer<RecorderDetail> safe (this);
        dialog::confirm (this, dialog::Kind::question, "Cut the recording?",
                         file.getFileName() + "\n\nKeeps " + formatTime (rec.getTrimIn()) + " - " + formatTime (rec.getTrimOut()) + ". "
                             + (wav ? "The original file goes to the Recycle Bin." : "The result is saved as a new WAV file next to it."),
                         "Cut", [safe, file]
                         {
                             if (safe == nullptr || safe->rec.getLoadedFile() != file) return;
                             juce::String err;
                             if (! safe->rec.applyTrim (err))
                                 dialog::message (safe.getComponent(), dialog::Kind::warning, "Recorder", err);
                             safe->refreshTexts();
                         });
    }

    void chooseFolder()
    {
        chooser = std::make_unique<juce::FileChooser> ("Folder for recordings", rec.getFolder());
        juce::Component::SafePointer<RecorderDetail> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [safe] (const juce::FileChooser& fc)
                              {
                                  if (safe == nullptr || fc.getResult() == juce::File()) return;
                                  safe->rec.folder = fc.getResult();
                                  safe->rec.stateVersion.fetch_add (1);
                              });
    }

    void chooseFormat()
    {
        juce::PopupMenu m;
        for (int bits : { 16, 24, 32 })
            m.addItem (bits == 32 ? "WAV 32-bit float" : "WAV " + juce::String (bits) + "-bit", true, rec.bitDepth == bits,
                       [this, bits] { rec.bitDepth = bits; rec.stateVersion.fetch_add (1); });
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&formatChip));
    }

    void openFile()
    {
        const auto start = rec.getLoadedFile() != juce::File() ? rec.getLoadedFile().getParentDirectory() : rec.getFolder();
        chooser = std::make_unique<juce::FileChooser> ("Open audio file", start, "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3;*.wma");
        juce::Component::SafePointer<RecorderDetail> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe] (const juce::FileChooser& fc)
                              {
                                  if (safe == nullptr || ! fc.getResult().existsAsFile()) return;
                                  juce::String err;
                                  if (! safe->rec.loadFile (fc.getResult(), err))
                                      dialog::message (safe.getComponent(), dialog::Kind::warning, "Recorder", err);
                              });
    }

    UiContext& ctx;
    Recorder& rec;
    Section sourceSec, takeSec, outSec;
    Toggle recBtn, playBtn, stopBtn, loopBtn;
    TrashButton deleteBtn;
    Knob level;
    WaveformView wave;
    Chip folderChip { "Folder...", theme::text }, formatChip { "WAV 24", theme::text };
    Chip openChip { "Open File...", theme::text }, lastChip { "Last Take", theme::text }, showChip { "Show", theme::text };
    Chip cutChip { "Cut", theme::solo }, resetTrimChip { "Reset", theme::text };
    std::vector<std::unique_ptr<Toggle>> sources, hwOuts, virtOuts;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Rectangle<int> timeArea, sourceInfo, fileArea, hwLabel, virtLabel, sourceLabel;
    juce::String shownTime, stateText, sourceText, fileTitle, fileInfo;
    uint64_t seenVersion = 0;
};
} // namespace

std::unique_ptr<juce::Component> makeStemsPanel (UiContext& c, ChannelFx& s) { return std::make_unique<StemsPanel> (c, s); }
std::unique_ptr<juce::Component> makeUtilityPanel (UiContext& c, UtilityFx& u) { return std::make_unique<UtilityPanel> (c, u); }
std::unique_ptr<juce::Component> makeTransposePanel (UiContext& c, ChannelFx& s) { return std::make_unique<TransposePanel> (c, s.transpose); }
std::unique_ptr<juce::Component> makeCompPanel (UiContext& c, CompFx& f) { return std::make_unique<CompPanel> (c, f); }

//==============================================================================
EqGraph::EqGraph (UiContext& c, ParamEq& e, juce::Colour col, bool lg) : ctx (c), eq (e), colour (col), large (lg)
{
    ParamPoller::attach (this);
    setTooltip ("Click an empty spot to add a band. Drag a point: frequency / gain (Q for cut, notch, band pass). "
                "Mouse wheel: Q. Double-click or right-click a point: remove / change type.");
}

EqGraph::~EqGraph() { ParamPoller::detach (this); }

void EqGraph::resized()
{
    const auto r = getLocalBounds().toFloat();
    plot = large ? r.withTrimmedLeft (36.0f).withTrimmedRight (40.0f).withTrimmedTop (10.0f).withTrimmedBottom (22.0f) : r;
    rebuildCurve();
}

float EqGraph::freqToX (float f) const { return plot.getX() + plot.getWidth() * std::log (f / 20.0f) / std::log (1000.0f); }
float EqGraph::xToFreq (float x) const
{
    return juce::jlimit (20.0f, 20000.0f, 20.0f * std::pow (1000.0f, (x - plot.getX()) / juce::jmax (1.0f, plot.getWidth())));
}
float EqGraph::dbToY (float db) const   { return plot.getCentreY() - db / kViewDb * plot.getHeight() * 0.5f; }
float EqGraph::yToDb (float y) const    { return (plot.getCentreY() - y) / juce::jmax (1.0f, plot.getHeight() * 0.5f) * kViewDb; }
float EqGraph::specToY (float db) const { return plot.getY() + plot.getHeight() * (kSpecTopDb - db) / (kSpecTopDb - kSpecBottomDb); }

juce::Point<float> EqGraph::nodePos (int k) const
{
    const int t = juce::roundToInt (eq.type[(size_t) k]->get());
    const float y = ParamEq::typeUsesGain (t) ? dbToY (eq.gain[(size_t) k]->get()) : dbToY (0.0f);
    return { freqToX (eq.freq[(size_t) k]->get()), y };
}

void EqGraph::setSelectedBand (int b)
{
    b = juce::jlimit (0, ParamEq::kBands - 1, b);
    if (b == selected) return;
    selected = b;
    repaint();
    if (onBandSelected) onBandSelected (b);
}

void EqGraph::pollParams()
{
    if (const auto sum = eq.versionSum(); sum != seen) { seen = sum; rebuildCurve(); repaint(); }
}

float EqGraph::curveDbAt (float f) const
{
    const double sr = 48000.0;
    double total = 0.0;
    for (int k = 0; k < ParamEq::kBands; ++k)
        if (eq.bandOn[(size_t) k]->isOn() && eq.bandActive (k))
            total += eq.coeffs (k, sr).magnitudeDb (f, sr);
    return (float) juce::jlimit (-40.0, 40.0, total);
}

void EqGraph::rebuildCurve()
{
    curve.clear();
    for (auto& p : bandCurves) p.clear();
    if (plot.getWidth() < 2.0f) return;

    const double sr = 48000.0;
    dsp::BiquadCoeffs cs[ParamEq::kBands];
    bool used[ParamEq::kBands];
    for (int k = 0; k < ParamEq::kBands; ++k)
    {
        used[k] = eq.bandOn[(size_t) k]->isOn();
        if (used[k]) cs[k] = eq.coeffs (k, sr);
    }

    const float y0 = dbToY (0.0f);
    bool first = true;
    for (float x = plot.getX(); x <= plot.getRight() + 0.5f; x += 2.0f)
    {
        const float f = xToFreq (x);
        double total = 0.0;
        for (int k = 0; k < ParamEq::kBands; ++k)
        {
            if (! used[k]) continue;
            const double db = cs[k].magnitudeDb (f, sr);
            if (eq.bandActive (k)) total += db;
            const float y = dbToY ((float) juce::jlimit (-40.0, 40.0, db));
            if (first) { bandCurves[(size_t) k].startNewSubPath (x, y0); }
            bandCurves[(size_t) k].lineTo (x, y);
        }
        const float y = dbToY ((float) juce::jlimit (-40.0, 40.0, total));
        if (first) curve.startNewSubPath (x, y);
        else       curve.lineTo (x, y);
        first = false;
    }
    for (int k = 0; k < ParamEq::kBands; ++k)
        if (used[k])
        {
            bandCurves[(size_t) k].lineTo (plot.getRight(), y0);
            bandCurves[(size_t) k].closeSubPath();
        }
}

juce::Path EqGraph::spectrumPath (const std::vector<float>& cols) const
{
    if (cols.size() < 2 || plot.getWidth() <= 2.0f) return {};
    juce::Path p;
    p.startNewSubPath (plot.getX(), plot.getBottom());
    float x = plot.getX();
    for (size_t i = 0; i < cols.size(); ++i)
    {
        x = juce::jmin (plot.getRight(), plot.getX() + (float) (i * kSpectrumStep));
        p.lineTo (x, specToY (juce::jlimit (kSpecBottomDb - 6.0f, kSpecTopDb + 6.0f, cols[i])));
    }
    p.lineTo (x, plot.getBottom());
    p.closeSubPath();
    return p.createPathWithRoundedCorners (5.0f);
}

void EqGraph::setSpectrum (const std::vector<float>& cols)
{
    spectrum = spectrumPath (cols);
    repaint (plot.getSmallestIntegerContainer());
}

void EqGraph::setInputSpectrum (const std::vector<float>& cols)
{
    if (cols.empty() && inputSpectrum.isEmpty()) return;
    inputSpectrum = spectrumPath (cols);   // repainted with the coloured one (setSpectrum follows each frame)
}

void EqGraph::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    // The graph keeps the Legacy colours in every theme.
    const juce::Colour graphBg (0xff0d0f13), graphLine (0xff2a313e), graphDim (0xff8b94a5);
    const auto accent = colour == theme::accentHw ? juce::Colour (0xff8d6cff) : colour;
    g.setColour (graphBg);
    g.fillRoundedRectangle (r, 6.0f);

    // Grid + scales
    g.setFont (theme::font (large ? 10.5f : 9.5f));
    for (float f : { 20.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f })
    {
        const float x = freqToX (f);
        if (f > 20.0f && f < 20000.0f)
        {
            g.setColour (graphLine.withAlpha (0.5f));
            g.fillRect (x, plot.getY(), 1.0f, plot.getHeight());
        }
        else if (! large) continue;
        const auto label = f >= 1000 ? juce::String ((int) (f / 1000)) + "k" : juce::String ((int) f);
        g.setColour (theme::textFaint);
        if (large)
            g.drawText (label, juce::Rectangle<float> (x - 20.0f, plot.getBottom() + 4.0f, 40.0f, 14.0f), juce::Justification::centred);
        else
            g.drawText (label, (int) x + 2, (int) r.getBottom() - 13, 30, 12, juce::Justification::left);
    }
    for (float db = -18.0f; db <= 18.0f; db += 6.0f)
    {
        const float y = dbToY (db);
        g.setColour (db == 0.0f ? graphLine : graphLine.withAlpha (0.4f));
        g.fillRect (plot.getX(), y, plot.getWidth(), 1.0f);
        if (large)
        {
            g.setColour (db == 0.0f ? graphDim : theme::textFaint);
            g.drawText ((db > 0 ? "+" : "") + juce::String ((int) db), juce::Rectangle<float> (0.0f, y - 7.0f, plot.getX() - 6.0f, 14.0f),
                        juce::Justification::centredRight);
        }
    }
    if (large && ! spectrum.isEmpty())
    {
        // Spectrum level scale (dBFS) on the right
        g.setColour (accent.withAlpha (0.55f));
        for (float db = kSpecTopDb; db >= kSpecBottomDb; db -= 24.0f)
            g.drawText (juce::String ((int) db), juce::Rectangle<float> (plot.getRight() + 6.0f, specToY (db) - 7.0f, r.getRight() - plot.getRight() - 8.0f, 14.0f),
                        juce::Justification::centredLeft);
    }

    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (plot.getSmallestIntegerContainer());

        if (! inputSpectrum.isEmpty())   // before the EQ: grey, behind (where the EQ cuts, grey shows above the colour)
        {
            g.setColour (juce::Colour (0xff9aa3b5).withAlpha (0.13f));
            g.fillPath (inputSpectrum);
            g.setColour (juce::Colour (0xff9aa3b5).withAlpha (0.45f));
            g.strokePath (inputSpectrum, juce::PathStrokeType (1.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        if (! spectrum.isEmpty())
        {
            g.setGradientFill (juce::ColourGradient (accent.withAlpha (0.28f), 0.0f, plot.getY(),
                                                     accent.withAlpha (0.03f), 0.0f, plot.getBottom(), false));
            g.fillPath (spectrum);
            g.setColour (accent.withAlpha (0.5f));
            g.strokePath (spectrum, juce::PathStrokeType (1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        const bool on = eq.on->isOn();
        for (int k = 0; k < ParamEq::kBands; ++k)
        {
            if (bandCurves[(size_t) k].isEmpty() || ! eq.enabled[(size_t) k]->isOn()) continue;
            const auto bc = on ? theme::bandColour (k) : graphDim;
            const bool emphasise = k == selected || k == hovered;
            g.setColour (bc.withAlpha (emphasise ? 0.22f : 0.10f));
            g.fillPath (bandCurves[(size_t) k]);
            if (emphasise)
            {
                g.setColour (bc.withAlpha (0.6f));
                g.strokePath (bandCurves[(size_t) k], juce::PathStrokeType (1.0f));
            }
        }
        g.setColour (on ? theme::text.withAlpha (0.92f) : theme::textFaint);
        g.strokePath (curve, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Points: only the bands in use
    bool any = false;
    const float rad = large ? 8.0f : 7.0f;
    for (int k = 0; k < ParamEq::kBands; ++k)
    {
        if (! eq.bandOn[(size_t) k]->isOn()) continue;
        any = true;
        const auto p = nodePos (k);
        const bool bandEnabled = eq.enabled[(size_t) k]->isOn();
        const auto bc = eq.on->isOn() ? theme::bandColour (k) : graphDim;
        auto node = juce::Rectangle<float> (p.x - rad, p.y - rad, rad * 2.0f, rad * 2.0f);
        if (k == selected || k == hovered)
        {
            g.setColour (bc.withAlpha (k == selected ? 0.3f : 0.18f));
            g.fillEllipse (node.expanded (5.0f));
        }
        if (bandEnabled)
        {
            g.setColour (bc);
            g.fillEllipse (node);
        }
        else   // switched off: hollow
        {
            g.setColour (graphBg);
            g.fillEllipse (node);
            g.setColour (bc.withAlpha (0.7f));
            g.drawEllipse (node.reduced (0.75f), 1.5f);
        }
        if (k == selected)
        {
            g.setColour (theme::text);
            g.drawEllipse (node.expanded (1.5f), 1.5f);
        }
        g.setColour (bandEnabled ? graphBg : bc);
        g.setFont (theme::font (large ? 10.5f : 9.5f, true));
        g.drawText (juce::String (k + 1), node, juce::Justification::centred);
    }
    if (ghostBand >= 0)
    {
        const auto bc = theme::bandColour (ghostBand);
        auto node = juce::Rectangle<float> (ghostPos.x - rad, ghostPos.y - rad, rad * 2.0f, rad * 2.0f);
        g.setColour (bc.withAlpha (0.22f));
        g.fillEllipse (node);
        g.setColour (bc.withAlpha (0.7f));
        g.drawEllipse (node, 1.2f);
        g.setFont (theme::font (large ? 10.5f : 9.5f, true));
        g.drawText (juce::String (ghostBand + 1), node, juce::Justification::centred);
    }
    if (! any && ghostBand < 0)
    {
        g.setColour (theme::textFaint);
        g.setFont (theme::font (large ? 13.0f : 11.0f));
        g.drawText ("Click to add a band", plot.withTrimmedBottom (plot.getHeight() * 0.5f + 8.0f), juce::Justification::centredBottom);
    }
}

int EqGraph::hitBand (juce::Point<float> p) const
{
    int best = -1;
    float bestD = large ? 18.0f : 16.0f;
    for (int k = 0; k < ParamEq::kBands; ++k)
    {
        if (! eq.bandOn[(size_t) k]->isOn()) continue;
        const float d = nodePos (k).getDistanceFrom (p);
        if (d < bestD) { bestD = d; best = k; }
    }
    return best;
}

int EqGraph::addBand (float f, float gainDb)
{
    for (int k = 0; k < ParamEq::kBands; ++k)
    {
        const auto i = (size_t) k;
        if (eq.bandOn[i]->isOn()) continue;
        eq.type[i]->set ((float) ParamEq::Bell);
        eq.q[i]->set (1.0f);
        eq.freq[i]->set (f);
        eq.gain[i]->set (std::round (gainDb * 10.0f) / 10.0f);
        eq.enabled[i]->set (1.0f);
        eq.bandOn[i]->set (1.0f);
        if (! eq.on->isOn()) eq.on->set (1.0f);
        return k;
    }
    return -1;
}

void EqGraph::removeBand (int k)
{
    const auto i = (size_t) k;
    eq.bandOn[i]->set (0.0f);
    for (auto* p : { eq.type[i], eq.gain[i], eq.freq[i], eq.q[i], eq.enabled[i] }) p->reset();
    if (hovered == k) hovered = -1;
    if (selected == k)
        for (int j = 0; j < ParamEq::kBands; ++j)
            if (eq.bandOn[(size_t) j]->isOn()) { setSelectedBand (j); break; }
    repaint();
}

void EqGraph::showBandMenu (int k)
{
    juce::PopupMenu m;
    m.addSectionHeader ("Band " + juce::String (k + 1));
    const auto names = ParamEq::typeNames();
    const int cur = juce::roundToInt (eq.type[(size_t) k]->get());
    juce::Component::SafePointer<EqGraph> safe (this);
    for (int t = 0; t < names.size(); ++t)
        m.addItem (names[t], true, t == cur, [safe, k, t] { if (safe != nullptr) safe->eq.type[(size_t) k]->set ((float) t); });
    m.addSeparator();
    const bool en = eq.enabled[(size_t) k]->isOn();
    m.addItem (en ? "Switch band off" : "Switch band on", [safe, k, en] { if (safe != nullptr) safe->eq.enabled[(size_t) k]->set (en ? 0.0f : 1.0f); });
    m.addItem ("Remove band", [safe, k] { if (safe != nullptr) safe->removeBand (k); });
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
}

juce::Rectangle<float> EqGraph::ghostBounds() const
{
    return juce::Rectangle<float> (ghostPos, ghostPos).expanded (12.0f);
}

void EqGraph::setGhost (int band, juce::Point<float> pos)
{
    if (band == ghostBand && (band < 0 || pos == ghostPos)) return;
    if (ghostBand >= 0) repaint (ghostBounds().getSmallestIntegerContainer());
    ghostBand = band;
    ghostPos = pos;
    if (ghostBand >= 0) repaint (ghostBounds().getSmallestIntegerContainer());
    else repaint(); // the "Click to add a band" hint may come back
}

void EqGraph::mouseMove (const juce::MouseEvent& e)
{
    const int k = hitBand (e.position);
    if (k != hovered) { hovered = k; repaint(); }

    // Ghost point where a click would add the next free band: only near the curve, stuck to it
    // at the mouse's horizontal position.
    int freeBand = -1;
    juce::Point<float> pos;
    if (k < 0 && plot.contains (e.position))
    {
        pos = { e.position.x, dbToY (juce::jlimit (-18.0f, 18.0f, curveDbAt (xToFreq (e.position.x)))) };
        if (std::abs (pos.y - e.position.y) <= (large ? 14.0f : 11.0f))
            for (int j = 0; j < ParamEq::kBands && freeBand < 0; ++j)
                if (! eq.bandOn[(size_t) j]->isOn()) freeBand = j;
    }
    setGhost (freeBand, pos);
}

void EqGraph::mouseExit (const juce::MouseEvent&)
{
    if (hovered >= 0) { hovered = -1; repaint(); }
    setGhost (-1, {});
}

void EqGraph::mouseDown (const juce::MouseEvent& e)
{
    const bool onGhost = ghostBand >= 0;
    const auto ghostAt = ghostPos;
    setGhost (-1, {});
    int k = hitBand (e.position);
    if (e.mods.isPopupMenu())
    {
        if (k >= 0) { setSelectedBand (k); showBandMenu (k); }
        return;
    }
    if (k < 0)
    {
        if (! plot.contains (e.position)) return;
        const auto at = onGhost ? ghostAt : e.position; // on the curve: the band lands where the ghost was
        k = addBand (xToFreq (at.x), juce::jlimit (-18.0f, 18.0f, yToDb (at.y)));
        if (k < 0) return; // all bands in use
        justAdded = k;
        justAddedTime = juce::Time::getMillisecondCounterHiRes();
    }
    setSelectedBand (k);
    dragging = true;
    dragStartQ = eq.q[(size_t) k]->getNormalised();
}

void EqGraph::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging) return;
    const auto k = (size_t) selected;
    eq.freq[k]->set (xToFreq (juce::jlimit (plot.getX(), plot.getRight(), e.position.x)));
    if (ParamEq::typeUsesGain (juce::roundToInt (eq.type[k]->get())))
        eq.gain[k]->set (yToDb (e.position.y));
    else
        eq.q[k]->setNormalised (dragStartQ - (float) e.getDistanceFromDragStartY() / juce::jmax (1.0f, plot.getHeight()));
    if (! eq.on->isOn()) eq.on->set (1.0f);
}

void EqGraph::mouseDoubleClick (const juce::MouseEvent& e)
{
    // The first click of a double-click on an empty spot already added this band - keep it.
    const int k = hitBand (e.position);
    if (k < 0 || (k == justAdded && juce::Time::getMillisecondCounterHiRes() - justAddedTime < 600.0)) return;
    dragging = false;
    removeBand (k);
}

void EqGraph::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    int k = hitBand (e.position);
    if (k < 0) k = selected;
    if (! eq.bandOn[(size_t) k]->isOn()) return;
    auto* q = eq.q[(size_t) k];
    q->setNormalised (q->getNormalised() + (w.deltaY > 0 ? 0.03f : -0.03f));
}

//==============================================================================
/** One band in the large EQ editor (in the band's colour): type picker, gain / freq / Q with
    permanent readouts and "Remove". An unused band shows a placeholder that adds it. */
struct EqEditor::BandColumn final : public juce::Component, private ParamPoller::Client
{
    BandColumn (EqEditor& o, int k)
        : owner (o), band (k), colour (theme::bandColour (k)),
          typeBar (o.ctx, *o.eq.type[(size_t) k], colour),
          gain (o.ctx, *o.eq.gain[(size_t) k], "GAIN", colour),
          freq (o.ctx, *o.eq.freq[(size_t) k], "FREQ", colour),
          q (o.ctx, *o.eq.q[(size_t) k], "Q", colour),
          enabled (o.ctx, *o.eq.enabled[(size_t) k], "ON", colour)
    {
        for (juce::Component* c : std::initializer_list<juce::Component*> { &typeBar, &removeChip, &gain, &freq, &q, &enabled })
            addAndMakeVisible (c);
        for (auto* kn : { &gain, &freq, &q }) kn->alwaysShowValue = true;
        enabled.setTooltip ("Switch this band on or off (off keeps its settings)");
        removeChip.showArrow = false;
        removeChip.centred = true;
        removeChip.setTooltip ("Remove this band");
        removeChip.onClick = [this]
        {
            juce::Component::SafePointer<EqEditor> o (&owner);
            const int b = band;
            juce::MessageManager::callAsync ([o, b] { if (o != nullptr) o->graph.removeBand (b); });
        };
        addMouseListener (this, true); // any click in the column selects the band
        ParamPoller::attach (this);
        pollParams();
    }

    ~BandColumn() override { ParamPoller::detach (this); }

    bool inUse() const { return owner.eq.bandOn[(size_t) band]->isOn(); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (inUse()) { owner.selectBand (band); return; }
        if (e.eventComponent == this)
        {
            if (const int k = owner.graph.addBand (ParamEq::defaultFreq (band), 0.0f); k >= 0) owner.selectBand (k);
        }
    }

    void pollParams() override
    {
        const auto k = (size_t) band;
        const auto v = owner.eq.type[k]->getVersion() + owner.eq.gain[k]->getVersion() + owner.eq.freq[k]->getVersion()
                     + owner.eq.q[k]->getVersion() + owner.eq.bandOn[k]->getVersion() + owner.eq.enabled[k]->getVersion();
        if (v == seen) return;
        seen = v;
        const bool use = inUse();
        for (juce::Component* c : std::initializer_list<juce::Component*> { &typeBar, &removeChip, &gain, &freq, &q, &enabled })
            c->setVisible (use);
        if ((use ? 1 : 0) != shownUse)
        {
            const bool first = shownUse < 0;
            shownUse = use ? 1 : 0;
            if (! first) owner.layoutBands();   // added / removed: only the bands in use have a column
        }
        const float dim = owner.eq.enabled[k]->isOn() ? 1.0f : 0.45f;
        for (juce::Component* c : std::initializer_list<juce::Component*> { &typeBar, &freq, &q }) c->setAlpha (dim);
        gain.setAlpha (dim * (ParamEq::typeUsesGain (juce::roundToInt (owner.eq.type[k]->get())) ? 1.0f : 0.35f));
        setMouseCursor (use ? juce::MouseCursor::NormalCursor : juce::MouseCursor::PointingHandCursor);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const bool use = inUse();
        const bool sel = use && owner.graph.getSelectedBand() == band;
        auto r = getLocalBounds().toFloat().reduced (3.0f);
        g.setColour (use ? theme::panel2 : theme::panel2.withAlpha (0.45f));
        g.fillRoundedRectangle (r, 8.0f);
        g.setColour (sel ? colour.withAlpha (0.85f) : use ? colour.withAlpha (0.25f) : theme::outline.withAlpha (0.4f));
        g.drawRoundedRectangle (r, 8.0f, sel ? 1.5f : 1.0f);

        auto head = getLocalBounds().reduced (12, 8).removeFromTop (22);
        g.setColour (use ? colour : theme::textFaint);
        g.fillEllipse (head.removeFromLeft (10).withSizeKeepingCentre (10, 10).toFloat());
        head.removeFromLeft (8);
        g.setFont (theme::font (12.5f, true));
        g.drawText ("BAND " + juce::String (band + 1), head, juce::Justification::centredLeft);

        if (! use)
        {
            g.setColour (theme::textFaint);
            g.setFont (theme::font (12.0f));
            g.drawText ("+  Add band", getLocalBounds(), juce::Justification::centred);
            return;
        }
        g.setColour (theme::textDim);
        g.setFont (theme::font (11.5f));
        g.drawText (owner.eq.type[(size_t) band]->toText(), head.withTrimmedLeft (58).withTrimmedRight (110), juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12, 8);
        auto head = r.removeFromTop (22);
        removeChip.setBounds (head.removeFromRight (30));
        head.removeFromRight (4);
        enabled.setBounds (head.removeFromRight (42));
        r.removeFromTop (8);
        typeBar.setBounds (r.removeFromLeft (36));
        r.removeFromLeft (8);
        // Knob size and spacing in the proportions of the small EQ, centred when the column is wider.
        const int w = juce::jmin (r.getWidth() / 3, 68);
        auto knobs = r.withSizeKeepingCentre (w * 3, juce::jmin (r.getHeight(), 96));
        gain.setBounds (knobs.removeFromLeft (w));
        freq.setBounds (knobs.removeFromLeft (w));
        q.setBounds (knobs);
    }

    EqEditor& owner;
    int band;
    juce::Colour colour;
    EqTypeBar typeBar;
    TrashButton removeChip;
    Knob gain, freq, q;
    Toggle enabled;
    uint64_t seen = ~0ull;
    int shownUse = -1;
};

EqEditor::EqEditor (UiContext& c, int sel, int k)
    : ctx (c), selectIndex (sel), instance (k),
      eq (sel < kMaxInputs ? c.engine.inputs[(size_t) sel].eqs[(size_t) k] : c.engine.buses[(size_t) (sel - kMaxInputs)].eqs[(size_t) k]),
      colour (sel < kMaxInputs ? theme::accentIn : theme::busColour (sel - kMaxInputs)),
      onToggle (c, *eq.on, "EQ ON", theme::fxEq), graph (c, eq, colour, true),
      tap (sel < kMaxInputs ? c.engine.inputs[(size_t) sel].spectrum : c.engine.buses[(size_t) (sel - kMaxInputs)].spectrum),
      fft (std::make_unique<juce::dsp::FFT> (kFftOrder))
{
    after.history.assign (kFftSize, 0.0f);
    before.history.assign (kFftSize, 0.0f);
    fftData.assign (kFftSize * 2, 0.0f);
    window.resize (kFftSize);
    for (int i = 0; i < kFftSize; ++i) // Hann
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (kFftSize - 1));
    tap.eqInstance.store (k);
    tap.ring.skip (tap.ring.available()); // stale audio from an earlier view
    tap.users.fetch_add (1);

    title = sel < kMaxInputs ? "IN " + juce::String (sel + 1) + "   " + c.engine.inputs[(size_t) sel].name
                             : busLabel (sel - kMaxInputs) + "   " + c.engine.buses[(size_t) (sel - kMaxInputs)].name;
    if (k > 0) title << "   -   EQ " << (k + 1);
    addAndMakeVisible (onToggle);
    addAndMakeVisible (graph);
    addAndMakeVisible (menu);
    addAndMakeVisible (info);
    menu.showArrow = false;
    menu.centred = true;
    menu.setTooltip ("Import or export this EQ, remove all bands");
    menu.onClick = [this] { showEqMenu (ctx, selectIndex, instance, menu); };
    info.setText ("Using the EQ\nClick: add a band.  Drag a point: frequency / gain.  Mouse wheel: Q.\n"
                  "Double-click a point: remove it.  Right-click: type, switch off, remove.\n"
                  "Click a value under a knob to type it (e.g. 2.5k, -3).  Ctrl+Z / Ctrl+Y: undo / redo.\n\n"
                  "Background: live spectrum after the EQ (dBFS scale on the right). \"Show input\" adds the spectrum "
                  "before the EQ in grey behind it, so you see what the EQ changes.");
    showInput.setTooltip ("Grey spectrum of the signal before this EQ, behind the coloured one (after the EQ)");
    showInput.onClick = [this]
    {
        if (ctx.session != nullptr) { ctx.session->prefs.eqShowInput = showInput.getToggleState(); ctx.session->prefsVersion.fetch_add (1); }
        setShowInput (showInput.getToggleState());
    };
    addAndMakeVisible (showInput);
    showInput.setToggleState (ctx.session != nullptr && ctx.session->prefs.eqShowInput, juce::dontSendNotification);
    setShowInput (showInput.getToggleState());
    for (int k = 0; k < ParamEq::kBands; ++k)
    {
        bands.push_back (std::make_unique<BandColumn> (*this, k));
        addAndMakeVisible (*bands.back());
    }
    graph.onBandSelected = [this] (int) { for (auto& b : bands) b->repaint(); };
    setWantsKeyboardFocus (true);
    for (int k = 0; k < ParamEq::kBands; ++k) // start on the first band in use
        if (eq.bandOn[(size_t) k]->isOn()) { graph.setSelectedBand (k); break; }
    setSize (1240, 820);
}

EqEditor::~EqEditor()
{
    setShowInput (false);
    tap.users.fetch_sub (1);
}

void EqEditor::setShowInput (bool on)
{
    if (on == inputShown) return;
    inputShown = on;
    if (on)
    {
        tap.preRing.skip (tap.preRing.available());   // stale audio from an earlier view
        std::fill (before.history.begin(), before.history.end(), 0.0f);
        before.smoothed.clear();
        tap.preUsers.fetch_add (1);
    }
    else
    {
        tap.preUsers.fetch_sub (1);
        graph.setInputSpectrum ({});
    }
}

void EqEditor::analyse (double t)
{
    if (t - lastFrame < 1.0 / 66.0) return; // high-refresh displays: stay at ~60 Hz
    const float dt = lastFrame > 0.0 ? (float) juce::jlimit (0.0, 0.2, t - lastFrame) : 1.0f / 60.0f;
    lastFrame = t;
    if (inputShown && runAnalyzer (before, tap.preRing, dt)) graph.setInputSpectrum (before.columns);
    if (runAnalyzer (after, tap.ring, dt)) graph.setSpectrum (after.columns);
}

bool EqEditor::runAnalyzer (Analyzer& a, SpscAudioRing& ring, float dt)
{
    auto& history = a.history;
    auto& historyPos = a.historyPos;
    // 1. Collect the new audio (mono sum) into a sliding history of kFftSize samples.
    for (;;)
    {
        auto* const* ch = readBuf.getArrayOfWritePointers();
        const int got = ring.read (ch, 2, readBuf.getNumSamples());
        if (got <= 0) break;
        for (int i = 0; i < got; ++i)
        {
            history[(size_t) historyPos] = 0.5f * (ch[0][i] + ch[1][i]);
            historyPos = (historyPos + 1) & (kFftSize - 1);
        }
    }

    // 2. Windowed FFT of the latest kFftSize samples.
    for (int i = 0; i < kFftSize; ++i)
        fftData[(size_t) i] = history[(size_t) ((historyPos + i) & (kFftSize - 1))] * window[(size_t) i];
    std::fill (fftData.begin() + kFftSize, fftData.end(), 0.0f);
    fft->performFrequencyOnlyForwardTransform (fftData.data(), true);

    // 3. One dB value per graph column (log frequency), +3 dB/oct tilt like most analyzers.
    const auto plot = graph.getPlotArea();
    const int numCols = (int) (plot.getWidth() / (float) EqGraph::kSpectrumStep) + 1;
    if (numCols < 8) return false;
    auto& smoothed = a.smoothed;
    auto& raw = a.raw;
    auto& columns = a.columns;
    if ((int) smoothed.size() != numCols) smoothed.assign ((size_t) numCols, -120.0f);
    raw.resize ((size_t) numCols);
    columns.resize ((size_t) numCols);

    const double sr = juce::jmax (8000.0, ctx.engine.getSampleRate());
    const float binHz = (float) sr / (float) kFftSize;
    const float scale = 4.0f / (float) kFftSize; // amplitude of a full-scale sine -> 1.0 (Hann gain 0.5)
    auto bin = [this] (float k)
    {
        const int k0 = juce::jlimit (1, kFftSize / 2 - 2, (int) k);
        const float frac = juce::jlimit (0.0f, 1.0f, k - (float) k0);
        return fftData[(size_t) k0] + frac * (fftData[(size_t) k0 + 1] - fftData[(size_t) k0]);
    };

    for (int c = 0; c < numCols; ++c)
    {
        const float f0 = graph.xToFreq (plot.getX() + (float) (c * EqGraph::kSpectrumStep));
        const float f1 = graph.xToFreq (plot.getX() + (float) ((c + 1) * EqGraph::kSpectrumStep));
        const float k0 = f0 / binHz, k1 = f1 / binHz;
        float mag = 0.0f;
        if (k1 - k0 < 1.0f)
            mag = bin (0.5f * (k0 + k1));
        else
            for (int k = (int) k0; k <= (int) k1 && k < kFftSize / 2; ++k)
                mag = juce::jmax (mag, fftData[(size_t) k]);
        const float fc = std::sqrt (f0 * f1);
        raw[(size_t) c] = 20.0f * std::log10 (mag * scale + 1.0e-9f) + 3.0f * std::log2 (fc / 1000.0f);
    }

    // 4. Smooth across frequency (~1/12 octave, triangular) and over time (fast rise, slow fall).
    const int radius = 3;
    const float up = 1.0f - std::exp (-dt / 0.02f), down = 1.0f - std::exp (-dt / 0.28f);
    for (int c = 0; c < numCols; ++c)
    {
        float sum = 0.0f, wsum = 0.0f;
        for (int d = -radius; d <= radius; ++d)
        {
            const int i = juce::jlimit (0, numCols - 1, c + d);
            const float w = (float) (radius + 1 - std::abs (d));
            sum += raw[(size_t) i] * w;
            wsum += w;
        }
        const float target = sum / wsum;
        auto& sm = smoothed[(size_t) c];
        sm += (target - sm) * (target > sm ? up : down);
        columns[(size_t) c] = sm;
    }
    return true;
}

void EqEditor::selectBand (int b)
{
    graph.setSelectedBand (b);
}

void EqEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);
    auto head = getLocalBounds().reduced (18, 0).removeFromTop (44);
    g.setColour (colour);
    g.setFont (theme::font (15.0f, true));
    g.drawText (title, head, juce::Justification::centredLeft);
}

void EqEditor::resized()
{
    auto r = getLocalBounds().reduced (14, 0);
    auto head = r.removeFromTop (44);
    onToggle.setBounds (head.removeFromRight (96).withSizeKeepingCentre (96, 26));
    head.removeFromRight (8);
    menu.setBounds (head.removeFromRight (90).withSizeKeepingCentre (90, 26));
    head.removeFromRight (10);
    info.setBounds (head.removeFromRight (InfoButton::kSize).withSizeKeepingCentre (InfoButton::kSize, InfoButton::kSize));
    head.removeFromRight (14);
    showInput.setBounds (head.removeFromRight (110).withSizeKeepingCentre (110, 24));
    layoutBands();
}

void EqEditor::layoutBands()
{
    // Only the bands in use get a column (16 would not fit), plus one "+ Add band" column while a band is free.
    // As many side by side as fit (at most 8), further rows below; the curve takes the rest. They glide there.
    auto r = getLocalBounds().reduced (14, 0).withTrimmedTop (44);
    std::vector<BandColumn*> shown;
    bool addShown = false;
    for (auto& b : bands)
    {
        const bool use = b->inUse();
        const bool show = use || ! addShown;
        if (! use && show) addShown = true;
        if (show) shown.push_back (b.get());
        else { animator.forget (*b); b->setVisible (false); }
    }
    const int n = juce::jmax (1, (int) shown.size());
    const int fullRow = juce::jlimit (1, 8, r.getWidth() / 190);
    const int perRow = juce::jmin (fullRow, n);
    const int rows = (n + perRow - 1) / perRow;
    const int rowH = juce::jlimit (140, 230, r.getHeight() / (rows + 2));
    auto bottom = r.removeFromBottom (rowH * rows + 10).withTrimmedBottom (10);
    r.removeFromBottom (8);
    animator.moveTo (graph, r);
    // Columns keep the width they would have in a full row, so they do not change size as bands come and go.
    const int w = bottom.getWidth() / fullRow;
    for (int k = 0; k < (int) shown.size(); ++k)
    {
        auto* c = shown[(size_t) k];
        const int inRow = juce::jmin (perRow, n - (k / perRow) * perRow);
        const int x0 = bottom.getX() + (bottom.getWidth() - inRow * w) / 2;   // a row centred
        const juce::Rectangle<int> target (x0 + (k % perRow) * w, bottom.getY() + (k / perRow) * rowH, w, rowH);
        if (! c->isVisible() || c->getBounds().isEmpty()) { animator.place (*c, target); c->setVisible (true); }
        else animator.moveTo (*c, target);
    }
}

//==============================================================================
//==============================================================================
juce::Image renderFxBlock (UiContext& ctx, const juce::var& d, float pixelScale, int& logicalWidth)
{
    // The block as it would look in the chain of the selected channel (a built-in effect: a free instance of it,
    // so its defaults), at the height of the inspector's chain.
    const int h = juce::jmax (120, gRackHeight);
    const auto kind = d.getProperty ("hydraFx", {}).toString();
    const auto fxIdStr = d.getProperty ("fx", {}).toString();
    int ch = ctx.selected;
    if (ch < 0 || ch >= kRecorderSelect) ch = 0;
    const auto colour = ch < kMaxInputs ? theme::accentIn : theme::busColour (ch - kMaxInputs);
    auto snapshot = [&] (juce::Component& c, int w)
    {
        logicalWidth = w;
        c.setBounds (0, 0, w, h);
        auto img = c.createComponentSnapshot (c.getLocalBounds(), true, pixelScale).convertedToFormat (juce::Image::ARGB);
        img.multiplyAllAlphas (0.92f);
        return img;
    };
    if ((kind == "builtin" || kind == "preset") && fxIdStr.isNotEmpty())
    {
        const auto f = fxFromId (fxIdStr);
        const auto& chain = ctx.engine.chainFor (ch);
        int k = 0;
        for (int i = 0; i < Engine::maxInstances (f); ++i) if (! chain.hasBuiltin (f, i)) { k = i; break; }
        const auto title = kind == "preset" ? d.getProperty ("name", {}).toString() : fxName (f);
        if (auto made = makeBuiltinUnit (ctx, ch, f, k, colour, title))
        {
            made->section->enableChainControls ([] {});
            const int w = juce::jmax (made->width, made->section->minimumWidth());
            return snapshot (*made->comp, w);
        }
        return {};
    }
    // A plugin or a chain: a card like the plugin cards.
    struct Card final : juce::Component
    {
        juce::String tag, name, sub;
        juce::Colour accent;
        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat().reduced (3.0f);
            if (theme::deep()) theme::drawCard (g, r, theme::panel2, accent.withAlpha (0.6f), accent);
            else { g.setColour (theme::panel2); g.fillRoundedRectangle (r, 8.0f); g.setColour (accent.withAlpha (0.6f)); g.drawRoundedRectangle (r, 8.0f, 1.0f); }
            drawGrip (g, { r.getX() + 9.0f, r.getY() + 9.0f, 8.0f, 14.0f });
            auto t = r.reduced (10.0f, 7.0f).withTrimmedLeft (14.0f);
            g.setColour (theme::textFaint);
            g.setFont (theme::font (11.0f, true));
            g.drawText (tag, t.removeFromTop (16.0f), juce::Justification::centredLeft);
            t.removeFromTop (4.0f);
            g.setColour (theme::text);
            g.setFont (theme::font (13.0f, true));
            g.drawFittedText (name, t.removeFromTop (18.0f).toNearestInt(), juce::Justification::centredLeft, 1);
            g.setColour (theme::textDim);
            g.setFont (theme::font (11.0f));
            g.drawFittedText (sub, t.removeFromTop (15.0f).toNearestInt(), juce::Justification::centredLeft, 1);
        }
    } card;
    card.accent = colour;
    card.name = d.getProperty ("name", {}).toString();
    if (kind == "plugin") { card.tag = "VST3"; }
    else if ((bool) d.getProperty ("chain", false)) { card.tag = "EFFECT CHAIN"; card.sub = "preset"; card.accent = theme::fx; }
    else { card.tag = "VST3 PRESET"; }
    return snapshot (card, 190);
}

Inspector::Inspector (UiContext& c) : ctx (c) {}
Inspector::~Inspector() = default;

void Inspector::showChannel (int sel)
{
    if (sel == current && content != nullptr) return;
    current = sel;
    auto scan = [this] { if (onScanRequested) onScanRequested(); };
    bypass.reset();
    post.reset();
    idleShown = channelIdle();
    if (sel == kRecorderSelect) content = std::make_unique<RecorderDetail> (ctx);
    else
    {
        content = std::make_unique<FxRack> (ctx, sel, scan);
        auto& p = sel < kMaxInputs ? *ctx.engine.inputs[(size_t) sel].fxBypass : *ctx.engine.buses[(size_t) (sel - kMaxInputs)].fxBypass;
        bypass = std::make_unique<Toggle> (ctx, p, "BYPASS", theme::mute);
        bypass->setTooltip ("Bypass the whole effect chain of this channel (the blocks are dimmed meanwhile)");
        addAndMakeVisible (*bypass);
        auto& pp = sel < kMaxInputs ? *ctx.engine.inputs[(size_t) sel].fxPost : *ctx.engine.buses[(size_t) (sel - kMaxInputs)].fxPost;
        post = std::make_unique<Toggle> (ctx, pp, "PRE", sel < kMaxInputs ? theme::accentIn : theme::busColour (sel - kMaxInputs));
        post->onText = "POST";
        post->setTooltip (sel < kMaxInputs
            ? "PRE: the effect chain runs before the fader and pan (default).\nPOST: after them, so the effects get the level the fader sets. Click to switch."
            : "PRE: the effect chain runs before mono and the fader (default).\nPOST: after them, e.g. a limiter that also catches a fader above 0 dB. Click to switch.");
        addAndMakeVisible (*post);
    }
    addAndMakeVisible (*content);
    resized();
    repaint();
}

juce::String Inspector::shownName() const
{
    if (current < 0 || current >= kRecorderSelect) return {};
    return current < kMaxInputs ? ctx.engine.inputs[(size_t) current].name : ctx.engine.buses[(size_t) (current - kMaxInputs)].name;
}

void Inspector::layoutHeader()
{
    if (bypass == nullptr) return;
    // Right after the channel's name (it can be renamed, so this follows it).
    auto title = getLocalBounds().reduced (14, 8).removeFromTop (20);
    const int nameW = juce::jmin (300, juce::GlyphArrangement::getStringWidthInt (theme::font (13.0f, true), shownName()));
    bypass->setBounds (title.getX() + codeWidth() + nameW + 14, title.getY(), 74, title.getHeight());
    if (post != nullptr) post->setBounds (bypass->getRight() + 6, title.getY(), 54, title.getHeight());
}

int Inspector::codeWidth() const
{
    // The channel's code ("IN 3", "A2"), then the name right after it.
    return juce::GlyphArrangement::getStringWidthInt (theme::font (13.0f, true), channelCode (current)) + 8;
}

void Inspector::paint (juce::Graphics& g)
{
    g.setColour (theme::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (2.0f), 10.0f);
    auto title = getLocalBounds().reduced (14, 8).removeFromTop (20);
    if (current == kRecorderSelect)
    {
        g.setColour (theme::mute);
        g.setFont (theme::font (13.0f, true));
        g.drawText ("REC", title.removeFromLeft (50), juce::Justification::centredLeft);
        g.setColour (theme::text);
        g.drawText ("Recorder", title.removeFromLeft (300), juce::Justification::centredLeft);
        return;
    }
    const bool isBus = current >= kMaxInputs;
    const int idx = isBus ? current - kMaxInputs : current;
    const auto colour = isBus ? theme::busColour (idx) : theme::accentIn;
    const auto name = isBus ? ctx.engine.buses[(size_t) idx].name : ctx.engine.inputs[(size_t) idx].name;
    g.setColour (colour);
    g.setFont (theme::font (13.0f, true));
    g.drawText (channelCode (current), title.removeFromLeft (codeWidth()), juce::Justification::centredLeft);
    g.setColour (theme::text);
    g.drawText (name, title.removeFromLeft (300), juce::Justification::centredLeft);
    if (idleShown && bypass != nullptr)   // the engine skips this chain while the channel cannot be heard
    {
        g.setColour (theme::textFaint);
        g.setFont (theme::font (11.0f));
        g.drawText (isBus ? "effects paused: output not heard" : "effects paused: channel not heard",
                    getLocalBounds().withLeft ((post != nullptr ? post->getRight() : bypass->getRight()) + 10).withY (bypass->getY()).withHeight (bypass->getHeight()),
                    juce::Justification::centredLeft, true);
    }
}

bool Inspector::channelIdle() const
{
    if (current < 0 || current >= kRecorderSelect) return false;
    return current < kMaxInputs ? ctx.engine.inputs[(size_t) current].fxIdle.load (std::memory_order_relaxed)
                                : ctx.engine.buses[(size_t) (current - kMaxInputs)].fxIdle.load (std::memory_order_relaxed);
}

void Inspector::resized()
{
    layoutHeader();
    if (content != nullptr)
        content->setBounds (getLocalBounds().reduced (8, 6).withTrimmedTop (24));
}

void Inspector::updateMeters()
{
    layoutHeader();
    if (const bool idle = channelIdle(); idle != idleShown)
    {
        idleShown = idle;
        repaint (getLocalBounds().removeFromTop (32));
    }
    if (auto* d = dynamic_cast<FxRack*> (content.get()))
        d->updateMeters();
    else if (auto* rd = dynamic_cast<RecorderDetail*> (content.get()))
        rd->updateMeters();
}
} // namespace hydra

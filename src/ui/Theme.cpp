#include "Theme.h"
#include "ChannelIcons.h"

namespace hydra::theme
{
void apply (Style s)
{
    style = s;
    const bool d = s != Style::legacy;
    // Dark: slate with about half the blue of the earlier palette (user 13.11.: "too blue"), a step lighter.
    // Darker: graphite, only a hint of blue. Legacy keeps its own colours.
    struct Palette { juce::uint32 bg, panel, panel2, raised, outline, textDim; };
    static constexpr Palette legacyP { 0xff0d0f13, 0xff151920, 0xff1b2029, 0xff232935, 0xff2a313e, 0xff8b94a5 };
    static constexpr Palette darkP   { 0xff111317, 0xff1a1d22, 0xff21252b, 0xff2a2f36, 0xff30353e, 0xff929aa7 };
    static constexpr Palette darkerP { 0xff0d0d0f, 0xff17181b, 0xff1d1e22, 0xff25272b, 0xff2c2e33, 0xff8f949e };
    // Light ("Nebel", user 13.11.): bg = the gaps, panel = strips, panel2 = cards / selected, raised = buttons.
    static constexpr Palette lightP  { 0xffe3e7ed, 0xfff5f6f8, 0xffffffff, 0xffe4e8ee, 0xffd3d9e1, 0xff586172 };
    const bool l = s == Style::light;
    const auto& p = s == Style::dark ? darkP : s == Style::darker ? darkerP : l ? lightP : legacyP;
    bg       = juce::Colour (p.bg);
    panel    = juce::Colour (p.panel);
    panel2   = juce::Colour (p.panel2);
    raised   = juce::Colour (p.raised);
    outline  = juce::Colour (p.outline);
    cardLine = l ? outline.withAlpha (0.8f) : d ? juce::Colours::transparentBlack : outline.withAlpha (0.6f);   // light: white cards need an edge
    textDim  = juce::Colour (p.textDim);
    text      = juce::Colour (l ? 0xff1b2130 : 0xffe8ebf1);
    textFaint = juce::Colour (l ? 0xff8a93a3 : 0xff5a6273);

    // Accents: on a light background the bright neon tones lose contrast, so they get darker, deeper versions.
    auto pick = [l] (juce::uint32 dark, juce::uint32 bright) { return juce::Colour (l ? bright : dark); };
    accentIn     = pick (0xff35d0f4, 0xff0e9cc4);
    accentHw     = l ? juce::Colour (0xff7452ea) : juce::Colour (d ? 0xffa288ff : 0xff8d6cff);
    accentVirt   = pick (0xffff6aa6, 0xffdb4687);
    mute         = pick (0xffff4d5e, 0xffe0374a);
    solo         = pick (0xffffc23f, 0xffd99410);
    good         = pick (0xff3ee08f, 0xff17a862);
    warn         = pick (0xffffa53d, 0xffe0811a);
    learn        = pick (0xffffd400, 0xffd4a800);
    fxEq         = pick (0xff35d0f4, 0xff0e9cc4);
    fxGate       = pick (0xffffc23f, 0xffd18e0a);
    fxComp       = pick (0xffff8f4d, 0xffe06a22);
    fxLimiter    = pick (0xffff6a7d, 0xffde4258);
    fxCorrection = pick (0xffa78bfa, 0xff7d5ce8);
    fxUtility    = pick (0xffff6ac1, 0xffd9469d);
    fxTranspose  = pick (0xffc77dff, 0xffa055e6);
    fxStems      = pick (0xff56e0c8, 0xff16a890);
    fxHeadphone  = pick (0xff7c9cff, 0xff4f72ea);
    fxVoice      = pick (0xffb8f060, 0xff5c9a12);
    fxSaturation = pick (0xffff5a3c, 0xffd8401f);
    fxMatch      = pick (0xff5c9dff, 0xff2f6fd6);
    fx           = pick (0xff6cacfa, 0xff3b82e0);
}

Style styleFromName (const juce::String& name)
{
    if (name == "legacy") return Style::legacy;
    if (name == "darker") return Style::darker;
    if (name == "light")  return Style::light;
    return Style::dark;   // "dark", the old "default", anything unknown
}

const char* styleName (Style s)
{
    switch (s)
    {
        case Style::legacy: return "legacy";
        case Style::darker: return "darker";
        case Style::light:  return "light";
        default:            return "dark";
    }
}

void drawSelectedTop (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c, bool vertical)
{
    const float reach = vertical ? juce::jmin (70.0f, r.getWidth() * 0.5f) : juce::jmin (90.0f, r.getHeight() * 0.45f);
    // The same glow in every theme, only the colours differ (user 13.11.: "etwas weniger Glow", "in allen Themes gleich").
    if (glow)
    {
        // Light falling from the line into the card: strong right below it, gone after `end`.
        const float end = reach * 0.8f;
        juce::ColourGradient fall (c.withAlpha (0.22f), r.getX(), r.getY(),
                                   c.withAlpha (0.0f), vertical ? r.getX() + end : r.getX(), vertical ? r.getY() : r.getY() + end, false);
        fall.addColour (0.25, c.withAlpha (0.09f));
        g.setGradientFill (fall);
        g.fillRect (vertical ? r.withWidth (end) : r.withHeight (end));
    }
    else
    {
        // Without glow only a faint wash, so the selection still reads at a glance.
        g.setColour (c.withAlpha (0.05f));
        g.fillRect (r);
    }
    g.setColour (c);
    g.fillRect (vertical ? r.withWidth (kSelectedLine) : r.withHeight (kSelectedLine));
    if (glow)
    {
        // A brighter core in the line, like a lit tube.
        g.setColour (c.brighter (0.6f).withAlpha (0.45f));
        g.fillRect (vertical ? r.withWidth (1.5f).translated (1.25f, 0.0f).reduced (0.0f, 2.0f)
                             : r.withHeight (1.5f).translated (0.0f, 1.25f).reduced (2.0f, 0.0f));
    }
}

LookAndFeel::LookAndFeel() { refreshColours(); }

void LookAndFeel::refreshColours()
{
    auto scheme = juce::LookAndFeel_V4::getDarkColourScheme();
    scheme.setUIColour (ColourScheme::windowBackground, bg);
    scheme.setUIColour (ColourScheme::widgetBackground, panel2);
    scheme.setUIColour (ColourScheme::menuBackground, panel2);
    scheme.setUIColour (ColourScheme::outline, outline);
    scheme.setUIColour (ColourScheme::defaultText, text);
    scheme.setUIColour (ColourScheme::defaultFill, accentIn);
    scheme.setUIColour (ColourScheme::highlightedText, bg);
    scheme.setUIColour (ColourScheme::highlightedFill, accentIn);
    scheme.setUIColour (ColourScheme::menuText, text);
    setColourScheme (scheme);

    setColour (juce::TextButton::buttonColourId, raised);
    setColour (juce::TextButton::buttonOnColourId, accentIn);
    setColour (juce::TextButton::textColourOffId, text);
    setColour (juce::TextButton::textColourOnId, bg);
    setColour (juce::ComboBox::backgroundColourId, panel2);
    setColour (juce::ComboBox::outlineColourId, outline);
    setColour (juce::ComboBox::arrowColourId, textDim);
    // Not fully opaque: JUCE then makes the menu's window transparent, so its corners can be round.
    setColour (juce::PopupMenu::backgroundColourId, panel2.withAlpha (0.995f));
    setColour (juce::PopupMenu::highlightedBackgroundColourId, fx.withAlpha (0.2f));
    setColour (juce::PopupMenu::highlightedTextColourId, text);
    setColour (juce::PopupMenu::headerTextColourId, textDim);
    setColour (juce::TextEditor::backgroundColourId, panel2);
    setColour (juce::TextEditor::outlineColourId, outline);
    setColour (juce::TextEditor::focusedOutlineColourId, accentIn);
    setColour (juce::TextEditor::highlightColourId, accentIn.withAlpha (0.3f));
    setColour (juce::Label::textColourId, text);
    setColour (juce::ListBox::backgroundColourId, panel);
    setColour (juce::ScrollBar::thumbColourId, juce::Colour (light() ? 0xffb3bbc7 : 0xff4a5366));
    setColour (juce::TabbedComponent::backgroundColourId, panel);
    setColour (juce::TabbedComponent::outlineColourId, juce::Colours::transparentBlack);
    setColour (juce::TabbedButtonBar::tabOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::ResizableWindow::backgroundColourId, bg);
    setColour (juce::DocumentWindow::textColourId, text);
    setColour (juce::ProgressBar::foregroundColourId, accentIn);
    setColour (juce::ProgressBar::backgroundColourId, panel2);
    setColour (juce::ToggleButton::tickColourId, accentIn);
    setColour (juce::Slider::trackColourId, accentIn);
    setColour (juce::Slider::backgroundColourId, panel2);
    setColour (juce::Slider::thumbColourId, text);
    setColour (juce::ToggleButton::textColourId, text);
    setColour (juce::ToggleButton::tickDisabledColourId, textFaint);
    setColour (juce::TableHeaderComponent::backgroundColourId, panel2);
    setColour (juce::TableHeaderComponent::textColourId, textDim);
    setColour (juce::TableHeaderComponent::outlineColourId, outline);
    setColour (juce::TooltipWindow::backgroundColourId, panel2);
    setColour (juce::TooltipWindow::textColourId, text);
    setColour (juce::TooltipWindow::outlineColourId, outline);
}

void LookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    auto c = b.getToggleState() ? b.findColour (juce::TextButton::buttonOnColourId) : b.findColour (juce::TextButton::buttonColourId);
    if (! b.isEnabled()) c = c.withMultipliedAlpha (0.4f);
    else if (down) c = c.brighter (0.15f);
    else if (over) c = c.brighter (0.07f);
    g.setColour (c);
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (outline.withAlpha (0.8f));
    g.drawRoundedRectangle (r, 6.0f, 1.0f);
}

void LookAndFeel::drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box)
{
    auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f);
    g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (box.hasKeyboardFocus (true) ? accentIn : outline);
    g.drawRoundedRectangle (r, 6.0f, 1.0f);

    juce::Path p;
    const float cx = (float) w - 14.0f, cy = (float) h * 0.5f;
    p.addTriangle (cx - 4.0f, cy - 2.0f, cx + 4.0f, cy - 2.0f, cx, cy + 3.0f);
    g.setColour (textDim);
    g.fillPath (p);
}

void LookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int w, int h)
{
    const auto r = juce::Rectangle<float> ((float) w, (float) h).reduced (0.5f);
    g.setColour (panel2);
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (outline.brighter (0.15f));
    g.drawRoundedRectangle (r, 8.0f, 1.0f);
}

void LookAndFeel::drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive, bool isHighlighted,
                                     bool isTicked, bool hasSubMenu, const juce::String& itemText, const juce::String& shortcutKeyText,
                                     const juce::Drawable* icon, const juce::Colour* textColour)
{
    if (isSeparator)
    {
        g.setColour (outline);
        g.fillRect (area.reduced (8, 0).withSizeKeepingCentre (area.getWidth() - 16, 1));
        return;
    }
    const auto box = area.toFloat().reduced (1.0f, 0.5f);
    if (isHighlighted && isActive)
    {
        g.setColour (fx.withAlpha (0.2f));
        g.fillRoundedRectangle (box, 5.0f);
    }
    auto r = area.reduced (9, 0);
    const auto colour = ! isActive ? textFaint : textColour != nullptr ? *textColour : (isHighlighted ? text : text.withAlpha (0.88f));

    auto mark = r.removeFromLeft (16).toFloat();
    if (icon != nullptr)
        icon->drawWithin (g, mark.withSizeKeepingCentre (14.0f, 14.0f), juce::RectanglePlacement::centred, isActive ? 1.0f : 0.4f);
    else if (isTicked)
    {
        const auto c = mark.withSizeKeepingCentre (10.0f, 9.0f);
        juce::Path p;
        p.startNewSubPath (c.getX(), c.getCentreY());
        p.lineTo (c.getX() + c.getWidth() * 0.38f, c.getBottom());
        p.lineTo (c.getRight(), c.getY());
        g.setColour (fx);
        g.strokePath (p, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    r.removeFromLeft (4);

    if (hasSubMenu)
    {
        const auto a = r.removeFromRight (10).toFloat().withSizeKeepingCentre (5.0f, 9.0f);
        juce::Path p;
        p.startNewSubPath (a.getX(), a.getY());
        p.lineTo (a.getRight(), a.getCentreY());
        p.lineTo (a.getX(), a.getBottom());
        g.setColour (isActive ? textDim : textFaint);
        g.strokePath (p, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    g.setFont (getPopupMenuFont());
    if (shortcutKeyText.isNotEmpty())
    {
        g.setColour (textDim);
        g.drawText (shortcutKeyText, r, juce::Justification::centredRight, true);
    }
    g.setColour (colour);
    g.drawFittedText (itemText, r, juce::Justification::centredLeft, 1, 0.9f);
}

void LookAndFeel::drawPopupMenuSectionHeader (juce::Graphics& g, const juce::Rectangle<int>& area, const juce::String& sectionName)
{
    g.setColour (textDim);
    g.setFont (font (11.0f, true));
    g.drawFittedText (sectionName.toUpperCase(), area.reduced (10, 0), juce::Justification::centredLeft, 1, 0.8f);
}

void LookAndFeel::getIdealPopupMenuSectionHeaderSizeWithOptions (const juce::String& sectionName, int, int& idealWidth, int& idealHeight,
                                                                 const juce::PopupMenu::Options&)
{
    const auto f = font (11.0f, true);
    idealWidth = juce::GlyphArrangement::getStringWidthInt (f, sectionName.toUpperCase()) + 2 * 10 + 8;
    idealHeight = juce::roundToInt (f.getHeight()) + 6;
}

void LookAndFeel::getIdealPopupMenuItemSize (const juce::String& itemText, bool isSeparator, int standardMenuItemHeight, int& idealWidth, int& idealHeight)
{
    if (isSeparator)
    {
        idealWidth = 50;
        idealHeight = 9;
        return;
    }
    const auto f = getPopupMenuFont();
    idealHeight = standardMenuItemHeight > 0 ? standardMenuItemHeight : juce::roundToInt (f.getHeight() * 1.9f);
    idealWidth = juce::GlyphArrangement::getStringWidthInt (f, itemText) + 9 + 16 + 4 + 10 + 16;
}

void LookAndFeel::drawTabButton (juce::TabBarButton& b, juce::Graphics& g, bool over, bool)
{
    auto r = b.getLocalBounds().toFloat();
    const bool active = b.isFrontTab();
    g.setColour (active ? text : (over ? text.withAlpha (0.8f) : textDim));
    g.setFont (font (14.0f, active));
    g.drawText (b.getButtonText(), r, juce::Justification::centred);
    if (active)
    {
        g.setColour (accentIn);
        g.fillRoundedRectangle (r.removeFromBottom (3.0f).reduced (18.0f, 0.0f), 1.5f);
    }
}

void LookAndFeel::drawTickBox (juce::Graphics& g, juce::Component&, float x, float y, float w, float h,
                               bool ticked, bool enabled, bool over, bool)
{
    juce::Rectangle<float> r (x, y, w, h);
    r = r.withSizeKeepingCentre (juce::jmin (w, h), juce::jmin (w, h)).reduced (1.0f);
    g.setColour (ticked ? accentIn : (over ? raised.brighter (0.1f) : raised));
    if (! enabled) g.setOpacity (0.4f);
    g.fillRoundedRectangle (r, 4.0f);
    if (ticked)
    {
        juce::Path p;
        p.startNewSubPath (r.getX() + r.getWidth() * 0.25f, r.getCentreY());
        p.lineTo (r.getX() + r.getWidth() * 0.43f, r.getBottom() - r.getHeight() * 0.28f);
        p.lineTo (r.getRight() - r.getWidth() * 0.22f, r.getY() + r.getHeight() * 0.27f);
        g.setColour (bg);
        g.strokePath (p, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}

void drawLogo (juce::Graphics& g, juce::Rectangle<float> r)
{
    // Three rounded bars, the middle one taller, cyan -> violet.
    const float bw = r.getWidth() / 3.6f, gap = (r.getWidth() - bw * 3.0f) / 2.0f;
    g.setGradientFill (juce::ColourGradient (accentIn, r.getX(), 0, accentHw, r.getRight(), 0, false));
    for (int i = 0; i < 3; ++i)
    {
        const float h = r.getHeight() * (i == 1 ? 1.0f : 0.68f);
        g.fillRoundedRectangle (r.getX() + (float) i * (bw + gap), r.getBottom() - h, bw, h, bw * 0.5f);
    }
}

void drawCard (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour fill, juce::Colour line, juce::Colour top)
{
    constexpr float corner = 4.0f;
    g.setColour (fill);
    g.fillRoundedRectangle (r, corner);
    g.setColour (line);
    g.drawRoundedRectangle (r.reduced (0.5f), corner, 1.0f);
    juce::Path shape;
    shape.addRoundedRectangle (r, corner);
    juce::Graphics::ScopedSaveState clip (g);
    g.reduceClipRegion (shape);
    g.setColour (top);
    g.fillRect (r.withHeight (2.0f));
}

void drawSwitch (juce::Graphics& g, juce::Rectangle<float> area, bool on, juce::Colour onColour, bool hover, bool enabled)
{
    drawSwitch (g, area, on ? 1.0f : 0.0f, onColour, hover, enabled);
}

void drawSwitch (juce::Graphics& g, juce::Rectangle<float> area, float position, juce::Colour onColour, bool hover, bool enabled)
{
    const float h = area.getHeight();
    auto pill = area.withWidth (h * 1.8f);
    const float alpha = enabled ? 1.0f : 0.4f;
    const auto off = hover ? raised.brighter (0.12f) : raised;
    g.setColour (off.interpolatedWith (onColour, position).withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (pill, h * 0.5f);
    if (position < 1.0f)
    {
        g.setColour (outline.brighter (0.2f).withMultipliedAlpha (alpha * (1.0f - position)));
        g.drawRoundedRectangle (pill.reduced (0.5f), h * 0.5f, 1.0f);
    }
    const float d = h - 6.0f;
    const float x = pill.getX() + 3.0f + position * (pill.getWidth() - 6.0f - d);
    g.setColour (textDim.interpolatedWith (bg, position).withMultipliedAlpha (alpha));
    g.fillEllipse (x, pill.getY() + 3.0f, d, d);
}

void LookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool over, bool)
{
    auto r = b.getLocalBounds().toFloat();
    const float h = juce::jmin (20.0f, r.getHeight() - 4.0f);
    drawSwitch (g, r.withSizeKeepingCentre (r.getWidth(), h).withX (r.getX() + 1.0f), b.getToggleState(), accentIn, over, b.isEnabled());
    g.setColour (b.isEnabled() ? theme::text : textFaint);
    g.setFont (font (13.0f));
    g.drawFittedText (b.getButtonText(), r.withTrimmedLeft (h * 1.8f + 12.0f).toNearestInt(), juce::Justification::centredLeft, 2);
}

void LookAndFeel::drawDocumentWindowTitleBar (juce::DocumentWindow& w, juce::Graphics& g, int width, int height,
                                              int, int, const juce::Image*, bool)
{
    const bool active = w.isActiveWindow();
    g.fillAll (panel);
    g.setColour (outline.withAlpha (0.6f));
    g.fillRect (0, height - 1, width, 1);

    const float logoH = juce::jmin (16.0f, (float) height - 14.0f);
    drawLogo (g, { 13.0f, ((float) height - logoH) * 0.5f, logoH * 0.95f, logoH });
    g.setColour (active ? text : textDim);
    g.setFont (font (13.0f, true));
    g.drawText (w.getName(), 40, 0, juce::jmax (0, width - 40 - 150), height, juce::Justification::centredLeft);
}

namespace
{
/** Flat title bar button (minimise / maximise-restore / close) in the app style. */
class WindowButton final : public juce::Button
{
public:
    explicit WindowButton (int t)
        : juce::Button (t == juce::DocumentWindow::closeButton ? "Close"
                        : t == juce::DocumentWindow::minimiseButton ? "Minimise" : "Maximise"),
          type (t)
    {
        setWantsKeyboardFocus (false);
    }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat();
        const bool isClose = type == juce::DocumentWindow::closeButton;
        const auto hoverFill = raised.brighter (down ? 0.15f : 0.0f);
        if (over || down)
        {
            g.setColour (isClose ? mute.withAlpha (down ? 0.7f : 0.9f) : hoverFill);
            g.fillRect (r);
        }

        auto* window = findParentComponentOfClass<juce::DocumentWindow>();
        const bool active = window == nullptr || window->isActiveWindow();
        const auto glyph = isClose && over ? juce::Colours::white : (active || over ? theme::text : textDim);
        g.setColour (glyph);

        const auto c = r.getCentre();
        const float s = 5.0f;
        if (isClose)
        {
            juce::Path p;
            p.startNewSubPath (c.x - s, c.y - s); p.lineTo (c.x + s, c.y + s);
            p.startNewSubPath (c.x + s, c.y - s); p.lineTo (c.x - s, c.y + s);
            g.strokePath (p, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        else if (type == juce::DocumentWindow::minimiseButton)
        {
            g.fillRoundedRectangle (c.x - s, c.y - 0.6f, s * 2.0f, 1.3f, 0.6f);
        }
        else if (window != nullptr && window->isFullScreen())
        {
            // restore: two overlapping frames
            g.drawRoundedRectangle (c.x - s + 2.0f, c.y - s, s * 2.0f - 2.0f, s * 2.0f - 2.0f, 1.5f, 1.1f);
            g.setColour (over || down ? hoverFill : panel);
            g.fillRoundedRectangle (c.x - s - 0.5f, c.y - s + 1.5f, s * 2.0f - 1.0f, s * 2.0f - 1.0f, 1.5f);
            g.setColour (glyph);
            g.drawRoundedRectangle (c.x - s, c.y - s + 2.0f, s * 2.0f - 2.0f, s * 2.0f - 2.0f, 1.5f, 1.1f);
        }
        else
        {
            g.drawRoundedRectangle (c.x - s, c.y - s, s * 2.0f, s * 2.0f, 1.8f, 1.1f);
        }
    }

private:
    int type;
};
} // namespace

juce::Button* LookAndFeel::createDocumentWindowButton (int buttonType)
{
    return new WindowButton (buttonType);
}

void LookAndFeel::positionDocumentWindowButtons (juce::DocumentWindow&, int x, int y, int w, int h,
                                                 juce::Button* minimiseButton, juce::Button* maximiseButton,
                                                 juce::Button* closeButton, bool)
{
    int right = x + w;
    for (auto* b : { closeButton, maximiseButton, minimiseButton })
    {
        if (b == nullptr) continue;
        right -= 46;
        b->setBounds (right, y, 46, h - 1);
    }
}

void LookAndFeel::drawResizableWindowBorder (juce::Graphics& g, int w, int h, const juce::BorderSize<int>& border,
                                             juce::ResizableWindow&)
{
    // No visible frame: the resize zone looks like the window itself (the content covers it, except beside the title bar).
    g.setColour (panel);
    g.drawRect (0, 0, w, h, juce::jmax (1, border.getTop()));
}
juce::TextLayout layoutTooltip (const juce::String& text, float maxWidth)
{
    // "Title\nbody": the first line is bold when more follows; "\n\n" separates paragraphs.
    juce::AttributedString s;
    s.setWordWrap (juce::AttributedString::byWord);
    s.setLineSpacing (2.0f);
    const auto title = text.upToFirstOccurrenceOf ("\n", false, false);
    const auto body = text.fromFirstOccurrenceOf ("\n", false, false);
    if (body.isNotEmpty() && title.length() <= 60)
    {
        s.append (title + "\n", font (13.0f, true), theme::text);
        s.append (body, font (12.5f), theme::text.withAlpha (0.82f));
    }
    else
        s.append (text, font (12.5f), theme::text);
    juce::TextLayout tl;
    tl.createLayout (s, maxWidth);
    // Shrink to the longest line, so short tips do not get a wide empty card.
    float w = 0.0f;
    for (int i = 0; i < tl.getNumLines(); ++i) w = juce::jmax (w, tl.getLine (i).getLineBoundsX().getLength());
    tl.createLayout (s, juce::jmin (maxWidth, std::ceil (w) + 1.0f));
    return tl;
}

namespace
{
// Route tooltips (icons::routeTip): a first line "[icon] from  ->  [icon] to" drawn by hand, then the body text.
constexpr float kRouteLine = 22.0f, kRouteIcon = 16.0f, kRouteGap = 6.0f;

float routeLineWidth (const icons::RouteTip& t)
{
    const auto f = font (13.0f, true);
    auto side = [&] (const juce::String& icon, const juce::String& name)
    {
        return (icons::exists (icon) ? kRouteIcon + 5.0f : 0.0f) + juce::GlyphArrangement::getStringWidth (f, name);
    };
    return side (t.fromIcon, t.fromName) + kRouteGap * 2.0f + 14.0f + side (t.toIcon, t.toName);
}

void drawRouteLine (juce::Graphics& g, const icons::RouteTip& t, juce::Rectangle<float> r)
{
    const auto f = font (13.0f, true);
    g.setFont (f);
    auto side = [&] (const juce::String& icon, const juce::String& name)
    {
        if (icons::exists (icon))
        {
            icons::draw (g, icon, r.removeFromLeft (kRouteIcon).withSizeKeepingCentre (kRouteIcon, kRouteIcon), theme::text);
            r.removeFromLeft (5.0f);
        }
        const float w = juce::GlyphArrangement::getStringWidth (f, name) + 1.0f;
        g.setColour (theme::text);
        g.drawText (name, r.removeFromLeft (w), juce::Justification::centredLeft, false);
    };
    side (t.fromIcon, t.fromName);
    r.removeFromLeft (kRouteGap);
    {
        // Arrow
        auto a = r.removeFromLeft (14.0f).withSizeKeepingCentre (14.0f, 10.0f);
        juce::Path p;
        p.startNewSubPath (a.getX(), a.getCentreY());
        p.lineTo (a.getRight() - 1.0f, a.getCentreY());
        p.startNewSubPath (a.getRight() - 5.0f, a.getY());
        p.lineTo (a.getRight() - 1.0f, a.getCentreY());
        p.lineTo (a.getRight() - 5.0f, a.getBottom());
        g.setColour (theme::text.withAlpha (0.6f));
        g.strokePath (p, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    r.removeFromLeft (kRouteGap);
    side (t.toIcon, t.toName);
}
} // namespace

juce::Rectangle<int> LookAndFeel::getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos, juce::Rectangle<int> parentArea)
{
    icons::RouteTip route;
    const bool isRoute = icons::parseRouteTip (tipText, route);
    const auto tl = layoutTooltip (isRoute ? route.body : tipText, kTipWidth);
    float textW = isRoute && route.body.isEmpty() ? 0.0f : tl.getWidth();
    float textH = isRoute && route.body.isEmpty() ? 0.0f : tl.getHeight();
    if (isRoute)
    {
        textW = juce::jmax (textW, routeLineWidth (route));
        textH += kRouteLine + (route.body.isNotEmpty() ? 4.0f : 0.0f);
    }
    const int w = (int) std::ceil (textW + kTipPadX * 2.0f) + 2;
    const int h = (int) std::ceil (textH + kTipPadY * 2.0f) + 2;
    return juce::Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 10) : screenPos.x + 16,
                                 screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 10) : screenPos.y + 20, w, h)
        .constrainedWithin (parentArea);
}

void LookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height)
{
    auto r = juce::Rectangle<float> ((float) width, (float) height).reduced (0.5f);
    g.setColour (light() ? panel2 : juce::Colour (0xff20252f));
    g.fillRoundedRectangle (r, 7.0f);
    g.setColour (light() ? outline.darker (0.15f) : juce::Colour (0xff3a4354));
    g.drawRoundedRectangle (r, 7.0f, 1.0f);
    auto inner = r.reduced (kTipPadX, kTipPadY);
    if (icons::RouteTip route; icons::parseRouteTip (text, route))
    {
        drawRouteLine (g, route, inner.removeFromTop (kRouteLine));
        if (route.body.isEmpty()) return;
        inner.removeFromTop (4.0f);
        layoutTooltip (route.body, kTipWidth).draw (g, inner);
        return;
    }
    layoutTooltip (text, kTipWidth).draw (g, inner);
}

void LookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float minPos, float maxPos,
                                    juce::Slider::SliderStyle style, juce::Slider& s)
{
    if (style != juce::Slider::LinearHorizontal)
        return juce::LookAndFeel_V4::drawLinearSlider (g, x, y, w, h, pos, minPos, maxPos, style, s);
    const auto r = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h);
    const auto track = r.withSizeKeepingCentre (r.getWidth(), 5.0f);
    const float alpha = s.isEnabled() ? 1.0f : 0.4f;
    g.setColour (s.findColour (juce::Slider::backgroundColourId).brighter (0.15f).withAlpha (alpha));
    g.fillRoundedRectangle (track, 2.5f);
    g.setColour (s.findColour (juce::Slider::trackColourId).withAlpha (alpha));
    g.fillRoundedRectangle (track.withRight (juce::jlimit (track.getX(), track.getRight(), pos)), 2.5f);
    const float d = 13.0f;
    const auto thumb = juce::Rectangle<float> (d, d).withCentre ({ juce::jlimit (r.getX() + d * 0.5f, r.getRight() - d * 0.5f, pos), r.getCentreY() });
    g.setColour (juce::Colours::black.withAlpha (0.3f * alpha));
    g.fillEllipse (thumb.translated (0.0f, 1.0f));
    g.setColour (s.findColour (juce::Slider::thumbColourId).withAlpha (alpha).brighter (s.isMouseOverOrDragging() ? 0.2f : 0.0f));
    g.fillEllipse (thumb);
    g.setColour (bg.withAlpha (0.7f * alpha));
    g.drawEllipse (thumb, 1.0f);
}
} // namespace hydra::theme
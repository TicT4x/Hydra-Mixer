#include "Theme.h"

namespace hydra::theme
{
LookAndFeel::LookAndFeel()
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
    setColour (juce::PopupMenu::backgroundColourId, panel2);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, accentIn.withAlpha (0.18f));
    setColour (juce::PopupMenu::highlightedTextColourId, text);
    setColour (juce::PopupMenu::headerTextColourId, textDim);
    setColour (juce::TextEditor::backgroundColourId, panel2);
    setColour (juce::TextEditor::outlineColourId, outline);
    setColour (juce::TextEditor::focusedOutlineColourId, accentIn);
    setColour (juce::TextEditor::highlightColourId, accentIn.withAlpha (0.3f));
    setColour (juce::Label::textColourId, text);
    setColour (juce::ListBox::backgroundColourId, panel);
    setColour (juce::ScrollBar::thumbColourId, raised);
    setColour (juce::TabbedComponent::backgroundColourId, panel);
    setColour (juce::TabbedComponent::outlineColourId, juce::Colours::transparentBlack);
    setColour (juce::TabbedButtonBar::tabOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::ResizableWindow::backgroundColourId, bg);
    setColour (juce::DocumentWindow::textColourId, text);
    setColour (juce::ProgressBar::foregroundColourId, accentIn);
    setColour (juce::ProgressBar::backgroundColourId, panel2);
    setColour (juce::ToggleButton::tickColourId, accentIn);
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
    g.fillAll (panel2);
    g.setColour (outline);
    g.drawRect (0, 0, w, h);
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

void drawSwitch (juce::Graphics& g, juce::Rectangle<float> area, bool on, juce::Colour onColour, bool hover, bool enabled)
{
    const float h = area.getHeight();
    auto pill = area.withWidth (h * 1.8f);
    g.setOpacity (enabled ? 1.0f : 0.4f);
    g.setColour (on ? onColour : (hover ? raised.brighter (0.12f) : raised));
    g.fillRoundedRectangle (pill, h * 0.5f);
    if (! on)
    {
        g.setColour (outline.brighter (0.2f));
        g.drawRoundedRectangle (pill.reduced (0.5f), h * 0.5f, 1.0f);
    }
    const float d = h - 6.0f;
    const float x = on ? pill.getRight() - 3.0f - d : pill.getX() + 3.0f;
    g.setColour (on ? bg : textDim);
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
    g.setColour (outline);
    g.drawRect (0, 0, w, h, juce::jmax (1, border.getTop()));
}
} // namespace hydra::theme

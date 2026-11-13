#pragma once
#include "Common.h"

namespace hydra::theme
{
/** Look of the app (Settings > Display > Theme). The palette below follows it; the same colours are
    read when painting, so apply() + a repaint switches open windows. Message thread only.
    dark / darker = the current look (channels as one block, rectangular faders) in two palettes: Dark is the
    default (blue-grey slate), Darker nearly neutral graphite, Light the same look bright ("Nebel": cool
    blue-grey, the accents a little darker so they carry on white). The palette below starts as Legacy: the
    plugin never calls apply() and keeps that look. Colours are copied by value when components are built;
    MainComponent::themeChanged rebuilds strips and inspector, so keep no static copies of them. */
enum class Style { legacy, dark, darker, light };
inline Style style = Style::legacy;
/** The current look (Dark, Darker or Light), as opposed to Legacy. */
inline bool deep() noexcept { return style != Style::legacy; }
/** Bright background: shadows lighter, glow weaker, text dark. */
inline bool light() noexcept { return style == Style::light; }
/** Glow of selected strips and effect blocks: always on in the app (no option since 13.11.); HydraShots can switch it
    off (HYDRA_SHOTS_GLOW=0), then only the thicker line on top. */
inline bool glow = true;
void apply (Style);
Style styleFromName (const juce::String&);   // session value ("dark", "darker", "legacy"; old "default" = dark)
const char* styleName (Style);

/** Selected strip or effect block: the coloured line on top gets thicker and (with glow) lights the card
    from there down, plus a soft halo just around the line. r = the card, clipped to its shape by the caller
    when it has rounded corners. vertical = the line runs down the left side (horizontal channel rows). */
void drawSelectedTop (juce::Graphics&, juce::Rectangle<float> r, juce::Colour, bool vertical = false);
constexpr float kSelectedLine = 4.0f;

inline juce::Colour bg        { 0xff0d0f13 };
inline juce::Colour panel     { 0xff151920 };
inline juce::Colour panel2    { 0xff1b2029 };
inline juce::Colour raised    { 0xff232935 };
inline juce::Colour outline   { 0xff2a313e };
inline juce::Colour cardLine  { outline.withAlpha (0.6f) };   // frame of the effect blocks
inline juce::Colour text      { 0xffe8ebf1 };
inline juce::Colour textDim   { 0xff8b94a5 };
inline juce::Colour textFaint { 0xff5a6273 };

inline juce::Colour accentIn   { 0xff35d0f4 }; // input strips
inline juce::Colour accentHw   { 0xff8d6cff }; // H buses
inline juce::Colour accentVirt { 0xffff6aa6 }; // V buses
inline juce::Colour mute       { 0xffff4d5e };
inline juce::Colour solo       { 0xffffc23f };
inline juce::Colour good       { 0xff3ee08f };
inline juce::Colour warn       { 0xffffa53d };
inline juce::Colour learn      { 0xffffd400 };

// Each kind of effect block has its own colour (denoiser: good; plugins: the channel's colour).
inline juce::Colour fxEq         { 0xff35d0f4 };
inline juce::Colour fxGate       { 0xffffc23f };
inline juce::Colour fxComp       { 0xffff8f4d };
inline juce::Colour fxLimiter    { 0xffff6a7d };
inline juce::Colour fxCorrection { 0xffa78bfa };
inline juce::Colour fxUtility    { 0xffff6ac1 };   // was the phase block's colour
inline juce::Colour fxTranspose  { 0xffc77dff };
inline juce::Colour fxStems      { 0xff56e0c8 };
inline juce::Colour fxHeadphone  { 0xff7c9cff };/** Colour of each stem of the Stems effect (vocals, drums, bass, guitar, rest). */
inline juce::Colour stemColour (int s)
{
    static const juce::Colour c[] = { juce::Colour (0xffff6ac1), juce::Colour (0xffffa53d), juce::Colour (0xff8d6cff),
                                      juce::Colour (0xffffd84a), juce::Colour (0xff35c8f4) };
    return c[juce::jlimit (0, 4, s)];
}
// Menus (highlight, tick mark): the middle of the logo's cyan -> violet gradient.
inline juce::Colour fx { 0xff6cacfa };

inline juce::Colour busColour (int b) { return b < kMaxHwBuses ? accentHw : accentVirt; }

/** Colour code of EQ band k (0..15), used for its point, its curve and its controls. Bands are added in this
    order, so neighbours are far apart in hue (no orange next to yellow). */
inline juce::Colour bandColour (int k)
{
    static const juce::Colour c[] = { juce::Colour (0xffff5f6d), juce::Colour (0xffffd84a), juce::Colour (0xff35c8f4),
                                      juce::Colour (0xffffa53d), juce::Colour (0xffa47cff), juce::Colour (0xff3ee08f),
                                      juce::Colour (0xffff6ac1), juce::Colour (0xff5b7cff) };
    // Bands 9-16: the colours of 1-8, a little shifted and lighter (so each band keeps a colour of its own).
    k = juce::jlimit (0, 15, k);
    return k < 8 ? c[k] : c[k - 8].withRotatedHue (0.06f).brighter (0.35f);
}

/** Text size factor (Settings > Display > Text size). Applies to fonts created after a change,
    so the UI rebuilds its strips / inspector when it changes. Message thread only. */
inline float fontScale = 1.0f;

inline juce::Font font (float size, bool bold = false)
{
    return juce::Font (juce::FontOptions ("Segoe UI", size * fontScale, bold ? juce::Font::bold : juce::Font::plain));
}

inline juce::Font mono (float size)
{
    return juce::Font (juce::FontOptions ("Cascadia Mono", size * fontScale, juce::Font::plain));
}

/** The Hydra logo (three gradient bars) drawn into area; also used for the tray icon. */
void drawLogo (juce::Graphics&, juce::Rectangle<float> area);

/** Oval on/off switch with a round knob (pill fills area's height, 1.8 x as wide). */
void drawSwitch (juce::Graphics&, juce::Rectangle<float> area, bool on, juce::Colour onColour, bool hover, bool enabled = true);
/** The same with the knob on its way: position 0 = off .. 1 = on (colour and knob follow). */
void drawSwitch (juce::Graphics&, juce::Rectangle<float> area, float position, juce::Colour onColour, bool hover, bool enabled);

/** Deep theme: card of an effect block, slightly rounded with a line in its colour on top (like the strips). */
void drawCard (juce::Graphics&, juce::Rectangle<float> area, juce::Colour fill, juce::Colour line, juce::Colour top);

class LookAndFeel : public juce::LookAndFeel_V4
{
public:
    LookAndFeel();
    /** Takes over the current palette (after apply()). */
    void refreshColours();

    juce::Font getPopupMenuFont() override               { return font (14.0f); }
    juce::Font getComboBoxFont (juce::ComboBox&) override { return font (13.5f); }
    juce::Font getTextButtonFont (juce::TextButton&, int) override { return font (13.5f, true); }
    juce::Font getLabelFont (juce::Label&) override       { return font (13.5f); }

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool over, bool down) override;
    void drawComboBox (juce::Graphics&, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    /** Menus: rounded card (the window is transparent around it), rounded highlight, small caps section headers. */
    void drawPopupMenuBackground (juce::Graphics&, int w, int h) override;
    void drawPopupMenuItem (juce::Graphics&, const juce::Rectangle<int>& area, bool isSeparator, bool isActive, bool isHighlighted,
                            bool isTicked, bool hasSubMenu, const juce::String& text, const juce::String& shortcutKeyText,
                            const juce::Drawable* icon, const juce::Colour* textColour) override;
    void drawPopupMenuSectionHeader (juce::Graphics&, const juce::Rectangle<int>& area, const juce::String& sectionName) override;
    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int standardMenuItemHeight, int& idealWidth, int& idealHeight) override;
    /** Section headers hug their text (JUCE: 1.5 x the height of an item, which left a wide gap above the title). */
    void getIdealPopupMenuSectionHeaderSizeWithOptions (const juce::String& text, int standardMenuItemHeight, int& idealWidth, int& idealHeight,
                                                        const juce::PopupMenu::Options&) override;
    int getPopupMenuBorderSize() override { return 5; }
    void drawTabButton (juce::TabBarButton&, juce::Graphics&, bool over, bool down) override;
    int getTabButtonBestWidth (juce::TabBarButton&, int) override { return 140; }
    void drawTickBox (juce::Graphics&, juce::Component&, float x, float y, float w, float h,
                      bool ticked, bool enabled, bool over, bool down) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool over, bool down) override;
    /** Horizontal sliders: thin track filled up to the value (trackColourId), round thumb. */
    void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h, float pos, float minPos, float maxPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    void drawDocumentWindowTitleBar (juce::DocumentWindow&, juce::Graphics&, int w, int h,
                                     int titleSpaceX, int titleSpaceW, const juce::Image*, bool drawTitleTextOnLeft) override;
    juce::Button* createDocumentWindowButton (int buttonType) override;
    void positionDocumentWindowButtons (juce::DocumentWindow&, int titleBarX, int titleBarY, int titleBarW, int titleBarH,
                                        juce::Button* minimiseButton, juce::Button* maximiseButton, juce::Button* closeButton,
                                        bool positionTitleBarButtonsOnLeft) override;
    void drawResizableWindowBorder (juce::Graphics&, int w, int h, const juce::BorderSize<int>&, juce::ResizableWindow&) override;
    /** Tooltips: rounded card, wrapped at kTipWidth; a first line followed by a line break is drawn as a bold title. */
    juce::Rectangle<int> getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos, juce::Rectangle<int> parentArea) override;
    void drawTooltip (juce::Graphics&, const juce::String& text, int width, int height) override;
};

/** Layout of a tooltip's text (see LookAndFeel::drawTooltip), wrapped at maxWidth. */
juce::TextLayout layoutTooltip (const juce::String& text, float maxWidth);
inline constexpr float kTipWidth = 320.0f, kTipPadX = 12.0f, kTipPadY = 9.0f;

/** The app's tooltip window: transparent around the rounded card. The one instance (MainComponent) is
    reachable through get(), so an info button can show its text at once (TooltipWindow::displayTip). */
class TooltipWindow final : public juce::TooltipWindow
{
public:
    TooltipWindow() : juce::TooltipWindow (nullptr, 600) { setOpaque (false); instance = this; }
    ~TooltipWindow() override { if (instance == this) instance = nullptr; }
    static TooltipWindow* get() noexcept { return instance; }

private:
    static inline TooltipWindow* instance = nullptr;
};
} // namespace hydra::theme

#pragma once
#include "Common.h"

namespace hydra::theme
{
inline const juce::Colour bg        { 0xff0d0f13 };
inline const juce::Colour panel     { 0xff151920 };
inline const juce::Colour panel2    { 0xff1b2029 };
inline const juce::Colour raised    { 0xff232935 };
inline const juce::Colour outline   { 0xff2a313e };
inline const juce::Colour text      { 0xffe8ebf1 };
inline const juce::Colour textDim   { 0xff8b94a5 };
inline const juce::Colour textFaint { 0xff5a6273 };

inline const juce::Colour accentIn   { 0xff35d0f4 }; // input strips
inline const juce::Colour accentHw   { 0xff8d6cff }; // A buses
inline const juce::Colour accentVirt { 0xffff6aa6 }; // B buses
inline const juce::Colour mute       { 0xffff4d5e };
inline const juce::Colour solo       { 0xffffc23f };
inline const juce::Colour good       { 0xff3ee08f };
inline const juce::Colour warn       { 0xffffa53d };
inline const juce::Colour learn      { 0xffffd400 };

inline juce::Colour busColour (int b) { return b < kMaxHwBuses ? accentHw : accentVirt; }

/** Colour code of EQ band k (0..7), used for its point, its curve and its controls. */
inline juce::Colour bandColour (int k)
{
    static const juce::Colour c[] = { juce::Colour (0xffff5f6d), juce::Colour (0xffffa53d), juce::Colour (0xffffd84a),
                                      juce::Colour (0xff3ee08f), juce::Colour (0xff35c8f4), juce::Colour (0xffa47cff),
                                      juce::Colour (0xff5b7cff), juce::Colour (0xffff6ac1) };
    return c[juce::jlimit (0, 7, k)];
}

/** Text size factor (Settings > General > Text size). Applies to fonts created after a change,
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

class LookAndFeel : public juce::LookAndFeel_V4
{
public:
    LookAndFeel();

    juce::Font getPopupMenuFont() override               { return font (14.0f); }
    juce::Font getComboBoxFont (juce::ComboBox&) override { return font (13.5f); }
    juce::Font getTextButtonFont (juce::TextButton&, int) override { return font (13.5f, true); }
    juce::Font getLabelFont (juce::Label&) override       { return font (13.5f); }

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool over, bool down) override;
    void drawComboBox (juce::Graphics&, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    void drawPopupMenuBackground (juce::Graphics&, int w, int h) override;
    void drawTabButton (juce::TabBarButton&, juce::Graphics&, bool over, bool down) override;
    int getTabButtonBestWidth (juce::TabBarButton&, int) override { return 140; }
    void drawTickBox (juce::Graphics&, juce::Component&, float x, float y, float w, float h,
                      bool ticked, bool enabled, bool over, bool down) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool over, bool down) override;
    void drawDocumentWindowTitleBar (juce::DocumentWindow&, juce::Graphics&, int w, int h,
                                     int titleSpaceX, int titleSpaceW, const juce::Image*, bool drawTitleTextOnLeft) override;
    juce::Button* createDocumentWindowButton (int buttonType) override;
    void positionDocumentWindowButtons (juce::DocumentWindow&, int titleBarX, int titleBarY, int titleBarW, int titleBarH,
                                        juce::Button* minimiseButton, juce::Button* maximiseButton, juce::Button* closeButton,
                                        bool positionTitleBarButtonsOnLeft) override;
    void drawResizableWindowBorder (juce::Graphics&, int w, int h, const juce::BorderSize<int>&, juce::ResizableWindow&) override;
};
} // namespace hydra::theme

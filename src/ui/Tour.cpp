#include "Tour.h"
#include "ChannelIcons.h"

namespace hydra
{
namespace
{
constexpr int kCardW = 430, kPad = 20, kSpotPad = 6;

juce::TextLayout layoutText (const juce::String& text, const juce::Font& font, juce::Colour colour, float width)
{
    juce::AttributedString s;
    s.setWordWrap (juce::AttributedString::byWord);
    s.append (text, font, colour);
    juce::TextLayout l;
    l.createLayout (s, width);
    return l;
}

/** Card illustrations. */
enum Art { none = 0, flow, rename, rightClick, clock, sources, picker, pickerOut, outputs, virtualOut, hearYourself, fader, muteSolo, fxChain };

/** Dots flowing along a path (routing animation). */
void flowDots (juce::Graphics& g, const juce::Path& p, juce::Colour c, float t, float spacing = 22.0f, float radius = 2.6f)
{
    const float len = p.getLength();
    if (len <= 1.0f) return;
    const float offset = std::fmod (t * 70.0f, spacing);
    for (float d = offset; d < len; d += spacing)
    {
        const auto pt = p.getPointAlongPath (d);
        const float fade = juce::jmin (1.0f, d / 18.0f, (len - d) / 18.0f);
        g.setColour (c.withAlpha (0.25f * fade));
        g.fillEllipse (pt.x - radius * 2.2f, pt.y - radius * 2.2f, radius * 4.4f, radius * 4.4f);
        g.setColour (c.withAlpha (fade));
        g.fillEllipse (pt.x - radius, pt.y - radius, radius * 2.0f, radius * 2.0f);
    }
}

juce::Path arc (juce::Point<float> a, juce::Point<float> b, float lift)
{
    juce::Path p;
    p.startNewSubPath (a);
    const float top = juce::jmin (a.y, b.y) - lift;
    p.cubicTo ({ a.x, top }, { b.x, top }, b);
    return p;
}

/** Rounded box with an icon / text, used by the card illustrations. */
void pill (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c, const juce::String& text, float alpha = 1.0f, const char* icon = nullptr)
{
    g.setColour (c.withAlpha (0.14f * alpha));
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (c.withAlpha (0.75f * alpha));
    g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
    auto inner = r.reduced (6.0f, 0.0f);
    if (icon != nullptr)
    {
        icons::draw (g, icon, inner.removeFromLeft (r.getHeight() - 6.0f).withSizeKeepingCentre (r.getHeight() - 8.0f, r.getHeight() - 8.0f),
                     c.withAlpha (alpha));
        inner.removeFromLeft (4.0f);
    }
    g.setColour (theme::text.withAlpha (alpha));
    g.setFont (theme::font (12.0f, true));
    g.drawText (text, inner, icon != nullptr ? juce::Justification::centredLeft : juce::Justification::centred, true);
}

/** Sound waves next to a point (pulsing arcs). */
void waves (juce::Graphics& g, juce::Point<float> p, juce::Colour c, float t, float size = 16.0f)
{
    for (int i = 0; i < 3; ++i)
    {
        const float k = std::fmod (t * 0.9f + (float) i / 3.0f, 1.0f);
        const float r = size * (0.35f + 0.9f * k);
        juce::Path arcP;
        arcP.addCentredArc (p.x, p.y, r, r, 0.0f, juce::MathConstants<float>::pi * 0.25f, juce::MathConstants<float>::pi * 0.75f, true);
        g.setColour (c.withAlpha (0.8f * (1.0f - k)));
        g.strokePath (arcP, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}

/** A small strip: header line in its colour, fader, meter. level 0..1 of the knob. */
void miniStrip (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c, float level, float meter)
{
    g.setColour (theme::panel2);
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (c);
    g.fillRoundedRectangle (r.withHeight (3.0f), 1.5f);
    const auto track = juce::Rectangle<float> (r.getX() + r.getWidth() * 0.3f, r.getY() + 10.0f, 3.0f, r.getHeight() - 18.0f);
    g.setColour (theme::outline);
    g.fillRoundedRectangle (track, 1.5f);
    const float y = track.getBottom() - level * track.getHeight();
    g.setColour (c.withAlpha (0.7f));
    g.fillRect (track.withTop (y));
    g.setColour (theme::text);
    g.fillRoundedRectangle (track.getCentreX() - 8.0f, y - 3.0f, 16.0f, 6.0f, 2.0f);
    const auto m = juce::Rectangle<float> (r.getRight() - r.getWidth() * 0.3f, track.getY(), 4.0f, track.getHeight());
    g.setColour (theme::outline);
    g.fillRect (m);
    g.setColour (theme::good);
    g.fillRect (m.withTop (m.getBottom() - meter * m.getHeight()));
}

/** Illustrations added on 11.11. (more graphics in the tour): true if `art` was one of them. */
bool paintMoreArt (juce::Graphics& g, juce::Rectangle<float> a, int art, float t)
{
    const float pi = juce::MathConstants<float>::pi;
    if (art == Art::sources)
    {
        // Sources flowing into one input strip each.
        const struct { const char* icon; const char* name; } src[] = { { "headset", "Mic" }, { "game", "Game" }, { "music", "Spotify" }, { "chat", "Cable" } };
        for (int i = 0; i < 4; ++i)
        {
            const float x = a.getX() + 12.0f + (float) i * ((a.getWidth() - 24.0f) / 4.0f);
            const float w = (a.getWidth() - 24.0f) / 4.0f - 10.0f;
            const juce::Rectangle<float> p (x, a.getY() + 8.0f, w, 24.0f);
            pill (g, p, theme::accentIn, src[i].name, 1.0f, src[i].icon);
            const juce::Rectangle<float> s (p.getCentreX() - 14.0f, a.getY() + 52.0f, 28.0f, a.getHeight() - 58.0f);
            juce::Path line;
            line.startNewSubPath (p.getCentreX(), p.getBottom() + 2.0f);
            line.lineTo (s.getCentreX(), s.getY() - 2.0f);
            flowDots (g, line, theme::accentIn, t + (float) i * 0.2f, 9.0f, 1.8f);
            const float lvl = 0.45f + 0.35f * std::abs (std::sin (t * 2.2f + (float) i * 1.3f));
            miniStrip (g, s, theme::accentIn, 0.6f, lvl);
        }
        return true;
    }
    if (art == Art::picker || art == Art::pickerOut)
    {
        // The device window: Connection -> Device -> Channels, the highlight walks from column to column.
        const bool out = art == Art::pickerOut;
        const char* titles[] = { "Connection", "Device", "Channels" };
        const char* rows[3][3] = { { out ? "Hardware" : "Hardware", out ? "Windows" : "Application", out ? "Hydra Link" : "Virtual cable" },
                                   { out ? "Focusrite USB" : "Focusrite USB", out ? "Headset" : "Spotify", out ? "TV" : "Game" },
                                   { "1 / 2", "3 / 4", "5 / 6" } };
        const int step = (int) std::fmod (t * 0.8f, 4.0f);   // 0..2 the columns, 3 = done
        const float colW = (a.getWidth() - 24.0f) / 3.0f;
        for (int c = 0; c < 3; ++c)
        {
            const juce::Rectangle<float> col (a.getX() + 8.0f + (float) c * (colW + 4.0f), a.getY() + 6.0f, colW, a.getHeight() - 12.0f);
            const bool active = c == step;
            g.setColour (active ? theme::accentIn.withAlpha (0.08f) : juce::Colours::transparentBlack);
            g.fillRoundedRectangle (col, 6.0f);
            g.setColour (active ? theme::accentIn : theme::textFaint);
            g.setFont (theme::font (11.0f, true));
            g.drawText (juce::String (c + 1) + "  " + titles[c], col.withHeight (16.0f).translated (6.0f, 2.0f), juce::Justification::centredLeft);
            for (int r = 0; r < 3; ++r)
            {
                const auto row = juce::Rectangle<float> (col.getX() + 4.0f, col.getY() + 22.0f + (float) r * 26.0f, col.getWidth() - 8.0f, 22.0f);
                const bool chosen = r == (c == 0 ? (out ? 0 : 1) : c == 1 ? 1 : 0) && c <= step;
                g.setColour (chosen ? theme::accentIn.withAlpha (0.25f) : theme::raised.withAlpha (0.6f));
                g.fillRoundedRectangle (row, 5.0f);
                g.setColour (chosen ? theme::text : theme::textDim);
                g.setFont (theme::font (11.5f, chosen));
                g.drawText (rows[c][r], row.withTrimmedLeft (8.0f), juce::Justification::centredLeft, true);
            }
            if (c < 2)
            {
                juce::Path arrow;
                const float x = col.getRight() + 2.0f, y = col.getCentreY();
                arrow.addTriangle (x - 3.0f, y - 4.0f, x + 2.0f, y, x - 3.0f, y + 4.0f);
                g.setColour (c < step ? theme::accentIn : theme::outline);
                g.fillPath (arrow);
            }
        }
        if (step < 3)
        {
            // The pointer clicking the chosen row.
            const juce::Rectangle<float> col (a.getX() + 8.0f + (float) step * (colW + 4.0f), a.getY() + 6.0f, colW, a.getHeight() - 12.0f);
            const int r = step == 0 ? (out ? 0 : 1) : step == 1 ? 1 : 0;
            const juce::Point<float> m (col.getRight() - 22.0f, col.getY() + 34.0f + (float) r * 26.0f);
            const float k = std::fmod (t * 0.8f, 1.0f);
            g.setColour (theme::text.withAlpha (0.5f * (1.0f - k)));
            g.drawEllipse (m.x - 3.0f - 9.0f * k, m.y - 3.0f - 9.0f * k, 6.0f + 18.0f * k, 6.0f + 18.0f * k, 1.2f);
            juce::Path p;
            p.addTriangle (m, m.translated (0.0f, 13.0f), m.translated (9.0f, 9.0f));
            g.setColour (theme::text);
            g.fillPath (p);
        }
        return true;
    }
    if (art == Art::outputs || art == Art::virtualOut)
    {
        // One output strip playing to devices (H) or to apps through a cable (V).
        const bool virt = art == Art::virtualOut;
        const auto col = virt ? theme::accentVirt : theme::accentHw;
        const juce::Rectangle<float> s (a.getX() + 30.0f, a.getY() + 10.0f, 34.0f, a.getHeight() - 20.0f);
        miniStrip (g, s, col, 0.62f, 0.5f + 0.3f * std::abs (std::sin (t * 2.0f)));
        const struct { const char* icon; const char* name; } dst[3] = {
            { virt ? "discord" : "speakers", virt ? "Discord" : "Speakers" },
            { virt ? "obs" : "headphones", virt ? "OBS" : "Headphones" },
            { virt ? "recording" : "tv", virt ? "Recording" : "TV" } };
        for (int i = 0; i < 3; ++i)
        {
            const juce::Rectangle<float> d (a.getRight() - 150.0f, a.getY() + 10.0f + (float) i * ((a.getHeight() - 20.0f) / 3.0f), 132.0f, 26.0f);
            juce::Path p;
            p.startNewSubPath (s.getRight() + 4.0f, s.getCentreY());
            p.cubicTo ({ s.getRight() + 60.0f, s.getCentreY() }, { d.getX() - 60.0f, d.getCentreY() }, { d.getX() - 4.0f, d.getCentreY() });
            g.setColour (theme::outline);
            g.strokePath (p, juce::PathStrokeType (1.0f));
            if (virt)
            {
                // Through a cable: a small plug in the middle of the line.
                const auto mid = p.getPointAlongPath (p.getLength() * 0.5f);
                g.setColour (theme::raised);
                g.fillRoundedRectangle (mid.x - 9.0f, mid.y - 5.0f, 18.0f, 10.0f, 3.0f);
                g.setColour (col);
                g.drawRoundedRectangle (mid.x - 9.0f, mid.y - 5.0f, 18.0f, 10.0f, 3.0f, 1.0f);
            }
            flowDots (g, p, col, t + (float) i * 0.25f, 14.0f, 2.0f);
            pill (g, d, col, dst[i].name, 1.0f, dst[i].icon);
            if (! virt) waves (g, { d.getRight() + 4.0f, d.getCentreY() }, col, t + (float) i * 0.3f, 8.0f);
        }
        if (virt)
        {
            g.setColour (theme::textDim);
            g.setFont (theme::font (11.0f));
            g.drawText ("apps choose it as their microphone", a.withTrimmedTop (a.getHeight() - 16.0f).withTrimmedLeft (80.0f), juce::Justification::centredLeft);
        }
        return true;
    }
    if (art == Art::hearYourself)
    {
        // Microphone -> [H1] -> headphones. The button switches on and off; with it on you hear yourself.
        const bool on = std::fmod (t * 0.5f, 2.0f) < 1.4f;
        const juce::Rectangle<float> mic (a.getX() + 10.0f, a.getCentreY() - 14.0f, 104.0f, 28.0f);
        const juce::Rectangle<float> btn (a.getCentreX() - 22.0f, a.getCentreY() - 14.0f, 44.0f, 28.0f);
        const juce::Rectangle<float> hp (a.getRight() - 130.0f, a.getCentreY() - 14.0f, 112.0f, 28.0f);
        pill (g, mic, theme::accentIn, "Your mic", 1.0f, "headset");
        juce::Path in, out;
        in.startNewSubPath (mic.getRight() + 4.0f, mic.getCentreY());
        in.lineTo (btn.getX() - 4.0f, btn.getCentreY());
        out.startNewSubPath (btn.getRight() + 4.0f, btn.getCentreY());
        out.lineTo (hp.getX() - 4.0f, hp.getCentreY());
        g.setColour (theme::outline);
        g.strokePath (in, juce::PathStrokeType (1.0f));
        g.strokePath (out, juce::PathStrokeType (1.0f));
        flowDots (g, in, theme::accentIn, t, 12.0f, 2.0f);
        if (on) flowDots (g, out, theme::accentHw, t, 12.0f, 2.0f);
        g.setColour (on ? theme::accentHw.withAlpha (0.35f) : theme::raised);
        g.fillRoundedRectangle (btn, 6.0f);
        g.setColour (on ? theme::accentHw : theme::outline);
        g.drawRoundedRectangle (btn.reduced (0.5f), 6.0f, 1.2f);
        g.setColour (on ? theme::text : theme::textDim);
        g.setFont (theme::font (13.0f, true));
        g.drawText ("H1", btn, juce::Justification::centred);
        pill (g, hp, theme::accentHw, "Headphones", on ? 1.0f : 0.45f, "headphones");
        if (on) waves (g, { hp.getRight() + 4.0f, hp.getCentreY() }, theme::accentHw, t, 10.0f);
        g.setColour (on ? theme::good : theme::textFaint);
        g.setFont (theme::font (11.5f, true));
        g.drawText (on ? "you hear yourself" : "silent", a.withTrimmedTop (a.getHeight() - 22.0f), juce::Justification::centred);
        return true;
    }
    if (art == Art::fader)
    {
        // The knob is dragged down, then a double-click brings it back to 0 dB.
        const float cycle = std::fmod (t * 0.45f, 1.0f);
        const float zero = 0.72f, low = 0.3f;
        float pos = zero;
        if (cycle < 0.35f) pos = zero - (zero - low) * (cycle / 0.35f);   // dragged down
        else if (cycle < 0.6f) pos = low;                                  // waits
        else pos = zero;                                                    // double-click: back
        const juce::Rectangle<float> track (a.getX() + 90.0f, a.getY() + 12.0f, 4.0f, a.getHeight() - 24.0f);
        g.setColour (theme::outline);
        g.fillRoundedRectangle (track, 2.0f);
        const float zy = track.getBottom() - zero * track.getHeight();
        g.setColour (theme::textFaint);
        g.drawLine (track.getX() - 12.0f, zy, track.getX() - 4.0f, zy, 1.0f);
        g.setFont (theme::font (10.5f));
        g.drawText ("0", juce::Rectangle<float> (track.getX() - 34.0f, zy - 7.0f, 18.0f, 14.0f), juce::Justification::centredRight);
        const float y = track.getBottom() - pos * track.getHeight();
        g.setColour (theme::accentIn.withAlpha (0.7f));
        g.fillRect (track.withTop (y));
        const juce::Rectangle<float> knob (track.getCentreX() - 14.0f, y - 5.0f, 28.0f, 10.0f);
        g.setColour (theme::text);
        g.fillRoundedRectangle (knob, 3.0f);
        if (cycle >= 0.6f && cycle < 0.78f)
        {
            // Double-click ripples.
            for (int i = 0; i < 2; ++i)
            {
                const float k = juce::jlimit (0.0f, 1.0f, (cycle - 0.6f - (float) i * 0.05f) / 0.12f);
                g.setColour (theme::learn.withAlpha (0.8f * (1.0f - k)));
                g.drawEllipse (knob.getCentreX() - 4.0f - 16.0f * k, knob.getCentreY() - 4.0f - 16.0f * k, 8.0f + 32.0f * k, 8.0f + 32.0f * k, 1.5f);
            }
        }
        const float db = pos >= zero - 0.001f ? 0.0f : -40.0f * (zero - pos) / (zero - low) * 0.5f;
        const juce::String readout = db == 0.0f ? "0.0 dB" : juce::String (db, 1) + " dB";
        g.setColour (db == 0.0f ? theme::good : theme::text);
        g.setFont (theme::font (20.0f, true));
        g.drawText (readout, juce::Rectangle<float> (track.getRight() + 40.0f, a.getCentreY() - 26.0f, 130.0f, 28.0f), juce::Justification::centredLeft);
        g.setColour (theme::textDim);
        g.setFont (theme::font (12.0f));
        g.drawText (cycle < 0.6f ? "drag" : "double-click: 0 dB", juce::Rectangle<float> (track.getRight() + 40.0f, a.getCentreY() + 2.0f, 180.0f, 20.0f),
                    juce::Justification::centredLeft);
        return true;
    }
    if (art == Art::muteSolo)
    {
        // Three channels: SOLO on the middle one silences the others, MUTE silences one.
        const float phase = std::fmod (t * 0.35f, 2.0f);
        const bool solo = phase < 1.0f;
        for (int i = 0; i < 3; ++i)
        {
            const juce::Rectangle<float> s (a.getX() + 40.0f + (float) i * 80.0f, a.getY() + 6.0f, 30.0f, a.getHeight() - 38.0f);
            const bool silent = solo ? i != 1 : i == 2;
            const float lvl = silent ? 0.0f : 0.5f + 0.3f * std::abs (std::sin (t * 2.4f + (float) i));
            miniStrip (g, s, theme::accentIn, 0.6f, lvl);
            const juce::Rectangle<float> b (s.getX() - 8.0f, s.getBottom() + 6.0f, 46.0f, 20.0f);
            const bool lit = solo ? i == 1 : i == 2;
            const auto c = solo ? theme::solo : theme::mute;
            g.setColour (lit ? c : theme::raised);
            g.fillRoundedRectangle (b, 4.0f);
            g.setColour (lit ? theme::bg : theme::textDim);
            g.setFont (theme::font (10.5f, true));
            g.drawText (solo ? "SOLO" : "MUTE", b, juce::Justification::centred);
        }
        return true;
    }
    if (art == Art::fxChain)
    {
        // The chain: blocks in a row. Phase 1 "+" adds a block, phase 2 a block is dragged to another place,
        // phase 3 the expand symbol opens its window.
        const float cycle = std::fmod (t * 0.3f, 3.0f);
        const int phase = (int) cycle;
        const float k = cycle - (float) phase;
        const float ease = 1.0f - std::pow (1.0f - juce::jlimit (0.0f, 1.0f, k * 1.6f), 3.0f);
        struct B { const char* name; juce::Colour c; };
        const B blocks[] = { { "EQ", theme::fxEq }, { "GATE", theme::fxGate }, { "COMP", theme::fxComp }, { "LIMITER", theme::fxLimiter } };
        const float bw = 64.0f, gap = 8.0f, top = a.getY() + 24.0f, bh = a.getHeight() - 36.0f;
        const int count = phase == 0 ? 3 : 4;
        const float x0 = a.getX() + 12.0f;
        // Order: phase 2 swaps EQ and GATE (drag), else 0 1 2 3.
        float slot[4] = { 0, 1, 2, 3 };
        if (phase == 1) { slot[0] = ease; slot[1] = 1.0f - ease; }
        if (phase == 2) { slot[0] = 1; slot[1] = 0; }
        for (int i = 0; i < count; ++i)
        {
            auto r = juce::Rectangle<float> (x0 + slot[i] * (bw + gap), top, bw, bh);
            const float alpha = 1.0f;
            if (phase == 1 && i == 0) r = r.translated (0.0f, -8.0f * std::sin (ease * pi));   // lifted while dragged
            theme::drawCard (g, r, theme::panel2, theme::cardLine, blocks[i].c.withAlpha (alpha));
            g.setColour (blocks[i].c);
            g.setFont (theme::font (10.5f, true));
            g.drawText (blocks[i].name, r.withHeight (18.0f).translated (6.0f, 4.0f), juce::Justification::centredLeft);
            g.setColour (theme::raised);
            g.fillEllipse (r.getCentreX() - 11.0f, r.getCentreY() - 4.0f, 22.0f, 22.0f);
            g.setColour (blocks[i].c.withAlpha (0.8f));
            g.drawEllipse (r.getCentreX() - 9.0f, r.getCentreY() - 2.0f, 18.0f, 18.0f, 1.5f);
        }
        // Phase 0: the new block grows in from the "+".
        const float plusX = x0 + 3.0f * (bw + gap);
        if (phase == 0)
        {
            const auto r = juce::Rectangle<float> (plusX, top, bw * ease, bh);
            if (ease > 0.05f)
                theme::drawCard (g, r, theme::panel2, theme::cardLine, blocks[3].c.withAlpha (ease));
            if (ease > 0.7f)
            {
                g.setColour (blocks[3].c.withAlpha ((ease - 0.7f) / 0.3f));
                g.setFont (theme::font (10.5f, true));
                g.drawText (blocks[3].name, r.withHeight (18.0f).translated (6.0f, 4.0f), juce::Justification::centredLeft);
            }
            const auto plus = juce::Rectangle<float> (plusX + bw * ease + 6.0f, top + bh / 2.0f - 12.0f, 24.0f, 24.0f);
            g.setColour (theme::accentIn.withAlpha (0.25f));
            g.fillEllipse (plus);
            g.setColour (theme::accentIn);
            g.drawLine (plus.getCentreX() - 6.0f, plus.getCentreY(), plus.getCentreX() + 6.0f, plus.getCentreY(), 2.0f);
            g.drawLine (plus.getCentreX(), plus.getCentreY() - 6.0f, plus.getCentreX(), plus.getCentreY() + 6.0f, 2.0f);
        }
        if (phase == 2)
        {
            // The expand symbol of COMP, then its window slides open.
            const auto blockR = juce::Rectangle<float> (x0 + 2.0f * (bw + gap), top, bw, bh);
            const auto ex = juce::Rectangle<float> (blockR.getRight() - 16.0f, blockR.getY() + 5.0f, 11.0f, 11.0f);
            g.setColour (theme::learn);
            g.drawRect (ex, 1.2f);
            const auto win = juce::Rectangle<float> (blockR.getRight() + 30.0f, a.getY() + 6.0f, (a.getRight() - blockR.getRight() - 40.0f) * ease, a.getHeight() - 12.0f);
            if (win.getWidth() > 4.0f)
            {
                g.setColour (theme::panel2);
                g.fillRoundedRectangle (win, 6.0f);
                g.setColour (theme::fxComp);
                g.drawRoundedRectangle (win, 6.0f, 1.0f);
                juce::Path curve;
                const auto c = win.reduced (10.0f, 14.0f);
                curve.startNewSubPath (c.getX(), c.getBottom());
                curve.lineTo (c.getX() + c.getWidth() * 0.55f, c.getY() + c.getHeight() * 0.4f);
                curve.lineTo (c.getRight(), c.getY() + c.getHeight() * 0.25f);
                g.strokePath (curve, juce::PathStrokeType (1.5f));
            }
        }
        const char* captions[] = { "+ adds an effect or a VST3 plugin", "drag a block to change the order", "the expand symbol opens all settings" };
        g.setColour (theme::text);
        g.setFont (theme::font (12.0f, true));
        g.drawText (captions[phase], a.withHeight (20.0f).translated (12.0f, 2.0f), juce::Justification::centredLeft);
        return true;
    }
    return false;
}
} // namespace

//==============================================================================
struct Tour::Step
{
    juce::String title, text, task;                                   // task: what the user should try ("" = none)
    std::function<std::vector<juce::Rectangle<int>>()> lit;           // highlighted areas, the first `clicks` take clicks
    int clicks = 0;
    std::function<void()> enter;                                      // when the step is shown
    std::function<bool()> done;                                       // the task is done
    int art = Art::none;
    int paths = 0;                                                    // 1: routing of the tour's input (all buses), 2: hint to H
};

//==============================================================================
class Tour::Card final : public juce::Component
{
public:
    explicit Card (Tour& t) : tour (t)
    {
        addAndMakeVisible (backBtn);
        addAndMakeVisible (nextBtn);
        backBtn.onClick = [this] { tour.back(); };
        nextBtn.onClick = [this] { tour.next(); };
    }

    void set (const Step& s, int index, int count, bool taskDone)
    {
        step = &s;
        stepIndex = index;
        stepCount = count;
        done = taskDone;
        backBtn.setVisible (index > 0);
        nextBtn.setText (index == count - 1 ? "Finish" : (s.task.isNotEmpty() && ! taskDone ? "Skip" : "Next"));
        nextBtn.spec.role = (s.task.isNotEmpty() && ! taskDone) ? dialog::Button::Role::normal : dialog::Button::Role::primary;
        layoutContent();
        repaint();
    }

    int artHeight() const { return step != nullptr && step->art != Art::none ? 140 : 0; }

    void layoutContent()
    {
        if (step == nullptr) return;
        const float w = (float) (kCardW - 2 * kPad);
        title = layoutText (step->title, theme::font (20.0f, true), theme::text, w);
        body = layoutText (step->text, theme::font (15.0f), theme::textDim.brighter (0.25f), w - 2.0f);
        taskLayout = layoutText (step->task, theme::font (14.5f, true), theme::text, w - 28.0f);
        int h = kPad + artHeight() + 18 + (int) std::ceil (title.getHeight()) + 6 + (int) std::ceil (body.getHeight());
        if (step->task.isNotEmpty()) h += 12 + juce::jmax (24, (int) std::ceil (taskLayout.getHeight()) + 10);
        h += 18 + 36 + kPad - 2;
        setSize (kCardW, h);
    }

    void resized() override
    {
        auto row = juce::Rectangle<int> (kPad, getHeight() - kPad + 2 - 34, getWidth() - 2 * kPad, 34);
        nextBtn.setBounds (row.removeFromRight (juce::jmax (96, nextBtn.idealWidth())));
        row.removeFromRight (8);
        backBtn.setBounds (row.removeFromRight (90));
        closeArea = { getWidth() - 34, 8, 26, 26 };
    }

    void paint (juce::Graphics& g) override
    {
        if (step == nullptr) return;
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        juce::Path shape;
        shape.addRoundedRectangle (r, 12.0f);
        g.setColour (theme::panel);
        g.fillPath (shape);
        // Gradient line on top (the logo's cyan -> violet).
        {
            juce::Graphics::ScopedSaveState s (g);
            g.reduceClipRegion (shape);
            g.setGradientFill (juce::ColourGradient (theme::accentIn, r.getX(), 0, theme::accentHw, r.getRight(), 0, false));
            g.fillRect (r.withHeight (3.0f));
        }
        g.setColour (theme::outline);
        g.strokePath (shape, juce::PathStrokeType (1.0f));

        float y = (float) kPad;
        if (artHeight() > 0)
        {
            paintArt (g, juce::Rectangle<float> ((float) kPad, y, (float) (getWidth() - 2 * kPad), (float) artHeight() - 12));
            y += (float) artHeight();
        }
        // Step counter: dots.
        const float dot = 6.0f, gap = 4.0f;
        const int shown = juce::jmin (stepCount, 20);
        for (int i = 0; i < shown; ++i)
        {
            const bool isNow = i == stepIndex;
            g.setColour (isNow ? theme::accentIn : i < stepIndex ? theme::textDim : theme::outline);
            const float x = (float) kPad + (float) i * (dot + gap);
            if (isNow) g.fillRoundedRectangle (x, y + 3.0f, dot * 2.2f, dot, dot / 2);
            else       g.fillEllipse (x + (i > stepIndex ? dot * 1.2f : 0.0f), y + 3.0f, dot, dot);
        }
        g.setColour (theme::textFaint);
        g.setFont (theme::font (12.5f));
        g.drawText (juce::String (stepIndex + 1) + " / " + juce::String (stepCount), getWidth() - kPad - 80, (int) y - 1, 80 - 30, 14,
                    juce::Justification::centredRight);
        y += 18.0f;
        title.draw (g, { (float) kPad, y, (float) (getWidth() - 2 * kPad), title.getHeight() });
        y += title.getHeight() + 6.0f;
        body.draw (g, { (float) kPad, y, (float) (getWidth() - 2 * kPad), body.getHeight() });
        y += body.getHeight();
        if (step->task.isNotEmpty())
        {
            y += 12.0f;
            const float th = juce::jmax (24.0f, taskLayout.getHeight() + 10.0f);
            const juce::Rectangle<float> box ((float) kPad, y, (float) (getWidth() - 2 * kPad), th);
            const auto c = done ? theme::good : theme::learn;
            g.setColour (c.withAlpha (0.10f));
            g.fillRoundedRectangle (box, 8.0f);
            g.setColour (c.withAlpha (0.45f));
            g.drawRoundedRectangle (box.reduced (0.5f), 8.0f, 1.0f);
            const auto mark = juce::Rectangle<float> (box.getX() + 8.0f, box.getCentreY() - 6.0f, 12.0f, 12.0f);
            if (done)
            {
                juce::Path tick;
                tick.startNewSubPath (mark.getX() + 1.5f, mark.getCentreY());
                tick.lineTo (mark.getX() + 5.0f, mark.getBottom() - 2.0f);
                tick.lineTo (mark.getRight() - 1.0f, mark.getY() + 2.0f);
                g.setColour (theme::good);
                g.strokePath (tick, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }
            else
            {
                const float pulse = 0.5f + 0.5f * std::sin ((float) phase * 5.0f);
                g.setColour (c.withAlpha (0.35f + 0.4f * pulse));
                g.fillEllipse (mark.reduced (3.0f - 2.0f * pulse));
                g.setColour (c);
                g.fillEllipse (mark.reduced (3.5f));
            }
            taskLayout.draw (g, box.withTrimmedLeft (26.0f).withSizeKeepingCentre (box.getWidth() - 34.0f, taskLayout.getHeight()));
        }

        // Close (pause) cross.
        g.setColour (closeHover ? theme::text : theme::textFaint);
        const auto c = closeArea.toFloat().reduced (8.0f);
        g.drawLine (c.getX(), c.getY(), c.getRight(), c.getBottom(), 1.6f);
        g.drawLine (c.getRight(), c.getY(), c.getX(), c.getBottom(), 1.6f);
    }

    /** Animated area (the art and the task dot): repainted each frame. */
    juce::Rectangle<int> animatedArea() const
    {
        if (step == nullptr) return {};
        if (step->task.isNotEmpty() && ! done) return getLocalBounds();   // the pulsing dot of the task (the card is small)
        return artHeight() > 0 ? juce::Rectangle<int> (0, kPad, getWidth(), artHeight()) : juce::Rectangle<int>();
    }

    void setPhase (double t) { phase = t; }
    void setPhaseForDemo (double t) { phase = t; repaint(); }
    void mouseMove (const juce::MouseEvent& e) override { setCloseHover (closeArea.contains (e.getPosition())); }
    void mouseExit (const juce::MouseEvent&) override   { setCloseHover (false); }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (closeArea.contains (e.getPosition()) && e.mouseWasClicked()) tour.pause();
    }

private:
    void setCloseHover (bool h) { if (h != closeHover) { closeHover = h; repaint (closeArea); } }

    void paintArt (juce::Graphics& g, juce::Rectangle<float> a)
    {
        const float t = (float) phase;
        g.setColour (theme::bg.withAlpha (0.6f));
        g.fillRoundedRectangle (a, 10.0f);
        if (paintMoreArt (g, a, step->art, t)) return;
        if (step->art == Art::flow)
        {
            // Sources -> Hydra -> outputs, dots flowing through.
            const char* srcs[] = { "Microphone", "Game", "Spotify" };
            const char* outs[] = { "Headphones", "Speakers", "Discord" };
            const juce::Colour outCol[] = { theme::accentHw, theme::accentHw, theme::accentVirt };
            const juce::Rectangle<float> logo (a.getCentreX() - 17.0f, a.getCentreY() - 19.0f, 34.0f, 38.0f);
            for (int i = 0; i < 3; ++i)
            {
                const float y = a.getY() + 18.0f + (float) i * ((a.getHeight() - 36.0f) / 2.0f);
                const juce::Rectangle<float> s (a.getX() + 10.0f, y - 10.0f, 86.0f, 20.0f), o (a.getRight() - 96.0f, y - 10.0f, 86.0f, 20.0f);
                juce::Path in, out;
                in.startNewSubPath (s.getRight(), y);
                in.cubicTo ({ s.getRight() + 30.0f, y }, { logo.getX() - 30.0f, logo.getCentreY() }, { logo.getX() - 4.0f, logo.getCentreY() });
                out.startNewSubPath (logo.getRight() + 4.0f, logo.getCentreY());
                out.cubicTo ({ logo.getRight() + 30.0f, logo.getCentreY() }, { o.getX() - 30.0f, y }, { o.getX(), y });
                g.setColour (theme::outline);
                g.strokePath (in, juce::PathStrokeType (1.0f));
                g.strokePath (out, juce::PathStrokeType (1.0f));
                flowDots (g, in, theme::accentIn, t + (float) i * 0.13f, 18.0f, 2.0f);
                flowDots (g, out, outCol[i], t + (float) i * 0.21f, 18.0f, 2.0f);
                for (auto [r, text, col] : { std::tuple { s, srcs[i], theme::accentIn }, std::tuple { o, outs[i], outCol[i] } })
                {
                    g.setColour (col.withAlpha (0.14f));
                    g.fillRoundedRectangle (r, 6.0f);
                    g.setColour (col.withAlpha (0.7f));
                    g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
                    g.setColour (theme::text);
                    g.setFont (theme::font (11.5f, true));
                    g.drawText (text, r, juce::Justification::centred);
                }
            }
            theme::drawLogo (g, logo);
        }
        else if (step->art == Art::rename)
        {
            // A1 -> H1, B1 -> V1 (Voicemeeter's names next to Hydra's).
            const struct { const char* from; const char* to; const char* what; juce::Colour c; } rows[] = {
                { "A1", "H1", "hardware outputs", theme::accentHw }, { "B1", "V1", "virtual outputs", theme::accentVirt } };
            for (int i = 0; i < 2; ++i)
            {
                const float y = a.getY() + 18.0f + (float) i * 44.0f;
                const juce::Rectangle<float> from (a.getX() + 22.0f, y, 48.0f, 30.0f), to (a.getX() + 140.0f, y, 48.0f, 30.0f);
                g.setColour (theme::raised);
                g.fillRoundedRectangle (from, 6.0f);
                g.setColour (theme::textDim);
                g.setFont (theme::font (14.0f, true));
                g.drawText (rows[i].from, from, juce::Justification::centred);
                juce::Path p;
                p.startNewSubPath (from.getRight() + 8.0f, from.getCentreY());
                p.lineTo (to.getX() - 10.0f, to.getCentreY());
                g.setColour (theme::outline);
                g.strokePath (p, juce::PathStrokeType (1.0f));
                flowDots (g, p, rows[i].c, t, 14.0f, 2.0f);
                const float pulse = 0.5f + 0.5f * std::sin (t * 3.0f + (float) i);
                g.setColour (rows[i].c.withAlpha (0.18f + 0.12f * pulse));
                g.fillRoundedRectangle (to, 6.0f);
                g.setColour (rows[i].c);
                g.drawRoundedRectangle (to.reduced (0.5f), 6.0f, 1.2f);
                g.setColour (theme::text);
                g.drawText (rows[i].to, to, juce::Justification::centred);
                g.setColour (theme::textDim);
                g.setFont (theme::font (12.5f));
                g.drawText (rows[i].what, juce::Rectangle<float> (to.getRight() + 12.0f, y, a.getRight() - to.getRight() - 16.0f, 30.0f),
                            juce::Justification::centredLeft);
            }
        }
        else if (step->art == Art::rightClick)
        {
            // A control with its right-click menu.
            const juce::Rectangle<float> knob (a.getX() + 34.0f, a.getCentreY() - 22.0f, 44.0f, 44.0f);
            g.setColour (theme::raised);
            g.fillEllipse (knob);
            g.setColour (theme::accentIn);
            g.drawEllipse (knob.reduced (3.0f), 2.0f);
            const juce::Rectangle<float> menu (knob.getRight() + 34.0f, a.getY() + 10.0f, 170.0f, a.getHeight() - 20.0f);
            g.setColour (theme::panel2);
            g.fillRoundedRectangle (menu, 7.0f);
            g.setColour (theme::outline);
            g.drawRoundedRectangle (menu, 7.0f, 1.0f);
            const char* items[] = { "MIDI Learn", "Keyboard shortcut...", "Reset to default" };
            const int lit = ((int) (t * 0.8f)) % 3;
            for (int i = 0; i < 3; ++i)
            {
                const auto row = menu.reduced (5.0f).withHeight (22.0f).translated (0.0f, (float) i * 24.0f + 2.0f);
                if (i == lit) { g.setColour (theme::fx.withAlpha (0.25f)); g.fillRoundedRectangle (row, 5.0f); }
                g.setColour (i == lit ? theme::text : theme::textDim);
                g.setFont (theme::font (12.5f));
                g.drawText (items[i], row.withTrimmedLeft (8.0f), juce::Justification::centredLeft);
            }
            // Mouse pointer with a right-click ripple.
            const juce::Point<float> m (knob.getCentreX() + 8.0f, knob.getCentreY() + 6.0f);
            const float ripple = std::fmod (t * 0.9f, 1.0f);
            g.setColour (theme::text.withAlpha (0.5f * (1.0f - ripple)));
            g.drawEllipse (m.x - 4.0f - 14.0f * ripple, m.y - 4.0f - 14.0f * ripple, 8.0f + 28.0f * ripple, 8.0f + 28.0f * ripple, 1.4f);
            juce::Path arrow;
            arrow.addTriangle (m, m.translated (0.0f, 16.0f), m.translated (11.0f, 11.0f));
            g.setColour (theme::text);
            g.fillPath (arrow);
        }
        else if (step->art == Art::clock)
        {
            // One master clock, other devices synced to it.
            const juce::Point<float> c (a.getX() + 70.0f, a.getCentreY());
            g.setColour (theme::accentIn.withAlpha (0.18f));
            g.fillEllipse (c.x - 30.0f, c.y - 30.0f, 60.0f, 60.0f);
            g.setColour (theme::accentIn);
            g.drawEllipse (c.x - 30.0f, c.y - 30.0f, 60.0f, 60.0f, 1.5f);
            const float ang = t * 2.0f;
            g.drawLine (c.x, c.y, c.x + 22.0f * std::sin (ang), c.y - 22.0f * std::cos (ang), 2.0f);
            g.setFont (theme::font (11.0f, true));
            g.drawText ("MASTER", juce::Rectangle<float> (c.x - 40.0f, c.y + 31.0f, 80.0f, 14.0f), juce::Justification::centred);
            const char* devs[] = { "USB headset", "Virtual cable", "Other interface" };
            for (int i = 0; i < 3; ++i)
            {
                const juce::Rectangle<float> d (a.getRight() - 140.0f, a.getY() + 10.0f + (float) i * 30.0f, 126.0f, 22.0f);
                juce::Path p;
                p.startNewSubPath (c.x + 32.0f, c.y);
                p.cubicTo ({ c.x + 80.0f, c.y }, { d.getX() - 40.0f, d.getCentreY() }, { d.getX(), d.getCentreY() });
                g.setColour (theme::outline);
                g.strokePath (p, juce::PathStrokeType (1.0f));
                flowDots (g, p, theme::accentIn, t + (float) i * 0.3f, 16.0f, 1.8f);
                g.setColour (theme::raised);
                g.fillRoundedRectangle (d, 6.0f);
                g.setColour (theme::textDim);
                g.setFont (theme::font (11.5f));
                g.drawText (devs[i], d, juce::Justification::centred);
            }
        }
    }

    Tour& tour;
    const Step* step = nullptr;
    int stepIndex = 0, stepCount = 1;
    bool done = false, closeHover = false;
    double phase = 0.0;
    juce::TextLayout title, body, taskLayout;
    juce::Rectangle<int> closeArea;
    dialog::ActionButton backBtn { { "Back" }, theme::accentIn }, nextBtn { { "Next", dialog::Button::Role::primary }, theme::accentIn };
};

//==============================================================================
Tour::Tour (UiContext& c, juce::Component& r, Host h, Kind k, int step)
    : ctx (c), root (r), host (std::move (h)), kind (k)
{
    auto& e = ctx.engine;
    in = 0;
    for (int i = 0; i < kMaxInputs; ++i) if (e.isInputActive (i)) { in = i; break; }
    hw = 0;
    for (int b = 0; b < kMaxHwBuses; ++b) if (e.isBusActive (b)) { hw = b; break; }
    virt = -1;
    for (int b = kMaxHwBuses; b < kMaxBuses; ++b) if (e.isBusActive (b)) { virt = b; break; }

    if (kind == Kind::voicemeeter) buildVoicemeeter();
    else buildFull();

    card = std::make_unique<Card> (*this);
    addAndMakeVisible (*card);
    setWantsKeyboardFocus (true);
    setAlwaysOnTop (true);
    root.addAndMakeVisible (this);
    setBounds (root.getLocalBounds());
    startTime = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    show (juce::jlimit (0, (int) steps.size() - 1, step));
    grabKeyboardFocus();
}

Tour::~Tour() = default;

int Tour::getNumSteps() const noexcept { return (int) steps.size(); }

void Tour::demoShow (int step, double t)
{
    show (step);
    lastT = t;
    card->setPhaseForDemo (t);
    updateSpots();
    repaint();
}

bool Tour::parseState (const juce::String& s, Kind& k, int& step)
{
    if (! s.containsChar (':')) return false;
    const auto id = s.upToFirstOccurrenceOf (":", false, false);
    k = id == "vm" ? Kind::voicemeeter : Kind::full;
    step = s.fromFirstOccurrenceOf (":", false, false).getIntValue();
    return id == "vm" || id == "full";
}

juce::Component* Tour::find (const juce::String& id) const
{
    // Cached: the tour asks for the same few dozen ids every frame (the strips are rebuilt now and then).
    if (auto it = found.find (id); it != found.end())
        if (auto* c = it->second.getComponent(); c != nullptr && c->getComponentID() == id && root.isParentOf (c))
            return c;
    std::function<juce::Component* (juce::Component&)> search = [&] (juce::Component& c) -> juce::Component*
    {
        for (auto* child : c.getChildren())
        {
            if (child == this) continue;
            if (child->getComponentID() == id) return child;
            if (auto* f = search (*child)) return f;
        }
        return nullptr;
    };
    auto* c = search (root);
    if (c != nullptr) found[id] = c;
    return c;
}

juce::Rectangle<int> Tour::rectOf (const juce::String& id) const
{
    auto* c = find (id);
    if (c == nullptr) return {};
    for (auto* p = c; p != nullptr && p != &root; p = p->getParentComponent())
        if (! p->isVisible()) return {};   // (not isShowing: also right without a window, for the screenshot tool)
    auto r = getLocalArea (c, c->getLocalBounds());
    // Inside a scrolling pane: only the visible part.
    for (auto* p = c->getParentComponent(); p != nullptr && p != &root; p = p->getParentComponent())
        if (auto* vp = dynamic_cast<juce::Viewport*> (p))
            r = r.getIntersection (getLocalArea (vp, vp->getLocalBounds()));
    return r;
}

juce::Rectangle<int> Tour::groupRect (int group) const
{
    juce::Rectangle<int> u;
    const int from = group == 0 ? 0 : group == 1 ? 0 : kMaxHwBuses, to = group == 0 ? kMaxInputs : group == 1 ? kMaxHwBuses : kMaxBuses;
    for (int i = from; i < to; ++i)
    {
        const auto r = rectOf ((group == 0 ? "in" : "bus") + juce::String (i));
        if (! r.isEmpty()) u = u.isEmpty() ? r : u.getUnion (r);
    }
    return u;
}

juce::Point<float> Tour::routeStart (int input, int bus) const
{
    const auto r = rectOf ("in" + juce::String (input) + ".route." + juce::String (bus));
    return r.isEmpty() ? juce::Point<float> (-1, -1) : r.toFloat().getCentre();
}

juce::Point<float> Tour::busEnd (int bus) const
{
    // The left edge of the output's strip, at the height of the tour input's routing buttons (a flat arc over the
    // strips in between, not up through the captions).
    const auto r = rectOf ("bus" + juce::String (bus));
    if (r.isEmpty()) return { -1, -1 };
    const auto from = routeStart (in, bus);
    const float y = from.y < 0 ? (float) r.getCentreY() : juce::jlimit ((float) r.getY() + 20.0f, (float) r.getBottom() - 20.0f, from.y);
    return { (float) r.getX() + 10.0f, y };
}

//==============================================================================
void Tour::buildFull()
{
    auto& e = ctx.engine;
    const juce::String inName = e.inputs[(size_t) in].name;
    const juce::String hwLabel = busLabel (hw), hwName = e.buses[(size_t) hw].name;
    auto* route = e.inputs[(size_t) in].route[(size_t) hw];
    auto* gain = e.inputs[(size_t) in].gain;
    const juce::String si (in), sh (hw);
    // The device window: done once it was open in this step and is closed again.
    auto pickerClosed = [this]
    {
        const bool open = host.windowOpen != nullptr && host.windowOpen ("device");
        if (open && ! pickerSeen && host.moveDeviceWindowAside && card != nullptr)
            host.moveDeviceWindowAside (card->getScreenBounds());   // the card explains it: both must be visible
        if (open) pickerSeen = true;
        return pickerSeen && ! open;
    };

    steps.push_back ({ "Welcome to Hydra",
                       "Hydra takes the sound of your sources and mixes it for each output: headphones, speakers, Discord, a stream. "
                       "Two minutes, mostly you clicking. Esc pauses the tour.", {}, {}, 0, {}, {}, Art::flow });

    steps.push_back ({ "Inputs",
                       "Every channel here is one source: your microphone, a game, Spotify, a virtual cable. Add more with +.", {},
                       [this] { return std::vector<juce::Rectangle<int>> { groupRect (0), rectOf ("add.in") }; }, 0, {}, {}, Art::sources });

    steps.push_back ({ "What an input hears",
                       "The source of a channel is shown at its top. Click it to choose another one.",
                       "Click the source of " + inName + ".",
                       [this, si] { return std::vector<juce::Rectangle<int>> { rectOf ("in" + si + ".source") }; }, 1,
                       [this] { if (host.reveal) host.reveal (in); },
                       [this] { return host.windowOpen != nullptr && host.windowOpen ("device"); } });

    steps.push_back ({ "Choosing a source",
                       "1. the kind of connection: hardware, a Windows device, an app (only its sound), a cable or another Hydra. "
                       "2. the device. 3. its channels. A click takes it at once, a double-click also closes the window.",
                       "Pick a source (or keep it) and close the window.",
                       [this, si] { return std::vector<juce::Rectangle<int>> { rectOf ("in" + si + ".source") }; }, 0,
                       [this] { pickerSeen = false; }, pickerClosed, Art::picker });

    steps.push_back ({ "Hardware outputs (H)",
                       "Your speakers, headphones and audio interfaces. Each H output plays its own mix to one device.", {},
                       [this] { return std::vector<juce::Rectangle<int>> { groupRect (1), rectOf ("add.hw") }; }, 0, {}, {}, Art::outputs });

    steps.push_back ({ "Where an output plays",
                       "The device of an output is shown at its top, like on the inputs.",
                       "Click the device of " + hwName + ".",
                       [this, sh] { return std::vector<juce::Rectangle<int>> { rectOf ("bus" + sh + ".output") }; }, 1,
                       [this] { if (host.reveal) host.reveal (busSelectIndex (hw)); },
                       [this] { return host.windowOpen != nullptr && host.windowOpen ("device"); } });

    steps.push_back ({ "Choosing an output device",
                       "The same window: connection, device, channels. Hardware outputs list your interfaces and Windows playback devices; "
                       "a channel pair like 3 / 4 is e.g. the headphone output of an interface.",
                       "Pick a device (or keep it) and close the window.",
                       [this, sh] { return std::vector<juce::Rectangle<int>> { rectOf ("bus" + sh + ".output") }; }, 0,
                       [this] { pickerSeen = false; }, pickerClosed, Art::pickerOut });

    if (virt >= 0)
        steps.push_back ({ "Virtual outputs (V)",
                           "Mixes for other apps: Discord, OBS, a recording. Each plays into a virtual cable, and the app picks that cable as its microphone.", {},
                           [this] { return std::vector<juce::Rectangle<int>> { groupRect (2), rectOf ("add.virt") }; }, 0, {}, {}, Art::virtualOut });

    steps.push_back ({ "Routing: hear yourself",
                       "These buttons send " + inName + " to the outputs. Lit = it plays there. Switch on " + hwLabel
                           + " and you hear " + inName + " on " + hwName + " (watch the path).",
                       route->isOn() ? "It is on: click " + hwLabel + " to switch it off, then on again."
                                     : "Click " + hwLabel + " on " + inName + ".",
                       [this] { const juce::String r = "in" + juce::String (in) + ".route." + juce::String (hw);
                                return std::vector<juce::Rectangle<int>> { rectOf (r), rectOf ("bus" + juce::String (hw)) }; }, 1,
                       [this, route] { if (host.reveal) host.reveal (in); routeWas = route->isOn(); sawOff = false; },
                       [this, route]
                       {
                           if (! route->isOn()) sawOff = true;
                           return route->isOn() && (! routeWas || sawOff);
                       }, Art::hearYourself, 2 });

    steps.push_back ({ "One source, many mixes",
                       "Switch on as many routes as you like: your mic to your headphones, to Discord and to the recording, "
                       "each with its own level (SEL on an output).", {},
                       [this] { const auto r = rectOf ("in" + juce::String (in));
                                return std::vector<juce::Rectangle<int>> { r, groupRect (1), groupRect (2) }; }, 1,
                       {}, {}, Art::flow, 1 });

    steps.push_back ({ "Level",
                       "Drag the fader up or down. The meter beside it shows what comes in. A click on the value lets you type one.",
                       "Move the fader of " + inName + ".",
                       [this, si] { return std::vector<juce::Rectangle<int>> { rectOf ("in" + si + ".fader") }; }, 1,
                       [this, gain] { gainVersion = gain->getVersion(); },
                       [this, gain] { return gain->getVersion() != gainVersion; }, Art::fader });

    steps.push_back ({ "Back to 0 dB",
                       "A double-click puts any fader or knob back to its default, here 0 dB.",
                       "Double-click the fader.",
                       [this, si] { return std::vector<juce::Rectangle<int>> { rectOf ("in" + si + ".fader") }; }, 1,
                       [this, gain] { sawAway = std::abs (gain->get() - gain->defaultValue) > 0.01f; },
                       [this, gain]
                       {
                           const bool atDefault = std::abs (gain->get() - gain->defaultValue) <= 0.01f;
                           if (! atDefault) sawAway = true;
                           return atDefault && sawAway;
                       }, Art::fader });

    steps.push_back ({ "Mute and solo",
                       "MUTE silences the channel, SOLO plays only it (Shift + click: several).", {},
                       [this, si] { return std::vector<juce::Rectangle<int>> { rectOf ("in" + si + ".mute"), rectOf ("in" + si + ".solo") }; }, 2,
                       {}, {}, Art::muteSolo });

    steps.push_back ({ "Effects of a channel",
                       "Each channel has its own effects. They show for the selected channel.",
                       "Click " + inName + " to select it.",
                       [this, si] { return std::vector<juce::Rectangle<int>> { rectOf ("in" + si) }; }, 1,
                       [this] { if (host.reveal) host.reveal (in); },
                       [this] { return ctx.selected == in; } });

    steps.push_back ({ "The effect chain",
                       "The sound runs from left to right through these blocks. Add effects, edit them with their knobs or open "
                       "all settings, drag them into another order.", {},
                       [this] { return std::vector<juce::Rectangle<int>> { rectOf ("inspector") }; }, 1, {}, {}, Art::fxChain });

    steps.push_back ({ "Add an effect",
                       "+ adds EQ, denoiser, gate, compressor, limiter, stems, transpose or a VST3 plugin.",
                       "Click + and pick one (or skip).",
                       [this] { return std::vector<juce::Rectangle<int>> { rectOf ("fx.add") }; }, 1,
                       [this] { chainWas = chainVersion(); },
                       [this] { return chainVersion() != chainWas; } });

    steps.push_back ({ "Settings and tools",
                       "Status and latency of the audio engine, the tools (scenes, Latency Finder, MIDI learn, recorder) and the settings.", {},
                       [this] { return std::vector<juce::Rectangle<int>> { rectOf ("top.status"), rectOf ("top.tools"), rectOf ("top.settings") }; }, 0,
                       {}, {}, Art::clock });

    steps.push_back ({ "Right-click anything",
                       "Every knob, fader and button: MIDI learn, a keyboard shortcut, reset. Right-click a strip to rename, copy or reorder.",
                       {}, {}, 0, {}, {}, Art::rightClick });

    steps.push_back ({ "That's it",
                       "Have fun mixing. You can watch this tour again any time: Tools > Guided tour.", {}, {}, 0, {}, {}, Art::flow });
}

void Tour::buildVoicemeeter()
{
    auto* route = ctx.engine.inputs[(size_t) in].route[(size_t) hw];
    const juce::String si (in);
    steps.push_back ({ "Coming from Voicemeeter",
                       "Most of it will feel familiar. Here is what is different, in one minute.", {}, {}, 0, {}, {}, Art::rename });

    steps.push_back ({ "A and B are now H and V",
                       "A1, A2 ... are hardware outputs H1-H8, B1, B2 ... are virtual outputs V1-V8. Your import already uses them.", {},
                       [this] { return std::vector<juce::Rectangle<int>> { groupRect (1), groupRect (2) }; }, 0, {}, {}, Art::rename });

    steps.push_back ({ "Inputs are all the same",
                       "No fixed hardware and virtual strips: up to 16 inputs, and each can be any source. Add one with +.", {},
                       [this] { return std::vector<juce::Rectangle<int>> { groupRect (0), rectOf ("add.in") }; }, 0, {}, {}, Art::sources });

    steps.push_back ({ "Apps without a cable",
                       "A source can be an app directly (Spotify, a game): Hydra hears only that app, no virtual cable needed. "
                       "Choose Application in the first column.",
                       "Click the source of a channel.",
                       [this, si] { return std::vector<juce::Rectangle<int>> { rectOf ("in" + si + ".source") }; }, 1,
                       [this] { if (host.reveal) host.reveal (in); },
                       [this] { return host.windowOpen != nullptr && host.windowOpen ("device"); }, Art::picker });

    steps.push_back ({ "Routing as you know it",
                       "The buttons work like A1 / B1 in Voicemeeter, there are just as many as you have outputs.",
                       "Click " + busLabel (hw) + ".",
                       [this] { const juce::String r = "in" + juce::String (in) + ".route." + juce::String (hw);
                                return std::vector<juce::Rectangle<int>> { rectOf (r), rectOf ("bus" + juce::String (hw)) }; }, 1,
                       [this, route] { if (host.reveal) host.reveal (in); routeWas = route->isOn(); },
                       [this, route] { return route->isOn() != routeWas; }, Art::hearYourself, 2 });

    steps.push_back ({ "Effects per channel",
                       "Every channel and output has its own chain, in any order, with VST3 plugins. Your EQ, gate, compressor and denoiser were imported as blocks.",
                       {}, [this] { return std::vector<juce::Rectangle<int>> { rectOf ("inspector") }; }, 1, {}, {}, Art::fxChain });

    steps.push_back ({ "One clock",
                       "The master device (top) is the clock, like A1 in Voicemeeter. Every other device is synced to it automatically, ASIO or not.",
                       {}, [this] { return std::vector<juce::Rectangle<int>> { rectOf ("top.status") }; }, 0, {}, {}, Art::clock });

    steps.push_back ({ "Tools",
                       "Scenes recall whole mixes, the Latency Finder shows the delay of every path. Right-click any control for MIDI learn: your Voicemeeter MIDI map was imported.",
                       {}, [this] { return std::vector<juce::Rectangle<int>> { rectOf ("top.tools") }; }, 0, {}, {}, Art::rightClick });

    steps.push_back ({ "Ready",
                       "That's the difference. The full tour is in Tools > Guided tour if you want to see more.", {}, {}, 0, {}, {}, Art::flow });
}

uint32_t Tour::chainVersion() const
{
    const int s = ctx.selected;
    if (s >= 0 && s < kMaxInputs) return ctx.engine.inputs[(size_t) s].plugins.getVersion();
    if (s >= kMaxInputs && s < kRecorderSelect) return ctx.engine.buses[(size_t) (s - kMaxInputs)].plugins.getVersion();
    return 0;
}

//==============================================================================
void Tour::show (int step)
{
    current = juce::jlimit (0, (int) steps.size() - 1, step);
    taskDone = false;
    doneAt = 0.0;
    const auto& s = steps[(size_t) current];
    if (s.enter) s.enter();
    card->set (s, current, (int) steps.size(), false);
    updateSpots();
    placeCard();
    repaint();
}

void Tour::next()
{
    if (current + 1 >= (int) steps.size()) { finish(); return; }
    show (current + 1);
}

void Tour::back()
{
    if (current > 0) show (current - 1);
}

void Tour::finish()
{
    auto cb = onClose;
    setVisible (false);
    if (cb) cb (true, current);   // may delete this
}

void Tour::pause()
{
    auto cb = onClose;
    setVisible (false);
    if (cb) cb (false, current);   // may delete this
}

void Tour::updateSpots()
{
    const auto& s = steps[(size_t) current];
    std::vector<juce::Rectangle<int>> now;
    if (s.lit)
        for (auto& r : s.lit())
            now.push_back (r.isEmpty() ? r : r.expanded (kSpotPad));
    if (now != spots)
    {
        spots = std::move (now);
        interactiveCount = juce::jmin (s.clicks, (int) spots.size());
        placeCard();
        repaint();
    }
}

void Tour::placeCard()
{
    if (card == nullptr) return;
    const auto area = getLocalBounds().reduced (12);
    const int w = card->getWidth(), h = card->getHeight();
    juce::Rectangle<int> u;
    for (auto& r : spots) if (! r.isEmpty()) u = u.isEmpty() ? r : u.getUnion (r);
    juce::Rectangle<int> pos;
    if (u.isEmpty())
        pos = juce::Rectangle<int> (w, h).withCentre (area.getCentre());
    else
    {
        // Next to the first highlighted area (that is where the user looks), outside all of them if possible.
        const auto first = spots.front().isEmpty() ? u : spots.front();
        auto clampX = [&] (int x) { return juce::jlimit (area.getX(), area.getRight() - w, x); };
        auto clampY = [&] (int y) { return juce::jlimit (area.getY(), area.getBottom() - h, y); };
        const juce::Rectangle<int> candidates[] = {
            { clampX (first.getCentreX() - w / 2), u.getBottom() + 14, w, h },
            { clampX (first.getCentreX() - w / 2), u.getY() - 14 - h, w, h },
            { first.getRight() + 16, clampY (first.getCentreY() - h / 2), w, h },
            { first.getX() - 16 - w, clampY (first.getCentreY() - h / 2), w, h },
            { clampX (first.getCentreX() - w / 2), first.getBottom() + 14, w, h },
            { clampX (first.getCentreX() - w / 2), first.getY() - 14 - h, w, h } };
        pos = { clampX (area.getRight() - w), clampY (area.getBottom() - h), w, h };   // fallback: bottom right
        int bestOverlap = std::numeric_limits<int>::max();
        for (auto& c : candidates)
        {
            if (! area.contains (c)) continue;
            int overlap = 0;
            for (auto& r : spots) { const auto i = r.getIntersection (c); overlap += i.getWidth() * i.getHeight(); }
            if (overlap < bestOverlap) { bestOverlap = overlap; pos = c; }
            if (overlap == 0) break;
        }
    }
    card->setBounds (pos);
}

void Tour::resized()
{
    if (card != nullptr) { updateSpots(); placeCard(); }
}

void Tour::parentSizeChanged()
{
    setBounds (root.getLocalBounds());
}

bool Tour::hitTest (int x, int y)
{
    if (card != nullptr && card->getBounds().contains (x, y)) return true;
    for (int i = 0; i < interactiveCount; ++i)
        if (spots[(size_t) i].contains (x, y)) return false;   // the mixer below gets the click
    return true;   // everything else is dimmed and takes no clicks
}

bool Tour::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey) { pause(); return true; }
    if (k == juce::KeyPress::rightKey || k == juce::KeyPress::returnKey) { next(); return true; }
    if (k == juce::KeyPress::leftKey) { back(); return true; }
    return false;   // the rest (undo, shortcuts) goes on to the main window
}

//==============================================================================
void Tour::frame (double)
{
    if (! isShowing()) return;
    const double t = juce::Time::getMillisecondCounterHiRes() / 1000.0 - startTime;
    lastT = t;
    const auto& s = steps[(size_t) current];
    if (s.task.isNotEmpty() && ! taskDone && s.done && s.done())
    {
        taskDone = true;
        doneAt = t;
        card->set (s, current, (int) steps.size(), true);
        placeCard();
    }
    if (taskDone && doneAt > 0.0 && t - doneAt > 1.1)
    {
        doneAt = 0.0;
        next();
        return;
    }
    updateSpots();
    card->setPhase (t);
    if (auto a = card->animatedArea(); ! a.isEmpty()) card->repaint (a);

    // Only the moving parts: the rings around the highlighted areas and the routing paths.
    juce::Rectangle<int> area;
    for (auto& r : spots) if (! r.isEmpty()) area = area.isEmpty() ? r.expanded (8) : area.getUnion (r.expanded (8));
    if (s.paths != 0)
    {
        juce::Rectangle<int> p;
        for (int b = 0; b < kMaxBuses; ++b)
        {
            const auto a = routeStart (in, b), z = busEnd (b);
            if (a.x < 0 || z.x < 0) continue;
            const auto r = juce::Rectangle<float> (a, z).toNearestInt().withTop ((int) (juce::jmin (a.y, z.y) - 100.0f)).expanded (24);
            p = p.isEmpty() ? r : p.getUnion (r);
        }
        area = area.isEmpty() ? p : area.getUnion (p);
    }
    const auto paintArea = area.getUnion (animArea);
    animArea = area;
    if (! paintArea.isEmpty()) repaint (paintArea);
}

void Tour::paintPaths (juce::Graphics& g, float t)
{
    const auto& s = steps[(size_t) current];
    if (s.paths == 0) return;
    auto& strip = ctx.engine.inputs[(size_t) in];
    for (int b = 0; b < kMaxBuses; ++b)
    {
        if (! ctx.engine.isBusActive (b)) continue;
        const bool on = strip.route[(size_t) b]->isOn();
        const bool hint = s.paths == 2 && b == hw;
        if (! on && ! hint) continue;
        const auto a = routeStart (in, b), z = busEnd (b);
        if (a.x < 0 || z.x < 0) continue;
        const auto p = arc (a, z, 24.0f + 9.0f * (float) (b % kMaxHwBuses));
        const auto c = theme::busColour (b);
        if (on)
        {
            g.setColour (c.withAlpha (0.35f));
            g.strokePath (p, juce::PathStrokeType (2.0f));
            flowDots (g, p, c, t + (float) b * 0.17f);
            // Arrival: a soft pulse at the output.
            const float pulse = std::fmod (t * 1.4f + (float) b * 0.2f, 1.0f);
            g.setColour (c.withAlpha (0.5f * (1.0f - pulse)));
            g.drawEllipse (z.x - 6.0f - 10.0f * pulse, z.y - 6.0f - 10.0f * pulse, 12.0f + 20.0f * pulse, 12.0f + 20.0f * pulse, 1.5f);
        }
        else
        {
            // Not routed yet: a dashed hint where it would go.
            juce::Path dashed;
            const float dashes[] = { 5.0f, 6.0f };
            juce::PathStrokeType (1.5f).createDashedStroke (dashed, p, dashes, 2);
            g.setColour (c.withAlpha (0.35f + 0.25f * std::sin (t * 4.0f)));
            g.fillPath (dashed);
        }
    }
}

void Tour::paint (juce::Graphics& g)
{
    const float t = (float) lastT;
    // Dim everything but the highlighted areas.
    juce::Path dim;
    dim.addRectangle (getLocalBounds().toFloat());
    for (auto& r : spots)
        if (! r.isEmpty()) dim.addRoundedRectangle (r.toFloat(), 9.0f);
    dim.setUsingNonZeroWinding (false);
    g.setColour (juce::Colours::black.withAlpha (0.62f));
    g.fillPath (dim);

    // Rings: the area to click pulses in yellow, the others are framed calmly.
    for (size_t i = 0; i < spots.size(); ++i)
    {
        const auto r = spots[i].toFloat();
        if (r.isEmpty()) continue;
        const bool click = (int) i < interactiveCount && steps[(size_t) current].task.isNotEmpty() && ! taskDone;
        if (click)
        {
            const float pulse = 0.5f + 0.5f * std::sin (t * 4.0f);
            g.setColour (theme::learn.withAlpha (0.25f * pulse));
            g.drawRoundedRectangle (r.expanded (2.0f + 3.0f * pulse), 11.0f, 4.0f);
            g.setColour (theme::learn);
            g.drawRoundedRectangle (r, 9.0f, 2.0f);
        }
        else
        {
            g.setColour (theme::accentIn.withAlpha (0.65f));
            g.drawRoundedRectangle (r, 9.0f, 1.5f);
        }
    }
    paintPaths (g, t);
}

//==============================================================================
TourResumeCard::TourResumeCard (juce::String t) : text (std::move (t))
{
    addAndMakeVisible (cont);
    addAndMakeVisible (restart);
    cont.onClick = [this] { if (onChoice) onChoice (0); };
    restart.onClick = [this] { if (onChoice) onChoice (1); };
    setSize (kWidth, kHeight);
}

void TourResumeCard::resized()
{
    auto row = getLocalBounds().reduced (14, 12).removeFromBottom (32);
    cont.setBounds (row.removeFromRight (104));
    row.removeFromRight (8);
    restart.setBounds (row.removeFromRight (94));
    closeArea = { getWidth() - 30, 6, 24, 24 };
}

void TourResumeCard::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    juce::Path shape;
    shape.addRoundedRectangle (r, 10.0f);
    juce::DropShadow (juce::Colours::black.withAlpha (0.5f), 18, { 0, 4 }).drawForPath (g, shape);
    g.setColour (theme::panel);
    g.fillPath (shape);
    g.setColour (theme::accentIn.withAlpha (0.6f));
    g.strokePath (shape, juce::PathStrokeType (1.0f));
    theme::drawLogo (g, { 14.0f, 14.0f, 18.0f, 20.0f });
    g.setColour (theme::text);
    g.setFont (theme::font (13.5f, true));
    g.drawText (text, 42, 12, getWidth() - 80, 22, juce::Justification::centredLeft, true);
    const auto c = closeArea.toFloat().reduced (7.0f);
    g.setColour (theme::textFaint);
    g.drawLine (c.getX(), c.getY(), c.getRight(), c.getBottom(), 1.5f);
    g.drawLine (c.getRight(), c.getY(), c.getX(), c.getBottom(), 1.5f);
}

void TourResumeCard::mouseUp (const juce::MouseEvent& e)
{
    if (closeArea.contains (e.getPosition()) && e.mouseWasClicked() && onChoice) onChoice (2);
}
} // namespace hydra

#include "CorrectionWizard.h"
#include "Dialog.h"
#include "core/ParamEq.h"

namespace hydra
{
namespace
{
// Level check: the expected sweep peak at the microphone should land here (dBFS), as on the Analysis page.
constexpr float kGreenLo = -18.0f, kGreenHi = -6.0f, kClipDb = -3.0f;
constexpr double kBurstS = 2.0, kPauseS = 0.7, kCheckSeconds = 600.0;
constexpr double kSweepSeconds = 5.0, kDelaySeconds = 5.0;
constexpr int kOtherDeviceId = 2000, kImportCalId = 1000;
constexpr int kStepCount = 8;   // shown in the step bar (Done has none of its own)

const char* const kStepNames[kStepCount] = { "Start", "Speakers", "Microphone", "Near field", "Placement", "Level", "Measure", "Result" };
const std::vector<double> kLevels { -30.0, -24.0, -18.0, -15.0, -12.0, -9.0, -6.0 };

// Result presets: strength = how much the correction may do, range = up to which frequency.
struct Strength { const char* name; const char* detail; double maxCut; int bands; double smoothing; };
const Strength kStrengths[] = {
    { "Gentle",   "Only the biggest peaks, up to -6 dB",     6.0,  8,  6.0 },
    { "Balanced", "Peaks and room modes, up to -12 dB",      12.0, 16, 12.0 },
    { "Strong",   "Follows the response closely, -18 dB",    18.0, 24, 24.0 },
};
struct Range { const char* name; const char* detail; double highHz; };
const Range kRanges[] = {
    { "Bass",       "20 - 500 Hz: room modes",       500.0 },
    { "Extended",   "20 Hz - 2 kHz",                 2000.0 },
    { "Full range", "20 Hz - 20 kHz",                20000.0 },
};
const int kPositionChoices[] = { 1, 5, 9 };
constexpr double kNearSweepSeconds = 2.0, kNearStartDb = -40.0, kNearMaxDb = -6.0;
constexpr double kPositionSpacingM = 0.3;   // the marks of the measuring pattern are this far from the centre
constexpr double kPositionToleranceM = 0.15; // a measured position this close to its mark is fine (grey area in the picture)

juce::String hz (double f)
{
    return f >= 1000.0 ? (f >= 10000.0 ? juce::String (juce::roundToInt (f / 1000.0)) : juce::String (f / 1000.0, 1)) + " kHz" : juce::String (juce::roundToInt (f)) + " Hz";
}

juce::String titled (const juce::String& code, const juce::String& name) { return name.isEmpty() || name == code ? code : code + "  " + name; }
juce::String inputTitle (const Engine& e, int i) { return titled ("IN " + juce::String (i + 1), e.inputs[(size_t) i].name); }

void dashedLine (juce::Graphics& g, juce::Point<float> a, juce::Point<float> b, float width, float dash = 5.0f)
{
    juce::Path p, d;
    p.startNewSubPath (a);
    p.lineTo (b);
    const float dashes[] = { dash, dash * 0.8f };
    juce::PathStrokeType (width).createDashedStroke (d, p, dashes, 2);
    g.fillPath (d);
}

/** Microphone offsets around the listening position (units of the spacing; -y = towards the speakers). The first
    5 / 9 make the patterns of the position choices: centre, the four diagonals, then left / right / front / back. */
juce::Point<float> micOffset (int k)
{
    static const juce::Point<float> p[] = { { 0.0f, 0.0f }, { -0.7f, -0.7f }, { 0.7f, -0.7f }, { -0.7f, 0.7f }, { 0.7f, 0.7f },
                                            { -1.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, -1.0f }, { 0.0f, 1.0f } };
    return p[juce::jlimit (0, 8, k)];
}

//==============================================================================
// Pictures. All drawn with paths, in the theme's colours.

/** Speaker seen from above at c, facing 'towards'; playing = sound waves in front of it. */
void drawSpeaker (juce::Graphics& g, juce::Point<float> c, float w, juce::Point<float> towards, juce::Colour colour, bool playing, float alpha = 1.0f)
{
    const float angle = std::atan2 (towards.y - c.y, towards.x - c.x) - juce::MathConstants<float>::halfPi;
    const auto t = juce::AffineTransform::rotation (angle).translated (c);
    const float d = w * 0.78f;
    juce::Path body;
    body.addRoundedRectangle (-w * 0.5f, -d * 0.5f, w, d, w * 0.14f);
    g.setColour (theme::raised.withAlpha (alpha));
    g.fillPath (body, t);
    g.setColour ((playing ? colour : theme::textDim).withAlpha (alpha));
    g.strokePath (body, juce::PathStrokeType (1.6f), t);
    // Front with the cone (local +y faces the listener).
    juce::Path cone;
    cone.addRoundedRectangle (-w * 0.32f, d * 0.5f - w * 0.1f, w * 0.64f, w * 0.14f, w * 0.06f);
    g.fillPath (cone, t);
    if (! playing) return;
    for (int k = 0; k < 3; ++k)
    {
        juce::Path arc;
        const float r = d * 0.5f + w * (0.38f + 0.32f * (float) k);
        arc.addCentredArc (0.0f, 0.0f, r, r, 0.0f, juce::MathConstants<float>::pi - 0.55f, juce::MathConstants<float>::pi + 0.55f, true);
        g.setColour (colour.withAlpha (alpha * (0.85f - 0.25f * (float) k)));
        g.strokePath (arc, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded), t);
    }
}

/** Listener seen from above (shoulders, head, nose towards -y). */
void drawListener (juce::Graphics& g, juce::Point<float> c, float s, float alpha)
{
    g.setColour (theme::textFaint.withAlpha (0.55f * alpha));
    g.fillEllipse (juce::Rectangle<float> (s * 1.9f, s * 0.75f).withCentre (c.translated (0.0f, s * 0.22f)));
    g.setColour (theme::textDim.withAlpha (alpha));
    g.fillEllipse (juce::Rectangle<float> (s * 0.18f, s * 0.3f).withCentre (c.translated (-s * 0.42f, 0.0f)));
    g.fillEllipse (juce::Rectangle<float> (s * 0.18f, s * 0.3f).withCentre (c.translated (s * 0.42f, 0.0f)));
    g.fillEllipse (juce::Rectangle<float> (s * 0.8f, s * 0.9f).withCentre (c));
    juce::Path nose;
    nose.addTriangle (c.x - s * 0.1f, c.y - s * 0.4f, c.x + s * 0.1f, c.y - s * 0.4f, c.x, c.y - s * 0.56f);
    g.fillPath (nose);
}

struct RoomPicture
{
    int playing = 0;          // bit 0 = left, bit 1 = right speaker plays
    bool triangle = true;     // the stereo triangle (dashed)
    int positions = 1;        // microphone positions shown (0 = no microphone)
    int current = -1;         // the position to measure next (highlighted)
    int done = 0;             // positions measured
    float listener = 0.35f;   // listener's alpha (faint: the microphone takes the head's place)
};

/** The room from above: front wall with both speakers, the listening position at the triangle's tip, the
    microphone positions around it. */
void drawRoom (juce::Graphics& g, juce::Rectangle<float> area, const RoomPicture& p)
{
    // Room with the proportions of a real room, as large as the area allows.
    auto room = area.withSizeKeepingCentre (juce::jmin (area.getWidth(), area.getHeight() * 1.05f), juce::jmin (area.getHeight(), area.getWidth() / 1.05f));
    g.setColour (theme::panel2);
    g.fillRoundedRectangle (room, 14.0f);
    g.setColour (theme::outline);
    g.drawRoundedRectangle (room, 14.0f, 2.0f);
    g.setColour (theme::textFaint);
    g.setFont (theme::font (11.0f));
    g.drawText ("front wall", room.removeFromTop (22.0f).toNearestInt(), juce::Justification::centred);

    const float d = room.getWidth() * 0.5f;                         // speaker spacing = distance to the listener
    const juce::Point<float> ear (room.getCentreX(), room.getY() + d * 0.866f + room.getHeight() * 0.18f);
    const juce::Point<float> spk[2] { { ear.x - d * 0.5f, ear.y - d * 0.866f }, { ear.x + d * 0.5f, ear.y - d * 0.866f } };
    const float sw = d * 0.2f;

    if (p.triangle)
    {
        g.setColour (theme::textFaint.withAlpha (0.7f));
        dashedLine (g, spk[0], spk[1], 1.2f);
        dashedLine (g, spk[0], ear, 1.2f);
        dashedLine (g, spk[1], ear, 1.2f);
    }
    for (int s = 0; s < 2; ++s)
    {
        const bool on = (p.playing >> s) & 1;
        drawSpeaker (g, spk[s], sw, ear, theme::accentIn, on, p.playing != 0 && ! on ? 0.55f : 1.0f);
        g.setColour (on ? theme::accentIn : theme::textDim);
        g.setFont (theme::font (12.0f, true));
        g.drawText (s == 0 ? "L" : "R", juce::Rectangle<float> (40.0f, 16.0f).withCentre (spk[s].translated (0.0f, -sw * 0.75f)), juce::Justification::centred);
    }
    drawListener (g, ear, d * 0.17f, p.listener);

    const float spacing = d * 0.16f;
    for (int k = 0; k < p.positions; ++k)
    {
        const auto at = ear + micOffset (k) * spacing;
        const float r = d * 0.034f + 3.0f;
        const auto dot = juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (at);
        if (k < p.done)
        {
            g.setColour (theme::good);
            g.fillEllipse (dot);
        }
        else if (k == p.current)
        {
            g.setColour (theme::accentIn.withAlpha (0.25f));
            g.fillEllipse (dot.expanded (r * 0.9f));
            g.setColour (theme::accentIn);
            g.fillEllipse (dot);
        }
        else
        {
            g.setColour (theme::panel2);
            g.fillEllipse (dot);
            g.setColour (theme::textDim);
            g.drawEllipse (dot, 1.4f);
        }
        if (p.positions > 1)
        {
            g.setColour (k < p.done || k == p.current ? theme::bg : theme::textDim);
            g.setFont (theme::font (juce::jmax (9.0f, r * 1.2f), true));
            g.drawText (juce::String (k + 1), dot, juce::Justification::centred);
        }
    }
}

/** Measurement microphone seen from the side, on a stand at ear height, capsule pointing up. */
void drawMicSide (juce::Graphics& g, juce::Rectangle<float> area)
{
    const float floorY = area.getBottom() - 10.0f;
    const float h = area.getHeight() - 30.0f;
    const float earY = floorY - h * 0.62f;
    const float mx = area.getX() + area.getWidth() * 0.62f;

    g.setColour (theme::outline);
    g.fillRect (area.getX(), floorY, area.getWidth(), 2.0f);

    // Seated listener (ghost) left of the microphone: the microphone takes the place of the head.
    const float hx = area.getX() + area.getWidth() * 0.25f, hs = h * 0.13f;
    g.setColour (theme::textFaint.withAlpha (0.35f));
    g.fillEllipse (juce::Rectangle<float> (hs, hs * 1.15f).withCentre ({ hx, earY }));
    g.fillRoundedRectangle (hx - hs * 0.6f, earY + hs * 0.65f, hs * 1.2f, h * 0.22f, hs * 0.4f);
    g.fillRoundedRectangle (hx - hs * 0.6f, earY + hs * 0.65f + h * 0.2f, hs * 2.2f, hs * 0.5f, hs * 0.2f);
    g.fillRect (hx + hs * 1.2f, earY + hs * 0.65f + h * 0.2f, hs * 0.4f, floorY - (earY + hs * 0.65f + h * 0.2f));

    // Stand: tripod and pole.
    g.setColour (theme::textDim);
    juce::Path legs;
    legs.startNewSubPath (mx - h * 0.12f, floorY);
    legs.lineTo (mx, floorY - h * 0.12f);
    legs.lineTo (mx + h * 0.12f, floorY);
    g.strokePath (legs, juce::PathStrokeType (2.0f));
    g.fillRect (mx - 1.5f, earY + h * 0.2f, 3.0f, floorY - h * 0.12f - (earY + h * 0.2f));
    // Microphone: thin tube with the capsule on top.
    const float mw = juce::jmax (6.0f, h * 0.04f);
    g.setColour (theme::text);
    g.fillRoundedRectangle (mx - mw * 0.5f, earY, mw, h * 0.22f, mw * 0.3f);
    g.setColour (theme::accentIn);
    g.fillEllipse (juce::Rectangle<float> (mw * 0.9f, mw * 0.9f).withCentre ({ mx, earY }));

    // Ear height and the direction of the capsule.
    g.setColour (theme::accentIn.withAlpha (0.8f));
    dashedLine (g, { hx + hs * 0.6f, earY }, { mx - mw, earY }, 1.3f);
    g.setFont (theme::font (11.5f));
    g.drawText ("ear height", juce::Rectangle<float> (hx, earY - 20.0f, mx - hx, 16.0f), juce::Justification::centred);
    juce::Path arrow;
    arrow.addArrow ({ mx, earY - 8.0f, mx, earY - h * 0.2f }, 2.0f, 10.0f, 9.0f);
    g.fillPath (arrow);
    g.drawText ("capsule points to the ceiling", juce::Rectangle<float> (mx + 10.0f, earY - h * 0.2f - 2.0f, area.getRight() - mx - 10.0f, 16.0f),
                juce::Justification::centredLeft);
}

/** A measurement microphone lying horizontally: capsule at x0, len long (the cable leaves 5 % further right). */
void drawMeasureMicAt (juce::Graphics& g, float x0, float y, float len)
{
    const float d = len * 0.085f;
    const juce::Point<float> c (x0 + len * 0.5f, y);
    // Capsule head (thin), tapered neck, body, connector.
    juce::Path body;
    body.startNewSubPath (x0 + d * 0.3f, c.y - d * 0.32f);
    body.lineTo (x0 + len * 0.16f, c.y - d * 0.32f);
    body.lineTo (x0 + len * 0.30f, c.y - d * 0.5f);
    body.lineTo (x0 + len * 0.92f, c.y - d * 0.5f);
    body.lineTo (x0 + len * 0.92f, c.y + d * 0.5f);
    body.lineTo (x0 + len * 0.30f, c.y + d * 0.5f);
    body.lineTo (x0 + len * 0.16f, c.y + d * 0.32f);
    body.lineTo (x0 + d * 0.3f, c.y + d * 0.32f);
    body.closeSubPath();
    g.setGradientFill (juce::ColourGradient (theme::text, 0.0f, c.y - d * 0.5f, theme::textDim, 0.0f, c.y + d * 0.5f, false));
    g.fillPath (body);
    // Connector and the cable leaving to the right.
    g.setColour (theme::textFaint);
    g.fillRoundedRectangle (x0 + len * 0.92f, c.y - d * 0.36f, len * 0.05f, d * 0.72f, 2.0f);
    juce::Path cable;
    cable.startNewSubPath (x0 + len * 0.97f, c.y);
    cable.quadraticTo (x0 + len * 1.03f, c.y, x0 + len * 1.05f, c.y + d * 1.2f);
    g.strokePath (cable, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    // Capsule: accent cap with its grille.
    const auto cap = juce::Rectangle<float> (d * 0.7f, d * 0.7f).withCentre ({ x0 + d * 0.3f, c.y });
    g.setColour (theme::accentIn);
    g.fillEllipse (cap);
    g.setColour (theme::bg.withAlpha (0.5f));
    g.drawEllipse (cap.reduced (d * 0.14f), 1.0f);
    // Rings on the body (where the grip is).
    g.setColour (theme::bg.withAlpha (0.35f));
    for (float t : { 0.55f, 0.6f, 0.65f })
        g.fillRect (x0 + len * t, c.y - d * 0.5f, 1.5f, d);
}

/** The same microphone, centred in area. */
void drawMeasureMic (juce::Graphics& g, juce::Rectangle<float> area)
{
    const float len = juce::jmin (area.getWidth() * 0.8f, area.getHeight() * 5.0f);
    drawMeasureMicAt (g, area.getCentreX() - len * 0.5f, area.getCentreY(), len);
}

/** The microphone step: the microphone with its input above, and below it the frequency response of the chosen
    calibration file (a flat line without one): that is what is taken out of every measurement. */
void drawMicStep (juce::Graphics& g, juce::Rectangle<float> area, const juce::String& input, const room::Calibration* cal)
{
    auto top = area.removeFromTop (area.getHeight() * 0.36f);
    auto caption = top.removeFromBottom (50.0f);
    drawMeasureMic (g, top.reduced (10.0f, 6.0f));
    g.setColour (input.isEmpty() ? theme::warn : theme::text);
    g.setFont (theme::font (15.0f, true));
    g.drawText (input.isEmpty() ? juce::String ("No microphone input chosen yet") : input, caption.removeFromTop (24.0f), juce::Justification::centred, true);
    g.setColour (theme::textDim);
    g.setFont (theme::font (12.5f));
    g.drawText (cal != nullptr ? "Calibration: " + juce::String (cal->name) : juce::String ("No calibration file"), caption, juce::Justification::centredTop, true);

    area.removeFromTop (10.0f);
    g.setColour (theme::panel2);
    g.fillRoundedRectangle (area, 10.0f);
    auto inner = area.reduced (14.0f, 10.0f);
    g.setColour (theme::text);
    g.setFont (theme::font (13.0f, true));
    g.drawText ("Microphone response", inner.removeFromTop (20.0f), juce::Justification::centredLeft);
    g.setColour (theme::textDim);
    g.setFont (theme::font (11.5f));
    g.drawText (cal != nullptr ? juce::String ("From the calibration file - taken out of every measurement")
                               : juce::String ("Without a calibration file the microphone counts as flat"),
                inner.removeFromTop (16.0f), juce::Justification::centredLeft, true);
    inner.removeFromTop (8.0f);

    const auto plot = inner.withTrimmedLeft (40.0f).withTrimmedBottom (18.0f).withTrimmedRight (6.0f);
    constexpr double lo = 20.0, hi = 20000.0;
    constexpr int points = 240;
    std::vector<double> db ((size_t) points + 1, 0.0);
    double range = 3.0;
    if (cal != nullptr && ! cal->empty())
        for (int i = 0; i <= points; ++i)
        {
            db[(size_t) i] = cal->at (lo * std::pow (hi / lo, (double) i / points));
            range = juce::jmax (range, std::abs (db[(size_t) i]) * 1.2);
        }
    range = juce::jmin (20.0, std::ceil (range / 3.0) * 3.0);
    auto fx = [&] (double f) { return plot.getX() + plot.getWidth() * (float) (std::log (f / lo) / std::log (hi / lo)); };
    auto fy = [&] (double v) { return plot.getCentreY() - plot.getHeight() * 0.5f * (float) (juce::jlimit (-range, range, v) / range); };

    g.setFont (theme::font (10.5f));
    for (double f : { 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
    {
        g.setColour (theme::outline.withAlpha (0.6f));
        g.drawVerticalLine (juce::roundToInt (fx (f)), plot.getY(), plot.getBottom());
        g.setColour (theme::textFaint);
        g.drawText (f >= 1000.0 ? juce::String (juce::roundToInt (f / 1000.0)) + "k" : juce::String (juce::roundToInt (f)),
                    juce::Rectangle<float> (fx (f) - 20.0f, plot.getBottom() + 3.0f, 40.0f, 13.0f), juce::Justification::centred);
    }
    const double step = range > 9.0 ? 6.0 : 3.0;
    for (double v = -range; v <= range + 0.01; v += step)
    {
        g.setColour (theme::outline.withAlpha (std::abs (v) < 0.01 ? 1.0f : 0.4f));
        g.drawHorizontalLine (juce::roundToInt (fy (v)), plot.getX(), plot.getRight());
        g.setColour (theme::textFaint);
        g.drawText ((v > 0.01 ? "+" : "") + juce::String (juce::roundToInt (v)) + " dB",
                    juce::Rectangle<float> (inner.getX(), fy (v) - 7.0f, 36.0f, 14.0f), juce::Justification::centredRight);
    }

    juce::Path line;
    for (int i = 0; i <= points; ++i)
    {
        const juce::Point<float> pt (plot.getX() + plot.getWidth() * (float) i / points, fy (db[(size_t) i]));
        if (i == 0) line.startNewSubPath (pt); else line.lineTo (pt);
    }
    juce::Path fill (line);
    fill.lineTo (plot.getRight(), fy (0.0));
    fill.lineTo (plot.getX(), fy (0.0));
    fill.closeSubPath();
    const auto c = cal != nullptr ? theme::accentIn : theme::textDim;
    g.setColour (c.withAlpha (0.12f));
    g.fillPath (fill);
    g.setColour (c);
    g.strokePath (line, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

/** Near field: a speaker from the side with the microphone right at its larger driver; below, from above, the
    other speaker playing towards the microphone (their distance). */
void drawNearField (juce::Graphics& g, juce::Rectangle<float> area, int speaker, bool playingOther)
{
    auto side = area.removeFromTop (area.getHeight() * 0.58f);
    // Speaker cabinet with tweeter and woofer.
    const float h = side.getHeight() * 0.9f, w = h * 0.55f;
    const auto box = juce::Rectangle<float> (w, h).withCentre ({ side.getX() + side.getWidth() * 0.32f, side.getCentreY() });
    const auto c = speaker == 1 ? theme::accentVirt : theme::accentIn;
    g.setColour (theme::raised);
    g.fillRoundedRectangle (box, 8.0f);
    g.setColour (theme::outline);
    g.drawRoundedRectangle (box, 8.0f, 1.2f);
    const auto tweeter = juce::Rectangle<float> (w * 0.28f, w * 0.28f).withCentre ({ box.getCentreX(), box.getY() + h * 0.2f });
    const auto woofer = juce::Rectangle<float> (w * 0.7f, w * 0.7f).withCentre ({ box.getCentreX(), box.getY() + h * 0.58f });
    for (auto d : { tweeter, woofer })
    {
        g.setColour (theme::bg);
        g.fillEllipse (d);
        g.setColour (theme::textDim);
        g.drawEllipse (d, 1.4f);
        g.drawEllipse (d.reduced (d.getWidth() * 0.32f), 1.0f);
    }
    // The microphone pointing at the woofer, 1-2 cm away (drawn a little further for clarity).
    const float my = woofer.getCentreY(), mx = box.getRight() + w * 0.12f, len = (side.getRight() - mx - 10.0f) / 1.05f, mh = len * 0.085f;
    drawMeasureMicAt (g, mx, my, len);
    g.setColour (c);
    g.setFont (theme::font (12.5f, true));
    g.drawText ("1 - 2 cm", juce::Rectangle<float> (box.getRight() - 30.0f, my + mh + 4.0f, 90.0f, 16.0f), juce::Justification::centred);
    g.setColour (theme::textDim);
    g.setFont (theme::font (12.0f));
    g.drawText (juce::String ("the larger driver, ") + (speaker == 1 ? "right" : "left") + " speaker", juce::Rectangle<float> (box.getX() - 20.0f, box.getBottom() + 4.0f, w + 160.0f, 16.0f),
                juce::Justification::centredLeft);

    // From above: microphone at one speaker, the other one plays for the distance.
    auto top = area.reduced (area.getWidth() * 0.12f, 18.0f);
    const float y = top.getY() + 14.0f, gap = top.getWidth() * 0.7f;
    const juce::Point<float> sp[2] = { { top.getCentreX() - gap * 0.5f, y }, { top.getCentreX() + gap * 0.5f, y } };
    const int at = speaker < 0 ? 0 : speaker;
    for (int k = 0; k < 2; ++k)
    {
        const auto b = juce::Rectangle<float> (26.0f, 20.0f).withCentre (sp[k]);
        g.setColour (k == at ? c.withAlpha (0.25f) : theme::raised);
        g.fillRoundedRectangle (b, 4.0f);
        g.setColour (k == 0 ? theme::accentIn : theme::accentVirt);
        g.drawRoundedRectangle (b, 4.0f, 1.4f);
        g.setFont (theme::font (11.0f, true));
        g.drawText (k == 0 ? "L" : "R", b, juce::Justification::centred);
    }
    const auto mic = sp[at].translated (0.0f, 18.0f);
    g.setColour (theme::accentIn);
    g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre (mic));
    g.setColour (playingOther ? theme::accentIn : theme::textFaint);
    juce::Path arrow;
    arrow.addArrow ({ sp[1 - at].translated (at == 0 ? -16.0f : 16.0f, 6.0f), mic.translated (at == 0 ? 10.0f : -10.0f, 0.0f) }, 1.4f, 9.0f, 8.0f);
    g.fillPath (arrow);
    g.setFont (theme::font (12.0f));
    g.setColour (theme::textDim);
    g.drawFittedText ("Afterwards the other speaker plays a moment: from its travel time Hydra knows how far apart the speakers stand.",
                      juce::Rectangle<float> (top.getX(), mic.y + 16.0f, top.getWidth(), 40.0f).toNearestInt(), juce::Justification::centredTop, 2);
}

/** Deviation from flat (rms, dB) of a response between lo and hi, around its median there; -1 if it cannot tell. */
double rmsDeviation (const room::Analysis& a, double lo, double hi, double smoothing)
{
    if (a.magDb.size() != a.freqs.size()) return -1.0;
    const auto s = room::smooth (a.magDb, smoothing);
    std::vector<double> v;
    for (size_t i = 0; i < s.size(); ++i)
        if (a.freqs[i] >= std::max (lo, a.sweepStartHz) && a.freqs[i] <= std::min (hi, a.sweepEndHz)) v.push_back (s[i]);
    if (v.size() < 4) return -1.0;
    auto sorted = v;
    std::nth_element (sorted.begin(), sorted.begin() + (long) sorted.size() / 2, sorted.end());
    const double m = sorted[sorted.size() / 2];
    double sum = 0.0;
    for (double x : v) sum += (x - m) * (x - m);
    return std::sqrt (sum / (double) v.size());
}

void drawCheck (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c, float width)
{
    juce::Path tick;
    tick.startNewSubPath (r.getX() + r.getWidth() * 0.18f, r.getY() + r.getHeight() * 0.52f);
    tick.lineTo (r.getX() + r.getWidth() * 0.42f, r.getY() + r.getHeight() * 0.76f);
    tick.lineTo (r.getX() + r.getWidth() * 0.84f, r.getY() + r.getHeight() * 0.28f);
    g.setColour (c);
    g.strokePath (tick, juce::PathStrokeType (width, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void addPara (juce::AttributedString& s, const juce::String& text, juce::Colour c = theme::textDim, bool bold = false, float size = 14.5f)
{
    s.append (text, theme::font (size, bold), c);
}
} // namespace

//==============================================================================
//==============================================================================
/** Before / after of one side: the measured response (grey), the correction (orange), the result (green), the
    flat target (dashed) and the corrected range (tinted). Read-out under the mouse. */
class CorrectionWizard::ResultChart final : public juce::Component
{
public:
    explicit ResultChart (CorrectionWizard& w) : wiz (w) {}

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        g.setColour (theme::panel);
        g.fillRoundedRectangle (b, 10.0f);
        auto* m = wiz.results[wiz.viewSide].get();
        const auto& eq = wiz.eqs[wiz.viewSide];
        if (m == nullptr || ! m->analysed)
        {
            g.setColour (theme::textDim);
            g.setFont (theme::font (15.0f));
            g.drawText (wiz.analysing ? "Analysing the measurement..." : "No measurement", getLocalBounds(), juce::Justification::centred);
            return;
        }
        const auto& a = m->analysis;
        const auto plot = b.withTrimmedLeft (50.0f).withTrimmedRight (18.0f).withTrimmedTop (44.0f).withTrimmedBottom (28.0f);
        constexpr double lo = 20.0, hi = 20000.0, top = 15.0, bottom = -25.0;
        auto fx = [&] (double f) { return plot.getX() + plot.getWidth() * (float) (std::log (f / lo) / std::log (hi / lo)); };
        auto fy = [&] (double db) { return plot.getY() + plot.getHeight() * (float) ((top - juce::jlimit (bottom, top, db)) / (top - bottom)); };

        const double rangeHi = kRanges[wiz.range].highHz;
        g.setColour (theme::accentIn.withAlpha (0.05f));
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (fx (20.0), plot.getY(), fx (juce::jmin (rangeHi, hi)), plot.getBottom()));

        g.setFont (theme::font (11.0f));
        for (double decade = 10.0; decade <= 10000.0; decade *= 10.0)
            for (int k = 1; k <= 9; ++k)
            {
                const double f = decade * k;
                if (f < lo || f > hi) continue;
                const bool major = k == 1 || k == 2 || k == 5;
                g.setColour (theme::outline.withAlpha (major ? 0.8f : 0.35f));
                g.drawVerticalLine (juce::roundToInt (fx (f)), plot.getY(), plot.getBottom());
                if (major)
                {
                    g.setColour (theme::textFaint);
                    g.drawText (f >= 1000.0 ? juce::String (juce::roundToInt (f / 1000.0)) + "k" : juce::String (juce::roundToInt (f)),
                                juce::Rectangle<float> (fx (f) - 24.0f, plot.getBottom() + 5.0f, 48.0f, 14.0f), juce::Justification::centred);
                }
            }
        for (double db = bottom; db <= top + 0.1; db += 5.0)
        {
            g.setColour (theme::outline.withAlpha (db == 0.0 ? 1.0f : 0.45f));
            g.drawHorizontalLine (juce::roundToInt (fy (db)), plot.getX(), plot.getRight());
            g.setColour (theme::textFaint);
            g.drawText ((db > 0 ? "+" : "") + juce::String (juce::roundToInt (db)), juce::Rectangle<float> (0.0f, fy (db) - 7.0f, plot.getX() - 8.0f, 14.0f),
                        juce::Justification::centredRight);
        }

        // Without a design (yet): the smoothed response alone.
        std::vector<double> before = eq.smoothedDb;
        if (before.size() != a.freqs.size())
        {
            before = room::smooth (a.magDb, 12.0);
            for (auto& x : before) x -= a.referenceDb;
        }
        auto curve = [&] (const std::vector<double>& db)
        {
            juce::Path p;
            bool started = false;
            for (size_t i = 0; i < a.freqs.size() && i < db.size(); ++i)
            {
                if (a.freqs[i] < a.sweepStartHz || a.freqs[i] > a.sweepEndHz) continue;
                const juce::Point<float> pt (fx (a.freqs[i]), fy (db[i]));
                if (! started) { p.startNewSubPath (pt); started = true; }
                else           p.lineTo (pt);
            }
            return p;
        };
        g.saveState();
        g.reduceClipRegion (plot.expanded (2.0f).toNearestInt());
        auto stroke = [&] (const juce::Path& p, juce::Colour c, float w)
        {
            g.setColour (c);
            g.strokePath (p, juce::PathStrokeType (w, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        };
        const bool designed = ! eq.bands.empty() && eq.correctedDb.size() == a.freqs.size();
        if (designed)
        {
            g.setColour (theme::text.withAlpha (0.55f));
            dashedLine (g, { fx (20.0), fy (eq.targetDb) }, { fx (juce::jmin (rangeHi, hi)), fy (eq.targetDb) }, 1.2f);
            // The correction as a filled area around 0 dB.
            auto corr = curve (eq.eqDb);
            juce::Path area (corr);
            area.lineTo (fx (a.sweepEndHz), fy (0.0));
            area.lineTo (fx (a.sweepStartHz), fy (0.0));
            area.closeSubPath();
            g.setColour (theme::warn.withAlpha (0.12f));
            g.fillPath (area);
            stroke (corr, theme::warn.withAlpha (0.9f), 1.5f);
        }
        const bool validating = wiz.mode == Mode::validate;
        if (validating && wiz.original != nullptr)
        {
            // Before the calibration (the measurement the correction came from), relative to its own level.
            const auto* o = wiz.original->side (wiz.viewSide);
            if (o->analysed && o->analysis.magDb.size() == a.freqs.size())
            {
                auto old = room::smooth (o->analysis.magDb, 12.0);
                for (auto& x : old) x -= o->analysis.referenceDb;
                stroke (curve (old), theme::textDim, 1.6f);
            }
        }
        stroke (curve (before), validating ? theme::accentVirt : theme::textDim, designed ? 1.6f : 2.2f);
        if (designed && eq.usedNear && eq.fitDb.size() == a.freqs.size())
        {
            // What the correction follows above ~400 Hz: the speaker's own response (near field), dashed.
            juce::Path p, dashed;
            bool started = false;
            for (size_t i = 0; i < a.freqs.size(); ++i)
            {
                if (a.freqs[i] < 350.0 || a.freqs[i] > a.sweepEndHz) continue;
                const juce::Point<float> pt (fx (a.freqs[i]), fy (eq.fitDb[i]));
                if (! started) { p.startNewSubPath (pt); started = true; }
                else           p.lineTo (pt);
            }
            const float dashes[] = { 4.0f, 3.0f };
            juce::PathStrokeType (1.4f).createDashedStroke (dashed, p, dashes, 2);
            g.setColour (theme::accentVirt.withAlpha (0.9f));
            g.fillPath (dashed);
        }
        if (designed) stroke (curve (eq.correctedDb), theme::good, 2.4f);

        if (plot.contains (hover))
        {
            const double f = lo * std::pow (hi / lo, (double) ((hover.x - plot.getX()) / plot.getWidth()));
            g.setColour (theme::text.withAlpha (0.3f));
            g.drawVerticalLine (juce::roundToInt (hover.x), plot.getY(), plot.getBottom());
            juce::String t = hz (f) + "   before " + juce::String (room::valueAt (a.freqs, before, f), 1) + " dB";
            if (designed) t << "   after " << juce::String (room::valueAt (a.freqs, eq.correctedDb, f), 1) << " dB";
            g.setFont (theme::font (12.0f));
            const float w = (float) juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), t) + 16.0f;
            auto box = juce::Rectangle<float> (hover.x + 10.0f, plot.getY() + 6.0f, w, 20.0f);
            if (box.getRight() > plot.getRight()) box.setX (hover.x - 10.0f - w);
            g.setColour (theme::raised.withAlpha (0.95f));
            g.fillRoundedRectangle (box, 4.0f);
            g.setColour (theme::text);
            g.drawText (t, box, juce::Justification::centred);
        }
        g.restoreState();

        std::vector<std::pair<juce::String, juce::Colour>> legend { { validating ? "Before calibration" : "Before", theme::textDim } };
        if (validating) legend.push_back ({ "Now, with the correction", theme::accentVirt });
        if (designed)
        {
            legend.push_back ({ validating ? "After refining" : "After", theme::good });
            legend.push_back ({ validating ? "Refinement" : "Correction", theme::warn });
            legend.push_back ({ "Target", theme::text.withAlpha (0.55f) });
            if (eq.usedNear) legend.push_back ({ "Speaker (near field)", theme::accentVirt });
        }
        drawLegend (g, getLocalBounds().withTrimmedRight (16).removeFromTop (34), legend);
        g.setColour (theme::text);
        g.setFont (theme::font (14.0f, true));
        g.drawText (wiz.separate ? (wiz.viewSide == 0 ? "Left speaker" : "Right speaker") : "Both speakers",
                    getLocalBounds().reduced (16, 0).removeFromTop (34), juce::Justification::centredLeft);
    }

    void mouseMove (const juce::MouseEvent& e) override { hover = e.position; repaint(); }
    void mouseExit (const juce::MouseEvent&) override   { hover = { -1.0f, -1.0f }; repaint(); }

private:
    CorrectionWizard& wiz;
    juce::Point<float> hover { -1.0f, -1.0f };
};

//==============================================================================
CorrectionWizard::CorrectionWizard (UiContext& c) : ctx (c)
{
    chart = std::make_unique<ResultChart> (*this);
    addChildComponent (*chart);

    // Speakers: separately or together.
    separateCard = std::make_unique<ChoiceCard> ("Left and right separately",
        "Each speaker gets its own correction: they stand at different spots, so the room treats them differently. "
        "Takes twice the sweeps.",
        [] (juce::Graphics& g, juce::Rectangle<float> r, bool sel)
        {
            const auto c = r.getCentre().translated (0.0f, r.getHeight() * 0.2f);
            drawSpeaker (g, { r.getX() + r.getWidth() * 0.25f, r.getCentreY() - 6.0f }, 20.0f, c, sel ? theme::accentIn : theme::textDim, true);
            drawSpeaker (g, { r.getRight() - r.getWidth() * 0.25f, r.getCentreY() - 6.0f }, 20.0f, c, sel ? theme::accentVirt : theme::textDim, true);
        }, "Recommended");
    togetherCard = std::make_unique<ChoiceCard> ("Both together",
        "One correction for the sum of both speakers. Faster; also for a single speaker or a mono setup.",
        [] (juce::Graphics& g, juce::Rectangle<float> r, bool sel)
        {
            const auto c = r.getCentre().translated (0.0f, r.getHeight() * 0.2f);
            for (float x : { 0.25f, 0.75f })
                drawSpeaker (g, { r.getX() + r.getWidth() * x, r.getCentreY() - 6.0f }, 20.0f, c, sel ? theme::accentIn : theme::textDim, true);
        });
    separateCard->onClick = [this] { separate = true; updateButtons(); repaint(); };
    togetherCard->onClick = [this] { separate = false; updateButtons(); repaint(); };

    // Microphone positions.
    const char* posNames[] = { "Quick", "Standard", "Thorough" };
    const char* posText[] = { "1 position, about a minute. Fits exactly one spot.",
                              "5 positions around your head. Fits where you sit.",
                              "9 positions. The widest area, the most reliable bass." };
    for (int i = 0; i < 3; ++i)
    {
        const int n = kPositionChoices[i];
        posCards[i] = std::make_unique<ChoiceCard> (posNames[i], posText[i],
            [n] (juce::Graphics& g, juce::Rectangle<float> r, bool sel)
            {
                const auto c = r.getCentre();
                const float s = juce::jmin (r.getWidth(), r.getHeight()) * 0.3f;
                for (int k = 0; k < n; ++k)
                {
                    g.setColour (sel ? theme::accentIn : theme::textDim);
                    g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre (c + micOffset (k) * s));
                }
            }, i == 1 ? "Recommended" : "");
        posCards[i]->iconTop = true;
        posCards[i]->onClick = [this, n] { positions = n; updateButtons(); repaint(); };
    }

    // Result presets.
    for (int i = 0; i < 3; ++i)
    {
        strengthCards[i] = std::make_unique<ChoiceCard> (kStrengths[i].name, kStrengths[i].detail);
        strengthCards[i]->onClick = [this, i] { strength = i; designEq(); };
        rangeCards[i] = std::make_unique<ChoiceCard> (kRanges[i].name, kRanges[i].detail);
        rangeCards[i]->onClick = [this, i] { range = i; rangeChosen = true; designEq(); };
    }
    for (int s = 0; s < 2; ++s)
    {
        auto& chip = sideChips[s];
        chip.showArrow = false;
        chip.centred = true;
        chip.colour = theme::text;
        chip.onClick = [this, s] { viewSide = s; updateButtons(); chart->repaint(); repaint(); };
        addChildComponent (chip);
    }

    for (auto* card : { separateCard.get(), togetherCard.get(), posCards[0].get(), posCards[1].get(), posCards[2].get(),
                        strengthCards[0].get(), strengthCards[1].get(), strengthCards[2].get(),
                        rangeCards[0].get(), rangeCards[1].get(), rangeCards[2].get() })
        addChildComponent (*card);

    // Microphone
    for (auto* b : { &micBox, &micChannelBox, &calBox, &levelBox }) addChildComponent (*b);
    micChannelBox.addItem ("Left", 1);
    micChannelBox.addItem ("Right", 2);
    micChannelBox.setSelectedId (1, juce::dontSendNotification);
    micChannelBox.setTooltip ("Which channel of the input carries the microphone (a mono microphone on input 2 of an interface: Right).");
    micBox.onChange = [this] { micChosen(); };
    calBox.onChange = [this]
    {
        if (calBox.getSelectedId() == kImportCalId) importCalibration();
        else shownCal = calBox.getSelectedId();
        repaint();
    };
    refreshCalibrations ({});
    for (size_t i = 0; i < kLevels.size(); ++i) levelBox.addItem (juce::String (juce::roundToInt (kLevels[i])) + " dBFS", (int) i + 1);
    levelBox.setSelectedId (4, juce::dontSendNotification);
    levelBox.setTooltip ("Level of the test signal. Prefer the speakers' volume knob; use this if the knob doesn't go far enough.");
    levelBox.onChange = [this] { if (checking) startLevelCheck(); };

    // Buttons
    for (auto* b : { &backButton, &nextButton }) addAndMakeVisible (*b);
    for (auto* b : { &deviceButton, &checkButton, &measureButton, &redoButton, &analysisButton, &fineTuneButton, &showCorrButton, &restartButton, &abButton })
        addChildComponent (*b);
    addChildComponent (delayToggle);
    addChildComponent (boostToggle);
    for (auto* b : { &nextButton, &measureButton })
    {
        b->setColour (juce::TextButton::buttonColourId, theme::accentIn);
        b->setColour (juce::TextButton::textColourOffId, theme::bg);
    }
    backButton.onClick = [this] { back(); };
    nextButton.onClick = [this] { next(); };
    deviceButton.onClick = [this] { if (ctx.openDevicePicker && output >= 0) ctx.openDevicePicker (output); };
    checkButton.onClick = [this] { if (checking) stopRun(); else startLevelCheck(); };
    measureButton.onClick = [this]
    {
        if (isRunning() || counting) { counting = false; stopRun(); updateButtons(); repaint(); }
        else startPosition (false);
    };
    redoButton.onClick = [this]
    {
        if (redoIndex >= 0) { redoIndex = -1; status = {}; updateButtons(); repaint(); }
        else startPosition (true);
    };
    redoButton.setTooltip ("Measures the last position again (e.g. after a noise or with the microphone moved by accident). "
                           "Any other position: click it in the picture.");
    analysisButton.onClick = [this] { if (onOpenAnalysis) onOpenAnalysis(); };
    analysisButton.setTooltip ("The measurement on the Analysis page: response, waterfall, decay times and resonances.");
    fineTuneButton.onClick = [this]
    {
        if (step == Step::result)   // Edit works on what is on the output: this result goes there first
        {
            if (eqPending > 0) return;
            apply();
        }
        if (onFineTune) onFineTune();
    };
    fineTuneButton.setTooltip ("Puts this correction on the output and opens the Edit page with every setting of the design.");
    showCorrButton.onClick = [this] { if (onShowCorrection) onShowCorrection(); };
    restartButton.onClick = [this] { restart(); };
    addChildComponent (keepButton);
    keepButton.setTooltip ("Leaves the correction unchanged.");
    keepButton.onClick = [this] { refined = false; showStep (Step::done); };
    boostToggle.onClick = [this] { designEq(); };
    boostToggle.setTooltip ("Dips are often cancellations that no boost can fill; small boosts are safe, large ones only cost headroom.");
    abButton.onClick = [this]
    {
        if (auto* corr = ctx.engine.correctionFor (output)) corr->on->set (corr->on->isOn() ? 0.0f : 1.0f);
        updateButtons();
    };

    for (int k = 0; k < 2; ++k)
    {
        addChildComponent (nearButtons[k]);
        nearButtons[k].onClick = [this, k] { if (nearSpeaker == k) stopRun(); else startNear (k); };
    }

    refreshMicBoxes();
    showStep (Step::welcome);
    startTimerHz (30);
}

CorrectionWizard::~CorrectionWizard()
{
    stopTimer();
    picker.reset();
    stopRun();
    ctx.engine.setMicLevelTap (-1, 0);
    ctx.engine.releaseMeasureInput();   // a device opened only for measuring closes with the window
}

//==============================================================================
void CorrectionWizard::setOutput (int selectIndex)
{
    if (! Engine::isHwOutput (selectIndex) || selectIndex == output) return;
    output = selectIndex;
    // Validating belongs to the old output's correction: back to calibrating.
    if (mode == Mode::validate && ! isRunning() && ! analysing)
    {
        restart();
        return;
    }
    // Measurements belong to one output: another output starts over from the speakers.
    if (! isRunning())
    {
        for (auto& c : captures) c.clear();
        for (auto& p : geometry.posMs) p.clear();
    for (auto& c : captureSync) c.clear();
    redoIndex = -1;
        ++posGeneration;
        clearNear();
        pass = 0;
        results[0].reset();
        results[1].reset();
        eqs[0] = eqs[1] = {};
        measuredKey = {};
        if (step > Step::speakers) showStep (Step::speakers);
    }
    resized();
    repaint();
}

bool CorrectionWizard::holdsOutput() const
{
    return step >= Step::level || isRunning();
}

bool CorrectionWizard::isRunning() const
{
    return ownRun != nullptr && ctx.engine.getMeasurement() == ownRun;
}

void CorrectionWizard::restart()
{
    stopRun();
    counting = false;
    if (mode == Mode::validate) clearNear();
    mode = Mode::calibrate;
    original.reset();
    for (auto& c : captures) c.clear();
    for (auto& p : geometry.posMs) p.clear();
    for (auto& c : captureSync) c.clear();
    redoIndex = -1;
    ++posGeneration;
    rangeChosen = false;
    pass = 0;
    results[0].reset();
    results[1].reset();
    eqs[0] = eqs[1] = {};
    measuredKey = {};
    showStep (Step::welcome);
}

//==============================================================================
void CorrectionWizard::showStep (Step s)
{
    if (step == Step::level && s != Step::level) stopRun();
    step = s;
    status = {};
    statusColour = theme::textDim;

    if (s == Step::microphone) refreshMicBoxes();
    if (s == Step::level)
    {
        levelSeen = levelGood = false;
        greenTicks = 0;
        startLevelCheck();
    }
    if (s == Step::nearField && nearKey() != nearMeasuredKey && ! nearMeasuredKey.isEmpty()) clearNear();
    if (s == Step::measure && configKey() != measuredKey)
    {
        for (auto& c : captures) c.clear();
        for (auto& p : geometry.posMs) p.clear();
        for (auto& c : captureSync) c.clear();
        redoIndex = -1;
        ++posGeneration;
        pass = 0;
        results[0].reset();
        results[1].reset();
        measuredKey = configKey();
    }
    if (s == Step::result)
    {
        viewSide = 0;
        // With the speakers' near field the correction can safely go higher: full range unless chosen otherwise.
        if (! rangeChosen && mode == Mode::calibrate) range = results[0] != nullptr && ! results[0]->analysis.nearDb.empty() ? 2 : 0;
        if (eqs[0].bands.empty() && eqs[1].bands.empty()) designEq();
    }

    separateCard->setVisible (s == Step::speakers);
    togetherCard->setVisible (s == Step::speakers);
    deviceButton.setVisible (s == Step::speakers);
    for (auto* b : { &micBox, &micChannelBox, &calBox }) b->setVisible (s == Step::microphone);
    for (auto& c : posCards) c->setVisible (s == Step::placement);
    for (auto& b : nearButtons) b.setVisible (s == Step::nearField);
    levelBox.setVisible (s == Step::level);
    checkButton.setVisible (s == Step::level);
    measureButton.setVisible (s == Step::measure);
    redoButton.setVisible (s == Step::measure);
    delayToggle.setVisible (s == Step::measure);
    chart->setVisible (s == Step::result);
    const bool calibrating = mode == Mode::calibrate;
    for (auto& c : strengthCards) c->setVisible (s == Step::result && calibrating);
    for (auto& c : rangeCards) c->setVisible (s == Step::result && calibrating);
    boostToggle.setVisible (s == Step::result && calibrating);
    analysisButton.setVisible ((s == Step::result || s == Step::done) && calibrating);
    fineTuneButton.setVisible ((s == Step::result || s == Step::done) && calibrating);
    keepButton.setVisible (s == Step::result && ! calibrating);
    for (int k = 0; k < 2; ++k) sideChips[k].setVisible (s == Step::result && separate);
    abButton.setVisible (s == Step::done);
    showCorrButton.setVisible (s == Step::done);
    restartButton.setVisible (s == Step::done);
    nextButton.setVisible (s != Step::done);
    backButton.setVisible (s != stepList().front() && s != Step::done);

    updateButtons();
    resized();
    repaint();
}

bool CorrectionWizard::canContinue() const
{
    auto& e = ctx.engine;
    switch (step)
    {
        case Step::welcome:    return true;
        case Step::speakers:   return output >= 0 && e.buses[(size_t) (output - kMaxInputs)].output.kind != IoTarget::Kind::None;
        case Step::microphone: return micInput() >= 0;
        case Step::nearField:  return nearSpeaker < 0;
        case Step::placement:  return true;
        case Step::level:      return levelSeen && ! counting;
        case Step::measure:    return positionsDone() >= positions && ! isRunning() && ! counting && ! analysing;
        case Step::result:     return eqPending == 0 && (mode == Mode::validate || ! eqs[0].bands.empty() || ! eqs[1].bands.empty());
        case Step::done:       return false;
    }
    return false;
}

void CorrectionWizard::next()
{
    if (! canContinue()) return;
    switch (step)
    {
        case Step::welcome:    showStep (Step::speakers); break;
        case Step::speakers:   showStep (Step::microphone); break;
        case Step::microphone: showStep (mode == Mode::validate ? Step::placement : Step::nearField); break;
        case Step::nearField:  showStep (Step::placement); break;
        case Step::placement:  showStep (Step::level); break;
        case Step::level:      showStep (Step::measure); break;
        case Step::measure:    if (results[0] != nullptr) showStep (Step::result); else analyseAll(); break;
        case Step::result:     if (mode == Mode::validate) refine(); else apply(); break;
        case Step::done:       break;
    }
}

void CorrectionWizard::back()
{
    const auto steps = stepList();
    const auto at = std::find (steps.begin(), steps.end(), step);
    if (isRunning() || counting || at == steps.begin() || at == steps.end()) return;
    showStep (*(at - 1));
}

std::vector<CorrectionWizard::Step> CorrectionWizard::stepList() const
{
    if (mode == Mode::validate) return { Step::microphone, Step::placement, Step::level, Step::measure, Step::result };
    return { Step::welcome, Step::speakers, Step::microphone, Step::nearField, Step::placement, Step::level, Step::measure, Step::result };
}

void CorrectionWizard::startValidation (const RoomMeasurement& m)
{
    if (isRunning() || analysing) return;
    counting = false;
    mode = Mode::validate;
    original = std::make_shared<RoomMeasurement> (m);
    if (m.right != nullptr) original->right = std::make_shared<RoomMeasurement> (*m.right);
    separate = m.sides() == 2;
    positions = m.numPositions() >= 9 ? 9 : m.numPositions() >= 5 ? 5 : 1;
    // Where the speakers stand is known from the calibration: the picture shows the positions again.
    clearNear();
    geometry = m.geometry;
    for (auto& c : captures) c.clear();
    for (auto& p : geometry.posMs) p.clear();
    for (auto& c : captureSync) c.clear();
    redoIndex = -1;
    ++posGeneration;
    pass = 0;
    results[0].reset();
    results[1].reset();
    eqs[0] = eqs[1] = {};
    measuredKey = {};
    refined = false;
    showStep (Step::microphone);
}

void CorrectionWizard::startCalibration()
{
    if (mode == Mode::calibrate || isRunning() || analysing) return;
    restart();
}

void CorrectionWizard::mouseUp (const juce::MouseEvent& e)
{
    // Measure: a click on a measured position (or its mark) measures it again.
    if (step == Step::measure && artArea.contains (e.getPosition()) && ! isRunning() && ! counting && ! analysing)
    {
        const auto lay = layout();
        if (separate && lay.valid)
        {
            const int k = hitLayoutSpot (artArea.toFloat().reduced (24.0f), measurePicture (lay), e.position);
            if (k >= 0 && k < positionsDone())
            {
                redoIndex = redoIndex == k ? -1 : k;
                status = redoIndex >= 0 ? "Put the microphone on mark " + juce::String (k + 1) + " and click \"Measure position "
                                              + juce::String (k + 1) + " again\"."
                                        : juce::String();
                statusColour = theme::text;
                updateButtons();
                repaint();
            }
        }
        return;
    }
    // The step bar: finished steps can be visited again.
    if (! stepsArea.contains (e.getPosition()) || isRunning() || counting || analysing || step == Step::done) return;
    const auto steps = stepList();
    const int count = (int) steps.size();
    const int current = (int) (std::find (steps.begin(), steps.end(), step) - steps.begin());
    const int i = (e.x - stepsArea.getX()) * count / juce::jmax (1, stepsArea.getWidth());
    if (juce::isPositiveAndBelow (i, juce::jmin (current, count)))
        showStep (steps[(size_t) i]);
}

void CorrectionWizard::updateButtons()
{
    separateCard->selected = separate;
    togetherCard->selected = ! separate;
    for (int i = 0; i < 3; ++i)
    {
        posCards[i]->selected = positions == kPositionChoices[i];
        strengthCards[i]->selected = strength == i;
        rangeCards[i]->selected = range == i;
    }
    for (int k = 0; k < 2; ++k) sideChips[k].active = viewSide == k;

    juce::String nextText = "Next";
    switch (step)
    {
        case Step::welcome: nextText = "Let's start"; break;
        case Step::nearField: nextText = nearMeasured() ? juce::String ("Next") : juce::String ("Skip"); break;
        case Step::level:   nextText = levelGood || ! levelSeen ? juce::String ("Next") : juce::String ("Continue anyway"); break;
        case Step::measure: nextText = "Show result"; break;
        case Step::result:  nextText = mode == Mode::calibrate ? juce::String ("Apply correction")
                                     : ! eqs[0].bands.empty() || ! eqs[1].bands.empty() ? juce::String ("Refine correction")
                                                                                        : juce::String ("Finish"); break;
        default: break;
    }
    nextButton.setButtonText (nextText);
    nextButton.setEnabled (canContinue());
    fineTuneButton.setEnabled (step != Step::result || eqPending == 0);
    backButton.setEnabled (! isRunning() && ! counting && ! analysing);

    const bool running = isRunning() || counting;
    const int done = positionsDone();
    measureButton.setButtonText (running ? juce::String ("Stop")
                                 : done >= positions ? juce::String ("All positions measured")
                                 : positions == 1 ? juce::String ("Start measurement")
                                 : "Measure position " + juce::String (done + 1) + " of " + juce::String (positions));
    measureButton.setEnabled (running || (done < positions && ! analysing));
    redoButton.setEnabled (! running && done > 0 && ! analysing);
    redoButton.setButtonText (redoIndex >= 0 ? juce::String ("Cancel") : juce::String ("Repeat last position"));
    if (redoIndex >= 0 && ! running)
        measureButton.setButtonText ("Measure position " + juce::String (redoIndex + 1) + " again");
    measureButton.setEnabled (running || ((done < positions || redoIndex >= 0) && ! analysing));
    delayToggle.setEnabled (! running);
    checkButton.setButtonText (checking ? "Stop test noise" : "Play test noise");
    for (int k = 0; k < 2; ++k)
    {
        const juce::String name = k == 0 ? "left" : "right";
        nearButtons[k].setButtonText (nearSpeaker == k ? juce::String ("Stop")
                                      : nearDone[k] ? "Measure " + name + " speaker again"
                                                    : "Measure " + name + " speaker");
        nearButtons[k].setEnabled (nearSpeaker < 0 || nearSpeaker == k);
        const bool primary = ! nearDone[k] && (k == 0 || nearDone[0]) && nearSpeaker < 0;
        nearButtons[k].setColour (juce::TextButton::buttonColourId, primary ? theme::accentIn : theme::raised);
        nearButtons[k].setColour (juce::TextButton::textColourOffId, primary ? theme::bg : theme::text);
    }

    if (auto* corr = ctx.engine.correctionFor (output))
    {
        const bool on = corr->on->isOn();
        abButton.setButtonText (on ? "Correction ON  -  click to hear without" : "Correction OFF  -  click to hear with");
        abButton.setColour (juce::TextButton::buttonColourId, on ? theme::good : theme::raised);
        abButton.setColour (juce::TextButton::textColourOffId, on ? theme::bg : theme::text);
    }
}

//==============================================================================
void CorrectionWizard::refreshMicBoxes()
{
    auto& e = ctx.engine;
    const int mic = micBox.getSelectedId();
    micBox.clear (juce::dontSendNotification);
    int firstDevice = 0, first = 0;
    for (int i = 0; i < kMaxInputs; ++i)
    {
        if (! e.isInputActive (i)) continue;
        const auto& src = e.inputs[(size_t) i].source;
        micBox.addItem (inputTitle (e, i) + (src.kind == IoTarget::Kind::None ? juce::String ("   (no device)") : "   -   " + src.describe()), i + 1);
        if (first == 0) first = i + 1;
        if (firstDevice == 0 && src.kind == IoTarget::Kind::Device) firstDevice = i + 1;
    }
    if (const int slot = e.getMeasureInput(); slot >= 0)
    {
        const auto& src = e.inputs[(size_t) slot].source;
        micBox.addItem (src.kind == IoTarget::Kind::None ? juce::String ("Device: choose...") : "Device: " + src.describe(), slot + 1);
    }
    micBox.addSeparator();
    micBox.addItem ("Other device (not on a channel)...", kOtherDeviceId);
    micBox.setSelectedId (micBox.indexOfItemId (mic) >= 0 && mic != kOtherDeviceId ? mic : (firstDevice > 0 ? firstDevice : first), juce::dontSendNotification);
}

void CorrectionWizard::micChosen()
{
    auto& e = ctx.engine;
    if (micBox.getSelectedId() == kOtherDeviceId) return chooseMicDevice();
    if (e.getMeasureInput() >= 0 && micBox.getSelectedId() != e.getMeasureInput() + 1)
    {
        e.releaseMeasureInput();
        refreshMicBoxes();
    }
    updateButtons();
    repaint();
}

void CorrectionWizard::chooseMicDevice()
{
    auto& e = ctx.engine;
    const int slot = e.borrowMeasureInput();
    if (slot < 0)
    {
        refreshMicBoxes();
        return dialog::message (this, dialog::Kind::warning, "No free slot",
                                "All 16 inputs are in use, so no further device can be opened. Remove a channel first.");
    }
    refreshMicBoxes();
    micBox.setSelectedId (slot + 1, juce::dontSendNotification);
    pickerBackdrop = std::make_unique<Backdrop>();
    pickerBackdrop->onClick = [this] { closePicker(); };
    addAndMakeVisible (*pickerBackdrop);
    picker = std::make_unique<DevicePicker> (ctx, slot);
    picker->onChanged = [this, slot]
    {
        refreshMicBoxes();
        micBox.setSelectedId (slot + 1, juce::dontSendNotification);
        updateButtons();
        repaint();
    };
    juce::Component::SafePointer<CorrectionWizard> safe (this);
    picker->onClose = [safe] { juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->closePicker(); }); };
    addAndMakeVisible (*picker);
    resized();
    picker->grabKeyboardFocus();
}

void CorrectionWizard::closePicker()
{
    picker.reset();
    pickerBackdrop.reset();
    refreshMicBoxes();
    updateButtons();
    repaint();
}

int CorrectionWizard::micInput() const
{
    const int i = micBox.getSelectedId() - 1;
    if (! juce::isPositiveAndBelow (i, kMaxInputs)) return -1;
    return ctx.engine.inputs[(size_t) i].source.kind != IoTarget::Kind::None ? i : -1;
}

juce::String CorrectionWizard::micDescription() const
{
    const int i = micInput();
    if (i < 0) return {};
    auto& e = ctx.engine;
    return (i == e.getMeasureInput() ? e.inputs[(size_t) i].source.describe() : inputTitle (e, i))
           + (micChannelBox.getSelectedId() == 2 ? " (R)" : "");
}

juce::String CorrectionWizard::speakerDescription() const
{
    if (output < 0) return {};
    const int b = output - kMaxInputs;
    return titled (busLabel (b), ctx.engine.buses[(size_t) b].name);
}

void CorrectionWizard::refreshCalibrations (const juce::String& select)
{
    const auto previous = calBox.getText();
    calList = calibrations::all();
    calBox.clear (juce::dontSendNotification);
    calBox.addItem ("None (flat microphone)", 1);
    for (size_t k = 0; k < calList.size(); ++k) calBox.addItem (juce::String (calList[k].name), (int) k + 2);
    calBox.addSeparator();
    calBox.addItem ("Import calibration file...", kImportCalId);
    int id = 1;
    for (size_t k = 0; k < calList.size(); ++k)
        if (juce::String (calList[k].name) == (select.isNotEmpty() ? select : previous)) id = (int) k + 2;
    calBox.setSelectedId (id, juce::dontSendNotification);
    shownCal = id;
}

void CorrectionWizard::importCalibration()
{
    calBox.setSelectedId (shownCal, juce::dontSendNotification);
    chooser = std::make_unique<juce::FileChooser> ("Import microphone calibration", juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
                                                   "*.txt;*.cal;*.frd;*.csv");
    juce::Component::SafePointer<CorrectionWizard> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc)
    {
        const auto f = fc.getResult();
        if (safe == nullptr || ! f.existsAsFile()) return;
        juce::String err;
        const auto cal = calibrations::import (f, err);
        if (cal.empty())
            return dialog::message (safe.getComponent(), dialog::Kind::warning, "Cannot import", f.getFileName() + ": " + err);
        safe->refreshCalibrations (juce::String (cal.name));
        safe->repaint();
    });
}

//==============================================================================
float CorrectionWizard::levelDb() const
{
    const int i = levelBox.getSelectedId() - 1;
    return (float) (juce::isPositiveAndBelow (i, (int) kLevels.size()) ? kLevels[(size_t) i] : -15.0);
}

std::unique_ptr<MeasureRun> CorrectionWizard::makeRun (std::vector<float> stimulus, int outChannels)
{
    auto& e = ctx.engine;
    const int input = micInput();
    if (e.getMasterDevice() == nullptr)
    {
        dialog::message (this, dialog::Kind::warning, "Audio engine not running", "Choose a master device in Settings > Audio Engine first.");
        return {};
    }
    if (input < 0 || output < 0 || e.buses[(size_t) (output - kMaxInputs)].output.kind == IoTarget::Kind::None)
    {
        dialog::message (this, dialog::Kind::warning, "Setup incomplete", "The speakers' output or the microphone has no device. Go back and choose one.");
        return {};
    }
    if (auto* run = e.getMeasurement(); run != nullptr && run != ownRun)
    {
        dialog::message (this, dialog::Kind::warning, "A measurement is running", "Another measurement is running right now. Wait for it to finish.");
        return {};
    }
    auto run = std::make_unique<MeasureRun>();
    run->input = input;
    run->inputChannel = micChannelBox.getSelectedId() == 2 ? 1 : 0;
    run->bus = output - kMaxInputs;
    run->outChannels = outChannels;
    run->muteOthers = true;
    run->busEq = false;
    run->busCorrection = mode == Mode::validate;
    if (mode == Mode::validate)
        if (auto* corr = e.correctionFor (output)) corr->on->set (1.0f);
    run->stimulus = std::move (stimulus);
    run->capture.assign (run->stimulus.size(), 0.0f);
    return run;
}

void CorrectionWizard::stopRun()
{
    if (isRunning()) ctx.engine.stopMeasurement();
}

void CorrectionWizard::startLevelCheck()
{
    auto& e = ctx.engine;
    if (isRunning())   // a new level: the old loop ends first (the timer starts the new one)
    {
        e.stopMeasurement();
        e.finishMeasurement();
        ownRun = nullptr;
        checking = false;
    }
    const double sr = e.getSampleRate();
    if (sr <= 0.0) return;
    checkBurst = (int) (kBurstS * sr);
    checkPattern = checkBurst + (int) (kPauseS * sr);
    const auto burst = room::makePinkNoise (checkBurst, levelDb() - 3.0);   // a sine's RMS is 3 dB below its peak
    std::vector<float> stimulus;
    const int total = (int) (kCheckSeconds * sr) / checkPattern * checkPattern;
    stimulus.reserve ((size_t) total);
    const int fade = (int) (0.02 * sr);
    while ((int) stimulus.size() + checkPattern <= total)
    {
        const auto start = stimulus.size();
        stimulus.insert (stimulus.end(), burst.begin(), burst.end());
        for (int i = 0; i < fade; ++i)
        {
            const float w = (float) i / (float) fade;
            stimulus[start + (size_t) i] *= w;
            stimulus[start + (size_t) (checkBurst - 1 - i)] *= w;
        }
        stimulus.resize (stimulus.size() + (size_t) (checkPattern - checkBurst), 0.0f);
    }
    // Separately: the left speaker alone (one speaker is what each sweep will see).
    auto run = makeRun (std::move (stimulus), separate ? 1 : 3);
    if (run == nullptr) return;
    checking = true;
    signalRms = noiseRms = clipPeak = micLevel = 0.0f;
    greenTicks = 0;
    stopTicks = 0;
    ownRun = run.get();
    e.startMeasurement (std::move (run));
    updateButtons();
}

void CorrectionWizard::updateLevelCheck (float peak, const MeasureRun& run)
{
    // RMS of the last quarter second, only well inside a burst or a pause (latency and decay smear the edges).
    const int sr = (int) ctx.engine.getSampleRate(), win = sr / 4, margin = sr / 7;
    const int pos = juce::jmin (run.pos.load (std::memory_order_acquire), (int) run.capture.size());
    clipPeak = juce::jmax (clipPeak, peak);
    if (pos < win || checkPattern <= 0) return;
    double sum = 0.0;
    for (const float* x = run.capture.data() + pos - win, * end = run.capture.data() + pos; x < end; ++x) sum += (double) *x * *x;
    const float rms = (float) std::sqrt (sum / win);
    const int at = pos % checkPattern;
    auto follow = [] (float& v, float x) { v = v > 0.0f ? 0.6f * v + 0.4f * x : x; };
    if (at > win + margin && at < checkBurst)                       follow (signalRms, rms);
    else if (at > checkBurst + win + margin || at < margin - win)   follow (noiseRms, rms);

    if (signalRms > 0.0f)
    {
        levelSeen = true;
        const float db = gainToDb (signalRms * 2.0f);   // on the sweep's peak scale (+6 dB)
        const bool green = db >= kGreenLo && db <= kGreenHi && gainToDb (clipPeak) < -1.0f;
        greenTicks = green ? greenTicks + 1 : 0;
        const bool good = greenTicks > 30;   // ~1 s in the green range
        if (good != levelGood) { levelGood = good; updateButtons(); }
        if (! nextButton.isEnabled()) updateButtons();
    }
}

//==============================================================================
double CorrectionWizard::syncBufferMs() const
{
    auto& e = ctx.engine;
    const int in = micInput();
    if (in < 0 || output < 0) return 0.0;
    return e.getIoLatency (e.inputs[(size_t) in].source, true).clockSyncMs
           + e.getIoLatency (e.buses[(size_t) (output - kMaxInputs)].output, false).clockSyncMs;
}

juce::String CorrectionWizard::nearKey() const
{
    return juce::String (output) + "|" + juce::String (micInput()) + "|" + juce::String (micChannelBox.getSelectedId());
}

void CorrectionWizard::clearNear()
{
    for (int k = 0; k < 2; ++k)
    {
        nearDone[k] = false;
        nearIr[k] = {};
        geometry.nearMs[k] = geometry.crossMs[k] = -1.0;
    }
    nearMeasuredKey = {};
    nearLevelDb = kNearStartDb;
}

void CorrectionWizard::startNear (int speaker)
{
    if (isRunning() || analysing) return;
    if (nearKey() != nearMeasuredKey && ! nearMeasuredKey.isEmpty()) clearNear();
    nearSpeaker = speaker;
    nearPhase = 0;
    nearTries = 0;
    status = {};
    startNearRun();
}

void CorrectionWizard::startNearRun()
{
    // Phase 0: quiet sweep from this speaker, without the deep bass (the microphone is right at the driver).
    // Phase 1: the other speaker at the normal level: how far away it is.
    nearSpec = {};
    nearSpec.sampleRate = ctx.engine.getSampleRate();
    nearSpec.startHz = RoomMeasurement::kNearSweepStartHz;
    nearSpec.endHz = juce::jmin (20000.0, nearSpec.sampleRate * 0.45);
    nearSpec.seconds = kNearSweepSeconds;
    nearSpec.repeats = 1;
    nearSpec.levelDb = nearPhase == 0 ? nearLevelDb : (double) levelDb();
    const int other = 1 - nearSpeaker;
    auto run = makeRun (room::makeStimulus (nearSpec), 1 << (nearPhase == 0 ? nearSpeaker : other));
    if (run == nullptr) { nearSpeaker = -1; updateButtons(); return; }
    checking = false;
    stopTicks = 0;
    micLevel = 0.0f;
    syncSum = 0.0;
    syncCount = 0;
    ownRun = run.get();
    ctx.engine.startMeasurement (std::move (run));
    updateButtons();
    repaint();
}

void CorrectionWizard::nearFinished (std::vector<float> capture, bool stopped, bool noInput)
{
    const int k = nearSpeaker;
    const juce::String name = k == 0 ? "left" : "right", other = k == 0 ? "right" : "left";
    auto fail = [this] (const juce::String& text, juce::Colour c)
    {
        nearSpeaker = -1;
        status = text;
        statusColour = c;
        updateButtons();
    };
    if (stopped || capture.empty()) return fail ("Stopped.", theme::textDim);
    if (noInput) return fail ("The microphone delivered no audio (device offline?).", theme::mute);

    const auto ir = room::deconvolve (capture, nearSpec);
    if (nearPhase == 0)
    {
        // Automatic level: the microphone sits right at the speaker, so the right level is found by trying.
        const bool tooLoud = ir.clipped || ir.capturePeakDb > -1.0, tooQuiet = ir.capturePeakDb < -32.0;
        if ((tooLoud || (tooQuiet && nearLevelDb < kNearMaxDb)) && nearTries < 3)
        {
            ++nearTries;
            nearLevelDb = tooLoud ? nearLevelDb - 12.0 : juce::jmin (kNearMaxDb, nearLevelDb + juce::jmin (24.0, -14.0 - ir.capturePeakDb));
            status = tooLoud ? "Too loud at the microphone - trying again quieter..." : "Quite quiet - trying again a little louder...";
            statusColour = theme::textDim;
            return startNearRun();
        }
        if (tooLoud) return fail ("The microphone still clips. Turn the speaker volume down a little and measure again.", theme::warn);
        if (tooQuiet) return fail ("Hardly any sound at the microphone. Is the " + name + " speaker on, and the microphone right in front of it?", theme::warn);
        nearIr[k] = ir;
        geometry.nearMs[k] = ir.latencyMs - lastSyncMs;
        nearPhase = 1;
        status = "Now the " + other + " speaker plays for a moment (for the distance between them)...";
        statusColour = theme::textDim;
        return startNearRun();
    }

    // The other speaker, heard from here: only its arrival time counts.
    const bool heard = ! ir.clipped && ir.irSnrDb > 15.0 && ir.capturePeakDb > -60.0;
    geometry.crossMs[k] = heard ? ir.latencyMs - lastSyncMs : -1.0;
    nearDone[k] = true;
    nearMeasuredKey = nearKey();
    nearSpeaker = -1;
    const auto lay = layout();
    const auto tick = juce::String::fromUTF8 ("\xe2\x9c\x93  ");
    if (! heard)
        status = "The " + name + " speaker is measured. The " + other + " speaker was not heard clearly from there, so the distance "
                 "between them is missing (is it on?).";
    else if (! nearDone[1 - k])
        status = tick + "The " + name + " speaker is measured. Now hold the microphone in front of the " + other + " speaker.";
    else
        status = tick + "Both speakers measured" + (lay.valid ? ", " + juce::String (lay.spacingM, 2) + " m apart." : juce::String ("."));
    statusColour = heard ? theme::good : theme::warn;
    updateButtons();
}

juce::String CorrectionWizard::configKey() const
{
    return juce::String (output) + "|" + juce::String ((int) separate) + "|" + juce::String (positions) + "|" + juce::String (micInput())
           + "|" + juce::String (micChannelBox.getSelectedId()) + "|" + calBox.getText() + "|" + juce::String (levelDb());
}

void CorrectionWizard::startPosition (bool repeat)
{
    if (isRunning() || analysing) return;
    // An unfinished position (stopped between the two speakers) starts over.
    for (int s = 0; s < sides(); ++s)
        while ((int) captures[s].size() > positionsDone()) captures[s].pop_back();
    for (int s = 0; s < 2; ++s)
    {
        captureSync[s].resize (captures[s].size());
        if (geometry.posMs[s].size() > captures[s].size()) geometry.posMs[s].resize (captures[s].size());
    }
    if (repeat) redoIndex = positionsDone() - 1;   // "Repeat": the last position
    if (redoIndex >= positionsDone()) redoIndex = -1;
    if (redoIndex < 0 && positionsDone() >= positions) return;
    pass = 0;
    passWarning = {};
    results[0].reset();
    results[1].reset();
    eqs[0] = eqs[1] = {};

    if (positionsDone() == 0 && redoIndex < 0)   // the settings of the whole series
    {
        spec = {};
        spec.sampleRate = ctx.engine.getSampleRate();
        spec.endHz = juce::jmin (20000.0, spec.sampleRate * 0.45);
        spec.seconds = kSweepSeconds;
        spec.repeats = positions == 1 ? 2 : 1;   // several positions average the noise anyway
        spec.levelDb = levelDb();
        const int k = calBox.getSelectedId() - 2;
        runCal = juce::isPositiveAndBelow (k, (int) calList.size()) ? calList[(size_t) k] : room::Calibration();
    }
    if (delayToggle.getToggleState())
    {
        counting = true;
        countdownEnd = juce::Time::getMillisecondCounterHiRes() + 1000.0 * kDelaySeconds;
        status = {};
        updateButtons();
        repaint();
        return;
    }
    startPass();
}

void CorrectionWizard::startPass()
{
    auto run = makeRun (room::makeStimulus (spec), separate ? (pass == 0 ? 1 : 2) : 3);
    if (run == nullptr) { updateButtons(); return; }
    checking = false;
    stopTicks = 0;
    micLevel = 0.0f;
    ownRun = run.get();
    ctx.engine.startMeasurement (std::move (run));
    status = {};
    updateButtons();
    repaint();
}

void CorrectionWizard::passFinished (std::vector<float> capture, bool stopped, bool noInput)
{
    if (stopped || capture.empty() || noInput)
    {
        // The position is not complete: its first speaker's sweep (if any) is dropped.
        for (int s = 0; s < sides(); ++s)
            while ((int) captures[s].size() > positionsDone()) captures[s].pop_back();
        for (int s = 0; s < 2; ++s) captureSync[s].resize (captures[s].size());
        pass = 0;
        status = noInput ? "The microphone delivered no audio (device offline?). Check it and measure this position again."
                         : "Stopped. Measure this position again when you're ready.";
        statusColour = noInput ? theme::mute : theme::textDim;
        updateButtons();
        repaint();
        return;
    }

    // Quick check of the recording: clipping or a far too weak signal spoil the result.
    float peak = 0.0f;
    for (float x : capture) peak = juce::jmax (peak, std::abs (x));
    const float db = gainToDb (peak);
    const juce::String who = separate ? (pass == 0 ? "left" : "right") : "the";
    if (db > -0.5f)
        passWarning = "The microphone clipped during the " + who + " speaker's sweep. Lower the speaker volume a little and repeat this position.";
    else if (db < -45.0f)
        passWarning = "Very weak signal from the " + who + " speaker (" + juce::String (juce::roundToInt (db)) + " dBFS). Is it connected and turned up? Repeat this position.";

    const int side = separate ? pass : 0;
    if (redoIndex >= 0)   // measured again: replaces the position once both speakers are done
    {
        redoCapture[side] = std::move (capture);
        redoSync[side] = lastSyncMs;
    }
    else
    {
        // Where the microphone is: evaluated right away (left / right separately only).
        if (separate) evaluatePosition (side, (int) captures[side].size(), capture, lastSyncMs);
        captures[side].push_back (std::move (capture));
        captureSync[side].push_back (lastSyncMs);
    }
    ++pass;
    if (pass < sides())   // the other speaker follows right away, the microphone stays where it is
        return startPass();

    pass = 0;
    const int done = positionsDone();
    const int redone = redoIndex;
    if (redoIndex >= 0)
    {
        for (int s = 0; s < sides(); ++s)
        {
            if (separate) evaluatePosition (s, redoIndex, redoCapture[s], redoSync[s]);
            captures[s][(size_t) redoIndex] = std::move (redoCapture[s]);
            captureSync[s][(size_t) redoIndex] = redoSync[s];
        }
        redoIndex = -1;
    }
    if (passWarning.isNotEmpty())
    {
        status = passWarning;
        statusColour = theme::warn;
    }
    else if (redone >= 0)
    {
        status = "Position " + juce::String (redone + 1) + " measured again.";
        statusColour = theme::good;
    }
    else if (done < positions)
    {
        status = "Position " + juce::String (done) + " done. Move the microphone to position " + juce::String (done + 1)
                 + " (marked in the picture): about 30 cm from the centre, same height, still pointing up.";
        statusColour = theme::good;
    }
    if (done >= positions)
    {
        if (passWarning.isEmpty()) analyseAll();
        else status << " Or continue with \"Show result\".";
    }
    updateButtons();
    repaint();
}

void CorrectionWizard::evaluatePosition (int side, int index, std::vector<float> capture, double syncMs)
{
    const int gen = posGeneration;
    juce::Component::SafePointer<CorrectionWizard> safe (this);
    juce::Thread::launch ([safe, capture = std::move (capture), sp = spec, side, index, gen, syncMs]
    {
        const double ms = room::deconvolve (capture, sp).latencyMs - syncMs;
        juce::MessageManager::callAsync ([safe, side, index, gen, ms]
        {
            if (safe == nullptr || gen != safe->posGeneration) return;
            auto& p = safe->geometry.posMs[side];
            if ((int) p.size() <= index) p.resize ((size_t) index + 1, -1.0);
            p[(size_t) index] = ms;
            safe->checkPosition (index);
            safe->repaint();
        });
    });
}

void CorrectionWizard::checkPosition (int index)
{
    // Both speakers of this position evaluated: is the microphone where its mark is?
    const auto lay = layout();
    if (! lay.valid || index <= 0 || index >= (int) lay.mics.size() || positions <= 1) return;
    const auto& s = lay.mics[(size_t) index];
    if (s.distL <= 0.0 || s.distR <= 0.0) return;
    const auto pic = measurePicture (lay);
    juce::String text;
    if (! s.ok)
        text = "Position " + juce::String (index + 1) + " doesn't fit the speakers' distance (a reflection may have been louder than the "
               "speaker). Click it in the picture to measure it again.";
    else if (juce::isPositiveAndBelow (index, (int) pic.planned.size()))
    {
        const auto& mark = pic.planned[(size_t) index];
        const double off = std::hypot (s.x - mark.x, s.y - mark.y);
        if (off > kPositionToleranceM)
            text = "Position " + juce::String (index + 1) + " is " + juce::String (juce::roundToInt (100.0 * off)) + " cm from its mark, outside the "
                   "grey area. Fine if you meant it; otherwise click it in the picture and measure it again.";
    }
    if (text.isEmpty()) return;
    status = text;
    statusColour = theme::warn;
    repaint();
}

LayoutPicture CorrectionWizard::measurePicture (const room::Layout& lay) const
{
    LayoutPicture pic;
    pic.layout = &lay;
    pic.toleranceM = kPositionToleranceM;
    juce::Point<double> centre (0.0, lay.spacingM * std::sqrt (3.0) * 0.5);
    for (auto& spot : lay.mics)
        if (spot.ok) { centre = { spot.x, spot.y }; break; }
    if (positions > 1)
        for (int k = 0; k < positions; ++k)
            pic.planned.push_back ({ centre.x + micOffset (k).x * kPositionSpacingM, centre.y + micOffset (k).y * kPositionSpacingM });
    const int next = redoIndex >= 0 ? redoIndex : positionsDone() < positions ? positionsDone() : -1;
    pic.nextPlanned = positions > 1 ? next : -1;
    pic.highlight = redoIndex;
    if (pic.highlight < 0)
        for (int k = (int) lay.mics.size() - 1; k >= 0; --k)
            if (lay.mics[(size_t) k].ok) { pic.highlight = k; break; }
    return pic;
}

void CorrectionWizard::analyseAll()
{
    analysing = true;
    status = "Analysing...";
    statusColour = theme::text;
    juce::Component::SafePointer<CorrectionWizard> safe (this);
    std::array<std::vector<std::vector<float>>, 2> caps { captures[0], captures[1] };
    std::array<std::vector<double>, 2> syncs { captureSync[0], captureSync[1] };
    const int n = sides();
    const juce::String mic = micDescription(), out = speakerDescription();
    // Near field: each speaker's own one (together: both in the one measurement).
    std::array<std::vector<room::ImpulseResponse>, 2> nears;
    const bool nearValid = nearKey() == nearMeasuredKey;
    if (nearValid)
        for (int k = 0; k < 2; ++k)
            if (nearDone[k]) nears[(size_t) (separate ? k : 0)].push_back (nearIr[k]);
    auto geo = geometry;
    if (! nearValid) geo = {};
    const bool validating = mode == Mode::validate;
    auto orig = original;
    juce::Thread::launch ([safe, caps = std::move (caps), syncs, n, s = spec, cal = runCal, mic, out, sep = separate, nears, geo, validating, orig]
    {
        auto made = std::make_shared<std::vector<std::shared_ptr<RoomMeasurement>>>();
        for (int side = 0; side < n; ++side)
        {
            auto m = std::make_shared<RoomMeasurement>();
            m->calibration = cal;
            m->speakers = sep ? side + 1 : 3;
            m->throughEq = validating;
            m->nearFields = nears[(size_t) side];
            if (side == 0) m->geometry = geo;
            for (size_t k = 0; k < caps[side].size(); ++k)
                (k == 0 ? m->ir : m->positions.emplace_back()) = room::deconvolve (caps[side][k], s);
            // Arrival time at every position (clock-sync buffering taken out): where the microphone was.
            if (sep)
            {
                auto& times = made->empty() ? m->geometry.posMs[0] : made->front()->geometry.posMs[1];
                times.clear();
                for (size_t k = 0; k < caps[side].size(); ++k)
                    times.push_back ((k == 0 ? m->ir : m->positions[k - 1]).latencyMs - (k < syncs[side].size() ? syncs[side][k] : 0.0));
            }
            m->spec = s;
            m->analysis = measurements::analyse (*m, measurements::analysisOptions (*m));
            m->analysed = true;
            m->spec = s;
            m->microphone = mic;
            m->output = out + (sep ? (side == 0 ? " (L)" : " (R)") : juce::String());
            m->created = juce::Time::getCurrentTime();
            m->name = m->output + "  ->  " + mic + (caps[side].size() > 1 ? "  (" + juce::String ((int) caps[side].size()) + " positions)" : juce::String());
            made->push_back (m);
        }
        // Validating: the original needs its response (the library may not have analysed it yet).
        if (orig != nullptr)
            for (int k = 0; k < orig->sides(); ++k)
                if (! orig->side (k)->analysed)
                {
                    auto opts = measurements::analysisOptions (*orig->side (k));
                    opts.responseOnly = true;
                    orig->side (k)->analysis = measurements::analyse (*orig->side (k), opts);
                    orig->side (k)->analysed = true;
                }
        juce::MessageManager::callAsync ([safe, made, out, mic, validating]
        {
            if (safe == nullptr) return;
            juce::String warn;
            for (auto& m : *made)
                if (m->ir.irSnrDb < 35.0)
                    warn = "The measurement is a bit noisy (" + juce::String (juce::roundToInt (m->ir.irSnrDb))
                           + " dB signal-to-noise). The result is usable; for a cleaner one keep the room quieter or raise the level.";
            // One file for both speakers: the left one carries the right one.
            auto own = std::make_unique<RoomMeasurement> (*made->front());
            if (made->size() == 2)
            {
                own->right = std::make_shared<RoomMeasurement> (*(*made)[1]);
                own->output = out;
                own->right->output = out;
                own->name = out + "  ->  " + mic + (own->numPositions() > 1 ? "  (" + juce::String (own->numPositions()) + " positions)" : juce::String());
                own->right->name = own->name;
            }
            if (! validating && ! measurements::saveToLibrary (*own))
                logLine ("Room measurement: cannot save " + own->file.getFullPathName());
            for (size_t side = 0; side < made->size(); ++side)
            {
                safe->results[side] = std::make_unique<RoomMeasurement> (*(*made)[side]);
                safe->results[side]->file = own->file;
            }
            if (safe->onMeasured && ! validating) safe->onMeasured (*own);
            safe->analysing = false;
            safe->status = warn;
            safe->statusColour = theme::warn;
            safe->showStep (Step::result);
            safe->status = warn;
            safe->statusColour = theme::warn;
        });
    });
    updateButtons();
    repaint();
}

//==============================================================================
void CorrectionWizard::designEq()
{
    const int generation = ++eqGeneration;
    eqPending = 0;
    for (int side = 0; side < 2; ++side)
    {
        eqs[side] = {};
        auto* m = results[side].get();
        if (m == nullptr || ! m->analysed) continue;
        room::AutoEqSettings s;
        const auto& st = kStrengths[strength];
        s.lowHz = 20.0;
        s.highHz = kRanges[range].highHz;
        s.maxBands = st.bands;
        s.maxCutDb = st.maxCut;
        s.smoothing = st.smoothing;
        s.maxBoostDb = boostToggle.getToggleState() ? 3.0 : 0.0;
        s.matchLoudness = true;
        s.nearDb = m->analysis.nearDb;
        if (mode == Mode::validate)
        {
            // What is left after the correction, in the bass (above, the room only gives position-dependent ripples),
            // gently: at most 8 bands of at most -6 dB, within the profile's band limit.
            auto* corr = ctx.engine.correctionFor (output);
            if (corr == nullptr) continue;
            const auto& prof = corr->getProfile();
            const auto& d = prof.design;
            s.lowHz = d.valid ? d.lowHz : 20.0;
            s.highHz = juce::jmin (d.valid ? d.highHz : 500.0, 500.0);
            s.smoothing = d.valid ? d.detail : 12.0;
            s.maxCutDb = juce::jmin (d.valid ? d.maxCutDb : 12.0, 6.0);
            s.maxBoostDb = 0.0;
            s.nearDb.clear();
            s.maxBands = juce::jlimit (0, 8, CorrectionProfile::kMaxBands - (int) prof.side (separate ? side : 0).bands.size());
            devNow[side] = rmsDeviation (m->analysis, s.lowHz, s.highHz, s.smoothing);
            auto* o = original != nullptr ? original->side (side) : nullptr;
            devBefore[side] = o != nullptr && o->analysed ? rmsDeviation (o->analysis, s.lowHz, s.highHz, s.smoothing) : -1.0;
            if (s.maxBands == 0) continue;
        }
        s.sampleRate = ctx.engine.getSampleRate() > 0.0 ? ctx.engine.getSampleRate() : m->ir.sampleRate;
        ++eqPending;
        juce::Component::SafePointer<CorrectionWizard> safe (this);
        juce::Thread::launch ([safe, generation, side, s, a = m->analysis]
        {
            auto result = std::make_shared<room::AutoEqResult> (room::designEq (a, s));
            juce::MessageManager::callAsync ([safe, generation, side, result]
            {
                if (safe == nullptr || generation != safe->eqGeneration) return;   // settings changed meanwhile
                safe->eqs[side] = std::move (*result);
                --safe->eqPending;
                safe->updateButtons();
                safe->chart->repaint();
                safe->repaint();
            });
        });
    }
    updateButtons();
    chart->repaint();
    repaint();
}

void CorrectionWizard::apply()
{
    auto* corr = ctx.engine.correctionFor (output);
    if (corr == nullptr || results[0] == nullptr) return;
    auto side = [this] (int k)
    {
        CorrectionProfile::Side s;
        s.preampDb = (float) eqs[k].preampDb;
        for (auto& b : eqs[k].bands) s.bands.push_back ({ ParamEq::Bell, (float) b.freq, (float) b.gainDb, (float) b.q });
        return s;
    };
    CorrectionProfile p;
    p.name = "Calibration " + results[0]->created.formatted ("%Y-%m-%d %H:%M") + " (" + kStrengths[strength].name + ", " + kRanges[range].name + ")";
    p.source = results[0]->file.getFileName();   // the Correction page marks the measurement as loaded
    if (separate)
    {
        p.setSide (0, side (0));
        p.setSide (1, side (1));
    }
    else
        p.setSide (-1, side (0));
    // How it was made (the Edit page continues from here) and the time alignment of the two speakers.
    auto& d = p.design;
    const auto& st = kStrengths[strength];
    d.valid = true;
    d.lowHz = 20.0;
    d.highHz = kRanges[range].highHz;
    d.maxCutDb = st.maxCut;
    d.bands = st.bands;
    d.detail = st.smoothing;
    d.maxBoostDb = boostToggle.getToggleState() ? 3.0 : 0.0;
    d.keepLoudness = true;
    d.useNear = true;
    d.timeAlign = separate;
    if (separate)
    {
        // Same as the saved measurement: the first position, clock-sync buffering taken out.
        RoomMeasurement both (*results[0]);
        both.right = std::make_shared<RoomMeasurement> (*results[1]);
        measurements::alignSpeakers (p, both);
    }
    ctx.engine.setCorrection (output, p);
    corr->on->set (1.0f);
    showStep (Step::done);
}

void CorrectionWizard::refine()
{
    auto* corr = ctx.engine.correctionFor (output);
    if (corr == nullptr) return;
    auto p = corr->getProfile();
    bool any = false;
    for (int k = 0; k < sides(); ++k)
    {
        if (eqs[k].bands.empty()) continue;
        any = true;
        auto s = p.side (separate ? k : 0);
        for (auto& b : eqs[k].bands)
            if ((int) s.bands.size() < CorrectionProfile::kMaxBands)
                s.bands.push_back ({ ParamEq::Bell, (float) b.freq, (float) b.gainDb, (float) b.q });
        s.preampDb = juce::jlimit (-30.0f, 12.0f, s.preampDb + (float) eqs[k].preampDb);
        p.setSide (separate ? k : -1, s);
    }
    if (any)
    {
        p.design.refined = true;
        ctx.engine.setCorrection (output, p);
        corr->on->set (1.0f);
    }
    refined = any;
    showStep (Step::done);
}

//==============================================================================
void CorrectionWizard::timerCallback()
{
    auto& e = ctx.engine;
    if (++layoutTick >= 30)
    {
        layoutTick = 0;
        // Names / devices of the inputs (the microphone box), the speaker's device and the A/B button.
        juce::String sig (e.getLayoutVersion());
        for (int i = 0; i < kMaxInputs; ++i) sig << "|" << e.inputs[(size_t) i].name << e.inputs[(size_t) i].source.describe();
        if (output >= 0) sig << "|" << e.buses[(size_t) (output - kMaxInputs)].output.describe();
        if (sig != shownChannels && ! micBox.isPopupActive() && picker == nullptr)
        {
            shownChannels = sig;
            refreshMicBoxes();
            resized();
            repaint();
        }
        updateButtons();
    }

    // Microphone step: the live level of the chosen input (only measured while it is shown).
    {
        const int tap = step == Step::microphone && isShowing() ? micInput() : -1;
        e.setMicLevelTap (tap, micChannelBox.getSelectedId() == 2 ? 1 : 0);
        const float pk = e.takeMicLevel();
        const float level = tap < 0 ? 0.0f : pk > micPreview ? pk : micPreview * 0.85f < 1.0e-5f ? 0.0f : micPreview * 0.85f;
        if (level != micPreview)
        {
            micPreview = level;
            repaint (micMeterArea);
        }
    }

    if (counting)
    {
        if (juce::Time::getMillisecondCounterHiRes() >= countdownEnd)
        {
            counting = false;
            startPass();
        }
        repaint (textArea);
        repaint (artArea);
    }

    auto* run = e.getMeasurement();
    if (run == nullptr || run != ownRun) return;
    if (checking && ! isShowing()) stopRun();   // tab switched or window closed: the test noise stops
    const float pk = run->inputPeak.exchange (0.0f);
    micLevel = pk > micLevel ? pk : micLevel * 0.85f;
    if (checking) updateLevelCheck (pk, *run);
    else { syncSum += syncBufferMs(); ++syncCount; }
    if (run->stop.load() && ! run->finished() && ++stopTicks > 15)   // no audio callbacks (engine stopped)
        run->pos.store (run->length);
    repaint (textArea);

    if (! run->finished()) return;
    const bool stopped = run->stop.load(), noInput = run->noInput.load();
    auto capture = e.finishMeasurement();
    ownRun = nullptr;
    lastSyncMs = syncCount > 0 ? syncSum / syncCount : syncBufferMs();
    syncSum = 0.0;
    syncCount = 0;
    if (nearSpeaker >= 0)
    {
        nearFinished (std::move (capture), stopped, noInput);
        repaint();
        return;
    }
    if (checking)
    {
        checking = false;
        if (noInput) { status = "The microphone delivered no audio (device offline?)."; statusColour = theme::mute; }
        updateButtons();
        repaint();
        return;
    }
    passFinished (std::move (capture), stopped, noInput);
    repaint (artArea);
}

//==============================================================================
juce::AttributedString CorrectionWizard::bodyText() const
{
    juce::AttributedString s;
    s.setLineSpacing (4.0f);
    const auto out = speakerDescription();
    switch (step)
    {
        case Step::welcome:
            addPara (s, "This assistant measures how your speakers sound at your listening position and builds a correction that "
                        "removes what the room adds: booming bass notes, dips and colourations. It takes about 5 to 10 minutes.\n\n");
            addPara (s, "You need\n", theme::text, true);
            addPara (s, juce::String::fromUTF8 ("\xe2\x80\xa2  A measurement microphone, e.g. miniDSP UMIK-1, Behringer ECM8000 or Dayton EMM-6. Headset and "
                        "webcam microphones are not accurate enough.\n"
                        "\xe2\x80\xa2  A microphone stand (or a tripod) so the microphone stays still.\n"
                        "\xe2\x80\xa2  A quiet room: no music, fans or talking during the test sounds.\n"
                        "\xe2\x80\xa2  Your speakers on output ") + out + ".");
            break;
        case Step::speakers:
            addPara (s, "The test sounds play on " + out + ". Set your speakers (or the volume knob of your interface) to the level you "
                        "usually listen at, and leave it there from now on.\n\nHow should the speakers be corrected?");
            break;
        case Step::microphone:
            addPara (s, "Choose the input your measurement microphone is plugged into. If it isn't on a channel yet, pick "
                        "\"Other device\": it is opened only for the measurement.\n\n");
            addPara (s, "Calibration file  ", theme::text, true);
            addPara (s, juce::String::fromUTF8 ("USB measurement microphones come with a personal calibration file (download it with the serial number). "
                        "Import it here; choose the 90\xc2\xb0 file if there are two, because the microphone will point at the ceiling."));
            break;
        case Step::nearField:
            addPara (s, "Optional, about a minute. Hold the microphone 1 - 2 cm in front of the larger driver of the left speaker, "
                        "pointing at it, and click Measure. A quiet sweep plays (its level adjusts itself), then the other speaker "
                        "plays for a moment. Do the same at the right speaker.\n\n");
            addPara (s, "Why?  ", theme::text, true);
            addPara (s, "Up close the speaker drowns out the room: Hydra hears the speaker's own response and can correct it up to "
                        "the highest frequencies, without chasing reflections that only exist at one spot of the room. The travel "
                        "times also show where the speakers stand and, later, where the microphone really is.");
            break;
        case Step::placement:
            addPara (s, juce::String::fromUTF8 ("\xe2\x80\xa2  Put the microphone where your head is when you listen.\n"
                        "\xe2\x80\xa2  At ear height, pointing straight up at the ceiling.\n"
                        "\xe2\x80\xa2  On a stand, not in your hand. Nothing between the microphone and the speakers.\n\n"));
            addPara (s, "Measuring several positions around your head gives a correction that works in the whole listening area "
                        "instead of one exact point.");
            break;
        case Step::level:
            addPara (s, juce::String ("Pink noise plays from ") + (separate ? "the left speaker" : "both speakers") + " in short bursts. "
                        "Turn the speaker volume or the microphone gain until the marker stays in the green area, then continue. "
                        "Don't change the volume after this step.");
            break;
        case Step::measure:
            if (mode == Mode::validate)
                addPara (s, "The sweeps now play through the correction: this measures what you really hear at the listening position. ");
            addPara (s, positions == 1 ? "The microphone stays at the listening position. "
                                       : "Each position is measured once, then you move the microphone to the next mark. ");
            addPara (s, juce::String ("Every position plays a sweep (a rising tone) ") + (separate ? "from the left, then from the right speaker" : "from both speakers")
                        + ". Keep the room quiet and stay still while it plays.");
            if (separate && layout().valid && positions > 1)
                addPara (s, " The grey areas show where each position is fine; a position outside its area turns yellow.");
            break;
        case Step::result:
            if (mode == Mode::validate)
            {
                addPara (s, "Grey is how the speakers sounded before the calibration, blue how they sound now with the correction. "
                            "If something is left in the bass, \"Refine\" adds up to 8 small bands for it (green: after refining). "
                            "Above 500 Hz nothing is added: there the room only makes ripples that change with every head movement.");
                break;
            }
            addPara (s, "Grey is how your speakers sound in the room now, green with the correction. The correction (orange) mainly cuts "
                        "the peaks the room adds; dips are left alone, boosting them would only cost headroom.");
            if (results[0] != nullptr && ! results[0]->analysis.nearDb.empty())
                addPara (s, " Above 400 Hz it follows the speaker's own response from the near field (dashed blue): the room's ripples "
                            "up there are left alone.");
            break;
        case Step::done:
            if (mode == Mode::validate)
            {
                addPara (s, refined ? out + " now plays through the refined correction. Switch it off and on to hear the difference.\n\n"
                                            "Changing the correction on the Edit page designs it again from the calibration and drops "
                                            "the refinement; validate again afterwards."
                                    : out + " keeps its correction as it is.");
                break;
            }
            addPara (s, out + " now plays through the correction. Play music you know well and switch the correction off and on to "
                        "hear the difference.\n\nThe level, the bands and export are on the Correction page. Edit fine-tunes the "
                        "correction from this measurement, Analysis shows the room in detail (waterfall, decay, resonances).");
            break;
    }
    return s;
}

void CorrectionWizard::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);
    auto& e = ctx.engine;

    // Step bar: numbered circles joined by a line; done = green tick, current = accent.
    {
        const auto steps = stepList();
        const int count = (int) steps.size();
        const int current = step == Step::done ? count : (int) (std::find (steps.begin(), steps.end(), step) - steps.begin());
        const float w = (float) stepsArea.getWidth() / count;
        const float cy = (float) stepsArea.getY() + 22.0f;
        for (int i = 0; i < count; ++i)
        {
            const float cx = (float) stepsArea.getX() + w * ((float) i + 0.5f);
            if (i + 1 < count)
            {
                g.setColour (i < current ? theme::good.withAlpha (0.6f) : theme::outline);
                g.fillRect (cx + 15.0f, cy - 1.0f, w - 30.0f, 2.0f);
            }
            const auto dot = juce::Rectangle<float> (26.0f, 26.0f).withCentre ({ cx, cy });
            if (i < current)
            {
                g.setColour (theme::good.withAlpha (0.2f));
                g.fillEllipse (dot);
                drawCheck (g, dot.reduced (6.0f), theme::good, 2.2f);
            }
            else
            {
                g.setColour (i == current ? theme::accentIn : theme::panel2);
                g.fillEllipse (dot);
                if (i != current) { g.setColour (theme::outline); g.drawEllipse (dot, 1.2f); }
                g.setColour (i == current ? theme::bg : theme::textFaint);
                g.setFont (theme::font (12.0f, true));
                g.drawText (juce::String (i + 1), dot, juce::Justification::centred);
            }
            const auto s = steps[(size_t) i];
            const juce::String name = s == Step::result && mode == Mode::validate ? juce::String ("Check") : juce::String (kStepNames[(int) s]);
            g.setColour (i == current ? theme::text : i < current ? theme::textDim : theme::textFaint);
            g.setFont (theme::font (12.0f, i == current));
            g.drawText (name, juce::Rectangle<float> (cx - w * 0.5f, cy + 16.0f, w, 18.0f), juce::Justification::centred);
        }
        g.setColour (theme::outline.withAlpha (0.6f));
        g.drawHorizontalLine (stepsArea.getBottom() - 1, 0.0f, (float) getWidth());
    }
    g.setColour (theme::outline.withAlpha (0.6f));
    g.drawHorizontalLine (footerArea.getY(), 0.0f, (float) getWidth());

    // Picture
    if (step != Step::result)
    {
        g.setColour (theme::panel);
        g.fillRoundedRectangle (artArea.toFloat(), 14.0f);
        auto art = artArea.toFloat().reduced (24.0f);
        RoomPicture room;
        switch (step)
        {
            case Step::welcome:
                room.playing = 3;
                break;
            case Step::speakers:
                room.playing = separate ? 1 : 3;
                room.positions = 0;
                room.listener = 1.0f;
                break;
            case Step::microphone:
            {
                const int k = calBox.getSelectedId() - 2;
                drawMicStep (g, art, micDescription(), juce::isPositiveAndBelow (k, (int) calList.size()) ? &calList[(size_t) k] : nullptr);
            }
                break;
            case Step::placement:
            {
                auto side = art.removeFromBottom (art.getHeight() * 0.42f);
                room.positions = positions;
                room.current = 0;
                drawRoom (g, art, room);
                drawMicSide (g, side.reduced (10.0f, 0.0f));
                break;
            }
            case Step::level:
                room.playing = checking ? (separate ? 1 : 3) : 0;
                break;
            case Step::nearField:
                drawNearField (g, art, nearSpeaker >= 0 ? nearSpeaker : nearDone[0] && ! nearDone[1] ? 1 : 0, nearSpeaker >= 0 && nearPhase == 1);
                break;
            case Step::measure:
            {
                const bool running = isRunning();
                room.playing = running ? (separate ? (pass == 0 ? 1 : 2) : 3) : 0;
                room.positions = positions;
                room.done = positionsDone();
                room.current = positionsDone() < positions ? positionsDone() : -1;
                if (const auto lay = layout(); separate && lay.valid)
                {
                    // To scale, from the travel times: where the microphone really was at each position.
                    auto pic = measurePicture (lay);
                    pic.playing = room.playing;
                    drawSpeakerLayout (g, art, pic);
                    room.positions = -1;   // drawn
                }
                break;
            }
            case Step::done:
            {
                const auto c = art.getCentre().translated (0.0f, -art.getHeight() * 0.1f);
                const float s = juce::jmin (art.getWidth(), art.getHeight()) * 0.42f;
                const auto circle = juce::Rectangle<float> (s, s).withCentre (c);
                g.setColour (theme::good.withAlpha (0.12f));
                g.fillEllipse (circle.expanded (s * 0.12f));
                g.setColour (theme::good.withAlpha (0.25f));
                g.fillEllipse (circle);
                drawCheck (g, circle.reduced (s * 0.22f), theme::good, s * 0.07f);
                g.setColour (theme::text);
                g.setFont (theme::font (16.0f, true));
                g.drawText ("Your room is calibrated", juce::Rectangle<float> (art.getX(), circle.getBottom() + s * 0.2f, art.getWidth(), 24.0f), juce::Justification::centred);
                break;
            }
            case Step::result: break;
        }
        if (step != Step::microphone && step != Step::nearField && step != Step::placement && step != Step::done && room.positions >= 0)
            drawRoom (g, art, room);

        if (counting)   // countdown over the picture
        {
            g.setColour (theme::bg.withAlpha (0.7f));
            g.fillRoundedRectangle (artArea.toFloat(), 14.0f);
            const int left = juce::jmax (1, (int) std::ceil ((countdownEnd - juce::Time::getMillisecondCounterHiRes()) / 1000.0));
            g.setColour (theme::accentIn);
            g.setFont (theme::font (110.0f, true));
            g.drawText (juce::String (left), artArea.withTrimmedBottom (40), juce::Justification::centred);
            g.setColour (theme::textDim);
            g.setFont (theme::font (15.0f));
            g.drawText ("Step back and keep quiet", artArea.withTrimmedTop (artArea.getHeight() / 2 + 60).withHeight (24), juce::Justification::centred);
        }
    }

    // Title and text
    {
        const char* titles[] = { "Calibrate your speakers", "Speakers", "Measurement microphone", "Measure the speakers up close", "Place the microphone",
                                 "Set the level", "Measure", "Your result", "Correction applied" };
        auto t = textArea;
        g.setColour (theme::text);
        g.setFont (theme::font (24.0f, true));
        juce::String title = titles[(int) step];
        if (mode == Mode::validate)
        {
            if (step == Step::microphone) title = "Validate: measurement microphone";
            if (step == Step::result)     title = "Check the correction";
            if (step == Step::done)       title = refined ? "Correction refined" : "Correction checked";
        }
        g.drawText (title, t.removeFromTop (36), juce::Justification::centredLeft, true);
        if (step != Step::result) bodyLayout.draw (g, t.withTrimmedTop (8).toFloat());
        else
        {
            g.setColour (theme::textFaint);
            g.setFont (theme::font (12.0f));
            g.drawText (speakerDescription(), t.removeFromTop (18), juce::Justification::centredLeft, true);
        }
    }

    // Captions of the controls
    g.setColour (theme::textDim);
    g.setFont (theme::font (12.0f));
    auto caption = [&g] (juce::Component& c, const juce::String& text)
    {
        if (c.isVisible()) g.drawText (text, c.getX() + 2, c.getY() - 18, c.getWidth(), 16, juce::Justification::centredLeft, true);
    };
    caption (micBox, "Microphone input");
    caption (micChannelBox, "Channel");
    caption (calBox, "Calibration file");
    caption (levelBox, "Test signal level");
    if (step == Step::result)
    {
        caption (*strengthCards[0], "Strength");
        caption (*rangeCards[0], "Range");
    }

    // Speakers: the output's device.
    if (step == Step::speakers && output >= 0)
    {
        const auto& dev = e.buses[(size_t) (output - kMaxInputs)].output;
        auto r = deviceButton.getBounds().withX (textArea.getX()).withRight (deviceButton.getX() - 10);
        g.setColour (dev.kind == IoTarget::Kind::None ? theme::warn : theme::text);
        g.setFont (theme::font (13.5f, true));
        g.drawText (dev.kind == IoTarget::Kind::None ? juce::String ("This output has no device yet") : dev.describe(), r, juce::Justification::centredLeft, true);
    }

    // Microphone: the live level of the chosen input and channel, to see that it is the right one.
    if (step == Step::microphone && ! micMeterArea.isEmpty())
    {
        auto r = micMeterArea;
        auto head = r.removeFromTop (18);
        const float db = gainToDb (micPreview);
        const bool hasInput = micInput() >= 0, silent = micPreview <= 0.0f || db < -80.0f;
        g.setColour (theme::textDim);
        g.setFont (theme::font (12.0f));
        g.drawText ("Microphone level", head.withTrimmedLeft (2), juce::Justification::centredLeft);
        g.setColour (silent ? theme::textFaint : theme::text);
        g.drawText (! hasInput ? juce::String ("no input") : silent ? juce::String ("no signal") : juce::String (db, 1) + " dBFS",
                    head.withTrimmedRight (2), juce::Justification::centredRight);
        const auto bar = r.removeFromTop (14).toFloat();
        g.setColour (theme::panel2);
        g.fillRoundedRectangle (bar, 4.0f);
        if (! silent)
        {
            const float width = bar.getWidth() * juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
            g.setColour (db > kClipDb ? theme::mute : db > kGreenHi ? theme::warn : theme::good);
            g.fillRoundedRectangle (bar.withWidth (width), 4.0f);
        }
        g.setColour (theme::outline);
        g.drawRoundedRectangle (bar, 4.0f, 1.0f);
        g.setColour (theme::textFaint);
        g.setFont (theme::font (11.5f));
        g.drawText ("Tap the microphone or speak: the bar should move.", r.withTrimmedTop (4).withTrimmedLeft (2), juce::Justification::centredLeft, true);
    }

    // Level gauge: big meter -60..0 dBFS with the green range, the expected sweep peak as marker.
    if (step == Step::level)
    {
        auto r = gaugeArea;
        const float db = signalRms > 0.0f ? gainToDb (signalRms * 2.0f) : -100.0f;
        const bool clips = gainToDb (clipPeak) > -1.0f;
        juce::String word = "Listening...";
        juce::Colour c = theme::textDim;
        if (! checking && ! levelSeen) word = "Test noise stopped";
        else if (signalRms > 0.0f)
        {
            if (clips || db > kClipDb) { word = "Too loud"; c = theme::mute; }
            else if (db > kGreenHi)    { word = "A little loud"; c = theme::warn; }
            else if (db < kGreenLo)    { word = "Too quiet"; c = theme::warn; }
            else                       { word = levelGood ? "Good" : "Good - hold it there"; c = theme::good; }
        }
        auto head = r.removeFromTop (44);
        g.setColour (c);
        g.setFont (theme::font (30.0f, true));
        g.drawText (word, head, juce::Justification::centredLeft);
        if (signalRms > 0.0f)
        {
            g.setColour (theme::textDim);
            g.setFont (theme::font (15.0f));
            g.drawText (juce::String (db, 1) + " dBFS", head, juce::Justification::centredRight);
        }
        r.removeFromTop (6);
        auto meter = r.removeFromTop (34).toFloat();
        auto px = [&] (float d) { return meter.getX() + meter.getWidth() * juce::jlimit (0.0f, 1.0f, (d + 60.0f) / 60.0f); };
        g.saveState();
        {
            juce::Path clip;
            clip.addRoundedRectangle (meter, 8.0f);
            g.reduceClipRegion (clip);
            g.setColour (theme::panel2);
            g.fillRect (meter);
            auto zone = [&] (float from, float to, juce::Colour col)
            {
                g.setColour (col);
                g.fillRect (juce::Rectangle<float>::leftTopRightBottom (px (from), meter.getY(), px (to), meter.getBottom()));
            };
            zone (-30.0f, kGreenLo, theme::warn.withAlpha (0.10f));
            zone (kGreenLo, kGreenHi, theme::good.withAlpha (0.30f));
            zone (kGreenHi, kClipDb, theme::warn.withAlpha (0.16f));
            zone (kClipDb, 0.0f, theme::mute.withAlpha (0.3f));
            const float live = gainToDb (micLevel);
            g.setColour (theme::text.withAlpha (0.25f));
            g.fillRect (meter.withRight (px (live)).reduced (0.0f, 12.0f));
        }
        g.restoreState();
        g.setColour (theme::outline);
        g.drawRoundedRectangle (meter, 8.0f, 1.0f);
        if (signalRms > 0.0f)
        {
            const float x = px (db);
            g.setColour (c);
            g.fillRoundedRectangle (x - 3.0f, meter.getY() - 6.0f, 6.0f, meter.getHeight() + 12.0f, 3.0f);
        }
        auto scale = r.removeFromTop (18).toFloat();
        g.setFont (theme::font (11.0f));
        for (float d : { -60.0f, -48.0f, -36.0f, -24.0f, kGreenLo, kGreenHi, 0.0f })
        {
            g.setColour (d == kGreenLo || d == kGreenHi ? theme::good : theme::textFaint);
            const float x = juce::jlimit (scale.getX() + 10.0f, scale.getRight() - 10.0f, px (d));
            g.drawText (juce::String (juce::roundToInt (d)), juce::Rectangle<float> (x - 16.0f, scale.getY() + 2.0f, 32.0f, 14.0f), juce::Justification::centred);
        }
        r.removeFromTop (10);
        juce::String advice;
        juce::Colour ac = theme::textDim;
        if (signalRms > 0.0f)
        {
            if (clips)                 { advice = "The microphone clips: turn its gain (or the speakers) down."; ac = theme::mute; }
            else if (db > kGreenHi)    advice = "Turn the speakers or the microphone gain down a little.";
            else if (db < kGreenLo)    advice = "Turn the speakers or the microphone gain up.";
            else                       advice = "Perfect. Continue when you're ready.";
            if (noiseRms > 0.0f)
            {
                const int snr = juce::roundToInt (db - gainToDb (noiseRms * 2.0f));
                advice << "\nRoom noise: " << snr << " dB below the test signal" << (snr >= 40 ? " - quiet enough." : " - make the room quieter if you can (fans, computer).");
                if (snr < 40) ac = theme::warn;
            }
        }
        else if (checking)
            advice = "Waiting for the first burst...";
        g.setColour (ac);
        g.setFont (theme::font (13.5f));
        g.drawFittedText (advice, r.removeFromTop (44), juce::Justification::topLeft, 3);
    }

    // Measure: overall progress and what plays right now.
    if (step == Step::measure)
    {
        auto r = gaugeArea;
        const int total = positions * sides();
        int doneSweeps = positionsDone() * sides() + (pass > 0 ? pass : 0);
        float frac = (float) doneSweeps / (float) juce::jmax (1, total);
        auto* run = e.getMeasurement();
        const bool running = isRunning();
        if (running && run != nullptr) frac += run->progress() / (float) juce::jmax (1, total);
        auto bar = r.removeFromTop (14).toFloat();
        g.setColour (theme::panel2);
        g.fillRoundedRectangle (bar, 7.0f);
        g.setColour (theme::accentIn);
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, frac)), 7.0f);
        r.removeFromTop (8);
        juce::String phase;
        if (counting) phase = "Starting in a moment - step back and keep quiet.";
        else if (running && run != nullptr)
        {
            phase = (positions > 1 ? "Position " + juce::String (positionsDone() + 1) + " of " + juce::String (positions) + "  -  " : juce::String())
                    + (separate ? (pass == 0 ? "left speaker" : "right speaker") : juce::String ("both speakers"));
            const int pos = run->pos.load();
            phase << (pos < spec.leadLength() ? "  -  listening to the silence..." : "  -  sweep playing...");
        }
        else if (analysing) phase = "Analysing...";
        else phase = juce::String (positionsDone()) + " of " + juce::String (positions) + (positions == 1 ? " position" : " positions") + " measured";
        g.setColour (theme::text);
        g.setFont (theme::font (14.0f, true));
        g.drawText (phase, r.removeFromTop (22), juce::Justification::centredLeft, true);
    }

    // Result: numbers before -> after.
    if (step == Step::result && ! statsArea.isEmpty())
    {
        auto r = statsArea;
        const int k = viewSide;
        const auto& eq = eqs[k];
        if (mode == Mode::validate && eqPending == 0 && results[k] != nullptr)
        {
            auto row = r.removeFromTop (64);
            auto first = row.removeFromLeft ((row.getWidth() - 10) / 2);
            row.removeFromLeft (10);
            auto tile = [&g] (juce::Rectangle<int> t, const juce::String& label, double before, double after)
            {
                g.setColour (theme::panel2);
                g.fillRoundedRectangle (t.toFloat(), 8.0f);
                t.reduce (12, 8);
                g.setColour (theme::textDim);
                g.setFont (theme::font (11.5f));
                g.drawText (label, t.removeFromTop (16), juce::Justification::centredLeft, true);
                g.setFont (theme::font (20.0f, true));
                const auto b = before >= 0.0 ? juce::String (before, 1) + " dB" : juce::String ("-"), a = juce::String (after, 1) + " dB";
                g.drawText (b, t.removeFromLeft (juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), b) + 4), juce::Justification::centredLeft);
                g.setFont (theme::font (14.0f));
                g.drawText ("->", t.removeFromLeft (26), juce::Justification::centred);
                g.setColour (theme::good);
                g.setFont (theme::font (20.0f, true));
                g.drawText (a, t, juce::Justification::centredLeft);
            };
            tile (first, "Deviation: before calibration -> now", devBefore[k], devNow[k]);
            if (! eq.bands.empty()) tile (row, "Now -> after refining", eq.rmsBeforeDb, eq.rmsAfterDb);
            r.removeFromTop (10);
            g.setFont (theme::font (13.0f));
            g.setColour (eq.bands.empty() ? theme::good : theme::textDim);
            g.drawFittedText (eq.bands.empty() ? juce::String ("Nothing left to refine: the correction does its job.")
                                               : "Refine adds " + juce::String ((int) eq.bands.size()) + " small bands (bass range).",
                              r.removeFromTop (40), juce::Justification::topLeft, 2);
        }
        else if (eqPending > 0)
        {
            g.setColour (theme::textDim);
            g.setFont (theme::font (13.5f));
            g.drawText ("Designing the correction...", r.removeFromTop (22), juce::Justification::centredLeft);
        }
        else if (results[k] != nullptr && eq.bands.empty())
        {
            g.setColour (theme::good);
            g.setFont (theme::font (13.5f));
            g.drawFittedText ("Nothing to correct in this range: the response is already flat.", r.removeFromTop (40), juce::Justification::topLeft, 2);
        }
        else if (results[k] != nullptr)
        {
            auto tile = [&g] (juce::Rectangle<int> t, const juce::String& label, double before, double after)
            {
                g.setColour (theme::panel2);
                g.fillRoundedRectangle (t.toFloat(), 8.0f);
                t.reduce (12, 8);
                g.setColour (theme::textDim);
                g.setFont (theme::font (11.5f));
                g.drawText (label, t.removeFromTop (16), juce::Justification::centredLeft, true);
                g.setFont (theme::font (20.0f, true));
                g.setColour (theme::textDim);
                const auto b = juce::String (before, 1) + " dB", a = juce::String (after, 1) + " dB";
                const int bw = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), b);
                g.drawText (b, t.removeFromLeft (bw + 4), juce::Justification::centredLeft);
                g.setFont (theme::font (14.0f));
                g.drawText ("->", t.removeFromLeft (26), juce::Justification::centred);
                g.setColour (theme::good);
                g.setFont (theme::font (20.0f, true));
                g.drawText (a, t, juce::Justification::centredLeft);
            };
            auto row = r.removeFromTop (64);
            tile (row.removeFromLeft ((row.getWidth() - 10) / 2), "Deviation from flat (rms)", eq.rmsBeforeDb, eq.rmsAfterDb);
            row.removeFromLeft (10);
            tile (row, "Highest peak", eq.peakBeforeDb, eq.peakAfterDb);
            r.removeFromTop (10);
            g.setFont (theme::font (13.0f));
            g.setColour (theme::textDim);
            juce::String line = juce::String ((int) eq.bands.size()) + " filter bands, level " + (eq.preampDb > 0.0 ? "+" : "") + juce::String (eq.preampDb, 1) + " dB";
            const auto& res = results[k]->analysis.resonances;
            if (! res.empty())
                line << "\nStrongest room mode: " << hz (res.front().freq) << " (+" << juce::String (res.front().excessDb, 1) << " dB, rings "
                     << juce::roundToInt (res.front().ringMs) << " ms)";
            g.drawFittedText (line, r.removeFromTop (40), juce::Justification::topLeft, 2);
        }
    }

    if (step == Step::result && status.isEmpty())
        bodyLayout.draw (g, statusArea.toFloat());
    if (status.isNotEmpty() && ! statusArea.isEmpty())
    {
        g.setColour (statusColour);
        g.setFont (theme::font (13.5f));
        g.drawFittedText (status, statusArea, juce::Justification::topLeft, 4);
    }
}

void CorrectionWizard::resized()
{
    auto r = getLocalBounds();
    stepsArea = r.removeFromTop (64).reduced (40, 0);
    footerArea = r.removeFromBottom (64);
    auto body = r.reduced (28, 20);

    {
        auto f = footerArea.reduced (28, 14);
        backButton.setBounds (f.removeFromLeft (110));
        nextButton.setBounds (f.removeFromRight (190));
        f.removeFromRight (10);
        if (step == Step::done)
        {
            showCorrButton.setBounds (nextButton.getBounds());
            restartButton.setBounds (backButton.getBounds());
        }
        analysisButton.setBounds (f.removeFromRight (190));
        keepButton.setBounds (analysisButton.getBounds());
        f.removeFromRight (10);
        fineTuneButton.setBounds (f.removeFromRight (130));
    }

    statusArea = {};
    statsArea = {};
    gaugeArea = {};
    micMeterArea = {};
    if (step == Step::result)
    {
        textArea = body.removeFromRight (juce::jlimit (360, 460, body.getWidth() * 36 / 100));
        body.removeFromRight (24);
        artArea = body;
        chart->setBounds (artArea);
        auto t = textArea;
        t.removeFromTop (36 + 24);
        if (separate)
        {
            auto chips = t.removeFromTop (30);
            sideChips[0].setBounds (chips.removeFromLeft (chips.getWidth() / 2 - 4));
            chips.removeFromLeft (8);
            sideChips[1].setBounds (chips);
            t.removeFromTop (14);
        }
        statsArea = t.removeFromTop (128);
        t.removeFromTop (8);
        auto row = [&t] (std::unique_ptr<ChoiceCard>* cards)
        {
            t.removeFromTop (20);
            auto rr = t.removeFromTop (78);
            const int w = (rr.getWidth() - 16) / 3;
            for (int i = 0; i < 3; ++i)
            {
                cards[i]->setBounds (rr.removeFromLeft (w));
                rr.removeFromLeft (8);
            }
            t.removeFromTop (12);
        };
        if (mode == Mode::calibrate)
        {
            row (strengthCards);
            row (rangeCards);
            boostToggle.setBounds (t.removeFromTop (26));
            t.removeFromTop (10);
        }
        // Below the choices: a warning if there is one, otherwise the explanation of the chart.
        bodyLayout.createLayout (bodyText(), (float) t.getWidth());
        statusArea = t;
        return;
    }

    artArea = body.removeFromLeft (body.getWidth() * 47 / 100);
    body.removeFromLeft (36);
    textArea = body;
    body.removeFromTop (36 + 8);
    bodyLayout.createLayout (bodyText(), (float) textArea.getWidth());
    body.removeFromTop ((int) std::ceil (bodyLayout.getHeight()) + 22);
    auto c = body;

    switch (step)
    {
        case Step::speakers:
        {
            deviceButton.setBounds (c.removeFromTop (30).removeFromRight (160));
            c.removeFromTop (16);
            separateCard->setBounds (c.removeFromTop (112));
            c.removeFromTop (10);
            togetherCard->setBounds (c.removeFromTop (112));
            break;
        }
        case Step::microphone:
        {
            c.removeFromTop (18);
            auto row = c.removeFromTop (30);
            micChannelBox.setBounds (row.removeFromRight (100));
            row.removeFromRight (8);
            micBox.setBounds (row);
            c.removeFromTop (30);
            calBox.setBounds (c.removeFromTop (30));
            c.removeFromTop (14);
            micMeterArea = c.removeFromTop (54);
            c.removeFromTop (8);
            break;
        }
        case Step::nearField:
        {
            for (auto& b : nearButtons)
            {
                b.setBounds (c.removeFromTop (40).removeFromLeft (juce::jmin (320, c.getWidth())));
                c.removeFromTop (10);
            }
            c.removeFromTop (8);
            statusArea = c.removeFromTop (90);
            break;
        }
        case Step::placement:
        {
            auto row = c.removeFromTop (190);
            const int w = (row.getWidth() - 20) / 3;
            for (auto& card : posCards)
            {
                card->setBounds (row.removeFromLeft (w));
                row.removeFromLeft (10);
            }
            break;
        }
        case Step::level:
        {
            gaugeArea = c.removeFromTop (170);
            c.removeFromTop (10);
            c.removeFromTop (18);
            auto row = c.removeFromTop (30);
            levelBox.setBounds (row.removeFromLeft (150));
            row.removeFromLeft (10);
            checkButton.setBounds (row.removeFromLeft (160));
            break;
        }
        case Step::measure:
        {
            measureButton.setBounds (c.removeFromTop (46).removeFromLeft (juce::jmin (320, c.getWidth())));
            c.removeFromTop (12);
            auto row = c.removeFromTop (28);
            redoButton.setBounds (row.removeFromLeft (190));
            c.removeFromTop (8);
            delayToggle.setBounds (c.removeFromTop (26));
            c.removeFromTop (18);
            gaugeArea = c.removeFromTop (46);
            c.removeFromTop (8);
            statusArea = c.removeFromTop (90);
            break;
        }
        case Step::done:
        {
            abButton.setBounds (c.removeFromTop (54).removeFromLeft (juce::jmin (420, c.getWidth())));
            c.removeFromTop (14);
            statusArea = c.removeFromTop (60);
            break;
        }
        case Step::welcome:
        case Step::result:
            break;
    }
    if (statusArea.isEmpty()) statusArea = c.removeFromTop (80);

    if (picker != nullptr)
    {
        pickerBackdrop->setBounds (getLocalBounds());
        picker->setBounds (getLocalBounds().withSizeKeepingCentre (juce::jmin (960, getWidth() - 60), juce::jmin (600, getHeight() - 60)));
    }
}
} // namespace hydra

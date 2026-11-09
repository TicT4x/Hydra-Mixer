#include "RoomPages.h"
#include "Dialog.h"
#include "core/ParamEq.h"

namespace hydra
{
namespace
{
// Analysis page: chart options (combo box item i+1 = value i).
const std::vector<double> kSmoothing { 3.0, 6.0, 12.0, 24.0, 0.0 };   // 1/n octave, 0 = none
const std::vector<double> kSpans     { 50.0, 100.0, 300.0, 1000.0 };
// Waterfall presets (any other value can be typed into the boxes).
const std::vector<double> kWfLows    { 15.0, 20.0, 30.0, 50.0, 100.0, 200.0 };
const std::vector<double> kWfHighs   { 200.0, 300.0, 500.0, 1000.0, 2000.0, 5000.0, 20000.0 };
const std::vector<double> kWfLengths { 50.0, 100.0, 200.0, 300.0, 500.0, 800.0, 1000.0 };
constexpr double kWfMaxMs = 1200.0;   // the impulse response ends 1.5 s after the direct sound

// Edit page: range choices and the strength presets (the same as the calibration wizard's).
const std::vector<double> kEqLow  { 20.0, 25.0, 30.0, 40.0, 60.0 };
const std::vector<double> kEqHigh { 200.0, 300.0, 500.0, 1000.0, 2000.0, 5000.0, 20000.0 };
struct Preset { const char* name; const char* detail; double maxCut; int bands; double detailOct; };
const Preset kPresets[] = {
    { "Gentle",   "Only the biggest peaks",  6.0,  8,  6.0 },
    { "Balanced", "Peaks and room modes",    12.0, 16, 12.0 },
    { "Strong",   "Follows the response",    18.0, 24, 24.0 },
};

constexpr int kPad = 16, kGap = 8, kCaptionH = 18;

/** Frequency typed by the user: "500", "2.5k", "1 kHz", "80 Hz". Returns <= 0 if it is not a number. */
double parseHz (juce::String s)
{
    s = s.trim().toLowerCase().removeCharacters (" ").replace ("hz", "");
    const bool kilo = s.endsWithChar ('k');
    if (kilo) s = s.dropLastCharacters (1);
    if (! s.containsOnly ("0123456789.,")) return -1.0;
    const double v = s.replaceCharacter (',', '.').getDoubleValue();
    return kilo ? v * 1000.0 : v;
}

/** Time typed by the user: "300", "300 ms", "1 s", "1.2s". Returns <= 0 if it is not a number. */
double parseMs (juce::String s)
{
    s = s.trim().toLowerCase().removeCharacters (" ");
    double scale = 1.0;
    if (s.endsWith ("ms"))     s = s.dropLastCharacters (2);
    else if (s.endsWith ("s")) { s = s.dropLastCharacters (1); scale = 1000.0; }
    if (! s.containsOnly ("0123456789.,")) return -1.0;
    return s.replaceCharacter (',', '.').getDoubleValue() * scale;
}

juce::String msText (double ms) { return ms >= 1000.0 ? juce::String (ms / 1000.0, ms == std::round (ms / 1000.0) * 1000.0 ? 0 : 2) + " s" : juce::String (juce::roundToInt (ms)) + " ms"; }

double valueOf (const juce::ComboBox& box, const std::vector<double>& values, double fallback)
{
    const int i = box.getSelectedId() - 1;
    return juce::isPositiveAndBelow (i, (int) values.size()) ? values[(size_t) i] : fallback;
}

void fill (juce::ComboBox& box, const juce::StringArray& items, int defaultIndex)
{
    box.clear (juce::dontSendNotification);
    for (int i = 0; i < items.size(); ++i) box.addItem (items[i], i + 1);
    box.setSelectedId (defaultIndex + 1, juce::dontSendNotification);
}

juce::String hz (double f)
{
    return f >= 1000.0 ? (f >= 10000.0 ? juce::String (juce::roundToInt (f / 1000.0)) : juce::String (f / 1000.0, 1)) + " kHz" : juce::String (juce::roundToInt (f)) + " Hz";
}

juce::String shortHz (double f)
{
    return f >= 1000.0 ? juce::String (juce::roundToInt (f / 1000.0)) + "k" : juce::String (juce::roundToInt (f));
}

juce::String typeName (int type)
{
    static const char* names[] = { "Bell", "Notch", "Band Pass", "High Cut", "Low Cut", "Low Shelf", "High Shelf" };
    return names[juce::jlimit (0, 6, type)];
}

/** Where measuring happens: here (the mixer's Room Correction window has the wizard) or in the mixer (the plugin). */
juce::String calibrateFirst (bool here)
{
    return here ? juce::String ("Calibrate your speakers first") : juce::String ("Calibrate your speakers in Hydra Mixer first (Tools > Room Correction)");
}

juce::String dot() { return juce::String::fromUTF8 ("  \xc2\xb7  "); }

/** One line about how a measurement was made. */
juce::String describe (const RoomMeasurement& m)
{
    juce::String s = m.created.formatted ("%Y-%m-%d %H:%M");
    if (m.output.isNotEmpty())     s << dot() << m.output;
    if (m.microphone.isNotEmpty()) s << dot() << "mic " << m.microphone;
    s << dot() << (m.numPositions() > 1 ? juce::String (m.numPositions()) + " positions" : juce::String ("1 position"));
    s << dot() << (m.calibration.empty() ? juce::String ("no mic calibration") : "cal " + juce::String (m.calibration.name));
    if (m.throughEq) s << dot() << "through the correction";
    return s;
}

/** A problem of the measurement worth telling, empty if it is fine. */
juce::String quality (const RoomMeasurement& m)
{
    if (m.ir.clipped)
        return "The microphone clipped during this measurement: the result is distorted. Measure again with less level.";
    if (m.ir.capturePeakDb < -45.0)
        return "The microphone signal was very weak (" + juce::String (juce::roundToInt (m.ir.capturePeakDb)) + " dBFS): the result is noisy.";
    if (m.ir.irSnrDb < 35.0)
        return "Noisy measurement (" + juce::String (juce::roundToInt (m.ir.irSnrDb)) + " dB signal-to-noise): the details may not be reliable.";
    return {};
}

/** Colour of a waterfall slice: warm at 0 ms, cool for the late slices. */
juce::Colour timeColour (float t)
{
    static const juce::Colour stops[] = { juce::Colour (0xffff5f6d), juce::Colour (0xffffa53d), juce::Colour (0xffffd84a),
                                          juce::Colour (0xff3ee08f), juce::Colour (0xff35c8f4), juce::Colour (0xff5b7cff) };
    constexpr int n = (int) std::size (stops);
    const float x = juce::jlimit (0.0f, 1.0f, t) * (float) (n - 1);
    const int i = juce::jmin (n - 2, (int) x);
    return stops[i].interpolatedWith (stops[i + 1], x - (float) i);
}

void drawFreqGrid (juce::Graphics& g, juce::Rectangle<float> plot, double lo, double hi, bool labels)
{
    auto fx = [&] (double f) { return plot.getX() + plot.getWidth() * (float) (std::log (f / lo) / std::log (hi / lo)); };
    g.setFont (theme::font (11.0f));
    for (double decade = 10.0; decade <= 10000.0; decade *= 10.0)
        for (int k = 1; k <= 9; ++k)
        {
            const double f = decade * k;
            if (f < lo * 0.999 || f > hi * 1.001) continue;
            const float x = fx (f);
            const bool major = k == 1 || k == 2 || k == 5;
            g.setColour (theme::outline.withAlpha (major ? 0.8f : 0.35f));
            g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());
            if (labels && major)
            {
                g.setColour (theme::textFaint);
                g.drawText (shortHz (f), juce::Rectangle<float> (x - 24.0f, plot.getBottom() + 4.0f, 48.0f, 14.0f), juce::Justification::centred);
            }
        }
}

void dashedLine (juce::Graphics& g, juce::Point<float> a, juce::Point<float> b, float width)
{
    juce::Path p, d;
    p.startNewSubPath (a);
    p.lineTo (b);
    const float dashes[] = { 5.0f, 4.0f };
    juce::PathStrokeType (width).createDashedStroke (d, p, dashes, 2);
    g.fillPath (d);
}

/** Read-out box under the mouse (charts). */
void drawReadout (juce::Graphics& g, juce::Rectangle<float> plot, float x, const juce::String& text)
{
    g.setColour (theme::text.withAlpha (0.3f));
    g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());
    g.setFont (theme::font (12.0f));
    const float w = (float) juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), text) + 16.0f;
    auto box = juce::Rectangle<float> (x + 10.0f, plot.getY() + 6.0f, w, 20.0f);
    if (box.getRight() > plot.getRight()) box.setX (x - 10.0f - w);
    g.setColour (theme::raised.withAlpha (0.95f));
    g.fillRoundedRectangle (box, 4.0f);
    g.setColour (theme::text);
    g.drawText (text, box, juce::Justification::centred);
}

/** Big title and a dim line under it (page headers, like the wizard's steps). */
void drawHeading (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& title, const juce::String& sub)
{
    g.setColour (theme::text);
    g.setFont (theme::font (22.0f, true));
    g.drawText (title, r.removeFromTop (32), juce::Justification::centredLeft, true);
    g.setColour (theme::textDim);
    g.setFont (theme::font (12.5f));
    g.drawText (sub, r.removeFromTop (20), juce::Justification::centredLeft, true);
}

/** A key number: label, big value, a short note under it. */
void drawTile (juce::Graphics& g, juce::Rectangle<int> t, const juce::String& label, const juce::String& value, juce::Colour c, const juce::String& note)
{
    g.setColour (theme::panel);
    g.fillRoundedRectangle (t.toFloat(), 10.0f);
    t.reduce (14, 8);
    g.setColour (theme::textDim);
    g.setFont (theme::font (11.5f));
    g.drawText (label, t.removeFromTop (16), juce::Justification::centredLeft, true);
    g.setColour (c);
    g.setFont (theme::font (21.0f, true));
    g.drawText (value, t.removeFromTop (28), juce::Justification::centredLeft, true);
    g.setColour (theme::textFaint);
    g.setFont (theme::font (11.0f));
    g.drawText (note, t, juce::Justification::centredLeft, true);
}

/** Before -> after as a tile (Edit page). */
void drawChangeTile (juce::Graphics& g, juce::Rectangle<int> t, const juce::String& label, double before, double after)
{
    g.setColour (theme::panel);
    g.fillRoundedRectangle (t.toFloat(), 10.0f);
    t.reduce (14, 8);
    g.setColour (theme::textDim);
    g.setFont (theme::font (11.5f));
    g.drawText (label, t.removeFromTop (16), juce::Justification::centredLeft, true);
    g.setFont (theme::font (20.0f, true));
    const auto b = juce::String (before, 1) + " dB", a = juce::String (after, 1) + " dB";
    const int bw = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), b);
    g.drawText (b, t.removeFromLeft (bw + 4), juce::Justification::centredLeft);
    g.setFont (theme::font (14.0f));
    g.drawText ("->", t.removeFromLeft (26), juce::Justification::centred);
    g.setColour (theme::good);
    g.setFont (theme::font (20.0f, true));
    g.drawText (a, t, juce::Justification::centredLeft);
}

void addPara (juce::AttributedString& s, const juce::String& text, juce::Colour c = theme::textDim, bool bold = false, float size = 13.5f)
{
    s.append (text, theme::font (size, bold), c);
}

void accent (juce::TextButton& b)
{
    b.setColour (juce::TextButton::buttonColourId, theme::accentIn);
    b.setColour (juce::TextButton::textColourOffId, theme::bg);
}

/** Save dialog for an export; write returns false on failure (then a message). */
void saveAs (juce::Component* owner, std::unique_ptr<juce::FileChooser>& chooser, const juce::String& title, const juce::String& base,
             const juce::String& pattern, std::function<bool (const juce::File&)> write)
{
    const auto name = juce::File::createLegalFileName (base).replace ("->", "to").replaceCharacters (" ", "_");
    chooser = std::make_unique<juce::FileChooser> (title, juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                                                              .getChildFile (name + pattern.fromLastOccurrenceOf ("*", false, false)), pattern);
    juce::Component::SafePointer<juce::Component> safe (owner);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe, write] (const juce::FileChooser& fc)
    {
        const auto f = fc.getResult();
        if (safe == nullptr || f == juce::File()) return;
        if (! write (f))
            dialog::message (safe.getComponent(), dialog::Kind::warning, "Cannot save", "Could not write " + f.getFullPathName() + ".");
    });
}
} // namespace

//==============================================================================
namespace
{
/** Metres -> pixels of a layout picture: everything (speakers, ideal spot, spots, rings) fits in, same scale on both axes. */
struct LayoutMap
{
    double x0 = 0.0, x1 = 0.0, y0 = 0.0, y1 = 0.0;
    float scale = 1.0f;
    juce::Point<float> origin;
    juce::Rectangle<float> summary;
    juce::Point<float> px (double x, double y) const { return origin + juce::Point<float> ((float) x * scale, (float) y * scale); }

    LayoutMap (juce::Rectangle<float> area, const LayoutPicture& pic)
    {
        const auto& lay = *pic.layout;
        const double half = lay.spacingM * 0.5, apex = lay.spacingM * std::sqrt (3.0) * 0.5;
        summary = pic.summary ? area.removeFromBottom (46.0f) : juce::Rectangle<float>();
        x0 = -half - 0.4; x1 = half + 0.4; y0 = -0.4; y1 = apex + 0.4;
        auto extend = [&] (double x, double y)
        {
            x0 = std::min (x0, x - 0.35);
            x1 = std::max (x1, x + 0.35);
            y1 = std::max (y1, y + 0.4);
        };
        for (auto& s : lay.mics)
            if (s.distL > 0.0 || s.distR > 0.0) extend (s.x, s.y);
        for (auto& p : pic.planned) extend (p.x, p.y);
        scale = (float) std::min (area.getWidth() / (x1 - x0), area.getHeight() / (y1 - y0));
        origin = area.getCentre() - juce::Point<float> ((float) ((x0 + x1) * 0.5) * scale, (float) ((y0 + y1) * 0.5) * scale);
    }
};
} // namespace

int hitLayoutSpot (juce::Rectangle<float> area, const LayoutPicture& pic, juce::Point<float> at)
{
    if (pic.layout == nullptr || ! pic.layout->valid) return -1;
    const LayoutMap map (area, pic);
    int best = -1;
    float bestD = 16.0f;
    for (size_t k = 0; k < pic.layout->mics.size(); ++k)
    {
        const auto& s = pic.layout->mics[k];
        if (s.distL <= 0.0 && s.distR <= 0.0) continue;
        if (const float d = map.px (s.x, s.y).getDistanceFrom (at); d < bestD) { bestD = d; best = (int) k; }
    }
    for (size_t k = 0; k < pic.planned.size(); ++k)
        if (const float d = map.px (pic.planned[k].x, pic.planned[k].y).getDistanceFrom (at); d < bestD) { bestD = d; best = (int) k; }
    return best;
}

void drawSpeakerLayout (juce::Graphics& g, juce::Rectangle<float> area, const LayoutPicture& pic)
{
    if (pic.layout == nullptr || ! pic.layout->valid) return;
    const auto& lay = *pic.layout;
    const double half = lay.spacingM * 0.5, apex = lay.spacingM * std::sqrt (3.0) * 0.5;
    const LayoutMap map (area, pic);
    const auto summaryArea = map.summary;
    const double x0 = map.x0, x1 = map.x1, y0 = map.y0, y1 = map.y1;
    const float scale = map.scale;
    auto px = [&map] (double x, double y) { return map.px (x, y); };
    const room::Layout::Spot* main = nullptr;   // the listening position: the first spot that fits
    for (auto& s : lay.mics)
        if (s.ok) { main = &s; break; }

    // Grid, every half metre.
    g.setColour (theme::outline.withAlpha (0.22f));
    for (double x = std::ceil (x0 * 2.0) * 0.5; x <= x1; x += 0.5)
        g.drawVerticalLine (juce::roundToInt (px (x, 0.0).x), px (0.0, y0).y, px (0.0, y1).y);
    for (double y = std::ceil (y0 * 2.0) * 0.5; y <= y1; y += 0.5)
        g.drawHorizontalLine (juce::roundToInt (px (0.0, y).y), px (x0, 0.0).x, px (x1, 0.0).x);
    g.setColour (theme::textFaint);
    g.setFont (theme::font (10.5f));
    g.drawText ("grid 50 cm", juce::Rectangle<float> (px (x0, y1).x + 4.0f, px (x0, y1).y - 16.0f, 80.0f, 14.0f), juce::Justification::centredLeft);

    const auto L = px (-half, 0.0), R = px (half, 0.0), ideal = px (0.0, apex);
    // The ideal stereo triangle (dashed) and its listening spot.
    g.setColour (theme::textFaint.withAlpha (0.7f));
    dashedLine (g, L, ideal, 1.0f);
    dashedLine (g, R, ideal, 1.0f);
    g.drawEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (ideal), 1.0f);
    g.drawText ("ideal", juce::Rectangle<float> (ideal.x + 8.0f, ideal.y - 7.0f, 50.0f, 14.0f), juce::Justification::centredLeft);
    // Between the speakers.
    g.setColour (theme::textDim);
    g.drawLine (L.x, L.y, R.x, R.y, 1.0f);
    g.setFont (theme::font (12.0f, true));
    g.drawText (juce::String (lay.spacingM, 2) + " m", juce::Rectangle<float> ((L.x + R.x) * 0.5f - 40.0f, L.y - 24.0f, 80.0f, 16.0f), juce::Justification::centred);

    // Around the planned spots: the area in which a measurement is fine (one path: overlapping areas stay even).
    if (pic.toleranceM > 0.0 && ! pic.planned.empty())
    {
        const float d = (float) pic.toleranceM * 2.0f * scale;
        juce::Path areas;
        for (auto& spot : pic.planned)
            areas.addEllipse (juce::Rectangle<float> (d, d).withCentre (px (spot.x, spot.y)));
        g.setColour (theme::textDim.withAlpha (0.2f));
        g.fillPath (areas);
    }

    // Planned spots: dashed rings, the next one in the accent colour.
    for (size_t k = 0; k < pic.planned.size(); ++k)
    {
        const auto p = px (pic.planned[k].x, pic.planned[k].y);
        const bool next = (int) k == pic.nextPlanned;
        const auto ring = juce::Rectangle<float> (20.0f, 20.0f).withCentre (p);
        g.setColour (next ? theme::accentIn.withAlpha (0.18f) : theme::panel2);
        g.fillEllipse (ring);
        g.setColour (next ? theme::accentIn : theme::textFaint);
        g.drawEllipse (ring, next ? 1.8f : 1.0f);
        g.setFont (theme::font (10.5f, true));
        g.drawText (juce::String ((int) k + 1), ring, juce::Justification::centred);
    }

    // The listening position: lines to the speakers with their distance, and the angle between them.
    if (main != nullptr)
    {
        const auto m = px (main->x, main->y);
        const juce::Point<float> sp[2] = { L, R };
        const double dist[2] = { main->distL, main->distR };
        for (int k = 0; k < 2; ++k)
        {
            const auto c = k == 0 ? theme::accentIn : theme::accentVirt;
            g.setColour (c.withAlpha (0.8f));
            g.drawLine (sp[k].x, sp[k].y, m.x, m.y, 1.6f);
            const auto mid = (sp[k] + m) * 0.5f;
            g.setFont (theme::font (12.0f, true));
            g.drawText (juce::String (dist[k], 2) + " m", juce::Rectangle<float> (k == 0 ? mid.x - 74.0f : mid.x + 8.0f, mid.y - 8.0f, 66.0f, 16.0f),
                        k == 0 ? juce::Justification::centredRight : juce::Justification::centredLeft);
        }
        // Arc between the two directions (JUCE angles: 0 = up, clockwise).
        auto angle = [&] (juce::Point<float> to) { return std::atan2 (to.x - m.x, -(to.y - m.y)); };
        const float r = juce::jlimit (22.0f, 46.0f, m.getDistanceFrom (L) * 0.25f);
        juce::Path arc;
        arc.addCentredArc (m.x, m.y, r, r, 0.0f, angle (L), angle (R), true);
        g.setColour (theme::text.withAlpha (0.6f));
        g.strokePath (arc, juce::PathStrokeType (1.2f));
        g.setFont (theme::font (11.5f, true));
        const double opening = main->angleR - main->angleL;
        g.drawText (juce::String (juce::roundToInt (opening)) + juce::String::fromUTF8 ("\xc2\xb0"),
                    juce::Rectangle<float> (m.x - 30.0f, m.y - r - 18.0f, 60.0f, 14.0f), juce::Justification::centred);
    }

    // Measured spots: dots, numbered; the highlighted one bigger. Distances that do not fit together (hollow, orange)
    // still show, so every measured position is there.
    for (size_t k = 0; k < lay.mics.size(); ++k)
    {
        const auto& s = lay.mics[k];
        if (s.distL <= 0.0 && s.distR <= 0.0) continue;
        const auto p = px (s.x, s.y);
        if (! s.ok)
        {
            const auto r = juce::Rectangle<float> (12.0f, 12.0f).withCentre (p);
            g.setColour (theme::warn);
            g.drawEllipse (r, 1.6f);
            g.setFont (theme::font (9.5f, true));
            g.drawText (juce::String ((int) k + 1), r.translated (11.0f, -9.0f), juce::Justification::centred);
            continue;
        }
        const bool big = (int) k == pic.highlight;
        const float d = big ? 14.0f : 10.0f;
        const bool outside = pic.toleranceM > 0.0 && k < pic.planned.size()
                             && std::hypot (s.x - pic.planned[k].x, s.y - pic.planned[k].y) > pic.toleranceM;
        if (big)
        {
            g.setColour ((outside ? theme::learn : theme::good).withAlpha (0.25f));
            g.fillEllipse (juce::Rectangle<float> (d * 2.0f, d * 2.0f).withCentre (p));
        }
        g.setColour (outside ? theme::learn : &s == main ? theme::text : theme::good);
        g.fillEllipse (juce::Rectangle<float> (d, d).withCentre (p));
        g.setColour (theme::bg);
        g.setFont (theme::font (9.5f, true));
        g.drawText (juce::String ((int) k + 1), juce::Rectangle<float> (d, d).withCentre (p), juce::Justification::centred);
    }

    // The speakers on top.
    const float w = juce::jlimit (16.0f, 40.0f, 0.24f * scale), h = w * 0.8f;
    for (int k = 0; k < 2; ++k)
    {
        const auto c = k == 0 ? theme::accentIn : theme::accentVirt;
        const auto box = juce::Rectangle<float> (w, h).withCentre (k == 0 ? L : R).translated (0.0f, -h * 0.3f);
        if ((pic.playing >> k) & 1)
        {
            g.setColour (c.withAlpha (0.25f));
            g.fillRoundedRectangle (box.expanded (6.0f), 6.0f);
        }
        g.setColour (theme::raised);
        g.fillRoundedRectangle (box, 4.0f);
        g.setColour (c);
        g.drawRoundedRectangle (box, 4.0f, 1.6f);
        g.setFont (theme::font (11.0f, true));
        g.drawText (k == 0 ? "L" : "R", box, juce::Justification::centred);
    }

    if (! summaryArea.isEmpty())
    {
        auto r = summaryArea;
        juce::String line = "Speakers " + juce::String (lay.spacingM, 2) + " m apart";
        juce::String second;
        if (main != nullptr)
        {
            const double opening = main->angleR - main->angleL;
            line << dot() << "listening position " << juce::String (main->distL, 2) << " m / " << juce::String (main->distR, 2) << " m away"
                 << dot() << "angle " << juce::roundToInt (opening) << juce::String::fromUTF8 ("\xc2\xb0 (ideal 60\xc2\xb0)");
            const int off = juce::roundToInt (main->x * 100.0);
            second = std::abs (off) < 3 ? juce::String ("Centred between the speakers.")
                                        : juce::String (std::abs (off)) + " cm " + (off > 0 ? "right" : "left") + " of the centre: the "
                                              + (off > 0 ? "right" : "left") + " speaker arrives "
                                              + juce::String (std::abs (main->distL - main->distR) / 0.343, 2) + " ms earlier.";
        }
        else
            second = "The microphone positions show up here once they are measured.";
        g.setColour (theme::text);
        g.setFont (theme::font (13.0f, true));
        g.drawText (line, r.removeFromTop (22.0f), juce::Justification::centred, true);
        g.setColour (theme::textDim);
        g.setFont (theme::font (12.0f));
        g.drawText (second + juce::String::fromUTF8 ("  Measured from the sound's travel time (about \xc2\xb1" "5 cm)."), r, juce::Justification::centred, true);
    }
}

//==============================================================================
RoomLibrary::RoomLibrary() : list (measurements::loadLibrary())
{
    if (! list.empty()) select (0);
}

RoomMeasurement* RoomLibrary::current() const
{
    return juce::isPositiveAndBelow (selected, (int) list.size()) ? list[(size_t) selected].get() : nullptr;
}

int RoomLibrary::indexOf (const RoomMeasurement* m) const
{
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i].get() == m) return (int) i;
    return -1;
}

void RoomLibrary::select (int index)
{
    selected = juce::isPositiveAndBelow (index, (int) list.size()) ? index : -1;
    analyseIfNeeded (current(), false);   // ready by the time it is loaded
    changed();
}

void RoomLibrary::add (std::unique_ptr<RoomMeasurement> m)
{
    if (m == nullptr) return;
    for (auto& x : list)
        if (x->file == m->file && m->file != juce::File()) return;
    if (m->right != nullptr) m->right = std::make_shared<RoomMeasurement> (*m->right);   // its own copy of the right speaker
    list.insert (list.begin(), std::move (m));
    select (0);
}

void RoomLibrary::remove (RoomMeasurement* m)
{
    const int i = indexOf (m);
    if (i < 0) return;
    m->file.deleteFile();
    if (loadedPtr == m) loadedPtr = nullptr;
    for (auto* x : { (const RoomMeasurement*) m, (const RoomMeasurement*) m->right.get() })
        analysing.erase (std::remove (analysing.begin(), analysing.end(), x), analysing.end());
    list.erase (list.begin() + i);
    select (selected > i ? selected - 1 : juce::jmin (selected, (int) list.size() - 1));   // the next one takes its place
}

void RoomLibrary::rename (RoomMeasurement* m, const juce::String& name)
{
    if (indexOf (m) < 0 || name.trim().isEmpty()) return;
    m->name = name.trim();
    measurements::save (*m, m->file);
    changed();
}

void RoomLibrary::setCalibration (RoomMeasurement* m, const room::Calibration& cal)
{
    if (indexOf (m) < 0) return;
    for (int k = 0; k < m->sides(); ++k)
    {
        m->side (k)->calibration = cal;
        m->side (k)->analysed = false;
    }
    measurements::save (*m, m->file);
    analyseIfNeeded (m);
    changed();
}

void RoomLibrary::setLoaded (RoomMeasurement* m)
{
    if (indexOf (m) < 0) m = nullptr;
    if (m == loadedPtr) return;
    loadedPtr = m;
    analyseIfNeeded (m);
    changed();
}

RoomMeasurement* RoomLibrary::findByFile (const juce::String& fileName) const
{
    if (fileName.isEmpty()) return nullptr;
    for (auto& m : list)
        if (m->file.getFileName() == fileName) return m.get();
    return nullptr;
}

void RoomLibrary::setWaterfallMs (double ms)
{
    if (std::abs (ms - waterfallMs) < 0.5) return;
    waterfallMs = ms;
    analyseIfNeeded (loadedPtr);
}

bool RoomLibrary::isAnalysing (const RoomMeasurement* m) const
{
    return std::find (analysing.begin(), analysing.end(), m) != analysing.end();
}

void RoomLibrary::analyseIfNeeded (RoomMeasurement* m, bool full)
{
    if (m == nullptr) return;
    analyseSide (m, full);
    if (m->right != nullptr) analyseSide (m->right.get(), full);
}

void RoomLibrary::setLightweight (bool on)
{
    parallel = ! on;
    deferFull = on;
}

void RoomLibrary::allowFull()
{
    if (! deferFull) return;
    deferFull = false;
    analyseIfNeeded (loadedPtr);
}

bool RoomLibrary::wantsFull (const RoomMeasurement* m) const
{
    return ! deferFull && m != nullptr && loadedPtr != nullptr && (m == loadedPtr || m == loadedPtr->right.get());
}

void RoomLibrary::analyseSide (RoomMeasurement* m, bool full)
{
    // Not analysed yet: the response first (quick). Then, if wanted, the rest with the current waterfall length;
    // a measurement analysed before keeps showing its old analysis meanwhile.
    if (m == nullptr || isAnalysing (m)) return;
    full = full && ! deferFull;
    const bool quick = ! m->analysed;
    const bool upToDate = m->analysed && (! full || (isComplete (*m) && std::abs (m->analysis.waterfallMs.back() - waterfallMs) < 0.5));
    if (upToDate) return;
    analysing.push_back (m);
    auto opts = measurements::analysisOptions (*m);
    opts.waterfallMs = waterfallMs;
    opts.responseOnly = quick;
    opts.parallel = parallel;
    auto copy = std::make_shared<RoomMeasurement> (*m);   // the measurement may be deleted meanwhile
    copy->right.reset();
    juce::WeakReference<RoomLibrary> weak (this);
    // Low priority: the audio threads come first (in the mixer and in a DAW).
    juce::Thread::launch (juce::Thread::Priority::low, [weak, m, copy, opts]
    {
        auto a = std::make_shared<room::Analysis> (measurements::analyse (*copy, opts));
        juce::MessageManager::callAsync ([weak, m, a, copy]
        {
            auto* self = weak.get();
            if (self == nullptr || ! self->isAnalysing (m)) return;   // gone, or deleted meanwhile
            self->analysing.erase (std::remove (self->analysing.begin(), self->analysing.end(), m), self->analysing.end());
            if (m->calibration.points != copy->calibration.points)   // another calibration was chosen meanwhile: this result is stale
            {
                m->analysed = false;
                self->analyseSide (m, self->wantsFull (m));
                return;
            }
            m->analysis = std::move (*a);
            m->analysed = true;
            self->changed();
            self->analyseSide (m, self->wantsFull (m));   // the rest, or the waterfall length changed meanwhile
        });
    });
}

//==============================================================================
void ChoiceCard::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (selected ? theme::accentIn.withAlpha (0.12f) : hover ? theme::raised : theme::panel2);
    g.fillRoundedRectangle (r, 10.0f);
    g.setColour (selected ? theme::accentIn : theme::outline);
    g.drawRoundedRectangle (r, 10.0f, selected ? 2.0f : 1.0f);

    auto inner = r.reduced (14.0f, 10.0f);
    // Radio mark at the top right.
    auto mark = juce::Rectangle<float> (16.0f, 16.0f).withPosition (r.getRight() - 26.0f, r.getY() + 10.0f);
    g.setColour (selected ? theme::accentIn : theme::textFaint);
    g.drawEllipse (mark, 1.6f);
    if (selected) g.fillEllipse (mark.reduced (4.0f));

    if (painter)
    {
        const auto icon = iconTop ? inner.removeFromTop (inner.getHeight() * 0.4f) : inner.removeFromLeft (juce::jmin (inner.getHeight() * 1.5f, 90.0f));
        painter (g, icon, selected);
        if (iconTop) inner.removeFromTop (4.0f); else inner.removeFromLeft (12.0f);
    }
    if (badge.isNotEmpty())
    {
        g.setFont (theme::font (10.5f, true));
        const float w = (float) juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), badge) + 14.0f;
        auto b = juce::Rectangle<float> (w, 17.0f).withPosition (inner.getX(), inner.getY());
        g.setColour (theme::good.withAlpha (0.18f));
        g.fillRoundedRectangle (b, 8.5f);
        g.setColour (theme::good);
        g.drawText (badge, b, juce::Justification::centred);
        inner.removeFromTop (20.0f);
    }
    g.setColour (isEnabled() ? theme::text : theme::textFaint);
    g.setFont (theme::font (15.0f, true));
    g.drawText (title, inner.removeFromTop (22.0f).withTrimmedRight (iconTop ? 0.0f : 18.0f), juce::Justification::centredLeft, true);
    g.setColour (theme::textDim);
    g.setFont (theme::font (12.5f));
    g.drawFittedText (detail, inner.toNearestInt(), juce::Justification::topLeft, 3);
}

//==============================================================================
void AddButton::paintButton (juce::Graphics& g, bool over, bool down)
{
    const auto b = getLocalBounds().toFloat().reduced (0.5f);
    const auto c = ! isEnabled() ? theme::raised : down ? theme::accentIn.darker (0.15f) : over ? theme::accentIn.brighter (0.12f) : theme::accentIn;
    g.setColour (c);
    g.fillRoundedRectangle (b, 6.0f);
    const auto fg = isEnabled() ? theme::bg : theme::textFaint;
    g.setFont (theme::font (13.5f, true));
    const float textW = (float) juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), getButtonText());
    const float icon = 16.0f, gap = 8.0f;
    const float x = b.getCentreX() - (icon + gap + textW) * 0.5f;
    // "+" in a circle.
    const auto circle = juce::Rectangle<float> (icon, icon).withCentre ({ x + icon * 0.5f, b.getCentreY() });
    g.setColour (fg);
    g.drawEllipse (circle.reduced (0.8f), 1.6f);
    g.fillRect (juce::Rectangle<float> (icon * 0.5f, 1.8f).withCentre (circle.getCentre()));
    g.fillRect (juce::Rectangle<float> (1.8f, icon * 0.5f).withCentre (circle.getCentre()));
    g.drawText (getButtonText(), juce::Rectangle<float> (x + icon + gap, b.getY(), textW + 4.0f, b.getHeight()), juce::Justification::centredLeft);
}

//==============================================================================
MeasurementList::MeasurementList (RoomLibrary& l) : lib (l)
{
    list.setRowHeight (48);
    list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    list.setOutlineThickness (0);
    list.setTooltip ("Saved measurements. Double click to rename, right click for more.");
    addAndMakeVisible (list);
    for (auto* b : { &importButton, &exportButton, &deleteButton, &loadButton }) addAndMakeVisible (*b);
    addAndMakeVisible (calibrateButton);
    calibrateButton.setTooltip ("Measures your speakers in the room step by step and makes a new correction (a new measurement).");
    importButton.onClick = [this] { importFile(); };
    exportButton.onClick = [this] { showExportMenu(); };
    deleteButton.onClick = [this] { deleteSelected(); };
    loadButton.onClick = [this] { if (onLoad) onLoad(); };
    calibrateButton.onClick = [this] { if (onCalibrate) onCalibrate(); };
    importButton.setTooltip ("Adds a measurement file (.hydrameasure) to the library.");
    exportButton.setTooltip ("Saves the selected measurement: as a measurement file, its frequency response for REW, or its impulse response.");
    deleteButton.setTooltip ("Deletes the selected measurement from the library.");
    loadButton.setTooltip ("Makes the output's correction from the selected measurement (with the settings of the Edit page). "
                           "Analysis and Edit then show this measurement.");
    lib.addListener (this);
    libraryChanged();
}

MeasurementList::~MeasurementList() { lib.removeListener (this); }

void MeasurementList::libraryChanged()
{
    list.updateContent();
    if (lib.currentIndex() >= 0) list.selectRow (lib.currentIndex(), false, true);
    else                         list.deselectAllRows();
    const bool has = lib.current() != nullptr;
    for (auto* b : { &exportButton, &deleteButton }) b->setEnabled (has);
    const bool isLoaded = has && lib.current() == lib.loaded();
    loadButton.setButtonText (isLoaded ? "Loaded" : "Load");
    loadButton.setEnabled (has && ! isLoaded);
    list.repaint();
    repaint();
}

void MeasurementList::selectedRowsChanged (int row)
{
    if (row >= 0 && row != lib.currentIndex()) lib.select (row);
}

void MeasurementList::listBoxItemClicked (int row, const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu() || ! juce::isPositiveAndBelow (row, lib.size())) return;
    if (row != lib.currentIndex()) lib.select (row);
    auto* m = lib.items()[(size_t) row].get();
    juce::Component::SafePointer<MeasurementList> safe (this);
    juce::PopupMenu menu;
    menu.addItem ("Rename", [safe, row] { if (safe != nullptr) safe->startRename (row); });
    juce::PopupMenu cal;
    cal.addItem ("None (flat microphone)", true, m->calibration.empty(), [safe, m] { if (safe != nullptr) safe->lib.setCalibration (m, {}); });
    for (auto& c : calibrations::all())
        cal.addItem (juce::String (c.name), true, c.name == m->calibration.name && ! m->calibration.empty(),
                     [safe, m, c] { if (safe != nullptr) safe->lib.setCalibration (m, c); });
    menu.addSubMenu ("Mic calibration", cal);
    menu.addSeparator();
    menu.addItem ("Delete", [safe] { if (safe != nullptr) safe->deleteSelected(); });
    menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
}

void MeasurementList::importFile()
{
    chooser = std::make_unique<juce::FileChooser> ("Import room measurement", juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
                                                   juce::String ("*") + measurements::kExtension);
    juce::Component::SafePointer<MeasurementList> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc)
    {
        const auto f = fc.getResult();
        if (safe == nullptr || ! f.existsAsFile()) return;
        juce::String err;
        auto m = measurements::load (f, err);
        if (m == nullptr)
            return dialog::message (safe.getComponent(), dialog::Kind::warning, "Cannot import", f.getFileName() + ": " + err);
        m->file = juce::File();   // a copy in the library
        measurements::saveToLibrary (*m);
        safe->lib.add (std::move (m));
    });
}

void MeasurementList::showExportMenu()
{
    auto* m = lib.current();
    if (m == nullptr) return;
    juce::Component::SafePointer<MeasurementList> safe (this);
    juce::PopupMenu menu;
    menu.addItem ("Measurement file (.hydrameasure)...", [safe, m = *m]
    {
        if (safe != nullptr)
            saveAs (safe.getComponent(), safe->chooser, "Export measurement", m.name, juce::String ("*") + measurements::kExtension,
                    [m] (const juce::File& f) { return measurements::save (m, f.withFileExtension (measurements::kExtension)); });
    });
    for (int k = 0; k < m->sides(); ++k)
    {
        const auto& side = *m->side (k);
        const juce::String which = m->sides() == 1 ? juce::String() : k == 0 ? juce::String (" - left") : juce::String (" - right");
        const auto base = m->name + (m->sides() == 1 ? juce::String() : k == 0 ? juce::String (" L") : juce::String (" R"));
        menu.addItem ("Frequency response" + which + " (.txt, for REW)...", side.analysed, false, [safe, m = side, base]
        {
            if (safe != nullptr)
                saveAs (safe.getComponent(), safe->chooser, "Export frequency response", base, "*.txt",
                        [m] (const juce::File& f) { return measurements::exportResponse (m, f.withFileExtension (".txt"), 12.0); });
        });
        menu.addItem ("Impulse response" + which + " (.wav)...", [safe, m = side, base]
        {
            if (safe != nullptr)
                saveAs (safe.getComponent(), safe->chooser, "Export impulse response", base, "*.wav",
                        [m] (const juce::File& f) { return measurements::exportImpulse (m, f.withFileExtension (".wav")); });
        });
    }
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&exportButton).withMinimumWidth (260));
}

void MeasurementList::deleteSelected()
{
    auto* m = lib.current();
    if (m == nullptr) return;
    juce::Component::SafePointer<MeasurementList> safe (this);
    dialog::confirm (this, dialog::Kind::danger, "Delete measurement?", "\"" + m->name + "\" is removed from the library.", "Delete", [safe, m]
    {
        if (safe != nullptr) safe->lib.remove (m);
    });
}

void MeasurementList::startRename (int row)
{
    if (! juce::isPositiveAndBelow (row, lib.size())) return;
    finishRename (true);
    renameRow = row;
    renameEditor = std::make_unique<juce::TextEditor>();
    auto& ed = *renameEditor;
    addAndMakeVisible (ed);
    ed.setFont (theme::font (13.5f, true));
    ed.setText (lib.items()[(size_t) row]->name, false);
    const auto r = list.getRowPosition (row, true).translated (list.getX(), list.getY());
    ed.setBounds (r.withHeight (24).reduced (6, 0).translated (0, 4));
    ed.onReturnKey = [this] { finishRename (true); };
    ed.onEscapeKey = [this] { finishRename (false); };
    ed.onFocusLost = [this] { finishRename (true); };
    ed.grabKeyboardFocus();
    ed.selectAll();
}

void MeasurementList::finishRename (bool apply)
{
    if (renameEditor == nullptr || renameRow < 0) return;
    const int row = renameRow;
    renameRow = -1;
    const auto text = renameEditor->getText().trim();
    renameEditor->setVisible (false);
    // Deleted later: this runs inside the editor's own callbacks.
    juce::Component::SafePointer<MeasurementList> safe (this);
    juce::MessageManager::callAsync ([safe] { if (safe != nullptr && safe->renameRow < 0) safe->renameEditor.reset(); });
    if (apply && juce::isPositiveAndBelow (row, lib.size()))
        lib.rename (lib.items()[(size_t) row].get(), text);
}

void MeasurementList::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (! juce::isPositiveAndBelow (row, lib.size())) return;
    const auto& m = *lib.items()[(size_t) row];
    auto r = juce::Rectangle<int> (0, 0, w, h).reduced (2, 2);
    if (selected)
    {
        g.setColour (theme::accentIn.withAlpha (0.14f));
        g.fillRoundedRectangle (r.toFloat(), 8.0f);
        g.setColour (theme::accentIn);
        g.fillRoundedRectangle (r.removeFromLeft (3).toFloat().reduced (0.0f, 8.0f), 1.5f);
    }
    else
        r.removeFromLeft (3);
    r.reduce (10, 4);
    // Right of the name: which speakers, and "loaded" for the one the correction comes from.
    auto top = r.removeFromTop (20);
    auto pill = [&g, &top] (const juce::String& text, juce::Colour c)
    {
        g.setFont (theme::font (10.5f, true));
        const float w = (float) juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), text) + 10.0f;
        auto b = top.removeFromRight ((int) w).toFloat().withSizeKeepingCentre (w, 16.0f);
        top.removeFromRight (4);
        g.setColour (c.withAlpha (0.2f));
        g.fillRoundedRectangle (b, 4.0f);
        g.setColour (c);
        g.drawText (text, b, juce::Justification::centred);
    };
    if (&m == lib.loaded()) pill ("loaded", theme::good);
    if (m.sides() == 2)     pill ("L + R", theme::accentVirt);
    else if (m.speakers != 3) pill (m.speakers == 1 ? "L" : "R", m.speakers == 1 ? theme::accentIn : theme::accentVirt);
    g.setColour (selected ? theme::text : theme::text.withAlpha (0.9f));
    g.setFont (theme::font (13.5f, true));
    g.drawText (m.name, top, juce::Justification::centredLeft, true);
    g.setColour (theme::textFaint);
    g.setFont (theme::font (11.5f));
    juce::String info = m.created.formatted ("%Y-%m-%d %H:%M");
    if (m.numPositions() > 1) info << dot() << m.numPositions() << " positions";
    if (m.throughEq) info << dot() << "through correction";
    if (! m.calibration.empty()) info << dot() << "cal";
    g.drawText (info, r, juce::Justification::centredLeft, true);
}

void MeasurementList::paint (juce::Graphics& g)
{
    g.setColour (theme::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 10.0f);
    auto r = getLocalBounds().reduced (kPad, 12);
    auto title = r.removeFromTop (22);
    g.setColour (theme::text);
    g.setFont (theme::font (15.0f, true));
    g.drawText ("Measurements", title, juce::Justification::centredLeft);
    g.setColour (theme::textFaint);
    g.setFont (theme::font (12.0f));
    if (lib.size() > 0) g.drawText (juce::String (lib.size()), title, juce::Justification::centredRight);
    if (lib.size() == 0)
    {
        g.setColour (theme::textDim);
        g.setFont (theme::font (13.0f));
        g.drawFittedText (onCalibrate != nullptr ? "No measurements yet.\n\n\"Calibrate speakers\" measures your room step by step and keeps the measurement here."
                                                 : "No measurements yet.\n\nMeasure your speakers in Hydra Mixer (Tools > Room Correction > Calibrate): "
                                                   "its measurements show up here.",
                          emptyArea, juce::Justification::centredTop, 6);
    }
}

void MeasurementList::resized()
{
    auto r = getLocalBounds().reduced (kPad, 12);
    r.removeFromTop (22 + 8);
    calibrateButton.setVisible (onCalibrate != nullptr);
    if (onCalibrate != nullptr)
    {
        calibrateButton.setBounds (r.removeFromTop (38));
        r.removeFromTop (12);
    }
    auto bottom = r.removeFromBottom (28);
    const int w = (bottom.getWidth() - 2 * kGap) / 3;
    importButton.setBounds (bottom.removeFromLeft (w));
    bottom.removeFromLeft (kGap);
    exportButton.setBounds (bottom.removeFromLeft (w));
    bottom.removeFromLeft (kGap);
    deleteButton.setBounds (bottom);
    r.removeFromBottom (kGap);
    loadButton.setBounds (r.removeFromBottom (34));
    r.removeFromBottom (12);
    list.setBounds (r.expanded (6, 0));
    emptyArea = r.withTrimmedTop (20).withHeight (120);
}

//==============================================================================
PoolList::PoolList (juce::String app) : madeIn (std::move (app))
{
    list.setRowHeight (48);
    list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    list.setOutlineThickness (0);
    list.setTooltip ("Corrections made in Hydra Mixer or in the Room Correction plugin, shared by both. "
                     "Double click to rename, right click for more.");
    addAndMakeVisible (list);
    for (auto* b : { &useButton, &deleteButton }) addAndMakeVisible (*b);
    useButton.setTooltip ("Puts the selected correction here. It stays linked: when it is changed (here, in the other app or "
                          "in another plugin instance), this follows.");
    deleteButton.setTooltip ("Removes the selected correction from the saved ones. Where it is in use, it stays active (unlinked).");
    useButton.onClick = [this] { useSelected(); };
    deleteButton.onClick = [this] { deleteSelected(); };
    refresh (true);
    startTimer (1000);
}

PoolList::~PoolList() { stopTimer(); }

void PoolList::setTarget (CorrectionTarget* t)
{
    target = t;
    updateButtons();
    list.repaint();
}

void PoolList::timerCallback()
{
    refresh (false);
    // The "in use" mark follows the target.
    const auto linked = target != nullptr ? target->getProfile().poolId : juce::String();
    if (linked != shownLinked)
    {
        shownLinked = linked;
        updateButtons();
        list.repaint();
    }
}

void PoolList::refresh (bool force)
{
    juce::String sig;
    for (const auto& f : pool::folder().findChildFiles (juce::File::findFiles, false, juce::String ("*") + pool::kExtension))
        sig << f.getFileName() << f.getLastModificationTime().toMilliseconds() << "|";
    if (! force && sig == signature) return;
    signature = sig;
    const auto keep = selectedEntry() != nullptr ? selectedEntry()->id : juce::String();
    entries = pool::list();
    list.updateContent();
    list.deselectAllRows();
    for (int i = 0; i < (int) entries.size(); ++i)
        if (entries[(size_t) i].id == keep) list.selectRow (i, false, true);
    updateButtons();
    list.repaint();
    repaint();
}

const pool::Entry* PoolList::selectedEntry() const
{
    const int row = list.getSelectedRow();
    return juce::isPositiveAndBelow (row, (int) entries.size()) ? &entries[(size_t) row] : nullptr;
}

void PoolList::updateButtons()
{
    const auto* e = selectedEntry();
    const bool inUse = e != nullptr && target != nullptr && target->getProfile().poolId == e->id;
    useButton.setButtonText (inUse ? "In use" : "Use");
    useButton.setEnabled (e != nullptr && target != nullptr && ! inUse);
    deleteButton.setEnabled (e != nullptr);
}

void PoolList::useSelected()
{
    const auto* e = selectedEntry();
    if (e == nullptr || target == nullptr) return;
    target->setProfile (e->profile);
    target->setOn (true);
    updateButtons();
    list.repaint();
    if (onUse) onUse();
}

void PoolList::deleteSelected()
{
    const auto* e = selectedEntry();
    if (e == nullptr) return;
    juce::Component::SafePointer<PoolList> safe (this);
    dialog::confirm (this, dialog::Kind::danger, "Delete correction?",
                     "\"" + e->profile.name + "\" is removed from the saved corrections (Hydra Mixer and plugin). Where it is in use, "
                     "it stays active but is no longer linked.", "Delete", [safe, id = e->id]
    {
        if (safe == nullptr) return;
        pool::remove (id);
        safe->refresh (true);
    });
}

void PoolList::listBoxItemClicked (int row, const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu() || ! juce::isPositiveAndBelow (row, (int) entries.size())) return;
    list.selectRow (row);
    juce::Component::SafePointer<PoolList> safe (this);
    juce::PopupMenu menu;
    menu.addItem ("Use here", target != nullptr, false, [safe] { if (safe != nullptr) safe->useSelected(); });
    menu.addItem ("Rename", [safe, row] { if (safe != nullptr) safe->startRename (row); });
    menu.addItem ("Duplicate", [safe, entry = entries[(size_t) row]]
    {
        if (safe == nullptr) return;
        auto p = entry.profile;
        p.poolId = pool::newId();
        p.name << " (copy)";
        pool::write (p, safe->madeIn);
        safe->refresh (true);
    });
    menu.addSeparator();
    menu.addItem ("Delete", [safe] { if (safe != nullptr) safe->deleteSelected(); });
    menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
}

void PoolList::startRename (int row)
{
    if (! juce::isPositiveAndBelow (row, (int) entries.size())) return;
    finishRename (true);
    renameRow = row;
    renameEditor = std::make_unique<juce::TextEditor>();
    auto& ed = *renameEditor;
    addAndMakeVisible (ed);
    ed.setFont (theme::font (13.5f, true));
    ed.setText (entries[(size_t) row].profile.name, false);
    const auto r = list.getRowPosition (row, true).translated (list.getX(), list.getY());
    ed.setBounds (r.withHeight (24).reduced (6, 0).translated (0, 4));
    ed.onReturnKey = [this] { finishRename (true); };
    ed.onEscapeKey = [this] { finishRename (false); };
    ed.onFocusLost = [this] { finishRename (true); };
    ed.grabKeyboardFocus();
    ed.selectAll();
}

void PoolList::finishRename (bool apply)
{
    if (renameEditor == nullptr || renameRow < 0) return;
    const int row = renameRow;
    renameRow = -1;
    const auto text = renameEditor->getText().trim();
    renameEditor->setVisible (false);
    // Deleted later: this runs inside the editor's own callbacks.
    juce::Component::SafePointer<PoolList> safe (this);
    juce::MessageManager::callAsync ([safe] { if (safe != nullptr && safe->renameRow < 0) safe->renameEditor.reset(); });
    if (apply && text.isNotEmpty() && juce::isPositiveAndBelow (row, (int) entries.size()) && text != entries[(size_t) row].profile.name)
    {
        // Linked targets take the new name from the pool.
        pool::rename (entries[(size_t) row].id, text, madeIn);
        refresh (true);
    }
}

void PoolList::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (! juce::isPositiveAndBelow (row, (int) entries.size())) return;
    const auto& e = entries[(size_t) row];
    auto r = juce::Rectangle<int> (0, 0, w, h).reduced (2, 2);
    if (selected)
    {
        g.setColour (theme::accentIn.withAlpha (0.14f));
        g.fillRoundedRectangle (r.toFloat(), 8.0f);
        g.setColour (theme::accentIn);
        g.fillRoundedRectangle (r.removeFromLeft (3).toFloat().reduced (0.0f, 8.0f), 1.5f);
    }
    else
        r.removeFromLeft (3);
    r.reduce (10, 4);
    auto top = r.removeFromTop (20);
    auto pill = [&g, &top] (const juce::String& text, juce::Colour c)
    {
        g.setFont (theme::font (10.5f, true));
        const float pw = (float) juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), text) + 10.0f;
        auto b = top.removeFromRight ((int) pw).toFloat().withSizeKeepingCentre (pw, 16.0f);
        top.removeFromRight (4);
        g.setColour (c.withAlpha (0.2f));
        g.fillRoundedRectangle (b, 4.0f);
        g.setColour (c);
        g.drawText (text, b, juce::Justification::centred);
    };
    if (target != nullptr && target->getProfile().poolId == e.id) pill ("in use", theme::good);
    if (e.profile.separate) pill ("L + R", theme::accentVirt);
    g.setColour (selected ? theme::text : theme::text.withAlpha (0.9f));
    g.setFont (theme::font (13.5f, true));
    g.drawText (e.profile.name.isNotEmpty() ? e.profile.name : juce::String ("Unnamed"), top, juce::Justification::centredLeft, true);
    g.setColour (theme::textFaint);
    g.setFont (theme::font (11.5f));
    const int bands = (int) e.profile.bands.size() + (e.profile.separate ? (int) e.profile.right.bands.size() : 0);
    juce::String info = e.modified.formatted ("%Y-%m-%d %H:%M");
    info << dot() << bands << (bands == 1 ? " band" : " bands");
    if (e.madeIn == pool::kPlugin) info << dot() << "plugin";
    g.drawText (info, r, juce::Justification::centredLeft, true);
}

void PoolList::paint (juce::Graphics& g)
{
    g.setColour (theme::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 10.0f);
    auto r = getLocalBounds().reduced (kPad, 12);
    auto title = r.removeFromTop (22);
    g.setColour (theme::text);
    g.setFont (theme::font (15.0f, true));
    g.drawText ("Saved corrections", title, juce::Justification::centredLeft);
    g.setColour (theme::textFaint);
    g.setFont (theme::font (12.0f));
    if (! entries.empty()) g.drawText (juce::String ((int) entries.size()), title, juce::Justification::centredRight);
    if (entries.empty())
    {
        g.setColour (theme::textDim);
        g.setFont (theme::font (13.0f));
        g.drawFittedText ("No corrections yet.\n\nEvery correction made from a measurement is kept here, for Hydra Mixer and the "
                          "Room Correction plugin.", emptyArea, juce::Justification::centredTop, 6);
    }
}

void PoolList::resized()
{
    auto r = getLocalBounds().reduced (kPad, 12);
    r.removeFromTop (22 + 8);
    auto bottom = r.removeFromBottom (30);
    const int w = (bottom.getWidth() - kGap) / 2;
    useButton.setBounds (bottom.removeFromLeft (w));
    bottom.removeFromLeft (kGap);
    deleteButton.setBounds (bottom);
    r.removeFromBottom (kGap);
    list.setBounds (r.expanded (6, 0));
    emptyArea = r.withTrimmedTop (10).withHeight (100);
}

//==============================================================================
class AnalysisPage::Chart final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit Chart (AnalysisPage& p) : page (p) {}

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        g.setColour (theme::panel);
        g.fillRoundedRectangle (b, 10.0f);
        auto* m = page.shown();
        if (m == nullptr) return;
        if (! m->analysed) return empty (g, "Analysing...");
        if ((page.mode == Mode::waterfall || page.mode == Mode::decay || page.mode == Mode::impulse) && ! RoomLibrary::isComplete (*m))
            return empty (g, "Analysing...");
        switch (page.mode)
        {
            case Mode::response:  paintResponse (g, *m); break;
            case Mode::waterfall: paintWaterfall (g, *m); break;
            case Mode::decay:     paintDecay (g, *m); break;
            case Mode::impulse:   paintImpulse (g, *m); break;
            case Mode::positions: paintPositions (g); break;
        }
    }

    void mouseMove (const juce::MouseEvent& e) override { hover = e.position; repaint(); }
    void mouseExit (const juce::MouseEvent&) override   { hover = { -1.0f, -1.0f }; repaint(); }
    void mouseDown (const juce::MouseEvent&) override   { dragStart = tilt; }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (page.mode != Mode::waterfall) return;
        tilt.x = juce::jlimit (0.0f, 0.45f, dragStart.x - (float) e.getDistanceFromDragStartX() / (float) juce::jmax (1, getWidth()));
        tilt.y = juce::jlimit (0.0f, 0.6f, dragStart.y - (float) e.getDistanceFromDragStartY() / (float) juce::jmax (1, getHeight()));
        repaint();
    }
    void mouseDoubleClick (const juce::MouseEvent&) override { tilt = { 0.22f, 0.40f }; repaint(); }

private:
    void empty (juce::Graphics& g, const juce::String& text)
    {
        g.setColour (theme::textDim);
        g.setFont (theme::font (15.0f));
        g.drawFittedText (text, getLocalBounds().reduced (30), juce::Justification::centred, 3);
    }

    /** Path of a curve on the log frequency axis between lo and hi. */
    static juce::Path curve (const std::vector<double>& freqs, const std::vector<double>& db, double lo, double hi,
                             const std::function<float (double)>& fx, const std::function<float (double)>& fy)
    {
        juce::Path p;
        bool started = false;
        for (size_t i = 0; i < freqs.size() && i < db.size(); ++i)
        {
            if (freqs[i] < lo || freqs[i] > hi) continue;
            const juce::Point<float> pt (fx (freqs[i]), fy (db[i]));
            if (! started) { p.startNewSubPath (pt); started = true; }
            else           p.lineTo (pt);
        }
        return p;
    }

    double smoothing() const { return valueOf (page.viewOption, kSmoothing, 12.0); }

    void paintResponse (juce::Graphics& g, const RoomMeasurement& m)
    {
        const auto& a = m.analysis;
        const auto plot = getLocalBounds().toFloat().withTrimmedLeft (50.0f).withTrimmedRight (16.0f).withTrimmedTop (40.0f).withTrimmedBottom (26.0f);
        constexpr double lo = 20.0, hi = 20000.0, top = 18.0, bottom = -30.0;
        auto fx = [&] (double f) { return plot.getX() + plot.getWidth() * (float) (std::log (f / lo) / std::log (hi / lo)); };
        auto fy = [&] (double db) { return plot.getY() + plot.getHeight() * (float) ((top - juce::jlimit (bottom, top, db)) / (top - bottom)); };

        drawFreqGrid (g, plot, lo, hi, true);
        g.setFont (theme::font (11.0f));
        for (double db = bottom; db <= top + 0.1; db += 6.0)
        {
            g.setColour (theme::outline.withAlpha (db == 0.0 ? 1.0f : 0.45f));
            g.drawHorizontalLine (juce::roundToInt (fy (db)), plot.getX(), plot.getRight());
            g.setColour (theme::textFaint);
            g.drawText ((db > 0 ? "+" : "") + juce::String (juce::roundToInt (db)) + " dB",
                        juce::Rectangle<float> (0.0f, fy (db) - 7.0f, plot.getX() - 6.0f, 14.0f), juce::Justification::centredRight);
        }

        const double ref = a.referenceDb, sm = smoothing();
        const double fLo = a.sweepStartHz, fHi = a.sweepEndHz;
        auto relative = [ref] (std::vector<double> v) { for (auto& x : v) x -= ref; return v; };
        const auto smoothed = relative (room::smooth (a.magDb, sm));

        g.saveState();
        g.reduceClipRegion (plot.expanded (1.0f).toNearestInt());
        auto stroke = [&] (const juce::Path& p, juce::Colour c, float w)
        {
            g.setColour (c);
            g.strokePath (p, juce::PathStrokeType (w, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        };

        std::vector<std::pair<juce::String, juce::Colour>> legend { { "Response", theme::accentIn } };
        if (! a.positionDb.empty())   // multi-point: every position thin, the average on top
        {
            for (auto& pos : a.positionDb)
                stroke (curve (a.freqs, relative (room::smooth (pos, sm)), fLo, fHi, fx, fy), theme::textFaint.withAlpha (0.45f), 1.0f);
            legend.push_back ({ juce::String ((int) a.positionDb.size()) + " positions", theme::textFaint });
        }
        else if (sm > 0.0)
            stroke (curve (a.freqs, relative (a.magDb), fLo, fHi, fx, fy), theme::textFaint.withAlpha (0.5f), 1.0f);
        const auto* c = page.lib.indexOf (page.comparePtr) >= 0 && page.comparePtr != page.lib.loaded() ? page.comparePtr : nullptr;
        if (c != nullptr && c->analysed)
        {
            // Relative to this measurement's reference: before / after shows the real level difference.
            auto other = room::smooth (c->analysis.magDb, sm);
            for (auto& x : other) x -= ref;
            stroke (curve (c->analysis.freqs, other, c->analysis.sweepStartHz, c->analysis.sweepEndHz, fx, fy), theme::accentVirt.withAlpha (0.85f), 1.6f);
            legend.push_back ({ c->name, theme::accentVirt });
        }
        if (! a.nearDb.empty() && a.nearDb.size() == a.freqs.size())
        {
            // The speaker alone (near field), moved to the response's level between 500 Hz and 5 kHz.
            double sum = 0.0;
            int count = 0;
            for (size_t i = 0; i < a.freqs.size(); ++i)
                if (a.freqs[i] >= 500.0 && a.freqs[i] <= 5000.0) { sum += smoothed[i] - a.nearDb[i]; ++count; }
            auto near = room::smooth (a.nearDb, sm > 0.0 ? sm : 24.0);
            for (auto& x : near) x += count > 0 ? sum / count : -ref;
            stroke (curve (a.freqs, near, juce::jmax (fLo, RoomMeasurement::kNearSweepStartHz * 1.5), fHi, fx, fy), theme::accentVirt.withAlpha (0.8f), 1.4f);
            legend.push_back ({ "Speaker alone (near field)", theme::accentVirt });
        }
        // The response with a soft fill down to the bottom.
        auto main = curve (a.freqs, smoothed, fLo, fHi, fx, fy);
        juce::Path area (main);
        area.lineTo (fx (fHi), plot.getBottom());
        area.lineTo (fx (fLo), plot.getBottom());
        area.closeSubPath();
        g.setGradientFill (juce::ColourGradient (theme::accentIn.withAlpha (0.14f), 0.0f, plot.getY(), theme::accentIn.withAlpha (0.0f), 0.0f, plot.getBottom(), false));
        g.fillPath (area);
        stroke (main, theme::accentIn, 2.2f);

        // Resonances: marker above the peak, frequency on the strongest ones.
        const auto fine = relative (room::smooth (a.magDb, 12.0));
        g.setFont (theme::font (11.0f, true));
        for (size_t i = 0; i < a.resonances.size(); ++i)
        {
            const auto& r = a.resonances[i];
            const float x = fx (r.freq), y = fy (room::valueAt (a.freqs, fine, r.freq)) - 7.0f;
            juce::Path tri;
            tri.addTriangle (x - 4.5f, y - 7.0f, x + 4.5f, y - 7.0f, x, y);
            g.setColour (theme::mute.withAlpha (i < 6 ? 1.0f : 0.6f));
            g.fillPath (tri);
            if (i < 6)
                g.drawText (hz (r.freq), juce::Rectangle<float> (x - 30.0f, y - 21.0f, 60.0f, 13.0f), juce::Justification::centred);
        }
        if (! a.resonances.empty()) legend.push_back ({ "Room resonance", theme::mute });

        if (plot.contains (hover))
        {
            const double f = lo * std::pow (hi / lo, (double) ((hover.x - plot.getX()) / plot.getWidth()));
            juce::String t = hz (f) + "   " + juce::String (room::valueAt (a.freqs, smoothed, f), 1) + " dB";
            if (c != nullptr && c->analysed)
                t << "   " << c->name.substring (0, 18) << " " << juce::String (room::valueAt (c->analysis.freqs, room::smooth (c->analysis.magDb, sm), f) - ref, 1) << " dB";
            drawReadout (g, plot, hover.x, t);
        }
        g.restoreState();
        drawLegend (g, getLocalBounds().withTrimmedRight (16).removeFromTop (34), legend);
    }

    void paintPositions (juce::Graphics& g)
    {
        const auto* m = page.lib.loaded();
        const auto lay = m != nullptr ? room::locate (m->geometry) : room::Layout();
        if (! lay.valid)
            return empty (g, m != nullptr && ! m->geometry.empty()
                                 ? "The distance between the speakers is missing: the other speaker was not heard in the near-field step."
                                 : "Measured without the near-field step, so the positions are unknown.\nCalibrate again with the near-field "
                                   "step (left and right separately) to see where speakers and microphone stand.");
        LayoutPicture pic;
        pic.layout = &lay;
        drawSpeakerLayout (g, getLocalBounds().toFloat().reduced (20.0f, 16.0f), pic);
    }

    void paintWaterfall (juce::Graphics& g, const RoomMeasurement& m)
    {
        const auto& a = m.analysis;
        const int slices = (int) a.waterfall.size();
        if (slices < 2) return empty (g, "No waterfall data.");
        const double lo = juce::jmax (page.wfLo, a.sweepStartHz), hi = juce::jmin (page.wfHi, a.sweepEndHz);
        if (hi <= lo * 1.05) return empty (g, "The chosen range lies outside the measured sweep.");
        constexpr double top = 3.0, floorDb = -45.0;

        const auto area = getLocalBounds().toFloat().withTrimmedLeft (52.0f).withTrimmedRight (70.0f).withTrimmedTop (24.0f).withTrimmedBottom (30.0f);
        const float ox = tilt.x * area.getWidth(), oy = tilt.y * area.getHeight();
        const auto front = juce::Rectangle<float> (area.getX(), area.getY() + oy, area.getWidth() - ox, area.getHeight() - oy);
        auto fx = [&] (double f) { return front.getX() + front.getWidth() * (float) (std::log (f / lo) / std::log (hi / lo)); };
        auto fy = [&] (double db) { return front.getY() + front.getHeight() * (float) ((top - juce::jlimit (floorDb, top, db)) / (top - floorDb)); };
        auto shift = [&] (int s) { const float d = 1.0f - (float) s / (float) (slices - 1); return juce::Point<float> (ox * d, -oy * d); };

        // Back wall: level lines of the first (back) slice, and the floor.
        const auto back = shift (0);
        g.setFont (theme::font (11.0f));
        for (double db = 0.0; db >= floorDb; db -= 15.0)
        {
            g.setColour (theme::outline.withAlpha (0.6f));
            g.drawLine (front.getX() + back.x, fy (db) + back.y, front.getRight() + back.x, fy (db) + back.y, 1.0f);
            g.drawLine (front.getX(), fy (db), front.getX() + back.x, fy (db) + back.y, 0.8f);
            g.setColour (theme::textFaint);
            g.drawText (juce::String (juce::roundToInt (db)) + " dB", juce::Rectangle<float> (0.0f, fy (db) - 7.0f, front.getX() - 6.0f, 14.0f),
                        juce::Justification::centredRight);
        }
        juce::Path floorPath;
        floorPath.addQuadrilateral (front.getX(), front.getBottom(), front.getRight(), front.getBottom(),
                                    front.getRight() + back.x, front.getBottom() + back.y, front.getX() + back.x, front.getBottom() + back.y);
        g.setColour (theme::panel2);
        g.fillPath (floorPath);

        // Slices from the back (0 ms) to the front (latest), each hiding what lies behind it.
        for (int s = 0; s < slices; ++s)
        {
            const auto& v = a.waterfall[(size_t) s];
            const auto o = shift (s);
            juce::Path p, edge;
            p.startNewSubPath (fx (lo) + o.x, front.getBottom() + o.y);
            bool started = false;
            float topY = front.getBottom();
            for (size_t i = 0; i < a.freqs.size(); ++i)
            {
                if (a.freqs[i] < lo || a.freqs[i] > hi) continue;
                const juce::Point<float> pt (fx (a.freqs[i]) + o.x, fy (v[i]) + o.y);
                p.lineTo (pt);
                if (! started) { edge.startNewSubPath (pt); started = true; }
                else           edge.lineTo (pt);
                topY = juce::jmin (topY, pt.y);
            }
            p.lineTo (fx (hi) + o.x, front.getBottom() + o.y);
            p.closeSubPath();

            const auto c = timeColour ((float) s / (float) (slices - 1));
            g.setGradientFill (juce::ColourGradient (c.withMultipliedBrightness (0.62f), 0.0f, topY,
                                                     theme::panel2.interpolatedWith (c, 0.12f), 0.0f, front.getBottom() + o.y, false));
            g.fillPath (p);
            g.setColour (c.withAlpha (0.95f));
            g.strokePath (edge, juce::PathStrokeType (s == slices - 1 ? 1.6f : 1.1f));
        }

        // Axes: frequency along the front, time along the right side.
        drawFreqGrid (g, juce::Rectangle<float> (front.getX(), front.getBottom(), front.getWidth(), 0.0f), lo, hi, true);
        g.setColour (theme::textFaint);
        g.setFont (theme::font (11.0f));
        const double span = a.waterfallMs.back();
        const double step = span > 600.0 ? 100.0 : span > 250.0 ? 50.0 : span > 100.0 ? 20.0 : 10.0;
        for (double t = 0.0; t <= span + 0.1; t += step)
        {
            const float d = 1.0f - (float) (t / span);
            const juce::Point<float> pt (front.getRight() + ox * d, front.getBottom() - oy * d);
            g.drawText (juce::String (juce::roundToInt (t)) + " ms", juce::Rectangle<float> (pt.x + 6.0f, pt.y - 7.0f, 60.0f, 14.0f),
                        juce::Justification::centredLeft);
        }
    }

    void paintDecay (juce::Graphics& g, const RoomMeasurement& m)
    {
        const auto& bands = m.analysis.decay;
        const auto plot = getLocalBounds().toFloat().withTrimmedLeft (52.0f).withTrimmedRight (20.0f).withTrimmedTop (44.0f).withTrimmedBottom (40.0f);
        double maxT = 0.8;
        for (auto& d : bands) maxT = juce::jmax (maxT, d.t20 * 1.15, d.edt * 1.15);
        maxT = std::ceil (maxT * 5.0) / 5.0;
        auto fy = [&] (double t) { return plot.getBottom() - plot.getHeight() * (float) (juce::jlimit (0.0, maxT, t) / maxT); };

        // Typical range for living rooms, home studios and control rooms.
        g.setColour (theme::good.withAlpha (0.07f));
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (plot.getX(), fy (0.5), plot.getRight(), fy (0.2)));

        g.setFont (theme::font (11.0f));
        for (double t = 0.0; t <= maxT + 0.001; t += maxT > 1.6 ? 0.4 : 0.2)
        {
            g.setColour (theme::outline.withAlpha (0.6f));
            g.drawHorizontalLine (juce::roundToInt (fy (t)), plot.getX(), plot.getRight());
            g.setColour (theme::textFaint);
            g.drawText (juce::String (t, 1) + " s", juce::Rectangle<float> (0.0f, fy (t) - 7.0f, plot.getX() - 6.0f, 14.0f), juce::Justification::centredRight);
        }

        const float slot = plot.getWidth() / (float) juce::jmax (1, (int) bands.size());
        for (size_t i = 0; i < bands.size(); ++i)
        {
            const auto& d = bands[i];
            const float x = plot.getX() + slot * (float) i;
            auto col = juce::Rectangle<float> (x, plot.getY(), slot, plot.getHeight()).reduced (slot * 0.22f, 0.0f);
            g.setColour (theme::textDim);
            g.setFont (theme::font (12.0f, true));
            g.drawText (shortHz (d.centreHz) + " Hz", juce::Rectangle<float> (x, plot.getBottom() + 6.0f, slot, 16.0f), juce::Justification::centred);

            const double t = d.t20 > 0.0 ? d.t20 : d.t30;
            if (t <= 0.0)
            {
                g.setColour (theme::textFaint);
                g.setFont (theme::font (11.0f));
                g.drawText ("-", col.withTop (plot.getBottom() - 20.0f), juce::Justification::centred);
                continue;
            }
            const auto bar = col.withTop (fy (t));
            g.setGradientFill (juce::ColourGradient (theme::accentIn, 0.0f, bar.getY(), theme::accentIn.withAlpha (0.35f), 0.0f, bar.getBottom(), false));
            g.fillRoundedRectangle (bar, 4.0f);
            if (d.edt > 0.0)
            {
                g.setColour (theme::warn);
                g.fillRect (col.getX() - 4.0f, fy (d.edt) - 1.5f, col.getWidth() + 8.0f, 3.0f);
            }
            g.setColour (theme::text);
            g.setFont (theme::font (12.0f, true));
            g.drawText (juce::String (t, 2) + " s", juce::Rectangle<float> (x, bar.getY() - 18.0f, slot, 16.0f), juce::Justification::centred);
        }
        drawLegend (g, getLocalBounds().withTrimmedRight (16).removeFromTop (34),
                    { { "Reverberation time (T20)", theme::accentIn }, { "Early decay (EDT)", theme::warn },
                      { "Typical 0.2 - 0.5 s", theme::good.withAlpha (0.3f) } });
    }

    void paintImpulse (juce::Graphics& g, const RoomMeasurement& m)
    {
        const auto& a = m.analysis;
        const auto plot = getLocalBounds().toFloat().withTrimmedLeft (52.0f).withTrimmedRight (20.0f).withTrimmedTop (24.0f).withTrimmedBottom (26.0f);
        const double t0 = -5.0, t1 = valueOf (page.viewOption, kSpans, 300.0), bottom = -80.0;
        auto fx = [&] (double t) { return plot.getX() + plot.getWidth() * (float) ((t - t0) / (t1 - t0)); };
        auto fy = [&] (double db) { return plot.getY() + plot.getHeight() * (float) (-juce::jlimit (bottom, 0.0, db) / -bottom); };

        g.setFont (theme::font (11.0f));
        for (double db = 0.0; db >= bottom; db -= 10.0)
        {
            g.setColour (theme::outline.withAlpha (0.5f));
            g.drawHorizontalLine (juce::roundToInt (fy (db)), plot.getX(), plot.getRight());
            g.setColour (theme::textFaint);
            g.drawText (juce::String (juce::roundToInt (db)) + " dB", juce::Rectangle<float> (0.0f, fy (db) - 7.0f, plot.getX() - 6.0f, 14.0f), juce::Justification::centredRight);
        }
        const double step = t1 <= 60.0 ? 5.0 : t1 <= 120.0 ? 10.0 : t1 <= 400.0 ? 50.0 : 100.0;
        for (double t = 0.0; t <= t1; t += step)
        {
            g.setColour (theme::outline.withAlpha (t == 0.0 ? 1.0f : 0.5f));
            g.drawVerticalLine (juce::roundToInt (fx (t)), plot.getY(), plot.getBottom());
            g.setColour (theme::textFaint);
            g.drawText (juce::String (juce::roundToInt (t)) + " ms", juce::Rectangle<float> (fx (t) - 30.0f, plot.getBottom() + 4.0f, 60.0f, 14.0f), juce::Justification::centred);
        }

        juce::Path p;
        bool started = false;
        for (size_t i = 0; i < a.etcMs.size(); ++i)
        {
            if (a.etcMs[i] < t0 || a.etcMs[i] > t1) continue;
            const juce::Point<float> pt (fx (a.etcMs[i]), fy (a.etcDb[i]));
            if (! started) { p.startNewSubPath (pt); started = true; }
            else           p.lineTo (pt);
        }
        if (! started) return;
        juce::Path area (p);
        area.lineTo (p.getCurrentPosition().x, plot.getBottom());
        area.lineTo (fx (juce::jmax (t0, a.etcMs.front())), plot.getBottom());
        area.closeSubPath();
        g.setColour (theme::accentIn.withAlpha (0.12f));
        g.fillPath (area);
        g.setColour (theme::accentIn);
        g.strokePath (p, juce::PathStrokeType (1.3f));
    }

    AnalysisPage& page;
    juce::Point<float> hover { -1.0f, -1.0f };
    juce::Point<float> tilt { 0.22f, 0.40f }, dragStart;
};

//==============================================================================
AnalysisPage::AnalysisPage (RoomLibrary& l) : lib (l)
{
    chart = std::make_unique<Chart> (*this);
    addAndMakeVisible (*chart);

    for (int k = 0; k < 2; ++k)
    {
        auto& chip = sideChips[k];
        chip.showArrow = false;
        chip.centred = true;
        chip.colour = theme::text;
        chip.onClick = [this, k] { viewSide = k; libraryChanged(); };
        addChildComponent (chip);
    }

    const Mode modes[] = { Mode::response, Mode::waterfall, Mode::decay, Mode::impulse, Mode::positions };
    for (int i = 0; i < 5; ++i)
    {
        auto& t = tabs[i];
        t.showArrow = false;
        t.centred = true;
        t.colour = theme::text;
        t.onClick = [this, m = modes[i]] { setMode (m); };
        addAndMakeVisible (t);
    }
    addAndMakeVisible (viewOption);
    addAndMakeVisible (compareBox);
    viewOption.onChange = [this] { chart->repaint(); };
    compareBox.setTooltip ("Shows a second measurement in the response, e.g. before / after a correction, or the other speaker.");
    compareBox.onChange = [this]
    {
        const int i = compareBox.getSelectedId() - 2;
        comparePtr = juce::isPositiveAndBelow (i, lib.size()) ? lib.items()[(size_t) i].get() : nullptr;
        if (comparePtr != nullptr) lib.analyseIfNeeded (const_cast<RoomMeasurement*> (comparePtr));
        chart->repaint();
    };

    // Waterfall: range and length from the presets or typed in freely.
    auto wfBox = [this] (juce::ComboBox& box, const std::vector<double>& values, bool time, double current, const juce::String& tip)
    {
        addAndMakeVisible (box);
        box.setEditableText (true);
        for (size_t i = 0; i < values.size(); ++i) box.addItem (time ? msText (values[i]) : hz (values[i]), (int) i + 1);
        box.setText (time ? msText (current) : hz (current), juce::dontSendNotification);
        box.setTooltip (tip);
        box.onChange = [this, &box] { waterfallSettingChanged (box); };
    };
    wfBox (wfLowBox, kWfLows, false, wfLo, "Lowest frequency of the waterfall: choose or type a value (e.g. 25, 40 Hz).");
    wfBox (wfHighBox, kWfHighs, false, wfHi, "Highest frequency of the waterfall: choose or type a value (e.g. 800, 2.5k).");
    wfBox (wfLengthBox, kWfLengths, true, lib.getWaterfallMs(), "How long after the direct sound the waterfall reaches (up to "
                                                                + msText (kWfMaxMs) + "): choose or type a value (e.g. 150 ms, 0.6 s).");

    addAndMakeVisible (exportButton);
    exportButton.setTooltip ("Saves the chart as an image, or its data (response for REW, waterfall as CSV).");
    exportButton.onClick = [this] { showExportMenu(); };
    addChildComponent (calibrateButton);
    accent (calibrateButton);
    calibrateButton.onClick = [this] { if (onCalibrate) onCalibrate(); };

    setMode (Mode::response);
    lib.addListener (this);
    libraryChanged();
}

AnalysisPage::~AnalysisPage() { lib.removeListener (this); }

void AnalysisPage::libraryChanged()
{
    refreshCompareBox();
    auto* m = lib.loaded();
    if (m == nullptr || m->sides() == 1) viewSide = 0;
    for (int k = 0; k < 2; ++k)
    {
        sideChips[k].setVisible (m != nullptr && m->sides() == 2);
        sideChips[k].active = viewSide == k;
        sideChips[k].repaint();
    }
    const bool has = m != nullptr;
    for (auto* c : std::initializer_list<juce::Component*> { chart.get(), &exportButton, &tabs[0], &tabs[1], &tabs[2], &tabs[3], &tabs[4] })
        c->setVisible (has);
    calibrateButton.setVisible (! has && onCalibrate != nullptr);
    setMode (mode);   // the option boxes follow
}

RoomMeasurement* AnalysisPage::shown() const
{
    auto* m = lib.loaded();
    return m != nullptr ? m->side (viewSide) : nullptr;
}

void AnalysisPage::refreshCompareBox()
{
    if (lib.indexOf (comparePtr) < 0 || comparePtr == lib.loaded()) comparePtr = nullptr;
    compareBox.clear (juce::dontSendNotification);
    compareBox.addItem ("Compare: none", 1);
    for (int i = 0; i < lib.size(); ++i)
        if (lib.items()[(size_t) i].get() != lib.loaded()) compareBox.addItem ("Compare: " + lib.items()[(size_t) i]->name, i + 2);
    compareBox.setSelectedId (comparePtr != nullptr ? lib.indexOf (comparePtr) + 2 : 1, juce::dontSendNotification);
}

void AnalysisPage::setMode (Mode m)
{
    const bool changedMode = m != mode;
    mode = m;
    for (int i = 0; i < 5; ++i)
    {
        tabs[i].active = (int) m == i;
        tabs[i].repaint();
    }
    if (changedMode || viewOption.getNumItems() == 0)
    {
        if (m == Mode::response)
        {
            fill (viewOption, { "1/3 octave", "1/6 octave", "1/12 octave", "1/24 octave", "No smoothing" }, 2);
            viewOption.setTooltip ("Smoothing of the response: less detail shows the overall balance, more detail the room's peaks and dips.");
        }
        else if (m == Mode::impulse)
        {
            fill (viewOption, { "50 ms", "100 ms", "300 ms", "1 s" }, 2);
            viewOption.setTooltip ("Time span shown.");
        }
    }
    const bool has = lib.loaded() != nullptr;
    viewOption.setVisible (has && (m == Mode::response || m == Mode::impulse));
    compareBox.setVisible (has && m == Mode::response && lib.size() > 1);
    for (auto* b : { &wfLowBox, &wfHighBox, &wfLengthBox }) b->setVisible (has && m == Mode::waterfall);
    chart->setTooltip (m == Mode::waterfall ? "Drag to tilt the view, double click to reset." : juce::String());
    resized();
    repaint();
    chart->repaint();
}

void AnalysisPage::waterfallSettingChanged (juce::ComboBox& box)
{
    const bool time = &box == &wfLengthBox;
    double v = time ? parseMs (box.getText()) : parseHz (box.getText());
    if (&box == &wfLowBox)       wfLo = v > 0.0 ? juce::jlimit (10.0, 10000.0, v) : wfLo;
    else if (&box == &wfHighBox) wfHi = v > 0.0 ? juce::jlimit (50.0, 20000.0, v) : wfHi;
    // The range needs some width: the other end moves out of the way.
    if (wfHi < wfLo * 1.5)
    {
        if (&box == &wfLowBox) wfHi = juce::jmin (20000.0, wfLo * 1.5);
        else                   wfLo = juce::jmax (10.0, wfHi / 1.5);
    }
    wfLowBox.setText (hz (wfLo), juce::dontSendNotification);
    wfHighBox.setText (hz (wfHi), juce::dontSendNotification);
    if (time)
    {
        v = v > 0.0 ? juce::jlimit (20.0, kWfMaxMs, v) : lib.getWaterfallMs();
        wfLengthBox.setText (msText (v), juce::dontSendNotification);
        lib.setWaterfallMs (v);   // the waterfall is part of the analysis: computed again (the old one stays meanwhile)
    }
    chart->repaint();
}

void AnalysisPage::showExportMenu()
{
    auto* m = shown();
    if (m == nullptr || ! m->analysed) return;
    juce::Component::SafePointer<AnalysisPage> safe (this);
    juce::PopupMenu menu;
    menu.addItem ("Chart as image (.png)...", [safe, name = m->name]
    {
        if (safe == nullptr) return;
        auto image = safe->chart->createComponentSnapshot (safe->chart->getLocalBounds(), true, 2.0f);
        saveAs (safe.getComponent(), safe->chooser, "Export chart", name, "*.png", [image] (const juce::File& f)
        {
            const auto file = f.withFileExtension (".png");
            file.deleteFile();
            juce::FileOutputStream out (file);
            juce::PNGImageFormat png;
            return out.openedOk() && png.writeImageToStream (image, out);
        });
    });
    menu.addItem ("Frequency response (.txt, for REW)...", [safe, m = *m]
    {
        if (safe == nullptr) return;
        const double sm = valueOf (safe->viewOption, kSmoothing, 12.0);
        saveAs (safe.getComponent(), safe->chooser, "Export frequency response", m.name, "*.txt",
                [m, sm] (const juce::File& f) { return measurements::exportResponse (m, f.withFileExtension (".txt"), sm); });
    });
    menu.addItem ("Waterfall (.csv)...", [safe, m = *m]
    {
        if (safe != nullptr)
            saveAs (safe.getComponent(), safe->chooser, "Export waterfall", m.name, "*.csv",
                    [m] (const juce::File& f) { return measurements::exportWaterfall (m, f.withFileExtension (".csv")); });
    });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&exportButton).withMinimumWidth (260));
}

juce::AttributedString AnalysisPage::explanation() const
{
    juce::AttributedString s;
    s.setLineSpacing (3.0f);
    const char* titles[] = { "Frequency response", "Waterfall", "Decay time", "Impulse", "Speakers and microphone" };
    addPara (s, juce::String (titles[(int) mode]) + "\n", theme::text, true, 14.5f);
    switch (mode)
    {
        case Mode::response:
            addPara (s, "How loud each frequency arrives at the listening position, relative to the average. The red markers are room "
                        "resonances: bass notes the room makes louder and lets ring. Dips are mostly reflections cancelling the "
                        "sound; an EQ can't fill them.");
            if (auto* m = shown(); m != nullptr && m->numPositions() > 1)
                addPara (s, " The thin grey lines are the single microphone positions, the bright line their average.");
            break;
        case Mode::waterfall:
            addPara (s, "How the sound dies away after it stops: from the back (0 ms) to the front (later). Ridges that reach far to "
                        "the front are frequencies that keep ringing; they make bass boomy and blur it. Drag to tilt the view.");
            break;
        case Mode::decay:
            addPara (s, "How long the room needs to get 60 dB quieter, per octave. Small rooms and studios sit around 0.2 - 0.5 s. "
                        "Much longer bars in the bass mean boomy bass; acoustic treatment (bass traps) helps there, an EQ only partly.");
            break;
        case Mode::positions:
            addPara (s, "Seen from above, to scale, worked out from the sound's travel time: the speakers, the listening position "
                        "(white) with its distance and angle to each speaker, and every other microphone position (green). The "
                        "dashed triangle is the ideal: both speakers at the same distance, 60 degrees apart. A position off the "
                        "centre line makes one speaker arrive earlier and pulls the stereo image to its side.");
            break;
        case Mode::impulse:
            addPara (s, "The sound's energy over time. 0 ms is the direct sound from the speaker; spikes after it are reflections from "
                        "walls, desk and ceiling. Strong spikes in the first 20 ms smear the stereo image.");
            if (auto* m = shown())
                addPara (s, " The sound reached the microphone " + juce::String (m->ir.latencyMs, 1) + " ms after it left the output (device latency + distance).");
            break;
    }
    return s;
}

void AnalysisPage::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);
    auto* m = shown();
    if (m == nullptr)
    {
        auto r = emptyArea;
        g.setColour (theme::text);
        g.setFont (theme::font (22.0f, true));
        g.drawText (lib.size() == 0 ? "No measurement yet" : "No measurement loaded", r.removeFromTop (34), juce::Justification::centred);
        g.setColour (theme::textDim);
        g.setFont (theme::font (14.0f));
        g.drawFittedText (lib.size() == 0 ? calibrateFirst (onCalibrate != nullptr) + ": the wizard measures your room step by step. The "
                                            "measurement then shows up here in detail."
                                          : "Select a measurement in the list on the Correction page and click \"Load\".", r.removeFromTop (60), juce::Justification::centredTop, 3);
        return;
    }

    drawHeading (g, headArea.withTrimmedRight (exportButton.getWidth() + (sideChips[0].isVisible() ? 340 : 0) + 30), lib.loaded()->name, describe (*m));

    // Key numbers.
    {
        auto r = tilesArea;
        const int w = (r.getWidth() - 3 * 10) / 4;
        auto next = [&] { auto t = r.removeFromLeft (w); r.removeFromLeft (10); return t; };
        const auto& a = m->analysis;
        if (! m->analysed)
        {
            for (auto* label : { "Signal-to-noise", "Reverberation time", "Strongest room mode", "Microphone level" })
                drawTile (g, next(), label, "...", theme::textFaint, "analysing");
        }
        else
        {
            const double snr = m->ir.irSnrDb;
            drawTile (g, next(), "Signal-to-noise", juce::String (juce::roundToInt (snr)) + " dB", snr >= 35.0 ? theme::good : theme::warn,
                      snr >= 35.0 ? "clean measurement" : "noisy: details may vary");
            double sum = 0.0;
            int n = 0;
            for (auto& d : a.decay)
                if (d.centreHz >= 120.0 && d.centreHz <= 4500.0 && (d.t20 > 0.0 || d.t30 > 0.0)) { sum += d.t20 > 0.0 ? d.t20 : d.t30; ++n; }
            const double rt = n > 0 ? sum / n : -1.0;
            drawTile (g, next(), "Reverberation time (RT60)", rt > 0.0 ? juce::String (rt, 2) + " s" : juce::String ("-"),
                      rt > 0.6 ? theme::warn : theme::text, "125 Hz - 4 kHz, typical 0.2 - 0.5 s");
            if (a.resonances.empty())
                drawTile (g, next(), "Strongest room mode", "none", theme::good, "nothing above 3 dB below 400 Hz");
            else
            {
                const auto& r0 = a.resonances.front();
                drawTile (g, next(), "Strongest room mode", hz (r0.freq), r0.excessDb > 6.0 ? theme::mute : theme::warn,
                          "+" + juce::String (r0.excessDb, 1) + " dB, rings " + juce::String (juce::roundToInt (r0.ringMs)) + " ms");
            }
            const double pk = m->ir.capturePeakDb;
            drawTile (g, next(), "Microphone level", juce::String (pk, 1) + " dBFS",
                      m->ir.clipped ? theme::mute : pk < -45.0 ? theme::warn : theme::text,
                      m->ir.clipped ? "clipped!" : pk < -45.0 ? "very weak" : "peak during the sweep");
        }
        const auto warn = quality (*m);
        if (warn.isNotEmpty())
        {
            g.setColour (theme::warn);
            g.setFont (theme::font (12.5f));
            g.drawText (warn, tilesArea.withY (tilesArea.getBottom() + 4).withHeight (20), juce::Justification::centredLeft, true);
        }
    }

    // Captions of the waterfall boxes.
    if (wfLowBox.isVisible())
    {
        g.setColour (theme::textDim);
        g.setFont (theme::font (12.0f));
        for (auto [box, label] : { std::pair<juce::Component*, const char*> { &wfLowBox, "Frequencies from" }, { &wfHighBox, "to" },
                                   { &wfLengthBox, "Decay shown" } })
        {
            const int w = box == &wfHighBox ? 24 : 104;
            g.drawText (label, box->getX() - w - 6, box->getY(), w, box->getHeight(), juce::Justification::centredRight);
        }
    }

    // Right column: what the chart shows, then the resonances.
    auto card = [&g] (juce::Rectangle<int> r) { g.setColour (theme::panel); g.fillRoundedRectangle (r.toFloat(), 10.0f); };
    card (explainArea);
    explainLayout.draw (g, explainArea.reduced (kPad, 12).toFloat());
    card (resonanceArea);
    {
        auto r = resonanceArea.reduced (kPad, 12);
        g.setColour (theme::text);
        g.setFont (theme::font (14.5f, true));
        g.drawText ("Room resonances", r.removeFromTop (22), juce::Justification::centredLeft);
        g.setColour (theme::textDim);
        g.setFont (theme::font (12.0f));
        g.drawFittedText ("Bass notes the room makes louder. The bar shows how long each keeps ringing.", r.removeFromTop (34), juce::Justification::topLeft, 2);
        r.removeFromTop (6);
        const auto& res = m->analysis.resonances;
        if (! m->analysed || res.empty())
        {
            g.setColour (m->analysed ? theme::good : theme::textFaint);
            g.drawFittedText (m->analysed ? "None above 3 dB below 400 Hz: the bass is even." : "Analysing...", r, juce::Justification::topLeft, 3);
            return;
        }
        const int cols[] = { 0, 70, 132 };
        g.setColour (theme::textFaint);
        g.setFont (theme::font (11.0f));
        auto head = r.removeFromTop (18);
        const char* heads[] = { "Frequency", "Peak", "Rings for" };
        for (int c = 0; c < 3; ++c) g.drawText (heads[c], head.getX() + cols[c], head.getY(), 70, 18, juce::Justification::centredLeft);
        const double span = m->analysis.waterfallMs.empty() ? 300.0 : juce::jmax (1.0, m->analysis.waterfallMs.back());
        const int rows = juce::jmin ((int) res.size(), r.getHeight() / 24);
        for (int i = 0; i < rows; ++i)
        {
            const auto& x = res[(size_t) i];
            auto row = r.removeFromTop (24);
            g.setColour (theme::text);
            g.setFont (theme::font (13.0f, i < 3));
            g.drawText (hz (x.freq), row.getX() + cols[0], row.getY(), 68, 24, juce::Justification::centredLeft);
            g.setColour (theme::mute.interpolatedWith (theme::text, juce::jlimit (0.0f, 1.0f, 1.0f - (float) (x.excessDb - 3.0) / 6.0f)));
            g.drawText ("+" + juce::String (x.excessDb, 1) + " dB", row.getX() + cols[1], row.getY(), 60, 24, juce::Justification::centredLeft);
            const bool atEnd = x.ringMs >= span - 0.5;
            g.setColour (theme::textDim);
            g.drawText ((atEnd ? "> " : "") + juce::String (juce::roundToInt (x.ringMs)) + " ms", row.getX() + cols[2], row.getY(), 64, 24, juce::Justification::centredLeft);
            const int barX = cols[2] + 66;
            const float frac = (float) juce::jlimit (0.0, 1.0, x.ringMs / span);
            const auto track = juce::Rectangle<float> ((float) (row.getX() + barX), (float) row.getY() + 9.0f, (float) (row.getWidth() - barX), 6.0f);
            g.setColour (theme::panel2);
            g.fillRoundedRectangle (track, 3.0f);
            g.setColour (timeColour (frac).withAlpha (0.85f));
            g.fillRoundedRectangle (track.withWidth (juce::jmax (4.0f, track.getWidth() * frac)), 3.0f);
        }
        if ((int) res.size() > rows)
        {
            g.setColour (theme::textFaint);
            g.setFont (theme::font (11.5f));
            g.drawText ("+ " + juce::String ((int) res.size() - rows) + " more", r.removeFromTop (18), juce::Justification::centredLeft);
        }
    }
}

void AnalysisPage::resized()
{
    auto r = getLocalBounds().reduced (24, 18);
    emptyArea = r.withSizeKeepingCentre (juce::jmin (520, r.getWidth()), 110).translated (0, -40);
    calibrateButton.setBounds (emptyArea.getCentreX() - 110, emptyArea.getBottom() + 10, 220, 38);

    headArea = r.removeFromTop (56);
    {
        auto h = headArea.withTrimmedTop (kCaptionH);
        exportButton.setBounds (h.removeFromRight (100).withHeight (30));
        h.removeFromRight (10);
        if (lib.loaded() != nullptr && lib.loaded()->sides() == 2)
        {
            auto chips = h.removeFromRight (320).withHeight (30);
            sideChips[1].setBounds (chips.removeFromRight (156));
            chips.removeFromRight (8);
            sideChips[0].setBounds (chips.removeFromRight (156));
        }
    }
    r.removeFromTop (12);
    tilesArea = r.removeFromTop (78);
    r.removeFromTop (shown() != nullptr && quality (*shown()).isNotEmpty() ? 30 : 14);

    sideArea = r.removeFromRight (juce::jlimit (300, 380, r.getWidth() * 28 / 100));
    r.removeFromRight (14);

    auto top = r.removeFromTop (32);
    for (auto& t : tabs)
    {
        t.setBounds (top.removeFromLeft (112));
        top.removeFromLeft (6);
    }
    top.removeFromLeft (10);
    viewOption.setBounds (top.removeFromRight (140));
    top.removeFromRight (kGap);
    compareBox.setBounds (top.removeFromRight (juce::jmin (300, juce::jmax (0, top.getWidth()))));
    if (mode == Mode::waterfall)   // frequency range and how long after the sound: a row of their own
    {
        r.removeFromTop (8);
        auto row = r.removeFromTop (30);
        for (auto* b : { &wfLowBox, &wfHighBox, &wfLengthBox })
        {
            row.removeFromLeft (b == &wfLowBox ? 120 : b == &wfHighBox ? 32 : 110);
            b->setBounds (row.removeFromLeft (110));
        }
    }
    r.removeFromTop (10);
    chart->setBounds (r);

    explainLayout.createLayout (explanation(), (float) (sideArea.getWidth() - 2 * kPad));
    explainArea = sideArea.removeFromTop ((int) std::ceil (explainLayout.getHeight()) + 26);
    sideArea.removeFromTop (12);
    resonanceArea = sideArea;
}

//==============================================================================
/** Before / after of one side: the measured response (grey), the correction (orange, filled), the result (green),
    the flat target (dashed), the corrected range (tinted) and a dot per band. Read-out under the mouse. */
class EditPage::Chart final : public juce::Component
{
public:
    explicit Chart (EditPage& p) : page (p) {}

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        g.setColour (theme::panel);
        g.fillRoundedRectangle (b, 10.0f);
        auto* m = page.measured[page.viewSide];
        if (m == nullptr) return;
        if (! m->analysed)
        {
            g.setColour (theme::textDim);
            g.setFont (theme::font (15.0f));
            g.drawText ("Analysing the measurement...", getLocalBounds(), juce::Justification::centred);
            return;
        }
        const auto& a = m->analysis;
        const auto& eq = page.eqs[page.viewSide];
        const auto plot = b.withTrimmedLeft (50.0f).withTrimmedRight (18.0f).withTrimmedTop (44.0f).withTrimmedBottom (28.0f);
        constexpr double lo = 20.0, hi = 20000.0, top = 15.0, bottom = -25.0;
        auto fx = [&] (double f) { return plot.getX() + plot.getWidth() * (float) (std::log (f / lo) / std::log (hi / lo)); };
        auto fy = [&] (double db) { return plot.getY() + plot.getHeight() * (float) ((top - juce::jlimit (bottom, top, db)) / (top - bottom)); };

        const double rangeLo = valueOf (page.lowBox, kEqLow, 20.0), rangeHi = valueOf (page.highBox, kEqHigh, 500.0);
        g.setColour (theme::accentIn.withAlpha (0.05f));
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (fx (rangeLo), plot.getY(), fx (juce::jmin (rangeHi, hi)), plot.getBottom()));

        drawFreqGrid (g, plot, lo, hi, true);
        g.setFont (theme::font (11.0f));
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
        const bool designed = ! eq.bands.empty() && eq.correctedDb.size() == a.freqs.size() && eq.eqDb.size() == a.freqs.size();
        if (designed)
        {
            g.setColour (theme::text.withAlpha (0.55f));
            dashedLine (g, { fx (rangeLo), fy (eq.targetDb) }, { fx (juce::jmin (rangeHi, hi)), fy (eq.targetDb) }, 1.2f);
            auto corr = curve (eq.eqDb);
            juce::Path area (corr);
            area.lineTo (fx (a.sweepEndHz), fy (0.0));
            area.lineTo (fx (a.sweepStartHz), fy (0.0));
            area.closeSubPath();
            g.setColour (theme::warn.withAlpha (0.12f));
            g.fillPath (area);
            stroke (corr, theme::warn.withAlpha (0.9f), 1.5f);
        }
        stroke (curve (before), theme::textDim, designed ? 1.6f : 2.2f);
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
        if (designed)
        {
            stroke (curve (eq.correctedDb), theme::good, 2.4f);
            for (size_t k = 0; k < eq.bands.size(); ++k)   // a dot per band on the correction curve
            {
                const auto& band = eq.bands[k];
                const juce::Point<float> pt (fx (band.freq), fy (room::valueAt (a.freqs, eq.eqDb, band.freq)));
                g.setColour (theme::bandColour ((int) k % 8));
                g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre (pt));
                g.setColour (theme::panel);
                g.drawEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre (pt), 1.2f);
            }
        }

        if (plot.contains (hover))
        {
            const double f = lo * std::pow (hi / lo, (double) ((hover.x - plot.getX()) / plot.getWidth()));
            juce::String t = hz (f) + "   before " + juce::String (room::valueAt (a.freqs, before, f), 1) + " dB";
            if (designed)
                t << "   correction " << juce::String (room::valueAt (a.freqs, eq.eqDb, f), 1) << " dB   after "
                  << juce::String (room::valueAt (a.freqs, eq.correctedDb, f), 1) << " dB";
            drawReadout (g, plot, hover.x, t);
        }
        g.restoreState();

        std::vector<std::pair<juce::String, juce::Colour>> legend { { "Before", theme::textDim } };
        if (designed)
        {
            legend.push_back ({ "After", theme::good });
            legend.push_back ({ "Correction", theme::warn });
            legend.push_back ({ "Target", theme::text.withAlpha (0.55f) });
            if (eq.usedNear) legend.push_back ({ "Speaker (near field)", theme::accentVirt });
        }
        drawLegend (g, getLocalBounds().withTrimmedRight (16).removeFromTop (34), legend);
        g.setColour (theme::text);
        g.setFont (theme::font (14.0f, true));
        g.drawText (m->speakers == 1 ? "Left speaker" : m->speakers == 2 ? "Right speaker" : "Both speakers",
                    getLocalBounds().reduced (16, 0).removeFromTop (34), juce::Justification::centredLeft);
    }

    void mouseMove (const juce::MouseEvent& e) override { hover = e.position; repaint(); }
    void mouseExit (const juce::MouseEvent&) override   { hover = { -1.0f, -1.0f }; repaint(); }

private:
    EditPage& page;
    juce::Point<float> hover { -1.0f, -1.0f };
};

//==============================================================================
EditPage::EditPage (RoomLibrary& l) : lib (l)
{
    chart = std::make_unique<Chart> (*this);
    addAndMakeVisible (*chart);


    for (int i = 0; i < 3; ++i)
    {
        presetCards[i] = std::make_unique<ChoiceCard> (kPresets[i].name, kPresets[i].detail);
        presetCards[i]->onClick = [this, i] { choosePreset (i); };
        addAndMakeVisible (*presetCards[i]);
    }
    levelCards[0] = std::make_unique<ChoiceCard> ("Keep loudness", "Music stays as loud as before. The limiter catches rare peaks.");
    levelCards[1] = std::make_unique<ChoiceCard> ("Keep headroom", "Nothing can clip, but everything gets quieter.");
    for (int i = 0; i < 2; ++i)
    {
        levelCards[i]->onClick = [this, i] { keepLoudness = i == 0; updateControls(); userChanged(); };
        addAndMakeVisible (*levelCards[i]);
    }

    auto combo = [this] (juce::ComboBox& box, const juce::String& caption, const juce::StringArray& items, int def, const juce::String& tip)
    {
        addAndMakeVisible (box);
        fill (box, items, def);
        box.setTooltip (tip);
        box.onChange = [this] { userChanged(); };
        captions.push_back ({ &box, caption });
    };
    combo (lowBox, "From", { "20 Hz", "25 Hz", "30 Hz", "40 Hz", "60 Hz" }, 0,
           "Lowest frequency the correction works on (below it the speaker's own roll-off stays).");
    combo (highBox, "To", { "200 Hz", "300 Hz", "500 Hz", "1 kHz", "2 kHz", "5 kHz", "20 kHz" }, 2,
           "Highest frequency the correction works on. Room modes live below ~300 - 500 Hz; above that a correction mostly fits "
           "the microphone position, not your ears.");
    addChildComponent (calBox);
    calBox.setTooltip ("The microphone's calibration file. Changing it analyses the measurement again and designs the correction anew.");
    calBox.onChange = [this] { calChosen(); };
    captions.push_back ({ &calBox, "Microphone calibration" });
    addChildComponent (nearToggle);
    nearToggle.setToggleState (true, juce::dontSendNotification);
    nearToggle.setTooltip ("Above 400 Hz the correction follows the speaker's own response (measured 1 - 2 cm in front of it) "
                           "instead of the listening position, where the room's reflections make ripples that move with your head.");
    nearToggle.onClick = [this] { userChanged(); };
    addChildComponent (alignToggle);
    alignToggle.setToggleState (true, juce::dontSendNotification);
    alignToggle.setTooltip ("The nearer speaker waits a little, so the sound of both arrives at the listening position at the same "
                            "time: a centred, sharp stereo image (phase coherence between the speakers).");
    alignToggle.onClick = [this] { userChanged(); };

    auto slider = [this] (juce::Slider& s, const juce::String& caption, double lo, double hi, double step, double value, double mid, const juce::String& tip)
    {
        addAndMakeVisible (s);
        captions.push_back ({ &s, caption });
        s.setSliderStyle (juce::Slider::LinearHorizontal);
        s.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        s.setRange (lo, hi, step);
        if (mid > 0.0) s.setSkewFactorFromMidPoint (mid);
        s.setValue (value, juce::dontSendNotification);
        s.setDoubleClickReturnValue (true, value);
        s.setColour (juce::Slider::trackColourId, theme::accentIn);
        s.setTooltip (tip + " Double click = default.");
        s.onValueChange = [this] { dirtyTicks = 6; updateControls(); repaint(); };   // designs again ~0.2 s after the slider rests
        s.onDragEnd = [this] { dirtyTicks = 1; };
    };
    slider (cutSlider, "Strength (deepest cut)", 0.0, 24.0, 0.5, 12.0, 0.0, "How deep the correction may cut peaks.");
    slider (bandsSlider, "Filter bands", 1.0, 24.0, 1.0, 16.0, 0.0, "Most bands the correction may use. More bands follow the response more closely.");
    slider (detailSlider, "Detail", 2.0, 48.0, 0.5, 12.0, 12.0,
            "How fine the response is that the correction follows: 1/3 octave = only broad trends, 1/48 octave = every wiggle. "
            "Around 1/12 octave suits room modes.");
    slider (boostSlider, "Max boost", 0.0, 12.0, 0.5, 0.0, 0.0,
            "Dips are often cancellations that no boost can fill; boosting them only costs headroom. Keep it off or small.");
    cutSlider.textFromValueFunction = [] (double v) { return "-" + juce::String (v, 1) + " dB"; };
    bandsSlider.textFromValueFunction = [] (double v) { return juce::String (juce::roundToInt (v)); };
    detailSlider.textFromValueFunction = [] (double v) { return "1/" + juce::String (juce::roundToInt (v)) + " octave"; };
    boostSlider.textFromValueFunction = [] (double v) { return v < 0.05 ? juce::String ("none") : "+" + juce::String (v, 1) + " dB"; };

    for (int s = 0; s < 2; ++s)
    {
        auto& chip = sideChips[s];
        chip.showArrow = false;
        chip.centred = true;
        chip.colour = theme::text;
        chip.onClick = [this, s] { viewSide = s; updateControls(); resized(); chart->repaint(); repaint(); };
        addChildComponent (chip);
    }

    addChildComponent (calibrateButton);
    accent (calibrateButton);
    calibrateButton.onClick = [this] { if (onCalibrate) onCalibrate(); };

    lib.addListener (this);
    libraryChanged();
    updateControls();
    startTimerHz (30);
}

EditPage::~EditPage()
{
    stopTimer();
    lib.removeListener (this);
}

void EditPage::setTarget (CorrectionTarget* t)
{
    target = t;
    takeDesign();
    design();
    updateControls();
}

void EditPage::load (RoomMeasurement* m)
{
    if (m == nullptr) return;
    applyWhenDesigned = true;
    status = {};
    if (m == lib.loaded()) design();
    else                   lib.setLoaded (m);   // designs (libraryChanged), then applies
}

void EditPage::libraryChanged()
{
    // The selection (or its analysis) changed: design again.
    RoomMeasurement* before[2] = { measured[0], measured[1] };
    const bool wasAnalysed[2] = { measured[0] != nullptr && measured[0]->analysed, measured[1] != nullptr && measured[1]->analysed };
    takeSelection();
    // Another calibration for the same measurement (here or in the list): the output gets the new design.
    const juce::String calName = measured[0] != nullptr ? juce::String (measured[0]->calibration.name) : juce::String();
    if (measured[0] != shownCalFor || calName != shownCalName)
    {
        if (measured[0] != nullptr && measured[0] == shownCalFor) applyWhenDesigned = true;
        refreshCalBox();
    }
    bool redo = false;
    for (int k = 0; k < 2; ++k)
        redo = redo || measured[k] != before[k] || (measured[k] != nullptr && measured[k]->analysed != wasAnalysed[k]);
    if (redo)
    {
        if (measured[0] != before[0])
        {
            viewSide = 0;
            status = {};
            if (! applyWhenDesigned) takeDesign();   // Load keeps the settings on the page
        }
        design();
    }
    const bool has = measured[0] != nullptr;
    for (auto* c : std::initializer_list<juce::Component*> { chart.get(), &lowBox, &highBox, &calBox, &cutSlider, &bandsSlider,
                                                              &detailSlider, &boostSlider,
                                                              presetCards[0].get(), presetCards[1].get(), presetCards[2].get(),
                                                              levelCards[0].get(), levelCards[1].get() })
        c->setVisible (has);
    calibrateButton.setVisible (! has && onCalibrate != nullptr);
    nearToggle.setVisible (has && ! measured[0]->nearFields.empty());
    alignToggle.setVisible (has && ! std::isnan (measurements::arrivalDifferenceMs (*measured[0])));
    updateControls();
    resized();
    repaint();
}

void EditPage::refreshCalBox()
{
    constexpr int kOtherCalId = 999, kImportCalId = 1000;
    auto* m = measured[0];
    shownCalFor = m;
    shownCalName = m != nullptr ? juce::String (m->calibration.name) : juce::String();
    calList = calibrations::all();
    calBox.clear (juce::dontSendNotification);
    calBox.addItem ("None (flat microphone)", 1);
    shownCalId = 1;
    for (size_t k = 0; k < calList.size(); ++k)
    {
        calBox.addItem (juce::String (calList[k].name), (int) k + 2);
        if (m != nullptr && ! m->calibration.empty() && calList[k].name == m->calibration.name) shownCalId = (int) k + 2;
    }
    if (m != nullptr && ! m->calibration.empty() && shownCalId == 1)   // kept in the measurement, no longer among the files
    {
        calBox.addItem (shownCalName, kOtherCalId);
        shownCalId = kOtherCalId;
    }
    calBox.addSeparator();
    calBox.addItem ("Import calibration file...", kImportCalId);
    calBox.setSelectedId (shownCalId, juce::dontSendNotification);
}

void EditPage::calChosen()
{
    constexpr int kImportCalId = 1000;
    auto* m = measured[0];
    const int id = calBox.getSelectedId();
    if (m == nullptr || id == shownCalId) return;
    if (id == kImportCalId)
    {
        calBox.setSelectedId (shownCalId, juce::dontSendNotification);
        return importCalibration();
    }
    const int k = id - 2;
    lib.setCalibration (m, juce::isPositiveAndBelow (k, (int) calList.size()) ? calList[(size_t) k] : room::Calibration());
}

void EditPage::importCalibration()
{
    chooser = std::make_unique<juce::FileChooser> ("Import microphone calibration", juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
                                                   "*.txt;*.cal;*.frd;*.csv");
    juce::Component::SafePointer<EditPage> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc)
    {
        const auto f = fc.getResult();
        if (safe == nullptr || safe->measured[0] == nullptr || ! f.existsAsFile()) return;
        juce::String err;
        const auto cal = calibrations::import (f, err);
        if (cal.empty())
            return dialog::message (safe.getComponent(), dialog::Kind::warning, "Cannot import", f.getFileName() + ": " + err);
        safe->applyWhenDesigned = true;   // also a new file under the same name
        safe->lib.setCalibration (safe->measured[0], cal);
        safe->refreshCalBox();
    });
}

void EditPage::takeSelection()
{
    auto* m = lib.loaded();
    measured[0] = m;
    measured[1] = m != nullptr ? m->right.get() : nullptr;
    viewSide = juce::jmin (viewSide, sides() - 1);
}

void EditPage::timerCallback()
{
    const bool dragging = cutSlider.isMouseButtonDown() || bandsSlider.isMouseButtonDown() || detailSlider.isMouseButtonDown()
                          || boostSlider.isMouseButtonDown();
    if (dirtyTicks > 0 && --dirtyTicks == 0)
    {
        if (dragging) dirtyTicks = 1;   // still dragging: try again next tick
        else          userChanged();
    }
}

void EditPage::choosePreset (int i)
{
    const auto& p = kPresets[i];
    cutSlider.setValue (p.maxCut, juce::dontSendNotification);
    bandsSlider.setValue (p.bands, juce::dontSendNotification);
    detailSlider.setValue (p.detailOct, juce::dontSendNotification);
    updateControls();
    userChanged();
}

void EditPage::userChanged()
{
    applyWhenDesigned = true;
    status = {};
    design();
}

void EditPage::takeDesign()
{
    if (target == nullptr || measured[0] == nullptr) return;
    const auto& p = target->getProfile();
    if (! p.design.valid || p.source != measured[0]->file.getFileName()) return;
    const auto& d = p.design;
    auto pick = [] (juce::ComboBox& box, const std::vector<double>& values, double v)
    {
        size_t best = 0;
        for (size_t i = 1; i < values.size(); ++i)
            if (std::abs (std::log (values[i] / v)) < std::abs (std::log (values[best] / v))) best = i;
        box.setSelectedId ((int) best + 1, juce::dontSendNotification);
    };
    pick (lowBox, kEqLow, d.lowHz);
    pick (highBox, kEqHigh, d.highHz);
    cutSlider.setValue (d.maxCutDb, juce::dontSendNotification);
    bandsSlider.setValue (d.bands, juce::dontSendNotification);
    detailSlider.setValue (d.detail, juce::dontSendNotification);
    boostSlider.setValue (d.maxBoostDb, juce::dontSendNotification);
    keepLoudness = d.keepLoudness;
    nearToggle.setToggleState (d.useNear, juce::dontSendNotification);
    alignToggle.setToggleState (d.timeAlign, juce::dontSendNotification);
    updateControls();
}

int EditPage::matchingPreset() const
{
    for (int i = 0; i < 3; ++i)
        if (std::abs (cutSlider.getValue() - kPresets[i].maxCut) < 0.01 && juce::roundToInt (bandsSlider.getValue()) == kPresets[i].bands
            && std::abs (detailSlider.getValue() - kPresets[i].detailOct) < 0.01)
            return i;
    return -1;
}

void EditPage::design()
{
    const int gen = ++generation;
    pending = 0;
    for (int side = 0; side < 2; ++side)
    {
        eqs[side] = {};
        auto* m = measured[side];
        if (m == nullptr || ! m->analysed) continue;
        room::AutoEqSettings s;
        s.lowHz = valueOf (lowBox, kEqLow, 20.0);
        s.highHz = valueOf (highBox, kEqHigh, 500.0);
        s.maxBands = juce::roundToInt (bandsSlider.getValue());
        s.maxCutDb = cutSlider.getValue();
        s.maxBoostDb = boostSlider.getValue() < 0.05 ? 0.0 : boostSlider.getValue();
        s.smoothing = detailSlider.getValue();
        s.matchLoudness = keepLoudness;
        if (nearToggle.getToggleState()) s.nearDb = m->analysis.nearDb;
        s.sampleRate = target != nullptr && target->getSampleRate() > 0.0 ? target->getSampleRate() : m->ir.sampleRate;
        ++pending;
        juce::Component::SafePointer<EditPage> safe (this);
        juce::Thread::launch (juce::Thread::Priority::low, [safe, gen, side, s, a = m->analysis]
        {
            auto result = std::make_shared<room::AutoEqResult> (room::designEq (a, s));
            juce::MessageManager::callAsync ([safe, gen, side, result]
            {
                if (safe == nullptr || gen != safe->generation) return;   // settings changed meanwhile
                safe->eqs[side] = std::move (*result);
                if (--safe->pending == 0 && safe->applyWhenDesigned && safe->ready()) safe->applyNow();
                safe->updateControls();
                safe->resized();   // the band list's height follows the number of bands
                safe->chart->repaint();
                safe->repaint();
            });
        });
    }
    if (pending == 0 && applyWhenDesigned && ready()) applyNow();
    updateControls();
    chart->repaint();
    repaint();
}

void EditPage::updateControls()
{
    const int preset = matchingPreset();
    for (int i = 0; i < 3; ++i)
    {
        presetCards[i]->selected = preset == i;
        presetCards[i]->repaint();
    }
    for (int i = 0; i < 2; ++i)
    {
        levelCards[i]->selected = keepLoudness == (i == 0);
        levelCards[i]->repaint();
    }
    for (int s = 0; s < 2; ++s)
    {
        sideChips[s].setVisible (sides() == 2);
        sideChips[s].active = viewSide == s;
        sideChips[s].repaint();
    }
}

CorrectionProfile EditPage::profile() const
{
    auto side = [this] (int k)
    {
        CorrectionProfile::Side s;
        s.preampDb = (float) eqs[k].preampDb;
        for (auto& b : eqs[k].bands) s.bands.push_back ({ ParamEq::Bell, (float) b.freq, (float) b.gainDb, (float) b.q });
        return s;
    };
    CorrectionProfile p;
    p.name = measured[0] != nullptr ? measured[0]->name : juce::String ("Room correction");
    p.source = measured[0] != nullptr ? measured[0]->file.getFileName() : juce::String();
    if (sides() == 2)
    {
        p.setSide (0, side (0));
        p.setSide (1, side (1));
    }
    else
        p.setSide (-1, side (0));
    // How it was made (shown again when it is loaded) and the time alignment.
    auto& d = p.design;
    d.valid = true;
    d.lowHz = valueOf (lowBox, kEqLow, 20.0);
    d.highHz = valueOf (highBox, kEqHigh, 500.0);
    d.maxCutDb = cutSlider.getValue();
    d.maxBoostDb = boostSlider.getValue();
    d.detail = detailSlider.getValue();
    d.bands = juce::roundToInt (bandsSlider.getValue());
    d.keepLoudness = keepLoudness;
    d.useNear = nearToggle.getToggleState();
    d.timeAlign = alignToggle.getToggleState();
    if (d.timeAlign && measured[0] != nullptr) measurements::alignSpeakers (p, *measured[0]);
    return p;
}

/** What Apply puts on the output: a measurement of one speaker corrects only that channel, the other keeps its correction. */
static CorrectionProfile merged (const CorrectionProfile& made, const CorrectionProfile& old, int only)
{
    if (only < 0) return made;
    auto p = old;
    p.source = made.source;
    p.design = made.design;
    if (old.isEmpty() || ! old.separate) p.name = made.name;
    p.setSide (only, made.side (0));
    return p;
}

void EditPage::applyNow()
{
    applyWhenDesigned = false;
    auto* m = measured[0];
    if (target == nullptr || m == nullptr || pending > 0 || m->throughEq) return;
    const auto old = target->getProfile();
    const int only = sides() == 1 && m->speakers != 3 ? m->speakers - 1 : -1;
    auto p = merged (profile(), old, only);
    // A level set by hand on the Correction page stays (same measurement).
    const double offset = old.source == p.source && old.design.valid ? old.design.levelOffsetDb : 0.0;
    p.design.levelOffsetDb = offset;
    // Designed again from the same measurement: still the same correction (its pool entry follows), otherwise a new one.
    if (p.poolId.isEmpty() && old.source == p.source) p.poolId = old.poolId;
    p.preampDb = juce::jlimit (-30.0f, 12.0f, p.preampDb + (float) offset);
    if (p.separate) p.right.preampDb = juce::jlimit (-30.0f, 12.0f, p.right.preampDb + (float) offset);
    const bool droppedRefinement = old.design.refined && old.source == p.source;
    target->setProfile (p);
    target->setOn (true);
    status = juce::String::fromUTF8 ("\xe2\x9c\x93  Active on ") + target->label()
             + (p.alignMs[0] > 0.0f || p.alignMs[1] > 0.0f ? juce::String::fromUTF8 ("  \xc2\xb7  ") + (p.alignMs[0] > 0.0f ? "left" : "right")
                                                                 + " speaker delayed " + juce::String (juce::jmax (p.alignMs[0], p.alignMs[1]), 2) + " ms"
                                                           : juce::String());
    statusColour = theme::good;
    if (droppedRefinement)
    {
        status << "  -  the refinement from validating was replaced: validate again to refine.";
        statusColour = theme::warn;
    }
    repaint();
}

void EditPage::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);
    auto* m = measured[0];
    if (m == nullptr)
    {
        auto r = emptyArea;
        g.setColour (theme::text);
        g.setFont (theme::font (22.0f, true));
        g.drawText (lib.size() == 0 ? "Nothing to edit yet" : "No measurement loaded", r.removeFromTop (34), juce::Justification::centred);
        g.setColour (theme::textDim);
        g.setFont (theme::font (14.0f));
        g.drawFittedText (lib.size() == 0 ? "The correction is designed from a measurement of your room. " + calibrateFirst (onCalibrate != nullptr)
                                            + ", then fine-tune the result here."
                                          : "Select a measurement in the list on the Correction page and click \"Load\": its correction "
                                            "can then be fine-tuned here.", r.removeFromTop (60), juce::Justification::centredTop, 3);
        return;
    }

    drawHeading (g, headArea, "Fine-tune the correction",
                 m->name + dot() + (sides() == 2 ? juce::String ("left and right speaker, each with its own correction") : describe (*m)));

    g.setColour (theme::outline.withAlpha (0.6f));
    g.drawHorizontalLine (footerArea.getY(), 0.0f, (float) getWidth());

    // Captions (sliders: the value on the right).
    g.setFont (theme::font (12.0f));
    for (auto& [comp, text] : captions)
    {
        if (! comp->isVisible()) continue;
        g.setColour (theme::textDim);
        g.drawText (text, comp->getX() + 2, comp->getY() - kCaptionH, comp->getWidth(), kCaptionH, juce::Justification::centredLeft, true);
        if (auto* s = dynamic_cast<juce::Slider*> (comp))
        {
            g.setColour (theme::text);
            g.drawText (s->getTextFromValue (s->getValue()), s->getX(), s->getY() - kCaptionH, s->getWidth() - 2, kCaptionH, juce::Justification::centredRight);
        }
    }
    auto section = [&g] (juce::Component& first, const juce::String& title)
    {
        g.setColour (theme::text);
        g.setFont (theme::font (14.0f, true));
        g.drawText (title, first.getX(), first.getY() - 24, 300, 20, juce::Justification::centredLeft);
    };
    section (*presetCards[0], "Strength");
    section (*levelCards[0], "Level");

    // Numbers before -> after of the shown side.
    {
        auto r = statsArea;
        const auto& eq = eqs[viewSide];
        auto* shown = measured[viewSide];
        if (pending > 0 || shown == nullptr || ! shown->analysed)
        {
            g.setColour (theme::textDim);
            g.setFont (theme::font (13.5f));
            g.drawText ("Designing the correction...", r.removeFromTop (24), juce::Justification::centredLeft);
        }
        else if (eq.bands.empty())
        {
            g.setColour (theme::good);
            g.setFont (theme::font (13.5f));
            g.drawText ("Nothing to correct with these settings: the response is already flat in this range.", r.removeFromTop (24), juce::Justification::centredLeft);
        }
        else
        {
            auto row = r.removeFromTop (70);
            const int w = (row.getWidth() - 2 * 10) / 3;
            drawChangeTile (g, row.removeFromLeft (w), "Deviation from flat (rms)", eq.rmsBeforeDb, eq.rmsAfterDb);
            row.removeFromLeft (10);
            drawChangeTile (g, row.removeFromLeft (w), "Highest peak", eq.peakBeforeDb, eq.peakAfterDb);
            row.removeFromLeft (10);
            drawTile (g, row, "Filter", juce::String ((int) eq.bands.size()) + " bands",  theme::text,
                      "level " + juce::String (eq.preampDb > 0.0 ? "+" : "") + juce::String (eq.preampDb, 1) + " dB");
            r.removeFromTop (10);
            // The bands as small coloured chips.
            const int perRow = juce::jmax (1, r.getWidth() / 150), cw = r.getWidth() / perRow;
            for (size_t k = 0; k < eq.bands.size(); ++k)
            {
                auto cell = juce::Rectangle<int> (r.getX() + (int) (k % (size_t) perRow) * cw, r.getY() + (int) (k / (size_t) perRow) * 24, cw - 6, 20);
                if (cell.getBottom() > r.getBottom()) break;
                const auto& b = eq.bands[k];
                g.setColour (theme::bandColour ((int) k % 8).withAlpha (0.15f));
                g.fillRoundedRectangle (cell.toFloat(), 5.0f);
                g.setColour (theme::bandColour ((int) k % 8));
                g.setFont (theme::font (11.5f, true));
                g.drawText (juce::String ((int) k + 1), cell.removeFromLeft (20), juce::Justification::centred);
                g.setColour (theme::text);
                g.setFont (theme::font (11.5f));
                g.drawText (hz (b.freq) + "  " + juce::String (b.gainDb, 1) + " dB  Q " + juce::String (b.q, 1), cell, juce::Justification::centredLeft, true);
            }
        }
    }

    // Status in the footer.
    juce::String text = status;
    juce::Colour c = statusColour;
    if (m->throughEq) { text = "Measured through the correction: use it to check a correction, not to design one."; c = theme::warn; }
    else if (target == nullptr) { text = "Choose an output at the top of the window."; c = theme::warn; }
    else if (text.isEmpty()) text = "Every change goes to " + target->label() + " right away.";
    g.setColour (c);
    g.setFont (theme::font (13.0f));
    g.drawFittedText (text, statusArea, juce::Justification::centredLeft, 2);
}

void EditPage::resized()
{
    auto r = getLocalBounds();
    emptyArea = r.reduced (24).withSizeKeepingCentre (juce::jmin (520, r.getWidth() - 48), 110).translated (0, -40);
    calibrateButton.setBounds (emptyArea.getCentreX() - 110, emptyArea.getBottom() + 10, 220, 38);

    footerArea = r.removeFromBottom (48);
    statusArea = footerArea.reduced (24, 8);
    r.reduce (24, 18);
    headArea = r.removeFromTop (56);
    calBox.setBounds (headArea.removeFromRight (300).withTrimmedTop (kCaptionH).withHeight (30));
    headArea.removeFromRight (16);
    r.removeFromTop (16);

    // Settings column.
    settingsArea = r.removeFromLeft (juce::jlimit (340, 420, r.getWidth() * 32 / 100));
    r.removeFromLeft (24);
    {
        auto s = settingsArea;
        s.removeFromTop (24);
        auto row = s.removeFromTop (74);
        const int w = (row.getWidth() - 2 * kGap) / 3;
        for (auto& card : presetCards)
        {
            card->setBounds (row.removeFromLeft (w));
            row.removeFromLeft (kGap);
        }
        s.removeFromTop (12);
        for (auto* sl : { &cutSlider, &bandsSlider, &detailSlider, &boostSlider })
        {
            s.removeFromTop (kCaptionH);
            sl->setBounds (s.removeFromTop (24));
            s.removeFromTop (8);
        }
        s.removeFromTop (kCaptionH);
        row = s.removeFromTop (30);
        lowBox.setBounds (row.removeFromLeft ((row.getWidth() - kGap) / 2));
        row.removeFromLeft (kGap);
        highBox.setBounds (row);
        s.removeFromTop (20 + 24);
        row = s.removeFromTop (78);
        levelCards[0]->setBounds (row.removeFromLeft ((row.getWidth() - kGap) / 2));
        row.removeFromLeft (kGap);
        levelCards[1]->setBounds (row);
        s.removeFromTop (14);
        nearToggle.setBounds (s.removeFromTop (26));
        s.removeFromTop (6);
        alignToggle.setBounds (s.removeFromTop (26));
    }

    // Chart and numbers.
    if (sides() == 2)
    {
        auto chips = r.removeFromTop (30).removeFromLeft (320);
        sideChips[0].setBounds (chips.removeFromLeft (156));
        chips.removeFromLeft (8);
        sideChips[1].setBounds (chips.removeFromLeft (156));
        r.removeFromTop (10);
    }
    const auto& eq = eqs[viewSide];
    const int chipRows = eq.bands.empty() ? 0 : juce::jmin (4, ((int) eq.bands.size() + juce::jmax (1, r.getWidth() / 150) - 1) / juce::jmax (1, r.getWidth() / 150));
    statsArea = r.removeFromBottom (70 + (chipRows > 0 ? 10 + chipRows * 24 : 0));
    r.removeFromBottom (14);
    chart->setBounds (r);
}

//==============================================================================
/** The profile's response (bands + preamp) on a log frequency axis, a dot per band, read-out under the mouse. */
class CorrectionPage::Curve final : public juce::Component
{
public:
    explicit Curve (CorrectionPage& v) : view (v) {}

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        g.setColour (theme::bg);
        g.fillRoundedRectangle (b, 8.0f);
        const auto& p = view.target.getProfile();
        if (p.isEmpty())
        {
            g.setColour (theme::textDim);
            g.setFont (theme::font (14.0f));
            g.drawFittedText (view.emptyHint, getLocalBounds().reduced (60), juce::Justification::centred, 5);
            return;
        }

        const auto plot = b.withTrimmedLeft (52.0f).withTrimmedRight (16.0f).withTrimmedTop (14.0f).withTrimmedBottom (24.0f);
        constexpr double lo = 20.0, hi = 20000.0;
        constexpr int points = 400;
        // One curve, or one per channel when left and right are corrected separately.
        const int channels = p.separate ? 2 : 1;
        std::vector<double> db[2];
        double range = 6.0;
        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i <= points; ++i)
            {
                db[ch].push_back (p.magnitudeDb (lo * std::pow (hi / lo, (double) i / points), 48000.0, ch));
                range = juce::jmax (range, std::abs (db[ch].back()) * 1.15);
            }
        range = juce::jmin (30.0, std::ceil (range / 3.0) * 3.0);
        auto fx = [&] (double f) { return plot.getX() + plot.getWidth() * (float) (std::log (f / lo) / std::log (hi / lo)); };
        auto fy = [&] (double v) { return plot.getCentreY() - plot.getHeight() * 0.5f * (float) (juce::jlimit (-range, range, v) / range); };

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
                                juce::Rectangle<float> (fx (f) - 24.0f, plot.getBottom() + 4.0f, 48.0f, 14.0f), juce::Justification::centred);
                }
            }
        const double step = range > 18.0 ? 6.0 : 3.0;
        for (double v = -range; v <= range + 0.01; v += step)
        {
            g.setColour (theme::outline.withAlpha (v == 0.0 ? 1.0f : 0.45f));
            g.drawHorizontalLine (juce::roundToInt (fy (v)), plot.getX(), plot.getRight());
            g.setColour (theme::textFaint);
            g.drawText ((v > 0 ? "+" : "") + juce::String (juce::roundToInt (v)) + " dB",
                        juce::Rectangle<float> (0.0f, fy (v) - 7.0f, plot.getX() - 6.0f, 14.0f), juce::Justification::centredRight);
        }

        const bool on = view.target.isOn();
        for (int ch = 0; ch < channels; ++ch)
        {
            const auto c = channelColour (ch, channels);
            juce::Path line;
            for (int i = 0; i <= points; ++i)
            {
                const juce::Point<float> pt (plot.getX() + plot.getWidth() * (float) i / points, fy (db[ch][(size_t) i]));
                if (i == 0) line.startNewSubPath (pt); else line.lineTo (pt);
            }
            juce::Path area (line);
            area.lineTo (plot.getRight(), fy (0.0));
            area.lineTo (plot.getX(), fy (0.0));
            area.closeSubPath();
            g.setColour (c.withAlpha (on ? 0.12f : 0.05f));
            g.fillPath (area);
            g.setColour (c.withAlpha (on ? 1.0f : 0.4f));
            g.strokePath (line, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            const auto s = p.side (ch);
            if (s.preampDb != 0.0f)
            {
                juce::Path pre, dashed;
                pre.startNewSubPath (plot.getX(), fy (s.preampDb));
                pre.lineTo (plot.getRight(), fy (s.preampDb));
                const float dashes[] = { 5.0f, 4.0f };
                juce::PathStrokeType (1.0f).createDashedStroke (dashed, pre, dashes, 2);
                g.setColour ((channels > 1 ? c : theme::text).withAlpha (0.45f));
                g.fillPath (dashed);
                g.setFont (theme::font (11.0f));
                g.drawText ((channels > 1 ? juce::String (ch == 0 ? "L " : "R ") : juce::String()) + "level " + juce::String (s.preampDb, 1) + " dB",
                            juce::Rectangle<float> (plot.getRight() - 140.0f, fy (s.preampDb) - 16.0f, 136.0f, 14.0f), juce::Justification::centredRight);
            }
            for (size_t k = 0; k < s.bands.size(); ++k)
            {
                const auto& band = s.bands[k];
                const juce::Point<float> pt (fx (juce::jlimit (lo, hi, (double) band.freq)), fy (p.magnitudeDb (band.freq, 48000.0, ch)));
                g.setColour ((channels > 1 ? c : theme::bandColour ((int) k % 8)).withAlpha (on ? 1.0f : 0.5f));
                g.fillEllipse (juce::Rectangle<float> (9.0f, 9.0f).withCentre (pt));
                g.setColour (theme::bg);
                g.drawEllipse (juce::Rectangle<float> (9.0f, 9.0f).withCentre (pt), 1.2f);
            }
        }
        if (channels > 1)
            drawLegend (g, getLocalBounds().withTrimmedRight (16).removeFromTop (26),
                        { { "Left", channelColour (0, 2) }, { "Right", channelColour (1, 2) } });

        if (plot.contains (hover))
        {
            const double f = lo * std::pow (hi / lo, (double) ((hover.x - plot.getX()) / plot.getWidth()));
            g.setColour (theme::text.withAlpha (0.3f));
            g.drawVerticalLine (juce::roundToInt (hover.x), plot.getY(), plot.getBottom());
            const auto t = hz (f) + "   " + (p.separate ? "L " + juce::String (p.magnitudeDb (f, 48000.0, 0), 1) + " dB   R " + juce::String (p.magnitudeDb (f, 48000.0, 1), 1)
                                                   : juce::String (p.magnitudeDb (f), 1)) + " dB";
            g.setFont (theme::font (12.0f));
            const float w = (float) juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), t) + 16.0f;
            auto box = juce::Rectangle<float> (hover.x + 10.0f, plot.getY() + 6.0f, w, 20.0f);
            if (box.getRight() > plot.getRight()) box.setX (hover.x - 10.0f - w);
            g.setColour (theme::raised.withAlpha (0.95f));
            g.fillRoundedRectangle (box, 4.0f);
            g.setColour (theme::text);
            g.drawText (t, box, juce::Justification::centred);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override { hover = e.position; repaint(); }
    void mouseExit (const juce::MouseEvent&) override   { hover = { -1.0f, -1.0f }; repaint(); }

private:
    juce::Colour channelColour (int ch, int channels) const { return channels > 1 && ch == 1 ? theme::accentVirt : view.colour; }

    CorrectionPage& view;
    juce::Point<float> hover { -1.0f, -1.0f };
};

//==============================================================================
CorrectionPage::CorrectionPage (CorrectionTarget& t) : target (t), colour (t.colour())
{
    curve = std::make_unique<Curve> (*this);
    addAndMakeVisible (*curve);

    addAndMakeVisible (level);
    level.setSliderStyle (juce::Slider::LinearHorizontal);
    level.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    level.setRange (-30.0, 12.0, 0.1);
    level.setDoubleClickReturnValue (true, 0.0);
    level.setColour (juce::Slider::trackColourId, colour);
    level.setTooltip ("Level of the whole correction (preamp). Lower it if boosts make the output clip, raise it if the "
                      "correction made the output quieter. Double click = 0 dB.");
    level.onValueChange = [this] { setPreamp ((float) level.getValue()); };

    for (auto* b : { &validateButton, &exportButton, &clearButton }) addAndMakeVisible (*b);
    exportButton.setTooltip ("Saves this correction as a text file for Equalizer APO: preamp, filters and, if measured, left / right "
                             "and the time alignment.");
    exportButton.onClick = [this] { exportApo(); };
    validateButton.setColour (juce::TextButton::buttonColourId, theme::accentIn);
    validateButton.setColour (juce::TextButton::textColourOffId, theme::bg);
    setCanValidate (false);
    validateButton.onClick = [this] { if (onValidate) onValidate(); };
    clearButton.onClick = [this] { clear(); };

    profileChanged();
    startTimerHz (10);
}

CorrectionPage::~CorrectionPage() { stopTimer(); }

void CorrectionPage::timerCallback()
{
    if (target.getVersion() != shownVersion) profileChanged();
    if (target.isOn() != shownOn)
    {
        shownOn = target.isOn();
        curve->repaint();
    }
}

void CorrectionPage::profileChanged()
{
    shownVersion = target.getVersion();
    const auto& p = target.getProfile();
    const bool has = ! p.isEmpty();
    if (! level.isMouseButtonDown()) level.setValue (p.preampDb, juce::dontSendNotification);
    level.setEnabled (has);
    exportButton.setEnabled (has);
    clearButton.setEnabled (has);
    resized();   // the band list's height follows the number of bands
    curve->repaint();
    repaint();
}

void CorrectionPage::setPreamp (float db)
{
    auto p = target.getProfile();
    if (p.isEmpty() || p.preampDb == db) return;
    // Separate channels move together: their difference (the balance) stays.
    if (p.separate) p.right.preampDb = juce::jlimit (-30.0f, 12.0f, p.right.preampDb + db - p.preampDb);
    p.design.levelOffsetDb += db - p.preampDb;   // kept when the Edit page designs the bands again
    p.preampDb = db;
    target.setProfile (p);
    shownVersion = target.getVersion();   // the slider is the source: no need to set it back
    curve->repaint();
    repaint (levelInfoArea);
    repaint (infoArea);
}

void CorrectionPage::setCanValidate (bool can)
{
    validateButton.setEnabled (can);
    validateButton.setTooltip (can ? "Measures again through this correction and shows how well it works; if something is left in the "
                                     "bass, it can be refined."
                                   : "Validating needs a correction made from a measurement: load one from the list on the left, "
                                     "or calibrate the speakers.");
}

void CorrectionPage::exportApo()
{
    const auto& p = target.getProfile();
    if (p.isEmpty()) return;
    saveAs (this, chooser, "Export correction for Equalizer APO", p.name, "*.txt",
            [text = p.toApo()] (const juce::File& f) { return f.replaceWithText (text); });
}

void CorrectionPage::clear()
{
    if (target.getProfile().isEmpty()) return;
    juce::Component::SafePointer<CorrectionPage> safe (this);
    dialog::confirm (this, dialog::Kind::danger, "Clear correction?",
                     "The profile \"" + target.getProfile().name + "\" is removed from " + target.label()
                     + ". It stays in the saved corrections.", "Clear", [safe]
    {
        if (safe == nullptr) return;
        safe->target.setOn (false);
        safe->target.setProfile ({});
    });
}

//==============================================================================
void CorrectionPage::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);
    const auto& p = target.getProfile();

    // Cards: curve (with the buttons above it), level, bands.
    auto card = [&g] (juce::Rectangle<int> r)
    {
        g.setColour (theme::panel);
        g.fillRoundedRectangle (r.toFloat(), 8.0f);
    };
    card (curve->getBounds().expanded (kPad, 0).withTop (validateButton.getY() - kPad).withBottom (curve->getBottom() + kPad));
    card (levelArea.expanded (kPad, 8));
    card (bandsArea.expanded (kPad, 8));

    g.setColour (p.isEmpty() ? theme::textFaint : theme::text);
    g.setFont (theme::font (14.0f, true));
    auto info = infoArea;
    g.drawText (p.isEmpty() ? juce::String ("No profile") : p.name, info.removeFromTop (18), juce::Justification::centredLeft, true);
    g.setColour (theme::textDim);
    g.setFont (theme::font (12.0f));
    g.drawText (p.isEmpty() ? juce::String ("Measure the room or import a file")
                            : p.describe() + (p.separate ? juce::String ("   -   left and right speaker corrected separately") : juce::String()),
                info, juce::Justification::centredLeft, true);

    // Level row
    {
        auto r = levelArea;
        g.setColour (theme::text);
        g.setFont (theme::font (13.0f, true));
        g.drawText ("Level", r.removeFromLeft (60), juce::Justification::centredLeft);
        g.setFont (theme::font (13.0f));
        g.drawText (p.isEmpty() ? juce::String ("-") : p.separate ? "L " + juce::String (p.preampDb, 1) + "  R " + juce::String (p.right.preampDb, 1)
                                                    : juce::String (p.preampDb, 1) + " dB", level.getBounds().withX (level.getRight() + 10).withWidth (90),
                    juce::Justification::centredLeft);

        if (! p.isEmpty())
        {
            const double boost = p.maxBoostDb(), over = p.maxGainDb();
            const bool limiter = target.hasLimiter();
            juce::String text;
            juce::Colour c = theme::textDim;
            if (over > 0.05)
            {
                text << "Boosts reach " << juce::String (over, 1) << " dB above the input level: loud passages can clip. "
                     << (limiter ? "The output's limiter catches them." : "Lower the level or put a limiter after it.");
                c = limiter ? theme::textDim : theme::warn;
            }
            else
                text << "Highest boost " << juce::String (boost, 1) << " dB: with this level nothing can clip.";
            g.setColour (c);
            g.setFont (theme::font (12.0f));
            g.drawFittedText (text, levelInfoArea, juce::Justification::centredLeft, 2);
        }
    }

    // Bands
    {
        auto r = bandsArea;
        g.setColour (theme::text);
        g.setFont (theme::font (13.0f, true));
        g.drawText ("Bands", r.removeFromTop (22), juce::Justification::centredLeft);
        if (p.bands.empty() && p.right.bands.empty())
        {
            g.setColour (theme::textFaint);
            g.setFont (theme::font (12.0f));
            g.drawText ("-", r.removeFromTop (20), juce::Justification::centredLeft);
            return;
        }
        const int perRow = juce::jlimit (1, 6, r.getWidth() / 190), cw = r.getWidth() / perRow;
        auto drawSide = [&] (const std::vector<CorrectionProfile::Band>& bands)
        {
            const int rows = ((int) bands.size() + perRow - 1) / perRow;
            auto block = r.removeFromTop (rows * 24);
            for (size_t k = 0; k < bands.size(); ++k)
            {
                const auto& b = bands[k];
                auto cell = juce::Rectangle<int> (block.getX() + (int) (k % (size_t) perRow) * cw, block.getY() + (int) (k / (size_t) perRow) * 24, cw - 6, 20);
                if (cell.getBottom() > bandsArea.getBottom()) break;
                g.setColour (theme::bandColour ((int) k % 8).withAlpha (0.15f));
                g.fillRoundedRectangle (cell.toFloat(), 5.0f);
                g.setColour (theme::bandColour ((int) k % 8));
                g.setFont (theme::font (12.0f, true));
                g.drawText (juce::String ((int) k + 1), cell.removeFromLeft (22), juce::Justification::centred);
                g.setColour (theme::text);
                g.setFont (theme::font (12.0f));
                juce::String s = (b.type == ParamEq::Bell ? juce::String() : typeName (b.type) + "  ") + hz (b.freq);
                if (ParamEq::typeUsesGain (b.type)) s << "  " << juce::String (b.gainDb, 1) << " dB";
                s << "  Q " << juce::String (b.q, 2);
                g.drawText (s, cell, juce::Justification::centredLeft, true);
            }
        };
        if (! p.separate)
            return drawSide (p.bands);
        for (int ch = 0; ch < 2; ++ch)   // left, then right, each under its own title
        {
            g.setColour (ch == 0 ? colour : theme::accentVirt);
            g.setFont (theme::font (12.0f, true));
            g.drawText (ch == 0 ? "Left" : "Right", r.removeFromTop (20), juce::Justification::centredLeft);
            drawSide (p.side (ch).bands);
            r.removeFromTop (6);
        }
    }
}
void CorrectionPage::resized()
{
    auto r = getLocalBounds().reduced (kPad + 12, kPad + 8);

    const auto& prof = target.getProfile();
    validateButton.setVisible (onValidate != nullptr);
    const int bandRows = juce::jmax (1, ((int) prof.bands.size() + 3) / 4) + (prof.separate ? juce::jmax (1, ((int) prof.right.bands.size() + 3) / 4) : 0);
    bandsArea = r.removeFromBottom (22 + (prof.separate ? 52 : 0) + juce::jlimit (1, 10, bandRows) * 24);
    r.removeFromBottom (24);
    levelArea = r.removeFromBottom (32);
    r.removeFromBottom (24);

    auto head = r.removeFromTop (44);
    clearButton.setBounds (head.removeFromRight (80).withTrimmedTop (14));
    head.removeFromRight (kGap);
    exportButton.setBounds (head.removeFromRight (120).withTrimmedTop (14));
    head.removeFromRight (kGap);
    validateButton.setBounds (head.removeFromRight (190).withTrimmedTop (14));
    head.removeFromRight (kGap * 2);
    infoArea = head.withTrimmedTop (8);
    r.removeFromTop (12);
    curve->setBounds (r);

    auto lv = levelArea;
    lv.removeFromLeft (60);
    level.setBounds (lv.removeFromLeft (juce::jmin (360, lv.getWidth() / 2)));
    lv.removeFromLeft (110);
    levelInfoArea = lv;
}

} // namespace hydra

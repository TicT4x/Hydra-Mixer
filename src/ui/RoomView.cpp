#include "RoomView.h"

namespace hydra
{
namespace
{
// Choices of the setup and the Auto EQ (combo box item i+1 = value i).
const std::vector<double> kLengths   { 2.0, 5.0, 10.0 };
const std::vector<double> kRepeats   { 1.0, 2.0, 4.0 };
const std::vector<double> kLevels    { -30.0, -24.0, -18.0, -15.0, -12.0, -9.0, -6.0 };
const std::vector<double> kEqLow     { 20.0, 25.0, 30.0, 40.0, 60.0 };
const std::vector<double> kEqHigh    { 200.0, 300.0, 500.0, 1000.0, 2000.0, 5000.0, 20000.0 };
const std::vector<double> kCuts      { 6.0, 9.0, 12.0, 15.0, 18.0 };
const std::vector<double> kBoosts    { 0.0, 3.0, 6.0 };
const std::vector<double> kSmoothing { 3.0, 6.0, 12.0, 24.0, 0.0 };   // 1/n octave, 0 = none
const std::vector<double> kWfRanges  { 300.0, 500.0, 1000.0, 5000.0, 20000.0 };
const std::vector<double> kSpans     { 50.0, 100.0, 300.0, 1000.0 };

constexpr int kPad = 14, kCaptionH = 16, kRowH = 28, kGap = 8, kTitleH = 26;

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

juce::String titled (const juce::String& code, const juce::String& name)
{
    return name.isEmpty() || name == code ? code : code + "  " + name;
}
juce::String inputTitle (const Engine& e, int i) { return titled ("IN " + juce::String (i + 1), e.inputs[(size_t) i].name); }
juce::String busTitle (const Engine& e, int b)   { return titled (busCode (b), e.buses[(size_t) b].name); }
juce::String channelTitle (const Engine& e, int sel)
{
    return sel < kMaxInputs ? inputTitle (e, sel) : busTitle (e, sel - kMaxInputs);
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
            g.setColour (theme::outline.withAlpha (major ? 0.9f : 0.4f));
            g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());
            if (labels && major)
            {
                g.setColour (theme::textFaint);
                g.drawText (shortHz (f), juce::Rectangle<float> (x - 24.0f, plot.getBottom() + 3.0f, 48.0f, 14.0f), juce::Justification::centred);
            }
        }
}
} // namespace

//==============================================================================
class RoomView::Chart final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit Chart (RoomView& v) : view (v) {}

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        g.setColour (theme::panel);
        g.fillRoundedRectangle (b, 8.0f);

        auto* m = view.current();
        if (m == nullptr)
            return empty (g, "No measurement yet.\nChoose a microphone and a speaker on the left and start a measurement.");
        if (! m->analysed)
            return empty (g, "Analysing...");
        switch (view.mode)
        {
            case Mode::response:  paintResponse (g, *m); break;
            case Mode::waterfall: paintWaterfall (g, *m); break;
            case Mode::decay:     paintDecay (g, *m); break;
            case Mode::impulse:   paintImpulse (g, *m); break;
        }
    }

    void mouseMove (const juce::MouseEvent& e) override { hover = e.position; repaint(); }
    void mouseExit (const juce::MouseEvent&) override   { hover = { -1.0f, -1.0f }; repaint(); }
    void mouseDown (const juce::MouseEvent&) override   { dragStart = tilt; }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (view.mode != Mode::waterfall) return;
        tilt.x = juce::jlimit (0.0f, 0.45f, dragStart.x - (float) e.getDistanceFromDragStartX() / (float) juce::jmax (1, getWidth()));
        tilt.y = juce::jlimit (0.0f, 0.6f, dragStart.y - (float) e.getDistanceFromDragStartY() / (float) juce::jmax (1, getHeight()));
        repaint();
    }
    void mouseDoubleClick (const juce::MouseEvent&) override { tilt = { 0.22f, 0.40f }; repaint(); }

private:
    void empty (juce::Graphics& g, const juce::String& text)
    {
        g.setColour (theme::textDim);
        g.setFont (theme::font (14.0f));
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

    void paintResponse (juce::Graphics& g, const RoomMeasurement& m)
    {
        const auto& a = m.analysis;
        const auto plot = getLocalBounds().toFloat().withTrimmedLeft (48.0f).withTrimmedRight (14.0f).withTrimmedTop (30.0f).withTrimmedBottom (24.0f);
        constexpr double lo = 20.0, hi = 20000.0, top = 18.0, bottom = -30.0;
        auto fx = [&] (double f) { return plot.getX() + plot.getWidth() * (float) (std::log (f / lo) / std::log (hi / lo)); };
        auto fy = [&] (double db) { return plot.getY() + plot.getHeight() * (float) ((top - juce::jlimit (bottom, top, db)) / (top - bottom)); };
        auto xToF = [&] (float x) { return lo * std::pow (hi / lo, (double) ((x - plot.getX()) / plot.getWidth())); };

        // EQ range
        const double eqLo = valueOf (view.eqLowBox, kEqLow, 25.0), eqHi = valueOf (view.eqHighBox, kEqHigh, 500.0);
        g.setColour (theme::warn.withAlpha (0.05f));
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (fx (eqLo), plot.getY(), fx (eqHi), plot.getBottom()));

        drawFreqGrid (g, plot, lo, hi, true);
        g.setFont (theme::font (11.0f));
        for (double db = bottom; db <= top + 0.1; db += 6.0)
        {
            const float y = fy (db);
            g.setColour (theme::outline.withAlpha (db == 0.0 ? 1.0f : 0.5f));
            g.drawHorizontalLine (juce::roundToInt (y), plot.getX(), plot.getRight());
            g.setColour (theme::textFaint);
            g.drawText ((db > 0 ? "+" : "") + juce::String (juce::roundToInt (db)) + " dB",
                        juce::Rectangle<float> (0.0f, y - 7.0f, plot.getX() - 6.0f, 14.0f), juce::Justification::centredRight);
        }

        const double ref = a.referenceDb, sm = view.responseSmoothing();
        const double fLo = a.sweepStartHz, fHi = a.sweepEndHz;
        auto relative = [ref] (std::vector<double> v) { for (auto& x : v) x -= ref; return v; };
        const auto raw = relative (a.magDb);
        const auto smoothed = relative (room::smooth (a.magDb, sm));

        g.saveState();
        g.reduceClipRegion (plot.expanded (1.0f).toNearestInt());
        auto stroke = [&] (const juce::Path& p, juce::Colour c, float w)
        {
            g.setColour (c);
            g.strokePath (p, juce::PathStrokeType (w, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        };

        std::vector<std::pair<juce::String, juce::Colour>> legend;
        if (sm > 0.0)
            stroke (curve (a.freqs, raw, fLo, fHi, fx, fy), theme::textFaint.withAlpha (0.5f), 1.0f);
        if (auto* c = view.compared(); c != nullptr && c->analysed)
        {
            // Relative to this measurement's reference: before / after shows the real level difference.
            auto other = room::smooth (c->analysis.magDb, sm);
            for (auto& x : other) x -= ref;
            stroke (curve (c->analysis.freqs, other, c->analysis.sweepStartHz, c->analysis.sweepEndHz, fx, fy), theme::accentVirt.withAlpha (0.85f), 1.6f);
            legend.push_back ({ c->name, theme::accentVirt });
        }
        stroke (curve (a.freqs, smoothed, fLo, fHi, fx, fy), theme::accentIn, 2.0f);
        legend.insert (legend.begin(), { "Response", theme::accentIn });

        const auto& eq = view.eq;
        if (! eq.bands.empty())
        {
            const float ty = fy (eq.targetDb);
            juce::Path target;
            target.startNewSubPath (fx (eqLo), ty);
            target.lineTo (fx (eqHi), ty);
            const float dashes[] = { 5.0f, 4.0f };
            juce::Path dashed;
            juce::PathStrokeType (1.2f).createDashedStroke (dashed, target, dashes, 2);
            g.setColour (theme::text.withAlpha (0.6f));
            g.fillPath (dashed);
            stroke (curve (a.freqs, eq.eqDb, fLo, fHi, fx, fy), theme::warn, 1.5f);
            stroke (curve (a.freqs, eq.correctedDb, fLo, fHi, fx, fy), theme::good, 1.7f);
            legend.push_back ({ "EQ", theme::warn });
            legend.push_back ({ "With EQ", theme::good });
            legend.push_back ({ "Target", theme::text.withAlpha (0.6f) });
        }

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

        // Read-out under the mouse.
        if (plot.contains (hover))
        {
            const double f = xToF (hover.x);
            g.setColour (theme::text.withAlpha (0.35f));
            g.drawVerticalLine (juce::roundToInt (hover.x), plot.getY(), plot.getBottom());
            juce::String t = hz (f) + "   " + juce::String (room::valueAt (a.freqs, smoothed, f), 1) + " dB";
            if (! eq.bands.empty())
                t << "   EQ " << juce::String (room::valueAt (a.freqs, eq.eqDb, f), 1) << " dB";
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
        drawLegend (g, getLocalBounds().withTrimmedRight (14).removeFromTop (28), legend);
    }

    void paintWaterfall (juce::Graphics& g, const RoomMeasurement& m)
    {
        const auto& a = m.analysis;
        const int slices = (int) a.waterfall.size();
        if (slices < 2) return empty (g, "No waterfall data.");
        const double lo = juce::jmax (20.0, a.sweepStartHz), hi = juce::jmin (view.waterfallMaxHz(), a.sweepEndHz);
        constexpr double top = 3.0, floorDb = -45.0;

        const auto area = getLocalBounds().toFloat().withTrimmedLeft (52.0f).withTrimmedRight (70.0f).withTrimmedTop (34.0f).withTrimmedBottom (30.0f);
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
        const double step = span > 200.0 ? 50.0 : 20.0;
        for (double t = 0.0; t <= span + 0.1; t += step)
        {
            const float d = 1.0f - (float) (t / span);
            const juce::Point<float> pt (front.getRight() + ox * d, front.getBottom() - oy * d);
            g.drawText (juce::String (juce::roundToInt (t)) + " ms", juce::Rectangle<float> (pt.x + 6.0f, pt.y - 7.0f, 60.0f, 14.0f),
                        juce::Justification::centredLeft);
        }
        g.setColour (theme::textDim);
        g.setFont (theme::font (12.0f));
        g.drawText ("Drag to tilt, double click to reset. Ridges that reach far to the front are frequencies that keep ringing (room modes).",
                    getLocalBounds().reduced (14, 8).removeFromTop (18), juce::Justification::centredLeft);
    }

    void paintDecay (juce::Graphics& g, const RoomMeasurement& m)
    {
        const auto& bands = m.analysis.decay;
        const auto plot = getLocalBounds().toFloat().withTrimmedLeft (52.0f).withTrimmedRight (20.0f).withTrimmedTop (34.0f).withTrimmedBottom (40.0f);
        double maxT = 0.8;
        for (auto& d : bands) maxT = juce::jmax (maxT, d.t20 * 1.15, d.edt * 1.15);
        maxT = std::ceil (maxT * 5.0) / 5.0;
        auto fy = [&] (double t) { return plot.getBottom() - plot.getHeight() * (float) (juce::jlimit (0.0, maxT, t) / maxT); };

        // Typical range for living rooms, home studios and control rooms.
        g.setColour (theme::good.withAlpha (0.07f));
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (plot.getX(), fy (0.5), plot.getRight(), fy (0.2)));

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
        drawLegend (g, getLocalBounds().withTrimmedRight (14).removeFromTop (30),
                    { { "T20 (reverberation time, RT60)", theme::accentIn }, { "EDT (early decay)", theme::warn },
                      { "0.2 - 0.5 s: typical for small rooms / studios", theme::good.withAlpha (0.3f) } });
    }

    void paintImpulse (juce::Graphics& g, const RoomMeasurement& m)
    {
        const auto& a = m.analysis;
        const auto plot = getLocalBounds().toFloat().withTrimmedLeft (52.0f).withTrimmedRight (20.0f).withTrimmedTop (34.0f).withTrimmedBottom (26.0f);
        const double t0 = -5.0, t1 = view.impulseSpanMs(), bottom = -80.0;
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
        juce::Path area (p);
        area.lineTo (p.getCurrentPosition().x, plot.getBottom());
        area.lineTo (fx (juce::jmax (t0, a.etcMs.empty() ? t0 : a.etcMs.front())), plot.getBottom());
        area.closeSubPath();
        g.setColour (theme::accentIn.withAlpha (0.12f));
        g.fillPath (area);
        g.setColour (theme::accentIn);
        g.strokePath (p, juce::PathStrokeType (1.3f));

        g.setColour (theme::textDim);
        g.setFont (theme::font (12.0f));
        g.drawText ("Energy over time, 0 ms = direct sound (" + juce::String (m.ir.latencyMs, 1)
                        + " ms after the output: device latency + distance). Spikes after it are reflections.",
                    getLocalBounds().reduced (14, 8).removeFromTop (18), juce::Justification::centredLeft);
    }

    RoomView& view;
    juce::Point<float> hover { -1.0f, -1.0f };
    juce::Point<float> tilt { 0.22f, 0.40f }, dragStart;
};

//==============================================================================
RoomView::RoomView (UiContext& c) : ctx (c)
{
    chart = std::make_unique<Chart> (*this);
    addAndMakeVisible (*chart);

    auto combo = [this] (juce::ComboBox& box, const juce::String& caption)
    {
        addAndMakeVisible (box);
        if (caption.isNotEmpty()) captions.push_back ({ &box, caption });
    };

    // Setup
    combo (micBox, "Microphone (input)");
    combo (micChannelBox, "Channel");
    combo (busBox, "Speaker (output bus)");
    combo (speakerBox, "Plays on");
    combo (lengthBox, "Sweep");
    combo (repeatsBox, "Sweeps");
    combo (levelBox, "Level");
    fill (micChannelBox, { "Left", "Right" }, 0);
    fill (speakerBox, { "Left + Right", "Left", "Right" }, 0);
    fill (lengthBox, { "2 s", "5 s", "10 s" }, 1);
    fill (repeatsBox, { "1", "2 (average)", "4 (average)" }, 1);
    fill (levelBox, { "-30 dBFS", "-24 dBFS", "-18 dBFS", "-15 dBFS", "-12 dBFS", "-9 dBFS", "-6 dBFS" }, 3);
    micBox.setTooltip ("The input strip whose source is the measurement microphone. Its signal is taken straight from the "
                       "device, before trim, denoiser, EQ, gate, compressor and plugins.");
    micChannelBox.setTooltip ("Which channel of the input's source carries the microphone.");
    busBox.setTooltip ("The bus that plays the sweep. During the measurement the sweep replaces everything on this bus, "
                       "and its EQ, plugins, fader and limiter are skipped.");
    speakerBox.setTooltip ("Measure one speaker at a time for the most precise result, or both together for the sum you hear.");
    lengthBox.setTooltip ("Longer sweeps give more signal-to-noise (and resolve low frequencies better).");
    repeatsBox.setTooltip ("Several sweeps in a row are averaged: less background noise in the result.");
    levelBox.setTooltip ("Level of the sweep. Start low and go up until the microphone shows a healthy level (around -20 to -6 dBFS).");
    busBox.onChange = [this] { if (! targetChosen) refreshChannelBoxes(); };

    addAndMakeVisible (muteOthers);
    addAndMakeVisible (throughEq);
    muteOthers.setToggleState (true, juce::dontSendNotification);
    muteOthers.setTooltip ("Every other output is silent while measuring (music or voice chat would disturb the measurement).");
    throughEq.setTooltip ("The sweep passes the output bus's EQ: measure again after applying a correction to see the result. "
                          "Plugins, fader and limiter are still skipped.");
    addAndMakeVisible (startButton);
    startButton.onClick = [this] { startOrStop(); };

    // Library
    list.setRowHeight (44);
    list.setColour (juce::ListBox::backgroundColourId, theme::panel);
    list.setOutlineThickness (0);
    addAndMakeVisible (list);
    for (auto* b : { &importButton, &exportButton, &deleteButton }) addAndMakeVisible (*b);
    importButton.onClick = [this] { importFile(); };
    exportButton.onClick = [this] { showExportMenu(); };
    deleteButton.onClick = [this] { deleteSelected(); };
    deleteButton.setTooltip ("Deletes the selected measurement from the library.");
    list.setTooltip ("Saved measurements (%APPDATA%\\HydraMixer\\Measurements). Double click to rename.");

    // Chart controls
    for (auto* t : { &tabResponse, &tabWaterfall, &tabDecay, &tabImpulse })
    {
        t->showArrow = false;
        t->centred = true;
        t->colour = theme::text;
        addAndMakeVisible (*t);
    }
    tabResponse.onClick  = [this] { setMode (Mode::response); };
    tabWaterfall.onClick = [this] { setMode (Mode::waterfall); };
    tabDecay.onClick     = [this] { setMode (Mode::decay); };
    tabImpulse.onClick   = [this] { setMode (Mode::impulse); };
    addAndMakeVisible (viewOption);
    addAndMakeVisible (compareBox);
    viewOption.onChange = [this] { chart->repaint(); };
    compareBox.setTooltip ("Shows a second measurement in the response, e.g. before / after a correction.");
    compareBox.onChange = [this]
    {
        comparePtr = nullptr;
        const int i = compareBox.getSelectedId() - 2;
        if (juce::isPositiveAndBelow (i, (int) library.size()))
        {
            comparePtr = library[(size_t) i].get();
            analyseIfNeeded (comparePtr);
        }
        chart->repaint();
    };

    // Auto EQ
    combo (eqLowBox, "From");
    combo (eqHighBox, "To");
    combo (eqBandsBox, "Bands");
    combo (eqCutBox, "Max cut");
    combo (eqBoostBox, "Max boost");
    combo (eqSmoothBox, "Smoothing");
    combo (targetBox, "Apply to");
    fill (eqLowBox, { "20 Hz", "25 Hz", "30 Hz", "40 Hz", "60 Hz" }, 1);
    fill (eqHighBox, { "200 Hz", "300 Hz", "500 Hz", "1 kHz", "2 kHz", "5 kHz", "20 kHz" }, 2);
    fill (eqBandsBox, { "1", "2", "3", "4", "5", "6", "7", "8" }, 7);
    fill (eqCutBox, { "-6 dB", "-9 dB", "-12 dB", "-15 dB", "-18 dB" }, 3);
    fill (eqBoostBox, { "None", "+3 dB", "+6 dB" }, 0);
    fill (eqSmoothBox, { "1/3 oct", "1/6 oct", "1/12 oct", "1/24 oct" }, 2);
    eqLowBox.setTooltip ("Lowest frequency the EQ corrects (below it the speaker's own roll-off should stay).");
    eqHighBox.setTooltip ("Highest frequency the EQ corrects. Room modes live below ~300-500 Hz; above that a correction only "
                          "fits the microphone position.");
    eqBoostBox.setTooltip ("Dips are often cancellations that no boost can fill (it only costs headroom). Keep boosting off "
                           "or small.");
    eqSmoothBox.setTooltip ("How detailed the response is that the EQ follows. 1/12 octave suits room modes.");
    targetBox.setTooltip ("Channel whose EQ gets the bands (its 8 bands are replaced). Usually the bus you measured.");
    for (auto* b : { &eqLowBox, &eqHighBox, &eqBandsBox, &eqCutBox, &eqBoostBox, &eqSmoothBox })
        b->onChange = [this] { updateEq(); };
    targetBox.onChange = [this] { targetChosen = true; updateApplyButton(); };
    addAndMakeVisible (applyButton);
    addAndMakeVisible (exportEqButton);
    applyButton.onClick = [this] { applyEq(); };
    exportEqButton.onClick = [this]
    {
        if (eq.bands.empty()) return;
        auto bands = eq.bands;
        const auto title = current() != nullptr ? "Room correction for " + current()->name : juce::String ("Room correction");
        exportTo ("Export EQ", "*.txt", [bands, title] (const juce::File& f) { return measurements::exportEq (bands, title, f.withFileExtension (".txt")); });
    };
    exportEqButton.setTooltip ("Saves the bands as text (Equalizer APO format, works for other EQs too).");

    library = measurements::loadLibrary();
    list.updateContent();
    refreshChannelBoxes();
    setMode (Mode::response);
    select (library.empty() ? -1 : 0);
    status = "Place the microphone at the listening position, pointing up. Keep the room quiet during the measurement.";
    setSize (1320, 860);
    startTimerHz (30);
}

RoomView::~RoomView()
{
    stopTimer();
    if (ctx.engine.getMeasurement() != nullptr)
    {
        ctx.engine.stopMeasurement();
        ctx.engine.finishMeasurement();
    }
}

//==============================================================================
void RoomView::refreshChannelBoxes()
{
    auto& e = ctx.engine;
    const int mic = micBox.getSelectedId(), bus = busBox.getSelectedId(), target = targetBox.getSelectedId();

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
    micBox.setSelectedId (micBox.indexOfItemId (mic) >= 0 ? mic : (firstDevice > 0 ? firstDevice : first), juce::dontSendNotification);

    busBox.clear (juce::dontSendNotification);
    int firstBus = 0;
    for (int b = 0; b < kMaxBuses; ++b)
    {
        if (! e.isBusActive (b)) continue;
        const auto& out = e.buses[(size_t) b].output;
        busBox.addItem (busTitle (e, b) + (out.kind == IoTarget::Kind::None ? juce::String ("   (no device)") : "   -   " + out.describe()), b + 1);
        if (firstBus == 0) firstBus = b + 1;
    }
    busBox.setSelectedId (busBox.indexOfItemId (bus) >= 0 ? bus : firstBus, juce::dontSendNotification);

    targetBox.clear (juce::dontSendNotification);
    for (int b = 0; b < kMaxBuses; ++b)
        if (e.isBusActive (b)) targetBox.addItem (busTitle (e, b), busSelectIndex (b) + 1);
    for (int i = 0; i < kMaxInputs; ++i)
        if (e.isInputActive (i)) targetBox.addItem (inputTitle (e, i), i + 1);
    const int measured = busBox.getSelectedId() > 0 ? busSelectIndex (busBox.getSelectedId() - 1) + 1 : 0;
    targetBox.setSelectedId (targetChosen && targetBox.indexOfItemId (target) >= 0 ? target : measured, juce::dontSendNotification);
    updateApplyButton();
}

int RoomView::targetChannel() const
{
    const int id = targetBox.getSelectedId();
    return id > 0 ? id - 1 : -1;
}

double RoomView::responseSmoothing() const { return mode == Mode::response ? valueOf (viewOption, kSmoothing, 12.0) : 12.0; }
double RoomView::waterfallMaxHz() const    { return mode == Mode::waterfall ? valueOf (viewOption, kWfRanges, 500.0) : 500.0; }
double RoomView::impulseSpanMs() const     { return mode == Mode::impulse ? valueOf (viewOption, kSpans, 300.0) : 300.0; }

RoomMeasurement* RoomView::compared() const
{
    for (auto& m : library)
        if (m.get() == comparePtr && comparePtr != current()) return comparePtr;
    return nullptr;
}

//==============================================================================
void RoomView::startOrStop()
{
    auto& e = ctx.engine;
    if (auto* run = e.getMeasurement(); run != nullptr && ! run->finished())
    {
        e.stopMeasurement();
        status = "Stopping...";
        statusColour = theme::textDim;
        repaint();
        return;
    }

    const int input = micBox.getSelectedId() - 1, bus = busBox.getSelectedId() - 1;
    if (e.getMasterDevice() == nullptr)
        return dialog::message (this, dialog::Kind::warning, "Audio engine not running",
                                "Choose a master device in Settings > Audio Engine first.");
    if (input < 0 || e.inputs[(size_t) input].source.kind == IoTarget::Kind::None)
        return dialog::message (this, dialog::Kind::warning, "No microphone",
                                "Choose an input whose source is the measurement microphone (set its device in the input strip first).");
    if (bus < 0 || e.buses[(size_t) bus].output.kind == IoTarget::Kind::None)
        return dialog::message (this, dialog::Kind::warning, "No speaker",
                                "Choose an output bus that plays on the speakers you want to measure (give it an output device first).");

    room::SweepSpec spec;
    spec.sampleRate = e.getSampleRate();
    spec.endHz = juce::jmin (20000.0, spec.sampleRate * 0.45);
    spec.seconds = valueOf (lengthBox, kLengths, 5.0);
    spec.repeats = (int) valueOf (repeatsBox, kRepeats, 2.0);
    spec.levelDb = valueOf (levelBox, kLevels, -15.0);

    auto run = std::make_unique<MeasureRun>();
    run->input = input;
    run->inputChannel = micChannelBox.getSelectedId() == 2 ? 1 : 0;
    run->bus = bus;
    const int speakers = speakerBox.getSelectedId();
    run->outChannels = speakers == 2 ? 1 : speakers == 3 ? 2 : 3;
    run->muteOthers = muteOthers.getToggleState();
    run->busEq = throughEq.getToggleState();
    run->stimulus = room::makeStimulus (spec);
    run->capture.assign (run->stimulus.size(), 0.0f);

    runSpec = spec;
    runMic = inputTitle (e, input) + (micChannelBox.getSelectedId() == 2 ? " (R)" : "");
    runOut = busTitle (e, bus) + (speakers == 2 ? " (L)" : speakers == 3 ? " (R)" : "");
    runThroughEq = run->busEq;
    stopTicks = 0;
    micHold = 0.0f;
    e.startMeasurement (std::move (run));

    startButton.setButtonText ("Stop");
    for (auto* b : std::initializer_list<juce::Component*> { &micBox, &micChannelBox, &busBox, &speakerBox, &lengthBox, &repeatsBox, &levelBox, &muteOthers, &throughEq })
        b->setEnabled (false);
    status = "Measuring... keep the room quiet.";
    statusColour = theme::text;
    repaint();
}

void RoomView::timerCallback()
{
    auto& e = ctx.engine;
    if (++layoutTick >= 30)   // names and devices of the channels (rebuilt only when they changed)
    {
        layoutTick = 0;
        juce::String sig (e.getLayoutVersion());
        for (int i = 0; i < kMaxInputs; ++i) sig << "|" << e.inputs[(size_t) i].name << e.inputs[(size_t) i].source.describe();
        for (int b = 0; b < kMaxBuses; ++b)  sig << "|" << e.buses[(size_t) b].name << e.buses[(size_t) b].output.describe();
        if (sig != shownChannels && e.getMeasurement() == nullptr
            && ! micBox.isPopupActive() && ! busBox.isPopupActive() && ! targetBox.isPopupActive())
        {
            shownChannels = sig;
            refreshChannelBoxes();
        }
    }

    auto* run = e.getMeasurement();
    if (run == nullptr) return;

    const float pk = run->inputPeak.exchange (0.0f);
    micLevel = pk > micLevel ? pk : micLevel * 0.85f;
    micHold = juce::jmax (micHold, pk);
    if (run->stop.load() && ! run->finished() && ++stopTicks > 15)   // no audio callbacks (engine stopped)
        run->pos.store (run->length);
    repaint (setupArea);

    if (! run->finished()) return;
    const bool stopped = run->stop.load(), noInput = run->noInput.load();
    auto capture = e.finishMeasurement();
    startButton.setButtonText ("Start measurement");
    for (auto* b : std::initializer_list<juce::Component*> { &micBox, &micChannelBox, &busBox, &speakerBox, &lengthBox, &repeatsBox, &levelBox, &muteOthers, &throughEq })
        b->setEnabled (true);
    micLevel = 0.0f;

    if (stopped || capture.empty())
    {
        status = "Measurement stopped.";
        statusColour = theme::textDim;
    }
    else if (noInput)
    {
        status = "The microphone's input delivered no audio (device offline?). Check the input's source and measure again.";
        statusColour = theme::mute;
    }
    else
        measurementFinished (std::move (capture), runSpec, runMic, runOut, runThroughEq);
    repaint();
}

void RoomView::measurementFinished (std::vector<float> capture, room::SweepSpec spec, juce::String mic, juce::String out, bool eqOn)
{
    status = "Analysing...";
    statusColour = theme::text;
    juce::Component::SafePointer<RoomView> safe (this);
    juce::Thread::launch ([safe, capture = std::move (capture), spec, mic, out, eqOn]
    {
        auto m = std::make_shared<RoomMeasurement>();
        m->ir = room::deconvolve (capture, spec);
        m->analysis = room::analyse (m->ir, {}, spec.startHz, spec.endHz);
        m->analysed = true;
        m->spec = spec;
        m->microphone = mic;
        m->output = out;
        m->throughEq = eqOn;
        m->created = juce::Time::getCurrentTime();
        m->name = out + "  ->  " + mic + (eqOn ? "  (with EQ)" : "");
        juce::MessageManager::callAsync ([safe, m]
        {
            if (safe == nullptr) return;
            auto own = std::make_unique<RoomMeasurement> (std::move (*m));
            if (! measurements::saveToLibrary (*own))
                logLine ("Room measurement: cannot save " + own->file.getFullPathName());
            const auto warn = safe->quality (*own);
            safe->status = warn.isNotEmpty() ? warn
                                             : "Done. Direct sound after " + juce::String (own->ir.latencyMs, 1) + " ms, "
                                               + juce::String (juce::roundToInt (own->ir.irSnrDb)) + " dB signal-to-noise. "
                                               + juce::String ((int) own->analysis.resonances.size()) + " resonances found.";
            safe->statusColour = warn.isNotEmpty() ? theme::warn : theme::good;
            safe->library.insert (safe->library.begin(), std::move (own));
            safe->list.updateContent();
            safe->select (0);
        });
    });
    repaint();
}

juce::String RoomView::quality (const RoomMeasurement& m) const
{
    if (m.ir.clipped)
        return "The microphone clipped: lower the level (or the microphone gain) and measure again.";
    if (m.ir.capturePeakDb < -45.0)
        return "The microphone signal was very weak (" + juce::String (juce::roundToInt (m.ir.capturePeakDb))
               + " dBFS): raise the level or the microphone gain.";
    if (m.ir.irSnrDb < 35.0)
        return "Noisy measurement (" + juce::String (juce::roundToInt (m.ir.irSnrDb))
               + " dB signal-to-noise): use more sweeps, a longer sweep or a higher level.";
    return {};
}

//==============================================================================
void RoomView::select (int index)
{
    currentIndex = juce::isPositiveAndBelow (index, (int) library.size()) ? index : -1;
    if (currentIndex >= 0) list.selectRow (currentIndex, false, true);
    else                   list.deselectAllRows();

    compareBox.clear (juce::dontSendNotification);
    compareBox.addItem ("Compare: none", 1);
    for (size_t i = 0; i < library.size(); ++i)
        if ((int) i != currentIndex) compareBox.addItem ("Compare: " + library[i]->name, (int) i + 2);
    int compareId = 1;
    for (size_t i = 0; i < library.size(); ++i)
        if (library[i].get() == comparePtr && (int) i != currentIndex) compareId = (int) i + 2;
    compareBox.setSelectedId (compareId, juce::dontSendNotification);
    if (compareId == 1) comparePtr = nullptr;

    analyseIfNeeded (current());
    updateEq();
    exportButton.setEnabled (current() != nullptr);
    deleteButton.setEnabled (current() != nullptr);
    repaint();
}

void RoomView::selectedRowsChanged (int row)
{
    if (row != currentIndex && row >= 0) select (row);
}

void RoomView::analyseIfNeeded (RoomMeasurement* m)
{
    if (m == nullptr || m->analysed || std::find (analysing.begin(), analysing.end(), m) != analysing.end()) return;
    analysing.push_back (m);
    juce::Component::SafePointer<RoomView> safe (this);
    juce::Thread::launch ([safe, m, ir = m->ir, spec = m->spec]
    {
        auto a = std::make_shared<room::Analysis> (room::analyse (ir, {}, spec.startHz, spec.endHz));
        juce::MessageManager::callAsync ([safe, m, a]
        {
            if (safe == nullptr) return;
            auto& busy = safe->analysing;
            busy.erase (std::remove (busy.begin(), busy.end(), m), busy.end());
            for (auto& x : safe->library)   // still in the library?
                if (x.get() == m)
                {
                    m->analysis = std::move (*a);
                    m->analysed = true;
                    if (m == safe->current()) safe->updateEq();
                    safe->repaint();
                }
        });
    });
}

void RoomView::updateEq()
{
    auto* m = current();
    eq = {};
    if (m != nullptr && m->analysed)
    {
        room::AutoEqSettings s;
        s.lowHz = valueOf (eqLowBox, kEqLow, 25.0);
        s.highHz = valueOf (eqHighBox, kEqHigh, 500.0);
        s.maxBands = eqBandsBox.getSelectedId() > 0 ? eqBandsBox.getSelectedId() : 8;
        s.maxCutDb = valueOf (eqCutBox, kCuts, 15.0);
        s.maxBoostDb = valueOf (eqBoostBox, kBoosts, 0.0);
        s.smoothing = valueOf (eqSmoothBox, kSmoothing, 12.0);
        s.sampleRate = ctx.engine.getSampleRate() > 0.0 ? ctx.engine.getSampleRate() : m->ir.sampleRate;
        eq = room::designEq (m->analysis, s);
    }
    updateApplyButton();
}

void RoomView::updateApplyButton()
{
    auto* m = current();
    const int target = targetChannel();
    applyButton.setButtonText (target >= 0 ? "Apply EQ to " + channelCode (target) : juce::String ("Apply EQ"));
    applyButton.setEnabled (! eq.bands.empty() && target >= 0 && m != nullptr && ! m->throughEq);
    exportEqButton.setEnabled (! eq.bands.empty());
    applyButton.setTooltip (m != nullptr && m->throughEq
                                ? "This measurement already includes the output's EQ. Design a correction from a measurement without it."
                                : "Replaces the 8 bands of the chosen channel's EQ with these bands and switches its EQ on. Ctrl+Z undoes it.");
    repaint();
}

void RoomView::applyEq()
{
    const int target = targetChannel();
    auto* peq = ctx.eqFor (target);
    if (peq == nullptr || eq.bands.empty()) return;
    const auto bands = eq.bands;
    const auto name = channelTitle (ctx.engine, target);
    juce::Component::SafePointer<RoomView> safe (this);
    dialog::confirm (this, dialog::Kind::question, "Apply EQ to " + name + "?",
                     juce::String ((int) bands.size()) + " bands replace the current EQ bands of " + name
                         + ". Ctrl+Z undoes it.\n\nTip: measure again with \"Through the output's EQ\" to check the result.",
                     "Apply", [safe, peq, bands, name]
    {
        auto& e = *peq;
        for (int k = 0; k < ParamEq::kBands; ++k)
        {
            if (k < (int) bands.size())
            {
                const auto& b = bands[(size_t) k];
                e.type[(size_t) k]->set ((float) ParamEq::Bell);
                e.freq[(size_t) k]->set ((float) b.freq);
                e.gain[(size_t) k]->set ((float) b.gainDb);
                e.q[(size_t) k]->set ((float) b.q);
                e.bandOn[(size_t) k]->set (1.0f);
            }
            else
            {
                e.bandOn[(size_t) k]->set (0.0f);
                e.gain[(size_t) k]->set (0.0f);
            }
        }
        e.on->set (1.0f);
        if (safe != nullptr)
        {
            safe->status = "EQ applied to " + name + ".";
            safe->statusColour = theme::good;
            safe->repaint();
        }
    });
}

//==============================================================================
void RoomView::setMode (Mode m)
{
    mode = m;
    tabResponse.active = m == Mode::response;
    tabWaterfall.active = m == Mode::waterfall;
    tabDecay.active = m == Mode::decay;
    tabImpulse.active = m == Mode::impulse;
    for (auto* t : { &tabResponse, &tabWaterfall, &tabDecay, &tabImpulse }) t->repaint();
    updateModeControls();
    chart->setTooltip (m == Mode::waterfall ? "Drag to tilt the view, double click to reset." : juce::String());
    chart->repaint();
}

void RoomView::updateModeControls()
{
    switch (mode)
    {
        case Mode::response:
            fill (viewOption, { "1/3 octave", "1/6 octave", "1/12 octave", "1/24 octave", "No smoothing" }, 2);
            viewOption.setTooltip ("Smoothing of the response curve.");
            break;
        case Mode::waterfall:
            fill (viewOption, { "20 - 300 Hz", "20 - 500 Hz", "20 Hz - 1 kHz", "20 Hz - 5 kHz", "20 Hz - 20 kHz" }, 1);
            viewOption.setTooltip ("Frequency range of the waterfall.");
            break;
        case Mode::impulse:
            fill (viewOption, { "50 ms", "100 ms", "300 ms", "1 s" }, 2);
            viewOption.setTooltip ("Time span shown.");
            break;
        case Mode::decay:
            break;
    }
    viewOption.setVisible (mode != Mode::decay);
    compareBox.setVisible (mode == Mode::response);
}

//==============================================================================
void RoomView::exportTo (const juce::String& title, const juce::String& pattern, std::function<bool (const juce::File&)> write)
{
    auto* m = current();
    const auto base = m != nullptr ? juce::File::createLegalFileName (m->name).replace ("->", "to").replaceCharacters (" ", "_") : juce::String ("Room");
    chooser = std::make_unique<juce::FileChooser> (title,
        juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile (base + pattern.fromLastOccurrenceOf ("*", false, false)),
        pattern);
    juce::Component::SafePointer<RoomView> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe, write] (const juce::FileChooser& fc)
    {
        const auto f = fc.getResult();
        if (safe == nullptr || f == juce::File()) return;
        const bool ok = write (f);
        safe->status = ok ? "Saved " + f.getFileName() + "." : "Could not write " + f.getFullPathName() + ".";
        safe->statusColour = ok ? theme::good : theme::mute;
        safe->repaint();
    });
}

void RoomView::showExportMenu()
{
    auto* m = current();
    if (m == nullptr) return;
    const bool ready = m->analysed;
    juce::Component::SafePointer<RoomView> safe (this);
    juce::PopupMenu menu;
    menu.addItem ("Measurement file (.hydrameasure)...", [safe]
    {
        if (safe == nullptr || safe->current() == nullptr) return;
        safe->exportTo ("Export measurement", juce::String ("*") + measurements::kExtension, [m = *safe->current()] (const juce::File& f)
        {
            return measurements::save (m, f.withFileExtension (measurements::kExtension));
        });
    });
    menu.addItem ("Frequency response (.txt, for REW)...", ready, false, [safe]
    {
        if (safe == nullptr || safe->current() == nullptr) return;
        const double sm = safe->responseSmoothing();
        safe->exportTo ("Export frequency response", "*.txt", [m = *safe->current(), sm] (const juce::File& f)
        {
            return measurements::exportResponse (m, f.withFileExtension (".txt"), sm);
        });
    });
    menu.addItem ("Impulse response (.wav)...", [safe]
    {
        if (safe == nullptr || safe->current() == nullptr) return;
        safe->exportTo ("Export impulse response", "*.wav", [m = *safe->current()] (const juce::File& f)
        {
            return measurements::exportImpulse (m, f.withFileExtension (".wav"));
        });
    });
    menu.addItem ("Waterfall (.csv)...", ready, false, [safe]
    {
        if (safe == nullptr || safe->current() == nullptr) return;
        safe->exportTo ("Export waterfall", "*.csv", [m = *safe->current()] (const juce::File& f)
        {
            return measurements::exportWaterfall (m, f.withFileExtension (".csv"));
        });
    });
    menu.addItem ("Chart as image (.png)...", ready, false, [safe]
    {
        if (safe == nullptr) return;
        auto image = safe->chart->createComponentSnapshot (safe->chart->getLocalBounds(), true, 2.0f);
        safe->exportTo ("Export chart", "*.png", [image] (const juce::File& f)
        {
            const auto file = f.withFileExtension (".png");
            file.deleteFile();
            juce::FileOutputStream out (file);
            juce::PNGImageFormat png;
            return out.openedOk() && png.writeImageToStream (image, out);
        });
    });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&exportButton).withMinimumWidth (260));
}

void RoomView::importFile()
{
    chooser = std::make_unique<juce::FileChooser> ("Import room measurement", juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
                                                   juce::String ("*") + measurements::kExtension);
    juce::Component::SafePointer<RoomView> safe (this);
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
        safe->library.insert (safe->library.begin(), std::move (m));
        safe->list.updateContent();
        safe->select (0);
    });
}

void RoomView::deleteSelected()
{
    auto* m = current();
    if (m == nullptr) return;
    juce::Component::SafePointer<RoomView> safe (this);
    dialog::confirm (this, dialog::Kind::danger, "Delete measurement?", "\"" + m->name + "\" is removed from the library.", "Delete", [safe, m]
    {
        if (safe == nullptr) return;
        auto& lib = safe->library;
        const auto it = std::find_if (lib.begin(), lib.end(), [m] (const auto& x) { return x.get() == m; });
        if (it == lib.end()) return;
        m->file.deleteFile();
        const int index = (int) (it - lib.begin());
        if (safe->comparePtr == m) safe->comparePtr = nullptr;
        lib.erase (it);
        safe->list.updateContent();
        safe->currentIndex = -1;
        safe->select (juce::jmin (index, (int) lib.size() - 1));
    });
}

void RoomView::startRename (int row)
{
    if (! juce::isPositiveAndBelow (row, (int) library.size())) return;
    finishRename (true);
    renameRow = row;
    renameEditor = std::make_unique<juce::TextEditor>();
    auto& ed = *renameEditor;
    addAndMakeVisible (ed);
    ed.setFont (theme::font (13.5f, true));
    ed.setText (library[(size_t) row]->name, false);
    const auto r = list.getRowPosition (row, true).translated (list.getX(), list.getY());
    ed.setBounds (r.withHeight (24).reduced (6, 0).translated (0, 3));
    ed.onReturnKey = [this] { finishRename (true); };
    ed.onEscapeKey = [this] { finishRename (false); };
    ed.onFocusLost = [this] { finishRename (true); };
    ed.grabKeyboardFocus();
    ed.selectAll();
}

void RoomView::finishRename (bool apply)
{
    if (renameEditor == nullptr || renameRow < 0) return;
    const int row = renameRow;
    renameRow = -1;
    const auto text = renameEditor->getText().trim();
    renameEditor->setVisible (false);
    // Deleted later: this runs inside the editor's own callbacks.
    juce::Component::SafePointer<RoomView> safe (this);
    juce::MessageManager::callAsync ([safe] { if (safe != nullptr && safe->renameRow < 0) safe->renameEditor.reset(); });
    if (apply && text.isNotEmpty() && juce::isPositiveAndBelow (row, (int) library.size()))
    {
        auto& m = *library[(size_t) row];
        m.name = text;
        measurements::save (m, m.file);
        list.repaintRow (row);
        select (currentIndex);
    }
}

void RoomView::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (! juce::isPositiveAndBelow (row, (int) library.size())) return;
    const auto& m = *library[(size_t) row];
    auto r = juce::Rectangle<int> (0, 0, w, h).reduced (4, 2);
    if (selected)
    {
        g.setColour (theme::accentIn.withAlpha (0.14f));
        g.fillRoundedRectangle (r.toFloat(), 6.0f);
    }
    r.reduce (8, 3);
    g.setColour (selected ? theme::text : theme::text.withAlpha (0.9f));
    g.setFont (theme::font (13.5f, true));
    g.drawText (m.name, r.removeFromTop (19), juce::Justification::centredLeft, true);
    g.setColour (theme::textFaint);
    g.setFont (theme::font (11.5f));
    juce::String info = m.created.formatted ("%Y-%m-%d %H:%M") + "   " + juce::String (m.spec.sampleRate / 1000.0, 1) + " kHz, "
                        + juce::String (juce::roundToInt (m.spec.seconds)) + " s x " + juce::String (m.spec.repeats);
    if (m.throughEq) info << "   with EQ";
    g.drawText (info, r, juce::Justification::centredLeft, true);
}

//==============================================================================
void RoomView::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);
    auto card = [&g] (juce::Rectangle<int> r, const juce::String& title)
    {
        g.setColour (theme::panel);
        g.fillRoundedRectangle (r.toFloat(), 8.0f);
        g.setColour (theme::text);
        g.setFont (theme::font (14.5f, true));
        g.drawText (title, r.reduced (kPad, 0).removeFromTop (kTitleH + 8).withTrimmedTop (6), juce::Justification::centredLeft);
    };
    card (setupArea, "Setup");
    card (libraryArea, "Measurements");
    card (resonanceArea, "Resonances");
    card (eqArea, "Auto EQ");

    g.setFont (theme::font (11.5f));
    g.setColour (theme::textDim);
    for (auto& [comp, text] : captions)
        if (comp->isVisible())
            g.drawText (text, comp->getX() + 2, comp->getY() - kCaptionH, comp->getWidth(), kCaptionH, juce::Justification::centredLeft, true);

    // Progress and microphone level.
    {
        auto r = progressArea;
        auto* run = ctx.engine.getMeasurement();
        auto bar = r.removeFromTop (16).toFloat();
        g.setColour (theme::panel2);
        g.fillRoundedRectangle (bar, 4.0f);
        juce::String phase = "Ready";
        if (run != nullptr)
        {
            g.setColour (theme::accentIn);
            g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * run->progress()), 4.0f);
            const int pos = run->pos.load(), lead = runSpec.leadLength(), seg = runSpec.segmentLength();
            if (pos < lead) phase = "Measuring the background noise...";
            else
            {
                const int k = juce::jmin (runSpec.repeats - 1, (pos - lead) / juce::jmax (1, seg));
                phase = (pos - lead - k * seg) < runSpec.sweepLength()
                            ? "Sweep " + juce::String (k + 1) + " of " + juce::String (runSpec.repeats)
                            : juce::String ("Listening to the decay...");
            }
        }
        r.removeFromTop (6);
        auto meter = r.removeFromTop (12).toFloat();
        const float db = gainToDb (micLevel), holdDb = gainToDb (micHold);
        auto pos = [&] (float d) { return juce::jlimit (0.0f, 1.0f, (d + 60.0f) / 60.0f); };
        g.setColour (theme::panel2);
        g.fillRoundedRectangle (meter, 3.0f);
        g.setColour (db > -3.0f ? theme::mute : db > -12.0f ? theme::warn : theme::good);
        g.fillRoundedRectangle (meter.withWidth (meter.getWidth() * pos (db)), 3.0f);
        if (micHold > 0.0f)
        {
            g.setColour (theme::text);
            g.fillRect (meter.getX() + meter.getWidth() * pos (holdDb) - 1.0f, meter.getY(), 2.0f, meter.getHeight());
        }
        g.setFont (theme::font (11.5f));
        g.setColour (theme::textDim);
        g.drawText (phase, r.removeFromTop (18), juce::Justification::centredLeft);
        if (run != nullptr || micHold > 0.0f)
            g.drawText ("Mic peak " + (micHold > 0.0f ? juce::String (holdDb, 1) + " dBFS" : juce::String ("-")),
                        progressArea.withTrimmedTop (34).withHeight (18), juce::Justification::centredRight);
    }

    g.setColour (statusColour);
    g.setFont (theme::font (12.5f));
    g.drawFittedText (status, statusArea, juce::Justification::topLeft, 5);

    // Info line above the chart.
    if (auto* m = current())
    {
        juce::String t = m->name + "   |   " + m->created.formatted ("%Y-%m-%d %H:%M");
        if (m->analysed)
            t << "   |   latency " << juce::String (m->ir.latencyMs, 1) << " ms   |   mic peak " << juce::String (m->ir.capturePeakDb, 1)
              << " dBFS   |   noise " << juce::String (juce::roundToInt (m->ir.noiseFloorDb)) << " dBFS   |   S/N " << juce::String (juce::roundToInt (m->ir.irSnrDb)) << " dB";
        const auto warn = quality (*m);
        g.setFont (theme::font (12.0f));
        g.setColour (warn.isNotEmpty() ? theme::warn : theme::textDim);
        g.drawText (warn.isNotEmpty() ? t + "   |   " + warn : t, infoArea, juce::Justification::centredLeft, true);
    }

    // Resonance table.
    {
        auto r = resonanceArea.reduced (kPad, 0).withTrimmedTop (kTitleH + 10).withTrimmedBottom (10);
        auto* m = current();
        g.setFont (theme::font (11.5f));
        if (m == nullptr || ! m->analysed || m->analysis.resonances.empty())
        {
            g.setColour (theme::textDim);
            g.drawFittedText (m != nullptr && m->analysed ? "No resonances above 3 dB found below 400 Hz." : "-", r, juce::Justification::topLeft, 3);
        }
        else
        {
            const int cols[] = { 0, 80, 140, 196 };
            g.setColour (theme::textFaint);
            const char* heads[] = { "Frequency", "Peak", "Q", "Rings for" };
            auto head = r.removeFromTop (18);
            for (int c = 0; c < 4; ++c)
                g.drawText (heads[c], head.getX() + cols[c], head.getY(), 80, 18, juce::Justification::centredLeft);
            const auto& res = m->analysis.resonances;
            const int rows = juce::jmin ((int) res.size(), r.getHeight() / 19);
            // Bars scale with the ringing time (20 dB decay): long ringing is what makes bass boomy.
            const int barX = cols[3] + 74;
            for (int i = 0; i < rows; ++i)
            {
                const auto& x = res[(size_t) i];
                auto row = r.removeFromTop (19);
                g.setColour (theme::text);
                g.setFont (theme::font (12.5f, i < 3));
                g.drawText (hz (x.freq), row.getX() + cols[0], row.getY(), 76, 19, juce::Justification::centredLeft);
                g.setColour (theme::mute.interpolatedWith (theme::text, juce::jlimit (0.0f, 1.0f, 1.0f - (float) (x.excessDb - 3.0) / 6.0f)));
                g.drawText ("+" + juce::String (x.excessDb, 1) + " dB", row.getX() + cols[1], row.getY(), 60, 19, juce::Justification::centredLeft);
                g.setColour (theme::textDim);
                g.drawText (juce::String (x.q, 1), row.getX() + cols[2], row.getY(), 50, 19, juce::Justification::centredLeft);
                const bool atEnd = ! m->analysis.waterfallMs.empty() && x.ringMs >= m->analysis.waterfallMs.back() - 0.5;
                g.drawText ((atEnd ? "> " : "") + juce::String (juce::roundToInt (x.ringMs)) + " ms", row.getX() + cols[3], row.getY(), 70, 19, juce::Justification::centredLeft);
                const float w = (float) (row.getWidth() - barX) * (float) juce::jlimit (0.0, 1.0, x.ringMs / 300.0);
                g.setColour (timeColour ((float) juce::jlimit (0.0, 1.0, x.ringMs / 300.0)).withAlpha (0.8f));
                g.fillRoundedRectangle ((float) (row.getX() + barX), (float) row.getY() + 6.0f, juce::jmax (2.0f, w), 7.0f, 3.0f);
            }
        }
    }

    // EQ summary and bands.
    {
        auto r = bandsArea;
        auto* m = current();
        g.setFont (theme::font (12.5f));
        if (m == nullptr || ! m->analysed)
        {
            g.setColour (theme::textDim);
            g.drawText ("Measure first: the EQ is designed from the selected measurement.", r.removeFromTop (20), juce::Justification::centredLeft);
        }
        else if (eq.bands.empty())
        {
            g.setColour (theme::good);
            g.drawText ("Nothing to correct in this range: the response is already flat within 0.75 dB.", r.removeFromTop (20), juce::Justification::centredLeft);
        }
        else
        {
            g.setColour (theme::text);
            g.drawText (juce::String ((int) eq.bands.size()) + " bands.  Highest peak +" + juce::String (eq.peakBeforeDb, 1) + " dB  ->  +"
                            + juce::String (eq.peakAfterDb, 1) + " dB,  deviation " + juce::String (eq.rmsBeforeDb, 1) + "  ->  "
                            + juce::String (eq.rmsAfterDb, 1) + " dB rms",
                        r.removeFromTop (20), juce::Justification::centredLeft);
            if (m->throughEq)
            {
                g.setColour (theme::warn);
                g.drawText ("Measured through the output's EQ: use it to check the correction (compare with the measurement before).",
                            r.removeFromTop (18), juce::Justification::centredLeft, true);
            }
            r.removeFromTop (6);
            const int perRow = juce::jlimit (1, 4, r.getWidth() / 175), cw = r.getWidth() / perRow;
            for (size_t k = 0; k < eq.bands.size(); ++k)
            {
                const auto& b = eq.bands[k];
                auto cell = juce::Rectangle<int> (r.getX() + (int) (k % perRow) * cw, r.getY() + (int) (k / perRow) * 26, cw - 8, 22);
                g.setColour (theme::bandColour ((int) k).withAlpha (0.15f));
                g.fillRoundedRectangle (cell.toFloat(), 5.0f);
                g.setColour (theme::bandColour ((int) k));
                g.setFont (theme::font (12.0f, true));
                g.drawText (juce::String ((int) k + 1), cell.removeFromLeft (20), juce::Justification::centred);
                g.setColour (theme::text);
                g.setFont (theme::font (12.0f));
                g.drawText (hz (b.freq) + "  " + juce::String (b.gainDb, 1) + " dB  Q " + juce::String (b.q, 1), cell, juce::Justification::centredLeft, true);
            }
        }
    }
}

void RoomView::resized()
{
    auto r = getLocalBounds().reduced (12);
    auto left = r.removeFromLeft (330);
    r.removeFromLeft (12);

    auto rowWithCaption = [] (juce::Rectangle<int>& area)
    {
        area.removeFromTop (kCaptionH);
        auto row = area.removeFromTop (kRowH);
        area.removeFromTop (kGap);
        return row;
    };

    // Setup
    setupArea = left.removeFromTop (juce::jmin (left.getHeight() - 200, 500));
    left.removeFromTop (12);
    libraryArea = left;
    {
        auto s = setupArea.reduced (kPad, 0).withTrimmedTop (kTitleH + 6).withTrimmedBottom (kPad);
        auto row = rowWithCaption (s);
        micChannelBox.setBounds (row.removeFromRight (80));
        row.removeFromRight (kGap);
        micBox.setBounds (row);
        row = rowWithCaption (s);
        speakerBox.setBounds (row.removeFromRight (110));
        row.removeFromRight (kGap);
        busBox.setBounds (row);
        row = rowWithCaption (s);
        const int w = (row.getWidth() - 2 * kGap) / 3;
        lengthBox.setBounds (row.removeFromLeft (w));
        row.removeFromLeft (kGap);
        repeatsBox.setBounds (row.removeFromLeft (w));
        row.removeFromLeft (kGap);
        levelBox.setBounds (row);
        muteOthers.setBounds (s.removeFromTop (24));
        throughEq.setBounds (s.removeFromTop (24));
        s.removeFromTop (kGap);
        startButton.setBounds (s.removeFromTop (34));
        s.removeFromTop (12);
        progressArea = s.removeFromTop (54);
        s.removeFromTop (6);
        statusArea = s;
    }
    {
        auto l = libraryArea.reduced (kPad, 0).withTrimmedTop (kTitleH + 8).withTrimmedBottom (kPad);
        auto buttons = l.removeFromBottom (28);
        l.removeFromBottom (kGap);
        list.setBounds (l);
        const int w = (buttons.getWidth() - 2 * kGap) / 3;
        importButton.setBounds (buttons.removeFromLeft (w));
        buttons.removeFromLeft (kGap);
        exportButton.setBounds (buttons.removeFromLeft (w));
        buttons.removeFromLeft (kGap);
        deleteButton.setBounds (buttons);
    }

    // Tabs and view options
    auto top = r.removeFromTop (30);
    for (auto* t : { &tabResponse, &tabWaterfall, &tabDecay, &tabImpulse })
    {
        t->setBounds (top.removeFromLeft (124));
        top.removeFromLeft (6);
    }
    viewOption.setBounds (top.removeFromRight (150));
    top.removeFromRight (kGap);
    compareBox.setBounds (top.removeFromRight (juce::jmin (280, juce::jmax (120, top.getWidth()))));
    r.removeFromTop (4);
    infoArea = r.removeFromTop (22);
    r.removeFromTop (4);

    auto bottom = r.removeFromBottom (262);
    r.removeFromBottom (12);
    chart->setBounds (r);

    resonanceArea = bottom.removeFromLeft (juce::jmax (360, bottom.getWidth() * 36 / 100));
    bottom.removeFromLeft (12);
    eqArea = bottom;
    {
        auto e = eqArea.reduced (kPad, 0).withTrimmedTop (kTitleH + 6).withTrimmedBottom (kPad);
        auto row = rowWithCaption (e);
        juce::ComboBox* boxes[] = { &eqLowBox, &eqHighBox, &eqBandsBox, &eqCutBox, &eqBoostBox, &eqSmoothBox };
        const int w = (row.getWidth() - 5 * kGap) / 6;
        for (auto* b : boxes)
        {
            b->setBounds (row.removeFromLeft (w));
            row.removeFromLeft (kGap);
        }
        row = rowWithCaption (e);
        targetBox.setBounds (row.removeFromLeft (juce::jmin (260, row.getWidth() / 3)));
        row.removeFromLeft (kGap);
        applyButton.setBounds (row.removeFromLeft (180));
        row.removeFromLeft (kGap);
        exportEqButton.setBounds (row.removeFromLeft (120));
        bandsArea = e;
    }
}
} // namespace hydra

#include "HeadphoneView.h"
#include "Dialog.h"

namespace hydra::hpui
{
namespace
{
const juce::Colour kFromColour = theme::warn;    // the headphones as they are
const juce::Colour kResultColour = theme::good;  // with the correction

juce::String brandOf (const hp::Entry& e)
{
    return e.name.upToFirstOccurrenceOf (" ", false, false);
}

/** Headphones grouped by brand (built-in ones; own ones come first, ungrouped). */
std::map<juce::String, std::vector<const hp::Entry*>, std::function<bool (const juce::String&, const juce::String&)>> byBrand()
{
    std::map<juce::String, std::vector<const hp::Entry*>, std::function<bool (const juce::String&, const juce::String&)>> m (
        [] (const juce::String& a, const juce::String& b) { return a.compareIgnoreCase (b) < 0; });
    for (auto& e : hp::Library::get().headphones())
        if (! e.isUser()) m[brandOf (e)].push_back (&e);
    return m;
}

HeadphoneFx& fxOf (Engine& e, int ch, int k) { return e.fxOf (ch).headphones[(size_t) juce::jlimit (0, kFxInstances - 1, k)]; }

void choose (UiContext& ctx, int ch, int k, bool target, const juce::String& id)
{
    auto& h = fxOf (ctx.engine, ch, k);
    if (target) ctx.engine.setHeadphone (ch, k, h.from, id);
    else        ctx.engine.setHeadphone (ch, k, id, h.to);
    h.on->set (1.0f);
    ctx.engine.sendChangeMessage();   // autosave
}

/** Light smoothing for the picture (measurements are spiky in the treble): 1/6 octave, wider above 5 kHz. */
hp::Curve smoothed (const hp::Curve& c)
{
    hp::Curve out (c.size());
    for (int i = 0; i < (int) c.size(); ++i)
    {
        const double f = hp::gridFreq (i);
        const int h = f < 5000.0 ? 1 : f < 10000.0 ? 2 : 3;
        double sum = 0.0;
        int n = 0;
        for (int k = juce::jmax (0, i - h); k <= juce::jmin ((int) c.size() - 1, i + h); ++k) { sum += c[(size_t) k]; ++n; }
        out[(size_t) i] = (float) (sum / n);
    }
    return out;
}

juce::String plotLabel (double f)
{
    return f >= 1000.0 ? juce::String (f / 1000.0, 0) + "k" : juce::String ((int) f);
}
} // namespace

juce::String nameOf (const juce::String& id, bool target)
{
    if (target && (id.isEmpty() || id == "target:neutral")) return "Neutral (Harman)";
    if (id.isEmpty()) return {};
    if (auto* e = hp::Library::get().find (id)) return e->name;
    return id.fromFirstOccurrenceOf (":", false, false) + " (missing)";
}

//==============================================================================
void showMenu (UiContext& ctx, int ch, int k, bool target, juce::Component* anchor, std::function<void()> changed)
{
    auto& lib = hp::Library::get();
    auto& h = fxOf (ctx.engine, ch, k);
    const auto current = target ? (h.to.isEmpty() ? juce::String ("target:neutral") : h.to) : h.from;
    juce::Component::SafePointer<juce::Component> safe (anchor);
    auto pick = [&ctx, ch, k, target, changed] (juce::String id)
    {
        return [&ctx, ch, k, target, changed, id] { choose (ctx, ch, k, target, id); if (changed) changed(); };
    };

    juce::PopupMenu m;
    if (target)
    {
        m.addSectionHeader ("Sound");
        for (auto& t : lib.targets()) m.addItem (t.name, true, current == t.id, pick (t.id));
        m.addSectionHeader ("Like other headphones");
    }
    else
        m.addSectionHeader ("Your headphones");

    bool own = false;
    juce::PopupMenu mine;
    for (auto& e : lib.headphones())
        if (e.isUser()) { mine.addItem (e.name, true, current == e.id, pick (e.id)); own = true; }
    if (own) m.addSubMenu ("My headphones", mine, true, nullptr, current.startsWith ("user:"));
    for (auto& [brand, list] : byBrand())
    {
        juce::PopupMenu sub;
        bool ticked = false;
        for (auto* e : list)
        {
            sub.addItem (e->name, true, current == e->id, pick (e->id));
            ticked |= current == e->id;
        }
        m.addSubMenu (brand, sub, true, nullptr, ticked);
    }
    m.addSeparator();
    m.addItem ("Add headphones from a file...", [&ctx, ch, k, target, changed, safe]
    {
        importFile (safe.getComponent(), [&ctx, ch, k, target, changed] (juce::String id)
        {
            choose (ctx, ch, k, target, id);
            if (changed) changed();
        });
    });
    if (! target && ! h.from.isEmpty())
        m.addItem ("No headphones (correction off)", [&ctx, ch, k, changed] { ctx.engine.setHeadphone (ch, k, {}, {}); if (changed) changed(); });
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (anchor));
}

//==============================================================================
void importFile (juce::Component* near, std::function<void (juce::String)> done)
{
    static std::unique_ptr<juce::FileChooser> chooser;
    chooser = std::make_unique<juce::FileChooser> ("Add headphones: frequency response file",
                                                   juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
                                                   hp::Library::filePatterns());
    juce::Component::SafePointer<juce::Component> safe (near);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe, done] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (! file.existsAsFile()) return;
        bool ownTarget = file.hasFileExtension (".hydrahp");
        if (! ownTarget)
        {
            hp::Parsed p;
            juce::String error;
            if (! hp::parseResponse (file.loadFileAsString(), p, error))
            {
                dialog::message (safe.getComponent(), dialog::Kind::warning, "Cannot read " + file.getFileName(), error);
                return;
            }
            ownTarget = p.hasOwnTarget;
        }
        auto finish = [safe, done, file] (juce::String name, juce::String kind)
        {
            juce::String error;
            const auto id = hp::Library::get().import (file, name, kind, "gras", error);
            if (id.isEmpty()) { dialog::message (safe.getComponent(), dialog::Kind::warning, "Cannot add the headphones", error); return; }
            if (done) done (id);
        };
        dialog::askText (safe.getComponent(), "Add headphones", "Name of the headphones:", file.getFileNameWithoutExtension(), "Next",
                         [safe, finish, ownTarget] (juce::String name)
        {
            if (ownTarget) { finish (name, "over-ear"); return; }
            dialog::show (safe.getComponent(), dialog::Kind::question, "What kind of headphones?",
                          "The measurement is compared with the neutral target of its kind: over-ear and on-ear headphones "
                          "are measured on an ear simulator (squig.link, oratory1990, crinacle), in-ears on an IEC 711 coupler.",
                          { { "Over-ear / on-ear", dialog::Button::Role::primary }, { "In-ear" }, { "Cancel", dialog::Button::Role::cancel } },
                          [finish, name] (int r)
            {
                if (r == 0) finish (name, "over-ear");
                else if (r == 1) finish (name, "in-ear");
            });
        });
    });
}

//==============================================================================
void drawCurves (juce::Graphics& g, juce::Rectangle<float> area, Engine& engine, int ch, int k, bool compact)
{
    auto& h = fxOf (engine, ch, k);
    auto& lib = hp::Library::get();
    const auto* from = lib.find (h.from);
    const auto* to = h.to.isEmpty() ? lib.find ("target:neutral") : lib.find (h.to);
    const float amount = h.amount->get() / 100.0f;
    const auto eqProfile = hp::scaled (h.full, amount);
    const auto eq = hp::responseOf (eqProfile);
    const bool lit = h.on->isOn();

    g.setColour (theme::bg.withAlpha (0.6f));
    g.fillRoundedRectangle (area, 5.0f);
    auto plot = area.reduced (compact ? 4.0f : 34.0f, compact ? 4.0f : 14.0f);
    if (! compact) plot.removeFromBottom (12.0f);

    double range = compact ? 6.0 : 10.0;
    const auto grow = [&] (const hp::Curve& c) { for (auto v : c) range = juce::jmax (range, std::abs ((double) v) * 1.1); };
    grow (eq);
    if (! compact)
    {
        if (from != nullptr) grow (from->db);
        if (to != nullptr) grow (to->db);
    }
    range = juce::jmin (range, compact ? 20.0 : 25.0);
    if (! compact) range = std::ceil (range / 5.0) * 5.0;

    const auto fx = [&] (int i) { return plot.getX() + plot.getWidth() * (float) i / (float) (hp::kPoints - 1); };
    const auto fxHz = [&] (double f) { return plot.getX() + plot.getWidth() * (float) (std::log (f / 20.0) / std::log (hp::gridFreq (hp::kPoints - 1) / 20.0)); };
    const auto fy = [&] (double v) { return plot.getCentreY() - (float) (juce::jlimit (-range, range, v) / range) * plot.getHeight() * 0.5f; };

    if (! compact)
    {
        g.setFont (theme::font (10.0f));
        for (double f : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
        {
            const float x = fxHz (f);
            g.setColour (theme::outline.withAlpha (0.5f));
            g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());
            g.setColour (theme::textFaint);
            g.drawText (plotLabel (f), juce::Rectangle<float> (x - 20.0f, plot.getBottom() + 2.0f, 40.0f, 12.0f), juce::Justification::centred);
        }
        for (double v = -range; v <= range + 0.1; v += 5.0)
        {
            const float y = fy (v);
            g.setColour (theme::outline.withAlpha (v == 0.0 ? 1.0f : 0.4f));
            g.drawHorizontalLine (juce::roundToInt (y), plot.getX(), plot.getRight());
            g.setColour (theme::textFaint);
            g.drawText ((v > 0 ? "+" : "") + juce::String ((int) v), juce::Rectangle<float> (area.getX(), y - 6.0f, 30.0f, 12.0f),
                        juce::Justification::centredRight);
        }
    }
    else
    {
        g.setColour (theme::outline);
        g.drawHorizontalLine (juce::roundToInt (plot.getCentreY()), plot.getX(), plot.getRight());
    }

    const auto pathOf = [&] (const hp::Curve& c)
    {
        juce::Path p;
        for (int i = 0; i < hp::kPoints; ++i)
        {
            if (i == 0) p.startNewSubPath (fx (i), fy (c[(size_t) i]));
            else        p.lineTo (fx (i), fy (c[(size_t) i]));
        }
        return p;
    };
    const auto stroke = [&] (const juce::Path& p, juce::Colour c, float w, bool dashed = false)
    {
        g.setColour (c);
        juce::PathStrokeType st (w, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        if (dashed)
        {
            juce::Path d;
            const float dash[] = { 5.0f, 4.0f };
            st.createDashedStroke (d, p, dash, 2);
            g.fillPath (d);
        }
        else
            g.strokePath (p, st);
    };

    if (! compact && from != nullptr)
    {
        const auto fromDb = smoothed (from->db);
        if (to != nullptr) stroke (pathOf (smoothed (to->db)), theme::text.withAlpha (0.55f), 1.4f, true);
        stroke (pathOf (fromDb), kFromColour.withAlpha (0.8f), 1.5f);
        if (! h.full.isEmpty())
        {
            hp::Curve result ((size_t) hp::kPoints);
            for (int i = 0; i < hp::kPoints; ++i) result[(size_t) i] = fromDb[(size_t) i] + eq[(size_t) i];
            stroke (pathOf (result), kResultColour.withAlpha (lit ? 0.95f : 0.4f), 1.8f);
        }
    }
    if (! h.full.isEmpty())
    {
        auto p = pathOf (eq);
        juce::Path fill (p);
        fill.lineTo (fx (hp::kPoints - 1), fy (0.0));
        fill.lineTo (fx (0), fy (0.0));
        fill.closeSubPath();
        g.setColour (theme::fxHeadphone.withAlpha (lit ? 0.18f : 0.07f));
        g.fillPath (fill);
        stroke (p, theme::fxHeadphone.withAlpha (lit ? 1.0f : 0.35f), compact ? 1.6f : 2.0f);
    }

    if (compact)
    {
        g.setColour (theme::textFaint);
        g.setFont (theme::font (9.5f));
        g.drawText ("+-" + juce::String (juce::roundToInt (range)) + " dB", area.reduced (5.0f, 2.0f), juce::Justification::topRight);
        return;
    }

    // Legend
    struct Item { juce::String text; juce::Colour c; bool dashed; };
    std::vector<Item> items;
    if (from != nullptr) items.push_back ({ from->name, kFromColour, false });
    if (to != nullptr && from != nullptr) items.push_back ({ "Target: " + to->name, theme::text.withAlpha (0.7f), true });
    if (! h.full.isEmpty()) items.push_back ({ "Correction", theme::fxHeadphone, false });
    if (! h.full.isEmpty() && from != nullptr) items.push_back ({ "Result", kResultColour, false });
    float x = plot.getX() + 8.0f;
    float y = plot.getY() + 6.0f;
    g.setFont (theme::font (11.0f, true));
    for (auto& it : items)
    {
        const float w = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), it.text);
        if (x > plot.getX() + 8.0f && x + w + 25.0f > plot.getRight()) { x = plot.getX() + 8.0f; y += 17.0f; }   // next line
        juce::Path line;
        line.startNewSubPath (x, y + 6.0f);
        line.lineTo (x + 16.0f, y + 6.0f);
        stroke (line, it.c, 2.0f, it.dashed);
        g.setColour (theme::text);
        g.drawText (it.text, juce::Rectangle<float> (x + 21.0f, y, w + 4.0f, 12.0f), juce::Justification::centredLeft);
        x += w + 40.0f;
    }
    if (from == nullptr)
    {
        g.setColour (theme::textDim);
        g.setFont (theme::font (14.0f));
        g.drawText ("Choose your headphones on the left", plot, juce::Justification::centred);
    }
}

//==============================================================================
namespace
{
/** Searchable list of headphones (or targets + headphones). */
class PickList final : public juce::Component, private juce::ListBoxModel
{
public:
    PickList (juce::String title, bool target) : caption (std::move (title)), forTarget (target)
    {
        search.setTextToShowWhenEmpty ("Search", theme::textFaint);
        search.setFont (theme::font (13.0f));
        search.setColour (juce::TextEditor::backgroundColourId, theme::bg);
        search.setColour (juce::TextEditor::textColourId, theme::text);
        search.setColour (juce::TextEditor::outlineColourId, theme::outline);
        search.setColour (juce::TextEditor::focusedOutlineColourId, theme::fxHeadphone);
        search.setIndents (8, 5);
        search.onTextChange = [this] { rebuild(); };
        search.onEscapeKey = [this] { search.clear(); rebuild(); };
        addAndMakeVisible (search);
        list.setModel (this);
        list.setRowHeight (24);
        list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        list.setColour (juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
        addAndMakeVisible (list);
        rebuild();
    }

    std::function<void (const juce::String&)> onPick;
    std::function<void (const juce::String&)> onMenu;   // right-click on an own entry
    juce::String selected;

    void rebuild()
    {
        rows.clear();
        const auto f = search.getText().trim().toLowerCase();
        const auto match = [&] (const hp::Entry& e) { return f.isEmpty() || e.name.toLowerCase().contains (f) || e.source.toLowerCase().contains (f); };
        auto& lib = hp::Library::get();
        if (forTarget)
        {
            for (auto& t : lib.targets()) if (match (t)) rows.push_back ({ &t, {} });
            rows.push_back ({ nullptr, "Like other headphones" });
        }
        bool header = false;
        for (auto& e : lib.headphones())
            if (e.isUser() && match (e))
            {
                if (! header) { rows.push_back ({ nullptr, "My headphones" }); header = true; }
                rows.push_back ({ &e, {} });
            }
        juce::String brand;
        for (auto& e : lib.headphones())
            if (! e.isUser() && match (e))
            {
                const auto b = e.name.upToFirstOccurrenceOf (" ", false, false);
                if (b != brand && f.isEmpty()) rows.push_back ({ nullptr, b });
                brand = b;
                rows.push_back ({ &e, {}, f.isEmpty() });
            }
        list.updateContent();
        list.repaint();
    }

    void showSelected()
    {
        for (int i = 0; i < (int) rows.size(); ++i)
            if (rows[(size_t) i].e != nullptr && rows[(size_t) i].e->id == selected) { list.scrollToEnsureRowIsOnscreen (i); break; }
        list.repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (theme::textDim);
        g.setFont (theme::font (11.5f, true));
        g.drawText (caption.toUpperCase(), getLocalBounds().removeFromTop (18), juce::Justification::centredLeft);
        auto r = list.getBounds().toFloat().expanded (1.0f);
        g.setColour (theme::bg.withAlpha (0.5f));
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (theme::cardLine);
        g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        r.removeFromTop (22);
        search.setBounds (r.removeFromTop (28));
        r.removeFromTop (6);
        list.setBounds (r.reduced (1));
    }

private:
    struct Row { const hp::Entry* e; juce::String header; bool underBrand = false; };

    int getNumRows() override { return (int) rows.size(); }
    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool) override
    {
        if (! juce::isPositiveAndBelow (row, (int) rows.size())) return;
        const auto& r = rows[(size_t) row];
        if (r.e == nullptr)
        {
            g.setColour (theme::textFaint);
            g.setFont (theme::font (10.5f, true));
            g.drawText (r.header.toUpperCase(), 10, 4, w - 20, h - 4, juce::Justification::centredLeft);
            return;
        }
        const bool sel = r.e->id == selected || (forTarget && selected.isEmpty() && r.e->id == "target:neutral");
        if (sel)
        {
            g.setColour (theme::fxHeadphone.withAlpha (0.2f));
            g.fillRoundedRectangle (juce::Rectangle<float> (3.0f, 1.0f, (float) w - 6.0f, (float) h - 2.0f), 4.0f);
        }
        g.setColour (sel ? theme::text : theme::text.withAlpha (0.88f));
        g.setFont (theme::font (12.5f, sel));
        auto text = juce::Rectangle<int> (12, 0, w - 24, h);
        auto src = text.removeFromRight (r.e->isTarget() ? 0 : 74);
        const auto name = r.underBrand ? r.e->name.fromFirstOccurrenceOf (" ", false, false) : r.e->name;
        g.drawText (name, text, juce::Justification::centredLeft, true);
        g.setColour (theme::textFaint);
        g.setFont (theme::font (10.0f));
        g.drawText (r.e->kind == "in-ear" ? r.e->source + " · IEM" : r.e->source, src, juce::Justification::centredRight, true);
    }
    void listBoxItemClicked (int row, const juce::MouseEvent& e) override
    {
        if (! juce::isPositiveAndBelow (row, (int) rows.size()) || rows[(size_t) row].e == nullptr) return;
        const auto id = rows[(size_t) row].e->id;
        if (e.mods.isPopupMenu()) { if (rows[(size_t) row].e->isUser() && onMenu) onMenu (id); return; }
        selected = id;
        list.repaint();
        if (onPick) onPick (id);
    }

    juce::String caption;
    bool forTarget;
    juce::TextEditor search;
    juce::ListBox list;
    std::vector<Row> rows;
};

class HeadphonePanel final : public juce::Component, private juce::Timer
{
public:
    HeadphonePanel (UiContext& c, int channel, int instance)
        : ctx (c), ch (channel), k (instance), h (fxOf (c.engine, channel, instance)),
          fromList ("Your headphones", false), toList ("Should sound like", true),
          on (c, *h.on, "ON", theme::fxHeadphone), amount (c, *h.amount, "AMOUNT", theme::fxHeadphone),
          gain (c, *h.gain, "GAIN", theme::fxHeadphone), autoGain (c, *h.autoGain, "AUTO MAKEUP", theme::fxHeadphone),
          add ({ "Add headphones..." }, theme::fxHeadphone)
    {
        amount.alwaysShowValue = true;
        gain.alwaysShowValue = true;
        for (juce::Component* x : std::initializer_list<juce::Component*> { &fromList, &toList, &on, &amount, &gain, &autoGain, &add })
            addAndMakeVisible (x);
        on.setTooltip ("Headphone Correction on or off (zero latency)");
        amount.setTooltip ("How much of the correction is applied: 100 % = fully like the target, 50 % = half way");
        gain.setTooltip ("Output level on top of the preamp: the correction turns everything down so its boosts cannot clip, "
                         "GAIN brings it back up");
        autoGain.setTooltip ("Gives back the loudness the correction takes away (pink noise as loud as without it). "
                             "Loud peaks can then clip: lower GAIN or use a limiter after it");
        add.onClick = [this] { importFile (this, [this] (juce::String id) { fromList.rebuild(); toList.rebuild(); pick (false, id); }); };
        fromList.onPick = [this] (const juce::String& id) { pick (false, id); };
        toList.onPick = [this] (const juce::String& id) { pick (true, id); };
        fromList.onMenu = toList.onMenu = [this] (const juce::String& id) { ownMenu (id); };
        sync();
        setSize (1060, 520);
        startTimerHz (10);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::panel);
        drawCurves (g, graph.toFloat(), ctx.engine, ch, k, false);
        auto r = footer;
        g.setFont (theme::font (11.5f));
        g.setColour (theme::textDim);
        juce::String text;
        if (! h.full.isEmpty())
        {
            const auto out = h.output();
            double peak = -100.0;
            for (double f = 20.0; f <= 20000.0; f *= 1.02) peak = juce::jmax (peak, out.magnitudeDb (f));
            text << (int) h.full.bands.size() << " filters (zero latency), deviation " << juce::String (h.rmsBeforeDb, 1) << " -> "
                 << juce::String (h.rmsAfterDb, 1) << " dB. Loudness " << juce::String (hp::loudnessDb (out), 1) << " dB";
            if (peak > 0.05) text << ", peaks up to +" << juce::String (peak, 1) << " dB: can clip";
            text << ".";
        }
        else if (h.from.isNotEmpty())
            text = "Headphones and target are the same: nothing to correct.";
        else
            text = "Pick your headphones and what they should sound like. Missing yours? Add a measurement (squig.link, REW, AutoEq: .csv / .txt).";
        g.drawFittedText (text, r.removeFromTop (18), juce::Justification::centredLeft, 1);
        g.setColour (theme::textFaint);
        g.setFont (theme::font (10.5f));
        g.drawFittedText ("Built-in measurements: AutoEq project (MIT license) by oratory1990, Rtings, Kuulokenurkka and others. "
                          "Every pair of headphones sounds a little different on your head: use AMOUNT if it is too much.",
                          r, juce::Justification::topLeft, 2);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16);
        auto lists = r.removeFromLeft (520);
        fromList.setBounds (lists.removeFromLeft (256));
        lists.removeFromLeft (8);
        toList.setBounds (lists);
        r.removeFromLeft (16);
        auto top = r.removeFromTop (60);
        on.setBounds (top.removeFromRight (52).withSizeKeepingCentre (52, 24).withY (top.getY()));
        top.removeFromRight (8);
        autoGain.setBounds (top.removeFromRight (104).withSizeKeepingCentre (104, 26));
        gain.setBounds (top.removeFromRight (70).withTrimmedRight (10).withY (top.getY() - 2).withHeight (64));
        amount.setBounds (top.removeFromRight (70).withTrimmedRight (10).withY (top.getY() - 2).withHeight (64));
        add.setBounds (top.removeFromLeft (170).withHeight (30));
        r.removeFromTop (8);
        footer = r.removeFromBottom (48);
        r.removeFromBottom (6);
        graph = r;
        fromList.showSelected();   // the lists know their height only now
        toList.showSelected();
    }

private:
    void pick (bool target, const juce::String& id)
    {
        choose (ctx, ch, k, target, id);
        sync();
    }

    void sync()
    {
        fromList.selected = h.from;
        toList.selected = h.to;
        fromList.showSelected();
        toList.showSelected();
        shown = h.version;
        repaint();
    }

    void ownMenu (const juce::String& id)
    {
        juce::PopupMenu m;
        m.addItem ("Rename...", [this, id]
        {
            dialog::askText (this, "Rename headphones", "New name:", nameOf (id, false), "Rename", [this, id] (juce::String name)
            {
                hp::Library::get().rename (id, name);
                fromList.rebuild(); toList.rebuild(); repaint();
            });
        });
        m.addItem ("Show file", [id]
        {
            hp::Library::userFolder().getChildFile (id.fromFirstOccurrenceOf (":", false, false) + ".hydrahp").revealToUser();
        });
        m.addItem ("Delete", [this, id]
        {
            dialog::confirm (this, dialog::Kind::danger, "Delete " + nameOf (id, false) + "?",
                             "The measurement goes to the recycle bin. Channels using it keep their correction.", "Delete", [this, id]
            {
                hp::Library::get().remove (id);
                fromList.rebuild(); toList.rebuild(); repaint();
            });
        });
        m.showMenuAsync (juce::PopupMenu::Options());
    }

    void timerCallback() override
    {
        if (h.version != shown) { sync(); return; }
        const auto v = (uint32_t) juce::roundToInt (h.amount->get()) * 8192u + (uint32_t) juce::roundToInt (h.gain->get() * 10.0f + 200.0f) * 4u
                     + (h.autoGain->isOn() ? 2u : 0u) + (h.on->isOn() ? 1u : 0u);
        if (v != shownAmount) { shownAmount = v; repaint (graph.getUnion (footer)); }
    }

    UiContext& ctx;
    int ch, k;
    HeadphoneFx& h;
    PickList fromList, toList;
    Toggle on;
    Knob amount, gain;
    Toggle autoGain;
    dialog::ActionButton add;
    juce::Rectangle<int> graph, footer;
    uint32_t shown = ~0u, shownAmount = ~0u;
};
} // namespace

std::unique_ptr<juce::Component> makePanel (UiContext& ctx, int channel, int k)
{
    return std::make_unique<HeadphonePanel> (ctx, channel, k);
}
} // namespace hydra::hpui

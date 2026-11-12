#include "Welcome.h"
#include "state/Session.h"
#include "state/VoicemeeterImport.h"
#include "state/VacCables.h"
#include "state/Autostart.h"
#include "state/Install.h"
#include "engine/IoStreams.h"

namespace hydra
{
namespace
{
constexpr int kCardW = 760, kCardH = 560, kPad = 28;

juce::TextLayout layoutText (const juce::String& text, const juce::Font& font, juce::Colour colour, float width,
                             juce::Justification j = juce::Justification::topLeft)
{
    juce::AttributedString s;
    s.setWordWrap (juce::AttributedString::byWord);
    s.setJustification (j);
    s.append (text, font, colour);
    juce::TextLayout l;
    l.createLayout (s, width);
    return l;
}

void drawWrapped (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> r, const juce::Font& f, juce::Colour c,
                  juce::Justification j = juce::Justification::topLeft)
{
    layoutText (text, f, c, r.getWidth(), j).draw (g, r);
}

void styleToggle (juce::ToggleButton& b, const juce::String& tip = {})
{
    b.setColour (juce::ToggleButton::textColourId, theme::text);
    if (tip.isNotEmpty()) b.setTooltip (tip);
}

/** List of choices with a radio dot, a title, a line below and an optional tag ("recommended"). */
class ChoiceList final : public juce::Component
{
public:
    struct Item { juce::String title, sub, tag; juce::Colour colour = theme::accentIn; };
    std::function<void (int)> onSelect, onActivate;

    void setItems (std::vector<Item> list, int sel)
    {
        items = std::move (list);
        selected = sel;
        content.setSize (getWidth(), juce::jmax (1, (int) items.size() * kRow));
        repaint();
        content.repaint();
    }
    int getSelected() const noexcept { return selected; }
    int size() const noexcept { return (int) items.size(); }
    void select (int i) { selected = i; content.repaint(); if (onSelect) onSelect (i); }

    ChoiceList()
    {
        addAndMakeVisible (view);
        view.setViewedComponent (&content, false);
        view.setScrollBarsShown (true, false);
        view.setScrollBarThickness (8);
        content.owner = this;
    }
    void resized() override
    {
        view.setBounds (getLocalBounds());
        content.setSize (view.getMaximumVisibleWidth(), juce::jmax (1, (int) items.size() * kRow));
    }
    void paint (juce::Graphics& g) override
    {
        g.setColour (theme::bg.withAlpha (0.5f));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 10.0f);
        if (items.empty() && emptyText.isNotEmpty())
        {
            g.setColour (theme::textDim);
            g.setFont (theme::font (13.5f));
            g.drawFittedText (emptyText, getLocalBounds().reduced (20), juce::Justification::centred, 3);
        }
    }
    juce::String emptyText;
    static constexpr int kRow = 52;

private:
    struct Content final : juce::Component
    {
        ChoiceList* owner = nullptr;
        int hover = -1;
        void paint (juce::Graphics& g) override
        {
            for (int i = 0; i < (int) owner->items.size(); ++i)
            {
                const auto& it = owner->items[(size_t) i];
                auto r = juce::Rectangle<int> (0, i * kRow, getWidth(), kRow).reduced (6, 3).toFloat();
                const bool sel = i == owner->selected;
                if (sel || i == hover)
                {
                    g.setColour (sel ? it.colour.withAlpha (0.13f) : theme::raised.withAlpha (0.6f));
                    g.fillRoundedRectangle (r, 8.0f);
                }
                if (sel)
                {
                    g.setColour (it.colour.withAlpha (0.7f));
                    g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, 1.0f);
                }
                const auto dot = juce::Rectangle<float> (r.getX() + 14.0f, r.getCentreY() - 8.0f, 16.0f, 16.0f);
                g.setColour (sel ? it.colour : theme::textFaint);
                g.drawEllipse (dot.reduced (1.0f), 1.5f);
                if (sel) g.fillEllipse (dot.reduced (4.5f));
                auto text = r.withTrimmedLeft (44.0f).reduced (0, 6.0f);
                if (it.tag.isNotEmpty())
                {
                    const int tw = juce::GlyphArrangement::getStringWidthInt (theme::font (11.0f, true), it.tag) + 16;
                    const auto tag = text.removeFromRight ((float) tw + 8.0f).withSizeKeepingCentre ((float) tw, 20.0f);
                    g.setColour (theme::good.withAlpha (0.15f));
                    g.fillRoundedRectangle (tag, 10.0f);
                    g.setColour (theme::good);
                    g.setFont (theme::font (11.0f, true));
                    g.drawText (it.tag, tag, juce::Justification::centred);
                }
                g.setColour (theme::text);
                g.setFont (theme::font (14.0f, true));
                g.drawText (it.title, text.removeFromTop (it.sub.isEmpty() ? text.getHeight() : 20.0f), juce::Justification::centredLeft, true);
                if (it.sub.isNotEmpty())
                {
                    g.setColour (theme::textDim);
                    g.setFont (theme::font (12.5f));
                    g.drawText (it.sub, text, juce::Justification::centredLeft, true);
                }
            }
        }
        int rowAt (juce::Point<int> p) const { const int i = p.y / kRow; return juce::isPositiveAndBelow (i, (int) owner->items.size()) ? i : -1; }
        void mouseMove (const juce::MouseEvent& e) override { if (const int h = rowAt (e.getPosition()); h != hover) { hover = h; repaint(); } }
        void mouseExit (const juce::MouseEvent&) override { hover = -1; repaint(); }
        void mouseDown (const juce::MouseEvent& e) override { if (const int i = rowAt (e.getPosition()); i >= 0) owner->select (i); }
        void mouseDoubleClick (const juce::MouseEvent& e) override
        {
            if (const int i = rowAt (e.getPosition()); i >= 0 && owner->onActivate) owner->onActivate (i);
        }
    };
    friend struct Content;
    std::vector<Item> items;
    int selected = -1;
    juce::Viewport view;
    Content content;
};

/** - value + */
class Stepper final : public juce::Component
{
public:
    Stepper (juce::String t, juce::String s, juce::Colour c, int lo, int hi, int v)
        : title (std::move (t)), sub (std::move (s)), colour (c), minV (lo), maxV (hi), value (v) {}
    std::function<void()> onChange;
    int getValue() const noexcept { return value; }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (theme::bg.withAlpha (0.5f));
        g.fillRoundedRectangle (r, 10.0f);
        g.setColour (colour);
        g.fillRoundedRectangle (r.removeFromLeft (4.0f).reduced (0, 10.0f), 2.0f);
        auto text = r.reduced (16.0f, 8.0f);
        auto ctrl = text.removeFromRight (140.0f);
        g.setColour (theme::text);
        g.setFont (theme::font (14.5f, true));
        g.drawText (title, text.removeFromTop (text.getHeight() / 2 + 2), juce::Justification::bottomLeft);
        g.setColour (theme::textDim);
        g.setFont (theme::font (12.5f));
        g.drawText (sub, text, juce::Justification::topLeft, true);
        minus = ctrl.removeFromLeft (38.0f).withSizeKeepingCentre (34.0f, 34.0f);
        plus = ctrl.removeFromRight (38.0f).withSizeKeepingCentre (34.0f, 34.0f);
        for (auto [b, sym, on] : { std::tuple { minus, "-", value > minV }, std::tuple { plus, "+", value < maxV } })
        {
            g.setColour (on ? theme::raised : theme::raised.withAlpha (0.4f));
            g.fillRoundedRectangle (b, 8.0f);
            g.setColour (on ? theme::text : theme::textFaint);
            g.setFont (theme::font (19.0f, true));
            g.drawText (sym, b.translated (0, -1.0f), juce::Justification::centred);
        }
        g.setColour (colour);
        g.setFont (theme::font (24.0f, true));
        g.drawText (juce::String (value), ctrl, juce::Justification::centred);
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto p = e.position;
        if (minus.contains (p) && value > minV) set (value - 1);
        else if (plus.contains (p) && value < maxV) set (value + 1);
    }
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        if (w.deltaY > 0 && value < maxV) set (value + 1);
        else if (w.deltaY < 0 && value > minV) set (value - 1);
    }

private:
    void set (int v) { value = v; repaint(); if (onChange) onChange(); }
    juce::String title, sub;
    juce::Colour colour;
    int minV, maxV, value;
    juce::Rectangle<float> minus, plus;
};

/** Big tile of the start page. */
class Tile final : public juce::Component
{
public:
    Tile (int icon, juce::String t, juce::String s, juce::Colour c) : kind (icon), title (std::move (t)), sub (std::move (s)), colour (c)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }
    std::function<void()> onClick;
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (hover ? colour.withAlpha (0.10f) : theme::bg.withAlpha (0.5f));
        g.fillRoundedRectangle (r, 12.0f);
        g.setColour (hover ? colour : theme::outline);
        g.drawRoundedRectangle (r, 12.0f, hover ? 1.5f : 1.0f);
        const auto icon = juce::Rectangle<float> (r.getCentreX() - 30.0f, r.getY() + 26.0f, 60.0f, 60.0f);
        g.setColour (colour.withAlpha (0.15f));
        g.fillEllipse (icon);
        g.setColour (colour);
        const auto c = icon.getCentre();
        if (kind == 0)
        {
            // Fresh start: a spark.
            juce::Path p;
            p.addStar (c, 4, 6.0f, 19.0f, 0.0f);
            g.fillPath (p);
        }
        else if (kind == 1)
        {
            // Voicemeeter: two bars flowing into an arrow.
            g.fillRoundedRectangle (c.x - 16.0f, c.y - 12.0f, 14.0f, 5.0f, 2.0f);
            g.fillRoundedRectangle (c.x - 16.0f, c.y + 7.0f, 14.0f, 5.0f, 2.0f);
            juce::Path a;
            a.startNewSubPath (c.x - 2.0f, c.y - 9.5f);
            a.quadraticTo (c.x + 4.0f, c.y, c.x + 12.0f, c.y);
            a.startNewSubPath (c.x - 2.0f, c.y + 9.5f);
            a.quadraticTo (c.x + 4.0f, c.y, c.x + 12.0f, c.y);
            g.strokePath (a, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            juce::Path head;
            head.addTriangle (c.x + 10.0f, c.y - 6.0f, c.x + 18.0f, c.y, c.x + 10.0f, c.y + 6.0f);
            g.fillPath (head);
        }
        else
        {
            // Hydra settings file: a page with the logo's bars.
            const juce::Rectangle<float> page (c.x - 12.0f, c.y - 16.0f, 24.0f, 32.0f);
            g.drawRoundedRectangle (page, 3.0f, 2.0f);
            for (int i = 0; i < 3; ++i) g.fillRoundedRectangle (page.getX() + 5.0f, page.getY() + 8.0f + (float) i * 6.5f, 14.0f - (float) i * 3.0f, 3.0f, 1.5f);
        }
        g.setColour (theme::text);
        g.setFont (theme::font (16.0f, true));
        g.drawText (title, r.withTop (icon.getBottom() + 16.0f).withHeight (24.0f), juce::Justification::centred);
        drawWrapped (g, sub, r.withTop (icon.getBottom() + 46.0f).reduced (18.0f, 0), theme::font (13.0f), theme::textDim,
                     juce::Justification::centredTop);
    }
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { hover = false; repaint(); }
    void mouseUp (const juce::MouseEvent& e) override  { if (e.mouseWasClicked() && onClick) onClick(); }

private:
    int kind;
    juce::String title, sub;
    juce::Colour colour;
    bool hover = false;
};
} // namespace

//==============================================================================
class WelcomeWizard::PageBase : public juce::Component
{
public:
    explicit PageBase (WelcomeWizard& w) : wizard (w), ctx (w.ctx) {}
    virtual juce::String heading() const = 0;
    virtual juce::String intro() const { return {}; }
    virtual bool canContinue() const { return true; }
    virtual juce::String continueText() const { return "Continue"; }
    virtual bool showsContinue() const { return true; }
    /** Continue pressed: apply the choice; false = stay (error shown). */
    virtual bool apply() { return true; }
    virtual Page nextPage() const = 0;

protected:
    WelcomeWizard& wizard;
    UiContext& ctx;
};

//==============================================================================
class WelcomeWizard::StartPage final : public PageBase
{
public:
    explicit StartPage (WelcomeWizard& w) : PageBase (w)
    {
        for (auto* t : { &fresh, &vm, &hydraFiles }) addAndMakeVisible (t);
        fresh.onClick = [this] { wizard.go (Page::device); };
        vm.onClick = [this] { wizard.go (Page::voicemeeter); };
        hydraFiles.onClick = [this] { wizard.go (Page::hydra); };
    }
    juce::String heading() const override { return "Welcome to Hydra Mixer"; }
    juce::String intro() const override { return "A few questions and your mixer is ready. You can change everything later."; }
    bool showsContinue() const override { return false; }
    Page nextPage() const override { return Page::device; }
    void resized() override
    {
        auto r = getLocalBounds().withTrimmedTop (8);
        const int w = (r.getWidth() - 2 * 14) / 3;
        fresh.setBounds (r.removeFromLeft (w).withHeight (juce::jmin (240, r.getHeight())));
        r.removeFromLeft (14);
        vm.setBounds (r.removeFromLeft (w).withHeight (juce::jmin (240, r.getHeight())));
        r.removeFromLeft (14);
        hydraFiles.setBounds (r.withHeight (juce::jmin (240, r.getHeight())));
    }

private:
    Tile fresh { 0, "Start fresh", "Choose your audio device and how many channels you want.", theme::accentIn },
         vm { 1, "Import from Voicemeeter", "Takes over devices, routing, EQ, names and your MIDI mapping.", theme::accentHw },
         hydraFiles { 2, "I have Hydra settings", "Load an exported settings file or a backup.", theme::accentVirt };
};

//==============================================================================
class WelcomeWizard::DevicePage final : public PageBase
{
public:
    explicit DevicePage (WelcomeWizard& w) : PageBase (w)
    {
        addAndMakeVisible (list);
        addAndMakeVisible (note);
        note.setColour (juce::Label::textColourId, theme::textDim);
        note.setFont (theme::font (12.5f));
        list.emptyText = "No audio device found.";
        list.onActivate = [this] (int) { wizard.continuePressed(); };

        const auto current = ctx.engine.getMasterConfig();
        std::vector<ChoiceList::Item> items;
        int sel = -1;
        bool haveLowLatency = false;
        for (auto* t : ctx.engine.getDeviceTypes()) haveLowLatency |= t->getTypeName().containsIgnoreCase ("Low Latency");
        for (auto* t : ctx.engine.getDeviceTypes())
        {
            const auto type = t->getTypeName();
            const bool asio = type == "ASIO";
            const bool windows = haveLowLatency ? type.containsIgnoreCase ("Low Latency") : type == "Windows Audio";
            if (! asio && ! windows) continue;
            for (auto& n : t->getDeviceNames (false))
            {
                if (isCableName (n)) continue;   // a virtual cable / Voicemeeter's virtual ASIO is no main device
                if (current.typeName == type && current.deviceName == n) sel = (int) items.size();
                items.push_back ({ n, asio ? "ASIO driver: lowest latency, for audio interfaces" : "Windows device: works with any sound card",
                                   {}, asio ? theme::accentIn : theme::accentHw });
                choices.push_back ({ type, n });
            }
        }
        // Recommended: the running ASIO device, else the first ASIO device (an interface's own driver).
        int rec = sel >= 0 && choices[(size_t) sel].first == "ASIO" ? sel : -1;
        for (int i = 0; rec < 0 && i < (int) choices.size(); ++i) if (choices[(size_t) i].first == "ASIO") rec = i;
        if (rec >= 0) items[(size_t) rec].tag = "recommended";
        if (sel < 0 && ! items.empty()) sel = rec >= 0 ? rec : 0;
        list.setItems (std::move (items), sel);
        note.setText ("The master device is Hydra's clock. Every other device (headset, cables, a second interface) is synced to it.",
                      juce::dontSendNotification);
    }

    static bool isCableName (const juce::String& n)
    {
        return n.containsIgnoreCase ("cable") || n.containsIgnoreCase ("voicemeeter") || n.containsIgnoreCase ("virtual");
    }
    juce::String heading() const override { return "Your main audio device"; }
    juce::String intro() const override { return "Pick the interface or sound card your speakers or headphones are connected to."; }
    bool canContinue() const override { return list.getSelected() >= 0 || choices.empty(); }
    Page nextPage() const override { return Page::channels; }

    bool apply() override
    {
        const int i = list.getSelected();
        if (! juce::isPositiveAndBelow (i, (int) choices.size())) return true;
        const auto current = ctx.engine.getMasterConfig();
        if (current.typeName == choices[(size_t) i].first && current.deviceName == choices[(size_t) i].second && ctx.engine.getMasterDevice() != nullptr)
            return true;
        MasterConfig cfg;
        cfg.typeName = choices[(size_t) i].first;
        cfg.deviceName = choices[(size_t) i].second;
        const auto err = ctx.engine.setMasterConfig (cfg);
        if (err.isNotEmpty())
        {
            dialog::message (&wizard, dialog::Kind::warning, "This device did not start", err + "\n\nIs another program using it (Voicemeeter, a DAW)? Pick another one or close that program.");
            return false;
        }
        if (ctx.session != nullptr) ctx.session->requestedMaster = cfg;
        if (wizard.onChanged) wizard.onChanged();
        return true;
    }

    void resized() override
    {
        auto r = getLocalBounds();
        note.setBounds (r.removeFromBottom (24));
        r.removeFromBottom (8);
        list.setBounds (r);
    }

private:
    ChoiceList list;
    juce::Label note;
    std::vector<std::pair<juce::String, juce::String>> choices;   // type, device
};

//==============================================================================
class WelcomeWizard::ChannelsPage final : public PageBase
{
public:
    explicit ChannelsPage (WelcomeWizard& w) : PageBase (w)
    {
        for (auto* s : { &ins, &hws, &virts })
        {
            addAndMakeVisible (s);
            s->onChange = [this] { startGlide(); };
        }
        for (int g = 0; g < 3; ++g) shown[g] = (float) value (g);
    }
    juce::String heading() const override { return "How many channels?"; }
    juce::String intro() const override { return "Start small: + adds a channel any time, right-click removes one."; }
    Page nextPage() const override { return Page::cables; }

    bool apply() override
    {
        auto& e = ctx.engine;
        auto setCount = [&e, this] (int group, int want)
        {
            const bool isInput = group == 0, virt = group == 2;
            auto count = [&] { return isInput ? e.numActiveInputs() : e.numActiveBuses (virt); };
            while (count() < want && (isInput ? e.addInput() : e.addBus (virt)) >= 0) {}
            for (int guard = 0; count() > want && guard < kMaxBuses + kMaxInputs; ++guard)
            {
                // Remove from the end (the last active slot of the group); its MIDI mappings and shortcuts go too.
                std::vector<Param*> gone;
                if (isInput)
                {
                    for (int i = kMaxInputs - 1; i >= 0; --i) if (e.isInputActive (i)) { gone = e.removeInput (i); break; }
                }
                else
                {
                    const int from = virt ? kMaxBuses - 1 : kMaxHwBuses - 1, to = virt ? kMaxHwBuses : 0;
                    for (int b = from; b >= to; --b) if (e.isBusActive (b)) { gone = e.removeBus (b); break; }
                }
                for (auto* p : gone)
                {
                    if (ctx.midi.hasMapping (p)) ctx.midi.removeMappingsFor (p);
                    if (ctx.hotkeys.getKeyFor (p).isValid()) ctx.hotkeys.remove (p);
                }
            }
        };
        setCount (0, ins.getValue());
        setCount (1, hws.getValue());
        setCount (2, virts.getValue());
        if (wizard.onChanged) wizard.onChanged();
        return true;
    }

    void resized() override
    {
        auto r = getLocalBounds();
        preview = r.removeFromTop (120);
        r.removeFromTop (14);
        for (auto* s : { &ins, &hws, &virts })
        {
            s->setBounds (r.removeFromTop (66));
            r.removeFromTop (10);
        }
    }

    void paint (juce::Graphics& g) override
    {
        // Mini mixer: one bar per channel, in the colours of the groups.
        auto r = preview.toFloat();
        g.setColour (theme::bg.withAlpha (0.5f));
        g.fillRoundedRectangle (r, 10.0f);
        r = r.reduced (16.0f, 16.0f);
        const juce::Colour cols[] = { theme::accentIn, theme::accentHw, theme::accentVirt };
        const float total = shown[0] + shown[1] + shown[2];
        const float gap = 18.0f;
        const float barW = juce::jmin (22.0f, (r.getWidth() - 2.0f * gap) / juce::jmax (1.0f, total) - 4.0f);
        float x = r.getCentreX() - (total * (barW + 4.0f) + 2.0f * gap) / 2.0f;
        for (int grp = 0; grp < 3; ++grp)
        {
            const int whole = (int) std::ceil (shown[grp] - 0.001f);
            for (int i = 0; i < whole; ++i)
            {
                const float part = juce::jlimit (0.0f, 1.0f, shown[grp] - (float) i);
                const auto bar = juce::Rectangle<float> (x, r.getY(), (barW + 4.0f) * part - 4.0f, r.getHeight());
                if (bar.getWidth() > 0.5f)
                {
                    g.setColour (cols[grp].withAlpha (0.16f * part));
                    g.fillRoundedRectangle (bar, 4.0f);
                    g.setColour (cols[grp].withAlpha (part));
                    g.fillRoundedRectangle (bar.withHeight (3.0f), 1.5f);
                    // Fader knob.
                    const float y = bar.getY() + bar.getHeight() * (0.35f + 0.12f * (float) ((i * 7 + grp * 3) % 5) / 4.0f);
                    g.setColour (theme::text.withAlpha (0.8f * part));
                    g.fillRoundedRectangle (bar.getX() + 2.0f, y, juce::jmax (0.0f, bar.getWidth() - 4.0f), 6.0f, 2.0f);
                }
                x += (barW + 4.0f) * part;
            }
            x += gap;
        }
    }

private:
    int value (int g) const { return g == 0 ? ins.getValue() : g == 1 ? hws.getValue() : virts.getValue(); }
    void startGlide() { gliding = true; }
    void frame()
    {
        if (! gliding) return;
        bool moving = false;
        for (int g = 0; g < 3; ++g)
        {
            const float target = (float) value (g);
            shown[g] += (target - shown[g]) * 0.22f;
            if (std::abs (target - shown[g]) < 0.01f) shown[g] = target;
            else moving = true;
        }
        gliding = moving;
        repaint (preview);
    }

    /** First start: a small mixer (4 / 2 / 2). Run again later (Tools > Setup wizard): what there is now, so
        Continue never removes channels unasked. */
    bool firstSetup() const { return ctx.session != nullptr && ! ctx.session->prefs.setupDone; }
    int startCount (int group, int small) const
    {
        const int now = group == 0 ? ctx.engine.numActiveInputs() : ctx.engine.numActiveBuses (group == 2);
        return firstSetup() ? juce::jmin (small, juce::jmax (group == 2 ? 0 : 1, now)) : now;
    }
    Stepper ins { "Inputs", "Microphone, apps, games, cables: one channel per source", theme::accentIn, 1, kMaxInputs, startCount (0, 4) },
            hws { "Hardware outputs (H)", "Speakers, headphones, audio interfaces", theme::accentHw, 1, kMaxHwBuses, startCount (1, 2) },
            virts { "Virtual outputs (V)", "Mixes for apps: Discord, OBS, recordings", theme::accentVirt, 0, kMaxVirtBuses, startCount (2, 2) };
    juce::Rectangle<int> preview;
    float shown[3] {};
    bool gliding = false;
    juce::VBlankAttachment vblank { this, [this] (double) { frame(); } };
};

//==============================================================================
class WelcomeWizard::CablesPage final : public PageBase
{
public:
    explicit CablesPage (WelcomeWizard& w) : PageBase (w)
    {
        addAndMakeVisible (list);
        addAndMakeVisible (ownCables);
        addAndMakeVisible (getVac);
        addAndMakeVisible (getVb);
        styleToggle (ownCables, "Each virtual output gets a cable of its own (Hydra In / Hydra Out in Windows). Changing the number of "
                                "cables restarts the cable driver (maybe one administrator prompt).");
        ownCables.setToggleState (true, juce::dontSendNotification);
        getVac.onClick = [] { juce::URL ("https://vac.muzychenko.net/en/").launchInDefaultBrowser(); };
        getVb.onClick = [] { juce::URL ("https://vb-audio.com/Cable/").launchInDefaultBrowser(); };

        vacInstalled = ctx.session != nullptr && ctx.session->vacCables != nullptr && ctx.session->vacCables->isAvailable();
        int others = 0;
        for (auto& c : findVirtualCables (ctx.engine.getDeviceTypes()))
            if (! c.label.containsIgnoreCase ("Line") || ! vacInstalled) ++others;
        std::vector<ChoiceList::Item> items;
        items.push_back ({ "Virtual Audio Cable", vacInstalled ? "Installed. Hydra manages the cables for you (recommended)."
                                                               : "Not installed. Hydra can manage its cables for you.",
                           vacInstalled ? "recommended" : juce::String(), theme::accentVirt });
        items.push_back ({ "VB-Cable or other cables", others > 0 ? juce::String (others) + " cable(s) found. Pick one per output in its device list."
                                                                  : "None found. Install them, then pick one per output.",
                           {}, theme::accentHw });
        items.push_back ({ "No cables for now", "Inputs still hear apps directly. Add cables later in Settings > Virtual Cables.", {}, theme::textDim });
        list.setItems (std::move (items), vacInstalled ? 0 : others > 0 ? 1 : 2);
        list.onSelect = [this] (int) { resized(); };
    }
    juce::String heading() const override { return "Virtual cables"; }
    juce::String intro() const override
    {
        return "Hydra hears apps directly. For Discord, OBS & co. to hear a virtual output, it plays into a virtual cable "
               "that the app uses as its microphone.";
    }
    Page nextPage() const override { return Page::plugins; }

    bool apply() override
    {
        if (list.getSelected() != 0 || ! vacInstalled || ! ownCables.getToggleState() || ctx.session == nullptr) return true;
        // One cable per virtual output, one after the other (each may restart the driver).
        auto* cables = ctx.session->vacCables;
        auto pending = std::make_shared<std::vector<int>>();
        for (int b = kMaxHwBuses; b < kMaxBuses; ++b)
            if (ctx.engine.isBusActive (b) && cables->cableOf (ctx.engine.buses[(size_t) b].output) == 0) pending->push_back (b);
        auto step = std::make_shared<std::function<void()>>();
        *step = [cables, pending, step]
        {
            if (pending->empty()) { *step = nullptr; return; }
            const int b = pending->front();
            pending->erase (pending->begin());
            cables->addCableFor (false, b, [step] (bool, const juce::String&) { if (*step) (*step)(); });
        };
        (*step)();
        return true;
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto bottom = r.removeFromBottom (34);
        r.removeFromBottom (10);
        list.setBounds (r.removeFromTop (juce::jmin (r.getHeight(), 3 * ChoiceList::kRow + 4)));
        const int sel = list.getSelected();
        ownCables.setVisible (sel == 0 && vacInstalled);
        getVac.setVisible (sel == 0 && ! vacInstalled);
        getVb.setVisible (sel == 1);
        ownCables.setBounds (bottom);
        getVac.setBounds (bottom.withWidth (240));
        getVb.setBounds (bottom.withWidth (200));
    }

private:
    ChoiceList list;
    juce::ToggleButton ownCables { "Give each virtual output its own cable now" };
    dialog::ActionButton getVac { { "Get Virtual Audio Cable" }, theme::accentIn }, getVb { { "Get VB-Cable" }, theme::accentIn };
    bool vacInstalled = false;
};

//==============================================================================
class WelcomeWizard::PluginsPage final : public PageBase
{
public:
    explicit PluginsPage (WelcomeWizard& w) : PageBase (w)
    {
        addAndMakeVisible (scan);
        addAndMakeVisible (folders);
        styleToggle (scan);
        folders.setMultiLine (true);
        folders.setReadOnly (true);
        folders.setFont (theme::mono (12.0f));
        folders.setColour (juce::TextEditor::backgroundColourId, theme::bg.withAlpha (0.5f));
        folders.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        juce::StringArray lines;
        bool any = false;
        for (auto& p : ctx.engine.pluginHost.getSearchPaths())
        {
            const juce::File f (p);
            const int n = f.isDirectory() ? f.getNumberOfChildFiles (juce::File::findFilesAndDirectories, "*.vst3") : 0;
            any |= n > 0;
            lines.add (p + (f.isDirectory() ? "   (" + juce::String (n) + " found)" : "   (missing)"));
        }
        folders.setText (lines.joinIntoString ("\n"), false);
        scan.setToggleState (any || ctx.engine.pluginHost.known.getNumTypes() == 0, juce::dontSendNotification);
    }
    juce::String heading() const override { return "VST3 plugins"; }
    juce::String intro() const override
    {
        return "Every channel can use VST3 effects next to Hydra's own. Scanning runs in the background, "
               "a plugin that crashes is skipped.";
    }
    Page nextPage() const override { return Page::extras; }
    bool apply() override
    {
        if (scan.getToggleState() && wizard.onScanPlugins) wizard.onScanPlugins();
        return true;
    }
    void resized() override
    {
        auto r = getLocalBounds();
        scan.setBounds (r.removeFromTop (30));
        r.removeFromTop (12);
        folders.setBounds (r.removeFromTop (juce::jmin (r.getHeight(), 130)));
    }

private:
    juce::ToggleButton scan { "Look for VST3 plugins in these folders" };
    juce::TextEditor folders;
};

//==============================================================================
class WelcomeWizard::ExtrasPage final : public PageBase
{
public:
    explicit ExtrasPage (WelcomeWizard& w) : PageBase (w)
    {
        styleToggle (autostart, "Starts right when you log on, with high priority, so audio works from the first moment.");
        styleToggle (remote, "Hydra serves a page for your phone or tablet: faders, routing and scenes over Wi-Fi.");
        styleToggle (network, "Hydra Link: send and receive audio between computers running Hydra, and the Hydra Link phone app.");
        styleToggle (updates);
        styleToggle (beta);
        for (auto* b : { &autostart, &remote, &network, &updates, &beta }) addAndMakeVisible (b);
        addAndMakeVisible (note);
        note.setFont (theme::font (12.5f));
        note.setColour (juce::Label::textColourId, theme::textDim);
        note.setJustificationType (juce::Justification::topLeft);
        if (ctx.session != nullptr)
        {
            remote.setToggleState (ctx.session->prefs.remoteEnabled, juce::dontSendNotification);
            network.setToggleState (ctx.session->prefs.netEnabled, juce::dontSendNotification);
            updates.setToggleState (ctx.session->prefs.updateCheck, juce::dontSendNotification);
            beta.setToggleState (ctx.session->prefs.updateBeta, juce::dontSendNotification);
        }
        updates.onClick = [this] { beta.setEnabled (updates.getToggleState()); };
        remote.onClick = network.onClick = [this] { updateNote(); };
        beta.setEnabled (updates.getToggleState());
        updateNote();
    }
    juce::String heading() const override { return "A few extras"; }
    juce::String intro() const override { return "All of these can be changed later in the settings."; }
    Page nextPage() const override { return Page::finish; }

    bool apply() override
    {
        if (ctx.session == nullptr) return true;
        auto& p = ctx.session->prefs;
        p.updateCheck = updates.getToggleState();
        p.updateBeta = beta.getToggleState();
        const bool wantNet = remote.getToggleState() || network.getToggleState();
        const bool wantAuto = autostart.getToggleState();
        auto applyNet = [&ctx = ctx, r = remote.getToggleState(), n = network.getToggleState()]
        {
            ctx.session->prefs.remoteEnabled = r;
            ctx.session->prefs.netEnabled = n;
            ctx.session->prefsVersion.fetch_add (1);
            if (ctx.prefsChanged) ctx.prefsChanged();
        };
        ctx.session->prefsVersion.fetch_add (1);
        // Network access: the firewall rule first (one administrator prompt instead of Windows asking), then switch on.
        const bool needRule = wantNet && ! install::firewallAllows (install::runningExe());
        if (wantAuto || needRule)
        {
            juce::Thread::launch ([wantAuto, needRule, applyNet]
            {
                if (wantAuto && ! Autostart::isEnabled()) Autostart::setEnabled (true);
                if (needRule) install::requestFirewall (install::runningExe(), true);
                juce::MessageManager::callAsync (applyNet);
            });
        }
        else
            applyNet();
        return true;
    }

    void resized() override
    {
        auto r = getLocalBounds();
        for (auto* b : { &autostart, &remote, &network })
        {
            b->setBounds (r.removeFromTop (30));
            r.removeFromTop (4);
        }
        r.removeFromTop (12);
        updates.setBounds (r.removeFromTop (30));
        r.removeFromTop (4);
        beta.setBounds (r.removeFromTop (30).withTrimmedLeft (28));
        r.removeFromTop (14);
        note.setBounds (r.removeFromTop (60));
    }

private:
    void updateNote()
    {
        const bool net = remote.getToggleState() || network.getToggleState();
        note.setText (net ? "Network access: Windows asks once for permission (an administrator prompt), not at every start."
                          : juce::String(), juce::dontSendNotification);
    }
    juce::ToggleButton autostart { "Start Hydra Mixer with Windows" },
                       remote { "Control Hydra from my phone or tablet (Hydra Remote)" },
                       network { "Send audio over the network to other computers and phones (Hydra Link)" },
                       updates { "Tell me when a new version is available" },
                       beta { "Include beta versions (new features earlier, less tested)" };
    juce::Label note;
};

//==============================================================================
class WelcomeWizard::FinishPage final : public PageBase
{
public:
    explicit FinishPage (WelcomeWizard& w) : PageBase (w)
    {
        addAndMakeVisible (tour);
        addAndMakeVisible (later);
        tour.onClick = [this] { wizard.done (Tour::full); };
        later.onClick = [this] { wizard.done (Tour::none); };
    }
    juce::String heading() const override { return "Your mixer is ready"; }
    juce::String intro() const override
    {
        auto& e = ctx.engine;
        juce::String s;
        if (auto* d = e.getMasterDevice()) s << "Running on " << d->getName() << " with ";
        s << e.numActiveInputs() << " inputs, " << e.numActiveBuses (false) << " hardware and " << e.numActiveBuses (true)
          << " virtual outputs.";
        return s;
    }
    bool showsContinue() const override { return false; }
    Page nextPage() const override { return Page::finish; }
    void paint (juce::Graphics& g) override
    {
        g.setColour (theme::text);
        g.setFont (theme::font (16.0f, true));
        g.drawText ("Want a quick tour?", getLocalBounds().withHeight (28), juce::Justification::centredLeft);
        drawWrapped (g, "2 minutes, right in your mixer: what the areas are, how routing works, where the effects are. "
                        "You do most of it yourself. You can pause it any time and continue later.",
                     getLocalBounds().toFloat().withTrimmedTop (32.0f).withHeight (60.0f), theme::font (13.5f), theme::textDim);
    }
    void resized() override
    {
        auto r = getLocalBounds().withTrimmedTop (104).removeFromTop (40);
        tour.setBounds (r.removeFromLeft (230));
        r.removeFromLeft (10);
        later.setBounds (r.removeFromLeft (120));
    }

private:
    dialog::ActionButton tour { { "Show me around", dialog::Button::Role::primary }, theme::accentIn }, later { { "Not now" }, theme::accentIn };
};

//==============================================================================
class WelcomeWizard::VoicemeeterPage final : public PageBase
{
public:
    explicit VoicemeeterPage (WelcomeWizard& w) : PageBase (w)
    {
        addAndMakeVisible (list);
        addAndMakeVisible (browse);
        addAndMakeVisible (note);
        note.setFont (theme::font (12.5f));
        note.setColour (juce::Label::textColourId, theme::warn);
        note.setText ("Close Voicemeeter first, so its ASIO driver is free for Hydra.", juce::dontSendNotification);
        list.emptyText = "Looking for Voicemeeter settings in Documents...";
        list.onSelect = [this] (int) { wizard.updateButtons(); };
        list.onActivate = [this] (int) { wizard.continuePressed(); };
        browse.onClick = [this]
        {
            chooser = std::make_unique<juce::FileChooser> ("Voicemeeter settings (Menu > Save Settings)",
                                                           juce::File::getSpecialLocation (juce::File::userDocumentsDirectory), "*.xml");
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc)
            {
                const auto f = fc.getResult();
                if (! f.existsAsFile()) return;
                files.insert (files.begin(), f);
                refill (0);
            });
        };
        juce::Component::SafePointer<VoicemeeterPage> safe (this);
        juce::Thread::launch ([safe]
        {
            auto found = VoicemeeterImport::findCandidates();
            juce::MessageManager::callAsync ([safe, found]
            {
                if (safe == nullptr) return;
                for (auto& f : found) safe->files.push_back (f);
                safe->list.emptyText = "No Voicemeeter settings found in Documents. In Voicemeeter: Menu > Save Settings, then Browse.";
                safe->refill (safe->list.getSelected() >= 0 ? safe->list.getSelected() : 0);
            });
        });
    }
    juce::String heading() const override { return "Import from Voicemeeter"; }
    juce::String intro() const override { return "Pick the settings file Voicemeeter saved (Menu > Save Settings). The newest is on top."; }
    bool canContinue() const override { return juce::isPositiveAndBelow (list.getSelected(), (int) files.size()); }
    juce::String continueText() const override { return "Import"; }
    Page nextPage() const override { return Page::vmDone; }

    bool apply() override
    {
        const int i = list.getSelected();
        if (! juce::isPositiveAndBelow (i, (int) files.size())) return false;
        if (ctx.session != nullptr) { ctx.session->saveDefault(); ctx.session->backup ("before-voicemeeter-import"); }
        const auto result = VoicemeeterImport::run (files[(size_t) i], ctx.engine, ctx.midi, true);
        if (! result.ok)
        {
            dialog::message (&wizard, dialog::Kind::warning, "Import failed", result.report.joinIntoString ("\n"));
            return false;
        }
        wizard.vmReport = result.report;
        if (wizard.onChanged) wizard.onChanged();
        return true;
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto bottom = r.removeFromBottom (34);
        browse.setBounds (bottom.removeFromRight (120));
        note.setBounds (bottom);
        r.removeFromBottom (10);
        list.setBounds (r);
    }

private:
    void refill (int sel)
    {
        std::vector<ChoiceList::Item> items;
        for (auto& f : files)
            items.push_back ({ f.getFileName(), f.getParentDirectory().getFullPathName() + "  ·  " + f.getLastModificationTime().formatted ("%d.%m.%Y %H:%M"),
                               {}, theme::accentHw });
        list.setItems (std::move (items), files.empty() ? -1 : juce::jlimit (0, (int) files.size() - 1, sel));
        wizard.updateButtons();
    }
    ChoiceList list;
    dialog::ActionButton browse { { "Browse..." }, theme::accentIn };
    juce::Label note;
    std::vector<juce::File> files;
    std::unique_ptr<juce::FileChooser> chooser;
};

class WelcomeWizard::VmDonePage final : public PageBase
{
public:
    explicit VmDonePage (WelcomeWizard& w) : PageBase (w)
    {
        addAndMakeVisible (report);
        addAndMakeVisible (tour);
        addAndMakeVisible (later);
        report.setMultiLine (true);
        report.setReadOnly (true);
        report.setFont (theme::mono (12.0f));
        report.setColour (juce::TextEditor::backgroundColourId, theme::bg.withAlpha (0.5f));
        report.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        report.setText (w.vmReport.joinIntoString ("\n"), false);
        tour.onClick = [this] { wizard.done (Tour::voicemeeter); };
        later.onClick = [this] { wizard.done (Tour::none); };
    }
    juce::String heading() const override { return "Imported"; }
    juce::String intro() const override { return "Your Voicemeeter setup is now in Hydra. Want to see what is different (1 minute)?"; }
    bool showsContinue() const override { return false; }
    Page nextPage() const override { return Page::vmDone; }
    void resized() override
    {
        auto r = getLocalBounds();
        auto row = r.removeFromBottom (40);
        tour.setBounds (row.removeFromLeft (250));
        row.removeFromLeft (10);
        later.setBounds (row.removeFromLeft (120));
        r.removeFromBottom (14);
        report.setBounds (r);
    }

private:
    juce::TextEditor report;
    dialog::ActionButton tour { { "Show the differences", dialog::Button::Role::primary }, theme::accentIn }, later { { "Not now" }, theme::accentIn };
};

//==============================================================================
class WelcomeWizard::HydraPage final : public PageBase
{
public:
    explicit HydraPage (WelcomeWizard& w) : PageBase (w)
    {
        addAndMakeVisible (list);
        addAndMakeVisible (browse);
        list.emptyText = "No backups on this computer. Use Browse to load an exported settings file (.xml).";
        list.onSelect = [this] (int) { wizard.updateButtons(); };
        list.onActivate = [this] (int) { wizard.continuePressed(); };
        if (ctx.session != nullptr)
            for (auto& f : ctx.session->listBackups())
                files.push_back (f);
        browse.onClick = [this]
        {
            chooser = std::make_unique<juce::FileChooser> ("Hydra Mixer settings", juce::File::getSpecialLocation (juce::File::userDocumentsDirectory), "*.xml");
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc)
            {
                const auto f = fc.getResult();
                if (! f.existsAsFile()) return;
                files.insert (files.begin(), f);
                refill (0);
            });
        };
        refill (files.empty() ? -1 : 0);
    }
    juce::String heading() const override { return "Load your Hydra settings"; }
    juce::String intro() const override { return "An exported settings file (Settings > Backup & Transfer) or one of the automatic backups."; }
    bool canContinue() const override { return juce::isPositiveAndBelow (list.getSelected(), (int) files.size()); }
    juce::String continueText() const override { return "Load"; }
    Page nextPage() const override { return Page::hydra; }

    bool apply() override
    {
        const int i = list.getSelected();
        if (! juce::isPositiveAndBelow (i, (int) files.size()) || ctx.session == nullptr) return false;
        juce::String err;
        if (! ctx.session->importFrom (files[(size_t) i], err))
        {
            dialog::message (&wizard, dialog::Kind::warning, "Not loaded", err);
            return false;
        }
        if (wizard.onChanged) wizard.onChanged();
        wizard.done (Tour::none);   // people with their own settings know Hydra: no tour
        return false;               // (the wizard is gone)
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto bottom = r.removeFromBottom (34);
        browse.setBounds (bottom.removeFromRight (120));
        r.removeFromBottom (10);
        list.setBounds (r);
    }

private:
    void refill (int sel)
    {
        std::vector<ChoiceList::Item> items;
        for (auto& f : files)
            items.push_back ({ f.getFileName(), f.getLastModificationTime().formatted ("%d.%m.%Y %H:%M") + "  ·  " + f.getParentDirectory().getFullPathName(),
                               {}, theme::accentVirt });
        list.setItems (std::move (items), sel);
        wizard.updateButtons();
    }
    ChoiceList list;
    dialog::ActionButton browse { { "Browse..." }, theme::accentIn };
    std::vector<juce::File> files;
    std::unique_ptr<juce::FileChooser> chooser;
};

//==============================================================================
WelcomeWizard::WelcomeWizard (UiContext& c, juce::Component& host) : ctx (c)
{
    for (auto* b : { &backBtn, &nextBtn, &skipBtn }) addAndMakeVisible (b);
    backBtn.onClick = [this] { backPressed(); };
    nextBtn.onClick = [this] { continuePressed(); };
    skipBtn.onClick = [this]
    {
        juce::Component::SafePointer<WelcomeWizard> safe (this);
        dialog::confirm (this, dialog::Kind::question, "Skip the setup?",
                         "Hydra starts with its default channels on your Windows default device. You can run this setup again "
                         "any time: Tools > Setup wizard.", "Skip", [safe] { if (safe != nullptr) safe->done (Tour::none); });
    };
    setWantsKeyboardFocus (true);
    setAlwaysOnTop (true);
    host.addAndMakeVisible (this);
    setBounds (host.getLocalBounds());
    go (Page::start);
    setAlpha (0.0f);
    juce::Desktop::getInstance().getAnimator().animateComponent (this, getBounds(), 1.0f, 180, false, 1.0, 1.0);
    grabKeyboardFocus();
}

WelcomeWizard::~WelcomeWizard() = default;

void WelcomeWizard::go (Page p, bool forward)
{
    if (forward && content != nullptr) history.push_back (page);
    page = p;
    std::unique_ptr<PageBase> next;
    switch (p)
    {
        case Page::start:       next = std::make_unique<StartPage> (*this); break;
        case Page::device:      next = std::make_unique<DevicePage> (*this); break;
        case Page::channels:    next = std::make_unique<ChannelsPage> (*this); break;
        case Page::cables:      next = std::make_unique<CablesPage> (*this); break;
        case Page::plugins:     next = std::make_unique<PluginsPage> (*this); break;
        case Page::extras:      next = std::make_unique<ExtrasPage> (*this); break;
        case Page::finish:      next = std::make_unique<FinishPage> (*this); break;
        case Page::voicemeeter: next = std::make_unique<VoicemeeterPage> (*this); break;
        case Page::vmDone:      next = std::make_unique<VmDonePage> (*this); break;
        case Page::hydra:       next = std::make_unique<HydraPage> (*this); break;
    }
    content = std::move (next);
    addAndMakeVisible (*content);
    resized();
    updateButtons();
    repaint();
}

void WelcomeWizard::updateButtons()
{
    if (content == nullptr) return;
    nextBtn.setVisible (content->showsContinue());
    nextBtn.setText (content->continueText());
    nextBtn.setEnabled (content->canContinue());
    backBtn.setVisible (! history.empty() && page != Page::vmDone);
    skipBtn.setVisible (page == Page::start);
    resized();
}

void WelcomeWizard::continuePressed()
{
    if (content == nullptr || ! content->showsContinue() || ! content->canContinue()) return;
    juce::Component::SafePointer<WelcomeWizard> safe (this);
    const auto target = content->nextPage();
    if (! content->apply() || safe == nullptr) return;
    go (target);
}

void WelcomeWizard::backPressed()
{
    if (history.empty()) return;
    const auto p = history.back();
    history.pop_back();
    go (p, false);
}

void WelcomeWizard::done (Tour t)
{
    auto cb = onDone;
    setVisible (false);
    if (cb) cb (t);   // deletes this
}

bool WelcomeWizard::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::returnKey) { continuePressed(); return true; }
    if (k == juce::KeyPress::escapeKey) { if (page == Page::start) skipBtn.onClick(); else backPressed(); return true; }
    return true;   // the mixer below does not get keys meanwhile
}

void WelcomeWizard::resized()
{
    card = juce::Rectangle<int> (juce::jmin (kCardW, getWidth() - 32), juce::jmin (kCardH, getHeight() - 32)).withCentre (getLocalBounds().getCentre());
    auto r = card.reduced (kPad);
    auto footer = r.removeFromBottom (36);
    r.removeFromBottom (16);
    // Heading and intro are painted; the page gets the rest.
    const auto introH = content != nullptr && content->intro().isNotEmpty()
                            ? (int) std::ceil (layoutText (content->intro(), theme::font (14.0f), theme::textDim, (float) r.getWidth()).getHeight())
                            : 0;
    r.removeFromTop (34 + 30 + introH + 20);
    if (content != nullptr) content->setBounds (r);
    nextBtn.setBounds (footer.removeFromRight (juce::jmax (120, nextBtn.idealWidth())));
    footer.removeFromRight (8);
    if (backBtn.isVisible()) backBtn.setBounds (footer.removeFromRight (100));
    skipBtn.setBounds (footer.removeFromLeft (120));
}

void WelcomeWizard::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black.withAlpha (0.7f));
    const auto c = card.toFloat();
    juce::Path shape;
    shape.addRoundedRectangle (c, 16.0f);
    juce::DropShadow (juce::Colours::black.withAlpha (0.6f), 40, { 0, 10 }).drawForPath (g, shape);
    g.setColour (theme::panel);
    g.fillPath (shape);
    {
        juce::Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (shape);
        g.setGradientFill (juce::ColourGradient (theme::accentIn, c.getX(), 0, theme::accentHw, c.getRight(), 0, false));
        g.fillRect (c.withHeight (3.0f));
    }
    g.setColour (theme::outline);
    g.strokePath (shape, juce::PathStrokeType (1.0f));
    if (content == nullptr) return;

    auto r = card.reduced (kPad).toFloat();
    theme::drawLogo (g, r.removeFromTop (34.0f).withWidth (30.0f));
    // Progress of the fresh path (5 steps).
    const Page freshPages[] = { Page::device, Page::channels, Page::cables, Page::plugins, Page::extras, Page::finish };
    int at = -1;
    for (int i = 0; i < 6; ++i) if (freshPages[i] == page) at = i;
    if (at >= 0)
    {
        for (int i = 0; i < 6; ++i)
        {
            const auto d = juce::Rectangle<float> (card.getRight() - kPad - (6 - i) * 26.0f, (float) card.getY() + kPad + 12.0f, 20.0f, 4.0f);
            g.setColour (i <= at ? theme::accentIn : theme::outline);
            g.fillRoundedRectangle (d, 2.0f);
        }
    }
    g.setColour (theme::text);
    g.setFont (theme::font (22.0f, true));
    g.drawText (content->heading(), r.removeFromTop (30.0f), juce::Justification::centredLeft, true);
    const auto intro = content->intro();
    if (intro.isNotEmpty())
    {
        auto l = layoutText (intro, theme::font (14.0f), theme::textDim, r.getWidth());
        l.draw (g, r.removeFromTop (l.getHeight() + 2.0f));
    }
}

//==============================================================================
InstallerPanel::InstallerPanel (Mode m) : mode (m)
{
    const auto version = install::currentVersion();
    for (auto* b : { &startMenu, &desktop, &firewall, &launch, &keepSettings }) styleToggle (*b);
    addAndMakeVisible (primary);
    addAndMakeVisible (secondary);
    if (mode == Mode::install)
    {
        title = "Install Hydra Mixer " + version;
        text = "Installs for your Windows user in " + install::installDir().getFullPathName()
             + ". No administrator rights needed, and updates install themselves from inside the app.";
        if (install::isInstalled())
        {
            const auto old = install::installedVersion();
            text << "\n\nHydra Mixer " << (old.isNotEmpty() ? old + " " : juce::String()) << "is installed: it is replaced, your settings stay.";
            primary.setText (old.isNotEmpty() && install::compareVersions (old, version) > 0 ? "Install older version" : "Update");
        }
        for (auto* b : { &startMenu, &desktop, &firewall, &launch }) addAndMakeVisible (b);
        startMenu.setToggleState (true, juce::dontSendNotification);
        firewall.setToggleState (! install::firewallAllows (install::installedExe()), juce::dontSendNotification);
        firewall.setTooltip ("Windows remembers network access per program path. Allowed once here, Hydra Link and the phone "
                             "control work in every network without asking again, also after updates.");
        launch.setToggleState (true, juce::dontSendNotification);
        primary.onClick = [this] { startInstall(); };
        secondary.onClick = [this] { if (onFinished) onFinished (false); };
        secondary.setVisible (! install::isMixerRunning());   // one mixer at a time
    }
    else
    {
        title = "Uninstall Hydra Mixer";
        text = "Removes Hydra Mixer from this computer: the program, its Start menu entry, start with Windows and its firewall rule.";
        addAndMakeVisible (keepSettings);
        keepSettings.setToggleState (true, juce::dontSendNotification);
        primary.spec.role = dialog::Button::Role::destructive;
        primary.setText ("Uninstall");
        secondary.setText ("Cancel");
        primary.onClick = [this] { startUninstall(); };
        secondary.onClick = [this] { if (onFinished) onFinished (true); };
    }
    setSize (600, 420);
}

InstallerPanel::~InstallerPanel()
{
    stopTimer();
    if (worker != nullptr) worker->stopThread (30000);
}

void InstallerPanel::resized()
{
    auto r = getLocalBounds().reduced (32, 28);
    auto buttons = r.removeFromBottom (36);
    primary.setBounds (buttons.removeFromRight (juce::jmax (130, primary.idealWidth())));
    buttons.removeFromRight (8);
    secondary.setBounds (buttons.removeFromRight (juce::jmax (110, secondary.idealWidth())));
    r.removeFromBottom (14);
    auto opts = r.removeFromBottom (mode == Mode::install ? 4 * 28 : 28);
    for (auto* b : { &startMenu, &desktop, &firewall, &launch })
        if (mode == Mode::install) b->setBounds (opts.removeFromTop (28));
    if (mode == Mode::uninstall) keepSettings.setBounds (opts);
}

void InstallerPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    g.setGradientFill (juce::ColourGradient (theme::accentIn, 0, 0, theme::accentHw, (float) getWidth(), 0, false));
    g.fillRect (0, 0, getWidth(), 3);
    auto r = getLocalBounds().reduced (32, 28).toFloat();
    theme::drawLogo (g, r.removeFromTop (44.0f).withWidth (40.0f));
    r.removeFromTop (10.0f);
    g.setColour (theme::text);
    g.setFont (theme::font (22.0f, true));
    g.drawText (title, r.removeFromTop (32.0f), juce::Justification::centredLeft);
    r.removeFromTop (6.0f);
    juce::String shown = text;
    juce::Colour colour = theme::textDim;
    {
        const juce::ScopedLock sl (lock);
        if (state.load() == 1) shown = status;
        if (state.load() == 3) { shown = error; colour = theme::warn; }
    }
    drawWrapped (g, shown, r.removeFromTop (120.0f), theme::font (14.0f), colour);
}

void InstallerPanel::startInstall()
{
    state = 1;
    status = "Installing...";
    primary.setEnabled (false);
    secondary.setEnabled (false);
    for (auto* b : { &startMenu, &desktop, &firewall, &launch }) b->setEnabled (false);
    install::Options o;
    o.startMenu = startMenu.getToggleState();
    o.desktop = desktop.getToggleState();
    o.firewall = firewall.getToggleState();
    const bool start = launch.getToggleState();
    repaint();
    struct Job final : juce::Thread
    {
        Job (InstallerPanel& p, install::Options opt, bool s) : juce::Thread ("Install"), panel (p), o (opt), start (s) {}
        void run() override
        {
            auto say = [this] (const juce::String& s) { const juce::ScopedLock sl (panel.lock); panel.status = s; };
            say ("Closing the running Hydra Mixer...");
            install::closeRunningInstance (10000);
            say (o.firewall ? "Copying, then Windows asks once to allow network access..." : "Copying...");
            const auto err = install::installFrom (install::runningExe(), install::currentVersion(), o);
            if (err.isNotEmpty())
            {
                { const juce::ScopedLock sl (panel.lock); panel.error = "Installation failed: " + err; }
                panel.state = 3;
                return;
            }
            if (start) install::launchInstalled (true);
            panel.state = 2;
        }
        InstallerPanel& panel;
        install::Options o;
        bool start;
    };
    worker = std::make_unique<Job> (*this, o, start);
    worker->startThread();
    startTimerHz (10);
}

void InstallerPanel::startUninstall()
{
    state = 1;
    status = "Removing Hydra Mixer...";
    primary.setEnabled (false);
    secondary.setEnabled (false);
    keepSettings.setEnabled (false);
    const bool keep = keepSettings.getToggleState();
    repaint();
    struct Job final : juce::Thread
    {
        Job (InstallerPanel& p, bool k) : juce::Thread ("Uninstall"), panel (p), keep (k) {}
        void run() override
        {
            install::closeRunningInstance (10000);
            install::uninstall (keep);
            panel.state = 2;
        }
        InstallerPanel& panel;
        bool keep;
    };
    worker = std::make_unique<Job> (*this, keep);
    worker->startThread();
    startTimerHz (10);
}

void InstallerPanel::timerCallback()
{
    repaint();
    const int s = state.load();
    if (s == 3)
    {
        stopTimer();
        primary.setEnabled (true);
        secondary.setEnabled (true);
        primary.setText ("Try again");
        for (auto* b : { &startMenu, &desktop, &firewall, &launch }) b->setEnabled (true);
    }
    if (s == 2)
    {
        stopTimer();
        if (onFinished) onFinished (true);
    }
}
} // namespace hydra

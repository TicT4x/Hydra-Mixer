#include "Inspector.h"
#include <juce_dsp/juce_dsp.h>

namespace hydra
{
//==============================================================================
namespace
{
class PluginWindow final : public juce::DocumentWindow
{
public:
    explicit PluginWindow (PluginSlot& s)
        : juce::DocumentWindow (s.instance->getName(), theme::panel, juce::DocumentWindow::closeButton), slot (s)
    {
        setUsingNativeTitleBar (true);
        juce::AudioProcessorEditor* ed = s.instance->hasEditor() ? s.instance->createEditorAndMakeActive() : nullptr;
        if (ed == nullptr)
            ed = new juce::GenericAudioProcessorEditor (*s.instance);
        setContentOwned (ed, true);
        setResizable (ed->isResizable(), false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
        toFront (true);
    }

    void closeButtonPressed() override
    {
        // Destroy the editor (not just hide it) so plugin GUIs stop consuming CPU.
        juce::Component::SafePointer<PluginWindow> self (this);
        auto* s = &slot;
        juce::MessageManager::callAsync ([self, s] { if (self != nullptr) s->editorWindow.reset(); });
    }

private:
    PluginSlot& slot;
};

/** Rounded section with title, optional ON toggle (headerSlot) and optional settings gear. */
struct Section : juce::Component
{
    Section (juce::String t, juce::Colour c) : title (std::move (t)), colour (c)
    {
        gear.showArrow = false;
        gear.centred = true;
        gear.setTooltip ("More settings");
    }

    void enableGear (std::function<void()> onClick)
    {
        gear.onClick = std::move (onClick);
        addAndMakeVisible (gear);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (3.0f);
        g.setColour (theme::panel2);
        g.fillRoundedRectangle (r, 8.0f);
        g.setColour (theme::outline.withAlpha (0.6f));
        g.drawRoundedRectangle (r, 8.0f, 1.0f);
        g.setColour (theme::textDim);
        g.setFont (theme::font (11.0f, true));
        auto titleArea = r.reduced (10.0f, 6.0f).removeFromTop (16.0f);
        if (gear.isVisible()) titleArea.removeFromRight (72.0f); // ON toggle + gear
        g.drawFittedText (title.toUpperCase(), titleArea.toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
    }

    void resized() override { gear.setBounds (headerSlot().translated (-28, 0).withWidth (24)); }

    juce::Rectangle<int> content() const    { return getLocalBounds().reduced (12, 8).withTrimmedTop (24); }
    juce::Rectangle<int> headerSlot() const { return getLocalBounds().reduced (10, 7).removeFromTop (20).removeFromRight (44); }

    juce::String title;
    juce::Colour colour;
    Chip gear { "...", theme::text };
};

void rowLayout (juce::Rectangle<int> r, const std::vector<juce::Component*>& comps, int gap = 4)
{
    const int n = (int) comps.size();
    if (n == 0) return;
    const int w = (r.getWidth() - gap * (n - 1)) / n;
    for (auto* c : comps) { c->setBounds (r.removeFromLeft (w)); r.removeFromLeft (gap); }
}

/** Small live bar (vertical or horizontal) fed by a lambda; repaints only on change. */
class LiveBar final : public juce::Component
{
public:
    LiveBar (std::function<float()> src, juce::Colour c, juce::String cap = {}, float markerValue = -1.0f)
        : source (std::move (src)), colour (c), caption (std::move (cap)), marker (markerValue)
    {
        setInterceptsMouseClicks (false, false);
    }

    void update()
    {
        const float v = juce::jlimit (0.0f, 1.0f, source());
        if (std::abs (v - shown) > 0.01f) { shown = v; repaint(); }
    }

    void setMarker (float m) { if (m != marker) { marker = m; repaint(); } }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        const bool vertical = r.getHeight() > r.getWidth();
        auto bar = r;
        if (caption.isNotEmpty())
        {
            g.setColour (theme::textFaint);
            g.setFont (theme::font (9.5f));
            if (vertical) g.drawText (caption, bar.removeFromBottom (12.0f), juce::Justification::centred);
            else          g.drawText (caption, bar.removeFromLeft (44.0f), juce::Justification::centredLeft);
        }
        if (vertical) bar = bar.withSizeKeepingCentre (juce::jmin (10.0f, bar.getWidth()), bar.getHeight());
        g.setColour (theme::bg);
        g.fillRoundedRectangle (bar, 3.0f);
        g.setColour (colour);
        const float v = juce::jmax (0.0f, shown);
        if (vertical) g.fillRoundedRectangle (bar.withTop (bar.getBottom() - bar.getHeight() * v), 3.0f);
        else          g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * v), 3.0f);
        if (marker >= 0.0f)
        {
            g.setColour (theme::text);
            if (vertical) g.fillRect (bar.getX() - 2.0f, bar.getBottom() - bar.getHeight() * marker - 0.75f, bar.getWidth() + 4.0f, 1.5f);
            else          g.fillRect (bar.getX() + bar.getWidth() * marker - 0.75f, bar.getY() - 2.0f, 1.5f, bar.getHeight() + 4.0f);
        }
    }

private:
    std::function<float()> source;
    juce::Colour colour;
    juce::String caption;
    float marker = -1.0f, shown = -1.0f;
};

/** Base for floating advanced panels: polls its live displays at 30 Hz while open. */
class AdvancedPanel : public juce::Component, private juce::Timer
{
public:
    AdvancedPanel() { startTimerHz (30); }
    void paint (juce::Graphics& g) override { g.fillAll (theme::panel); }

protected:
    virtual void tick() {}
    std::vector<LiveBar*> bars;

private:
    void timerCallback() override { for (auto* b : bars) b->update(); tick(); }
};

//==============================================================================
/** Horizontal device rack (like Ableton's device view): all sections of a channel side by side at their
    own width in one row that scrolls sideways as a whole when it does not fit. The last section takes
    any room that is left. */
class EffectRack final : public juce::Component
{
public:
    struct Row final : juce::Component
    {
        std::function<void (juce::Graphics&)> over;   // drawn above the sections (row coordinates)
        void paintOverChildren (juce::Graphics& g) override { if (over) over (g); }
    };

    EffectRack()
    {
        view.setViewedComponent (&row, false);
        view.setScrollBarsShown (false, true);
        view.setScrollBarThickness (8);
        view.setSingleStepSizes (40, 40);
        addAndMakeVisible (view);
    }

    Row& content() noexcept { return row; }

    /** Lays the sections out left to right; widths[i] is the width of sections[i], the last one gets at
        least lastMin and otherwise everything that is left. */
    void layout (const std::vector<juce::Component*>& sections, std::vector<int> widths, int lastMin)
    {
        view.setBounds (getLocalBounds());
        int fixed = 0;
        for (size_t i = 0; i + 1 < widths.size(); ++i) fixed += widths[i];
        const bool scrolls = fixed + lastMin > getWidth();
        widths.back() = juce::jmax (lastMin, getWidth() - fixed);
        const int h = getHeight() - (scrolls ? view.getScrollBarThickness() + 2 : 0);
        row.setSize (fixed + widths.back(), juce::jmax (40, h));
        int x = 0;
        for (size_t i = 0; i < sections.size(); ++i)
        {
            sections[i]->setBounds (x, 0, widths[i], row.getHeight());
            x += widths[i];
        }
    }

private:
    juce::Viewport view;
    Row row;
};

//==============================================================================
/** Small icon button: two diagonal arrows ("open larger"). */
class ExpandButton final : public juce::Component, public juce::SettableTooltipClient
{
public:
    std::function<void()> onClick;

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (hover ? theme::raised.brighter (0.1f) : theme::raised);
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour (hover ? theme::text : theme::textDim);
        const auto c = r.getCentre();
        const float s = 4.5f;
        juce::Path p;
        p.startNewSubPath (c.x - s, c.y + s); p.lineTo (c.x + s, c.y - s);       // diagonal
        p.startNewSubPath (c.x + s - 3.5f, c.y - s); p.lineTo (c.x + s, c.y - s); p.lineTo (c.x + s, c.y - s + 3.5f);
        p.startNewSubPath (c.x - s, c.y + s - 3.5f); p.lineTo (c.x - s, c.y + s); p.lineTo (c.x - s + 3.5f, c.y + s);
        g.strokePath (p, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    void mouseUp (const juce::MouseEvent& e) override { if (e.mouseWasClicked() && onClick) onClick(); }
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { hover = false; repaint(); }

private:
    bool hover = false;
};

//==============================================================================
/** 8-band EQ: type picker | curve | selected band controls. Shared by inputs and buses. */
class EqSection final : public juce::Component, private ParamPoller::Client
{
public:
    EqSection (UiContext& c, ParamEq& e, juce::Colour col, int selectIndex)
        : ctx (c), eq (e), colour (col), section ("Equalizer", col), onToggle (c, *e.on, "ON", col), graph (c, e, col)
    {
        addAndMakeVisible (section);
        section.addAndMakeVisible (onToggle);
        section.addAndMakeVisible (graph);
        section.addAndMakeVisible (expand);
        expand.setTooltip ("Open the EQ in a large window");
        expand.onClick = [this, selectIndex] { if (ctx.openEqWindow) ctx.openEqWindow (selectIndex); };
        graph.onBandSelected = [this] (int b) { bindBand (b); };
        bindBand (0);
        for (int k = 0; k < ParamEq::kBands; ++k) // show the first band in use
            if (eq.bandOn[(size_t) k]->isOn()) { graph.setSelectedBand (k); break; }
        ParamPoller::attach (this);
    }

    ~EqSection() override { ParamPoller::detach (this); }

    void resized() override
    {
        section.setBounds (getLocalBounds());
        onToggle.setBounds (section.headerSlot());
        expand.setBounds (section.headerSlot().translated (-32, 0).withWidth (26));
        auto r = section.content();
        typeBar->setBounds (r.removeFromLeft (34));
        r.removeFromLeft (6);
        auto col = r.removeFromRight (168);
        r.removeFromRight (8);
        graph.setBounds (r);

        auto head = col.removeFromTop (22);
        bandTitle = head.translated (section.getX(), section.getY());
        bandToggle->setBounds (head.removeFromRight (62));
        col.removeFromTop (4);
        rowLayout (col.withSizeKeepingCentre (col.getWidth(), juce::jmin (70, col.getHeight())), { gainKnob.get(), freqKnob.get(), qKnob.get() }, 2);
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        const int b = graph.getSelectedBand();
        g.setColour (theme::bandColour (b));
        g.setFont (theme::font (12.0f, true));
        g.drawText ("BAND " + juce::String (b + 1), bandTitle.withWidth (60), juce::Justification::centredLeft);
        g.setColour (theme::textDim);
        g.setFont (theme::font (11.5f));
        g.drawText (eq.type[(size_t) b]->toText(), bandTitle.withTrimmedLeft (58).withTrimmedRight (64), juce::Justification::centredLeft);
    }

private:
    void bindBand (int b)
    {
        const auto k = (size_t) b;
        const auto bc = theme::bandColour (b);
        typeBar = std::make_unique<EqTypeBar> (ctx, *eq.type[k], bc);
        bandToggle = std::make_unique<Toggle> (ctx, *eq.bandOn[k], "IN USE", bc);
        bandToggle->setTooltip ("Band in use (shown as a point). Click an empty spot in the curve to add a band.");
        gainKnob = std::make_unique<Knob> (ctx, *eq.gain[k], "GAIN", bc);
        freqKnob = std::make_unique<Knob> (ctx, *eq.freq[k], "FREQ", bc);
        qKnob = std::make_unique<Knob> (ctx, *eq.q[k], "Q", bc);
        for (juce::Component* c : std::initializer_list<juce::Component*> { typeBar.get(), (juce::Component*) bandToggle.get(),
                                    (juce::Component*) gainKnob.get(), (juce::Component*) freqKnob.get(), (juce::Component*) qKnob.get() })
            section.addAndMakeVisible (c);
        typeVersion = 0;
        pollParams();
        resized();
        repaint();
    }

    void pollParams() override
    {
        const auto* t = eq.type[(size_t) graph.getSelectedBand()];
        if (t->getVersion() != typeVersion)
        {
            typeVersion = t->getVersion();
            gainKnob->setAlpha (ParamEq::typeUsesGain (juce::roundToInt (t->get())) ? 1.0f : 0.35f);
            repaint (bandTitle);
        }
    }

    UiContext& ctx;
    ParamEq& eq;
    juce::Colour colour;
    Section section;
    Toggle onToggle;
    EqGraph graph;
    ExpandButton expand;
    std::unique_ptr<EqTypeBar> typeBar;
    std::unique_ptr<Toggle> bandToggle;
    std::unique_ptr<Knob> gainKnob, freqKnob, qKnob;
    juce::Rectangle<int> bandTitle;
    uint32_t typeVersion = 0;
};

//==============================================================================
class GatePanel final : public AdvancedPanel
{
public:
    GatePanel (UiContext& c, InputStrip& s)
        : strip (s),
          thr (c, *s.gateThr, "THRESHOLD", theme::accentIn), range (c, *s.gateRange, "RANGE", theme::accentIn),
          attack (c, *s.gateAttack, "ATTACK", theme::accentIn), hold (c, *s.gateHold, "HOLD", theme::accentIn),
          release (c, *s.gateRelease, "RELEASE", theme::accentIn),
          sc (c, *s.gateScOn, "SIDECHAIN BP", theme::accentIn), scFreq (c, *s.gateScFreq, "SC FREQ", theme::accentIn),
          on (c, *s.gateOn, "GATE ON", theme::accentIn),
          voice (c, *s.gateVoice, "VOICE KEY", theme::good), voiceThr (c, *s.gateVoiceThr, "VOICE THR", theme::good),
          state ([&s] { return s.gateOn->isOn() ? juce::jmap (gainToDb (s.gate.shownGain.load()), -80.0f, 0.0f, 0.0f, 1.0f) : 0.0f; }, theme::good, "OPEN"),
          vad ([&s] { return s.dnOn->isOn() ? s.denoiser->getVad() : 0.0f; }, theme::good, "VOICE")
    {
        for (juce::Component* k : std::initializer_list<juce::Component*> { &thr, &range, &attack, &hold, &release, &sc, &scFreq,
                                                                           &on, &voice, &voiceThr, &state, &vad })
            addAndMakeVisible (k);
        voice.setTooltip ("Voice Key: the gate only opens while the denoiser detects speech (needs the denoiser on)");
        bars.push_back (&state);
        bars.push_back (&vad);
        setSize (660, 236);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (14);
        rowLayout (r.removeFromTop (78), { &thr, &range, &attack, &hold, &release });
        r.removeFromTop (14);
        auto row = r.removeFromTop (78);
        auto toggles = row.removeFromLeft (124);
        on.setBounds (toggles.removeFromTop (26));
        toggles.removeFromTop (8);
        sc.setBounds (toggles.removeFromTop (26));
        scFreq.setBounds (row.removeFromLeft (86));
        row.removeFromLeft (18);
        voice.setBounds (row.removeFromLeft (104).removeFromTop (26));
        voiceThr.setBounds (row.removeFromLeft (86));
        row.removeFromLeft (16);
        auto barsArea = row.withSizeKeepingCentre (row.getWidth(), 40);
        state.setBounds (barsArea.removeFromTop (14));
        barsArea.removeFromTop (12);
        vad.setBounds (barsArea.removeFromTop (14));
        info = r.withTrimmedTop (4);
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        g.setColour (theme::textFaint);
        g.setFont (theme::font (11.0f));
        const bool needsDn = strip.gateVoice->isOn() && ! strip.dnOn->isOn();
        if (needsDn) g.setColour (theme::warn);
        g.drawText (needsDn ? juce::String ("Voice Key needs the denoiser switched on - until then the gate works on level only.")
                            : juce::String ("Sidechain band-pass: listen only to this frequency region.  Voice Key: open only on speech."),
                    info, juce::Justification::centredLeft);
    }

private:
    void tick() override
    {
        vad.setMarker (strip.gateVoice->isOn() ? strip.gateVoiceThr->get() * 0.01f : -1.0f);
        const auto v = strip.gateVoice->getVersion() + strip.dnOn->getVersion();
        if (v != hintVersion) { hintVersion = v; repaint (info); }
    }

    InputStrip& strip;
    Knob thr, range, attack, hold, release;
    Toggle sc;
    Knob scFreq;
    Toggle on, voice;
    Knob voiceThr;
    LiveBar state, vad;
    juce::Rectangle<int> info;
    uint32_t hintVersion = 0;
};

//==============================================================================
class DenoiserPanel final : public AdvancedPanel
{
public:
    DenoiserPanel (UiContext& c, InputStrip& s)
        : strip (s),
          mode (c, *s.dnMode, theme::good), amount (c, *s.dnAmount, "AMOUNT", theme::good),
          on (c, *s.dnOn, "DENOISER ON", theme::good),
          vad ([&s] { return s.dnOn->isOn() ? s.denoiser->getVad() : 0.0f; }, theme::good, "VOICE")
    {
        for (juce::Component* k : std::initializer_list<juce::Component*> { &mode, &amount, &on, &vad })
            addAndMakeVisible (k);
        bars.push_back (&vad);
        setSize (620, 210);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (14);
        auto top = r.removeFromTop (30);
        on.setBounds (top.removeFromLeft (130));
        top.removeFromLeft (12);
        mode.setBounds (top);
        r.removeFromTop (14);
        auto row = r.removeFromTop (80);
        amount.setBounds (row.removeFromLeft (84));
        row.removeFromLeft (20);
        vad.setBounds (row.withSizeKeepingCentre (row.getWidth(), 14));
        info = r.withTrimmedTop (8);
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        g.setColour (theme::textDim);
        g.setFont (theme::font (11.5f));
        const auto lat = strip.denoiser->isSupported()
                             ? juce::String (dsp::Denoiser::latencyForMode (juce::roundToInt (strip.dnMode->get())) / 48.0, 0) + " ms"
                             : juce::String ("bypassed (engine must run at 48 kHz)");
        g.drawFittedText ("Latency while on: " + lat + ".  Ultra Low / Low / Aligned apply RNNoise's band gains with a "
                          "zero-latency filterbank (less latency = gains react a little later at word onsets). Native HQ = "
                          "original RNNoise synthesis. To mute between words, use the Gate with \"Voice Key\".",
                          info, juce::Justification::topLeft, 3);
    }

private:
    void tick() override
    {
        if (const auto v = strip.dnMode->getVersion(); v != modeVersion) { modeVersion = v; repaint (info); }
    }

    InputStrip& strip;
    ChoiceSegments mode;
    Knob amount;
    Toggle on;
    LiveBar vad;
    juce::Rectangle<int> info;
    uint32_t modeVersion = 0;
};

//==============================================================================
/** Static compressor curve with the live gain reduction. */
class CompCurve final : public juce::Component
{
public:
    explicit CompCurve (InputStrip& s) : strip (s) { setInterceptsMouseClicks (false, false); }

    void update()
    {
        const float gr = strip.compGr.load();
        uint64_t key = 0;
        for (auto* p : { strip.compInput, strip.compThr, strip.compRatio, strip.compKnee, strip.compMakeup, strip.compAuto })
            key += p->getVersion();
        if (std::abs (gr - shownGr) > 0.1f || key != shownKey) { shownGr = gr; shownKey = key; repaint(); }
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (theme::bg);
        g.fillRoundedRectangle (r, 6.0f);
        auto map = [r] (float inDb, float outDb)
        {
            return juce::Point<float> (r.getX() + r.getWidth() * (inDb + 60.0f) / 60.0f,
                                       r.getBottom() - r.getHeight() * (juce::jlimit (-60.0f, 0.0f, outDb) + 60.0f) / 60.0f);
        };
        g.setColour (theme::outline);
        g.drawLine ({ map (-60, -60), map (0, 0) }, 1.0f);
        const auto s = strip.compSettings();
        juce::Path p;
        for (float in = -60.0f; in <= 0.0f; in += 0.5f)
        {
            const float x = in + s.inputDb;
            const auto pt = map (in, x + s.staticGainDb (x) + s.totalMakeupDb());
            if (in == -60.0f) p.startNewSubPath (pt); else p.lineTo (pt);
        }
        g.setColour (theme::accentIn);
        g.strokePath (p, juce::PathStrokeType (2.0f));
        const auto t = map (s.thresholdDb, s.thresholdDb);
        g.setColour (theme::textFaint);
        g.fillRect (t.x, r.getY(), 1.0f, r.getHeight());
        g.setColour (theme::warn);
        g.setFont (theme::font (11.0f, true));
        g.drawText ("GR " + juce::String (juce::jmax (0.0f, shownGr), 1) + " dB", r.reduced (8.0f, 6.0f), juce::Justification::topLeft);
    }

private:
    InputStrip& strip;
    float shownGr = -1.0f;
    uint64_t shownKey = 0;
};

class CompPanel final : public AdvancedPanel
{
public:
    CompPanel (UiContext& c, InputStrip& s)
        : input (c, *s.compInput, "INPUT", theme::accentIn), thr (c, *s.compThr, "THRESHOLD", theme::accentIn),
          ratio (c, *s.compRatio, "RATIO", theme::accentIn), knee (c, *s.compKnee, "KNEE", theme::accentIn),
          attack (c, *s.compAttack, "ATTACK", theme::accentIn), release (c, *s.compRelease, "RELEASE", theme::accentIn),
          makeup (c, *s.compMakeup, "MAKEUP", theme::accentIn), autoMakeup (c, *s.compAuto, "AUTO GAIN", theme::accentIn),
          on (c, *s.compOn, "COMP ON", theme::accentIn), curve (s)
    {
        for (juce::Component* k : std::initializer_list<juce::Component*> { &input, &thr, &ratio, &knee, &attack, &release, &makeup, &autoMakeup, &on, &curve })
            addAndMakeVisible (k);
        setSize (660, 230);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (14);
        curve.setBounds (r.removeFromRight (200));
        r.removeFromRight (14);
        rowLayout (r.removeFromTop (80), { &input, &thr, &ratio, &knee });
        r.removeFromTop (14);
        auto row = r.removeFromTop (80);
        auto toggles = row.removeFromRight (100);
        on.setBounds (toggles.removeFromTop (26));
        toggles.removeFromTop (8);
        autoMakeup.setBounds (toggles.removeFromTop (26));
        row.removeFromRight (8);
        rowLayout (row, { &attack, &release, &makeup });
    }

private:
    void tick() override { curve.update(); }

    Knob input, thr, ratio, knee, attack, release, makeup;
    Toggle autoMakeup, on;
    CompCurve curve;
};

//==============================================================================
class InputDetail final : public juce::Component
{
public:
    InputDetail (UiContext& c, int i, std::function<void()> scan)
        : ctx (c), strip (c.engine.inputs[(size_t) i]),
          eqSec (c, strip.eq, theme::accentIn, i),
          gateSec ("Gate", theme::accentIn), dnSec ("Denoiser", theme::good),
          compSec ("Compressor", theme::accentIn),
          fxSec ("VST3 Insert Chain", theme::accentIn),
          gateOn (c, *strip.gateOn, "ON", theme::accentIn), gateThr (c, *strip.gateThr, "THRESH", theme::accentIn),
          gateState ([this] { return strip.gateOn->isOn() ? juce::jmap (gainToDb (strip.gate.shownGain.load()), -80.0f, 0.0f, 0.0f, 1.0f) : 0.0f; }, theme::good, "OPEN"),
          dnOn (c, *strip.dnOn, "ON", theme::good), dnAmount (c, *strip.dnAmount, "AMOUNT", theme::good),
          compOn (c, *strip.compOn, "ON", theme::accentIn), compThr (c, *strip.compThr, "THRESH", theme::accentIn),
          compRatio (c, *strip.compRatio, "RATIO", theme::accentIn),
          compGr ([this] { return strip.compGr.load() / 24.0f; }, theme::warn, "GR"),
          phase (c, *strip.phase, "PHASE", theme::warn), bypass (c, *strip.fxBypass, "BYPASS", theme::mute),
          chain (c, strip.plugins, theme::accentIn)
    {
        chain.onScanRequested = std::move (scan);
        chain.onChainChanged = [this] { resized(); };
        addAndMakeVisible (rack);
        for (juce::Component* s : std::initializer_list<juce::Component*> { &eqSec, &gateSec, &dnSec, &compSec, &fxSec }) rack.content().addAndMakeVisible (s);
        rack.content().over = [this] (juce::Graphics& g) { paintMode (g); };
        for (juce::Component* k : std::initializer_list<juce::Component*> { &gateOn, &gateThr, &gateState }) gateSec.addAndMakeVisible (k);
        for (juce::Component* k : std::initializer_list<juce::Component*> { &dnOn, &dnAmount }) dnSec.addAndMakeVisible (k);
        for (juce::Component* k : std::initializer_list<juce::Component*> { &compOn, &compThr, &compRatio, &compGr }) compSec.addAndMakeVisible (k);
        // Rarely used switches live in the header of the plugin section.
        fxSec.addAndMakeVisible (phase);
        fxSec.addAndMakeVisible (bypass);
        phase.setTooltip ("Invert the polarity of this input");
        bypass.setTooltip ("Bypass all plugins of this input");
        fxSec.addAndMakeVisible (chain);

        gateSec.enableGear ([this] { ctx.openPanel (std::make_unique<GatePanel> (ctx, strip), "Gate  -  " + strip.name, theme::accentIn); });
        dnSec.enableGear   ([this] { ctx.openPanel (std::make_unique<DenoiserPanel> (ctx, strip), "Denoiser (RNNoise)  -  " + strip.name, theme::good); });
        compSec.enableGear ([this] { ctx.openPanel (std::make_unique<CompPanel> (ctx, strip), "Compressor  -  " + strip.name, theme::accentIn); });
    }

    void resized() override
    {
        rack.setBounds (getLocalBounds());
        rack.layout ({ &eqSec, &gateSec, &dnSec, &compSec, &fxSec }, { 520, 150, 140, 200, 0 }, chain.getIdealWidth() + 24);

        gateOn.setBounds (gateSec.headerSlot());
        auto gc = gateSec.content();
        gateState.setBounds (gc.removeFromRight (26));
        gateThr.setBounds (gc.withSizeKeepingCentre (gc.getWidth(), 70));

        dnOn.setBounds (dnSec.headerSlot());
        auto dc = dnSec.content();
        modeArea = dc.removeFromBottom (16).translated (dnSec.getX(), dnSec.getY());
        dnAmount.setBounds (dc.withSizeKeepingCentre (dc.getWidth(), 70));

        compOn.setBounds (compSec.headerSlot());
        auto cc = compSec.content();
        compGr.setBounds (cc.removeFromBottom (12));
        rowLayout (cc.withSizeKeepingCentre (cc.getWidth(), 70), { &compThr, &compRatio });

        auto fh = fxSec.getLocalBounds().reduced (10, 7).removeFromTop (20);
        bypass.setBounds (fh.removeFromRight (64));
        fh.removeFromRight (6);
        phase.setBounds (fh.removeFromRight (60));
        chain.setBounds (fxSec.content());
    }

    void paintMode (juce::Graphics& g)
    {
        g.setColour (strip.denoiser->isSupported() ? theme::textFaint : theme::warn);
        g.setFont (theme::font (10.0f));
        g.drawText (strip.denoiser->isSupported() ? strip.dnMode->toText() : juce::String ("needs 48 kHz"), modeArea, juce::Justification::centred);
    }

    void updateMeters()
    {
        gateState.update();
        compGr.update();
        if (const auto v = strip.dnMode->getVersion(); v != modeVersion) { modeVersion = v; rack.content().repaint (modeArea); }
    }

private:
    UiContext& ctx;
    InputStrip& strip;
    EffectRack rack;
    EqSection eqSec;
    Section gateSec, dnSec, compSec, fxSec;
    Toggle gateOn;
    Knob gateThr;
    LiveBar gateState;
    Toggle dnOn;
    Knob dnAmount;
    Toggle compOn;
    Knob compThr, compRatio;
    LiveBar compGr;
    Toggle phase, bypass;
    PluginChainView chain;
    juce::Rectangle<int> modeArea;
    uint32_t modeVersion = 0;
};

//==============================================================================
class BusDetail final : public juce::Component
{
public:
    BusDetail (UiContext& c, int b, std::function<void()> scan)
        : bus (c.engine.buses[(size_t) b]), colour (theme::busColour (b)),
          eqSec (c, bus.eq, colour, kMaxInputs + b), limSec ("Limiter", colour),
          fxSec ("VST3 Insert Chain", colour),
          limOn (c, *bus.limOn, "ON", colour), ceiling (c, *bus.limCeil, "CEILING", colour),
          bypass (c, *bus.fxBypass, "BYPASS", theme::mute),
          chain (c, bus.plugins, colour)
    {
        chain.onScanRequested = std::move (scan);
        chain.onChainChanged = [this] { resized(); };
        addAndMakeVisible (rack);
        for (juce::Component* s : std::initializer_list<juce::Component*> { &eqSec, &limSec, &fxSec }) rack.content().addAndMakeVisible (s);
        limSec.addAndMakeVisible (limOn);
        limSec.addAndMakeVisible (ceiling);
        fxSec.addAndMakeVisible (bypass);
        bypass.setTooltip ("Bypass all plugins of this bus");
        fxSec.addAndMakeVisible (chain);
    }

    void resized() override
    {
        rack.setBounds (getLocalBounds());
        rack.layout ({ &eqSec, &limSec, &fxSec }, { 560, 120, 0 }, chain.getIdealWidth() + 24);

        limOn.setBounds (limSec.headerSlot());
        ceiling.setBounds (limSec.content().withSizeKeepingCentre (76, 70));
        bypass.setBounds (fxSec.getLocalBounds().reduced (10, 7).removeFromTop (20).removeFromRight (64));
        chain.setBounds (fxSec.content());
    }

private:
    Bus& bus;
    juce::Colour colour;
    EffectRack rack;
    EqSection eqSec;
    Section limSec, fxSec;
    Toggle limOn;
    Knob ceiling;
    Toggle bypass;
    PluginChainView chain;
};

//==============================================================================
juce::String formatTime (double seconds)
{
    const auto t = (int) seconds;
    const auto hms = t >= 3600 ? juce::String (t / 3600) + ":" + juce::String ((t / 60) % 60).paddedLeft ('0', 2)
                               : juce::String (t / 60).paddedLeft ('0', 2);
    return hms + ":" + juce::String (t % 60).paddedLeft ('0', 2) + "." + juce::String ((int) (seconds * 10.0) % 10);
}

/** Waveform of the take being recorded, or of the loaded file with the play position. Click or drag
    to move the play position (like a DAW timeline); while stopped it sets where PLAY starts.
    Trim like a clip in a video editor: drag the handle at the left / right edge inwards; the parts outside
    are greyed out and not played (Recorder::setTrim). "Cut" in the recorder writes the trimmed file. */
class WaveformView final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit WaveformView (Recorder& r) : rec (r)
    {
        setTooltip ("Click / drag: jump to that position.  Drag the handles at the edges to trim the start / end.");
        setMouseCursor (juce::MouseCursor::IBeamCursor);
    }

    /** Called ~30x per second: repaints only if the waveform or the position changed. */
    void update()
    {
        const bool recording = rec.isRecording();
        const auto& wave = recording ? rec.getRecordWaveform() : rec.getPlayWaveform();
        const double pos = recording ? rec.getRecordSeconds() : rec.isPlaying() ? rec.getPlaySeconds() : rec.getStartSeconds();
        const auto v = wave.getVersion();
        const int px = juce::roundToInt (pos / juce::jmax (0.001, length()) * getWidth());
        const auto trim = std::make_pair (rec.getTrimIn(), rec.getTrimOut());
        if (v != shownVersion || recording != shownRecording || px != shownPx || rec.getLoadedFile() != shownFile || trim != shownTrim)
        {
            shownTrim = trim;
            shownVersion = v;
            shownRecording = recording;
            shownPx = px;
            shownFile = rec.getLoadedFile();
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (theme::bg);
        g.fillRoundedRectangle (r, 6.0f);
        auto plot = r.reduced (6.0f, 4.0f).withTrimmedBottom (12.0f);

        const bool recording = rec.isRecording();
        const auto& wave = recording ? rec.getRecordWaveform() : rec.getPlayWaveform();
        const double len = length();
        const auto colour = recording ? theme::mute : theme::good;

        // Time ruler
        g.setFont (theme::font (9.5f));
        const double step = len <= 10 ? 1 : len <= 60 ? 5 : len <= 300 ? 30 : len <= 1200 ? 60 : len <= 3600 ? 300 : 600;
        for (double s = 0; s <= len && len > 0.0; s += step)
        {
            const float x = plot.getX() + (float) (s / len) * plot.getWidth();
            g.setColour (theme::outline.withAlpha (0.5f));
            g.fillRect (x, plot.getY(), 1.0f, plot.getHeight());
            g.setColour (theme::textFaint);
            const int t = (int) s;
            g.drawText (juce::String (t / 60) + ":" + juce::String (t % 60).paddedLeft ('0', 2),
                        juce::Rectangle<float> (x + 2.0f, plot.getBottom(), 40.0f, 12.0f), juce::Justification::centredLeft, false);
        }

        if (wave.getSeconds() <= 0.0 || len <= 0.0)
        {
            g.setColour (theme::textFaint);
            g.setFont (theme::font (12.0f));
            g.drawText (recording ? "Recording..." : rec.getLoadedFile() == juce::File() ? "Record something or open a file - its waveform appears here"
                                                                                          : "Reading the file...",
                        plot, juce::Justification::centred);
            return;
        }

        // Waveform: one column per pixel, mirrored around the centre line (level in dB-ish scale).
        const int cols = juce::jmax (1, (int) plot.getWidth());
        columns.resize ((size_t) cols);
        wave.getColumns (0.0, len, columns.data(), cols);
        const float mid = plot.getCentreY(), half = plot.getHeight() * 0.5f;
        juce::Path p;
        p.startNewSubPath (plot.getX(), mid);
        auto toH = [half] (float v) { return half * juce::jlimit (0.0f, 1.0f, std::sqrt (v)); };
        for (int c = 0; c < cols; ++c) p.lineTo (plot.getX() + (float) c, mid - toH (columns[(size_t) c]));
        for (int c = cols; --c >= 0;) p.lineTo (plot.getX() + (float) c, mid + toH (columns[(size_t) c]));
        p.closeSubPath();
        g.setColour (colour.withAlpha (0.55f));
        g.fillPath (p);
        g.setColour (colour.withAlpha (0.25f));
        g.fillRect (plot.getX(), mid - 0.5f, plot.getWidth(), 1.0f);

        if (recording) return;

        // Trim: greyed out outside [in, out], a handle at each end
        const float inX = xOf (rec.getTrimIn()), outX = xOf (rec.getTrimOut());
        g.setColour (theme::bg.withAlpha (0.72f));
        g.fillRect (juce::Rectangle<float> (plot.getX(), plot.getY(), inX - plot.getX(), plot.getHeight()));
        g.fillRect (juce::Rectangle<float> (outX, plot.getY(), plot.getRight() - outX, plot.getHeight()));
        for (int k = 0; k < 2; ++k)
        {
            const float x = k == 0 ? inX : outX;
            const bool active = dragHandle == k || hoverHandle == k;
            g.setColour (active ? theme::solo : theme::solo.withAlpha (0.75f));
            g.fillRect (x - 1.0f, plot.getY(), 2.0f, plot.getHeight());
            // Grip tab pointing into the kept part
            auto tab = juce::Rectangle<float> (k == 0 ? x : x - 9.0f, plot.getCentreY() - 14.0f, 9.0f, 28.0f);
            g.fillRoundedRectangle (tab, 3.0f);
            g.setColour (theme::bg.withAlpha (0.8f));
            for (int j = -1; j <= 1; ++j)
                g.fillRect (tab.getCentreX() - 2.0f, tab.getCentreY() + (float) j * 5.0f - 0.5f, 4.0f, 1.0f);
        }

        // Start cursor (where PLAY begins) and play head
        const float startX = plot.getX() + (float) (rec.getStartSeconds() / len) * plot.getWidth();
        g.setColour (theme::text.withAlpha (0.5f));
        g.fillRect (startX - 0.5f, plot.getY(), 1.0f, plot.getHeight());
        juce::Path flag;
        flag.addTriangle (startX - 4.0f, plot.getY(), startX + 4.0f, plot.getY(), startX, plot.getY() + 6.0f);
        g.fillPath (flag);
        if (rec.isPlaying())
        {
            const float x = plot.getX() + (float) (rec.getPlaySeconds() / len) * plot.getWidth();
            g.setColour (theme::text);
            g.fillRect (x - 1.0f, plot.getY() - 2.0f, 2.0f, plot.getHeight() + 4.0f);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragHandle = handleAt (e.x);
        if (dragHandle < 0) seekTo (e.x);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragHandle < 0) { seekTo (e.x); return; }
        const double t = secondsAt (e.x);
        if (dragHandle == 0) rec.setTrim (t, rec.getTrimOut());
        else                 rec.setTrim (rec.getTrimIn(), t);
        update();
    }

    void mouseUp (const juce::MouseEvent&) override { dragHandle = -1; repaint(); }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int h = handleAt (e.x);
        if (h != hoverHandle) { hoverHandle = h; repaint(); }
        setMouseCursor (h >= 0 ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::IBeamCursor);
    }

    void mouseExit (const juce::MouseEvent&) override { if (hoverHandle >= 0) { hoverHandle = -1; repaint(); } }

private:
    static constexpr float kLeft = 6.0f;
    float plotWidth() const { return (float) getWidth() - 2.0f * kLeft; }
    float xOf (double seconds) const
    {
        const double len = rec.getPlayLengthSeconds();
        return kLeft + (len > 0.0 ? (float) (seconds / len) : 0.0f) * plotWidth();
    }
    double secondsAt (int x) const
    {
        return juce::jlimit (0.0, 1.0, (double) ((float) x - kLeft) / plotWidth()) * rec.getPlayLengthSeconds();
    }
    /** 0 = in handle, 1 = out handle, -1 = none (grab zone of 8 px; the handles also catch the outer padding). */
    int handleAt (int x) const
    {
        if (rec.isRecording() || rec.getLoadedFile() == juce::File() || rec.getPlayLengthSeconds() <= 0.0) return -1;
        const float fx = (float) x, inX = xOf (rec.getTrimIn()), outX = xOf (rec.getTrimOut());
        const float dIn = std::abs (fx - inX) - (fx < inX ? 6.0f : 0.0f), dOut = std::abs (fx - outX) - (fx > outX ? 6.0f : 0.0f);
        if (dIn <= 8.0f && dIn <= dOut) return 0;
        if (dOut <= 8.0f) return 1;
        return -1;
    }

    double length() const
    {
        if (rec.isRecording()) return juce::jmax (1.0, rec.getRecordSeconds());
        return rec.getPlayLengthSeconds();
    }

    void seekTo (int x)
    {
        if (rec.isRecording() || rec.getLoadedFile() == juce::File()) return;
        rec.seek (secondsAt (x));
        update();
    }

    Recorder& rec;
    std::vector<float> columns;
    uint32_t shownVersion = 0;
    bool shownRecording = false;
    int shownPx = -1;
    juce::File shownFile;
    std::pair<double, double> shownTrim;
    int dragHandle = -1, hoverHandle = -1;
};

/** Recorder: left what is recorded (a B bus), in the middle the recording itself (transport, waveform,
    file), on the right where playback goes (any A / B buses). */
class RecorderDetail final : public juce::Component
{
public:
    explicit RecorderDetail (UiContext& c)
        : ctx (c), rec (c.engine.recorder),
          sourceSec ("Record Source", theme::accentVirt), takeSec ("Recording", theme::mute), outSec ("Play To", theme::accentHw),
          recBtn (c, *rec.record, "REC", theme::mute, Toggle::Icon::record),
          playBtn (c, *rec.play, "PLAY", theme::good, Toggle::Icon::playPause),
          stopBtn (c, *rec.stop, "STOP", theme::text, Toggle::Icon::stop),
          loopBtn (c, *rec.loop, "LOOP", theme::good, Toggle::Icon::loop),
          level (c, *rec.level, "LEVEL", theme::good),
          wave (rec)
    {
        for (juce::Component* s : std::initializer_list<juce::Component*> { &sourceSec, &takeSec, &outSec }) addAndMakeVisible (s);
        for (juce::Component* k : std::initializer_list<juce::Component*> { &folderChip, &formatChip }) sourceSec.addAndMakeVisible (k);
        for (juce::Component* k : std::initializer_list<juce::Component*> { &recBtn, &playBtn, &stopBtn, &loopBtn, &deleteBtn, &wave, &openChip, &lastChip, &showChip,
                                                                           &cutChip, &resetTrimChip })
            takeSec.addAndMakeVisible (k);
        outSec.addAndMakeVisible (level);
        for (int b = 0; b < kMaxBuses; ++b)
        {
            if (! c.engine.isBusActive (b)) continue;
            (isHwBus (b) ? hwOuts : virtOuts).push_back (std::make_unique<Toggle> (c, *rec.out[(size_t) b], busCode (b), theme::busColour (b)));
            outSec.addAndMakeVisible (*(isHwBus (b) ? hwOuts : virtOuts).back());
        }
        // Record source: the B buses as the same kind of buttons as Play To (one of them is lit).
        for (int k = 0; k < kMaxVirtBuses; ++k)
        {
            const int b = virtBusIndex (k);
            if (! c.engine.isBusActive (b)) continue;
            sources.push_back (std::make_unique<Toggle> (c, *rec.source, busCode (b), theme::busColour (b)));
            sources.back()->choiceValue = k;
            sources.back()->setTooltip ("Record " + busCode (b) + " (" + c.engine.buses[(size_t) b].name + ")");
            sourceSec.addAndMakeVisible (*sources.back());
        }
        for (auto* chip : { &folderChip, &formatChip, &openChip, &lastChip, &showChip, &cutChip, &resetTrimChip })
        {
            chip->showArrow = false;
            chip->centred = true;
        }
        formatChip.showArrow = true;

        recBtn.setTooltip ("Record the selected B bus to a WAV file (stop with a second click)");
        playBtn.setTooltip ("Play / pause the loaded file on the buses selected under Play To (starts at the marker in the waveform)");
        stopBtn.setTooltip ("Stop recording / playback and go back to the start");
        loopBtn.setTooltip ("Repeat the file");
        deleteBtn.setTooltip ("Delete this recording: unloads it and moves the file to the Recycle Bin");
        deleteBtn.onClick = [this] { confirmDelete(); };
        cutChip.setTooltip ("Cut the file to the part between the trim handles (the original goes to the Recycle Bin)");
        resetTrimChip.setTooltip ("Move the trim handles back to the start and end");
        cutChip.onClick = [this] { confirmCut(); };
        resetTrimChip.onClick = [this] { rec.resetTrim(); };
        folderChip.setTooltip ("Folder for new recordings");
        openChip.setTooltip ("Load an audio file for playback (WAV, AIFF, FLAC, Ogg, MP3)");
        lastChip.setTooltip ("Load the most recent recording");
        showChip.setTooltip ("Show the loaded file in Explorer");
        level.setTooltip ("Playback level");

        folderChip.onClick = [this] { chooseFolder(); };
        formatChip.onClick = [this] { chooseFormat(); };
        openChip.onClick = [this] { openFile(); };
        lastChip.onClick = [this]
        {
            juce::String err;
            const auto f = rec.getLastRecording();
            if (f.existsAsFile()) rec.loadFile (f, err);
            refreshTexts();
        };
        showChip.onClick = [this]
        {
            const auto f = rec.getLoadedFile();
            if (f.existsAsFile()) f.revealToUser();
            else rec.getFolder().revealToUser();
        };
        refreshTexts();
    }

    void resized() override
    {
        // Source and outputs take what they need, the recording gets the rest.
        auto r = getLocalBounds();
        const int most = (int) juce::jmax (hwOuts.size(), virtOuts.size());
        const int perRow = most == 5 ? 5 : 4;   // 5 A buses in one row, otherwise rows of four
        sourceSec.setBounds (r.removeFromLeft (juce::jmax (210, 24 + perRow * 40 + 8)));
        outSec.setBounds (r.removeFromRight (24 + perRow * 40 + 8 + 70));
        takeSec.setBounds (r);

        // Rows of bus buttons under a small caption (record source and Play To look the same).
        auto group = [perRow] (juce::Rectangle<int>& area, std::vector<std::unique_ptr<Toggle>>& list, juce::Rectangle<int>& label,
                               juce::Point<int> origin)
        {
            label = {};
            if (list.empty()) return;
            label = area.removeFromTop (14) + origin;
            for (size_t k = 0; k < list.size(); k += (size_t) perRow)
            {
                auto row = area.removeFromTop (22);
                area.removeFromTop (4);
                for (size_t j = k; j < juce::jmin (list.size(), k + (size_t) perRow); ++j)
                {
                    list[j]->setBounds (row.removeFromLeft (36));
                    row.removeFromLeft (4);
                }
            }
            area.removeFromTop (4);
        };

        // Source: B bus, what it records, folder / format
        auto s = sourceSec.content();
        group (s, sources, sourceLabel, sourceSec.getPosition());
        s.removeFromTop (2);
        sourceInfo = s.removeFromTop (30).translated (sourceSec.getX(), sourceSec.getY());
        s.removeFromTop (6);
        auto chips = s.removeFromTop (26);
        folderChip.setBounds (chips.removeFromLeft (chips.getWidth() / 2 - 3));
        chips.removeFromLeft (6);
        formatChip.setBounds (chips);

        // Recording: transport + time, waveform, file
        auto t = takeSec.content();
        auto transport = t.removeFromTop (28);
        for (juce::Component* b : std::initializer_list<juce::Component*> { &recBtn, &playBtn, &stopBtn })
        {
            b->setBounds (transport.removeFromLeft (38));
            transport.removeFromLeft (4);
        }
        transport.removeFromLeft (6);
        loopBtn.setBounds (transport.removeFromLeft (38));
        transport.removeFromLeft (10);
        deleteBtn.setBounds (transport.removeFromLeft (34));
        transport.removeFromLeft (10);
        cutChip.setBounds (transport.removeFromLeft (64));
        transport.removeFromLeft (4);
        resetTrimChip.setBounds (transport.removeFromLeft (58));
        transport.removeFromLeft (14);
        timeArea = transport.translated (takeSec.getX(), takeSec.getY());
        t.removeFromTop (6);
        auto fileRow = t.removeFromBottom (24);
        t.removeFromBottom (6);
        wave.setBounds (t);
        showChip.setBounds (fileRow.removeFromRight (64));
        fileRow.removeFromRight (5);
        lastChip.setBounds (fileRow.removeFromRight (84));
        fileRow.removeFromRight (5);
        openChip.setBounds (fileRow.removeFromRight (92));
        fileRow.removeFromRight (10);
        fileArea = fileRow.translated (takeSec.getX(), takeSec.getY());

        // Play to: A buses, then B buses (own rows), level knob on the right
        auto o = outSec.content();
        level.setBounds (o.removeFromRight (64).withSizeKeepingCentre (64, 74));
        o.removeFromRight (8);
        group (o, hwOuts, hwLabel, outSec.getPosition());
        group (o, virtOuts, virtLabel, outSec.getPosition());
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        const bool recording = rec.isRecording(), playing = rec.isPlaying();
        auto t = timeArea;
        g.setFont (theme::mono (20.0f));
        g.setColour (recording ? theme::mute : playing ? theme::good : theme::textDim);
        const int tw = juce::jmin (t.getWidth(), juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), shownTime) + 16);
        g.drawText (shownTime, t.removeFromLeft (tw), juce::Justification::centredLeft);
        g.setFont (theme::font (11.5f));
        g.setColour (rec.getLastError().isNotEmpty() && ! recording && ! playing ? theme::warn : theme::textDim);
        g.drawFittedText (stateText, t, juce::Justification::centredRight, 2, 0.85f);

        g.setColour (theme::textFaint);
        g.setFont (theme::font (11.0f));
        g.drawFittedText (sourceText, sourceInfo, juce::Justification::topLeft, 2, 0.85f);

        g.setFont (theme::font (10.5f, true));
        if (! hwLabel.isEmpty())   { g.setColour (theme::accentHw.withAlpha (0.8f));   g.drawText ("HARDWARE (A)", hwLabel, juce::Justification::centredLeft); }
        if (! virtLabel.isEmpty()) { g.setColour (theme::accentVirt.withAlpha (0.8f)); g.drawText ("VIRTUAL (B)", virtLabel, juce::Justification::centredLeft); }
        if (! sourceLabel.isEmpty()) { g.setColour (theme::accentVirt.withAlpha (0.8f)); g.drawText ("VIRTUAL (B)", sourceLabel, juce::Justification::centredLeft); }

        auto f = fileArea;
        g.setColour (rec.getLoadedFile() != juce::File() ? theme::text : theme::textFaint);
        g.setFont (theme::font (12.5f, true));
        const int fw = juce::jmin (f.getWidth(), juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), fileTitle) + 12);
        g.drawFittedText (fileTitle, f.removeFromLeft (fw), juce::Justification::centredLeft, 1, 0.8f);
        g.setColour (theme::textDim);
        g.setFont (theme::font (11.0f));
        g.drawFittedText (fileInfo, f, juce::Justification::centredLeft, 1, 0.8f);
    }

    void updateMeters()
    {
        const bool recording = rec.isRecording(), playing = rec.isPlaying();
        juce::String time;
        if (recording)    time = formatTime (rec.getRecordSeconds());
        else if (playing) time = formatTime (rec.getPlaySeconds()) + " / " + formatTime (rec.getPlayLengthSeconds());
        else if (rec.getLoadedFile() != juce::File())
            time = formatTime (rec.getStartSeconds()) + " / " + formatTime (rec.getPlayLengthSeconds());
        else              time = formatTime (0.0);
        const auto v = rec.stateVersion.load() + rec.source->getVersion();
        if (v != seenVersion) { seenVersion = v; refreshTexts(); }
        if (time != shownTime) { shownTime = time; repaint (timeArea); }
        wave.update();
    }

private:
    void refreshTexts()
    {
        const bool recording = rec.isRecording(), playing = rec.isPlaying();
        if (recording)    stateText = "Recording " + busCode (rec.getRecordSourceBus()) + "  ->  " + rec.getRecordingFile().getFileName();
        else if (playing) stateText = "Playing";
        else if (rec.getLastError().isNotEmpty()) stateText = rec.getLastError();
        else              stateText = "Stopped";

        sourceText = sources.empty() ? juce::String ("No virtual output (B bus) in use - add one to record.")
                                     : "Records " + busCode (rec.getRecordSourceBus()) + " after its fader and limiter into  "
                                           + rec.getFolder().getFileName();
        folderChip.text = "Folder...";
        formatChip.text = rec.bitDepth >= 32 ? "WAV 32f" : "WAV " + juce::String (rec.bitDepth);

        const auto file = rec.getLoadedFile();
        deleteBtn.setEnabled (file != juce::File() && ! recording);
        const bool trimmed = rec.isTrimmed() && ! recording;
        for (auto* chip : { &cutChip, &resetTrimChip })
        {
            chip->setEnabled (trimmed);
            chip->setAlpha (trimmed ? 1.0f : 0.4f);
        }
        cutChip.active = trimmed;
        fileTitle = file != juce::File() ? file.getFileName() : juce::String ("No file loaded");
        fileInfo = file != juce::File() ? file.getParentDirectory().getFullPathName() : juce::String();
        if (rec.isTrimmed())
            fileInfo = "keeps " + formatTime (rec.getTrimIn()) + " - " + formatTime (rec.getTrimOut()) + "  ("
                     + formatTime (rec.getTrimOut() - rec.getTrimIn()) + ")  -  Cut writes it";
        for (auto* chip : { &folderChip, &formatChip }) chip->repaint();
        repaint();
    }

    void confirmDelete()
    {
        const auto file = rec.getLoadedFile();
        if (file == juce::File() || rec.isRecording()) return;
        juce::Component::SafePointer<RecorderDetail> safe (this);
        dialog::confirm (this, dialog::Kind::danger, "Delete this recording?",
                         file.getFileName() + "\n\nThe track is removed from the recorder and the file is moved to the Recycle Bin.",
                         "Delete", [safe, file]
                         {
                             if (safe == nullptr || safe->rec.getLoadedFile() != file) return;
                             juce::String err;
                             if (! safe->rec.deleteLoadedFile (err))
                                 dialog::message (safe.getComponent(), dialog::Kind::warning, "Recorder", err);
                             safe->refreshTexts();
                         });
    }

    void confirmCut()
    {
        const auto file = rec.getLoadedFile();
        if (file == juce::File() || ! rec.isTrimmed()) return;
        const bool wav = file.hasFileExtension ("wav");
        juce::Component::SafePointer<RecorderDetail> safe (this);
        dialog::confirm (this, dialog::Kind::question, "Cut the recording?",
                         file.getFileName() + "\n\nKeeps " + formatTime (rec.getTrimIn()) + " - " + formatTime (rec.getTrimOut()) + ". "
                             + (wav ? "The original file goes to the Recycle Bin." : "The result is saved as a new WAV file next to it."),
                         "Cut", [safe, file]
                         {
                             if (safe == nullptr || safe->rec.getLoadedFile() != file) return;
                             juce::String err;
                             if (! safe->rec.applyTrim (err))
                                 dialog::message (safe.getComponent(), dialog::Kind::warning, "Recorder", err);
                             safe->refreshTexts();
                         });
    }

    void chooseFolder()
    {
        chooser = std::make_unique<juce::FileChooser> ("Folder for recordings", rec.getFolder());
        juce::Component::SafePointer<RecorderDetail> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [safe] (const juce::FileChooser& fc)
                              {
                                  if (safe == nullptr || fc.getResult() == juce::File()) return;
                                  safe->rec.folder = fc.getResult();
                                  safe->rec.stateVersion.fetch_add (1);
                              });
    }

    void chooseFormat()
    {
        juce::PopupMenu m;
        for (int bits : { 16, 24, 32 })
            m.addItem (bits == 32 ? "WAV 32-bit float" : "WAV " + juce::String (bits) + "-bit", true, rec.bitDepth == bits,
                       [this, bits] { rec.bitDepth = bits; rec.stateVersion.fetch_add (1); });
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&formatChip));
    }

    void openFile()
    {
        const auto start = rec.getLoadedFile() != juce::File() ? rec.getLoadedFile().getParentDirectory() : rec.getFolder();
        chooser = std::make_unique<juce::FileChooser> ("Open audio file", start, "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3;*.wma");
        juce::Component::SafePointer<RecorderDetail> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe] (const juce::FileChooser& fc)
                              {
                                  if (safe == nullptr || ! fc.getResult().existsAsFile()) return;
                                  juce::String err;
                                  if (! safe->rec.loadFile (fc.getResult(), err))
                                      dialog::message (safe.getComponent(), dialog::Kind::warning, "Recorder", err);
                              });
    }

    UiContext& ctx;
    Recorder& rec;
    Section sourceSec, takeSec, outSec;
    Toggle recBtn, playBtn, stopBtn, loopBtn;
    TrashButton deleteBtn;
    Knob level;
    WaveformView wave;
    Chip folderChip { "Folder...", theme::text }, formatChip { "WAV 24", theme::text };
    Chip openChip { "Open File...", theme::text }, lastChip { "Last Take", theme::text }, showChip { "Show", theme::text };
    Chip cutChip { "Cut", theme::solo }, resetTrimChip { "Reset", theme::text };
    std::vector<std::unique_ptr<Toggle>> sources, hwOuts, virtOuts;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Rectangle<int> timeArea, sourceInfo, fileArea, hwLabel, virtLabel, sourceLabel;
    juce::String shownTime, stateText, sourceText, fileTitle, fileInfo;
    uint64_t seenVersion = 0;
};
} // namespace

//==============================================================================
struct PluginChainView::Card : juce::Component, juce::SettableTooltipClient
{
    Card (PluginChainView& o, int i) : owner (o), index (i)
    {
        for (auto* b : { &bypass, &edit, (Chip*) &remove })
        {
            addAndMakeVisible (b);
            b->showArrow = false;
            b->centred = true;
        }
        bypass.onClick = [this]
        {
            if (auto* s = owner.chain.getSlot (index)) { s->bypassed = ! s->bypassed.load(); refresh(); }
        };
        edit.onClick   = [this] { owner.openEditor (index); };
        // Chain edits rebuild the cards, so defer them until this click handler has returned.
        remove.onClick = [this]
        {
            juce::Component::SafePointer<PluginChainView> o (&owner);
            const int ix = index;
            juce::MessageManager::callAsync ([o, ix] { if (o != nullptr) { o->chain.remove (ix); o->refresh(); } });
        };
        bypass.setTooltip ("Bypass this plugin");
        edit.setTooltip ("Open plugin editor");
        remove.setTooltip ("Remove plugin");
        setTooltip ("Drag to change the order. Double-click: open the editor.");
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        refresh();
    }

    void refresh()
    {
        auto* s = owner.chain.getSlot (index);
        if (s == nullptr) return;
        name = s->instance->getName();
        vendor = s->description.manufacturerName;
        bypass.text = s->bypassed ? "OFF" : "ON";
        bypass.active = ! s->bypassed;
        bypass.colour = s->bypassed ? theme::textDim : owner.accent;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (2.0f);
        if (dragging)
            juce::DropShadow (juce::Colours::black.withAlpha (0.5f), 12, { 0, 3 }).drawForRectangle (g, r.toNearestInt());
        g.setColour (dragging ? theme::raised.brighter (0.08f) : theme::raised);
        g.fillRoundedRectangle (r, 7.0f);
        g.setColour (dragging ? owner.accent : bypass.active ? owner.accent.withAlpha (0.6f) : theme::outline);
        g.drawRoundedRectangle (r, 7.0f, dragging ? 1.5f : 1.0f);

        // Grip (drag handle) at the top right
        g.setColour (theme::textFaint);
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 2; ++col)
                g.fillEllipse (r.getRight() - 16.0f + (float) col * 5.0f, r.getY() + 9.0f + (float) row * 5.0f, 2.2f, 2.2f);

        auto t = r.reduced (10.0f, 8.0f).withTrimmedRight (12.0f);
        g.setColour (theme::text);
        g.setFont (theme::font (13.0f, true));
        g.drawFittedText (name, t.removeFromTop (18.0f).toNearestInt(), juce::Justification::centredLeft, 1);
        g.setColour (theme::textDim);
        g.setFont (theme::font (11.0f));
        g.drawFittedText (vendor, t.removeFromTop (15.0f).toNearestInt(), juce::Justification::centredLeft, 1);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8).removeFromBottom (22);
        remove.setBounds (r.removeFromRight (30));
        r.removeFromRight (3);
        rowLayout (r, { &bypass, &edit }, 3);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = false;
        grabX = e.getMouseDownX();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragging)
        {
            if (e.getDistanceFromDragStart() < 6) return;
            dragging = true;
            toFront (false);
            repaint();
        }
        owner.cardDragged (index, e.getEventRelativeTo (getParentComponent()).x - grabX);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (! dragging) return;
        dragging = false;
        repaint();
        owner.cardDropped (index);
    }

    void mouseDoubleClick (const juce::MouseEvent&) override { owner.openEditor (index); }

    PluginChainView& owner;
    int index;
    bool dragging = false;
    int grabX = 0;
    juce::String name, vendor;
    Chip bypass { "ON" }, edit { "EDIT" };
    TrashButton remove;
};

PluginChainView::PluginChainView (UiContext& c, PluginChain& ch, juce::Colour a) : ctx (c), chain (ch), accent (a)
{
    addAndMakeVisible (addButton);
    addButton.showArrow = false;
    addButton.centred = true;
    addButton.onClick = [this] { showAddMenu (addButton); };
    plusCard.colour = accent;
    plusCard.setTooltip ("Add a VST3 plugin at the end of the chain");
    plusCard.onClick = [this] { showAddMenu (plusCard); };
    viewport.setViewedComponent (&row, false);
    viewport.setScrollBarsShown (false, true);
    viewport.setScrollBarThickness (8);
    addAndMakeVisible (viewport);
    row.addAndMakeVisible (plusCard);
    refresh();
}

void PluginChainView::refresh()
{
    cards.clear();
    dragIndex = dropIndex = -1;
    for (int i = 0; i < chain.size(); ++i)
    {
        cards.push_back (std::make_unique<Card> (*this, i));
        row.addAndMakeVisible (*cards.back());
    }
    resized();
    repaint();
    if (onChainChanged) onChainChanged();
}

void PluginChainView::paint (juce::Graphics& g)
{
    if (cards.empty())
    {
        g.setColour (theme::textFaint);
        g.setFont (theme::font (12.0f));
        g.drawText ("No plugins. Inserts run pre-fader, in order, with zero added buffering.",
                    getLocalBounds().withTrimmedLeft (190), juce::Justification::centredLeft);
    }
}

void PluginChainView::resized()
{
    // Empty chain: a labelled "Add" button. With plugins: their cards and a "+" after the last one.
    const bool empty = cards.empty();
    addButton.setVisible (empty);
    viewport.setVisible (! empty);
    if (empty)
    {
        addButton.setBounds (getLocalBounds().removeFromLeft (170).withSizeKeepingCentre (170, 32));
        return;
    }
    viewport.setBounds (getLocalBounds());
    const int n = (int) cards.size();
    cardWidth = juce::jlimit (kMinCardWidth, kMaxCardWidth, (getWidth() - kPlusWidth) / n - kCardGap);
    const int needed = n * (cardWidth + kCardGap) + kPlusWidth;
    const int h = needed > getWidth() ? getHeight() - viewport.getScrollBarThickness() : getHeight();
    row.setSize (juce::jmax (needed, getWidth()), juce::jmax (40, h));
    layoutCards();
}

int PluginChainView::getIdealWidth() const
{
    if (cards.empty()) return 420;   // add button + hint
    return (int) cards.size() * (kMaxCardWidth + kCardGap) + kPlusWidth;
}

void PluginChainView::layoutCards()
{
    const int h = row.getHeight();
    int slot = 0;
    for (int i = 0; i < (int) cards.size(); ++i)
    {
        if (i == dragIndex) continue;            // follows the mouse
        if (slot == dropIndex) ++slot;           // gap where the dragged card would land
        cards[(size_t) i]->setBounds (slot * (cardWidth + kCardGap), 0, cardWidth, h);
        ++slot;
    }
    plusCard.setBounds ((int) cards.size() * (cardWidth + kCardGap), 0, kPlusWidth, h);
}

void PluginChainView::cardDragged (int index, int x)
{
    auto& card = *cards[(size_t) index];
    const int n = (int) cards.size();
    const int maxX = (n - 1) * (cardWidth + kCardGap);
    card.setTopLeftPosition (juce::jlimit (0, juce::jmax (0, maxX), x), 0);
    const int target = juce::jlimit (0, n - 1, (card.getX() + cardWidth / 2) / (cardWidth + kCardGap));
    if (dragIndex != index || dropIndex != target)
    {
        dragIndex = index;
        dropIndex = target;
        layoutCards();
    }
    viewport.autoScroll (card.getX() - viewport.getViewPositionX() + cardWidth / 2, 10, 30, 8);
}

void PluginChainView::cardDropped (int index)
{
    const int to = dropIndex;
    dragIndex = dropIndex = -1;
    if (to < 0 || to == index) { layoutCards(); return; }
    // Rebuilding the cards destroys the one that called us: do it after its mouse handler returned.
    juce::Component::SafePointer<PluginChainView> safe (this);
    juce::MessageManager::callAsync ([safe, index, to]
    {
        if (safe == nullptr) return;
        safe->chain.move (index, to);
        safe->refresh();
    });
}

void PluginChainView::showAddMenu (juce::Component& target)
{
    auto types = ctx.engine.pluginHost.known.getTypes();
    juce::PopupMenu m;
    std::map<juce::String, juce::PopupMenu> byVendor;
    for (auto& d : types)
    {
        if (d.isInstrument) continue;
        juce::Component::SafePointer<PluginChainView> safe (this);
        byVendor[d.manufacturerName.isEmpty() ? juce::String ("Unknown") : d.manufacturerName]
            .addItem (d.name, [safe, d]
            {
                if (safe == nullptr) return;
                auto& eng = safe->ctx.engine;
                juce::String err;
                auto inst = eng.pluginHost.create (d, eng.getSampleRate(), eng.getBlockSize(), err);
                if (inst == nullptr)
                {
                    dialog::message (safe.getComponent(), dialog::Kind::warning, "Plugin could not be loaded", d.name + "\n" + err);
                    return;
                }
                safe->chain.add (std::move (inst), d);
                safe->refresh();
                safe->openEditor (safe->chain.size() - 1);
            });
    }
    for (auto& [vendor, sub] : byVendor)
        m.addSubMenu (vendor, sub);
    if (byVendor.empty())
        m.addItem ("(no plugins found yet)", false, false, nullptr);
    m.addSeparator();
    m.addItem ("Scan for VST3 plugins...", [this] { if (onScanRequested) onScanRequested(); });
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target));
}

void PluginChainView::openEditor (int index)
{
    auto* s = chain.getSlot (index);
    if (s == nullptr) return;
    if (s->editorWindow == nullptr)
        s->editorWindow = std::make_unique<PluginWindow> (*s);
    else
        s->editorWindow->toFront (true);
}

//==============================================================================
EqGraph::EqGraph (UiContext& c, ParamEq& e, juce::Colour col, bool lg) : ctx (c), eq (e), colour (col), large (lg)
{
    ParamPoller::attach (this);
    setTooltip ("Click an empty spot to add a band. Drag a point: frequency / gain (Q for cut, notch, band pass). "
                "Mouse wheel: Q. Double-click or right-click a point: remove / change type.");
}

EqGraph::~EqGraph() { ParamPoller::detach (this); }

void EqGraph::resized()
{
    const auto r = getLocalBounds().toFloat();
    plot = large ? r.withTrimmedLeft (36.0f).withTrimmedRight (40.0f).withTrimmedTop (10.0f).withTrimmedBottom (22.0f) : r;
    rebuildCurve();
}

float EqGraph::freqToX (float f) const { return plot.getX() + plot.getWidth() * std::log (f / 20.0f) / std::log (1000.0f); }
float EqGraph::xToFreq (float x) const
{
    return juce::jlimit (20.0f, 20000.0f, 20.0f * std::pow (1000.0f, (x - plot.getX()) / juce::jmax (1.0f, plot.getWidth())));
}
float EqGraph::dbToY (float db) const   { return plot.getCentreY() - db / kViewDb * plot.getHeight() * 0.5f; }
float EqGraph::yToDb (float y) const    { return (plot.getCentreY() - y) / juce::jmax (1.0f, plot.getHeight() * 0.5f) * kViewDb; }
float EqGraph::specToY (float db) const { return plot.getY() + plot.getHeight() * (kSpecTopDb - db) / (kSpecTopDb - kSpecBottomDb); }

juce::Point<float> EqGraph::nodePos (int k) const
{
    const int t = juce::roundToInt (eq.type[(size_t) k]->get());
    const float y = ParamEq::typeUsesGain (t) ? dbToY (eq.gain[(size_t) k]->get()) : dbToY (0.0f);
    return { freqToX (eq.freq[(size_t) k]->get()), y };
}

void EqGraph::setSelectedBand (int b)
{
    b = juce::jlimit (0, ParamEq::kBands - 1, b);
    if (b == selected) return;
    selected = b;
    repaint();
    if (onBandSelected) onBandSelected (b);
}

void EqGraph::pollParams()
{
    if (const auto sum = eq.versionSum(); sum != seen) { seen = sum; rebuildCurve(); repaint(); }
}

float EqGraph::curveDbAt (float f) const
{
    const double sr = 48000.0;
    double total = 0.0;
    for (int k = 0; k < ParamEq::kBands; ++k)
        if (eq.bandOn[(size_t) k]->isOn() && eq.bandActive (k))
            total += eq.coeffs (k, sr).magnitudeDb (f, sr);
    return (float) juce::jlimit (-40.0, 40.0, total);
}

void EqGraph::rebuildCurve()
{
    curve.clear();
    for (auto& p : bandCurves) p.clear();
    if (plot.getWidth() < 2.0f) return;

    const double sr = 48000.0;
    dsp::BiquadCoeffs cs[ParamEq::kBands];
    bool used[ParamEq::kBands];
    for (int k = 0; k < ParamEq::kBands; ++k)
    {
        used[k] = eq.bandOn[(size_t) k]->isOn();
        if (used[k]) cs[k] = eq.coeffs (k, sr);
    }

    const float y0 = dbToY (0.0f);
    bool first = true;
    for (float x = plot.getX(); x <= plot.getRight() + 0.5f; x += 2.0f)
    {
        const float f = xToFreq (x);
        double total = 0.0;
        for (int k = 0; k < ParamEq::kBands; ++k)
        {
            if (! used[k]) continue;
            const double db = cs[k].magnitudeDb (f, sr);
            if (eq.bandActive (k)) total += db;
            const float y = dbToY ((float) juce::jlimit (-40.0, 40.0, db));
            if (first) { bandCurves[(size_t) k].startNewSubPath (x, y0); }
            bandCurves[(size_t) k].lineTo (x, y);
        }
        const float y = dbToY ((float) juce::jlimit (-40.0, 40.0, total));
        if (first) curve.startNewSubPath (x, y);
        else       curve.lineTo (x, y);
        first = false;
    }
    for (int k = 0; k < ParamEq::kBands; ++k)
        if (used[k])
        {
            bandCurves[(size_t) k].lineTo (plot.getRight(), y0);
            bandCurves[(size_t) k].closeSubPath();
        }
}

void EqGraph::setSpectrum (const std::vector<float>& cols)
{
    spectrum.clear();
    if (cols.size() >= 2 && plot.getWidth() > 2.0f)
    {
        juce::Path p;
        p.startNewSubPath (plot.getX(), plot.getBottom());
        float x = plot.getX();
        for (size_t i = 0; i < cols.size(); ++i)
        {
            x = juce::jmin (plot.getRight(), plot.getX() + (float) (i * kSpectrumStep));
            p.lineTo (x, specToY (juce::jlimit (kSpecBottomDb - 6.0f, kSpecTopDb + 6.0f, cols[i])));
        }
        p.lineTo (x, plot.getBottom());
        p.closeSubPath();
        spectrum = p.createPathWithRoundedCorners (5.0f);
    }
    repaint (plot.getSmallestIntegerContainer());
}

void EqGraph::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    g.setColour (theme::bg);
    g.fillRoundedRectangle (r, 6.0f);

    // Grid + scales
    g.setFont (theme::font (large ? 10.5f : 9.5f));
    for (float f : { 20.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f })
    {
        const float x = freqToX (f);
        if (f > 20.0f && f < 20000.0f)
        {
            g.setColour (theme::outline.withAlpha (0.5f));
            g.fillRect (x, plot.getY(), 1.0f, plot.getHeight());
        }
        else if (! large) continue;
        const auto label = f >= 1000 ? juce::String ((int) (f / 1000)) + "k" : juce::String ((int) f);
        g.setColour (theme::textFaint);
        if (large)
            g.drawText (label, juce::Rectangle<float> (x - 20.0f, plot.getBottom() + 4.0f, 40.0f, 14.0f), juce::Justification::centred);
        else
            g.drawText (label, (int) x + 2, (int) r.getBottom() - 13, 30, 12, juce::Justification::left);
    }
    for (float db = -18.0f; db <= 18.0f; db += 6.0f)
    {
        const float y = dbToY (db);
        g.setColour (db == 0.0f ? theme::outline : theme::outline.withAlpha (0.4f));
        g.fillRect (plot.getX(), y, plot.getWidth(), 1.0f);
        if (large)
        {
            g.setColour (db == 0.0f ? theme::textDim : theme::textFaint);
            g.drawText ((db > 0 ? "+" : "") + juce::String ((int) db), juce::Rectangle<float> (0.0f, y - 7.0f, plot.getX() - 6.0f, 14.0f),
                        juce::Justification::centredRight);
        }
    }
    if (large && ! spectrum.isEmpty())
    {
        // Spectrum level scale (dBFS) on the right
        g.setColour (colour.withAlpha (0.55f));
        for (float db = kSpecTopDb; db >= kSpecBottomDb; db -= 24.0f)
            g.drawText (juce::String ((int) db), juce::Rectangle<float> (plot.getRight() + 6.0f, specToY (db) - 7.0f, r.getRight() - plot.getRight() - 8.0f, 14.0f),
                        juce::Justification::centredLeft);
    }

    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (plot.getSmallestIntegerContainer());

        if (! spectrum.isEmpty())
        {
            g.setGradientFill (juce::ColourGradient (colour.withAlpha (0.28f), 0.0f, plot.getY(),
                                                     colour.withAlpha (0.03f), 0.0f, plot.getBottom(), false));
            g.fillPath (spectrum);
            g.setColour (colour.withAlpha (0.5f));
            g.strokePath (spectrum, juce::PathStrokeType (1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        const bool on = eq.on->isOn();
        for (int k = 0; k < ParamEq::kBands; ++k)
        {
            if (bandCurves[(size_t) k].isEmpty()) continue;
            const auto bc = on ? theme::bandColour (k) : theme::textDim;
            const bool emphasise = k == selected || k == hovered;
            g.setColour (bc.withAlpha (emphasise ? 0.22f : 0.10f));
            g.fillPath (bandCurves[(size_t) k]);
            if (emphasise)
            {
                g.setColour (bc.withAlpha (0.6f));
                g.strokePath (bandCurves[(size_t) k], juce::PathStrokeType (1.0f));
            }
        }
        g.setColour (on ? theme::text.withAlpha (0.92f) : theme::textFaint);
        g.strokePath (curve, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Points: only the bands in use
    bool any = false;
    const float rad = large ? 8.0f : 7.0f;
    for (int k = 0; k < ParamEq::kBands; ++k)
    {
        if (! eq.bandOn[(size_t) k]->isOn()) continue;
        any = true;
        const auto p = nodePos (k);
        const auto bc = eq.on->isOn() ? theme::bandColour (k) : theme::textDim;
        auto node = juce::Rectangle<float> (p.x - rad, p.y - rad, rad * 2.0f, rad * 2.0f);
        if (k == selected || k == hovered)
        {
            g.setColour (bc.withAlpha (k == selected ? 0.3f : 0.18f));
            g.fillEllipse (node.expanded (5.0f));
        }
        g.setColour (bc);
        g.fillEllipse (node);
        if (k == selected)
        {
            g.setColour (theme::text);
            g.drawEllipse (node.expanded (1.5f), 1.5f);
        }
        g.setColour (theme::bg);
        g.setFont (theme::font (large ? 10.5f : 9.5f, true));
        g.drawText (juce::String (k + 1), node, juce::Justification::centred);
    }
    if (ghostBand >= 0)
    {
        const auto bc = theme::bandColour (ghostBand);
        auto node = juce::Rectangle<float> (ghostPos.x - rad, ghostPos.y - rad, rad * 2.0f, rad * 2.0f);
        g.setColour (bc.withAlpha (0.22f));
        g.fillEllipse (node);
        g.setColour (bc.withAlpha (0.7f));
        g.drawEllipse (node, 1.2f);
        g.setFont (theme::font (large ? 10.5f : 9.5f, true));
        g.drawText (juce::String (ghostBand + 1), node, juce::Justification::centred);
    }
    if (! any && ghostBand < 0)
    {
        g.setColour (theme::textFaint);
        g.setFont (theme::font (large ? 13.0f : 11.0f));
        g.drawText ("Click to add a band", plot.withTrimmedBottom (plot.getHeight() * 0.5f + 8.0f), juce::Justification::centredBottom);
    }
}

int EqGraph::hitBand (juce::Point<float> p) const
{
    int best = -1;
    float bestD = large ? 18.0f : 16.0f;
    for (int k = 0; k < ParamEq::kBands; ++k)
    {
        if (! eq.bandOn[(size_t) k]->isOn()) continue;
        const float d = nodePos (k).getDistanceFrom (p);
        if (d < bestD) { bestD = d; best = k; }
    }
    return best;
}

int EqGraph::addBand (float f, float gainDb)
{
    for (int k = 0; k < ParamEq::kBands; ++k)
    {
        const auto i = (size_t) k;
        if (eq.bandOn[i]->isOn()) continue;
        eq.type[i]->set ((float) ParamEq::Bell);
        eq.q[i]->set (1.0f);
        eq.freq[i]->set (f);
        eq.gain[i]->set (std::round (gainDb * 10.0f) / 10.0f);
        eq.bandOn[i]->set (1.0f);
        if (! eq.on->isOn()) eq.on->set (1.0f);
        return k;
    }
    return -1;
}

void EqGraph::removeBand (int k)
{
    const auto i = (size_t) k;
    eq.bandOn[i]->set (0.0f);
    for (auto* p : { eq.type[i], eq.gain[i], eq.freq[i], eq.q[i] }) p->reset();
    if (hovered == k) hovered = -1;
    if (selected == k)
        for (int j = 0; j < ParamEq::kBands; ++j)
            if (eq.bandOn[(size_t) j]->isOn()) { setSelectedBand (j); break; }
    repaint();
}

void EqGraph::showBandMenu (int k)
{
    juce::PopupMenu m;
    m.addSectionHeader ("Band " + juce::String (k + 1));
    const auto names = ParamEq::typeNames();
    const int cur = juce::roundToInt (eq.type[(size_t) k]->get());
    juce::Component::SafePointer<EqGraph> safe (this);
    for (int t = 0; t < names.size(); ++t)
        m.addItem (names[t], true, t == cur, [safe, k, t] { if (safe != nullptr) safe->eq.type[(size_t) k]->set ((float) t); });
    m.addSeparator();
    m.addItem ("Remove band", [safe, k] { if (safe != nullptr) safe->removeBand (k); });
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
}

juce::Rectangle<float> EqGraph::ghostBounds() const
{
    return juce::Rectangle<float> (ghostPos, ghostPos).expanded (12.0f);
}

void EqGraph::setGhost (int band, juce::Point<float> pos)
{
    if (band == ghostBand && (band < 0 || pos == ghostPos)) return;
    if (ghostBand >= 0) repaint (ghostBounds().getSmallestIntegerContainer());
    ghostBand = band;
    ghostPos = pos;
    if (ghostBand >= 0) repaint (ghostBounds().getSmallestIntegerContainer());
    else repaint(); // the "Click to add a band" hint may come back
}

void EqGraph::mouseMove (const juce::MouseEvent& e)
{
    const int k = hitBand (e.position);
    if (k != hovered) { hovered = k; repaint(); }

    // Ghost point where a click would add the next free band: only near the curve, stuck to it
    // at the mouse's horizontal position.
    int freeBand = -1;
    juce::Point<float> pos;
    if (k < 0 && plot.contains (e.position))
    {
        pos = { e.position.x, dbToY (juce::jlimit (-18.0f, 18.0f, curveDbAt (xToFreq (e.position.x)))) };
        if (std::abs (pos.y - e.position.y) <= (large ? 14.0f : 11.0f))
            for (int j = 0; j < ParamEq::kBands && freeBand < 0; ++j)
                if (! eq.bandOn[(size_t) j]->isOn()) freeBand = j;
    }
    setGhost (freeBand, pos);
}

void EqGraph::mouseExit (const juce::MouseEvent&)
{
    if (hovered >= 0) { hovered = -1; repaint(); }
    setGhost (-1, {});
}

void EqGraph::mouseDown (const juce::MouseEvent& e)
{
    const bool onGhost = ghostBand >= 0;
    const auto ghostAt = ghostPos;
    setGhost (-1, {});
    int k = hitBand (e.position);
    if (e.mods.isPopupMenu())
    {
        if (k >= 0) { setSelectedBand (k); showBandMenu (k); }
        return;
    }
    if (k < 0)
    {
        if (! plot.contains (e.position)) return;
        const auto at = onGhost ? ghostAt : e.position; // on the curve: the band lands where the ghost was
        k = addBand (xToFreq (at.x), juce::jlimit (-18.0f, 18.0f, yToDb (at.y)));
        if (k < 0) return; // all bands in use
        justAdded = k;
        justAddedTime = juce::Time::getMillisecondCounterHiRes();
    }
    setSelectedBand (k);
    dragging = true;
    dragStartQ = eq.q[(size_t) k]->getNormalised();
}

void EqGraph::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging) return;
    const auto k = (size_t) selected;
    eq.freq[k]->set (xToFreq (juce::jlimit (plot.getX(), plot.getRight(), e.position.x)));
    if (ParamEq::typeUsesGain (juce::roundToInt (eq.type[k]->get())))
        eq.gain[k]->set (yToDb (e.position.y));
    else
        eq.q[k]->setNormalised (dragStartQ - (float) e.getDistanceFromDragStartY() / juce::jmax (1.0f, plot.getHeight()));
    if (! eq.on->isOn()) eq.on->set (1.0f);
}

void EqGraph::mouseDoubleClick (const juce::MouseEvent& e)
{
    // The first click of a double-click on an empty spot already added this band - keep it.
    const int k = hitBand (e.position);
    if (k < 0 || (k == justAdded && juce::Time::getMillisecondCounterHiRes() - justAddedTime < 600.0)) return;
    dragging = false;
    removeBand (k);
}

void EqGraph::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    int k = hitBand (e.position);
    if (k < 0) k = selected;
    if (! eq.bandOn[(size_t) k]->isOn()) return;
    auto* q = eq.q[(size_t) k];
    q->setNormalised (q->getNormalised() + (w.deltaY > 0 ? 0.03f : -0.03f));
}

//==============================================================================
/** One band in the large EQ editor (in the band's colour): type picker, gain / freq / Q with
    permanent readouts and "Remove". An unused band shows a placeholder that adds it. */
struct EqEditor::BandColumn final : public juce::Component, private ParamPoller::Client
{
    BandColumn (EqEditor& o, int k)
        : owner (o), band (k), colour (theme::bandColour (k)),
          typeBar (o.ctx, *o.eq.type[(size_t) k], colour),
          gain (o.ctx, *o.eq.gain[(size_t) k], "GAIN", colour),
          freq (o.ctx, *o.eq.freq[(size_t) k], "FREQ", colour),
          q (o.ctx, *o.eq.q[(size_t) k], "Q", colour)
    {
        for (juce::Component* c : std::initializer_list<juce::Component*> { &typeBar, &removeChip, &gain, &freq, &q })
            addAndMakeVisible (c);
        removeChip.showArrow = false;
        removeChip.centred = true;
        removeChip.setTooltip ("Remove this band");
        removeChip.onClick = [this]
        {
            juce::Component::SafePointer<EqEditor> o (&owner);
            const int b = band;
            juce::MessageManager::callAsync ([o, b] { if (o != nullptr) o->graph.removeBand (b); });
        };
        addMouseListener (this, true); // any click in the column selects the band
        ParamPoller::attach (this);
        pollParams();
    }

    ~BandColumn() override { ParamPoller::detach (this); }

    bool inUse() const { return owner.eq.bandOn[(size_t) band]->isOn(); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (inUse()) { owner.selectBand (band); return; }
        if (e.eventComponent == this)
        {
            if (const int k = owner.graph.addBand (ParamEq::defaultFreq (band), 0.0f); k >= 0) owner.selectBand (k);
        }
    }

    void pollParams() override
    {
        const auto k = (size_t) band;
        const auto v = owner.eq.type[k]->getVersion() + owner.eq.gain[k]->getVersion() + owner.eq.freq[k]->getVersion()
                     + owner.eq.q[k]->getVersion() + owner.eq.bandOn[k]->getVersion();
        if (v == seen) return;
        seen = v;
        const bool use = inUse();
        for (juce::Component* c : std::initializer_list<juce::Component*> { &typeBar, &removeChip, &gain, &freq, &q })
            c->setVisible (use);
        gain.setAlpha (ParamEq::typeUsesGain (juce::roundToInt (owner.eq.type[k]->get())) ? 1.0f : 0.35f);
        setMouseCursor (use ? juce::MouseCursor::NormalCursor : juce::MouseCursor::PointingHandCursor);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const bool use = inUse();
        const bool sel = use && owner.graph.getSelectedBand() == band;
        auto r = getLocalBounds().toFloat().reduced (3.0f);
        g.setColour (use ? theme::panel2 : theme::panel2.withAlpha (0.45f));
        g.fillRoundedRectangle (r, 8.0f);
        g.setColour (sel ? colour.withAlpha (0.85f) : use ? colour.withAlpha (0.25f) : theme::outline.withAlpha (0.4f));
        g.drawRoundedRectangle (r, 8.0f, sel ? 1.5f : 1.0f);

        auto head = getLocalBounds().reduced (12, 8).removeFromTop (22);
        g.setColour (use ? colour : theme::textFaint);
        g.fillEllipse (head.removeFromLeft (10).withSizeKeepingCentre (10, 10).toFloat());
        head.removeFromLeft (8);
        g.setFont (theme::font (12.5f, true));
        g.drawText ("BAND " + juce::String (band + 1), head, juce::Justification::centredLeft);

        if (! use)
        {
            g.setColour (theme::textFaint);
            g.setFont (theme::font (12.0f));
            g.drawText ("+  Add band", getLocalBounds(), juce::Justification::centred);
            return;
        }
        g.setColour (theme::textDim);
        g.setFont (theme::font (11.5f));
        g.drawText (owner.eq.type[(size_t) band]->toText(), head.withTrimmedLeft (58).withTrimmedRight (70), juce::Justification::centredLeft);

        // Values stay visible under the knobs (not only on hover).
        g.setFont (theme::mono (11.0f));
        g.setColour (theme::text);
        const auto k = (size_t) band;
        const Param* ps[] = { owner.eq.gain[k], owner.eq.freq[k], owner.eq.q[k] };
        const Knob* ks[] = { &gain, &freq, &q };
        for (int i = 0; i < 3; ++i)
            g.drawText (ps[i]->toText(), ks[i]->getBounds().withY (ks[i]->getBottom() + 2).withHeight (14), juce::Justification::centred);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12, 8);
        auto head = r.removeFromTop (22);
        removeChip.setBounds (head.removeFromRight (66));
        r.removeFromTop (8);
        typeBar.setBounds (r.removeFromLeft (36));
        r.removeFromLeft (8);
        auto knobs = r.withSizeKeepingCentre (r.getWidth(), juce::jmin (r.getHeight() - 16, 84)).translated (0, -8);
        const int w = knobs.getWidth() / 3;
        gain.setBounds (knobs.removeFromLeft (w));
        freq.setBounds (knobs.removeFromLeft (w));
        q.setBounds (knobs);
    }

    EqEditor& owner;
    int band;
    juce::Colour colour;
    EqTypeBar typeBar;
    Chip removeChip { "Remove", theme::textDim };
    Knob gain, freq, q;
    uint64_t seen = ~0ull;
};

EqEditor::EqEditor (UiContext& c, int sel)
    : ctx (c), selectIndex (sel),
      eq (sel < kMaxInputs ? c.engine.inputs[(size_t) sel].eq : c.engine.buses[(size_t) (sel - kMaxInputs)].eq),
      colour (sel < kMaxInputs ? theme::accentIn : theme::busColour (sel - kMaxInputs)),
      onToggle (c, *eq.on, "EQ ON", colour), graph (c, eq, colour, true),
      tap (sel < kMaxInputs ? c.engine.inputs[(size_t) sel].spectrum : c.engine.buses[(size_t) (sel - kMaxInputs)].spectrum),
      fft (std::make_unique<juce::dsp::FFT> (kFftOrder))
{
    history.assign (kFftSize, 0.0f);
    fftData.assign (kFftSize * 2, 0.0f);
    window.resize (kFftSize);
    for (int i = 0; i < kFftSize; ++i) // Hann
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (kFftSize - 1));
    tap.ring.skip (tap.ring.available()); // stale audio from an earlier view
    tap.users.fetch_add (1);

    title = sel < kMaxInputs ? "IN " + juce::String (sel + 1) + "   " + c.engine.inputs[(size_t) sel].name
                             : busCode (sel - kMaxInputs) + "   " + c.engine.buses[(size_t) (sel - kMaxInputs)].name;
    addAndMakeVisible (onToggle);
    addAndMakeVisible (graph);
    for (int k = 0; k < ParamEq::kBands; ++k)
    {
        bands.push_back (std::make_unique<BandColumn> (*this, k));
        addAndMakeVisible (*bands.back());
    }
    graph.onBandSelected = [this] (int) { for (auto& b : bands) b->repaint(); };
    setWantsKeyboardFocus (true);
    for (int k = 0; k < ParamEq::kBands; ++k) // start on the first band in use
        if (eq.bandOn[(size_t) k]->isOn()) { graph.setSelectedBand (k); break; }
    setSize (1240, 820);
}

EqEditor::~EqEditor()
{
    tap.users.fetch_sub (1);
}

void EqEditor::analyse (double t)
{
    if (t - lastFrame < 1.0 / 66.0) return; // high-refresh displays: stay at ~60 Hz
    const float dt = lastFrame > 0.0 ? (float) juce::jlimit (0.0, 0.2, t - lastFrame) : 1.0f / 60.0f;
    lastFrame = t;

    // 1. Collect the new audio (mono sum) into a sliding history of kFftSize samples.
    for (;;)
    {
        auto* const* ch = readBuf.getArrayOfWritePointers();
        const int got = tap.ring.read (ch, 2, readBuf.getNumSamples());
        if (got <= 0) break;
        for (int i = 0; i < got; ++i)
        {
            history[(size_t) historyPos] = 0.5f * (ch[0][i] + ch[1][i]);
            historyPos = (historyPos + 1) & (kFftSize - 1);
        }
    }

    // 2. Windowed FFT of the latest kFftSize samples.
    for (int i = 0; i < kFftSize; ++i)
        fftData[(size_t) i] = history[(size_t) ((historyPos + i) & (kFftSize - 1))] * window[(size_t) i];
    std::fill (fftData.begin() + kFftSize, fftData.end(), 0.0f);
    fft->performFrequencyOnlyForwardTransform (fftData.data(), true);

    // 3. One dB value per graph column (log frequency), +3 dB/oct tilt like most analyzers.
    const auto plot = graph.getPlotArea();
    const int numCols = (int) (plot.getWidth() / (float) EqGraph::kSpectrumStep) + 1;
    if (numCols < 8) return;
    if ((int) smoothed.size() != numCols) smoothed.assign ((size_t) numCols, -120.0f);
    raw.resize ((size_t) numCols);
    columns.resize ((size_t) numCols);

    const double sr = juce::jmax (8000.0, ctx.engine.getSampleRate());
    const float binHz = (float) sr / (float) kFftSize;
    const float scale = 4.0f / (float) kFftSize; // amplitude of a full-scale sine -> 1.0 (Hann gain 0.5)
    auto bin = [this] (float k)
    {
        const int k0 = juce::jlimit (1, kFftSize / 2 - 2, (int) k);
        const float frac = juce::jlimit (0.0f, 1.0f, k - (float) k0);
        return fftData[(size_t) k0] + frac * (fftData[(size_t) k0 + 1] - fftData[(size_t) k0]);
    };

    for (int c = 0; c < numCols; ++c)
    {
        const float f0 = graph.xToFreq (plot.getX() + (float) (c * EqGraph::kSpectrumStep));
        const float f1 = graph.xToFreq (plot.getX() + (float) ((c + 1) * EqGraph::kSpectrumStep));
        const float k0 = f0 / binHz, k1 = f1 / binHz;
        float mag = 0.0f;
        if (k1 - k0 < 1.0f)
            mag = bin (0.5f * (k0 + k1));
        else
            for (int k = (int) k0; k <= (int) k1 && k < kFftSize / 2; ++k)
                mag = juce::jmax (mag, fftData[(size_t) k]);
        const float fc = std::sqrt (f0 * f1);
        raw[(size_t) c] = 20.0f * std::log10 (mag * scale + 1.0e-9f) + 3.0f * std::log2 (fc / 1000.0f);
    }

    // 4. Smooth across frequency (~1/12 octave, triangular) and over time (fast rise, slow fall).
    const int radius = 3;
    const float up = 1.0f - std::exp (-dt / 0.02f), down = 1.0f - std::exp (-dt / 0.28f);
    for (int c = 0; c < numCols; ++c)
    {
        float sum = 0.0f, wsum = 0.0f;
        for (int d = -radius; d <= radius; ++d)
        {
            const int i = juce::jlimit (0, numCols - 1, c + d);
            const float w = (float) (radius + 1 - std::abs (d));
            sum += raw[(size_t) i] * w;
            wsum += w;
        }
        const float target = sum / wsum;
        auto& sm = smoothed[(size_t) c];
        sm += (target - sm) * (target > sm ? up : down);
        columns[(size_t) c] = sm;
    }
    graph.setSpectrum (columns);
}

void EqEditor::selectBand (int b)
{
    graph.setSelectedBand (b);
}

void EqEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);
    auto head = getLocalBounds().reduced (18, 0).removeFromTop (44);
    g.setColour (colour);
    g.setFont (theme::font (15.0f, true));
    g.drawText (title, head, juce::Justification::centredLeft);
    g.setColour (theme::textFaint);
    g.setFont (theme::font (11.5f));
    g.drawText ("Click: add band.  Drag: frequency / gain.  Wheel: Q.  Double-click / right-click a point: remove, type.  "
                "Ctrl+Z / Ctrl+Y: undo / redo.  Background: live spectrum after the EQ (dBFS on the right).",
                head.withTrimmedLeft (260).withTrimmedRight (120), juce::Justification::centredLeft);
}

void EqEditor::resized()
{
    auto r = getLocalBounds().reduced (14, 0);
    auto head = r.removeFromTop (44);
    onToggle.setBounds (head.removeFromRight (96).withSizeKeepingCentre (96, 26));
    // All bands in one row on wide windows, otherwise two rows of four.
    const int perRow = r.getWidth() / ParamEq::kBands >= 210 ? ParamEq::kBands : ParamEq::kBands / 2;
    const int rows = ParamEq::kBands / perRow;
    const int rowH = juce::jlimit (150, 220, r.getHeight() / (rows == 1 ? 3 : 4));
    auto bottom = r.removeFromBottom (rowH * rows + 10).withTrimmedBottom (10);
    r.removeFromBottom (8);
    graph.setBounds (r);
    const int w = bottom.getWidth() / perRow;
    for (int k = 0; k < (int) bands.size(); ++k)
        bands[(size_t) k]->setBounds (bottom.getX() + (k % perRow) * w, bottom.getY() + (k / perRow) * rowH, w, rowH);
}

//==============================================================================
Inspector::Inspector (UiContext& c) : ctx (c) {}
Inspector::~Inspector() = default;

void Inspector::showChannel (int sel)
{
    if (sel == current && content != nullptr) return;
    current = sel;
    auto scan = [this] { if (onScanRequested) onScanRequested(); };
    if (sel == kRecorderSelect) content = std::make_unique<RecorderDetail> (ctx);
    else if (sel < kMaxInputs)  content = std::make_unique<InputDetail> (ctx, sel, scan);
    else                        content = std::make_unique<BusDetail> (ctx, sel - kMaxInputs, scan);
    addAndMakeVisible (*content);
    resized();
    repaint();
}

void Inspector::paint (juce::Graphics& g)
{
    g.setColour (theme::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (2.0f), 10.0f);
    auto title = getLocalBounds().reduced (14, 8).removeFromTop (20);
    if (current == kRecorderSelect)
    {
        g.setColour (theme::mute);
        g.setFont (theme::font (13.0f, true));
        g.drawText ("REC", title.removeFromLeft (50), juce::Justification::centredLeft);
        g.setColour (theme::text);
        g.drawText ("Recorder", title.removeFromLeft (300), juce::Justification::centredLeft);
        return;
    }
    const bool isBus = current >= kMaxInputs;
    const int idx = isBus ? current - kMaxInputs : current;
    const auto colour = isBus ? theme::busColour (idx) : theme::accentIn;
    const auto name = isBus ? ctx.engine.buses[(size_t) idx].name : ctx.engine.inputs[(size_t) idx].name;
    g.setColour (colour);
    g.setFont (theme::font (13.0f, true));
    const auto code = isBus ? busCode (idx) : "IN " + juce::String (idx + 1);
    g.drawText (code, title.removeFromLeft (50), juce::Justification::centredLeft);
    g.setColour (theme::text);
    g.drawText (name, title.removeFromLeft (300), juce::Justification::centredLeft);
}

void Inspector::resized()
{
    if (content != nullptr)
        content->setBounds (getLocalBounds().reduced (8, 6).withTrimmedTop (24));
}

void Inspector::updateMeters()
{
    if (auto* d = dynamic_cast<InputDetail*> (content.get()))
        d->updateMeters();
    else if (auto* rd = dynamic_cast<RecorderDetail*> (content.get()))
        rd->updateMeters();
}
} // namespace hydra

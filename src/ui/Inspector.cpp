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
/** 6-band EQ: type picker | curve | selected band controls. Shared by inputs and buses. */
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
        for (juce::Component* s : std::initializer_list<juce::Component*> { &eqSec, &gateSec, &dnSec, &compSec, &fxSec }) addAndMakeVisible (s);
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
        auto r = getLocalBounds();
        eqSec.setBounds (r.removeFromLeft (juce::jlimit (440, 600, r.getWidth() / 3)));
        gateSec.setBounds (r.removeFromLeft (150));
        dnSec.setBounds (r.removeFromLeft (140));
        compSec.setBounds (r.removeFromLeft (200));
        fxSec.setBounds (r);

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

    void paintOverChildren (juce::Graphics& g) override
    {
        g.setColour (strip.denoiser->isSupported() ? theme::textFaint : theme::warn);
        g.setFont (theme::font (10.0f));
        g.drawText (strip.denoiser->isSupported() ? strip.dnMode->toText() : juce::String ("needs 48 kHz"), modeArea, juce::Justification::centred);
    }

    void updateMeters()
    {
        gateState.update();
        compGr.update();
        if (const auto v = strip.dnMode->getVersion(); v != modeVersion) { modeVersion = v; repaint (modeArea); }
    }

private:
    UiContext& ctx;
    InputStrip& strip;
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
          eqSec (c, bus.eq, colour, kNumInputs + b), limSec ("Limiter", colour),
          fxSec ("VST3 Insert Chain", colour),
          limOn (c, *bus.limOn, "ON", colour), ceiling (c, *bus.limCeil, "CEILING", colour),
          bypass (c, *bus.fxBypass, "BYPASS", theme::mute),
          chain (c, bus.plugins, colour)
    {
        chain.onScanRequested = std::move (scan);
        for (juce::Component* s : std::initializer_list<juce::Component*> { &eqSec, &limSec, &fxSec }) addAndMakeVisible (s);
        limSec.addAndMakeVisible (limOn);
        limSec.addAndMakeVisible (ceiling);
        fxSec.addAndMakeVisible (bypass);
        bypass.setTooltip ("Bypass all plugins of this bus");
        fxSec.addAndMakeVisible (chain);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        eqSec.setBounds (r.removeFromLeft (juce::jlimit (480, 720, r.getWidth() * 2 / 5)));
        limSec.setBounds (r.removeFromLeft (120));
        fxSec.setBounds (r);

        limOn.setBounds (limSec.headerSlot());
        ceiling.setBounds (limSec.content().withSizeKeepingCentre (76, 70));
        bypass.setBounds (fxSec.getLocalBounds().reduced (10, 7).removeFromTop (20).removeFromRight (64));
        chain.setBounds (fxSec.content());
    }

private:
    Bus& bus;
    juce::Colour colour;
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

/** Recorder: record a virtual bus (B1-B3) to WAV, play back files to any buses. */
class RecorderDetail final : public juce::Component
{
public:
    explicit RecorderDetail (UiContext& c)
        : ctx (c), rec (c.engine.recorder),
          transportSec ("Recorder", theme::mute), sourceSec ("Record Source", theme::accentVirt),
          playSec ("Playback", theme::good), outSec ("Playback Outputs", theme::accentHw),
          recBtn (c, *rec.record, "REC", theme::mute), playBtn (c, *rec.play, "PLAY", theme::good),
          loopBtn (c, *rec.loop, "LOOP", theme::good),
          source (c, *rec.source, theme::accentVirt), level (c, *rec.level, "LEVEL", theme::good)
    {
        for (juce::Component* s : std::initializer_list<juce::Component*> { &transportSec, &sourceSec, &playSec, &outSec }) addAndMakeVisible (s);
        for (juce::Component* k : std::initializer_list<juce::Component*> { &recBtn, &playBtn, &loopBtn }) transportSec.addAndMakeVisible (k);
        for (juce::Component* k : std::initializer_list<juce::Component*> { &source, &folderChip, &formatChip }) sourceSec.addAndMakeVisible (k);
        for (juce::Component* k : std::initializer_list<juce::Component*> { &openChip, &lastChip, &showChip, &level }) playSec.addAndMakeVisible (k);
        for (int b = 0; b < kNumBuses; ++b)
        {
            outs.push_back (std::make_unique<Toggle> (c, *rec.out[(size_t) b], busCode (b), theme::busColour (b)));
            outSec.addAndMakeVisible (*outs.back());
        }
        for (auto* chip : { &folderChip, &formatChip, &openChip, &lastChip, &showChip })
        {
            chip->showArrow = false;
            chip->centred = true;
        }
        formatChip.showArrow = true;

        recBtn.setTooltip ("Record the selected bus to a WAV file (stop with a second click)");
        playBtn.setTooltip ("Play the loaded file to the buses selected under Playback Outputs");
        folderChip.setTooltip ("Folder for new recordings");
        openChip.setTooltip ("Load an audio file for playback (WAV, AIFF, FLAC, Ogg, MP3)");
        lastChip.setTooltip ("Load the most recent recording");
        showChip.setTooltip ("Show the loaded file in Explorer");

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
        auto r = getLocalBounds();
        transportSec.setBounds (r.removeFromLeft (360));
        sourceSec.setBounds (r.removeFromLeft (250));
        playSec.setBounds (r.removeFromLeft (380));
        outSec.setBounds (r);

        auto t = transportSec.content();
        auto buttons = t.removeFromBottom (30);
        rowLayout (buttons, { &recBtn, &playBtn, &loopBtn }, 6);
        timeArea = t.translated (transportSec.getX(), transportSec.getY());

        auto s = sourceSec.content();
        source.setBounds (s.removeFromTop (26));
        s.removeFromTop (8);
        sourceInfo = s.removeFromTop (16).translated (sourceSec.getX(), sourceSec.getY());
        auto chips = s.removeFromBottom (26);
        folderChip.setBounds (chips.removeFromLeft (chips.getWidth() / 2 - 3));
        chips.removeFromLeft (6);
        formatChip.setBounds (chips);

        auto p = playSec.content();
        level.setBounds (p.removeFromRight (70).withSizeKeepingCentre (70, 72));
        p.removeFromRight (8);
        auto pchips = p.removeFromBottom (26);
        rowLayout (pchips, { &openChip, &lastChip, &showChip }, 5);
        fileArea = p.translated (playSec.getX(), playSec.getY());

        auto o = outSec.content().withSizeKeepingCentre (outSec.content().getWidth(), 50);
        auto hw = o.removeFromTop (22);
        const int cell = juce::jmin (64, (hw.getWidth() - 4 * 4) / 5);
        for (int b = 0; b < kNumHwBuses; ++b) { outs[(size_t) b]->setBounds (hw.removeFromLeft (cell)); hw.removeFromLeft (4); }
        o.removeFromTop (6);
        for (int b = kNumHwBuses; b < kNumBuses; ++b) { outs[(size_t) b]->setBounds (o.removeFromLeft (cell)); o.removeFromLeft (4); }
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        // Transport: big time display + state
        const bool recording = rec.isRecording(), playing = rec.isPlaying();
        auto t = timeArea;
        g.setFont (theme::mono (30.0f));
        g.setColour (recording ? theme::mute : playing ? theme::good : theme::textDim);
        g.drawText (shownTime, t.removeFromTop (juce::jmin (46, t.getHeight() - 18)), juce::Justification::centredLeft);
        g.setFont (theme::font (11.5f));
        g.setColour (rec.getLastError().isNotEmpty() && ! recording && ! playing ? theme::warn : theme::textDim);
        g.drawFittedText (stateText, t, juce::Justification::topLeft, 2);

        g.setColour (theme::textFaint);
        g.setFont (theme::font (11.0f));
        g.drawFittedText (sourceText, sourceInfo, juce::Justification::centredLeft, 1);

        g.setColour (rec.getLoadedFile() != juce::File() ? theme::text : theme::textFaint);
        g.setFont (theme::font (12.5f, true));
        auto f = fileArea;
        g.drawFittedText (fileTitle, f.removeFromTop (20), juce::Justification::centredLeft, 1);
        g.setColour (theme::textDim);
        g.setFont (theme::font (11.0f));
        g.drawFittedText (fileInfo, f.removeFromTop (34), juce::Justification::topLeft, 2);
    }

    void updateMeters()
    {
        const bool recording = rec.isRecording(), playing = rec.isPlaying();
        juce::String time;
        if (recording)    time = formatTime (rec.getRecordSeconds());
        else if (playing) time = formatTime (rec.getPlaySeconds()) + "  /  " + formatTime (rec.getPlayLengthSeconds());
        else              time = formatTime (0.0);
        const auto v = rec.stateVersion.load() + rec.source->getVersion();
        if (v != seenVersion) { seenVersion = v; refreshTexts(); }
        if (time != shownTime) { shownTime = time; repaint (timeArea); }
    }

private:
    void refreshTexts()
    {
        const bool recording = rec.isRecording(), playing = rec.isPlaying();
        if (recording)    stateText = "Recording " + busCode (rec.getRecordSourceBus()) + "  ->  " + rec.getRecordingFile().getFileName();
        else if (playing) stateText = "Playing " + rec.getLoadedFile().getFileName();
        else if (rec.getLastError().isNotEmpty()) stateText = rec.getLastError();
        else              stateText = "Stopped. Records the selected bus after its fader and limiter.";

        sourceText = "Records " + busCode (rec.getRecordSourceBus()) + "  ->  " + rec.getFolder().getFileName();
        folderChip.text = "Folder...";
        formatChip.text = rec.bitDepth >= 32 ? "WAV 32f" : "WAV " + juce::String (rec.bitDepth);

        const auto file = rec.getLoadedFile();
        fileTitle = file != juce::File() ? file.getFileName() : juce::String ("No file loaded");
        fileInfo = file != juce::File() ? formatTime (rec.getPlayLengthSeconds()) + "   ·   " + file.getParentDirectory().getFullPathName()
                                        : juce::String ("Record something or open a file.");
        for (auto* chip : { &folderChip, &formatChip }) chip->repaint();
        repaint();
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
                                      juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Recorder", err);
                              });
    }

    UiContext& ctx;
    Recorder& rec;
    Section transportSec, sourceSec, playSec, outSec;
    Toggle recBtn, playBtn, loopBtn;
    ChoiceSegments source;
    Knob level;
    Chip folderChip { "Folder...", theme::text }, formatChip { "WAV 24", theme::text };
    Chip openChip { "Open File...", theme::text }, lastChip { "Last Take", theme::text }, showChip { "Show", theme::text };
    std::vector<std::unique_ptr<Toggle>> outs;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Rectangle<int> timeArea, sourceInfo, fileArea;
    juce::String shownTime, stateText, sourceText, fileTitle, fileInfo;
    uint64_t seenVersion = 0;
};
} // namespace

//==============================================================================
struct PluginChainView::Card : juce::Component
{
    Card (PluginChainView& o, int i) : owner (o), index (i)
    {
        for (auto* b : { &bypass, &edit, &left, &right, &remove })
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
        auto later = [this] (std::function<void (PluginChain&, int)> fn)
        {
            juce::Component::SafePointer<PluginChainView> o (&owner);
            const int ix = index;
            juce::MessageManager::callAsync ([o, ix, fn] { if (o != nullptr) { fn (o->chain, ix); o->refresh(); } });
        };
        left.onClick   = [later] { later ([] (PluginChain& c, int i) { c.move (i, i - 1); }); };
        right.onClick  = [later] { later ([] (PluginChain& c, int i) { c.move (i, i + 1); }); };
        remove.onClick = [later] { later ([] (PluginChain& c, int i) { c.remove (i); }); };
        bypass.setTooltip ("Bypass this plugin");
        edit.setTooltip ("Open plugin editor");
        remove.setTooltip ("Remove plugin");
        refresh();
    }

    void refresh()
    {
        auto* s = owner.chain.getSlot (index);
        if (s == nullptr) return;
        name = s->instance->getName();
        vendor = s->description.manufacturerName;
        const int lat = s->instance->getLatencySamples();
        latency = lat > 0 ? juce::String (lat) + " smp latency" : juce::String();
        bypass.text = s->bypassed ? "OFF" : "ON";
        bypass.active = ! s->bypassed;
        bypass.colour = s->bypassed ? theme::textDim : owner.accent;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (2.0f);
        g.setColour (theme::raised);
        g.fillRoundedRectangle (r, 7.0f);
        g.setColour (bypass.active ? owner.accent.withAlpha (0.6f) : theme::outline);
        g.drawRoundedRectangle (r, 7.0f, 1.0f);
        auto t = r.reduced (10.0f, 8.0f);
        g.setColour (theme::text);
        g.setFont (theme::font (13.0f, true));
        g.drawFittedText (name, t.removeFromTop (18.0f).toNearestInt(), juce::Justification::centredLeft, 1);
        g.setColour (theme::textDim);
        g.setFont (theme::font (11.0f));
        g.drawFittedText (vendor, t.removeFromTop (15.0f).toNearestInt(), juce::Justification::centredLeft, 1);
        if (latency.isNotEmpty())
        {
            g.setColour (theme::warn);
            g.drawFittedText (latency, t.removeFromTop (15.0f).toNearestInt(), juce::Justification::centredLeft, 1);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8).removeFromBottom (22);
        rowLayout (r, { &bypass, &edit, &left, &right, &remove }, 3);
    }

    void mouseDoubleClick (const juce::MouseEvent&) override { owner.openEditor (index); }

    PluginChainView& owner;
    int index;
    juce::String name, vendor, latency;
    Chip bypass { "ON" }, edit { "EDIT" }, left { "<" }, right { ">" }, remove { "X" };
};

PluginChainView::PluginChainView (UiContext& c, PluginChain& ch, juce::Colour a) : ctx (c), chain (ch), accent (a)
{
    addAndMakeVisible (addButton);
    addButton.showArrow = false;
    addButton.centred = true;
    addButton.onClick = [this] { showAddMenu(); };
    refresh();
}

void PluginChainView::refresh()
{
    cards.clear();
    for (int i = 0; i < chain.size(); ++i)
    {
        cards.push_back (std::make_unique<Card> (*this, i));
        addAndMakeVisible (*cards.back());
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
    auto r = getLocalBounds();
    addButton.setBounds (r.removeFromLeft (170).withSizeKeepingCentre (170, 32));
    r.removeFromLeft (8);
    for (auto& c : cards)
    {
        if (r.getWidth() < 120) { c->setVisible (false); continue; }
        c->setVisible (true);
        c->setBounds (r.removeFromLeft (juce::jmin (200, r.getWidth())));
        r.removeFromLeft (4);
    }
}

void PluginChainView::showAddMenu()
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
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Plugin", "Could not load " + d.name + "\n" + err);
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
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&addButton));
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
        if (k < 0) return; // all six bands in use
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
            static const float defFreq[ParamEq::kBands] = { 80, 250, 800, 2500, 6000, 12000 };
            if (const int k = owner.graph.addBand (defFreq[band], 0.0f); k >= 0) owner.selectBand (k);
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
      eq (sel < kNumInputs ? c.engine.inputs[(size_t) sel].eq : c.engine.buses[(size_t) (sel - kNumInputs)].eq),
      colour (sel < kNumInputs ? theme::accentIn : theme::busColour (sel - kNumInputs)),
      onToggle (c, *eq.on, "EQ ON", colour), graph (c, eq, colour, true),
      tap (sel < kNumInputs ? c.engine.inputs[(size_t) sel].spectrum : c.engine.buses[(size_t) (sel - kNumInputs)].spectrum),
      fft (std::make_unique<juce::dsp::FFT> (kFftOrder))
{
    history.assign (kFftSize, 0.0f);
    fftData.assign (kFftSize * 2, 0.0f);
    window.resize (kFftSize);
    for (int i = 0; i < kFftSize; ++i) // Hann
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (kFftSize - 1));
    tap.ring.skip (tap.ring.available()); // stale audio from an earlier view
    tap.users.fetch_add (1);

    title = sel < kNumInputs ? "IN " + juce::String (sel + 1) + "   " + c.engine.inputs[(size_t) sel].name
                             : busCode (sel - kNumInputs) + "   " + c.engine.buses[(size_t) (sel - kNumInputs)].name;
    addAndMakeVisible (onToggle);
    addAndMakeVisible (graph);
    for (int k = 0; k < ParamEq::kBands; ++k)
    {
        bands.push_back (std::make_unique<BandColumn> (*this, k));
        addAndMakeVisible (*bands.back());
    }
    graph.onBandSelected = [this] (int) { for (auto& b : bands) b->repaint(); };
    for (int k = 0; k < ParamEq::kBands; ++k) // start on the first band in use
        if (eq.bandOn[(size_t) k]->isOn()) { graph.setSelectedBand (k); break; }
    setSize (1240, 780);
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
                "Background: live spectrum after the EQ (dBFS on the right).",
                head.withTrimmedLeft (260).withTrimmedRight (120), juce::Justification::centredLeft);
}

void EqEditor::resized()
{
    auto r = getLocalBounds().reduced (14, 0);
    auto head = r.removeFromTop (44);
    onToggle.setBounds (head.removeFromRight (96).withSizeKeepingCentre (96, 26));
    auto bottom = r.removeFromBottom (juce::jlimit (170, 230, r.getHeight() / 3)).withTrimmedBottom (10);
    r.removeFromBottom (8);
    graph.setBounds (r);
    const int w = bottom.getWidth() / ParamEq::kBands;
    for (auto& b : bands) b->setBounds (bottom.removeFromLeft (w));
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
    else if (sel < kNumInputs)  content = std::make_unique<InputDetail> (ctx, sel, scan);
    else                        content = std::make_unique<BusDetail> (ctx, sel - kNumInputs, scan);
    addAndMakeVisible (*content);
    shownLatency = latencyText();
    resized();
    repaint();
}

juce::String Inspector::latencyText() const
{
    auto& e = ctx.engine;
    const double sr = juce::jmax (1.0, e.getSampleRate());
    auto ms = [] (double v) { return juce::String (v, 1) + " ms"; };
    juce::String t;
    double total = 0.0;

    if (current == kRecorderSelect)
    {
        const auto& rec = e.recorder;
        if (rec.isRecording()) return "RECORDING   " + busCode (rec.getRecordSourceBus());
        if (rec.isPlaying())   return "PLAYING";
        return {};
    }
    if (current < kNumInputs)
    {
        const auto& s = e.inputs[(size_t) current];
        if (s.source.kind == IoTarget::Kind::None) return "LATENCY   no source selected";
        const double dev = e.getIoLatencyMs (s.source, true);
        const double dn = 1000.0 * s.dspLatency.load() / sr;
        const double fx = 1000.0 * s.plugins.getTotalLatency() / sr;
        t << "LATENCY   input " << (dev < 0 ? juce::String ("offline") : ms (dev));
        if (s.dnOn->isOn()) t << "  +  denoiser " << ms (dn);
        if (fx > 0.0) t << "  +  plugins " << ms (fx);
        total = juce::jmax (0.0, dev) + dn + fx;
    }
    else
    {
        const auto& b = e.buses[(size_t) (current - kNumInputs)];
        const double fx = 1000.0 * b.plugins.getTotalLatency() / sr;
        const double dev = e.getIoLatencyMs (b.output, false);
        t << "LATENCY   output " << (dev < 0 ? juce::String ("offline") : ms (dev));
        if (fx > 0.0) t << "  +  plugins " << ms (fx);
        total = juce::jmax (0.0, dev) + fx;
    }
    t << "   =   " << ms (total);
    return t;
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
        g.setColour (theme::textDim);
        g.setFont (theme::mono (12.0f));
        g.drawText (shownLatency, title, juce::Justification::centredRight);
        return;
    }
    const bool isBus = current >= kNumInputs;
    const int idx = isBus ? current - kNumInputs : current;
    const auto colour = isBus ? theme::busColour (idx) : theme::accentIn;
    const auto name = isBus ? ctx.engine.buses[(size_t) idx].name : ctx.engine.inputs[(size_t) idx].name;
    g.setColour (colour);
    g.setFont (theme::font (13.0f, true));
    const auto code = isBus ? busCode (idx) : "IN " + juce::String (idx + 1);
    g.drawText (code, title.removeFromLeft (50), juce::Justification::centredLeft);
    g.setColour (theme::text);
    g.drawText (name, title.removeFromLeft (300), juce::Justification::centredLeft);

    g.setColour (theme::textDim);
    g.setFont (theme::mono (12.0f));
    g.drawText (shownLatency, title, juce::Justification::centredRight);
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

    if (++tick >= 5) // ~6 Hz is plenty for the latency readout
    {
        tick = 0;
        auto t = latencyText();
        if (t != shownLatency)
        {
            shownLatency = t;
            repaint (getLocalBounds().reduced (14, 8).removeFromTop (20));
        }
    }
}
} // namespace hydra

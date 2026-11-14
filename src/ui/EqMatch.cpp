#include "EqMatch.h"
#include <map>
#include <numeric>

namespace hydra::eqmatch
{
namespace
{
juce::String channelLabel (UiContext& ctx, int c)
{
    const auto n = ctx.engine.channelName (c);
    return channelCode (c) + (n.isNotEmpty() ? "  " + n : juce::String());
}

int freeBands (const ParamEq& eq)
{
    int n = 0;
    for (int k = 0; k < ParamEq::kBands; ++k)
        if (! eq.bandOn[(size_t) k]->isOn()) ++n;
    return n;
}
} // namespace

//==============================================================================
State::State (int ch, int inst) : channel (ch), instance (inst)
{
    juce::StringArray counts;
    for (int i = 1; i <= ParamEq::kBands; ++i) counts.add (juce::String (i));
    bands = &reg.addChoice ("bands", "Bands", counts, 7);
}

void State::feedPre (const float* l, const float* r, int n)
{
    if (! capturingPre() || n <= 0) return;
    analyzer.feed (l, r, n);
}

State& stateFor (int channel, int instance)
{
    static auto& all = *new std::map<int, std::unique_ptr<State>>();   // never freed (see State::ring)
    auto& p = all[channel * 16 + instance];
    if (p == nullptr) p = std::make_unique<State> (channel, instance);
    return *p;
}

int writeBands (ParamEq& eq, const std::vector<CorrectionProfile::Band>& bands, const std::vector<int>& sides)
{
    std::vector<size_t> order (bands.size());
    std::iota (order.begin(), order.end(), (size_t) 0);
    std::sort (order.begin(), order.end(), [&] (size_t a, size_t b) { return bands[a].freq < bands[b].freq; });
    int written = 0;
    for (const auto index : order)
    {
        const auto& b = bands[index];
        const int side = index < sides.size() ? sides[index] : ParamEq::both;
        int k = 0;
        while (k < ParamEq::kBands && eq.bandOn[(size_t) k]->isOn()) ++k;
        if (k >= ParamEq::kBands) break;
        const auto i = (size_t) k;
        eq.type[i]->set ((float) b.type);
        eq.q[i]->set (b.q);
        eq.freq[i]->set (b.freq);
        eq.gain[i]->set (std::round (juce::jlimit (-18.0f, 18.0f, b.gainDb) * 10.0f) / 10.0f);
        eq.enabled[i]->set (1.0f);
        eq.chan[i]->set ((float) side);
        eq.bandOn[i]->set (1.0f);
        ++written;
    }
    if (written > 0 && ! eq.on->isOn()) eq.on->set (1.0f);
    return written;
}

//==============================================================================
/** Small picture of a captured spectrum (at the current smoothing) with its name; while capturing, how long. */
struct Panel::Thumb final : juce::Component
{
    Thumb (Panel& o, bool ref) : owner (o), reference (ref) { setInterceptsMouseClicks (false, false); }

    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat();
        const bool light = theme::light();
        g.setColour (light ? juce::Colour (0xfff0f2f6) : juce::Colour (0xff0d0f13));
        g.fillRoundedRectangle (r, 5.0f);
        const auto colour = reference ? kReferenceColour : kSourceColour;
        auto& m = owner.eq.matchData;
        const auto& curve = owner.eq.matchSpectrum (reference, owner.state.viewSide);
        const bool capturing = owner.state.capture == (reference ? State::Capture::reference : State::Capture::source);
        const auto plot = r.reduced (4.0f, 4.0f).withTrimmedTop (14.0f);
        if (! curve.empty())
        {
            const auto shown = match::smoothFine (curve, match::smoothingOctaves (owner.eq.matchSmooth->get() * 0.01f));
            auto y = [&] (double db) { return plot.getY() + plot.getHeight() * (float) ((12.0 - juce::jlimit (-36.0, 12.0, db)) / 48.0); };
            juce::Path line;
            for (int i = 0; i < match::kFinePoints; ++i)
            {
                const float x = plot.getX() + plot.getWidth() * (float) i / (float) (match::kFinePoints - 1);
                const float yy = y (shown[(size_t) i]);
                if (i == 0) line.startNewSubPath (x, yy); else line.lineTo (x, yy);
            }
            juce::Path area (line);
            area.lineTo (plot.getRight(), plot.getBottom());
            area.lineTo (plot.getX(), plot.getBottom());
            area.closeSubPath();
            g.setGradientFill (juce::ColourGradient (colour.withAlpha (0.45f), 0.0f, plot.getY(), colour.withAlpha (0.08f), 0.0f, plot.getBottom(), false));
            g.fillPath (area);
            g.setColour (colour);
            g.strokePath (line, juce::PathStrokeType (1.3f));
        }
        g.setFont (theme::font (9.5f, true));
        g.setColour (theme::textDim);
        const auto name = reference ? m.referenceName : m.sourceName;
        const auto caption = r.reduced (6.0f, 3.0f).removeFromTop (12.0f);
        if (capturing)
        {
            g.setColour (colour);
            g.drawText ("LISTENING  " + juce::String (owner.state.analyzer.totalSeconds(), 0) + " s", caption, juce::Justification::centredLeft);
        }
        else if (! curve.empty())
            g.drawText (name.toUpperCase(), caption, juce::Justification::centredLeft);
        if (curve.empty() && ! capturing)
        {
            g.setFont (theme::font (10.5f));
            g.setColour (theme::textFaint);
            const auto hint = reference ? (owner.state.listenTo >= 0 ? "Press Capture while " + channelLabel (owner.ctx, owner.state.listenTo) + " plays"
                                                                     : juce::String ("Choose what this channel should sound like"))
                                        : juce::String ("Let this channel play, then press Capture");
            g.drawFittedText (hint, getLocalBounds().reduced (8), juce::Justification::centred, 2, 0.9f);
        }
    }

    Panel& owner;
    bool reference;
};

//==============================================================================
Panel::Panel (UiContext& c, State& s, ParamEq& e)
    : ctx (c), state (s), eq (e),
      onToggle (c, *e.matchOn, "MATCH ON", theme::fxMatch), separate (c, *e.matchSplit, "SEPARATE L/R", theme::fxMatch),
      smooth (c, *e.matchSmooth, "SMOOTH", theme::fxMatch), amount (c, *e.matchAmount, "AMOUNT", theme::fxMatch),
      bandCount (c, *s.bands, "BANDS", theme::fxMatch),
      refThumb (std::make_unique<Thumb> (*this, true)), srcThumb (std::make_unique<Thumb> (*this, false))
{
    for (juce::Component* x : std::initializer_list<juce::Component*> { &source, &captureRef, &clearRef, &captureSrc, &clearSrc, &fullRange, &intoBands,
                                                                         &convert, &cancel, &onToggle, &separate, &smooth, &amount, &bandCount, &info,
                                                                         refThumb.get(), srcThumb.get() })
        addAndMakeVisible (x);
    for (auto* ch : { &captureRef, &clearRef, &captureSrc, &clearSrc, &fullRange, &intoBands, &convert, &cancel })
    {
        ch->centred = true;
        ch->showArrow = false;
    }
    for (auto* k : { &smooth, &amount, &bandCount }) k->alwaysShowValue = true;

    source.setTooltip ("What the reference is: this channel (before the EQ, e.g. a recording that sounded good), another "
                       "channel (after its effects), an audio file, standard speech or flat. Choosing a channel does not "
                       "record yet: press Capture");
    captureRef.setTooltip ("Captures the reference while it plays; press again to stop (the curve builds up meanwhile)");
    captureSrc.setTooltip ("Captures how this channel sounds before the EQ while it plays; press again to stop");
    smooth.setTooltip ("0 = every detail of the difference (1/48 octave), 100 % = only broad strokes (1.5 octaves)");
    amount.setTooltip ("How far this channel is moved toward the reference");
    onToggle.setTooltip ("Switches the match on / off (compare)");
    separate.setTooltip ("Off (default): one match from the sum of both channels, on both. On: left and right (or mid and side) "
                         "are matched separately; the EQ switches to Left / Right if it was Stereo. Capture again after switching it on, "
                         "so the sides are analysed");
    fullRange.setTooltip ("Match 20 Hz .. 20 kHz again. Drag the two handles in the curve to limit the range");
    intoBands.setTooltip ("Turns the match into normal EQ bands that you can edit. You choose how many");
    bandCount.setTooltip ("How many EQ bands may be used (only free bands; yours stay)");
    convert.setTooltip ("Writes the bands into the EQ and switches the high-resolution match off");
    info.setText ("Match\n1. REFERENCE: choose what this channel should sound like and press Capture while it plays "
                  "(or pick an audio file, standard speech or flat).\n2. THIS CHANNEL: press Capture while this channel plays.\n"
                  "3. The difference is applied at high resolution. Shape it with SMOOTH, AMOUNT and the two range "
                  "handles in the curve.\n4. Optional: INTO BANDS turns it into normal EQ bands.\n\n"
                  "Orange: reference, blue: this channel, coloured area: what the match does.");

    source.onClick = [this] { showSourceMenu(); };
    captureRef.onClick = [this]
    {
        if (state.capture == State::Capture::reference) stopCapture (true);
        else startCapture (State::Capture::reference);
        refresh();
    };
    captureSrc.onClick = [this]
    {
        if (state.capture == State::Capture::source) stopCapture (true);
        else startCapture (State::Capture::source);
        refresh();
    };
    clearRef.onClick = [this]
    {
        if (state.capture == State::Capture::reference) stopCapture (false);
        for (auto* c : { &eq.matchData.reference, &eq.matchData.referenceL, &eq.matchData.referenceR, &eq.matchData.referenceS }) c->clear();
        eq.matchData.referenceName = {};
        ++eq.matchData.version;
        refresh();
    };
    clearSrc.onClick = [this]
    {
        if (state.capture == State::Capture::source) stopCapture (false);
        for (auto* c : { &eq.matchData.source, &eq.matchData.sourceL, &eq.matchData.sourceR, &eq.matchData.sourceS }) c->clear();
        eq.matchData.sourceName = {};
        ++eq.matchData.version;
        refresh();
    };
    fullRange.onClick = [this] { eq.matchLow->set (20.0f); eq.matchHigh->set (20000.0f); };
    intoBands.onClick = [this]
    {
        if (! eq.matchData.ready()) { state.say ("Capture the reference and this channel first"); repaint(); return; }
        state.converting = true;
        previewKey = ~0ull;
        updatePreview();
        refresh();
    };
    cancel.onClick = [this]
    {
        state.converting = false;
        state.preview.clear();
        ++state.previewVersion;
        refresh();
    };
    convert.onClick = [this]
    {
        const int n = writeBands (eq, state.preview, state.previewSides);
        eq.matchOn->set (0.0f);
        eq.matchData.converted = true;
        ++eq.matchData.version;
        state.converting = false;
        state.preview.clear();
        ++state.previewVersion;
        state.say (juce::String (n) + (n == 1 ? " band" : " bands") + " added; the match is off");
        ctx.engine.updateEqMatches();
        if (onConverted) onConverted();
    };
    startTimerHz (30);
    refresh();
}

Panel::~Panel()
{
    stopTimer();
    if (state.capture != State::Capture::idle) stopCapture (true);
    if (state.converting) { state.converting = false; state.preview.clear(); ++state.previewVersion; }
}

void Panel::resized()
{
    auto r = getLocalBounds();
    constexpr int gap = 8;
    const int w = r.getWidth() - 3 * gap;
    cards[0] = r.removeFromLeft ((int) (w * 0.30)); r.removeFromLeft (gap);
    cards[1] = r.removeFromLeft ((int) (w * 0.27)); r.removeFromLeft (gap);
    cards[2] = r.removeFromLeft ((int) (w * 0.19)); r.removeFromLeft (gap);
    cards[3] = r;

    auto body = [] (juce::Rectangle<int> c) { return c.reduced (10, 0).withTrimmedTop (28).withTrimmedBottom (10); };
    {
        source.setBounds (cards[0].getRight() - 10 - cards[0].getWidth() / 2, cards[0].getY() + 4, cards[0].getWidth() / 2, 22);
        auto b = body (cards[0]);
        auto buttons = b.removeFromRight (96);
        b.removeFromRight (8);
        refThumb->setBounds (b);
        captureRef.setBounds (buttons.removeFromTop (34));
        buttons.removeFromTop (6);
        clearRef.setBounds (buttons.removeFromTop (26));
    }
    {
        auto b = body (cards[1]);
        auto buttons = b.removeFromRight (96);
        b.removeFromRight (8);
        srcThumb->setBounds (b);
        captureSrc.setBounds (buttons.removeFromTop (34));
        buttons.removeFromTop (6);
        clearSrc.setBounds (buttons.removeFromTop (26));
    }
    {
        info.setBounds (cards[2].getRight() - 10 - InfoButton::kSize, cards[2].getY() + 6, InfoButton::kSize, InfoButton::kSize);
        auto b = cards[2].reduced (6, 0).withTrimmedTop (24).withTrimmedBottom (6);
        smooth.setBounds (b.removeFromLeft (b.getWidth() / 2));
        amount.setBounds (b);
    }
    {
        auto b = body (cards[3]);
        // RESULT
        auto row = [&b] (int h) { auto x = b.removeFromTop (h); b.removeFromTop (6); return x; };
        auto bb = b;
        onToggle.setBounds (row (28));
        {
            auto r2 = row (26);
            fullRange.setBounds (r2.removeFromLeft (r2.getWidth() / 2 - 3));
            r2.removeFromLeft (6);
            separate.setBounds (r2);
        }
        intoBands.setBounds (row (30));
        // INTO BANDS
        b = bb;
        bandCount.setBounds (b.removeFromLeft (76));
        b.removeFromLeft (8);
        b.removeFromTop (34);   // text line
        convert.setBounds (b.removeFromTop (32));
        b.removeFromTop (6);
        cancel.setBounds (b.removeFromTop (26));
    }
}

void Panel::paint (juce::Graphics& g)
{
    const juce::String heads[4] = { "REFERENCE", "THIS CHANNEL", "FINE TUNE", state.converting ? "INTO BANDS" : "RESULT" };
    for (int i = 0; i < 4; ++i)
    {
        g.setColour (theme::panel);
        g.fillRoundedRectangle (cards[i].toFloat(), 7.0f);
        g.setColour (theme::cardLine);
        g.drawRoundedRectangle (cards[i].toFloat().reduced (0.5f), 7.0f, 1.0f);
        g.setColour (theme::fxMatch);
        g.setFont (theme::font (9.5f, true));
        g.drawText (heads[i], cards[i].getX() + 10, cards[i].getY() + 7, 120, 16, juce::Justification::centredLeft);
    }
    if (state.converting)
    {
        auto t = cards[3].reduced (10, 0).withTrimmedTop (28).withTrimmedLeft (84).removeFromTop (30);
        g.setFont (theme::font (10.5f));
        g.setColour (theme::textDim);
        const int wanted = juce::roundToInt (state.bands->get()) + 1;
        juce::String text = juce::String ((int) state.preview.size()) + " bands, " + juce::String (state.previewRms, 1) + " dB from the match";
        if (eq.matchModeIndex() != ParamEq::stereo) text = juce::String ((int) state.preview.size()) + " bands (BANDS per side), " + juce::String (state.previewRms, 1) + " dB off";
        if (state.previewFree < wanted) text << "\n(only " << state.previewFree << " free bands)";
        g.drawFittedText (text, t, juce::Justification::topLeft, 2, 0.9f);
    }
    // A result or an error, a few seconds, under the cards' titles.
    if (state.message.isNotEmpty() && juce::Time::getMillisecondCounter() - state.messageTime < 4000)
    {
        g.setFont (theme::font (10.0f));
        g.setColour (theme::warn);
        g.drawFittedText (state.message, cards[3].getX() + 90, cards[3].getY() + 5, cards[3].getWidth() - 100, 20, juce::Justification::centredRight, 2, 0.8f);
    }
}

void Panel::refresh()
{
    const bool conv = state.converting;
    for (juce::Component* x : std::initializer_list<juce::Component*> { &onToggle, &fullRange, &separate, &intoBands }) x->setVisible (! conv);
    separate.text = eq.modeIndex() == ParamEq::midSide ? "SEPARATE M/S" : "SEPARATE L/R";
    separate.repaint();
    for (juce::Component* x : std::initializer_list<juce::Component*> { &bandCount, &convert, &cancel }) x->setVisible (conv);

    source.text = state.listenTo >= 0 ? channelLabel (ctx, state.listenTo) + (state.listenTo == state.channel ? "  (before the EQ)" : "")
                                      : juce::String ("CHOOSE ...");
    const bool capRef = state.capture == State::Capture::reference, capSrc = state.capture == State::Capture::source;
    captureRef.text = capRef ? "STOP" : "CAPTURE";
    captureRef.active = capRef;
    const bool canRef = capRef || state.listenTo >= 0;
    captureRef.setEnabled (canRef);
    captureRef.setAlpha (canRef ? 1.0f : 0.4f);
    captureSrc.text = capSrc ? "STOP" : "CAPTURE";
    captureSrc.active = capSrc;
    clearRef.setAlpha (eq.matchData.reference.empty() && ! capRef ? 0.4f : 1.0f);
    clearSrc.setAlpha (eq.matchData.source.empty() && ! capSrc ? 0.4f : 1.0f);
    intoBands.setAlpha (eq.matchData.ready() ? 1.0f : 0.4f);
    intoBands.active = false;
    convert.active = true;
    for (juce::Component* x : std::initializer_list<juce::Component*> { &source, &captureRef, &clearRef, &captureSrc, &clearSrc, &fullRange, &intoBands, &convert, &cancel })
        x->repaint();
    refThumb->repaint();
    srcThumb->repaint();
    repaint();
}

void Panel::startCapture (State::Capture kind)
{
    stopCapture (true);
    const int from = kind == State::Capture::source ? state.channel : state.listenTo;
    if (from < 0) { state.say ("Choose what to listen to first"); return; }
    state.tapFrom = -1;
    if (from != state.channel)
    {
        if (state.ring.getCapacity() < 1) state.ring.allocate (2, 384000);   // once, while nothing writes to it
        else state.ring.skip (state.ring.available());
        SpscAudioRing* none = nullptr;
        if (! ctx.engine.fxOf (from).refTap.compare_exchange_strong (none, &state.ring))
        {
            state.say ("Another Match is listening to " + channelCode (from) + " right now");
            return;
        }
        state.tapFrom = from;
    }
    // The single channels are only analysed when the match works on them (saves three of four FFTs while capturing).
    state.analyzer.reset (juce::jmax (8000.0, ctx.engine.getSampleRate()), eq.matchModeIndex() != ParamEq::stereo);
    state.capture = kind;
}

void Panel::stopCapture (bool keep)
{
    const auto was = state.capture;
    if (was == State::Capture::idle) return;
    if (state.tapFrom >= 0 && isFxChannel (state.tapFrom))
    {
        SpscAudioRing* mine = &state.ring;
        ctx.engine.fxOf (state.tapFrom).refTap.compare_exchange_strong (mine, nullptr);
    }
    const int from = was == State::Capture::source ? state.channel : (state.tapFrom >= 0 ? state.tapFrom : state.channel);
    state.tapFrom = -1;
    state.capture = State::Capture::idle;
    if (! keep) return;
    if (! storeCapture (was == State::Capture::source, from, 1.0))
        state.say ("Not enough sound heard: capture again while it plays");
}

bool Panel::storeCapture (bool source, int from, double minSeconds)
{
    std::array<match::Fine, match::FineAnalyzer::numWhich> c;
    if (! state.analyzer.result (c[0], minSeconds)) return false;
    for (int w = 1; w < match::FineAnalyzer::numWhich; ++w)
        if (! state.analyzer.result (c[(size_t) w], 0.0, w)) c[(size_t) w].clear();   // linked capture: the sides were not analysed
    auto& m = eq.matchData;
    m.converted = false;
    const auto name = channelLabel (ctx, from) + (! source && from == state.channel ? " (before the EQ)" : "");
    if (source) { m.source = std::move (c[0]); m.sourceL = std::move (c[1]); m.sourceR = std::move (c[2]); m.sourceS = std::move (c[3]); m.sourceName = name; }
    else { m.reference = std::move (c[0]); m.referenceL = std::move (c[1]); m.referenceR = std::move (c[2]); m.referenceS = std::move (c[3]); m.referenceName = name; }
    ++m.version;
    return true;
}

void Panel::setReference (std::vector<float> curve, const juce::String& name, std::vector<float> l, std::vector<float> r, std::vector<float> side)
{
    if (state.capture == State::Capture::reference) stopCapture (false);
    auto& m = eq.matchData;
    m.reference = std::move (curve);
    m.referenceL = std::move (l);
    m.referenceR = std::move (r);
    m.referenceS = std::move (side);
    m.referenceName = name;
    m.converted = false;
    ++m.version;
    refresh();
}

void Panel::showSourceMenu()
{
    auto& e = ctx.engine;
    juce::PopupMenu m, others;
    m.addItem (channelLabel (ctx, state.channel) + "  (this channel, before the EQ)", true, state.listenTo == state.channel,
               [this] { state.listenTo = state.channel; refresh(); });
    for (int c = 0; c < kNumChannels; ++c)
        if (c != state.channel && isFxChannel (c) && e.isChannelActive (c))
            others.addItem (channelLabel (ctx, c), true, state.listenTo == c, [this, c] { state.listenTo = c; refresh(); });
    m.addSubMenu ("Another channel (after its effects)", others);
    m.addSeparator();
    m.addItem ("An audio file (a song or a voice you like) ...", [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Reference for Match", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
        juce::Component::SafePointer<Panel> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc)
        {
            if (safe == nullptr || ! fc.getResult().existsAsFile()) return;
            safe->state.job.reset();
            safe->state.job = std::make_unique<match::FileJob> (fc.getResult());
            safe->state.job->startThread (juce::Thread::Priority::low);
            safe->state.say ("Reading " + fc.getResult().getFileName() + " ...");
            safe->repaint();
        });
    });
    m.addItem ("Standard speech (a typical voice)", [this] { setReference (match::speechFine(), "Standard speech"); });
    m.addItem ("Flat (pink noise, a balanced mix)", [this] { setReference (match::Fine ((size_t) match::kFinePoints, 0.0f), "Flat (pink noise)"); });
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&source));
}

void Panel::updatePreview()
{
    const int free = freeBands (eq);
    const uint64_t key = (eq.matchKey() * 31ull + state.bands->getVersion()) * 31ull + (uint64_t) free;
    if (key == previewKey) return;
    previewKey = key;
    state.preview.clear();
    state.previewSides.clear();
    state.previewRms = 0.0;
    state.previewFree = free;
    const double sr = juce::jmax (8000.0, ctx.engine.getSampleRate());
    const int wanted = juce::roundToInt (state.bands->get()) + 1;
    if (eq.matchCurve (0) != nullptr)
    {
        if (eq.matchModeIndex() == ParamEq::stereo)
        {
            state.preview = match::fitBands (*eq.matchCurve (0), juce::jmin (wanted, free), sr, eq.matchLow->get(), eq.matchHigh->get(), &state.previewRms);
            state.previewSides.assign (state.preview.size(), ParamEq::both);
        }
        else   // left / right or mid / side: BANDS per side
        {
            const int n0 = juce::jmin (wanted, free / 2 + free % 2), n1 = juce::jmin (wanted, free - n0);
            double rms[2] {};
            for (int s = 0; s < 2; ++s)
            {
                const auto bands = match::fitBands (*eq.matchCurve (s), s == 0 ? n0 : n1, sr, eq.matchLow->get(), eq.matchHigh->get(), &rms[s]);
                for (const auto& b : bands) { state.preview.push_back (b); state.previewSides.push_back (s + 1); }
            }
            state.previewRms = 0.5 * (rms[0] + rms[1]);
        }
    }
    ++state.previewVersion;
    repaint();
}

void Panel::timerCallback()
{
    ++tick;
    if (state.capture != State::Capture::idle)
    {
        if (state.tapFrom >= 0)
        {
            float l[2048], r[2048];
            float* dst[2] = { l, r };
            for (;;)
            {
                const int got = state.ring.read (dst, 2, 2048);
                if (got <= 0) break;
                state.analyzer.feed (l, r, got);
            }
        }
        if (tick % 2 == 0)   // the curve builds up while it listens (15 times a second)
        {
            const bool src = state.capture == State::Capture::source;
            storeCapture (src, src ? state.channel : (state.tapFrom >= 0 ? state.tapFrom : state.channel), 0.5);
            (state.capture == State::Capture::source ? srcThumb : refThumb)->repaint();
        }
        if (state.analyzer.totalSeconds() >= 1800.0) { stopCapture (true); refresh(); }
    }
    if (state.job != nullptr && state.job->done.load())
    {
        auto job = std::move (state.job);
        job->stopThread (2000);
        if (job->ok) { setReference (job->curves[0], job->name, job->curves[1], job->curves[2], job->curves[3]); state.say ({}); }
        else if (! job->cancel.load()) state.say (job->error);
        repaint();
    }
    if (state.capture == State::Capture::idle || tick % 6 == 0)
        ctx.engine.updateEqMatches();   // the filter follows the knobs quickly while the window is open (while capturing 5x/s)
    if (state.converting) updatePreview();
    if (eq.matchSplit->getVersion() != seenSplit)   // separate sides: the EQ needs a split channel mode
    {
        const bool first = seenSplit == 0;
        seenSplit = eq.matchSplit->getVersion();
        if (! first && eq.matchSplit->isOn())
        {
            if (eq.modeIndex() == ParamEq::stereo) eq.mode->set ((float) ParamEq::leftRight);
            const bool haveSides = eq.modeIndex() == ParamEq::midSide ? ! eq.matchData.sourceS.empty() : ! eq.matchData.sourceL.empty();
            if (eq.matchData.ready() && ! haveSides) state.say ("Capture again to analyse the sides separately");
        }
        refresh();
    }
    const uint64_t seen = eq.matchData.version * 7ull + (uint64_t) state.capture + (state.converting ? 100 : 0) + eq.matchSmooth->getVersion();
    if (seen != lastSeen) { lastSeen = seen; refresh(); }
    if (state.message.isNotEmpty() && juce::Time::getMillisecondCounter() - state.messageTime < 4100 && tick % 6 == 0) repaint();
}
} // namespace hydra::eqmatch

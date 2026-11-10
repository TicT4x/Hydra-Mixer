#include "UiWatch.h"
#include "Settings.h"
#include "state/VoicemeeterImport.h"
#include "state/Session.h"
#include "state/Autostart.h"
#include "state/VacCables.h"
#include "engine/NetLink.h"
#include "remote/RemoteControl.h"
#include "remote/QrCode.h"

namespace hydra
{
/** Puts an "i" right after a title label's text. */
static void placeInfoAfter (InfoButton& info, const juce::Label& title)
{
    const int w = juce::GlyphArrangement::getStringWidthInt (title.getFont(), title.getText());
    info.setBounds (title.getX() + w + 10, title.getY() + (title.getHeight() - InfoButton::kSize) / 2, InfoButton::kSize, InfoButton::kSize);
}

namespace
{
void styleLabel (juce::Label& l, bool dim = true, float size = 13.0f)
{
    l.setFont (theme::font (size));
    l.setColour (juce::Label::textColourId, dim ? theme::textDim : theme::text);
}

/** Base of every settings page, so they all look alike: sections with a small title in capitals (optionally
    with an "i" that explains it), a fine line between the sections, rows with a label column. */
class SettingsPage : public juce::Component
{
public:
    struct Heading { juce::Label title; InfoButton info; };
    static constexpr int kLabelWidth = 150, kLine = 30, kMaxWidth = 660;

    void paint (juce::Graphics& g) override
    {
        g.setColour (theme::outline.withAlpha (0.6f));
        for (auto& d : dividers) g.fillRect (d);
    }

protected:
    void addHeading (Heading& h, const juce::String& text, const juce::String& info = {}, bool visible = true)
    {
        addChildComponent (h.title);
        h.title.setText (text.toUpperCase(), juce::dontSendNotification);
        h.title.setFont (theme::font (12.0f, true));
        h.title.setColour (juce::Label::textColourId, theme::textDim);
        h.title.setBorderSize ({});
        h.title.setVisible (visible);
        if (info.isNotEmpty())
        {
            h.info.setText (text + "\n" + info);
            addChildComponent (h.info);
            h.info.setVisible (visible);
        }
    }
    static void showHeading (Heading& h, bool shown)
    {
        h.title.setVisible (shown);
        if (h.info.getParentComponent() != nullptr) h.info.setVisible (shown);
    }

    /** Start of resized(): the area of the page. */
    juce::Rectangle<int> beginLayout()
    {
        dividers.clear();
        firstSection = true;
        return getLocalBounds().reduced (24, 18);
    }
    /** A new section at the top of r (with a line above it, except the first one). Returns the title's row
        (room right of the title is free for buttons). */
    juce::Rectangle<int> section (juce::Rectangle<int>& r, Heading& h)
    {
        if (! firstSection)
        {
            r.removeFromTop (14);
            dividers.push_back (r.removeFromTop (1));
            r.removeFromTop (14);
        }
        firstSection = false;
        auto row = r.removeFromTop (22);
        h.title.setBounds (row);
        if (h.info.getParentComponent() != nullptr) placeInfoAfter (h.info, h.title);
        r.removeFromTop (8);
        return row;
    }
    /** Label in the label column, the control right of it (at most w wide). */
    static void labelled (juce::Rectangle<int>& r, juce::Label& l, juce::Component& c, int w = 380)
    {
        auto line = r.removeFromTop (kLine);
        l.setBounds (line.removeFromLeft (kLabelWidth));
        c.setBounds (line.withWidth (juce::jmin (w, line.getWidth())));
        r.removeFromTop (8);
    }
    /** A check box line. */
    static void option (juce::Rectangle<int>& r, juce::Component& b)
    {
        b.setBounds (r.removeFromTop (28).withWidth (juce::jmin (kMaxWidth, r.getWidth())));
    }

private:
    std::vector<juce::Rectangle<int>> dividers;
    bool firstSection = true;
};

/** Check box bound to one of the app preferences (stored with the session). */
void bindPref (UiContext& ctx, juce::ToggleButton& b, bool Session::AppPrefs::* field, const juce::String& text, const juce::String& tip = {})
{
    b.setButtonText (text);
    if (tip.isNotEmpty()) b.setTooltip (tip);
    b.setToggleState (ctx.session != nullptr && ctx.session->prefs.*field, juce::dontSendNotification);
    b.onClick = [&ctx, &b, field]
    {
        if (ctx.session == nullptr) return;
        ctx.session->prefs.*field = b.getToggleState();
        ctx.session->prefsVersion.fetch_add (1);
        if (ctx.prefsChanged) ctx.prefsChanged();
    };
}

//==============================================================================
class AudioTab final : public SettingsPage, private juce::Timer
{
public:
    AudioTab (UiContext& c, std::function<void()> changed) : ctx (c), onChanged (std::move (changed))
    {
        for (auto* l : { &typeL, &devL, &rateL, &bufL }) { addAndMakeVisible (l); styleLabel (*l); }
        for (auto* b : { &typeBox, &devBox, &rateBox, &bufBox }) addAndMakeVisible (b);
        addAndMakeVisible (apply);
        addAndMakeVisible (panel);
        addAndMakeVisible (status);
        styleLabel (status, false);
        status.setJustificationType (juce::Justification::topLeft);
        addHeading (masterH, "Master device",
                    "The master device is the clock of the whole mixer. All of its inputs/outputs are "
                    "processed with zero extra buffering. Use an ASIO driver here for the lowest possible "
                    "latency; every other device is synchronised automatically with an adaptive, "
                    "self-tuning drift compensator.");
        addHeading (statusH, "Status");

        for (auto* t : ctx.engine.getDeviceTypes())
            typeBox.addItem (t->getTypeName(), typeBox.getNumItems() + 1);

        typeBox.onChange = [this] { fillDevices(); };
        devBox.onChange  = [this] { fillFormats(); };
        apply.onClick    = [this] { applyConfig(); };
        panel.onClick    = [this]
        {
            if (auto* d = ctx.engine.getMasterDevice(); d != nullptr && d->hasControlPanel())
                if (d->showControlPanel()) applyConfig();
        };

        loadCurrent();
        startTimerHz (2);
    }

    void resized() override
    {
        auto r = beginLayout();
        section (r, masterH);
        labelled (r, typeL, typeBox, 420);
        labelled (r, devL, devBox, 420);
        labelled (r, rateL, rateBox, 420);
        labelled (r, bufL, bufBox, 420);
        auto buttons = r.removeFromTop (32).withTrimmedLeft (kLabelWidth);
        apply.setBounds (buttons.removeFromLeft (150));
        buttons.removeFromLeft (10);
        panel.setBounds (buttons.removeFromLeft (190));
        section (r, statusH);
        status.setBounds (r.removeFromTop (44).withWidth (juce::jmin (kMaxWidth + 100, r.getWidth())));
    }

private:
    void loadCurrent()
    {
        const auto cfg = ctx.engine.getMasterConfig();
        for (int i = 0; i < typeBox.getNumItems(); ++i)
            if (typeBox.getItemText (i) == cfg.typeName) typeBox.setSelectedItemIndex (i, juce::dontSendNotification);
        fillDevices();
        for (int i = 0; i < devBox.getNumItems(); ++i)
            if (devBox.getItemText (i) == cfg.deviceName) devBox.setSelectedItemIndex (i, juce::dontSendNotification);
        fillFormats();
    }

    void fillDevices()
    {
        devBox.clear (juce::dontSendNotification);
        if (auto* t = ctx.engine.findType (typeBox.getText()))
        {
            // No scan here (it froze the window): the lists are read at start and JUCE updates them on device changes.
            for (auto& n : t->getDeviceNames (false))
                devBox.addItem (n, devBox.getNumItems() + 1);
            const int def = t->getDefaultDeviceIndex (false);
            devBox.setSelectedItemIndex (juce::jmax (0, def), juce::dontSendNotification);
        }
        fillFormats();
        panel.setVisible (typeBox.getText() == "ASIO");
    }

    void fillFormats()
    {
        const auto cfg = ctx.engine.getMasterConfig();
        auto* dev = ctx.engine.getMasterDevice();
        const bool same = dev != nullptr && cfg.typeName == typeBox.getText() && cfg.deviceName == devBox.getText();

        juce::Array<double> rates = same ? dev->getAvailableSampleRates() : juce::Array<double> { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
        juce::Array<int> sizes = same ? dev->getAvailableBufferSizes() : juce::Array<int> { 16, 32, 48, 64, 96, 128, 192, 256, 384, 480, 512, 1024, 2048 };

        rateBox.clear (juce::dontSendNotification);
        for (auto r : rates) rateBox.addItem (juce::String ((int) r) + " Hz", (int) r);
        rateBox.setSelectedId ((int) (same ? dev->getCurrentSampleRate() : 48000.0), juce::dontSendNotification);
        if (rateBox.getSelectedId() == 0) rateBox.setSelectedItemIndex (0, juce::dontSendNotification);

        bufBox.clear (juce::dontSendNotification);
        bufBox.addItem ("Device default", 100000);
        const double sr = juce::jmax (1, rateBox.getSelectedId());
        for (auto s : sizes)
            bufBox.addItem (juce::String (s) + " samples  (" + juce::String (1000.0 * s / sr, 2) + " ms)", s);
        bufBox.setSelectedId (same ? dev->getCurrentBufferSizeSamples() : 100000, juce::dontSendNotification);
        if (bufBox.getSelectedId() == 0) bufBox.setSelectedId (100000, juce::dontSendNotification);
    }

    void applyConfig()
    {
        MasterConfig cfg;
        cfg.typeName = typeBox.getText();
        cfg.deviceName = devBox.getText();
        cfg.sampleRate = rateBox.getSelectedId() > 0 ? rateBox.getSelectedId() : 48000.0;
        cfg.bufferSize = bufBox.getSelectedId() == 100000 ? 0 : bufBox.getSelectedId();
        const auto err = ctx.engine.setMasterConfig (cfg);
        if (err.isNotEmpty())
            dialog::message (this, dialog::Kind::warning, "Audio device", err);
        loadCurrent();
        if (onChanged) onChanged();
    }

    Engine::LoadMeter loadMeter;

    void timerCallback() override
    {
        if (! isShowing()) return;
        auto& e = ctx.engine;
        juce::String s;
        if (auto* d = e.getMasterDevice())
            s << "Running: " << d->getName() << "  |  " << (int) e.getSampleRate() << " Hz  |  "
              << d->getCurrentBufferSizeSamples() << " samples\nIn + out latency ~" << juce::String (e.getMasterLatencyMs(), 2)
              << " ms  |  DSP load " << juce::String (loadMeter.update (e) * 100.0f, 1) << " % (peak "
              << juce::String (e.getPeakLoad() * 100.0f, 0) << " %)";
        else
            s << "Stopped. " << e.getLastError();
        status.setText (s, juce::dontSendNotification);
    }

    UiContext& ctx;
    std::function<void()> onChanged;
    juce::Label typeL { {}, "Driver" }, devL { {}, "Device" }, rateL { {}, "Sample rate" }, bufL { {}, "Buffer size" };
    juce::ComboBox typeBox, devBox, rateBox, bufBox;
    juce::TextButton apply { "Apply / Restart" }, panel { "ASIO Control Panel" };
    juce::Label status;
    Heading masterH, statusH;
};

//==============================================================================
class MidiTab final : public SettingsPage
{
public:
    explicit MidiTab (UiContext& c) : ctx (c)
    {
        addAndMakeVisible (outL); styleLabel (outL);
        addAndMakeVisible (outBox);
        addAndMakeVisible (refresh);
        addHeading (inH, "Controllers (MIDI inputs)",
                    "MIDI learn: right-click ANY control in the mixer and choose \"MIDI Learn\", "
                    "or enable MIDI Learn Mode in the Tools menu (wrench) and click controls one after another, moving a "
                    "hardware knob/fader/button after each click. Buttons toggle, faders/knobs track absolute "
                    "values (Pitch-Bend faders supported).");
        addHeading (feedbackH, "Feedback", "Sends the values back to the controller: LEDs light up, motor faders move.");
        addHeading (ledH, "LED feedback profile",
                    "Pad colours by velocity. Active = button on, Inactive = mapped but off, Unbound = no mapping, "
                    "Muted row = unbound pads in a row whose channel is muted. APC mini mk1: 0 off, 1 green, 3 red, "
                    "5 yellow (2 / 4 / 6 = blinking).");
        refresh.onClick = [this] { rebuild(); };
        outBox.onChange = [this]
        {
            ctx.midi.setFeedbackOutput (outBox.getSelectedId() <= 1 ? juce::String() : outBox.getText());
            loadLed(); // an APC mini selects its LED profile automatically
        };

        addAndMakeVisible (faderTop);
        faderTop.setToggleState (ctx.midi.getFaderTopAt0dB(), juce::dontSendNotification);
        faderTop.onClick = [this] { ctx.midi.setFaderTopAt0dB (faderTop.getToggleState()); };
        faderTop.setTooltip ("Applies to all channel, bus and send faders mapped to MIDI (now and when learned later). "
                             "Single faders can still be calibrated with right-click > MIDI Range.");

        addAndMakeVisible (ledL); styleLabel (ledL);
        addAndMakeVisible (ledBox);
        const auto presets = LedProfile::presets();
        for (int i = 0; i < (int) presets.size(); ++i) ledBox.addItem (presets[(size_t) i].name, i + 1);
        ledBox.addItem ("Custom", 100);
        ledBox.onChange = [this]
        {
            const auto presets = LedProfile::presets();
            const int i = ledBox.getSelectedId() - 1;
            if (juce::isPositiveAndBelow (i, (int) presets.size()))
            {
                ctx.midi.setLedProfile (presets[(size_t) i]);
                loadLed();
            }
        };
        static const char* names[] = { "First note", "Last note", "Row size", "Channel", "Active", "Inactive", "Unbound", "Muted row" };
        for (int i = 0; i < 8; ++i)
        {
            auto& f = fields[(size_t) i];
            f.caption.setText (names[i], juce::dontSendNotification);
            styleLabel (f.caption, true, 11.5f);
            f.caption.setJustificationType (juce::Justification::centred);
            f.value.setEditable (true, true, false);
            f.value.setJustificationType (juce::Justification::centred);
            f.value.setColour (juce::Label::backgroundColourId, theme::bg);
            f.value.setColour (juce::Label::outlineColourId, theme::outline);
            f.value.onTextChange = [this] { applyFields(); };
            addAndMakeVisible (f.caption);
            addAndMakeVisible (f.value);
        }
        rebuild();
        loadLed();
    }

    void loadLed()
    {
        const auto p = ctx.midi.getLedProfile();
        int id = 100;
        const auto presets = LedProfile::presets();
        for (int i = 0; i < (int) presets.size(); ++i) if (presets[(size_t) i].name == p.name) id = i + 1;
        ledBox.setSelectedId (id, juce::dontSendNotification);
        const int v[8] = { p.firstNote, p.lastNote, p.rowSize, p.channel, p.velOn, p.velOff, p.velUnbound, p.velMuted };
        for (int i = 0; i < 8; ++i)
        {
            fields[(size_t) i].value.setText (juce::String (v[i]), juce::dontSendNotification);
            fields[(size_t) i].value.setEnabled (p.grid);
        }
    }

    void applyFields()
    {
        auto p = ctx.midi.getLedProfile();
        auto val = [this] (int i, int lo, int hi) { return juce::jlimit (lo, hi, fields[(size_t) i].value.getText().getIntValue()); };
        p.firstNote = val (0, 0, 127); p.lastNote = val (1, 0, 127); p.rowSize = val (2, 1, 127); p.channel = val (3, 1, 16);
        p.velOn = val (4, 0, 127); p.velOff = val (5, 0, 127); p.velUnbound = val (6, 0, 127); p.velMuted = val (7, 0, 127);
        p.grid = true;
        p.name = "Custom";
        juce::Component::SafePointer<MidiTab> safe (this);
        juce::MessageManager::callAsync ([safe, p] { if (safe != nullptr) { safe->ctx.midi.setLedProfile (p); safe->loadLed(); } });
    }

    void rebuild()
    {
        toggles.clear();
        for (auto& info : ctx.midi.availableInputs())
        {
            auto t = std::make_unique<juce::ToggleButton> (info.name);
            t->setToggleState (ctx.midi.isInputEnabled (info.name), juce::dontSendNotification);
            auto name = info.name;
            auto* raw = t.get();
            t->onClick = [this, name, raw] { ctx.midi.setInputEnabled (name, raw->getToggleState()); };
            addAndMakeVisible (*t);
            toggles.push_back (std::move (t));
        }
        outBox.clear (juce::dontSendNotification);
        outBox.addItem ("None", 1);
        int id = 2;
        for (auto& info : ctx.midi.availableOutputs())
        {
            outBox.addItem (info.name, id);
            if (info.name == ctx.midi.getFeedbackOutput()) outBox.setSelectedId (id, juce::dontSendNotification);
            ++id;
        }
        if (outBox.getSelectedId() == 0) outBox.setSelectedId (1, juce::dontSendNotification);
        resized();
    }

    void resized() override
    {
        auto r = beginLayout();
        auto top = section (r, inH);
        refresh.setBounds (top.removeFromRight (110).withSizeKeepingCentre (110, 26));
        for (auto& t : toggles) option (r, *t);
        if (toggles.empty()) noDevices = r.removeFromTop (28);
        r.removeFromTop (4);
        option (r, faderTop);
        section (r, feedbackH);
        labelled (r, outL, outBox);
        section (r, ledH);
        labelled (r, ledL, ledBox);
        auto caps = r.removeFromTop (18).withTrimmedLeft (kLabelWidth);
        auto vals = r.removeFromTop (26).withTrimmedLeft (kLabelWidth);
        const int w = juce::jmin (78, caps.getWidth() / 8);
        for (auto& f : fields)
        {
            f.caption.setBounds (caps.removeFromLeft (w));
            f.value.setBounds (vals.removeFromLeft (w).reduced (4, 0));
        }
    }

    void paint (juce::Graphics& g) override
    {
        SettingsPage::paint (g);
        if (toggles.empty())
        {
            g.setColour (theme::textFaint);
            g.setFont (theme::font (12.5f));
            g.drawText ("No MIDI input devices found.", noDevices, juce::Justification::centredLeft);
        }
    }

private:
    UiContext& ctx;
    juce::Label outL { {}, "Output" };
    juce::ComboBox outBox;
    juce::TextButton refresh { "Refresh" };
    juce::ToggleButton faderTop { "MIDI faders stop at 0 dB (instead of +12 dB)" };
    Heading inH, feedbackH, ledH;
    juce::Rectangle<int> noDevices;
    std::vector<std::unique_ptr<juce::ToggleButton>> toggles;
    juce::Label ledL { {}, "Profile" };
    juce::ComboBox ledBox;
    struct NumField { juce::Label caption, value; };
    std::array<NumField, 8> fields;
};

//==============================================================================
class PluginTab final : public SettingsPage, private juce::Timer, private juce::Thread
{
public:
    explicit PluginTab (UiContext& c) : juce::Thread ("Plugin scan"), ctx (c)
    {
        addHeading (pathsH, "VST3 search folders", "One folder per line. Scanning looks for VST3 plugins in these folders "
                                                   "and their subfolders. Plugins that crash during a scan are blacklisted automatically.");
        addAndMakeVisible (paths);
        paths.setMultiLine (true);
        paths.setReturnKeyStartsNewLine (true);
        paths.setFont (theme::font (13.0f));
        paths.setText (ctx.engine.pluginHost.getSearchPaths().joinIntoString ("\n"));
        addAndMakeVisible (scan);
        addAndMakeVisible (bar);
        addAndMakeVisible (status); styleLabel (status);
        scan.onClick = [this]
        {
            if (isThreadRunning()) { cancel = true; return; }
            ctx.engine.pluginHost.setSearchPaths (juce::StringArray::fromLines (paths.getText()));
            cancel = false;
            progress = 0.0;
            startThread();
            startTimerHz (10);
            scan.setButtonText ("Cancel");
        };
        updateStatus();
    }

    ~PluginTab() override { cancel = true; stopThread (10000); }

    void resized() override
    {
        auto r = beginLayout();
        section (r, pathsH);
        paths.setBounds (r.removeFromTop (110).withWidth (juce::jmin (kMaxWidth, r.getWidth())));
        r.removeFromTop (12);
        auto row = r.removeFromTop (32);
        scan.setBounds (row.removeFromLeft (160));
        row.removeFromLeft (12);
        bar.setBounds (row.withHeight (22).withY (row.getY() + 5).withWidth (juce::jmin (kMaxWidth - 172, row.getWidth())));
        r.removeFromTop (10);
        status.setBounds (r.removeFromTop (40));
    }

private:
    void run() override
    {
        ctx.engine.pluginHost.scan ([this] (float p, const juce::String& name)
        {
            progress = p;
            const juce::ScopedLock sl (nameLock);
            current = name;
        }, cancel);
    }

    void timerCallback() override
    {
        {
            const juce::ScopedLock sl (nameLock);
            status.setText ("Scanning: " + current, juce::dontSendNotification);
        }
        if (! isThreadRunning())
        {
            stopTimer();
            scan.setButtonText ("Scan for plugins");
            progress = 1.0;
            updateStatus();
        }
    }

    void updateStatus()
    {
        status.setText (juce::String (ctx.engine.pluginHost.known.getNumTypes()) + " plugins known. Plugins that crash "
                        "during a scan are blacklisted automatically.", juce::dontSendNotification);
    }

    UiContext& ctx;
    Heading pathsH;
    juce::TextEditor paths;
    juce::TextButton scan { "Scan for plugins" };
    double progress = 0.0;
    juce::ProgressBar bar { progress };
    juce::Label status;
    std::atomic<bool> cancel { false };
    juce::CriticalSection nameLock;
    juce::String current;
};

//==============================================================================
/** Import of a Voicemeeter settings XML (part of the Backup & Transfer tab). */
class VoicemeeterSection final : public juce::Component
{
public:
    VoicemeeterSection (UiContext& c, std::function<void()> changed) : ctx (c), onChanged (std::move (changed))
    {
        addAndMakeVisible (title);
        title.setText ("IMPORT FROM VOICEMEETER", juce::dontSendNotification);
        title.setFont (theme::font (12.0f, true));
        title.setColour (juce::Label::textColourId, theme::textDim);
        title.setBorderSize ({});
        addAndMakeVisible (hint);
        hint.setText ("Import from Voicemeeter\nReads a Voicemeeter (Potato) settings XML (Voicemeeter: Menu -> Save Settings) and takes over "
                      "devices, ASIO channel patches, names, routing, faders, EQ, gate, compressor, denoiser and the "
                      "MIDI mapping.\n\nClose Voicemeeter first so the ASIO driver is free. Your current Hydra settings are replaced.");
        addAndMakeVisible (files);
        addAndMakeVisible (browse);
        addAndMakeVisible (import);
        addAndMakeVisible (report);
        report.setMultiLine (true);
        report.setReadOnly (true);
        report.setFont (theme::mono (12.0f));

        // Looking through Documents can take long (OneDrive): in the background, the window opens at once.
        files.setTextWhenNothingSelected ("Looking for Voicemeeter settings in Documents...");
        juce::Component::SafePointer<VoicemeeterSection> safe (this);
        juce::Thread::launch ([safe]
        {
            auto found = VoicemeeterImport::findCandidates();
            juce::StringArray names;
            for (auto& f : found) names.add (f.getFileName() + "   (" + f.getLastModificationTime().formatted ("%d.%m.%Y %H:%M") + ")");
            juce::MessageManager::callAsync ([safe, found, names]
            {
                if (safe != nullptr) safe->addFound (found, names);
            });
        });

        browse.onClick = [this]
        {
            chooser = std::make_unique<juce::FileChooser> ("Voicemeeter settings", juce::File::getSpecialLocation (juce::File::userDocumentsDirectory), "*.xml");
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc)
            {
                const auto f = fc.getResult();
                if (! f.existsAsFile()) return;
                candidates.add (f);
                files.addItem (f.getFileName(), candidates.size());
                files.setSelectedId (candidates.size(), juce::dontSendNotification);
            });
        };

        import.onClick = [this]
        {
            const int i = files.getSelectedId() - 1;
            if (! juce::isPositiveAndBelow (i, candidates.size())) return;
            if (ctx.session != nullptr) { ctx.session->saveDefault(); ctx.session->backup ("before-voicemeeter-import"); }
            const auto result = VoicemeeterImport::run (candidates[i], ctx.engine, ctx.midi, true);
            report.setText (result.report.joinIntoString ("\n"), false);
            report.setVisible (true);
            if (onChanged) onChanged();
        };
        report.setVisible (false);   // shown after an import
    }

    void resized() override
    {
        auto r = getLocalBounds();
        title.setBounds (r.removeFromTop (22));
        placeInfoAfter (hint, title);
        r.removeFromTop (8);
        auto row = r.removeFromTop (30);
        import.setBounds (row.removeFromRight (120));
        row.removeFromRight (8);
        browse.setBounds (row.removeFromRight (100));
        row.removeFromRight (8);
        files.setBounds (row);
        r.removeFromTop (12);
        report.setBounds (r);
    }

private:
    void addFound (const juce::Array<juce::File>& found, const juce::StringArray& names)
    {
        const bool hadSelection = files.getSelectedId() != 0;   // a file chosen with Browse meanwhile stays selected
        for (int i = 0; i < found.size(); ++i)
        {
            candidates.add (found[i]);
            files.addItem (names[i], candidates.size());
        }
        if (! hadSelection && ! found.isEmpty()) files.setSelectedId (candidates.size() - found.size() + 1, juce::dontSendNotification);
        if (candidates.isEmpty()) files.setTextWhenNothingSelected ("No Voicemeeter XML found in Documents - use Browse");
    }

    UiContext& ctx;
    std::function<void()> onChanged;
    juce::Label title;
    InfoButton hint;
    juce::ComboBox files;
    juce::TextButton browse { "Browse..." }, import { "Import" };
    juce::TextEditor report;
    juce::Array<juce::File> candidates;
    std::unique_ptr<juce::FileChooser> chooser;
};

//==============================================================================
/** Export / import of the complete settings, restore of automatic backups and the Voicemeeter import. */
class BackupTab final : public SettingsPage
{
public:
    BackupTab (UiContext& c, std::function<void()> replaced, std::function<void()> engineChanged)
        : ctx (c), onReplaced (std::move (replaced)),
          voicemeeter (c, [this, engineChanged]
          {
              reloadList();   // the import made a backup first
              if (engineChanged) engineChanged();
          })
    {
        addAndMakeVisible (voicemeeter);
        addAndMakeVisible (status); styleLabel (status); status.setJustificationType (juce::Justification::topLeft);
        addHeading (exportH, "Export / Import Settings",
                    "Saves everything (devices, routing, names, faders, EQ, dynamics, denoiser, plugins, MIDI map, "
                    "keyboard shortcuts) to one file, e.g. to move it to another PC or keep it safe before an update.");
        addHeading (backupH, "Automatic Backups",
                    "Hydra keeps a copy of your settings at every start and before every import (newest "
                    + juce::String (Session::kMaxBackups) + "). An unreadable settings file is never overwritten.");
        addHeading (vmH, {});   // only the line above the Voicemeeter import (it has its own title)
        for (auto* b : { &exportButton, &importButton, &restoreButton, &folderButton }) addAndMakeVisible (b);
        addAndMakeVisible (backups);

        exportButton.onClick = [this] { exportSettings(); };
        importButton.onClick = [this]
        {
            chooser = std::make_unique<juce::FileChooser> ("Import Hydra settings", juce::File::getSpecialLocation (juce::File::userDocumentsDirectory), "*.xml");
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this] (const juce::FileChooser& fc) { if (fc.getResult().existsAsFile()) confirmAndLoad (fc.getResult()); });
        };
        restoreButton.onClick = [this]
        {
            const int i = backups.getSelectedId() - 1;
            if (juce::isPositiveAndBelow (i, files.size())) confirmAndLoad (files[i]);
        };
        folderButton.onClick = [this]
        {
            if (ctx.session != nullptr) { ctx.session->backupDir().createDirectory(); ctx.session->backupDir().startAsProcess(); }
        };
        reloadList();
    }

    void visibilityChanged() override { if (isVisible()) reloadList(); }

    void resized() override
    {
        auto r = beginLayout();
        section (r, exportH);
        auto row = r.removeFromTop (30);
        exportButton.setBounds (row.removeFromLeft (180));
        row.removeFromLeft (10);
        importButton.setBounds (row.removeFromLeft (180));
        section (r, backupH);
        row = r.removeFromTop (30).withWidth (juce::jmin (kMaxWidth + 100, r.getWidth()));
        folderButton.setBounds (row.removeFromRight (130));
        row.removeFromRight (8);
        restoreButton.setBounds (row.removeFromRight (120));
        row.removeFromRight (8);
        backups.setBounds (row);
        r.removeFromTop (6);
        status.setBounds (r.removeFromTop (36));
        // The Voicemeeter import has its own title: only the line above it (the section's title row is skipped).
        section (r, vmH);
        voicemeeter.setBounds (r.withTop (r.getY() - 30).withWidth (juce::jmin (kMaxWidth + 100, r.getWidth())));
    }

private:
    void reloadList()
    {
        backups.clear (juce::dontSendNotification);
        files.clear();
        if (ctx.session == nullptr) return;
        for (auto& f : ctx.session->listBackups())
        {
            files.add (f);
            // session_2026-10-13_20-30-05-123_start.xml -> "13.10.2026  20:30:05      start"
            const auto n = f.getFileNameWithoutExtension().fromFirstOccurrenceOf ("session_", false, false);
            const auto d = n.substring (0, 10), t = n.substring (11, 19).replaceCharacter ('-', ':');
            const auto reason = n.fromFirstOccurrenceOf (n.substring (11, 19), false, false).fromFirstOccurrenceOf ("_", false, false);
            backups.addItem (d.substring (8, 10) + "." + d.substring (5, 7) + "." + d.substring (0, 4) + "  " + t
                             + "      " + reason.replaceCharacter ('-', ' '), files.size());
        }
        if (files.isEmpty()) backups.setTextWhenNothingSelected ("No backups yet");
        else backups.setSelectedId (1, juce::dontSendNotification);
        restoreButton.setEnabled (! files.isEmpty());
    }

    void exportSettings()
    {
        const auto name = "Hydra Settings " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d") + ".xml";
        chooser = std::make_unique<juce::FileChooser> ("Export Hydra settings",
                                                       juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile (name), "*.xml");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult();
            if (f == juce::File() || ctx.session == nullptr) return;
            if (! f.hasFileExtension ("xml")) f = f.withFileExtension ("xml");
            const bool ok = ctx.session->exportTo (f);
            status.setText (ok ? "Exported to " + f.getFullPathName() : "Export FAILED: cannot write " + f.getFullPathName(),
                            juce::dontSendNotification);
        });
    }

    void confirmAndLoad (const juce::File& f)
    {
        if (ctx.session == nullptr) return;
        if (! Session::isSessionFile (f))
        {
            status.setText (f.getFileName() + " is not a Hydra Mixer settings file - nothing changed.", juce::dontSendNotification);
            return;
        }
        juce::Component::SafePointer<BackupTab> safe (this);
        dialog::confirm (this, dialog::Kind::warning, "Load settings?",
            "Replace all current settings with\n" + f.getFullPathName() + "\n\nYour current settings are backed up first.",
            "Load", [safe, f]
            {
                if (safe == nullptr) return;
                juce::String err;
                const bool ok = safe->ctx.session->importFrom (f, err);
                safe->status.setText (ok ? "Loaded " + f.getFileName() + "  (previous settings are in the backup list)" : err,
                                      juce::dontSendNotification);
                safe->reloadList();
                if (ok && safe->onReplaced) safe->onReplaced();
            });
    }

    UiContext& ctx;
    std::function<void()> onReplaced;
    juce::Label status;
    Heading exportH, backupH, vmH;
    juce::TextButton exportButton { "Export Settings..." }, importButton { "Import Settings..." },
                     restoreButton { "Restore" }, folderButton { "Open Folder" };
    juce::ComboBox backups;
    juce::Array<juce::File> files;
    std::unique_ptr<juce::FileChooser> chooser;
    VoicemeeterSection voicemeeter;
};

//==============================================================================
class CablesTab final : public SettingsPage, private juce::Timer
{
public:
    CablesTab (UiContext& c, std::function<void()> changed) : ctx (c), onChanged (std::move (changed))
    {
        addHeading (vacH, "Virtual Audio Cable",
                    "Hydra sets up Virtual Audio Cable itself: each input or bus that uses a virtual cable gets one of its "
                    "own (device picker > Virtual Cable). Adding or removing cables restarts the driver.", false);
        addHeading (cablesH, "Detected virtual cables",
                    "Software inputs without a driver of our own:\n"
                    "  1) Per-app capture: choose \"Application\" in a strip's source menu (e.g. Spotify.exe). "
                    "Works with any app, no install. To avoid hearing the app twice, set its output in "
                    "Windows (Settings > Sound > Volume mixer) to a device you don't listen to.\n"
                    "  2) Virtual cables (signed drivers, Secure Boot stays on): free VB-Cable (vb-audio.com/Cable, "
                    "up to 5 cables with A+B / C+D) or Virtual Audio Cable (vac.muzychenko.net, any number; Hydra adds cables itself). "
                    "Apps play into the cable's playback side, Hydra records the other side. For V buses it's reversed.");
        addHeading (renameH, "Device names in Windows",
                    "Windows and your apps then list the device a software input listens to under the channel's name: "
                    "an input \"Chrome\" that records a loopback of \"Voicemeeter In 4\" turns that device into "
                    "\"Hydra Chrome\" (unnamed: \"Hydra In 4\"). For a virtual cable the side apps use is renamed, "
                    "for V buses the side apps record from (\"Hydra V1\"). Hardware devices keep their names. "
                    "Windows asks for administrator rights (once, see below). Original names come back when a "
                    "channel no longer uses the device or this is switched off.");
        addAndMakeVisible (list);
        list.setMultiLine (true);
        list.setReadOnly (true);
        list.setFont (theme::mono (12.0f));
        addAndMakeVisible (refresh);
        addAndMakeVisible (assign);
        refresh.onClick = [this] { ctx.engine.rescanDevices(); update(); };
        assign.onClick = [this] { autoAssign(); };
        assign.setTooltip ("Gives every input / V bus that still points to a 'Hydra Virtual' placeholder the next free cable");

        // Virtual Audio Cable: Hydra sets the number of cables itself (VacCables)
        addChildComponent (vacState); styleLabel (vacState);
        vacState.setJustificationType (juce::Justification::topLeft);
        addChildComponent (vacAdd);
        addChildComponent (vacRemove);
        addChildComponent (vacManage);
        addChildComponent (vacDirect);
        vacDirect.setButtonText ("Connect cables directly (Kernel Streaming, cable clock synced to the master)");
        vacDirect.setTooltip ("Hydra reads / writes Virtual Audio Cable's driver buffer itself instead of going through Windows, "
                              "and keeps the cable's clock in step with the master: no resampling, less latency and CPU. "
                              "Apps keep using the cable as usual. Off: WASAPI like any other Windows device.");
        vacDirect.setToggleState (ctx.session == nullptr || ctx.session->prefs.vacDirect, juce::dontSendNotification);
        addChildComponent (vacHide);
        vacHide.setButtonText ("Hide the side Hydra uses from other apps");
        vacHide.setTooltip ("An input on \"Line 1\" records the cable's recording side: Windows then lists Line 1 only as a "
                            "playback device, so apps can only play into it (a bus: only as a recording device). Needs the direct "
                            "connection. Shown again when no channel uses the cable any more, or when this is switched off.");
        vacHide.setToggleState (ctx.session == nullptr || ctx.session->prefs.vacHideOwnSide, juce::dontSendNotification);
        addChildComponent (vacHideUnused);
        vacHideUnused.setButtonText ("Hide unused cables until a channel uses them");
        vacHideUnused.setTooltip ("Cables no channel of Hydra uses are hidden in Windows on both sides, so apps only see the "
                                  "cables Hydra has set up. A cable an app is streaming on right now stays visible. Hydra's "
                                  "device picker still lists them. Needs the direct connection.");
        vacHideUnused.setToggleState (ctx.session == nullptr || ctx.session->prefs.vacHideUnused, juce::dontSendNotification);
        addChildComponent (vacAutoName);
        vacAutoName.setButtonText ("Name cables after their channel (\"Hydra In 2\", \"Hydra Out 1\")");
        vacAutoName.setTooltip ("A cable an input records is called \"Hydra In <input number>\" in Windows, a cable a bus plays "
                                "into \"Hydra Out <V number>\" (a hardware bus: \"Hydra Out H1\"). It gets its old name back "
                                "when no channel uses it any more. Renaming needs administrator rights (see \"Rename without asking\" below).");
        vacAutoName.setToggleState (ctx.session == nullptr || ctx.session->prefs.vacAutoName, juce::dontSendNotification);
        auto bindPref = [this] (juce::ToggleButton& b, bool Session::AppPrefs::* field)
        {
            b.onClick = [this, &b, field]
            {
                if (ctx.session == nullptr) return;
                ctx.session->prefs.*field = b.getToggleState();
                ctx.session->prefsVersion.fetch_add (1);
                if (ctx.prefsChanged) ctx.prefsChanged();
            };
        };
        bindPref (vacHide, &Session::AppPrefs::vacHideOwnSide);
        bindPref (vacHideUnused, &Session::AppPrefs::vacHideUnused);
        bindPref (vacAutoName, &Session::AppPrefs::vacAutoName);
        vacDirect.onClick = [this]
        {
            if (ctx.session == nullptr) return;
            ctx.session->prefs.vacDirect = vacDirect.getToggleState();
            ctx.session->prefsVersion.fetch_add (1);
            if (ctx.prefsChanged) ctx.prefsChanged();
        };
        vacManage.onClick = [this] { if (ctx.openCableManager) ctx.openCableManager(); };
        vacAdd.setTooltip ("Adds one cable (\"Line N (Virtual Audio Cable)\"). The driver restarts: all cables are gone for a "
                           "moment, so apps using one may need to reconnect. Windows asks for administrator rights.");
        vacRemove.setTooltip ("Removes the cables at the end that no channel uses and no app is playing / recording on.");
        auto report = [this] (bool, const juce::String& msg) { update (msg); if (onChanged) onChanged(); };
        vacAdd.onClick = [this, report] { if (auto* v = vac()) v->setCount (v->info().numCables + 1, report); };
        vacRemove.onClick = [this, report] { if (auto* v = vac()) v->removeUnused (report); };

        // Device names in Windows (DeviceNamer)
        addAndMakeVisible (renameToggle);
        renameToggle.setButtonText ("Name software devices in Windows after their channels");
        renameToggle.setToggleState (ctx.session != nullptr && ctx.session->prefs.renameDevices, juce::dontSendNotification);
        renameToggle.onClick = [this]
        {
            if (ctx.session == nullptr) return;
            ctx.session->prefs.renameDevices = renameToggle.getToggleState();
            ctx.session->prefsVersion.fetch_add (1);
            if (ctx.prefsChanged) ctx.prefsChanged();
        };
        addAndMakeVisible (renameSilent);
        renameSilent.setButtonText ("Rename without asking each time (Windows asks once to set this up)");
        renameSilent.setTooltip ("Renaming a device needs administrator rights. With this on, the first rename also sets up a "
                                 "Windows task (\"Hydra Mixer Device Names\") that Hydra starts for later renames, so Windows "
                                 "does not ask again. The task only renames audio devices; it starts this copy of Hydra Mixer "
                                 "with administrator rights. Off: the task is removed and Windows asks each time.");
        renameSilent.setToggleState (ctx.session == nullptr || ctx.session->prefs.renameNoPrompt, juce::dontSendNotification);
        renameSilent.onClick = [this]
        {
            if (ctx.session == nullptr) return;
            ctx.session->prefs.renameNoPrompt = renameSilent.getToggleState();
            ctx.session->prefsVersion.fetch_add (1);
            if (! ctx.session->prefs.renameNoPrompt && ctx.session->deviceNamer != nullptr) ctx.session->deviceNamer->removeTask();
            if (ctx.prefsChanged) ctx.prefsChanged();
        };
        addAndMakeVisible (renameStatus);
        renameStatus.setMultiLine (true);
        renameStatus.setReadOnly (true);
        renameStatus.setFont (theme::mono (12.0f));
        update();
        updateVac();
        startTimer (500);
    }

    void resized() override
    {
        // Virtual Audio Cable (when installed) | detected cables | device names in Windows
        auto r = beginLayout();
        const int width = juce::jmin (kMaxWidth + 160, r.getWidth());
        if (vacState.isVisible())
        {
            section (r, vacH);
            auto v = r.removeFromTop (36);
            vacManage.setBounds (v.removeFromRight (140).withSizeKeepingCentre (140, 30));
            v.removeFromRight (8);
            vacRemove.setBounds (v.removeFromRight (190).withSizeKeepingCentre (190, 30));
            v.removeFromRight (8);
            vacAdd.setBounds (v.removeFromRight (120).withSizeKeepingCentre (120, 30));
            v.removeFromRight (12);
            vacState.setBounds (v);
            r.removeFromTop (6);
            for (auto* b : { &vacDirect, &vacHide, &vacHideUnused, &vacAutoName }) option (r, *b);
        }
        auto top = section (r, cablesH);
        assign.setBounds (top.removeFromRight (180).withSizeKeepingCentre (180, 26));
        top.removeFromRight (8);
        refresh.setBounds (top.removeFromRight (100).withSizeKeepingCentre (100, 26));
        // What is left is shared by the two text boxes (cable list, renamed devices).
        const int fixed = 14 + 1 + 14 + 30 + 28 + 28 + 6;
        const int boxes = juce::jmax (100, r.getHeight() - fixed);
        list.setBounds (r.removeFromTop (boxes * 55 / 100).withWidth (width));
        section (r, renameH);
        option (r, renameToggle);
        option (r, renameSilent);
        r.removeFromTop (6);
        renameStatus.setBounds (r.withWidth (width));
    }

private:
    VacCables* vac() const { return ctx.session != nullptr ? ctx.session->vacCables : nullptr; }

    void timerCallback() override
    {
        // Asks the VAC driver every 2 s: only while this page is on screen (the settings window stays alive when closed).
        if (! isShowing()) return;
        UiActivity busy ("settings: cables page refresh");
        const auto t = ctx.session != nullptr && ctx.session->deviceNamer != nullptr ? ctx.session->deviceNamer->describe() : juce::String();
        if (t != renameStatus.getText()) renameStatus.setText (t, false);
        updateVac();
    }

    void updateVac()
    {
        auto* v = vac();
        if (v != nullptr && ! v->isBusy() && ++vacTick % 4 == 0) v->refresh();   // every 2 s
        const bool shown = v != nullptr && v->isAvailable();
        if (shown != vacState.isVisible())
        {
            for (auto* c : { (juce::Component*) &vacState, (juce::Component*) &vacAdd, (juce::Component*) &vacRemove, (juce::Component*) &vacManage, (juce::Component*) &vacDirect, (juce::Component*) &vacHide,
                             (juce::Component*) &vacHideUnused, (juce::Component*) &vacAutoName })
                c->setVisible (shown);
            showHeading (vacH, shown);
            resized();
            repaint();
        }
        if (! shown) return;
        const auto& i = v->info();
        if (i.numCables != shownCables && ! v->isBusy()) { shownCables = i.numCables; update(); }   // changed in the Cable Manager too
        juce::String t;
        t << i.numCables << (i.numCables == 1 ? " cable" : " cables") << " - version " << i.version << " (" << i.edition << ")";
        if (i.restartPending) t << " - restart pending";
        if (const auto st = v->status(); st.isNotEmpty()) t << juce::newLine << st;
        if (t != vacState.getText()) vacState.setText (t, juce::dontSendNotification);
        vacAdd.setEnabled (! v->isBusy() && i.edition != "lite" && i.numCables < 256);
        vacRemove.setEnabled (! v->isBusy() && i.numCables > 1);
    }

    void update (const juce::String& extra = {})
    {
        juce::String t;
        const auto cables = findVirtualCables (ctx.engine.getDeviceTypes());
        t << "Detected virtual cables: " << (int) cables.size() << "\n";
        for (auto& c : cables)
            t << "  " << c.label.paddedRight (' ', 18) << "  playback: " << c.playbackName << "\n"
              << "  " << juce::String().paddedRight (' ', 18) << "  recording: " << c.recordingName << "\n";
        if (cables.empty()) t << "  (none - install VB-Cable or Virtual Audio Cable, then press Refresh)\n";
        if (extra.isNotEmpty()) t << "\n" << extra;
        list.setText (t, false);
    }

    void autoAssign()
    {
        auto& eng = ctx.engine;
        const auto cables = findVirtualCables (eng.getDeviceTypes());
        auto inUse = [&] (const VirtualCable& c)
        {
            for (auto& s : eng.inputs) if (s.source.deviceName == c.recordingName || s.source.deviceName == c.playbackName) return true;
            for (auto& b : eng.buses)  if (b.output.deviceName == c.recordingName || b.output.deviceName == c.playbackName) return true;
            return false;
        };
        auto nextFree = [&] () -> const VirtualCable*
        {
            for (auto& c : cables) if (! inUse (c)) return &c;
            return nullptr;
        };

        juce::StringArray rep;
        for (int i = 0; i < kMaxInputs; ++i)
        {
            auto& s = eng.inputs[(size_t) i];
            if (! eng.isInputActive (i) || s.source.kind != IoTarget::Kind::Virtual) continue;
            const auto* c = nextFree();
            if (c == nullptr) { rep.add ("In " + juce::String (i + 1) + " \"" + s.name + "\": no free cable left"); continue; }
            IoTarget t; t.kind = IoTarget::Kind::Device; t.typeName = "Windows Audio (Low Latency Mode)"; t.deviceName = c->recordingName;
            eng.setInputSource (i, t);
            rep.add ("In " + juce::String (i + 1) + " \"" + s.name + "\": apps should play to \"" + c->playbackName + "\"");
        }
        for (int b = kMaxHwBuses; b < kMaxBuses; ++b)
        {
            auto& bus = eng.buses[(size_t) b];
            if (! eng.isBusActive (b) || bus.output.kind != IoTarget::Kind::Virtual) continue;
            const auto* c = nextFree();
            if (c == nullptr) { rep.add (busLabel (b) + ": no free cable left"); continue; }
            IoTarget t; t.kind = IoTarget::Kind::Device; t.typeName = "Windows Audio (Low Latency Mode)"; t.deviceName = c->playbackName;
            eng.setBusOutput (b, t);
            rep.add (busLabel (b) + ": apps (Discord, OBS ...) record from \"" + c->recordingName + "\"");
        }
        if (rep.isEmpty()) rep.add ("Nothing to assign: no input / V bus points to a 'Hydra Virtual' placeholder.");
        update (rep.joinIntoString ("\n"));
        if (onChanged) onChanged();
    }

    UiContext& ctx;
    std::function<void()> onChanged;
    Heading vacH, cablesH, renameH;
    juce::TextEditor list;
    juce::ToggleButton renameToggle, renameSilent;
    juce::TextEditor renameStatus;
    juce::TextButton refresh { "Refresh" }, assign { "Auto-assign cables" };
    juce::Label vacState;
    juce::TextButton vacAdd { "+ Add cable" }, vacRemove { "Remove unused cables" }, vacManage { "Manage cables..." };
    juce::ToggleButton vacDirect, vacHide, vacHideUnused, vacAutoName;
    int vacTick = 0, shownCables = -1;
};

//==============================================================================
/** Hydra Link pairing: the connected devices (Remove) and the ones found in the network (Connect). */
class LinkDevices final : public juce::Component
{
public:
    static constexpr int kRow = 40;

    /** Rebuilds the rows when something changed. True if the number of rows changed (the page lays out again). */
    bool refresh()
    {
        auto& hub = net::Hub::get();
        std::vector<Row> next;
        const auto peers = hub.isRunning() ? hub.getPeers() : std::vector<net::PeerInfo>();
        const bool openMode = hub.getConfig().password.isNotEmpty();
        auto kindText = [] (net::DeviceKind k) { return k == net::DeviceKind::phone ? juce::String ("Phone") : juce::String ("Computer"); };
        for (auto& t : hub.getTrusted())
        {
            Row r { t.name, {}, t.id, 2, false };
            auto on = std::find_if (peers.begin(), peers.end(), [&] (const net::PeerInfo& p) { return p.deviceId == t.id; });
            r.online = on != peers.end();
            r.detail = kindText (t.kind) + ", connected" + (r.online ? " - " + on->address : juce::String (" - not on the network right now"));
            next.push_back (r);
        }
        for (auto& p : peers)
        {
            if (p.name.isEmpty() || (p.deviceId != 0 && hub.isTrusted (p.deviceId))) continue;
            Row r { p.name, {}, p.deviceId, 1, true };
            if (p.deviceId == 0)    { r.action = 0; r.detail = "an older Hydra: update it to connect"; }
            else if (openMode)      { r.action = 0; r.detail = kindText (p.kind) + " - " + p.address + ", connected by the network password"; }
            else switch (p.pairing)
            {
                case net::PairState::asking:   r.action = 3; r.detail = "Waiting for the answer on " + p.name + " ..."; break;
                case net::PairState::denied:   r.detail = "Denied on " + p.name + " (it can be asked again in a few minutes)"; break;
                case net::PairState::noAnswer: r.detail = "No answer from " + p.name; break;
                default:                       r.detail = kindText (p.kind) + " - " + p.address + ", not connected"; break;
            }
            next.push_back (r);
        }
        if (next == rows) return false;
        const bool countChanged = next.size() != rows.size();
        rows = std::move (next);
        buttons.clear();
        for (auto& r : rows)
        {
            auto* b = buttons.add (new juce::TextButton (r.action == 2 ? "Remove" : r.action == 3 ? "Asking ..." : "Connect"));
            addChildComponent (b);
            b->setVisible (r.action != 0);
            b->setEnabled (r.action == 1 || r.action == 2);
            const auto id = r.id;
            const auto name = r.name;
            if (r.action == 1) b->onClick = [name] { net::Hub::get().requestPairing (name); };
            if (r.action == 2)
                b->onClick = [this, id, name]
                {
                    dialog::confirm (this, dialog::Kind::warning, "Remove " + name + "?",
                                     name + " can no longer send audio to this Hydra or get audio from it (it is told so). "
                                     "Connecting again needs a new confirmation.",
                                     "Remove", [id] { net::Hub::get().forget (id); });
                };
        }
        resized();
        repaint();
        return countChanged;
    }

    int wantedHeight() const { return rows.empty() ? 30 : (int) rows.size() * kRow; }

    void paint (juce::Graphics& g) override
    {
        if (rows.empty())
        {
            g.setColour (theme::textDim);
            g.setFont (theme::font (12.5f));
            g.drawText (net::Hub::get().isRunning() ? "No other Hydra or phone found in the network yet."
                                                    : "Turn Hydra Link on to find other devices.",
                        getLocalBounds(), juce::Justification::centredLeft);
            return;
        }
        for (size_t i = 0; i < rows.size(); ++i)
        {
            auto r = juce::Rectangle<int> (0, (int) i * kRow, getWidth(), kRow);
            g.setColour ((i & 1) ? theme::panel : theme::panel2);
            g.fillRect (r);
            r = r.reduced (10, 3);
            const auto& row = rows[i];
            g.setColour (row.action == 2 && row.online ? theme::good : theme::textFaint);
            g.fillEllipse (r.removeFromLeft (8).withSizeKeepingCentre (8, 8).toFloat());
            r.removeFromLeft (10);
            r.removeFromRight (110);
            g.setColour (theme::text);
            g.setFont (theme::font (13.0f, true));
            g.drawText (row.name, r.withHeight (r.getHeight() / 2 + 2), juce::Justification::bottomLeft, true);
            g.setColour (theme::textDim);
            g.setFont (theme::font (11.5f));
            g.drawText (row.detail, r.withTrimmedTop (r.getHeight() / 2 + 2), juce::Justification::topLeft, true);
        }
    }

    void resized() override
    {
        for (int i = 0; i < buttons.size(); ++i)
            buttons[i]->setBounds (getWidth() - 106, i * kRow + 7, 96, kRow - 14);
    }

private:
    struct Row
    {
        juce::String name, detail;
        uint64_t id = 0;
        int action = 0;   // 0 none, 1 connect, 2 remove, 3 asking
        bool online = false;
        bool operator== (const Row&) const = default;
    };
    std::vector<Row> rows;
    juce::OwnedArray<juce::TextButton> buttons;
};

//==============================================================================
/** Hydra Link: audio between computers (engine/NetLink.h). */
class NetworkTab final : public SettingsPage, private juce::Timer
{
public:
    explicit NetworkTab (UiContext& c) : ctx (c)
    {
        addHeading (linkH, "Hydra Link",
                    "Sends and receives audio between Hydras on different computers. On the sending computer choose "
                    "\"Network\" as the output of a V bus, on the receiving one \"Network\" as the source of an input: every "
                    "stream the computer hears is listed there.\n\n"
                    "In the local network the audio goes uncompressed (lowest latency, ~2.3 Mbit/s per stream). Beyond it "
                    "(internet, VPN) Opus compresses it to ~128 kbit/s. Windows may ask once whether Hydra may use the network.");
        addHeading (secH, "Password",
                    "Without a password Hydra Link only talks to computers in the local network (home / studio network) and "
                    "the audio is not encrypted.\n\n"
                    "With a password everything is encrypted (AES-256) and only Hydras with the same password hear each other. "
                    "This is needed for anything outside the local network. Use the same password on all your computers.");
        addHeading (peersH, "Other computers",
                    "Hydras in the same local network find each other by themselves. Others (over a VPN such as Tailscale or "
                    "ZeroTier, or the internet) need their address here, one per line: \"100.101.102.103\" or "
                    "\"studio.example.com:6980\". Over the internet the receiving side must forward the UDP port in its router "
                    "(a VPN needs no forwarding).");
        addHeading (devicesH, "Devices",
                    "Other Hydras and phones (Hydra Link app) connect once: one side asks, the other confirms, both remember "
                    "it. Audio is only taken from connected devices. Choosing a Hydra as the output of a V bus asks it by "
                    "itself; \"Connect\" asks it right away. A phone that is allowed can also control the mixer.\n\n"
                    "With a network password every Hydra that has it counts as connected.");
        addHeading (heardH, "On the network");
        addAndMakeVisible (devices);

        addAndMakeVisible (enabled);
        enabled.setButtonText ("Send and receive audio over the network");
        enabled.setToggleState (prefs().netEnabled, juce::dontSendNotification);
        enabled.onClick = [this] { prefs().netEnabled = enabled.getToggleState(); changed(); };

        for (auto* l : { &nameL, &portL, &passwordL }) { addAndMakeVisible (l); styleLabel (*l); }
        addAndMakeVisible (status);
        styleLabel (status, false);

        auto field = [this] (juce::TextEditor& e, const juce::String& text, std::function<void()> commit)
        {
            addAndMakeVisible (e);
            e.setText (text, false);
            e.onReturnKey = commit;
            e.onFocusLost = commit;
        };
        field (name, prefs().netName, [this]
        {
            const auto v = name.getText().trim();
            if (v != prefs().netName) { prefs().netName = v; changed(); }
        });
        name.setTextToShowWhenEmpty (net::Hub::computerName(), theme::textDim);
        name.setTooltip ("How the other computers list this one (empty: the Windows computer name)");
        field (port, prefs().netPort != net::kDefaultPort ? juce::String (prefs().netPort) : juce::String(), [this]
        {
            const int v = port.getText().trim().isEmpty() ? net::kDefaultPort : juce::jlimit (1, 65535, port.getText().getIntValue());
            if (v != prefs().netPort) { prefs().netPort = v; changed(); }
        });
        port.setInputRestrictions (5, "0123456789");
        port.setTextToShowWhenEmpty (juce::String (net::kDefaultPort), theme::textDim);
        port.setTooltip ("UDP port (the same on all computers, so they find each other)");
        field (password, prefs().netPassword, [this]
        {
            if (password.getText() != prefs().netPassword) { prefs().netPassword = password.getText(); changed(); }
        });
        password.setPasswordCharacter ((juce::juce_wchar) 0x2022);
        password.setTextToShowWhenEmpty ("none: local network only, not encrypted", theme::textDim);
        password.setTooltip ("Stored encrypted for your Windows account (on another computer or account enter it again)");

        addAndMakeVisible (peers);
        peers.setMultiLine (true, false);
        peers.setReturnKeyStartsNewLine (true);
        peers.setText (prefs().netPeers.joinIntoString ("\n"), false);
        peers.setTextToShowWhenEmpty ("e.g. 100.101.102.103 or studio.example.com:6980", theme::textDim);
        peers.onFocusLost = [this]
        {
            auto list = juce::StringArray::fromLines (peers.getText());
            list.trim();
            list.removeEmptyStrings();
            list.removeDuplicates (true);
            if (list != prefs().netPeers) { prefs().netPeers = list; changed(); }
        };

        addAndMakeVisible (heard);
        heard.setMultiLine (true);
        heard.setReadOnly (true);
        heard.setFont (theme::mono (12.0f));
        refresh();
        startTimer (1000);
    }

    void resized() override
    {
        auto r = beginLayout();
        section (r, linkH);
        option (r, enabled);
        status.setBounds (r.removeFromTop (22).withWidth (juce::jmin (kMaxWidth + 160, r.getWidth())));
        r.removeFromTop (8);
        labelled (r, nameL, name, 260);
        labelled (r, portL, port, 100);
        section (r, secH);
        labelled (r, passwordL, password, 260);
        section (r, devicesH);
        devices.setBounds (r.removeFromTop (devices.wantedHeight()).withWidth (juce::jmin (kMaxWidth, r.getWidth())));
        section (r, peersH);
        peers.setBounds (r.removeFromTop (58).withWidth (juce::jmin (420, r.getWidth())));
        section (r, heardH);
        heard.setBounds (r.withWidth (juce::jmin (kMaxWidth + 160, r.getWidth())));
    }

private:
    Session::AppPrefs& prefs() { return ctx.session->prefs; }

    void changed()
    {
        ctx.session->prefsVersion.fetch_add (1);
        if (ctx.prefsChanged) ctx.prefsChanged();
        refresh();
    }

    void timerCallback() override
    {
        UiActivity busy ("settings: network page refresh");
        if (isShowing()) refresh();   // the settings window stays alive when closed: only while on screen
    }

    void refresh()
    {
        auto& hub = net::Hub::get();
        const bool on = hub.isRunning();
        juce::String s;
        if (! prefs().netEnabled) s = "Off: no port is open.";
        else if (! on)            s = "Not running: " + hub.getLastError();
        else
        {
            const auto cfg = hub.getConfig();
            s << "On, port " << cfg.port << ". This computer is \"" << hub.getName() << "\"";
            s << (cfg.password.isNotEmpty() ? ", encrypted." : ", local network only.");
        }
        status.setText (s, juce::dontSendNotification);
        status.setColour (juce::Label::textColourId, prefs().netEnabled && ! on ? theme::mute : theme::text);
        if (devices.refresh()) resized();
        for (auto* c : { (juce::Component*) &name, (juce::Component*) &port, (juce::Component*) &password, (juce::Component*) &peers })
            c->setAlpha (prefs().netEnabled ? 1.0f : 0.5f);

        juce::String t;
        t << "Addresses of this computer: " << net::Hub::localAddresses().joinIntoString (", ") << "\n\n";
        if (on)
        {
            const auto list = hub.getPeers();
            t << "Hydras:" << (list.empty() ? "  none heard yet" : "") << "\n";
            for (auto& p : list)
            {
                t << "  " << (p.name.isNotEmpty() ? p.name : juce::String ("(no answer)")).paddedRight (' ', 22) << "  " << p.address;
                if (p.port != net::kDefaultPort) t << ":" << p.port;
                if (! p.lan) t << "  (outside the local network)";
                if (p.name.isEmpty()) t << "  - not reachable or other password";
                t << "\n";
            }
            const auto streams = hub.getStreams();
            t << "\nStreams this computer receives:" << (streams.empty() ? "  none" : "") << "\n";
            for (auto& st : streams)
            {
                t << "  " << (st.stream + " from " + st.sender).paddedRight (' ', 34) << "  " << net::codecName (st.codec);
                if (st.codec == net::Codec::opus) t << " " << st.kbps << " kbit/s";
                t << ", " << juce::String (st.rate / 1000.0, 1) << " kHz" << (st.used ? ", in use" : "") << "\n";
            }
            if (const auto why = hub.getRejectHint(); why.isNotEmpty()) t << "\nTurned away: " << why << "\n";
        }
        if (t != heard.getText()) heard.setText (t, false);
    }

    UiContext& ctx;
    Heading linkH, secH, peersH, heardH, devicesH;
    LinkDevices devices;
    juce::ToggleButton enabled;
    juce::Label status, nameL { {}, "Computer name" }, portL { {}, "Port" }, passwordL { {}, "Network password" };
    juce::TextEditor name, port, password, peers, heard;
};

//==============================================================================
/** The QR code of the link a phone opens (camera app: scan, tap, connected). */
class QrView final : public juce::Component
{
public:
    void setText (const juce::String& t)
    {
        if (t == text) return;
        text = t;
        code = qr::Code::encode (t.toStdString());
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        if (code.size() == 0) return;
        const int quiet = 3;   // light border in modules (white card around it)
        const int cells = code.size() + 2 * quiet;
        const float cell = std::floor ((float) juce::jmin (getWidth(), getHeight()) / (float) cells);
        const float side = cell * (float) cells;
        const auto area = juce::Rectangle<float> (side, side).withCentre (getLocalBounds().toFloat().getCentre());
        g.setColour (juce::Colours::white);
        g.fillRoundedRectangle (area, cell * 1.5f);
        g.setColour (juce::Colours::black);
        for (int y = 0; y < code.size(); ++y)
            for (int x = 0; x < code.size(); ++x)
                if (code.dark (x, y))
                    g.fillRect (area.getX() + (float) (x + quiet) * cell, area.getY() + (float) (y + quiet) * cell, cell, cell);
    }

private:
    juce::String text;
    qr::Code code;
};

/** Hydra Remote: phones and tablets control Hydra in their browser (remote/RemoteControl.h). */
class RemoteTab final : public SettingsPage, private juce::Timer
{
public:
    explicit RemoteTab (UiContext& c) : ctx (c)
    {
        addHeading (remoteH, "Phone & Tablet",
                    "Control Hydra from a phone or tablet: faders, mute, solo, routing, SEL and scenes, with live meters. "
                    "Nothing has to be installed: the phone opens a page that Hydra serves.\n\n"
                    "The phone must be in the same network (Wi-Fi of the same router). Scan the QR code with the camera app, "
                    "or open the address in the browser and enter the access code. In Chrome choose \"Add to Home screen\" to "
                    "get an app icon.\n\nWindows may ask once whether Hydra may use the network: allow it for private networks.");
        addHeading (codeH, "Access code",
                    "Every phone needs this code once (the QR code contains it). \"New code\" disconnects all phones; each "
                    "has to enter the new one.\n\nThe connection is not encrypted (local network only): use it at home or "
                    "in the studio, not in public Wi-Fi.");
        addHeading (clientsH, "Connected");

        addAndMakeVisible (enabled);
        enabled.setButtonText ("Control Hydra from phones and tablets");
        enabled.setToggleState (prefs().remoteEnabled, juce::dontSendNotification);
        enabled.onClick = [this] { prefs().remoteEnabled = enabled.getToggleState(); changed(); };

        for (auto* l : { &portL, &codeL }) { addAndMakeVisible (l); styleLabel (*l); }
        addAndMakeVisible (status);
        styleLabel (status, false);
        addAndMakeVisible (links);
        styleLabel (links, false);
        links.setFont (theme::mono (14.0f));
        links.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (codeText);
        codeText.setFont (theme::mono (22.0f));
        codeText.setColour (juce::Label::textColourId, theme::text);
        addAndMakeVisible (newCode);
        newCode.onClick = [this]
        {
            prefs().remoteCode = remote::Server::newCode();
            changed();
        };
        addAndMakeVisible (port);
        port.setText (prefs().remotePort != remote::kDefaultPort ? juce::String (prefs().remotePort) : juce::String(), false);
        port.setInputRestrictions (5, "0123456789");
        port.setTextToShowWhenEmpty (juce::String (remote::kDefaultPort), theme::textDim);
        port.setTooltip ("TCP port of the page the phones open");
        auto commitPort = [this]
        {
            const int v = port.getText().trim().isEmpty() ? remote::kDefaultPort : juce::jlimit (1, 65535, port.getText().getIntValue());
            if (v != prefs().remotePort) { prefs().remotePort = v; changed(); }
        };
        port.onReturnKey = commitPort;
        port.onFocusLost = commitPort;
        addChildComponent (qr);
        addAndMakeVisible (clients);
        clients.setMultiLine (true);
        clients.setReadOnly (true);
        clients.setFont (theme::mono (12.0f));
        refresh();
        startTimer (1000);
    }

    void resized() override
    {
        auto r = beginLayout();
        section (r, remoteH);
        option (r, enabled);
        status.setBounds (r.removeFromTop (22).withWidth (juce::jmin (kMaxWidth + 160, r.getWidth())));
        r.removeFromTop (8);
        auto top = r.removeFromTop (180);
        qr.setBounds (top.removeFromLeft (180));
        top.removeFromLeft (20);
        links.setBounds (top);
        r.removeFromTop (8);
        labelled (r, portL, port, 100);
        section (r, codeH);
        auto line = r.removeFromTop (kLine + 6);
        codeL.setBounds (line.removeFromLeft (kLabelWidth));
        codeText.setBounds (line.removeFromLeft (170));
        newCode.setBounds (line.removeFromLeft (110).reduced (0, 4));
        section (r, clientsH);
        clients.setBounds (r.withWidth (juce::jmin (kMaxWidth, r.getWidth())));
    }

private:
    Session::AppPrefs& prefs() { return ctx.session->prefs; }

    void changed()
    {
        ctx.session->prefsVersion.fetch_add (1);
        if (ctx.prefsChanged) ctx.prefsChanged();   // the app applies it (and makes a code the first time)
        refresh();
    }

    void timerCallback() override
    {
        UiActivity busy ("settings: remote page refresh");
        if (isShowing()) refresh();
    }

    void refresh()
    {
        auto* rc = RemoteControl::get();
        const bool on = rc != nullptr && rc->getServer().isRunning();
        juce::String s;
        if (! prefs().remoteEnabled) s = "Off: no port is open.";
        else if (! on)               s = "Not running: " + (rc != nullptr ? rc->getServer().getLastError() : juce::String());
        else                         s = "On. Scan the code with the phone's camera, or open this address in its browser:";
        status.setText (s, juce::dontSendNotification);
        status.setColour (juce::Label::textColourId, prefs().remoteEnabled && ! on ? theme::mute : theme::text);

        const auto code = prefs().remoteCode;
        codeText.setText (code.isEmpty() ? juce::String ("-") : remote::Server::formatCode (code), juce::dontSendNotification);
        const auto all = on ? rc->links() : juce::StringArray();
        links.setText (on ? (all.isEmpty() ? juce::String ("No network connection") : all.joinIntoString ("\n")) : juce::String(),
                       juce::dontSendNotification);
        const auto link = on ? rc->preferredLink() : juce::String();
        qr.setVisible (link.isNotEmpty());
        if (link.isNotEmpty()) qr.setText (link + "#k=" + remote::Server::normaliseCode (code));
        for (auto* c : { (juce::Component*) &port, (juce::Component*) &codeText, (juce::Component*) &newCode })
            c->setAlpha (prefs().remoteEnabled ? 1.0f : 0.5f);

        juce::String t;
        if (on)
        {
            const auto list = rc->getServer().getClients();
            if (list.empty()) t = "No phone or tablet connected.";
            for (auto& cl : list)
            {
                const int min = (int) (cl.seconds / 60.0);
                t << cl.device.paddedRight (' ', 18) << "  " << cl.address.paddedRight (' ', 16) << "  "
                  << (min < 1 ? juce::String ("just now") : juce::String (min) + " min") << "\n";
            }
        }
        if (t != clients.getText()) clients.setText (t, false);
    }

    UiContext& ctx;
    Heading remoteH, codeH, clientsH;
    juce::ToggleButton enabled;
    juce::Label status, links, codeText, portL { {}, "Port" }, codeL { {}, "Code" };
    juce::TextButton newCode { "New code" };
    juce::TextEditor port, clients;
    QrView qr;
};

} // namespace

//==============================================================================
//==============================================================================
namespace
{
juce::StringArray activeHwLabels (const Engine& e)
{
    juce::StringArray l;
    for (int a = 0; a < kMaxHwBuses; ++a) if (e.isBusActive (hwBusIndex (a))) l.add (busLabel (hwBusIndex (a)));
    return l;
}

std::vector<int> activeHwValues (const Engine& e)
{
    std::vector<int> v;
    for (int a = 0; a < kMaxHwBuses; ++a) if (e.isBusActive (hwBusIndex (a))) v.push_back (a);
    return v;
}
} // namespace

/** Start with Windows, system tray, devices, monitoring of the selected bus, shutdown. */
class GeneralTab final : public SettingsPage
{
public:
    explicit GeneralTab (UiContext& c)
        : ctx (c), monOn (c, *c.engine.monOn, "Monitor selected bus", theme::learn),
          monBus (c, *c.engine.monBus, theme::accentHw, activeHwLabels (c.engine), activeHwValues (c.engine))
    {
        addHeading (startH, "Startup", "\"Start when Windows starts\" starts Hydra right when you log on, before other apps and "
                                       "with high priority, so audio works from the first moment. Combine it with \"Start hidden in the tray\".");
        addHeading (trayH, "System tray", "With \"Close to tray\" the close button only hides the window and the mixer keeps running. "
                                          "Quit with \"Shut down\" below or in the right-click menu of the tray icon.");
        addHeading (devicesH, "Devices", "A device that was unplugged or not there at startup is opened again as soon as it is back "
                                         "(checked every 3 s). Off: choose it again or use Tools > Restart Audio Engine.");
        addHeading (monH, "Monitoring (SEL)", "Press SEL on a bus: the input faders then set each input's level on that bus only.\n\n"
                                              "With monitoring on, the selected bus is heard on the monitor output - its normal mix is muted "
                                              "while monitoring, so you hear only the monitored bus (through the monitor bus's EQ, fader and limiter). "
                                              "Press SEL again to return to normal.");
        addAndMakeVisible (monBusL);
        styleLabel (monBusL);
        addAndMakeVisible (monOn);
        addAndMakeVisible (monBus);

        bindPref (ctx, trayIcon,  &Session::AppPrefs::trayIcon,       "Show Hydra Mixer in the system tray (notification area)");
        bindPref (ctx, closeTray, &Session::AppPrefs::closeToTray,    "Close button hides to the tray (audio keeps running)");
        bindPref (ctx, minTray,   &Session::AppPrefs::minimiseToTray, "Minimise to the tray instead of the taskbar");
        bindPref (ctx, startTray, &Session::AppPrefs::startInTray,    "Start hidden in the tray");
        bindPref (ctx, reconnect, &Session::AppPrefs::autoReconnect,  "Reconnect devices automatically when they are back");
        bindPref (ctx, expertModes, &Session::AppPrefs::expertDeviceModes, "Show all Windows audio modes in the device selection (expert)",
                  "Off: Windows devices are offered in one mode only, Low Latency (shared), which suits almost everything. "
                  "On: also Exclusive (lowest latency, blocks the device for other apps) and Shared (compatibility).");
        for (auto* b : { &autostart, &trayIcon, &closeTray, &minTray, &startTray, &reconnect, &expertModes }) addAndMakeVisible (b);

        addAndMakeVisible (shutdown);
        shutdown.setColour (juce::TextButton::buttonColourId, theme::mute.withAlpha (0.25f));
        shutdown.onClick = [this] { if (ctx.shutdown) ctx.shutdown(); };

        // Start with Windows (Task Scheduler entry, see Autostart). Asking schtasks.exe can take seconds and froze
        // the window while the settings opened: asked in the background, the box is greyed out until the answer is there.
        autostart.setButtonText ("Start Hydra Mixer when Windows starts");
        autostart.setTooltip ("Starts right when you log on, before other apps and with high priority, "
                              "so audio works from the first moment. Combine with \"Start hidden in the tray\".");
        autostart.setEnabled (false);
        autostart.onClick = [this]
        {
            const bool want = autostart.getToggleState();
            autostart.setEnabled (false);
            runAutostart ([want] { return Autostart::setEnabled (want); });
        };
        runAutostart ({});
    }

    void resized() override
    {
        auto r = beginLayout();
        section (r, startH);
        option (r, autostart);
        option (r, startTray);
        section (r, trayH);
        option (r, trayIcon);
        option (r, closeTray);
        option (r, minTray);
        section (r, devicesH);
        option (r, reconnect);
        option (r, expertModes);
        section (r, monH);
        auto row = r.removeFromTop (28);
        monOn.setBounds (row.removeFromLeft (230));
        row.removeFromLeft (24);
        monBusL.setBounds (row.removeFromLeft (110));
        monBus.setBounds (row.removeFromLeft (juce::jmin (300, row.getWidth())));
        // Shut down at the bottom, set apart from the options.
        auto bottom = getLocalBounds().reduced (24, 18).removeFromBottom (32);
        shutdown.setBounds (bottom.removeFromLeft (220).withY (juce::jmax (bottom.getY(), r.getY() + 20)));
    }

private:
    /** Runs action (may be empty) and then reads the autostart state, both on a background thread. */
    void runAutostart (std::function<juce::String()> action)
    {
        juce::Component::SafePointer<GeneralTab> safe (this);
        juce::Thread::launch ([safe, action]
        {
            const auto error = action != nullptr ? action() : juce::String();
            const bool on = Autostart::isEnabled();
            juce::MessageManager::callAsync ([safe, error, on]
            {
                if (safe == nullptr) return;
                safe->autostart.setToggleState (on, juce::dontSendNotification);
                safe->autostart.setEnabled (true);
                if (error.isNotEmpty())
                    dialog::message (safe.getComponent(), dialog::Kind::warning, "Start with Windows", error);
            });
        });
    }

    UiContext& ctx;
    Heading startH, trayH, devicesH, monH;
    juce::Label monBusL { {}, "Monitor output" };
    Switch monOn;
    ChoiceSegments monBus;
    juce::ToggleButton autostart, trayIcon, closeTray, minTray, startTray, reconnect, expertModes;
    juce::TextButton shutdown { "Shut down Hydra Mixer" };
};

//==============================================================================
/** Size of the interface and the text, theme, how the channels are laid out. */
class DisplayTab final : public SettingsPage
{
public:
    explicit DisplayTab (UiContext& c) : ctx (c)
    {
        addHeading (lookH, "Size & theme");
        addHeading (layoutH, "Channel layout", "Adaptive strips: with room to spare the groups stay compact and centred and the "
                                               "strips grow a little. Horizontal channels: one row per channel with the fader sideways. "
                                               "Dividers: while not all channels fit, drag them to give a group more room.");
        addHeading (labH, "Experimental", "Options that are being tried out. They may change or go away.");
        for (auto* l : { &uiScaleL, &fontScaleL, &themeL }) { addAndMakeVisible (l); styleLabel (*l); }

        auto& prefs = ctx.session->prefs;
        // Zoom of the main window and text size, both with the same steps.
        auto setupScale = [this] (juce::ComboBox& box, float current, float Session::AppPrefs::* field)
        {
            addAndMakeVisible (box);
            int id = 1, sel = 0, bestDiff = 1000;
            for (int pc : { 75, 80, 90, 100, 110, 125, 150, 175, 200 })
            {
                box.addItem (juce::String (pc) + " %", id);
                if (const int d = std::abs (pc - juce::roundToInt (current * 100.0f)); d < bestDiff) { bestDiff = d; sel = id; }
                ++id;
            }
            box.setSelectedId (sel, juce::dontSendNotification);
            box.onChange = [this, &box, field]
            {
                ctx.session->prefs.*field = box.getText().getFloatValue() / 100.0f;
                ctx.session->prefsVersion.fetch_add (1);
                if (ctx.prefsChanged) ctx.prefsChanged();
            };
        };
        setupScale (uiScale, prefs.uiScale, &Session::AppPrefs::uiScale);
        setupScale (fontScale, prefs.fontScale, &Session::AppPrefs::fontScale);
        uiScale.setTooltip ("Size of everything in the main window. The strips scroll sideways when they no longer fit.");
        fontScale.setTooltip ("Size of the text in all windows (open windows update when they are rebuilt / reopened)");

        addAndMakeVisible (themeBox);
        themeBox.addItem ("Default", 1);
        themeBox.addItem ("Legacy", 2);
        themeBox.setSelectedId (theme::styleFromName (prefs.theme) == theme::Style::legacy ? 2 : 1, juce::dontSendNotification);
        themeBox.setTooltip ("Look of the app. Default: dark, channels as one block, rectangular faders. Legacy: rounded cards and round faders.");
        themeBox.onChange = [this]
        {
            ctx.session->prefs.theme = theme::styleName (themeBox.getSelectedId() == 2 ? theme::Style::legacy : theme::Style::deep);
            ctx.session->prefsVersion.fetch_add (1);
            if (ctx.prefsChanged) ctx.prefsChanged();
        };

        bindPref (ctx, adaptiveStrips, &Session::AppPrefs::adaptiveStrips,   "Adaptive strips: compact groups, centred, larger strips when there is room");
        bindPref (ctx, horizontal,     &Session::AppPrefs::horizontalStrips, "Horizontal channels: one row per channel, faders sideways");
        bindPref (ctx, stripDividers,  &Session::AppPrefs::stripDividers,    "Draggable dividers when the channels don't fit (off: one shared scroll bar)");
        bindPref (ctx, fitWindow,      &Session::AppPrefs::fitWindow,        "Window always as large as needed: takes its fitting size each time it appears");
        bindPref (ctx, sidePanel,      &Session::AppPrefs::sidePanel,        "Side panel with the routing matrix when there is room");
        for (auto* b : { &adaptiveStrips, &horizontal, &stripDividers, &fitWindow, &sidePanel }) addAndMakeVisible (b);
    }

    void resized() override
    {
        auto r = beginLayout();
        section (r, lookH);
        labelled (r, uiScaleL, uiScale, 140);
        labelled (r, fontScaleL, fontScale, 140);
        labelled (r, themeL, themeBox, 140);
        section (r, layoutH);
        for (auto* b : { &adaptiveStrips, &horizontal, &stripDividers, &fitWindow }) option (r, *b);
        section (r, labH);
        option (r, sidePanel);
    }

private:
    UiContext& ctx;
    Heading lookH, layoutH, labH;
    juce::Label uiScaleL { {}, "Interface size" }, fontScaleL { {}, "Text size" }, themeL { {}, "Theme" };
    juce::ComboBox uiScale, fontScale, themeBox;
    juce::ToggleButton adaptiveStrips, horizontal, stripDividers, fitWindow, sidePanel;
};

//==============================================================================
/** A tab whose page is built the first time it is shown. Why: building all nine pages up front (device lists, MIDI
 *  ports, VAC queries, plugin list ...) made the first opening of the Settings window slow; now only the page that is
 *  opened is built. Pages that take long to build are reported in hydra.log. */
class LazyTab final : public juce::Component
{
public:
    LazyTab (juce::String n, std::function<juce::Component*()> f) : name (std::move (n)), make (std::move (f)) {}
    void visibilityChanged() override { build(); }
    void resized() override { if (content != nullptr) content->setBounds (getLocalBounds()); }
    void build()
    {
        if (content != nullptr || make == nullptr || ! isVisible()) return;
        UiActivity busy ("settings: building a page");
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        content.reset (make());
        make = nullptr;
        addAndMakeVisible (*content);
        content->setBounds (getLocalBounds());
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
        if (ms > 40.0) logLine ("settings: building the page " + name + " took " + juce::String (juce::roundToInt (ms)) + " ms");
    }

private:
    juce::String name;
    std::function<juce::Component*()> make;
    std::unique_ptr<juce::Component> content;
};

SettingsPanel::SettingsPanel (UiContext& c) : ctx (c)
{
    addAndMakeVisible (tabs);
    tabs.setTabBarDepth (40);
    auto add = [this] (const juce::String& name, std::function<juce::Component*()> make)
    {
        tabs.addTab (name, theme::panel, new LazyTab (name, std::move (make)), true);
    };
    // Same order as SettingsPanel::Tab.
    add ("General", [this] { return new GeneralTab (ctx); });
    add ("Display", [this] { return new DisplayTab (ctx); });
    add ("Audio Engine", [this] { return new AudioTab (ctx, [this] { if (onEngineChanged) onEngineChanged(); }); });
    add ("Virtual Cables", [this] { return new CablesTab (ctx, [this] { if (onEngineChanged) onEngineChanged(); }); });
    add ("Network", [this] { return new NetworkTab (ctx); });
    add ("Remote", [this] { return new RemoteTab (ctx); });
    add ("MIDI", [this] { return new MidiTab (ctx); });
    add ("Plugins", [this] { return new PluginTab (ctx); });
    add ("Backup & Transfer", [this] { return new BackupTab (ctx, [this] { if (onSessionReplaced) onSessionReplaced(); },
                                                                  [this] { if (onEngineChanged) onEngineChanged(); }); });
}

SettingsPanel::~SettingsPanel() = default;

void SettingsPanel::paint (juce::Graphics& g)
{
    if (paintFrom > 0.0) uiActivity.store ("settings: drawing the window");   // cleared in paintOverChildren
    g.fillAll (theme::bg);
}

void SettingsPanel::paintOverChildren (juce::Graphics&)
{
    if (paintFrom <= 0.0) return;
    uiActivity.store (nullptr);
    const double ms = juce::Time::getMillisecondCounterHiRes() - paintFrom;
    paintFrom = 0.0;
    if (ms > 100.0) logLine ("settings: shown after " + juce::String (juce::roundToInt (ms)) + " ms (opening + first drawing)");
}

void SettingsPanel::lookAndFeelChanged()
{
    for (int i = 0; i < tabs.getNumTabs(); ++i)
        tabs.setTabBackgroundColour (i, theme::panel);
}
void SettingsPanel::resized() { tabs.setBounds (getLocalBounds().reduced (8)); }

//==============================================================================
struct MidiMapPanel::Row : juce::Component
{
    Row (MidiMapPanel& o) : owner (o)
    {
        addAndMakeVisible (mode);
        addAndMakeVisible (invert);
        addAndMakeVisible (remove);
        for (int i = 0; i < 4; ++i)
            mode.addItem (MidiMapping::modeName ((MidiMapping::Mode) i), i + 1);
        mode.onChange = [this] { owner.ctx.midi.setMappingMode (row, (MidiMapping::Mode) (mode.getSelectedId() - 1)); };
        invert.setButtonText ("Invert");
        invert.onClick = [this] { owner.ctx.midi.setMappingInvert (row, invert.getToggleState()); };
        remove.onClick = [this] { auto& o2 = owner; const int r = row; juce::MessageManager::callAsync ([&o2, r] { o2.ctx.midi.removeMapping (r); }); };

        for (auto* l : { &minLabel, &maxLabel })
        {
            addAndMakeVisible (l);
            l->setEditable (true, true, false);
            l->setJustificationType (juce::Justification::centred);
            l->setFont (theme::font (12.5f));
            l->setColour (juce::Label::backgroundColourId, theme::bg);
            l->setColour (juce::Label::outlineColourId, theme::outline);
            l->setTooltip ("MIDI range: type a value (e.g. 0 or -inf) - the hardware control sweeps only this range");
        }
        minLabel.onTextChange = [this] { applyRange (minLabel.getText(), true); };
        maxLabel.onTextChange = [this] { applyRange (maxLabel.getText(), false); };
    }

    /** Parses "0", "-6 dB", "-inf", "75 %" ... in the parameter's own unit. */
    void applyRange (const juce::String& text, bool isMin)
    {
        if (param == nullptr) return;
        const auto t = text.trim().toLowerCase();
        float v = t.startsWith ("-inf") ? param->range.start : t.retainCharacters ("-+.,0123456789").replaceCharacter (',', '.').getFloatValue();
        v = juce::jlimit (param->range.start, param->range.end, v);
        const float n = param->range.convertTo0to1 (v);
        auto& midi = owner.ctx.midi;
        const int r = row;
        const float lo = isMin ? n : minN, hi = isMin ? maxN : n;
        juce::MessageManager::callAsync ([&midi, r, lo, hi] { midi.setMappingRange (r, lo, hi); });
    }

    void set (int r, const MidiMapping& m)
    {
        row = r;
        paramName = m.param != nullptr ? m.param->name : m.paramId;
        source = m.describe();
        mode.setSelectedId ((int) m.mode + 1, juce::dontSendNotification);
        invert.setToggleState (m.invert, juce::dontSendNotification);
        param = m.param;
        minN = m.minN;
        maxN = m.maxN;
        const bool ranged = param != nullptr && param->type == ParamType::Continuous && ! m.isSwitch();
        minLabel.setVisible (ranged);
        maxLabel.setVisible (ranged);
        if (ranged)
        {
            const auto txt = m.rangeText();
            minLabel.setText (txt.upToFirstOccurrenceOf ("  ->  ", false, false), juce::dontSendNotification);
            maxLabel.setText (txt.fromFirstOccurrenceOf ("  ->  ", false, false), juce::dontSendNotification);
        }
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour ((row & 1) ? theme::panel : theme::panel2);
        g.fillRect (getLocalBounds());
        auto r = getLocalBounds().reduced (10, 0);
        g.setColour (theme::text);
        g.setFont (theme::font (13.0f, true));
        g.drawText (paramName, r.removeFromLeft (220), juce::Justification::centredLeft);
        g.setColour (theme::textDim);
        g.setFont (theme::font (12.5f));
        g.drawText (source, r.removeFromLeft (260), juce::Justification::centredLeft);
        if (minLabel.isVisible())
        {
            g.setColour (theme::textFaint);
            g.drawText ("->", minLabel.getRight(), 0, maxLabel.getX() - minLabel.getRight(), getHeight(), juce::Justification::centred);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6, 5);
        remove.setBounds (r.removeFromRight (70));
        r.removeFromRight (8);
        invert.setBounds (r.removeFromRight (80));
        r.removeFromRight (8);
        mode.setBounds (r.removeFromRight (150));
        r.removeFromRight (12);
        maxLabel.setBounds (r.removeFromRight (90).reduced (0, 2));
        r.removeFromRight (24);
        minLabel.setBounds (r.removeFromRight (90).reduced (0, 2));
    }

    MidiMapPanel& owner;
    int row = 0;
    Param* param = nullptr;
    float minN = 0.0f, maxN = 1.0f;
    juce::Label minLabel, maxLabel;
    juce::String paramName, source;
    juce::ComboBox mode;
    juce::ToggleButton invert;
    juce::TextButton remove { "Remove" };
};

MidiMapPanel::MidiMapPanel (UiContext& c) : ctx (c)
{
    addAndMakeVisible (list);
    list.setModel (this);
    list.setRowHeight (36);
    list.setColour (juce::ListBox::backgroundColourId, theme::panel);
    addAndMakeVisible (learnButton);
    addAndMakeVisible (clearButton);
    addAndMakeVisible (hint);
    hint.setText ("MIDI mapping\nRight-click any control -> MIDI Learn / MIDI Range, or use Learn Mode.\n\n"
                  "Click a range value in the table to type it (e.g. 0 or -inf).");
    learnButton.setClickingTogglesState (true);
    learnButton.onClick = [this]
    {
        ctx.learnMode = learnButton.getToggleState();
        if (! ctx.learnMode) ctx.midi.cancelLearn();
        if (ctx.learnModeChanged) ctx.learnModeChanged();
    };
    clearButton.onClick = [this]
    {
        juce::Component::SafePointer<MidiMapPanel> safe (this);
        dialog::confirm (this, dialog::Kind::danger, "Clear all MIDI mappings?",
                         "Every MIDI assignment is removed. Keyboard shortcuts stay.", "Remove all",
                         [safe] { if (safe != nullptr) safe->ctx.midi.clearAll(); });
    };
    startTimerHz (5);
    reload();
}

void MidiMapPanel::paint (juce::Graphics& g) { g.fillAll (theme::bg); }

void MidiMapPanel::resized()
{
    auto r = getLocalBounds().reduced (12);
    auto top = r.removeFromTop (32);
    learnButton.setBounds (top.removeFromLeft (130));
    top.removeFromLeft (8);
    clearButton.setBounds (top.removeFromLeft (110));
    top.removeFromLeft (12);
    hint.setBounds (top.removeFromLeft (InfoButton::kSize).withSizeKeepingCentre (InfoButton::kSize, InfoButton::kSize));
    r.removeFromTop (10);
    list.setBounds (r);
}

juce::Component* MidiMapPanel::refreshComponentForRow (int row, bool, juce::Component* existing)
{
    auto* r = dynamic_cast<Row*> (existing);
    if (! juce::isPositiveAndBelow (row, (int) rows.size()))
    {
        delete existing;
        return nullptr;
    }
    if (r == nullptr) { delete existing; r = new Row (*this); }
    r->set (row, rows[(size_t) row]);
    return r;
}

void MidiMapPanel::timerCallback()
{
    learnButton.setToggleState (ctx.learnMode, juce::dontSendNotification);
    if (ctx.midi.mapVersion.load() != seenVersion) reload();
}

void MidiMapPanel::reload()
{
    seenVersion = ctx.midi.mapVersion.load();
    rows = ctx.midi.getMappings();
    list.updateContent();
    list.repaint();
}
} // namespace hydra

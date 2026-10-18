#include "Settings.h"
#include "state/VoicemeeterImport.h"
#include "state/Session.h"
#include "state/Autostart.h"

namespace hydra
{
namespace
{
void styleLabel (juce::Label& l, bool dim = true, float size = 13.0f)
{
    l.setFont (theme::font (size));
    l.setColour (juce::Label::textColourId, dim ? theme::textDim : theme::text);
}

//==============================================================================
class AudioTab final : public juce::Component, private juce::Timer
{
public:
    AudioTab (UiContext& c, std::function<void()> changed) : ctx (c), onChanged (std::move (changed))
    {
        for (auto* l : { &typeL, &devL, &rateL, &bufL }) { addAndMakeVisible (l); styleLabel (*l); }
        for (auto* b : { &typeBox, &devBox, &rateBox, &bufBox }) addAndMakeVisible (b);
        addAndMakeVisible (apply);
        addAndMakeVisible (panel);
        addAndMakeVisible (status);
        addAndMakeVisible (info);
        styleLabel (status, false);
        styleLabel (info);
        info.setJustificationType (juce::Justification::topLeft);
        info.setText ("The master device is the clock of the whole mixer. All of its inputs/outputs are "
                      "processed with zero extra buffering. Use an ASIO driver here for the lowest possible "
                      "latency; every other device is synchronised automatically with an adaptive, "
                      "self-tuning drift compensator.", juce::dontSendNotification);

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
        auto r = getLocalBounds().reduced (24, 20);
        auto row = [&r] (juce::Label& l, juce::Component& c)
        {
            auto line = r.removeFromTop (30);
            l.setBounds (line.removeFromLeft (130));
            c.setBounds (line.withWidth (juce::jmin (420, line.getWidth())));
            r.removeFromTop (10);
        };
        row (typeL, typeBox);
        row (devL, devBox);
        row (rateL, rateBox);
        row (bufL, bufBox);
        auto buttons = r.removeFromTop (32).withTrimmedLeft (130);
        apply.setBounds (buttons.removeFromLeft (150));
        buttons.removeFromLeft (10);
        panel.setBounds (buttons.removeFromLeft (190));
        r.removeFromTop (16);
        status.setBounds (r.removeFromTop (44));
        r.removeFromTop (8);
        info.setBounds (r.removeFromTop (80));
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
            t->scanForDevices();
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

    void timerCallback() override
    {
        auto& e = ctx.engine;
        juce::String s;
        if (auto* d = e.getMasterDevice())
            s << "Running: " << d->getName() << "  |  " << (int) e.getSampleRate() << " Hz  |  "
              << d->getCurrentBufferSizeSamples() << " samples  |  in+out latency ~" << juce::String (e.getMasterLatencyMs(), 2)
              << " ms  |  DSP load " << juce::String (e.getCpuLoad() * 100.0f, 1) << " %";
        else
            s << "Stopped. " << e.getLastError();
        status.setText (s, juce::dontSendNotification);
    }

    UiContext& ctx;
    std::function<void()> onChanged;
    juce::Label typeL { {}, "Driver" }, devL { {}, "Device" }, rateL { {}, "Sample rate" }, bufL { {}, "Buffer size" };
    juce::ComboBox typeBox, devBox, rateBox, bufBox;
    juce::TextButton apply { "Apply / Restart" }, panel { "ASIO Control Panel" };
    juce::Label status, info;
};

//==============================================================================
class MidiTab final : public juce::Component
{
public:
    explicit MidiTab (UiContext& c) : ctx (c)
    {
        addAndMakeVisible (inL); styleLabel (inL);
        addAndMakeVisible (outL); styleLabel (outL);
        addAndMakeVisible (outBox);
        addAndMakeVisible (refresh);
        addAndMakeVisible (help); styleLabel (help);
        help.setJustificationType (juce::Justification::topLeft);
        help.setText ("MIDI learn: right-click ANY control in the mixer and choose \"MIDI Learn\", "
                      "or enable MIDI Learn Mode in the Tools menu (wrench) and click controls one after another, moving a "
                      "hardware knob/fader/button after each click. Buttons toggle, faders/knobs track absolute "
                      "values (Pitch-Bend faders supported). The feedback output sends values back to "
                      "LEDs / motor faders.", juce::dontSendNotification);
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
        addAndMakeVisible (ledHelp); styleLabel (ledHelp, true, 12.0f);
        ledHelp.setJustificationType (juce::Justification::topLeft);
        ledHelp.setText ("Pad colours by velocity. Active = button on, Inactive = mapped but off, Unbound = no mapping, "
                         "Muted row = unbound pads in a row whose channel is muted. APC mini mk1: 0 off, 1 green, 3 red, "
                         "5 yellow (2 / 4 / 6 = blinking).", juce::dontSendNotification);
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
        auto r = getLocalBounds().reduced (24, 20);
        auto top = r.removeFromTop (30);
        inL.setBounds (top.removeFromLeft (200));
        refresh.setBounds (top.removeFromRight (110));
        r.removeFromTop (4);
        for (auto& t : toggles) t->setBounds (r.removeFromTop (28).withTrimmedLeft (8));
        if (toggles.empty()) r.removeFromTop (28);
        r.removeFromTop (16);
        auto o = r.removeFromTop (30);
        outL.setBounds (o.removeFromLeft (200));
        outBox.setBounds (o.withWidth (juce::jmin (380, o.getWidth())));
        r.removeFromTop (12);
        faderTop.setBounds (r.removeFromTop (28).withTrimmedLeft (200).withWidth (juce::jmin (520, r.getWidth() - 200)));
        r.removeFromTop (12);
        auto l = r.removeFromTop (30);
        ledL.setBounds (l.removeFromLeft (200));
        ledBox.setBounds (l.withWidth (juce::jmin (380, l.getWidth())));
        r.removeFromTop (6);
        auto caps = r.removeFromTop (18).withTrimmedLeft (200);
        auto vals = r.removeFromTop (26).withTrimmedLeft (200);
        const int w = juce::jmin (78, caps.getWidth() / 8);
        for (auto& f : fields)
        {
            f.caption.setBounds (caps.removeFromLeft (w));
            f.value.setBounds (vals.removeFromLeft (w).reduced (4, 0));
        }
        r.removeFromTop (4);
        ledHelp.setBounds (r.removeFromTop (34).withTrimmedLeft (200));
        r.removeFromTop (12);
        help.setBounds (r.removeFromTop (90));
    }

    void paint (juce::Graphics& g) override
    {
        if (toggles.empty())
        {
            g.setColour (theme::textFaint);
            g.setFont (theme::font (12.5f));
            g.drawText ("No MIDI input devices found.", 32, 54, 400, 24, juce::Justification::centredLeft);
        }
    }

private:
    UiContext& ctx;
    juce::Label inL { {}, "MIDI inputs (controllers)" }, outL { {}, "Feedback output (LEDs, motor faders)" };
    juce::ComboBox outBox;
    juce::TextButton refresh { "Refresh" };
    juce::ToggleButton faderTop { "MIDI faders stop at 0 dB (instead of +12 dB)" };
    juce::Label help;
    std::vector<std::unique_ptr<juce::ToggleButton>> toggles;
    juce::Label ledL { {}, "LED feedback profile" }, ledHelp;
    juce::ComboBox ledBox;
    struct NumField { juce::Label caption, value; };
    std::array<NumField, 8> fields;
};

//==============================================================================
class PluginTab final : public juce::Component, private juce::Timer, private juce::Thread
{
public:
    explicit PluginTab (UiContext& c) : juce::Thread ("Plugin scan"), ctx (c)
    {
        addAndMakeVisible (pathsL); styleLabel (pathsL);
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
        auto r = getLocalBounds().reduced (24, 20);
        pathsL.setBounds (r.removeFromTop (24));
        paths.setBounds (r.removeFromTop (110));
        r.removeFromTop (12);
        auto row = r.removeFromTop (32);
        scan.setBounds (row.removeFromLeft (160));
        row.removeFromLeft (12);
        bar.setBounds (row.withHeight (22).withY (row.getY() + 5));
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
    juce::Label pathsL { {}, "VST3 search folders (one per line)" };
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
        addAndMakeVisible (title); styleLabel (title, false, 15.0f);
        addAndMakeVisible (hint); styleLabel (hint);
        hint.setJustificationType (juce::Justification::topLeft);
        hint.setText ("Reads a Voicemeeter (Potato) settings XML (Voicemeeter: Menu -> Save Settings) and takes over "
                      "devices, ASIO channel patches, names, routing, faders, EQ, gate, compressor, denoiser and the "
                      "MIDI mapping. Close Voicemeeter first so the ASIO driver is free. Your current Hydra settings are replaced.",
                      juce::dontSendNotification);
        addAndMakeVisible (files);
        addAndMakeVisible (browse);
        addAndMakeVisible (import);
        addAndMakeVisible (report);
        report.setMultiLine (true);
        report.setReadOnly (true);
        report.setFont (theme::mono (12.0f));

        for (auto& f : VoicemeeterImport::findCandidates())
        {
            candidates.add (f);
            files.addItem (f.getFileName() + "   (" + f.getLastModificationTime().formatted ("%d.%m.%Y %H:%M") + ")", candidates.size());
        }
        if (! candidates.isEmpty()) files.setSelectedId (1, juce::dontSendNotification);
        else files.setTextWhenNothingSelected ("No Voicemeeter XML found in Documents - use Browse");

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
        title.setBounds (r.removeFromTop (26));
        hint.setBounds (r.removeFromTop (40));
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
    UiContext& ctx;
    std::function<void()> onChanged;
    juce::Label title { {}, "Import from Voicemeeter" }, hint;
    juce::ComboBox files;
    juce::TextButton browse { "Browse..." }, import { "Import" };
    juce::TextEditor report;
    juce::Array<juce::File> candidates;
    std::unique_ptr<juce::FileChooser> chooser;
};

//==============================================================================
/** Export / import of the complete settings, restore of automatic backups and the Voicemeeter import. */
class BackupTab final : public juce::Component
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
        for (auto* l : { &title, &backupTitle }) { addAndMakeVisible (l); styleLabel (*l, false, 15.0f); }
        for (auto* l : { &hint, &backupHint, &status }) { addAndMakeVisible (l); styleLabel (*l); l->setJustificationType (juce::Justification::topLeft); }
        hint.setText ("Saves everything (devices, routing, names, faders, EQ, dynamics, denoiser, plugins, MIDI map, "
                      "keyboard shortcuts) to one file, e.g. to move it to another PC or keep it safe before an update.",
                      juce::dontSendNotification);
        backupHint.setText ("Hydra keeps a copy of your settings at every start and before every import (newest "
                            + juce::String (Session::kMaxBackups) + "). An unreadable settings file is never overwritten.",
                            juce::dontSendNotification);
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
        auto r = getLocalBounds().reduced (24, 20);
        title.setBounds (r.removeFromTop (26));
        hint.setBounds (r.removeFromTop (40));
        auto row = r.removeFromTop (30);
        exportButton.setBounds (row.removeFromLeft (180));
        row.removeFromLeft (10);
        importButton.setBounds (row.removeFromLeft (180));
        r.removeFromTop (24);
        backupTitle.setBounds (r.removeFromTop (26));
        backupHint.setBounds (r.removeFromTop (40));
        row = r.removeFromTop (30);
        folderButton.setBounds (row.removeFromRight (130));
        row.removeFromRight (8);
        restoreButton.setBounds (row.removeFromRight (120));
        row.removeFromRight (8);
        backups.setBounds (row);
        r.removeFromTop (6);
        status.setBounds (r.removeFromTop (36));
        r.removeFromTop (14);
        voicemeeter.setBounds (r);
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
    juce::Label title { {}, "Export / Import Settings" }, hint, backupTitle { {}, "Automatic Backups" }, backupHint, status;
    juce::TextButton exportButton { "Export Settings..." }, importButton { "Import Settings..." },
                     restoreButton { "Restore" }, folderButton { "Open Folder" };
    juce::ComboBox backups;
    juce::Array<juce::File> files;
    std::unique_ptr<juce::FileChooser> chooser;
    VoicemeeterSection voicemeeter;
};

//==============================================================================
class CablesTab final : public juce::Component
{
public:
    CablesTab (UiContext& c, std::function<void()> changed) : ctx (c), onChanged (std::move (changed))
    {
        addAndMakeVisible (title); styleLabel (title, false, 15.0f);
        addAndMakeVisible (info); styleLabel (info);
        info.setJustificationType (juce::Justification::topLeft);
        info.setText ("Software inputs without a driver of our own:\n"
                      "  1) Per-app capture: choose \"Application\" in a strip's source menu (e.g. Spotify.exe). "
                      "Works with any app, no install. To avoid hearing the app twice, set its output in "
                      "Windows (Settings > Sound > Volume mixer) to a device you don't listen to.\n"
                      "  2) Virtual cables (signed drivers, Secure Boot stays on): free VB-Cable (vb-audio.com/Cable, "
                      "up to 5 cables with A+B / C+D) or Virtual Audio Cable (vac.muzychenko.net, any number, freely named). "
                      "Apps play into the cable's playback side, Hydra records the other side. For B buses it's reversed.",
                      juce::dontSendNotification);
        addAndMakeVisible (list);
        list.setMultiLine (true);
        list.setReadOnly (true);
        list.setFont (theme::mono (12.0f));
        addAndMakeVisible (refresh);
        addAndMakeVisible (assign);
        refresh.onClick = [this] { ctx.engine.rescanDevices(); update(); };
        assign.onClick = [this] { autoAssign(); };
        assign.setTooltip ("Gives every input / B bus that still points to a 'Hydra Virtual' placeholder the next free cable");
        update();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (24, 20);
        title.setBounds (r.removeFromTop (26));
        info.setBounds (r.removeFromTop (120));
        r.removeFromTop (8);
        auto row = r.removeFromTop (30);
        assign.setBounds (row.removeFromRight (200));
        row.removeFromRight (8);
        refresh.setBounds (row.removeFromRight (100));
        r.removeFromTop (10);
        list.setBounds (r);
    }

private:
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
            if (c == nullptr) { rep.add (busCode (b) + ": no free cable left"); continue; }
            IoTarget t; t.kind = IoTarget::Kind::Device; t.typeName = "Windows Audio (Low Latency Mode)"; t.deviceName = c->playbackName;
            eng.setBusOutput (b, t);
            rep.add (busCode (b) + ": apps (Discord, OBS ...) record from \"" + c->recordingName + "\"");
        }
        if (rep.isEmpty()) rep.add ("Nothing to assign: no input / B bus points to a 'Hydra Virtual' placeholder.");
        update (rep.joinIntoString ("\n"));
        if (onChanged) onChanged();
    }

    UiContext& ctx;
    std::function<void()> onChanged;
    juce::Label title { {}, "Software inputs & virtual cables" }, info;
    juce::TextEditor list;
    juce::TextButton refresh { "Refresh" }, assign { "Auto-assign cables" };
};

} // namespace

//==============================================================================
//==============================================================================
namespace
{
juce::StringArray activeHwLabels (const Engine& e)
{
    juce::StringArray l;
    for (int a = 0; a < kMaxHwBuses; ++a) if (e.isBusActive (hwBusIndex (a))) l.add (busCode (hwBusIndex (a)));
    return l;
}

std::vector<int> activeHwValues (const Engine& e)
{
    std::vector<int> v;
    for (int a = 0; a < kMaxHwBuses; ++a) if (e.isBusActive (hwBusIndex (a))) v.push_back (a);
    return v;
}
} // namespace

/** Monitoring of the selected bus, system tray behaviour, shutdown. */
class GeneralTab final : public juce::Component
{
public:
    explicit GeneralTab (UiContext& c)
        : ctx (c), monOn (c, *c.engine.monOn, "Monitor selected bus", theme::learn),
          monBus (c, *c.engine.monBus, theme::accentHw, activeHwLabels (c.engine), activeHwValues (c.engine))
    {
        for (auto* l : { &monTitle, &trayTitle, &displayTitle })
        {
            addAndMakeVisible (l);
            l->setFont (theme::font (12.0f, true));
            l->setColour (juce::Label::textColourId, theme::textDim);
        }
        for (auto* l : { &monHelp, &monBusL, &trayHelp, &uiScaleL, &fontScaleL })
        {
            addAndMakeVisible (l);
            styleLabel (*l, true, 12.5f);
            l->setJustificationType (juce::Justification::topLeft);
        }
        for (auto* l : { &uiScaleL, &fontScaleL }) l->setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (monOn);
        addAndMakeVisible (monBus);
        monHelp.setText ("Press SEL on a bus: the input faders then set each input's level on that bus only. "
                         "With monitoring on, the selected bus is heard on the monitor output below - its normal mix is muted "
                         "while monitoring, so you hear only the monitored bus (through the monitor bus's EQ, fader and limiter). "
                         "Press SEL again to return to normal.", juce::dontSendNotification);

        trayHelp.setText ("With \"Close to tray\" the close button only hides the window and the mixer keeps running. "
                          "Quit with \"Shut down\" below or in the right-click menu of the tray icon.",
                          juce::dontSendNotification);

        auto& prefs = ctx.session->prefs;
        struct Opt { juce::ToggleButton* b; bool Session::AppPrefs::* field; const char* text; };
        const Opt opts[] = {
            { &trayIcon,   &Session::AppPrefs::trayIcon,       "Show Hydra Mixer in the system tray (notification area)" },
            { &closeTray,  &Session::AppPrefs::closeToTray,    "Close button hides to the tray (audio keeps running)" },
            { &minTray,    &Session::AppPrefs::minimiseToTray, "Minimise to the tray instead of the taskbar" },
            { &startTray,  &Session::AppPrefs::startInTray,    "Start hidden in the tray" },
        };
        for (auto& o : opts)
        {
            addAndMakeVisible (o.b);
            o.b->setButtonText (o.text);
            o.b->setToggleState (prefs.*(o.field), juce::dontSendNotification);
            auto field = o.field;
            auto* b = o.b;
            o.b->onClick = [this, b, field]
            {
                ctx.session->prefs.*field = b->getToggleState();
                ctx.session->prefsVersion.fetch_add (1);
                if (ctx.prefsChanged) ctx.prefsChanged();
            };
        }

        // Display: zoom of the main window and text size.
        auto setupScale = [this] (juce::ComboBox& box, std::initializer_list<int> percents, float current, float Session::AppPrefs::* field)
        {
            addAndMakeVisible (box);
            int id = 1, sel = 0, bestDiff = 1000;
            for (int pc : percents)
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
        // Both use the same steps.
        const std::initializer_list<int> steps { 75, 80, 90, 100, 110, 125, 150, 175, 200 };
        setupScale (uiScale, steps, prefs.uiScale, &Session::AppPrefs::uiScale);
        setupScale (fontScale, steps, prefs.fontScale, &Session::AppPrefs::fontScale);
        uiScale.setTooltip ("Size of everything in the main window. The strips scroll sideways when they no longer fit.");
        fontScale.setTooltip ("Size of the text in all windows (open windows update when they are rebuilt / reopened)");

        addAndMakeVisible (shutdown);
        shutdown.setColour (juce::TextButton::buttonColourId, theme::mute.withAlpha (0.25f));
        shutdown.onClick = [this] { if (ctx.shutdown) ctx.shutdown(); };

        // Start with Windows (Task Scheduler entry, see Autostart)
        addAndMakeVisible (autostart);
        autostart.setButtonText ("Start Hydra Mixer when Windows starts");
        autostart.setToggleState (Autostart::isEnabled(), juce::dontSendNotification);
        autostart.setTooltip ("Starts right when you log on, before other apps and with high priority, "
                              "so audio works from the first moment. Combine with \"Start hidden in the tray\".");
        autostart.onClick = [this]
        {
            const bool want = autostart.getToggleState();
            const auto error = Autostart::setEnabled (want);
            autostart.setToggleState (Autostart::isEnabled(), juce::dontSendNotification);
            if (error.isNotEmpty())
                dialog::message (this, dialog::Kind::warning, "Start with Windows", error);
        };
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (theme::outline.withAlpha (0.6f));
        g.fillRect (divider);
        g.fillRect (divider2);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (24, 18);
        monTitle.setBounds (r.removeFromTop (22));
        r.removeFromTop (6);
        auto row = r.removeFromTop (28);
        monOn.setBounds (row.removeFromLeft (230));
        row.removeFromLeft (24);
        monBusL.setBounds (row.removeFromLeft (110));
        monBus.setBounds (row.removeFromLeft (300));
        r.removeFromTop (10);
        monHelp.setBounds (r.removeFromTop (64));
        r.removeFromTop (10);
        divider = r.removeFromTop (1);
        r.removeFromTop (16);

        displayTitle.setBounds (r.removeFromTop (22));
        r.removeFromTop (6);
        auto drow = r.removeFromTop (28);
        uiScaleL.setBounds (drow.removeFromLeft (110));
        uiScale.setBounds (drow.removeFromLeft (110));
        drow.removeFromLeft (36);
        fontScaleL.setBounds (drow.removeFromLeft (80));
        fontScale.setBounds (drow.removeFromLeft (110));
        r.removeFromTop (16);
        divider2 = r.removeFromTop (1);
        r.removeFromTop (16);

        trayTitle.setBounds (r.removeFromTop (22));
        r.removeFromTop (4);
        for (auto* b : { &autostart, &trayIcon, &closeTray, &minTray, &startTray })
            b->setBounds (r.removeFromTop (28).withWidth (520));
        r.removeFromTop (8);
        trayHelp.setBounds (r.removeFromTop (40));
        r.removeFromTop (8);
        shutdown.setBounds (r.removeFromTop (32).withWidth (220));
    }

private:
    UiContext& ctx;
    juce::Label monTitle { {}, "MONITORING (SEL)" }, trayTitle { {}, "STARTUP, WINDOW & SYSTEM TRAY" }, displayTitle { {}, "DISPLAY" };
    juce::Label monHelp, monBusL { {}, "Monitor output" }, trayHelp, uiScaleL { {}, "Interface size" }, fontScaleL { {}, "Text size" };
    juce::ComboBox uiScale, fontScale;
    Switch monOn;
    ChoiceSegments monBus;
    juce::ToggleButton autostart, trayIcon, closeTray, minTray, startTray;
    juce::TextButton shutdown { "Shut down Hydra Mixer" };
    juce::Rectangle<int> divider, divider2;
};

//==============================================================================
SettingsPanel::SettingsPanel (UiContext& c) : ctx (c)
{
    addAndMakeVisible (tabs);
    tabs.setTabBarDepth (40);
    tabs.addTab ("General", theme::panel, new GeneralTab (c), true);
    tabs.addTab ("Audio Engine", theme::panel, new AudioTab (c, [this] { if (onEngineChanged) onEngineChanged(); }), true);
    tabs.addTab ("MIDI", theme::panel, new MidiTab (c), true);
    tabs.addTab ("Plugins", theme::panel, new PluginTab (c), true);
    tabs.addTab ("Virtual Cables", theme::panel, new CablesTab (c, [this] { if (onEngineChanged) onEngineChanged(); }), true);
    tabs.addTab ("Backup & Transfer", theme::panel, new BackupTab (c, [this] { if (onSessionReplaced) onSessionReplaced(); },
                                                                  [this] { if (onEngineChanged) onEngineChanged(); }), true);
}

SettingsPanel::~SettingsPanel() = default;

void SettingsPanel::paint (juce::Graphics& g) { g.fillAll (theme::bg); }
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
    styleLabel (hint);
    hint.setText ("Right-click any control -> MIDI Learn / MIDI Range, or use Learn Mode. Click a range value to type it (e.g. 0 or -inf).", juce::dontSendNotification);
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
    hint.setBounds (top);
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

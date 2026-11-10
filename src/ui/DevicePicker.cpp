#include "DevicePicker.h"
#include "state/Session.h"
#include "state/VacCables.h"
#include "engine/VacStream.h"
#include "engine/NetLink.h"

namespace hydra
{
namespace
{
const char* kLowLatency = "Windows Audio (Low Latency Mode)";
const char* kExclusive  = "Windows Audio (Exclusive Mode)";
const char* kShared     = "Windows Audio";

constexpr int kPad = 16, kGap = 12, kHeader = 58, kTitle = 24, kFooter = 52;
} // namespace

//==============================================================================
/** One column: section captions and clickable rows (text + optional second line). */
class DevicePicker::List final : public juce::Component
{
public:
    struct Row
    {
        juce::String text, detail;
        bool header = false, enabled = true;
        bool current = false;    // tick (channel) / dot (leads to the current choice)
        int index = -1;          // index into the model (-1 for captions)
        bool plus = false;       // an action ("+ Add a virtual cable"): plus icon, accent colour
        bool check = false;      // a check box instead of the dot (apps: several at once), ticked when current
        juce::String used;       // other channels on it ("In 2, A1"), drawn as a tag on the right
    };

    explicit List (juce::Colour a) : accent (a) {}

    std::function<void (int index)> onClick, onDoubleClick;
    bool ticks = false;          // the channel column marks the current row with a tick instead of a dot
    juce::String empty;          // shown when there are no rows

    void setRows (std::vector<Row> r, int sel)
    {
        rows = std::move (r);
        selected = sel;
        hover = -1;
        setSize (getWidth(), juce::jmax (1, totalHeight()));
        repaint();
    }
    void setSelected (int index) { if (index != selected) { selected = index; repaint(); } }
    /** Top of the row of a model index (to scroll it into view), -1 if absent. */
    int rowTop (int index) const
    {
        int y = 0;
        for (auto& r : rows)
        {
            if (! r.header && r.index == index) return y;
            y += heightOf (r);
        }
        return -1;
    }

    void paint (juce::Graphics& g) override
    {
        if (rows.empty())
        {
            g.setColour (theme::textFaint);
            g.setFont (theme::font (13.0f));
            g.drawFittedText (empty, getLocalBounds().reduced (10, 8).withHeight (60), juce::Justification::topLeft, 3);
            return;
        }
        int y = 0;
        for (size_t i = 0; i < rows.size(); ++i)
        {
            auto& r = rows[i];
            const int h = heightOf (r);
            auto area = juce::Rectangle<int> (0, y, getWidth(), h);
            y += h;
            if (r.header)
            {
                g.setColour (theme::textFaint);
                g.setFont (theme::font (11.0f, true));
                g.drawText (r.text.toUpperCase(), area.reduced (10, 0).withTrimmedTop (8), juce::Justification::centredLeft, true);
                continue;
            }
            auto box = area.reduced (2, 1).toFloat();
            const bool sel = r.index == selected, hov = (int) i == hover && r.enabled;
            if (sel || hov)
            {
                g.setColour (sel ? accent.withAlpha (0.16f) : theme::raised);
                g.fillRoundedRectangle (box, 6.0f);
                if (sel)
                {
                    g.setColour (accent.withAlpha (0.55f));
                    g.drawRoundedRectangle (box.reduced (0.5f), 6.0f, 1.0f);
                }
            }
            auto content = area.reduced (10, 0);
            auto mark = content.removeFromLeft (16).toFloat();
            if (r.used.isNotEmpty())
            {
                // Tag on the right: which other channels use it already.
                const auto f = theme::font (11.0f, true);
                const int tw = juce::jmin (content.getWidth() / 2, juce::GlyphArrangement::getStringWidthInt (f, r.used) + 12);
                auto tag = content.removeFromRight (tw).withHeight (18).withY (area.getY() + 7).toFloat();
                content.removeFromRight (6);
                g.setColour (theme::warn.withAlpha (0.16f));
                g.fillRoundedRectangle (tag, 4.0f);
                g.setColour (theme::warn.withAlpha (0.9f));
                g.setFont (f);
                g.drawText (r.used, tag.reduced (4.0f, 0.0f), juce::Justification::centred, true);
            }
            if (r.check)
            {
                const auto c = mark.withSizeKeepingCentre (13.0f, 13.0f).withY ((float) area.getY() + 9.0f);
                g.setColour (r.current ? theme::good : theme::textFaint);
                g.drawRoundedRectangle (c.reduced (0.5f), 3.0f, 1.3f);
                if (r.current)
                {
                    juce::Path p;
                    const auto t = c.reduced (3.0f);
                    p.startNewSubPath (t.getX(), t.getCentreY());
                    p.lineTo (t.getX() + t.getWidth() * 0.38f, t.getBottom());
                    p.lineTo (t.getRight(), t.getY());
                    g.strokePath (p, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                }
            }
            else if (r.current)
            {
                g.setColour (theme::good);
                if (ticks)
                {
                    juce::Path p;
                    const auto c = mark.withSizeKeepingCentre (11.0f, 11.0f).withY ((float) area.getY() + 10.0f);
                    p.startNewSubPath (c.getX(), c.getCentreY());
                    p.lineTo (c.getX() + c.getWidth() * 0.38f, c.getBottom());
                    p.lineTo (c.getRight(), c.getY());
                    g.strokePath (p, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                }
                else
                {
                    g.fillEllipse (mark.withSizeKeepingCentre (7.0f, 7.0f).withY ((float) area.getY() + 12.0f));
                }
            }
            if (r.plus)
            {
                const auto c = mark.withSizeKeepingCentre (16.0f, 16.0f).withY ((float) area.getY() + 7.0f);
                g.setColour (accent.withAlpha (r.enabled ? 1.0f : 0.4f));
                g.drawEllipse (c.reduced (0.75f), 1.5f);
                g.drawLine (c.getCentreX(), c.getY() + 4.5f, c.getCentreX(), c.getBottom() - 4.5f, 1.6f);
                g.drawLine (c.getX() + 4.5f, c.getCentreY(), c.getRight() - 4.5f, c.getCentreY(), 1.6f);
                content.removeFromLeft (6);
            }
            content.removeFromLeft (2);
            auto textCol = ! r.enabled ? (r.current ? theme::text : theme::textFaint) : r.current ? theme::text : sel ? theme::text : theme::text.withAlpha (0.86f);
            if (r.plus && r.enabled) textCol = accent.brighter (0.2f);
            g.setColour (textCol);
            g.setFont (theme::font (13.5f, sel || r.current));
            g.drawText (r.text, content.withHeight (30), juce::Justification::centredLeft, true);
            if (r.detail.isNotEmpty())
            {
                g.setColour (theme::textDim);
                g.setFont (theme::font (11.5f));
                g.drawText (r.detail, content.withTrimmedTop (24).withHeight (16), juce::Justification::centredLeft, true);
            }
        }
    }

    void mouseMove (const juce::MouseEvent& e) override { setHover (rowAt (e.y)); }
    void mouseExit (const juce::MouseEvent&) override   { setHover (-1); }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! e.mouseWasClicked()) return;
        const int i = rowAt (e.y);
        if (i < 0 || rows[(size_t) i].header || ! rows[(size_t) i].enabled) return;
        const int index = rows[(size_t) i].index;
        if (e.getNumberOfClicks() >= 2) { if (onDoubleClick) onDoubleClick (index); }
        else if (onClick) onClick (index);
    }

private:
    static int heightOf (const Row& r) { return r.header ? 30 : r.detail.isNotEmpty() ? 46 : 32; }
    int totalHeight() const { int h = 0; for (auto& r : rows) h += heightOf (r); return h + 4; }
    int rowAt (int y) const
    {
        int top = 0;
        for (size_t i = 0; i < rows.size(); ++i)
        {
            const int h = heightOf (rows[i]);
            if (y >= top && y < top + h) return (int) i;
            top += h;
        }
        return -1;
    }
    void setHover (int i)
    {
        if (i >= 0 && (rows[(size_t) i].header || ! rows[(size_t) i].enabled)) i = -1;
        if (i != hover) { hover = i; setMouseCursor (i >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor); repaint(); }
    }

    juce::Colour accent;
    std::vector<Row> rows;
    int selected = -1, hover = -1;
};

//==============================================================================
DevicePicker::DevicePicker (UiContext& c, int ch)
    : ctx (c), channel (ch), isInput (ch < kMaxInputs)
{
    const auto accent = isInput ? theme::accentIn : theme::busColour (ch - kMaxInputs);
    catList = std::make_unique<List> (accent);
    devList = std::make_unique<List> (accent);
    optList = std::make_unique<List> (accent);
    optList->ticks = true;

    for (auto* v : { &catView, &devView, &optView })
    {
        v->setScrollBarsShown (true, false);
        v->setScrollBarThickness (8);
        addAndMakeVisible (*v);
    }
    catView.setViewedComponent (catList.get(), false);
    devView.setViewedComponent (devList.get(), false);
    optView.setViewedComponent (optList.get(), false);

    catList->onClick = [this] (int i) { selectCategory (i); chooseFirstChannel(); };
    devList->onClick = [this] (int i) { selectDevice (i); chooseFirstChannel(); };
    devList->onDoubleClick = [this] (int i)
    {
        if (category >= 0 && categories[(size_t) category].apps)
        {
            // The first click ticked / unticked it already: make sure it is on, then close.
            if (i >= 0 && i < (int) categories[(size_t) category].devices.size())
                toggleApp (categories[(size_t) category].devices[(size_t) i].stream.deviceName, true);
            if (onClose) onClose();
            return;
        }
        selectDevice (i);
        if (device >= 0 && categories[(size_t) category].devices[(size_t) device].options.size() == 1 && onClose) onClose();
    };
    optList->onClick = [this] (int i)
    {
        auto& d = categories[(size_t) category].devices[(size_t) device];
        apply (d.options[(size_t) i].target);
    };
    optList->onDoubleClick = [this] (int i)
    {
        auto& d = categories[(size_t) category].devices[(size_t) device];
        apply (d.options[(size_t) i].target);
        if (onClose) onClose();
    };

    addAndMakeVisible (hint);

    refresh.setTooltip ("Look for new devices and apps");
    refresh.onClick = [this] { refreshDevices(); refreshApps(); };
    done.onClick = [this] { if (onClose) onClose(); };
    addAndMakeVisible (refresh);
    addAndMakeVisible (done);
    showAll.setTooltip ("Off: only devices that fit this channel (hardware outputs: hardware, virtual outputs: software; "
                        "Hydra's own cables not among the Windows devices). On: every device (virtual outputs never list hardware).");
    showAll.setToggleState (ctx.session != nullptr && ctx.session->prefs.pickerShowAll, juce::dontSendNotification);
    showAll.onClick = [this]
    {
        if (ctx.session == nullptr) return;
        ctx.session->prefs.pickerShowAll = showAll.getToggleState();
        ctx.session->prefsVersion.fetch_add (1);
        refreshDevices();
    };
    addAndMakeVisible (showAll);
    if (ctx.session != nullptr && ctx.session->vacCables != nullptr) ctx.session->vacCables->addChangeListener (this);

    setWantsKeyboardFocus (true);
    setSize (960, 580);

    buildModel();
    shown = current();
    // Open where the current choice is.
    int startCat = 0, startDev = -1;
    for (size_t ci = 0; ci < categories.size(); ++ci)
    {
        auto& cat = categories[ci];
        if (cat.isNone && shown.kind == IoTarget::Kind::None) { startCat = (int) ci; break; }
        bool found = false;
        for (size_t di = 0; di < cat.devices.size() && ! found; ++di)
            if (isCurrentDevice (cat.devices[di], shown)) { startCat = (int) ci; startDev = cat.apps ? -1 : (int) di; found = true; }
        if (found) break;
    }
    if (shown.kind == IoTarget::Kind::None && categories.size() > 1)
        startCat = 1;   // nothing chosen yet: start at the hardware, not at "None"
    selectCategory (startCat);
    if (startDev >= 0) selectDevice (startDev);
    refreshApps();
    startTimerHz (4);
}

void DevicePicker::refreshApps()
{
    if (appsLoading || ! isInput) return;
    appsLoading = true;
    juce::Component::SafePointer<DevicePicker> safe (this);
    listAudioAppsAsync ([safe] (juce::StringArray names)
    {
        if (safe == nullptr) return;
        safe->appsLoading = false;
        if (names == safe->appNames) return;
        safe->appNames = std::move (names);
        safe->refreshDevices();
    });
}

void DevicePicker::chooseFirstChannel()
{
    if (category < 0 || device < 0) return;
    const auto& d = categories[(size_t) category].devices[(size_t) device];
    if (d.stream.sameStream (shown) || d.options.empty() || ! d.options[0].enabled) return;
    apply (d.options[0].target);
}

DevicePicker::~DevicePicker()
{
    if (ctx.session != nullptr && ctx.session->vacCables != nullptr) ctx.session->vacCables->removeChangeListener (this);
    catView.setViewedComponent (nullptr, false);
    devView.setViewedComponent (nullptr, false);
    optView.setViewedComponent (nullptr, false);
}

juce::String DevicePicker::windowTitle (const UiContext& c, int ch)
{
    const bool in = ch < kMaxInputs;
    const auto& name = in ? c.engine.inputs[(size_t) ch].name : c.engine.buses[(size_t) (ch - kMaxInputs)].name;
    return "Hydra Mixer - " + juce::String (in ? "Input " : "Output ") + channelCode (ch) + (name.isNotEmpty() ? "  " + name : juce::String());
}

IoTarget DevicePicker::current() const
{
    return isInput ? ctx.engine.inputs[(size_t) channel].source : ctx.engine.buses[(size_t) (channel - kMaxInputs)].output;
}

void DevicePicker::apply (const IoTarget& t)
{
    if (t != current())
    {
        if (isInput) ctx.engine.setInputSource (channel, t);
        else         ctx.engine.setBusOutput (channel - kMaxInputs, t);
        if (onChanged) onChanged();
    }
    shown = current();
    showLists();
    repaint (headerArea);
}

//==============================================================================
void DevicePicker::buildModel()
{
    auto& engine = ctx.engine;
    categories.clear();
    const auto cur = current();
    const bool showEverything = ctx.session != nullptr && ctx.session->prefs.pickerShowAll;
    const bool expert = ctx.session != nullptr && ctx.session->prefs.expertDeviceModes;
    // Which kinds of devices fit this channel: hardware outputs (H) play to hardware, virtual outputs (V) to software.
    // A virtual output never offers hardware, not even with "Show all devices" (wish of the user, 9.11.): a device
    // it already uses stays listed (keep), so nothing vanishes from an older session.
    const bool hwBus = ! isInput && isHwBus (channel - kMaxInputs);
    const bool virtBus = ! isInput && ! hwBus;
    const bool wantHardware = isInput || hwBus || (showEverything && ! virtBus);
    const bool wantSoftware = isInput || ! hwBus || showEverything;

    // Software endpoints (virtual cables, Voicemeeter): listed in their own categories, not among the Windows devices.
    const auto cables = findVirtualCables (engine.getDeviceTypes());
    auto isVm = [] (const juce::String& n) { return n.containsIgnoreCase ("Voicemeeter"); };
    auto isSoftwareEndpoint = [&] (const juce::String& n)
    {
        if (vac::isCableEndpoint (n) || isVm (n)) return true;
        return std::any_of (cables.begin(), cables.end(), [&] (const VirtualCable& c) { return c.playbackName == n || c.recordingName == n; });
    };
    auto stereoOptions = [] (const IoTarget& stream)
    {
        IoTarget m1 = stream; m1.mono = true;
        IoTarget m2 = m1; m2.channel = 1;
        return std::vector<Option> { { "Stereo 1/2", {}, stream }, { "Mono 1", "left channel", m1 }, { "Mono 2", "right channel", m2 } };
    };
    // A category or device that does not fit stays listed while this channel uses it (nothing vanishes).
    auto keep = [&] (bool fits, const Category& cat)
    {
        return fits || std::any_of (cat.devices.begin(), cat.devices.end(), [&] (const Device& d) { return isCurrentDevice (d, cur); });
    };
    auto add = [&] (Category&& cat, bool fits) { if (keep (fits, cat)) categories.push_back (std::move (cat)); };

    auto channelOptions = [] (const juce::StringArray& names, const IoTarget& base)
    {
        std::vector<Option> out;
        for (int c = 0; c + 1 < names.size(); c += 2)
        {
            IoTarget t = base; t.channel = c; t.mono = false;
            out.push_back ({ "Stereo " + juce::String (c + 1) + "/" + juce::String (c + 2), names[c] + " + " + names[c + 1], t });
        }
        for (int c = 0; c < names.size(); ++c)
        {
            IoTarget t = base; t.channel = c; t.mono = true;
            out.push_back ({ "Mono " + juce::String (c + 1), names[c], t });
        }
        return out;
    };

    {
        Category none;
        none.name = "None";
        none.isNone = true;
        none.hint = isInput ? "The strip receives nothing." : "The bus is mixed but not sent to any device.";
        categories.push_back (std::move (none));
    }

    //--- Hardware
    if (auto* dev = engine.getMasterDevice())
    {
        Category cat;
        cat.section = "Hardware";
        cat.name = "Master: " + dev->getName();
        cat.hint = "Channels of the master device (" + dev->getTypeName() + ") run directly on its clock: no extra buffer, "
                   "the lowest latency Hydra has.";
        Device d;
        d.name = dev->getName();
        d.detail = dev->getTypeName() + " - master clock";
        d.stream.kind = IoTarget::Kind::Master;
        const auto names = isInput ? engine.getMasterInputNames() : engine.getMasterOutputNames();
        d.options = channelOptions (names, d.stream);
        if (names.isEmpty() && isInput)
            d.options.push_back ({ "(no inputs)", "Choose an ASIO driver as master for direct inputs (Settings > Audio Engine)", {}, false });
        cat.devices.push_back (std::move (d));
        add (std::move (cat), wantHardware);
    }

    if (const auto drivers = engine.getSecondaryAsioDrivers(); ! drivers.isEmpty())
    {
        Category cat;
        cat.section = "Hardware";
        cat.name = "Other ASIO Devices";
        cat.hint = "Another interface with its own clock, synchronised to the master (small clock-sync buffer). "
                   "All channels of one interface stay sample-accurate to each other.";
        for (auto& driver : drivers)
        {
            Device d;
            d.name = driver;
            d.detail = "ASIO - synced to the master clock";
            d.stream.kind = IoTarget::Kind::Device; d.stream.typeName = kAsioTypeName; d.stream.deviceName = driver;
            d.lazyAsio = true; d.loaded = false;   // probing a driver loads it: only when it is opened here
            cat.devices.push_back (std::move (d));
        }
        add (std::move (cat), wantHardware);
    }

    //--- Virtual cables (right after the hardware: the usual way to bring apps in / send to apps)
    {
        Category cat;
        cat.section = "Virtual";
        cat.name = "Virtual Cable";
        cat.hint = isInput ? "Apps play into the cable, Hydra records its other end."
                           : "Hydra plays into the cable, apps (Discord, OBS, ...) record its other end.";
        cat.empty = "No virtual cable installed (VB-Cable, Virtual Audio Cable). See Settings > Virtual Cables.";
        // Virtual Audio Cable with the direct connection: no list of cables. Choosing "Virtual Cable" gives the
        // channel a cable of its own (a free one, or one more), named "Hydra In 4" / "Hydra Out 1" in Windows;
        // only this channel's cable is shown. A cable no channel uses any more disappears from Windows.
        auto* vacs = ctx.session != nullptr ? ctx.session->vacCables : nullptr;
        const bool vacListed = vacs != nullptr && vacs->isAvailable() && isVacDirectEnabled();
        if (vacListed)
        {
            cat.autoVac = true;
            cat.hint = isInput ? "Hydra sets up a cable of its own for this input. Apps (games, browsers, Discord) play into it "
                                 "under its name in Windows; nothing else to do."
                               : "Hydra sets up a cable of its own for this output. Apps (Discord, OBS, ...) record from it "
                                 "under its name in Windows; nothing else to do.";
            if (const int k = vacs->cableOf (cur); k > 0)
            {
                const auto other = vacs->endpointName (k, ! isInput);
                const auto shortOther = other.upToLastOccurrenceOf (" (", false, false).trim();
                Device d;
                d.name = shortOther.isNotEmpty() ? shortOther : other;
                d.detail = (isInput ? "apps play to \"" : "apps record from \"") + d.name + "\"";
                d.stream = cur;
                d.options = { { "Stereo", d.detail, d.stream } };
                cat.devices.push_back (std::move (d));
            }
            else
            {
                Device d;
                d.name = isInput ? "Add a virtual cable to this input" : "Add a virtual cable to this output";
                d.detail = isInput ? "apps then play into \"" + DeviceNamer::cableNameFor (true, channel, ctx.engine.inputs[(size_t) channel].name) + "\""
                                   : "apps then record from \"" + DeviceNamer::cableNameFor (false, channel - kMaxInputs,
                                                                                              ctx.engine.buses[(size_t) (channel - kMaxInputs)].name) + "\"";
                d.createVac = true;
                cat.devices.push_back (std::move (d));
            }
        }
        for (auto& c : cables)
        {
            if (vacListed && vac::isCableEndpoint (c.playbackName)) continue;   // listed above
            Device d;
            d.name = c.label;
            d.detail = isInput ? "apps play to \"" + c.playbackName + "\"" : "apps record from \"" + c.recordingName + "\"";
            d.stream.kind = IoTarget::Kind::Device; d.stream.typeName = kLowLatency;
            d.stream.deviceName = isInput ? c.recordingName : c.playbackName;
            d.options = { { "Stereo", d.detail, d.stream } };
            cat.devices.push_back (std::move (d));
        }
        // Virtual Audio Cable without the direct connection: Hydra can still add a cable itself (driver restart).
        if (auto* vac = ctx.session != nullptr ? ctx.session->vacCables : nullptr; ! vacListed && vac != nullptr && vac->isAvailable()
            && vac->info().edition != "lite" && vac->info().numCables < 256)
        {
            Device d;
            d.name = isInput ? "+ New input cable for this channel" : "+ New output cable for this channel";
            d.detail = vac->freeCable() > 0 ? "takes a free cable and connects it here"
                                            : "Virtual Audio Cable gets one more cable, connected here";
            d.createVac = true;
            cat.devices.push_back (std::move (d));
        }
        add (std::move (cat), wantSoftware);
    }

    //--- Windows devices. One mode by default (Low Latency, shared: works with everything, other apps keep the
    // device); the expert setting adds Exclusive and Shared. A mode the channel uses stays listed anyway.
    auto windowsCategory = [&] (const char* typeName, juce::String name, juce::String hint, juce::String notice, bool offered)
    {
        Category cat;
        cat.section = isInput ? "Windows Recording Devices" : "Windows Playback Devices";
        cat.name = std::move (name);
        cat.hint = std::move (hint);
        cat.notice = std::move (notice);
        cat.empty = "No device of this kind.";
        if (auto* type = engine.findType (typeName))
            for (auto& devName : type->getDeviceNames (isInput))
            {
                Device d;
                d.name = devName;
                d.stream.kind = IoTarget::Kind::Device; d.stream.typeName = typeName; d.stream.deviceName = devName;
                // Cables, Voicemeeter: in their own categories (and Hydra's cables never belong here).
                if (! showEverything && isSoftwareEndpoint (devName) && ! d.stream.sameStream (cur)) continue;
                d.options = stereoOptions (d.stream);
                cat.devices.push_back (std::move (d));
            }
        add (std::move (cat), offered && wantHardware);
    };
    windowsCategory (kLowLatency, expert ? "Low Latency (shared)" : (isInput ? "Recording devices" : "Playback devices"),
                     expert ? "Windows shared mode with the smallest buffer the driver allows. Other apps can keep using the device. "
                              "Synchronised to the master clock."
                            : juce::String (isInput ? "Microphones and other inputs Windows knows. " : "Speakers, headphones and other outputs Windows knows. ")
                              + "Other apps can keep using the device; Hydra uses the smallest buffer it allows.\n\n"
                                "More modes (Exclusive, Shared): Settings > General > Devices.",
                     {}, true);
    windowsCategory (kExclusive, "Exclusive (lowest latency)",
                     "Hydra takes the device for itself: the lowest Windows latency, but no other app can use it meanwhile.",
                     "Exclusive: while Hydra uses this device, no other application can use it.", expert || showEverything);
    windowsCategory (kShared, "Shared (compatibility)",
                     "Standard Windows mode (about 10 ms buffers). Works with every device; use it if the other modes fail.",
                     {}, expert || showEverything);

    //--- Software
    if (isInput)
    {
        Category apps;
        apps.section = "Software";
        apps.name = "Application";
        apps.apps = true;
        apps.hint = "Records only the sound of a program, no driver needed. Tick several to hear them together on this "
                    "channel. If one is not running yet, Hydra connects as soon as it plays.";
        apps.empty = "No app is playing audio right now. Start playback in the app, then press Refresh.";
        auto running = appNames;   // filled in the background (refreshApps)
        if (cur.kind == IoTarget::Kind::App)
            for (auto& exe : cur.appNames()) running.addIfNotAlreadyThere (exe, true);
        for (auto& exe : running)
        {
            Device d;
            d.name = exe.endsWithIgnoreCase (".exe") ? exe.dropLastCharacters (4) : exe;
            d.stream = IoTarget::apps ({ exe });
            if (! appNames.contains (exe, true)) d.detail = "not playing right now";
            apps.devices.push_back (std::move (d));
        }
        categories.push_back (std::move (apps));
    }

    // Voicemeeter's virtual devices (VAIO, AUX, VAIO3), if installed: apps that still play to "Voicemeeter Input"
    // are picked up by loopback, Voicemeeter's own outputs (Out A1 / B1 ...) only deliver sound while it runs.
    {
        Category cat;
        cat.section = "Software";
        cat.name = "Voicemeeter";
        cat.hint = isInput ? "Voicemeeter's virtual devices. \"Apps play to\": catches what apps send to a Voicemeeter input "
                             "(works without Voicemeeter running). \"Voicemeeter output\": only has sound while Voicemeeter runs."
                           : "Plays into a Voicemeeter virtual input (only heard while Voicemeeter runs).";
        if (isInput)
            for (auto& name : listRenderEndpoints())
                if (isVm (name))
                {
                    Device d;
                    d.name = name;
                    d.detail = "apps play to it - Hydra listens (loopback)";
                    d.stream.kind = IoTarget::Kind::Loopback; d.stream.deviceName = name;
                    d.options = { { "Stereo", d.detail, d.stream } };
                    cat.devices.push_back (std::move (d));
                }
        if (auto* type = engine.findType (kLowLatency))
            for (auto& name : type->getDeviceNames (isInput))
                if (isVm (name))
                {
                    Device d;
                    d.name = name;
                    d.detail = isInput ? "Voicemeeter output" : "Voicemeeter input";
                    d.stream.kind = IoTarget::Kind::Device; d.stream.typeName = kLowLatency; d.stream.deviceName = name;
                    d.options = stereoOptions (d.stream);
                    cat.devices.push_back (std::move (d));
                }
        if (! cat.devices.empty()) add (std::move (cat), wantSoftware);   // only when Voicemeeter is installed
    }

    if (isInput)
    {
        Category cat;
        cat.section = "Software";
        cat.name = "Loopback (Playback Device)";
        cat.hint = "Records everything Windows plays on a playback device.";
        cat.empty = "No playback device found.";
        for (auto& name : listRenderEndpoints())
        {
            Device d;
            d.name = name;
            d.stream.kind = IoTarget::Kind::Loopback; d.stream.deviceName = name;
            // Listening to a cable's or Voicemeeter's input: that is what their own categories do.
            if (! showEverything && isSoftwareEndpoint (name) && ! d.stream.sameStream (cur)) continue;
            d.options = { { "Stereo", "what this device plays", d.stream } };
            cat.devices.push_back (std::move (d));
        }
        categories.push_back (std::move (cat));
    }

    //--- Network (Hydra Link): streams this computer hears / Hydras it can send to.
    {
        auto& hub = net::Hub::get();
        const bool running = hub.isRunning();
        Category cat;
        cat.section = "Network";
        cat.name = "Hydra Link";
        cat.hint = isInput ? "Audio from Hydra on another computer. There, choose \"Network\" as the output of a V bus and this "
                             "computer: its stream shows up here. Local network: uncompressed, a few ms; beyond it Opus."
                           : "Sends this output to Hydra on another computer, where it shows up as a source for its inputs. "
                             "Auto: uncompressed in the local network (lowest latency), Opus beyond it (internet, VPN).";
        cat.empty = ! running ? juce::String ("Hydra Link is off. Turn it on in Settings > Network (on both computers).")
                  : isInput ? juce::String ("No stream heard yet. On the other computer, choose Network as the output of a V bus.")
                            : juce::String ("No other Hydra found yet. It needs Hydra Link turned on; computers outside the local "
                                            "network are added in Settings > Network.");
        if (isInput)
        {
            bool haveCurrent = false;
            if (running)
                for (auto& st : hub.getStreams())
                {
                    Device d;
                    d.name = st.stream + " from " + st.sender;
                    d.detail = net::codecName (st.codec) + (st.codec == net::Codec::opus ? " " + juce::String (st.kbps) + " kbit/s" : juce::String())
                             + ", " + juce::String (st.rate / 1000.0, 1) + " kHz" + (st.lan ? juce::String() : ", over the internet");
                    d.stream = st.source().target();
                    d.options = { { "Stereo", d.detail, d.stream } };
                    haveCurrent |= d.stream.sameStream (cur);
                    cat.devices.push_back (std::move (d));
                }
            if (cur.kind == IoTarget::Kind::Network && ! haveCurrent)
            {
                const auto src = net::Source::parse (cur.deviceName);
                Device d;
                d.name = src.stream + " from " + src.sender;
                d.detail = "not heard right now";
                d.stream = cur;
                d.options = { { "Stereo", d.detail, d.stream } };
                cat.devices.insert (cat.devices.begin(), std::move (d));
            }
        }
        else
        {
            const auto& bus = engine.buses[(size_t) (channel - kMaxInputs)];
            const auto curDest = cur.kind == IoTarget::Kind::Network ? net::Destination::parse (cur.deviceName) : net::Destination();
            // The stream is called like the bus (kept when only the codec changes).
            auto streamName = curDest.stream.isNotEmpty() ? curDest.stream : net::cleanName (bus.name);
            if (streamName.isEmpty()) streamName = busLabel (channel - kMaxInputs);
            const bool encrypted = running && hub.getConfig().password.isNotEmpty();

            auto addHost = [&] (const juce::String& host, int port, bool lan, const juce::String& detail)
            {
                Device d;
                d.name = host;
                d.detail = detail;
                struct Choice { net::Codec codec; int kbps; const char* text; const char* info; };
                static const Choice choices[] = {
                    { net::Codec::automatic, 128, "Auto", "" },
                    { net::Codec::pcm24, 0, "PCM 24 (uncompressed)", "~2.5 Mbit/s, lowest latency" },
                    { net::Codec::pcm16, 0, "PCM 16", "~1.7 Mbit/s" },
                    { net::Codec::float32, 0, "PCM 32 float", "~3.3 Mbit/s, bit-exact" },
                    { net::Codec::opus, 96, "Opus 96 kbit/s", "compressed: Wi-Fi, internet" },
                    { net::Codec::opus, 128, "Opus 128 kbit/s", "compressed: Wi-Fi, internet" },
                    { net::Codec::opus, 192, "Opus 192 kbit/s", "compressed, near transparent" },
                    { net::Codec::opus, 256, "Opus 256 kbit/s", "compressed, transparent" } };
                for (auto& c : choices)
                {
                    net::Destination dest { host, port, streamName, c.codec, c.kbps > 0 ? c.kbps : 128 };
                    Option o { c.text, c.info, dest.target() };
                    if (c.codec == net::Codec::automatic)
                        o.detail = lan ? "uncompressed here (local network)" : "Opus 128 kbit/s (outside the local network)";
                    if (! lan && ! encrypted)
                    {
                        o.enabled = false;
                        o.detail = "needs a network password (Settings > Network)";
                    }
                    d.options.push_back (std::move (o));
                }
                d.stream = curDest.host.equalsIgnoreCase (host) ? cur : d.options[0].target;
                cat.devices.push_back (std::move (d));
            };
            juce::StringArray listed;
            if (running)
                for (auto& p : hub.getPeers())
                {
                    const auto host = p.name.isNotEmpty() ? p.name : p.address;
                    juce::String detail = p.name.isEmpty() ? juce::String ("added in Settings, no answer yet")
                                                           : p.address + (p.lan ? juce::String() : ", outside the local network");
                    if (p.name.isNotEmpty() && ! p.trusted)
                        detail << (p.deviceId == 0 ? ", older Hydra: update it"
                                   : p.pairing == net::PairState::asking ? ", waiting for the answer there"
                                   : p.pairing == net::PairState::denied ? ", denied there"
                                   : ", not connected yet: choosing it asks there");
                    addHost (host, p.port, p.lan, detail);
                    listed.add (host);
                }
            if (cur.kind == IoTarget::Kind::Network && ! listed.contains (curDest.host, true))
                addHost (curDest.host, curDest.port, net::Hub::isLocalAddress (curDest.host) || ! curDest.host.containsOnly ("0123456789."),
                         "not on the network right now");
        }
        add (std::move (cat), wantSoftware);
    }

    // Mark what other channels use already.
    for (auto& cat : categories)
        for (auto& d : cat.devices)
        {
            if (d.createVac) continue;
            d.used = usedBy (d.stream, false);
            for (auto& o : d.options)
                if (o.enabled && (d.options.size() > 1 || d.stream.kind == IoTarget::Kind::Master)) o.used = usedBy (o.target, true);
        }
}

bool DevicePicker::isCurrentDevice (const Device& d, const IoTarget& cur) const
{
    if (d.createVac) return false;
    if (d.stream.kind == IoTarget::Kind::App)
        return cur.kind == IoTarget::Kind::App && cur.appNames().contains (d.stream.deviceName, true);
    return d.stream.sameStream (cur);
}

juce::String DevicePicker::usedBy (const IoTarget& t, bool exact) const
{
    if (t.kind == IoTarget::Kind::None) return {};
    auto overlaps = [] (const IoTarget& a, const IoTarget& b)
    {
        const int a0 = a.channel, a1 = a.mono ? a.channel : a.channel + 1;
        const int b0 = b.channel, b1 = b.mono ? b.channel : b.channel + 1;
        return a0 <= b1 && b0 <= a1;
    };
    auto same = [&] (const IoTarget& other)
    {
        if (t.kind == IoTarget::Kind::App)
        {
            if (other.kind != IoTarget::Kind::App) return false;
            const auto mine = t.appNames(), theirs = other.appNames();
            return std::any_of (mine.begin(), mine.end(), [&] (const juce::String& e) { return theirs.contains (e, true); });
        }
        return t.sameStream (other) && (! exact || overlaps (t, other));
    };
    // Same direction only: an input and an output on the same device do not get in each other's way.
    juce::StringArray codes;
    if (isInput)
    {
        for (int i = 0; i < kMaxInputs; ++i)
            if (i != channel && ctx.engine.isInputActive (i) && same (ctx.engine.inputs[(size_t) i].source)) codes.add (channelCode (i));
    }
    else
    {
        for (int b = 0; b < kMaxBuses; ++b)
            if (kMaxInputs + b != channel && ctx.engine.isBusActive (b) && same (ctx.engine.buses[(size_t) b].output)) codes.add (channelCode (busSelectIndex (b)));
    }
    if (codes.size() > 3) return codes[0] + ", " + codes[1] + " +" + juce::String (codes.size() - 2);
    return codes.joinIntoString (", ");
}

void DevicePicker::toggleApp (const juce::String& exe, bool onlyAdd)
{
    const auto cur = current();
    auto set = cur.kind == IoTarget::Kind::App ? cur.appNames() : juce::StringArray();
    if (set.contains (exe, true)) { if (! onlyAdd) set.removeString (exe, true); }
    else set.add (exe);
    apply (IoTarget::apps (set));   // the last one unticked: nothing
}

void DevicePicker::loadOptions (Device& d)
{
    if (d.loaded) return;
    d.loaded = true;
    const auto names = ctx.engine.getAsioChannelNames (d.stream.deviceName, isInput);
    d.options.clear();
    if (names.isEmpty())
    {
        d.options.push_back ({ "(cannot be loaded)", juce::String ("No ") + (isInput ? "inputs" : "outputs")
                                                     + " - is the driver used by another program?", {}, false });
        return;
    }
    for (int c = 0; c + 1 < names.size(); c += 2)
    {
        IoTarget t = d.stream; t.channel = c; t.mono = false;
        d.options.push_back ({ "Stereo " + juce::String (c + 1) + "/" + juce::String (c + 2), names[c] + " + " + names[c + 1], t });
    }
    for (int c = 0; c < names.size(); ++c)
    {
        IoTarget t = d.stream; t.channel = c; t.mono = true;
        d.options.push_back ({ "Mono " + juce::String (c + 1), names[c], t });
    }
}

//==============================================================================
bool DevicePicker::showsVacCables() const
{
    auto* vac = ctx.session != nullptr ? ctx.session->vacCables : nullptr;
    return vac != nullptr && vac->isAvailable() && category >= 0 && categories[(size_t) category].name == "Virtual Cable";
}

void DevicePicker::changeListenerCallback (juce::ChangeBroadcaster*)
{
    auto* vac = ctx.session != nullptr ? ctx.session->vacCables : nullptr;
    if (vac == nullptr) return;
    if (vac->isBusy()) { cableProgress = vac->status(); repaint (noticeArea); return; }
    // Done (here or in the Cable Manager): the new / renamed cables show up at once.
    refreshDevices();
    shown = current();
    showLists();
    repaint (headerArea);
}

void DevicePicker::selectCategory (int index)
{
    if (index < 0 || index >= (int) categories.size()) return;
    category = index;
    device = -1;
    auto& cat = categories[(size_t) index];
    if (cat.isNone)
    {
        apply ({});
        return;
    }
    if (cat.devices.size() == 1 && ! cat.apps)
        device = 0;   // master: only one device, show its channels right away
    if (device >= 0) loadOptions (cat.devices[(size_t) device]);
    showLists();
    devView.setViewPosition (0, 0);
    optView.setViewPosition (0, 0);
}

void DevicePicker::selectDevice (int index)
{
    if (category < 0) return;
    auto& cat = categories[(size_t) category];
    if (index < 0 || index >= (int) cat.devices.size()) return;
    auto& d = cat.devices[(size_t) index];
    if (d.createVac) { createCable(); return; }
    if (cat.apps) { toggleApp (d.stream.deviceName, false); return; }   // several apps at once: tick / untick
    device = index;
    loadOptions (d);
    optView.setViewPosition (0, 0);
    // A device with a single way to use it (apps, cables, loopback) is chosen with the click on it.
    if (d.options.size() == 1 && d.options[0].enabled) apply (d.options[0].target);
    else showLists();
}

void DevicePicker::createCable()
{
    auto* vac = ctx.session != nullptr ? ctx.session->vacCables : nullptr;
    if (vac == nullptr || vac->isBusy()) return;
    vac->addCableFor (isInput, isInput ? channel : channel - kMaxInputs,
                      [safe = juce::Component::SafePointer<DevicePicker> (this)] (bool, const juce::String& msg)
    {
        if (safe == nullptr) return;
        safe->cableProgress = msg;
        safe->refreshDevices();
        safe->shown = safe->current();
        safe->showLists();
        safe->repaint();
        if (safe->onChanged) safe->onChanged();
    });
    cableProgress = vac->status();
    repaint (noticeArea);
}

void DevicePicker::showLists()
{
    const auto cur = shown;
    std::vector<List::Row> rows;
    juce::String section;
    for (size_t ci = 0; ci < categories.size(); ++ci)
    {
        auto& cat = categories[ci];
        if (cat.section != section && cat.section.isNotEmpty())
        {
            section = cat.section;
            rows.push_back ({ section, {}, true });
        }
        List::Row r;
        r.text = cat.name;
        r.index = (int) ci;
        r.current = cat.isNone ? cur.kind == IoTarget::Kind::None
                               : std::any_of (cat.devices.begin(), cat.devices.end(), [&] (const Device& d) { return isCurrentDevice (d, cur); });
        if (! cat.isNone && cat.devices.size() > 1)
            r.detail = juce::String ((int) cat.devices.size()) + " devices";
        rows.push_back (r);
    }
    catList->setRows (std::move (rows), category);

    rows.clear();
    if (category >= 0)
    {
        auto& cat = categories[(size_t) category];
        for (size_t di = 0; di < cat.devices.size(); ++di)
        {
            auto& d = cat.devices[di];
            List::Row row { d.name, d.detail, false, true, isCurrentDevice (d, cur), (int) di };
            row.plus = d.createVac;
            row.check = cat.apps;
            row.used = d.used;
            rows.push_back (row);
        }
        devList->empty = cat.isNone ? juce::String() : cat.empty;
        hint.setText (cat.name + "\n" + cat.hint);
        hint.setVisible (cat.hint.isNotEmpty());
    }
    devList->setRows (std::move (rows), device);

    rows.clear();
    const bool none = category >= 0 && categories[(size_t) category].isNone;
    const bool apps = category >= 0 && categories[(size_t) category].apps;
    optList->empty = apps ? juce::String ("Tick one or more apps: all of them play on this channel together.")
                   : device < 0 && ! none ? juce::String ("Choose a device.") : juce::String();
    if (apps && shown.kind == IoTarget::Kind::App)
    {
        // The apps column shows what is on the channel now.
        rows.push_back ({ "On this channel", {}, true });
        for (auto& exe : shown.appNames())
            rows.push_back ({ exe.endsWithIgnoreCase (".exe") ? exe.dropLastCharacters (4) : exe, "stereo, per-app capture", false, false, true, -1 });
    }
    if (category >= 0 && device >= 0)
    {
        auto& d = categories[(size_t) category].devices[(size_t) device];
        bool stereoHeader = false, monoHeader = false;
        for (size_t oi = 0; oi < d.options.size(); ++oi)
        {
            auto& o = d.options[oi];
            if (d.options.size() > 3 && o.enabled)
            {
                if (! o.target.mono && ! stereoHeader) { rows.push_back ({ "Stereo pairs", {}, true }); stereoHeader = true; }
                if (o.target.mono && ! monoHeader)     { rows.push_back ({ "Mono channels", {}, true }); monoHeader = true; }
            }
            List::Row row { o.text, o.detail, false, o.enabled, o.enabled && o.target == cur, (int) oi };
            row.used = o.used;
            rows.push_back (row);
        }
    }
    int selOpt = -1;
    for (auto& r : rows) if (r.current) selOpt = r.index;
    optList->setRows (std::move (rows), selOpt);
    resized();
    repaint (noticeArea);
}

void DevicePicker::refreshDevices()
{
    const auto catName = category >= 0 ? categories[(size_t) category].name : juce::String();
    const auto devName = category >= 0 && device >= 0 ? categories[(size_t) category].devices[(size_t) device].name : juce::String();
    const auto devPos = devView.getViewPosition();
    buildModel();
    category = device = -1;
    for (size_t ci = 0; ci < categories.size(); ++ci)
        if (categories[ci].name == catName) category = (int) ci;
    if (category < 0 && ! categories.empty()) category = juce::jmin (1, (int) categories.size() - 1);
    auto& cat = categories[(size_t) category];
    for (size_t di = 0; di < cat.devices.size(); ++di)
        if (cat.devices[di].name == devName) device = (int) di;
    if (device < 0 && cat.devices.size() == 1 && ! cat.apps) device = 0;
    if (device >= 0) loadOptions (cat.devices[(size_t) device]);
    showLists();
    devView.setViewPosition (devPos);
}

void DevicePicker::timerCallback()
{
    if (! isShowing()) return;
    // A slot borrowed for a room measurement counts as in use (it is not a channel).
    const bool active = isInput ? ctx.engine.isInputActive (channel) || ctx.engine.getMeasureInput() == channel
                                : ctx.engine.isBusActive (channel - kMaxInputs) || ctx.engine.getMeasureBus() == channel - kMaxInputs;
    if (! active)
    {
        stopTimer();
        if (onClose) onClose();
        return;
    }
    if (auto* vac = ctx.session != nullptr ? ctx.session->vacCables : nullptr; vac != nullptr && vac->isBusy())
        if (const auto st = vac->status(); st != cableProgress) { cableProgress = st; repaint (noticeArea); }
    if (ctx.session != nullptr)   // Settings: expert modes switched (or "Show all" in another picker)
        if (const int key = (ctx.session->prefs.expertDeviceModes ? 1 : 0) + (ctx.session->prefs.pickerShowAll ? 2 : 0); key != prefsKey)
        {
            const bool first = prefsKey < 0;
            prefsKey = key;
            showAll.setToggleState (ctx.session->prefs.pickerShowAll, juce::dontSendNotification);
            if (! first) refreshDevices();
        }
    if (current() != shown)   // changed elsewhere (import, restore, reorder)
    {
        shown = current();
        showLists();
        repaint (headerArea);
    }
    // The list of apps playing audio changes all the time: keep it fresh while it is shown.
    if (++tick % 8 == 0 && category >= 0 && categories[(size_t) category].name == "Application")
        refreshApps();
    // Hydra Link: streams / other Hydras come and go.
    if (category >= 0 && categories[(size_t) category].name == "Hydra Link")
        if (const auto n = net::Hub::get().getChangeCount(); n != netChanges) { netChanges = n; refreshDevices(); }
}

bool DevicePicker::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey || k == juce::KeyPress::returnKey)
    {
        if (onClose) onClose();
        return true;
    }
    return false;
}

//==============================================================================
void DevicePicker::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);
    const auto accent = isInput ? theme::accentIn : theme::busColour (channel - kMaxInputs);

    // Header: badge, channel name, what is chosen now.
    auto h = headerArea;
    auto badge = h.removeFromLeft (44).withSizeKeepingCentre (40, 30).toFloat();
    g.setColour (accent.withAlpha (0.18f));
    g.fillRoundedRectangle (badge, 7.0f);
    g.setColour (accent);
    g.setFont (theme::font (13.0f, true));
    const bool forMeasuring = isInput ? ctx.engine.getMeasureInput() == channel : ctx.engine.getMeasureBus() == channel - kMaxInputs;
    g.drawText (forMeasuring ? juce::String (isInput ? "MIC" : "OUT") : channelCode (channel), badge, juce::Justification::centred);
    h.removeFromLeft (10);
    const auto name = forMeasuring ? juce::String ("for the room measurement (not on a channel)")
                                   : isInput ? ctx.engine.inputs[(size_t) channel].name : ctx.engine.buses[(size_t) (channel - kMaxInputs)].name;
    g.setColour (theme::text);
    g.setFont (theme::font (17.0f, true));
    g.drawText ((isInput ? "Input source" : "Output device") + juce::String (name.isNotEmpty() ? "  -  " + name : juce::String()),
                h.removeFromTop (h.getHeight() / 2 + 2), juce::Justification::bottomLeft, true);
    g.setFont (theme::font (13.0f));
    g.setColour (theme::textDim);
    juce::String now = "Now: ";
    g.drawText (now, h, juce::Justification::topLeft, false);
    const int nowW = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), now);
    g.setColour (shown.kind == IoTarget::Kind::None ? theme::textFaint : theme::good);
    g.drawText (shown.describe(), h.withTrimmedLeft (nowW), juce::Justification::topLeft, true);

    // Column titles and column backgrounds.
    const char* titles[] = { "Connection", "Device", "Channels" };
    juce::Viewport* views[] = { &catView, &devView, &optView };
    for (int i = 0; i < 3; ++i)
    {
        g.setColour (theme::textDim);
        g.setFont (theme::font (12.0f, true));
        g.drawText (titles[i], columnTitles[i], juce::Justification::bottomLeft, false);
        g.setColour (theme::panel);
        g.fillRoundedRectangle (views[i]->getBounds().expanded (2).toFloat(), 8.0f);
        g.setColour (theme::outline);
        g.drawRoundedRectangle (views[i]->getBounds().expanded (2).toFloat().reduced (0.5f), 8.0f, 1.0f);
    }

    if (cableProgress.isNotEmpty() && category >= 0 && categories[(size_t) category].name == "Virtual Cable")
    {
        g.setColour (theme::textDim);
        g.setFont (theme::font (13.0f));
        g.drawFittedText (cableProgress, noticeArea, juce::Justification::centredLeft, 2, 0.9f);
    }
    else if (category >= 0 && categories[(size_t) category].notice.isNotEmpty())
    {
        auto area = noticeArea;
        Chip::drawMarker (g, Chip::Marker::attention, area.removeFromLeft (9).withSizeKeepingCentre (9, 9).toFloat());
        area.removeFromLeft (8);
        g.setColour (theme::warn);
        g.setFont (theme::font (13.0f));
        g.drawFittedText (categories[(size_t) category].notice, area, juce::Justification::centredLeft, 2, 0.9f);
    }
}

void DevicePicker::resized()
{
    auto r = getLocalBounds().reduced (kPad);
    headerArea = r.removeFromTop (kHeader).withTrimmedBottom (8);

    auto footer = r.removeFromBottom (kFooter).withTrimmedTop (14);
    done.setBounds (footer.removeFromRight (100));
    footer.removeFromRight (8);
    refresh.setBounds (footer.removeFromRight (100));
    noticeArea = footer.withTrimmedRight (12).withTrimmedLeft (4);

    auto titles = r.removeFromTop (kTitle);
    const int w = r.getWidth() - 2 * kGap;
    const int cw = w * 30 / 100, dw = w * 38 / 100;
    const int widths[] = { cw, dw, w - cw - dw };
    juce::Viewport* views[] = { &catView, &devView, &optView };
    List* lists[] = { catList.get(), devList.get(), optList.get() };
    for (int i = 0; i < 3; ++i)
    {
        columnTitles[i] = titles.removeFromLeft (widths[i]).withTrimmedLeft (4).withTrimmedBottom (6);
        titles.removeFromLeft (kGap);
        views[i]->setBounds (r.removeFromLeft (widths[i]).reduced (2, 4));
        r.removeFromLeft (kGap);
        const bool scroll = lists[i]->getHeight() > views[i]->getHeight();
        lists[i]->setSize (views[i]->getWidth() - (scroll ? views[i]->getScrollBarThickness() : 0), lists[i]->getHeight());
    }
    // The "i" after the "Device" title explains the chosen connection.
    const int tw = juce::GlyphArrangement::getStringWidthInt (theme::font (12.0f, true), "Device");
    showAll.setBounds (columnTitles[1].withLeft (columnTitles[1].getRight() - 140).withTrimmedRight (2).withSizeKeepingCentre (140, 22).translated (0, 3));
    hint.setBounds (columnTitles[1].getX() + tw + 8, columnTitles[1].getBottom() - InfoButton::kSize - 1, InfoButton::kSize, InfoButton::kSize);
}
} // namespace hydra

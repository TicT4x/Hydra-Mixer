#include "DevicePicker.h"

namespace hydra
{
namespace
{
const char* kLowLatency = "Windows Audio (Low Latency Mode)";
const char* kExclusive  = "Windows Audio (Exclusive Mode)";
const char* kShared     = "Windows Audio";

constexpr int kPad = 16, kGap = 12, kHeader = 58, kTitle = 24, kFooter = 52, kHintHeight = 44;
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
            if (r.current)
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
            content.removeFromLeft (2);
            const auto textCol = ! r.enabled ? theme::textFaint : r.current ? theme::text : sel ? theme::text : theme::text.withAlpha (0.86f);
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

    catList->onClick = [this] (int i) { selectCategory (i); };
    devList->onClick = [this] (int i) { selectDevice (i); };
    devList->onDoubleClick = [this] (int i)
    {
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

    hint.setFont (theme::font (12.5f));
    hint.setColour (juce::Label::textColourId, theme::textDim);
    hint.setJustificationType (juce::Justification::topLeft);
    hint.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (hint);

    refresh.setTooltip ("Look for new devices and apps");
    refresh.onClick = [this] { refreshDevices(); };
    done.onClick = [this] { if (onClose) onClose(); };
    addAndMakeVisible (refresh);
    addAndMakeVisible (done);

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
            if (cat.devices[di].stream.sameStream (shown)) { startCat = (int) ci; startDev = (int) di; found = true; }
        if (found) break;
    }
    if (shown.kind == IoTarget::Kind::None && categories.size() > 1)
        startCat = 1;   // nothing chosen yet: start at the hardware, not at "None"
    selectCategory (startCat);
    if (startDev >= 0) selectDevice (startDev);
    startTimerHz (4);
}

DevicePicker::~DevicePicker()
{
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
        categories.push_back (std::move (cat));
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
        categories.push_back (std::move (cat));
    }

    auto windowsCategory = [&] (const char* typeName, juce::String name, juce::String hint)
    {
        Category cat;
        cat.section = isInput ? "Windows Recording Devices" : "Windows Playback Devices";
        cat.name = std::move (name);
        cat.hint = std::move (hint);
        cat.empty = "No device of this kind.";
        if (auto* type = engine.findType (typeName))
            for (auto& devName : type->getDeviceNames (isInput))
            {
                Device d;
                d.name = devName;
                d.stream.kind = IoTarget::Kind::Device; d.stream.typeName = typeName; d.stream.deviceName = devName;
                IoTarget m1 = d.stream; m1.mono = true;
                IoTarget m2 = m1; m2.channel = 1;
                d.options = { { "Stereo 1/2", {}, d.stream }, { "Mono 1", "left channel", m1 }, { "Mono 2", "right channel", m2 } };
                cat.devices.push_back (std::move (d));
            }
        categories.push_back (std::move (cat));
    };
    windowsCategory (kLowLatency, "Low Latency (shared)",
                     "Windows shared mode with the smallest buffer the driver allows. Other apps can keep using the device. "
                     "Synchronised to the master clock.");
    windowsCategory (kExclusive, "Exclusive (lowest latency)",
                     "Hydra takes the device for itself: the lowest Windows latency, but no other app can use it meanwhile.");
    windowsCategory (kShared, "Shared (compatibility)",
                     "Standard Windows mode (about 10 ms buffers). Works with every device; use it if the other modes fail.");

    const auto cables = findVirtualCables (engine.getDeviceTypes());
    if (isInput)
    {
        Category apps;
        apps.section = "Software";
        apps.name = "Application";
        apps.hint = "Records only the sound of one program, no driver needed. If it is not running yet, Hydra connects "
                    "as soon as it plays.";
        apps.empty = "No app is playing audio right now. Start playback in the app, then press Refresh.";
        auto running = listAudioApps();
        const auto cur = current();
        if (cur.kind == IoTarget::Kind::App) running.addIfNotAlreadyThere (cur.deviceName, true);
        for (auto& exe : running)
        {
            Device d;
            d.name = exe;
            d.stream.kind = IoTarget::Kind::App; d.stream.deviceName = exe;
            d.options = { { "Stereo", "per-app capture", d.stream } };
            apps.devices.push_back (std::move (d));
        }
        categories.push_back (std::move (apps));
    }

    {
        Category cat;
        cat.section = "Software";
        cat.name = "Virtual Cable";
        cat.hint = isInput ? "Apps play into the cable, Hydra records its other end."
                           : "Hydra plays into the cable, apps (Discord, OBS, ...) record its other end.";
        cat.empty = "No virtual cable installed (VB-Cable, Virtual Audio Cable). See Settings > Virtual Cables.";
        for (auto& c : cables)
        {
            Device d;
            d.name = c.label;
            d.detail = isInput ? "apps play to \"" + c.playbackName + "\"" : "apps record from \"" + c.recordingName + "\"";
            d.stream.kind = IoTarget::Kind::Device; d.stream.typeName = kLowLatency;
            d.stream.deviceName = isInput ? c.recordingName : c.playbackName;
            d.options = { { "Stereo", d.detail, d.stream } };
            cat.devices.push_back (std::move (d));
        }
        categories.push_back (std::move (cat));
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
            d.options = { { "Stereo", "what this device plays", d.stream } };
            cat.devices.push_back (std::move (d));
        }
        categories.push_back (std::move (cat));
    }
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
    if (cat.devices.size() == 1)
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
    device = index;
    auto& d = cat.devices[(size_t) index];
    loadOptions (d);
    optView.setViewPosition (0, 0);
    // A device with a single way to use it (apps, cables, loopback) is chosen with the click on it.
    if (d.options.size() == 1 && d.options[0].enabled) apply (d.options[0].target);
    else showLists();
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
                               : std::any_of (cat.devices.begin(), cat.devices.end(), [&] (const Device& d) { return d.stream.sameStream (cur); });
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
            rows.push_back ({ d.name, d.detail, false, true, d.stream.sameStream (cur), (int) di });
        }
        devList->empty = cat.isNone ? juce::String() : cat.empty;
        hint.setText (cat.hint, juce::dontSendNotification);
    }
    devList->setRows (std::move (rows), device);

    rows.clear();
    const bool none = category >= 0 && categories[(size_t) category].isNone;
    optList->empty = device < 0 && ! none ? juce::String ("Choose a device.") : juce::String();
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
            rows.push_back ({ o.text, o.detail, false, o.enabled, o.enabled && o.target == cur, (int) oi });
        }
    }
    int selOpt = -1;
    for (auto& r : rows) if (r.current) selOpt = r.index;
    optList->setRows (std::move (rows), selOpt);
    resized();
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
    if (device < 0 && cat.devices.size() == 1) device = 0;
    if (device >= 0) loadOptions (cat.devices[(size_t) device]);
    showLists();
    devView.setViewPosition (devPos);
}

void DevicePicker::timerCallback()
{
    if (! isShowing()) return;
    const bool active = isInput ? ctx.engine.isInputActive (channel) : ctx.engine.isBusActive (channel - kMaxInputs);
    if (! active)
    {
        stopTimer();
        if (onClose) onClose();
        return;
    }
    if (current() != shown)   // changed elsewhere (import, restore, reorder)
    {
        shown = current();
        showLists();
        repaint (headerArea);
    }
    // The list of apps playing audio changes all the time: keep it fresh while it is shown.
    if (++tick % 8 == 0 && category >= 0 && categories[(size_t) category].name == "Application")
        refreshDevices();
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
    g.drawText (channelCode (channel), badge, juce::Justification::centred);
    h.removeFromLeft (10);
    const auto& name = isInput ? ctx.engine.inputs[(size_t) channel].name : ctx.engine.buses[(size_t) (channel - kMaxInputs)].name;
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
}

void DevicePicker::resized()
{
    auto r = getLocalBounds().reduced (kPad);
    headerArea = r.removeFromTop (kHeader).withTrimmedBottom (8);

    auto footer = r.removeFromBottom (kFooter).withTrimmedTop (14);
    done.setBounds (footer.removeFromRight (100));
    footer.removeFromRight (8);
    refresh.setBounds (footer.removeFromRight (100));

    hint.setBounds (r.removeFromBottom (kHintHeight).withTrimmedTop (10));

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
}
} // namespace hydra

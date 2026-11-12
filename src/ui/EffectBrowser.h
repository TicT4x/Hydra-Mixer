#pragma once
#include "Widgets.h"
#include "state/Presets.h"
#include <set>

namespace hydra
{
/** Dragging effects (from the effect browser) onto channels: the drag description is a juce::var object with
    "hydraFx" = "builtin" (+ "fx": fx id), "plugin" (+ "plugin": PluginDescription XML) or "preset" (+ "file"). */
namespace fxdrag
{
juce::var builtin (Fx);
juce::var plugin (const juce::PluginDescription&);
juce::var preset (const presets::Preset&);
bool isFxDrag (const juce::var&);
/** "Equalizer", "Pro-Q 3", "Vocal chain" (for the drag image and messages). */
juce::String describe (const juce::var&);
/** Whether the channel can take it (built-in effect allowed there and an instance free). */
bool canAdd (const Engine&, int channel, const juce::var&);
/** Inserts it into the channel's chain at index (-1 = end). done (first slot, count) runs after it was added
    (maybe after a question, e.g. a second Stems channel); messages near `near`. */
void add (UiContext&, int channel, const juce::var&, int index, juce::Component* near,
          std::function<void (int, int)> done = nullptr);
/** Where the mouse holds the dragged block's picture: in its header, near the grip (logical units). */
constexpr int kGrabX = 26, kGrabY = 14;
/** Small picture that follows the mouse while dragging. */
juce::Image dragImage (const juce::String& text, juce::Colour);
} // namespace fxdrag

/** Strips take dropped effects: the drop appends to the channel's chain; holding still over the strip for a
    moment shows the channel in the inspector, where the effect can be dropped at any place of the chain. */
class FxDropZone final : private juce::Timer
{
public:
    FxDropZone (UiContext& c, juce::Component& o, int selectIndex) : ctx (c), owner (o), channel (selectIndex) {}
    bool interested (const juce::var& d) const { return fxdrag::isFxDrag (d); }
    void enter (const juce::var&);
    void exit();
    void dropped (const juce::var&);
    /** Outline while an effect is over the strip (green: it can go there, grey: it cannot). */
    void paintOver (juce::Graphics&, juce::Rectangle<float> area) const;
    static constexpr int kOpenMs = 650;

private:
    void timerCallback() override;
    UiContext& ctx;
    juce::Component& owner;
    int channel;
    bool lit = false, allowed = false;
};

/** Effect browser at the left edge of the main window (like Ableton's browser, collapsed by default):
    built-in effects, VST3 plugins by vendor and saved chains; every effect folds out its presets. Drag an entry
    onto a strip (appended to its chain) or into the effect chain in the inspector (anywhere in it);
    double-click adds it to the selected channel. Right-click a preset: rename, delete, export. */
class EffectBrowser final : public juce::Component, private juce::Timer
{
public:
    explicit EffectBrowser (UiContext& c);
    ~EffectBrowser() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;
    std::function<void (const juce::String&)> onMessage;
    static constexpr int kWidth = 260;

    struct Entry;
    class Row;

private:
    void timerCallback() override;
    void rebuild();
    void toggle (const juce::String& key);
    void showMenu();
    void importPresets();
    std::vector<Entry> makeEntries() const;

    UiContext& ctx;
    juce::TextEditor search;
    Chip menuChip { "...", theme::text };
    juce::Viewport view;
    juce::Component list;
    std::vector<std::unique_ptr<Row>> rows;
    std::set<juce::String> open;   // keys of folded-out entries
    uint32_t presetsSeen = 0;
    int pluginsSeen = -1;
    std::unique_ptr<juce::FileChooser> chooser;
    friend class Row;
};

/** Narrow tab "EFFECTS" at the left edge, next to the effect browser; click folds it out / in. */
class BrowserTab final : public juce::Component, public juce::SettableTooltipClient
{
public:
    BrowserTab() { setTooltip ("Effect browser: drag effects and presets onto channels (Ctrl+Alt+B)"); setMouseCursor (juce::MouseCursor::PointingHandCursor); }
    std::function<void()> onClick;
    void setOpen (bool o) { if (o != open) { open = o; repaint(); } }
    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { hover = false; repaint(); }
    void mouseUp (const juce::MouseEvent& e) override  { if (e.mouseWasClicked() && onClick) onClick(); }
    static constexpr int kWidth = 22;

private:
    bool hover = false, open = false;
};
} // namespace hydra

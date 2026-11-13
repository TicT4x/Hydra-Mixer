#pragma once
#include "Widgets.h"

namespace hydra
{
/** Settings: a list of pages at the left, in groups (Audio: audio engine, virtual cables, plugins; Connections: network
    = Hydra Link + phones / tablets, MIDI; Application: general, display, backup). Each page is built when first shown. */
class SettingsPanel final : public juce::Component
{
public:
    /** In the order of the list; the audio engine always comes first. */
    enum Tab { audio = 0, cables, plugins, network, midi, general, display, backup };
    explicit SettingsPanel (UiContext& c);
    ~SettingsPanel() override;
    void lookAndFeelChanged() override;   // theme switched: the tabs keep their colour otherwise
    void paint (juce::Graphics&) override;
    void resized() override;
    void showTab (int i);
    /** Logs in hydra.log how long it took from openedAt (ms counter) until the window was drawn, if that was slow. */
    void measureFirstPaint (double openedAt) { paintFrom = openedAt; }
    void paintOverChildren (juce::Graphics&) override;

    std::function<void()> onEngineChanged;
    std::function<void()> onSessionReplaced;   // complete settings were loaded from a file

    static constexpr int kNavWidth = 210;

private:
    class Nav;
    struct Page { juce::String group, name, subtitle; std::unique_ptr<juce::Component> content; };
    double paintFrom = 0.0;
    UiContext& ctx;
    std::vector<Page> pages;
    std::unique_ptr<Nav> nav;
    int current = -1;
    juce::Rectangle<int> headerArea;   // title and subtitle of the page
};

/** Live table of all MIDI mappings. */
class MidiMapPanel final : public juce::Component, private juce::ListBoxModel, private juce::Timer
{
public:
    explicit MidiMapPanel (UiContext& c);
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    int getNumRows() override { return (int) rows.size(); }
    void paintListBoxItem (int, juce::Graphics&, int, int, bool) override {}
    juce::Component* refreshComponentForRow (int row, bool selected, juce::Component* existing) override;
    void timerCallback() override;
    void reload();

    struct Row;
    UiContext& ctx;
    juce::ListBox list;
    juce::TextButton learnButton { "Learn Mode" }, clearButton { "Clear All" };
    InfoButton hint;
    std::vector<MidiMapping> rows;
    uint32_t seenVersion = ~0u;
};

/** Generic floating tool window. */
class ToolWindow final : public juce::DocumentWindow
{
public:
    ToolWindow (const juce::String& title, juce::Component* content, int w, int h,
                int buttons = juce::DocumentWindow::closeButton)
        : juce::DocumentWindow (title, theme::bg, buttons)
    {
        setUsingNativeTitleBar (false);   // title bar drawn in the app style (theme::LookAndFeel)
        setTitleBarHeight (32);
        setContentOwned (content, false);
        setResizable (true, false);
        centreWithSize (w, h);
        placeOnAppDisplay (*this, true);   // on the screen of the main window, not always the primary one
        setVisible (true);
    }
    void closeButtonPressed() override
    {
        setVisible (false);
        if (onClose) onClose();
    }
    std::function<void()> onClose;   // after the close button hid the window
    void visibilityChanged() override
    {
        juce::DocumentWindow::visibilityChanged();
        if (! isVisible()) return;
        placeOnAppDisplay (*this, false);   // shown again: follow the app to its screen
        removeNativeWindowBorder (*this);
    }

    /** The content reaches the window's edge: the resize zone lies on top of it (native hit test), without a visible frame. */
    juce::BorderSize<int> getContentComponentBorder() const override
    {
        return { juce::DocumentWindow::getContentComponentBorder().getTop(), 0, 0, 0 };
    }

    /** Keys not used by the content (e.g. Ctrl+Z / Ctrl+Y: undo / redo, handled by the main window). */
    std::function<bool (const juce::KeyPress&)> onKey;
    bool keyPressed (const juce::KeyPress& k) override { return onKey != nullptr && onKey (k); }

    /** The title bar buttons are ordinary components, not native caption buttons: only then do they get the
        hover and pressed states and their clicks reliably. */
    WindowControlKind findControlAtPoint (juce::Point<float> p) const override
    {
        const auto kind = juce::DocumentWindow::findControlAtPoint (p);
        const bool isButton = kind == WindowControlKind::close || kind == WindowControlKind::minimise || kind == WindowControlKind::maximise;
        return isButton ? WindowControlKind::client : kind;
    }

    void activeWindowStatusChanged() override
    {
        juce::DocumentWindow::activeWindowStatusChanged();
        removeNativeWindowBorder (*this);   // also here: JUCE may have recreated the native window since it was shown
        // JUCE disables the buttons of an inactive window, which also switches off their hover state.
        for (auto* b : { getMinimiseButton(), getMaximiseButton(), getCloseButton() })
            if (b != nullptr) b->setEnabled (true);
        // Content that handles keys itself (EQ editor: Ctrl+Z) gets the focus when the window is activated.
        if (isActiveWindow())
            if (auto* c = getContentComponent(); c != nullptr && c->getWantsKeyboardFocus() && ! c->hasKeyboardFocus (true))
                c->grabKeyboardFocus();
    }

    /** Replaces the content (the old one is deleted). */
    void replaceContent (juce::Component* content, const juce::String& title)
    {
        setName (title);
        setContentOwned (content, false);
        repaint();
    }
};
} // namespace hydra

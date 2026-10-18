#pragma once
#include "Widgets.h"

namespace hydra
{
/** Tabbed settings: audio engine (master clock device), MIDI, plugins. */
class SettingsPanel final : public juce::Component
{
public:
    explicit SettingsPanel (UiContext& c);
    ~SettingsPanel() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void showTab (int i) { tabs.setCurrentTabIndex (i); }

    std::function<void()> onEngineChanged;
    std::function<void()> onSessionReplaced;   // complete settings were loaded from a file

private:
    UiContext& ctx;
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
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
    juce::Label hint;
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
        setVisible (true);
    }
    void closeButtonPressed() override { setVisible (false); }

    /** Keys not used by the content (e.g. Ctrl+Z / Ctrl+Y: undo / redo, handled by the main window). */
    std::function<bool (const juce::KeyPress&)> onKey;
    bool keyPressed (const juce::KeyPress& k) override { return onKey != nullptr && onKey (k); }

    void activeWindowStatusChanged() override
    {
        juce::DocumentWindow::activeWindowStatusChanged();
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

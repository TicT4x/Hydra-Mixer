#pragma once
#include "core/Params.h"

namespace hydra
{
/** Keyboard shortcuts for switch-like parameters (mute, solo, routing, on/off, choices).

    Shortcuts with Ctrl or Alt, and F-keys, are registered system-wide (RegisterHotKey), so they
    work while a game or another app has focus. Plain keys only act while Hydra has focus,
    so they never steal typing from other programs. If Windows refuses a global registration
    (another app owns the combination), the shortcut still works while Hydra is focused.

    Mouse buttons (middle, thumb buttons 4/5, optionally with Ctrl/Alt/Shift) are always global.
    Windows has no hotkey registration for them, so a low-level mouse hook watches them; it is
    only installed while a mouse binding exists or a new shortcut is being captured. The click
    itself is passed on to the other app. */
class Hotkeys
{
public:
    struct Binding
    {
        juce::String paramId;
        Param* param = nullptr;
        juce::KeyPress key;
        bool global = false;      // registered with Windows
    };

    explicit Hotkeys (ParamRegistry& registry);
    ~Hotkeys();

    /** Assigns key to p (replacing p's previous key and any other parameter using the same key). */
    void assign (Param& p, const juce::KeyPress& key);
    void remove (const Param* p);
    void clearAll();
    /** Channels were reordered: shortcuts of the parameters in moves (first) now trigger the parameter
        that holds their value (second). */
    void remapParams (const std::vector<std::pair<Param*, Param*>>& moves);
    int countBindings (const std::vector<std::pair<Param*, Param*>>& moves) const;
    juce::KeyPress getKeyFor (const Param* p) const;
    std::vector<Binding> getBindings() const { return bindings; }

    /** Key press while a Hydra window has focus. Returns true if it triggered something. */
    bool handleLocalKey (const juce::KeyPress& key);

    /** While suspended (assigning a new key) no global shortcuts are registered. */
    void setSuspended (bool shouldBeSuspended);

    static bool canAssign (const Param& p) noexcept { return p.type == ParamType::Toggle || ! p.choices.isEmpty(); }
    static bool wantsGlobal (const juce::KeyPress& key) noexcept;
    static void trigger (Param& p) noexcept;
    static juce::String describe (const juce::KeyPress& key);

    /** Mouse buttons as KeyPress codes (outside JUCE's key code range): 3 = middle, 4/5 = thumb buttons. */
    static constexpr int kMouseCodeBase = 0x7ff00000;
    static bool isMouseButton (const juce::KeyPress& key) noexcept { return (key.getKeyCode() & 0x7fffff00) == kMouseCodeBase; }

    /** While set (capturing a new shortcut), mouse buttons 3-5 are passed here instead of triggering. */
    void setMouseCapture (std::function<void (const juce::KeyPress&)> callback);

    std::unique_ptr<juce::XmlElement> toXml() const;
    void fromXml (const juce::XmlElement& xml);

    /** Bumped on every change (UI refresh, autosave). */
    std::atomic<uint32_t> version { 0 };

private:
    void registerAll();
    void unregisterAll();
    void onGlobalHotkey (int id);
    void onMouseButton (const juce::KeyPress& key);
    void updateMouseHook();

    struct Window;
    ParamRegistry& params;
    std::vector<Binding> bindings;
    std::unique_ptr<Window> window;
    bool suspended = false;
    std::function<void (const juce::KeyPress&)> mouseCapture;
    void* mouseHook = nullptr; // HHOOK
};
} // namespace hydra

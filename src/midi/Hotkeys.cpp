#include "Hotkeys.h"
#include <windows.h>

namespace hydra
{
namespace
{
constexpr int kFirstId = 0x4800;
constexpr UINT kMouseMsg = WM_APP + 0x48;   // posted by the mouse hook: wParam = button (3..5), lParam = JUCE modifier flags
HWND hookTarget = nullptr;                  // window that receives kMouseMsg (one Hotkeys instance per app)

LRESULT CALLBACK mouseHookProc (int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && hookTarget != nullptr && (wp == WM_MBUTTONDOWN || wp == WM_XBUTTONDOWN))
    {
        int button = 3;
        if (wp == WM_XBUTTONDOWN)
            button = HIWORD (((const MSLLHOOKSTRUCT*) lp)->mouseData) == XBUTTON2 ? 5 : 4;
        int mods = 0;
        if (GetAsyncKeyState (VK_CONTROL) & 0x8000) mods |= juce::ModifierKeys::ctrlModifier;
        if (GetAsyncKeyState (VK_MENU) & 0x8000)    mods |= juce::ModifierKeys::altModifier;
        if (GetAsyncKeyState (VK_SHIFT) & 0x8000)   mods |= juce::ModifierKeys::shiftModifier;
        // Keep the hook fast (Windows drops slow hooks): handle it later on the message thread.
        PostMessageW (hookTarget, kMouseMsg, (WPARAM) button, (LPARAM) mods);
    }
    return CallNextHookEx (nullptr, code, wp, lp);
}

/** JUCE key code -> Windows virtual key (0 = not representable). */
UINT toVirtualKey (const juce::KeyPress& k)
{
    const int code = k.getKeyCode();
    if ((code & 0x10000) != 0) return (UINT) (code & 0xffff);  // JUCE's extendedKeyModifier (F-keys, arrows, numpad...)
    if ((code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9')) return (UINT) code;
    if (code >= 'a' && code <= 'z') return (UINT) (code - 'a' + 'A');
    const SHORT vk = VkKeyScanW ((WCHAR) code);
    return vk == -1 ? 0u : (UINT) LOBYTE (vk);
}

UINT toWinModifiers (const juce::KeyPress& k)
{
    const auto m = k.getModifiers();
    UINT r = MOD_NOREPEAT;
    if (m.isCtrlDown())  r |= MOD_CONTROL;
    if (m.isAltDown())   r |= MOD_ALT;
    if (m.isShiftDown()) r |= MOD_SHIFT;
    return r;
}

bool isFunctionKey (const juce::KeyPress& k)
{
    const int code = k.getKeyCode();
    return code >= juce::KeyPress::F1Key && code <= juce::KeyPress::F24Key;
}
} // namespace

//==============================================================================
/** Hidden message-only window that receives WM_HOTKEY on the message thread. */
struct Hotkeys::Window
{
    explicit Window (Hotkeys& o) : owner (o)
    {
        WNDCLASSEXW wc {};
        wc.cbSize = sizeof (wc);
        wc.lpfnWndProc = &Window::proc;
        wc.hInstance = GetModuleHandleW (nullptr);
        wc.lpszClassName = L"HydraMixerHotkeys";
        RegisterClassExW (&wc); // fails harmlessly if already registered
        hwnd = CreateWindowExW (0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        if (hwnd != nullptr)
            SetWindowLongPtrW (hwnd, GWLP_USERDATA, (LONG_PTR) this);
    }

    ~Window()
    {
        if (hwnd != nullptr) DestroyWindow (hwnd);
    }

    static LRESULT CALLBACK proc (HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_HOTKEY)
            if (auto* self = (Window*) GetWindowLongPtrW (h, GWLP_USERDATA))
            {
                self->owner.onGlobalHotkey ((int) wp);
                return 0;
            }
        if (msg == kMouseMsg)
            if (auto* self = (Window*) GetWindowLongPtrW (h, GWLP_USERDATA))
            {
                self->owner.onMouseButton (juce::KeyPress (Hotkeys::kMouseCodeBase + (int) wp, juce::ModifierKeys ((int) lp), 0));
                return 0;
            }
        return DefWindowProcW (h, msg, wp, lp);
    }

    Hotkeys& owner;
    HWND hwnd = nullptr;
};

//==============================================================================
Hotkeys::Hotkeys (ParamRegistry& registry) : params (registry)
{
    if (juce::MessageManager::getInstanceWithoutCreating() != nullptr)
        window = std::make_unique<Window> (*this);
}

Hotkeys::~Hotkeys()
{
    unregisterAll();
    mouseCapture = nullptr;
    bindings.clear();
    updateMouseHook();
}

juce::String Hotkeys::describe (const juce::KeyPress& key)
{
    if (! isMouseButton (key)) return key.getTextDescriptionWithIcons();
    juce::String s;
    const auto m = key.getModifiers();
    if (m.isCtrlDown())  s << "Ctrl + ";
    if (m.isAltDown())   s << "Alt + ";
    if (m.isShiftDown()) s << "Shift + ";
    return s + "Mouse " + juce::String (key.getKeyCode() - kMouseCodeBase);
}

void Hotkeys::setMouseCapture (std::function<void (const juce::KeyPress&)> callback)
{
    mouseCapture = std::move (callback);
    updateMouseHook();
}

void Hotkeys::updateMouseHook()
{
    bool needed = mouseCapture != nullptr;
    for (auto& b : bindings) needed |= b.param != nullptr && isMouseButton (b.key);
    needed &= window != nullptr && window->hwnd != nullptr;

    if (needed && mouseHook == nullptr)
    {
        hookTarget = window->hwnd;
        mouseHook = SetWindowsHookExW (WH_MOUSE_LL, &mouseHookProc, GetModuleHandleW (nullptr), 0);
    }
    else if (! needed && mouseHook != nullptr)
    {
        UnhookWindowsHookEx ((HHOOK) mouseHook);
        mouseHook = nullptr;
        hookTarget = nullptr;
    }
}

void Hotkeys::onMouseButton (const juce::KeyPress& key)
{
    if (mouseCapture != nullptr)
    {
        auto cb = mouseCapture; // the callback may clear the capture
        cb (key);
        return;
    }
    if (suspended) return;
    for (auto& b : bindings)
        if (b.param != nullptr && b.key == key)
        {
            trigger (*b.param);
            return;
        }
}

bool Hotkeys::wantsGlobal (const juce::KeyPress& key) noexcept
{
    if (isMouseButton (key)) return true;
    const auto m = key.getModifiers();
    return m.isCtrlDown() || m.isAltDown() || isFunctionKey (key);
}

void Hotkeys::trigger (Param& p) noexcept
{
    if (! p.choices.isEmpty())
        p.set ((float) ((juce::roundToInt (p.get()) + 1) % p.choices.size()));
    else
        p.toggle();
}

void Hotkeys::assign (Param& p, const juce::KeyPress& key)
{
    unregisterAll();
    bindings.erase (std::remove_if (bindings.begin(), bindings.end(),
                                    [&] (const Binding& b) { return b.param == &p || b.key == key; }),
                    bindings.end());
    Binding b;
    b.paramId = p.id;
    b.param = &p;
    b.key = key;
    bindings.push_back (b);
    registerAll();
    updateMouseHook();
    ++version;
}

void Hotkeys::remove (const Param* p)
{
    unregisterAll();
    bindings.erase (std::remove_if (bindings.begin(), bindings.end(), [p] (const Binding& b) { return b.param == p; }),
                    bindings.end());
    registerAll();
    updateMouseHook();
    ++version;
}

void Hotkeys::clearAll()
{
    unregisterAll();
    bindings.clear();
    updateMouseHook();
    ++version;
}

void Hotkeys::remapParams (const std::vector<std::pair<Param*, Param*>>& moves)
{
    unregisterAll();
    for (auto& b : bindings)
        for (auto& [from, to] : moves)
            if (b.param == from)
            {
                b.param = to;
                b.paramId = to->id;
                break;
            }
    registerAll();
    ++version;
}

int Hotkeys::countBindings (const std::vector<std::pair<Param*, Param*>>& moves) const
{
    int n = 0;
    for (auto& b : bindings)
        for (auto& m : moves)
            if (b.param == m.first) { ++n; break; }
    return n;
}

juce::KeyPress Hotkeys::getKeyFor (const Param* p) const
{
    for (auto& b : bindings)
        if (b.param == p) return b.key;
    return {};
}

bool Hotkeys::handleLocalKey (const juce::KeyPress& key)
{
    for (auto& b : bindings)
        if (b.param != nullptr && ! b.global && b.key == key)
        {
            trigger (*b.param);
            return true;
        }
    return false;
}

void Hotkeys::setSuspended (bool s)
{
    if (s == suspended) return;
    suspended = s;
    if (s) unregisterAll();
    else   registerAll();
}

void Hotkeys::registerAll()
{
    if (suspended || window == nullptr || window->hwnd == nullptr) return;
    for (size_t i = 0; i < bindings.size(); ++i)
    {
        auto& b = bindings[i];
        if (isMouseButton (b.key)) { b.global = b.param != nullptr; continue; } // handled by the mouse hook
        const UINT vk = toVirtualKey (b.key);
        b.global = b.param != nullptr && vk != 0 && wantsGlobal (b.key)
                   && RegisterHotKey (window->hwnd, kFirstId + (int) i, toWinModifiers (b.key), vk);
    }
}

void Hotkeys::unregisterAll()
{
    for (size_t i = 0; i < bindings.size(); ++i)
        if (bindings[i].global && ! isMouseButton (bindings[i].key))
        {
            if (window != nullptr) UnregisterHotKey (window->hwnd, kFirstId + (int) i);
            bindings[i].global = false;
        }
}

void Hotkeys::onGlobalHotkey (int id)
{
    const int i = id - kFirstId;
    if (juce::isPositiveAndBelow (i, (int) bindings.size()) && bindings[(size_t) i].param != nullptr)
        trigger (*bindings[(size_t) i].param);
}

//==============================================================================
std::unique_ptr<juce::XmlElement> Hotkeys::toXml() const
{
    auto xml = std::make_unique<juce::XmlElement> ("HOTKEYS");
    for (auto& b : bindings)
    {
        auto* e = xml->createNewChildElement ("KEY");
        e->setAttribute ("param", b.paramId);
        e->setAttribute ("key", b.key.getTextDescription());
    }
    return xml;
}

void Hotkeys::fromXml (const juce::XmlElement& xml)
{
    unregisterAll();
    bindings.clear();
    for (auto* e : xml.getChildWithTagNameIterator ("KEY"))
    {
        Binding b;
        b.paramId = e->getStringAttribute ("param");
        b.param = params.find (b.paramId);
        b.key = juce::KeyPress::createFromDescription (e->getStringAttribute ("key"));
        if (b.param != nullptr && b.key.isValid())
            bindings.push_back (b);
    }
    registerAll();
    updateMouseHook();
    ++version;
}
} // namespace hydra

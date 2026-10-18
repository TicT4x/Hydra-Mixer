#include "Dialog.h"

namespace hydra::dialog
{
namespace
{
constexpr int kPad = 24, kIcon = 38, kIconGap = 16, kButtonH = 34, kButtonGap = 8, kMinWidth = 440, kMaxWidth = 600;

juce::Colour kindColour (Kind k)
{
    switch (k)
    {
        case Kind::warning: return theme::warn;
        case Kind::danger:  return theme::mute;
        case Kind::info:
        case Kind::question:
        default:            return theme::accentIn;
    }
}

class DialogButton final : public juce::Component
{
public:
    DialogButton (Button b, juce::Colour accent) : spec (std::move (b)), accent (accent)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }
    std::function<void()> onClick;
    const Button spec;

    int idealWidth() const
    {
        return juce::jmax (96, juce::GlyphArrangement::getStringWidthInt (theme::font (13.5f, true), spec.text) + 36);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        using Role = Button::Role;
        const bool filled = spec.role == Role::primary || spec.role == Role::destructive;
        auto fill = spec.role == Role::destructive ? theme::mute : spec.role == Role::primary ? accent : theme::raised;
        if (down)       fill = fill.brighter (0.18f);
        else if (hover) fill = fill.brighter (0.09f);
        g.setColour (fill);
        g.fillRoundedRectangle (r, 8.0f);
        if (! filled)
        {
            g.setColour (hover ? theme::textFaint : theme::outline);
            g.drawRoundedRectangle (r, 8.0f, 1.0f);
        }
        g.setColour (filled ? (spec.role == Role::destructive ? juce::Colours::white : theme::bg) : theme::text);
        g.setFont (theme::font (13.5f, true));
        g.drawText (spec.text, getLocalBounds(), juce::Justification::centred, true);
    }
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { hover = false; repaint(); }
    void mouseDown (const juce::MouseEvent&) override  { down = true; repaint(); }
    void mouseUp (const juce::MouseEvent& e) override
    {
        down = false;
        repaint();
        if (e.mouseWasClicked() && contains (e.getPosition()) && onClick) onClick();
    }

private:
    juce::Colour accent;
    bool hover = false, down = false;
};

class Overlay final : public juce::Component, private juce::ComponentListener
{
public:
    Overlay (juce::Component& h, Kind k, juce::String t, juce::String m, std::vector<Button> b, std::function<void (int)> d)
        : host (&h), kind (k), title (std::move (t)), text (std::move (m)), done (std::move (d))
    {
        for (size_t i = 0; i < b.size(); ++i)
        {
            auto btn = std::make_unique<DialogButton> (b[i], kindColour (kind));
            btn->onClick = [this, i] { finish ((int) i); };
            addAndMakeVisible (*btn);
            buttons.push_back (std::move (btn));
        }
        setWantsKeyboardFocus (true);
        setAlwaysOnTop (true);
        host->addAndMakeVisible (this);
        host->addComponentListener (this);
        setBounds (host->getLocalBounds());
        toFront (true);
        enterModalState (true, nullptr, false);
        grabKeyboardFocus();
        setAlpha (0.0f);
        juce::Desktop::getInstance().getAnimator().animateComponent (this, getBounds(), 1.0f, 110, false, 1.0, 1.0);
    }

    ~Overlay() override
    {
        if (host != nullptr) host->removeComponentListener (this);
    }

    void resized() override
    {
        // Card size: wide enough for the buttons and the title, the message wraps.
        int buttonsW = 0;
        for (auto& b : buttons) buttonsW += b->idealWidth() + kButtonGap;
        buttonsW -= kButtonGap;
        const int titleW = juce::GlyphArrangement::getStringWidthInt (theme::font (17.0f, true), title) + kIcon + kIconGap;
        const int maxW = juce::jmax (280, juce::jmin (kMaxWidth, getWidth() - 32));
        const int w = juce::jlimit (juce::jmin (kMinWidth, maxW), maxW, juce::jmax (buttonsW, titleW) + 2 * kPad);
        const int textW = w - 2 * kPad - kIcon - kIconGap;

        titleLayout = makeLayout (title, theme::font (17.0f, true), theme::text, (float) textW);
        messageLayout = makeLayout (text, theme::font (13.5f), theme::textDim, (float) textW);
        const int textH = (int) std::ceil (titleLayout.getHeight()) + (text.isNotEmpty() ? 8 + (int) std::ceil (messageLayout.getHeight()) : 0);
        const int h = kPad + juce::jmax (kIcon, textH) + 26 + kButtonH + kPad - 4;
        card = juce::Rectangle<int> (w, juce::jmin (h, getHeight() - 24)).withCentre (getLocalBounds().getCentre());

        auto row = juce::Rectangle<int> (card.getX() + kPad, card.getBottom() - kPad + 4 - kButtonH, card.getWidth() - 2 * kPad, kButtonH);
        for (auto it = buttons.rbegin(); it != buttons.rend(); ++it)
        {
            (*it)->setBounds (row.removeFromRight (juce::jmin ((*it)->idealWidth(), row.getWidth())));
            row.removeFromRight (kButtonGap);
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.55f));

        const auto c = card.toFloat();
        juce::Path shape;
        shape.addRoundedRectangle (c, 12.0f);
        juce::DropShadow (juce::Colours::black.withAlpha (0.6f), 28, { 0, 8 }).drawForPath (g, shape);
        g.setColour (theme::panel);
        g.fillPath (shape);
        g.setColour (theme::outline);
        g.strokePath (shape, juce::PathStrokeType (1.0f));

        // Coloured line along the top edge.
        const auto colour = kindColour (kind);
        {
            juce::Graphics::ScopedSaveState s (g);
            g.reduceClipRegion (shape);
            juce::ColourGradient grad (colour.withAlpha (0.15f), c.getX(), c.getY(), colour.withAlpha (0.0f), c.getRight(), c.getY(), false);
            grad.addColour (0.35, colour.withAlpha (0.9f));
            g.setGradientFill (grad);
            g.fillRect (c.withHeight (3.0f));
        }

        // Symbol: ring with "?" / "!" / "i".
        auto icon = juce::Rectangle<float> ((float) card.getX() + kPad, (float) card.getY() + kPad - 2, (float) kIcon, (float) kIcon);
        g.setColour (colour.withAlpha (0.14f));
        g.fillEllipse (icon);
        g.setColour (colour.withAlpha (0.8f));
        g.drawEllipse (icon.reduced (0.75f), 1.5f);
        g.setColour (colour);
        g.setFont (theme::font (19.0f, true));
        g.drawText (kind == Kind::question ? "?" : kind == Kind::info ? "i" : "!", icon.translated (0.0f, -0.5f), juce::Justification::centred, false);

        auto textArea = juce::Rectangle<float> (icon.getRight() + kIconGap, (float) card.getY() + kPad - 1,
                                                (float) (card.getRight() - kPad) - (icon.getRight() + kIconGap), (float) card.getHeight());
        titleLayout.draw (g, textArea);
        textArea.removeFromTop (titleLayout.getHeight() + 8.0f);
        messageLayout.draw (g, textArea.withBottom ((float) (card.getBottom() - kPad - kButtonH - 10)));
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::escapeKey) { finish (-1); return true; }
        if (k == juce::KeyPress::returnKey)
        {
            for (size_t i = 0; i < buttons.size(); ++i)
                if (buttons[i]->spec.role == Button::Role::primary || buttons[i]->spec.role == Button::Role::destructive)
                {
                    finish ((int) i);
                    return true;
                }
            if (buttons.size() == 1) finish (0);
            return true;
        }
        return true;   // nothing behind the dialog reacts to keys while it is open
    }

    // A click elsewhere in the app while the dialog is open: show it instead of beeping.
    void inputAttemptWhenModal() override
    {
        if (auto* top = getTopLevelComponent()) top->toFront (true);
        grabKeyboardFocus();
    }

private:
    static juce::TextLayout makeLayout (const juce::String& s, const juce::Font& f, juce::Colour c, float width)
    {
        juce::AttributedString a;
        a.setWordWrap (juce::AttributedString::byWord);
        a.setLineSpacing (2.0f);
        a.append (s, f, c);
        juce::TextLayout l;
        l.createLayout (a, juce::jmax (40.0f, width));
        return l;
    }

    void finish (int result)
    {
        if (finished) return;
        finished = true;
        exitModalState (result);
        if (host != nullptr) host->removeComponentListener (this);
        setVisible (false);
        auto cb = std::move (done);
        juce::Component::SafePointer<Overlay> safe (this);
        juce::MessageManager::callAsync ([safe] { delete safe.getComponent(); });
        if (cb) cb (result);
    }

    void componentMovedOrResized (juce::Component& c, bool, bool wasResized) override
    {
        if (wasResized && &c == host) setBounds (host->getLocalBounds());
    }
    void componentBeingDeleted (juce::Component& c) override
    {
        if (&c != host) return;
        host = nullptr;
        finished = true;   // the window goes away: no answer (the caller's objects may be going too)
        if (isCurrentlyModal()) exitModalState (-1);
        juce::Component::SafePointer<Overlay> safe (this);
        juce::MessageManager::callAsync ([safe] { delete safe.getComponent(); });
    }

    juce::Component* host;
    Kind kind;
    juce::String title, text;
    std::function<void (int)> done;
    std::vector<std::unique_ptr<DialogButton>> buttons;
    juce::Rectangle<int> card;
    juce::TextLayout titleLayout, messageLayout;
    bool finished = false;
};

juce::Component* hostFor (juce::Component* near)
{
    if (near != nullptr)
        if (auto* top = near->getTopLevelComponent()) return top;
    if (auto* w = juce::TopLevelWindow::getActiveTopLevelWindow()) return w;
    return juce::TopLevelWindow::getNumTopLevelWindows() > 0 ? juce::TopLevelWindow::getTopLevelWindow (0) : nullptr;
}
} // namespace

void show (juce::Component* near, Kind kind, juce::String title, juce::String message,
           std::vector<Button> buttons, std::function<void (int)> done)
{
    auto* host = hostFor (near);
    if (host == nullptr) { if (done) done (-1); return; }
    if (buttons.empty()) buttons.push_back ({ "OK", Button::Role::primary });
    new Overlay (*host, kind, std::move (title), std::move (message), std::move (buttons), std::move (done));   // deletes itself
}

void confirm (juce::Component* near, Kind kind, juce::String title, juce::String message,
              juce::String okText, std::function<void()> onOk)
{
    const auto role = kind == Kind::danger ? Button::Role::destructive : Button::Role::primary;
    show (near, kind, std::move (title), std::move (message),
          { { std::move (okText), role }, { "Cancel", Button::Role::cancel } },
          [onOk = std::move (onOk)] (int r) { if (r == 0 && onOk) onOk(); });
}

void message (juce::Component* near, Kind kind, juce::String title, juce::String text)
{
    show (near, kind, std::move (title), std::move (text), { { "OK", Button::Role::primary } }, nullptr);
}
} // namespace hydra::dialog

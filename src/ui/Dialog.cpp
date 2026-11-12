#include "Dialog.h"

namespace hydra::dialog
{
namespace
{
constexpr int kFieldH = 32;
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

using DialogButton = ActionButton;

class Overlay final : public juce::Component, private juce::ComponentListener
{
public:
    Overlay (juce::Component& h, Kind k, juce::String t, juce::String m, std::vector<Button> b, std::function<void (int)> d,
             const juce::String* fieldText = nullptr, std::function<void (juce::String)> onText = nullptr)
        : host (&h), kind (k), title (std::move (t)), text (std::move (m)), done (std::move (d)), textDone (std::move (onText))
    {
        if (fieldText != nullptr)
        {
            field = std::make_unique<juce::TextEditor>();
            field->setFont (theme::font (14.0f));
            field->setText (*fieldText, false);
            field->setColour (juce::TextEditor::backgroundColourId, theme::bg);
            field->setColour (juce::TextEditor::textColourId, theme::text);
            field->setColour (juce::TextEditor::outlineColourId, theme::outline);
            field->setColour (juce::TextEditor::focusedOutlineColourId, kindColour (k));
            field->setColour (juce::TextEditor::highlightColourId, kindColour (k).withAlpha (0.35f));
            field->setIndents (8, 7);
            field->onReturnKey = [this] { finish (0); };
            field->onEscapeKey = [this] { finish (-1); };
            addAndMakeVisible (*field);
        }
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
        if (field != nullptr) { field->grabKeyboardFocus(); field->selectAll(); }
        else grabKeyboardFocus();
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
        const int fieldH = field != nullptr ? kFieldH + 14 : 0;
        const int h = kPad + juce::jmax (kIcon, textH) + fieldH + 26 + kButtonH + kPad - 4;
        card = juce::Rectangle<int> (w, juce::jmin (h, getHeight() - 24)).withCentre (getLocalBounds().getCentre());
        if (field != nullptr)
            field->setBounds (card.getX() + kPad + kIcon + kIconGap, card.getBottom() - kPad + 4 - kButtonH - 22 - kFieldH, textW, kFieldH);

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
        messageLayout.draw (g, textArea.withBottom ((float) (field != nullptr ? field->getY() - 6 : card.getBottom() - kPad - kButtonH - 10)));
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
        auto tcb = std::move (textDone);
        const auto typed = field != nullptr ? field->getText().trim() : juce::String();
        juce::Component::SafePointer<Overlay> safe (this);
        juce::MessageManager::callAsync ([safe] { delete safe.getComponent(); });
        if (cb) cb (result);
        if (tcb && result == 0 && typed.isNotEmpty()) tcb (typed);
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
    std::function<void (juce::String)> textDone;
    std::unique_ptr<juce::TextEditor> field;
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
void askText (juce::Component* near, juce::String title, juce::String message, juce::String initial,
              juce::String okText, std::function<void (juce::String)> onOk)
{
    auto* host = hostFor (near);
    if (host == nullptr) return;
    new Overlay (*host, Kind::question, std::move (title), std::move (message),
                 { { std::move (okText), Button::Role::primary }, { "Cancel", Button::Role::cancel } }, nullptr,
                 &initial, std::move (onOk));   // deletes itself
}
} // namespace hydra::dialog

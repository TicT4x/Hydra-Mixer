#include "Controls.h"
#include <windows.h>
#include <dwmapi.h>
#pragma comment (lib, "dwmapi.lib")

namespace hydra
{
//==============================================================================
void removeNativeWindowBorder (juce::Component& window)
{
    auto* peer = window.getPeer();
    if (peer == nullptr) return;
    const auto hwnd = (HWND) peer->getNativeHandle();
    constexpr DWORD darkModeAttribute = 20;         // DWMWA_USE_IMMERSIVE_DARK_MODE
    constexpr DWORD borderColourAttribute = 34;     // DWMWA_BORDER_COLOR (Windows 11)
    constexpr DWORD captionColourAttribute = 35;    // DWMWA_CAPTION_COLOR (Windows 11)
    constexpr COLORREF noColour = 0xFFFFFFFE;       // DWMWA_COLOR_NONE

    // The window's system frame stays underneath the content (JUCE extends it into the client area). Left light,
    // it shows as white pixels at the edge and at the rounded corners, so it gets the app's dark colour.
    const BOOL dark = TRUE;
    const COLORREF caption = RGB (theme::bg.getRed(), theme::bg.getGreen(), theme::bg.getBlue());
    DwmSetWindowAttribute (hwnd, darkModeAttribute, &dark, sizeof (dark));
    DwmSetWindowAttribute (hwnd, captionColourAttribute, &caption, sizeof (caption));
    DwmSetWindowAttribute (hwnd, borderColourAttribute, &noColour, sizeof (noColour));
}

//==============================================================================
void LayoutAnimator::moveTo (juce::Component& c, juce::Rectangle<int> target, bool snapSize)
{
    for (auto& it : items)
        if (it.comp.getComponent() == &c)
        {
            it.target = target;
            if (snapSize)
            {
                it.pos.setSize ((float) target.getWidth(), (float) target.getHeight());
                c.setSize (target.getWidth(), target.getHeight());
            }
            return;
        }
    if (c.getBounds() == target) return;
    if (! owner.isShowing()) { c.setBounds (target); return; }
    if (snapSize)
    {
        c.setSize (target.getWidth(), target.getHeight());
        if (c.getPosition() == target.getPosition()) return;
    }
    if (items.empty()) lastFrame = 0.0;
    items.push_back ({ &c, c.getBounds().toFloat(), target });
}

void LayoutAnimator::shift (juce::Component& c, int dx)
{
    if (dx == 0) return;
    for (auto& it : items)
        if (it.comp.getComponent() == &c) { it.pos.translate ((float) dx, 0.0f); break; }
    c.setTopLeftPosition (c.getX() + dx, c.getY());
}

void LayoutAnimator::place (juce::Component& c, juce::Rectangle<int> target)
{
    forget (c);
    c.setBounds (target);
}

void LayoutAnimator::forget (const juce::Component& c)
{
    items.erase (std::remove_if (items.begin(), items.end(), [&c] (const Item& it) { return it.comp.getComponent() == &c; }), items.end());
}

void LayoutAnimator::frame (double t)
{
    if (items.empty()) return;
    const double dt = lastFrame > 0.0 ? juce::jlimit (0.0, 0.05, t - lastFrame) : 1.0 / 60.0;
    lastFrame = t;
    const float k = 1.0f - (float) std::exp (-dt / timeConstant);
    for (auto it = items.begin(); it != items.end();)
    {
        auto* c = it->comp.getComponent();
        if (c == nullptr) { it = items.erase (it); continue; }
        const auto to = it->target.toFloat();
        auto& p = it->pos;
        p.setBounds (p.getX() + (to.getX() - p.getX()) * k, p.getY() + (to.getY() - p.getY()) * k,
                     p.getWidth() + (to.getWidth() - p.getWidth()) * k, p.getHeight() + (to.getHeight() - p.getHeight()) * k);
        const bool done = std::abs (to.getX() - p.getX()) < 0.5f && std::abs (to.getY() - p.getY()) < 0.5f
                       && std::abs (to.getWidth() - p.getWidth()) < 0.5f && std::abs (to.getHeight() - p.getHeight()) < 0.5f;
        c->setBounds (done ? it->target : p.toNearestInt());
        it = done ? items.erase (it) : it + 1;
    }
    if (onFrame) onFrame();
}

//==============================================================================
void placeOnAppDisplay (juce::Component& window, bool always)
{
    if (appComponent == nullptr || appComponent->getTopLevelComponent() == &window || ! appComponent->isShowing()) return;
    auto& displays = juce::Desktop::getInstance().getDisplays();
    const auto* app = displays.getDisplayForRect (appComponent->getTopLevelComponent()->getScreenBounds());
    if (app == nullptr) return;
    const auto* now = displays.getDisplayForRect (window.getScreenBounds());
    if (! always && now == app) return;
    const auto area = app->userBounds.getSmallestIntegerContainer();
    window.setBounds (window.getBounds().withSize (juce::jmin (window.getWidth(), area.getWidth()), juce::jmin (window.getHeight(), area.getHeight()))
                          .withCentre (area.getCentre()));
}

//==============================================================================
void drawLegend (juce::Graphics& g, juce::Rectangle<int> legend, const std::vector<std::pair<juce::String, juce::Colour>>& items)
{
    int x = legend.getRight();
    g.setFont (theme::font (12.0f));
    for (auto it = items.rbegin(); it != items.rend(); ++it)
    {
        const int w = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), it->first);
        x -= w;
        g.setColour (theme::textDim);
        g.drawText (it->first, x, legend.getY(), w, legend.getHeight(), juce::Justification::centredLeft);
        x -= 18;
        g.setColour (it->second);
        g.fillRoundedRectangle ((float) x, (float) legend.getCentreY() - 5.0f, 12.0f, 10.0f, 2.5f);
        x -= 18;
    }
}

//==============================================================================
void Chip::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    const bool underline = flat && theme::deep();
    g.setColour (underline ? (hover ? theme::raised : juce::Colours::transparentBlack)
                           : active ? colour.withAlpha (0.22f) : (hover ? theme::raised.brighter (0.06f) : theme::panel2));
    g.fillRoundedRectangle (r, 6.0f);
    bool offline = false, attention = false;
    for (auto& s : status) { offline |= s.kind == Marker::offline; attention |= s.kind == Marker::attention || s.kind == Marker::dropouts; }
    g.setColour (offline ? theme::mute.withAlpha (0.7f) : attention ? theme::warn.withAlpha (0.8f)
                 : active && ! underline ? colour.withAlpha (0.8f) : theme::outline);
    if (underline) g.fillRect (r.withTop (r.getBottom() - 1.0f));
    else           g.drawRoundedRectangle (r, 6.0f, 1.0f);

    auto tr = r.reduced (7.0f, 0.0f);
    if (showArrow)
    {
        auto ar = tr.removeFromRight (8.0f);
        juce::Path p;
        const float cy = ar.getCentreY();
        p.addTriangle (ar.getX(), cy - 2.0f, ar.getRight(), cy - 2.0f, ar.getCentreX(), cy + 2.5f);
        g.setColour (theme::textDim);
        g.fillPath (p);
        tr.removeFromRight (3.0f);
    }
    // Status dots left of the name (red first)
    markerAreas.clear();
    for (auto& st : status)
    {
        auto m = tr.removeFromLeft (9.0f).withSizeKeepingCentre (9.0f, 9.0f);
        drawMarker (g, st.kind, m);
        markerAreas.push_back (m.expanded (3.0f, 6.0f));
        tr.removeFromLeft (4.0f);
    }
    if (icon != Icon::none)
    {
        const float s = juce::jmin (14.0f, tr.getHeight() - 6.0f);
        auto ir = text.isEmpty() ? tr.withSizeKeepingCentre (s, s) : tr.removeFromLeft (s).withSizeKeepingCentre (s, s);
        drawIcon (g, icon, ir, active ? theme::text : colour);
        if (text.isEmpty()) return;
        tr.removeFromLeft (6.0f);
        if (centred) tr.removeFromRight (s + 6.0f);   // keep the text centred under the whole chip
    }
    g.setColour (active ? theme::text : colour);
    g.setFont (theme::font (12.0f));
    g.drawFittedText (text, tr.toNearestInt(), centred ? juce::Justification::centred : juce::Justification::centredLeft, 1, 0.8f);
}

void Chip::setStatus (std::vector<Status> s)
{
    // One dot per colour: dropouts and "needs attention" are both yellow and share a dot (and its tooltip).
    // Order: red, yellow, green.
    std::vector<Status> merged;
    for (int pass = 0; pass < 3; ++pass)
    {
        const bool red = pass == 0, green = pass == 2;
        Status dot { red ? Marker::offline : green ? Marker::connected : Marker::attention, {} };
        bool any = false;
        auto colourPass = [] (Marker k) { return k == Marker::offline ? 0 : k == Marker::connected ? 2 : 1; };
        for (auto& x : s)
            if (colourPass (x.kind) == pass)
            {
                if (any) dot.tip << "\n\n";
                dot.tip << x.tip;
                if (x.kind == Marker::dropouts && ! red) dot.kind = Marker::dropouts;
                any = true;
            }
        if (any) merged.push_back (std::move (dot));
    }

    auto same = [] (const std::vector<Status>& a, const std::vector<Status>& b)
    {
        if (a.size() != b.size()) return false;
        for (size_t k = 0; k < a.size(); ++k)
            if (a[k].kind != b[k].kind || a[k].tip != b[k].tip) return false;
        return true;
    };
    if (same (merged, status)) return;
    const bool symbolsChanged = merged.size() != status.size()
                             || ! std::equal (merged.begin(), merged.end(), status.begin(), [] (auto& a, auto& b) { return a.kind == b.kind; });
    status = std::move (merged);
    if (symbolsChanged) repaint();
}

juce::String Chip::getTooltip()
{
    // Over a dot: just what it means. Elsewhere: the problems first, then the chip's own tooltip.
    const auto p = getMouseXYRelative().toFloat();
    for (size_t k = 0; k < markerAreas.size() && k < status.size(); ++k)
        if (markerAreas[k].contains (p)) return status[k].tip;
    juce::String tip;
    for (auto& st : status) tip << st.tip << "\n\n";
    return tip + juce::SettableTooltipClient::getTooltip();
}

void Chip::drawMarker (juce::Graphics& g, Marker kind, juce::Rectangle<float> r)
{
    // Red dot = offline, yellow dot = dropouts / needs attention, green = connected (Hydra Link), with a faint halo so it reads on any chip.
    const auto c = kind == Marker::offline ? theme::mute : kind == Marker::connected ? theme::good : theme::warn;
    const auto d = juce::jmin (r.getWidth(), r.getHeight());
    const auto dot = juce::Rectangle<float> (d, d).withCentre (r.getCentre());
    g.setColour (c.withAlpha (0.25f));
    g.fillEllipse (dot.expanded (1.5f));
    g.setColour (c);
    g.fillEllipse (dot.reduced (0.5f));
}

void Chip::drawIcon (juce::Graphics& g, Icon icon, juce::Rectangle<float> r, juce::Colour colour)
{
    const juce::PathStrokeType stroke (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    g.setColour (colour);
    if (icon == Icon::save)
    {
        // Floppy disk: body with a cut corner, shutter at the top, label at the bottom.
        auto b = r.reduced (0.5f);
        const float cut = b.getWidth() * 0.24f;
        juce::Path body;
        body.startNewSubPath (b.getX(), b.getY());
        body.lineTo (b.getRight() - cut, b.getY());
        body.lineTo (b.getRight(), b.getY() + cut);
        body.lineTo (b.getRight(), b.getBottom());
        body.lineTo (b.getX(), b.getBottom());
        body.closeSubPath();
        g.strokePath (body, stroke);
        g.fillRect (juce::Rectangle<float> (b.getX() + b.getWidth() * 0.26f, b.getY(), b.getWidth() * 0.4f, b.getHeight() * 0.3f));
        g.drawRoundedRectangle (juce::Rectangle<float> (b.getX() + b.getWidth() * 0.2f, b.getY() + b.getHeight() * 0.56f,
                                                        b.getWidth() * 0.6f, b.getHeight() * 0.44f), 1.0f, 1.2f);
    }
    else if (icon == Icon::revert)
    {
        // Counter-clockwise arrow (back to the stored state).
        const auto c = r.getCentre();
        const float rad = r.getWidth() * 0.4f;
        juce::Path arc;
        arc.addCentredArc (c.x, c.y, rad, rad, 0.0f, -juce::MathConstants<float>::pi * 0.35f,
                           juce::MathConstants<float>::pi * 1.25f, true);
        g.strokePath (arc, stroke);
        // Arrow head at the start of the arc, pointing against the drawing direction (= counter-clockwise).
        const float sa = -juce::MathConstants<float>::pi * 0.35f;
        const juce::Point<float> start (c.x + rad * std::sin (sa), c.y - rad * std::cos (sa));
        const juce::Point<float> dir (-std::cos (sa), -std::sin (sa));   // tangent, counter-clockwise
        const juce::Point<float> nrm (dir.y, -dir.x);
        juce::Path head;
        const float s = r.getWidth() * 0.26f;
        head.addTriangle (start + dir * s, start - nrm * (s * 0.8f), start + nrm * (s * 0.8f));
        g.fillPath (head);
    }
}
//==============================================================================
void TrashButton::drawIcon (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour)
{
    // Trash can: lid with handle, tapered bin with three ribs.
    const float s = juce::jmin (area.getWidth(), area.getHeight());
    auto r = juce::Rectangle<float> (s * 0.8f, s).withCentre (area.getCentre());
    const float stroke = juce::jmax (1.2f, s * 0.09f);
    const juce::PathStrokeType pst (stroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    juce::Path p;
    const float lidY = r.getY() + r.getHeight() * 0.2f;
    p.startNewSubPath (r.getX(), lidY);
    p.lineTo (r.getRight(), lidY);
    const float hw = r.getWidth() * 0.18f;
    p.startNewSubPath (r.getCentreX() - hw, lidY);
    p.lineTo (r.getCentreX() - hw, r.getY() + stroke * 0.5f);
    p.lineTo (r.getCentreX() + hw, r.getY() + stroke * 0.5f);
    p.lineTo (r.getCentreX() + hw, lidY);
    const float top = lidY + stroke * 0.9f, bottom = r.getBottom() - stroke * 0.5f;
    const float inset = r.getWidth() * 0.12f, taper = r.getWidth() * 0.06f;
    p.startNewSubPath (r.getX() + inset, top);
    p.lineTo (r.getX() + inset + taper, bottom);
    p.lineTo (r.getRight() - inset - taper, bottom);
    p.lineTo (r.getRight() - inset, top);
    for (float f : { 0.36f, 0.5f, 0.64f })
    {
        const float x = r.getX() + r.getWidth() * f;
        p.startNewSubPath (x, top + r.getHeight() * 0.14f);
        p.lineTo (x, bottom - r.getHeight() * 0.14f);
    }
    g.setColour (colour);
    g.strokePath (p, pst);
}

void TrashButton::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    const bool hot = hover && isEnabled();
    g.setColour (hot ? theme::mute.withAlpha (0.2f) : theme::panel2);
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (hot ? theme::mute.withAlpha (0.8f) : theme::outline);
    g.drawRoundedRectangle (r, 6.0f, 1.0f);
    drawIcon (g, r.withSizeKeepingCentre (14.0f, 14.0f), hot ? theme::mute : isEnabled() ? theme::textDim : theme::textFaint.withAlpha (0.5f));
}

//==============================================================================
void PlusButton::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    if (card && theme::deep())
    {
        g.setColour (hover ? colour.withAlpha (0.1f) : theme::panel2);
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (hover ? colour.withAlpha (0.7f) : theme::cardLine);
        g.drawRoundedRectangle (r, 4.0f, 1.0f);
        const auto m = r.getCentre();
        g.setColour (colour.withAlpha (hover ? 1.0f : 0.18f));
        g.fillEllipse (juce::Rectangle<float> (26.0f, 26.0f).withCentre (m));
        g.setColour (hover ? theme::bg : colour);
        g.fillRoundedRectangle (m.x - 6.0f, m.y - 1.0f, 12.0f, 2.0f, 1.0f);
        g.fillRoundedRectangle (m.x - 1.0f, m.y - 6.0f, 2.0f, 12.0f, 1.0f);
        return;
    }
    const float corner = juce::jmin (8.0f, r.getHeight() * 0.3f);
    g.setColour (hover ? colour.withAlpha (0.22f) : theme::panel2);
    g.fillRoundedRectangle (r, corner);
    g.setColour (colour.withAlpha (hover ? 0.9f : 0.45f));
    g.drawRoundedRectangle (r, corner, 1.0f);

    const auto c = r.getCentre();
    const float s = juce::jmin (r.getWidth(), r.getHeight()) * 0.22f;
    g.setColour (hover ? theme::text : colour);
    g.fillRoundedRectangle (c.x - s, c.y - 1.0f, s * 2.0f, 2.0f, 1.0f);
    g.fillRoundedRectangle (c.x - 1.0f, c.y - s, 2.0f, s * 2.0f, 1.0f);
}

//==============================================================================
void InfoButton::paint (juce::Graphics& g)
{
    const float d = (float) juce::jmin (kSize, getWidth(), getHeight()) - 2.0f;
    auto r = juce::Rectangle<float> (d, d).withCentre (getLocalBounds().toFloat().getCentre());
    const auto c = hover ? theme::accentIn : theme::textDim;
    if (hover)
    {
        g.setColour (theme::accentIn.withAlpha (0.15f));
        g.fillEllipse (r);
    }
    g.setColour (c);
    g.drawEllipse (r.reduced (0.6f), 1.2f);
    const auto m = r.getCentre();
    const float s = d / 14.0f;   // drawn on a 14 px grid
    g.fillEllipse (m.x - 0.9f * s, m.y - 4.3f * s, 1.8f * s, 1.8f * s);
    g.fillRoundedRectangle (m.x - 0.8f * s, m.y - 1.4f * s, 1.6f * s, 5.2f * s, 0.8f * s);
}

void InfoButton::mouseExit (const juce::MouseEvent&)
{
    hover = false;
    repaint();
    if (auto* tips = theme::TooltipWindow::get(); tips != nullptr && ! pinned) tips->hideTip();
}

void InfoButton::show()
{
    if (text.isEmpty()) return;
    if (auto* tips = theme::TooltipWindow::get())
        tips->displayTip (localPointToGlobal (juce::Point<int> (getWidth() / 2, getHeight() / 2)), text);
}
} // namespace hydra

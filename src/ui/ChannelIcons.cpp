#include "ChannelIcons.h"

namespace hydra::icons
{
const std::vector<Info>& all()
{
    static const std::vector<Info> list {
        { "speakers",   "Speakers" },
        { "headphones", "Headphones" },
        { "headset",    "Headset" },
        { "subwoofer",  "Subwoofer" },
        { "bluetooth",  "Bluetooth" },
        { "monitor",    "Monitor" },
        { "tv",         "TV" },
        { "laptop",     "Laptop" },
        { "phone",      "Phone" },
        { "chat",       "Chat" },
        { "discord",    "Discord" },
        { "teamspeak",  "TeamSpeak" },
        { "camera",     "Video call" },
        { "obs",        "OBS" },
        { "stream",     "Stream" },
        { "recording",  "Recording" },
        { "game",       "Game" },
        { "music",      "Music" },
    };
    return list;
}

bool exists (const juce::String& id)
{
    if (id.isEmpty()) return false;
    for (auto& i : all()) if (id == i.id) return true;
    return false;
}

juce::String nameOf (const juce::String& id)
{
    for (auto& i : all()) if (id == i.id) return i.name;
    return {};
}

namespace
{
constexpr float kPi = juce::MathConstants<float>::pi;

/** Outline (stroked) and solid (filled) parts of an icon in the 24 x 24 grid. */
struct Shape
{
    juce::Path line, solid;
};

void headphoneBand (juce::Path& p, float cx, float cy, float r)
{
    p.addCentredArc (cx, cy, r, r, 0.0f, -kPi * 0.5f, kPi * 0.5f, true);
}

Shape build (const juce::String& id)
{
    Shape s;
    auto& l = s.line;
    auto& f = s.solid;
    if (id == "speakers")
    {
        l.addRoundedRectangle (5.5f, 2.5f, 13.0f, 19.0f, 2.5f);
        l.addEllipse (12.0f - 4.2f, 15.0f - 4.2f, 8.4f, 8.4f);
        f.addEllipse (12.0f - 1.7f, 7.0f - 1.7f, 3.4f, 3.4f);
    }
    else if (id == "headphones" || id == "headset")
    {
        const bool mic = id == "headset";
        headphoneBand (l, 12.0f, 13.0f, 8.0f);
        f.addRoundedRectangle (2.5f, 12.0f, 5.0f, mic ? 7.0f : 8.5f, 1.6f);
        f.addRoundedRectangle (16.5f, 12.0f, 5.0f, mic ? 7.0f : 8.5f, 1.6f);
        if (mic)
        {
            l.startNewSubPath (5.0f, 19.0f);
            l.quadraticTo (5.2f, 22.3f, 9.5f, 22.3f);
            f.addRoundedRectangle (9.5f, 21.0f, 4.0f, 2.6f, 1.3f);
        }
    }
    else if (id == "subwoofer")
    {
        l.addRoundedRectangle (3.0f, 3.0f, 18.0f, 18.0f, 2.5f);
        l.addEllipse (12.0f - 6.0f, 12.0f - 6.0f, 12.0f, 12.0f);
        f.addEllipse (12.0f - 2.2f, 12.0f - 2.2f, 4.4f, 4.4f);
    }
    else if (id == "bluetooth")
    {
        l.startNewSubPath (7.0f, 7.5f);
        l.lineTo (17.0f, 16.5f);
        l.lineTo (12.0f, 21.0f);
        l.lineTo (12.0f, 3.0f);
        l.lineTo (17.0f, 7.5f);
        l.lineTo (7.0f, 16.5f);
    }
    else if (id == "monitor")
    {
        l.addRoundedRectangle (2.5f, 3.5f, 19.0f, 13.0f, 2.0f);
        l.startNewSubPath (12.0f, 16.5f); l.lineTo (12.0f, 20.5f);
        l.startNewSubPath (7.5f, 20.8f);  l.lineTo (16.5f, 20.8f);
    }
    else if (id == "tv")
    {
        l.addRoundedRectangle (2.5f, 7.0f, 19.0f, 13.0f, 2.0f);
        l.startNewSubPath (8.0f, 2.5f);  l.lineTo (12.0f, 7.0f);
        l.startNewSubPath (16.0f, 2.5f); l.lineTo (12.0f, 7.0f);
    }
    else if (id == "laptop")
    {
        l.addRoundedRectangle (5.0f, 5.0f, 14.0f, 10.5f, 1.5f);
        f.addRoundedRectangle (2.0f, 17.0f, 20.0f, 2.6f, 1.3f);
    }
    else if (id == "phone")
    {
        // Classic handset: a bent handle with the ear and the mouth piece at its ends.
        l.startNewSubPath (6.0f, 6.0f);
        l.quadraticTo (5.0f, 19.0f, 18.0f, 18.0f);
        f.addEllipse (6.0f - 3.0f, 6.0f - 3.0f, 6.0f, 6.0f);
        f.addEllipse (18.0f - 3.0f, 18.0f - 3.0f, 6.0f, 6.0f);
    }
    else if (id == "chat")
    {
        l.startNewSubPath (7.0f, 4.0f);
        l.lineTo (17.0f, 4.0f);
        l.quadraticTo (21.0f, 4.0f, 21.0f, 8.0f);
        l.lineTo (21.0f, 12.0f);
        l.quadraticTo (21.0f, 16.0f, 17.0f, 16.0f);
        l.lineTo (11.5f, 16.0f);
        l.lineTo (7.0f, 20.5f);
        l.lineTo (7.0f, 16.0f);
        l.quadraticTo (3.0f, 16.0f, 3.0f, 12.0f);
        l.lineTo (3.0f, 8.0f);
        l.quadraticTo (3.0f, 4.0f, 7.0f, 4.0f);
        l.closeSubPath();
        for (float x : { 8.0f, 12.0f, 16.0f }) f.addEllipse (x - 1.25f, 10.0f - 1.25f, 2.5f, 2.5f);
    }
    else if (id == "discord")
    {
        // A friendly face with two eyes (voice chat), not the brand's logo.
        l.startNewSubPath (6.5f, 6.5f);
        l.quadraticTo (12.0f, 4.5f, 17.5f, 6.5f);
        l.quadraticTo (21.5f, 11.0f, 21.0f, 17.0f);
        l.quadraticTo (19.0f, 19.0f, 16.0f, 19.5f);
        l.lineTo (15.0f, 17.5f);
        l.quadraticTo (12.0f, 18.3f, 9.0f, 17.5f);
        l.lineTo (8.0f, 19.5f);
        l.quadraticTo (5.0f, 19.0f, 3.0f, 17.0f);
        l.quadraticTo (2.5f, 11.0f, 6.5f, 6.5f);
        l.closeSubPath();
        f.addEllipse (9.0f - 1.7f, 12.5f - 2.0f, 3.4f, 4.0f);
        f.addEllipse (15.0f - 1.7f, 12.5f - 2.0f, 3.4f, 4.0f);
    }
    else if (id == "teamspeak")
    {
        // Headset in a circle.
        l.addEllipse (2.0f, 2.0f, 20.0f, 20.0f);
        headphoneBand (l, 12.0f, 13.0f, 5.0f);
        f.addRoundedRectangle (6.0f, 12.0f, 3.2f, 5.0f, 1.2f);
        f.addRoundedRectangle (14.8f, 12.0f, 3.2f, 5.0f, 1.2f);
    }
    else if (id == "camera")
    {
        l.addRoundedRectangle (2.5f, 6.5f, 13.0f, 11.0f, 2.2f);
        l.startNewSubPath (15.5f, 10.5f);
        l.lineTo (21.5f, 7.5f);
        l.lineTo (21.5f, 16.5f);
        l.lineTo (15.5f, 13.5f);
    }
    else if (id == "obs")
    {
        // A ring with three dots (broadcast software), not the brand's logo.
        l.addEllipse (2.0f, 2.0f, 20.0f, 20.0f);
        for (int k = 0; k < 3; ++k)
        {
            const float a = -kPi * 0.5f + (float) k * 2.0f * kPi / 3.0f;
            const float x = 12.0f + 4.6f * std::cos (a), y = 12.0f + 4.6f * std::sin (a);
            f.addEllipse (x - 2.6f, y - 2.6f, 5.2f, 5.2f);
        }
    }
    else if (id == "stream")
    {
        f.addEllipse (12.0f - 2.2f, 12.0f - 2.2f, 4.4f, 4.4f);
        for (float r : { 5.5f, 9.0f })
        {
            l.addCentredArc (12.0f, 12.0f, r, r, 0.0f, kPi * 0.28f, kPi * 0.72f, true);
            l.addCentredArc (12.0f, 12.0f, r, r, 0.0f, -kPi * 0.72f, -kPi * 0.28f, true);
        }
    }
    else if (id == "recording")
    {
        l.addEllipse (2.5f, 2.5f, 19.0f, 19.0f);
        f.addEllipse (12.0f - 5.0f, 12.0f - 5.0f, 10.0f, 10.0f);
    }
    else if (id == "game")
    {
        l.addRoundedRectangle (2.0f, 7.0f, 20.0f, 11.0f, 5.5f);
        l.startNewSubPath (5.0f, 12.5f); l.lineTo (9.0f, 12.5f);
        l.startNewSubPath (7.0f, 10.5f); l.lineTo (7.0f, 14.5f);
        f.addEllipse (15.5f - 1.3f, 11.2f - 1.3f, 2.6f, 2.6f);
        f.addEllipse (18.0f - 1.3f, 13.8f - 1.3f, 2.6f, 2.6f);
    }
    else if (id == "music")
    {
        l.startNewSubPath (14.5f, 17.0f);
        l.lineTo (14.5f, 3.5f);
        l.quadraticTo (19.5f, 4.5f, 19.5f, 9.5f);
        f.addEllipse (8.0f, 14.3f, 7.4f, 5.6f);
    }
    return s;
}
} // namespace

void draw (juce::Graphics& g, const juce::String& id, juce::Rectangle<float> area, juce::Colour colour)
{
    if (! exists (id)) return;
    const float size = juce::jmin (area.getWidth(), area.getHeight());
    if (size <= 2.0f) return;
    const auto box = juce::Rectangle<float> (size, size).withCentre (area.getCentre());
    const float scale = size / 24.0f;
    const auto t = juce::AffineTransform::scale (scale).translated (box.getX(), box.getY());
    auto s = build (id);
    s.line.applyTransform (t);
    s.solid.applyTransform (t);
    g.setColour (colour);
    g.strokePath (s.line, juce::PathStrokeType (juce::jmax (1.1f, 1.9f * scale), juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));
    g.fillPath (s.solid);
}

void addMenu (juce::PopupMenu& m, const juce::String& current, std::function<void (juce::String)> chosen)
{
    juce::PopupMenu sub;
    sub.addItem ("None (show the code)", true, current.isEmpty(), [chosen] { chosen ({}); });
    sub.addSeparator();
    for (auto& i : all())
    {
        juce::PopupMenu::Item item (i.name);
        item.isTicked = current == i.id;
        const juce::String id (i.id);
        item.action = [chosen, id] { chosen (id); };
        // Small drawable of the icon in front of the name.
        auto d = std::make_unique<juce::DrawablePath>();
        {
            auto s = build (id);
            juce::Path all;
            all.addPath (s.solid);
            juce::PathStrokeType (1.9f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded).createStrokedPath (s.line, s.line);
            all.addPath (s.line);
            d->setPath (all);
            d->setFill (juce::Colours::white.withAlpha (0.85f));
        }
        item.image = std::move (d);
        sub.addItem (std::move (item));
    }
    m.addSubMenu ("Icon", sub);
}

//==============================================================================
namespace
{
constexpr juce::juce_wchar kTipMark = 0x2063;   // invisible separator: marks a tooltip made by routeTip
}

juce::String routeTip (const juce::String& fromIcon, const juce::String& fromName,
                       const juce::String& toIcon, const juce::String& toName, const juce::String& body)
{
    const juce::String m = juce::String::charToString (kTipMark);
    return m + fromIcon + m + fromName + m + toIcon + m + toName + m + body;
}

bool parseRouteTip (const juce::String& text, RouteTip& out)
{
    if (text.isEmpty() || text[0] != kTipMark) return false;
    juce::StringArray parts;
    parts.addTokens (text.substring (1), juce::String::charToString (kTipMark), {});
    if (parts.size() < 5) return false;
    out = { parts[0], parts[1], parts[2], parts[3], parts[4] };
    for (int k = 5; k < parts.size(); ++k) out.body << parts[k];
    return true;
}
} // namespace hydra::icons

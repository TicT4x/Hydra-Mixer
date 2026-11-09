#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

namespace hydra::icons
{
/** Optional symbol of an output channel (Bus::icon). Shown instead of "H1" / "V1" on the routing buttons of the
    input strips, in the badge of the bus strip and in the routing tooltips ("Spotify -> [headphones] Headphones").
    Stored by id in the session, so the list may grow; an unknown id draws nothing (the code is shown instead). */
struct Info
{
    const char* id;
    const char* name;
};

const std::vector<Info>& all();
bool exists (const juce::String& id);
juce::String nameOf (const juce::String& id);

/** Line icon in a 24 x 24 grid, scaled into the square centred in area. */
void draw (juce::Graphics&, const juce::String& id, juce::Rectangle<float> area, juce::Colour);

/** Adds an "Icon" submenu (with "None") to m; chosen is called with the new id ("" = none). */
void addMenu (juce::PopupMenu& m, const juce::String& current, std::function<void (juce::String)> chosen);

//==============================================================================
/** Tooltip with icons: a first line "from -> to" where both sides may carry an icon, then an ordinary tooltip
    text. Encoded into the tooltip string (see LookAndFeel::drawTooltip), so any TooltipClient can use it. */
juce::String routeTip (const juce::String& fromIcon, const juce::String& fromName,
                       const juce::String& toIcon, const juce::String& toName, const juce::String& body);

struct RouteTip
{
    juce::String fromIcon, fromName, toIcon, toName, body;
};
/** Decodes a tooltip made by routeTip (false for any other text). */
bool parseRouteTip (const juce::String& text, RouteTip& out);
} // namespace hydra::icons

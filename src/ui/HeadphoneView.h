#pragma once
#include "Widgets.h"

/** Headphone Correction UI: the menus of the block, importing own headphones, the big window. */
namespace hydra::hpui
{
/** Menu of headphones (target = false) or of what they should sound like (target = true), by brand.
    Choosing applies it to the channel; changed runs afterwards. */
void showMenu (UiContext&, int channel, int k, bool target, juce::Component* anchor, std::function<void()> changed = {});
/** Asks for a frequency response file, its name and kind, and adds it as own headphones. done gets the new id. */
void importFile (juce::Component* near, std::function<void (juce::String id)> done);
/** The Headphone Correction window of a channel. */
std::unique_ptr<juce::Component> makePanel (UiContext&, int channel, int k = 0);

/** Name shown for a library id ("" = nothing chosen; empty target = neutral). */
juce::String nameOf (const juce::String& id, bool target);

/** Draws the curves of a channel's correction into area (block: compact = true). */
void drawCurves (juce::Graphics&, juce::Rectangle<float> area, Engine&, int channel, int k, bool compact);
} // namespace hydra::hpui

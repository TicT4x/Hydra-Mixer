#pragma once
#include "engine/Engine.h"

namespace hydra::recproject
{
/** Connects the recorder's project files with the mixer: the settings of every recorder track (fader, gain, pan,
    mute, solo, mono, effect chain with plugin states) are stored in the project file (TRACKSETTINGS) and restored
    when it is opened; a new take starts with fresh tracks named after the recorded inputs. Called once (Session). */
void attach (Engine&);

/** Parameter ids of a track that are not part of its effect chain (stored as values). */
juce::StringArray stripSuffixes();
} // namespace hydra::recproject

#pragma once
#include "Widgets.h"

/** Window "Phone camera": the picture of the phone's camera (large, live) and its settings, as the phone describes
    them (engine/PhoneCamera.h, cam::Control): camera, zoom, light, exposure / ISO / shutter, focus (click into the
    picture), white balance, scene modes, ... While the window is open the phone films (it counts as a watcher). */
namespace hydra
{
std::unique_ptr<juce::Component> makeCameraView (UiContext&);
}

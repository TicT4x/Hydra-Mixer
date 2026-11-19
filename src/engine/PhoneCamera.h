#pragma once
#include "NetLink.h"
#include <juce_graphics/juce_graphics.h>

/** Hydra Camera: a phone's camera as a webcam for every app (Windows 11, no kernel driver).

      phone (Hydra Link app): camera -> hardware H.264 encoder -> Hydra Link "Video" packets (UDP, paced)
      Hydra (this):           network thread reassembles frames -> camera thread decodes (Media Foundation, low
                              latency) -> fits the picture into the size the app chose (rotation, letterbox) ->
                              shared memory -> camera/HydraCamera.cpp in Windows' Frame Server -> the apps.

    Resource saving: the phone only encodes and sends while an app actually shows Hydra Camera (or the preview in
    Settings is open): Hydra tells it once a second (VideoCtl "want"). Without a watcher nothing is decoded.
    Lost packets: the frame is dropped, Hydra asks for a key frame and shows the last picture until it arrives.

    Setup: the camera source must be registered for the whole machine once (UAC; see camera/VideoCodec.h). */
namespace hydra::cam
{
struct Status
{
    bool enabled = false;
    bool supported = true;          // Windows 11
    bool setUp = false;             // camera source registered (current version)
    bool setUpOld = false;          // registered, but an older version (setup again)
    bool cameraOn = false;          // "Hydra Camera" exists for the apps right now
    bool inUse = false;             // an app shows it (or the preview)
    bool receiving = false;         // frames arrive
    juce::String phone;             // the phone whose camera is used ("" = none heard)
    juce::StringArray phones;       // phones that offer a camera
    int width = 0, height = 0;      // what the phone sends
    int outWidth = 0, outHeight = 0;// what the app gets
    double fps = 0, kbps = 0;
    uint32_t lostFrames = 0;
    juce::String error;
    // The last picture as the phone sent it (before turning): to map a click on the picture back to the phone.
    int frameWidth = 0, frameHeight = 0, rotation = 0;
};

/** One setting of the phone's camera, as the phone describes it (CamCaps packet, engine/NetLink.cpp). The phone
    decides what exists (it knows its camera); Hydra draws it generically. */
struct Control
{
    enum Kind { toggle = 0, range = 1, choice = 2, button = 3, point = 4 };
    enum Flags { usable = 1, logarithmic = 2, seconds = 4, diopters = 8, live = 16 };
    enum Group { camera = 0, exposure = 1, focus = 2, colour = 3, image = 4 };
    int id = 0, kind = toggle, flags = usable, group = camera;
    juce::String label, unit;
    float min = 0, max = 1, step = 0, value = 0;
    juce::StringArray options;
    bool isUsable() const noexcept { return (flags & usable) != 0; }
    /** The value as text ("1/250 s", "0.5 m", "+0.7 EV", "Daylight"). */
    juce::String format (float v) const;
};

class PhoneCamera final : private net::Hub::VideoHandler
{
public:
    PhoneCamera();
    ~PhoneCamera() override;

    /** The app's camera (listens on net::Hub::get()). */
    static PhoneCamera& get();

    /** On: "Hydra Camera" is offered to the apps (if set up) and phones' cameras are used. phone = which one (its
        name), empty = any paired phone that offers its camera (the most recent). */
    void setEnabled (bool on, const juce::String& phone);
    Status getStatus() const;

    /** Preview: counts as a watcher while on; the newest picture, ~10 per second, `width` pixels wide (the widest
        user wins). user: 0 = Settings, 1 = the camera window. */
    void setPreview (bool on, int user = 0, int width = 320);
    juce::Image getPreview (uint32_t& version) const;

    /** The settings of the phone in use (empty: none heard yet). version changes with every update. A value set
        here shows at once and stays until the phone confirms it. */
    std::vector<Control> getControls (uint32_t& version) const;
    void setControl (int id, float value);
    void pressButton (int id);
    /** A point on the upright picture (0..1), e.g. focus / expose there. */
    void setPoint (int id, float x, float y);

    /** Setup with administrator rights (UAC prompt, waits). install = false removes it. */
    static bool requestSetup (bool install);
    static constexpr const char* kSetupArgument = "--camera";
    static bool isSetupJob (const juce::String& commandLine) { return commandLine.contains (kSetupArgument); }
    /** In the elevated helper: "--camera install|remove". Returns the exit code. */
    static int runSetupJob (const juce::String& commandLine);

    //--------------------------------------------------------------------------
    /** Tests: a hub other than the app's, frames without an app watching, every output frame. */
    void attach (net::Hub&);
    std::atomic<bool> forceWanted { false };
    std::function<void (const uint8_t* nv12, int width, int height)> onFrame;   // camera thread
    /** Tests: the output size when no app is watching (an app's choice wins). */
    std::atomic<int> testWidth { 1280 }, testHeight { 720 };
    /** Tests: run without registering the virtual camera; shared memory under the test name (CameraShared.h). */
    std::atomic<bool> noVirtualCamera { false }, testNames { false };

    /** Screenshots (tools/Shots.cpp): status, controls and preview shown instead of the real ones. */
    void setDemo (const Status&, std::vector<Control>, const juce::Image& preview);

    struct Impl;

private:
    void videoPacket (uint64_t device, const juce::String& sender, net::Hub::VideoPacket type, const uint8_t* body, int bytes, double nowMs) override;
    std::unique_ptr<Impl> impl;
};

/** NV12 (y / uv planes with pitch, w x h) turned by `rotation` quarter turns clockwise and fitted into a W x H NV12
    picture (stride W), black borders. Straight copy when nothing changes, else bilinear (luma) / nearest (chroma). */
void fitNv12 (const uint8_t* y, const uint8_t* uv, int pitch, int w, int h, int rotation, uint8_t* out, int W, int H);
} // namespace hydra::cam

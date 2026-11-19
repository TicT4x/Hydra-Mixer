#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

/** Media Foundation pieces of Hydra Camera (camera/VideoCodec.cpp, a translation unit without JUCE: it needs the
    Windows 11 SDK declarations of the virtual camera). All of it on one thread that called comInit(). */
namespace hydra::cam
{
/** COM (multithreaded) + Media Foundation for the calling thread. */
bool comInit();
void comShutdown();

/** One decoded picture: NV12, `pitch` bytes per row, the chroma plane at `uv`. width / height = coded size. */
struct Picture
{
    const uint8_t* y = nullptr;
    const uint8_t* uv = nullptr;
    int pitch = 0, width = 0, height = 0;
};

/** H.264 (Annex B) -> NV12 with Microsoft's decoder MFT in low-latency mode (one picture out per frame in, no
    reordering: the phone sends baseline (Wi-Fi) or High profile (USB cable) without B frames). Up to 4K. */
class H264Decoder
{
public:
    H264Decoder();
    ~H264Decoder();
    /** False: the decoder is missing (Windows N without the media pack) -> error(). */
    bool open();
    /** Decodes one access unit; onPicture is called for every picture that comes out. False = the stream is broken
        (corrupt data, missing reference): flushed, the caller waits for the next key frame. */
    bool decode (const uint8_t* data, int bytes, int64_t time100ns, const std::function<void (const Picture&)>& onPicture);
    void flush();
    std::string error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

/** NV12 -> H.264 Annex B with Microsoft's encoder MFT (tests: stand-in for the phone's encoder). */
class H264Encoder
{
public:
    H264Encoder();
    ~H264Encoder();
    bool open (int width, int height, int fps, int kbps);
    /** nv12: width * height * 3 / 2 bytes. out = the access unit (may be empty while the encoder fills up). */
    bool encode (const uint8_t* nv12, int64_t time100ns, bool forceKey, std::vector<uint8_t>& out, bool& key);
    /** SPS / PPS (Annex B), if the encoder gives them separately. */
    std::vector<uint8_t> config() const;
    std::string error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

/** The virtual camera "Hydra Camera" (Windows 11: MFCreateVirtualCamera), for this Windows session. Exists while
    the object lives (and Hydra runs). Apps see it only if the camera source is registered (setup below). */
class VirtualCamera
{
public:
    VirtualCamera();
    ~VirtualCamera();
    bool start (std::string& error);
    void stop();
    bool isStarted() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

/** Windows 11 (the virtual camera API exists). */
bool virtualCameraSupported();

/** Setup of the camera source (camera/HydraCamera.cpp, embedded in Hydra's exe as resource 203). The Frame Server
    (LocalService) cannot see per-user COM registrations or read the user's profile: the DLL goes to
    %ProgramFiles%\Hydra Mixer\Camera\HydraCamera-<hash>.dll and is registered under HKLM (administrator, once). */
enum class SetupState { ok, missing, outdated, noResource };
SetupState setupState();
/** Elevated: copies the embedded DLL and registers it (install) or removes both. 0 = done. */
int setupMachine (bool install);
/** The embedded DLL (from this process' exe). */
bool embeddedDll (const void*& data, size_t& bytes);
/** Where setupMachine puts this version. */
std::wstring installedDllPath();
} // namespace hydra::cam

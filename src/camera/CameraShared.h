#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <windows.h>

/** Hydra Camera: the frames Hydra hands to the virtual camera (camera/HydraCamera.cpp, loaded by Windows' Frame Server
    service, not by Hydra). Shared memory + an event, both created by the camera source (the Frame Server runs as
    LocalService in session 0 and may create "Global\" objects; a normal user process may not) with a DACL that lets
    the interactive user write. Hydra opens them (engine/PhoneCamera.cpp).

    Layout: 4 KB header, then two NV12 slots of the largest size. Hydra writes the slot that is not the newest
    (seqlock per slot: odd while writing), then makes it the newest and sets the event. The source copies the newest
    slot for each sample an app asks for. Hydra only decodes while the source says an app watches (consumerTick
    fresh, wantWidth / wantHeight = the format the app chose). Changing anything here: raise kVersion. */
namespace hydra::camshare
{
/** COM class of the camera source (HKLM\SOFTWARE\Classes\CLSID\<this>\InprocServer32 = HydraCamera.dll). */
constexpr wchar_t kClsidText[] = L"{6E3A1F52-9C7B-4D2E-A8F1-3B5C7D9E0A14}";
constexpr GUID kClsid = { 0x6e3a1f52, 0x9c7b, 0x4d2e, { 0xa8, 0xf1, 0x3b, 0x5c, 0x7d, 0x9e, 0x0a, 0x14 } };
constexpr wchar_t kFriendlyName[] = L"Hydra Camera";

constexpr wchar_t kMappingName[] = L"HydraCamera.Frames.1";
constexpr wchar_t kEventName[]   = L"HydraCamera.NewFrame.1";
constexpr uint32_t kMagic = 0x4D414348u;   // "HCAM"
constexpr uint32_t kVersion = 2;                     // 2 (18.11.): up to 4K (was 1080p)
constexpr int kMaxWidth = 3840, kMaxHeight = 2160;   // two 4K NV12 slots: ~25 MB, only while the camera source runs
constexpr uint32_t kSlotBytes = (uint32_t) kMaxWidth * kMaxHeight * 3 / 2;
constexpr uint32_t kDataOffset = 4096;
constexpr uint64_t kMappingBytes = kDataOffset + 2ull * kSlotBytes;
/** Who may open the objects: system, Local Service, administrators, the interactive user. */
constexpr wchar_t kSddl[] = L"D:(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;BA)(A;;GA;;;IU)";

struct Slot
{
    volatile LONG seq;           // odd while Hydra writes
    uint32_t width, height;      // NV12, stride = width
    uint32_t pad;
};

struct Header
{
    uint32_t magic, version;
    volatile LONG wantWidth, wantHeight;   // set by the source while an app streams (0 = stopped)
    volatile LONG64 consumerTick;          // GetTickCount64 of the last sample the source delivered
    volatile LONG64 producerTick;          // GetTickCount64 of Hydra's last frame
    volatile LONG latest;                  // slot of the newest frame
    volatile LONG frames;                  // grows with each frame Hydra writes (0 = none yet)
    Slot slots[2];
};
static_assert (sizeof (Header) <= kDataOffset);

/** "Global\<base>"; the tests use "Local\<base>.test" (environment HYDRA_CAMERA_TEST=1 for the source in the
    test process, PhoneCamera::testNames for Hydra's side), so a test never meets a running Hydra. */
inline std::wstring objectName (const wchar_t* base, bool test)
{
    return test ? std::wstring (L"Local\\") + base + L".test" : std::wstring (L"Global\\") + base;
}

inline uint8_t* slotData (void* base, int i) noexcept { return (uint8_t*) base + kDataOffset + (size_t) i * kSlotBytes; }

/** Hydra side: writes one NV12 frame (stride = width). */
inline void writeFrame (void* base, const uint8_t* nv12, int width, int height) noexcept
{
    auto* h = (Header*) base;
    const int s = h->frames == 0 ? 0 : (h->latest ^ 1);
    auto& slot = h->slots[s];
    InterlockedIncrement (&slot.seq);
    slot.width = (uint32_t) width;
    slot.height = (uint32_t) height;
    std::memcpy (slotData (base, s), nv12, (size_t) width * height * 3 / 2);
    InterlockedIncrement (&slot.seq);
    InterlockedExchange (&h->latest, s);
    InterlockedIncrement (&h->frames);
    InterlockedExchange64 (&h->producerTick, (LONG64) GetTickCount64());
}

/** Source side: copies the newest frame if it has exactly this size. False: none, another size, or torn twice. */
inline bool readFrame (void* base, uint8_t* nv12, int width, int height) noexcept
{
    auto* h = (Header*) base;
    if (h->frames == 0) return false;
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        const int s = h->latest & 1;
        auto& slot = h->slots[s];
        const LONG a = InterlockedCompareExchange (&slot.seq, 0, 0);
        if ((a & 1) != 0) continue;
        if ((int) slot.width != width || (int) slot.height != height) return false;
        std::memcpy (nv12, slotData (base, s), (size_t) width * height * 3 / 2);
        if (InterlockedCompareExchange (&slot.seq, 0, 0) == a) return true;
    }
    return false;
}
} // namespace hydra::camshare

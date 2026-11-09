#pragma once
#include "Common.h"

struct OpusEncoder;
struct OpusDecoder;

namespace hydra::net
{
/** How the audio of a Hydra Link stream travels. The numbers are part of the packet format. */
enum class Codec : uint8_t
{
    automatic = 0,   // output setting only: PCM 24 in the local network, Opus everywhere else
    pcm16 = 1,
    pcm24 = 2,
    float32 = 3,
    opus = 4
};

juce::String codecName (Codec);           // "PCM 24", "Opus" ...
juce::String codecId (Codec);             // "pcm24", "opus" ... (stored in the device name)
Codec codecFromId (const juce::String&);  // unknown -> automatic

/** Bytes per sample of an uncompressed codec, 0 for Opus / automatic. */
constexpr int pcmBytes (Codec c) noexcept
{
    return c == Codec::pcm16 ? 2 : c == Codec::pcm24 ? 3 : c == Codec::float32 ? 4 : 0;
}

/** Planar float -> interleaved little-endian PCM. 16 bit gets TPDF dither. */
void encodePcm (Codec, const float* const* in, int channels, int frames, uint8_t* out) noexcept;
/** Interleaved little-endian PCM -> planar float. */
void decodePcm (Codec, const uint8_t* in, int channels, int frames, float* const* out) noexcept;

//==============================================================================
/** Opus at 48 kHz, low-delay mode (CELT only: 2.5 ms look-ahead, no speech/music switching), in-band FEC. */
class OpusEnc
{
public:
    ~OpusEnc();
    /** frames per packet: 120 / 240 / 480 / 960 (2.5 / 5 / 10 / 20 ms). */
    bool prepare (int channels, int frames, int kbps, int expectedLossPercent);
    /** Planar input, `frames` frames. Returns the packet size or < 0. */
    int encode (const float* const* in, uint8_t* out, int maxBytes) noexcept;
    int getFrames() const noexcept { return frames; }

private:
    OpusEncoder* enc = nullptr;
    int channels = 2, frames = 240;
    std::vector<float> inter;
};

class OpusDec
{
public:
    ~OpusDec();
    bool prepare (int channels, int frames);
    /** Decodes one packet into planar output (frames frames). data == nullptr: concealment (PLC).
        fec: decode the redundancy of the packet *after* a lost one (data = that next packet) for the lost one. */
    bool decode (const uint8_t* data, int bytes, float* const* out, bool fec) noexcept;
    /** Latency the encoder adds (look-ahead), frames at 48 kHz. */
    static constexpr int kLookahead = 120;

private:
    OpusDecoder* dec = nullptr;
    int channels = 2, frames = 240;
    std::vector<float> inter;
};
} // namespace hydra::net

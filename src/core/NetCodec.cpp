#include "NetCodec.h"
#include <opus.h>

namespace hydra::net
{
juce::String codecName (Codec c)
{
    switch (c)
    {
        case Codec::pcm16:   return "PCM 16";
        case Codec::pcm24:   return "PCM 24";
        case Codec::float32: return "PCM 32f";
        case Codec::opus:    return "Opus";
        case Codec::automatic: break;
    }
    return "Auto";
}

juce::String codecId (Codec c)
{
    switch (c)
    {
        case Codec::pcm16:   return "pcm16";
        case Codec::pcm24:   return "pcm24";
        case Codec::float32: return "float32";
        case Codec::opus:    return "opus";
        case Codec::automatic: break;
    }
    return "auto";
}

Codec codecFromId (const juce::String& s)
{
    for (auto c : { Codec::pcm16, Codec::pcm24, Codec::float32, Codec::opus })
        if (s.equalsIgnoreCase (codecId (c))) return c;
    return Codec::automatic;
}

//==============================================================================
void encodePcm (Codec codec, const float* const* in, int channels, int frames, uint8_t* out) noexcept
{
    static uint32_t seed = 0x1234567u;   // dither noise (only the network thread encodes)
    auto rnd = [] { seed = seed * 1664525u + 1013904223u; return (float) (seed >> 8) * (1.0f / 16777216.0f); };

    for (int i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c)
        {
            const float x = in[c][i];
            switch (codec)
            {
                case Codec::pcm16:
                {
                    // TPDF dither of +-1 LSB: no distortion of quiet signals, noise at -96 dBFS.
                    const float d = rnd() - rnd();
                    const int v = juce::jlimit (-32768, 32767, (int) std::lrint (x * 32767.0f + d));
                    out[0] = (uint8_t) (v & 0xff);
                    out[1] = (uint8_t) ((v >> 8) & 0xff);
                    out += 2;
                    break;
                }
                case Codec::pcm24:
                {
                    const int v = juce::jlimit (-8388608, 8388607, (int) std::lrint ((double) x * 8388607.0));
                    out[0] = (uint8_t) (v & 0xff);
                    out[1] = (uint8_t) ((v >> 8) & 0xff);
                    out[2] = (uint8_t) ((v >> 16) & 0xff);
                    out += 3;
                    break;
                }
                case Codec::float32:
                    std::memcpy (out, &x, 4);   // x86: little-endian
                    out += 4;
                    break;
                default:
                    return;
            }
        }
}

void decodePcm (Codec codec, const uint8_t* in, int channels, int frames, float* const* out) noexcept
{
    for (int i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c)
        {
            float v = 0.0f;
            switch (codec)
            {
                case Codec::pcm16:
                    v = (float) (int16_t) (uint16_t) (in[0] | (in[1] << 8)) * (1.0f / 32767.0f);
                    in += 2;
                    break;
                case Codec::pcm24:
                {
                    int32_t s = (int32_t) ((uint32_t) in[0] | ((uint32_t) in[1] << 8) | ((uint32_t) in[2] << 16));
                    if (s & 0x800000) s -= 0x1000000;
                    v = (float) ((double) s * (1.0 / 8388607.0));
                    in += 3;
                    break;
                }
                case Codec::float32:
                    std::memcpy (&v, in, 4);
                    if (! std::isfinite (v)) v = 0.0f;   // never let a broken packet poison the mix
                    in += 4;
                    break;
                default:
                    return;
            }
            out[c][i] = v;
        }
}

//==============================================================================
OpusEnc::~OpusEnc()
{
    if (enc != nullptr) opus_encoder_destroy (enc);
}

bool OpusEnc::prepare (int numChannels, int numFrames, int kbps, int expectedLossPercent)
{
    if (enc != nullptr) { opus_encoder_destroy (enc); enc = nullptr; }
    channels = juce::jlimit (1, 2, numChannels);
    frames = numFrames;
    int err = 0;
    enc = opus_encoder_create (48000, channels, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
    if (enc == nullptr || err != OPUS_OK) { enc = nullptr; return false; }
    opus_encoder_ctl (enc, OPUS_SET_BITRATE (juce::jlimit (16, 510, kbps) * 1000));
    opus_encoder_ctl (enc, OPUS_SET_VBR (1));
    opus_encoder_ctl (enc, OPUS_SET_VBR_CONSTRAINT (1));   // packet sizes stay close to the bitrate (no bursts)
    opus_encoder_ctl (enc, OPUS_SET_COMPLEXITY (5));        // ~half the CPU of 10, barely audible at these rates
    opus_encoder_ctl (enc, OPUS_SET_PACKET_LOSS_PERC (juce::jlimit (0, 30, expectedLossPercent)));
    opus_encoder_ctl (enc, OPUS_SET_SIGNAL (OPUS_SIGNAL_MUSIC));
    inter.assign ((size_t) (frames * channels), 0.0f);
    return true;
}

int OpusEnc::encode (const float* const* in, uint8_t* out, int maxBytes) noexcept
{
    if (enc == nullptr) return -1;
    for (int i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c)
            inter[(size_t) (i * channels + c)] = in[c][i];
    return opus_encode_float (enc, inter.data(), frames, out, maxBytes);
}

OpusDec::~OpusDec()
{
    if (dec != nullptr) opus_decoder_destroy (dec);
}

bool OpusDec::prepare (int numChannels, int numFrames)
{
    if (dec != nullptr) { opus_decoder_destroy (dec); dec = nullptr; }
    channels = juce::jlimit (1, 2, numChannels);
    frames = numFrames;
    int err = 0;
    dec = opus_decoder_create (48000, channels, &err);
    if (dec == nullptr || err != OPUS_OK) { dec = nullptr; return false; }
    inter.assign ((size_t) (frames * channels), 0.0f);
    return true;
}

bool OpusDec::decode (const uint8_t* data, int bytes, float* const* out, bool fec) noexcept
{
    int got = -1;
    if (dec != nullptr)
        got = opus_decode_float (dec, data, data != nullptr ? bytes : 0, inter.data(), frames, fec ? 1 : 0);
    if (got != frames)
    {
        for (int c = 0; c < channels; ++c) std::memset (out[c], 0, (size_t) frames * sizeof (float));
        return false;
    }
    for (int i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c)
            out[c][i] = inter[(size_t) (i * channels + c)];
    return true;
}
} // namespace hydra::net

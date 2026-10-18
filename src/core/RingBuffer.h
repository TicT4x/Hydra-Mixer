#pragma once
#include "Common.h"

namespace hydra
{
/** Single-producer / single-consumer multichannel ring buffer (planar, power-of-two sized).
    Wait-free on both sides; safe between a device callback and the engine callback. */
class SpscAudioRing
{
public:
    void allocate (int channels, int minCapacity)
    {
        numChannels = juce::jmax (1, channels);
        capacity = (size_t) juce::nextPowerOfTwo (juce::jmax (64, minCapacity));
        mask = capacity - 1;
        data.assign ((size_t) numChannels * capacity, 0.0f);
        writePos.store (0);
        readPos.store (0);
    }

    int getNumChannels() const noexcept { return numChannels; }
    int getCapacity() const noexcept    { return (int) capacity; }

    int available() const noexcept
    {
        return (int) (writePos.load (std::memory_order_acquire) - readPos.load (std::memory_order_acquire));
    }

    int freeSpace() const noexcept { return (int) capacity - available(); }

    /** Producer. src[ch] may be nullptr (writes silence). Returns frames written. */
    int write (const float* const* src, int srcChannels, int n) noexcept
    {
        const auto w = writePos.load (std::memory_order_relaxed);
        const auto r = readPos.load (std::memory_order_acquire);
        n = juce::jmin (n, (int) (capacity - (w - r)));
        if (n <= 0) return 0;

        const size_t start = w & mask;
        const size_t first = juce::jmin ((size_t) n, capacity - start);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* dst = data.data() + (size_t) ch * capacity;
            const float* s = (ch < srcChannels) ? src[ch] : nullptr;
            if (s == nullptr && srcChannels > 0 && ch >= srcChannels)
                s = src[srcChannels - 1]; // mono source: duplicate last channel

            if (s != nullptr)
            {
                std::memcpy (dst + start, s, first * sizeof (float));
                if ((size_t) n > first) std::memcpy (dst, s + first, ((size_t) n - first) * sizeof (float));
            }
            else
            {
                std::memset (dst + start, 0, first * sizeof (float));
                if ((size_t) n > first) std::memset (dst, 0, ((size_t) n - first) * sizeof (float));
            }
        }

        writePos.store (w + (size_t) n, std::memory_order_release);
        return n;
    }

    /** Producer, interleaved source. */
    int writeInterleaved (const float* src, int srcChannels, int n) noexcept
    {
        const auto w = writePos.load (std::memory_order_relaxed);
        const auto r = readPos.load (std::memory_order_acquire);
        n = juce::jmin (n, (int) (capacity - (w - r)));
        if (n <= 0) return 0;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* dst = data.data() + (size_t) ch * capacity;
            const int sc = juce::jmin (ch, srcChannels - 1);
            for (int i = 0; i < n; ++i)
                dst[(w + (size_t) i) & mask] = src[(size_t) i * (size_t) srcChannels + (size_t) sc];
        }

        writePos.store (w + (size_t) n, std::memory_order_release);
        return n;
    }

    /** Consumer. Returns frames read; missing frames are NOT zero-filled. */
    int read (float* const* dst, int dstChannels, int n) noexcept
    {
        const auto r = readPos.load (std::memory_order_relaxed);
        const auto w = writePos.load (std::memory_order_acquire);
        n = juce::jmin (n, (int) (w - r));
        if (n <= 0) return 0;

        const size_t start = r & mask;
        const size_t first = juce::jmin ((size_t) n, capacity - start);

        for (int ch = 0; ch < dstChannels; ++ch)
        {
            if (dst[ch] == nullptr) continue;
            const float* s = data.data() + (size_t) juce::jmin (ch, numChannels - 1) * capacity;
            std::memcpy (dst[ch], s + start, first * sizeof (float));
            if ((size_t) n > first) std::memcpy (dst[ch] + first, s, ((size_t) n - first) * sizeof (float));
        }

        readPos.store (r + (size_t) n, std::memory_order_release);
        return n;
    }

    /** Consumer: drop frames (used to re-sync after a stall). */
    void skip (int n) noexcept
    {
        const auto r = readPos.load (std::memory_order_relaxed);
        const auto w = writePos.load (std::memory_order_acquire);
        n = juce::jmin (n, (int) (w - r));
        if (n > 0) readPos.store (r + (size_t) n, std::memory_order_release);
    }

private:
    std::vector<float> data;
    size_t capacity = 0, mask = 0;
    int numChannels = 0;
    alignas (64) std::atomic<size_t> writePos { 0 };
    alignas (64) std::atomic<size_t> readPos { 0 };
};
} // namespace hydra

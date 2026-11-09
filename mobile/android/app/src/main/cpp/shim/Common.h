#pragma once
// Stand-in for Hydra's src/Common.h on Android (no JUCE there): just what core/RingBuffer.h and core/Resampler.h use.
// This directory must come before src/ on the include path, so their #include "Common.h" finds this file.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace juce
{
template <typename T> constexpr T jmin (T a, T b) { return b < a ? b : a; }
template <typename T> constexpr T jmax (T a, T b) { return a < b ? b : a; }
template <typename T> constexpr T jlimit (T lo, T hi, T v) { return v < lo ? lo : (hi < v ? hi : v); }
inline int nextPowerOfTwo (int n) { int p = 1; while (p < n) p <<= 1; return p; }
template <typename T> struct MathConstants { static constexpr T pi = static_cast<T> (3.141592653589793238L); };
} // namespace juce

namespace hydra
{
constexpr int kMaxStreamChannels = 32;
constexpr int kMaxBlock = 4096;
} // namespace hydra

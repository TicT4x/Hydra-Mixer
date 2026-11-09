#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace hydra::qr
{
/** Minimal QR code encoder (ISO/IEC 18004) for the pairing link of the phone / tablet remote:
    byte mode, error correction level M, versions 1..6 (up to 106 bytes), automatic mask choice.
    No library: the algorithm follows the standard (structure as in Project Nayuki's reference encoder). */
class Code
{
public:
    /** Empty code (size() == 0) if the text is longer than version 6 can carry. */
    static Code encode (const std::string& bytes);

    int size() const noexcept { return n; }
    bool dark (int x, int y) const noexcept { return x >= 0 && y >= 0 && x < n && y < n && modules[(size_t) (y * n + x)] != 0; }
    int version() const noexcept { return ver; }
    int mask() const noexcept { return maskUsed; }

    // Building blocks, public for the tests.
    static uint8_t gfMultiply (uint8_t x, uint8_t y) noexcept;
    static std::vector<uint8_t> rsDivisor (int degree);
    static std::vector<uint8_t> rsRemainder (const std::vector<uint8_t>& data, const std::vector<uint8_t>& divisor);
    /** 15 format bits (level M) for a mask, as placed in the symbol. */
    static int formatBits (int mask) noexcept;

private:
    int n = 0, ver = 0, maskUsed = 0;
    std::vector<uint8_t> modules, isFunction;

    void setFunction (int x, int y, bool d);
    void drawFunctionPatterns();
    void drawFinder (int x, int y);
    void drawAlignment (int x, int y);
    void drawFormat (int mask);
    void placeData (const std::vector<uint8_t>& codewords);
    void applyMask (int mask);
    long penalty() const;
};
} // namespace hydra::qr

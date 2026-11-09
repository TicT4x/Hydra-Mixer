#include "QrCode.h"
#include <algorithm>
#include <climits>
#include <cstdlib>

namespace hydra::qr
{
namespace
{
// Level M, versions 1..6 (index = version).
constexpr int kEccPerBlock[7] = { 0, 10, 16, 26, 18, 24, 16 };
constexpr int kNumBlocks[7]   = { 0, 1, 1, 1, 2, 2, 4 };
constexpr int kMaxVersion = 6;

int rawDataModules (int ver)
{
    int result = (16 * ver + 128) * ver + 64;
    if (ver >= 2)
    {
        const int numAlign = ver / 7 + 2;
        result -= (25 * numAlign - 10) * numAlign - 55;
        if (ver >= 7) result -= 36;
    }
    return result;
}

int dataCodewords (int ver) { return rawDataModules (ver) / 8 - kEccPerBlock[ver] * kNumBlocks[ver]; }

bool bit (long x, int i) { return ((x >> i) & 1) != 0; }

bool maskBit (int mask, int x, int y)
{
    switch (mask)
    {
        case 0:  return (x + y) % 2 == 0;
        case 1:  return y % 2 == 0;
        case 2:  return x % 3 == 0;
        case 3:  return (x + y) % 3 == 0;
        case 4:  return (x / 3 + y / 2) % 2 == 0;
        case 5:  return x * y % 2 + x * y % 3 == 0;
        case 6:  return (x * y % 2 + x * y % 3) % 2 == 0;
        default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}
} // namespace

uint8_t Code::gfMultiply (uint8_t x, uint8_t y) noexcept
{
    int z = 0;
    for (int i = 7; i >= 0; --i)
    {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return (uint8_t) z;
}

std::vector<uint8_t> Code::rsDivisor (int degree)
{
    std::vector<uint8_t> result ((size_t) degree, 0);
    result.back() = 1;   // x^0
    uint8_t root = 1;
    for (int i = 0; i < degree; ++i)
    {
        // Multiply the product by (x - r^i).
        for (size_t j = 0; j < result.size(); ++j)
        {
            result[j] = gfMultiply (result[j], root);
            if (j + 1 < result.size()) result[j] ^= result[j + 1];
        }
        root = gfMultiply (root, 0x02);
    }
    return result;
}

std::vector<uint8_t> Code::rsRemainder (const std::vector<uint8_t>& data, const std::vector<uint8_t>& divisor)
{
    std::vector<uint8_t> result (divisor.size(), 0);
    for (auto b : data)
    {
        const uint8_t factor = (uint8_t) (b ^ result.front());
        result.erase (result.begin());
        result.push_back (0);
        for (size_t i = 0; i < result.size(); ++i)
            result[i] ^= gfMultiply (divisor[i], factor);
    }
    return result;
}

int Code::formatBits (int mask) noexcept
{
    const int data = (0 << 3) | mask;   // level M = 00
    int rem = data;
    for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    return ((data << 10) | rem) ^ 0x5412;
}

void Code::setFunction (int x, int y, bool d)
{
    modules[(size_t) (y * n + x)] = d ? 1 : 0;
    isFunction[(size_t) (y * n + x)] = 1;
}

void Code::drawFinder (int x, int y)
{
    for (int dy = -4; dy <= 4; ++dy)
        for (int dx = -4; dx <= 4; ++dx)
        {
            const int dist = std::max (std::abs (dx), std::abs (dy));
            const int xx = x + dx, yy = y + dy;
            if (xx >= 0 && xx < n && yy >= 0 && yy < n) setFunction (xx, yy, dist != 2 && dist != 4);
        }
}

void Code::drawAlignment (int x, int y)
{
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx)
            setFunction (x + dx, y + dy, std::max (std::abs (dx), std::abs (dy)) != 1);
}

void Code::drawFormat (int mask)
{
    const int bits = formatBits (mask);
    for (int i = 0; i <= 5; ++i) setFunction (8, i, bit (bits, i));
    setFunction (8, 7, bit (bits, 6));
    setFunction (8, 8, bit (bits, 7));
    setFunction (7, 8, bit (bits, 8));
    for (int i = 9; i < 15; ++i) setFunction (14 - i, 8, bit (bits, i));
    for (int i = 0; i < 8; ++i) setFunction (n - 1 - i, 8, bit (bits, i));
    for (int i = 8; i < 15; ++i) setFunction (8, n - 15 + i, bit (bits, i));
    setFunction (8, n - 8, true);   // the dark module
}

void Code::drawFunctionPatterns()
{
    for (int i = 0; i < n; ++i)
    {
        setFunction (6, i, i % 2 == 0);
        setFunction (i, 6, i % 2 == 0);
    }
    drawFinder (3, 3);
    drawFinder (n - 4, 3);
    drawFinder (3, n - 4);
    if (ver >= 2)
    {
        // Versions 2..6 have one alignment pattern (positions 6 and n - 7; only n - 7 / n - 7 is free).
        drawAlignment (n - 7, n - 7);
    }
    drawFormat (0);   // reserves the area, rewritten with the chosen mask
}

void Code::placeData (const std::vector<uint8_t>& data)
{
    size_t i = 0;
    for (int right = n - 1; right >= 1; right -= 2)
    {
        if (right == 6) right = 5;   // skip the vertical timing column
        for (int vert = 0; vert < n; ++vert)
            for (int j = 0; j < 2; ++j)
            {
                const int x = right - j;
                const bool upward = ((right + 1) & 2) == 0;
                const int y = upward ? n - 1 - vert : vert;
                if (! isFunction[(size_t) (y * n + x)] && i < data.size() * 8)
                {
                    modules[(size_t) (y * n + x)] = bit (data[i >> 3], 7 - (int) (i & 7)) ? 1 : 0;
                    ++i;
                }
            }
    }
}

void Code::applyMask (int mask)
{
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            if (! isFunction[(size_t) (y * n + x)] && maskBit (mask, x, y))
                modules[(size_t) (y * n + x)] ^= 1;
}

long Code::penalty() const
{
    long result = 0;
    auto at = [this] (int x, int y) { return modules[(size_t) (y * n + x)] != 0; };

    // 1: runs of five or more modules of one colour in a row / column.
    for (int pass = 0; pass < 2; ++pass)
        for (int a = 0; a < n; ++a)
        {
            int run = 0;
            bool colour = false;
            for (int b = 0; b < n; ++b)
            {
                const bool c = pass == 0 ? at (b, a) : at (a, b);
                if (b == 0 || c != colour) { colour = c; run = 1; }
                else if (++run == 5) result += 3;
                else if (run > 5) result += 1;
            }
        }
    // 2: 2x2 blocks of one colour.
    for (int y = 0; y < n - 1; ++y)
        for (int x = 0; x < n - 1; ++x)
        {
            const bool c = at (x, y);
            if (c == at (x + 1, y) && c == at (x, y + 1) && c == at (x + 1, y + 1)) result += 3;
        }
    // 3: finder-like patterns (1:1:3:1:1 with four light modules on one side; outside counts as light).
    static constexpr bool patA[11] = { 1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0 };
    static constexpr bool patB[11] = { 0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1 };
    for (int pass = 0; pass < 2; ++pass)
        for (int a = 0; a < n; ++a)
            for (int b = -10; b < n; ++b)
            {
                bool okA = true, okB = true;
                for (int k = 0; k < 11 && (okA || okB); ++k)
                {
                    const int p = b + k;
                    const bool c = p >= 0 && p < n && (pass == 0 ? at (p, a) : at (a, p));
                    okA = okA && c == patA[k];
                    okB = okB && c == patB[k];
                }
                if (okA) result += 40;
                if (okB) result += 40;
            }
    // 4: balance of dark and light.
    long dark = 0;
    for (auto m : modules) dark += m;
    const long total = (long) n * n;
    const long k = (std::abs (dark * 20 - total * 10) + total - 1) / total - 1;
    result += k * 10;
    return result;
}

Code Code::encode (const std::string& text)
{
    Code q;
    int ver = 1;
    // Bits: 4 (mode) + 8 (count, versions 1..9) + 8 per byte.
    while (ver <= kMaxVersion && 12 + 8 * (int) text.size() > dataCodewords (ver) * 8) ++ver;
    if (ver > kMaxVersion) return q;

    // Data bits.
    std::vector<bool> bits;
    auto append = [&bits] (uint32_t v, int count) { for (int i = count - 1; i >= 0; --i) bits.push_back (((v >> i) & 1) != 0); };
    append (0x4, 4);   // byte mode
    append ((uint32_t) text.size(), 8);
    for (unsigned char c : text) append (c, 8);
    const size_t capacity = (size_t) dataCodewords (ver) * 8;
    append (0, (int) std::min<size_t> (4, capacity - bits.size()));   // terminator
    append (0, (int) ((8 - bits.size() % 8) % 8));
    for (uint8_t pad = 0xEC; bits.size() < capacity; pad ^= 0xEC ^ 0x11) append (pad, 8);

    std::vector<uint8_t> data (bits.size() / 8, 0);
    for (size_t i = 0; i < bits.size(); ++i) if (bits[i]) data[i >> 3] |= (uint8_t) (0x80 >> (i & 7));

    // Blocks with their error correction codewords, interleaved.
    const int numBlocks = kNumBlocks[ver], eccLen = kEccPerBlock[ver];
    const int rawCodewords = rawDataModules (ver) / 8;
    const int numShort = numBlocks - rawCodewords % numBlocks;
    const int shortLen = rawCodewords / numBlocks;
    const auto divisor = rsDivisor (eccLen);
    std::vector<std::vector<uint8_t>> blocks;
    for (int i = 0, k = 0; i < numBlocks; ++i)
    {
        const int len = shortLen - eccLen + (i < numShort ? 0 : 1);
        std::vector<uint8_t> blk (data.begin() + k, data.begin() + k + len);
        k += len;
        const auto ecc = rsRemainder (blk, divisor);
        if (i < numShort) blk.push_back (0);   // placeholder, skipped when interleaving
        blk.insert (blk.end(), ecc.begin(), ecc.end());
        blocks.push_back (std::move (blk));
    }
    std::vector<uint8_t> codewords;
    for (size_t i = 0; i < blocks[0].size(); ++i)
        for (size_t j = 0; j < blocks.size(); ++j)
            if (i != (size_t) (shortLen - eccLen) || (int) j >= numShort)
                codewords.push_back (blocks[j][i]);

    q.ver = ver;
    q.n = ver * 4 + 17;
    q.modules.assign ((size_t) (q.n * q.n), 0);
    q.isFunction.assign ((size_t) (q.n * q.n), 0);
    q.drawFunctionPatterns();
    q.placeData (codewords);

    // The mask with the lowest penalty.
    long best = LONG_MAX;
    int bestMask = 0;
    for (int m = 0; m < 8; ++m)
    {
        q.applyMask (m);
        q.drawFormat (m);
        const long p = q.penalty();
        if (p < best) { best = p; bestMask = m; }
        q.applyMask (m);   // undo (XOR)
    }
    q.applyMask (bestMask);
    q.drawFormat (bestMask);
    q.maskUsed = bestMask;
    return q;
}
} // namespace hydra::qr

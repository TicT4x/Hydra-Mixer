#include "StemDsp.h"
#include <map>

namespace hydra::stems
{
//==============================================================================
Stft::Stft()
{
    window.resize (kFft);
    for (int i = 0; i < kFft; ++i)   // torch.hann_window (periodic)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (float) i / (float) kFft);
    frame.resize (2 * kFft);
}

void Stft::forward (const float* x, int samples, float* re, float* im)
{
    const int frames = numFrames (samples);
    const int pad = kFft / 2;
    padded.resize ((size_t) (samples + 2 * pad));
    // Reflect padding (torch pad_mode = "reflect": the edge sample is not repeated).
    for (int i = 0; i < pad; ++i)
    {
        padded[(size_t) i] = x[juce::jlimit (0, samples - 1, pad - i)];
        padded[(size_t) (pad + samples + i)] = x[juce::jlimit (0, samples - 1, samples - 2 - i)];
    }
    std::memcpy (padded.data() + pad, x, (size_t) samples * sizeof (float));
    for (int t = 0; t < frames; ++t)
    {
        const float* src = padded.data() + (size_t) t * kHop;
        for (int i = 0; i < kFft; ++i) frame[(size_t) i] = src[i] * window[(size_t) i];
        std::fill (frame.begin() + kFft, frame.end(), 0.0f);
        fft.performRealOnlyForwardTransform (frame.data(), true);
        for (int f = 0; f < kBins; ++f)
        {
            re[(size_t) f * (size_t) frames + (size_t) t] = frame[(size_t) (2 * f)];
            im[(size_t) f * (size_t) frames + (size_t) t] = frame[(size_t) (2 * f + 1)];
        }
    }
}

void Stft::inverse (const float* re, const float* im, int frames, float* y, int samples)
{
    const int total = kFft + kHop * (frames - 1);
    ola.assign ((size_t) total, 0.0f);
    env.assign ((size_t) total, 0.0f);
    for (int t = 0; t < frames; ++t)
    {
        for (int f = 0; f < kBins; ++f)
        {
            frame[(size_t) (2 * f)]     = re[(size_t) f * (size_t) frames + (size_t) t];
            frame[(size_t) (2 * f + 1)] = im[(size_t) f * (size_t) frames + (size_t) t];
        }
        // irfft ignores the imaginary parts of DC and Nyquist; make that explicit for JUCE's implementation.
        frame[1] = 0.0f;
        frame[(size_t) (2 * (kBins - 1) + 1)] = 0.0f;
        for (int f = kBins; f < kFft; ++f)   // negative frequencies: conjugate mirror
        {
            frame[(size_t) (2 * f)]     =  frame[(size_t) (2 * (kFft - f))];
            frame[(size_t) (2 * f + 1)] = -frame[(size_t) (2 * (kFft - f) + 1)];
        }
        fft.performRealOnlyInverseTransform (frame.data());   // scaled by 1 / kFft like irfft
        float* dst = ola.data() + (size_t) t * kHop;
        float* e = env.data() + (size_t) t * kHop;
        for (int i = 0; i < kFft; ++i)
        {
            dst[i] += frame[(size_t) i] * window[(size_t) i];
            e[i] += window[(size_t) i] * window[(size_t) i];
        }
    }
    const int pad = kFft / 2;
    for (int i = 0; i < samples; ++i)
    {
        const int k = pad + i;
        y[i] = k < total && env[(size_t) k] > 1.0e-11f ? ola[(size_t) k] / env[(size_t) k] : 0.0f;
    }
}

//==============================================================================
namespace
{
double besselI0 (double x)
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 64; ++k)
    {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < sum * 1.0e-12) break;
    }
    return sum;
}
} // namespace

Resampler::Resampler (double s) : step (s)
{
    constexpr double zeroCrossings = 32.0, beta = 9.0;
    cutoff = 0.95 * juce::jmin (1.0, 1.0 / step);   // relative to the input's Nyquist
    width = zeroCrossings / cutoff;
    const int n = (int) std::ceil (width * kTableRes) + 2;
    table.resize ((size_t) n);
    const double i0b = besselI0 (beta);
    for (int i = 0; i < n; ++i)
    {
        const double x = (double) i / kTableRes;
        if (x >= width) { table[(size_t) i] = 0.0f; continue; }
        const double sx = juce::MathConstants<double>::pi * cutoff * x;
        const double sinc = x == 0.0 ? 1.0 : std::sin (sx) / sx;
        const double r = x / width;
        const double win = besselI0 (beta * std::sqrt (juce::jmax (0.0, 1.0 - r * r))) / i0b;
        table[(size_t) i] = (float) (cutoff * sinc * win);
    }

    // Rational step p / q (q <= 2000): taps per phase f = phi / q for k = centre - hw + 1 + t, x = f + hw - 1 - t.
    for (int q = 1; q <= 2000; ++q)
    {
        const double p = std::round (step * q);
        if (std::abs (p / q - step) < 1.0e-12)
        {
            phases = q;
            break;
        }
    }
    if (phases > 0)
    {
        const int hw = (int) std::ceil (width);
        taps = 2 * hw;
        poly.resize ((size_t) phases * (size_t) taps);
        for (int ph = 0; ph < phases; ++ph)
            for (int t = 0; t < taps; ++t)
                poly[(size_t) ph * (size_t) taps + (size_t) t] = kernel ((double) ph / phases + hw - 1 - t);
    }
}

float Resampler::kernel (double x) const noexcept
{
    const double a = std::abs (x) * kTableRes;
    const auto i = (size_t) a;
    if (i + 1 >= table.size()) return 0.0f;
    const float f = (float) (a - (double) i);
    return table[i] + f * (table[i + 1] - table[i]);
}

void Resampler::process (const float* in, int inLen, double start, float* out, int outLen) const
{
    const int hw = (int) std::ceil (width);
    for (int m = 0; m < outLen; ++m)
    {
        const double pos = start + step * m;
        const int centre = (int) std::floor (pos);
        const int lo = centre - hw + 1, hi = centre + hw;
        if (phases > 0 && lo >= 0 && hi < inLen)
        {
            const double ph = (pos - centre) * phases;
            const int phase = (int) std::lround (ph);
            if (std::abs (ph - phase) < 1.0e-6 && phase < phases)
            {
                const float* h = poly.data() + (size_t) phase * (size_t) taps;
                const float* x = in + lo;
                float acc = 0.0f;
                for (int t = 0; t < taps; ++t) acc += x[t] * h[t];
                out[m] = acc;
                continue;
            }
        }
        float acc = 0.0f;
        for (int k = juce::jmax (0, lo); k <= juce::jmin (inLen - 1, hi); ++k) acc += in[k] * kernel (pos - k);
        out[m] = acc;
    }
}

//==============================================================================
// Minimal protobuf reading / writing for patchSplits (onnx.proto field numbers).
namespace
{
struct PbReader
{
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;

    uint64_t varint()
    {
        uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 7)
        {
            if (p >= end) break;
            const uint8_t b = *p++;
            v |= (uint64_t) (b & 0x7f) << shift;
            if ((b & 0x80) == 0) return v;
        }
        ok = false;
        return 0;
    }
    /** Next field: number, wire type, value span (payload for length-delimited fields). false at the end / on error. */
    bool next (int& field, int& wire, const uint8_t*& vs, const uint8_t*& ve)
    {
        if (p >= end || ! ok) return false;
        const uint64_t key = varint();
        field = (int) (key >> 3);
        wire = (int) (key & 7);
        vs = p;
        switch (wire)
        {
            case 0: varint(); break;
            case 1: p += 8; break;
            case 2:
            {
                const uint64_t n = varint();
                if (! ok || n > (uint64_t) (end - p)) { ok = false; return false; }
                vs = p;
                p += n;
                break;
            }
            case 5: p += 4; break;
            default: ok = false; return false;
        }
        if (p > end) ok = false;
        ve = p;
        return ok;
    }
};

uint64_t readVarint (const uint8_t* s, const uint8_t* e) { PbReader r { s, e }; return r.varint(); }
std::string str (const uint8_t* s, const uint8_t* e) { return std::string ((const char*) s, (size_t) (e - s)); }

void putVarint (juce::MemoryOutputStream& o, uint64_t v)
{
    while (v >= 0x80) { o.writeByte ((char) (uint8_t) ((v & 0x7f) | 0x80)); v >>= 7; }
    o.writeByte ((char) (uint8_t) v);
}
void putTag (juce::MemoryOutputStream& o, int field, int wire) { putVarint (o, ((uint64_t) field << 3) | (uint64_t) wire); }
void putBytes (juce::MemoryOutputStream& o, int field, const void* d, size_t n)
{
    putTag (o, field, 2);
    putVarint (o, n);
    o.write (d, n);
}
void putString (juce::MemoryOutputStream& o, int field, const std::string& s) { putBytes (o, field, s.data(), s.size()); }
void putMessage (juce::MemoryOutputStream& o, int field, const juce::MemoryOutputStream& m) { putBytes (o, field, m.getData(), m.getDataSize()); }

// AttributeProto: name 1, i 3, ints 8, type 20 (INT = 2, INTS = 7).
void putIntAttr (juce::MemoryOutputStream& node, const std::string& name, int64_t v)
{
    juce::MemoryOutputStream a;
    putString (a, 1, name);
    putTag (a, 3, 0); putVarint (a, (uint64_t) v);
    putTag (a, 20, 0); putVarint (a, 2);
    putMessage (node, 5, a);
}
void putIntsAttr (juce::MemoryOutputStream& node, const std::string& name, const std::vector<int64_t>& v)
{
    juce::MemoryOutputStream a;
    putString (a, 1, name);
    for (auto x : v) { putTag (a, 8, 0); putVarint (a, (uint64_t) x); }
    putTag (a, 20, 0); putVarint (a, 7);
    putMessage (node, 5, a);
}

// NodeProto: input 1, output 2, name 3, op_type 4, attribute 5, domain 7.
struct NodeInfo
{
    std::vector<std::string> in, out;
    std::string name, op, domain;
    std::map<std::string, std::vector<int64_t>> ints;   // INT / INTS attributes (an INT as one value)
    bool otherAttrs = false;                            // attributes of other types
    bool has (const std::string& a) const { return ints.count (a) > 0; }
    int64_t get (const std::string& a, int64_t def) const { auto it = ints.find (a); return it != ints.end() && ! it->second.empty() ? it->second[0] : def; }
};

NodeInfo readNode (const uint8_t* s, const uint8_t* e)
{
    NodeInfo n;
    PbReader r { s, e };
    int field, wire;
    const uint8_t *vs, *ve;
    while (r.next (field, wire, vs, ve))
    {
        if (wire != 2) continue;
        switch (field)
        {
            case 1: n.in.push_back (str (vs, ve)); break;
            case 2: n.out.push_back (str (vs, ve)); break;
            case 3: n.name = str (vs, ve); break;
            case 4: n.op = str (vs, ve); break;
            case 7: n.domain = str (vs, ve); break;
            case 5:
            {
                PbReader a { vs, ve };
                std::string name;
                std::vector<int64_t> values;
                bool isInt = false, other = false;
                int f2, w2;
                const uint8_t *s2, *e2;
                while (a.next (f2, w2, s2, e2))
                {
                    if (f2 == 1 && w2 == 2) name = str (s2, e2);
                    else if (f2 == 3 && w2 == 0) { values.push_back ((int64_t) readVarint (s2, e2)); isInt = true; }
                    else if (f2 == 8 && w2 == 0) { values.push_back ((int64_t) readVarint (s2, e2)); isInt = true; }
                    else if (f2 == 8 && w2 == 2) { PbReader p { s2, e2 }; while (p.p < p.end && p.ok) values.push_back ((int64_t) p.varint()); isInt = true; }
                    else if (f2 != 20) other = true;
                }
                if (isInt) n.ints[name] = values;
                else       n.otherAttrs = n.otherAttrs || other;
                break;
            }
            default: break;
        }
    }
    return n;
}

bool defaultDomain (const NodeInfo& n) { return n.domain.empty() || n.domain == "ai.onnx"; }

void putNode (juce::MemoryOutputStream& graph, const std::string& op, const std::vector<std::string>& in,
              const std::string& out, const std::string& name, const std::function<void (juce::MemoryOutputStream&)>& attrs = {})
{
    juce::MemoryOutputStream n;
    for (auto& i : in) putString (n, 1, i);
    putString (n, 2, out);
    putString (n, 3, name);
    putString (n, 4, op);
    if (attrs) attrs (n);
    putMessage (graph, 1, n);
}

/** Writes a Constant node with a 1-D int64 tensor and returns its output name. */
std::string putConstant (juce::MemoryOutputStream& graph, const std::string& name, const std::vector<int64_t>& v)
{
    putNode (graph, "Constant", {}, name, name, [&v] (juce::MemoryOutputStream& n) { putIntsAttr (n, "value_ints", v); });
    return name;
}

/** Equal two-way Split -> Shape (one dimension) / 2 = half, Slice [0, half) and [half, end). */
bool replaceSplit (const NodeInfo& s, juce::MemoryOutputStream& graph)
{
    const int64_t axis = s.get ("axis", 0);
    if (s.op != "Split" || ! defaultDomain (s) || s.out.size() != 2 || ! (s.in.size() == 1 || (s.in.size() == 2 && s.in[1].empty()))
        || s.has ("split") || axis < 0)
        return false;
    const auto base = s.name.empty() ? "hydra_split_" + s.out[0] : s.name + "_hydra";
    const auto ax   = putConstant (graph, base + "_axis", { axis });
    const auto two  = putConstant (graph, base + "_two", { 2 });
    const auto zero = putConstant (graph, base + "_zero", { 0 });
    const auto last = putConstant (graph, base + "_end", { std::numeric_limits<int64_t>::max() });
    const auto shape = base + "_shape", half = base + "_half";
    putNode (graph, "Shape", { s.in[0] }, shape, shape, [axis] (juce::MemoryOutputStream& n)
    {
        putIntAttr (n, "start", axis);
        putIntAttr (n, "end", axis + 1);
    });
    putNode (graph, "Div", { shape, two }, half, half);
    putNode (graph, "Slice", { s.in[0], zero, half, ax }, s.out[0], base + "_a");
    putNode (graph, "Slice", { s.in[0], half, last, ax }, s.out[1], base + "_b");
    return true;
}

/** 1-D ConvTranspose (x [N, C, T], w [C, 1, K], stride s, K = R * s) -> frames = w^T x [N, K, T], split into R parts of s
    samples, part q shifted by q frames, summed: [N, T + R - 1, s] -> [N, 1, (T + R - 1) s] = (T - 1) s + K samples. */
bool replaceConvTranspose (const NodeInfo& c, juce::MemoryOutputStream& graph)
{
    if (c.op != "ConvTranspose" || ! defaultDomain (c) || c.in.size() != 2 || c.out.size() != 1 || c.otherAttrs) return false;
    const auto kernel = c.ints.count ("kernel_shape") ? c.ints.at ("kernel_shape") : std::vector<int64_t>();
    const auto strides = c.ints.count ("strides") ? c.ints.at ("strides") : std::vector<int64_t> { 1 };
    if (kernel.size() != 1 || strides.size() != 1 || c.get ("group", 1) != 1 || c.has ("output_padding") || c.has ("output_shape")) return false;
    if (c.has ("pads")) for (auto p : c.ints.at ("pads")) if (p != 0) return false;
    if (c.has ("dilations")) for (auto d : c.ints.at ("dilations")) if (d != 1) return false;
    const int64_t K = kernel[0], s = strides[0];
    if (s <= 0 || K % s != 0 || K / s > 16) return false;
    const int64_t R = K / s;
    const auto base = (c.name.empty() ? "hydra_convt_" + c.out[0] : c.name) + "_hydra";
    const auto w2 = base + "_w2", wt = base + "_wt", frames = base + "_frames", f4 = base + "_f4", ft = base + "_ft";
    putNode (graph, "Squeeze", { c.in[1], putConstant (graph, base + "_ax1", { 1 }) }, w2, w2);              // [C, K]
    putNode (graph, "Transpose", { w2 }, wt, wt, [] (juce::MemoryOutputStream& n) { putIntsAttr (n, "perm", { 1, 0 }); });
    putNode (graph, "MatMul", { wt, c.in[0] }, frames, frames);                                               // [N, K, T]
    putNode (graph, "Reshape", { frames, putConstant (graph, base + "_shape4", { 0, R, s, -1 }) }, f4, f4);   // [N, R, s, T]
    putNode (graph, "Transpose", { f4 }, ft, ft, [] (juce::MemoryOutputStream& n) { putIntsAttr (n, "perm", { 0, 1, 3, 2 }); });
    std::string acc;
    for (int64_t q = 0; q < R; ++q)
    {
        const auto p = base + "_q" + std::to_string (q);
        putNode (graph, "Slice", { ft, putConstant (graph, p + "_s", { q }), putConstant (graph, p + "_e", { q + 1 }),
                                   putConstant (graph, p + "_a", { 1 }) }, p, p);                             // [N, 1, T, s]
        putNode (graph, "Squeeze", { p, putConstant (graph, p + "_sa", { 1 }) }, p + "_sq", p + "_sq");      // [N, T, s]
        putNode (graph, "Pad", { p + "_sq", putConstant (graph, p + "_pads", { 0, q, 0, 0, R - 1 - q, 0 }) }, p + "_pad", p + "_pad");
        if (acc.empty()) acc = p + "_pad";
        else
        {
            putNode (graph, "Add", { acc, p + "_pad" }, p + "_add", p + "_add");
            acc = p + "_add";
        }
    }
    putNode (graph, "Reshape", { acc, putConstant (graph, base + "_shape3", { 0, 1, -1 }) }, c.out[0], base + "_out");
    return true;
}

/** Copies a model, letting replace() write the replacement of a node (true) or keep it (false). Nodes stay in
    topological order: a replacement goes where its node was. -1 if the file is not a model of opset >= 15. */
int rewriteGraph (const juce::MemoryBlock& model, juce::MemoryBlock& result,
                  const std::function<bool (const NodeInfo&, juce::MemoryOutputStream&)>& replace)
{
    const auto* begin = static_cast<const uint8_t*> (model.getData());
    const auto* end = begin + model.getSize();

    // ModelProto: opset_import 8 (domain 1, version 2), graph 7. Shape with start / end needs opset 15.
    int64_t opset = -1;
    const uint8_t *graphS = nullptr, *graphE = nullptr;
    {
        PbReader r { begin, end };
        int field, wire;
        const uint8_t *vs, *ve;
        while (r.next (field, wire, vs, ve))
        {
            if (field == 7 && wire == 2) { graphS = vs; graphE = ve; }
            if (field == 8 && wire == 2)
            {
                PbReader o { vs, ve };
                std::string domain;
                int64_t version = -1;
                int f2, w2;
                const uint8_t *s2, *e2;
                while (o.next (f2, w2, s2, e2))
                {
                    if (f2 == 1 && w2 == 2) domain = str (s2, e2);
                    if (f2 == 2 && w2 == 0) version = (int64_t) readVarint (s2, e2);
                }
                if (domain.empty() || domain == "ai.onnx") opset = version;
            }
        }
        if (! r.ok || graphS == nullptr || opset < 15) return -1;
    }

    // GraphProto: node 1 is rewritten, the rest (initializers, inputs, outputs ...) copied.
    juce::MemoryOutputStream graph (model.getSize() + 1024 * 1024);
    int replaced = 0;
    {
        PbReader r { graphS, graphE };
        int field, wire;
        const uint8_t *vs, *ve;
        const uint8_t* fieldStart = r.p;
        while (r.next (field, wire, vs, ve))
        {
            bool copy = true;
            if (field == 1 && wire == 2 && replace (readNode (vs, ve), graph))
            {
                ++replaced;
                copy = false;
            }
            if (copy) graph.write (fieldStart, (size_t) (r.p - fieldStart));
            fieldStart = r.p;
        }
        if (! r.ok) return -1;
    }

    juce::MemoryOutputStream out (model.getSize() + 2 * 1024 * 1024);
    {
        PbReader r { begin, end };
        int field, wire;
        const uint8_t *vs, *ve;
        const uint8_t* fieldStart = r.p;
        while (r.next (field, wire, vs, ve))
        {
            if (field == 7 && wire == 2) putMessage (out, 7, graph);
            else out.write (fieldStart, (size_t) (r.p - fieldStart));
            fieldStart = r.p;
        }
    }
    result = out.getMemoryBlock();
    return replaced;
}
} // namespace

int patchSplits (const juce::MemoryBlock& model, juce::MemoryBlock& out) { return rewriteGraph (model, out, replaceSplit); }
int patchConvTranspose (const juce::MemoryBlock& model, juce::MemoryBlock& out) { return rewriteGraph (model, out, replaceConvTranspose); }

int patchForDirectML (const juce::MemoryBlock& model, juce::MemoryBlock& out)
{
    return rewriteGraph (model, out, [] (const NodeInfo& n, juce::MemoryOutputStream& g) { return replaceSplit (n, g) || replaceConvTranspose (n, g); });
}
} // namespace hydra::stems

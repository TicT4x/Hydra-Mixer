#include "PitchShift.h"
#include <signalsmith-stretch/signalsmith-stretch.h>

namespace hydra::dsp
{
namespace
{
/** Block and interval (seconds) per quality; latency 100 / 200 / 300 ms. Measured (HydraTests transpose): the
    library puts low tones slightly off pitch, the less the longer the block (worst over 30 tones / shifts:
    17 / 11 / 5 cents, at 196 Hz); its default preset (0.12 s) had 15 cents, 0.34 s blocks brought echoes back.
    Split computation: its work is spread evenly over the blocks (one interval more latency, no CPU spikes). */
struct Config { double block, interval; };
constexpr Config kConfigs[] = { { 0.080, 0.020 }, { 0.160, 0.040 }, { 0.240, 0.060 } };
constexpr bool kSplit = true;
constexpr double kFormantBaseHz = 200.0;
} // namespace

struct PitchShifter::Impl
{
    std::array<signalsmith::stretch::SignalsmithStretch<float>, numQualities> s;
    std::array<int, numQualities> latency {};
    std::array<std::vector<float>, 2> out;
    bool timbre = false;
};

PitchShifter::PitchShifter() = default;
PitchShifter::~PitchShifter() = default;

int PitchShifter::latencyFor (int q) const noexcept
{
    q = juce::jlimit (0, (int) numQualities - 1, q);
    return impl != nullptr ? impl->latency[(size_t) q] : latencyAt (sr, q);
}

int PitchShifter::latencyAt (double sampleRate, int q) noexcept
{
    // The library's latency: half a block in, half a block + one interval (split computation) out.
    const auto& c = kConfigs[juce::jlimit (0, (int) numQualities - 1, q)];
    const int block = juce::roundToInt (sampleRate * c.block), interval = juce::roundToInt (sampleRate * c.interval);
    return block + (kSplit ? interval : 0);
}

void PitchShifter::prepare (double sampleRate)
{
    sr = sampleRate;
    impl = std::make_unique<Impl>();
    for (int q = 0; q < numQualities; ++q)
    {
        auto& st = impl->s[(size_t) q];
        st.configure (2, juce::roundToInt (sr * kConfigs[q].block), juce::roundToInt (sr * kConfigs[q].interval), kSplit);
        impl->latency[(size_t) q] = st.inputLatency() + st.outputLatency();
        // Keep timbre needs a rough fundamental. A fixed one: the library's own guess (0) failed with the longer
        // blocks (Normal / High: tone colour off by 10..29 dB, worse than without), while 100 / 150 / 300 Hz all
        // kept a 150 Hz vowel within 1.1..2.5 dB of its formants (without: 5..8 dB). HydraTests transpose.
        st.setFormantBase ((float) (kFormantBaseHz / sr));
    }
    for (auto& o : impl->out) o.assign ((size_t) kChunk, 0.0f);
    for (auto& d : dry) d.assign ((size_t) kChunk, 0.0f);
    fadeStep = (float) (1.0 / (0.010 * sr));
    prepared = true;
    reset();
}

void PitchShifter::reset() noexcept
{
    if (impl != nullptr) for (auto& st : impl->s) st.reset();
    state = State::dry;
    mix = 0.0f;
    startLeft = 0;
}

void PitchShifter::run (float* l, float* r, int n, double ratio, bool keepTimbre) noexcept
{
    auto& st = impl->s[(size_t) quality];
    st.setTransposeFactor ((float) juce::jlimit (0.25, 4.0, ratio));
    if (keepTimbre != impl->timbre)
    {
        impl->timbre = keepTimbre;
        for (auto& s : impl->s) s.setFormantFactor (1.0f, keepTimbre);   // 1 + compensation = formants stay put
    }
    float* out[2] = { impl->out[0].data(), impl->out[1].data() };
    const float* in[2] = { l, r };
    st.process (in, n, out, n);
    std::memcpy (l, out[0], (size_t) n * sizeof (float));
    std::memcpy (r, out[1], (size_t) n * sizeof (float));
}

void PitchShifter::process (float* l, float* r, int n, double ratio, int q, bool keepTimbre) noexcept
{
    if (! prepared) return;
    q = juce::jlimit (0, (int) numQualities - 1, q);
    const bool shiftWanted = std::abs (ratio - 1.0) > 1.0e-7;
    for (int at = 0; at < n; at += kChunk)
    {
        const int m = juce::jmin (kChunk, n - at);
        float* cl = l + at;
        float* cr = r + at;

        // Still filling and nothing of it heard yet: back to dry (or restart with the new quality) at once.
        if (state == State::starting && (! shiftWanted || q != quality)) state = State::dry;
        if (state == State::dry)
        {
            quality = q;
            if (! shiftWanted) continue;   // straight through
            impl->s[(size_t) quality].reset();
            state = State::starting;
            startLeft = 2 * latencyFor (quality);
        }

        std::memcpy (dry[0].data(), cl, (size_t) m * sizeof (float));
        std::memcpy (dry[1].data(), cr, (size_t) m * sizeof (float));
        run (cl, cr, m, ratio, keepTimbre);

        if (state == State::starting)
        {
            // The stretcher fills up: the dry signal keeps playing.
            std::memcpy (cl, dry[0].data(), (size_t) m * sizeof (float));
            std::memcpy (cr, dry[1].data(), (size_t) m * sizeof (float));
            startLeft -= m;
            if (startLeft <= 0) { state = State::shifting; mix = 0.0f; }
            continue;
        }

        // Shifting: fade into the shifted signal, or out of it (back at ratio 1, or a new quality).
        const float target = shiftWanted && q == quality ? 1.0f : 0.0f;
        if (mix == 1.0f && target == 1.0f) continue;
        const float step = target > mix ? fadeStep : -fadeStep;
        for (int i = 0; i < m; ++i)
        {
            mix = juce::jlimit (0.0f, 1.0f, mix + step);
            const float w = mix * mix * (3.0f - 2.0f * mix);   // smooth step
            cl[i] = dry[0][(size_t) i] + w * (cl[i] - dry[0][(size_t) i]);
            cr[i] = dry[1][(size_t) i] + w * (cr[i] - dry[1][(size_t) i]);
        }
        if (mix == 0.0f && target == 0.0f) state = State::dry;
    }
}

void PitchShifter::processWet (float* l, float* r, int n, double ratio, int q, bool keepTimbre) noexcept
{
    if (! prepared) return;
    q = juce::jlimit (0, (int) numQualities - 1, q);
    if (q != quality) { quality = q; impl->s[(size_t) q].reset(); }
    for (int at = 0; at < n; at += kChunk)
        run (l + at, r + at, juce::jmin (kChunk, n - at), ratio, keepTimbre);
}
} // namespace hydra::dsp

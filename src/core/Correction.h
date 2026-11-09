#pragma once
#include "Params.h"
#include "Dsp.h"

namespace hydra
{
/** A correction curve for one hardware output (room + speaker, or headphone correction): any number of filter bands
    (up to kMaxBands) plus a preamp that keeps boosts from clipping. Made by the room measurement's Auto EQ
    or imported from an Equalizer APO / AutoEq parametric EQ file. Message thread data. */
struct CorrectionProfile
{
    static constexpr int kMaxBands = 32;
    /** What the profile corrects. Room and speaker are one kind: a measurement always captures both together
        (1 was a separate "Speaker" kind once and is read as room). */
    enum class Kind { room = 0, headphone = 2, other = 3 };
    static constexpr Kind kKinds[] = { Kind::room, Kind::headphone, Kind::other };
    static juce::String kindName (Kind k) { return k == Kind::headphone ? "Headphone" : k == Kind::other ? "Other" : "Room + Speaker"; }

    struct Band
    {
        int type = 0;   // ParamEq::BandType (Bell, Notch, BandPass, HighCut, LowCut, LowShelf, HighShelf)
        float freq = 1000.0f, gainDb = 0.0f, q = 0.707f;
        bool operator== (const Band& o) const { return type == o.type && freq == o.freq && gainDb == o.gainDb && q == o.q; }
    };

    /** One side's filters: up to kMaxBands bands plus a preamp. */
    struct Side
    {
        float preampDb = 0.0f;
        std::vector<Band> bands;
        bool isEmpty() const noexcept { return bands.empty() && preampDb == 0.0f; }
        bool operator== (const Side& o) const { return preampDb == o.preampDb && bands == o.bands; }
    };

    juce::String name;      // e.g. the measurement it came from or the imported file
    juce::String source;    // file name of the room measurement it was designed from (empty: imported / made by hand)
    /** Entry of the shared correction pool (%APPDATA%/HydraMixer/Corrections) this profile is linked to: the mixer's
        outputs and the Room Correction plugin follow changes of it (pool::Link). Empty: not in the pool (yet). */
    juce::String poolId;
    Kind kind = Kind::room;
    float preampDb = 0.0f;  // both channels, or the left one when 'separate'
    std::vector<Band> bands;
    /** Left and right speaker corrected separately (each stands at another place in the room): preampDb / bands
        are then the left channel's, 'right' the right one's. */
    bool separate = false;
    Side right;
    /** Time alignment: delay of the left / right channel (ms, up to kMaxAlignMs) so both speakers arrive at the
        listening position together (the nearer one waits). Usually only one of them is > 0. */
    static constexpr float kMaxAlignMs = 20.0f;
    float alignMs[2] { 0.0f, 0.0f };

    /** How the profile was designed from its room measurement (the Edit page shows and changes it). */
    struct Design
    {
        bool valid = false;           // made by hand / imported: no design
        double lowHz = 20.0, highHz = 500.0, maxCutDb = 12.0, maxBoostDb = 0.0, detail = 12.0;
        int bands = 16;
        bool keepLoudness = true, useNear = true, timeAlign = true;
        double levelOffsetDb = 0.0;   // the level slider of the Correction page moved the preamps by this much
        bool refined = false;         // bands from a validation measurement were added (the wizard's "Refine")
    };
    Design design;

    bool isEmpty() const noexcept
    {
        return bands.empty() && preampDb == 0.0f && (! separate || right.isEmpty()) && alignMs[0] == 0.0f && alignMs[1] == 0.0f;
    }
    juce::String describe() const;   // "Room + Speaker, 14 bands, preamp -3.2 dB" / "..., L 12 / R 10 bands, ..."

    /** The filters of one channel (0 = left, 1 = right). */
    Side side (int channel) const { return separate && channel == 1 ? right : Side { preampDb, bands }; }
    void setSide (int channel, Side s);   // channel 0 / 1: that side only (the profile becomes separate), -1: both
    /** Response of one channel in dB at f (preamp included). */
    double magnitudeDb (double f, double sampleRate = 48000.0, int channel = 0) const;
    /** Highest boost of the bands (without preamp) between 20 Hz and 20 kHz over both channels, >= 0. */
    double maxBoostDb (double sampleRate = 48000.0) const;
    /** Highest level of the whole correction (bands + preamp) over both channels: > 0 dB can clip. */
    double maxGainDb (double sampleRate = 48000.0) const;

    std::unique_ptr<juce::XmlElement> toXml() const;
    static CorrectionProfile fromXml (const juce::XmlElement* xml);

    /** Equalizer APO / AutoEq "ParametricEQ.txt": "Preamp: -6 dB" and "Filter n: ON PK Fc 100 Hz Gain -3 dB Q 1.4"
        lines (PK / PEQ, LSC / LS, HSC / HS, LP / LPQ, HP / HPQ, NO, BP). "Channel: L" / "Channel: R" / "Channel: all"
        sections make a separate left / right profile. Returns false (with error) if it holds no filter. */
    static bool fromApo (const juce::String& text, CorrectionProfile& out, juce::String& error);
    juce::String toApo() const;
};

/** The Correction stage of a hardware output (first in its chain): runs the profile's bands as zero-latency biquads.
    The message thread builds an immutable coefficient set and publishes it with one atomic pointer; the
    audio thread only reads it. Retired sets are freed once the audio thread has moved on (Engine timer). */
class Correction
{
public:
    Param* on {};   // nullptr: this channel has no correction (process does nothing)

    // Message thread
    const CorrectionProfile& getProfile() const noexcept { return profile; }
    uint32_t getVersion() const noexcept { return version; }
    void setProfile (CorrectionProfile p, double sampleRate, uint64_t callbackCount);
    /** Rebuilds the coefficients after a sample rate change and frees retired sets. */
    void update (double sampleRate, uint64_t callbackCount, bool running);

    // Audio thread
    void reset() noexcept;
    void process (float* l, float* r, int n, double sampleRate) noexcept;

private:
    struct Coeffs
    {
        double sampleRate = 0.0;
        int count[2] {};    // left, right
        float preamp[2] { 1.0f, 1.0f };
        std::array<dsp::BiquadCoeffs, CorrectionProfile::kMaxBands> c[2] {};
        int delay[2] {};    // samples (time alignment)
    };
    static constexpr int kDelaySize = 4096;   // > 20 ms at 192 kHz
    void publish (double sampleRate, uint64_t callbackCount);

    CorrectionProfile profile;
    uint32_t version = 0;
    std::unique_ptr<Coeffs> owned;
    std::atomic<Coeffs*> live { nullptr };
    std::vector<std::pair<std::unique_ptr<Coeffs>, uint64_t>> retired;

    // audio thread
    float z1[CorrectionProfile::kMaxBands][2] {}, z2[CorrectionProfile::kMaxBands][2] {};
    float lastPreamp[2] { 1.0f, 1.0f };
    bool wasOn = false;
    std::vector<float> delayBuf[2] { std::vector<float> (kDelaySize, 0.0f), std::vector<float> (kDelaySize, 0.0f) };
    int delayPos = 0;
};
} // namespace hydra

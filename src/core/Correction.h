#pragma once
#include "Params.h"
#include "Dsp.h"

namespace hydra
{
/** A correction curve for one channel (room, speaker or headphone correction): any number of filter bands
    (up to kMaxBands) plus a preamp that keeps boosts from clipping. Made by the room measurement's Auto EQ
    or imported from an Equalizer APO / AutoEq parametric EQ file. Message thread data. */
struct CorrectionProfile
{
    static constexpr int kMaxBands = 32;
    enum class Kind { room = 0, speaker, headphone, other };
    static juce::StringArray kindNames() { return { "Room", "Speaker", "Headphone", "Other" }; }

    struct Band
    {
        int type = 0;   // ParamEq::BandType (Bell, Notch, BandPass, HighCut, LowCut, LowShelf, HighShelf)
        float freq = 1000.0f, gainDb = 0.0f, q = 0.707f;
        bool operator== (const Band& o) const { return type == o.type && freq == o.freq && gainDb == o.gainDb && q == o.q; }
    };

    juce::String name;      // e.g. the measurement it came from or the imported file
    Kind kind = Kind::room;
    float preampDb = 0.0f;
    std::vector<Band> bands;

    bool isEmpty() const noexcept { return bands.empty() && preampDb == 0.0f; }
    juce::String describe() const;   // "Room, 14 bands, preamp -3.2 dB"

    /** Response of the whole profile in dB at f (preamp included). */
    double magnitudeDb (double f, double sampleRate = 48000.0) const;
    /** Highest boost of the bands (without preamp) between 20 Hz and 20 kHz, >= 0. */
    double maxBoostDb (double sampleRate = 48000.0) const;

    std::unique_ptr<juce::XmlElement> toXml() const;
    static CorrectionProfile fromXml (const juce::XmlElement* xml);

    /** Equalizer APO / AutoEq "ParametricEQ.txt": "Preamp: -6 dB" and "Filter n: ON PK Fc 100 Hz Gain -3 dB Q 1.4"
        lines (PK / PEQ, LSC / LS, HSC / HS, LP / LPQ, HP / HPQ, NO, BP). Returns false (with error) if it holds no filter. */
    static bool fromApo (const juce::String& text, CorrectionProfile& out, juce::String& error);
    juce::String toApo() const;
};

/** The Correction stage of an input strip or a bus: runs the profile's bands as zero-latency biquads.
    The message thread builds an immutable coefficient set and publishes it with one atomic pointer; the
    audio thread only reads it. Retired sets are freed once the audio thread has moved on (Engine timer). */
class Correction
{
public:
    Param* on {};

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
        int count = 0;
        float preamp = 1.0f;
        std::array<dsp::BiquadCoeffs, CorrectionProfile::kMaxBands> c {};
    };
    void publish (double sampleRate, uint64_t callbackCount);

    CorrectionProfile profile;
    uint32_t version = 0;
    std::unique_ptr<Coeffs> owned;
    std::atomic<Coeffs*> live { nullptr };
    std::vector<std::pair<std::unique_ptr<Coeffs>, uint64_t>> retired;

    // audio thread
    float z1[CorrectionProfile::kMaxBands][2] {}, z2[CorrectionProfile::kMaxBands][2] {};
    float lastPreamp = 1.0f;
    bool wasOn = false;
};
} // namespace hydra

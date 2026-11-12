#pragma once
#include "core/Params.h"
#include "core/PitchShift.h"
#include "core/KeyDetect.h"

namespace hydra
{
/** Transpose (inputs, once per channel): pitch shift in semitones + fine tuning (core/PitchShift.h), plus one-shot
    listeners (each hears ~10 s of music once, then stops; meanwhile the audio thread pays one atomic read per block):
    - Find Key: key, second choice, their probabilities and how far the song is off A = 440 Hz (Fine stays).
    - Add Part: hears another part of the song and combines it with what was heard before (each part counts the
      same), for a more reliable key; without a key found yet it is Find Key.
    - Auto Tune: sets Fine so the song is in tune; uses the key found, listens first only without one.
    Ids "inN.tp.*": on, semi, fine, quality, timbre; auto / findkey / addpart = triggers (not stored); the result: key, key2
    (0 = none, else 1 + key 0..23), prob, prob2 (0..1), cents, parts (how many parts were heard), pc0..pc11 (how
    strongly each pitch class was heard, 0..1). */
struct TransposeFx
{
    Param *on {}, *semi {}, *fine {}, *quality {}, *autoTune {}, *findKey {}, *addPart {}, *key {}, *cents {};
    Param *key2 {}, *prob {}, *prob2 {}, *parts {};
    Param *timbre {};   // "tp.timbre": Keep timbre (formant correction), default off
    /** How strongly each pitch class (C = 0) was heard by the last Find Key / Auto Tune (0..1, "tp.pc0" ...): the
        window marks the notes found. */
    std::array<Param*, 12> heard {};
    std::unique_ptr<dsp::PitchShifter> dsp;            // created when the effect is first added (message thread)
    std::atomic<dsp::PitchShifter*> live { nullptr };   // what the audio thread reads
    bool wasOn = false;                                 // audio thread

    /** Frequency ratio of the current setting. */
    double ratio() const noexcept { return std::pow (2.0, ((double) semi->get() + (double) fine->get() * 0.01) / 12.0); }

    //--------------------------------------------------------------------------
    // Listening (Auto Tune / Find Key).
    enum class State { idle, listening, analysing, done, noSignal };
    static constexpr double kListenSeconds = 10.0, kMinSeconds = 3.0, kTimeoutSeconds = 40.0;

    /** Audio thread: the signal at the effect's place in the chain (before the shift). Blocks quieter than
        -60 dBFS are skipped (pauses do not count). */
    void feed (const float* l, const float* r, int n) noexcept
    {
        auto* j = capture.load (std::memory_order_acquire);
        if (j == nullptr) return;
        const int at = j->filled.load (std::memory_order_relaxed);
        const int take = juce::jmin (n, (int) j->buf.size() - at);
        if (take <= 0) return;
        float pk = 0.0f;
        for (int i = 0; i < take; ++i)
        {
            const float m = 0.5f * (l[i] + r[i]);
            j->buf[(size_t) (at + i)] = m;
            pk = juce::jmax (pk, std::abs (m));
        }
        if (pk >= 0.001f) j->filled.store (at + take, std::memory_order_release);
    }

    /** Message thread, ~10 Hz (Engine timer): starts / finishes a listener. callbacks = audio callbacks so far
        (the buffer is analysed only once the audio thread cannot be writing into it any more). */
    void tick (uint64_t callbacks, bool running, double sampleRate);

    State getState() const noexcept { return state; }
    /** While listening: true = Add Part, false = Find Key / Auto Tune. */
    bool isAddingPart() const noexcept { return state == State::listening && mode == Mode::add; }
    /** 0..1 while listening. */
    float getProgress() const noexcept
    {
        if (auto* j = job.get(); j != nullptr && ! j->buf.empty()) return (float) j->filled.load() / (float) j->buf.size();
        return 0.0f;
    }
    /** Last result (valid after done; also after a restart from key / cents). */
    const dsp::KeyResult& getResult() const noexcept { return result; }
    bool lastRunApplied() const noexcept { return applied; }
    /** Any thread: Find Key / Add Part / Auto Tune is listening (the audio thread feeds it). */
    bool isListening() const noexcept { return capture.load (std::memory_order_relaxed) != nullptr; }

private:
    struct Job
    {
        std::vector<float> buf;
        std::atomic<int> filled { 0 };
        double sampleRate = 48000.0;
        std::atomic<bool> done { false };
        dsp::KeyResult result;
    };
    std::shared_ptr<Job> job;
    std::vector<std::pair<std::shared_ptr<Job>, uint64_t>> retired;   // freed once the audio thread let go
    std::atomic<Job*> capture { nullptr };
    State state = State::idle;
    enum class Mode { find, add, tune };
    Mode mode = Mode::find;
    bool applied = false;
    bool anyTrigger() const noexcept { return autoTune->isOn() || findKey->isOn() || addPart->isOn(); }
    void store (const dsp::KeyResult&);
    double startedMs = 0.0;
    uint64_t detachedAt = 0;
    bool detached = false;
    dsp::KeyResult result;
    void finish (State s);
};
} // namespace hydra

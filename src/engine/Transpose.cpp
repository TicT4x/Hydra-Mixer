#include "Transpose.h"
#include <complex>

namespace hydra
{
void TransposeFx::finish (State s)
{
    capture.store (nullptr, std::memory_order_release);
    state = s;
    autoTune->set (0.0f);
    findKey->set (0.0f);
    addPart->set (0.0f);
}

void TransposeFx::store (const dsp::KeyResult& r)
{
    key->set ((float) (r.key + 1));
    key2->set ((float) (r.key2 + 1));
    prob->set (r.probability);
    prob2->set (r.probability2);
    cents->set (r.cents);
    for (size_t pc = 0; pc < heard.size(); ++pc) heard[pc]->set (r.chroma[pc]);
}

void TransposeFx::tick (uint64_t callbacks, bool running, double sampleRate)
{
    const bool wanted = anyTrigger();
    const double now = juce::Time::getMillisecondCounterHiRes();
    retired.erase (std::remove_if (retired.begin(), retired.end(), [&] (const auto& r) { return callbacks >= r.second || ! running; }),
                   retired.end());

    if (state != State::listening && state != State::analysing)
    {
        if (! wanted) return;
        // Auto Tune with a key already found (Find Key, an earlier run): take that result, no new listening.
        if (autoTune->isOn() && juce::roundToInt (key->get()) > 0)
        {
            fine->set (std::round (-cents->get() * 10.0f) / 10.0f);
            applied = true;
            finish (State::done);
            return;
        }
        // Start: a buffer of kListenSeconds (allocated here, the audio thread only writes into it).
        if (job != nullptr) retired.emplace_back (std::move (job), callbacks + 2);
        auto j = std::make_shared<Job>();
        j->sampleRate = sampleRate;
        j->buf.assign ((size_t) juce::roundToInt (sampleRate * kListenSeconds), 0.0f);
        job = j;
        const bool known = juce::roundToInt (key->get()) > 0;
        mode = autoTune->isOn() ? Mode::tune : addPart->isOn() && known ? Mode::add : Mode::find;
        // one listener at a time: the other buttons go off (the one that runs stays lit)
        if (mode == Mode::tune) { findKey->set (0.0f); addPart->set (0.0f); }
        else if (addPart->isOn()) findKey->set (0.0f);
        detached = false;
        startedMs = now;
        state = State::listening;
        capture.store (job.get(), std::memory_order_release);
        return;
    }

    if (state == State::listening)
    {
        if (! wanted)   // cancelled with the button
        {
            capture.store (nullptr, std::memory_order_release);
            state = State::idle;
            retired.emplace_back (std::move (job), callbacks + 2);   // the audio thread may still be writing into it
            return;
        }
        const int filled = job->filled.load();
        if (! detached && (filled >= (int) job->buf.size() || now - startedMs > kTimeoutSeconds * 1000.0))
        {
            capture.store (nullptr, std::memory_order_release);
            detached = true;
            detachedAt = callbacks + 2;
        }
        if (detached && (callbacks >= detachedAt || ! running))
        {
            if (job->filled.load() < (int) (sampleRate * kMinSeconds)) { finish (State::noSignal); return; }
            state = State::analysing;
            auto j = job;
            juce::Thread::launch (juce::Thread::Priority::low, [j]
            {
                j->result = dsp::analyseKey (j->buf.data(), j->filled.load(), j->sampleRate);
                j->done.store (true, std::memory_order_release);
            });
        }
        return;
    }

    // analysing
    if (! job->done.load (std::memory_order_acquire)) return;
    result = job->result;
    job.reset();
    if (! result.valid) { finish (State::noSignal); return; }
    const int before = juce::roundToInt (parts->get());
    if (mode == Mode::add && before > 0)
    {
        // Every part counts the same: the pitch-class profiles add up, the tuning is their circular mean.
        dsp::KeyResult all = result;
        float mx = 0.0f;
        for (size_t pc = 0; pc < 12; ++pc)
        {
            all.chroma[pc] = (float) before * heard[pc]->get() + result.chroma[pc];
            mx = juce::jmax (mx, all.chroma[pc]);
        }
        for (auto& c : all.chroma) c = mx > 0.0f ? c / mx : 0.0f;
        const double tau = juce::MathConstants<double>::twoPi / 100.0;
        const std::complex<double> z = (double) before * std::polar (1.0, tau * cents->get()) + std::polar (1.0, tau * result.cents);
        all.cents = (float) (std::arg (z) / tau);
        dsp::rankKeys (all);
        result = all;
        parts->set ((float) (before + 1));
    }
    else
        parts->set (1.0f);
    store (result);
    applied = mode == Mode::tune;
    if (applied) fine->set (std::round (-result.cents * 10.0f) / 10.0f);
    finish (State::done);
}
} // namespace hydra

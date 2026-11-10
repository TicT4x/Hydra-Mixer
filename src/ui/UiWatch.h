#pragma once
#include "Common.h"
#include <atomic>

namespace hydra
{
/** What the message thread is busy with right now (a string literal, or nullptr), read by UiWatch. */
inline std::atomic<const char*> uiActivity { nullptr };

/** Marks a stretch of message-thread work for UiWatch (nests: the outer label comes back afterwards). */
struct UiActivity
{
    explicit UiActivity (const char* what) noexcept : previous (uiActivity.exchange (what)) {}
    ~UiActivity() { uiActivity.store (previous); }
    UiActivity (const UiActivity&) = delete;
    UiActivity& operator= (const UiActivity&) = delete;
    const char* previous;
};

/** Finds moments in which the user interface hangs and writes them to hydra.log ("ui: blocked 240 ms in ...").
    Why: lags that only happen "sometimes" (opening the settings ...) cannot be reproduced from Claude's side
    (the app must not be started there); the log says where the time went.
    A 50 Hz timer on the message thread beats, a low-priority thread looks every 10 ms how long the last beat
    is ago and notes what the message thread was marked busy with (UiActivity). Costs one atomic store per beat. */
class UiWatch final : private juce::Timer, private juce::Thread
{
public:
    UiWatch() : juce::Thread ("Hydra UI watch")
    {
        lastBeat.store (now());
        startTimer (20);
        startThread (juce::Thread::Priority::low);
    }
    ~UiWatch() override
    {
        stopTimer();
        stopThread (500);
    }

private:
    static constexpr double kStallMs = 120.0;
    static double now() { return juce::Time::getMillisecondCounterHiRes(); }

    void timerCallback() override
    {
        lastBeat.store (now());
        const double ms = reportMs.exchange (0.0);
        if (ms <= 0.0) return;
        const char* what = reportWhat.load();
        // Long gaps are sleep / hibernation / a debugger, not a hang.
        if (ms > 60000.0) return;
        logLine ("ui: blocked " + juce::String (juce::roundToInt (ms)) + " ms in "
                 + juce::String (what != nullptr ? what : "unmarked work (painting, layout or JUCE)"));
    }

    void run() override
    {
        double stallFrom = 0.0;
        const char* seen = nullptr;
        while (! threadShouldExit())
        {
            wait (10);
            const double beat = lastBeat.load();
            if (now() - beat > kStallMs)
            {
                if (stallFrom == 0.0) { stallFrom = beat; seen = nullptr; }
                if (seen == nullptr) seen = uiActivity.load();   // the first label seen is where it got stuck
            }
            else if (stallFrom != 0.0)
            {
                // Reported by the message thread (it is free again); a second stall before that overwrites it.
                reportWhat.store (seen);
                reportMs.store (beat - stallFrom);
                stallFrom = 0.0;
            }
        }
    }

    std::atomic<double> lastBeat { 0.0 }, reportMs { 0.0 };
    std::atomic<const char*> reportWhat { nullptr };
};
} // namespace hydra

#pragma once
#include "IoStreams.h"

namespace hydra
{
/** Virtual Audio Cable streams that bypass Windows on Hydra's side: Hydra opens the cable's Kernel Streaming
    wave filter itself (WaveRT pin, the driver's cyclic buffer mapped into Hydra) instead of going through the
    Windows audio engine and WASAPI. Apps keep playing into / recording from the cable's normal endpoint.

    Clock: if the pin runs at the engine rate, the stream is "direct" (AsyncStream::isDirect): no resampler,
    and the drift controller steers the cable's own clock (VAC "clock correction", 1e-9 steps, applied at
    10 Hz from poll()), so the whole cable runs in step with the master. Only one Hydra stream per cable
    steers; a second one on the same cable resamples as usual. The cable's previous clock is restored when
    the stream closes.

    Returns nullptr (and the reason) if the endpoint is no VAC cable or the pin cannot be opened; the caller
    then falls back to WASAPI. Needs COM on the calling thread (StreamOpener). */
std::shared_ptr<AsyncInput>  openVacInput  (const IoTarget&, double engineRate, int engineBlock, juce::String& error);
std::shared_ptr<AsyncOutput> openVacOutput (const IoTarget&, double engineRate, int engineBlock, juce::String& error);

/** Turns direct Kernel Streaming for VAC cables on/off (Settings; default on). New streams only. */
void setVacDirectEnabled (bool) noexcept;
bool isVacDirectEnabled() noexcept;
/** Times a cable played past what Hydra had written (all output streams so far; diagnostics). */
int vacLateWriteCount() noexcept;
juce::String vacLateWriteInfo();
} // namespace hydra

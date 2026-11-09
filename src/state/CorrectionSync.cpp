#include "CorrectionSync.h"
#include "ui/Theme.h"

namespace hydra
{
juce::Colour BusCorrectionTarget::colour() const { return theme::busColour (index - kMaxInputs); }

CorrectionPoolSync::CorrectionPoolSync (Engine& e) : engine (e)
{
    for (int b = 0; b < kMaxBuses; ++b)
        if (engine.correctionFor (kMaxInputs + b) != nullptr)
            outputs.emplace_back (std::make_unique<BusCorrectionTarget> (engine, kMaxInputs + b), pool::Link (pool::kMixer));
    timerCallback();
    startTimer (500);
}

void CorrectionPoolSync::timerCallback()
{
    for (auto& [target, link] : outputs) link.sync (*target);
}
} // namespace hydra

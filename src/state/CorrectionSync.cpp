#include "CorrectionSync.h"
#include "ui/Theme.h"

namespace hydra
{
juce::Colour BusCorrectionTarget::colour() const { return index < kMaxInputs ? theme::accentIn : theme::busColour (index - kMaxInputs); }

CorrectionPoolSync::CorrectionPoolSync (Engine& e) : engine (e)
{
    for (int ch = 0; ch < kMaxInputs + kMaxBuses; ++ch)   // every channel can have corrections since 12.11.
        for (int k = 0; k < kFxInstances; ++k)
            outputs.emplace_back (std::make_unique<BusCorrectionTarget> (engine, ch, k), pool::Link (pool::kMixer));
    timerCallback();
    startTimer (500);
}

void CorrectionPoolSync::timerCallback()
{
    for (auto& [target, link] : outputs) link.sync (*target);
}
} // namespace hydra

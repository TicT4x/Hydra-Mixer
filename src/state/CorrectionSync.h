#pragma once
#include "CorrectionPool.h"
#include "engine/Engine.h"

namespace hydra
{
/** A hardware output of the mixer as a CorrectionTarget (selection index scheme, see busSelectIndex). */
class BusCorrectionTarget final : public CorrectionTarget
{
public:
    BusCorrectionTarget (Engine& e, int selectIndex) : engine (e), index (selectIndex) {}

    const CorrectionProfile& getProfile() const override { return corr().getProfile(); }
    uint32_t getVersion() const override                 { return corr().getVersion(); }
    void setProfile (CorrectionProfile p) override       { engine.setCorrection (index, std::move (p)); }
    bool isOn() const override                           { return corr().on->isOn(); }
    void setOn (bool on) override                        { corr().on->set (on ? 1.0f : 0.0f); }
    double getSampleRate() const override                { return engine.getSampleRate(); }
    juce::String label() const override                  { return busLabel (index - kMaxInputs); }
    juce::Colour colour() const override;
    bool hasLimiter() const override                     { return engine.buses[(size_t) (index - kMaxInputs)].limOn->isOn(); }

private:
    Correction& corr() const { return *engine.correctionFor (index); }
    Engine& engine;
    const int index;
};

/** Keeps the corrections of all hardware outputs linked to the shared pool (pool::Link per output, twice a second):
    every correction made in the mixer is saved there, and changes made in the Room Correction plugin come back. */
class CorrectionPoolSync final : private juce::Timer
{
public:
    explicit CorrectionPoolSync (Engine&);
    ~CorrectionPoolSync() override { stopTimer(); }

private:
    void timerCallback() override;

    Engine& engine;
    std::vector<std::pair<std::unique_ptr<BusCorrectionTarget>, pool::Link>> outputs;
};
} // namespace hydra

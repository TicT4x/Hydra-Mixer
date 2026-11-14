#pragma once
#include "Widgets.h"
#include "core/MatchEq.h"

namespace hydra::eqmatch
{
inline const juce::Colour kReferenceColour { 0xffe8963a }, kSourceColour { 0xff3fb4e0 };
/** Match in the EQ window (14.11., modelled on the Match EQs of mastering suites): capture a REFERENCE (this channel
    before the EQ, another channel after its effects, an audio file, standard speech, flat) and THIS CHANNEL (before
    the EQ), each with its own Capture button. The difference runs at 1/48-octave resolution as a filter of the EQ
    (ParamEq match*, dsp::HiResFir) and is shaped with SMOOTH / AMOUNT and the range handles in the curve. Only on
    request it is turned into normal EQ bands (BANDS is chosen then); the window then shows the bands again.

    The spectra live in ParamEq::matchData (saved with the session). This state is only what the window does right
    now (capturing, the "into bands" step), per EQ for the whole session. Message thread. */
struct State
{
    enum class Capture { idle, source, reference };

    State (int channelIndex, int eqInstance);
    State (const State&) = delete;
    const int channel, instance;

    int listenTo = -1;                  // channel the reference is captured from (may be this one); -1 = not chosen
    Capture capture = Capture::idle;
    int tapFrom = -1;                   // capturing from another channel's refTap (-1: this channel's pre-EQ tap)
    match::FineAnalyzer analyzer;
    SpscAudioRing ring;                 // never freed: the audio thread may still hold it for one block
    std::unique_ptr<match::FileJob> job;
    juce::String message;
    juce::uint32 messageTime = 0;
    std::vector<float> scratch;

    // "Into bands"
    bool converting = false;
    ParamRegistry reg;
    Param* bands {};                    // 1..16 (choice index + 1)
    std::vector<CorrectionProfile::Band> preview;
    std::vector<int> previewSides;      // ParamEq::Side of each preview band
    double previewRms = 0.0;
    int viewSide = 2;                   // the window's L | L+R | R switch: 0 first side, 1 second, 2 both
    int previewFree = 0;
    uint32_t previewVersion = 0;

    bool capturingPre() const noexcept { return capture != Capture::idle && tapFrom < 0; }
    void feedPre (const float* l, const float* r, int n);
    void say (const juce::String& t) { message = t; messageTime = juce::Time::getMillisecondCounter(); }
};

State& stateFor (int channel, int instance);

/** Writes bands into the free band slots of eq; returns how many were written. */
int writeBands (ParamEq&, const std::vector<CorrectionProfile::Band>&, const std::vector<int>& sides);

/** The strip below the curve in match mode: REFERENCE, THIS CHANNEL, FINE TUNE, RESULT (or INTO BANDS). */
class Panel final : public juce::Component, private juce::Timer
{
public:
    static constexpr int kHeight = 150;
    Panel (UiContext&, State&, ParamEq&);
    ~Panel() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    /** A capture reads this channel before the EQ: the window feeds its pre-EQ tap into State::feedPre. */
    bool wantsPre() const noexcept { return state.capturingPre(); }
    /** The match became bands: the window shows the bands again. */
    std::function<void()> onConverted;

private:
    struct Thumb;
    void timerCallback() override;
    void startCapture (State::Capture);
    void stopCapture (bool keep);
    void showSourceMenu();
    /** Built-in references have only the mono curve (the single channels follow it, the side stays unchanged). */
    void setReference (std::vector<float> curve, const juce::String& name, std::vector<float> l = {}, std::vector<float> r = {},
                       std::vector<float> side = {});
    /** Takes the analyzer's curves (mono, left, right, side) as source or reference; false if not enough was heard. */
    bool storeCapture (bool source, int from, double minSeconds);
    void updatePreview();
    void refresh();

    UiContext& ctx;
    State& state;
    ParamEq& eq;
    Chip source { "CHOOSE ...", theme::fxMatch },
         captureRef { "CAPTURE", theme::fxMatch }, clearRef { "CLEAR", theme::textDim },
         captureSrc { "CAPTURE", theme::fxMatch }, clearSrc { "CLEAR", theme::textDim },
         fullRange { "FULL RANGE", theme::textDim }, intoBands { "INTO BANDS ...", theme::fxMatch },
         convert { "CONVERT", theme::fxMatch }, cancel { "CANCEL", theme::textDim };
    Toggle onToggle, separate;
    uint32_t seenSplit = 0;
    Knob smooth, amount, bandCount;
    InfoButton info;
    std::unique_ptr<Thumb> refThumb, srcThumb;
    juce::Rectangle<int> cards[4];
    std::unique_ptr<juce::FileChooser> chooser;
    int tick = 0;
    uint64_t previewKey = ~0ull, lastSeen = ~0ull;
};
} // namespace hydra::eqmatch

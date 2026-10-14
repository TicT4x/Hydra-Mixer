#pragma once
#include "Widgets.h"

namespace juce::dsp { class FFT; }

namespace hydra
{
/** Horizontal list of plugin "cards" for a chain, with add / bypass / edit / reorder / remove. */
class PluginChainView final : public juce::Component
{
public:
    PluginChainView (UiContext& c, PluginChain& chain, juce::Colour accent);
    void refresh();
    void paint (juce::Graphics&) override;
    void resized() override;

    std::function<void()> onScanRequested;
    std::function<void()> onChainChanged;

private:
    void showAddMenu();
    void openEditor (int index);

    struct Card;
    UiContext& ctx;
    PluginChain& chain;
    juce::Colour accent;
    std::vector<std::unique_ptr<Card>> cards;
    Chip addButton { "+  Add VST3 Plugin", theme::textDim };
};

//==============================================================================
/** Interactive response curve of a 6-band parametric EQ (inputs and buses).
    Only bands that are switched on appear as points. Clicking an empty spot adds a band there,
    double-click or the right-click menu removes it. Every band has its own colour (theme::bandColour). */
class EqGraph final : public juce::Component, public ParamPoller::Client, public juce::SettableTooltipClient
{
public:
    /** large: dB scales at the sides, frequency labels below the plot (EQ window). */
    EqGraph (UiContext& c, ParamEq& eq, juce::Colour colour, bool large = false);
    ~EqGraph() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void pollParams() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override { dragging = false; }
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    int getSelectedBand() const noexcept { return selected; }
    void setSelectedBand (int b);
    std::function<void (int)> onBandSelected;

    /** Switches on the first unused band at freq / gain. Returns its index, or -1 if all six are in use. */
    int addBand (float freqHz, float gainDb);
    /** Switches a band off and resets it to its defaults (its point disappears). */
    void removeBand (int k);

    /** Live spectrum behind the curve: dBFS values, one per kSpectrumStep pixels from the plot's left edge. */
    static constexpr int kSpectrumStep = 2;
    static constexpr float kSpecTopDb = 0.0f, kSpecBottomDb = -96.0f;
    void setSpectrum (const std::vector<float>& dbColumns);
    juce::Rectangle<float> getPlotArea() const noexcept { return plot; }
    float freqToX (float f) const;
    float xToFreq (float x) const;

private:
    static constexpr float kViewDb = 20.0f;   // EQ curve: +-20 dB over the plot height
    float dbToY (float db) const;
    float yToDb (float y) const;
    float specToY (float db) const;
    juce::Point<float> nodePos (int k) const;
    int hitBand (juce::Point<float> p) const;
    void rebuildCurve();
    float curveDbAt (float freqHz) const; // summed response of the bands in use
    void showBandMenu (int k);

    UiContext& ctx;
    ParamEq& eq;
    juce::Colour colour;
    bool large;
    juce::Rectangle<float> plot;
    juce::Path curve, spectrum;
    std::array<juce::Path, ParamEq::kBands> bandCurves;
    uint64_t seen = 0;
    int selected = 0, hovered = -1, justAdded = -1;
    int ghostBand = -1;            // hover over an empty spot: preview of the band a click would add
    juce::Point<float> ghostPos;
    void setGhost (int band, juce::Point<float> pos);
    juce::Rectangle<float> ghostBounds() const;
    double justAddedTime = 0.0;
    bool dragging = false;
    float dragStartQ = 1.0f;
};

//==============================================================================
/** Large EQ editor (own window): big curve with live spectrum plus the controls of all six bands. */
class EqEditor final : public juce::Component
{
public:
    EqEditor (UiContext& c, int selectIndex);
    ~EqEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    int getSelectIndex() const noexcept { return selectIndex; }

private:
    struct BandColumn;
    void selectBand (int b);
    void analyse (double timestamp);   // spectrum analyzer, once per display frame (max ~60 Hz)

    static constexpr int kFftOrder = 12, kFftSize = 1 << kFftOrder;

    UiContext& ctx;
    int selectIndex;
    ParamEq& eq;
    juce::Colour colour;
    juce::String title;
    Toggle onToggle;
    EqGraph graph;
    std::vector<std::unique_ptr<BandColumn>> bands;

    SpectrumTap& tap;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> history, window, fftData, smoothed, columns, raw;
    juce::AudioBuffer<float> readBuf { 2, 2048 };
    int historyPos = 0;
    double lastFrame = 0.0;
    juce::VBlankAttachment vblank { this, [this] (double t) { analyse (t); } };
};

//==============================================================================
/** Bottom panel showing the details of the selected input, bus or the recorder. */
class Inspector final : public juce::Component
{
public:
    explicit Inspector (UiContext& c);
    ~Inspector() override;
    void showChannel (int selectIndex);
    /** Rebuilds the shown channel (after loading other settings). */
    void reload() { const int c = current; current = -1; showChannel (c); }
    void paint (juce::Graphics&) override;
    void resized() override;
    void updateMeters();

    std::function<void()> onScanRequested;

private:
    juce::String latencyText() const;

    UiContext& ctx;
    int current = -1;
    std::unique_ptr<juce::Component> content;
    juce::String shownLatency;
    int tick = 0;
};
} // namespace hydra

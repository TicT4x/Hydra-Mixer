#include "CorrectionView.h"
#include "Dialog.h"
#include "core/ParamEq.h"

namespace hydra
{
namespace
{
constexpr int kPad = 16;
} // namespace

//==============================================================================
CorrectionView::CorrectionView (UiContext& c) : ctx (c), list (library), wizard (c), analysis (library), edit (library)
{
    addAndMakeVisible (outputBox);
    outputBox.setTooltip ("The hardware output whose correction is shown, measured and edited.");
    outputBox.onChange = [this] { showOutput (outputBox.getSelectedId() - 1); };
    const Page pages[] = { Page::correction, Page::wizard, Page::analysis, Page::edit };
    const char* tips[] = { "The output's correction profile (curve, level, bands), the saved corrections and the saved measurements.",
                           "Step-by-step calibration: measures the speakers of this output in your room.",
                           "The selected measurement in detail: response, waterfall, decay, resonances.",
                           "Fine-tunes the correction designed from the selected measurement and applies it to this output." };
    for (int i = 0; i < 4; ++i)
    {
        auto& t = tabs[i];
        t.showArrow = false;
        t.centred = true;
        t.colour = theme::text;
        t.setTooltip (tips[i]);
        t.onClick = [this, p = pages[i]] { showPage (p); };
        addAndMakeVisible (t);
    }
    for (auto* p : std::initializer_list<juce::Component*> { &pools, &list, &wizard, &analysis, &edit }) addChildComponent (*p);
    wizard.onShowCorrection = [this] { showPage (Page::correction); };
    wizard.onOpenAnalysis = [this]
    {
        // Before "Apply" the wizard's measurement is not the loaded one yet; Analysis shows what the wizard shows.
        if (auto* m = library.findByFile (wizard.measurementFile()); m != nullptr && m != library.loaded())
        {
            library.setLoaded (m);
            previewLoaded = true;
        }
        showPage (Page::analysis);
    };
    wizard.onFineTune = [this]
    {
        syncLoaded();   // the wizard has just put its correction on the output
        showPage (Page::edit);
    };
    wizard.onMeasured = [this] (const RoomMeasurement& m) { library.add (std::make_unique<RoomMeasurement> (m)); };
    list.onLoad = [this] { loadSelected(); };
    list.onCalibrate = [this] { wizard.startCalibration(); showPage (Page::wizard); };
    analysis.onCalibrate = [this] { showPage (Page::wizard); };
    edit.onCalibrate = [this] { showPage (Page::wizard); };
    refreshOutputs();
    setSize (1320, 900);
    startTimerHz (4);
}

void CorrectionView::refreshOutputs()
{
    auto& e = ctx.engine;
    outputBox.clear (juce::dontSendNotification);
    shownOutputs = {};
    for (int b = 0; b < kMaxBuses; ++b)
        if (e.isBusActive (b) && Engine::isHwOutput (kMaxInputs + b))
        {
            const auto& bus = e.buses[(size_t) b];
            const auto code = busLabel (b);
            outputBox.addItem (code + (bus.name.isNotEmpty() && bus.name != code ? "  " + bus.name : juce::String()), kMaxInputs + b + 1);
            shownOutputs << code << bus.name << "|";
        }
    if (outputBox.indexOfItemId (output + 1) >= 0) outputBox.setSelectedId (output + 1, juce::dontSendNotification);
}

void CorrectionView::loadSelected()
{
    auto* m = library.current();
    auto* corr = ctx.engine.correctionFor (output);
    if (m == nullptr || corr == nullptr) return;
    // A profile that came from no measurement (imported from a file) is gone afterwards: ask first.
    const auto& old = corr->getProfile();
    if (old.isEmpty() || library.findByFile (old.source) != nullptr) return edit.load (m);
    juce::Component::SafePointer<CorrectionView> safe (this);
    dialog::confirm (this, dialog::Kind::question, "Replace the profile of " + channelCode (output) + "?",
                     "\"" + old.name + "\" (" + old.describe() + ") is replaced by the correction of \"" + m->name + "\".", "Load", [safe, m]
    {
        if (safe != nullptr && safe->library.indexOf (m) >= 0) safe->edit.load (m);
    });
}

void CorrectionView::syncLoaded()
{
    // The loaded measurement is the one the output's profile came from (also after the wizard or an import).
    auto* corr = ctx.engine.correctionFor (output);
    previewLoaded = false;
    shownVersion = corr != nullptr ? corr->getVersion() : 0;
    library.setLoaded (corr != nullptr ? library.findByFile (corr->getProfile().source) : nullptr);
    if (correction != nullptr) correction->setCanValidate (library.loaded() != nullptr && ! library.loaded()->throughEq);
}

void CorrectionView::timerCallback()
{
    if (auto* corr = ctx.engine.correctionFor (output); corr != nullptr && corr->getVersion() != shownVersion) syncLoaded();

    // Names and the outputs in use change rarely: rebuild the list only then.
    auto& e = ctx.engine;
    juce::String sig;
    for (int b = 0; b < kMaxBuses; ++b)
        if (e.isBusActive (b) && Engine::isHwOutput (kMaxInputs + b)) sig << busLabel (b) << e.buses[(size_t) b].name << "|";
    if (sig != shownOutputs && ! outputBox.isPopupActive())
    {
        refreshOutputs();
        if (outputBox.indexOfItemId (output + 1) < 0 && outputBox.getNumItems() > 0)   // its output was removed
            showOutput (outputBox.getItemId (0) - 1);
        repaint();
    }
}

bool CorrectionView::isBusy() const
{
    return ctx.engine.getMeasurement() != nullptr || (page == Page::wizard && wizard.holdsOutput());
}

void CorrectionView::showOutput (int selectIndex)
{
    if (! Engine::isHwOutput (selectIndex) || ! ctx.engine.isBusActive (selectIndex - kMaxInputs)) return;
    if (selectIndex != output || correction == nullptr)
    {
        output = selectIndex;
        const auto colour = theme::busColour (selectIndex - kMaxInputs);
        onToggle = std::make_unique<Toggle> (ctx, *ctx.engine.correctionFor (selectIndex)->on, "ON", theme::fxCorrection);
        onToggle->setTooltip ("Correction on or off (zero latency, first in the output's chain, before the EQ)");
        addAndMakeVisible (*onToggle);
        auto t = std::make_unique<BusCorrectionTarget> (ctx.engine, selectIndex);
        correction = std::make_unique<CorrectionPage> (*t);
        edit.setTarget (t.get());
        pools.setTarget (t.get());
        target = std::move (t);
        correction->onValidate = [this]
        {
            if (auto* m = library.loaded())
            {
                wizard.startValidation (*m);
                showPage (Page::wizard);
            }
        };
        addChildComponent (*correction);
    }
    outputBox.setSelectedId (output + 1, juce::dontSendNotification);
    wizard.setOutput (output);
    edit.setTarget (target.get());
    syncLoaded();
    showPage (page);
}

void CorrectionView::showPage (Page p)
{
    page = p;
    if (previewLoaded && p != Page::analysis && p != Page::wizard) syncLoaded();   // these work on what is really on the output
    for (int i = 0; i < 4; ++i)
    {
        tabs[i].active = (int) p == i;
        tabs[i].repaint();
    }
    if (correction != nullptr) correction->setVisible (p == Page::correction);
    list.setVisible (p == Page::correction);
    pools.setVisible (p == Page::correction);
    wizard.setVisible (p == Page::wizard);
    analysis.setVisible (p == Page::analysis);
    edit.setVisible (p == Page::edit);
    resized();
    repaint();
}

void CorrectionView::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);
    g.setColour (theme::panel);
    g.fillRect (barArea);
    g.setColour (theme::outline.withAlpha (0.6f));
    g.drawHorizontalLine (barArea.getBottom() - 1, (float) barArea.getX(), (float) barArea.getRight());
    g.setColour (theme::textDim);
    g.setFont (theme::font (12.0f));
    g.drawText ("Output", outputBox.getBounds().withX (barArea.getX() + kPad).withRight (outputBox.getX() - 8), juce::Justification::centredRight);
    if (output >= 0)
    {
        g.setColour (theme::busColour (output - kMaxInputs));
        g.fillRoundedRectangle (juce::Rectangle<float> (4.0f, 22.0f).withCentre (outputBox.getBounds().toFloat().getCentre().withX ((float) outputBox.getX() - 66.0f)), 2.0f);
    }
}

void CorrectionView::resized()
{
    auto r = getLocalBounds();
    barArea = r.removeFromTop (52);
    auto bar = barArea.reduced (kPad, 10);
    bar.removeFromLeft (70);
    outputBox.setBounds (bar.removeFromLeft (240));
    bar.removeFromLeft (12);
    if (onToggle != nullptr) onToggle->setBounds (bar.removeFromLeft (64).withSizeKeepingCentre (64, 28));
    for (int i = 3; i >= 0; --i)
    {
        tabs[i].setBounds (bar.removeFromRight (112));
        bar.removeFromRight (6);
    }

    wizard.setBounds (r);
    analysis.setBounds (r);
    edit.setBounds (r);
    // Correction: the saved corrections and the measurements on the left, the profile on the right.
    auto c = r;
    auto left = c.removeFromLeft (juce::jlimit (260, 340, c.getWidth() / 4)).reduced (0, kPad).withTrimmedLeft (kPad);
    pools.setBounds (left.removeFromTop (juce::roundToInt (left.getHeight() * 0.4)));
    left.removeFromTop (12);
    list.setBounds (left);
    if (correction != nullptr) correction->setBounds (c);
}
} // namespace hydra
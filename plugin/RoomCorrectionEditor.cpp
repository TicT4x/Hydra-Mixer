#include "RoomCorrectionEditor.h"

namespace hydra
{
namespace
{
constexpr int kPad = 16;
}

RoomCorrectionEditor::RoomCorrectionEditor (RoomCorrectionProcessor& p) : juce::AudioProcessorEditor (p), proc (p)
{
    addAndMakeVisible (onButton);
    onButton.setTooltip ("Correction on or off (zero latency).");
    onButton.setToggleState (proc.isOn(), juce::dontSendNotification);
    onButton.onClick = [this] { proc.setOn (onButton.getToggleState()); };

    const Page pages[] = { Page::correction, Page::analysis, Page::edit };
    const char* tips[] = { "The plugin's correction (curve, level, bands), the saved corrections and the measurements. "
                           "Both are shared with Hydra Mixer.",
                           "The loaded measurement in detail: response, waterfall, decay, resonances.",
                           "Fine-tunes the correction designed from the loaded measurement; every change is heard at once." };
    for (int i = 0; i < 3; ++i)
    {
        auto& t = tabs[i];
        t.showArrow = false;
        t.centred = true;
        t.colour = theme::text;
        t.setTooltip (tips[i]);
        t.onClick = [this, pg = pages[i]] { showPage (pg); };
        addAndMakeVisible (t);
    }

    correction.emptyHint = "No correction yet.\n\nChoose one of the saved corrections on the left (made here or in Hydra Mixer), "
                           "or load a measurement: its correction is designed and can be fine-tuned on the Edit page.";
    library.setLightweight (true);   // inside the DAW: the analysis leaves the cores to the audio
    pools.setTarget (&proc);
    edit.setTarget (&proc);
    list.onLoad = [this] { loadSelected(); };
    for (auto* c : std::initializer_list<juce::Component*> { &correction, &pools, &list, &analysis, &edit }) addChildComponent (*c);

    setResizable (true, true);
    setResizeLimits (900, 600, 3000, 2000);
    setSize (proc.editorSize.x, proc.editorSize.y);
    syncLoaded();
    showPage (Page::correction);
    startTimerHz (4);
}

RoomCorrectionEditor::~RoomCorrectionEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

juce::String RoomCorrectionEditor::linkText() const
{
    const auto& p = proc.getProfile();
    if (p.isEmpty()) return "No correction";
    if (p.poolId.isEmpty()) return p.name + "  -  not linked";
    return p.name + juce::String::fromUTF8 ("  \xc2\xb7  linked: changes in Hydra Mixer or other instances follow");
}

void RoomCorrectionEditor::timerCallback()
{
    if (onButton.getToggleState() != proc.isOn()) onButton.setToggleState (proc.isOn(), juce::dontSendNotification);
    if (proc.getVersion() != shownVersion) syncLoaded();
    if (auto t = linkText(); t != shownLink)
    {
        shownLink = t;
        repaint (linkArea);
    }
}

void RoomCorrectionEditor::syncLoaded()
{
    // The loaded measurement is the one the correction came from (also when it came from the pool).
    shownVersion = proc.getVersion();
    library.setLoaded (library.findByFile (proc.getProfile().source));
}

void RoomCorrectionEditor::loadSelected()
{
    auto* m = library.current();
    if (m == nullptr) return;
    // A correction that came from no measurement in this library is replaced: ask first (it stays in the pool if saved there).
    const auto& old = proc.getProfile();
    if (old.isEmpty() || library.findByFile (old.source) != nullptr) return edit.load (m);
    juce::Component::SafePointer<RoomCorrectionEditor> safe (this);
    dialog::confirm (this, dialog::Kind::question, "Replace the correction?",
                     "\"" + old.name + "\" (" + old.describe() + ") is replaced by the correction of \"" + m->name + "\".", "Load", [safe, m]
    {
        if (safe != nullptr && safe->library.indexOf (m) >= 0) safe->edit.load (m);
    });
}

void RoomCorrectionEditor::showPage (Page p)
{
    page = p;
    if (p == Page::analysis) library.allowFull();   // waterfall, decay and impulse only when they are shown
    for (int i = 0; i < 3; ++i)
    {
        tabs[i].active = (int) p == i;
        tabs[i].repaint();
    }
    correction.setVisible (p == Page::correction);
    pools.setVisible (p == Page::correction);
    list.setVisible (p == Page::correction);
    analysis.setVisible (p == Page::analysis);
    edit.setVisible (p == Page::edit);
    resized();
    repaint();
}

void RoomCorrectionEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);
    g.setColour (theme::panel);
    g.fillRect (barArea);
    g.setColour (theme::outline.withAlpha (0.6f));
    g.drawHorizontalLine (barArea.getBottom() - 1, (float) barArea.getX(), (float) barArea.getRight());

    auto r = barArea.reduced (kPad, 0);
    theme::drawLogo (g, r.removeFromLeft (26).withSizeKeepingCentre (26, 26).toFloat());
    r.removeFromLeft (10);
    g.setColour (theme::text);
    g.setFont (theme::font (15.0f, true));
    g.drawText ("Room Correction", r.removeFromLeft (150), juce::Justification::centredLeft);

    g.setColour (proc.getProfile().poolId.isNotEmpty() ? theme::textDim : theme::textFaint);
    g.setFont (theme::font (12.5f));
    g.drawText (shownLink, linkArea, juce::Justification::centredLeft, true);
}

void RoomCorrectionEditor::resized()
{
    proc.editorSize = { getWidth(), getHeight() };
    auto r = getLocalBounds();
    barArea = r.removeFromTop (52);
    auto bar = barArea.reduced (kPad, 10);
    bar.removeFromLeft (26 + 10 + 150);
    onButton.setBounds (bar.removeFromLeft (64).withSizeKeepingCentre (64, 28));
    bar.removeFromLeft (16);
    for (int i = 2; i >= 0; --i)
    {
        tabs[i].setBounds (bar.removeFromRight (112));
        bar.removeFromRight (6);
    }
    bar.removeFromRight (10);
    linkArea = bar;

    analysis.setBounds (r);
    edit.setBounds (r);
    auto c = r;
    auto left = c.removeFromLeft (juce::jlimit (260, 340, c.getWidth() / 4)).reduced (0, kPad).withTrimmedLeft (kPad);
    pools.setBounds (left.removeFromTop (juce::roundToInt (left.getHeight() * 0.5)));
    left.removeFromTop (12);
    list.setBounds (left);
    correction.setBounds (c);
}
} // namespace hydra

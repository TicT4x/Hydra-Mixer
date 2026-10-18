#include "SceneEditor.h"

namespace hydra
{
namespace
{
const juce::Colour kAccent = theme::solo;
constexpr int kRowH = 50;
} // namespace

//==============================================================================
/** One scene: number, name (double-click = rename), its MIDI / shortcut assignment, recall button. */
class SceneSidebar::Row final : public juce::Component
{
public:
    Row (SceneSidebar& o, int i) : index (i), owner (o)
    {
        auto* trigger = owner.bank.trigger (i);
        go = std::make_unique<Toggle> (owner.ctx, *trigger, "GO", kAccent, Toggle::Icon::none);
        go->setTooltip ("Recall this scene. Right-click: MIDI Learn / keyboard shortcut");
        addAndMakeVisible (*go);
        addAndMakeVisible (name);
        name.setFont (theme::font (13.0f, true));
        name.setColour (juce::Label::textColourId, theme::text);
        name.setColour (juce::Label::backgroundWhenEditingColourId, theme::bg);
        name.setColour (juce::Label::textWhenEditingColourId, theme::text);
        name.setColour (juce::Label::outlineWhenEditingColourId, kAccent);
        name.setEditable (false, false, false);
        name.setInterceptsMouseClicks (false, false);
        name.onTextChange = [this] { owner.bank.rename (index, name.getText()); };
        refresh();
    }

    void refresh()
    {
        name.setText (owner.bank.get (index).name, juce::dontSendNotification);
        info = owner.assignmentText (index);
        repaint();
    }

    void rename()
    {
        name.setEditable (false, true, false);
        name.showEditor();
        name.setEditable (false, false, false);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (2.0f, 2.0f);
        const bool cur = owner.bank.getCurrent() == index;
        g.setColour (cur ? kAccent.withAlpha (0.10f) : hover ? theme::raised.withAlpha (0.6f) : theme::panel2);
        g.fillRoundedRectangle (r, 7.0f);
        g.setColour (cur ? kAccent.withAlpha (0.8f) : theme::outline.withAlpha (0.6f));
        g.drawRoundedRectangle (r, 7.0f, cur ? 1.4f : 1.0f);

        auto badge = juce::Rectangle<float> (8.0f, r.getCentreY() - 11.0f, 24.0f, 22.0f);
        g.setColour (kAccent.withAlpha (cur ? 0.9f : 0.18f));
        g.fillRoundedRectangle (badge, 6.0f);
        g.setColour (cur ? theme::bg : kAccent);
        g.setFont (theme::font (12.0f, true));
        g.drawText (juce::String (index + 1), badge, juce::Justification::centred);

        auto line = juce::Rectangle<int> (40, getHeight() / 2, go->getX() - 46, getHeight() / 2 - 6);
        if (cur && owner.differences > 0)
        {
            g.setColour (theme::warn);
            g.setFont (theme::font (11.0f, true));
            g.drawText ("edited - not saved", line, juce::Justification::centredLeft);
        }
        else
        {
            g.setColour (theme::textFaint);
            g.setFont (theme::font (11.0f));
            g.drawFittedText (info, line, juce::Justification::centredLeft, 1, 0.8f);
        }
    }

    void resized() override
    {
        go->setBounds (getWidth() - 52, getHeight() / 2 - 12, 42, 24);
        name.setBounds (36, 5, go->getX() - 40, getHeight() / 2 - 4);
    }

    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { hover = false; repaint(); }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) { owner.showRowMenu (index, *this); return; }
        if (! e.mouseWasClicked()) return;
        if (e.getNumberOfClicks() >= 2) rename();
        else owner.select (index);
    }

    const int index;

private:
    SceneSidebar& owner;
    std::unique_ptr<Toggle> go;
    juce::Label name;
    juce::String info;
    bool hover = false;
};

//==============================================================================
SceneSidebar::SceneSidebar (UiContext& c, SceneBank& b) : ctx (c), bank (b)
{
    for (auto* chip : { &closeChip, &addChip, &importChip, &exportChip, &saveChip, &revertChip, &learnChip, &keyChip, &moreChip })
    {
        chip->showArrow = false;
        chip->centred = true;
        addAndMakeVisible (chip);
    }
    exportChip.showArrow = true;
    saveChip.icon = Chip::Icon::save;
    revertChip.icon = Chip::Icon::revert;
    closeChip.setTooltip ("Close the Scene Editor");
    addChip.setTooltip ("New scene from the current mixer settings");
    importChip.setTooltip ("Add scenes from a .hydrascenes file");
    exportChip.setTooltip ("Save scenes to a .hydrascenes file");
    saveChip.setTooltip ("Store the current mixer settings in the selected scene");
    revertChip.setTooltip ("Recall the selected scene again (drops the unsaved edits)");
    learnChip.setTooltip ("Next MIDI control you move / press recalls the selected scene");
    keyChip.setTooltip ("Assign a keyboard shortcut (or mouse button) that recalls the selected scene");
    moreChip.setTooltip ("Rename, duplicate, move, delete...");

    closeChip.onClick  = [this] { if (onClose) onClose(); };
    addChip.onClick    = [this] { addScene(); };
    importChip.onClick = [this] { importScenes(); };
    exportChip.onClick = [this]
    {
        juce::PopupMenu m;
        m.addItem ("Export all scenes...", bank.size() > 0, false, [this] { exportScenes (false); });
        m.addItem ("Export selected scene...", bank.getCurrent() >= 0, false, [this] { exportScenes (true); });
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&exportChip));
    };
    saveChip.onClick   = [this] { saveCurrent(); };
    revertChip.onClick = [this] { if (bank.getCurrent() >= 0) select (bank.getCurrent()); };
    learnChip.onClick  = [this]
    {
        if (auto* t = bank.trigger (bank.getCurrent()))
        {
            ctx.midi.armParam (t);
            if (onMessage) onMessage ("Move or press a MIDI control to recall \"" + bank.get (bank.getCurrent()).name + "\"");
        }
    };
    keyChip.onClick  = [this] { if (auto* t = bank.trigger (bank.getCurrent()); t != nullptr && ctx.armHotkey) ctx.armHotkey (*t); };
    moreChip.onClick = [this] { if (bank.getCurrent() >= 0) showRowMenu (bank.getCurrent(), moreChip); };

    prevBtn = std::make_unique<Toggle> (c, *c.engine.scenePrev, "< PREV", kAccent);
    nextBtn = std::make_unique<Toggle> (c, *c.engine.sceneNext, "NEXT >", kAccent);
    prevBtn->setTooltip ("Previous scene. Right-click: MIDI Learn / keyboard shortcut");
    nextBtn->setTooltip ("Next scene. Right-click: MIDI Learn / keyboard shortcut");
    addAndMakeVisible (*prevBtn);
    addAndMakeVisible (*nextBtn);

    viewport.setViewedComponent (&list, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (8);
    addAndMakeVisible (viewport);
    rebuild();
    startTimerHz (10);
}

SceneSidebar::~SceneSidebar() { stopTimer(); }

juce::String SceneSidebar::assignmentText (int i) const
{
    auto* t = bank.trigger (i);
    juce::StringArray parts;
    for (auto& m : ctx.midi.getMappingsFor (t)) parts.add ("MIDI " + m.describe());
    if (const auto key = ctx.hotkeys.getKeyFor (t); key.isValid()) parts.add (Hotkeys::describe (key));
    return parts.isEmpty() ? juce::String ("no MIDI / shortcut") : parts.joinIntoString ("  ·  ");
}

void SceneSidebar::rebuild()
{
    rows.clear();
    for (int i = 0; i < bank.size(); ++i)
    {
        rows.push_back (std::make_unique<Row> (*this, i));
        list.addAndMakeVisible (*rows.back());
    }
    seenBank = bank.version.load();
    seenMap = ctx.midi.mapVersion.load();
    seenKeys = ctx.hotkeys.version.load();
    resized();
    refreshState();
    repaint();
}

void SceneSidebar::refreshState()
{
    const int cur = bank.getCurrent();
    const int diff = cur >= 0 ? bank.countDifferences (cur) : 0;
    if (diff == differences && cur == shownCurrent) return;
    differences = diff;
    shownCurrent = cur;
    saveChip.active = cur >= 0 && diff > 0;
    saveChip.colour = saveChip.active ? theme::warn : theme::text;
    for (auto* chip : { &saveChip, &revertChip, &learnChip, &keyChip, &moreChip })
    {
        chip->setEnabled (cur >= 0);
        chip->setAlpha (cur >= 0 ? 1.0f : 0.4f);
        chip->repaint();
    }
    for (auto& r : rows) r->repaint();
    repaint (footerArea);
}

void SceneSidebar::timerCallback()
{
    if (bank.version.load() != seenBank && (int) rows.size() != bank.size())
        rebuild();
    else if (bank.version.load() != seenBank || ctx.midi.mapVersion.load() != seenMap || ctx.hotkeys.version.load() != seenKeys)
    {
        seenBank = bank.version.load();
        seenMap = ctx.midi.mapVersion.load();
        seenKeys = ctx.hotkeys.version.load();
        for (auto& r : rows) r->refresh();
        shownCurrent = -2;   // the current scene may have changed (recalled by MIDI)
    }
    refreshState();
}

void SceneSidebar::select (int i)
{
    bank.recall (i);   // live: never asks about unsaved edits
    shownCurrent = -2;
    refreshState();
}

void SceneSidebar::addScene()
{
    const int i = bank.addFromMixer ({});
    if (i < 0)
    {
        if (onMessage) onMessage ("All " + juce::String (kMaxScenes) + " scene slots are in use");
        return;
    }
    rebuild();
    viewport.setViewPosition (0, juce::jmax (0, list.getHeight() - viewport.getHeight()));
    renameScene (i);
}

void SceneSidebar::saveCurrent()
{
    const int cur = bank.getCurrent();
    if (cur < 0) return;
    bank.store (cur);
    shownCurrent = -2;
    refreshState();
    if (onMessage) onMessage ("Scene \"" + bank.get (cur).name + "\" saved");
}

void SceneSidebar::renameScene (int i)
{
    if (juce::isPositiveAndBelow (i, (int) rows.size())) rows[(size_t) i]->rename();
}

void SceneSidebar::applyTriggerMoves (const std::vector<std::pair<Param*, Param*>>& moves, Param* removed)
{
    // MIDI mappings and shortcuts stay with their scene when it changes its place.
    if (removed != nullptr)
    {
        ctx.midi.removeMappingsFor (removed);
        if (ctx.hotkeys.getKeyFor (removed).isValid()) ctx.hotkeys.remove (removed);
    }
    if (! moves.empty())
    {
        ctx.midi.remapParams (moves);
        ctx.hotkeys.remapParams (moves);
    }
}

void SceneSidebar::deleteScene (int i)
{
    if (! juce::isPositiveAndBelow (i, bank.size())) return;
    juce::Component::SafePointer<SceneSidebar> safe (this);
    dialog::confirm (this, dialog::Kind::danger, "Delete scene \"" + bank.get (i).name + "\"?",
                     "Its MIDI assignment and shortcut are removed too. The mixer itself does not change.",
                     "Delete", [safe, i]
                                  {
                                      if (safe == nullptr) return;
                                      Param* removed = nullptr;
                                      const auto moves = safe->bank.remove (i, removed);
                                      safe->applyTriggerMoves (moves, removed);
                                      safe->rebuild();
                                  });
}

void SceneSidebar::moveScene (int from, int to)
{
    const auto moves = bank.move (from, to);
    if (moves.empty()) return;
    applyTriggerMoves (moves, nullptr);
    rebuild();
}

void SceneSidebar::showRowMenu (int i, juce::Component& target)
{
    if (! juce::isPositiveAndBelow (i, bank.size())) return;
    auto* t = bank.trigger (i);
    juce::Component::SafePointer<SceneSidebar> safe (this);
    auto run = [safe] (std::function<void (SceneSidebar&)> fn) { return [safe, fn] { if (safe != nullptr) fn (*safe); }; };

    juce::PopupMenu m;
    m.addSectionHeader (juce::String (i + 1) + "   " + bank.get (i).name);
    m.addItem ("Recall", run ([i] (SceneSidebar& s) { s.select (i); }));
    m.addItem ("Save current mixer into this scene", run ([i] (SceneSidebar& s) { s.bank.store (i); s.shownCurrent = -2; s.refreshState(); }));
    m.addItem ("Rename", run ([i] (SceneSidebar& s) { s.renameScene (i); }));
    m.addItem ("Duplicate", bank.size() < kMaxScenes, false, run ([i] (SceneSidebar& s) { s.bank.duplicate (i); s.rebuild(); }));
    m.addItem ("Move up", i > 0, false, run ([i] (SceneSidebar& s) { s.moveScene (i, i - 1); }));
    m.addItem ("Move down", i + 1 < bank.size(), false, run ([i] (SceneSidebar& s) { s.moveScene (i, i + 1); }));
    m.addSeparator();
    m.addItem ("MIDI Learn", run ([t] (SceneSidebar& s) { s.ctx.midi.armParam (t); }));
    m.addItem ("Clear MIDI Mapping", ctx.midi.hasMapping (t), false, run ([t] (SceneSidebar& s) { s.ctx.midi.removeMappingsFor (t); }));
    m.addItem ("Assign Keyboard Shortcut...", run ([t] (SceneSidebar& s) { if (s.ctx.armHotkey) s.ctx.armHotkey (*t); }));
    m.addItem ("Remove Keyboard Shortcut", ctx.hotkeys.getKeyFor (t).isValid(), false, run ([t] (SceneSidebar& s) { s.ctx.hotkeys.remove (t); }));
    m.addSeparator();
    m.addItem ("Delete...", run ([i] (SceneSidebar& s) { s.deleteScene (i); }));
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target));
}

void SceneSidebar::exportScenes (bool onlySelected)
{
    std::vector<int> only;
    if (onlySelected)
    {
        if (bank.getCurrent() < 0) return;
        only.push_back (bank.getCurrent());
    }
    const auto name = onlySelected ? juce::File::createLegalFileName (bank.get (bank.getCurrent()).name) : juce::String ("Hydra Scenes");
    chooser = std::make_unique<juce::FileChooser> ("Export scenes",
                                                   juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                                                       .getChildFile (name + ".hydrascenes"),
                                                   "*.hydrascenes");
    juce::Component::SafePointer<SceneSidebar> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe, only] (const juce::FileChooser& fc)
                          {
                              if (safe == nullptr || fc.getResult() == juce::File()) return;
                              const auto f = fc.getResult().withFileExtension ("hydrascenes");
                              const bool ok = safe->bank.exportTo (f, only);
                              if (safe->onMessage) safe->onMessage (ok ? "Scenes exported to " + f.getFileName() : "Cannot write " + f.getFullPathName());
                          });
}

void SceneSidebar::importScenes()
{
    chooser = std::make_unique<juce::FileChooser> ("Import scenes", juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
                                                   "*.hydrascenes;*.xml");
    juce::Component::SafePointer<SceneSidebar> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe] (const juce::FileChooser& fc)
                          {
                              if (safe == nullptr || ! fc.getResult().existsAsFile()) return;
                              juce::String err;
                              const int n = safe->bank.importFrom (fc.getResult(), err);
                              safe->rebuild();
                              if (safe->onMessage)
                                  safe->onMessage (n > 0 ? juce::String (n) + (n == 1 ? " scene imported" : " scenes imported") : err);
                          });
}

void SceneSidebar::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (4.0f, 2.0f);
    g.setColour (theme::panel);
    g.fillRoundedRectangle (r, 10.0f);
    g.setColour (kAccent.withAlpha (0.35f));
    g.drawRoundedRectangle (r, 10.0f, 1.0f);

    g.setColour (kAccent);
    g.setFont (theme::font (15.0f, true));
    g.drawText ("Scenes", getLocalBounds().reduced (18, 0).removeFromTop (44), juce::Justification::centredLeft);

    if (bank.size() == 0)
    {
        g.setColour (theme::textFaint);
        g.setFont (theme::font (12.0f));
        g.drawFittedText ("No scenes yet.\n\nSet up the mixer, then press \"+ New\" to keep these settings as a scene. "
                          "Click a scene to recall it.",
                          listArea.reduced (14, 10), juce::Justification::topLeft, 6, 0.9f);
    }

    // Footer: the scene being edited
    auto f = footerArea;
    g.setColour (theme::outline.withAlpha (0.7f));
    g.fillRect (f.getX(), f.getY(), f.getWidth(), 1);
    f.removeFromTop (8);
    const int cur = bank.getCurrent();
    g.setFont (theme::font (12.5f, true));
    g.setColour (cur >= 0 ? theme::text : theme::textFaint);
    g.drawFittedText (cur >= 0 ? "Editing:  " + bank.get (cur).name : juce::String ("No scene selected"),
                      f.removeFromTop (18), juce::Justification::centredLeft, 1, 0.8f);
    g.setFont (theme::font (11.5f));
    g.setColour (cur >= 0 && differences > 0 ? theme::warn : theme::textDim);
    g.drawFittedText (cur < 0 ? juce::String ("Click a scene to recall and edit it.")
                      : differences > 0 ? juce::String (differences) + (differences == 1 ? " setting" : " settings")
                                              + " changed - Save keeps them in the scene"
                                        : juce::String ("Mixer matches the scene"),
                      f.removeFromTop (18), juce::Justification::centredLeft, 1, 0.8f);

    g.setColour (theme::textDim);
    g.setFont (theme::font (11.0f, true));
    g.drawText ("STEP", stepArea.withWidth (40), juce::Justification::centredLeft);
}

void SceneSidebar::resized()
{
    auto r = getLocalBounds().reduced (12, 6);
    auto head = r.removeFromTop (36);
    closeChip.setBounds (head.removeFromRight (28).withSizeKeepingCentre (28, 24));
    r.removeFromTop (2);
    auto buttons = r.removeFromTop (26);
    const int w = (buttons.getWidth() - 12) / 3;
    addChip.setBounds (buttons.removeFromLeft (w));
    buttons.removeFromLeft (6);
    importChip.setBounds (buttons.removeFromLeft (w));
    buttons.removeFromLeft (6);
    exportChip.setBounds (buttons);
    r.removeFromTop (8);

    // Bottom: step buttons, then the footer for the selected scene above them
    stepArea = r.removeFromBottom (26);
    {
        auto s = stepArea.withTrimmedLeft (44);
        prevBtn->setBounds (s.removeFromLeft ((s.getWidth() - 6) / 2));
        s.removeFromLeft (6);
        nextBtn->setBounds (s);
    }
    r.removeFromBottom (8);
    footerArea = r.removeFromBottom (110);
    {
        auto f = footerArea.withTrimmedTop (8 + 36 + 4);
        auto row1 = f.removeFromTop (26);
        saveChip.setBounds (row1.removeFromLeft ((row1.getWidth() - 6) / 2));
        row1.removeFromLeft (6);
        revertChip.setBounds (row1);
        f.removeFromTop (6);
        auto row2 = f.removeFromTop (26);
        const int bw = (row2.getWidth() - 12) / 3;
        learnChip.setBounds (row2.removeFromLeft (bw));
        row2.removeFromLeft (6);
        keyChip.setBounds (row2.removeFromLeft (bw));
        row2.removeFromLeft (6);
        moreChip.setBounds (row2);
    }
    r.removeFromBottom (6);

    listArea = r;
    viewport.setBounds (r);
    const int listW = r.getWidth() - (bank.size() * kRowH > r.getHeight() ? viewport.getScrollBarThickness() + 2 : 0);
    list.setSize (listW, juce::jmax (r.getHeight(), bank.size() * kRowH));
    for (auto& row : rows) row->setBounds (0, row->index * kRowH, listW, kRowH);
}
} // namespace hydra

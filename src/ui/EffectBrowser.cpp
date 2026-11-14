#include "EffectBrowser.h"
#include "Dialog.h"
#include "Inspector.h"
#include "state/Session.h"

namespace hydra
{
namespace
{
juce::Colour fxColour (Fx f)
{
    switch (f)
    {
        case Fx::eq:         return theme::fxEq;
        case Fx::gate:       return theme::fxGate;
        case Fx::comp:       return theme::fxComp;
        case Fx::limiter:    return theme::fxLimiter;
        case Fx::correction: return theme::fxCorrection;
        case Fx::utility:    return theme::fxUtility;
        case Fx::transpose:  return theme::fxTranspose;
        case Fx::stems:      return theme::fxStems;
        case Fx::headphone:  return theme::fxHeadphone;
        case Fx::voice:      return theme::fxVoice;
        case Fx::saturation: return theme::fxSaturation;
        case Fx::denoiser:   return theme::good;
        default:             return theme::fx;
    }
}

/** The kind of channel an effect is made for, if that matters (every effect goes on every channel). */
juce::String placesOf (Fx f)
{
    switch (f)
    {
        case Fx::stems:      return "for inputs";
        case Fx::correction: return "for H outputs";
        case Fx::headphone:  return "for headphone outputs";
        case Fx::voice:      return "one knob for a studio voice";
        default:             return {};
    }
}

bool instanceFree (const Engine& e, int ch, Fx f)
{
    const auto allowed = e.allowedFx (ch);
    if (std::find (allowed.begin(), allowed.end(), f) == allowed.end()) return false;
    const auto& chain = e.fxOf (ch).plugins;
    for (int k = 0; k < Engine::maxInstances (f); ++k)
        if (! chain.hasBuiltin (f, k)) return true;
    return false;
}

std::vector<int> stemsInputs (const Engine& e)
{
    std::vector<int> v;
    for (int i = 0; i < kNumChannels; ++i)
        if (isFxChannel (i) && e.isChannelActive (i) && e.fxOf (i).plugins.hasBuiltin (Fx::stems)) v.push_back (i);
    return v;
}

// Order in the browser: the everyday effects first.
const Fx kBrowserOrder[] = { Fx::voice, Fx::eq, Fx::comp, Fx::gate, Fx::limiter, Fx::saturation, Fx::utility, Fx::denoiser, Fx::transpose, Fx::stems, Fx::headphone, Fx::correction };
} // namespace

//==============================================================================
namespace fxdrag
{
juce::var builtin (Fx f)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("hydraFx", "builtin");
    o->setProperty ("fx", fxId (f));
    return juce::var (o);
}

juce::var plugin (const juce::PluginDescription& d)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("hydraFx", "plugin");
    if (auto x = d.createXml()) o->setProperty ("plugin", x->toString (juce::XmlElement::TextFormat().singleLine().withoutHeader()));
    o->setProperty ("name", d.name);
    return juce::var (o);
}

juce::var preset (const presets::Preset& p)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("hydraFx", "preset");
    o->setProperty ("file", p.file.getFullPathName());
    o->setProperty ("name", p.name);
    o->setProperty ("chain", p.chain);
    o->setProperty ("fx", p.chain || p.fx == Fx::plugin ? juce::String() : fxId (p.fx));
    return juce::var (o);
}

bool isFxDrag (const juce::var& d)
{
    return d.isObject() && d.getProperty ("hydraFx", {}).toString().isNotEmpty();
}

static bool loadDescription (const juce::var& d, juce::PluginDescription& out)
{
    auto xml = juce::XmlDocument::parse (d.getProperty ("plugin", {}).toString());
    return xml != nullptr && out.loadFromXml (*xml);
}

juce::String describe (const juce::var& d)
{
    const auto kind = d.getProperty ("hydraFx", {}).toString();
    if (kind == "builtin") return fxName (fxFromId (d.getProperty ("fx", {}).toString()));
    return d.getProperty ("name", {}).toString();
}

bool canAdd (const Engine& e, int ch, const juce::var& d)
{
    if (! isFxChannel (ch)) return false;
    const auto kind = d.getProperty ("hydraFx", {}).toString();
    if (kind == "builtin") return instanceFree (e, ch, fxFromId (d.getProperty ("fx", {}).toString()));
    if (kind == "plugin") return true;
    if (kind == "preset")
    {
        if ((bool) d.getProperty ("chain", false)) return true;
        const auto fx = d.getProperty ("fx", {}).toString();
        return fx.isEmpty() || instanceFree (e, ch, fxFromId (fx));
    }
    return false;
}

/** The built-in effect a drag adds (a built-in effect or its preset), Fx::plugin otherwise. */
static Fx builtinOf (const juce::var& d)
{
    const auto kind = d.getProperty ("hydraFx", {}).toString();
    const auto id = d.getProperty ("fx", {}).toString();
    if ((kind == "builtin" || kind == "preset") && id.isNotEmpty()) return fxFromId (id);
    return Fx::plugin;
}

static void addNow (UiContext& ctx, int ch, const juce::var& d, int index, juce::Component* near, std::function<void (int, int)> done);

void add (UiContext& ctx, int ch, const juce::var& d, int index, juce::Component* near, std::function<void (int, int)> done)
{
    // Room correction and the Stem Separator work on every channel but are made for one kind: say so first
    // (unless switched off with "don't ask again").
    const auto f = builtinOf (d);
    const bool hw = Engine::isHwOutput (ch);
    const bool untypical = (f == Fx::correction && ! hw) || (f == Fx::stems && ch >= kMaxInputs && ! isRecTrackChannel (ch));
    juce::StringArray off;
    if (ctx.session != nullptr) off.addTokens (ctx.session->prefs.fxWarnOff, ",", {});
    if (untypical && ! off.contains (fxId (f)))
    {
        const juce::String text = f == Fx::correction
            ? "Speaker Correction is made for hardware outputs (speakers): it corrects how the room and the speakers sound "
              "and is measured there. On this channel it can use a saved correction, but it cannot be measured here."
            : "The Stem Separator is made for inputs (music from an app or a device). On an output it separates the whole mix "
              "of that output, which costs a lot of GPU time and adds its latency to everything you hear there.";
        juce::Component::SafePointer<juce::Component> safeNear (near);
        dialog::show (near, dialog::Kind::info, fxName (f) + " on " + channelCode (ch) + "?", text,
                      { { "Add", dialog::Button::Role::primary }, { "Add, don't ask again", dialog::Button::Role::normal }, { "Cancel", dialog::Button::Role::cancel } },
                      [&ctx, ch, d, index, safeNear, done, f] (int r)
        {
            if (r < 0 || r == 2) return;
            if (r == 1 && ctx.session != nullptr)
            {
                juce::StringArray list;
                list.addTokens (ctx.session->prefs.fxWarnOff, ",", {});
                list.addIfNotAlreadyThere (fxId (f));
                ctx.session->prefs.fxWarnOff = list.joinIntoString (",");
                ctx.session->prefsVersion.fetch_add (1);
            }
            addNow (ctx, ch, d, index, safeNear.getComponent(), done);
        });
        return;
    }
    addNow (ctx, ch, d, index, near, done);
}

static void addNow (UiContext& ctx, int ch, const juce::var& d, int index, juce::Component* near, std::function<void (int, int)> done)
{
    auto& e = ctx.engine;
    auto& chain = e.chainFor (ch);
    const int at = index < 0 || index > chain.size() ? chain.size() : index;
    const auto kind = d.getProperty ("hydraFx", {}).toString();
    const juce::String where = channelCode (ch) + " " + e.channelName (ch);
    if (! canAdd (e, ch, d))
    {
        dialog::message (near, dialog::Kind::info, describe (d) + " cannot go here",
                         where.trim() + " cannot have " + (kind == "builtin" && ! instanceFree (e, ch, fxFromId (d.getProperty ("fx", {}).toString()))
                                                               ? "another " + describe (d) + "." : describe (d) + "."));
        return;
    }
    if (kind == "builtin")
    {
        const auto f = fxFromId (d.getProperty ("fx", {}).toString());
        juce::Component::SafePointer<juce::Component> safeNear (near);
        auto run = [&ctx, ch, f, at, done]
        {
            if (ctx.engine.addBuiltin (ch, f, at) >= 0 && done) done (at, 1);
        };
        if (f == Fx::stems)
            if (const auto others = stemsInputs (e); ! others.empty())
            {
                juce::StringArray names;
                for (int i : others) names.add (channelCode (i) + " (" + (e.channelName (i)) + ")");
                dialog::confirm (near, dialog::Kind::warning, "The Stem Separator is already in use",
                                 names.joinIntoString (", ") + (others.size() == 1 ? " already separates" : " already separate")
                                 + " with the Stem Separator. Every channel with it needs its own share of the GPU. If the GPU "
                                   "cannot keep up, parts play unseparated.\n\nThe Live mode needs much less.",
                                 "Add anyway", run);
                return;
            }
        run();
        return;
    }
    if (kind == "plugin")
    {
        juce::PluginDescription desc;
        if (! loadDescription (d, desc)) return;
        juce::String err;
        auto inst = e.pluginHost.create (desc, e.getSampleRate(), e.getBlockSize(), err);
        if (inst == nullptr)
        {
            dialog::message (near, dialog::Kind::warning, "Plugin could not be loaded", desc.name + "\n" + err);
            return;
        }
        chain.add (std::move (inst), desc, at);
        if (done) done (at, 1);
        return;
    }
    if (kind == "preset")
    {
        auto xml = clip::parse (juce::File (d.getProperty ("file", {}).toString()).loadFileAsString());
        if (xml == nullptr)
        {
            dialog::message (near, dialog::Kind::warning, "Preset not found", "The preset file is gone or cannot be read.");
            presets::invalidate();
            return;
        }
        const int n = clip::insertBlocks (e, ch, *xml, at);
        const int wanted = clip::numBlocks (xml.get());
        if (n < wanted)
            dialog::message (near, dialog::Kind::info, n == 0 ? "Nothing was added" : "Some effects were left out",
                             juce::String (wanted - n) + " of the " + juce::String (wanted) + " effects cannot go on " + where.trim()
                             + " (an effect it cannot have, the most instances reached or a plugin that does not load).");
        if (n > 0 && done) done (at, n);
    }
}

juce::Image dragImage (const juce::String& text, juce::Colour c)
{
    const auto f = theme::font (13.0f, true);
    const int w = juce::jmin (280, juce::GlyphArrangement::getStringWidthInt (f, text) + 34), h = 30;
    juce::Image img (juce::Image::ARGB, w, h, true);
    juce::Graphics g (img);
    auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) w, (float) h).reduced (1.0f);
    g.setColour (theme::raised.withAlpha (0.95f));
    g.fillRoundedRectangle (r, 7.0f);
    g.setColour (c);
    g.drawRoundedRectangle (r, 7.0f, 1.5f);
    g.fillEllipse (10.0f, h / 2.0f - 4.0f, 8.0f, 8.0f);
    g.setColour (theme::text);
    g.setFont (f);
    g.drawFittedText (text, juce::Rectangle<int> (24, 0, w - 30, h), juce::Justification::centredLeft, 1, 0.8f);
    return img;
}
} // namespace fxdrag

//==============================================================================
void FxDropZone::enter (const juce::var& d)
{
    lit = true;
    allowed = fxdrag::canAdd (ctx.engine, channel, d);
    owner.repaint();
    if (ctx.selected != channel) startTimer (kOpenMs);
}

void FxDropZone::exit()
{
    stopTimer();
    if (lit) { lit = false; owner.repaint(); }
}

void FxDropZone::dropped (const juce::var& d)
{
    exit();
    fxdrag::add (ctx, channel, d, -1, &owner);
}

void FxDropZone::timerCallback()
{
    // Held over the strip: its chain opens in the inspector, where the drag can go on to any place in it.
    stopTimer();
    if (lit && ctx.select) ctx.select (channel);
}

void FxDropZone::paintOver (juce::Graphics& g, juce::Rectangle<float> area) const
{
    if (! lit) return;
    const auto c = allowed ? theme::good : theme::textFaint;
    g.setColour (c.withAlpha (0.10f));
    g.fillRoundedRectangle (area.reduced (2.0f), 8.0f);
    g.setColour (c);
    g.drawRoundedRectangle (area.reduced (2.0f), 8.0f, 2.0f);
}

//==============================================================================
struct EffectBrowser::Entry
{
    enum class Type { header, info, builtin, vendor, plugin, preset };
    Type type = Type::info;
    juce::String key, text, sub;
    int depth = 0;
    bool expandable = false, expanded = false;
    juce::Colour colour = theme::fx;
    juce::var drag;   // void: not draggable
    presets::Preset preset;
};

class EffectBrowser::Row final : public juce::Component, public juce::SettableTooltipClient
{
public:
    Row (EffectBrowser& b, Entry en) : owner (b), entry (std::move (en))
    {
        if (! entry.drag.isVoid()) setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        if (entry.type == Entry::Type::builtin)
            setTooltip ("Drag onto a channel or into the effect chain. Double-click: add to the selected channel."
                        + juce::String (entry.expandable ? " Click: show its presets." : ""));
        else if (entry.type == Entry::Type::preset)
            setTooltip (entry.preset.describe() + ". Drag onto a channel or into the effect chain; double-click: add to the selected channel. Right-click: rename, delete, export.");
        else if (entry.type == Entry::Type::plugin)
            setTooltip ("VST3 plugin. Drag onto a channel or into the effect chain; double-click: add to the selected channel.");
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        if (entry.type == Entry::Type::header)
        {
            g.setColour (theme::textFaint);
            g.setFont (theme::font (10.5f, true));
            g.drawText (entry.text, r.reduced (10.0f, 0.0f).withTrimmedTop (6.0f), juce::Justification::centredLeft);
            return;
        }
        if (entry.type == Entry::Type::info)
        {
            g.setColour (theme::textFaint);
            g.setFont (theme::font (11.0f));
            g.drawFittedText (entry.text, r.reduced (12.0f, 2.0f).toNearestInt(), juce::Justification::centredLeft, 2, 0.9f);
            return;
        }
        if (hover)
        {
            g.setColour (theme::raised.withAlpha (0.8f));
            g.fillRoundedRectangle (r.reduced (4.0f, 1.0f), 5.0f);
        }
        auto x = 10.0f + (float) entry.depth * 14.0f;
        const float cy = r.getCentreY();
        if (entry.expandable)
        {
            juce::Path p;
            if (entry.expanded) p.addTriangle (x, cy - 2.5f, x + 8.0f, cy - 2.5f, x + 4.0f, cy + 3.0f);
            else                p.addTriangle (x + 1.5f, cy - 4.0f, x + 1.5f, cy + 4.0f, x + 7.0f, cy);
            g.setColour (theme::textDim);
            g.fillPath (p);
        }
        x += 13.0f;
        if (entry.type == Entry::Type::builtin || entry.type == Entry::Type::plugin)
        {
            g.setColour (entry.colour);
            g.fillRoundedRectangle (x, cy - 4.0f, 8.0f, 8.0f, 2.0f);
            x += 14.0f;
        }
        else if (entry.type == Entry::Type::preset)
        {
            g.setColour (entry.colour.withAlpha (0.8f));
            g.drawEllipse (x + 1.0f, cy - 3.0f, 6.0f, 6.0f, 1.2f);
            x += 14.0f;
        }
        const bool bold = entry.type == Entry::Type::builtin || entry.type == Entry::Type::vendor;
        auto f = theme::font (12.5f, bold);
        const int subW = entry.sub.isEmpty() ? 0 : juce::GlyphArrangement::getStringWidthInt (theme::font (10.5f), entry.sub) + 10;
        g.setColour (entry.type == Entry::Type::vendor ? theme::textDim : theme::text);
        g.setFont (f);
        g.drawFittedText (entry.text, juce::Rectangle<float> (x, 0.0f, r.getWidth() - x - (float) subW - 8.0f, r.getHeight()).toNearestInt(),
                          juce::Justification::centredLeft, 1, 0.85f);
        if (subW > 0)
        {
            g.setColour (theme::textFaint);
            g.setFont (theme::font (10.5f));
            g.drawText (entry.sub, r.withTrimmedRight (10.0f), juce::Justification::centredRight);
        }
    }

    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { hover = false; repaint(); }
    void mouseDown (const juce::MouseEvent& e) override
    {
        dragged = false;
        if (e.mods.isPopupMenu()) showMenu();
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragged || e.mods.isPopupMenu() || entry.drag.isVoid() || e.getDistanceFromDragStart() < 5) return;
        dragged = true;
        if (auto* c = juce::DragAndDropContainer::findParentDragContainerFor (this))
        {
            // The rendered block itself, at the size it has in the chain (window zoom and display scale included).
            const float zoom = juce::Component::getApproximateScaleFactorForComponent (this);
            float dpi = 1.0f;
            if (auto* disp = juce::Desktop::getInstance().getDisplays().getDisplayForRect (getScreenBounds())) dpi = (float) disp->scale;
            int width = 0;
            auto img = renderFxBlock (owner.ctx, entry.drag, zoom * dpi, width);
            if (auto* obj = entry.drag.getDynamicObject()) obj->setProperty ("width", width);
            const juce::Point<int> grab (-fxdrag::kGrabX, -fxdrag::kGrabY);
            if (img.isValid())
                c->startDragging (entry.drag, this, juce::ScaledImage (img, dpi), false, &grab, &e.source);
            else
                c->startDragging (entry.drag, this, juce::ScaledImage (fxdrag::dragImage (fxdrag::describe (entry.drag), entry.colour)), false, nullptr, &e.source);
        }
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (dragged || e.mods.isPopupMenu() || ! e.mouseWasClicked()) return;
        if (e.getNumberOfClicks() >= 2 && ! entry.drag.isVoid())
        {
            addToSelected();
            return;
        }
        if (entry.expandable) owner.toggle (entry.key);
    }

    void addToSelected()
    {
        const int ch = owner.ctx.selected;
        if (ch < 0 || ch >= kRecorderSelect)
        {
            if (owner.onMessage) owner.onMessage ("Select a channel first (click a strip)");
            return;
        }
        fxdrag::add (owner.ctx, ch, entry.drag, -1, this);
    }

    void showMenu()
    {
        juce::PopupMenu m;
        juce::Component::SafePointer<Row> safe (this);
        if (! entry.drag.isVoid())
        {
            const int ch = owner.ctx.selected;
            m.addItem ("Add to " + (ch >= 0 && ch < kRecorderSelect ? channelCode (ch) : juce::String ("the selected channel")),
                       ch >= 0 && ch < kRecorderSelect, false, [safe] { if (safe != nullptr) safe->addToSelected(); });
        }
        if (entry.type == Entry::Type::preset)
        {
            const auto p = entry.preset;
            juce::Component::SafePointer<EffectBrowser> b (&owner);
            m.addSeparator();
            m.addItem ("Rename...", [b, p]
            {
                if (b == nullptr) return;
                dialog::askText (b.getComponent(), "Rename preset", p.describe(), p.name, "Rename", [b, p] (juce::String name)
                {
                    juce::String err;
                    if (! presets::rename (p, name, err) && b != nullptr) dialog::message (b.getComponent(), dialog::Kind::warning, "Could not rename", err);
                });
            });
            m.addItem ("Export...", [b, p]
            {
                if (b == nullptr) return;
                b->chooser = std::make_unique<juce::FileChooser> ("Export preset",
                    juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile (p.file.getFileName()), juce::String ("*") + presets::kExtension);
                b->chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [b, p] (const juce::FileChooser& fc)
                {
                    const auto f = fc.getResult();
                    if (f == juce::File() || b == nullptr) return;
                    if (! presets::exportTo (p, f.withFileExtension (presets::kExtension)))
                        dialog::message (b.getComponent(), dialog::Kind::warning, "Export failed", "Could not write " + f.getFullPathName());
                    else if (b->onMessage) b->onMessage ("Preset exported");
                });
            });
            m.addItem ("Show in Explorer", [p] { p.file.revealToUser(); });
            m.addItem ("Delete", [b, p]
            {
                if (b == nullptr) return;
                dialog::confirm (b.getComponent(), dialog::Kind::danger, "Delete preset \"" + p.name + "\"?",
                                 "The file goes to the recycle bin.", "Delete", [p] { presets::remove (p); });
            });
        }
        if (m.getNumItems() > 0) m.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
    }

    EffectBrowser& owner;
    Entry entry;
    bool hover = false, dragged = false;
};

//==============================================================================
EffectBrowser::EffectBrowser (UiContext& c) : ctx (c)
{
    search.setTextToShowWhenEmpty ("Search", theme::textFaint);
    search.setFont (theme::font (13.0f));
    search.setColour (juce::TextEditor::backgroundColourId, theme::bg);
    search.setColour (juce::TextEditor::textColourId, theme::text);
    search.setColour (juce::TextEditor::outlineColourId, theme::outline);
    search.setColour (juce::TextEditor::focusedOutlineColourId, theme::fx);
    search.setIndents (8, 5);
    search.onTextChange = [this] { rebuild(); };
    search.onEscapeKey = [this] { search.clear(); rebuild(); };
    addAndMakeVisible (search);
    menuChip.showArrow = false;
    menuChip.centred = true;
    addAndMakeVisible (menuChip);
    menuChip.setTooltip ("Import presets, open the presets folder");
    menuChip.onClick = [this] { showMenu(); };
    view.setViewedComponent (&list, false);
    view.setScrollBarsShown (true, false);
    view.setScrollBarThickness (8);
    addAndMakeVisible (view);
    rebuild();
    startTimer (1000);
}

EffectBrowser::~EffectBrowser() { stopTimer(); }

void EffectBrowser::visibilityChanged()
{
    if (isVisible()) startTimer (1000);
    else stopTimer();
}

void EffectBrowser::timerCallback()
{
    // Presets saved / imported elsewhere (or copied into the folder), plugins scanned.
    if (presets::version() != presetsSeen || ctx.engine.pluginHost.known.getNumTypes() != pluginsSeen)
        rebuild();
}

void EffectBrowser::toggle (const juce::String& key)
{
    if (! open.erase (key)) open.insert (key);
    rebuild();
}

std::vector<EffectBrowser::Entry> EffectBrowser::makeEntries() const
{
    using T = Entry::Type;
    const auto filter = search.getText().trim().toLowerCase();
    auto matches = [&filter] (const juce::String& s) { return filter.isEmpty() || s.toLowerCase().contains (filter); };
    std::vector<Entry> out;
    auto header = [&out] (const juce::String& t) { Entry e; e.type = T::header; e.text = t; out.push_back (e); };
    auto info = [&out] (const juce::String& t) { Entry e; e.type = T::info; e.text = t; out.push_back (e); };
    auto presetRows = [&] (const std::vector<presets::Preset>& ps, int depth, juce::Colour c, bool all)
    {
        for (auto& p : ps)
        {
            if (! all && ! matches (p.name)) continue;
            Entry e;
            e.type = T::preset;
            e.text = p.name;
            e.depth = depth;
            e.colour = c;
            e.preset = p;
            e.drag = fxdrag::preset (p);
            if (p.chain) e.sub = juce::String (p.blocks);
            out.push_back (e);
        }
    };

    header ("BUILT-IN EFFECTS");
    for (auto f : kBrowserOrder)
    {
        const auto ps = presets::forFx (f);
        const bool selfMatch = matches (fxName (f));
        bool presetMatch = false;
        for (auto& p : ps) presetMatch |= matches (p.name);
        if (! selfMatch && ! presetMatch) continue;
        Entry e;
        e.type = T::builtin;
        e.key = "fx:" + fxId (f);
        e.text = fxName (f) + (f == Fx::stems ? " (beta)" : "");
        e.sub = placesOf (f);
        e.colour = fxColour (f);
        e.expandable = ! ps.empty();
        e.expanded = e.expandable && (open.count (e.key) > 0 || (filter.isNotEmpty() && presetMatch));
        e.drag = fxdrag::builtin (f);
        out.push_back (e);
        if (e.expanded) presetRows (ps, 1, e.colour, selfMatch);
    }

    header ("VST3 PLUGINS");
    std::map<juce::String, std::vector<juce::PluginDescription>> byVendor;
    for (auto& d : ctx.engine.pluginHost.known.getTypes())
        if (! d.isInstrument) byVendor[d.manufacturerName.isEmpty() ? juce::String ("Unknown") : d.manufacturerName].push_back (d);
    if (byVendor.empty()) info ("No plugins yet: Settings > Plugins scans for VST3 plugins.");
    for (auto& [vendor, list] : byVendor)
    {
        std::vector<Entry> plugins;
        const size_t before = out.size();
        const bool vendorMatch = matches (vendor);
        Entry v;
        v.type = T::vendor;
        v.key = "vendor:" + vendor;
        v.text = vendor;
        v.sub = juce::String ((int) list.size());
        v.expandable = true;
        out.push_back (v);
        bool any = false;
        const bool vendorOpen = open.count (v.key) > 0 || filter.isNotEmpty();
        for (auto& d : list)
        {
            const auto ps = presets::forPlugin (d);
            const bool selfMatch = vendorMatch || matches (d.name);
            bool presetMatch = false;
            for (auto& p : ps) presetMatch |= matches (p.name);
            if (! selfMatch && ! presetMatch) continue;
            any = true;
            if (! vendorOpen) continue;
            Entry e;
            e.type = T::plugin;
            e.key = "plugin:" + d.createIdentifierString();
            e.text = d.name;
            e.depth = 1;
            e.colour = theme::accentIn;
            e.expandable = ! ps.empty();
            e.expanded = e.expandable && (open.count (e.key) > 0 || (filter.isNotEmpty() && presetMatch));
            e.drag = fxdrag::plugin (d);
            out.push_back (e);
            if (e.expanded) presetRows (ps, 2, e.colour, selfMatch);
        }
        if (! any) out.resize (before);
        else out[before].expanded = vendorOpen;
    }

    header ("EFFECT CHAINS");
    const auto cs = presets::chains();
    bool anyChain = false;
    for (auto& p : cs) anyChain |= matches (p.name);
    if (cs.empty()) info ("Save one: right-click the empty space of an effect chain > Save chain as preset.");
    else if (anyChain) presetRows (cs, 0, theme::fx, false);
    return out;
}

void EffectBrowser::rebuild()
{
    presetsSeen = presets::version();
    pluginsSeen = ctx.engine.pluginHost.known.getNumTypes();
    const auto pos = view.getViewPosition();
    rows.clear();
    for (auto& e : makeEntries())
    {
        auto row = std::make_unique<Row> (*this, e);
        list.addAndMakeVisible (*row);
        rows.push_back (std::move (row));
    }
    resized();
    view.setViewPosition (pos);
}

void EffectBrowser::showMenu()
{
    juce::PopupMenu m;
    juce::Component::SafePointer<EffectBrowser> safe (this);
    m.addItem ("Import presets...", [safe] { if (safe != nullptr) safe->importPresets(); });
    m.addItem ("Open the presets folder", [] { auto d = presets::rootDir(); d.createDirectory(); d.startAsProcess(); });
    m.addSeparator();
    m.addItem ("Fold all in", ! open.empty(), false, [safe] { if (safe != nullptr) { safe->open.clear(); safe->rebuild(); } });
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&menuChip));
}

void EffectBrowser::importPresets()
{
    juce::Component::SafePointer<EffectBrowser> safe (this);
    chooser = std::make_unique<juce::FileChooser> ("Import presets", juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
                                                   juce::String ("*") + presets::kExtension);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectMultipleItems,
                          [safe] (const juce::FileChooser& fc)
    {
        if (safe == nullptr || fc.getResults().isEmpty()) return;
        juce::String err;
        const int n = presets::importFiles (fc.getResults(), err);
        if (err.isNotEmpty()) dialog::message (safe.getComponent(), dialog::Kind::warning, "Some files were not imported", err);
        if (n > 0 && safe->onMessage) safe->onMessage (juce::String (n) + (n == 1 ? " preset imported" : " presets imported"));
        safe->rebuild();
    });
}

void EffectBrowser::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (2.0f, 2.0f);
    g.setColour (theme::panel);
    g.fillRoundedRectangle (r, 10.0f);
    g.setColour (theme::text);
    g.setFont (theme::font (13.0f, true));
    g.drawText ("Effects", getLocalBounds().reduced (14, 8).removeFromTop (22), juce::Justification::centredLeft);
}

void EffectBrowser::resized()
{
    auto r = getLocalBounds().reduced (8, 6);
    auto top = r.removeFromTop (26);
    menuChip.setBounds (top.removeFromRight (34));
    r.removeFromTop (6);
    search.setBounds (r.removeFromTop (28));
    r.removeFromTop (6);
    view.setBounds (r);
    int y = 0;
    const int w = view.getWidth() - (view.isVerticalScrollBarShown() ? view.getScrollBarThickness() : 0);
    for (auto& row : rows)
    {
        const auto t = row->entry.type;
        const int h = t == Entry::Type::header ? 26 : t == Entry::Type::info ? 36 : 24;
        row->setBounds (0, y, w, h);
        y += h;
    }
    list.setSize (w, y + 6);
}

//==============================================================================
void BrowserTab::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (2.0f, 2.0f);
    g.setColour (hover ? theme::raised : theme::panel);
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (hover ? theme::text : theme::textDim);
    if (open)
    {
        g.setColour (colour.isTransparent() ? theme::fx : colour);
        // The marker sits on the side of the panel.
        g.fillRoundedRectangle (r.withWidth (3.0f).withX (right ? r.getX() : r.getRight() - 3.0f).reduced (0.0f, 8.0f), 1.5f);
        g.setColour (hover ? theme::text : theme::textDim);
    }
    juce::Path p;
    const float cx = r.getCentreX(), y = r.getY() + 14.0f;
    const bool pointLeft = open != right;   // left panel: open = fold in (left); right panel: closed = fold out (left)
    if (pointLeft) p.addTriangle (cx + 3.0f, y - 4.0f, cx + 3.0f, y + 4.0f, cx - 2.5f, y);
    else           p.addTriangle (cx - 2.5f, y - 4.0f, cx - 2.5f, y + 4.0f, cx + 3.0f, y);
    g.fillPath (p);
    juce::Graphics::ScopedSaveState s (g);
    g.addTransform (juce::AffineTransform::rotation (-juce::MathConstants<float>::halfPi, cx, r.getCentreY()));
    g.setFont (theme::font (11.0f, true));
    const auto rot = juce::Rectangle<float> (r.getHeight(), r.getWidth()).withCentre (r.getCentre());
    g.drawText (label, rot, juce::Justification::centred);
}
} // namespace hydra

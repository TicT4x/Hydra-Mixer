#include "CameraView.h"
#include "engine/PhoneCamera.h"
#include "state/Session.h"

namespace hydra
{
namespace
{
constexpr int kPanelWidth = 380, kRow = 30, kPreviewWidth = 960;

const char* groupName (int g)
{
    switch (g)
    {
        case cam::Control::camera:   return "Camera";
        case cam::Control::exposure: return "Exposure";
        case cam::Control::focus:    return "Focus";
        case cam::Control::colour:   return "Colour";
        case cam::Control::image:    return "Image";
        default:                     return "More";
    }
}

/** Where the upright picture lies inside the preview image (letterbox), as a fraction of the image. */
juce::Rectangle<float> pictureInImage (const cam::Status& s, int imgW, int imgH)
{
    if (s.frameWidth <= 0 || s.frameHeight <= 0 || imgW <= 0 || imgH <= 0) return { 0, 0, 1, 1 };
    const bool turned = (s.rotation & 1) != 0;
    const float dw = (float) (turned ? s.frameHeight : s.frameWidth), dh = (float) (turned ? s.frameWidth : s.frameHeight);
    const float k = juce::jmin ((float) imgW / dw, (float) imgH / dh);
    const float fw = dw * k / (float) imgW, fh = dh * k / (float) imgH;
    return { (1.0f - fw) * 0.5f, (1.0f - fh) * 0.5f, fw, fh };
}

//==============================================================================
/** The live picture; a click sets the focus / metering point (when the phone offers one). */
class Screen final : public juce::Component
{
public:
    std::function<void (float, float)> onClickPicture;   // upright picture, 0..1
    bool clickable = false;
    juce::String hint;

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (juce::Colours::black);
        g.fillRoundedRectangle (r, 8.0f);
        uint32_t v = 0;
        img = cam::PhoneCamera::get().getPreview (v);
        if (img.isValid())
        {
            juce::Graphics::ScopedSaveState keep (g);
            juce::Path clip;
            clip.addRoundedRectangle (r, 8.0f);
            g.reduceClipRegion (clip);
            drawn = juce::RectanglePlacement (juce::RectanglePlacement::centred).appliedTo (img.getBounds().toFloat(), r);
            g.drawImage (img, drawn, juce::RectanglePlacement::stretchToFit);
        }
        else
        {
            drawn = {};
            g.setColour (juce::Colours::white.withAlpha (0.5f));
            g.setFont (theme::font (14.0f));
            g.drawFittedText (hint.isNotEmpty() ? hint : juce::String ("No picture yet"), getLocalBounds().reduced (30), juce::Justification::centred, 4);
        }
        // Where the last click went (fades out).
        const double age = juce::Time::getMillisecondCounterHiRes() - markAt;
        if (age < 1500.0 && ! drawn.isEmpty())
        {
            const float a = (float) (1.0 - age / 1500.0);
            const auto p = picture().getRelativePoint (mark.x, mark.y);
            g.setColour (theme::warn.withAlpha (a));
            g.drawEllipse (juce::Rectangle<float> (44, 44).withCentre (p), 2.0f);
            g.drawEllipse (juce::Rectangle<float> (6, 6).withCentre (p), 2.0f);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! clickable || drawn.isEmpty() || e.mouseWasDraggedSinceMouseDown()) return;
        const auto pr = picture();
        if (! pr.contains (e.position)) return;
        mark = { (e.position.x - pr.getX()) / pr.getWidth(), (e.position.y - pr.getY()) / pr.getHeight() };
        markAt = juce::Time::getMillisecondCounterHiRes();
        if (onClickPicture) onClickPicture (mark.x, mark.y);
        repaint();
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        setMouseCursor (clickable && ! drawn.isEmpty() && picture().contains (e.position) ? juce::MouseCursor::CrosshairCursor : juce::MouseCursor::NormalCursor);
    }

    bool marking() const { return juce::Time::getMillisecondCounterHiRes() - markAt < 1600.0; }

private:
    /** The upright camera picture on screen (without the black bars of the fitting). */
    juce::Rectangle<float> picture() const
    {
        const auto s = cam::PhoneCamera::get().getStatus();
        const auto f = pictureInImage (s, img.getWidth(), img.getHeight());
        return { drawn.getX() + f.getX() * drawn.getWidth(), drawn.getY() + f.getY() * drawn.getHeight(),
                 f.getWidth() * drawn.getWidth(), f.getHeight() * drawn.getHeight() };
    }

    juce::Image img;
    juce::Rectangle<float> drawn;
    juce::Point<float> mark;
    double markAt = -1.0e9;
};

//==============================================================================
/** One control as the phone described it: label + toggle / slider / choice / button. */
class Row final : public juce::Component
{
public:
    explicit Row (const cam::Control& c) : ctl (c)
    {
        addAndMakeVisible (label);
        label.setText (c.label, juce::dontSendNotification);
        label.setFont (theme::font (13.0f));
        label.setColour (juce::Label::textColourId, theme::textDim);
        label.setMinimumHorizontalScale (0.8f);
        switch (c.kind)
        {
            case cam::Control::toggle:
                addAndMakeVisible (toggle);
                toggle.onClick = [this] { cam::PhoneCamera::get().setControl (ctl.id, toggle.getToggleState() ? 1.0f : 0.0f); };
                break;
            case cam::Control::range:
                addAndMakeVisible (slider);
                addAndMakeVisible (value);
                slider.setSliderStyle (juce::Slider::LinearHorizontal);
                slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
                slider.setColour (juce::Slider::trackColourId, theme::accentVirt);
                slider.setColour (juce::Slider::thumbColourId, theme::text);
                slider.setColour (juce::Slider::backgroundColourId, theme::raised);
                value.setFont (theme::mono (12.0f));
                value.setColour (juce::Label::textColourId, theme::text);
                value.setJustificationType (juce::Justification::centredRight);
                setRange (c);
                slider.onValueChange = [this]
                {
                    value.setText (ctl.format ((float) slider.getValue()), juce::dontSendNotification);
                    if (! updating) cam::PhoneCamera::get().setControl (ctl.id, (float) slider.getValue());
                };
                slider.onDragEnd = [this] { cam::PhoneCamera::get().setControl (ctl.id, (float) slider.getValue()); };
                break;
            case cam::Control::choice:
                addAndMakeVisible (combo);
                for (int i = 0; i < c.options.size(); ++i) combo.addItem (c.options[i], i + 1);
                combo.onChange = [this] { if (! updating && combo.getSelectedId() > 0) cam::PhoneCamera::get().setControl (ctl.id, (float) (combo.getSelectedId() - 1)); };
                break;
            case cam::Control::button:
                addAndMakeVisible (button);
                button.setButtonText (c.label);
                label.setVisible (false);
                button.onClick = [this] { cam::PhoneCamera::get().pressButton (ctl.id); };
                break;
            default:   // a point: set by clicking the picture
                label.setText (c.label, juce::dontSendNotification);
                label.setColour (juce::Label::textColourId, theme::textFaint);
                break;
        }
        update (c);
    }

    /** Same control, maybe new value / range / usability. */
    void update (const cam::Control& c)
    {
        const auto old = ctl;
        ctl = c;
        const juce::ScopedValueSetter<bool> busy (updating, true);
        const bool usable = c.isUsable();
        for (auto* comp : { (juce::Component*) &toggle, (juce::Component*) &slider, (juce::Component*) &combo, (juce::Component*) &button })
            comp->setEnabled (usable);
        label.setAlpha (usable ? 1.0f : 0.6f);
        value.setAlpha (usable ? 1.0f : 0.6f);
        switch (c.kind)
        {
            case cam::Control::toggle: toggle.setToggleState (c.value >= 0.5f, juce::dontSendNotification); break;
            case cam::Control::range:
                if (old.min != c.min || old.max != c.max || old.step != c.step) setRange (c);
                if (! slider.isMouseButtonDown()) slider.setValue (c.value, juce::dontSendNotification);
                value.setText (((c.flags & cam::Control::live) != 0 ? "auto " : "") + c.format ((float) slider.getValue()), juce::dontSendNotification);
                break;
            case cam::Control::choice:
                if (! combo.isPopupActive()) combo.setSelectedId (juce::jlimit (0, c.options.size() - 1, (int) std::lround (c.value)) + 1, juce::dontSendNotification);
                break;
            default: break;
        }
    }

    void resized() override
    {
        auto r = getLocalBounds();
        if (ctl.kind == cam::Control::button) { button.setBounds (r.removeFromLeft (juce::jmin (220, r.getWidth())).reduced (0, 2)); return; }
        if (ctl.kind == cam::Control::point) { label.setBounds (r); return; }
        label.setBounds (r.removeFromLeft (130));
        if (ctl.kind == cam::Control::range)
        {
            value.setBounds (r.removeFromRight (92));
            slider.setBounds (r);
        }
        else if (ctl.kind == cam::Control::toggle) toggle.setBounds (r.removeFromLeft (40));
        else combo.setBounds (r.reduced (0, 2));
    }

    cam::Control ctl;

private:
    void setRange (const cam::Control& c)
    {
        const double lo = c.min, hi = juce::jmax (c.max, c.min + 1.0e-6f);
        slider.setRange (lo, hi, c.step > 0.0f ? (double) c.step : 0.0);
        if ((c.flags & cam::Control::logarithmic) != 0 && lo > 0.0) slider.setSkewFactorFromMidPoint (std::sqrt (lo * hi));
        else slider.setSkewFactor (1.0);
        slider.setDoubleClickReturnValue (false, 0.0);
    }

    juce::Label label, value;
    juce::ToggleButton toggle;
    juce::Slider slider;
    juce::ComboBox combo;
    juce::TextButton button;
    bool updating = false;
};

//==============================================================================
/** The controls, in their groups (scrolls). */
class Panel final : public juce::Component
{
public:
    void set (const std::vector<cam::Control>& list)
    {
        juce::String key;
        for (auto& c : list) key << c.id << ":" << c.kind << ":" << c.group << ":" << c.label << ":" << c.options.joinIntoString ("|") << ";";
        if (key != structure)
        {
            structure = key;
            rows.clear();
            titles.clear();
            for (int g = 0; g <= 5; ++g)
            {
                bool any = false;
                for (auto& c : list)
                    if (juce::jmin (c.group, 5) == g)
                    {
                        if (! any)
                        {
                            any = true;
                            auto t = std::make_unique<juce::Label> (juce::String(), juce::String (groupName (g)).toUpperCase());
                            t->setFont (theme::font (12.0f, true));
                            t->setColour (juce::Label::textColourId, theme::textDim);
                            addAndMakeVisible (*t);
                            titles.push_back ({ (int) rows.size(), std::move (t) });
                        }
                        rows.push_back (std::make_unique<Row> (c));
                        addAndMakeVisible (*rows.back());
                    }
            }
            layout();
            return;
        }
        for (auto& row : rows)
            for (auto& c : list)
                if (c.id == row->ctl.id) { row->update (c); break; }
    }

    int layout()
    {
        int y = 4, t = 0;
        const int w = getWidth();
        for (size_t i = 0; i < rows.size(); ++i)
        {
            if (t < (int) titles.size() && titles[(size_t) t].first == (int) i)
            {
                if (i > 0) y += 14;
                titles[(size_t) t].second->setBounds (0, y, w, 20);
                y += 26;
                ++t;
            }
            rows[i]->setBounds (0, y, w, kRow);
            y += kRow + 4;
        }
        if (getHeight() != y + 8) setSize (w, y + 8);
        return y + 8;
    }

    void resized() override { layout(); }

    /** The point control (focus / metering by clicking the picture), -1 if none. */
    int pointControl() const
    {
        for (auto& r : rows) if (r->ctl.kind == cam::Control::point && r->ctl.isUsable()) return r->ctl.id;
        return -1;
    }

private:
    juce::String structure;
    std::vector<std::unique_ptr<Row>> rows;
    std::vector<std::pair<int, std::unique_ptr<juce::Label>>> titles;
};

//==============================================================================
class CameraView final : public juce::Component, private juce::Timer
{
public:
    explicit CameraView (UiContext& c) : ctx (c)
    {
        addAndMakeVisible (screen);
        addAndMakeVisible (status);
        status.setFont (theme::font (13.0f));
        status.setColour (juce::Label::textColourId, theme::textDim);
        status.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (view);
        view.setViewedComponent (&panel, false);
        view.setScrollBarsShown (true, false);
        addAndMakeVisible (note);
        note.setFont (theme::font (12.0f));
        note.setColour (juce::Label::textColourId, theme::textFaint);
        note.setJustificationType (juce::Justification::topLeft);
        note.setText ("While this window is open the phone films and sends, also if no app uses Hydra Camera. "
                      "The settings are kept on the phone, per camera.", juce::dontSendNotification);
        screen.onClickPicture = [this] (float x, float y)
        {
            if (const int id = panel.pointControl(); id >= 0) cam::PhoneCamera::get().setPoint (id, x, y);
        };
        setSize (1180, 680);
        cam::PhoneCamera::get().setPreview (true, 1, kPreviewWidth);
        startTimerHz (15);
        refresh();
    }

    ~CameraView() override { cam::PhoneCamera::get().setPreview (false, 1); }

    void paint (juce::Graphics& g) override { g.fillAll (theme::bg); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16);
        status.setBounds (r.removeFromTop (24));
        r.removeFromTop (8);
        auto right = r.removeFromRight (kPanelWidth);
        r.removeFromRight (16);
        screen.setBounds (r);
        note.setBounds (right.removeFromBottom (44));
        right.removeFromBottom (6);
        view.setBounds (right);
        panel.setSize (right.getWidth() - view.getScrollBarThickness() - 4, panel.getHeight());
        panel.layout();
    }

private:
    void timerCallback() override
    {
        screen.repaint();
        if (++ticks % 3 == 0) refresh();
    }

    void refresh()
    {
        const auto s = cam::PhoneCamera::get().getStatus();
        juce::String t, hint;
        if (! s.enabled || ! ctx.session->prefs.cameraEnabled) hint = "Hydra Camera is off: switch it on in Settings > Network > Phone camera.";
        else if (! ctx.session->prefs.netEnabled) hint = "Hydra Link is off (Settings > Network): the picture comes over it.";
        else if (s.phones.isEmpty()) hint = "No phone offers its camera: switch on the Camera tile in the Hydra Link app (Audio page).";
        else if (! s.receiving) hint = "Waiting for the picture from " + (s.phone.isNotEmpty() ? s.phone : s.phones[0]) + "...";
        if (s.phone.isNotEmpty())
        {
            t << s.phone;
            if (s.receiving)
                t << ":  " << s.frameWidth << " x " << s.frameHeight << ", " << juce::String (s.fps, 0) << " fps, " << juce::String (s.kbps / 1000.0, 1) << " Mbit/s";
            if (s.lostFrames > 0) t << ", " << (int) s.lostFrames << " frames lost";
        }
        else t = "Phone camera";
        if (s.error.isNotEmpty()) t << "   " << s.error;
        status.setText (t, juce::dontSendNotification);
        screen.hint = hint;

        uint32_t v = 0;
        const auto list = cam::PhoneCamera::get().getControls (v);
        if (v != version)
        {
            version = v;
            panel.set (list);
            panel.setSize (view.getMaximumVisibleWidth() > 0 ? view.getMaximumVisibleWidth() : kPanelWidth, panel.getHeight());
            panel.layout();
        }
        screen.clickable = panel.pointControl() >= 0;
        if (list.empty() && s.phone.isNotEmpty() && hint.isEmpty()) status.setText (t + "   (this phone's app does not offer camera settings yet: update it)", juce::dontSendNotification);
    }

    UiContext& ctx;
    Screen screen;
    juce::Label status, note;
    juce::Viewport view;
    Panel panel;
    uint32_t version = ~0u;
    int ticks = 0;
};
} // namespace

std::unique_ptr<juce::Component> makeCameraView (UiContext& ctx)
{
    return std::make_unique<CameraView> (ctx);
}
} // namespace hydra

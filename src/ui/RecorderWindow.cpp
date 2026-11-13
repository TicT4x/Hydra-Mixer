#include "RecorderWindow.h"
#include "Inspector.h"
#include "Dialog.h"

namespace hydra
{
namespace
{
juce::String clockText (double seconds)
{
    seconds = juce::jmax (0.0, seconds);
    const auto t = (int) seconds;
    const auto hms = t >= 3600 ? juce::String (t / 3600) + ":" + juce::String ((t / 60) % 60).paddedLeft ('0', 2)
                               : juce::String (t / 60).paddedLeft ('0', 2);
    return hms + ":" + juce::String (t % 60).paddedLeft ('0', 2) + "." + juce::String ((int) (seconds * 1000.0) % 1000).paddedLeft ('0', 3);
}

void prepareChip (Chip& c, bool arrow = false)
{
    c.showArrow = arrow;
    c.centred = true;
}

constexpr int kLaneHeight = 86, kRulerHeight = 30;
} // namespace

//==============================================================================
/** The timeline: ruler (play position, loop range) and one lane per track with its clips. */
class RecorderWindow::Arrange final : public juce::Component, public juce::SettableTooltipClient
{
public:
    Arrange (UiContext& c, RecorderWindow& w) : ctx (c), rec (c.engine.recorder), owner (w)
    {
        setWantsKeyboardFocus (true);
        setOpaque (true);
    }

    std::function<void (int)> onTrackClicked;
    std::function<void()> onScrolled;

    int scrollY = 0;
    double viewStart = 0.0, pps = 40.0;   // seconds at the left edge, pixels per second

    //--------------------------------------------------------------------------
    // What is shown
    enum class Content { none, project, take, file };
    Content content() const
    {
        if (rec.getTakeTrackCount() > 0) return Content::take;
        if (rec.hasProject()) return Content::project;
        if (rec.getLoadedFile() != juce::File() || (rec.isRecording() && ! rec.recordsInputs())) return Content::file;
        return Content::none;
    }
    int numLanes() const
    {
        switch (content())
        {
            case Content::take:    return rec.getTakeTrackCount();
            case Content::project: return (int) data().tracks.size();
            case Content::file:    return 1;
            default:               return 0;
        }
    }
    const RecProjectData& data() const { return dragging ? edit : rec.getProject(); }
    double totalLength() const
    {
        switch (content())
        {
            case Content::take:    return rec.getRecordSeconds();
            case Content::project: return data().length();
            case Content::file:    return rec.isRecording() ? rec.getRecordSeconds() : rec.getPlayLengthSeconds();
            default:               return 0.0;
        }
    }

    float timeToX (double t) const { return (float) ((t - viewStart) * pps); }
    double xToTime (float x) const { return viewStart + x / pps; }
    juce::Rectangle<int> laneBounds (int t) const { return { 0, kRulerHeight + t * kLaneHeight - scrollY, getWidth(), kLaneHeight }; }
    int laneAt (int y) const { return y < kRulerHeight ? -1 : (y - kRulerHeight + scrollY) / kLaneHeight; }
    int contentHeight() const { return numLanes() * kLaneHeight; }

    void zoomToFit()
    {
        const double len = juce::jmax (10.0, totalLength() * 1.06);
        pps = juce::jlimit (0.5, 4000.0, (double) juce::jmax (100, getWidth()) / len);
        viewStart = 0.0;
        repaint();
    }

    /** While recording: the view follows the growing take. */
    void follow (double t)
    {
        const double right = xToTime ((float) getWidth() * 0.92f);
        if (t > right) viewStart = juce::jmax (0.0, t - getWidth() * 0.6 / pps);
    }

    //--------------------------------------------------------------------------
    // Undo (clip edits only; mixer settings of the tracks use the global undo)
    void commit (const RecProjectData& d)
    {
        if (d == rec.getProject()) return;
        past.push_back (rec.getProject());
        if (past.size() > 200) past.erase (past.begin());
        future.clear();
        rec.setProject (d);
        repaint();
    }
    bool undo()
    {
        if (past.empty()) return false;
        future.push_back (rec.getProject());
        rec.setProject (past.back());
        past.pop_back();
        clearSelection();
        repaint();
        return true;
    }
    bool redo()
    {
        if (future.empty()) return false;
        past.push_back (rec.getProject());
        rec.setProject (future.back());
        future.pop_back();
        clearSelection();
        repaint();
        return true;
    }
    bool canUndo() const { return ! past.empty(); }
    bool canRedo() const { return ! future.empty(); }
    void forgetHistory() { past.clear(); future.clear(); clearSelection(); }

    //--------------------------------------------------------------------------
    // Selection: one clip, or a time range over some tracks.
    int selTrack = -1, selClip = -1;
    bool hasRange = false;
    double rangeA = 0.0, rangeB = 0.0;
    int rangeTrackA = 0, rangeTrackB = 0;
    void clearSelection() { selClip = -1; hasRange = false; }
    bool hasSelection() const { return (selClip >= 0 && selTrack >= 0) || (hasRange && rangeB > rangeA); }

    /** Ctrl+E: splits the selected clip at the play position, or every track of the range at its edges,
        or every track at the play position. */
    void splitAtCursor()
    {
        if (content() != Content::project) return;
        auto d = rec.getProject();
        bool any = false;
        if (hasRange && rangeB > rangeA)
            for (int t = juce::jmin (rangeTrackA, rangeTrackB); t <= juce::jmax (rangeTrackA, rangeTrackB); ++t)
            {
                any |= d.split (t, rangeA);
                any |= d.split (t, rangeB);
            }
        else
            any = d.split (selClip >= 0 ? selTrack : -1, cursor());
        if (any) { commit (d); clearSelection(); }
    }

    void deleteSelection()
    {
        if (content() != Content::project) return;
        auto d = rec.getProject();
        if (hasRange && rangeB > rangeA)
        {
            bool any = false;
            for (int t = juce::jmin (rangeTrackA, rangeTrackB); t <= juce::jmax (rangeTrackA, rangeTrackB); ++t)
                any |= d.erase (t, rangeA, rangeB);
            if (any) commit (d);
        }
        else if (selTrack >= 0 && juce::isPositiveAndBelow (selClip, (int) d.tracks[(size_t) selTrack].clips.size()))
        {
            auto& clips = d.tracks[(size_t) selTrack].clips;
            clips.erase (clips.begin() + selClip);
            commit (d);
        }
        clearSelection();
    }

    /** Keeps only the range / loop range on every track (moved to the start). */
    void cropToRange()
    {
        if (content() != Content::project) return;
        auto d = rec.getProject();
        const double a = hasRange && rangeB > rangeA ? rangeA : d.loopStart;
        const double b = hasRange && rangeB > rangeA ? rangeB : d.loopEnd;
        if (b <= a + 0.01) return;
        if (d.crop (a, b)) { commit (d); clearSelection(); rec.seek (0.0); zoomToFit(); }
    }

    void loopFromRange()
    {
        if (content() != Content::project || ! hasRange || rangeB <= rangeA) return;
        auto d = rec.getProject();
        d.loopStart = rangeA;
        d.loopEnd = rangeB;
        commit (d);
        rec.loop->set (1.0f);
    }

    double cursor() const { return rec.isPlaying() ? rec.getPlaySeconds() : rec.getStartSeconds(); }

    /** Not muted and not soloed away (the whole lane is greyed out otherwise). */
    bool trackAudible (int t) const
    {
        const int n = rec.getNumTracks();
        bool anySolo = false;
        for (int k = 0; k < n; ++k) anySolo |= ctx.engine.recTracks[(size_t) k].solo->isOn();
        const auto& rt = ctx.engine.recTracks[(size_t) juce::jlimit (0, kMaxRecTracks - 1, t)];
        return anySolo ? rt.solo->isOn() : ! rt.mute->isOn();
    }
    /** Changes with every mute / solo of the tracks (the window repaints the lanes then). */
    uint64_t audibleVersion() const
    {
        uint64_t v = 0;
        for (int k = 0; k < rec.getNumTracks(); ++k)
            v += ctx.engine.recTracks[(size_t) k].mute->getVersion() * 3u + ctx.engine.recTracks[(size_t) k].solo->getVersion() * 5u;
        return v;
    }

    //--------------------------------------------------------------------------
    // Clip clipboard (inside the recorder: clips refer to their track's file, so they paste onto the same track)
    struct ClipBoard { bool has = false; int track = -1; RecClip clip; juce::File project; };
    static ClipBoard& clipboard() { static ClipBoard b; return b; }

    bool selectedClip (RecClip& out) const
    {
        const auto& d = rec.getProject();
        if (selTrack < 0 || selTrack >= (int) d.tracks.size() || ! juce::isPositiveAndBelow (selClip, (int) d.tracks[(size_t) selTrack].clips.size())) return false;
        out = d.tracks[(size_t) selTrack].clips[(size_t) selClip];
        return true;
    }

    void copyClip (bool cut)
    {
        RecClip c;
        if (content() != Content::project || ! selectedClip (c)) return;
        clipboard() = { true, selTrack, c, rec.getProjectFile() };
        if (cut) deleteSelection();
    }

    /** Pastes the copied clip onto its track at the play position (it wins over what it covers). */
    void pasteClip()
    {
        auto& cb = clipboard();
        if (content() != Content::project || ! cb.has || cb.project != rec.getProjectFile()) return;
        auto d = rec.getProject();
        if (! juce::isPositiveAndBelow (cb.track, (int) d.tracks.size())) return;
        placeClip (d, cb.track, cb.clip, cursor());
    }

    /** Ctrl+D: a copy of the selected clip right after it. */
    void duplicateClip()
    {
        RecClip c;
        if (content() != Content::project || ! selectedClip (c)) return;
        auto d = rec.getProject();
        placeClip (d, selTrack, c, c.end());
    }

    void placeClip (RecProjectData& d, int track, RecClip c, double at, bool moveCursor = true)
    {
        c.start = juce::jmax (0.0, at);
        d.erase (track, c.start, c.end());
        d.tracks[(size_t) track].clips.push_back (c);
        d.tidy (track);
        commit (d);
        selTrack = track;
        selClip = -1;
        const auto& clips = rec.getProject().tracks[(size_t) track].clips;
        for (int k = 0; k < (int) clips.size(); ++k)
            if (std::abs (clips[(size_t) k].start - c.start) < 1.0e-9) selClip = k;
        hasRange = false;
        if (moveCursor && ! rec.isPlaying()) rec.seek (c.end());
        repaint();
    }

    /** Arrow keys: the selected clip a little to the left / right (Shift: 1 s, else 10 ms). */
    void nudge (double by)
    {
        RecClip c;
        if (content() != Content::project || ! selectedClip (c)) return;
        auto d = rec.getProject();
        auto& clips = d.tracks[(size_t) selTrack].clips;
        clips.erase (clips.begin() + selClip);
        placeClip (d, selTrack, c, juce::jmax (0.0, c.start + by), false);
    }

    /** Clip gain so its loudest peak reaches 0 dBFS (from the waveform overview: peaks of 5 ms). */
    void normalizeClip()
    {
        RecClip c;
        if (content() != Content::project || ! selectedClip (c)) return;
        const auto& w = rec.getTrackWaveform (selTrack);
        const int cols = juce::jmax (1, (int) (c.length * Waveform::kPeaksPerSecond));
        std::vector<float> v ((size_t) cols);
        w.getColumns (c.offset, c.offset + c.length, v.data(), cols);
        float peak = 0.0f;
        for (auto x : v) peak = juce::jmax (peak, x);
        if (peak <= 1.0e-5f) return;
        auto d = rec.getProject();
        d.tracks[(size_t) selTrack].clips[(size_t) selClip].gainDb = (float) juce::jlimit (-24.0, 36.0, -20.0 * std::log10 ((double) peak));
        commit (d);
    }

    void setClipFades (double in, double out)
    {
        RecClip c;
        if (content() != Content::project || ! selectedClip (c)) return;
        auto d = rec.getProject();
        auto& clip = d.tracks[(size_t) selTrack].clips[(size_t) selClip];
        clip.fadeIn = juce::jmin (in, clip.length * 0.5);
        clip.fadeOut = juce::jmin (out, clip.length * 0.5);
        commit (d);
    }

    /** Ctrl+A: everything on every track as a range. */
    void selectAll()
    {
        if (content() != Content::project) return;
        selClip = -1;
        hasRange = true;
        rangeA = 0.0;
        rangeB = data().length();
        rangeTrackA = 0;
        rangeTrackB = juce::jmax (0, numLanes() - 1);
        repaint();
    }

    /** + / -: zoom around the play position. */
    void zoomBy (double factor)
    {
        const double at = cursor();
        const float x = juce::jlimit (0.0f, (float) getWidth(), timeToX (at));
        pps = juce::jlimit (0.5, 4000.0, pps * factor);
        viewStart = juce::jmax (0.0, at - x / pps);
        repaint();
    }

    //--------------------------------------------------------------------------
    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::bg);
        const auto c = content();
        const int lanes = numLanes();
        // Lanes
        for (int t = 0; t < lanes; ++t)
        {
            const auto lane = laneBounds (t);
            if (lane.getBottom() < kRulerHeight || lane.getY() > getHeight()) continue;
            juce::Graphics::ScopedSaveState s (g);
            g.reduceClipRegion (0, kRulerHeight, getWidth(), getHeight() - kRulerHeight);
            const bool sel = c == Content::project && t == owner.selectedTrack;
            const bool heard = c != Content::project || trackAudible (t);
            auto laneColour = sel ? theme::panel2 : (t % 2 == 0 ? theme::panel : theme::panel.interpolatedWith (theme::bg, 0.5f));
            if (! heard) laneColour = laneColour.interpolatedWith (theme::bg, 0.6f);   // muted / soloed away: the whole track greyed out
            g.setColour (laneColour);
            g.fillRect (lane);
            g.setColour (theme::outline.withAlpha (0.5f));
            g.fillRect (lane.getX(), lane.getBottom() - 1, lane.getWidth(), 1);
            if (c == Content::project) paintProjectLane (g, t, lane);
            else if (c == Content::take) paintRecording (g, rec.getTrackWaveform (t), lane, rec.getRecordSeconds(), rec.getTakeTrackName (t));
            else if (c == Content::file)
            {
                if (rec.isRecording()) paintRecording (g, rec.getRecordWaveform(), lane, rec.getRecordSeconds(), "V bus");
                else paintFile (g, lane);
            }
        }
        if (lanes == 0) paintEmpty (g);

        // Range selection
        if (hasRange && rangeB > rangeA && c == Content::project)
        {
            const int ta = juce::jmin (rangeTrackA, rangeTrackB), tb = juce::jmax (rangeTrackA, rangeTrackB);
            const auto top = laneBounds (ta), bottom = laneBounds (tb);
            const float x0 = timeToX (rangeA), x1 = timeToX (rangeB);
            g.setColour (theme::accentIn.withAlpha (0.16f));
            g.fillRect (juce::Rectangle<float> (x0, (float) top.getY(), x1 - x0, (float) (bottom.getBottom() - top.getY())));
        }

        paintRuler (g);

        // Play position
        const double pos = rec.isRecording() ? rec.getRecordSeconds() : cursor();
        const float px = timeToX (pos);
        if (px >= 0 && px <= (float) getWidth())
        {
            g.setColour (rec.isRecording() ? theme::mute : rec.isPlaying() ? theme::text : theme::textDim);
            g.fillRect (px - 0.5f, 0.0f, 1.5f, (float) getHeight());
            juce::Path tri;
            tri.addTriangle (px - 5.0f, 0.0f, px + 5.0f, 0.0f, px, 7.0f);
            g.fillPath (tri);
        }
        shownCursorX = px;
    }

    void paintEmpty (juce::Graphics& g)
    {
        auto r = getLocalBounds().withTrimmedTop (kRulerHeight).reduced (30);
        g.setColour (theme::textDim);
        g.setFont (theme::font (14.0f, true));
        g.drawFittedText ("No recording open", r.removeFromTop (24), juce::Justification::centred, 1);
        g.setColour (theme::textFaint);
        g.setFont (theme::font (12.5f));
        g.drawFittedText ("Left: choose Inputs, arm the inputs to record (red button) and press REC. With \"Separate tracks\" every input "
                          "gets its own track and WAV file in a new folder; the take opens here for editing.\n"
                          "Or open an earlier recording with the folder menu at the top.",
                          r.removeFromTop (80), juce::Justification::centredTop, 5);
    }

    void paintRuler (juce::Graphics& g)
    {
        auto r = getLocalBounds().removeFromTop (kRulerHeight);
        g.setColour (theme::panel2);
        g.fillRect (r);
        g.setColour (theme::outline);
        g.fillRect (r.removeFromBottom (1));
        // Loop range
        if (content() == Content::project)
        {
            const auto& d = data();
            if (d.hasLoop())
            {
                const float x0 = timeToX (d.loopStart), x1 = timeToX (d.loopEnd);
                const auto col = rec.loop->isOn() ? theme::good : theme::textFaint;
                g.setColour (col.withAlpha (0.25f));
                g.fillRect (juce::Rectangle<float> (x0, 2.0f, x1 - x0, 9.0f));
                g.setColour (col);
                g.fillRect (juce::Rectangle<float> (x0, 2.0f, 2.0f, 9.0f));
                g.fillRect (juce::Rectangle<float> (x1 - 2.0f, 2.0f, 2.0f, 9.0f));
            }
        }
        // Time grid: a step that gives at least ~80 px
        static const double steps[] = { 0.05, 0.1, 0.25, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800 };
        double step = steps[std::size (steps) - 1];
        for (double s : steps) if (s * pps >= 80.0) { step = s; break; }
        const double first = std::floor (viewStart / step) * step;
        g.setFont (theme::font (10.5f));
        for (double t = first; timeToX (t) < (float) getWidth(); t += step)
        {
            const float x = timeToX (t);
            if (x < -60) continue;
            g.setColour (theme::outline);
            g.fillRect (x, (float) kRulerHeight - 9.0f, 1.0f, 8.0f);
            g.setColour (theme::outline.withAlpha (0.25f));
            g.fillRect (x, (float) kRulerHeight, 1.0f, (float) (getHeight() - kRulerHeight));
            g.setColour (theme::textDim);
            const auto label = step < 1.0 ? juce::String (t, step < 0.1 ? 2 : 1) + " s"
                                          : juce::String ((int) t / 60) + ":" + juce::String ((int) std::fmod (t, 60.0)).paddedLeft ('0', 2);
            g.drawText (label, juce::Rectangle<float> (x + 3.0f, 11.0f, 70.0f, 14.0f), juce::Justification::centredLeft, false);
        }
    }

    void paintWave (juce::Graphics& g, const Waveform& w, juce::Rectangle<float> area, double fileStart, double fileEnd,
                    std::function<float (double)> gainAt, juce::Colour col)
    {
        const int cols = juce::jmax (1, (int) area.getWidth());
        std::vector<float> v ((size_t) cols);
        w.getColumns (fileStart, fileEnd, v.data(), cols);
        const float mid = area.getCentreY(), half = area.getHeight() * 0.5f - 2.0f;
        g.setColour (col);
        const double per = (fileEnd - fileStart) / cols;
        for (int i = 0; i < cols; ++i)
        {
            float a = v[(size_t) i];
            if (gainAt != nullptr) a *= gainAt (fileStart + (i + 0.5) * per);
            const float h = juce::jlimit (0.5f, half, std::sqrt (juce::jmin (1.0f, a)) * half);   // sqrt: quiet parts stay visible
            g.fillRect (area.getX() + (float) i, mid - h, 1.0f, 2.0f * h);
        }
    }

    juce::Rectangle<float> clipRect (const RecClip& c, juce::Rectangle<int> lane) const
    {
        const float x0 = timeToX (c.start), x1 = timeToX (c.end());
        return { x0, (float) lane.getY() + 3.0f, juce::jmax (2.0f, x1 - x0), (float) lane.getHeight() - 7.0f };
    }

    void paintProjectLane (juce::Graphics& g, int t, juce::Rectangle<int> lane)
    {
        const auto& d = data();
        const auto& track = d.tracks[(size_t) t];
        const auto& wave = rec.getTrackWaveform (t);
        const bool muted = ! trackAudible (t);
        for (int k = 0; k < (int) track.clips.size(); ++k)
        {
            const auto& c = track.clips[(size_t) k];
            auto r = clipRect (c, lane);
            if (r.getRight() < 0 || r.getX() > (float) getWidth()) continue;
            const bool sel = t == selTrack && k == selClip;
            const auto base = muted ? theme::textFaint : theme::accentIn;
            g.setColour (base.withAlpha (sel ? 0.30f : 0.16f));
            g.fillRoundedRectangle (r, 4.0f);
            g.setColour (base.withAlpha (sel ? 1.0f : 0.55f));
            g.drawRoundedRectangle (r, 4.0f, sel ? 1.6f : 1.0f);
            // header strip with the name
            auto head = r.removeFromTop (15.0f);
            g.setColour (base.withAlpha (0.32f));
            g.fillRoundedRectangle (head.reduced (1.0f, 1.0f), 3.0f);
            g.setColour (theme::text.withAlpha (muted ? 0.5f : 0.9f));
            g.setFont (theme::font (10.5f, true));
            const auto gainTxt = c.gainDb != 0.0f ? "  " + juce::String (c.gainDb, 1) + " dB" : juce::String();
            g.drawText (track.name + gainTxt, head.reduced (5.0f, 0.0f), juce::Justification::centredLeft, true);
            auto gainAt = [c] (double fileT)
            {
                const double x = fileT - c.offset;
                double k = juce::Decibels::decibelsToGain ((double) c.gainDb);
                if (c.fadeIn > 0.0 && x < c.fadeIn) k *= juce::jmax (0.0, x / c.fadeIn);
                if (c.fadeOut > 0.0 && x > c.length - c.fadeOut) k *= juce::jmax (0.0, (c.length - x) / c.fadeOut);
                return (float) k;
            };
            // visible part only
            const float vx0 = juce::jmax (r.getX(), 0.0f), vx1 = juce::jmin (r.getRight(), (float) getWidth());
            if (vx1 > vx0 + 1.0f)
                paintWave (g, wave, { vx0, r.getY(), vx1 - vx0, r.getHeight() },
                           c.offset + (xToTime (vx0) - c.start), c.offset + (xToTime (vx1) - c.start), gainAt,
                           base.withAlpha (muted ? 0.45f : 0.85f));
            // fades
            auto full = clipRect (c, lane);
            g.setColour (theme::text.withAlpha (0.75f));
            if (c.fadeIn > 0.0)
                g.drawLine (full.getX(), full.getBottom(), timeToX (c.start + c.fadeIn), full.getY() + 15.0f, 1.2f);
            if (c.fadeOut > 0.0)
                g.drawLine (timeToX (c.end() - c.fadeOut), full.getY() + 15.0f, full.getRight(), full.getBottom(), 1.2f);
            // fade handles (small squares at the top corners)
            if (sel || hoverClip == std::make_pair (t, k))
            {
                g.setColour (theme::text);
                g.fillRect (juce::Rectangle<float> (timeToX (c.start + c.fadeIn) - 3.0f, full.getY() + 1.0f, 6.0f, 6.0f));
                g.fillRect (juce::Rectangle<float> (timeToX (c.end() - c.fadeOut) - 3.0f, full.getY() + 1.0f, 6.0f, 6.0f));
            }
        }
    }

    void paintRecording (juce::Graphics& g, const Waveform& w, juce::Rectangle<int> lane, double seconds, const juce::String& name)
    {
        RecClip c { 0.0, 0.0, seconds };
        auto r = clipRect (c, lane);
        g.setColour (theme::mute.withAlpha (0.14f));
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (theme::mute.withAlpha (0.7f));
        g.drawRoundedRectangle (r, 4.0f, 1.0f);
        g.setColour (theme::text);
        g.setFont (theme::font (10.5f, true));
        g.drawText (name, r.removeFromTop (15.0f).reduced (5.0f, 0.0f), juce::Justification::centredLeft, true);
        const float vx0 = juce::jmax (r.getX(), 0.0f), vx1 = juce::jmin (r.getRight(), (float) getWidth());
        if (vx1 > vx0 + 1.0f)
            paintWave (g, w, { vx0, r.getY(), vx1 - vx0, r.getHeight() }, xToTime (vx0), xToTime (vx1), nullptr, theme::mute.withAlpha (0.85f));
    }

    void paintFile (juce::Graphics& g, juce::Rectangle<int> lane)
    {
        const double len = rec.getPlayLengthSeconds();
        RecClip c { 0.0, 0.0, len };
        auto r = clipRect (c, lane);
        g.setColour (theme::accentVirt.withAlpha (0.14f));
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (theme::text);
        g.setFont (theme::font (10.5f, true));
        g.drawText (rec.getLoadedFile().getFileName() + "   (drag the edges to trim; right-click: add tracks, cut, save a copy)",
                    r.removeFromTop (15.0f).reduced (5.0f, 0.0f), juce::Justification::centredLeft, true);
        const float vx0 = juce::jmax (r.getX(), 0.0f), vx1 = juce::jmin (r.getRight(), (float) getWidth());
        if (vx1 > vx0 + 1.0f)
            paintWave (g, rec.getPlayWaveform(), { vx0, r.getY(), vx1 - vx0, r.getHeight() }, xToTime (vx0), xToTime (vx1), nullptr,
                       theme::accentVirt.withAlpha (0.85f));
        // trimmed parts
        g.setColour (theme::bg.withAlpha (0.6f));
        const float ti = timeToX (rec.getTrimIn()), to = timeToX (rec.getTrimOut());
        g.fillRect (juce::Rectangle<float> (r.getX(), (float) lane.getY(), ti - r.getX(), (float) lane.getHeight()));
        g.fillRect (juce::Rectangle<float> (to, (float) lane.getY(), r.getRight() - to, (float) lane.getHeight()));
        // trim handles (as in the compact view)
        for (float x : { ti, to })
        {
            g.setColour (theme::solo.withAlpha (drag == Drag::fileIn || drag == Drag::fileOut ? 1.0f : 0.75f));
            g.fillRect (x - 1.0f, (float) lane.getY() + 3.0f, 2.0f, (float) lane.getHeight() - 7.0f);
            g.fillRoundedRectangle (juce::Rectangle<float> (x == ti ? x : x - 8.0f, (float) lane.getCentreY() - 12.0f, 8.0f, 24.0f), 3.0f);
        }
    }

    /** Right-click on a single file: what the compact view offers, and opening it as a multitrack recording. */
    void showFileMenu()
    {
        juce::PopupMenu m;
        juce::Component::SafePointer<Arrange> safe (this);
        auto& r = rec;
        auto report = [safe] (bool ok, const juce::String& err)
        {
            if (! ok && safe != nullptr) dialog::message (safe.getComponent(), dialog::Kind::warning, "Recorder", err);
        };
        m.addItem ("Edit as multitrack recording (add tracks, cut into clips)", ! r.isRecording(), false, [&r, report]
        {
            juce::String err;
            report (r.convertFileToProject (err), err);
        });
        m.addItem ("Add an audio file as a track...", ! r.isRecording(), false, [safe] { if (safe != nullptr) safe->owner.importTrack(); });
        m.addSeparator();
        m.addItem ("Reset trim", r.isTrimmed(), false, [&r] { r.resetTrim(); });
        m.addItem ("Save the trimmed part as a copy", r.isTrimmed(), false, [&r, safe]
        {
            juce::String err;
            const auto f = r.saveTrimmedCopy (err);
            if (safe == nullptr) return;
            if (f == juce::File()) dialog::message (safe.getComponent(), dialog::Kind::warning, "Recorder", err);
            else dialog::message (safe.getComponent(), dialog::Kind::info, "Recorder", "Saved as " + f.getFileName());
        });
        m.addItem ("Show the file in Explorer", [&r] { r.getLoadedFile().revealToUser(); });
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
    }

    //--------------------------------------------------------------------------
    // Mouse
    enum class Drag { none, seek, loop, move, trimL, trimR, fadeIn, fadeOut, range, fileIn, fileOut };
    Drag drag = Drag::none;
    bool dragging = false;
    RecProjectData edit;
    RecClip dragClip;
    double dragT0 = 0.0, grabOffset = 0.0;
    int dragTrack = -1, dragIndex = -1;
    std::pair<int, int> hoverClip { -1, -1 };

    struct Hit { int track = -1, clip = -1; Drag zone = Drag::none; };
    Hit hitTest (juce::Point<int> p) const
    {
        Hit h;
        if (content() != Content::project) return h;
        h.track = laneAt (p.y);
        if (! juce::isPositiveAndBelow (h.track, (int) data().tracks.size())) { h.track = -1; return h; }
        const auto lane = laneBounds (h.track);
        const auto& clips = data().tracks[(size_t) h.track].clips;
        for (int k = 0; k < (int) clips.size(); ++k)
        {
            const auto r = clipRect (clips[(size_t) k], lane);
            if (! r.expanded (4.0f, 0.0f).contains (p.toFloat())) continue;
            h.clip = k;
            const auto& c = clips[(size_t) k];
            const float fi = timeToX (c.start + c.fadeIn), fo = timeToX (c.end() - c.fadeOut);
            if ((float) p.y < r.getY() + 9.0f && std::abs ((float) p.x - fi) < 6.0f) h.zone = Drag::fadeIn;
            else if ((float) p.y < r.getY() + 9.0f && std::abs ((float) p.x - fo) < 6.0f) h.zone = Drag::fadeOut;
            else if (std::abs ((float) p.x - r.getX()) < 6.0f) h.zone = Drag::trimL;
            else if (std::abs ((float) p.x - r.getRight()) < 6.0f) h.zone = Drag::trimR;
            else h.zone = Drag::move;
            return h;
        }
        return h;
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto h = hitTest (e.getPosition());
        const auto cursorFor = [] (Drag z)
        {
            switch (z)
            {
                case Drag::trimL: case Drag::trimR: return juce::MouseCursor::LeftRightResizeCursor;
                case Drag::fadeIn: case Drag::fadeOut: return juce::MouseCursor::CrosshairCursor;
                case Drag::move: return juce::MouseCursor::DraggingHandCursor;
                default: return juce::MouseCursor::NormalCursor;
            }
        };
        auto zone = h.zone;
        if (content() == Content::file && ! rec.isRecording()
            && (std::abs ((float) e.x - timeToX (rec.getTrimIn())) < 7.0f || std::abs ((float) e.x - timeToX (rec.getTrimOut())) < 7.0f))
            zone = Drag::trimL;
        setMouseCursor (e.y < kRulerHeight ? juce::MouseCursor::PointingHandCursor : cursorFor (zone));
        const std::pair<int, int> hc { h.track, h.clip };
        if (hc != hoverClip) { hoverClip = hc; repaint(); }
        setTooltip (e.y < kRulerHeight ? "Click: play position. Drag: loop range (LOOP plays it)."
                    : h.zone == Drag::move ? "Drag: move the clip. Edges: trim. Squares at the top: fades.\n"
                                             "Ctrl+C / X / V / D: copy, cut, paste at the play position, duplicate. Arrow keys: nudge (Shift: 1 s). "
                                             "Right-click: normalize, fades, gain..."
                    : content() == Content::project && h.track >= 0 ? "Drag: select a range (also over several tracks). Right-click: track menu."
                    : juce::String());
    }
    void mouseExit (const juce::MouseEvent&) override { if (hoverClip.first >= 0) { hoverClip = { -1, -1 }; repaint(); } }

    /** Edges to snap to (other clips, the play position, 0) within 8 px; Alt switches it off. */
    double snap (double t, bool off, int skipTrack = -1, int skipClip = -1) const
    {
        if (off) return t;
        double best = t, dist = 8.0 / pps;
        auto consider = [&] (double c) { if (std::abs (c - t) < dist) { dist = std::abs (c - t); best = c; } };
        consider (0.0);
        consider (cursor());
        const auto& d = data();
        for (int tr = 0; tr < (int) d.tracks.size(); ++tr)
            for (int k = 0; k < (int) d.tracks[(size_t) tr].clips.size(); ++k)
            {
                if (tr == skipTrack && k == skipClip) continue;
                consider (d.tracks[(size_t) tr].clips[(size_t) k].start);
                consider (d.tracks[(size_t) tr].clips[(size_t) k].end());
            }
        return best;
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
        drag = Drag::none;
        dragging = false;
        const double t = juce::jmax (0.0, xToTime ((float) e.x));
        if (e.y < kRulerHeight)
        {
            if (e.mods.isPopupMenu()) { showRulerMenu(); return; }
            drag = Drag::seek;
            dragT0 = t;
            rec.seek (t);
            repaint();
            return;
        }
        const auto h = hitTest (e.getPosition());
        const int lane = laneAt (e.y);
        if (content() == Content::file)
        {
            if (e.mods.isPopupMenu()) { showFileMenu(); return; }
            // The edges trim the file (like the handles in the compact view), elsewhere: play position.
            const float xi = timeToX (rec.getTrimIn()), xo = timeToX (rec.getTrimOut());
            if (! rec.isRecording() && std::abs ((float) e.x - xi) < 7.0f) { drag = Drag::fileIn; return; }
            if (! rec.isRecording() && std::abs ((float) e.x - xo) < 7.0f) { drag = Drag::fileOut; return; }
            rec.seek (t);
            repaint();
            return;
        }
        if (content() != Content::project) return;
        if (juce::isPositiveAndBelow (lane, numLanes()) && onTrackClicked) onTrackClicked (lane);
        if (e.mods.isPopupMenu())
        {
            if (h.clip >= 0) { selTrack = h.track; selClip = h.clip; hasRange = false; repaint(); showClipMenu (t); }
            else if (juce::isPositiveAndBelow (lane, numLanes())) owner.showTrackMenu (lane, t);
            return;
        }
        edit = rec.getProject();
        if (h.clip >= 0)
        {
            selTrack = h.track;
            selClip = h.clip;
            hasRange = false;
            drag = h.zone;
            dragTrack = h.track;
            dragIndex = h.clip;
            dragClip = edit.tracks[(size_t) h.track].clips[(size_t) h.clip];
            grabOffset = t - dragClip.start;
            repaint();
            return;
        }
        // Empty lane: a time range (also over several tracks); a click without dragging sets the play position.
        clearSelection();
        drag = Drag::range;
        dragT0 = snap (t, e.mods.isAltDown());
        rangeTrackA = rangeTrackB = juce::jlimit (0, juce::jmax (0, numLanes() - 1), lane);
        selTrack = rangeTrackA;
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        const double t = juce::jmax (0.0, xToTime ((float) e.x));
        const bool noSnap = e.mods.isAltDown();
        if (drag == Drag::fileIn)  { rec.setTrim (t, rec.getTrimOut()); repaint(); return; }
        if (drag == Drag::fileOut) { rec.setTrim (rec.getTrimIn(), t); repaint(); return; }
        if (drag == Drag::seek)
        {
            if (std::abs (e.getDistanceFromDragStartX()) < 4 || content() != Content::project) { rec.seek (t); repaint(); return; }
            drag = Drag::loop;
        }
        if (drag == Drag::loop)
        {
            edit = rec.getProject();
            const double a = snap (dragT0, noSnap), b = snap (t, noSnap);
            edit.loopStart = juce::jmin (a, b);
            edit.loopEnd = juce::jmax (a, b);
            dragging = true;
            repaint();
            return;
        }
        if (drag == Drag::range)
        {
            if (e.getDistanceFromDragStart() < 3) return;
            const double b = snap (t, noSnap);
            rangeA = juce::jmin (dragT0, b);
            rangeB = juce::jmax (dragT0, b);
            rangeTrackB = juce::jlimit (0, juce::jmax (0, numLanes() - 1), laneAt (e.y));
            hasRange = rangeB > rangeA;
            repaint();
            return;
        }
        if (dragTrack < 0 || e.getDistanceFromDragStart() < 2) return;
        dragging = true;
        auto c = dragClip;
        const auto& info = edit.tracks[(size_t) dragTrack];
        switch (drag)
        {
            case Drag::move:
            {
                double start = juce::jmax (0.0, t - grabOffset);
                const double s1 = snap (start, noSnap, dragTrack, dragIndex), s2 = snap (start + c.length, noSnap, dragTrack, dragIndex) - c.length;
                start = std::abs (s1 - start) <= std::abs (s2 - start) ? s1 : s2;
                c.start = juce::jmax (0.0, start);
                break;
            }
            case Drag::trimL:
            {
                const double end = c.end();
                double s = snap (t, noSnap, dragTrack, dragIndex);
                s = juce::jlimit (juce::jmax (0.0, c.start - c.offset), end - RecProjectData::kMinClip, s);
                c.offset += s - c.start;
                c.length = end - s;
                c.start = s;
                break;
            }
            case Drag::trimR:
            {
                double s = snap (t, noSnap, dragTrack, dragIndex);
                const double maxEnd = c.start + (juce::jmax (info.fileSeconds, c.offset + c.length) - c.offset);
                s = juce::jlimit (c.start + RecProjectData::kMinClip, maxEnd, s);
                c.length = s - c.start;
                break;
            }
            case Drag::fadeIn:  c.fadeIn = juce::jlimit (0.0, c.length - c.fadeOut, t - c.start); break;
            case Drag::fadeOut: c.fadeOut = juce::jlimit (0.0, c.length - c.fadeIn, c.end() - t); break;
            default: break;
        }
        c.fadeIn = juce::jmin (c.fadeIn, c.length);
        c.fadeOut = juce::jmin (c.fadeOut, c.length - c.fadeIn);
        edit = rec.getProject();
        edit.tracks[(size_t) dragTrack].clips[(size_t) dragIndex] = c;
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        const auto d = drag;
        drag = Drag::none;
        if (d == Drag::range && ! hasRange)
        {
            rec.seek (juce::jmax (0.0, xToTime ((float) e.x)));   // a click into the empty lane: play position
            repaint();
            return;
        }
        if (! dragging) { repaint(); return; }
        dragging = false;
        if (d == Drag::loop) { commit (edit); return; }
        if (dragTrack >= 0 && dragIndex >= 0)
        {
            // The changed clip wins over what it now covers on its track.
            auto result = rec.getProject();
            const auto c = edit.tracks[(size_t) dragTrack].clips[(size_t) dragIndex];
            auto& clips = result.tracks[(size_t) dragTrack].clips;
            clips.erase (clips.begin() + dragIndex);
            result.erase (dragTrack, c.start, c.end());
            clips.push_back (c);
            result.tidy (dragTrack);
            selClip = -1;
            for (int k = 0; k < (int) clips.size(); ++k)
                if (std::abs (clips[(size_t) k].start - c.start) < 1.0e-9) selClip = k;
            commit (result);
        }
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (e.y < kRulerHeight) { rec.seek (0.0); repaint(); }
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        // Ctrl + wheel: zoom around the mouse. Wheel: tracks up / down (sideways if they all fit). Shift: sideways.
        if (e.mods.isCommandDown())
        {
            const double at = xToTime ((float) e.x);
            pps = juce::jlimit (0.5, 4000.0, pps * std::pow (1.25, (w.deltaY != 0 ? w.deltaY : w.deltaX) * 5.0));
            viewStart = juce::jmax (0.0, at - e.x / pps);
            repaint();
            return;
        }
        const int room = juce::jmax (0, contentHeight() - (getHeight() - kRulerHeight));
        const bool sideways = e.mods.isShiftDown() || room == 0 || w.deltaX != 0.0f;
        if (sideways)
        {
            const float d = w.deltaX != 0.0f ? w.deltaX : w.deltaY;
            viewStart = juce::jmax (0.0, viewStart - d * 300.0 / pps);
        }
        else
        {
            scrollY = juce::jlimit (0, room, scrollY - juce::roundToInt (w.deltaY * 240.0f));
            if (onScrolled) onScrolled();
        }
        repaint();
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        const auto m = k.getModifiers();
        const int code = k.getKeyCode();
        if (m.isCommandDown() && (code == 'Z' || code == 'z') && ! m.isShiftDown()) return undo() || ctx.handleUndoKey (k);
        if (m.isCommandDown() && ((code == 'Y' || code == 'y') || ((code == 'Z' || code == 'z') && m.isShiftDown()))) return redo() || ctx.handleUndoKey (k);
        if (m.isCommandDown() && (code == 'E' || code == 'e')) { splitAtCursor(); return true; }
        if (m.isCommandDown() && (code == 'L' || code == 'l')) { loopFromRange(); return true; }
        if (m.isCommandDown() && (code == 'C' || code == 'c')) { copyClip (false); return true; }
        if (m.isCommandDown() && (code == 'X' || code == 'x')) { copyClip (true); return true; }
        if (m.isCommandDown() && (code == 'V' || code == 'v')) { pasteClip(); return true; }
        if (m.isCommandDown() && (code == 'D' || code == 'd')) { duplicateClip(); return true; }
        if (m.isCommandDown() && (code == 'A' || code == 'a')) { selectAll(); return true; }
        if (k == juce::KeyPress::leftKey)  { nudge (m.isShiftDown() ? -1.0 : -0.01); return true; }
        if (k == juce::KeyPress::rightKey) { nudge (m.isShiftDown() ? 1.0 : 0.01); return true; }
        if (! m.isCommandDown() && (code == '+' || code == '=' || code == juce::KeyPress::numberPadAdd))      { zoomBy (1.5); return true; }
        if (! m.isCommandDown() && (code == '-' || code == juce::KeyPress::numberPadSubtract)) { zoomBy (1.0 / 1.5); return true; }
        if (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey) { deleteSelection(); return true; }
        if (k == juce::KeyPress::homeKey) { rec.seek (0.0); repaint(); return true; }
        if (k == juce::KeyPress::escapeKey) { clearSelection(); repaint(); return true; }
        return false;
    }

    void showClipMenu (double t)
    {
        juce::PopupMenu m;
        juce::Component::SafePointer<Arrange> safe (this);
        const int tr = selTrack, k = selClip;
        m.addItem ("Split here", [safe, t, tr]
        {
            if (safe == nullptr) return;
            auto d = safe->rec.getProject();
            if (d.split (tr, t)) safe->commit (d);
        });
        m.addItem ("Delete", [safe] { if (safe != nullptr) safe->deleteSelection(); });
        m.addSeparator();
        m.addItem ("Copy   (Ctrl+C)", [safe] { if (safe != nullptr) safe->copyClip (false); });
        m.addItem ("Cut   (Ctrl+X)", [safe] { if (safe != nullptr) safe->copyClip (true); });
        m.addItem ("Duplicate   (Ctrl+D)", [safe] { if (safe != nullptr) safe->duplicateClip(); });
        m.addSeparator();
        m.addItem ("Normalize (peak to 0 dB)", [safe] { if (safe != nullptr) safe->normalizeClip(); });
        juce::PopupMenu gain;
        for (float db : { 6.0f, 3.0f, 0.0f, -3.0f, -6.0f, -12.0f })
            gain.addItem ((db > 0 ? "+" : "") + juce::String (db, 0) + " dB", [safe, tr, k, db]
            {
                if (safe == nullptr) return;
                auto d = safe->rec.getProject();
                if (! juce::isPositiveAndBelow (k, (int) d.tracks[(size_t) tr].clips.size())) return;
                d.tracks[(size_t) tr].clips[(size_t) k].gainDb = db;
                safe->commit (d);
            });
        m.addSubMenu ("Clip gain", gain);
        juce::PopupMenu fades;
        for (double s : { 0.01, 0.1, 0.5, 2.0 })
            fades.addItem ("In and out: " + (s < 1.0 ? juce::String (juce::roundToInt (s * 1000.0)) + " ms" : juce::String (s, 0) + " s"),
                           [safe, s] { if (safe != nullptr) safe->setClipFades (s, s); });
        fades.addItem ("Fade in only: 2 s", [safe] { if (safe != nullptr) safe->setClipFades (2.0, 0.0); });
        fades.addItem ("Fade out only: 2 s", [safe] { if (safe != nullptr) safe->setClipFades (0.0, 2.0); });
        m.addSubMenu ("Fades", fades);
        m.addItem ("Remove fades", [safe, tr, k]
        {
            if (safe == nullptr) return;
            auto d = safe->rec.getProject();
            if (! juce::isPositiveAndBelow (k, (int) d.tracks[(size_t) tr].clips.size())) return;
            d.tracks[(size_t) tr].clips[(size_t) k].fadeIn = d.tracks[(size_t) tr].clips[(size_t) k].fadeOut = 0.0;
            safe->commit (d);
        });
        m.addItem ("Loop this clip", [safe, tr, k]
        {
            if (safe == nullptr) return;
            auto d = safe->rec.getProject();
            if (! juce::isPositiveAndBelow (k, (int) d.tracks[(size_t) tr].clips.size())) return;
            d.loopStart = d.tracks[(size_t) tr].clips[(size_t) k].start;
            d.loopEnd = d.tracks[(size_t) tr].clips[(size_t) k].end();
            safe->commit (d);
            safe->rec.loop->set (1.0f);
        });
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
    }

    void showRulerMenu()
    {
        if (content() != Content::project) return;
        juce::PopupMenu m;
        juce::Component::SafePointer<Arrange> safe (this);
        m.addItem ("Clear loop range", data().hasLoop(), false, [safe]
        {
            if (safe == nullptr) return;
            auto d = safe->rec.getProject();
            d.loopStart = d.loopEnd = 0.0;
            safe->commit (d);
        });
        m.addItem ("Zoom to fit", [safe] { if (safe != nullptr) safe->zoomToFit(); });
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
    }

    float shownCursorX = -1.0f;

private:
    UiContext& ctx;
    Recorder& rec;
    RecorderWindow& owner;
    std::vector<RecProjectData> past, future;
};

//==============================================================================
/** The strip of a track, right of its lane (like Ableton's track header): name, mute, solo, mono, gain, pan, fader, meter. */
class RecorderWindow::TrackHeader final : public juce::Component
{
public:
    TrackHeader (UiContext& c, int track, std::function<void (int)> select)
        : ctx (c), t (track), rt (c.engine.recTracks[(size_t) track]), onSelect (std::move (select)),
          mute (c, *rt.mute, "M", theme::mute), solo (c, *rt.solo, "S", theme::solo), mono (c, *rt.mono, "MONO", theme::accentIn),
          trim (c, *rt.trim, "GAIN", theme::accentIn), pan (c, *rt.pan, "PAN", theme::accentIn),
          fader (c, *rt.gain, theme::accentIn), meter (rt.meter, true)
    {
        for (juce::Component* k : std::initializer_list<juce::Component*> { &mute, &solo, &mono, &trim, &pan, &fader, &meter }) addAndMakeVisible (k);
        fader.horizontal = true;
        meter.horizontal = true;
        solo.exclusiveGroup = [this]
        {
            std::vector<Param*> v;
            for (int k = 0; k < ctx.engine.recorder.getNumTracks(); ++k) v.push_back (ctx.engine.recTracks[(size_t) k].solo);
            return v;
        };
        mute.setTooltip ("Mute this track (playback and export)");
        solo.setTooltip ("Solo: only this track plays (Shift + click: several)");
        mono.setTooltip ("Mono: left and right summed");
    }

    bool selected = false;
    std::function<void()> onMenu;   // right-click: rename, delete, add tracks

    /** Muted / soloed away: the track is greyed out (its M / S buttons stay clear). */
    void setHeard (bool h)
    {
        if (h == heard) return;
        heard = h;
        for (juce::Component* k : std::initializer_list<juce::Component*> { &mono, &trim, &pan, &fader, &meter }) k->setAlpha (h ? 1.0f : 0.4f);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (heard ? (selected ? theme::panel2 : theme::panel) : (selected ? theme::panel2 : theme::panel).interpolatedWith (theme::bg, 0.6f));
        g.fillRect (r);
        if (selected) { g.setColour (theme::accentIn); g.fillRect (r.removeFromLeft (3.0f)); }
        g.setColour (theme::outline.withAlpha (0.5f));
        g.fillRect (0.0f, r.getBottom() - 1.0f, r.getWidth() + 3.0f, 1.0f);
        auto na = nameArea;
        g.setColour (heard ? theme::accentIn : theme::textFaint);
        g.setFont (theme::font (10.5f, true));
        g.drawText (channelCode (recTrackChannel (t)), na.removeFromLeft (34), juce::Justification::centredLeft);
        g.setColour (heard ? theme::text : theme::textDim);
        g.setFont (theme::font (12.0f, true));
        g.drawText (rt.name, na, juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8, 5).withTrimmedLeft (2);
        nameArea = r.removeFromTop (18);
        r.removeFromTop (2);
        auto row = r.removeFromTop (22);
        mute.setBounds (row.removeFromLeft (26));
        row.removeFromLeft (3);
        solo.setBounds (row.removeFromLeft (26));
        row.removeFromLeft (3);
        mono.setBounds (row.removeFromLeft (44));
        row.removeFromLeft (4);
        pan.setBounds (row.removeFromRight (40).withHeight (getHeight() - row.getY() - 4));
        trim.setBounds (row.removeFromRight (40).withHeight (getHeight() - row.getY() - 4));
        r.removeFromTop (4);
        auto f = r.removeFromTop (24).withTrimmedRight (84);
        meter.setBounds (f.removeFromBottom (4));
        fader.setBounds (f);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (onSelect) onSelect (t);
        if (e.mods.isPopupMenu() && onMenu) onMenu();
    }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (nameArea.contains (e.getPosition())) rename();
    }

    void rename()
    {
        juce::Component::SafePointer<TrackHeader> safe (this);
        dialog::askText (this, "Rename track", "Name of the track (also the file name of its stem when exporting).", rt.name, "Rename",
                         [safe] (juce::String name)
                         {
                             if (safe == nullptr) return;
                             auto& rec = safe->ctx.engine.recorder;
                             auto d = rec.getProject();
                             if (! juce::isPositiveAndBelow (safe->t, (int) d.tracks.size())) return;
                             d.tracks[(size_t) safe->t].name = name;
                             safe->rt.name = name;
                             rec.setProject (d);
                             if (auto* p = safe->getParentComponent()) p->repaint();
                         });
    }

    void updateMeter (float dt) { meter.update (dt); }

private:
    UiContext& ctx;
    int t;
    RecTrack& rt;
    std::function<void (int)> onSelect;
    Toggle mute, solo, mono;
    Knob trim, pan;
    Fader fader;
    MeterView meter;
    juce::Rectangle<int> nameArea;
    bool heard = true;
};

//==============================================================================
/** Export (stems / mixdown) as a card over the window. */
class RecorderWindow::ExportPanel final : public juce::Component
{
public:
    ExportPanel (Recorder& r) : rec (r)
    {
        for (auto* b : { &stems, &mix, &mixer, &loopOnly }) { addAndMakeVisible (b); b->setColour (juce::ToggleButton::textColourId, theme::text); }
        stems.setToggleState (true, juce::dontSendNotification);
        mix.setToggleState (true, juce::dontSendNotification);
        mixer.setToggleState (true, juce::dontSendNotification);
        loopOnly.setEnabled (rec.getProject().hasLoop());
        for (auto* c : { &bits, &go, &cancel }) { addAndMakeVisible (c); prepareChip (*c); }
        prepareChip (bits, true);
        bitDepth = rec.bitDepth;
        updateBits();
        bits.onClick = [this]
        {
            juce::PopupMenu m;
            for (int b : { 16, 24, 32 })
                m.addItem (b == 32 ? "32-bit float" : juce::String (b) + "-bit", true, b == bitDepth, [this, b] { bitDepth = b; updateBits(); });
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&bits));
        };
        go.colour = theme::good;
        go.active = true;
        cancel.onClick = [this] { if (onDone) onDone(); };
        go.onClick = [this]
        {
            Recorder::ExportOptions o;
            o.stems = stems.getToggleState();
            o.mixdown = mix.getToggleState();
            o.withMixer = mixer.getToggleState();
            o.loopOnly = loopOnly.getToggleState() && loopOnly.isEnabled();
            o.bitDepth = bitDepth;
            juce::String err;
            if (! rec.startExport (o, err)) { dialog::message (this, dialog::Kind::warning, "Export", err); return; }
            if (onDone) onDone();
        };
        setSize (440, 290);
    }

    std::function<void()> onDone;

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (2.0f);
        theme::drawCard (g, r, theme::panel2, theme::outline, theme::accentIn);
        auto t = getLocalBounds().reduced (20, 16);
        g.setColour (theme::text);
        g.setFont (theme::font (15.0f, true));
        g.drawText ("Export", t.removeFromTop (22), juce::Justification::centredLeft);
        g.setColour (theme::textDim);
        g.setFont (theme::font (11.5f));
        g.drawFittedText ("The edited tracks as WAV files, named after the tracks, into a new folder \"Export <date>\" next to the "
                          "recording. The recorded originals stay untouched in the take folder.",
                          t.removeFromTop (34), juce::Justification::topLeft, 3);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (20, 16).withTrimmedTop (64);
        for (auto* b : { &stems, &mix, &mixer, &loopOnly }) b->setBounds (r.removeFromTop (28));
        r.removeFromTop (6);
        auto row = r.removeFromTop (28);
        bits.setBounds (row.removeFromLeft (130));
        cancel.setBounds (row.removeFromRight (90));
        row.removeFromRight (8);
        go.setBounds (row.removeFromRight (100));
    }

private:
    void updateBits() { bits.text = bitDepth == 32 ? "32-bit float" : juce::String (bitDepth) + "-bit"; bits.repaint(); }
    Recorder& rec;
    juce::ToggleButton stems { "Stems: one file per track" }, mix { "Mixdown: all tracks in one stereo file" },
                       mixer { "With the track settings and effects (fader, pan, mute, solo, chain)" }, loopOnly { "Only the loop range" };
    Chip bits, go { "Export" }, cancel { "Cancel" };
    int bitDepth = 24;
};

//==============================================================================
RecorderWindow::RecorderWindow (UiContext& c)
    : ctx (c), rec (c.engine.recorder),
      recBtn (c, *rec.record, "REC", theme::mute, Toggle::Icon::record),
      playBtn (c, *rec.play, "PLAY", theme::good, Toggle::Icon::playPause),
      stopBtn (c, *rec.stop, "STOP", theme::text, Toggle::Icon::stop),
      loopBtn (c, *rec.loop, "LOOP", theme::good, Toggle::Icon::loop),
      projectChip ("Recordings", theme::text), exportChip ("Export...", theme::accentIn), splitChip ("Split", theme::textDim),
      deleteChip ("Delete", theme::textDim), cropChip ("Crop", theme::textDim), undoChip ("Undo", theme::textDim), redoChip ("Redo", theme::textDim),
      closeChip ("Close", theme::textDim),
      modeSeg (c, *rec.mode, theme::mute, { "V bus", "Inputs" }),
      separateSwitch (c, *rec.separate, "Separate tracks", theme::mute),
      tapSeg (c, *rec.tap, theme::accentIn, { "Pre effects", "Post effects" }),
      folderChip ("Folder", theme::textDim), formatChip ("24-bit", theme::textDim),
      level (c, *rec.level, "LEVEL", theme::good)
{
    arrange = std::make_unique<Arrange> (c, *this);
    arrange->onTrackClicked = [this] (int t) { selectTrack (t); };
    arrange->onScrolled = [this] { resized(); };
    addAndMakeVisible (*arrange);
    for (juce::Component* k : std::initializer_list<juce::Component*> { &recBtn, &playBtn, &stopBtn, &loopBtn, &projectChip, &exportChip, &splitChip,
                                                                       &deleteChip, &cropChip, &undoChip, &redoChip, &closeChip, &modeSeg,
                                                                       &separateSwitch, &tapSeg, &folderChip, &formatChip, &level })
        addAndMakeVisible (k);
    for (auto* chip : { &projectChip, &exportChip, &splitChip, &deleteChip, &cropChip, &undoChip, &redoChip, &closeChip, &folderChip, &formatChip })
        prepareChip (*chip);
    prepareChip (projectChip, true);
    prepareChip (formatChip, true);

    recBtn.setTooltip ("Record (V bus: one file; inputs: the armed inputs)");
    playBtn.setTooltip ("Play / pause (Space)");
    stopBtn.setTooltip ("Stop and go back to the start");
    loopBtn.setTooltip ("Loop the loop range (drag it in the ruler), else the whole recording");
    projectChip.setTooltip ("Recordings in the recording folder: open one, show the folder");
    exportChip.setTooltip ("Export the edited tracks as stems and / or a mixdown");
    splitChip.setTooltip ("Split at the play position: the selected clip, the tracks of a range, or all tracks (Ctrl+E)");
    deleteChip.setTooltip ("Delete the selected clip or range (Delete)");
    cropChip.setTooltip ("Keep only the selected range (or the loop range) on every track");
    undoChip.setTooltip ("Undo the last edit of the clips (Ctrl+Z in the timeline)");
    redoChip.setTooltip ("Redo (Ctrl+Y)");
    closeChip.setTooltip ("Close the recording (it stays in its folder)");
    separateSwitch.setTooltip ("On: every input its own WAV file and track (a folder per take, named after the inputs).\n"
                               "Off: the armed inputs mixed into one stereo file.");
    tapSeg.setTooltip ("Pre effects: what comes in (after the input's Gain knob). The input's effect chain is copied onto its "
                       "track, so the effects can still be changed afterwards (bottom of this window).\n"
                       "Post effects: after the input's MONO and effect chain (before pan and fader); the track starts without effects.");

    projectChip.onClick = [this] { showProjectsMenu(); };
    exportChip.onClick = [this]
    {
        if (rec.isExporting()) { rec.cancelExport(); return; }
        showExport();
    };
    splitChip.onClick = [this] { arrange->splitAtCursor(); };
    deleteChip.onClick = [this] { arrange->deleteSelection(); };
    cropChip.onClick = [this] { arrange->cropToRange(); };
    undoChip.onClick = [this] { arrange->undo(); };
    redoChip.onClick = [this] { arrange->redo(); };
    closeChip.onClick = [this] { rec.closeProject(); };
    folderChip.onClick = [this]
    {
        static std::unique_ptr<juce::FileChooser> chooser;
        chooser = std::make_unique<juce::FileChooser> ("Recording folder", rec.getFolder());
        juce::Component::SafePointer<RecorderWindow> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [safe] (const juce::FileChooser& fc)
        {
            if (safe == nullptr || fc.getResult() == juce::File()) return;
            safe->rec.folder = fc.getResult();
            safe->repaint();
        });
    };
    formatChip.onClick = [this]
    {
        juce::PopupMenu m;
        for (int b : { 16, 24, 32 })
            m.addItem (b == 32 ? "32-bit float WAV" : juce::String (b) + "-bit WAV", true, rec.bitDepth == b, [this, b] { rec.bitDepth = b; rebuildSources(); });
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&formatChip));
    };

    for (int b = 0; b < kMaxBuses; ++b)
    {
        if (! c.engine.isBusActive (b)) continue;
        outs.push_back (std::make_unique<Toggle> (c, *rec.out[(size_t) b], busLabel (b), theme::busColour (b)));
        outs.back()->setTooltip ("Play to " + busLabel (b) + " (" + c.engine.buses[(size_t) b].name + ")");
        addAndMakeVisible (*outs.back());
    }
    shownLayout = c.engine.getLayoutVersion();
    setWantsKeyboardFocus (true);
    rebuildSources();
    rebuild();
    setSize (1320, 780);
    updateTransportText();
    startTimerHz (5);
}

RecorderWindow::~RecorderWindow() { stopTimer(); }

bool RecorderWindow::running() const { return rec.isPlaying() || rec.isRecording() || rec.isExporting(); }

void RecorderWindow::rebuildSources()
{
    busSources.clear();
    armRows.clear();
    const bool inputs = rec.recordsInputs();
    if (! inputs)
        for (int k = 0; k < kMaxVirtBuses; ++k)
        {
            const int b = virtBusIndex (k);
            if (! ctx.engine.isBusActive (b)) continue;
            busSources.push_back (std::make_unique<Toggle> (ctx, *rec.source, busLabel (b), theme::busColour (b)));
            busSources.back()->choiceValue = k;
            busSources.back()->setTooltip ("Record " + busLabel (b) + " (" + ctx.engine.buses[(size_t) b].name + ")");
            addAndMakeVisible (*busSources.back());
        }
    else
        for (int i = 0; i < kMaxInputs; ++i)
        {
            if (! ctx.engine.isInputActive (i)) continue;
            ArmRow row;
            row.input = i;
            row.arm = std::make_unique<Toggle> (ctx, *rec.arm[(size_t) i], "", theme::mute, Toggle::Icon::record);
            row.arm->setTooltip ("Arm " + ctx.engine.inputs[(size_t) i].name + ": recorded with REC");
            row.meter = std::make_unique<MeterView> (ctx.engine.inputs[(size_t) i].preMeter, true);
            row.meter->horizontal = true;
            addAndMakeVisible (*row.arm);
            addAndMakeVisible (*row.meter);
            armRows.push_back (std::move (row));
        }
    separateSwitch.setVisible (inputs);
    tapSeg.setVisible (inputs);
    formatChip.text = rec.bitDepth == 32 ? "32-bit float" : juce::String (rec.bitDepth) + "-bit";
    resized();
    repaint();
}

void RecorderWindow::makeRack()
{
    fxRack.reset();
    rackBypass.reset();
    rackPost.reset();
    if (rec.getNumTracks() <= 0) return;
    const int ch = recTrackChannel (selectedTrack);
    fxRack = makeFxRack (ctx, ch, [this] { if (onScanRequested) onScanRequested(); });
    addAndMakeVisible (*fxRack);
    auto& fx = ctx.engine.fxOf (ch);
    rackBypass = std::make_unique<Toggle> (ctx, *fx.fxBypass, "BYPASS", theme::mute);
    rackBypass->setTooltip ("Bypass the whole effect chain of this track");
    rackPost = std::make_unique<Toggle> (ctx, *fx.fxPost, "PRE", theme::accentIn);
    rackPost->onText = "POST";
    rackPost->setTooltip ("Sets whether the effect chain runs before the fader (PRE, default) or after it (POST). Click to switch.");
    addAndMakeVisible (*rackBypass);
    addAndMakeVisible (*rackPost);
}

void RecorderWindow::rebuild()
{
    headers.clear();
    fxRack.reset();
    shownTracks = rec.getNumTracks();
    shownTakeTracks = rec.getTakeTrackCount();
    shownProject = rec.getProjectFile();
    if (selectedTrack >= shownTracks) selectedTrack = 0;
    for (int t = 0; t < shownTracks; ++t)
    {
        headers.push_back (std::make_unique<TrackHeader> (ctx, t, [this] (int k) { selectTrack (k); }));
        headers.back()->onMenu = [this, t] { showTrackMenu (t, arrange->cursor()); };
        headers.back()->setHeard (arrange->trackAudible (t));
        addAndMakeVisible (*headers.back());
    }
    makeRack();
    for (int t = 0; t < (int) headers.size(); ++t) headers[(size_t) t]->selected = t == selectedTrack;
    fitted = false;
    resized();
    repaint();
}

void RecorderWindow::selectTrack (int t)
{
    if (! juce::isPositiveAndBelow (t, rec.getNumTracks()) || t == selectedTrack) return;
    selectedTrack = t;
    for (int k = 0; k < (int) headers.size(); ++k) { headers[(size_t) k]->selected = k == t; headers[(size_t) k]->repaint(); }
    makeRack();
    resized();
    arrange->repaint();
    repaint (rackTitle);
}

void RecorderWindow::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);
    auto card = [&] (juce::Rectangle<int> r, const juce::String& title, juce::Colour c)
    {
        if (r.isEmpty()) return;
        auto f = r.toFloat().reduced (2.0f);
        if (theme::deep()) theme::drawCard (g, f, theme::panel, theme::cardLine, c);
        else { g.setColour (theme::panel); g.fillRoundedRectangle (f, 8.0f); }
        g.setColour (c);
        g.setFont (theme::font (11.0f, true));
        g.drawText (title, r.reduced (12, 8).removeFromTop (16), juce::Justification::centredLeft);
    };
    card (sourceArea, "RECORD", theme::mute);
    card (outputArea, "PLAY TO", theme::accentHw);

    // top: time + status
    auto top = getLocalBounds().removeFromTop (kTopHeight).reduced (12, 8);
    top.removeFromLeft (4 * 40 + 3 * 4 + 16);
    g.setColour (theme::text);
    g.setFont (theme::mono (19.0f));
    g.drawText (timeText, top.removeFromLeft (220), juce::Justification::centredLeft);
    g.setColour (theme::textDim);
    g.setFont (theme::font (11.5f));
    g.drawText (statusText, top.withRight (projectChip.getX() - 10), juce::Justification::centredLeft, true);

    // source labels
    if (! sourceArea.isEmpty())
    {
        g.setFont (theme::font (12.0f));
        for (auto& row : armRows)
        {
            const auto b = row.arm->getBounds();
            g.setColour (rec.arm[(size_t) row.input]->isOn() ? theme::text : theme::textDim);
            g.drawText (ctx.engine.inputs[(size_t) row.input].name, b.getRight() + 8, b.getY(), sourceArea.getRight() - b.getRight() - 20, 14,
                        juce::Justification::centredLeft, true);
        }
        g.setColour (theme::textFaint);
        g.setFont (theme::font (10.5f));
        const auto f = rec.getFolder();
        g.drawText (f.getFullPathName(), folderChip.getBounds().withY (folderChip.getBottom() + 4).withHeight (14).withWidth (sourceArea.getWidth() - 24),
                    juce::Justification::centredLeft, true);
    }

    // effect rack title
    if (! rackTitle.isEmpty())
    {
        g.setColour (theme::panel);
        g.fillRect (rackArea);
        if (rec.getNumTracks() > 0)
        {
            g.setColour (theme::accentIn);
            g.setFont (theme::font (13.0f, true));
            auto t = rackTitle;
            g.drawText (channelCode (recTrackChannel (selectedTrack)), t.removeFromLeft (44), juce::Justification::centredLeft);
            g.setColour (theme::text);
            g.drawText (ctx.engine.recTracks[(size_t) selectedTrack].name + "   effect chain", t, juce::Justification::centredLeft, true);
        }
    }
}

void RecorderWindow::layoutSources (juce::Rectangle<int> r)
{
    r = r.reduced (12, 8).withTrimmedTop (22);
    modeSeg.setBounds (r.removeFromTop (26));
    r.removeFromTop (10);
    if (! rec.recordsInputs())
    {
        auto row = r.removeFromTop (28);
        int col = 0;
        for (auto& b : busSources)
        {
            if (col == 4) { row = r.removeFromTop (28); col = 0; r.removeFromTop (4); }
            b->setBounds (row.removeFromLeft ((r.getWidth() - 12) / 4));
            row.removeFromLeft (4);
            ++col;
        }
    }
    else
    {
        separateSwitch.setBounds (r.removeFromTop (24));
        r.removeFromTop (6);
        tapSeg.setBounds (r.removeFromTop (24));
        r.removeFromTop (10);
        const int rowH = juce::jlimit (22, 30, (r.getHeight() - 70) / juce::jmax (1, (int) armRows.size()));
        for (auto& row : armRows)
        {
            auto line = r.removeFromTop (rowH);
            row.arm->setBounds (line.removeFromLeft (26).withSizeKeepingCentre (26, 22));
            row.meter->setBounds (line.withTrimmedLeft (8).removeFromBottom (5).withTrimmedBottom (1));
        }
    }
    auto bottom = sourceArea.reduced (12, 10).removeFromBottom (44);
    auto chips = bottom.removeFromTop (24);
    folderChip.setBounds (chips.removeFromLeft (chips.getWidth() / 2 - 3));
    chips.removeFromLeft (6);
    formatChip.setBounds (chips);
}

void RecorderWindow::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (kTopHeight).reduced (12, 9);
    for (auto* b : { &recBtn, &playBtn, &stopBtn, &loopBtn }) { b->setBounds (top.removeFromLeft (40)); top.removeFromLeft (4); }
    exportChip.setBounds (top.removeFromRight (96));
    top.removeFromRight (14);
    closeChip.setBounds (top.removeFromRight (60));
    top.removeFromRight (6);
    projectChip.setBounds (top.removeFromRight (150));
    top.removeFromRight (14);
    for (auto* c : { &redoChip, &undoChip, &cropChip, &deleteChip, &splitChip }) { c->setBounds (top.removeFromRight (56)); top.removeFromRight (4); }

    r.reduce (6, 0);
    r.removeFromBottom (6);
    const bool hasTracks = rec.getNumTracks() > 0;
    const int rackH = hasTracks ? juce::jlimit (200, 300, getHeight() / 3) : 0;
    rackArea = r.removeFromBottom (rackH);
    if (hasTracks) r.removeFromBottom (6);
    sourceArea = r.removeFromLeft (kSourceWidth);
    r.removeFromLeft (6);
    outputArea = r.removeFromRight (kOutputWidth);
    r.removeFromRight (6);
    headerArea = hasTracks ? r.removeFromRight (kHeaderWidth) : juce::Rectangle<int>();
    arrange->setBounds (r);
    if (! fitted && arrange->getWidth() > 0) { arrange->zoomToFit(); fitted = true; }

    layoutSources (sourceArea);
    {
        auto o = outputArea.reduced (12, 8).withTrimmedTop (22);
        auto row = o.removeFromTop (28);
        int col = 0;
        for (auto& t : outs)
        {
            if (col == 2) { o.removeFromTop (4); row = o.removeFromTop (28); col = 0; }
            t->setBounds (row.removeFromLeft ((o.getWidth() - 4) / 2));
            row.removeFromLeft (4);
            ++col;
        }
        o.removeFromTop (14);
        level.setBounds (o.removeFromTop (70).withSizeKeepingCentre (60, 70));
    }

    // track strips next to their lanes (scroll with the timeline)
    for (int t = 0; t < (int) headers.size(); ++t)
    {
        const int y = arrange->getY() + kRulerHeight + t * kLaneHeight - arrange->scrollY;
        headers[(size_t) t]->setBounds (headerArea.getX(), y, headerArea.getWidth(), kLaneHeight);
        const bool visible = y + kLaneHeight > arrange->getY() + kRulerHeight && y < arrange->getBottom();
        headers[(size_t) t]->setVisible (visible);
    }

    rackTitle = {};
    if (fxRack != nullptr)
    {
        auto rr = rackArea.reduced (8, 6);
        rackTitle = rr.removeFromTop (22).withTrimmedLeft (6);
        const int nameW = 52 + juce::GlyphArrangement::getStringWidthInt (theme::font (13.0f, true),
                                                                       ctx.engine.recTracks[(size_t) selectedTrack].name + "   effect chain");
        if (rackBypass != nullptr) rackBypass->setBounds (rackTitle.getX() + nameW + 12, rackTitle.getY(), 74, rackTitle.getHeight());
        if (rackPost != nullptr && rackBypass != nullptr) rackPost->setBounds (rackBypass->getRight() + 6, rackTitle.getY(), 54, rackTitle.getHeight());
        rr.removeFromTop (4);
        fxRack->setBounds (rr);
    }
    if (exportPanel != nullptr) exportPanel->setCentrePosition (getLocalBounds().getCentre());
}

void RecorderWindow::showProjectsMenu()
{
    juce::PopupMenu m;
    const auto projects = rec.findProjects();
    juce::Component::SafePointer<RecorderWindow> safe (this);
    if (projects.isEmpty()) m.addItem ("No multitrack recordings in this folder yet", false, false, [] {});
    for (int i = 0; i < juce::jmin (30, projects.size()); ++i)
    {
        const auto f = projects[i];
        const bool open = f == rec.getProjectFile();
        m.addItem (f.getParentDirectory().getFileName() + "   " + f.getLastModificationTime().formatted ("%d.%m. %H:%M"), true, open, [safe, f]
        {
            if (safe == nullptr) return;
            juce::String err;
            if (! safe->rec.openProject (f, err)) dialog::message (safe.getComponent(), dialog::Kind::warning, "Recorder", err);
        });
    }
    m.addSeparator();
    m.addItem ("Open other...", [safe]
    {
        if (safe == nullptr) return;
        static std::unique_ptr<juce::FileChooser> chooser;
        chooser = std::make_unique<juce::FileChooser> ("Open a recording", safe->rec.getFolder(),
                                                       juce::String ("*") + Recorder::kProjectExtension + ";*.wav;*.flac;*.aif;*.aiff;*.mp3;*.ogg");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc)
        {
            const auto f = fc.getResult();
            if (safe == nullptr || f == juce::File()) return;
            juce::String err;
            const bool ok = f.hasFileExtension (juce::String (Recorder::kProjectExtension).substring (1)) ? safe->rec.openProject (f, err)
                                                                                                     : safe->rec.loadFile (f, err);
            if (! ok) dialog::message (safe.getComponent(), dialog::Kind::warning, "Recorder", err);
        });
    });
    m.addSeparator();
    m.addItem ("Add an audio file as a track...", (rec.hasProject() || rec.getLoadedFile() != juce::File()) && ! rec.isRecording(), false,
               [safe] { if (safe != nullptr) safe->importTrack(); });
    if (! rec.hasProject() && rec.getLoadedFile() != juce::File())
        m.addItem ("Edit the open file as a multitrack recording", ! rec.isRecording(), false, [safe]
        {
            if (safe == nullptr) return;
            juce::String err;
            if (! safe->rec.convertFileToProject (err)) dialog::message (safe.getComponent(), dialog::Kind::warning, "Recorder", err);
        });
    m.addSeparator();
    m.addItem ("Show the folder in Explorer", [safe]
    {
        if (safe == nullptr) return;
        const auto f = safe->rec.hasProject() ? safe->rec.getProjectFolder() : safe->rec.getFolder();
        f.createDirectory();
        f.startAsProcess();
    });
    if (rec.getLastExportFolder().isDirectory())
        m.addItem ("Show the last export", [safe] { if (safe != nullptr) safe->rec.getLastExportFolder().startAsProcess(); });
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&projectChip));
}

void RecorderWindow::showTrackMenu (int track, double atSeconds)
{
    if (! juce::isPositiveAndBelow (track, rec.getNumTracks())) return;
    juce::PopupMenu m;
    juce::Component::SafePointer<RecorderWindow> safe (this);
    m.addSectionHeader (channelCode (recTrackChannel (track)) + "  " + ctx.engine.recTracks[(size_t) track].name);
    m.addItem ("Rename...", [safe, track]
    {
        if (safe != nullptr && juce::isPositiveAndBelow (track, (int) safe->headers.size())) safe->headers[(size_t) track]->rename();
    });
    m.addItem ("Paste clip here   (Ctrl+V)", Arrange::clipboard().has && Arrange::clipboard().project == rec.getProjectFile()
                                                 && Arrange::clipboard().track == track, false, [safe, atSeconds]
    {
        if (safe == nullptr) return;
        safe->rec.seek (atSeconds);
        safe->arrange->pasteClip();
    });
    m.addSeparator();
    m.addItem ("Add an audio file as a track...", ! rec.isRecording(), false, [safe] { if (safe != nullptr) safe->importTrack(); });
    m.addItem ("Delete this track...", rec.getNumTracks() > 1 && ! rec.isRecording(), false, [safe, track]
    {
        if (safe == nullptr) return;
        const auto name = safe->ctx.engine.recTracks[(size_t) track].name;
        dialog::confirm (safe.getComponent(), dialog::Kind::danger, "Delete the track \"" + name + "\"?",
                         "The track and its clips leave the recording. Its file stays in the recording's folder.", "Delete", [safe, track]
                         {
                             if (safe == nullptr) return;
                             juce::String err;
                             safe->arrange->forgetHistory();   // the clip undo refers to the old tracks
                             if (! safe->rec.removeTrack (track, err)) dialog::message (safe.getComponent(), dialog::Kind::warning, "Recorder", err);
                         });
    });
    m.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
}

void RecorderWindow::importTrack()
{
    static std::unique_ptr<juce::FileChooser> chooser;
    chooser = std::make_unique<juce::FileChooser> ("Add an audio file as a track", rec.getFolder(), "*.wav;*.flac;*.aif;*.aiff;*.mp3;*.ogg;*.wma");
    juce::Component::SafePointer<RecorderWindow> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc)
    {
        const auto f = fc.getResult();
        if (safe == nullptr || ! f.existsAsFile()) return;
        auto& r = safe->rec;
        juce::String err;
        const double at = r.hasProject() ? safe->arrange->cursor() : 0.0;
        if (! r.hasProject() && ! r.convertFileToProject (err))
        {
            dialog::message (safe.getComponent(), dialog::Kind::warning, "Recorder",
                             err.isNotEmpty() ? err : juce::String ("Open or record something first: the file is added to that recording."));
            return;
        }
        safe->arrange->forgetHistory();
        if (! r.addTrackFromFile (f, at, err)) dialog::message (safe.getComponent(), dialog::Kind::warning, "Recorder", err);
    });
}

void RecorderWindow::showExport()
{
    if (! rec.hasProject()) return;
    exportPanel = std::make_unique<ExportPanel> (rec);
    exportPanel->onDone = [this] { juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<RecorderWindow> (this)] { if (safe != nullptr) safe->exportPanel.reset(); }); };
    addAndMakeVisible (*exportPanel);
    exportPanel->setCentrePosition (getLocalBounds().getCentre());
}

void RecorderWindow::updateTransportText()
{
    juce::String t, s;
    if (rec.isRecording())
    {
        t = clockText (rec.getRecordSeconds());
        s = rec.recordsInputs() ? (rec.getTakeTrackCount() > 0 ? "Recording " + juce::String (rec.getTakeTrackCount()) + " tracks"
                                                               : juce::String ("Recording the inputs into one file"))
                                : "Recording " + busLabel (rec.getRecordSourceBus());
    }
    else
    {
        t = clockText (arrange->cursor()) + " / " + clockText (rec.getPlayLengthSeconds()).dropLastCharacters (2);
        if (rec.isExporting()) s = "Exporting " + juce::String (juce::roundToInt (rec.getExportProgress() * 100.0f)) + " %";
        else if (rec.hasProject()) s = rec.getProjectFolder().getFileName() + "  -  " + juce::String (rec.getNumTracks()) + " tracks";
        else if (rec.getLoadedFile() != juce::File()) s = rec.getLoadedFile().getFileName();
        if (rec.getLastError().isNotEmpty()) s = rec.getLastError();
    }
    if (t != timeText || s != statusText)
    {
        timeText = t;
        statusText = s;
        repaint (getLocalBounds().removeFromTop (kTopHeight));
    }
}

void RecorderWindow::timerCallback()
{
    if (! isShowing()) return;
    // Mute / solo: the whole lane and strip of a track grey out (not only the strip the play head repaints).
    if (const auto a = arrange->audibleVersion(); a != shownAudible)
    {
        shownAudible = a;
        for (int t = 0; t < (int) headers.size(); ++t) headers[(size_t) t]->setHeard (arrange->trackAudible (t));
        arrange->repaint();
    }
    // Controls are only rebuilt while no mouse button is held: a button that is destroyed under the mouse loses
    // the click (the recorder's buttons seemed not to react for a moment).
    if (juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown())
    {
        updateTransportText();
        return;
    }
    // Something else changed the layout: inputs / buses added or removed, a take started or ended, another project.
    if (ctx.engine.getLayoutVersion() != shownLayout)
    {
        shownLayout = ctx.engine.getLayoutVersion();
        for (auto& o : outs) removeChildComponent (o.get());
        outs.clear();
        for (int b = 0; b < kMaxBuses; ++b)
        {
            if (! ctx.engine.isBusActive (b)) continue;
            outs.push_back (std::make_unique<Toggle> (ctx, *rec.out[(size_t) b], busLabel (b), theme::busColour (b)));
            addAndMakeVisible (*outs.back());
        }
        rebuildSources();
    }
    const bool modeInputs = rec.recordsInputs();
    if (modeInputs != shownModeInputs) { shownModeInputs = modeInputs; rebuildSources(); }
    if (rec.getNumTracks() != shownTracks || rec.getTakeTrackCount() != shownTakeTracks || rec.getProjectFile() != shownProject)
    {
        if (rec.getProjectFile() != shownProject) arrange->forgetHistory();
        rebuild();
    }
    if (const auto v = rec.stateVersion.load(); v != shownState)
    {
        shownState = v;
        arrange->repaint();
    }
    const bool project = rec.hasProject() && ! rec.isExporting();
    for (auto* c : { &splitChip, &deleteChip, &cropChip }) c->setEnabled (project);
    undoChip.setEnabled (project && arrange->canUndo());
    redoChip.setEnabled (project && arrange->canRedo());
    closeChip.setVisible (rec.hasProject());
    exportChip.setEnabled (rec.hasProject() || rec.isExporting());
    const auto exportText = rec.isExporting() ? juce::String ("Cancel") : juce::String ("Export...");
    if (exportChip.text != exportText) { exportChip.text = exportText; exportChip.repaint(); }
    updateTransportText();
}

void RecorderWindow::frame (double timestamp)
{
    if (! isShowing()) return;
    const float dt = lastFrame > 0.0 ? (float) juce::jmin (0.1, timestamp - lastFrame) : 0.016f;
    lastFrame = timestamp;
    for (auto& h : headers) if (h->isVisible()) h->updateMeter (dt);
    for (auto& row : armRows) row.meter->update (dt);
    updateFxRackMeters (fxRack.get());
    if (running())
    {
        if (rec.isRecording()) { arrange->follow (rec.getRecordSeconds()); arrange->repaint(); }
        else
        {
            // Only the strips around the old and the new play position.
            const float x = arrange->timeToX (arrange->cursor());
            if (std::abs (x - arrange->shownCursorX) >= 1.0f)
            {
                const auto old = arrange->shownCursorX;
                arrange->repaint (juce::Rectangle<float> (juce::jmin (old, x) - 6.0f, 0.0f, std::abs (x - old) + 12.0f, (float) arrange->getHeight()).getSmallestIntegerContainer());
            }
        }
        updateTransportText();
    }
}

bool RecorderWindow::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::spaceKey) { rec.play->toggle(); return true; }
    return ctx.handleUndoKey (k);
}

void RecorderWindow::visibilityChanged()
{
    if (isVisible()) { rebuildSources(); rebuild(); }
}

void RecorderWindow::parentHierarchyChanged() {}
} // namespace hydra

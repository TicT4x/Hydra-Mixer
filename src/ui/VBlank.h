#pragma once
#include "../Common.h"

namespace hydra
{
/** juce::VBlankAttachment that pauses while its window is hidden (tray) or minimised.
    Why: meters, analyzers and the animation checks follow the display refresh (60-144 Hz); a plain attachment kept
    waking the app at that rate also while nobody could see it. A shared check (4 Hz) detaches an attachment once
    its window has been hidden or minimised for a second and attaches it again as soon as the window is back; Main
    calls refreshAll() when it shows the main window, so nothing waits for that check. While the window is visible it
    behaves exactly like juce::VBlankAttachment. Message thread only. */
class VBlank final
{
public:
    VBlank (juce::Component* c, std::function<void (double)> cb) : comp (c), callback (std::move (cb))
    {
        attachment = std::make_unique<juce::VBlankAttachment> (comp, callback);
        list().push_back (this);
        Checker::ensure();
    }

    ~VBlank()
    {
        auto& l = list();
        l.erase (std::remove (l.begin(), l.end(), this), l.end());
        if (l.empty()) Checker::stop();
    }

    VBlank (const VBlank&) = delete;
    VBlank& operator= (const VBlank&) = delete;

    /** Attach every paused one whose window can be seen again (e.g. the main window came back from the tray). */
    static void refreshAll()
    {
        for (auto* v : std::vector<VBlank*> (list())) v->update (true);
    }

private:
    bool wanted() const
    {
        auto* peer = comp->getPeer();
        if (peer == nullptr) return true;   // not on screen yet: JUCE's attachment waits for the window itself
        return ! peer->isMinimised() && comp->getTopLevelComponent()->isVisible();
    }

    void update (bool onlyAttach)
    {
        if (wanted())
        {
            hiddenChecks = 0;
            if (attachment == nullptr) attachment = std::make_unique<juce::VBlankAttachment> (comp, callback);
        }
        else if (! onlyAttach && attachment != nullptr && ++hiddenChecks >= 4)   // hidden for about a second
            attachment.reset();
    }

    static std::vector<VBlank*>& list() { static std::vector<VBlank*> l; return l; }

    struct Checker final : private juce::Timer
    {
        static std::unique_ptr<Checker>& instance() { static std::unique_ptr<Checker> c; return c; }
        static void ensure() { if (instance() == nullptr) instance() = std::make_unique<Checker>(); }
        static void stop() { instance().reset(); }
        Checker() { startTimer (250); }
        ~Checker() override { stopTimer(); }
        void timerCallback() override
        {
            for (auto* v : std::vector<VBlank*> (list())) v->update (false);
        }
    };

    juce::Component* comp;
    std::function<void (double)> callback;
    std::unique_ptr<juce::VBlankAttachment> attachment;
    int hiddenChecks = 0;
};
} // namespace hydra

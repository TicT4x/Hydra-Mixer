#pragma once
#include "Common.h"

namespace hydra
{
/** Looks for new releases of Hydra Mixer on GitHub (TicT4x/Hydra-Mixer) and downloads them.

    Releases marked as pre-release on GitHub are betas: offered only with "Include beta versions" on. A release
    needs an asset "HydraMixer-<ver>-win64.exe"; its SHA-256 (GitHub's asset digest) is checked after the download.
    Checking and downloading run on background threads; the UI reads getStatus() (cheap, any time) and
    compares `version` to see changes. Installing (state/Install.h) is done by the app: save, swap, restart. */
class Updater
{
public:
    static constexpr const char* kRepo = "TicT4x/Hydra-Mixer";

    struct Release
    {
        juce::String version, tag, title, notes, page, url, sha256;
        juce::int64 size = 0;
        bool prerelease = false;
    };
    enum class State { idle, checking, upToDate, available, downloading, ready, failed };
    struct Status
    {
        State state = State::idle;
        Release release;          // newest release that is newer than this build (available / downloading / ready)
        float progress = 0.0f;    // download 0..1
        juce::String error;       // failed: what went wrong
        juce::Time lastCheck;
        juce::File file;          // ready: the downloaded exe
    };

    ~Updater();

    /** Asks GitHub (background thread). includeBeta: pre-releases count too. A check already running is not repeated. */
    void check (bool includeBeta);
    /** Downloads the release found by check() (background thread). */
    void download();
    void cancelDownload() { cancel = true; }

    Status getStatus() const;
    std::atomic<uint32_t> version { 0 };   // bumped whenever the status changes

    /** Parses the releases list of the GitHub API (JSON) and returns the newest release newer than `current`
        (pre-releases only with includeBeta); false = none. Public for the tests. */
    static bool pickRelease (const juce::var& json, const juce::String& current, bool includeBeta, Release& out);

private:
    void setStatus (std::function<void (Status&)> change);
    void join();

    mutable juce::CriticalSection lock;
    Status status;
    std::unique_ptr<juce::Thread> worker;
    std::atomic<bool> cancel { false };
};
} // namespace hydra

#include "Updater.h"
#include "Install.h"

#include <windows.h>
#include <bcrypt.h>

namespace hydra
{
namespace
{
/** A one-off background job (check or download). */
class Job final : public juce::Thread
{
public:
    Job (const juce::String& name, std::function<void()> f) : juce::Thread (name), fn (std::move (f)) {}
    void run() override { fn(); }

private:
    std::function<void()> fn;
};

juce::String sha256Of (const juce::File& file, const std::atomic<bool>& cancel)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider (&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    juce::String hex;
    if (BCryptCreateHash (alg, &hash, nullptr, 0, nullptr, 0, 0) == 0)
    {
        juce::FileInputStream in (file);
        std::vector<char> buf (1 << 20);
        bool ok = in.openedOk();
        while (ok && ! in.isExhausted() && ! cancel.load())
        {
            const int n = in.read (buf.data(), (int) buf.size());
            if (n <= 0) break;
            ok = BCryptHashData (hash, (PUCHAR) buf.data(), (ULONG) n, 0) == 0;
        }
        unsigned char digest[32] {};
        if (ok && ! cancel.load() && BCryptFinishHash (hash, digest, sizeof (digest), 0) == 0)
            hex = juce::String::toHexString (digest, sizeof (digest), 0);
        BCryptDestroyHash (hash);
    }
    BCryptCloseAlgorithmProvider (alg, 0);
    return hex.toLowerCase();
}

juce::String userAgent() { return "HydraMixer/" + install::currentVersion(); }
} // namespace

Updater::~Updater()
{
    cancel = true;
    join();
}

void Updater::join()
{
    if (worker != nullptr)
    {
        worker->stopThread (15000);
        worker.reset();
    }
}

Updater::Status Updater::getStatus() const
{
    const juce::ScopedLock sl (lock);
    return status;
}

void Updater::setStatus (std::function<void (Status&)> change)
{
    {
        const juce::ScopedLock sl (lock);
        change (status);
    }
    version.fetch_add (1);
}

bool Updater::pickRelease (const juce::var& json, const juce::String& current, bool includeBeta, Release& out)
{
    const auto* list = json.getArray();
    if (list == nullptr) return false;
    bool found = false;
    for (auto& r : *list)
    {
        if ((bool) r["draft"]) continue;
        const bool pre = (bool) r["prerelease"];
        if (pre && ! includeBeta) continue;
        const juce::String tag = r["tag_name"].toString();
        if (install::compareVersions (tag, current) <= 0) continue;
        if (found && install::compareVersions (tag, out.tag) <= 0) continue;
        // The Windows exe of the release.
        const auto* assets = r["assets"].getArray();
        if (assets == nullptr) continue;
        for (auto& a : *assets)
        {
            const juce::String name = a["name"].toString();
            if (! (name.startsWithIgnoreCase ("HydraMixer-") && name.endsWithIgnoreCase ("-win64.exe"))) continue;
            Release rel;
            rel.tag = tag;
            rel.version = tag.startsWithIgnoreCase ("v") ? tag.substring (1) : tag;
            rel.title = r["name"].toString().isNotEmpty() ? r["name"].toString() : "Hydra Mixer " + rel.version;
            rel.notes = r["body"].toString();
            rel.page = r["html_url"].toString();
            rel.url = a["browser_download_url"].toString();
            rel.size = (juce::int64) a["size"];
            const juce::String digest = a["digest"].toString();
            if (digest.startsWithIgnoreCase ("sha256:")) rel.sha256 = digest.substring (7).toLowerCase();
            rel.prerelease = pre;
            out = rel;
            found = true;
            break;
        }
    }
    return found;
}

void Updater::check (bool includeBeta)
{
    {
        const auto s = getStatus();
        if (s.state == State::checking || s.state == State::downloading || s.state == State::ready) return;
    }
    join();
    setStatus ([] (Status& s) { s.state = State::checking; s.error = {}; });
    worker = std::make_unique<Job> ("Update check", [this, includeBeta]
    {
        juce::String error;
        Release rel;
        bool found = false;
        const juce::URL url (juce::String ("https://api.github.com/repos/") + kRepo + "/releases?per_page=30");
        int statusCode = 0;
        auto stream = url.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                                 .withConnectionTimeoutMs (15000)
                                                 .withExtraHeaders ("Accept: application/vnd.github+json\r\nUser-Agent: " + userAgent())
                                                 .withStatusCode (&statusCode)
                                                 .withNumRedirectsToFollow (5));
        if (stream == nullptr) error = "No connection to github.com";
        else if (statusCode == 403 || statusCode == 429) error = "GitHub is busy (rate limit), trying again later";
        else if (statusCode != 200) error = "GitHub answered " + juce::String (statusCode);
        else
        {
            const auto json = juce::JSON::parse (stream->readEntireStreamAsString());
            found = pickRelease (json, install::currentVersion(), includeBeta, rel);
        }
        setStatus ([&] (Status& s)
        {
            s.lastCheck = juce::Time::getCurrentTime();
            s.error = error;
            if (error.isNotEmpty()) s.state = State::failed;
            else if (found) { s.state = State::available; s.release = rel; }
            else { s.state = State::upToDate; s.release = {}; }
        });
        juce::MessageManager::callAsync ([error, found, v = rel.version]
        {
            if (error.isNotEmpty()) logLine ("update check: " + error);
            else if (found) logLine ("update check: " + v + " available");
        });
    });
    worker->startThread (juce::Thread::Priority::low);
}

void Updater::download()
{
    const auto s = getStatus();
    if (s.state != State::available && s.state != State::failed) return;
    if (s.release.url.isEmpty()) return;
    join();
    cancel = false;
    const auto rel = s.release;
    setStatus ([] (Status& st) { st.state = State::downloading; st.progress = 0.0f; st.error = {}; });
    worker = std::make_unique<Job> ("Update download", [this, rel]
    {
        auto dir = install::installDir();
        if (! dir.createDirectory()) dir = juce::File::getSpecialLocation (juce::File::tempDirectory);
        const auto part = dir.getChildFile ("update-" + rel.version + ".part");
        const auto exe = dir.getChildFile ("HydraMixer-" + rel.version + "-win64.exe");
        juce::String error;
        {
            auto stream = juce::URL (rel.url).createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                                                    .withConnectionTimeoutMs (20000)
                                                                    .withExtraHeaders ("User-Agent: " + userAgent())
                                                                    .withNumRedirectsToFollow (8));
            if (stream == nullptr) error = "No connection to github.com";
            else
            {
                part.deleteFile();
                juce::FileOutputStream out (part);
                if (! out.openedOk()) error = "Cannot write " + part.getFullPathName();
                const juce::int64 total = rel.size > 0 ? rel.size : stream->getTotalLength();
                std::vector<char> chunk (1 << 18);
                juce::int64 done = 0;
                while (error.isEmpty() && ! stream->isExhausted())
                {
                    if (cancel.load() || juce::Thread::currentThreadShouldExit()) { error = "Cancelled"; break; }
                    const int n = stream->read (chunk.data(), (int) chunk.size());
                    if (n < 0) { error = "Download interrupted"; break; }
                    if (n == 0) break;
                    if (! out.write (chunk.data(), (size_t) n)) { error = "Disk full?"; break; }
                    done += n;
                    if (total > 0) setStatus ([&] (Status& st) { st.progress = (float) juce::jlimit (0.0, 1.0, (double) done / (double) total); });
                }
                out.flush();
                if (error.isEmpty() && rel.size > 0 && done != rel.size) error = "Download incomplete";
            }
        }
        if (error.isEmpty() && rel.sha256.isNotEmpty() && sha256Of (part, cancel) != rel.sha256)
            error = cancel.load() ? "Cancelled" : "The download is damaged (checksum differs)";
        if (error.isEmpty())
        {
            exe.deleteFile();
            if (! part.moveFileTo (exe)) error = "Cannot write " + exe.getFullPathName();
        }
        part.deleteFile();
        setStatus ([&] (Status& st)
        {
            if (error.isEmpty()) { st.state = State::ready; st.file = exe; st.progress = 1.0f; }
            else { st.state = error == "Cancelled" ? State::available : State::failed; st.error = error == "Cancelled" ? juce::String() : error; }
        });
        juce::MessageManager::callAsync ([error, v = rel.version]
        {
            logLine ("update " + v + (error.isEmpty() ? ": downloaded" : ": " + error));
        });
    });
    worker->startThread (juce::Thread::Priority::low);
}
} // namespace hydra

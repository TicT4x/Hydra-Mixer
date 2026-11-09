#include "RemoteControl.h"
#include "state/Scenes.h"
#include "engine/NetLink.h"
#include "RemoteData.h"

namespace hydra
{
namespace
{
RemoteControl*& instance() { static RemoteControl* r = nullptr; return r; }

juce::File apkFile() { return juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("HydraLink.apk"); }

uint8_t meterByte (float peak)
{
    const float db = gainToDb (peak);
    return (uint8_t) juce::jlimit (0, 255, juce::roundToInt ((db + 60.0f) / 66.0f * 255.0f));
}

juce::var paramValue (const Param& p)
{
    juce::Array<juce::var> a;
    a.add ((double) std::round (p.getNormalised() * 10000.0f) / 10000.0);
    a.add (p.toText());
    return a;
}
} // namespace

RemoteControl::RemoteControl (Engine& e, SceneBank& s) : engine (e), scenes (s)
{
    instance() = this;
    server.setFiles ({
        { "/index.html", "text/html; charset=utf-8", RemoteData::index_html, (size_t) RemoteData::index_htmlSize },
        { "/manifest.webmanifest", "application/manifest+json", RemoteData::manifest_webmanifest, (size_t) RemoteData::manifest_webmanifestSize },
        { "/icon.png", "image/png", RemoteData::icon_png, (size_t) RemoteData::icon_pngSize },
        // The phone app, if it lies next to the exe (build.ps1 copies it there): "Install" in the web app offers it.
        { "/HydraLink.apk", "application/vnd.android.package-archive", nullptr, 0, apkFile() },
    });
}

RemoteControl::~RemoteControl()
{
    stopTimer();
    server.stop();
    if (meterUsed) Meter::remoteUsers.fetch_sub (1);
    if (instance() == this) instance() = nullptr;
}

RemoteControl* RemoteControl::get() noexcept { return instance(); }

bool RemoteControl::apply (bool enabled, int port, const juce::String& code, juce::String& error)
{
    if (! enabled)
    {
        if (server.isRunning()) logLine ("Hydra Remote off");
        server.stop();
        clients.clear();
        meterClients.clear();
        updateMeterUsers();
        stopTimer();
        return true;
    }
    const bool was = server.isRunning();
    const int before = server.getPort();
    remote::Server::Config c;
    c.port = port;
    c.code = code;
    if (! server.start (c, error))
    {
        logLine ("Hydra Remote: " + error);
        return false;
    }
    if (! was || before != port) logLine ("Hydra Remote on, port " + juce::String (port));
    if (! isTimerRunning()) startTimerHz (4);
    return true;
}

juce::StringArray RemoteControl::links() const
{
    juce::StringArray out;
    if (! server.isRunning()) return out;
    for (auto& a : net::Hub::localAddresses())
        if (net::Hub::isLocalAddress (a) && ! a.startsWith ("127.") && ! a.startsWith ("169.254."))
            out.add ("http://" + a + ":" + juce::String (server.getPort()) + "/");
    return out;
}

juce::String RemoteControl::preferredLink() const
{
    const auto l = links();
    for (auto& s : l) if (s.contains ("//192.168.")) return s;   // home routers
    return l.isEmpty() ? juce::String() : l[0];
}

//==============================================================================
juce::String RemoteControl::layoutKey() const
{
    juce::String k (juce::String ((int) engine.getLayoutVersion()));
    for (int i = 0; i < kMaxInputs; ++i)
        if (engine.isInputActive (i)) k << '|' << engine.inputs[(size_t) i].name << '<' << engine.inputs[(size_t) i].source.describe();
    for (int b = 0; b < kMaxBuses; ++b)
        if (engine.isBusActive (b)) k << '|' << engine.buses[(size_t) b].name << '>' << engine.buses[(size_t) b].icon;
    k << '|' << (int) scenes.version.load() << ':' << scenes.getCurrent() << ':' << scenes.size();
    if (nameProvider) k << '|' << nameProvider();
    return k;
}

void RemoteControl::rebuildExposed()
{
    exposed.clear();
    byId.clear();
    shownInputs.clear();
    shownBuses.clear();
    auto add = [this] (Param* p)
    {
        if (p == nullptr || byId.count (p->id) != 0) return;
        exposed.push_back ({ p, p->getVersion() });
        byId[p->id] = p;
    };
    for (int b = 0; b < kMaxBuses; ++b)
        if (engine.isBusActive (b)) shownBuses.push_back (b);
    for (int i = 0; i < kMaxInputs; ++i)
    {
        if (! engine.isInputActive (i)) continue;
        shownInputs.push_back (i);
        auto& s = engine.inputs[(size_t) i];
        for (auto* p : { s.gain, s.trim, s.pan, s.mute, s.solo, s.mono, s.fxBypass }) add (p);
        for (int b : shownBuses) { add (s.route[(size_t) b]); add (s.send[(size_t) b]); }
    }
    for (int b : shownBuses)
    {
        auto& bus = engine.buses[(size_t) b];
        for (auto* p : { bus.gain, bus.mute, bus.mono, bus.sel, bus.fxBypass }) add (p);
    }
    add (engine.monOn);
}

void RemoteControl::sendState (int client)
{
    auto* root = new juce::DynamicObject();
    root->setProperty ("t", "state");
    root->setProperty ("name", nameProvider ? nameProvider() : net::Hub::computerName());
   #ifdef JUCE_APPLICATION_VERSION_STRING
    root->setProperty ("version", juce::String (JUCE_APPLICATION_VERSION_STRING));
   #endif

    juce::Array<juce::var> ins, outs, sceneNames;
    for (int i : shownInputs)
    {
        auto& s = engine.inputs[(size_t) i];
        auto* o = new juce::DynamicObject();
        o->setProperty ("i", i);
        o->setProperty ("id", "in" + juce::String (i + 1));
        o->setProperty ("name", s.name.isNotEmpty() ? s.name : "In " + juce::String (i + 1));
        o->setProperty ("src", s.source.describe());
        ins.add (juce::var (o));
    }
    for (int b : shownBuses)
    {
        auto& bus = engine.buses[(size_t) b];
        auto* o = new juce::DynamicObject();
        o->setProperty ("i", b);
        o->setProperty ("code", busCode (b));
        o->setProperty ("label", busLabel (b));
        o->setProperty ("name", bus.name.isNotEmpty() ? bus.name : busLabel (b));
        o->setProperty ("icon", bus.icon);
        o->setProperty ("hw", isHwBus (b));
        outs.add (juce::var (o));
    }
    for (int k = 0; k < scenes.size(); ++k) sceneNames.add (scenes.get (k).name);
    root->setProperty ("inputs", ins);
    root->setProperty ("buses", outs);
    root->setProperty ("scenes", sceneNames);
    root->setProperty ("scene", scenes.getCurrent());
    root->setProperty ("apk", apkFile().existsAsFile());

    auto* params = new juce::DynamicObject();
    for (auto& e : exposed)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("n", (double) e.p->getNormalised());
        o->setProperty ("v", e.p->toText());
        o->setProperty ("d", (double) e.p->range.convertTo0to1 (e.p->defaultValue));
        o->setProperty ("k", e.p->type == ParamType::Toggle ? "t" : "c");
        params->setProperty (e.p->id, juce::var (o));
    }
    root->setProperty ("params", juce::var (params));
    server.send (client, juce::JSON::toString (juce::var (root), true));
}

void RemoteControl::sendDiffs()
{
    auto* d = new juce::DynamicObject();
    bool any = false;
    for (auto& e : exposed)
    {
        const auto v = e.p->getVersion();
        if (v == e.version) continue;
        e.version = v;
        d->setProperty (e.p->id, paramValue (*e.p));
        any = true;
    }
    if (! any) { delete d; return; }
    auto* root = new juce::DynamicObject();
    root->setProperty ("t", "p");
    root->setProperty ("d", juce::var (d));
    server.send (-1, juce::JSON::toString (juce::var (root), true));
}

void RemoteControl::sendMeters()
{
    if (meterClients.empty()) return;
    std::vector<uint8_t> f;
    f.reserve (1 + 3 * (shownInputs.size() + shownBuses.size()));
    f.push_back (1);
    auto put = [&f] (Meter& m)
    {
        const auto [l, r] = m.takeRemote();
        f.push_back (meterByte (l));
        f.push_back (meterByte (r));
        f.push_back (m.muted.load (std::memory_order_relaxed) ? 1 : 0);
    };
    for (int i : shownInputs) put (engine.inputs[(size_t) i].meter);
    for (int b : shownBuses)  put (engine.buses[(size_t) b].meter);
    for (int c : meterClients) server.sendBinary (c, f.data(), f.size());
}

void RemoteControl::sendStatus()
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("t", "s");
    o->setProperty ("dsp", (double) std::round (load.update (engine) * 1000.0f) / 1000.0);
    o->setProperty ("rate", engine.getSampleRate());
    o->setProperty ("block", engine.getBlockSize());
    o->setProperty ("audio", engine.getMasterDevice() != nullptr);
    server.send (-1, juce::JSON::toString (juce::var (o), true));
}

void RemoteControl::updateMeterUsers()
{
    const bool want = ! meterClients.empty();
    if (want == meterUsed) return;
    meterUsed = want;
    Meter::remoteUsers.fetch_add (want ? 1 : -1);
}

//==============================================================================
void RemoteControl::handleMessage (int client, const juce::String& json)
{
    const auto v = juce::JSON::parse (json);
    const auto t = v.getProperty ("t", {}).toString();
    if (t == "meters")
    {
        if ((bool) v.getProperty ("on", false)) meterClients.insert (client);
        else meterClients.erase (client);
        updateMeterUsers();
        return;
    }
    if (t == "scene")
    {
        const int k = (int) v.getProperty ("i", -1);
        if (juce::isPositiveAndBelow (k, scenes.size())) scenes.recall (k);
        return;
    }
    auto it = byId.find (v.getProperty ("id", {}).toString());
    if (it == byId.end()) return;   // only the exposed parameters
    auto* p = it->second;
    if (t == "set")
    {
        const auto n = v.getProperty ("n", {});
        if (n.isDouble() || n.isInt() || n.isInt64()) p->setNormalised ((float) (double) n, true);
    }
    else if (t == "toggle") p->toggle (true);
    else if (t == "reset")  p->set (p->defaultValue, true);
}

void RemoteControl::timerCallback()
{
    for (auto& e : server.takeEvents())
    {
        switch (e.type)
        {
            case remote::Server::Event::Type::joined:
                if (clients.empty()) { rebuildExposed(); shownLayout = layoutKey(); }
                clients.insert (e.client);
                sendState (e.client);
                logLine ("Hydra Remote: phone / tablet connected (" + juce::String ((int) clients.size()) + ")");
                break;
            case remote::Server::Event::Type::left:
                clients.erase (e.client);
                meterClients.erase (e.client);
                updateMeterUsers();
                break;
            case remote::Server::Event::Type::message:
                if (clients.count (e.client) != 0) handleMessage (e.client, e.text);
                break;
        }
    }

    if (clients.empty())
    {
        if (getTimerInterval() != 250) startTimerHz (4);
        return;
    }
    if (getTimerInterval() != 40) startTimerHz (25);

    ++ticks;
    if (ticks % 5 == 0)
    {
        const auto key = layoutKey();
        if (key != shownLayout)
        {
            shownLayout = key;
            rebuildExposed();
            sendState (-1);
        }
    }
    sendDiffs();
    sendMeters();
    if (ticks % 25 == 0) sendStatus();
}
} // namespace hydra

#pragma once
#include "core/Correction.h"

namespace hydra
{
/** Where a correction is applied: a hardware output of the mixer or an instance of the Room Correction plugin.
    The Correction and Edit pages and pool::Link work through it. Message thread. */
struct CorrectionTarget
{
    virtual ~CorrectionTarget() = default;
    virtual const CorrectionProfile& getProfile() const = 0;
    /** Changes with every new profile. */
    virtual uint32_t getVersion() const = 0;
    virtual void setProfile (CorrectionProfile) = 0;
    virtual bool isOn() const = 0;
    virtual void setOn (bool) = 0;
    /** Sample rate the correction runs at, 0 if unknown. */
    virtual double getSampleRate() const = 0;
    /** Short name in texts: "A1", "this plugin". */
    virtual juce::String label() const = 0;
    virtual juce::Colour colour() const = 0;
    /** A limiter after the correction catches the peaks it lifts above 0 dBFS. */
    virtual bool hasLimiter() const { return false; }
};

/** The shared correction pool: every correction made in the mixer or in the Room Correction plugin, one file each
    (%APPDATA%/HydraMixer/Corrections/<id>.hydracorr, XML with the profile and a revision counter). Both apps read
    and write it; the measurements they are designed from are shared too (the Measurements folder). */
namespace pool
{
inline constexpr const char* kExtension = ".hydracorr";
inline constexpr const char* kMixer = "Hydra Mixer";
inline constexpr const char* kPlugin = "Room Correction plugin";

struct Entry
{
    juce::String id;
    CorrectionProfile profile;   // profile.poolId == id
    int revision = 0;            // +1 with every write
    juce::Time modified;
    juce::String madeIn;         // the app that wrote it last (kMixer / kPlugin)
    juce::File file;
};

/** Library folder (created on demand). */
juce::File folder();
/** Tests: a folder of their own instead of the user's pool (entries are then deleted, not moved to the recycle bin). */
inline juce::File folderOverride;
juce::File fileFor (const juce::String& id);
juce::String newId();
/** All entries, newest first. */
std::vector<Entry> list();
bool read (const juce::File&, Entry& out);
bool read (const juce::String& id, Entry& out);
/** Writes the profile as entry profile.poolId (atomically, revision = the file's + 1). Returns the revision, 0 on failure. */
int write (const CorrectionProfile& profile, const juce::String& madeIn);
/** Moves the entry's file to the recycle bin. Targets linked to it keep their correction, unlinked. */
bool remove (const juce::String& id);
bool rename (const juce::String& id, const juce::String& name, const juce::String& madeIn);
/** Same filters, level, time alignment, name and design (the id is not compared). */
bool same (const CorrectionProfile&, const CorrectionProfile&);

/** Keeps one target linked to its pool entry. Call sync about twice a second on the message thread:
    - a new profile on the target (its version changed) is written to the pool. A profile without an id gets a new one
      (a new correction: designed from another measurement, made by the wizard); an empty one is not linked.
    - a newer revision of the entry (changed in the other app, on another output or in another plugin instance) is put
      on the target.
    - the first time (session or plugin state loaded) and when an entry is chosen, the pool's version wins; a missing
      entry is written back from the target's copy (a project from another computer).
    - an entry deleted from the pool while linked: the target keeps its correction, unlinked (no id) until it is
      changed again (then it is a new entry). */
class Link
{
public:
    explicit Link (juce::String app) : madeIn (std::move (app)) {}
    void sync (CorrectionTarget&);
    const juce::String& linkedId() const noexcept { return id; }

private:
    void pollFile (CorrectionTarget&);
    void stamp (const juce::File&);

    juce::String madeIn, id;
    bool first = true, seenFile = false;
    uint32_t seenVersion = 0, unlinkedVersion = ~0u;
    int seenRevision = 0;
    juce::Time stampTime;
    juce::int64 stampSize = -1;
};
} // namespace pool
} // namespace hydra

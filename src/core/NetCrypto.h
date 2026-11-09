#pragma once
#include "Common.h"

namespace hydra::net
{
/** Encryption of Hydra Link packets with the network password (Windows CNG, no extra library).

    All Hydras that share a password derive the same group key from it (PBKDF2-SHA256). Each sender instance
    (a random 64-bit id, new at every start) encrypts with its own key = HMAC-SHA256 (group key, instance id),
    so two senders can never use the same key with the same nonce. Packets are sealed with AES-256-GCM:
    nonce = the sender's packet counter, the plain header is authenticated (AAD), 16-byte tag at the end.
    A packet with a wrong tag (other password, changed on the way) is rejected; repeated packets are rejected by
    the receiver's replay window (Hub).

    Not thread-safe: one KeyRing per network thread. */
class KeyRing
{
public:
    static constexpr int kTagBytes = 16;

    KeyRing();
    ~KeyRing();

    /** Group key from a password (slow on purpose, ~50 ms). */
    static std::array<uint8_t, 32> deriveGroupKey (const juce::String& password);
    /** Empty password = no encryption. */
    void setGroupKey (const std::array<uint8_t, 32>& key, bool on);
    bool isOn() const noexcept { return on; }

    /** Encrypts data in place and writes the tag. False on failure (send nothing then). */
    bool seal (uint64_t instance, uint64_t counter, const uint8_t* aad, int aadBytes, uint8_t* data, int bytes, uint8_t* tag) noexcept;
    /** Decrypts in place; false if the tag does not match. */
    bool open (uint64_t instance, uint64_t counter, const uint8_t* aad, int aadBytes, uint8_t* data, int bytes, const uint8_t* tag) noexcept;

private:
    void* keyFor (uint64_t instance) noexcept;
    void clearCache() noexcept;

    void* aes = nullptr;    // BCRYPT_ALG_HANDLE (AES, GCM)
    void* hmac = nullptr;   // BCRYPT_ALG_HANDLE (SHA-256, HMAC)
    std::array<uint8_t, 32> group {};
    bool on = false;
    struct Entry { uint64_t instance = 0; void* key = nullptr; uint32_t used = 0; };
    std::vector<Entry> cache;   // keys of the instances seen lately (at most kCache)
    uint32_t useClock = 0;
    static constexpr size_t kCache = 64;
};

/** Cryptographically random numbers. */
uint64_t randomU64() noexcept;

/** Text encrypted for the current Windows user (DPAPI), as base64; empty input gives "". */
juce::String protectForUser (const juce::String& plain);
juce::String unprotectForUser (const juce::String& stored);
} // namespace hydra::net

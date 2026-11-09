#include "NetCrypto.h"

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <dpapi.h>

#pragma comment (lib, "bcrypt.lib")
#pragma comment (lib, "crypt32.lib")

#ifndef NT_SUCCESS
 #define NT_SUCCESS(s) (((NTSTATUS) (s)) >= 0)
#endif

namespace hydra::net
{
namespace
{
BCRYPT_ALG_HANDLE openHmac()
{
    BCRYPT_ALG_HANDLE h = nullptr;
    if (! NT_SUCCESS (BCryptOpenAlgorithmProvider (&h, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG))) return nullptr;
    return h;
}

void makeNonce (uint64_t counter, uint8_t nonce[12]) noexcept
{
    std::memset (nonce, 0, 12);
    for (int i = 0; i < 8; ++i) nonce[4 + i] = (uint8_t) (counter >> (8 * i));
}
} // namespace

KeyRing::KeyRing()
{
    BCRYPT_ALG_HANDLE a = nullptr;
    if (NT_SUCCESS (BCryptOpenAlgorithmProvider (&a, BCRYPT_AES_ALGORITHM, nullptr, 0)))
    {
        if (NT_SUCCESS (BCryptSetProperty (a, BCRYPT_CHAINING_MODE, (PUCHAR) BCRYPT_CHAIN_MODE_GCM, sizeof (BCRYPT_CHAIN_MODE_GCM), 0)))
            aes = a;
        else
            BCryptCloseAlgorithmProvider (a, 0);
    }
    hmac = openHmac();
}

KeyRing::~KeyRing()
{
    clearCache();
    if (aes != nullptr) BCryptCloseAlgorithmProvider ((BCRYPT_ALG_HANDLE) aes, 0);
    if (hmac != nullptr) BCryptCloseAlgorithmProvider ((BCRYPT_ALG_HANDLE) hmac, 0);
    SecureZeroMemory (group.data(), group.size());
}

std::array<uint8_t, 32> KeyRing::deriveGroupKey (const juce::String& password)
{
    std::array<uint8_t, 32> key {};
    if (password.isEmpty()) return key;
    auto h = openHmac();
    if (h == nullptr) return key;
    const auto utf8 = password.toRawUTF8();
    static const char salt[] = "Hydra Link v1 group key";
    // 60 000 rounds: ~50 ms once per password change, but a guessed password costs an attacker as much each.
    BCryptDeriveKeyPBKDF2 (h, (PUCHAR) utf8, (ULONG) std::strlen (utf8), (PUCHAR) salt, (ULONG) (sizeof (salt) - 1),
                           60000, key.data(), (ULONG) key.size(), 0);
    BCryptCloseAlgorithmProvider (h, 0);
    return key;
}

void KeyRing::setGroupKey (const std::array<uint8_t, 32>& key, bool isOn)
{
    clearCache();
    group = key;
    on = isOn && aes != nullptr && hmac != nullptr;
}

void KeyRing::clearCache() noexcept
{
    for (auto& e : cache)
        if (e.key != nullptr) BCryptDestroyKey ((BCRYPT_KEY_HANDLE) e.key);
    cache.clear();
}

void* KeyRing::keyFor (uint64_t instance) noexcept
{
    ++useClock;
    for (auto& e : cache)
        if (e.instance == instance) { e.used = useClock; return e.key; }

    // Key of this instance: HMAC-SHA256 (group key, "hydralink" || instance).
    uint8_t msg[17] = { 'h', 'y', 'd', 'r', 'a', 'l', 'i', 'n', 'k' };
    for (int i = 0; i < 8; ++i) msg[9 + i] = (uint8_t) (instance >> (8 * i));
    uint8_t k[32] {};
    if (! NT_SUCCESS (BCryptHash ((BCRYPT_ALG_HANDLE) hmac, group.data(), (ULONG) group.size(), msg, sizeof (msg), k, sizeof (k))))
        return nullptr;
    BCRYPT_KEY_HANDLE handle = nullptr;
    const bool ok = NT_SUCCESS (BCryptGenerateSymmetricKey ((BCRYPT_ALG_HANDLE) aes, &handle, nullptr, 0, k, sizeof (k), 0));
    SecureZeroMemory (k, sizeof (k));
    if (! ok) return nullptr;

    if (cache.size() >= kCache)
    {
        // Drop the key used longest ago (senders that restarted or random junk).
        auto oldest = std::min_element (cache.begin(), cache.end(), [] (const Entry& a, const Entry& b) { return a.used < b.used; });
        BCryptDestroyKey ((BCRYPT_KEY_HANDLE) oldest->key);
        cache.erase (oldest);
    }
    cache.push_back ({ instance, handle, useClock });
    return handle;
}

bool KeyRing::seal (uint64_t instance, uint64_t counter, const uint8_t* aad, int aadBytes, uint8_t* data, int bytes, uint8_t* tag) noexcept
{
    auto* key = keyFor (instance);
    if (key == nullptr) return false;
    uint8_t nonce[12];
    makeNonce (counter, nonce);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO (info);
    info.pbNonce = nonce;       info.cbNonce = sizeof (nonce);
    info.pbAuthData = (PUCHAR) aad; info.cbAuthData = (ULONG) aadBytes;
    info.pbTag = tag;           info.cbTag = kTagBytes;
    ULONG done = 0;
    return NT_SUCCESS (BCryptEncrypt ((BCRYPT_KEY_HANDLE) key, data, (ULONG) bytes, &info, nullptr, 0, data, (ULONG) bytes, &done, 0))
        && (int) done == bytes;
}

bool KeyRing::open (uint64_t instance, uint64_t counter, const uint8_t* aad, int aadBytes, uint8_t* data, int bytes, const uint8_t* tag) noexcept
{
    auto* key = keyFor (instance);
    if (key == nullptr) return false;
    uint8_t nonce[12];
    makeNonce (counter, nonce);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO (info);
    info.pbNonce = nonce;       info.cbNonce = sizeof (nonce);
    info.pbAuthData = (PUCHAR) aad; info.cbAuthData = (ULONG) aadBytes;
    info.pbTag = (PUCHAR) tag;  info.cbTag = kTagBytes;
    ULONG done = 0;
    return NT_SUCCESS (BCryptDecrypt ((BCRYPT_KEY_HANDLE) key, data, (ULONG) bytes, &info, nullptr, 0, data, (ULONG) bytes, &done, 0))
        && (int) done == bytes;
}

//==============================================================================
uint64_t randomU64() noexcept
{
    uint64_t v = 0;
    if (! NT_SUCCESS (BCryptGenRandom (nullptr, (PUCHAR) &v, sizeof (v), BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        v = (uint64_t) juce::Random::getSystemRandom().nextInt64();
    return v;
}

juce::String protectForUser (const juce::String& plain)
{
    if (plain.isEmpty()) return {};
    const auto utf8 = plain.toRawUTF8();
    DATA_BLOB in { (DWORD) std::strlen (utf8), (BYTE*) utf8 }, out {};
    if (! CryptProtectData (&in, L"Hydra Link", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    const auto text = juce::Base64::toBase64 (out.pbData, out.cbData);
    LocalFree (out.pbData);
    return text;
}

juce::String unprotectForUser (const juce::String& stored)
{
    if (stored.isEmpty()) return {};
    juce::MemoryOutputStream bytes;
    if (! juce::Base64::convertFromBase64 (bytes, stored)) return {};
    DATA_BLOB in { (DWORD) bytes.getDataSize(), (BYTE*) bytes.getData() }, out {};
    if (! CryptUnprotectData (&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    const auto text = juce::String::fromUTF8 ((const char*) out.pbData, (int) out.cbData);
    SecureZeroMemory (out.pbData, out.cbData);
    LocalFree (out.pbData);
    return text;
}
} // namespace hydra::net

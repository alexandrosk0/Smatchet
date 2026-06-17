// Audit H2 — pure decision logic for the POSIX secret-at-rest guard. The chmod/stat plumbing in
// ConfigManager_PathUtils.cpp is POSIX-only and filesystem-bound (untestable in the Windows doctest
// rig), so the loose-vs-tight decision is extracted into the platform-agnostic helper
// IsLooseConfigFileMode and covered here on every platform.

#include "ConfigManager_Internal.h"

#include <doctest/doctest.h>

#include <functional>
#include <string>

using smatchet::config_detail::ApplyConfigSecretProvider;
using smatchet::config_detail::IsLooseConfigFileMode;

TEST_CASE("IsLooseConfigFileMode: owner-only modes are tight") {
    CHECK_FALSE(IsLooseConfigFileMode(0600u)); // rw-------  the target post-chmod state
    CHECK_FALSE(IsLooseConfigFileMode(0400u)); // r--------
    CHECK_FALSE(IsLooseConfigFileMode(0700u)); // rwx------  (exec bits are owner-only too)
    CHECK_FALSE(IsLooseConfigFileMode(0000u)); // ---------  degenerate but not group/world readable
}

TEST_CASE("IsLooseConfigFileMode: any group bit is loose") {
    CHECK(IsLooseConfigFileMode(0640u)); // rw-r-----  group read
    CHECK(IsLooseConfigFileMode(0620u)); // rw--w----  group write
    CHECK(IsLooseConfigFileMode(0610u)); // rw---x---  group exec
}

TEST_CASE("IsLooseConfigFileMode: any world bit is loose") {
    CHECK(IsLooseConfigFileMode(0604u)); // rw----r--  world read
    CHECK(IsLooseConfigFileMode(0602u)); // rw-----w-  world write
    CHECK(IsLooseConfigFileMode(0601u)); // rw------x  world exec
}

TEST_CASE("IsLooseConfigFileMode: the umask-default 0644 is flagged loose") {
    // The exact regression the guard exists for: a config created under the typical 0022 umask
    // lands group/world readable, leaking plaintext API secrets.
    CHECK(IsLooseConfigFileMode(0644u));
    CHECK(IsLooseConfigFileMode(0666u));
}

TEST_CASE("IsLooseConfigFileMode: high file-type bits do not affect the decision") {
    // st_mode carries S_IFREG (0100000) etc. above the permission bits; only the low 0077 matters.
    CHECK_FALSE(IsLooseConfigFileMode(0100600u)); // S_IFREG | rw-------  -> tight
    CHECK(IsLooseConfigFileMode(0100644u));       // S_IFREG | rw-r--r--  -> loose
}

// Audit H2 — Android Keystore secret-at-rest seam (ApplyConfigSecretProvider). The JNI plumbing is
// Android-only, but the routing/fail-safe DECISION is the platform-agnostic helper covered here on
// every platform: empty input, provider-unset fail-closed, provider transform, and fail-safe-empty.

TEST_CASE("ApplyConfigSecretProvider: empty input yields empty regardless of provider") {
    const std::function<std::string(const std::string&)> kNoProvider;
    CHECK(ApplyConfigSecretProvider(kNoProvider, "").empty());
    // A live provider is NOT consulted for empty input — short-circuits before any JNI round-trip.
    bool called = false;
    std::function<std::string(const std::string&)> spy = [&](const std::string& v) {
        called = true;
        return v + "!";
    };
    CHECK(ApplyConfigSecretProvider(spy, "").empty());
    CHECK_FALSE(called);
}

TEST_CASE("ApplyConfigSecretProvider: unset provider FAILS CLOSED (never passes the value through)") {
    // No Keystore bridge wired (pre-Keystore build / host-init failure): the seam must NOT return the
    // raw secret. On protect this drops the secret (never written cleartext); on unprotect it is
    // treated as absent. The loud dropped-secret warning lives at the ProtectSecretForConfig call site.
    const std::function<std::string(const std::string&)> kNoProvider;
    CHECK(ApplyConfigSecretProvider(kNoProvider, "sk-secret-123").empty());
}

TEST_CASE("ApplyConfigSecretProvider: installed provider transforms (round-trip)") {
    // Model the Keystore protect/unprotect pair as a reversible wrap so the round-trip is exact.
    std::function<std::string(const std::string&)> protect = [](const std::string& v) {
        return "ENC(" + v + ")";
    };
    std::function<std::string(const std::string&)> unprotect = [](const std::string& v) {
        // strip the ENC( ... ) wrapper
        if (v.size() >= 5 && v.compare(0, 4, "ENC(") == 0 && v.back() == ')') {
            return v.substr(4, v.size() - 5);
        }
        return std::string(); // not our ciphertext -> fail-safe empty
    };
    const std::string plain = "anthropic-key-xyz";
    const std::string token = ApplyConfigSecretProvider(protect, plain);
    CHECK_EQ(token, "ENC(anthropic-key-xyz)");
    CHECK_EQ(ApplyConfigSecretProvider(unprotect, token), plain);
}

TEST_CASE("ApplyConfigSecretProvider: provider returning empty is propagated (fail-safe)") {
    // The fail-safe contract: a Keystore/JNI failure surfaces as an empty result from the provider,
    // and the seam must propagate that empty (never fall back to returning the raw secret). For
    // protect this means the secret is not persisted; for unprotect it means "treat as no secret".
    std::function<std::string(const std::string&)> failing = [](const std::string&) {
        return std::string();
    };
    CHECK(ApplyConfigSecretProvider(failing, "should-not-leak").empty());
}

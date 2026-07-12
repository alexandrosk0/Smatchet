// ConfigSecretPersist.test.cpp — regression coverage for GitHub Issue #1770 (secret loss on DPAPI
// encryption failure). The Win32 WriteSecretFields arm routes every secret through
// config_detail::ApplySecretPersist / DecideSecretPersist; here we drive those directly with a
// stubbed encrypt result (an empty `encrypted` argument == DPAPI failure) and assert the plaintext
// fallback is persisted rather than dropped. Pure + json-level, so the Windows doctest rig covers
// the exact data-loss path without needing to force a real CryptProtectData failure.

#include "ConfigManager_Internal.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <string>

using smatchet::config_detail::ApplySecretPersist;
using smatchet::config_detail::DecideSecretPersist;
using smatchet::config_detail::SecretPersistAction;

TEST_CASE("DecideSecretPersist: encrypt success stores the ciphertext") {
    CHECK(DecideSecretPersist("my-secret", "BASE64CIPHERTEXT==") == SecretPersistAction::StoreCiphertext);
}

TEST_CASE("DecideSecretPersist: encrypt FAILURE on a real secret keeps plaintext (Issue #1770)") {
    // empty `encrypted` for a non-empty value is the DPAPI-failure signal.
    CHECK(DecideSecretPersist("my-secret", "") == SecretPersistAction::StorePlaintext);
}

TEST_CASE("DecideSecretPersist: no secret clears both keys (not resurrected)") {
    CHECK(DecideSecretPersist("", "") == SecretPersistAction::ClearBoth);
}

TEST_CASE("ApplySecretPersist: DPAPI failure persists the plaintext fallback, no empty ciphertext (Issue #1770)") {
    // Merged on-disk json carries a stale value; the current save must persist the NEW secret.
    nlohmann::json j;
    j["github_pat"] = "ghp_stale_on_disk";
    // stub the encrypt failure: encrypted == "" for a non-empty secret.
    ApplySecretPersist(j, "github_pat", "github_pat_enc", "ghp_new_secret", /*encrypted=*/"");
    REQUIRE(j.contains("github_pat"));
    CHECK(j["github_pat"].get<std::string>() == "ghp_new_secret"); // credential NOT lost
    CHECK_FALSE(j.contains("github_pat_enc"));                     // no empty ciphertext left behind
}

TEST_CASE("ApplySecretPersist: encrypt success writes ciphertext and drops the plaintext copy") {
    nlohmann::json j;
    j["token"] = "PLAINTEXT_TOKEN";
    ApplySecretPersist(j, "token", "token_enc", "the-secret", /*encrypted=*/"BASE64CIPHER==");
    REQUIRE(j.contains("token_enc"));
    CHECK(j["token_enc"].get<std::string>() == "BASE64CIPHER==");
    CHECK_FALSE(j.contains("token")); // plaintext erased on successful encrypt
}

TEST_CASE("ApplySecretPersist: an absent secret erases both keys") {
    nlohmann::json j;
    j["linear_api_key"] = "OLD";
    j["linear_api_key_enc"] = "OLDENC";
    ApplySecretPersist(j, "linear_api_key", "linear_api_key_enc", /*value=*/"", /*encrypted=*/"");
    CHECK_FALSE(j.contains("linear_api_key"));
    CHECK_FALSE(j.contains("linear_api_key_enc"));
}

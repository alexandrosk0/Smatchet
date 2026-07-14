// LuaScriptConsent — pure decision core for the first-run Lua script consent
// gate. Fail-closed: a new or content-changed Scripts/*.lua is Blocked until the
// user approves its exact bytes; the kill-switch (enforced=false) allows all.

#include <doctest/doctest.h>

#include "LuaScriptConsent.h"

#include <string>
#include <vector>

using namespace smatchet::lua_consent;

namespace {
std::vector<LuaScriptApproval> Approved(const std::string& path, const std::string& body) {
    return {{path, FingerprintLuaScript(body)}};
}
} // namespace

TEST_CASE("FingerprintLuaScript is content-sensitive") {
    CHECK(FingerprintLuaScript("print('hi')") == FingerprintLuaScript("print('hi')"));
    CHECK(FingerprintLuaScript("print('hi')") != FingerprintLuaScript("print('HI')"));
}

TEST_CASE("IsLuaScriptApproved requires an exact path + hash match") {
    const auto approvals = Approved("Hooks.lua", "-- v1\n");
    CHECK(IsLuaScriptApproved(approvals, "Hooks.lua", FingerprintLuaScript("-- v1\n")));
    // Same path, different content → not approved (must re-consent on change).
    CHECK_FALSE(IsLuaScriptApproved(approvals, "Hooks.lua", FingerprintLuaScript("-- v2 (tampered)\n")));
    // Different path → not approved.
    CHECK_FALSE(IsLuaScriptApproved(approvals, "Other.lua", FingerprintLuaScript("-- v1\n")));
}

TEST_CASE("DecideLuaConsent blocks an unknown script when enforced") {
    const std::vector<LuaScriptApproval> none;
    CHECK(DecideLuaConsent(true, none, "New.lua", FingerprintLuaScript("evil")) == LuaConsentVerdict::Blocked);
}

TEST_CASE("DecideLuaConsent approves a matching script when enforced") {
    const auto approvals = Approved("Setup.lua", "return {}");
    CHECK(DecideLuaConsent(true, approvals, "Setup.lua", FingerprintLuaScript("return {}")) ==
          LuaConsentVerdict::Approved);
}

TEST_CASE("DecideLuaConsent blocks a changed script even if the path was approved before") {
    const auto approvals = Approved("Setup.lua", "return {}");
    CHECK(DecideLuaConsent(true, approvals, "Setup.lua", FingerprintLuaScript("os.exit()")) ==
          LuaConsentVerdict::Blocked);
}

TEST_CASE("DecideLuaConsent kill-switch (enforced=false) approves everything") {
    const std::vector<LuaScriptApproval> none;
    CHECK(DecideLuaConsent(false, none, "Whatever.lua", FingerprintLuaScript("anything")) ==
          LuaConsentVerdict::Approved);
}

TEST_CASE("WithApproval adds a new fingerprint") {
    std::vector<LuaScriptApproval> approvals;
    approvals = WithApproval(approvals, "A.lua", FingerprintLuaScript("a"));
    REQUIRE(approvals.size() == 1);
    CHECK(IsLuaScriptApproved(approvals, "A.lua", FingerprintLuaScript("a")));
}

TEST_CASE("WithApproval replaces the prior fingerprint for the same path (no duplicates)") {
    std::vector<LuaScriptApproval> approvals;
    approvals = WithApproval(approvals, "A.lua", FingerprintLuaScript("v1"));
    approvals = WithApproval(approvals, "A.lua", FingerprintLuaScript("v2"));
    CHECK(approvals.size() == 1);
    CHECK_FALSE(IsLuaScriptApproved(approvals, "A.lua", FingerprintLuaScript("v1")));
    CHECK(IsLuaScriptApproved(approvals, "A.lua", FingerprintLuaScript("v2")));
}

TEST_CASE("WithoutApproval revokes a single path and leaves others") {
    std::vector<LuaScriptApproval> approvals;
    approvals = WithApproval(approvals, "A.lua", FingerprintLuaScript("a"));
    approvals = WithApproval(approvals, "B.lua", FingerprintLuaScript("b"));
    approvals = WithoutApproval(approvals, "A.lua");
    CHECK(approvals.size() == 1);
    CHECK_FALSE(IsLuaScriptApproved(approvals, "A.lua", FingerprintLuaScript("a")));
    CHECK(IsLuaScriptApproved(approvals, "B.lua", FingerprintLuaScript("b")));
}

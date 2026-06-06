// Pure-logic coverage for smatchet::logging::pure::MaskEmailForLog (#820 PII
// redaction), extracted from the anonymous namespace in SmatchetPreferencesUi.cpp
// into the standalone pure header "EmailMaskForLog.h" so the doctest rig can
// exercise it without that Ui TU's ImGui / cpr / AppController dependency chain.
//
// Contract: keep the first local-part char + the "@domain" tail, masking the
// rest as "***" (e.g. "alice@acme.com" -> "a***@acme.com"). Empty -> "(none)".
// No usable '@' (missing / at position 0 / nothing after it) -> "(redacted)".

#include "EmailMaskForLog.h"

#include <doctest/doctest.h>

#include <string>

using smatchet::logging::pure::MaskEmailForLog;

TEST_CASE("MaskEmailForLog: empty input is fully redacted as (none)") {
    CHECK(MaskEmailForLog(std::string()) == "(none)");
}

TEST_CASE("MaskEmailForLog: normal address keeps first char + domain") {
    CHECK(MaskEmailForLog("alice@acme.com") == "a***@acme.com");
    CHECK(MaskEmailForLog("bob.smith@example.co.uk") == "b***@example.co.uk");
}

TEST_CASE("MaskEmailForLog: single-char local part still masks to one char + ***") {
    // local part is exactly one char; the "***" is fixed-width regardless of
    // the actual hidden length, so a 1-char local part still emits "x***@...".
    CHECK(MaskEmailForLog("x@y.z") == "x***@y.z");
}

TEST_CASE("MaskEmailForLog: malformed values are fully redacted") {
    SUBCASE("no '@' at all") { CHECK(MaskEmailForLog("not-an-email") == "(redacted)"); }
    SUBCASE("'@' at position 0 (empty local part)") { CHECK(MaskEmailForLog("@domain.com") == "(redacted)"); }
    SUBCASE("'@' at the very end (empty domain)") { CHECK(MaskEmailForLog("user@") == "(redacted)"); }
    SUBCASE("'@' is the only character") { CHECK(MaskEmailForLog("@") == "(redacted)"); }
}

TEST_CASE("MaskEmailForLog: only the first '@' delimits local part vs domain") {
    // find('@') returns the FIRST '@'; everything from it onward is the domain
    // tail and is preserved verbatim (including any later '@').
    CHECK(MaskEmailForLog("a@b@c.com") == "a***@b@c.com");
}

TEST_CASE("MaskEmailForLog: domain tail is preserved verbatim, local part never leaks beyond char 0") {
    const std::string masked = MaskEmailForLog("longlocalpart@sub.domain.example");
    CHECK(masked == "l***@sub.domain.example");
    // The masked string must not contain any local-part char past the first.
    CHECK(masked.find("onglocalpart") == std::string::npos);
}

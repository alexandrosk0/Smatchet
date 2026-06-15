// Security backlog 2026-05-17 — defense-in-depth strip of header-smuggling control characters at the
// config PERSIST site. SanitizeConfigStringValue removes CR/LF/NUL from the header-bound string
// fields (API keys, base URLs, MCP auth token) in ConfigManager::Save so a value that round-trips
// through disk (e.g. injected via the MCP `config.set` or Lua-config write paths) can never carry
// the control characters used to splice extra HTTP headers into a later request. The helper is pure
// and platform-agnostic (defined in ConfigManager_PathUtils.cpp, already linked), so the decision is
// covered here on every platform.

#include "ConfigManager_Internal.h"

#include <doctest/doctest.h>

#include <string>

using smatchet::config_detail::SanitizeConfigStringValue;

TEST_CASE("SanitizeConfigStringValue: clean values pass through unchanged") {
    CHECK(SanitizeConfigStringValue("https://api.example.com/v1") == "https://api.example.com/v1");
    CHECK(SanitizeConfigStringValue("sk-abc123XYZ-_.") == "sk-abc123XYZ-_.");
    CHECK(SanitizeConfigStringValue("") == "");
    // Tabs and ordinary spaces are not header-smuggling chars and are preserved verbatim.
    CHECK(SanitizeConfigStringValue("token with space\tand tab") == "token with space\tand tab");
}

TEST_CASE("SanitizeConfigStringValue: strips embedded CR, LF, and NUL") {
    // A classic header-smuggling payload: a base URL with an injected CRLF + extra header.
    CHECK(SanitizeConfigStringValue("https://evil\r\nX-Injected: 1") == "https://evilX-Injected: 1");
    CHECK(SanitizeConfigStringValue("line1\nline2") == "line1line2");
    CHECK(SanitizeConfigStringValue("has\rcarriage") == "hascarriage");

    // Embedded NUL — build the input explicitly so the \0 is not treated as a C-string terminator.
    std::string withNul = "key";
    withNul.push_back('\0');
    withNul += "more";
    const std::string cleaned = SanitizeConfigStringValue(withNul);
    CHECK(cleaned == "keymore");
    CHECK(cleaned.size() == 7u);
}

TEST_CASE("SanitizeConfigStringValue: strips leading, trailing, and runs of control chars") {
    CHECK(SanitizeConfigStringValue("\r\nleading") == "leading");
    CHECK(SanitizeConfigStringValue("trailing\r\n") == "trailing");
    CHECK(SanitizeConfigStringValue("\n\r\n\rmiddle\r\n\nend\r") == "middleend");

    // A value made entirely of control chars sanitizes down to empty — important because the
    // secret-write fallback keys off emptiness to decide whether to drop the plaintext copy.
    CHECK(SanitizeConfigStringValue("\r\n\r\n").empty());

    std::string onlyNul;
    onlyNul.push_back('\0');
    onlyNul.push_back('\0');
    CHECK(SanitizeConfigStringValue(onlyNul).empty());
}

// Security backlog 2026-05-17 — defense-in-depth strip of header-smuggling control characters at the
// config PERSIST site. SanitizeConfigStringValue removes CR/LF/NUL from the header-bound string
// fields (API keys, base URLs, MCP auth token) in ConfigManager::Save so a value persisted to disk
// (via the Preferences UI -> Save path) can never carry the control characters used to splice extra
// HTTP headers into a later request. NB: the config.set / Lua direct-write paths cannot reach these
// fields (absent from the config.set allowlist), so this is parity hardening, not the primary guard
// — the use-site strip in AiAssistantController::BuildClientConfig stays the primary defense. The
// helper is pure and platform-agnostic (defined in ConfigManager_PathUtils.cpp, already linked), so
// the decision is covered here on every platform.

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

TEST_CASE("SanitizeConfigStringValue: strips CR/LF/NUL from the DeepSeek header-bound fields") {
    // Explicit regression for the persist-site parity fix that extended the sanitize to the DeepSeek
    // fields (ai_deepseek_api_key / ai_deepseek_base_url) in ConfigManager::Save, mirroring the
    // AiApiKey / AiAnthropicApiKey / AiBaseUrl / AiOllamaBaseUrl siblings. The Save path routes both
    // DeepSeek values through this same helper, so locking the helper's behaviour on DeepSeek-shaped
    // payloads guards the parity from silently regressing.

    // ai_deepseek_base_url — a CRLF + injected header spliced into the configured endpoint.
    CHECK(SanitizeConfigStringValue("https://api.deepseek.com/v1\r\nX-Injected: 1") ==
          "https://api.deepseek.com/v1X-Injected: 1");

    // ai_deepseek_api_key — an injected CRLF in the bearer secret.
    CHECK(SanitizeConfigStringValue("sk-deepseek-abc123\r\nXYZ") == "sk-deepseek-abc123XYZ");

    // Embedded NUL in the DeepSeek key — built explicitly so \0 is not a C-string terminator.
    std::string keyWithNul = "sk-deepseek-";
    keyWithNul.push_back('\0');
    keyWithNul += "tail";
    const std::string cleanedKey = SanitizeConfigStringValue(keyWithNul);
    CHECK(cleanedKey == "sk-deepseek-tail");
    CHECK(cleanedKey.size() == 16u);

    // A DeepSeek key that is nothing but control chars sanitizes to empty — the WriteSecretFields
    // empty-check now keys off this sanitized value to decide whether to drop the legacy plaintext.
    CHECK(SanitizeConfigStringValue("\r\n\r\n").empty());
}

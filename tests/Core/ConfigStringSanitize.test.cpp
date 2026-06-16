// Security backlog (2026-05-17, extended 2026-06-15) — defense-in-depth strip of header-smuggling
// control characters (CR/LF/NUL) from the header/URL-bound config string fields. Two layers are
// exercised here:
//   1. SanitizeConfigStringValue — the pure per-value strip applied in ConfigManager::Save to the
//      header-bound fields (API keys, base URLs, MCP auth token) before persist.
//   2. SanitizeHeaderBoundConfigKeys — the central pass applied inside ConfigManager::WriteConfigJson
//      (the chokepoint every writer funnels through: Save, the MCP `config.set` command, and the Lua
//      layout writer), which closes the gap where config.set's allowlisted URL keys (domain,
//      plane_url) reached disk via WriteConfigJson WITHOUT Save's per-field sanitize.
// The use-site strip in the tracker / AI clients (SanitizeBaseUrlOrLog / BuildClientConfig) stays the
// primary guard; these are defense-in-depth. Both helpers are pure + platform-agnostic (defined in
// ConfigManager_PathUtils.cpp, already linked), so this doctest covers them on every platform.

#include "ConfigManager_Internal.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <string>

using smatchet::config_detail::SanitizeConfigStringValue;
using smatchet::config_detail::SanitizeHeaderBoundConfigKeys;

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

TEST_CASE("SanitizeHeaderBoundConfigKeys: config.set-style write strips CR/LF/NUL from URL keys") {
    // Models the config.set / Lua direct-write round-trip: RunConfigSet builds a config JSON and
    // hands it to ConfigManager::WriteConfigJson, which now routes it through this central helper
    // before the value reaches disk. domain / plane_url / plane_workspace_slug are config.set-
    // allowlisted and are spliced into outbound tracker request URLs, so a CR/LF/NUL injected via
    // config.set must be stripped.
    nlohmann::json j;
    j["domain"] = "evil.example.com\r\nX-Injected: 1";
    j["plane_url"] = "https://plane.example.com\r\n\r\nGET /admin HTTP/1.1";

    // plane_workspace_slug is concatenated raw into every Plane workspace request path
    // (".../api/v1/workspaces/<slug>/projects/...") with NO use-site normalization, so it is strictly
    // less guarded than the base URL — the chokepoint strip is its only defense.
    j["plane_workspace_slug"] = "my-workspace\r\nX-Injected: 1";

    // Re-confirm an AI base URL stays covered at the chokepoint (already sanitized in Save; the
    // central pass guards it for any direct-write path too). Built with an embedded NUL explicitly
    // so the \0 is not treated as a C-string terminator.
    std::string aiUrlWithNul = "https://api.example.com";
    aiUrlWithNul.push_back('\0');
    aiUrlWithNul += "/v1";
    j["ai_base_url"] = aiUrlWithNul;

    // A key NOT in the header/URL-bound set is left verbatim — the use-site escaping owns it, and the
    // central pass must not over-reach into arbitrary config values (e.g. a JQL string with newlines).
    j["jql"] = "project = FOO\r\nORDER BY created";

    SanitizeHeaderBoundConfigKeys(j);

    CHECK(j["domain"].get<std::string>() == "evil.example.comX-Injected: 1");
    CHECK(j["plane_url"].get<std::string>() == "https://plane.example.comGET /admin HTTP/1.1");
    CHECK(j["plane_workspace_slug"].get<std::string>() == "my-workspaceX-Injected: 1");
    CHECK(j["ai_base_url"].get<std::string>() == "https://api.example.com/v1");
    CHECK(j["jql"].get<std::string>() == "project = FOO\r\nORDER BY created");
}

TEST_CASE("SanitizeHeaderBoundConfigKeys: absent keys not created, non-string/non-object untouched") {
    nlohmann::json j;
    j["mcp_port"] = 8765;   // a non-string scalar under a non-URL key -> ignored
    j["plane_url"] = 42;    // wrong type for a URL key -> left as-is, never stringified
    SanitizeHeaderBoundConfigKeys(j);
    CHECK_FALSE(j.contains("domain"));   // the helper must not insert an absent header/URL key
    CHECK(j["mcp_port"].get<int>() == 8765);
    CHECK(j["plane_url"].get<int>() == 42);

    // Non-object input (e.g. a bare array or scalar) is a no-op, not a crash.
    nlohmann::json arr = nlohmann::json::array({"a", "b"});
    SanitizeHeaderBoundConfigKeys(arr);
    CHECK(arr.size() == 2u);
}

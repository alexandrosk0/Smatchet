// Secret persistence for ConfigManager — the per-platform WriteSecretFields arms, legacy-key
// purge, and the Save/Load secret entry points. Split out of ConfigManager.cpp for the
// god-file-splits partition; behavior-identical body move. Concentrates the DPAPI / Android
// Keystore secret-at-rest surface into one reviewable TU.

#include "ConfigManager.h"
#include "ConfigManager_Internal.h"

#include "Logger.h"
#include "NewIssueInheritDefaults.h"
#include "SmatchetDefaults.h"

// Full nlohmann::json is needed here because we (a) define CommentTemplate's friend serializers
// and (b) construct json values in the view-disk helpers and the per-method bodies below. The
// public header (ConfigManager.h) only pulls <nlohmann/json_fwd.hpp> — every consumer pays the
// 75-LOC fwd-decl parse cost instead of the full 30k-LOC json.hpp.
#include <nlohmann/json.hpp>
// clang-format off
// SMATCHET_DEVIATION(rule=duplication; reason=the shared ConfigManager-TU include block + config_detail using-prologue is grandfathered across the god-file-split siblings (ConfigManager.cpp / _Save / _Load / _Secrets) — a behavior-preserving partition has no shared prologue header to factor into without worse coupling, and the DRY gate doc endorses an exemption over cross-context abstraction; owner=orchestrator; revisit=when a shared ConfigManager TU prologue header is introduced)
// clang-format on

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iterator>
#include <mutex>
#include <string>

using smatchet::config_detail::GetCachedConfigRef;
using smatchet::config_detail::GetCacheMutexRef;
using smatchet::config_detail::GetConfigRmwMutexRef;
using smatchet::config_detail::GetHasCachedConfigRef;
using smatchet::config_detail::SanitizeConfigStringValue;

#if defined(_WIN32) || defined(__ANDROID__)
using smatchet::config_detail::ProtectSecretForConfig;
using smatchet::config_detail::UnprotectSecretFieldFromConfig;
#endif
#if defined(_WIN32)
using smatchet::config_detail::ApplySecretPersist;
#endif

namespace {

// Purge legacy keys carried over from the deleted SMATCHET_WITH_AGENTIC config block.
// Save() merges over existing on-disk JSON via LoadMergedConfigJson(); without explicit
// j.erase() the agentic-era secrets + settings would persist indefinitely.
void PurgeLegacyAgenticKeys(nlohmann::json& j) {
    j.erase("github_pat");
    j.erase("github_pat_enc");
    j.erase("linear_api_key");
    j.erase("linear_api_key_enc");
    j.erase("agentic_poll_enabled");
    j.erase("agentic_poll_interval_sec");
    j.erase("agentic_poll_source");
    j.erase("agentic_poll_query");
    j.erase("handoff_harness_bin_path");
    j.erase("handoff_runner_name");
    j.erase("handoff_clarification_post_to_github");
    j.erase("handoff_auto_create_pr_if_missing");
    j.erase("handoff_pr_base_branch");
    j.erase("handoff_pr_body_template");
    j.erase("handoff_git_bin_path");
    j.erase("handoff_gh_bin_path");
    j.erase("handoff_pr_iteration_budget");
    j.erase("handoff_pr_comment_poll_interval_sec");
    j.erase("handoff_auto_start_on_approve");
    j.erase("coderabbit_react");
    j.erase("ci_react");
}

// WriteSecretFields is defined once per platform — the #if/#elif/#else below compiles exactly one
// definition. Splitting the platform switch across three separate function bodies (rather than one
// function with the switch inside it) keeps each arm under the function-length lint cap; the per-arm
// secret/erase contract is unchanged. See each arm's inline notes.
#if defined(_WIN32)
void WriteSecretFields(nlohmann::json& j, const TrackerConfig& config) {
    // Every secret DPAPI-encrypts, then routes through ApplySecretPersist (Issue #1770): store the
    // ciphertext on success, keep the plaintext fallback if protection FAILS (never drop the only
    // copy of the credential), or clear both keys when there is no secret. This mirrors the Android
    // arm's sealSecret lambda below — one contract, applied uniformly to every field.
    auto writeSecret = [&j](const char* plainKey, const char* encKey, const std::string& value) {
        ApplySecretPersist(j, plainKey, encKey, value, ProtectSecretForConfig(value));
    };
    writeSecret("token", "token_enc", config.ApiToken);
    writeSecret("plane_api_key", "plane_api_key_enc", config.PlaneApiKey);
    writeSecret("github_pat", "github_pat_enc", config.GitHubPat);
    writeSecret("linear_api_key", "linear_api_key_enc", config.LinearApiKey);
    // Bug-report PAT — persisted ONLY when the user opted in (BugReportPersistPat). When opt-out,
    // both keys are erased so no token is ever written to config; when opt-in, the same
    // encrypt-or-plaintext-fallback contract as the fields above.
    j.erase("bugreport_github_pat");
    j.erase("bugreport_github_pat_enc");
    if (config.BugReportPersistPat && !config.BugReportGitHubPat.empty()) {
        writeSecret("bugreport_github_pat", "bugreport_github_pat_enc", config.BugReportGitHubPat);
    }
    // Defense-in-depth (security backlog 2026-05-17): strip CR/LF/NUL from the header-bound secrets
    // (MCP auth token, AI API keys) before DPAPI-encrypting them, so the value can never carry
    // header-smuggling control chars once decrypted. Written only via Preferences UI -> Save; the
    // config.set / Lua direct-write paths cannot reach these fields (absent from the config.set
    // allowlist ConfigSetKeyTable, not funneled through Save) — any future allowlist addition of a
    // header-bound field must also route through this sanitize.
    writeSecret("mcp_auth_token", "mcp_auth_token_enc", SanitizeConfigStringValue(config.McpAuthToken));
    writeSecret("ai_api_key", "ai_api_key_enc", SanitizeConfigStringValue(config.AiApiKey));
    writeSecret("ai_anthropic_api_key", "ai_anthropic_api_key_enc",
                SanitizeConfigStringValue(config.AiAnthropicApiKey));
    writeSecret("ai_deepseek_api_key", "ai_deepseek_api_key_enc", SanitizeConfigStringValue(config.AiDeepSeekApiKey));
#if defined(SMATCHET_WITH_WHISPER)
    writeSecret("whisper_api_key", "whisper_api_key_enc", config.WhisperApiKey);
#endif
}
#elif defined(__ANDROID__)
void WriteSecretFields(nlohmann::json& j, const TrackerConfig& config) {
    // SECURITY (audit H21357#1357): Android seals EVERY secret at rest through the host
    // AndroidKeyStore AES-GCM provider. ProtectSecretForConfig routes to that provider and FAILS
    // CLOSED — it returns empty when no provider is wired or the JNI seal fails. Unlike the Win32
    // DPAPI arm above there is NO plaintext fallback: an Android profile file is not a reliable
    // owner-only boundary the way a chmod-0600 desktop file is, so a secret we cannot seal is
    // DROPPED rather than written cleartext. Every plaintext key is erased unconditionally; only a
    // non-empty Keystore ciphertext is persisted.
    const auto sealSecret = [&j](const char* plainKey, const char* encKey, const std::string& value) {
        j.erase(plainKey);
        const std::string enc = ProtectSecretForConfig(value);
        if (!enc.empty()) {
            j[encKey] = enc;
        } else {
            j.erase(encKey); // empty input, or fail-closed Keystore miss — drop, never cleartext.
        }
    };
    sealSecret("token", "token_enc", config.ApiToken);
    sealSecret("plane_api_key", "plane_api_key_enc", config.PlaneApiKey);
    sealSecret("github_pat", "github_pat_enc", config.GitHubPat);
    sealSecret("linear_api_key", "linear_api_key_enc", config.LinearApiKey);
    // Bug-report PAT — persisted only on opt-in; otherwise both keys stay erased.
    j.erase("bugreport_github_pat");
    j.erase("bugreport_github_pat_enc");
    if (config.BugReportPersistPat && !config.BugReportGitHubPat.empty()) {
        const std::string bugPatEnc = ProtectSecretForConfig(config.BugReportGitHubPat);
        if (!bugPatEnc.empty()) {
            j["bugreport_github_pat_enc"] = bugPatEnc;
        }
    }
    // Header-bound secrets: strip CR/LF/NUL before sealing (mirrors the Win32 arm).
    sealSecret("mcp_auth_token", "mcp_auth_token_enc", SanitizeConfigStringValue(config.McpAuthToken));
    sealSecret("ai_api_key", "ai_api_key_enc", SanitizeConfigStringValue(config.AiApiKey));
    sealSecret("ai_anthropic_api_key", "ai_anthropic_api_key_enc", SanitizeConfigStringValue(config.AiAnthropicApiKey));
    sealSecret("ai_deepseek_api_key", "ai_deepseek_api_key_enc", SanitizeConfigStringValue(config.AiDeepSeekApiKey));
#if defined(SMATCHET_WITH_WHISPER)
    sealSecret("whisper_api_key", "whisper_api_key_enc", config.WhisperApiKey);
#endif
}
#else
void WriteSecretFields(nlohmann::json& j, const TrackerConfig& config) {
    j.erase("token_enc");
    j.erase("plane_api_key_enc");
    j.erase("github_pat_enc");
    j.erase("linear_api_key_enc");
    j.erase("bugreport_github_pat_enc");
    j.erase("mcp_auth_token_enc");
    j.erase("ai_api_key_enc");
    j.erase("ai_anthropic_api_key_enc");
    j.erase("ai_deepseek_api_key_enc");
    j["token"] = config.ApiToken;
    j["plane_api_key"] = config.PlaneApiKey;
    j["github_pat"] = config.GitHubPat;
    j["linear_api_key"] = config.LinearApiKey;
    j.erase("bugreport_github_pat");
    if (config.BugReportPersistPat && !config.BugReportGitHubPat.empty()) {
        j["bugreport_github_pat"] = config.BugReportGitHubPat;
    }
    // Defense-in-depth (security backlog 2026-05-17): strip CR/LF/NUL from the header-bound secrets
    // before they land as plaintext JSON, so the value can never carry header-smuggling control
    // chars (mirrors the DPAPI path above — see its note re: the config.set allowlist).
    j["mcp_auth_token"] = SanitizeConfigStringValue(config.McpAuthToken);
    j["ai_api_key"] = SanitizeConfigStringValue(config.AiApiKey);
    j["ai_anthropic_api_key"] = SanitizeConfigStringValue(config.AiAnthropicApiKey);
    j["ai_deepseek_api_key"] = SanitizeConfigStringValue(config.AiDeepSeekApiKey);
#if defined(SMATCHET_WITH_WHISPER)
    j.erase("whisper_api_key_enc");
    j["whisper_api_key"] = config.WhisperApiKey;
#endif
}
#endif

} // namespace

namespace smatchet {
namespace config_detail {

// Persist-decision for one config secret. `encrypted` is the ProtectSecretForConfig result — empty
// means the DPAPI encrypt FAILED (or `value` was empty, i.e. nothing to protect). Issue #1770: a
// non-empty secret whose encrypt failed must fall back to the plaintext key rather than be dropped.
SecretPersistAction DecideSecretPersist(const std::string& value, const std::string& encrypted) {
    if (!encrypted.empty()) {
        return SecretPersistAction::StoreCiphertext;
    }
    if (value.empty()) {
        return SecretPersistAction::ClearBoth;
    }
    return SecretPersistAction::StorePlaintext; // encrypt failed on a real secret — keep plaintext
}

void ApplySecretPersist(nlohmann::json& j, const char* plainKey, const char* encKey, const std::string& value,
                        const std::string& encrypted) {
    switch (DecideSecretPersist(value, encrypted)) {
    case SecretPersistAction::StoreCiphertext:
        j[encKey] = encrypted; // ciphertext replaces any prior plaintext copy
        j.erase(plainKey);
        break;
    case SecretPersistAction::StorePlaintext:
        j[plainKey] = value; // DPAPI failed — persist the current plaintext, not an empty ciphertext
        j.erase(encKey);
        break;
    case SecretPersistAction::ClearBoth:
        j.erase(encKey);
        j.erase(plainKey);
        break;
    }
}

// Purges legacy keys carried over from LoadMergedConfigJson() and writes the secret fields
// (DPAPI-encrypted on Win32 with a plaintext fallback when protection fails; plaintext on other
// platforms). One source-of-truth for the full secret/erase contract — see the inline notes.
void SaveSecretsAndPurgeLegacy(nlohmann::json& j, const TrackerConfig& config) {
    PurgeLegacyAgenticKeys(j);
    WriteSecretFields(j, config);
}

// All DPAPI-encrypted secret fields with their legacy-plaintext fallback + migration flagging.
void LoadSecretFields(const nlohmann::json& j, TrackerConfig& cfg, SecretMigrationFlags& migrate) {
    (void)migrate;
#if defined(_WIN32)
    cfg.ApiToken = UnprotectSecretFieldFromConfig("token_enc", j.value("token_enc", std::string{}));
    if (cfg.ApiToken.empty()) {
        cfg.ApiToken = j.value("token", std::string{});
    }
    cfg.PlaneApiKey = UnprotectSecretFieldFromConfig("plane_api_key_enc", j.value("plane_api_key_enc", std::string{}));
    if (cfg.PlaneApiKey.empty()) {
        cfg.PlaneApiKey = j.value("plane_api_key", std::string{});
    }
    // GitHub PAT: same DPAPI + legacy-plaintext shape as PlaneApiKey.
    cfg.GitHubPat = UnprotectSecretFieldFromConfig("github_pat_enc", j.value("github_pat_enc", std::string{}));
    if (cfg.GitHubPat.empty()) {
        cfg.GitHubPat = j.value("github_pat", std::string{});
    }
    // Linear API key — same DPAPI + legacy-plaintext shape as GitHubPat.
    cfg.LinearApiKey =
        UnprotectSecretFieldFromConfig("linear_api_key_enc", j.value("linear_api_key_enc", std::string{}));
    if (cfg.LinearApiKey.empty()) {
        cfg.LinearApiKey = j.value("linear_api_key", std::string{});
    }
    // Bug-report PAT — DPAPI + legacy-plaintext, same shape as GitHubPat.
    cfg.BugReportGitHubPat =
        UnprotectSecretFieldFromConfig("bugreport_github_pat_enc", j.value("bugreport_github_pat_enc", std::string{}));
    if (cfg.BugReportGitHubPat.empty()) {
        cfg.BugReportGitHubPat = j.value("bugreport_github_pat", std::string{});
    }
    cfg.McpAuthToken =
        UnprotectSecretFieldFromConfig("mcp_auth_token_enc", j.value("mcp_auth_token_enc", std::string{}));
    if (cfg.McpAuthToken.empty()) {
        cfg.McpAuthToken = j.value("mcp_auth_token", std::string{});
        migrate.McpAuthToken = !cfg.McpAuthToken.empty();
    }
    cfg.AiApiKey = UnprotectSecretFieldFromConfig("ai_api_key_enc", j.value("ai_api_key_enc", std::string{}));
    if (cfg.AiApiKey.empty()) {
        cfg.AiApiKey = j.value("ai_api_key", std::string{});
        migrate.AiApiKey = !cfg.AiApiKey.empty();
    }
    cfg.AiAnthropicApiKey =
        UnprotectSecretFieldFromConfig("ai_anthropic_api_key_enc", j.value("ai_anthropic_api_key_enc", std::string{}));
    if (cfg.AiAnthropicApiKey.empty()) {
        cfg.AiAnthropicApiKey = j.value("ai_anthropic_api_key", std::string{});
        migrate.AiAnthropicApiKey = !cfg.AiAnthropicApiKey.empty();
    }
    cfg.AiDeepSeekApiKey =
        UnprotectSecretFieldFromConfig("ai_deepseek_api_key_enc", j.value("ai_deepseek_api_key_enc", std::string{}));
    if (cfg.AiDeepSeekApiKey.empty()) {
        cfg.AiDeepSeekApiKey = j.value("ai_deepseek_api_key", std::string{});
        migrate.AiDeepSeekApiKey = !cfg.AiDeepSeekApiKey.empty();
    }
#if defined(SMATCHET_WITH_WHISPER)
    cfg.WhisperApiKey =
        UnprotectSecretFieldFromConfig("whisper_api_key_enc", j.value("whisper_api_key_enc", std::string{}));
    if (cfg.WhisperApiKey.empty()) {
        cfg.WhisperApiKey = j.value("whisper_api_key", std::string{});
        migrate.WhisperApiKey = !cfg.WhisperApiKey.empty();
    }
#endif
#elif defined(__ANDROID__)
    // SECURITY (audit H21357#1357): unseal every secret through the host Keystore provider.
    // UnprotectSecretFieldFromConfig -> UnprotectSecretFromConfig FAILS SAFE to empty (no provider, a
    // Keystore/JNI decrypt failure, or ciphertext minted on another device). Plaintext fallback is gated
    // on the SEALED key being ABSENT — not on an empty unseal: a present-but-undecryptable `*_enc`
    // (tamper, key rotation, foreign-device ciphertext) is DROPPED, never downgraded to a sibling
    // plaintext an attacker could have injected. We fall back to legacy plaintext only when no sealed key
    // exists (a pre-Keystore config), flagging a migration so Load() re-Saves — re-sealing via Keystore,
    // or dropping fail-closed if no provider is wired. Either way pre-fix plaintext leaves disk on load.
    const auto unsealSecret = [&j, &migrate](const char* encKey, const char* plainKey) -> std::string {
        const std::string sealed = j.value(encKey, std::string{});
        if (!sealed.empty()) {
            // Sealed key present: trust ONLY a successful unseal. A failed unseal that coexists with a
            // plaintext sibling still flags migration (purge on re-Save) but never surfaces it.
            const std::string value = UnprotectSecretFieldFromConfig(encKey, sealed);
            if (value.empty() && !j.value(plainKey, std::string{}).empty()) {
                migrate.LegacyPlaintext = true; // purge stale plaintext on re-Save; do not trust it.
            }
            return value;
        }
        std::string value = j.value(plainKey, std::string{});
        if (!value.empty()) {
            migrate.LegacyPlaintext = true; // legacy plaintext, no sealed key — re-Save to seal/drop it.
        }
        return value;
    };
    cfg.ApiToken = unsealSecret("token_enc", "token");
    cfg.PlaneApiKey = unsealSecret("plane_api_key_enc", "plane_api_key");
    cfg.GitHubPat = unsealSecret("github_pat_enc", "github_pat");
    cfg.LinearApiKey = unsealSecret("linear_api_key_enc", "linear_api_key");
    cfg.BugReportGitHubPat = unsealSecret("bugreport_github_pat_enc", "bugreport_github_pat");
    cfg.McpAuthToken = unsealSecret("mcp_auth_token_enc", "mcp_auth_token");
    cfg.AiApiKey = unsealSecret("ai_api_key_enc", "ai_api_key");
    cfg.AiAnthropicApiKey = unsealSecret("ai_anthropic_api_key_enc", "ai_anthropic_api_key");
    cfg.AiDeepSeekApiKey = unsealSecret("ai_deepseek_api_key_enc", "ai_deepseek_api_key");
#if defined(SMATCHET_WITH_WHISPER)
    cfg.WhisperApiKey = unsealSecret("whisper_api_key_enc", "whisper_api_key");
#endif
#else
    cfg.ApiToken = j.value("token", std::string{});
    cfg.PlaneApiKey = j.value("plane_api_key", std::string{});
    cfg.GitHubPat = j.value("github_pat", std::string{});
    cfg.LinearApiKey = j.value("linear_api_key", std::string{});
    cfg.BugReportGitHubPat = j.value("bugreport_github_pat", std::string{});
    cfg.McpAuthToken = j.value("mcp_auth_token", std::string{});
    cfg.AiApiKey = j.value("ai_api_key", std::string{});
    cfg.AiAnthropicApiKey = j.value("ai_anthropic_api_key", std::string{});
    cfg.AiDeepSeekApiKey = j.value("ai_deepseek_api_key", std::string{});
#if defined(SMATCHET_WITH_WHISPER)
    cfg.WhisperApiKey = j.value("whisper_api_key", std::string{});
#endif
#endif
}

} // namespace config_detail
} // namespace smatchet

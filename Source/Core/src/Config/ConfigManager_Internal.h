#ifndef SMATCHET_CONFIG_MANAGER_INTERNAL_H
#define SMATCHET_CONFIG_MANAGER_INTERNAL_H

// Implementation-detail header shared by the three ConfigManager TUs
// (`ConfigManager.cpp`, `ConfigManager_PathUtils.cpp`, `ConfigManager_Views.cpp`).
// Not part of the public surface — never include from `Source/Core/include/` headers
// or non-ConfigManager .cpp files.
// All helpers live under `smatchet::config_detail` so the linker symbols stay
// segregated from any other "Utf8ToWide" / "FileExists" that the project may
// grow elsewhere. The defining TU is `ConfigManager_PathUtils.cpp`.

#include <functional>
#include <mutex>
#include <string>

#include "ConfigManager.h" // for TrackerConfig (used by GetCachedConfigRef)

namespace smatchet {
namespace config_detail {

// -------- path / filesystem helpers --------------------------------------------------

#if defined(_WIN32)
std::wstring Utf8ToWide(const std::string& s);
#endif

std::string NormalizeDirectoryPath(const std::string& baseDir);

// Defense-in-depth: pure, side-effect-free strip of every CR (\r), LF (\n), and NUL (\0) from a
// config value. Applied at the config PERSIST site (ConfigManager::Save) to the header-bound string
// fields (API keys, base URLs, MCP auth token) so a value that round-trips through disk — e.g. one
// injected via the MCP `config.set` or Lua-config write paths — can never carry the control
// characters used to smuggle extra HTTP headers when the value is later spliced into a request
// (behind the use-site strip in AiAssistantController::BuildClientConfig).
std::string SanitizeConfigStringValue(const std::string& value);

// Defense-in-depth (security backlog 2026-06-15): apply SanitizeConfigStringValue to every present
// header/URL-bound string key of a config JSON object, in place. Called from
// ConfigManager::WriteConfigJson — the single config-write chokepoint every writer funnels through
// (ConfigManager::Save, the MCP `config.set` command, and the Lua layout writer) — so a control
// character injected via the config.set / Lua direct-write paths (which bypass Save's per-field
// sanitize) can never reach disk. Covered keys: domain, plane_url, plane_workspace_slug, ai_base_url,
// ai_ollama_base_url, ai_deepseek_base_url. Absent keys are left absent (never inserted); non-string values are left
// untouched. Idempotent. The use-site strip in the tracker / AI clients stays the primary guard.
void SanitizeHeaderBoundConfigKeys(nlohmann::json& j);

bool EnsureDirectoryExists(const std::string& path);
void CreateDirectories(const std::string& rawPath);
void EnsureParentDirectoryForFile(const std::string& path);
bool FileExists(const std::string& path);

// -------- secret protection (DPAPI on Windows, passthrough elsewhere) -----------------

std::string ProtectSecretForConfig(const std::string& plainText);
std::string UnprotectSecretFromConfig(const std::string& protectedBase64);

// Persist-decision for one config secret, given its plaintext value and the ProtectSecretForConfig
// result (empty == encrypt FAILED, or there was nothing to protect). Pure so the Windows doctest rig
// covers it directly with no DPAPI stub. Fixes GitHub Issue #1770: a failed DPAPI encrypt must fall
// back to the plaintext key rather than drop the only copy of the credential. StorePlaintext is that
// fallback (non-empty value + empty ciphertext); ClearBoth is the no-secret case.
enum class SecretPersistAction { StoreCiphertext, StorePlaintext, ClearBoth };
SecretPersistAction DecideSecretPersist(const std::string& value, const std::string& encrypted);

// Applies DecideSecretPersist to a config json object for one secret: StoreCiphertext writes encKey +
// erases plainKey; StorePlaintext writes plainKey (the fallback) + erases encKey; ClearBoth erases
// both. The single source of truth the Win32 WriteSecretFields arm routes every secret through.
void ApplySecretPersist(nlohmann::json& j, const char* plainKey, const char* encKey, const std::string& value,
                        const std::string& encrypted);

// Pure seam for Android Keystore-backed secret-at-rest (audit H2). Routes a config secret through an
// optionally-installed host provider (the Keystore JNI bridge the mobile host wires at boot). All
// three arms are covered by the Windows doctest rig (ConfigSecretFilePerms.test.cpp):
//   * empty input            -> empty output (no secret to transform).
//   * provider NOT installed -> empty output (FAIL CLOSED, audit H21357#1357): a secret we cannot
//                               seal is dropped rather than passed through as cleartext, and on
//                               unseal a stored value is treated as absent. The Android
//                               ProtectSecretForConfig call site emits the loud DROPPED warning.
//   * provider installed     -> provider(input). The provider OWNS fail-safe: it returns an empty
//                               string on any Keystore/JNI failure, and that empty result is
//                               propagated as-is so Core never persists / surfaces a secret in
//                               cleartext after a failed encrypt, nor yields a half-decrypted value.
std::string ApplyConfigSecretProvider(const std::function<std::string(const std::string&)>& provider,
                                      const std::string& value);

// Process-wide host-installed Android secret providers (the Keystore JNI bridge). An empty
// std::function means "not installed" — Core falls back to the loud plaintext passthrough. Installed
// once via ConfigManager::SetAndroidSecretProvider from the mobile host at boot, BEFORE the first
// ConfigManager::Load. GetAndroidSecretProviderMutexRef guards the (boot-time) install against a
// background config Save/Load read; the accessors are copied-then-called under it (never invoked
// while the lock is held), so a slow JNI round-trip cannot stall an unrelated config writer.
std::mutex& GetAndroidSecretProviderMutexRef();
std::function<std::string(const std::string&)>& GetAndroidSecretProtectorRef();
std::function<std::string(const std::string&)>& GetAndroidSecretUnprotectorRef();

// Pure decision helper for the POSIX secret-at-rest guard (audit H2): given a POSIX file mode
// (the permission bits from stat::st_mode), returns true if any group/world bit is set, i.e. the
// file is readable by someone other than the owner. Platform-agnostic and side-effect-free so the
// Windows doctest rig can cover it; the actual chmod/stat plumbing is POSIX-only and lives in
// ConfigManager_PathUtils.cpp. Mask is 0077 (group rwx + other rwx).
bool IsLooseConfigFileMode(unsigned int mode);

// Helper used by Load() to surface a single warning per decrypt failure with the field name.
// Cross-platform (audit H21357#1357): the Win32 DPAPI path and the Android Keystore path both
// route field-name-logged unseals through here; on POSIX desktop UnprotectSecretFromConfig is a
// verbatim passthrough so this never warns.
std::string UnprotectSecretFieldFromConfig(const char* fieldName, const std::string& protectedBase64);

// -------- Meyers-singleton process-wide state -----------------------------------------

std::mutex& GetIoMutexRef();
// Serializes the whole config read-modify-write transaction (LoadMergedConfigJson -> modify ->
// WriteConfigJson) across the smatchet_config.json writers (Save, SaveAnnotateAnalysis), closing the
// lost-update window when those run on different threads (e.g. the coalescing config-save worker).
// DISTINCT from GetIoMutexRef (which WriteConfigJson holds for the atomic file write) so the inner
// WriteConfigJson never re-locks this — fixed lock order is RMW (outer) then IO (inner), never reversed.
std::mutex& GetConfigRmwMutexRef();
std::mutex& GetCacheMutexRef();
// Guards the base-directory globals (runtime-asset + user-data dirs) so a background reader
// — e.g. the BackendAuditTrail writer thread re-resolving its path per event, or the config-save
// worker — never observes a torn/realloc'd std::string while a UI/test thread reassigns it via
// SetUserDataDirectory/SetRuntimeAssetDirectory. The Get* accessors return BY VALUE under this
// lock; callers must not hold a reference into the globals across a possible Set.
std::mutex& GetBaseDirMutexRef();
TrackerConfig& GetCachedConfigRef();
bool& GetHasCachedConfigRef();
std::string& GetRuntimeAssetDirectoryRef();
std::string& GetUserDataDirectoryRef();

// -------- god-file-splits (ConfigManager slice): cross-TU seam between the Save/Load/Secrets TUs --
// SecretMigrationFlags is shared by ConfigManager_Secrets.cpp (LoadSecretFields) and
// ConfigManager_Load.cpp (ConfigManager::Load). The four free functions are the table-driven
// scalar pair (defined in ConfigManager.cpp, which owns the field tables) and the secret
// persistence pair (defined in ConfigManager_Secrets.cpp). Bodies moved verbatim.
// Flags raised when a secret is read from a legacy plaintext key (no `*_enc` present /
// undecryptable). Load() re-Saves once when any flag is set so the next on-disk state holds
// the DPAPI-protected form. Mirrors the prior local `migrateLegacyPlaintext*` bools verbatim.
struct SecretMigrationFlags {
    bool McpAuthToken = false;
    bool AiApiKey = false;
    bool AiAnthropicApiKey = false;
    bool AiDeepSeekApiKey = false;
#if defined(SMATCHET_WITH_WHISPER)
    bool WhisperApiKey = false;
#endif
#if defined(__ANDROID__)
    // Android fail-closed migration (audit H21357#1357): set when ANY secret fell back to a legacy
    // plaintext key — including token/plane/github/bugreport, which have no per-field flag. Forces a
    // one-shot re-Save so the plaintext is re-sealed via Keystore, or dropped fail-closed if no
    // provider is wired. The goal is to get pre-fix plaintext OFF disk on first load after upgrade.
    bool LegacyPlaintext = false;
#endif

    bool Any() const {
        bool any = McpAuthToken || AiApiKey || AiAnthropicApiKey || AiDeepSeekApiKey;
#if defined(SMATCHET_WITH_WHISPER)
        any = any || WhisperApiKey;
#endif
#if defined(__ANDROID__)
        any = any || LegacyPlaintext;
#endif
        return any;
    }
};

void SaveScalarFields(nlohmann::json& j, const TrackerConfig& config);
void LoadScalarFields(const nlohmann::json& j, TrackerConfig& cfg);
void SaveSecretsAndPurgeLegacy(nlohmann::json& j, const TrackerConfig& config);
void LoadSecretFields(const nlohmann::json& j, TrackerConfig& cfg, SecretMigrationFlags& migrate);

} // namespace config_detail
} // namespace smatchet

#endif // SMATCHET_CONFIG_MANAGER_INTERNAL_H

#ifndef SMATCHET_CONFIG_MANAGER_INTERNAL_H
#define SMATCHET_CONFIG_MANAGER_INTERNAL_H

// Implementation-detail header shared by the three ConfigManager TUs
// (`ConfigManager.cpp`, `ConfigManager_PathUtils.cpp`, `ConfigManager_Views.cpp`).
// Not part of the public surface — never include from `Source/Core/include/` headers
// or non-ConfigManager .cpp files.
// All helpers live under `smatchet::config_detail` so the linker symbols stay
// segregated from any other "Utf8ToWide" / "FileExists" that the project may
// grow elsewhere. The defining TU is `ConfigManager_PathUtils.cpp`.

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
// (behind the use-site strip in AiAssistantController::BuildClientConfig, PR #176).
std::string SanitizeConfigStringValue(const std::string& value);

// Defense-in-depth (security backlog 2026-06-15): apply SanitizeConfigStringValue to every present
// header/URL-bound string key of a config JSON object, in place. Called from
// ConfigManager::WriteConfigJson — the single config-write chokepoint every writer funnels through
// (ConfigManager::Save, the MCP `config.set` command, and the Lua layout writer) — so a control
// character injected via the config.set / Lua direct-write paths (which bypass Save's per-field
// sanitize) can never reach disk. Covered keys: domain, plane_url, ai_base_url, ai_ollama_base_url,
// ai_deepseek_base_url. Absent keys are left absent (never inserted); non-string values are left
// untouched. Idempotent. The use-site strip in the tracker / AI clients stays the primary guard.
void SanitizeHeaderBoundConfigKeys(nlohmann::json& j);

bool EnsureDirectoryExists(const std::string& path);
void CreateDirectories(const std::string& rawPath);
void EnsureParentDirectoryForFile(const std::string& path);
bool FileExists(const std::string& path);

// -------- cross-process advisory lock around a `<path>.lock` sidecar ------------------

// `handle_` / `fd_` typed as `void*` / `int` so consumers don't need <windows.h>.
// On Windows, `void*` is layout-compatible with `HANDLE` (which is itself a `void*` typedef).
class ScopedFileLock {
  public:
    explicit ScopedFileLock(const std::string& path);
    ~ScopedFileLock();
    ScopedFileLock(const ScopedFileLock&) = delete;
    ScopedFileLock& operator=(const ScopedFileLock&) = delete;

  private:
    void Acquire();
    void Release();

    std::string lockPath_;
#if defined(_WIN32)
    void* handle_;
#else
    int fd_;
#endif
};

// -------- secret protection (DPAPI on Windows, passthrough elsewhere) -----------------

std::string ProtectSecretForConfig(const std::string& plainText);
std::string UnprotectSecretFromConfig(const std::string& protectedBase64);

// Pure decision helper for the POSIX secret-at-rest guard (audit H2): given a POSIX file mode
// (the permission bits from stat::st_mode), returns true if any group/world bit is set, i.e. the
// file is readable by someone other than the owner. Platform-agnostic and side-effect-free so the
// Windows doctest rig can cover it; the actual chmod/stat plumbing is POSIX-only and lives in
// ConfigManager_PathUtils.cpp. Mask is 0077 (group rwx + other rwx).
bool IsLooseConfigFileMode(unsigned int mode);

#if defined(_WIN32)
// Helper used by Load() to surface a single warning per decrypt failure with the field name.
std::string UnprotectSecretFieldFromConfig(const char* fieldName, const std::string& protectedBase64);
#endif

// -------- Meyers-singleton process-wide state -----------------------------------------

std::mutex& GetIoMutexRef();
// Serializes the whole config read-modify-write transaction (LoadMergedConfigJson -> modify ->
// WriteConfigJson) across the smatchet_config.json writers (Save, SaveAnnotateAnalysis), closing the
// lost-update window when those run on different threads (e.g. the coalescing config-save worker).
// DISTINCT from GetIoMutexRef (which WriteConfigJson holds for the atomic file write) so the inner
// WriteConfigJson never re-locks this — fixed lock order is RMW (outer) then IO (inner), never reversed.
std::mutex& GetConfigRmwMutexRef();
std::mutex& GetCacheMutexRef();
TrackerConfig& GetCachedConfigRef();
bool& GetHasCachedConfigRef();
std::string& GetRuntimeAssetDirectoryRef();
std::string& GetUserDataDirectoryRef();

} // namespace config_detail
} // namespace smatchet

#endif // SMATCHET_CONFIG_MANAGER_INTERNAL_H

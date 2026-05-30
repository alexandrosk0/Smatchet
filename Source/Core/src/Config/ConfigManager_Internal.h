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

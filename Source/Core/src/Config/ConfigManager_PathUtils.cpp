// ConfigManager_PathUtils — filesystem / secret / singleton helpers and the path-and-IO
// half of the public `ConfigManager::*` surface (paths, storage preference, JSON read/write,
// ImGui default-layout management, NormalizeUiLanguageCode, atomic file replace).
// Split off `ConfigManager.cpp` per `docs/plans/shipped/large-files-and-phase-2.md` § A3 so the
// remaining file can focus on `Save(TrackerConfig)` / `Load(CliOverrides)` / annotate-analysis
// persistence. The helper namespace is `smatchet::config_detail` — declarations in
// `ConfigManager_Internal.h`.

#include "ConfigManager.h"
#include "ConfigManager_Internal.h"
#include "UiThreadAffinity.h"

#include "Json/BoundedJsonParse.h"
#include "Logger.h"
#include "StringUtil.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace smatchet {
namespace config_detail {

// Platform-specific helpers (Win32 / POSIX) and small string utilities.

#if defined(_WIN32)
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty())
        return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1)
        return std::wstring();
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
#endif

std::string NormalizeDirectoryPath(const std::string& baseDir) {
    if (baseDir.empty()) {
        return std::string();
    }
    std::string normalized = baseDir;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    if (normalized.back() != '/') {
        normalized.push_back('/');
    }
    return normalized;
}

// Defense-in-depth (security backlog 2026-05-17): strip CR/LF/NUL from a header-bound config value
// before it is persisted, so a value that round-trips through disk can never carry the control
// characters used for HTTP header smuggling. Pure + platform-agnostic so the doctest rig can cover
// it on every platform; declared in ConfigManager_Internal.h.
std::string SanitizeConfigStringValue(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c != '\r' && c != '\n' && c != '\0') {
            out.push_back(c);
        }
    }
    return out;
}

void SanitizeHeaderBoundConfigKeys(nlohmann::json& j) {
    if (!j.is_object()) {
        return;
    }
    // The persisted JSON keys whose value is spliced into an outbound HTTP request URL or header.
    // Keep in sync with the URL/header sinks: domain + plane_url -> tracker request URLs
    // (NormalizeBaseUrl in JiraClient / PlaneClient); plane_workspace_slug -> concatenated raw into
    // every Plane workspace request path (".../api/v1/workspaces/<slug>/projects/..." across
    // PlaneClient / PlaneFieldCatalog / PlaneIssueSearch / PlaneIssueMutation / PlaneActivityFeed) with
    // NO use-site normalization — so it is strictly LESS guarded than the base URL and must be stripped
    // here; ai_*_base_url -> AI endpoint URLs. Secret keys are intentionally absent — their on-disk form
    // is DPAPI ciphertext (no CR/LF) or POSIX plaintext already sanitized in Save before encryption.
    static const char* const kHeaderBoundKeys[] = {
        "domain", "plane_url", "plane_workspace_slug", "ai_base_url", "ai_ollama_base_url", "ai_deepseek_base_url",
    };
    for (const char* key : kHeaderBoundKeys) {
        nlohmann::json::iterator it = j.find(key);
        if (it != j.end() && it->is_string()) {
            *it = SanitizeConfigStringValue(it->get<std::string>());
        }
    }
}

bool EnsureDirectoryExists(const std::string& path) {
#if defined(_WIN32)
    // Use the wide-char APIs so non-ASCII paths (é/ñ/CJK) survive on systems where the
    // active code page is not UTF-8.
    const std::wstring wPath = Utf8ToWide(path);
    if (wPath.empty()) {
        return false;
    }
    const DWORD attrs = GetFileAttributesW(wPath.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    if (CreateDirectoryW(wPath.c_str(), nullptr) != 0) {
        return true;
    }
    return GetLastError() == ERROR_ALREADY_EXISTS;
#else
    struct stat st {};
    if (::stat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode) != 0;
    }
    if (::mkdir(path.c_str(), 0755) == 0) {
        return true;
    }
    return errno == EEXIST;
#endif
}

void CreateDirectories(const std::string& rawPath) {
    const std::string normalized = NormalizeDirectoryPath(rawPath);
    if (normalized.empty()) {
        return;
    }

    std::string current;
    std::string::size_type pos = 0;
    if (normalized.size() >= 2 && normalized[1] == ':') {
        current = normalized.substr(0, 2);
        pos = 2;
    }
    if (pos < normalized.size() && normalized[pos] == '/') {
        current.push_back('/');
        ++pos;
    }
    while (pos < normalized.size()) {
        const std::string::size_type next = normalized.find('/', pos);
        const std::string part = normalized.substr(pos, next - pos);
        if (!part.empty()) {
            if (!current.empty() && current.back() != '/') {
                current.push_back('/');
            }
            current += part;
            if (!EnsureDirectoryExists(current)) {
                LOG_WARN("ConfigManager: failed to create directory '%s'", current.c_str());
                return;
            }
        }
        if (next == std::string::npos) {
            break;
        }
        pos = next + 1;
    }
}

void EnsureParentDirectoryForFile(const std::string& path) {
    std::string normalizedPath = path;
    std::replace(normalizedPath.begin(), normalizedPath.end(), '\\', '/');
    const std::string::size_type slash = normalizedPath.find_last_of('/');
    if (slash == std::string::npos) {
        return;
    }
    std::string parent = normalizedPath.substr(0, slash);
    if (parent.empty()) {
        return;
    }
    CreateDirectories(parent);
}

bool FileExists(const std::string& path) {
#if defined(_WIN32)
    const std::wstring wPath = Utf8ToWide(path);
    if (wPath.empty()) {
        return false;
    }
    const DWORD attrs = GetFileAttributesW(wPath.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) != 0;
#endif
}

// ScopedFileLock — cross-process advisory lock on a `<path>.lock` sidecar.
// Public class lives in ConfigManager_Internal.h.

ScopedFileLock::ScopedFileLock(const std::string& path)
    : lockPath_(path + ".lock"),
#if defined(_WIN32)
      handle_(INVALID_HANDLE_VALUE)
#else
      fd_(-1)
#endif
{
    Acquire();
}

ScopedFileLock::~ScopedFileLock() { Release(); }

void ScopedFileLock::Acquire() {
#if defined(_WIN32)
    const std::wstring wLock = Utf8ToWide(lockPath_);
    if (wLock.empty())
        return;
    HANDLE h = CreateFileW(wLock.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        LOG_WARN("ConfigManager: CreateFileW failed for lock '%s' err=%lu — proceeding without exclusive access",
                 lockPath_.c_str(), GetLastError());
        return;
    }
    OVERLAPPED ov{};
    if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &ov)) {
        LOG_WARN("ConfigManager: LockFileEx failed for '%s' err=%lu — proceeding without exclusive access",
                 lockPath_.c_str(), GetLastError());
        CloseHandle(h);
        handle_ = INVALID_HANDLE_VALUE;
        return;
    }
    handle_ = h;
#else
    fd_ = ::open(lockPath_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd_ < 0) {
        LOG_WARN("ConfigManager: open() failed for lock '%s' errno=%d — proceeding without exclusive access",
                 lockPath_.c_str(), errno);
        return;
    }
    if (::flock(fd_, LOCK_EX) != 0) {
        LOG_WARN("ConfigManager: flock(LOCK_EX) failed for '%s' errno=%d — proceeding without exclusive access",
                 lockPath_.c_str(), errno);
        ::close(fd_);
        fd_ = -1;
    }
#endif
}

void ScopedFileLock::Release() {
#if defined(_WIN32)
    if (handle_ != INVALID_HANDLE_VALUE) {
        OVERLAPPED ov{};
        UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &ov);
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
#else
    if (fd_ >= 0) {
        ::flock(fd_, LOCK_UN);
        ::close(fd_);
        fd_ = -1;
    }
#endif
}

// DPAPI-based secret protection (Windows). Other platforms passthrough.

#if defined(_WIN32)
namespace {

std::string BinaryToBase64(const BYTE* data, DWORD dataSize) {
    if (!data || dataSize == 0) {
        return std::string();
    }
    DWORD outLen = 0;
    if (!CryptBinaryToStringA(data, dataSize, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &outLen) ||
        outLen == 0) {
        return std::string();
    }
    std::string out(static_cast<size_t>(outLen), '\0');
    if (!CryptBinaryToStringA(data, dataSize, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &out[0], &outLen)) {
        return std::string();
    }
    // outLen > 0 was checked above, so `out` is non-empty here. Strip the trailing null
    // terminator that CryptBinaryToStringA includes in its byte count.
    if (out.back() == '\0') {
        out.pop_back();
    }
    return out;
}

std::vector<BYTE> Base64ToBinary(const std::string& base64) {
    std::vector<BYTE> out;
    if (base64.empty()) {
        return out;
    }
    DWORD outLen = 0;
    if (!CryptStringToBinaryA(base64.c_str(), static_cast<DWORD>(base64.size()), CRYPT_STRING_BASE64, nullptr, &outLen,
                              nullptr, nullptr)) {
        return out;
    }
    out.resize(static_cast<size_t>(outLen));
    if (!CryptStringToBinaryA(base64.c_str(), static_cast<DWORD>(base64.size()), CRYPT_STRING_BASE64, out.data(),
                              &outLen, nullptr, nullptr)) {
        out.clear();
        return out;
    }
    out.resize(static_cast<size_t>(outLen));
    return out;
}

} // namespace

std::string ProtectSecretForConfig(const std::string& plainText) {
    if (plainText.empty()) {
        return std::string();
    }
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plainText.data()));
    in.cbData = static_cast<DWORD>(plainText.size());
    DATA_BLOB out{};
    constexpr DWORD kFlags = CRYPTPROTECT_UI_FORBIDDEN;
    if (!CryptProtectData(&in, L"SmatchetConfigSecret", nullptr, nullptr, nullptr, kFlags, &out)) {
        LOG_WARN("ConfigManager: CryptProtectData failed; protected config secret value will not be persisted.");
        return std::string();
    }
    std::string encoded = BinaryToBase64(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return encoded;
}

std::string UnprotectSecretFromConfig(const std::string& protectedBase64) {
    if (protectedBase64.empty()) {
        return std::string();
    }
    std::vector<BYTE> cipher = Base64ToBinary(protectedBase64);
    if (cipher.empty()) {
        return std::string();
    }
    DATA_BLOB in{};
    in.pbData = cipher.data();
    in.cbData = static_cast<DWORD>(cipher.size());
    DATA_BLOB out{};
    constexpr DWORD kFlags = CRYPTPROTECT_UI_FORBIDDEN;
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, kFlags, &out)) {
        return std::string();
    }
    std::string plain(reinterpret_cast<const char*>(out.pbData), static_cast<size_t>(out.cbData));
    LocalFree(out.pbData);
    return plain;
}
#else
// SECURITY (audit H2): no DPAPI off Windows. On Windows the CryptProtectData path above binds the
// ciphertext to the user/machine; the two non-Windows families are covered differently:
//   * POSIX desktop (Linux/macOS): no OS-backed encryption — the secret value is returned verbatim
//     (passthrough below) and lands as cleartext JSON. Mitigation: AtomicWriteTextFile opens the
//     temp file O_NOFOLLOW and fchmods it 0600 (owner-only) before the atomic rename, and
//     LoadJsonFile LOG_WARNs on a group/world-readable file. That bounds exposure to the file
//     owner; it is NOT encryption.
//   * Android (audit H2 / CR #1357): secret-at-rest IS sealed — ProtectSecretForConfig /
//     UnprotectSecretFromConfig below route every secret through the host-installed AndroidKeyStore
//     AES-GCM provider, FAIL CLOSED (drop rather than persist cleartext) when no provider is wired.
//     Filesystem perms are not a reliable owner-isolation boundary on Android, so the Keystore seal
//     — not file mode — is the at-rest protection. ConfigManager Write/LoadSecretFields take a
//     dedicated __ANDROID__ arm that persists only the sealed `_enc` keys.
std::string ProtectSecretForConfig(const std::string& plainText) {
#if defined(__ANDROID__)
    // Route through the host-installed Keystore provider when present (audit H2). Copy the
    // std::function out under the lock, then call it UNLOCKED so a slow JNI round-trip never
    // stalls an unrelated config writer holding the same mutex.
    std::function<std::string(const std::string&)> provider;
    {
        std::lock_guard<std::mutex> lk(GetAndroidSecretProviderMutexRef());
        provider = GetAndroidSecretProtectorRef();
    }
    if (!provider && !plainText.empty()) {
        // No Keystore provider wired (host init failed, or a pre-Keystore build). FAIL CLOSED: the
        // secret is dropped rather than persisted as cleartext (ApplyConfigSecretProvider returns
        // empty below). Loud so the dropped secret is diagnosable; the user re-enters it once a
        // Keystore provider is available.
        LOG_WARN("ConfigManager: Android config secret DROPPED — no Keystore provider installed "
                 "(audit H2 fail-closed). Re-enter the secret once Keystore is wired.");
    }
    return ApplyConfigSecretProvider(provider, plainText);
#else
    return plainText;
#endif
}
std::string UnprotectSecretFromConfig(const std::string& protectedBase64) {
#if defined(__ANDROID__)
    std::function<std::string(const std::string&)> provider;
    {
        std::lock_guard<std::mutex> lk(GetAndroidSecretProviderMutexRef());
        provider = GetAndroidSecretUnprotectorRef();
    }
    // Fail-safe to empty (treat as no secret) on either arm: a missing provider (fail closed — see
    // ApplyConfigSecretProvider) or an installed-provider decrypt failure. The latter also covers a
    // legacy plaintext value written before Keystore landed, or ciphertext minted by a different
    // keystore/device — the user re-enters the secret rather than us surfacing garbage.
    return ApplyConfigSecretProvider(provider, protectedBase64);
#else
    return protectedBase64;
#endif
}
#endif

// Field-name-logged wrapper around UnprotectSecretFromConfig, used by Load() to emit a single
// warning per decrypt failure (audit H2 / CR #1357). Cross-platform: on Win32 it wraps the DPAPI
// unprotect, on Android the Keystore unseal, on POSIX desktop a verbatim passthrough (so a
// non-empty input always yields a non-empty result and never warns).
std::string UnprotectSecretFieldFromConfig(const char* fieldName, const std::string& protectedBase64) {
    const std::string plainText = UnprotectSecretFromConfig(protectedBase64);
    if (!protectedBase64.empty() && plainText.empty()) {
        LOG_WARN("ConfigManager: failed to decrypt protected config secret '%s'.", fieldName);
    }
    return plainText;
}

// Pure, platform-agnostic decision helper (audit H2). Declared in ConfigManager_Internal.h so the
// Windows doctest rig can exercise the loose-permission decision without POSIX stat/chmod.
bool IsLooseConfigFileMode(unsigned int mode) {
    // 0077 = group rwx | other rwx. Any of those bits set means the file is reachable by a
    // principal other than the owner — too loose for a secret-bearing config.
    return (mode & 0077u) != 0u;
}

// Pure seam for Android Keystore-backed secret-at-rest (audit H2). Declared in
// ConfigManager_Internal.h so the Windows doctest rig can exercise all three arms without JNI.
std::string ApplyConfigSecretProvider(const std::function<std::string(const std::string&)>& provider,
                                      const std::string& value) {
    if (value.empty()) {
        return std::string(); // protect("") and unprotect("") are both empty — nothing to do.
    }
    if (!provider) {
        // FAIL CLOSED (audit H2): no host Keystore provider wired means we cannot seal a secret at
        // rest. Return empty rather than passing the raw value through — on protect the secret is
        // dropped (never written cleartext to an Android profile whose file perms are not a reliable
        // owner boundary); on unprotect a stored value is treated as absent. The caller re-prompts.
        return std::string();
    }
    return provider(value); // provider owns fail-safe (returns empty on Keystore/JNI failure).
}

// Meyers singletons for cross-call state (process-wide IO + cache mutexes,
// cached config, base directories). Previously private static methods of
// ConfigManager; semantically unchanged.

std::mutex& GetIoMutexRef() {
    static std::mutex s_mutex;
    return s_mutex;
}

std::mutex& GetConfigRmwMutexRef() {
    static std::mutex s_mutex;
    return s_mutex;
}

std::mutex& GetCacheMutexRef() {
    static std::mutex s_mutex;
    return s_mutex;
}

std::mutex& GetBaseDirMutexRef() {
    static std::mutex s_mutex;
    return s_mutex;
}

TrackerConfig& GetCachedConfigRef() {
    static TrackerConfig s_config;
    return s_config;
}

bool& GetHasCachedConfigRef() {
    static bool s_has = false;
    return s_has;
}

std::string& GetRuntimeAssetDirectoryRef() {
    static std::string s;
    return s;
}

std::string& GetUserDataDirectoryRef() {
    static std::string s;
    return s;
}

// Host-injected override for the platform shared-data dir. Lets a host (Android)
// supply a resolved path Core can't compute itself (no <android/...> headers in
// Core) without an #ifdef branch — empty means "fall through to the OS resolver".
std::string& GetPlatformSharedOverrideRef() {
    static std::string s;
    return s;
}

// Host-installed Android Keystore secret providers (audit H2). See ConfigManager_Internal.h for the
// install contract (set once at boot, before the first Load). An unset target means not installed.
std::mutex& GetAndroidSecretProviderMutexRef() {
    static std::mutex s_mutex;
    return s_mutex;
}
std::function<std::string(const std::string&)>& GetAndroidSecretProtectorRef() {
    static std::function<std::string(const std::string&)> s_protector;
    return s_protector;
}
std::function<std::string(const std::string&)>& GetAndroidSecretUnprotectorRef() {
    static std::function<std::string(const std::string&)> s_unprotector;
    return s_unprotector;
}

} // namespace config_detail
} // namespace smatchet

// ConfigManager — path / IO / preference / ImGui-settings public methods.
// Save(TrackerConfig) / Load(CliOverrides) / annotate-analysis live in
// ConfigManager.cpp; Views statics in ConfigManager_Views.cpp.

using smatchet::config_detail::EnsureParentDirectoryForFile;
using smatchet::config_detail::FileExists;
using smatchet::config_detail::GetAndroidSecretProtectorRef;
using smatchet::config_detail::GetAndroidSecretProviderMutexRef;
using smatchet::config_detail::GetAndroidSecretUnprotectorRef;
using smatchet::config_detail::GetBaseDirMutexRef;
using smatchet::config_detail::GetCacheMutexRef;
using smatchet::config_detail::GetHasCachedConfigRef;
using smatchet::config_detail::GetIoMutexRef;
using smatchet::config_detail::GetPlatformSharedOverrideRef;
using smatchet::config_detail::GetRuntimeAssetDirectoryRef;
using smatchet::config_detail::GetUserDataDirectoryRef;
using smatchet::config_detail::NormalizeDirectoryPath;
using smatchet::config_detail::ScopedFileLock;

void ConfigManager::SetBaseDirectoryForFiles(const std::string& baseDir) {
    const std::string normalized = NormalizeDirectoryPath(baseDir);
    std::lock_guard<std::mutex> lk(GetBaseDirMutexRef());
    GetRuntimeAssetDirectoryRef() = normalized;
    GetUserDataDirectoryRef() = normalized;
}

void ConfigManager::SetRuntimeAssetDirectory(const std::string& baseDir) {
    const std::string normalized = NormalizeDirectoryPath(baseDir);
    std::lock_guard<std::mutex> lk(GetBaseDirMutexRef());
    GetRuntimeAssetDirectoryRef() = normalized;
}

void ConfigManager::SetUserDataDirectory(const std::string& baseDir) {
    const std::string normalized = NormalizeDirectoryPath(baseDir);
    std::lock_guard<std::mutex> lk(GetBaseDirMutexRef());
    GetUserDataDirectoryRef() = normalized;
}

void ConfigManager::SetPlatformSharedUserDataDirectoryOverride(const std::string& dir) {
    GetPlatformSharedOverrideRef() = dir.empty() ? std::string() : NormalizeDirectoryPath(dir);
}

void ConfigManager::SetAndroidSecretProvider(std::function<std::string(const std::string&)> protector,
                                             std::function<std::string(const std::string&)> unprotector) {
    // Installed once from the mobile host at boot, before the first Load. The lock orders the
    // install against a background config Save/Load that reads the providers (copy-then-call).
    std::lock_guard<std::mutex> lk(GetAndroidSecretProviderMutexRef());
    GetAndroidSecretProtectorRef() = std::move(protector);
    GetAndroidSecretUnprotectorRef() = std::move(unprotector);
}

// NOTE: these return BY VALUE (a snapshot copy taken under GetBaseDirMutexRef), NOT a reference
// into the globals. Returning a reference would let a caller read the std::string buffer after the
// lock is dropped, racing a concurrent SetUserDataDirectory that reassigns/reallocs it (the
// BackendAuditTrail writer thread re-resolves its path per event; TSan flagged exactly this). The
// copy is cheap relative to the I/O these paths gate. GetFilesBaseDirectory calls the public getter
// (not under the lock), so there is no recursive lock.
std::string ConfigManager::GetFilesBaseDirectory() { return GetUserDataDirectory(); }

std::string ConfigManager::GetRuntimeAssetDirectory() {
    std::lock_guard<std::mutex> lk(GetBaseDirMutexRef());
    const std::string& runtimeDir = GetRuntimeAssetDirectoryRef();
    if (!runtimeDir.empty()) {
        return runtimeDir;
    }
    return GetUserDataDirectoryRef();
}

std::string ConfigManager::GetUserDataDirectory() {
    std::lock_guard<std::mutex> lk(GetBaseDirMutexRef());
    const std::string& userDataDir = GetUserDataDirectoryRef();
    if (!userDataDir.empty()) {
        return userDataDir;
    }
    return GetRuntimeAssetDirectoryRef();
}

std::string ConfigManager::GetDefaultSettingsPath() {
    const std::string& base = GetRuntimeAssetDirectory();
    if (base.empty())
        return "default_settings.json";
    return base + "default_settings.json";
}

std::string ConfigManager::GetStoragePreferenceFlagPath(const std::string& runtimeAssetDir) {
    const std::string normalized = NormalizeDirectoryPath(runtimeAssetDir);
    if (normalized.empty()) {
        return "smatchet_storage_mode.txt";
    }
    return normalized + "smatchet_storage_mode.txt";
}

bool ConfigManager::HasExplicitStoragePreference(const std::string& runtimeAssetDir) {
    return FileExists(GetStoragePreferenceFlagPath(runtimeAssetDir));
}

ConfigManager::StoragePreference ConfigManager::GetStoragePreference(const std::string& runtimeAssetDir,
                                                                     StoragePreference defaultIfMissing) {
    const std::string path = GetStoragePreferenceFlagPath(runtimeAssetDir);
    if (!FileExists(path)) {
        return defaultIfMissing;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return defaultIfMissing;
    }
    // Line-based parser: skip blank and `#`-prefixed comment lines, lowercase the first
    // bare token of the first content line, match against the two valid keywords.
    std::string line;
    while (std::getline(file, line)) {
        std::string::size_type start = 0;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t' || line[start] == '\r')) {
            ++start;
        }
        if (start == line.size() || line[start] == '#') {
            continue;
        }
        std::string token;
        while (start < line.size() && line[start] != ' ' && line[start] != '\t' && line[start] != '\r' &&
               line[start] != '\n' && line[start] != '#') {
            token.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(line[start]))));
            ++start;
        }
        if (token == "portable") {
            return StoragePreference::Portable;
        }
        if (token == "shared") {
            return StoragePreference::Shared;
        }
        break;
    }
    return defaultIfMissing;
}

bool ConfigManager::SetStoragePreference(const std::string& runtimeAssetDir, StoragePreference pref,
                                         std::string& outError) {
    outError.clear();
    const std::string path = GetStoragePreferenceFlagPath(runtimeAssetDir);
    EnsureParentDirectoryForFile(path);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        outError = "Could not write storage-mode marker file: " + path;
        return false;
    }
    file << (pref == StoragePreference::Portable ? "portable" : "shared") << '\n'
         << "# Smatchet storage-mode marker. Authoritative across launches. Change via\n"
         << "# Preferences -> Local data, or by editing the first non-comment line.\n";
    file.flush();
    if (file.fail()) {
        outError = "Marker write failed (flush): " + path;
        return false;
    }
    return true;
}

std::string ConfigManager::GetPlatformSharedUserDataDirectory() {
    const std::string& overrideDir = GetPlatformSharedOverrideRef();
    if (!overrideDir.empty()) {
        return overrideDir;
    }
#if defined(_WIN32)
    char buf[MAX_PATH] = {};
    DWORD n = ::GetEnvironmentVariableA("LOCALAPPDATA", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        return NormalizeDirectoryPath(std::string(buf) + "\\Smatchet");
    }
    n = ::GetEnvironmentVariableA("APPDATA", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        return NormalizeDirectoryPath(std::string(buf) + "\\Smatchet");
    }
    return std::string();
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME")) {
        return NormalizeDirectoryPath(std::string(home) + "/Library/Application Support/Smatchet");
    }
    return std::string();
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
        return NormalizeDirectoryPath(std::string(xdg) + "/Smatchet");
    }
    if (const char* home = std::getenv("HOME")) {
        return NormalizeDirectoryPath(std::string(home) + "/.config/Smatchet");
    }
    return std::string();
#endif
}

namespace {

// Latched user-config parse-failure notice (file name only); guarded by GetIoMutexRef().
std::string& PendingStartupConfigWarningRef() {
    static std::string warning;
    return warning;
}

} // namespace

std::string ConfigManager::TakeStartupConfigWarning() {
    std::lock_guard<std::mutex> lock(GetIoMutexRef());
    std::string out;
    out.swap(PendingStartupConfigWarningRef());
    return out;
}

nlohmann::json ConfigManager::LoadJsonFile(const std::string& path) {
    if (!FileExists(path)) {
        return nlohmann::json::object();
    }
    std::lock_guard<std::mutex> lock(GetIoMutexRef());
    ScopedFileLock fileLock(path);
    std::string raw;
#if defined(_WIN32)
    // Win32 read avoids MinGW/libstdc++ ifstream/stringstream issues seen in release at startup.
    {
        const std::wstring wPath = smatchet::config_detail::Utf8ToWide(path);
        if (!wPath.empty()) {
            HANDLE h = CreateFileW(wPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER li{};
                if (GetFileSizeEx(h, &li) && li.QuadPart > 0 &&
                    li.QuadPart <= static_cast<LONGLONG>(64 * 1024 * 1024)) {
                    const size_t n = static_cast<size_t>(li.QuadPart);
                    raw.resize(n);
                    size_t off = 0;
                    while (off < n) {
                        const size_t room = n - off;
                        const DWORD toRead = room > static_cast<size_t>(1u << 20) ? static_cast<DWORD>(1u << 20)
                                                                                  : static_cast<DWORD>(room);
                        DWORD rd = 0;
                        if (!ReadFile(h, &raw[off], toRead, &rd, nullptr) || rd == 0) {
                            raw.clear();
                            break;
                        }
                        off += static_cast<size_t>(rd);
                    }
                    if (off != n) {
                        raw.clear();
                    }
                }
                CloseHandle(h);
            }
        }
    }
#else
    // SECURITY (audit H2): on POSIX, verify the secret-bearing user config is not group/world
    // readable. AtomicWriteTextFile lands new writes at 0600, but a config created by an older build
    // (pre-H2) or relaxed by hand can still be loose — surface it loudly on read so the user can
    // re-tighten. Scoped to the user config path only (the read-only default-settings file carries
    // no secrets, so a warning there would just be noise).
    if (path == GetConfigPath()) {
        struct stat st;
        if (::stat(path.c_str(), &st) == 0 &&
            smatchet::config_detail::IsLooseConfigFileMode(static_cast<unsigned int>(st.st_mode))) {
            LOG_WARN("ConfigManager: '%s' is group/world-readable (mode %#o) — it holds API secrets in "
                     "plaintext; run 'chmod 600' on it (audit H2)",
                     path.c_str(), static_cast<unsigned int>(st.st_mode) & 0777u);
        }
    }
    {
        std::ifstream file(path, std::ios::binary);
        if (file.is_open()) {
            file.seekg(0, std::ios::end);
            const std::streamoff sz = file.tellg();
            file.seekg(0, std::ios::beg);
            // Cap the read at 64 MiB to match the Win32 sibling: bound memory so a
            // pathologically large file can't balloon the heap before the bounded parse.
            if (sz > 0 && sz <= static_cast<std::streamoff>(64 * 1024 * 1024)) {
                std::ostringstream ss;
                ss << file.rdbuf();
                raw = ss.str();
            }
        }
    }
#endif
    nlohmann::json j = nlohmann::json::object();
    if (!raw.empty()) {
        // Bounded parse: the config file is owner-only, but a deeply-nested document
        // would otherwise stack-overflow the recursive ~json teardown (Pillar 3).
        // ParseBounded never throws — it signals failure via a non-empty errOut.
        std::string parseErr;
        // Match the 64 MiB read cap above: a config in the 4–64 MiB window is
        // legitimately large (e.g. many recents/boards), so raise the parse byte
        // bound to the same ceiling instead of ParseBounded's 4 MiB default.
        j = smatchet::json_safe::ParseBounded(raw, parseErr, 64u * 1024u * 1024u);
        if (!parseErr.empty()) {
            LOG_ERROR("ConfigManager: failed to parse config '%s': %s", path.c_str(), parseErr.c_str());
            if (path == GetConfigPath()) {
                // User config specifically (this loader also reads default-settings and locale
                // files): latch the one-shot startup warning so the UI can tell the user their
                // settings silently reverted to defaults. Already under GetIoMutexRef().
                PendingStartupConfigWarningRef() = FileNameOfPath(path);
            }
            j = nlohmann::json::object();
        }
    }
    if (!j.is_object()) {
        j = nlohmann::json::object();
    }
    return j;
}

nlohmann::json ConfigManager::LoadMergedConfigJson() {
    nlohmann::json jDefault = LoadJsonFile(GetDefaultSettingsPath());
    nlohmann::json jUser = LoadJsonFile(GetConfigPath());
    if (jDefault.is_object() && jUser.is_object()) {
        jDefault.update(jUser);
        return jDefault;
    } else if (jUser.is_object()) {
        return jUser;
    } else {
        return jDefault;
    }
}

std::string ConfigManager::NormalizeUiLanguageCode(const std::string& code) {
    std::string s;
    s.reserve(code.size());
    for (char c : code) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    if (s == "fr" || s == "fr-fr" || s == "fr_fr") {
        return "fr-FR";
    }
    return "en-US";
}

void ConfigManager::WriteConfigJson(const nlohmann::json& j) {
    // Pillar-2 gate (close-gate-gaps Slice 1a): the central config-write chokepoint — every
    // ConfigManager::Save* funnels here. Blocking (lock + ScopedFileLock + atomic disk write),
    // so it must never run on the UI render thread (#565/#732). Warn-only for now; hardens to a
    // debug assert once Slice 1b clears the live violators.
    UiThreadAffinity::WarnIfOnUiThread("ConfigManager::WriteConfigJson");
    const std::string path = GetConfigPath();
    std::lock_guard<std::mutex> lock(GetIoMutexRef());
    ScopedFileLock fileLock(path);
    // Defense-in-depth (security backlog 2026-06-15): strip CR/LF/NUL from the header/URL-bound keys
    // at this central chokepoint so the `config.set` / Lua direct-write paths (which bypass Save's
    // per-field sanitize) cannot persist an HTTP-header/URL-smuggling payload. Copy-then-sanitize
    // leaves the caller's json untouched; the copy is cheap on this infrequent (non-per-frame) path.
    nlohmann::json sanitized = j;
    smatchet::config_detail::SanitizeHeaderBoundConfigKeys(sanitized);
    const std::string content = sanitized.dump(4);
    if (!AtomicWriteTextFile(path, content)) {
        LOG_ERROR("ConfigManager: atomic write failed for '%s'", path.c_str());
    }
}

void ConfigManager::InvalidateCache() {
    std::lock_guard<std::mutex> lock(GetCacheMutexRef());
    GetHasCachedConfigRef() = false;
}

std::string ConfigManager::GetConfigPath() {
    const std::string& base = GetUserDataDirectory();
    if (base.empty())
        return "smatchet_config.json";
    return base + "smatchet_config.json";
}

std::string ConfigManager::GetViewsPath() {
    const std::string& base = GetUserDataDirectory();
    if (base.empty())
        return "smatchet_views.json";
    return base + "smatchet_views.json";
}

std::string ConfigManager::GetImGuiSettingsPath() {
    const std::string& base = GetUserDataDirectory();
    if (base.empty())
        return "imgui.ini";
    return base + "imgui.ini";
}

std::string ConfigManager::GetMobileImGuiSettingsPath() {
    // Sibling of imgui.ini for the mobile content-dockspace layout. GetUserDataDirectory()
    // already resolves to the host-injected filesDir on Android, so the same base applies.
    const std::string& base = GetUserDataDirectory();
    if (base.empty())
        return "imgui_mobile.ini";
    return base + "imgui_mobile.ini";
}

bool ConfigManager::WriteDefaultImGuiSettingsFile() {
    const std::string path = GetImGuiSettingsPath();
    std::lock_guard<std::mutex> lock(GetIoMutexRef());
    ScopedFileLock fileLock(path);
    EnsureParentDirectoryForFile(path);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        LOG_WARN("ConfigManager: could not write default ImGui layout '%s'.", path.c_str());
        return false;
    }
    file << GetDefaultImGuiDockLayoutIni();
    return file.good();
}

void ConfigManager::EnsureDefaultImGuiSettingsFile() {
    const std::string path = GetImGuiSettingsPath();
    {
        std::lock_guard<std::mutex> lock(GetIoMutexRef());
        ScopedFileLock fileLock(path);
        std::ifstream file(path, std::ios::binary);
        if (file.good()) {
            return;
        }
    }
    WriteDefaultImGuiSettingsFile();
}

bool ConfigManager::AtomicWriteTextFile(const std::string& path, const std::string& content) {
    const std::string tmp = path + ".tmp";
    EnsureParentDirectoryForFile(path);
#if !defined(_WIN32)
    // SECURITY (audit H2 follow-up): on POSIX open the temp file through a raw fd with
    // O_NOFOLLOW + O_CREAT|O_TRUNC at mode 0600, then fchmod the fd before writing a byte.
    // This hardens the secret-bearing config write against two gaps the prior ofstream +
    // path-based chmod left open:
    //   - symlink redirect: O_NOFOLLOW makes open() fail (ELOOP) when '<path>.tmp' is a
    //     symlink, so a planted link can't divert the write — or its 0600 — onto another file.
    //   - 0644 window / chmod TOCTOU: fchmod on the fd pins owner-only perms before the write
    //     and the atomic rename, with no reliance on umask and no path-based chmod race.
    // Failure to write is fatal (drop the tmp, keep the prior config); a perms/fsync miss is
    // logged non-fatally so a hardening hiccup never loses the user's config write.
    {
        const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
        if (fd < 0) {
            LOG_ERROR("ConfigManager: failed to open temp file for write '%s' errno=%d", tmp.c_str(), errno);
            return false;
        }
        // O_CREAT's mode is masked by umask and only applies on creation; fchmod pins 0600
        // unconditionally (also covers a pre-existing .tmp left by an earlier interrupted write).
        if (::fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
            LOG_WARN("ConfigManager: fchmod 0600 on '%s' failed errno=%d; config may be group/world-readable",
                     tmp.c_str(), errno);
        }
        const char* data = content.data();
        size_t remaining = content.size();
        bool writeOk = true;
        while (remaining > 0) {
            const ssize_t n = ::write(fd, data, remaining);
            if (n < 0) {
                if (errno == EINTR) {
                    continue; // interrupted before any byte moved — retry the same chunk.
                }
                LOG_ERROR("ConfigManager: failed to write temp file '%s' errno=%d", tmp.c_str(), errno);
                writeOk = false;
                break;
            }
            data += n;
            remaining -= static_cast<size_t>(n);
        }
        if (writeOk && ::fsync(fd) != 0) {
            LOG_WARN("ConfigManager: fsync on '%s' failed errno=%d", tmp.c_str(), errno);
        }
        const bool closeOk = (::close(fd) == 0);
        if (!writeOk || !closeOk) {
            if (writeOk && !closeOk) {
                LOG_ERROR("ConfigManager: close failed on temp file '%s' errno=%d", tmp.c_str(), errno);
            }
            std::remove(tmp.c_str());
            return false;
        }
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        LOG_ERROR("ConfigManager: rename failed '%s' -> '%s' errno=%d", tmp.c_str(), path.c_str(), errno);
        std::remove(tmp.c_str());
        return false;
    }
    // Durability: fsync the parent directory so the rename's new dirent survives a crash / power
    // loss. fsync(fd) above persists the file's data+metadata, but the directory entry that now
    // points at it needs its own sync to be durable. Best-effort — a dir-sync miss is logged, not
    // fatal, and never undoes a successful publish (the rename itself already happened atomically).
    {
        const std::string::size_type slash = path.find_last_of('/');
        const std::string dir = (slash == std::string::npos) ? std::string(".")
                                : (slash == 0)               ? std::string("/")
                                                             : path.substr(0, slash);
        const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
        if (dfd >= 0) {
            if (::fsync(dfd) != 0) {
                LOG_WARN("ConfigManager: fsync on parent dir '%s' failed errno=%d", dir.c_str(), errno);
            }
            ::close(dfd);
        } else {
            LOG_WARN("ConfigManager: open parent dir '%s' for fsync failed errno=%d", dir.c_str(), errno);
        }
    }
    return true;
#else
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            LOG_ERROR("ConfigManager: failed to open temp file for write '%s'", tmp.c_str());
            return false;
        }
        if (!content.empty()) {
            file.write(content.data(), static_cast<std::streamsize>(content.size()));
        }
        file.flush();
        if (!file.good()) {
            LOG_ERROR("ConfigManager: failed to write temp file '%s'", tmp.c_str());
            file.close();
            std::remove(tmp.c_str());
            return false;
        }
    }
    const std::wstring wSrc = smatchet::config_detail::Utf8ToWide(tmp);
    const std::wstring wDst = smatchet::config_detail::Utf8ToWide(path);
    if (wSrc.empty() || wDst.empty()) {
        std::remove(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(wSrc.c_str(), wDst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        LOG_ERROR("ConfigManager: MoveFileEx failed '%s' -> '%s' err=%lu", tmp.c_str(), path.c_str(),
                  static_cast<unsigned long>(GetLastError()));
        DeleteFileW(wSrc.c_str());
        return false;
    }
    return true;
#endif
}

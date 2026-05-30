// ConfigManager_PathUtils — filesystem / secret / singleton helpers and the path-and-IO
// half of the public `ConfigManager::*` surface (paths, storage preference, JSON read/write,
// ImGui default-layout management, NormalizeUiLanguageCode, atomic file replace).
//
// Split off `ConfigManager.cpp` per `docs/plans/shipped/large-files-and-phase-2.md` § A3 so the
// remaining file can focus on `Save(TrackerConfig)` / `Load(CliOverrides)` / annotate-analysis
// persistence. The helper namespace is `smatchet::config_detail` — declarations in
// `ConfigManager_Internal.h`.

#include "ConfigManager.h"
#include "ConfigManager_Internal.h"

#include "Logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

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

// ---------------------------------------------------------------------------
// Platform-specific helpers (Win32 / POSIX) and small string utilities.
// ---------------------------------------------------------------------------

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
    struct stat st{};
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
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) != 0;
#endif
}

// ---------------------------------------------------------------------------
// ScopedFileLock — cross-process advisory lock on a `<path>.lock` sidecar.
// Public class lives in ConfigManager_Internal.h.
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// DPAPI-based secret protection (Windows). Other platforms passthrough.
// ---------------------------------------------------------------------------

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

std::string UnprotectSecretFieldFromConfig(const char* fieldName, const std::string& protectedBase64) {
    const std::string plainText = UnprotectSecretFromConfig(protectedBase64);
    if (!protectedBase64.empty() && plainText.empty()) {
        LOG_WARN("ConfigManager: failed to decrypt protected config secret '%s'.", fieldName);
    }
    return plainText;
}
#else
std::string ProtectSecretForConfig(const std::string& plainText) { return plainText; }
std::string UnprotectSecretFromConfig(const std::string& protectedBase64) { return protectedBase64; }
#endif

// ---------------------------------------------------------------------------
// Meyers singletons for cross-call state (process-wide IO + cache mutexes,
// cached config, base directories). Previously private static methods of
// ConfigManager; semantically unchanged.
// ---------------------------------------------------------------------------

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

} // namespace config_detail
} // namespace smatchet

// ===========================================================================
// ConfigManager — path / IO / preference / ImGui-settings public methods.
// Save(TrackerConfig) / Load(CliOverrides) / annotate-analysis live in
// ConfigManager.cpp; Views statics in ConfigManager_Views.cpp.
// ===========================================================================

using smatchet::config_detail::EnsureParentDirectoryForFile;
using smatchet::config_detail::FileExists;
using smatchet::config_detail::GetCacheMutexRef;
using smatchet::config_detail::GetHasCachedConfigRef;
using smatchet::config_detail::GetIoMutexRef;
using smatchet::config_detail::GetRuntimeAssetDirectoryRef;
using smatchet::config_detail::GetUserDataDirectoryRef;
using smatchet::config_detail::NormalizeDirectoryPath;
using smatchet::config_detail::ScopedFileLock;

void ConfigManager::SetBaseDirectoryForFiles(const std::string& baseDir) {
    const std::string normalized = NormalizeDirectoryPath(baseDir);
    GetRuntimeAssetDirectoryRef() = normalized;
    GetUserDataDirectoryRef() = normalized;
}

void ConfigManager::SetRuntimeAssetDirectory(const std::string& baseDir) {
    GetRuntimeAssetDirectoryRef() = NormalizeDirectoryPath(baseDir);
}

void ConfigManager::SetUserDataDirectory(const std::string& baseDir) {
    GetUserDataDirectoryRef() = NormalizeDirectoryPath(baseDir);
}

const std::string& ConfigManager::GetFilesBaseDirectory() { return GetUserDataDirectory(); }

const std::string& ConfigManager::GetRuntimeAssetDirectory() {
    const std::string& runtimeDir = GetRuntimeAssetDirectoryRef();
    if (!runtimeDir.empty()) {
        return runtimeDir;
    }
    return GetUserDataDirectoryRef();
}

const std::string& ConfigManager::GetUserDataDirectory() {
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
    {
        std::ifstream file(path, std::ios::binary);
        if (file.is_open()) {
            std::ostringstream ss;
            ss << file.rdbuf();
            raw = ss.str();
        }
    }
#endif
    nlohmann::json j = nlohmann::json::object();
    if (!raw.empty()) {
        try {
            j = nlohmann::json::parse(raw);
        } catch (const std::exception& ex) {
            LOG_ERROR("ConfigManager: failed to parse config '%s': %s", path.c_str(), ex.what());
            j = nlohmann::json::object();
        } catch (...) {
            LOG_ERROR("ConfigManager: failed to parse config '%s' with unknown exception", path.c_str());
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
    const std::string path = GetConfigPath();
    std::lock_guard<std::mutex> lock(GetIoMutexRef());
    ScopedFileLock fileLock(path);
    const std::string content = j.dump(4);
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
#if defined(_WIN32)
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
#else
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        LOG_ERROR("ConfigManager: rename failed '%s' -> '%s' errno=%d", tmp.c_str(), path.c_str(), errno);
        std::remove(tmp.c_str());
        return false;
    }
    return true;
#endif
}

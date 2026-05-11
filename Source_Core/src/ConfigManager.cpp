// ConfigManager — implementation of the SmatchetConfig persistence layer.
//
// Header previously held every byte of this code inline (1696 lines including a full Win32
// CreateFileW read path, DPAPI CryptProtectData helpers, ScopedFileLock with LockFileEx,
// AtomicWriteTextFile, NormalizeDirectoryPath / FileExists / CreateDirectories, the embedded
// ImGui default dock-layout ini, and the full 270-line `Load()`). Every consumer of any
// `TrackerConfig` field paid the parse cost on each #include. This file is the body half of
// the split; the public surface stays in `Source_Core/include/ConfigManager.h`.
//
// Anonymous-namespace helpers below are unchanged in behavior. The class methods take their
// declarations from the slim header and define their bodies here.

#include "ConfigManager.h"

#include "Logger.h"
#include "NewIssueInheritDefaults.h"
#include "SmatchetDefaults.h"

// Full nlohmann::json is needed here because we (a) define CommentTemplate's friend serializers
// and (b) construct json values in the view-disk helpers and the per-method bodies below. The
// public header (ConfigManager.h) only pulls <nlohmann/json_fwd.hpp> — every consumer pays the
// 75-LOC fwd-decl parse cost instead of the full 30k-LOC json.hpp.
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
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

namespace {

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
    for (char& c : normalized) {
        if (c == '\\') {
            c = '/';
        }
    }
    if (normalized.back() != '/') {
        normalized.push_back('/');
    }
    return normalized;
}

bool EnsureDirectoryExists(const std::string& path) {
#if defined(_WIN32)
    const DWORD attrs = GetFileAttributesA(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    if (CreateDirectoryA(path.c_str(), nullptr) != 0) {
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
    for (char& c : normalizedPath) {
        if (c == '\\') {
            c = '/';
        }
    }
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

// Cross-process advisory lock on a sibling <path>.lock file (was nested in ConfigManager).
class ScopedFileLock {
  public:
    explicit ScopedFileLock(const std::string& path) : lockPath_(path + ".lock") { Acquire(); }
    ~ScopedFileLock() { Release(); }
    ScopedFileLock(const ScopedFileLock&) = delete;
    ScopedFileLock& operator=(const ScopedFileLock&) = delete;

  private:
    void Acquire() {
#if defined(_WIN32)
        const std::wstring wLock = Utf8ToWide(lockPath_);
        if (wLock.empty())
            return;
        handle_ = CreateFileW(wLock.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE)
            return;
        OVERLAPPED ov{};
        if (!LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &ov)) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
#else
        fd_ = ::open(lockPath_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd_ < 0)
            return;
        if (::flock(fd_, LOCK_EX) != 0) {
            ::close(fd_);
            fd_ = -1;
        }
#endif
    }
    void Release() {
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

    std::string lockPath_;
#if defined(_WIN32)
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

// ---------------------------------------------------------------------------
// DPAPI-based secret protection (Windows). Other platforms passthrough.
// ---------------------------------------------------------------------------

#if defined(_WIN32)
std::string BinaryToBase64(const BYTE* data, DWORD dataSize) {
    if (!data || dataSize == 0) {
        return std::string();
    }
    DWORD outLen = 0;
    if (!CryptBinaryToStringA(data, dataSize, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &outLen)) {
        return std::string();
    }
    std::string out(static_cast<size_t>(outLen), '\0');
    if (!CryptBinaryToStringA(data, dataSize, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &out[0], &outLen)) {
        return std::string();
    }
    if (!out.empty() && out.back() == '\0') {
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
    if (!CryptStringToBinaryA(base64.c_str(), static_cast<DWORD>(base64.size()), CRYPT_STRING_BASE64, nullptr,
                              &outLen, nullptr, nullptr)) {
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
std::string UnprotectSecretFromConfig(const std::string& protectedValue) { return protectedValue; }
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

// ---------------------------------------------------------------------------
// Embedded default ImGui dock-layout ini. Used by WriteDefaultImGuiSettingsFile.
// ---------------------------------------------------------------------------

constexpr const char* kDefaultImGuiDockLayoutIni =
    "[Window][WindowOverViewport_11111111]\n"
    "Pos=0,22\n"
    "Size=1920,987\n"
    "Collapsed=0\n"
    "\n"
    "[Window][Debug##Default]\n"
    "Pos=60,60\n"
    "Size=400,400\n"
    "Collapsed=0\n"
    "\n"
    "[Window][Preferences]\n"
    "Pos=0,559\n"
    "Size=1920,450\n"
    "Collapsed=0\n"
    "DockId=0x0000000A,0\n"
    "\n"
    "[Window][Smatchet - Active Project]\n"
    "Pos=0,22\n"
    "Size=1611,535\n"
    "Collapsed=0\n"
    "DockId=0x00000002,0\n"
    "\n"
    "[Window][Views - Jira]\n"
    "Pos=926,22\n"
    "Size=354,698\n"
    "Collapsed=0\n"
    "DockId=0x00000008,0\n"
    "\n"
    "[Window][Performance]\n"
    "Pos=0,427\n"
    "Size=924,293\n"
    "Collapsed=0\n"
    "DockId=0x00000007,0\n"
    "\n"
    "[Window][SmatchetViewsDashboard]\n"
    "Pos=1613,22\n"
    "Size=307,535\n"
    "Collapsed=0\n"
    "DockId=0x00000004,0\n"
    "\n"
    "[Window][Log]\n"
    "Pos=0,559\n"
    "Size=1920,450\n"
    "Collapsed=0\n"
    "DockId=0x0000000A,1\n"
    "\n"
    "[Window][Backend Audit]\n"
    "Pos=0,559\n"
    "Size=1920,450\n"
    "Collapsed=0\n"
    "DockId=0x0000000A,2\n"
    "\n"
    "[Window][Scripting]\n"
    "Pos=0,559\n"
    "Size=1920,450\n"
    "Collapsed=0\n"
    "DockId=0x0000000A,3\n"
    "\n"
    "[Window][MCP Server]\n"
    "Pos=0,559\n"
    "Size=1920,450\n"
    "Collapsed=0\n"
    "DockId=0x0000000A,4\n"
    "\n"
    "[Window][Bulk import tickets]\n"
    "Pos=0,559\n"
    "Size=1920,450\n"
    "Collapsed=0\n"
    "DockId=0x0000000A,6\n"
    "\n"
    "[Window][Bulk export tickets]\n"
    "Pos=0,559\n"
    "Size=1920,450\n"
    "Collapsed=0\n"
    "DockId=0x0000000A,5\n"
    "\n"
    "[Table][0x518B645F,7]\n"
    "Column 0  Weight=1.0000\n"
    "Column 1  Weight=1.0000\n"
    "Column 2  Weight=1.0000 Sort=0^\n"
    "Column 3  Weight=1.0000\n"
    "Column 4  Weight=1.0000\n"
    "Column 5  Weight=1.0000\n"
    "Column 6  Weight=1.0000\n"
    "\n"
    "[Table][0x4BC636F5,8]\n"
    "RefScale=16\n"
    "\n"
    "[Table][0x70C366DB,5]\n"
    "RefScale=16\n"
    "Column 0  Width=48\n"
    "Column 1  Width=180\n"
    "Column 2  Weight=1.0000\n"
    "Column 3  Width=120\n"
    "Column 4  Width=220\n"
    "\n"
    "[Docking][Data]\n"
    "DockSpace         ID=0x08BD597D Window=0x1BBC0F80 Pos=0,22 Size=1920,987 Split=Y Selected=0x51577D15\n"
    "  DockNode        ID=0x00000009 Parent=0x08BD597D SizeRef=1920,535 Split=X\n"
    "    DockNode      ID=0x00000001 Parent=0x00000009 SizeRef=1611,987 Split=X\n"
    "      DockNode    ID=0x00000007 Parent=0x00000001 SizeRef=924,698 Split=X\n"
    "        DockNode  ID=0x00000002 Parent=0x00000007 SizeRef=1028,698 CentralNode=1 Selected=0x7EBEC904\n"
    "        DockNode  ID=0x00000003 Parent=0x00000007 SizeRef=250,698 Selected=0x51577D15\n"
    "      DockNode    ID=0x00000008 Parent=0x00000001 SizeRef=354,698 Selected=0x51577D15\n"
    "    DockNode      ID=0x00000004 Parent=0x00000009 SizeRef=307,987 Selected=0x74648FC6\n"
    "  DockNode        ID=0x0000000A Parent=0x08BD597D SizeRef=1920,450 Selected=0x6A4695A4\n";

// ---------------------------------------------------------------------------
// View-disk JSON helpers (previously the public `SmatchetViewsDiskDetail::*` namespace inline in
// ConfigManager.h). They had exactly one caller (this file) but every TU that included the header
// re-parsed them; moving them to anonymous namespace here drops that parse cost without changing
// behaviour.
// ---------------------------------------------------------------------------

ViewDefinition ParseViewDefinition(const nlohmann::json& viewJson) {
    ViewDefinition view;
    view.Id = viewJson.value("id", std::string());
    view.Name = viewJson.value("name", std::string());
    view.Jql = viewJson.value("jql", view.Jql);
    if (viewJson.contains("fields") && viewJson["fields"].is_array()) {
        for (const auto& field : viewJson["fields"]) {
            if (field.is_string()) {
                view.Fields.push_back(field.get<std::string>());
            }
        }
    }
    if (viewJson.contains("column_order") && viewJson["column_order"].is_array()) {
        for (const auto& col : viewJson["column_order"]) {
            if (col.is_string()) {
                view.ColumnOrder.push_back(col.get<std::string>());
            }
        }
    }
    if (viewJson.contains("column_widths") && viewJson["column_widths"].is_object()) {
        for (auto it = viewJson["column_widths"].begin(); it != viewJson["column_widths"].end(); ++it) {
            if (it.value().is_number()) {
                view.ColumnWidths[it.key()] = it.value().get<float>();
            }
        }
    }
    if (viewJson.contains("sort_specs") && viewJson["sort_specs"].is_array()) {
        for (const auto& specJson : viewJson["sort_specs"]) {
            if (specJson.is_object() && specJson.contains("column") && specJson["column"].is_string()) {
                ViewSortSpec spec;
                spec.ColumnKey = specJson["column"].get<std::string>();
                spec.Direction = specJson.value("direction", 0);
                if (spec.Direction != 0) {
                    view.SortSpecs.push_back(spec);
                }
            }
        }
    }
    if (view.Id.empty()) {
        view.Id = view.Name;
    }
    if (view.Name.empty()) {
        view.Name = view.Id.empty() ? std::string("View") : view.Id;
    }
    return view;
}

ViewWorkspaceState ParseWorkspaceObject(const nlohmann::json& root) {
    ViewWorkspaceState ws;
    ws.ActiveViewId = root.value("active_view_id", std::string());
    if (root.contains("views") && root["views"].is_array()) {
        for (const auto& viewJson : root["views"]) {
            if (!viewJson.is_object()) {
                continue;
            }
            ws.Views.push_back(ParseViewDefinition(viewJson));
        }
    }
    if (ws.ActiveViewId.empty() && !ws.Views.empty()) {
        ws.ActiveViewId = ws.Views.front().Id;
    }
    return ws;
}

nlohmann::json SerializeView(const ViewDefinition& view) {
    nlohmann::json viewJson;
    viewJson["id"] = view.Id;
    viewJson["name"] = view.Name;
    viewJson["jql"] = view.Jql;
    viewJson["fields"] = view.Fields;
    viewJson["column_order"] = view.ColumnOrder;
    viewJson["column_widths"] = nlohmann::json::object();
    for (const auto& kv : view.ColumnWidths) {
        viewJson["column_widths"][kv.first] = kv.second;
    }
    viewJson["sort_specs"] = nlohmann::json::array();
    for (const auto& spec : view.SortSpecs) {
        if (spec.Direction != 0) {
            viewJson["sort_specs"].push_back(nlohmann::json{{"column", spec.ColumnKey}, {"direction", spec.Direction}});
        }
    }
    return viewJson;
}

nlohmann::json SerializeWorkspace(const ViewWorkspaceState& ws) {
    nlohmann::json j = nlohmann::json::object();
    j["active_view_id"] = ws.ActiveViewId;
    j["views"] = nlohmann::json::array();
    for (const auto& view : ws.Views) {
        j["views"].push_back(SerializeView(view));
    }
    return j;
}

ViewWorkspaceState MakeDefaultViewWorkspaceForBackend(const std::string& backendKey, const TrackerConfig& cfg) {
    ViewWorkspaceState ws;
    if (backendKey == "Plane") {
        ViewDefinition v;
        v.Id = "plane_default_view";
        v.Name = "Default Plane View";
        v.Jql = "";
        v.Fields = {"summary", "status", "priority", "assignee", "labels", "created", "updated"};
        v.ColumnOrder = {"id"};
        for (const auto& fieldId : v.Fields) {
            v.ColumnOrder.push_back("field:" + fieldId);
        }
        v.ColumnWidths["id"] = 90.0f;
        ws.ActiveViewId = v.Id;
        ws.Views.push_back(std::move(v));
        return ws;
    }

    ViewDefinition defaultView;
    defaultView.Id = "default_view";
    defaultView.Name = "Default View";
    defaultView.Jql = cfg.JqlQuery.empty() ? std::string("assignee=currentUser()") : cfg.JqlQuery;
    defaultView.Fields = {"summary", "assignee", "priority", "status", "created", "updated"};
    defaultView.ColumnOrder = {"id"};
    for (const auto& fieldId : defaultView.Fields) {
        defaultView.ColumnOrder.push_back("field:" + fieldId);
    }
    defaultView.ColumnWidths["id"] = 90.0f;
    ws.ActiveViewId = defaultView.Id;
    ws.Views.push_back(std::move(defaultView));
    return ws;
}

ViewsStore ViewWorkspaceToViewsStoreImpl(const ViewWorkspaceState& ws) {
    ViewsStore s;
    s.Version = 2;
    s.ActiveViewId = ws.ActiveViewId;
    s.Views = ws.Views;
    return s;
}

void ViewsStoreToViewWorkspaceImpl(const ViewsStore& slice, ViewWorkspaceState& ws) {
    ws.ActiveViewId = slice.ActiveViewId;
    ws.Views = slice.Views;
}

} // namespace

// ---------------------------------------------------------------------------
// CommentTemplate friend serializers — declared in ConfigManager.h, defined here so the slim
// header pulls only <nlohmann/json_fwd.hpp>. Lives in the global namespace because that's where
// the friend declaration injects them; nlohmann::adl_serializer finds them via ADL at call sites.
// ---------------------------------------------------------------------------

void to_json(nlohmann::json& j, const CommentTemplate& t) {
    j = nlohmann::json{{"id", t.Id}, {"title", t.Title}, {"text", t.Text}};
}

void from_json(const nlohmann::json& j, CommentTemplate& t) {
    t.Id = j.value("id", "");
    t.Title = j.value("title", "");
    t.Text = j.value("text", "");
}

// ===========================================================================
// ConfigManager — public static methods.
// ===========================================================================

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
        const std::wstring wPath = Utf8ToWide(path);
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

void ConfigManager::Save(const TrackerConfig& config) {
    {
        std::lock_guard<std::mutex> lock(GetCacheMutexRef());
        GetHasCachedConfigRef() = false;
    }
    nlohmann::json j = LoadMergedConfigJson();
    j["domain"] = config.Domain;
    j["email"] = config.Email;
    j["project_key"] = config.ProjectKey;
    j["tracker_type"] = config.TrackerType;
    j["plane_url"] = config.PlaneUrl;
    j["plane_workspace_slug"] = config.PlaneWorkspaceSlug;
    j["plane_project_id"] = config.PlaneProjectId;
    j["jql"] = config.JqlQuery;
    j["field_overflow_tooltips"] = config.EnableFieldOverflowTooltips;
    j["read_only_mode"] = config.ReadOnlyMode;
    j["grid_end_wheel_swallows_before_horizontal"] = config.GridEndWheelSwallowsBeforeHorizontal;
    j["show_preferences_window"] = config.ShowPreferencesWindow;
    j["show_views_dashboard_window"] = config.ShowViewsDashboardWindow;
    j["show_performance_window"] = config.ShowPerformanceWindow;
    j["show_log_window"] = config.ShowLogWindow;
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    j["show_lua_automation_window"] = config.ShowLuaAutomationWindow;
#endif
    j["log_min_level"] = config.LogMinLevel;
    j["log_tracker_http_bodies"] = config.LogTrackerHttpBodies;
    j["log_p4_io"] = config.LogP4Io;
    j.erase("ai_model");
    j.erase("ai_base_url");
    j.erase("show_ai_assistant_window");
    j.erase("ai_api_key");
    j.erase("ai_api_key_enc");
    j["mcp_enabled"] = config.McpEnabled;
    j["mcp_port"] = config.McpPort;
    j["mcp_allow_remote"] = config.McpAllowRemote;
    j["mcp_allow_lua_execution"] = config.McpAllowLuaExecution;
    j["mcp_export_fields"] = config.McpExportFields;
    j["show_mcp_server_window"] = config.ShowMcpServerWindow;
    j["quick_comment_templates"] = config.QuickCommentTemplates;
    j["blame_comment_templates"] = config.BlameCommentTemplates;
    j["duration_suggestions"] = config.DurationSuggestions;
    j["worklog_comment_templates"] = config.WorkLogCommentTemplates;
    j["date_format_option"] = config.DateFormatOption;
    j["date_compact_relative_threshold_days"] = config.DateCompactRelativeThresholdDays;
    j["view_field_picker_height"] = config.ViewFieldPickerHeight;
    j["selected_font_name"] = config.SelectedFontName;
    j["ui_language"] = NormalizeUiLanguageCode(config.UiLanguage);
    j["update_check_enabled"] = config.UpdateCheckEnabled;
    j["update_include_prerelease"] = config.UpdateIncludePrerelease;
    j["update_skip_version"] = config.UpdateSkipVersion;
    j["update_github_repo"] = config.UpdateGithubRepo;
    j.erase("mcp_server_window_layout_valid");
    j.erase("mcp_server_window_x");
    j.erase("mcp_server_window_y");
    j.erase("mcp_server_window_w");
    j.erase("mcp_server_window_h");
    j["mcp_server_info_panel_height_px"] = config.McpServerInfoPanelHeightPx;
    j["mcp_server_activity_panel_height_px"] = config.McpServerActivityPanelHeightPx;
    j["blame_allow_custom_commands"] = config.BlameAllowCustomCommands;
    j["default_issue_type_id"] = config.DefaultIssueTypeId;
    j["default_issue_type_name"] = config.DefaultIssueTypeName;
    j["import_max_concurrent"] = config.ImportMaxConcurrent;
    j["last_import_directory"] = config.LastImportDirectory;
    j["last_export_directory"] = config.LastExportDirectory;
    {
        nlohmann::json inheritIds = nlohmann::json::array();
        std::copy_if(config.NewIssueInheritFieldIds.begin(), config.NewIssueInheritFieldIds.end(),
                     std::back_inserter(inheritIds), [](const std::string& id) { return id != "summary"; });
        j["new_issue_inherit_field_ids"] = std::move(inheritIds);
    }
    {
        nlohmann::json inheritIds = nlohmann::json::array();
        std::copy_if(config.NewIssueInheritFieldIdsPlane.begin(), config.NewIssueInheritFieldIdsPlane.end(),
                     std::back_inserter(inheritIds), [](const std::string& id) { return id != "summary"; });
        j["new_issue_inherit_field_ids_plane"] = std::move(inheritIds);
    }
#if defined(_WIN32)
    j.erase("token");
    j.erase("plane_api_key");
    j["token_enc"] = ProtectSecretForConfig(config.ApiToken);
    j["plane_api_key_enc"] = ProtectSecretForConfig(config.PlaneApiKey);
    const std::string mcpAuthTokenEnc = ProtectSecretForConfig(config.McpAuthToken);
    // New field migration: keep the legacy plaintext fallback if DPAPI fails instead of dropping the only copy.
    if (config.McpAuthToken.empty() || !mcpAuthTokenEnc.empty()) {
        j.erase("mcp_auth_token");
    }
    j["mcp_auth_token_enc"] = mcpAuthTokenEnc;
#else
    j.erase("token_enc");
    j.erase("plane_api_key_enc");
    j.erase("mcp_auth_token_enc");
    j["token"] = config.ApiToken;
    j["plane_api_key"] = config.PlaneApiKey;
    j["mcp_auth_token"] = config.McpAuthToken;
#endif
    WriteConfigJson(j);
}

BlameAnalysisConfig ConfigManager::LoadBlameAnalysis() {
    nlohmann::json j = LoadMergedConfigJson();
    BlameAnalysisConfig b;
    if (!j.contains("blame_analysis") || !j["blame_analysis"].is_object()) {
        return b;
    }
    const nlohmann::json& ba = j["blame_analysis"];
    b.P4Executable = ba.value("p4_exe", b.P4Executable);
    b.P4VcExecutable = ba.value("p4vc_exe", b.P4VcExecutable);
    b.TimelapseCommandTemplate = ba.value("timelapse_cmd", std::string());
    b.ChangeCommandTemplate = ba.value("change_cmd", std::string());
    b.AiChatUrl = ba.value("ai_chat_url", std::string());
    b.DefaultMaxFrames = ba.value("default_max_frames", b.DefaultMaxFrames);
    b.ChangelistCacheMaxEntries = ba.value("cl_cache_max", b.ChangelistCacheMaxEntries);
    b.CallstackTrackerFieldId = ba.value("callstack_jira_field_id", std::string());
    b.LastFoundClTrackerFieldId = ba.value("last_found_cl_jira_field_id", std::string());
    b.LastOccurrencesTrackerFieldId = ba.value("last_occurrences_jira_field_id", std::string());
    if (ba.contains("default_ignore_keywords") && ba["default_ignore_keywords"].is_array()) {
        for (const auto& item : ba["default_ignore_keywords"]) {
            if (item.is_string()) {
                b.DefaultIgnoreKeywords.push_back(item.get<std::string>());
            }
        }
    }
    if (ba.contains("path_remaps") && ba["path_remaps"].is_array()) {
        for (const auto& rule : ba["path_remaps"]) {
            if (!rule.is_object()) {
                continue;
            }
            PathRemapRule r;
            r.FromPrefix = rule.value("from", std::string());
            r.ToPrefix = rule.value("to", std::string());
            if (!r.FromPrefix.empty()) {
                b.PathRemaps.push_back(std::move(r));
            }
        }
    }
    if (ba.contains("ui_colors") && ba["ui_colors"].is_object()) {
        const nlohmann::json& uc = ba["ui_colors"];
        auto loadRgba = [&uc](const char* key, float* out) {
            if (!uc.contains(key) || !uc[key].is_array() || uc[key].size() < 4) {
                return;
            }
            try {
                float tmp[4];
                for (int i = 0; i < 4; ++i) {
                    tmp[i] = static_cast<float>(uc[key][i].get<double>());
                }
                std::memcpy(out, tmp, sizeof(tmp));
            } catch (...) {
            }
        };
        loadRgba("status_info", b.UiColors.StatusInfo);
        loadRgba("status_error", b.UiColors.StatusError);
        loadRgba("status_warning", b.UiColors.StatusWarning);
        loadRgba("find_highlight", b.UiColors.FindHighlight);
        loadRgba("text_disabled", b.UiColors.TextDisabled);
        loadRgba("import_existing", b.UiColors.ImportExisting);
        loadRgba("cl_tooltip_title", b.UiColors.ClTooltipTitle);
        loadRgba("syntax_keyword", b.UiColors.SyntaxKeyword);
        loadRgba("syntax_string", b.UiColors.SyntaxString);
        loadRgba("syntax_comment", b.UiColors.SyntaxComment);
        loadRgba("syntax_number", b.UiColors.SyntaxNumber);
        loadRgba("syntax_preprocessor", b.UiColors.SyntaxPreprocessor);
    }
    return b;
}

void ConfigManager::SaveBlameAnalysis(const BlameAnalysisConfig& b) {
    nlohmann::json j = LoadMergedConfigJson();
    nlohmann::json ba = nlohmann::json::object();
    ba["p4_exe"] = b.P4Executable;
    ba["p4vc_exe"] = b.P4VcExecutable;
    ba["timelapse_cmd"] = b.TimelapseCommandTemplate;
    ba["change_cmd"] = b.ChangeCommandTemplate;
    ba["ai_chat_url"] = b.AiChatUrl;
    ba["default_max_frames"] = b.DefaultMaxFrames;
    ba["cl_cache_max"] = b.ChangelistCacheMaxEntries;
    ba["callstack_jira_field_id"] = b.CallstackTrackerFieldId;
    ba["last_found_cl_jira_field_id"] = b.LastFoundClTrackerFieldId;
    ba["last_occurrences_jira_field_id"] = b.LastOccurrencesTrackerFieldId;
    ba["default_ignore_keywords"] = nlohmann::json::array();
    for (const auto& kw : b.DefaultIgnoreKeywords) {
        ba["default_ignore_keywords"].push_back(kw);
    }
    ba["path_remaps"] = nlohmann::json::array();
    for (const auto& r : b.PathRemaps) {
        ba["path_remaps"].push_back(nlohmann::json{{"from", r.FromPrefix}, {"to", r.ToPrefix}});
    }
    nlohmann::json uc = nlohmann::json::object();
    auto putRgba = [&uc](const char* key, const float* v) {
        uc[key] = nlohmann::json::array({v[0], v[1], v[2], v[3]});
    };
    putRgba("status_info", b.UiColors.StatusInfo);
    putRgba("status_error", b.UiColors.StatusError);
    putRgba("status_warning", b.UiColors.StatusWarning);
    putRgba("find_highlight", b.UiColors.FindHighlight);
    putRgba("text_disabled", b.UiColors.TextDisabled);
    putRgba("import_existing", b.UiColors.ImportExisting);
    putRgba("cl_tooltip_title", b.UiColors.ClTooltipTitle);
    putRgba("syntax_keyword", b.UiColors.SyntaxKeyword);
    putRgba("syntax_string", b.UiColors.SyntaxString);
    putRgba("syntax_comment", b.UiColors.SyntaxComment);
    putRgba("syntax_number", b.UiColors.SyntaxNumber);
    putRgba("syntax_preprocessor", b.UiColors.SyntaxPreprocessor);
    ba["ui_colors"] = std::move(uc);
    j["blame_analysis"] = std::move(ba);
    WriteConfigJson(j);
}

TrackerConfig ConfigManager::Load(const CliOverrides& cli) {
    const bool canUseCache = !cli.HasDbPath && !cli.HasBackendType && !cli.HasMcpPort && !cli.HasMcpAllowRemote;
    if (canUseCache) {
        std::lock_guard<std::mutex> lock(GetCacheMutexRef());
        // cppcheck-suppress knownConditionTrueFalse ; cache flag is set by Invalidate/Store paths cppcheck does not model
        if (GetHasCachedConfigRef()) {
            return GetCachedConfigRef();
        }
    }

    nlohmann::json j = LoadMergedConfigJson();
    const bool hasSetupConfig = !LoadJsonFile(GetConfigPath()).empty();
    TrackerConfig cfg;
    cfg.DbPath = SmatchetDefaults::kDefaultDbPath;
    cfg.TrackerType = SmatchetDefaults::kDefaultBackendType;
    cfg.McpPort = SmatchetDefaults::Mcp::kDefaultPort;
#if defined(_WIN32)
    bool migrateLegacyPlaintextMcpAuthToken = false;
#endif

    if (!j.empty()) {
        try {
            cfg.DbPath = j.value("db_path", cfg.DbPath);
            cfg.Domain = j.value("domain", std::string{});
            cfg.Email = j.value("email", std::string{});
#if defined(_WIN32)
            cfg.ApiToken = UnprotectSecretFieldFromConfig("token_enc", j.value("token_enc", std::string{}));
            if (cfg.ApiToken.empty()) {
                cfg.ApiToken = j.value("token", std::string{});
            }
#else
            cfg.ApiToken = j.value("token", std::string{});
#endif
            cfg.ProjectKey = j.value("project_key", std::string{});
            cfg.TrackerType = j.value("tracker_type", cfg.TrackerType);
            cfg.PlaneUrl = j.value("plane_url", cfg.PlaneUrl);
            cfg.PlaneWorkspaceSlug = j.value("plane_workspace_slug", cfg.PlaneWorkspaceSlug);
            cfg.PlaneProjectId = j.value("plane_project_id", cfg.PlaneProjectId);

#if defined(_WIN32)
            cfg.PlaneApiKey = UnprotectSecretFieldFromConfig("plane_api_key_enc",
                                                             j.value("plane_api_key_enc", std::string{}));
            if (cfg.PlaneApiKey.empty()) {
                cfg.PlaneApiKey = j.value("plane_api_key", std::string{});
            }
#else
            cfg.PlaneApiKey = j.value("plane_api_key", std::string{});
#endif

            cfg.JqlQuery = j.value("jql", cfg.JqlQuery);
            cfg.EnableFieldOverflowTooltips = j.value("field_overflow_tooltips", cfg.EnableFieldOverflowTooltips);
            cfg.ReadOnlyMode = j.value("read_only_mode", cfg.ReadOnlyMode);
            cfg.GridEndWheelSwallowsBeforeHorizontal =
                j.value("grid_end_wheel_swallows_before_horizontal", cfg.GridEndWheelSwallowsBeforeHorizontal);
            cfg.ShowPreferencesWindow = j.value("show_preferences_window", cfg.ShowPreferencesWindow);
            cfg.ShowViewsDashboardWindow =
                j.value("show_views_dashboard_window", cfg.ShowViewsDashboardWindow);
            cfg.ShowPerformanceWindow = j.value("show_performance_window", cfg.ShowPerformanceWindow);
            cfg.ShowLogWindow = j.value("show_log_window", cfg.ShowLogWindow);
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
            cfg.ShowLuaAutomationWindow =
                j.value("show_lua_automation_window", cfg.ShowLuaAutomationWindow);
#endif
            cfg.LogMinLevel = j.value("log_min_level", cfg.LogMinLevel);
            cfg.LogTrackerHttpBodies =
                j.value("log_tracker_http_bodies", j.value("log_jira_http_bodies", cfg.LogTrackerHttpBodies));
            cfg.LogP4Io = j.value("log_p4_io", cfg.LogP4Io);
            cfg.McpEnabled = j.value("mcp_enabled", cfg.McpEnabled);
            cfg.McpPort = j.value("mcp_port", cfg.McpPort);
            cfg.McpAllowRemote = j.value("mcp_allow_remote", cfg.McpAllowRemote);
#if defined(_WIN32)
            cfg.McpAuthToken = UnprotectSecretFieldFromConfig("mcp_auth_token_enc",
                                                               j.value("mcp_auth_token_enc", std::string{}));
            if (cfg.McpAuthToken.empty()) {
                cfg.McpAuthToken = j.value("mcp_auth_token", std::string{});
                migrateLegacyPlaintextMcpAuthToken = !cfg.McpAuthToken.empty();
            }
#else
            cfg.McpAuthToken = j.value("mcp_auth_token", std::string{});
#endif
            cfg.McpAllowLuaExecution = j.value("mcp_allow_lua_execution", cfg.McpAllowLuaExecution);
            if (j.contains("mcp_export_fields") && j["mcp_export_fields"].is_array()) {
                for (const auto& item : j["mcp_export_fields"]) {
                    if (item.is_string()) {
                        cfg.McpExportFields.push_back(item.get<std::string>());
                    }
                }
            }
            cfg.ShowMcpServerWindow = j.value("show_mcp_server_window", cfg.ShowMcpServerWindow);
            cfg.McpServerInfoPanelHeightPx = static_cast<float>(
                j.value("mcp_server_info_panel_height_px", static_cast<double>(cfg.McpServerInfoPanelHeightPx)));
            cfg.McpServerActivityPanelHeightPx = static_cast<float>(j.value(
                "mcp_server_activity_panel_height_px", static_cast<double>(cfg.McpServerActivityPanelHeightPx)));
            cfg.BlameAllowCustomCommands = j.value("blame_allow_custom_commands", cfg.BlameAllowCustomCommands);
            cfg.DateFormatOption = j.value("date_format_option", cfg.DateFormatOption);
            cfg.DateCompactRelativeThresholdDays =
                j.value("date_compact_relative_threshold_days", cfg.DateCompactRelativeThresholdDays);
            if (cfg.DateCompactRelativeThresholdDays < 1)
                cfg.DateCompactRelativeThresholdDays = 1;
            if (cfg.DateCompactRelativeThresholdDays > 365)
                cfg.DateCompactRelativeThresholdDays = 365;
            cfg.DefaultIssueTypeId = j.value("default_issue_type_id", cfg.DefaultIssueTypeId);
            cfg.DefaultIssueTypeName = j.value("default_issue_type_name", cfg.DefaultIssueTypeName);
            cfg.ImportMaxConcurrent = j.value("import_max_concurrent", cfg.ImportMaxConcurrent);
            cfg.LastImportDirectory = j.value("last_import_directory", cfg.LastImportDirectory);
            cfg.LastExportDirectory = j.value("last_export_directory", cfg.LastExportDirectory);
            cfg.ViewFieldPickerHeight = j.value("view_field_picker_height", cfg.ViewFieldPickerHeight);
            cfg.SelectedFontName = j.value("selected_font_name", cfg.SelectedFontName);
            cfg.UiLanguage = NormalizeUiLanguageCode(j.value("ui_language", cfg.UiLanguage));
            cfg.UpdateCheckEnabled = j.value("update_check_enabled", cfg.UpdateCheckEnabled);
            cfg.UpdateIncludePrerelease = j.value("update_include_prerelease", cfg.UpdateIncludePrerelease);
            cfg.UpdateSkipVersion = j.value("update_skip_version", cfg.UpdateSkipVersion);
            cfg.UpdateGithubRepo = j.value("update_github_repo", cfg.UpdateGithubRepo);
            if (j.contains("quick_comment_templates") && j["quick_comment_templates"].is_array()) {
                cfg.QuickCommentTemplates.clear();
                for (const auto& item : j["quick_comment_templates"]) {
                    try {
                        cfg.QuickCommentTemplates.push_back(item.get<CommentTemplate>());
                    } catch (...) {
                    }
                }
            } else {
                cfg.QuickCommentTemplates = GetDefaultQuickCommentTemplates();
            }

            if (j.contains("blame_comment_templates") && j["blame_comment_templates"].is_array()) {
                cfg.BlameCommentTemplates.clear();
                for (const auto& item : j["blame_comment_templates"]) {
                    try {
                        cfg.BlameCommentTemplates.push_back(item.get<CommentTemplate>());
                    } catch (...) {
                    }
                }
            } else {
                cfg.BlameCommentTemplates = GetDefaultBlameCommentTemplates();
            }

            if (j.contains("duration_suggestions") && j["duration_suggestions"].is_array()) {
                cfg.DurationSuggestions.clear();
                for (const auto& item : j["duration_suggestions"]) {
                    if (item.is_string()) {
                        cfg.DurationSuggestions.push_back(item.get<std::string>());
                    }
                }
            } else {
                cfg.DurationSuggestions = {"15m", "30m", "1h", "2h", "4h", "8h", "1d", "2d", "1w"};
            }

            if (j.contains("worklog_comment_templates") && j["worklog_comment_templates"].is_array()) {
                cfg.WorkLogCommentTemplates.clear();
                for (const auto& item : j["worklog_comment_templates"]) {
                    if (item.is_string()) {
                        cfg.WorkLogCommentTemplates.push_back(item.get<std::string>());
                    }
                }
            } else {
                cfg.WorkLogCommentTemplates = {
                    "Investigated and resolved the issue.", "Tested and verified on local environment.",
                    "Refactored code and ran static analysis.", "Discussed with team and updated implementation.",
                    "Wrote unit tests and verified all passing."};
            }

            {
                cfg.NewIssueInheritFieldIds = DefaultNewIssueInheritFieldIdsList();
                if (j.contains("new_issue_inherit_field_ids") && j["new_issue_inherit_field_ids"].is_array()) {
                    cfg.NewIssueInheritFieldIds.clear();
                    for (const auto& item : j["new_issue_inherit_field_ids"]) {
                        if (item.is_string()) {
                            std::string s = item.get<std::string>();
                            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
                                s.erase(0, 1);
                            }
                            while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
                                s.pop_back();
                            }
                            if (!s.empty() && s != "summary") {
                                cfg.NewIssueInheritFieldIds.push_back(std::move(s));
                            }
                        }
                    }
                }
                if (cfg.NewIssueInheritFieldIds.empty()) {
                    cfg.NewIssueInheritFieldIds = DefaultNewIssueInheritFieldIdsList();
                }
            }
            {
                cfg.NewIssueInheritFieldIdsPlane = DefaultNewIssueInheritFieldIdsList();
                if (j.contains("new_issue_inherit_field_ids_plane") &&
                    j["new_issue_inherit_field_ids_plane"].is_array()) {
                    cfg.NewIssueInheritFieldIdsPlane.clear();
                    for (const auto& item : j["new_issue_inherit_field_ids_plane"]) {
                        if (item.is_string()) {
                            std::string s = item.get<std::string>();
                            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
                                s.erase(0, 1);
                            }
                            while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
                                s.pop_back();
                            }
                            if (!s.empty() && s != "summary") {
                                cfg.NewIssueInheritFieldIdsPlane.push_back(std::move(s));
                            }
                        }
                    }
                }
                if (cfg.NewIssueInheritFieldIdsPlane.empty()) {
                    cfg.NewIssueInheritFieldIdsPlane = DefaultNewIssueInheritFieldIdsList();
                }
            }
        } catch (const std::exception& ex) {
            LOG_ERROR("ConfigManager: Load() parse error: %s", ex.what());
        } catch (...) {
            LOG_ERROR("ConfigManager: Load() parse error (unknown)");
        }
    }

    if (!hasSetupConfig && !j.contains("read_only_mode")) {
        cfg.ReadOnlyMode = true;
    }
    if (!hasSetupConfig && !j.contains("show_preferences_window")) {
        cfg.ShowPreferencesWindow = true;
    }

#if defined(_WIN32)
    // Only MCP gets an eager legacy cleanup here; older secrets keep their established lazy migration behavior.
    //
    // Ordering note (backlog #15): this Save(cfg) runs BEFORE the env/CLI override block below, by design.
    // - cfg here reflects what disk contained (with the legacy plaintext token already decoded into cfg.McpAuthToken).
    //   Save() re-encrypts McpAuthToken into mcp_auth_token_enc on disk, which is the whole point of the migration.
    // - Env / CLI overrides (DbPath, TrackerType, McpPort, McpAllowRemote) are ephemeral and must NOT be persisted —
    //   they're meant to take effect this process only. Running Save AFTER overrides would write override values
    //   back to disk and pollute the next launch when the env/CLI is no longer set.
    // - Consequence: for the rest of this Load(), in-memory cfg (and the cache filled at the bottom) hold
    //   override-applied values while disk holds pre-override values. That divergence is intentional and matches
    //   pre-split behavior. The standing limitation that any subsequent Save() with this cfg would write
    //   override values to disk is a pre-existing concern outside the scope of this migration.
    if (migrateLegacyPlaintextMcpAuthToken) {
        LOG_INFO("ConfigManager: migrating legacy plaintext mcp_auth_token to protected storage.");
        Save(cfg);
    }
#endif

    // Apply Option 1 overrides (Environment Variables)
    if (const char* envDbPath = std::getenv("SMATCHET_DB_PATH")) {
        cfg.DbPath = envDbPath;
    }
    if (const char* envBackend = std::getenv("SMATCHET_BACKEND_TYPE")) {
        cfg.TrackerType = envBackend;
    } else if (const char* envTracker = std::getenv("SMATCHET_TRACKER_TYPE")) {
        cfg.TrackerType = envTracker;
    }
    if (const char* envMcpPort = std::getenv("SMATCHET_MCP_PORT")) {
        try {
            cfg.McpPort = std::stoi(envMcpPort);
        } catch (...) {
        }
    }
    if (const char* envMcpRemote = std::getenv("SMATCHET_MCP_ALLOW_REMOTE")) {
        std::string s(envMcpRemote);
        cfg.McpAllowRemote = (s == "true" || s == "1");
    }

    // Apply Option 4 overrides (CLI parameters)
    if (cli.HasDbPath) {
        cfg.DbPath = cli.DbPath;
    }
    if (cli.HasBackendType) {
        cfg.TrackerType = cli.BackendType;
    }
    if (cli.HasMcpPort) {
        cfg.McpPort = cli.McpPort;
    }
    if (cli.HasMcpAllowRemote) {
        cfg.McpAllowRemote = cli.McpAllowRemote;
    }

    // Post-override clamp and safety bounds checking
    if (cfg.ImportMaxConcurrent < 1)
        cfg.ImportMaxConcurrent = 1;
    if (cfg.ImportMaxConcurrent > 32)
        cfg.ImportMaxConcurrent = 32;
    if (cfg.GridEndWheelSwallowsBeforeHorizontal < 0)
        cfg.GridEndWheelSwallowsBeforeHorizontal = 0;
    if (cfg.GridEndWheelSwallowsBeforeHorizontal > 32)
        cfg.GridEndWheelSwallowsBeforeHorizontal = 32;
    if (cfg.McpPort < 1 || cfg.McpPort > 65535) {
        cfg.McpPort = SmatchetDefaults::Mcp::kDefaultPort;
    }

    if (canUseCache) {
        std::lock_guard<std::mutex> lock(GetCacheMutexRef());
        GetCachedConfigRef() = cfg;
        GetHasCachedConfigRef() = true;
    }

    return cfg;
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

const char* ConfigManager::GetDefaultImGuiDockLayoutIni() {
    return kDefaultImGuiDockLayoutIni;
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

std::string ConfigManager::NormalizeViewsBackendKey(const std::string& trackerType) {
    std::string t;
    t.resize(trackerType.size());
    std::transform(trackerType.begin(), trackerType.end(), t.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (t == "plane") {
        return "Plane";
    }
    return SmatchetDefaults::kDefaultBackendType;
}

PersistentViewsFile ConfigManager::LoadPersistentViewsFromDisk() {
    PersistentViewsFile disk;
    disk.Version = 2;
    const std::string viewsPath = GetViewsPath();
    std::lock_guard<std::mutex> lock(GetIoMutexRef());
    ScopedFileLock fileLock(viewsPath);
    std::ifstream file(viewsPath);
    if (!file.is_open()) {
        return disk;
    }
    try {
        nlohmann::json j;
        file >> j;
        if (!j.is_object()) {
            return disk;
        }
        if (j.contains("backends") && j["backends"].is_object()) {
            disk.Version = j.value("version", 2);
            for (auto it = j["backends"].begin(); it != j["backends"].end(); ++it) {
                if (!it.value().is_object()) {
                    continue;
                }
                disk.Backends[it.key()] = ParseWorkspaceObject(it.value());
            }
            return disk;
        }
        // Legacy v1: top-level active_view_id + views → Jira bucket.
        disk.Backends[SmatchetDefaults::kDefaultBackendType] = ParseWorkspaceObject(j);
        return disk;
    } catch (const std::exception& ex) {
        LOG_ERROR("ConfigManager: failed to parse views '%s': %s", viewsPath.c_str(), ex.what());
        return disk;
    } catch (...) {
        LOG_ERROR("ConfigManager: failed to parse views '%s' with unknown exception", viewsPath.c_str());
        return disk;
    }
}

void ConfigManager::SavePersistentViewsToDisk(const PersistentViewsFile& disk) {
    const std::string viewsPath = GetViewsPath();
    nlohmann::json j;
    j["version"] = 2;
    j["backends"] = nlohmann::json::object();
    std::vector<std::string> keys;
    keys.reserve(disk.Backends.size());
    std::transform(disk.Backends.begin(), disk.Backends.end(), std::back_inserter(keys),
                   [](const auto& kv) { return kv.first; });
    std::sort(keys.begin(), keys.end());
    for (const auto& bk : keys) {
        const auto it = disk.Backends.find(bk);
        if (it == disk.Backends.end()) {
            continue;
        }
        j["backends"][bk] = SerializeWorkspace(it->second);
    }
    std::lock_guard<std::mutex> lock(GetIoMutexRef());
    ScopedFileLock fileLock(viewsPath);
    const std::string content = j.dump(4);
    if (!AtomicWriteTextFile(viewsPath, content)) {
        LOG_ERROR("ConfigManager: atomic write failed for views file '%s'", viewsPath.c_str());
    }
}

void ConfigManager::EnsureViewBucketBootstrapped(PersistentViewsFile& disk, const std::string& backendKey,
                                                 const TrackerConfig& cfg, bool& outDirty) {
    ViewWorkspaceState& ws = disk.Backends[backendKey];
    if (!ws.Views.empty()) {
        if (ws.ActiveViewId.empty()) {
            ws.ActiveViewId = ws.Views.front().Id;
            outDirty = true;
        }
        return;
    }
    ws = MakeDefaultViewWorkspaceForBackend(backendKey, cfg);
    outDirty = true;
}

ViewsStore ConfigManager::ViewWorkspaceToViewsStore(const ViewWorkspaceState& ws) {
    return ViewWorkspaceToViewsStoreImpl(ws);
}

void ConfigManager::ViewsStoreToViewWorkspace(const ViewsStore& slice, ViewWorkspaceState& ws) {
    ViewsStoreToViewWorkspaceImpl(slice, ws);
}

ViewsStore ConfigManager::LoadViewsOrBootstrap(const TrackerConfig& cfg) {
    try {
        PersistentViewsFile disk = LoadPersistentViewsFromDisk();
        const std::string key = NormalizeViewsBackendKey(cfg.TrackerType);
        bool dirty = false;
        EnsureViewBucketBootstrapped(disk, key, cfg, dirty);
        if (dirty) {
            SavePersistentViewsToDisk(disk);
        }
        return ViewWorkspaceToViewsStoreImpl(disk.Backends[key]);
    } catch (const std::exception& ex) {
        LOG_ERROR("ConfigManager: LoadViewsOrBootstrap error: %s", ex.what());
        const std::string key = NormalizeViewsBackendKey(cfg.TrackerType);
        return ViewWorkspaceToViewsStoreImpl(
            MakeDefaultViewWorkspaceForBackend(key, cfg));
    } catch (...) {
        LOG_ERROR("ConfigManager: LoadViewsOrBootstrap error (unknown)");
        const std::string key = NormalizeViewsBackendKey(cfg.TrackerType);
        return ViewWorkspaceToViewsStoreImpl(
            MakeDefaultViewWorkspaceForBackend(key, cfg));
    }
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
    const std::wstring wSrc = Utf8ToWide(tmp);
    const std::wstring wDst = Utf8ToWide(path);
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

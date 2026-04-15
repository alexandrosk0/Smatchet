#include "AppController.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>
#include <cstdlib>
#include <fstream>
#include <chrono>
#include <cstdint>
#include <cctype>
#include <tuple>
#include <unordered_set>
#include <exception>
#include <sstream>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

#include "imgui.h"
#include "ConfigManager.h"
#include "Logger.h"
#include "StringUtil.h"
#include "CompactDateFormat.h"

#include <cpr/cpr.h>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

namespace {
// Base64 encode (RFC 4648) so we can send Authorization exactly like PowerShell/curl.
std::string Base64Encode(const std::string& in) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        const unsigned char a = static_cast<unsigned char>(in[i]);
        const unsigned char b = (i + 1 < in.size()) ? static_cast<unsigned char>(in[i + 1]) : 0u;
        const unsigned char c = (i + 2 < in.size()) ? static_cast<unsigned char>(in[i + 2]) : 0u;
        out += table[a >> 2];
        out += table[((a & 3) << 4) | (b >> 4)];
        out += (i + 1 < in.size()) ? table[((b & 15) << 2) | (c >> 6)] : '=';
        out += (i + 2 < in.size()) ? table[c & 63] : '=';
    }
    return out;
}

std::string ExtensionFromMime(const std::string& mimeType) {
    if (mimeType == "image/png") return ".png";
    if (mimeType == "image/jpeg") return ".jpg";
    if (mimeType == "image/jpg") return ".jpg";
    if (mimeType == "image/gif") return ".gif";
    if (mimeType == "image/webp") return ".webp";
    if (mimeType == "application/pdf") return ".pdf";
    return ".bin";
}

std::string TrimAsciiWhitespace(const std::string& value) {
    size_t start = 0;
    size_t end = value.size();
    while (start < end && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(start, end - start);
}

std::string MimeTypeWithoutParams(const std::string& mimeType) {
    auto toLower = [](const std::string& value) {
        std::string lowered = value;
        for (char& c : lowered) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return lowered;
    };
    const std::string trimmed = TrimAsciiWhitespace(mimeType);
    if (trimmed.empty()) {
        return std::string();
    }
    const size_t sep = trimmed.find(';');
    if (sep == std::string::npos) {
        return toLower(trimmed);
    }
    return toLower(TrimAsciiWhitespace(trimmed.substr(0, sep)));
}

bool IsImageMimeType(const std::string& mimeType) {
    const std::string normalized = MimeTypeWithoutParams(mimeType);
    return normalized == "image/png" ||
           normalized == "image/jpeg" ||
           normalized == "image/jpg" ||
           normalized == "image/gif" ||
           normalized == "image/webp";
}

std::string MakeUniqueTempFilePath(const std::string& filename, const std::string& extension);

bool DownloadAttachmentToLocalFile(const std::string& url,
                                   const std::string& filename,
                                   const std::string& mimeType,
                                   std::string& outFilePath,
                                   std::string& outMime,
                                   std::string& outError) {
    outFilePath.clear();
    outMime.clear();
    outError.clear();
    if (url.empty()) {
        outError = "Attachment URL is empty.";
        return false;
    }

    JiraConfig cfg = ConfigManager::Load();
    if (cfg.ApiToken.empty() || cfg.Domain.empty()) {
        outError = "Missing Jira credentials/domain.";
        return false;
    }

    const std::string basicCred = cfg.Email + ":" + cfg.ApiToken;
    const std::string authHeader = "Basic " + Base64Encode(basicCred);
    cpr::Header headers{
        {"Accept", "*/*"},
        {"Authorization", authHeader},
        {"User-Agent", "Smatchet/1.0 Attachment-Downloader"}
    };
    cpr::Redirect redirect(true, true);

    const auto resp = cpr::Get(cpr::Url{url}, headers, redirect);
    if (resp.error.code != cpr::ErrorCode::OK || resp.status_code < 200 || resp.status_code >= 300) {
        outError = "Download failed: HTTP " + std::to_string(static_cast<int>(resp.status_code));
        return false;
    }

    outMime = mimeType;
    try {
        auto it = resp.header.find("Content-Type");
        if (it != resp.header.end() && !it->second.empty()) {
            outMime = it->second;
        }
    } catch (...) {
        LOG_DEBUG("DownloadAttachmentToLocalFile: response header parse failed; using provided mime type.");
    }
    if (outMime.empty()) {
        outMime = "application/octet-stream";
    }

    const std::string extension = ExtensionFromMime(outMime);
    outFilePath = MakeUniqueTempFilePath(filename, extension);
    std::ofstream ofs(outFilePath, std::ios::binary);
    if (!ofs.is_open()) {
        outError = "Failed to open local temp file.";
        outFilePath.clear();
        return false;
    }

    ofs.write(resp.text.data(), static_cast<std::streamsize>(resp.text.size()));
    if (!ofs.good()) {
        outError = "Failed to write downloaded attachment bytes.";
        ofs.close();
        outFilePath.clear();
        return false;
    }
    ofs.close();
    LOG_INFO("DownloadAttachmentToLocalFile: downloaded %zu bytes mime=%s path=%s",
             resp.text.size(),
             outMime.c_str(),
             outFilePath.c_str());
    return true;
}

std::string GetTempDir() {
    const char* env = std::getenv("TEMP");
    if (env && *env) return std::string(env);
    env = std::getenv("TMP");
    if (env && *env) return std::string(env);
    return ".";
}

std::string SanitizeFilename(const std::string& name) {
    std::string out = name;
    for (auto& ch : out) {
        if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '\"' ||
            ch == '<' || ch == '>' || ch == '|') {
            ch = '_';
        }
    }
    if (out.empty()) return std::string("attachment");
    return out;
}

std::string MakeUniqueTempFilePath(const std::string& filename, const std::string& extension) {
    const auto now = std::chrono::system_clock::now().time_since_epoch().count();
    const std::string safe = SanitizeFilename(filename);
    std::string base = safe;

    // If caller didn't include an extension, append our derived one.
    if (base.find_last_of('.') == std::string::npos) {
        base += extension;
    }

    const std::string tempDir = GetTempDir();
    if (tempDir.empty()) return base;

    // Ensure tempDir ends with a path separator.
    if (tempDir.back() != '/' && tempDir.back() != '\\') {
        return tempDir + "/" + std::to_string(now) + "_" + base;
    }
    return tempDir + std::to_string(now) + "_" + base;
}

bool LuaTruthy(const sol::object& o) {
    if (!o.valid()) {
        return false;
    }
    const sol::type t = o.get_type();
    if (t == sol::type::lua_nil) {
        return false;
    }
    if (t == sol::type::boolean) {
        return o.as<bool>();
    }
    return true;
}

sol::object JsonToLua(sol::state_view luaView, const nlohmann::json& j) {
    switch (j.type()) {
    case nlohmann::json::value_t::null:
        return sol::make_object(luaView, sol::nil);
    case nlohmann::json::value_t::boolean:
        return sol::make_object(luaView, j.get<bool>());
    case nlohmann::json::value_t::number_integer:
        return sol::make_object(luaView, static_cast<double>(j.get<std::int64_t>()));
    case nlohmann::json::value_t::number_unsigned:
        return sol::make_object(luaView, static_cast<double>(j.get<std::uint64_t>()));
    case nlohmann::json::value_t::number_float:
        return sol::make_object(luaView, j.get<double>());
    case nlohmann::json::value_t::string:
        return sol::make_object(luaView, j.get<std::string>());
    case nlohmann::json::value_t::array: {
        sol::table arr = luaView.create_table();
        std::size_t idx = 1;
        for (const auto& el : j) {
            arr[idx++] = JsonToLua(luaView, el);
        }
        return arr;
    }
    case nlohmann::json::value_t::object: {
        sol::table tbl = luaView.create_table();
        for (auto it = j.begin(); it != j.end(); ++it) {
            tbl[it.key()] = JsonToLua(luaView, it.value());
        }
        return tbl;
    }
    default:
        return sol::make_object(luaView, sol::nil);
    }
}

std::string AsciiLowerCopy(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

#if !defined(_WIN32)
std::string EscapeShellArg(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (char c : value) {
        if (c == '"' || c == '\\' || c == '`' || c == '$') {
            escaped.push_back('\\');
        }
        escaped.push_back(c);
    }
    return escaped;
}
#endif
} // namespace

void AppController::SetOpenUrlHandler(std::function<void(const std::string&)> handler) {
    OpenUrlHandler = std::move(handler);
}

void AppController::OpenUrl(const std::string& url) const {
    if (url.empty()) {
        return;
    }

    if (OpenUrlHandler) {
        OpenUrlHandler(url);
        return;
    }

#if defined(_WIN32)
    const HINSTANCE openResult = ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<intptr_t>(openResult) <= 32) {
        LOG_ERROR("AppController::OpenUrl failed url=%s err=%ld", TruncateForLog(url, 300).c_str(), GetLastError());
    }
#elif defined(__APPLE__)
    std::string cmd = "open \"" + EscapeShellArg(url) + "\"";
    const int rc = std::system(cmd.c_str());
    if (rc != 0) {
        LOG_ERROR("AppController::OpenUrl failed rc=%d url=%s", rc, TruncateForLog(url, 300).c_str());
    }
#else
    std::string cmd = "xdg-open \"" + EscapeShellArg(url) + "\"";
    const int rc = std::system(cmd.c_str());
    if (rc != 0) {
        LOG_ERROR("AppController::OpenUrl failed rc=%d url=%s", rc, TruncateForLog(url, 300).c_str());
    }
#endif
}

void AppController::AddAutomationLogSink(std::function<void(const std::string&)> sink) {
    if (sink) {
        AutomationLogSinks.push_back(std::move(sink));
    }
}

void AppController::SetAttachmentViewerHandler(AttachmentViewerHandler handler) {
    AttachmentViewerHandlerCallback = std::move(handler);
}

void AppController::SetAttachmentPreviewHandler(AttachmentPreviewHandler handler) {
    AttachmentPreviewHandlerCallback = std::move(handler);
}

void AppController::SetAttachmentCollectionHandler(AttachmentCollectionHandler handler) {
    AttachmentCollectionHandlerCallback = std::move(handler);
}

void AppController::ShowAttachmentCollection(const std::vector<AttachmentDescriptor>& attachments) {
    if (attachments.empty()) {
        return;
    }
    if (AttachmentCollectionHandlerCallback) {
        AttachmentCollectionHandlerCallback(attachments);
        return;
    }

    const AttachmentDescriptor& first = attachments.front();
    if (!first.Url.empty()) {
        OpenAttachment(first.Url, first.Filename, first.MimeType);
    }
}

void AppController::OpenAttachment(const std::string& url,
                                    const std::string& filename,
                                    const std::string& mimeType) {
    if (url.empty()) {
        return;
    }

    // If no file-based path exists, fall back to regular URL opening.
    if (!AttachmentViewerHandlerCallback && !AttachmentPreviewHandlerCallback) {
        LOG_INFO("OpenAttachment: no attachment handler, opening URL directly.");
        OpenUrl(url);
        return;
    }

    std::string outFilePath;
    std::string outMime;
    std::string outError;
    if (!DownloadAttachmentToLocalFile(url, filename, mimeType, outFilePath, outMime, outError)) {
        LOG_WARN("OpenAttachment: %s; falling back to URL open.", outError.c_str());
        OpenUrl(url);
        return;
    }
    if (AttachmentViewerHandlerCallback) {
        LOG_INFO("OpenAttachment: dispatching to host attachment viewer.");
        AttachmentViewerHandlerCallback(outFilePath, outMime, filename);
        return;
    }

    if (AttachmentPreviewHandlerCallback && IsImageMimeType(outMime)) {
        if (AttachmentPreviewHandlerCallback(outFilePath, outMime, filename, url)) {
            LOG_INFO("OpenAttachment: queued in-app image preview.");
            return;
        }
        LOG_WARN("OpenAttachment: in-app preview rejected; falling back to URL open.");
    }

    LOG_INFO("OpenAttachment: opening attachment via URL fallback.");
    OpenUrl(url);
}

bool AppController::DownloadAttachmentForPreview(const std::string& url,
                                                 const std::string& filename,
                                                 const std::string& mimeType,
                                                 std::string* outError) {
    auto fail = [outError](const std::string& errorMessage) {
        if (outError != nullptr) {
            *outError = errorMessage;
        }
        return false;
    };
    if (!AttachmentPreviewHandlerCallback || !IsImageMimeType(mimeType)) {
        return fail(!AttachmentPreviewHandlerCallback
                        ? std::string("Preview handler is unavailable.")
                        : std::string("Attachment is not a supported image type."));
    }
    std::string outFilePath;
    std::string outMime;
    std::string downloadError;
    if (!DownloadAttachmentToLocalFile(url, filename, mimeType, outFilePath, outMime, downloadError)) {
        LOG_WARN("DownloadAttachmentForPreview: %s", downloadError.c_str());
        return fail(downloadError);
    }
    if (!AttachmentPreviewHandlerCallback(outFilePath, outMime, filename, url)) {
        LOG_WARN("DownloadAttachmentForPreview: preview handler rejected file=%s mime=%s",
                 filename.c_str(),
                 outMime.c_str());
        return fail("Preview handler rejected the attachment.");
    }
    if (outError != nullptr) {
        outError->clear();
    }
    return true;
}

void AppController::Initialize(const std::string& dbPath, const std::string& backendType) {
    LOG_INFO("AppController::Initialize backendType=%s dbPath=%s", backendType.c_str(), dbPath.c_str());
    Cache = std::unique_ptr<LocalCacheManager>(new LocalCacheManager(dbPath));

    if (backendType == "Jira") {
        Backend = std::unique_ptr<ITrackerClient>(new JiraClient());
        JiraBackend = dynamic_cast<JiraClient*>(Backend.get());
        LOG_INFO("AppController: Jira backend initialized.");
    } else {
        LOG_WARN("AppController: unsupported backendType=%s; backend disabled.", backendType.c_str());
    }

    const std::string& fileBase = ConfigManager::GetFilesBaseDirectory();
    if (!fileBase.empty()) {
        luaScriptsDirectory_ = fileBase + "Scripts/";
    } else {
        luaScriptsDirectory_.clear();
    }

    // Defer SyncWithBackend to first SmatchetUI::Draw so active view JQL/fields are
    // applied first — avoids fetching issues twice at startup.
    RefreshLocalData();
    WarmIssueTypeEditMetaAtStartAsync();

    InitLua();
}

std::string AppController::ResolveLuaScriptPath(const std::string& filename) const {
    if (!luaScriptsDirectory_.empty()) {
        return luaScriptsDirectory_ + filename;
    }
    return std::string("Scripts/") + filename;
}

void AppController::InitLua() {
    lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::table);

    // Bind the CachedTicket struct
    lua.new_usertype<CachedTicket>("Ticket",
        "id", &CachedTicket::id,
        "get_field", &CachedTicket::GetFieldValue
    );

    lua.set_function("log_info", [this](std::string msg) {
        if (!AutomationLogSinks.empty()) {
            for (const auto& sink : AutomationLogSinks) {
                sink(msg);
            }
        } else {
            std::printf("[LUA] %s\n", msg.c_str());
        }
    });

    lua.set_function("decode_json", [this](const std::string& s) -> std::tuple<sol::object, std::string> {
        try {
            const nlohmann::json j = nlohmann::json::parse(s);
            return {JsonToLua(lua, j), std::string()};
        } catch (const std::exception& e) {
            return {sol::make_object(lua, sol::nil), std::string(e.what())};
        }
    });

    lua.set_function("register_field_display", [this](const std::string& fieldId, sol::function fn) {
        if (fieldId.empty() || !fn.valid()) {
            return;
        }
        fieldDisplayHandlers_[fieldId] = sol::protected_function(std::move(fn));
    });

    lua.set_function("unregister_field_display", [this](const std::string& fieldId) {
        fieldDisplayHandlers_.erase(fieldId);
    });

    lua.set_function("register_field_display_by_name", [this](const std::string& displayName, sol::function fn) {
        if (displayName.empty() || !fn.valid()) {
            return;
        }
        fieldDisplayHandlersByDisplayName_[AsciiLowerCopy(displayName)] = sol::protected_function(std::move(fn));
    });

    lua.set_function("unregister_field_display_by_name", [this](const std::string& displayName) {
        fieldDisplayHandlersByDisplayName_.erase(AsciiLowerCopy(displayName));
    });

    sol::table imgui = lua.create_table();
    imgui.set_function("progress_bar", [](float fraction, float width, float height) {
        ImVec2 sz(width, height);
        if (width < 0.0f) {
            sz.x = ImGui::GetContentRegionAvail().x;
        }
        if (height <= 0.0f) {
            sz.y = ImGui::GetFrameHeight();
        }
        ImGui::ProgressBar(fraction, sz);
    });
    imgui.set_function("text", [](const std::string& s) { ImGui::TextUnformatted(s.c_str()); });
    imgui.set_function("text_unformatted", [](const std::string& s) { ImGui::TextUnformatted(s.c_str()); });
    imgui.set_function("get_content_region_avail", []() {
        const ImVec2 v = ImGui::GetContentRegionAvail();
        return std::make_tuple(v.x, v.y);
    });
    lua["imgui"] = imgui;
}

void AppController::RunLuaSetupScript(const std::string& scriptPath) {
    auto logErr = [this](const char* prefix, const std::string& detail) {
        const std::string msg = std::string(prefix) + detail;
        if (!AutomationLogSinks.empty()) {
            for (const auto& sink : AutomationLogSinks) {
                sink(msg);
            }
        } else {
            std::printf("%s\n", msg.c_str());
        }
    };

    const std::string path = ResolveLuaScriptPath(scriptPath);
    try {
        lua.script_file(path);
    } catch (const sol::error& e) {
        logErr("[LUA setup] ", e.what());
    } catch (const std::exception& e) {
        logErr("[LUA setup] ", e.what());
    }
}

bool AppController::TryLuaFieldDisplay(const std::string& fieldId,
                                       const CachedTicket& ticket,
                                       const std::string& rawValue,
                                       const float availWidth,
                                       const JiraField* fieldMeta) {
    sol::protected_function* handler = nullptr;
    const auto itId = fieldDisplayHandlers_.find(fieldId);
    if (itId != fieldDisplayHandlers_.end() && itId->second.valid()) {
        handler = &itId->second;
    } else if (fieldMeta && !fieldMeta->Name.empty()) {
        const auto itName =
            fieldDisplayHandlersByDisplayName_.find(AsciiLowerCopy(fieldMeta->Name));
        if (itName != fieldDisplayHandlersByDisplayName_.end() && itName->second.valid()) {
            handler = &itName->second;
        }
    }
    if (handler == nullptr) {
        return false;
    }

    sol::table ctx = lua.create_table();
    ctx["issue_id"] = ticket.id;
    ctx["field_id"] = fieldId;
    ctx["raw"] = rawValue;
    ctx["avail_width"] = availWidth;
    const bool catalogReadOnly = fieldMeta ? fieldMeta->ReadOnly : false;
    const bool editMetaReadOnly = !CanEditJiraFieldForIssue(ticket.id, fieldId, fieldMeta);
    ctx["read_only"] = catalogReadOnly || editMetaReadOnly;
    if (fieldMeta) {
        ctx["field_name"] = fieldMeta->Name;
    } else {
        ctx["field_name"] = sol::lua_nil;
    }

    sol::protected_function_result pfr = (*handler)(ctx);
    if (!pfr.valid()) {
        sol::error err = pfr;
        const std::string msg = std::string("[LUA field display] ") + err.what();
        if (!AutomationLogSinks.empty()) {
            for (const auto& sink : AutomationLogSinks) {
                sink(msg);
            }
        } else {
            std::printf("%s\n", msg.c_str());
        }
        return false;
    }

    if (pfr.return_count() < 1) {
        return false;
    }
    const sol::object ret = pfr.get<sol::object>(0);
    return LuaTruthy(ret);
}

void AppController::RunAutoScript(const std::string& scriptPath) {
    const std::string path = ResolveLuaScriptPath(scriptPath);
    for (auto& ticket : ActiveTickets) {
        lua["ticket"] = &ticket;
        try {
            lua.script_file(path);
        } catch (const sol::error& e) {
            LOG_ERROR("RunAutoScript: lua error ticket=%s path=%s err=%s", ticket.id.c_str(), path.c_str(), e.what());
        } catch (const std::exception& e) {
            LOG_ERROR("RunAutoScript: exception ticket=%s path=%s err=%s", ticket.id.c_str(), path.c_str(), e.what());
        }
    }
}

void AppController::SyncWithBackend(const JiraConfig* configOverride, const ViewsStore* viewsOverride) {
    LOG_INFO("AppController::SyncWithBackend started.");
    if (Backend && Cache) {
        bool fullSyncCompleted = false;
        auto freshTickets = Backend->FetchIssues(&fullSyncCompleted, configOverride, viewsOverride);
        size_t saved = 0;
        for (const auto& t : freshTickets) {
            Cache->SaveTicket(t);
            ++saved;
        }
        size_t deleted = 0;
        if (fullSyncCompleted) {
            std::unordered_set<std::string> keepIds;
            keepIds.reserve(freshTickets.size());
            for (const auto& t : freshTickets) {
                if (!t.id.empty()) {
                    keepIds.insert(t.id);
                }
            }
            std::vector<CachedTicket> existing = Cache->GetAllTickets();
            for (const auto& row : existing) {
                if (keepIds.find(row.id) == keepIds.end()) {
                    Cache->DeleteTicket(row.id);
                    ++deleted;
                }
            }
        }
        LOG_INFO("AppController::SyncWithBackend finished fetched=%zu saved=%zu deleted=%zu fullSync=%d",
                 freshTickets.size(),
                 saved,
                 deleted,
                 fullSyncCompleted ? 1 : 0);
    } else {
        LOG_WARN("AppController::SyncWithBackend skipped: backend=%d cache=%d",
                 Backend ? 1 : 0,
                 Cache ? 1 : 0);
    }
    RefreshLocalData();
    WarmIssueTypeEditMetaAtStartAsync();
}

namespace {
bool IsSprintField(const JiraField& field) {
    return field.SchemaCustom.find("gh-sprint") != std::string::npos;
}

bool IsEditableTimetrackingEstimateFieldId(const std::string& fieldId) {
    return fieldId == "timeoriginalestimate" || fieldId == "timeestimate";
}

bool IsNonEditableTimetrackingFieldId(const std::string& fieldId) {
    return fieldId == "timespent" ||
           fieldId == "aggregatetimeoriginalestimate" ||
           fieldId == "aggregatetimeestimate" ||
           fieldId == "aggregatetimespent";
}

void DecodeCascadingSelection(const std::string& encoded, std::string& outParentId, std::string& outChildId) {
    const size_t sep = encoded.find('\x1f');
    if (sep == std::string::npos) {
        outParentId = encoded;
        outChildId.clear();
        return;
    }
    outParentId = encoded.substr(0, sep);
    outChildId = encoded.substr(sep + 1);
}

const JiraFieldOption* FindJiraFieldOptionByIdRecursive(const std::vector<JiraFieldOption>& options,
                                                        const std::string& id) {
    for (const auto& option : options) {
        if (option.Id == id) {
            return &option;
        }
        if (!option.Children.empty()) {
            if (const JiraFieldOption* nested = FindJiraFieldOptionByIdRecursive(option.Children, id)) {
                return nested;
            }
        }
    }
    return nullptr;
}

std::string ResolveJiraFieldOptionLabelRecursive(const std::vector<JiraFieldOption>& options,
                                                 const std::string& value) {
    for (const auto& option : options) {
        if (option.Id == value || option.Value == value) {
            return option.Value;
        }
        if (!option.Children.empty()) {
            const std::string nested = ResolveJiraFieldOptionLabelRecursive(option.Children, value);
            if (!nested.empty()) {
                return nested;
            }
        }
    }
    return {};
}

std::string ResolveDisplayValueForSubmittedSelection(const JiraField& field, const std::string& value) {
    if (field.Family == JiraFieldFamily::CascadingSelect) {
        std::string parentId;
        std::string childId;
        DecodeCascadingSelection(value, parentId, childId);
        const JiraFieldOption* parent = FindJiraFieldOptionByIdRecursive(field.AllowedValueOptions, parentId);
        if (parent == nullptr) {
            return value;
        }
        if (childId.empty()) {
            return parent->Value;
        }
        const JiraFieldOption* child = FindJiraFieldOptionByIdRecursive(parent->Children, childId);
        if (child == nullptr) {
            return parent->Value;
        }
        return parent->Value + " > " + child->Value;
    }
    const std::string resolved = ResolveJiraFieldOptionLabelRecursive(field.AllowedValueOptions, value);
    return resolved.empty() ? value : resolved;
}

nlohmann::json MinimalPayloadForStructuredOption(const nlohmann::json& raw) {
    if (!raw.is_object()) {
        return raw;
    }
    nlohmann::json out = nlohmann::json::object();
    if (raw.contains("id") && (raw["id"].is_string() || raw["id"].is_number())) {
        out["id"] = raw["id"];
        return out;
    }
    if (raw.contains("accountId") && raw["accountId"].is_string()) {
        out["accountId"] = raw["accountId"];
        return out;
    }
    if (raw.contains("groupId") && raw["groupId"].is_string()) {
        out["groupId"] = raw["groupId"];
        if (raw.contains("name") && raw["name"].is_string()) {
            out["name"] = raw["name"];
        }
        return out;
    }
    if (raw.contains("key") && raw["key"].is_string()) {
        out["key"] = raw["key"];
        return out;
    }
    if (raw.contains("value") && raw["value"].is_string()) {
        out["value"] = raw["value"];
        return out;
    }
    if (raw.contains("name") && raw["name"].is_string()) {
        out["name"] = raw["name"];
        return out;
    }
    return raw;
}

bool TryBuildStructuredOptionPayload(const JiraFieldOption& option,
                                     const std::string& nestedChildId,
                                     nlohmann::json& outPayload) {
    if (option.PayloadJson.empty()) {
        return false;
    }
    const nlohmann::json raw = nlohmann::json::parse(option.PayloadJson, nullptr, false);
    if (raw.is_discarded()) {
        return false;
    }
    outPayload = MinimalPayloadForStructuredOption(raw);
    if (!nestedChildId.empty()) {
        const JiraFieldOption* child = FindJiraFieldOptionByIdRecursive(option.Children, nestedChildId);
        if (child == nullptr) {
            return false;
        }
        nlohmann::json childPayload;
        if (!TryBuildStructuredOptionPayload(*child, std::string(), childPayload)) {
            childPayload = nlohmann::json::object({{"id", nestedChildId}});
        }
        if (!outPayload.is_object()) {
            outPayload = nlohmann::json::object();
        }
        outPayload["child"] = std::move(childPayload);
    }
    return true;
}

bool TryBuildFieldOptionPayload(const JiraField& field,
                                const std::string& selectedValue,
                                nlohmann::json& outPayload) {
    if (field.Family == JiraFieldFamily::CascadingSelect) {
        std::string parentId;
        std::string childId;
        DecodeCascadingSelection(selectedValue, parentId, childId);
        const JiraFieldOption* option = FindJiraFieldOptionByIdRecursive(field.AllowedValueOptions, parentId);
        if (option == nullptr) {
            return false;
        }
        return TryBuildStructuredOptionPayload(*option, childId, outPayload);
    }
    const JiraFieldOption* option = FindJiraFieldOptionByIdRecursive(field.AllowedValueOptions, selectedValue);
    if (option == nullptr) {
        return false;
    }
    return TryBuildStructuredOptionPayload(*option, std::string(), outPayload);
}

nlohmann::json FallbackPayloadForSelectableField(const JiraField& field, const std::string& scalarValue) {
    if (field.IsUserType) {
        return nlohmann::json{{"accountId", scalarValue}};
    }
    if (field.Family == JiraFieldFamily::Status) {
        return nlohmann::json{{"name", scalarValue}};
    }
    if (field.Family == JiraFieldFamily::IssueType) {
        return nlohmann::json{{"id", scalarValue}};
    }
    if (field.Type == "option" || field.Type == "component" || !field.AllowedValueOptions.empty()) {
        return nlohmann::json{{"id", scalarValue}};
    }
    return scalarValue;
}

bool TryParseJiraNumberValue(const std::string& rawValue, nlohmann::json& outValue) {
    const std::string trimmed = TrimCopy(rawValue);
    if (trimmed.empty()) {
        outValue = nullptr;
        return true;
    }

    size_t parsedChars = 0;
    try {
        const double parsed = std::stod(trimmed, &parsedChars);
        if (parsedChars != trimmed.size()) {
            return false;
        }
        outValue = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

nlohmann::json AdfDocumentFromPlainText(const std::string& plainText) {
    nlohmann::json content = nlohmann::json::array();
    std::string line;
    std::istringstream iss(plainText);
    while (std::getline(iss, line)) {
        nlohmann::json para = nlohmann::json::object();
        para["type"] = "paragraph";
        para["content"] = nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", line}}});
        content.push_back(std::move(para));
    }
    if (content.empty()) {
        nlohmann::json para = nlohmann::json::object();
        para["type"] = "paragraph";
        para["content"] = nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", ""}}});
        content.push_back(std::move(para));
    }
    return nlohmann::json{{"type", "doc"}, {"version", 1}, {"content", std::move(content)}};
}

bool ErrorTextContainsHttpStatus(const std::string& errorText, int statusCode) {
    if (statusCode < 100 || statusCode > 599) {
        return false;
    }
    const std::string needle = "HTTP " + std::to_string(statusCode);
    return errorText.find(needle) != std::string::npos;
}

}

void AppController::RefreshLocalData() {
    if (Cache) {
        ActiveTickets = Cache->GetAllTickets();
        PruneEditMetaCacheToActiveTickets();
    }
}

void AppController::UpdateTicket(const CachedTicket& ticket) {
    if (Cache) {
        Cache->SaveTicket(ticket);
        RefreshLocalData(); // Push changes back to ActiveTickets vector
    }
}

bool AppController::RefreshJiraFieldCatalog(const JiraConfig& cfg) {
    if (!JiraBackend) {
        LastJiraFieldCatalogError = "Jira backend is not initialized.";
        AvailableJiraFields.clear();
        AvailableJiraComponents.clear();
        LOG_WARN("AppController::RefreshJiraFieldCatalog skipped: Jira backend not initialized.");
        return false;
    }

    std::vector<JiraField> fetchedFields;
    std::vector<JiraComponent> fetchedComponents;
    std::string error;
    const bool ok = JiraBackend->FetchFieldCatalog(cfg, fetchedFields, fetchedComponents, error);
    if (!ok) {
        LastJiraFieldCatalogError = error;
        AvailableJiraFields.clear();
        AvailableJiraComponents.clear();
        LOG_ERROR("AppController::RefreshJiraFieldCatalog failed: %s", error.c_str());
        return false;
    }

    AvailableJiraFields = std::move(fetchedFields);
    AvailableJiraComponents = std::move(fetchedComponents);
    LastJiraFieldCatalogError.clear();
    return true;
}

void AppController::SetJiraFieldCatalog(std::vector<JiraField> fields,
                                       std::vector<JiraComponent> components,
                                       const std::string& error) {
    if (!error.empty()) {
        AvailableJiraFields.clear();
        AvailableJiraComponents.clear();
        LastJiraFieldCatalogError = error;
        LOG_ERROR("AppController::SetJiraFieldCatalog error: %s", error.c_str());
        return;
    }
    AvailableJiraFields = std::move(fields);
    AvailableJiraComponents = std::move(components);
    LastJiraFieldCatalogError.clear();
    for (auto& field : AvailableJiraFields) {
        if (field.Id == "comment" || IsNonEditableTimetrackingFieldId(field.Id)) {
            field.ReadOnly = true;
        }
    }
    if (FindJiraFieldById("history") == nullptr) {
        JiraField historyField;
        historyField.Id = "history";
        historyField.Name = "History";
        historyField.ReadOnly = true;
        AvailableJiraFields.push_back(std::move(historyField));
    }
}

const JiraField* AppController::FindJiraFieldById(const std::string& fieldId) const {
    const auto it = std::find_if(
        AvailableJiraFields.begin(),
        AvailableJiraFields.end(),
        [&](const JiraField& field) { return field.Id == fieldId; });
    return it == AvailableJiraFields.end() ? nullptr : &(*it);
}

std::string AppController::ResolveIssueTypeKeyForIssue(const std::string& issueId) const {
    if (issueId.empty()) {
        return std::string();
    }
    const auto it = std::find_if(
        ActiveTickets.begin(),
        ActiveTickets.end(),
        [&](const CachedTicket& ticket) { return ticket.id == issueId; });
    if (it == ActiveTickets.end()) {
        return std::string();
    }
    return ToLowerAsciiCopy(TrimCopy(it->GetFieldValue("issuetype")));
}

void AppController::WarmIssueTypeEditMetaAtStartAsync() {
    if (!JiraBackend) {
        return;
    }
    std::vector<std::pair<std::string, std::string>> representatives;
    std::unordered_set<std::string> seenTypes;
    seenTypes.reserve(ActiveTickets.size());
    {
        std::lock_guard<std::mutex> lock(editMetaMutex_);
        for (const auto& ticket : ActiveTickets) {
            if (ticket.id.empty()) {
                continue;
            }
            const std::string typeKey = ToLowerAsciiCopy(TrimCopy(ticket.GetFieldValue("issuetype")));
            if (typeKey.empty()) {
                continue;
            }
            if (seenTypes.find(typeKey) != seenTypes.end()) {
                continue;
            }
            const auto typeIt = issueTypeEditMeta_.find(typeKey);
            if (typeIt != issueTypeEditMeta_.end() && typeIt->second.loaded) {
                seenTypes.insert(typeKey);
                continue;
            }
            seenTypes.insert(typeKey);
            representatives.push_back({typeKey, ticket.id});
        }
    }
    if (representatives.empty()) {
        return;
    }
    std::thread([this, representatives]() {
        for (const auto& pair : representatives) {
            std::string ignored;
            EnsureIssueEditMetaLoaded(pair.second, &ignored);
        }
    }).detach();
}

bool AppController::CanEditJiraFieldForIssue(const std::string& issueId,
                                            const std::string& fieldId,
                                            const JiraField* fieldMeta) const {
    if (!JiraBackend || issueId.empty() || fieldId.empty()) {
        return true;
    }
    if (IsEditableTimetrackingEstimateFieldId(fieldId)) {
        return true;
    }
    const JiraField* meta = fieldMeta ? fieldMeta : FindJiraFieldById(fieldId);
    if (meta && IsSprintField(*meta)) {
        return true;
    }
    const std::string fieldKey = ToLowerAsciiCopy(fieldId);
    // Edit metadata usually does not include a plain "set" for status; Jira applies changes via
    // POST /issue/{key}/transitions (see JiraClient::UpdateIssueFields).
    if (fieldKey == "status") {
        return true;
    }
    const std::string issueTypeKey = ResolveIssueTypeKeyForIssue(issueId);
    std::lock_guard<std::mutex> lock(editMetaMutex_);
    const auto it = issueEditMeta_.find(issueId);
    if (it == issueEditMeta_.end() || !it->second.loaded) {
        if (!issueTypeKey.empty()) {
            const auto byType = issueTypeEditMeta_.find(issueTypeKey);
            if (byType != issueTypeEditMeta_.end() && byType->second.loaded) {
                const auto typeFieldIt = byType->second.fieldCanEdit.find(fieldKey);
                if (typeFieldIt == byType->second.fieldCanEdit.end()) {
                    return false;
                }
                return typeFieldIt->second;
            }
        }
        return true;
    }
    const auto fieldIt = it->second.fieldCanEdit.find(fieldKey);
    if (fieldIt == it->second.fieldCanEdit.end()) {
        return false;
    }
    return fieldIt->second;
}

bool AppController::EnsureIssueEditMetaLoaded(const std::string& issueId, std::string* outError) {
    if (outError) {
        outError->clear();
    }
    if (!JiraBackend || issueId.empty()) {
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(editMetaMutex_);
        const auto it = issueEditMeta_.find(issueId);
        if (it != issueEditMeta_.end() && it->second.loaded) {
            return true;
        }
    }
    const std::string issueTypeKey = ResolveIssueTypeKeyForIssue(issueId);
    if (!issueTypeKey.empty()) {
        std::lock_guard<std::mutex> lock(editMetaMutex_);
        const auto typeIt = issueTypeEditMeta_.find(issueTypeKey);
        if (typeIt != issueTypeEditMeta_.end() && typeIt->second.loaded) {
            issueEditMeta_[issueId] = typeIt->second;
            return true;
        }
    }

    const JiraConfig cfg = ConfigManager::Load();
    std::unordered_map<std::string, bool> meta;
    std::string fetchError;
    const bool ok = JiraBackend->FetchIssueEditMeta(cfg, issueId, meta, fetchError);

    IssueEditMetaCache cache;
    cache.loaded = true;
    if (ok) {
        cache.fieldCanEdit = std::move(meta);
    }
    {
        std::lock_guard<std::mutex> lock(editMetaMutex_);
        issueEditMeta_[issueId] = cache;
        if (ok && !issueTypeKey.empty()) {
            issueTypeEditMeta_[issueTypeKey] = cache;
        }
    }

    if (!ok) {
        LOG_WARN("AppController: editmeta fetch failed issue=%s err=%s", issueId.c_str(), fetchError.c_str());
        if (outError) {
            *outError = fetchError;
        }
    }
    return ok;
}

bool AppController::RefreshIssueEditMeta(const std::string& issueId, std::string* outError) {
    const std::string issueTypeKey = ResolveIssueTypeKeyForIssue(issueId);
    InvalidateIssueEditMeta(issueId);
    if (!issueTypeKey.empty()) {
        std::lock_guard<std::mutex> lock(editMetaMutex_);
        issueTypeEditMeta_.erase(issueTypeKey);
    }
    return EnsureIssueEditMetaLoaded(issueId, outError);
}

void AppController::InvalidateIssueEditMeta(const std::string& issueId) {
    if (issueId.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(editMetaMutex_);
    issueEditMeta_.erase(issueId);
}

void AppController::PruneEditMetaCacheToActiveTickets() {
    std::unordered_set<std::string> keep;
    std::unordered_set<std::string> keepTypes;
    keep.reserve(ActiveTickets.size());
    keepTypes.reserve(ActiveTickets.size());
    for (const auto& t : ActiveTickets) {
        if (!t.id.empty()) {
            keep.insert(t.id);
        }
        const std::string typeKey = ToLowerAsciiCopy(TrimCopy(t.GetFieldValue("issuetype")));
        if (!typeKey.empty()) {
            keepTypes.insert(typeKey);
        }
    }

    std::lock_guard<std::mutex> lock(editMetaMutex_);
    for (auto it = issueEditMeta_.begin(); it != issueEditMeta_.end();) {
        if (keep.find(it->first) == keep.end()) {
            it = issueEditMeta_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = issueTypeEditMeta_.begin(); it != issueTypeEditMeta_.end();) {
        if (keepTypes.find(it->first) == keepTypes.end()) {
            it = issueTypeEditMeta_.erase(it);
        } else {
            ++it;
        }
    }
}

void AppController::WarmIssueEditMetaAsync(const std::string& issueId) {
    if (!JiraBackend || issueId.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(editMetaMutex_);
        const auto it = issueEditMeta_.find(issueId);
        if (it != issueEditMeta_.end() && it->second.loaded) {
            return;
        }
    }

    std::thread([this, issueId]() {
        std::string ignored;
        EnsureIssueEditMetaLoaded(issueId, &ignored);
    }).detach();
}

bool AppController::SubmitJiraFieldEdit(const std::string& issueId,
                                        const JiraField& field,
                                        const std::vector<std::string>& rawValues,
                                        std::string& outError) {
    outError.clear();
    if (!Backend || !Cache) {
        outError = "Backend or cache is not initialized.";
        LOG_WARN("AppController::SubmitJiraFieldEdit skipped issue=%s field=%s: %s",
                 issueId.c_str(),
                 field.Id.c_str(),
                 outError.c_str());
        return false;
    }
    if (issueId.empty()) {
        outError = "Issue id is empty.";
        LOG_WARN("AppController::SubmitJiraFieldEdit skipped field=%s: %s", field.Id.c_str(), outError.c_str());
        return false;
    }

    std::vector<std::string> values;
    values.reserve(rawValues.size());
    for (const auto& value : rawValues) {
        if (!value.empty()) {
            values.push_back(value);
        }
    }

    if (IsSprintField(field)) {
        if (!JiraBackend) {
            outError = "Jira backend is not initialized.";
            return false;
        }
        if (values.empty()) {
            outError = "Clearing sprint is not supported by this action.";
            LOG_WARN("AppController::SubmitJiraFieldEdit sprint clear not supported issue=%s field=%s",
                     issueId.c_str(),
                     field.Id.c_str());
            return false;
        }
        const std::string sprintId = values.front();
        JiraConfig cfg = ConfigManager::Load();
        if (!JiraBackend->AddIssueToSprint(cfg, issueId, sprintId, outError)) {
            LOG_ERROR("AppController::SubmitJiraFieldEdit sprint update failed issue=%s field=%s sprint=%s err=%s",
                      issueId.c_str(),
                      field.Id.c_str(),
                      sprintId.c_str(),
                      outError.c_str());
            return false;
        }
        auto ticketIt = std::find_if(
            ActiveTickets.begin(),
            ActiveTickets.end(),
            [&](const CachedTicket& ticket) { return ticket.id == issueId; });
        if (ticketIt != ActiveTickets.end()) {
            CachedTicket updatedTicket = *ticketIt;
            std::string displayValue = sprintId;
            for (const auto& option : field.AllowedValueOptions) {
                if (option.Id == sprintId) {
                    displayValue = option.Value;
                    break;
                }
            }
            updatedTicket.fieldValues[field.Id] = displayValue;
            UpdateTicket(updatedTicket);
        } else {
            RefreshLocalData();
        }
        return true;
    }

    if (IsNonEditableTimetrackingFieldId(field.Id)) {
        outError = "This Jira time field is derived or worklog-backed and cannot be edited directly.";
        LOG_WARN("AppController::SubmitJiraFieldEdit blocked non-editable timetracking issue=%s field=%s",
                 issueId.c_str(),
                 field.Id.c_str());
        return false;
    }

    auto ticketIt = std::find_if(
        ActiveTickets.begin(),
        ActiveTickets.end(),
        [&](const CachedTicket& ticket) { return ticket.id == issueId; });

    if (IsEditableTimetrackingEstimateFieldId(field.Id)) {
        const std::string editedValue = values.empty() ? std::string() : values.front();
        if (editedValue.empty()) {
            outError = "Clearing Jira timetracking estimates is not supported by this editor.";
            LOG_WARN("AppController::SubmitJiraFieldEdit blocked timetracking clear issue=%s field=%s",
                     issueId.c_str(),
                     field.Id.c_str());
            return false;
        }

        std::string originalEstimate =
            (ticketIt != ActiveTickets.end()) ? ticketIt->GetFieldValue("timeoriginalestimate") : std::string();
        std::string remainingEstimate =
            (ticketIt != ActiveTickets.end()) ? ticketIt->GetFieldValue("timeestimate") : std::string();
        if (field.Id == "timeoriginalestimate") {
            originalEstimate = editedValue;
        } else {
            remainingEstimate = editedValue;
        }

        nlohmann::json timetrackingPayload = nlohmann::json::object();
        if (!originalEstimate.empty()) {
            timetrackingPayload["originalEstimate"] = originalEstimate;
        }
        if (!remainingEstimate.empty()) {
            timetrackingPayload["remainingEstimate"] = remainingEstimate;
        }
        if (timetrackingPayload.empty()) {
            outError = "Timetracking update requires at least one estimate value.";
            return false;
        }

        nlohmann::json fieldsPayload = nlohmann::json::object();
        fieldsPayload["timetracking"] = std::move(timetrackingPayload);
        if (!Backend->UpdateIssueFields(issueId, fieldsPayload, outError)) {
            std::string payloadForLog;
            try {
                payloadForLog = fieldsPayload.dump();
            } catch (...) {
                payloadForLog = "(payload dump failed)";
            }
            LOG_ERROR("AppController::SubmitJiraFieldEdit failed issue=%s field=%s jira_error=%s request=%s",
                      issueId.c_str(),
                      field.Id.c_str(),
                      outError.c_str(),
                      TruncateForLog(payloadForLog, 1200).c_str());
            return false;
        }

        if (ticketIt != ActiveTickets.end()) {
            CachedTicket updatedTicket = *ticketIt;
            updatedTicket.fieldValues["timeoriginalestimate"] = originalEstimate;
            updatedTicket.fieldValues["timeestimate"] = remainingEstimate;
            UpdateTicket(updatedTicket);
        } else {
            RefreshLocalData();
        }
        return true;
    }

    if (JiraBackend && !IsSprintField(field) && !IsEditableTimetrackingEstimateFieldId(field.Id)) {
        EnsureIssueEditMetaLoaded(issueId, nullptr);
    }

    if (JiraBackend && !IsSprintField(field) && !IsEditableTimetrackingEstimateFieldId(field.Id) &&
        !CanEditJiraFieldForIssue(issueId, field.Id, &field)) {
        outError = "Field cannot be edited for this issue (Jira edit metadata).";
        LOG_WARN("AppController::SubmitJiraFieldEdit blocked by editmeta issue=%s field=%s",
                 issueId.c_str(),
                 field.Id.c_str());
        return false;
    }

    nlohmann::json valuePayload;
    if (field.IsArray) {
        valuePayload = nlohmann::json::array();
        for (const auto& value : values) {
            if (field.IsUserType) {
                valuePayload.push_back(nlohmann::json{{"accountId", value}});
            } else if (field.Family == JiraFieldFamily::StructuredMulti ||
                       field.Family == JiraFieldFamily::IssueType ||
                       field.Family == JiraFieldFamily::Status ||
                       field.Family == JiraFieldFamily::CascadingSelect) {
                nlohmann::json optionPayload;
                if (TryBuildFieldOptionPayload(field, value, optionPayload)) {
                    valuePayload.push_back(std::move(optionPayload));
                } else if (field.ItemsType == "option" || field.ItemsType == "component" ||
                           !field.AllowedValueOptions.empty()) {
                    valuePayload.push_back(FallbackPayloadForSelectableField(field, value));
                } else {
                    valuePayload.push_back(value);
                }
            } else if (field.ItemsType == "option" || field.ItemsType == "component" || !field.AllowedValueOptions.empty()) {
                valuePayload.push_back(nlohmann::json{{"id", value}});
            } else {
                valuePayload.push_back(value);
            }
        }
    } else {
        const std::string scalarValue = values.empty() ? std::string() : values.front();
        if (scalarValue.empty()) {
            valuePayload = nullptr;
        } else if (field.IsUserType) {
            valuePayload = nlohmann::json{{"accountId", scalarValue}};
        } else if (field.Family == JiraFieldFamily::StructuredSingle ||
                   field.Family == JiraFieldFamily::IssueType ||
                   field.Family == JiraFieldFamily::Status ||
                   field.Family == JiraFieldFamily::CascadingSelect) {
            if (!TryBuildFieldOptionPayload(field, scalarValue, valuePayload)) {
                valuePayload = FallbackPayloadForSelectableField(field, scalarValue);
            }
        } else if (field.Type == "option" || field.Type == "component" || !field.AllowedValueOptions.empty()) {
            valuePayload = nlohmann::json{{"id", scalarValue}};
        } else if (field.Id == "description") {
            valuePayload = AdfDocumentFromPlainText(scalarValue);
        } else if (field.Type == "date" || field.Type == "datetime") {
            ParsedJiraDateTime parsed;
            if (!TryParseJiraDateTime(scalarValue, parsed)) {
                outError = "Invalid date/datetime value.";
                LOG_WARN("AppController::SubmitJiraFieldEdit invalid date issue=%s field=%s value=%s",
                         issueId.c_str(),
                         field.Id.c_str(),
                         scalarValue.c_str());
                return false;
            }
            valuePayload = FormatJiraDateOrDateTimeForApi(field.Type == "date", parsed);
        } else if (field.Type == "number") {
            if (!TryParseJiraNumberValue(scalarValue, valuePayload)) {
                outError = "Invalid numeric value: " + scalarValue;
                LOG_WARN("AppController::SubmitJiraFieldEdit invalid number issue=%s field=%s value=%s",
                         issueId.c_str(),
                         field.Id.c_str(),
                         scalarValue.c_str());
                return false;
            }
        } else {
            valuePayload = scalarValue;
        }
    }

    nlohmann::json fieldsPayload = nlohmann::json::object();
    fieldsPayload[field.Id] = valuePayload;
    bool updateOk = Backend->UpdateIssueFields(issueId, fieldsPayload, outError);
    bool didRetryAfter400 = false;
    if (!updateOk && JiraBackend && ErrorTextContainsHttpStatus(outError, 400)) {
        didRetryAfter400 = true;
        RefreshIssueEditMeta(issueId, nullptr);
        if (!CanEditJiraFieldForIssue(issueId, field.Id, &field)) {
            outError =
                "Field cannot be edited for this issue (Jira edit metadata refreshed after validation failure).";
            LOG_WARN("AppController::SubmitJiraFieldEdit blocked after editmeta refresh issue=%s field=%s",
                     issueId.c_str(),
                     field.Id.c_str());
            return false;
        }
        updateOk = Backend->UpdateIssueFields(issueId, fieldsPayload, outError);
    }
    if (!updateOk) {
        std::string payloadForLog;
        try {
            payloadForLog = fieldsPayload.dump();
        } catch (...) {
            payloadForLog = "(payload dump failed)";
        }
        LOG_ERROR("AppController::SubmitJiraFieldEdit failed issue=%s field=%s retried_after_400=%d jira_error=%s request=%s",
                  issueId.c_str(),
                  field.Id.c_str(),
                  didRetryAfter400 ? 1 : 0,
                  outError.c_str(),
                  TruncateForLog(payloadForLog, 1200).c_str());
        return false;
    }

    // Keep local cache and in-memory model in sync with the successful Jira update.
    if (ticketIt != ActiveTickets.end()) {
        CachedTicket updatedTicket = *ticketIt;

        std::string displayValue;
        if (!values.empty()) {
            for (size_t i = 0; i < values.size(); ++i) {
                const std::string displayPart = ResolveDisplayValueForSubmittedSelection(field, values[i]);
                if (i != 0) {
                    displayValue += ", ";
                }
                displayValue += displayPart;
            }
        }

        updatedTicket.fieldValues[field.Id] = displayValue;
        UpdateTicket(updatedTicket);
    } else {
        RefreshLocalData();
    }

    return true;
}

bool AppController::FetchIssueWatchers(const std::string& issueKey,
                                       std::vector<JiraUser>& outWatchers,
                                       std::string& outError) const {
    outWatchers.clear();
    outError.clear();
    if (!JiraBackend) {
        outError = "Jira backend is not initialized.";
        return false;
    }
    const JiraConfig cfg = ConfigManager::Load();
    const bool ok = JiraBackend->FetchIssueWatchers(cfg, issueKey, outWatchers, outError);
    if (!ok) {
        LOG_ERROR("AppController::FetchIssueWatchers failed issue=%s err=%s", issueKey.c_str(), outError.c_str());
    }
    return ok;
}

bool AppController::JiraSearchUsersByQuery(const std::string& query,
                                           std::vector<JiraUser>& outUsers,
                                           std::string& outError) const {
    outUsers.clear();
    outError.clear();
    if (!JiraBackend) {
        outError = "Jira backend is not initialized.";
        return false;
    }
    const JiraConfig cfg = ConfigManager::Load();
    const bool ok = JiraBackend->SearchUsersByQuery(cfg, query, outUsers, outError);
    if (!ok) {
        LOG_ERROR("AppController::JiraSearchUsersByQuery failed query=%s err=%s",
                  TruncateForLog(query, 120).c_str(),
                  outError.c_str());
    }
    return ok;
}

bool AppController::JiraAddIssueCommentPlain(const std::string& issueKey,
                                             const std::string& plainText,
                                             std::string& outError) {
    outError.clear();
    if (!JiraBackend) {
        outError = "Jira backend is not initialized.";
        return false;
    }
    const JiraConfig cfg = ConfigManager::Load();
    const bool ok = JiraBackend->AddIssueCommentPlain(cfg, issueKey, plainText, outError);
    if (!ok) {
        LOG_ERROR("AppController::JiraAddIssueCommentPlain failed issue=%s err=%s", issueKey.c_str(), outError.c_str());
    }
    return ok;
}

bool AppController::JiraAddIssueCommentBlameContext(const std::string& issueKey,
                                                    const std::string& p4User,
                                                    const std::string& functionName,
                                                    const std::string& filePath,
                                                    const int lineNumber,
                                                    const std::string& changelist,
                                                    const std::string& date,
                                                    const bool approximated,
                                                    const std::string& codeSnippet,
                                                    std::string& outError) {
    outError.clear();
    if (!JiraBackend) {
        outError = "Jira backend is not initialized.";
        return false;
    }
    const JiraConfig cfg = ConfigManager::Load();
    const bool ok = JiraBackend->AddIssueCommentBlameContext(cfg,
                                                             issueKey,
                                                             p4User,
                                                             functionName,
                                                             filePath,
                                                             lineNumber,
                                                             changelist,
                                                             date,
                                                             approximated,
                                                             codeSnippet,
                                                             outError);
    if (!ok) {
        LOG_ERROR("AppController::JiraAddIssueCommentBlameContext failed issue=%s err=%s",
                  issueKey.c_str(),
                  outError.c_str());
    }
    return ok;
}

bool AppController::JiraFetchUserGroupNames(const std::string& accountId,
                                            std::vector<std::string>& outGroupNames,
                                            std::string& outError) const {
    outGroupNames.clear();
    outError.clear();
    if (!JiraBackend) {
        outError = "Jira backend is not initialized.";
        return false;
    }
    const JiraConfig cfg = ConfigManager::Load();
    const bool ok = JiraBackend->FetchUserGroupNames(cfg, accountId, outGroupNames, outError);
    if (!ok) {
        LOG_ERROR("AppController::JiraFetchUserGroupNames failed account=%s err=%s",
                  TruncateForLog(accountId, 40).c_str(),
                  outError.c_str());
    }
    return ok;
}


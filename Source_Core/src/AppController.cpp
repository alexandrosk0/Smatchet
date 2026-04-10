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

#include <nlohmann/json.hpp>

#include "imgui.h"
#include "ConfigManager.h"
#include "Logger.h"
#include "StringUtil.h"

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

void AppController::OpenAttachment(const std::string& url,
                                    const std::string& filename,
                                    const std::string& mimeType) {
    if (url.empty()) {
        return;
    }

    // If the host doesn't support attachments-as-files, fall back to regular URL opening.
    if (!AttachmentViewerHandlerCallback) {
        LOG_INFO("OpenAttachment: no attachment handler, opening URL directly.");
        OpenUrl(url);
        return;
    }

    // Download synchronously on click (simple + reliable).
    // Unreal hosts should marshal the handler back to the game thread if needed.
    JiraConfig cfg = ConfigManager::Load();
    if (cfg.ApiToken.empty() || cfg.Domain.empty()) {
        LOG_WARN("OpenAttachment: missing Jira credentials/domain, falling back to URL open.");
        OpenUrl(url);
        return;
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
        LOG_WARN("OpenAttachment: download failed status=%d cprErr=%d msg=%s; falling back to URL open.",
                 static_cast<int>(resp.status_code),
                 static_cast<int>(resp.error.code),
                 resp.error.message.c_str());
        OpenUrl(url);
        return;
    }

    std::string outMime = mimeType;
    try {
        auto it = resp.header.find("Content-Type");
        if (it != resp.header.end() && !it->second.empty()) {
            outMime = it->second;
        }
    } catch (...) {
        LOG_DEBUG("OpenAttachment: response header parse failed; using provided mime type.");
    }
    if (outMime.empty()) {
        outMime = "application/octet-stream";
    }

    const std::string extension = ExtensionFromMime(outMime);
    const std::string outFilePath = MakeUniqueTempFilePath(filename, extension);

    std::ofstream ofs(outFilePath, std::ios::binary);
    if (!ofs.is_open()) {
        LOG_WARN("OpenAttachment: failed to open temp file path=%s; falling back to URL open.", outFilePath.c_str());
        OpenUrl(url);
        return;
    }

    ofs.write(resp.text.data(), static_cast<std::streamsize>(resp.text.size()));
    if (!ofs.good()) {
        LOG_WARN("OpenAttachment: failed to write downloaded bytes path=%s; falling back to URL open.", outFilePath.c_str());
        ofs.close();
        OpenUrl(url);
        return;
    }
    ofs.close();

    LOG_INFO("OpenAttachment: downloaded %zu bytes mime=%s path=%s",
             resp.text.size(),
             outMime.c_str(),
             outFilePath.c_str());
    AttachmentViewerHandlerCallback(outFilePath, outMime, filename);
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
    ctx["read_only"] = fieldMeta ? fieldMeta->ReadOnly : false;
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
}

void AppController::RefreshLocalData() {
    if (Cache) {
        ActiveTickets = Cache->GetAllTickets();
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
        if (field.Id == "comment") {
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

    nlohmann::json valuePayload;
    if (field.IsArray) {
        valuePayload = nlohmann::json::array();
        for (const auto& value : values) {
            if (field.IsUserType) {
                valuePayload.push_back(nlohmann::json{{"accountId", value}});
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
        } else if (field.Type == "option" || field.Type == "component" || !field.AllowedValueOptions.empty()) {
            valuePayload = nlohmann::json{{"id", scalarValue}};
        } else {
            valuePayload = scalarValue;
        }
    }

    nlohmann::json fieldsPayload = nlohmann::json::object();
    fieldsPayload[field.Id] = valuePayload;
    if (!Backend->UpdateIssueFields(issueId, fieldsPayload, outError)) {
        LOG_ERROR("AppController::SubmitJiraFieldEdit failed issue=%s field=%s err=%s",
                  issueId.c_str(),
                  field.Id.c_str(),
                  outError.c_str());
        return false;
    }

    // Keep local cache and in-memory model in sync with the successful Jira update.
    auto ticketIt = std::find_if(
        ActiveTickets.begin(),
        ActiveTickets.end(),
        [&](const CachedTicket& ticket) { return ticket.id == issueId; });
    if (ticketIt != ActiveTickets.end()) {
        CachedTicket updatedTicket = *ticketIt;

        std::string displayValue;
        if (!values.empty()) {
            for (size_t i = 0; i < values.size(); ++i) {
                std::string displayPart = values[i];
                for (const auto& option : field.AllowedValueOptions) {
                    if (option.Id == values[i]) {
                        displayPart = option.Value;
                        break;
                    }
                }
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


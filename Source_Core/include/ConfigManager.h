#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <cstring>
#include <string>
#include <exception>
#include <fstream>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <nlohmann/json.hpp>

struct JiraConfig {
    std::string Domain;    // e.g., "yourcompany.atlassian.net"
    std::string Email;     // e.g., "dev@company.com"
    std::string ApiToken;  // Your Atlassian API Token
    // Jira project key used by create meta enrichment calls (e.g. PROJ).
    std::string ProjectKey;
    // JQL used when querying Jira; defaults to issues assigned to the current user.
    std::string JqlQuery = "assignee=currentUser()";
    // Jira field keys to extract and cache (e.g. customfield_12345, duedate).
    std::vector<std::string> SelectedFields;
    // When true, show tooltips for clipped/multiline field values.
    bool EnableFieldOverflowTooltips = true;
    // Minimum log level: trace, debug, info, warn, error (see Logger::ParseLogLevelString).
    std::string LogMinLevel = "info";
    // When true, JiraClient logs truncated HTTP response bodies at Trace.
    bool LogJiraHttpBodies = false;
    // When true, P4Blame logs truncated p4 stdout at Trace (plus stderr on non-zero exit).
    bool LogP4Io = false;
};

struct ViewSortSpec {
    std::string ColumnKey;  // "id" or "field:status" etc.
    int Direction = 0;      // 0=None, 1=Ascending, 2=Descending (ImGuiSortDirection)
    bool operator==(const ViewSortSpec& o) const { return ColumnKey == o.ColumnKey && Direction == o.Direction; }
    bool operator!=(const ViewSortSpec& o) const { return !(*this == o); }
};

struct ViewDefinition {
    std::string Id;
    std::string Name;
    std::string Jql = "assignee=currentUser()";
    std::vector<std::string> Fields;
    std::vector<std::string> ColumnOrder;
    std::unordered_map<std::string, float> ColumnWidths;
    std::vector<ViewSortSpec> SortSpecs;
};

struct ViewsStore {
    int Version = 1;
    std::string ActiveViewId;
    std::vector<ViewDefinition> Views;
};

struct PathRemapRule {
    std::string FromPrefix;
    std::string ToPrefix;
};

/** RGBA for Blame Analysis UI (ImGui); each array is {r,g,b,a} in 0..1. */
struct BlameUiThemeColors {
    float StatusInfo[4] = {0.55f, 0.92f, 0.75f, 1.0f};
    float StatusError[4] = {1.0f, 0.55f, 0.35f, 1.0f};
    float StatusWarning[4] = {1.0f, 0.85f, 0.2f, 1.0f};
    float FindHighlight[4] = {0.25f, 0.35f, 0.55f, 0.55f};
    float TextDisabled[4] = {0.55f, 0.55f, 0.58f, 1.0f};
    float ImportExisting[4] = {0.65f, 0.82f, 1.0f, 1.0f};
    float ClTooltipTitle[4] = {0.35f, 1.0f, 0.45f, 1.0f};
    float SyntaxKeyword[4] = {0.78f, 0.5f, 1.0f, 1.0f};
    float SyntaxString[4] = {0.95f, 0.65f, 0.45f, 1.0f};
    float SyntaxComment[4] = {0.45f, 0.75f, 0.45f, 1.0f};
    float SyntaxNumber[4] = {0.65f, 0.85f, 1.0f, 1.0f};
    float SyntaxPreprocessor[4] = {0.85f, 0.85f, 0.5f, 1.0f};
};

/** Settings for the Blame Analysis tool (stored under `blame_analysis` in smatchet_config.json). */
struct BlameAnalysisConfig {
    std::string P4Executable = "p4";
    std::string P4VcExecutable = "p4vc";
    /** If non-empty, used instead of default `p4vc timelapse -l {line} {file}`. Placeholders: {file}, {line}, {cl}. */
    std::string TimelapseCommandTemplate;
    /** If non-empty, used instead of default `p4vc change {cl}`. Placeholders: {cl}, {file}, {line}. */
    std::string ChangeCommandTemplate;
    /** Opened after "Ask AI" copies context to the clipboard. */
    std::string AiChatUrl;
    int DefaultMaxFrames = 64;
    std::vector<std::string> DefaultIgnoreKeywords;
    std::vector<PathRemapRule> PathRemaps;
    int ChangelistCacheMaxEntries = 512;
    BlameUiThemeColors UiColors{};
    /** Jira field id (e.g. customfield_10001) whose value populates the blame callstack text. */
    std::string CallstackJiraFieldId;
};

class ConfigManager {
public:
    // Optional base directory for config/views (e.g. exe directory). If set, paths are baseDir + filename.
    static void SetBaseDirectoryForFiles(const std::string& baseDir) {
        GetBaseDirectoryRef() = baseDir;
    }

    /** Directory used for config/views (trailing separator if set). Empty if unset. */
    static const std::string& GetFilesBaseDirectory() { return GetBaseDirectoryRef(); }

    static nlohmann::json LoadMergedConfigJson() {
        std::ifstream file(GetConfigPath());
        nlohmann::json j = nlohmann::json::object();
        if (file.is_open()) {
            try {
                file >> j;
            } catch (...) {
                j = nlohmann::json::object();
            }
        }
        if (!j.is_object()) {
            j = nlohmann::json::object();
        }
        return j;
    }

    static void WriteConfigJson(const nlohmann::json& j) {
        std::ofstream file(GetConfigPath());
        file << j.dump(4);
    }

    static void Save(const JiraConfig& config) {
        nlohmann::json j = LoadMergedConfigJson();
        j["domain"] = config.Domain;
        j["email"] = config.Email;
        j["token"] = config.ApiToken;
        j["project_key"] = config.ProjectKey;
        j["jql"] = config.JqlQuery;
        j["field_overflow_tooltips"] = config.EnableFieldOverflowTooltips;
        j["log_min_level"] = config.LogMinLevel;
        j["log_jira_http_bodies"] = config.LogJiraHttpBodies;
        j["log_p4_io"] = config.LogP4Io;
        WriteConfigJson(j);
    }

    static BlameAnalysisConfig LoadBlameAnalysis() {
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
        b.CallstackJiraFieldId = ba.value("callstack_jira_field_id", std::string());
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

    static void SaveBlameAnalysis(const BlameAnalysisConfig& b) {
        nlohmann::json j = LoadMergedConfigJson();
        nlohmann::json ba = nlohmann::json::object();
        ba["p4_exe"] = b.P4Executable;
        ba["p4vc_exe"] = b.P4VcExecutable;
        ba["timelapse_cmd"] = b.TimelapseCommandTemplate;
        ba["change_cmd"] = b.ChangeCommandTemplate;
        ba["ai_chat_url"] = b.AiChatUrl;
        ba["default_max_frames"] = b.DefaultMaxFrames;
        ba["cl_cache_max"] = b.ChangelistCacheMaxEntries;
        ba["callstack_jira_field_id"] = b.CallstackJiraFieldId;
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

    static JiraConfig Load() {
        nlohmann::json j = LoadMergedConfigJson();
        if (j.empty()) {
            return {};
        }

        try {
            JiraConfig cfg;
            cfg.Domain = j.value("domain", std::string{});
            cfg.Email = j.value("email", std::string{});
            cfg.ApiToken = j.value("token", std::string{});
            cfg.ProjectKey = j.value("project_key", std::string{});
            cfg.JqlQuery = j.value("jql", cfg.JqlQuery);
            cfg.EnableFieldOverflowTooltips = j.value("field_overflow_tooltips", cfg.EnableFieldOverflowTooltips);
            cfg.LogMinLevel = j.value("log_min_level", cfg.LogMinLevel);
            cfg.LogJiraHttpBodies = j.value("log_jira_http_bodies", cfg.LogJiraHttpBodies);
            cfg.LogP4Io = j.value("log_p4_io", cfg.LogP4Io);
            return cfg;
        } catch (...) {
            return {};
        }
    }

    static std::string GetConfigPath() {
        const std::string& base = GetBaseDirectoryRef();
        if (base.empty()) return "smatchet_config.json";
        return base + "smatchet_config.json";
    }

    static std::string GetViewsPath() {
        const std::string& base = GetBaseDirectoryRef();
        if (base.empty()) return "smatchet_views.json";
        return base + "smatchet_views.json";
    }

    static void SaveViews(const ViewsStore& store) {
        nlohmann::json j;
        j["version"] = store.Version;
        j["active_view_id"] = store.ActiveViewId;
        j["views"] = nlohmann::json::array();

        for (const auto& view : store.Views) {
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
            j["views"].push_back(viewJson);
        }

        const std::string viewsPath = GetViewsPath();
        std::ofstream file(viewsPath);
        file << j.dump(4);
    }

    static ViewsStore LoadViews() {
        std::ifstream file(GetViewsPath());
        if (!file.is_open()) {
            return {};
        }

        try {
            nlohmann::json j;
            file >> j;
            ViewsStore store;
        store.Version = j.value("version", 1);
        store.ActiveViewId = j.value("active_view_id", std::string());

        if (j.contains("views") && j["views"].is_array()) {
            for (const auto& viewJson : j["views"]) {
                if (!viewJson.is_object()) {
                    continue;
                }
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
                store.Views.push_back(std::move(view));
            }
        }
        return store;
        } catch (...) {
            return {};
        }
    }

    static ViewsStore LoadViewsOrBootstrap(const JiraConfig& cfg) {
        try {
            ViewsStore store = LoadViews();
            if (!store.Views.empty()) {
                if (store.ActiveViewId.empty()) {
                    store.ActiveViewId = store.Views.front().Id;
                }
                return store;
            }

            ViewDefinition defaultView;
        defaultView.Id = "default_view";
        defaultView.Name = "Default View";
        defaultView.Jql = cfg.JqlQuery.empty() ? std::string("assignee=currentUser()") : cfg.JqlQuery;
        // Basic fields for new installs: exclude id (handled as a special column).
        defaultView.Fields = {
            "summary",
            "assignee",
            "priority",
            "status",
            "created",
            "updated"
        };
        defaultView.ColumnOrder = {"id"};
        for (const auto& fieldId : defaultView.Fields) {
            defaultView.ColumnOrder.push_back("field:" + fieldId);
        }
        defaultView.ColumnWidths["id"] = 90.0f;

        store.Version = 1;
        store.ActiveViewId = defaultView.Id;
        store.Views.push_back(defaultView);
        SaveViews(store);
        return store;
        } catch (...) {
            ViewsStore fallback;
            fallback.Version = 1;
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
            fallback.ActiveViewId = defaultView.Id;
            fallback.Views.push_back(std::move(defaultView));
            return fallback;
        }
    }

private:
    static std::string& GetBaseDirectoryRef() {
        static std::string s;
        return s;
    }
};

#endif
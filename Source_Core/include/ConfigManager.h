#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

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

class ConfigManager {
public:
    // Optional base directory for config/views (e.g. exe directory). If set, paths are baseDir + filename.
    static void SetBaseDirectoryForFiles(const std::string& baseDir) {
        GetBaseDirectoryRef() = baseDir;
    }

    /** Directory used for config/views (trailing separator if set). Empty if unset. */
    static const std::string& GetFilesBaseDirectory() { return GetBaseDirectoryRef(); }

    static void Save(const JiraConfig& config) {
        nlohmann::json j;
        j["domain"] = config.Domain;
        j["email"] = config.Email;
        j["token"] = config.ApiToken;
        j["project_key"] = config.ProjectKey;
        j["jql"]   = config.JqlQuery;
        j["field_overflow_tooltips"] = config.EnableFieldOverflowTooltips;
        j["log_min_level"] = config.LogMinLevel;
        j["log_jira_http_bodies"] = config.LogJiraHttpBodies;

        std::ofstream file(GetConfigPath());
        file << j.dump(4);
    }

    static JiraConfig Load() {
        std::ifstream file(GetConfigPath());
        if (!file.is_open()) {
            return {};
        }

        try {
            nlohmann::json j;
            file >> j;

            JiraConfig cfg;
            cfg.Domain   = j.value("domain", std::string{});
            cfg.Email    = j.value("email", std::string{});
            cfg.ApiToken = j.value("token", std::string{});
            cfg.ProjectKey = j.value("project_key", std::string{});
            cfg.JqlQuery = j.value("jql", cfg.JqlQuery);
            cfg.EnableFieldOverflowTooltips = j.value("field_overflow_tooltips", cfg.EnableFieldOverflowTooltips);
            cfg.LogMinLevel = j.value("log_min_level", cfg.LogMinLevel);
            cfg.LogJiraHttpBodies = j.value("log_jira_http_bodies", cfg.LogJiraHttpBodies);
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
// ConfigManager_Views — view-store JSON persistence + the `CommentTemplate` ADL
// serializers declared as friends in `ConfigManager.h`.
// Split off `ConfigManager.cpp` per `docs/plans/shipped/large-files-and-phase-2.md` § A3.
// The shared filesystem / IO / lock helpers live in `ConfigManager_PathUtils.cpp`
// behind the declarations in `ConfigManager_Internal.h`.

#include "ConfigManager.h"
#include "UiThreadAffinity.h"
#include "ConfigManager_Internal.h"
#include "FileIo.h"

#include "Logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <exception>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

using smatchet::config_detail::GetIoMutexRef;
using smatchet::fileio::ScopedFileLock;

namespace {

// View-disk JSON helpers (previously the public `SmatchetViewsDiskDetail::*` namespace inline in
// ConfigManager.h). They had exactly one caller (ConfigManager.cpp) but every TU that included
// the header re-parsed them; moving them to anonymous namespace here drops that parse cost
// without changing behaviour.

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
    if (root.contains("toolbar_append") && root["toolbar_append"].is_array()) {
        for (const auto& btnJson : root["toolbar_append"]) {
            if (!btnJson.is_object()) {
                continue;
            }
            try {
                ws.ToolbarAppend.push_back(btnJson.get<ToolbarButton>());
            } catch (...) { // catch-all-ok: skip a malformed toolbar button, keep the rest
            }
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
    if (!ws.ToolbarAppend.empty()) {
        j["toolbar_append"] = ws.ToolbarAppend;
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
    if (backendKey == "GitHub") {
        // GitHub has no `priority` / `issuetype` / `description`-as-column —
        // seed only fields present in TrackerFieldCatalog for the GitHub
        // backend. Users add `pr.head` / `pr.base` / `pr.mergeable` / `pr.draft`
        // when their JQL opts into `type:pr`.
        ViewDefinition v;
        v.Id = "github_default_view";
        v.Name = "Default GitHub View";
        v.Jql = cfg.JqlQuery.empty() ? std::string("assignee=currentUser()") : cfg.JqlQuery;
        v.Fields = {"summary", "description", "status", "assignee", "labels", "author", "created", "updated"};
        v.ColumnOrder = {"id"};
        for (const auto& fieldId : v.Fields) {
            v.ColumnOrder.push_back("field:" + fieldId);
        }
        v.ColumnWidths["id"] = 90.0f;
        ws.ActiveViewId = v.Id;
        ws.Views.push_back(std::move(v));
        return ws;
    }
    if (backendKey == "Linear") {
        ViewDefinition v;
        v.Id = "linear_default_view";
        v.Name = "Default Linear View";
        v.Jql = cfg.JqlQuery.empty() ? std::string("assignee=currentUser()") : cfg.JqlQuery;
        v.Fields = {"summary",  "description", "status",  "assignee", "labels",
                    "priority", "project",     "created", "updated"};
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
    defaultView.Fields = {"summary",   "assignee",    "priority", "status",
                          "issuetype", "description", "created",  "updated"};
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

// CommentTemplate friend serializers — declared in ConfigManager.h, defined here so the slim
// header pulls only <nlohmann/json_fwd.hpp>. Lives in the global namespace because that's where
// the friend declaration injects them; nlohmann::adl_serializer finds them via ADL at call sites.

void to_json(nlohmann::json& j, const CommentTemplate& t) {
    j = nlohmann::json{{"id", t.Id}, {"title", t.Title}, {"text", t.Text}};
}

void from_json(const nlohmann::json& j, CommentTemplate& t) {
    t.Id = j.value("id", "");
    t.Title = j.value("title", "");
    t.Text = j.value("text", "");
}

// ConfigManager — view-store public methods.

std::string ConfigManager::NormalizeViewsBackendKey(const std::string& trackerType) {
    std::string t;
    t.resize(trackerType.size());
    std::transform(trackerType.begin(), trackerType.end(), t.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (t == "jira") {
        return "Jira";
    }
    if (t == "plane") {
        return "Plane";
    }
    if (t == "github") {
        return "GitHub";
    }
    if (t == "linear") {
        return "Linear";
    }
    // Unknown tracker type collapses into the default bucket — meaning it SHARES the
    // default backend's views / tickets_v2 / pending-queue namespace, the exact
    // cross-backend collision per-backend keying exists to prevent. Warn (for a
    // non-empty type) so adding a new backend without extending this map is visible,
    // not silent — wire each new backend into this map (and KnownBackendKeys() — the
    // ConfigManagerViews test asserts the two agree) as part of its bring-up checklist.
    // An empty type is a normal pre-config transient, so default it silently. Latch the
    // warning: this normalizer is reached from per-frame render paths, so a corrupt /
    // hand-edited unmapped TrackerType must not flood the log at frame rate. The atomic
    // exchange keeps it race-free (called from the UI thread + sync workers).
    static std::atomic<bool> warned{false};
    if (!trackerType.empty() && !warned.exchange(true)) {
        LOG_WARN("NormalizeViewsBackendKey: unmapped tracker type '%s' -> default bucket '%s'; "
                 "extend the map or it shares '%s' storage.",
                 trackerType.c_str(), SmatchetDefaults::kDefaultBackendType, SmatchetDefaults::kDefaultBackendType);
    }
    return SmatchetDefaults::kDefaultBackendType;
}

const std::vector<std::string>& ConfigManager::KnownBackendKeys() {
    static const std::vector<std::string> keys = {"Jira", "Plane", "GitHub", "Linear"};
    return keys;
}

bool ConfigManager::BackendCredentialsPresent(const TrackerConfig& cfg, const std::string& backendKey) {
    if (backendKey == "Plane") {
        return !cfg.PlaneUrl.empty() && !cfg.PlaneApiKey.empty() && !cfg.PlaneWorkspaceSlug.empty();
    }
    if (backendKey == "GitHub") {
        return !cfg.GitHubPat.empty() && !cfg.GitHubOwner.empty() && !cfg.GitHubRepo.empty();
    }
    if (backendKey == "Linear") {
        return !cfg.LinearApiKey.empty() && (!cfg.LinearTeamId.empty() || !cfg.LinearTeamKey.empty());
    }
    // "Jira" and any future default backend: Domain + ApiToken required.
    return !cfg.Domain.empty() && !cfg.ApiToken.empty();
}

PersistentViewsFile ConfigManager::LoadPersistentViewsFromDisk() {
    // Pillar-2 gate (close-gate-gaps Slice 1a): blocking (lock + ScopedFileLock + sync ifstream
    // + JSON parse) — must not run on the UI render thread (#611). Warn-only for now.
    UiThreadAffinity::WarnIfOnUiThread("ConfigManager::LoadPersistentViewsFromDisk");
    PersistentViewsFile disk;
    disk.Version = 2;
    const std::string viewsPath = GetViewsPath();
    // CPP_CODE_AUDIT.md #11: was a bare `ifstream >> j` (nlohmann's stream-extraction operator
    // drives the same recursive-descent parser as `json::parse`) — a deeply-nested
    // smatchet_views.json stack-overflows the recursive ~json teardown. LoadJsonFile is the
    // hardened sibling (bounded parse + 64 MiB read cap + its own locking — do NOT also take
    // GetIoMutexRef()/ScopedFileLock here, LoadJsonFile already does and the mutex is
    // non-recursive).
    try {
        const nlohmann::json j = LoadJsonFile(viewsPath);
        if (j.empty()) {
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
        return ViewWorkspaceToViewsStoreImpl(MakeDefaultViewWorkspaceForBackend(key, cfg));
    } catch (...) {
        LOG_ERROR("ConfigManager: LoadViewsOrBootstrap error (unknown)");
        const std::string key = NormalizeViewsBackendKey(cfg.TrackerType);
        return ViewWorkspaceToViewsStoreImpl(MakeDefaultViewWorkspaceForBackend(key, cfg));
    }
}

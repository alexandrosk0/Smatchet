// ConfigManager_Panes — smatchet_panes.json persistence (multi-grid-tabs Slice 2,
// plan item 13, ADR-0018). Stores the ORDERED grid-pane identity list
// [{id,title,backendKey,viewId}] + focusedPaneId. Mirrors ConfigManager_Views.cpp:
// io-mutex + ScopedFileLock around reads/writes, AtomicWriteTextFile for the
// crash-safe write. Dock geometry deliberately does NOT live here — it rides
// ImGui's .ini. Default bootstrap derives exactly one pane from cfg.TrackerType +
// that backend's active view, so existing single-grid installs need no migration.

#include "ConfigManager.h"
#include "ConfigManager_Internal.h"

#include "Logger.h"

#include <nlohmann/json.hpp>

#include <exception>
#include <fstream>
#include <mutex>
#include <string>

using smatchet::config_detail::GetIoMutexRef;
using smatchet::config_detail::ScopedFileLock;

namespace {

GridPaneDescriptor ParsePaneDescriptor(const nlohmann::json& paneJson) {
    GridPaneDescriptor pane;
    pane.Id = paneJson.value("id", std::string());
    pane.Title = paneJson.value("title", std::string());
    pane.BackendKey = paneJson.value("backend_key", std::string());
    pane.ViewId = paneJson.value("view_id", std::string());
    return pane;
}

nlohmann::json SerializePaneDescriptor(const GridPaneDescriptor& pane) {
    nlohmann::json paneJson;
    paneJson["id"] = pane.Id;
    paneJson["title"] = pane.Title;
    paneJson["backend_key"] = pane.BackendKey;
    paneJson["view_id"] = pane.ViewId;
    return paneJson;
}

} // namespace

std::string ConfigManager::GetPanesPath() {
    const std::string& base = GetUserDataDirectory();
    if (base.empty()) {
        return "smatchet_panes.json";
    }
    return base + "smatchet_panes.json";
}

PersistentPanesFile ConfigManager::LoadPanesFromDisk() {
    PersistentPanesFile disk;
    const std::string panesPath = GetPanesPath();
    // CPP_CODE_AUDIT.md #11: was a bare `ifstream >> j` (nlohmann's stream-extraction operator
    // drives the same recursive-descent parser as `json::parse`) — a deeply-nested
    // smatchet_panes.json stack-overflows the recursive ~json teardown. LoadJsonFile is the
    // hardened sibling (bounded parse + 64 MiB read cap + its own locking — do NOT also take
    // GetIoMutexRef()/ScopedFileLock here, LoadJsonFile already does and the mutex is
    // non-recursive).
    try {
        const nlohmann::json j = LoadJsonFile(panesPath);
        if (j.empty()) {
            return disk;
        }
        disk.Version = j.value("version", 1);
        if (disk.Version > 1) {
            LOG_WARN("ConfigManager: panes file '%s' has version %d (this build writes 1) — fields a newer "
                     "build wrote may be dropped on the next save",
                     panesPath.c_str(), disk.Version);
        }
        disk.FocusedPaneId = j.value("focused_pane_id", std::string());
        if (j.contains("panes") && j["panes"].is_array()) {
            for (const auto& paneJson : j["panes"]) {
                if (!paneJson.is_object()) {
                    continue;
                }
                GridPaneDescriptor pane = ParsePaneDescriptor(paneJson);
                if (pane.Id.empty()) {
                    continue; // id is the pane's primary key — skip malformed rows, keep the rest
                }
                disk.Panes.push_back(pane);
            }
        }
        if (disk.FocusedPaneId.empty() && !disk.Panes.empty()) {
            disk.FocusedPaneId = disk.Panes.front().Id;
        }
        return disk;
    } catch (const std::exception& ex) {
        LOG_ERROR("ConfigManager: failed to parse panes '%s': %s", panesPath.c_str(), ex.what());
        return PersistentPanesFile();
    } catch (...) {
        LOG_ERROR("ConfigManager: failed to parse panes '%s' with unknown exception", panesPath.c_str());
        return PersistentPanesFile();
    }
}

void ConfigManager::SavePanesToDisk(const PersistentPanesFile& disk) {
    const std::string panesPath = GetPanesPath();
    nlohmann::json j;
    j["version"] = 1;
    j["focused_pane_id"] = disk.FocusedPaneId;
    j["panes"] = nlohmann::json::array();
    for (const auto& pane : disk.Panes) {
        if (pane.Id.empty()) {
            continue;
        }
        j["panes"].push_back(SerializePaneDescriptor(pane));
    }
    std::lock_guard<std::mutex> lock(GetIoMutexRef());
    ScopedFileLock fileLock(panesPath);
    const std::string content = j.dump(4);
    if (!AtomicWriteTextFile(panesPath, content)) {
        LOG_ERROR("ConfigManager: atomic write failed for panes file '%s'", panesPath.c_str());
    }
}

PersistentPanesFile ConfigManager::LoadPanesOrBootstrap(const TrackerConfig& cfg) {
    try {
        PersistentPanesFile disk = LoadPanesFromDisk();
        if (!disk.Panes.empty()) {
            return disk;
        }
        // Zero-migration default: one pane bound to the configured backend's
        // active view (bootstrapping the views bucket if this is a fresh install).
        const ViewsStore views = LoadViewsOrBootstrap(cfg);
        GridPaneDescriptor pane;
        pane.Id = "main";
        pane.BackendKey = NormalizeViewsBackendKey(cfg.TrackerType);
        pane.ViewId = views.ActiveViewId;
        pane.Title = "Grid";
        for (const auto& view : views.Views) {
            if (view.Id == views.ActiveViewId) {
                pane.Title = view.Name;
                break;
            }
        }
        disk.Panes.push_back(pane);
        disk.FocusedPaneId = pane.Id;
        SavePanesToDisk(disk);
        return disk;
    } catch (const std::exception& ex) {
        LOG_ERROR("ConfigManager: LoadPanesOrBootstrap error: %s", ex.what());
    } catch (...) {
        LOG_ERROR("ConfigManager: LoadPanesOrBootstrap error (unknown)");
    }
    // Last-resort in-memory default (never an empty pane list — Pillar 3).
    PersistentPanesFile fallback;
    GridPaneDescriptor pane;
    pane.Id = "main";
    pane.BackendKey = NormalizeViewsBackendKey(cfg.TrackerType);
    pane.Title = "Grid";
    fallback.Panes.push_back(pane);
    fallback.FocusedPaneId = pane.Id;
    return fallback;
}

#pragma once

#include <cctype>
#include <string>
#include <vector>
#include <algorithm>
#include "ConfigManager.h"

class Views {
  public:
    void EnsureLoaded(const TrackerConfig& cfg) {
        if (!Loaded) {
            Disk = ConfigManager::LoadPersistentViewsFromDisk();
            Loaded = true;
        }
        ApplyTrackerFromConfig(cfg);
    }

    const ViewsStore& GetStore() const { return Slice_; }
    ViewsStore& GetStoreMutable() { return Slice_; }

    const ViewDefinition* GetActiveView() const {
        const auto it =
            std::find_if(Slice_.Views.begin(), Slice_.Views.end(),
                         [&](const ViewDefinition& view) { return view.Id == Slice_.ActiveViewId; });
        return it != Slice_.Views.end() ? &*it : nullptr;
    }

    ViewDefinition* GetActiveViewMutable() {
        auto it = std::find_if(Slice_.Views.begin(), Slice_.Views.end(),
                               [&](const ViewDefinition& view) { return view.Id == Slice_.ActiveViewId; });
        return it != Slice_.Views.end() ? &*it : nullptr;
    }

    bool Activate(const std::string& viewId) {
        auto it = std::find_if(Slice_.Views.begin(), Slice_.Views.end(),
                               [&](const ViewDefinition& v) { return v.Id == viewId; });
        if (it == Slice_.Views.end()) {
            return false;
        }
        Slice_.ActiveViewId = viewId;
        Save();
        return true;
    }

    bool Create(const ViewDefinition& prototype) {
        ViewDefinition created = prototype;
        if (created.Id.empty()) {
            created.Id = BuildIdFromName(created.Name.empty() ? std::string("View") : created.Name);
        }
        if (created.Name.empty()) {
            created.Name = created.Id;
        }
        if (Exists(created.Id)) {
            created.Id = BuildUniqueId(created.Id);
        }
        if (created.ColumnOrder.empty()) {
            created.ColumnOrder = {"id"};
            for (const auto& fieldId : created.Fields) {
                created.ColumnOrder.push_back("field:" + fieldId);
            }
        }
        Slice_.Views.push_back(std::move(created));
        Slice_.ActiveViewId = Slice_.Views.back().Id;
        Save();
        return true;
    }

    bool UpdateActive(const ViewDefinition& updated) {
        ViewDefinition* active = GetActiveViewMutable();
        if (!active) {
            return false;
        }
        std::string preservedId = active->Id;
        *active = updated;
        active->Id = preservedId;
        if (active->Name.empty()) {
            active->Name = active->Id;
        }
        Save();
        return true;
    }

    bool DeleteActive() {
        if (Slice_.Views.size() <= 1) {
            return false;
        }
        auto it = std::remove_if(Slice_.Views.begin(), Slice_.Views.end(),
                                 [&](const ViewDefinition& v) { return v.Id == Slice_.ActiveViewId; });
        if (it == Slice_.Views.end()) {
            return false;
        }
        Slice_.Views.erase(it, Slice_.Views.end());
        if (!Slice_.Views.empty()) {
            Slice_.ActiveViewId = Slice_.Views.front().Id;
        } else {
            Slice_.ActiveViewId.clear();
        }
        Save();
        return true;
    }

    void Save() {
        if (!Loaded || !HasActiveBackend) {
            return;
        }
        ConfigManager::ViewsStoreToViewWorkspace(Slice_, Disk.Backends[ActiveBackendKey]);
        ConfigManager::SavePersistentViewsToDisk(Disk);
    }

  private:
    void ApplyTrackerFromConfig(const TrackerConfig& cfg) {
        const std::string newKey = ConfigManager::NormalizeViewsBackendKey(cfg.TrackerType);
        if (HasActiveBackend && newKey == ActiveBackendKey) {
            if (Slice_.Views.empty()) {
                bool dirty = false;
                ConfigManager::EnsureViewBucketBootstrapped(Disk, ActiveBackendKey, cfg, dirty);
                if (dirty) {
                    Slice_ = ConfigManager::ViewWorkspaceToViewsStore(Disk.Backends[ActiveBackendKey]);
                    ConfigManager::SavePersistentViewsToDisk(Disk);
                }
            }
            return;
        }
        if (HasActiveBackend && newKey != ActiveBackendKey) {
            ConfigManager::ViewsStoreToViewWorkspace(Slice_, Disk.Backends[ActiveBackendKey]);
        }
        ActiveBackendKey = newKey;
        HasActiveBackend = true;
        bool dirty = false;
        ConfigManager::EnsureViewBucketBootstrapped(Disk, ActiveBackendKey, cfg, dirty);
        if (dirty) {
            ConfigManager::SavePersistentViewsToDisk(Disk);
        }
        Slice_ = ConfigManager::ViewWorkspaceToViewsStore(Disk.Backends[ActiveBackendKey]);
    }

    static std::string BuildIdFromName(const std::string& name) {
        std::string id;
        id.reserve(name.size());
        for (char ch : name) {
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) {
                id.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
            } else if (ch == ' ' || ch == '-' || ch == '_') {
                id.push_back('_');
            }
        }
        if (id.empty()) {
            id = "view";
        }
        return id;
    }

    bool Exists(const std::string& id) const {
        return std::find_if(Slice_.Views.begin(), Slice_.Views.end(),
                            [&](const ViewDefinition& v) { return v.Id == id; }) != Slice_.Views.end();
    }

    std::string BuildUniqueId(const std::string& base) const {
        int suffix = 2;
        std::string candidate = base;
        while (Exists(candidate)) {
            candidate = base + "_" + std::to_string(suffix++);
        }
        return candidate;
    }

  private:
    bool Loaded = false;
    bool HasActiveBackend = false;
    std::string ActiveBackendKey;
    PersistentViewsFile Disk;
    ViewsStore Slice_;
};







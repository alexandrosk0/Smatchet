#include "Views.h"

#include <algorithm>
#include <cctype>

void Views::EnsureLoaded(const TrackerConfig& cfg) {
    if (!Loaded) {
        Disk = ConfigManager::LoadPersistentViewsFromDisk();
        Loaded = true;
    }
    ApplyTrackerFromConfig(cfg);
}

const ViewDefinition* Views::GetActiveView() const {
    const auto it =
        std::find_if(Slice_.Views.begin(), Slice_.Views.end(),
                     [&](const ViewDefinition& view) { return view.Id == Slice_.ActiveViewId; });
    return it != Slice_.Views.end() ? &*it : nullptr;
}

ViewDefinition* Views::GetActiveViewMutable() {
    auto it = std::find_if(Slice_.Views.begin(), Slice_.Views.end(),
                           [&](const ViewDefinition& view) { return view.Id == Slice_.ActiveViewId; });
    return it != Slice_.Views.end() ? &*it : nullptr;
}

bool Views::Activate(const std::string& viewId) {
    auto it = std::find_if(Slice_.Views.begin(), Slice_.Views.end(),
                           [&](const ViewDefinition& v) { return v.Id == viewId; });
    if (it == Slice_.Views.end()) {
        return false;
    }
    Slice_.ActiveViewId = viewId;
    Revision_.fetch_add(1);
    Save();
    return true;
}

bool Views::Create(const ViewDefinition& prototype) {
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
    Revision_.fetch_add(1);
    Save();
    return true;
}

bool Views::UpdateActive(const ViewDefinition& updated) {
    ViewDefinition* active = GetActiveViewMutable();
    if (!active) {
        return false;
    }
    const std::string preservedId = active->Id;
    *active = updated;
    active->Id = preservedId;
    if (active->Name.empty()) {
        active->Name = active->Id;
    }
    Revision_.fetch_add(1);
    Save();
    return true;
}

bool Views::DeleteActive() {
    return Delete(Slice_.ActiveViewId);
}

bool Views::Delete(const std::string& id) {
    if (Slice_.Views.size() <= 1) {
        return false;
    }
    const auto it = std::find_if(Slice_.Views.begin(), Slice_.Views.end(),
                                 [&](const ViewDefinition& v) { return v.Id == id; });
    if (it == Slice_.Views.end()) {
        return false;
    }
    // Capture the erased index BEFORE erase so we can pick the neighbour as the new active view
    // (UX nit: deleting the last view used to jump back to the first).
    const bool wasActive = (id == Slice_.ActiveViewId);
    const std::size_t erasedIndex = static_cast<std::size_t>(it - Slice_.Views.begin());
    Slice_.Views.erase(it);
    if (wasActive) {
        if (!Slice_.Views.empty()) {
            const std::size_t pickIndex =
                smatchet::views_merge::PickActiveViewIndexAfterErase(erasedIndex, Slice_.Views.size());
            Slice_.ActiveViewId = Slice_.Views[pickIndex].Id;
        } else {
            Slice_.ActiveViewId.clear();
        }
    }
    Revision_.fetch_add(1);
    Save();
    return true;
}

void Views::Save() {
    if (!Loaded || !HasActiveBackend) {
        return;
    }
    ConfigManager::ViewsStoreToViewWorkspace(Slice_, Disk.Backends[ActiveBackendKey]);
    // DR13a: re-read the file and fold in any out-of-band ToolbarAppend writes (the toolbar
    // editor's tracker-scope save worker writes ToolbarAppend straight to disk, bypassing this
    // wrapper). Without this merge, rewriting from our boot-time snapshot reverts those toolbar
    // buttons — they vanish on the next restart / backend swap.
    PersistentViewsFile fresh = ConfigManager::LoadPersistentViewsFromDisk();
    smatchet::views_merge::MergePersistentViewsToolbarAppend(Disk, fresh);
    ConfigManager::SavePersistentViewsToDisk(Disk);
}

void Views::ApplyTrackerFromConfig(const TrackerConfig& cfg) {
    const std::string newKey = ConfigManager::NormalizeViewsBackendKey(cfg.TrackerType);
    if (HasActiveBackend && newKey == ActiveBackendKey) {
        if (Slice_.Views.empty()) {
            bool dirty = false;
            ConfigManager::EnsureViewBucketBootstrapped(Disk, ActiveBackendKey, cfg, dirty);
            if (dirty) {
                Slice_ = ConfigManager::ViewWorkspaceToViewsStore(Disk.Backends[ActiveBackendKey]);
                Revision_.fetch_add(1);
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
    Revision_.fetch_add(1);
}

std::string Views::BuildIdFromName(const std::string& name) {
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

bool Views::Exists(const std::string& id) const {
    return std::find_if(Slice_.Views.begin(), Slice_.Views.end(),
                        [&](const ViewDefinition& v) { return v.Id == id; }) != Slice_.Views.end();
}

std::string Views::BuildUniqueId(const std::string& base) const {
    int suffix = 2;
    std::string candidate = base;
    while (Exists(candidate)) {
        candidate = base + "_" + std::to_string(suffix++);
    }
    return candidate;
}

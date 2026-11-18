#include "RecentSelections.h"

#include "ConfigSaveWorker.h"
#include "Logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <exception>
#include <mutex>
#include <utility>

namespace smatchet {
namespace recents {

const char* const kConfigJsonKey = "recent_selections";

namespace {

bool IsStorableKey(const std::string& s) { return !s.empty() && s.size() <= kMaxKeyBytes; }

} // namespace

bool RecentSelectionStore::Record(const std::string& scope, const std::string& id) {
    if (!IsStorableKey(scope) || !IsStorableKey(id)) {
        return false;
    }
    auto scopeIt = std::find_if(scopes_.begin(), scopes_.end(), [&scope](const Scope& s) { return s.Key == scope; });
    bool changed = false;
    if (scopeIt == scopes_.end()) {
        Scope fresh;
        fresh.Key = scope;
        scopes_.insert(scopes_.begin(), std::move(fresh));
        changed = true;
    } else if (scopeIt != scopes_.begin()) {
        std::rotate(scopes_.begin(), scopeIt, scopeIt + 1); // touched scope moves to the front
        changed = true;
    }
    std::vector<std::string>& ids = scopes_.front().Ids;
    auto idIt = std::find(ids.begin(), ids.end(), id);
    if (idIt == ids.end()) {
        ids.insert(ids.begin(), id);
        changed = true;
    } else if (idIt != ids.begin()) {
        std::rotate(ids.begin(), idIt, idIt + 1); // promote
        changed = true;
    }
    if (ids.size() > kMaxPerScope) {
        ids.resize(kMaxPerScope);
    }
    if (scopes_.size() > kMaxScopes) {
        scopes_.resize(kMaxScopes);
    }
    return changed;
}

std::vector<std::string> RecentSelectionStore::Get(const std::string& scope) const {
    for (const Scope& s : scopes_) {
        if (s.Key == scope) {
            return s.Ids;
        }
    }
    return {};
}

nlohmann::json RecentSelectionStore::ToJson() const {
    nlohmann::json out = nlohmann::json::array();
    for (const Scope& s : scopes_) {
        nlohmann::json entry = nlohmann::json::object();
        entry["scope"] = s.Key;
        entry["ids"] = s.Ids;
        out.push_back(std::move(entry));
    }
    return out;
}

void RecentSelectionStore::FromJson(const nlohmann::json& j) {
    scopes_.clear();
    if (!j.is_array()) {
        return;
    }
    for (const nlohmann::json& entry : j) {
        if (scopes_.size() >= kMaxScopes) {
            break;
        }
        if (!entry.is_object()) {
            continue;
        }
        const auto keyIt = entry.find("scope");
        const auto idsIt = entry.find("ids");
        if (keyIt == entry.end() || idsIt == entry.end() || !keyIt->is_string() || !idsIt->is_array()) {
            continue;
        }
        Scope scope;
        scope.Key = keyIt->get<std::string>();
        if (!IsStorableKey(scope.Key) ||
            std::any_of(scopes_.begin(), scopes_.end(), [&scope](const Scope& s) { return s.Key == scope.Key; })) {
            continue;
        }
        for (const nlohmann::json& idJson : *idsIt) {
            if (scope.Ids.size() >= kMaxPerScope) {
                break;
            }
            if (!idJson.is_string()) {
                continue;
            }
            std::string id = idJson.get<std::string>();
            if (IsStorableKey(id) && std::find(scope.Ids.begin(), scope.Ids.end(), id) == scope.Ids.end()) {
                scope.Ids.push_back(std::move(id));
            }
        }
        if (!scope.Ids.empty()) {
            scopes_.push_back(std::move(scope));
        }
    }
}

std::string MakeScopeKey(const std::string& backendKey, const std::string& fieldId) {
    return backendKey + "|" + fieldId;
}

namespace {

struct SharedState {
    std::mutex Mtx;
    RecentSelectionStore Store;
    bool Persist = false;
    // BeginSharedTestScope / EndSharedTestScope: what the store held before the tests took it over.
    bool InTestScope = false;
    RecentSelectionStore SetAside;
    bool SetAsidePersist = false;
};

SharedState& Shared() {
    static SharedState state;
    return state;
}

} // namespace

void LoadSharedFromConfigJson(const nlohmann::json& mergedConfig) {
    RecentSelectionStore loaded;
    try {
        if (mergedConfig.is_object()) {
            const auto it = mergedConfig.find(kConfigJsonKey);
            if (it != mergedConfig.end()) {
                if (!it->is_array()) {
                    LOG_WARN("RecentSelections: config key '%s' is not an array; starting empty", kConfigJsonKey);
                }
                loaded.FromJson(*it);
            }
        }
    } catch (const std::exception& ex) {
        LOG_WARN("RecentSelections: could not read config key '%s': %s", kConfigJsonKey, ex.what());
        loaded.Clear();
    }
    SharedState& shared = Shared();
    std::lock_guard<std::mutex> lk(shared.Mtx);
    shared.Store = std::move(loaded);
    shared.Persist = true;
}

void RecordAndPersist(const std::string& scope, const std::string& id) {
    nlohmann::json snapshot;
    bool save = false;
    {
        SharedState& shared = Shared();
        std::lock_guard<std::mutex> lk(shared.Mtx);
        if (shared.Store.Record(scope, id) && shared.Persist) {
            snapshot = shared.Store.ToJson();
            save = true;
        }
    }
    if (!save) {
        return;
    }
    try {
        // Coalesced on the config-save worker: the frame thread never touches the disk for a pick.
        smatchet::config_save::EnqueueConfigJsonKey(kConfigJsonKey, snapshot);
    } catch (const std::exception& ex) {
        LOG_WARN("RecentSelections: could not queue the save: %s", ex.what());
    }
}

std::vector<std::string> SharedGet(const std::string& scope) {
    SharedState& shared = Shared();
    std::lock_guard<std::mutex> lk(shared.Mtx);
    return shared.Store.Get(scope);
}

void ResetSharedForTests() {
    SharedState& shared = Shared();
    std::lock_guard<std::mutex> lk(shared.Mtx);
    shared.Store.Clear();
    shared.Persist = false;
}

void BeginSharedTestScope() {
    SharedState& shared = Shared();
    std::lock_guard<std::mutex> lk(shared.Mtx);
    if (!shared.InTestScope) {
        shared.SetAside = std::move(shared.Store);
        shared.SetAsidePersist = shared.Persist;
        shared.InTestScope = true;
    }
    shared.Store.Clear();
    shared.Persist = false;
}

void EndSharedTestScope() {
    SharedState& shared = Shared();
    std::lock_guard<std::mutex> lk(shared.Mtx);
    if (!shared.InTestScope) {
        return;
    }
    shared.Store = std::move(shared.SetAside);
    shared.SetAside.Clear();
    shared.Persist = shared.SetAsidePersist;
    shared.InTestScope = false;
}

} // namespace recents
} // namespace smatchet

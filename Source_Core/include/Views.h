#pragma once

#include <cctype>
#include <string>
#include <vector>
#include <algorithm>
#include "ConfigManager.h"

class Views {
  public:
    void EnsureLoaded(const JiraConfig& cfg) {
        if (Loaded) {
            return;
        }
        Store = ConfigManager::LoadViewsOrBootstrap(cfg);
        if (Store.ActiveViewId.empty() && !Store.Views.empty()) {
            Store.ActiveViewId = Store.Views.front().Id;
        }
        Loaded = true;
    }

    const ViewsStore& GetStore() const { return Store; }
    ViewsStore& GetStoreMutable() { return Store; }

    const ViewDefinition* GetActiveView() const {
        for (const auto& view : Store.Views) {
            if (view.Id == Store.ActiveViewId) {
                return &view;
            }
        }
        return nullptr;
    }

    ViewDefinition* GetActiveViewMutable() {
        for (auto& view : Store.Views) {
            if (view.Id == Store.ActiveViewId) {
                return &view;
            }
        }
        return nullptr;
    }

    bool Activate(const std::string& viewId) {
        auto it = std::find_if(Store.Views.begin(), Store.Views.end(),
                               [&](const ViewDefinition& v) { return v.Id == viewId; });
        if (it == Store.Views.end()) {
            return false;
        }
        Store.ActiveViewId = viewId;
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
        Store.Views.push_back(std::move(created));
        Store.ActiveViewId = Store.Views.back().Id;
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
        if (Store.Views.size() <= 1) {
            return false;
        }
        auto it = std::remove_if(Store.Views.begin(), Store.Views.end(),
                                 [&](const ViewDefinition& v) { return v.Id == Store.ActiveViewId; });
        if (it == Store.Views.end()) {
            return false;
        }
        Store.Views.erase(it, Store.Views.end());
        if (!Store.Views.empty()) {
            Store.ActiveViewId = Store.Views.front().Id;
        } else {
            Store.ActiveViewId.clear();
        }
        Save();
        return true;
    }

    void Save() { ConfigManager::SaveViews(Store); }

  private:
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
        return std::find_if(Store.Views.begin(), Store.Views.end(),
                            [&](const ViewDefinition& v) { return v.Id == id; }) != Store.Views.end();
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
    ViewsStore Store;
};

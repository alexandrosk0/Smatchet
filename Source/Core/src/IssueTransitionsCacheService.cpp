#include "IssueTransitionsCacheService.h"

#include "IEditMetaDeps.h"
#include "ITrackerBackend.h"
#include "ITrackerFieldCatalog.h"
#include "Config/ConfigManager.h"
#include "Logger.h"

#include <memory>
#include <mutex>
#include <string>

IssueTransitionsCacheService::IssueTransitionsCacheService(IEditMetaDeps& deps) : deps_(deps) {}

TransitionsLookup IssueTransitionsCacheService::GetAvailableTransitions(const std::string& issueId) const {
    if (issueId.empty()) {
        return TransitionsLookup();
    }
    std::lock_guard<std::mutex> lock(issueTransitionsMutex_);
    const auto it = issueTransitions_.find(issueId);
    if (it != issueTransitions_.end()) {
        TransitionsLookup result;
        result.applicable = it->second.applicable;
        result.loaded = it->second.loaded;
        result.options = it->second.options;
        return result;
    }
    return TransitionsLookup();
}

void IssueTransitionsCacheService::EnsureIssueTransitionsLoaded(const std::string& issueId,
                                                                const TrackerConfig* configSnapshot) {
    if (issueId.empty()) {
        return;
    }
    bool shouldFetch = false;
    {
        std::lock_guard<std::mutex> lock(issueTransitionsMutex_);
        const auto it = issueTransitions_.find(issueId);
        if (it == issueTransitions_.end() || !it->second.loaded) {
            // Mark as loading to prevent concurrent fetches (TOCTOU guard).
            issueTransitions_[issueId].loaded = false;
            shouldFetch = true;
        }
    }
    if (!shouldFetch) {
        return;
    }
    std::shared_ptr<ITrackerBackend> backend = deps_.BackendShared();
    if (!backend) {
        return;
    }
    TrackerConfig cfg = configSnapshot ? *configSnapshot : ConfigManager::Load();
    deps_.LaunchBackgroundTask([this, issueId, cfg, backend]() mutable {
        const auto result = backend->FieldCatalog()->FetchIssueTransitions(cfg, issueId);
        {
            std::lock_guard<std::mutex> lock(issueTransitionsMutex_);
            IssueTransitionsCache& cache = issueTransitions_[issueId];
            if (result.has_value()) {
                cache.applicable = true;
                cache.loaded = true;
                cache.options = result.value();
            } else {
                cache.applicable = false;
                cache.loaded = true;
                LOG_WARN("IssueTransitionsCacheService: failed to fetch transitions for issue %s: %s", issueId.c_str(),
                         result.error().Detail.c_str());
            }
        }
    });
}

void IssueTransitionsCacheService::InvalidateIssueTransitions(const std::string& issueId) {
    if (issueId.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(issueTransitionsMutex_);
    issueTransitions_.erase(issueId);
}

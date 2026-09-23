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
        result.loaded = it->second.loaded && !it->second.inFlight; // loaded only when fetch complete
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
    uint64_t capturedGen = 0;
    {
        std::lock_guard<std::mutex> lock(issueTransitionsMutex_);
        auto& e = issueTransitions_[issueId];
        if (!e.loaded && !e.inFlight) {
            // Mark as loading to prevent concurrent fetches (TOCTOU guard).
            e.gen = nextGen_++;
            capturedGen = e.gen;
            e.inFlight = true;
            shouldFetch = true;
        }
    }
    if (!shouldFetch) {
        return;
    }
    std::shared_ptr<ITrackerBackend> backend = deps_.BackendShared();
    ITrackerFieldCatalog* catalog = nullptr;
    if (backend) {
        catalog = backend->FieldCatalog(); // nullable if unsupported
    }
    if (!backend || !catalog) {
        std::lock_guard<std::mutex> lock(issueTransitionsMutex_);
        const auto it = issueTransitions_.find(issueId);
        if (it != issueTransitions_.end() && it->second.gen == capturedGen) {
            it->second.loaded = true;
            it->second.applicable = false;
            it->second.inFlight = false;
        }
        return;
    }
    TrackerConfig cfg = configSnapshot ? *configSnapshot : ConfigManager::Load();
    deps_.LaunchBackgroundTask([this, issueId, cfg, backend, catalog, capturedGen]() mutable {
        const auto result = catalog->FetchIssueTransitions(cfg, issueId);
        {
            std::lock_guard<std::mutex> lock(issueTransitionsMutex_);
            const auto it = issueTransitions_.find(issueId);
            if (it != issueTransitions_.end() && it->second.gen == capturedGen) {
                if (result.has_value()) {
                    it->second.applicable = true;
                    it->second.loaded = true;
                    it->second.options = result.value();
                } else {
                    it->second.applicable = false;
                    it->second.loaded = true;
                    LOG_WARN("IssueTransitionsCacheService: failed to fetch transitions for issue %s: %s",
                             issueId.c_str(), result.error().Detail.c_str());
                }
                it->second.inFlight = false;
            } else if (it != issueTransitions_.end()) {
                // Invalidated and re-requested: drop this stale result
                LOG_DEBUG("IssueTransitionsCacheService: dropped stale fetch for issue %s (gen mismatch)",
                          issueId.c_str());
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

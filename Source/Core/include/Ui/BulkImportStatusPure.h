#pragma once

// BulkImportStatusPure — the Status column of Bulk Import as data (pure: no ImGui, test-linkable). A row is
// waiting to be sent ("queued", "waiting for cache…", "submitting..."); done — created / updated ("ok KEY"),
// skipped, or saved to the offline queue because the tracker could not be reached ("queued offline #<id>",
// Quality Pillar 6); or failed / stopped (any other text).

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace smatchet {
namespace ui {
namespace bulkimport {

constexpr const char* kQueuedStatus = "queued"; ///< waiting to be sent — not the offline queue
constexpr const char* kWaitingForCacheStatus = "waiting for cache\xE2\x80\xA6";
constexpr const char* kSubmittingStatus = "submitting...";
constexpr const char* kQueuedOfflinePrefix = "queued offline #";

/// The status of a row saved to the offline queue as row `queueId`.
inline std::string QueuedOfflineStatus(std::int64_t queueId) { return kQueuedOfflinePrefix + std::to_string(queueId); }

/// True once a row has left the dispatch pipeline (done, failed, skipped, stopped or a parse error).
inline bool IsStatusTerminal(const std::string& status) {
    return !status.empty() && status != kQueuedStatus && status != kWaitingForCacheStatus &&
           status != kSubmittingStatus;
}

/// True when a re-run must not send the row again: created / updated, skipped, or saved to the offline
/// queue (the queue sends it on reconnect; sending it again would create the issue twice).
inline bool IsStatusHandedOff(const std::string& status) {
    return status.rfind("ok", 0) == 0 || status.rfind("skipped", 0) == 0 || status.rfind(kQueuedOfflinePrefix, 0) == 0;
}

/// Match completed handoffs by the entire serialized draft, independent of row order. Consume each
/// match once so adding an identical row still leaves that additional draft eligible for submission.
inline std::vector<std::string> PreserveHandedOffStatuses(const std::vector<std::string>& previousDrafts,
                                                          const std::vector<std::string>& previousStatuses,
                                                          const std::vector<std::string>& drafts) {
    std::map<std::string, std::deque<std::string>> handedOff;
    for (std::size_t i = 0; i < previousDrafts.size() && i < previousStatuses.size(); ++i) {
        if (IsStatusHandedOff(previousStatuses[i])) {
            handedOff[previousDrafts[i]].push_back(previousStatuses[i]);
        }
    }
    std::vector<std::string> statuses(drafts.size());
    for (std::size_t i = 0; i < drafts.size(); ++i) {
        auto found = handedOff.find(drafts[i]);
        if (found != handedOff.end() && !found->second.empty()) {
            statuses[i] = std::move(found->second.front());
            found->second.pop_front();
        }
    }
    return statuses;
}

} // namespace bulkimport
} // namespace ui
} // namespace smatchet

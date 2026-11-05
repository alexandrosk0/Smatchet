#pragma once

// LeftViewHistoryService — the history of issues that left a view (docs/plans/left-view-history-grid.md).
//
// The UI thread reads an in-memory newest-first snapshot; SQLite and tracker calls run on workers (Pillar 2). All
// cache work goes through one serial queue, in call order: reading a saved listing, diffing it and writing the
// result back must not interleave with another pane's turn on the same view. Each departure and the listing that
// proves it commit in one transaction, before any tracker call, so a quit during the reason lookup loses only the
// reason. No request is sent while offline, and a failed lookup keeps the row (Pillar 6).
//
// Threading: public methods are UI-thread only, except Flush.

#include "ConfigManager.h"
#include "ITrackerIssueReader.h"
#include "LeftViewRecord.h"
#include "SerialTaskQueue.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class LocalCacheManager;

namespace smatchet {

/// The pane's view at the moment departures were found.
struct LeftViewCapture {
    std::string backendKey;
    std::string paneId;
    std::string viewId;
    std::string viewName;
    std::string query;
    TrackerConfig cfg;
    std::shared_ptr<ITrackerIssueReader> reader;
    std::unordered_map<std::string, std::string> fieldLabels; ///< Field id -> display name; may be empty.
};

/// An issue the pane no longer shows, with its row as the pane last held it (empty when it held none).
struct LeftViewDeparture {
    std::string issueKey;
    bool deleted = false;
    std::unordered_map<std::string, std::string> fieldValues;
};

/// Live values for the details view. Saved values stay on the row, so a failed read loses nothing.
struct LeftViewDetails {
    enum class Status { Idle, Loading, Live, Failed, Offline, NotConnected };
    std::int64_t recordId = 0;
    Status status = Status::Idle;
    std::unordered_map<std::string, std::string> liveFields;
    std::string error;
};

class LeftViewHistoryService {
  public:
    struct Deps {
        /// Latched per job and never held across a tracker call: a cache recreate waits for every other holder.
        std::function<std::shared_ptr<LocalCacheManager>()> GetCache;
        std::function<void(std::function<void()>)> Launch;   ///< Runs a task on a background worker.
        std::function<void(std::function<void()>)> PostToUi; ///< Must not drop a completion.
        std::function<bool()> ShouldCancel;                  ///< True once a tracker call should stop waiting.
        std::function<bool()> IsOffline;                     ///< No tracker request while true. Any thread.
        std::function<int()> MaxRows;                        ///< Tunable: history row cap.
        std::function<int()> MaxReasonKeys;                  ///< Tunable: reason lookups per batch.
        std::function<std::int64_t()> NowUnixSec;
        /// A live reader for a backend key, when `cfg` points at that tracker; false when no pane is connected to it.
        std::function<bool(const std::string&, const TrackerConfig&, std::shared_ptr<ITrackerIssueReader>&)>
            ResolveReader;
        /// Field id -> display name for a backend key, from any live pane on it.
        std::function<std::unordered_map<std::string, std::string>(const std::string&)> ResolveFieldLabels;
        /// New departures a reload found, so the caller can notify; the monitor path has already notified.
        std::function<void(const std::vector<LeftViewRecord>&)> OnReloadDepartures;
        /// Views for a fetch, loaded on the calling worker.
        std::function<ViewsStore(const TrackerConfig&)> LoadViews;
        std::function<void(const std::string&)> OpenUrl;
    };

    explicit LeftViewHistoryService(Deps deps);

    /// Loads the saved history. Called at startup and after a cache recreate.
    void Load();
    /// The cache file was replaced: forget the rows of the old one and read the new one.
    void ResetForCacheRecreate();

    /// A complete reload of the view finished. `listed` is what the query returned, `stillHeld` what the pane keeps
    /// through the hierarchy walk, and `lastKnown` the rows the reload dropped. Records what left, if the saved
    /// listing answered the same query; otherwise this listing is only a baseline.
    void RecordListing(const LeftViewCapture& capture, std::vector<std::string> listed,
                       std::unordered_set<std::string> stillHeld, std::vector<LeftViewDeparture> lastKnown);
    /// The change monitor found departures. Only keys the saved listing still names are recorded, so two panes on
    /// one view and a later reload report each departure once.
    void RecordMonitorDepartures(const LeftViewCapture& capture, std::vector<LeftViewDeparture> departures);

    /// The change monitor added rows the view's roster listed, so a later departure of one is recorded too.
    void RecordListingAdditions(const std::string& backendKey, const std::string& viewId, const std::string& query,
                                std::vector<std::string> keys);

    const std::vector<LeftViewRecord>& Rows() const { return rows_; }
    /// Bumped on every change to Rows(), so a filtered projection can be cached.
    std::uint64_t Revision() const { return revision_; }

    void Dismiss(std::int64_t id);
    void ClearAll();

    /// Shows `record` in the details view and reads its live values, unless offline or not connected.
    void RequestDetails(const LeftViewRecord& record, const TrackerConfig& cfg);
    void CloseDetails();
    const LeftViewDetails& Details() const { return details_; }
    std::unordered_map<std::string, std::string> FieldLabelsFor(const std::string& backendKey) const;
    void OpenUrl(const std::string& url) const;

    /// Blocks until queued cache work has run. Shutdown and tests only.
    bool Flush(std::chrono::milliseconds timeout);

  private:
    struct LookupJob {
        std::int64_t id = 0;
        std::string issueKey;
        bool needsExistenceProbe = false;
        std::int64_t sinceMs = 0;
        std::unordered_map<std::string, std::string> lastKnown;
    };

    void CommitDepartures_(const std::shared_ptr<const LeftViewCapture>& capture, std::vector<LeftViewRecord> rows,
                           std::vector<LookupJob> jobs, const std::string& listingJson,
                           std::int64_t listingSavedAtUnixSec, std::uint64_t generation, bool notifyAsReload);
    void RunLookups_(const std::shared_ptr<const LeftViewCapture>& capture, const std::vector<LookupJob>& jobs,
                     std::uint64_t generation);
    void SettleUnavailable_(const std::vector<LookupJob>& jobs, std::uint64_t generation);
    void ApplyLookupResult_(LeftViewRecord updated, std::uint64_t generation);

    Deps deps_;
    SerialTaskQueue io_;
    std::vector<LeftViewRecord> rows_;
    std::uint64_t revision_ = 0;
    std::uint64_t generation_ = 0; ///< Moved by a cache recreate: completions of older work are dropped.
    LeftViewDetails details_;
    std::uint64_t detailsRequestSeq_ = 0;
};

/// A row loaded from disk whose lookup never finished gets a settled reason (the app quit first).
void SettleInterruptedLookup(LeftViewRecord& row);

} // namespace smatchet

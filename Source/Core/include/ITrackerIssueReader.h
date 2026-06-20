#pragma once
#include <chrono>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "CachedTicketTypes.h"
#include "SmatchetResult.h"
#include "TrackerError.h"
#include "TrackerFieldSchema.h"

struct TrackerConfig;
struct ViewsStore;

struct TrackerIssueFetchSummary {
    size_t FetchedCount = 0;
    bool FullSyncCompleted = false;
    // Hard failure: sync did not complete usefully. Drives the failure banner / error toast.
    std::string FetchError;
    // Soft warning: sync did produce useful data but with a caveat (e.g. pagination cap reached).
    // Distinct from FetchError so the UI can show "Sync Warning" without suppressing the success
    // notification and without flipping connectivity state to TransportDown.
    std::string Warning;
};

class ITrackerIssueReader {
  public:
    virtual ~ITrackerIssueReader() = default;

    virtual std::vector<CachedTicket> FetchIssues(bool* outFullSyncCompleted = nullptr,
                                                  const TrackerConfig* configOverride = nullptr,
                                                  const ViewsStore* viewsOverride = nullptr,
                                                  std::string* outFetchError = nullptr,
                                                  std::string* outWarning = nullptr) = 0;

    using BatchCallback = std::function<void(std::vector<CachedTicket>&&)>;
    using CancelCallback = std::function<bool()>;

    virtual TrackerIssueFetchSummary FetchIssuesStreamed(const BatchCallback& onBatch,
                                                         const CancelCallback& shouldCancel,
                                                         const TrackerConfig* configOverride = nullptr,
                                                         const ViewsStore* viewsOverride = nullptr) {
        TrackerIssueFetchSummary summary;
        std::string fetchError;
        bool fullSyncCompleted = false;
        std::vector<CachedTicket> tickets = FetchIssues(&fullSyncCompleted, configOverride, viewsOverride, &fetchError);
        summary.FetchedCount = tickets.size();
        summary.FullSyncCompleted = fullSyncCompleted;
        summary.FetchError = fetchError;
        if (!tickets.empty() && onBatch && (!shouldCancel || !shouldCancel())) {
            onBatch(std::move(tickets));
        }
        return summary;
    }

    virtual Result<std::vector<CachedTicket>, TrackerError>
    FetchIssuesForKeys(const TrackerConfig& cfg, const std::vector<std::string>& issueKeys,
                       const ViewsStore& views) = 0;

    // ---- ticket-change-monitor reader surface (ticket-change-monitor plan, S1b) ----
    // Three optional capabilities the per-pane change monitor leans on. Each ships with a
    // backend-agnostic DEFAULT here so an unsupporting backend keeps working (heavier, but
    // correct); concrete clients override with a native server-side filter / existence GET.

    /// Issues whose salient fields changed within `window`. The default ignores the filter
    /// and returns the FULL view (correct but heavy) — concrete backends override with a
    /// native "updated >= -Nm" / "since=" query restricted to `salientFields`.
    virtual Result<std::vector<CachedTicket>, TrackerError>
    FetchIssuesChangedSince(const TrackerConfig& cfg, const ViewsStore& views,
                            std::chrono::seconds /*window*/,
                            const std::vector<std::string>& /*salientFields*/) {
        bool full = false;
        std::string err;
        std::vector<CachedTicket> tickets = FetchIssues(&full, &cfg, &views, &err);
        if (!err.empty()) {
            return Result<std::vector<CachedTicket>, TrackerError>::Err(TrackerErrorUnknown(err));
        }
        return Result<std::vector<CachedTicket>, TrackerError>::Ok(std::move(tickets));
    }

    /// Just the issue keys currently in the view, for the membership reconcile. The default
    /// does a full fetch and projects the keys — concrete backends override with a keys-only
    /// (`fields=*none`) request.
    virtual Result<std::vector<std::string>, TrackerError>
    FetchIssueKeysForView(const TrackerConfig& cfg, const ViewsStore& views) {
        bool full = false;
        std::string err;
        std::vector<CachedTicket> tickets = FetchIssues(&full, &cfg, &views, &err);
        if (!err.empty()) {
            return Result<std::vector<std::string>, TrackerError>::Err(TrackerErrorUnknown(err));
        }
        std::vector<std::string> keys;
        keys.reserve(tickets.size());
        for (size_t i = 0; i < tickets.size(); ++i) {
            keys.push_back(tickets[i].id);
        }
        return Result<std::vector<std::string>, TrackerError>::Ok(std::move(keys));
    }

    /// Whether `issueKey` still exists server-side, to tell a deletion (false) apart from a
    /// ticket that merely left the view (true). The default is the SAFE conservative answer
    /// `true` (assume it still exists → "left view", never destructively delete a cache row
    /// on a backend that cannot probe). Concrete backends override with a `GET /issue/{key}`
    /// returning false on 404.
    virtual Result<bool, TrackerError> ProbeIssueExists(const TrackerConfig& /*cfg*/,
                                                        const std::string& /*issueKey*/) {
        return Result<bool, TrackerError>::Ok(true);
    }

    virtual std::string ResolveDisplayValue(const std::string& fieldId, const TrackerField* field,
                                            const std::string& value) const = 0;

    virtual std::string BuildBrowseUrl(const TrackerConfig& /*cfg*/, const std::string& /*issueKey*/) const {
        return {};
    }
};

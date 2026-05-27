#pragma once
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "CachedTicketTypes.h"
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

    virtual bool FetchIssuesForKeys(const TrackerConfig& cfg, const std::vector<std::string>& issueKeys,
                                    const ViewsStore& views, std::vector<CachedTicket>& outTickets,
                                    std::string& outError) = 0;

    virtual std::string ResolveDisplayValue(const std::string& fieldId, const TrackerField* field,
                                            const std::string& value) const = 0;

    virtual std::string BuildBrowseUrl(const TrackerConfig& /*cfg*/, const std::string& /*issueKey*/) const {
        return {};
    }
};

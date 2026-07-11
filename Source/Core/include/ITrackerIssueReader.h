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
    // Structured twin of FetchError (retire-transport-error-text item 12): the kind classified
    // at the backend's own composition site, where the HTTP status is in scope. Kind == None
    // means the backend did not classify (legacy path) — consumers fall back to the text
    // heuristic. When set, Error.Detail mirrors FetchError.
    TrackerError Error;
    // Soft warning: sync did produce useful data but with a caveat (e.g. pagination cap reached).
    // Distinct from FetchError so the UI can show "Sync Warning" without suppressing the success
    // notification and without flipping connectivity state to TransportDown.
    std::string Warning;
};

class ITrackerIssueReader {
  public:
    virtual ~ITrackerIssueReader() = default;

    /// `outFetchErrorStructured` (optional) receives the classified twin of `outFetchError`
    /// (retire-transport-error-text item 12). The string param stays so existing call sites
    /// keep working; backends fill both from the same composition site. Kind == None when the
    /// fetch succeeded (or a backend path hasn't classified yet — consumers must fall back to
    /// the text heuristic in that case).
    virtual std::vector<CachedTicket>
    FetchIssues(bool* outFullSyncCompleted = nullptr, const TrackerConfig* configOverride = nullptr,
                const ViewsStore* viewsOverride = nullptr, std::string* outFetchError = nullptr,
                std::string* outWarning = nullptr, TrackerError* outFetchErrorStructured = nullptr) = 0;

    using BatchCallback = std::function<void(std::vector<CachedTicket>&&)>;
    using CancelCallback = std::function<bool()>;

    virtual TrackerIssueFetchSummary FetchIssuesStreamed(const BatchCallback& onBatch,
                                                         const CancelCallback& shouldCancel,
                                                         const TrackerConfig* configOverride = nullptr,
                                                         const ViewsStore* viewsOverride = nullptr) {
        TrackerIssueFetchSummary summary;
        std::string fetchError;
        TrackerError fetchErrorStructured;
        bool fullSyncCompleted = false;
        std::vector<CachedTicket> tickets =
            FetchIssues(&fullSyncCompleted, configOverride, viewsOverride, &fetchError, nullptr, &fetchErrorStructured);
        summary.FetchedCount = tickets.size();
        summary.FullSyncCompleted = fullSyncCompleted;
        summary.FetchError = fetchError;
        summary.Error = fetchErrorStructured;
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
    FetchIssuesChangedSince(const TrackerConfig& cfg, const ViewsStore& views, std::chrono::seconds /*window*/,
                            const std::vector<std::string>& /*salientFields*/) {
        bool full = false;
        std::string err;
        std::string warn;
        std::vector<CachedTicket> tickets = FetchIssues(&full, &cfg, &views, &err, &warn);
        if (!err.empty()) {
            return Result<std::vector<CachedTicket>, TrackerError>::Err(TrackerErrorUnknown(err));
        }
        // A capped/partial or warned fetch is NOT an authoritative view snapshot; treating it as
        // one would let the membership reconcile raise spurious LeftView removals. Bail instead.
        if (!full || !warn.empty()) {
            return Result<std::vector<CachedTicket>, TrackerError>::Err(
                TrackerErrorUnknown("FetchIssuesChangedSince default received a partial/warned fetch"));
        }
        return Result<std::vector<CachedTicket>, TrackerError>::Ok(std::move(tickets));
    }

    /// Just the issue keys currently in the view, for the membership reconcile. The default
    /// does a full fetch and projects the keys — concrete backends override with a keys-only
    /// (`fields=*none`) request.
    virtual Result<std::vector<std::string>, TrackerError> FetchIssueKeysForView(const TrackerConfig& cfg,
                                                                                 const ViewsStore& views) {
        bool full = false;
        std::string err;
        std::string warn;
        std::vector<CachedTicket> tickets = FetchIssues(&full, &cfg, &views, &err, &warn);
        if (!err.empty()) {
            return Result<std::vector<std::string>, TrackerError>::Err(TrackerErrorUnknown(err));
        }
        // Partial/warned fetch → not an authoritative membership set; refuse rather than feed the
        // reconcile a truncated key list that would look like tickets having left the view.
        if (!full || !warn.empty()) {
            return Result<std::vector<std::string>, TrackerError>::Err(
                TrackerErrorUnknown("FetchIssueKeysForView default received a partial/warned fetch"));
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
    virtual Result<bool, TrackerError> ProbeIssueExists(const TrackerConfig& /*cfg*/, const std::string& /*issueKey*/) {
        return Result<bool, TrackerError>::Ok(true);
    }

    virtual std::string ResolveDisplayValue(const std::string& fieldId, const TrackerField* field,
                                            const std::string& value) const = 0;

    virtual std::string BuildBrowseUrl(const TrackerConfig& /*cfg*/, const std::string& /*issueKey*/) const {
        return {};
    }
};

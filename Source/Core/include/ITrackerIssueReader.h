#pragma once
#include <chrono>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "CachedTicketTypes.h"
#include "SmatchetResult.h"
#include "TrackerError.h"
#include "TrackerRoster.h"
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

    /// Adds `warning` to Warning ("; "-joined), keeping any warning already there: an earlier page
    /// or hop may carry its own caveat, and overwriting it would hide a real result-set problem.
    void AppendWarning(const std::string& warning) {
        if (!warning.empty()) {
            Warning = Warning.empty() ? warning : (Warning + "; " + warning);
        }
    }
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

    /// FetchIssuesForKeys that honours `shouldCancel` inside the request itself (the retry/backoff
    /// window of the HTTP layer), not just between calls - the parent walk of a superseded sync
    /// otherwise runs a whole request to its timeout. The default forwards to FetchIssuesForKeys
    /// (a backend without a cancellable transport loses nothing); a cancelled call returns a
    /// `Cancelled` error when the backend can tell.
    virtual Result<std::vector<CachedTicket>, TrackerError>
    FetchIssuesForKeysCancellable(const TrackerConfig& cfg, const std::vector<std::string>& issueKeys,
                                  const ViewsStore& views, const CancelCallback& /*shouldCancel*/) {
        return FetchIssuesForKeys(cfg, issueKeys, views);
    }

    /// Issues whose `parent` is one of `parentKeys` — the downward mirror of FetchIssuesForKeys.
    /// Used to pull a visible ticket's children into the cache when the active view's own
    /// filter doesn't independently return them (e.g. a `key = EPIC-1` view showing just the
    /// epic itself). The default is the safe no-op (empty result, Ok) — matches the existing
    /// missing-parent-fetch precedent for backends (GitHub Issues, Linear) that don't map a
    /// `parent` field at all; a concrete backend overrides with a native "children of" query.
    virtual Result<std::vector<CachedTicket>, TrackerError>
    FetchChildrenOfKeys(const TrackerConfig& /*cfg*/, const std::vector<std::string>& /*parentKeys*/,
                        const ViewsStore& /*views*/) {
        return Result<std::vector<CachedTicket>, TrackerError>::Ok({});
    }

    /// FetchChildrenOfKeys with the same in-request cancellation as FetchIssuesForKeysCancellable.
    virtual Result<std::vector<CachedTicket>, TrackerError>
    FetchChildrenOfKeysCancellable(const TrackerConfig& cfg, const std::vector<std::string>& parentKeys,
                                   const ViewsStore& views, const CancelCallback& /*shouldCancel*/) {
        return FetchChildrenOfKeys(cfg, parentKeys, views);
    }

    // ---- delta-sync roster surface (docs/plans/active/delta-sync.md § Phase 3) ----

    /// True when FetchViewRoster is implemented. Callers skip the roster path (and keep today's full
    /// fetch) on a backend that answers false - the default.
    virtual bool SupportsVersionedRoster() const { return false; }

    /// The keys currently in the view, each with the tracker's last-updated stamp, WITHOUT downloading
    /// bodies. The roster is `Complete` only when every page was read. Failures keep their kind
    /// (`Transport` stays retryable), and a cancelled walk returns `Cancelled`. The default reports the
    /// capability as missing.
    virtual Result<TrackerRoster, TrackerError>
    FetchViewRoster(const TrackerConfig& /*cfg*/, const ViewsStore& /*views*/, const CancelCallback& /*shouldCancel*/) {
        return Result<TrackerRoster, TrackerError>::Err(
            TrackerErrorInvalidRequest("This tracker cannot list ticket versions."));
    }

    /// Roster (key + last-updated stamp, no bodies) of the issues named by `keys`, for the delta parent walk.
    /// Only meaningful when SupportsVersionedRoster(). A key the tracker will not show fails the call (the
    /// caller then runs the full-body hop, which isolates refusals); `Complete` is true when every key's
    /// batch was read.
    virtual Result<TrackerRoster, TrackerError> FetchRosterForKeys(const TrackerConfig& /*cfg*/,
                                                                   const std::vector<std::string>& /*keys*/,
                                                                   const CancelCallback& /*shouldCancel*/) {
        return Result<TrackerRoster, TrackerError>::Err(
            TrackerErrorInvalidRequest("This tracker cannot list ticket versions."));
    }

    /// Roster of the issues whose parent is one of `parentKeys` - the downward mirror of FetchRosterForKeys,
    /// for the delta child walk. `Complete` is false when a page cap cut the walk short.
    virtual Result<TrackerRoster, TrackerError> FetchChildRosterOfKeys(const TrackerConfig& /*cfg*/,
                                                                       const std::vector<std::string>& /*parentKeys*/,
                                                                       const CancelCallback& /*shouldCancel*/) {
        return Result<TrackerRoster, TrackerError>::Err(
            TrackerErrorInvalidRequest("This tracker cannot list ticket versions."));
    }

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
        TrackerError classified;
        std::vector<CachedTicket> tickets = FetchIssues(&full, &cfg, &views, &err, &warn, &classified);
        if (!err.empty()) {
            // Keep the kind the backend classified: an unreachable tracker stays Transport (retryable).
            return Result<std::vector<CachedTicket>, TrackerError>::Err(classified.IsOk() ? TrackerErrorUnknown(err)
                                                                                          : classified);
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
        TrackerError classified;
        std::vector<CachedTicket> tickets = FetchIssues(&full, &cfg, &views, &err, &warn, &classified);
        if (!err.empty()) {
            return Result<std::vector<std::string>, TrackerError>::Err(classified.IsOk() ? TrackerErrorUnknown(err)
                                                                                         : classified);
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

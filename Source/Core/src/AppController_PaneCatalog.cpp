// AppController_PaneContexts.cpp's pane-catalog cluster, split out to keep that TU under the 1,200-line ceiling
// (behavior-preserving move; tu-line-ceiling). Holds a grid pane's OWN field catalog: the fetch the pane sync kick
// starts (kickPaneCatalogFetch_ -> populatePaneCatalogAfterSync_), parking it until the pane's backend is installed
// (notePaneBackendInstalled_ / resumePaneCatalogFetch_), the off-thread fetch with its saved-snapshot fallback, the
// main-thread apply (applyPaneCatalogOnMainThread_ / landPaneCatalog_ / writePaneCatalog_), the grid's status and
// Retry, and the per-pane catalog READ routing. Method declarations stay in AppController.h; only definitions moved
// here.
// clang-format off
// SMATCHET_DEVIATION(rule=app-controller-fan-in; reason=behavior-preserving TU split of AppController_PaneContexts.cpp, a companion TU defining the AppController pane-catalog methods needs the full class definition and adds no new coupling; owner=orchestrator; revisit=when AppController.h is narrowed per ADR-0020 / debt.md)
#include "AppController.h"
// clang-format on

#include "CacheBackendKeyPure.h"
#include "CatalogOfflinePolicyPure.h"
#include "ComponentOptionsPickPure.h" // CatalogIsRowProject: does the loaded catalog already have the view's scope
#include "FieldCatalogCache.h"
#include "GridLiveContext.h"
#include "ITrackerBackend.h"      // per-pane catalog fetch: backend->FieldCatalog()->FetchFieldCatalog
#include "ITrackerFieldCatalog.h" // TrackerFieldCatalogResult + FetchFieldCatalog
#include "PaneCatalogScopePure.h"
#include "PaneSyncKickPolicy.h"
#include "TicketSyncService.h" // RetryPaneFieldCatalog: is the pane's sync for the new site running

#include "Sync/CatalogRestorePure.h"

#include "Logger.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

void AppController::kickPaneCatalogFetch_(const std::string& paneId, const TrackerConfig& cfg,
                                          const std::string& ownViewQuery) {
    // UI thread. The catalog is scoped to the single project the pane's OWN resolved view names, read with
    // the pane's own backend parser, as the focused catalog's ResolveViewProjectKeyForCatalog does: Jira only
    // runs createmeta (components, versions, project custom-field options) for a scoped fetch. Empty
    // (unscoped) is the documented fallback for a view naming zero or several projects, or a kick that
    // resolved no view (its config then carries the focused view's query, which is another pane's scope).
    const std::map<std::string, std::unique_ptr<GridLiveContext>>::iterator it = gridContexts_.find(paneId);
    if (it == gridContexts_.end()) {
        return;
    }
    GridLiveContext& ctx = *it->second;
    GridLiveContext::PaneCatalogFetchState& state = ctx.paneCatalogFetch;
    // Read after the sync kick: SyncPaneWithBackend may have just created the backend.
    const std::shared_ptr<ITrackerBackend> paneBackend = std::atomic_load(&ctx.Backend);
    if (!paneBackend) {
        // The pane's first sync was held back (a cache recreate), so it has no backend yet: no fetch can start, and the
        // view's project cannot be read with the backend's parser. Park the request instead of recording a failure
        // nothing would retry: installing the backend resumes it (resumePaneCatalogFetch_).
        state.Config = std::make_shared<const TrackerConfig>(cfg);
        state.AwaitingBackend = true;
        state.AwaitingViewQuery = ownViewQuery;
        state.Failed = false;
        state.LastError.clear();
        LOG_INFO("AppController::kickPaneCatalogFetch_ pane='%s': no backend yet (its sync is held back); the catalog "
                 "fetch starts once the backend is installed.",
                 paneId.c_str());
        return;
    }
    state.AwaitingBackend = false;
    state.AwaitingViewQuery.clear();
    const std::string paneProjectKey =
        smatchet::catalog::PaneCatalogProjectKey(&paneBackend->Connectivity(), ownViewQuery);
    populatePaneCatalogAfterSync_(paneId, cfg, paneProjectKey, 0);
}

void AppController::notePaneBackendInstalled_(const std::string& paneId) {
    // Any thread: only posts. A completion, so the dispatcher never drops it the way it drops ordinary posts. It runs
    // after the sync start that installed the backend has returned, so the catalog it fetches latches the generation
    // and epoch that start left (a tracker switch clears the catalog around the install).
    mainThreadDispatcher.PostCompletionToMainThread([this, paneId]() { resumePaneCatalogFetch_(paneId); });
}

void AppController::resumePaneCatalogFetch_(const std::string& paneId) {
    const std::map<std::string, std::unique_ptr<GridLiveContext>>::iterator it = gridContexts_.find(paneId);
    if (it == gridContexts_.end()) {
        return; // retired meanwhile
    }
    GridLiveContext& ctx = *it->second;
    const GridLiveContext::PaneCatalogFetchState& state = ctx.paneCatalogFetch;
    if (!state.AwaitingBackend || state.Config == nullptr || !std::atomic_load(&ctx.Backend)) {
        return; // nothing parked (the usual case: every backend install posts this), or still no backend
    }
    // Copies: the kick rewrites the state it reads them from.
    const TrackerConfig cfg = *state.Config;
    const std::string viewQuery = state.AwaitingViewQuery;
    LOG_INFO("AppController::resumePaneCatalogFetch_ pane='%s': the backend is installed; starting the parked catalog "
             "fetch.",
             paneId.c_str());
    kickPaneCatalogFetch_(paneId, cfg, viewQuery);
}

void AppController::KickPaneCatalogFetchForTest(const std::string& paneId, const TrackerConfig& cfg,
                                                const std::string& ownViewQuery) {
    kickPaneCatalogFetch_(paneId, cfg, ownViewQuery);
}

namespace {

// True when `cat` holds a live catalog that already answers a request (or a result) scoped to `projectKey`: one
// fetched for that same project, or any live catalog for an unscoped request (a superset). A catalog of another
// scope (an unscoped seed, the pane's previous view's project) does not: its components and versions are not this
// project's. Caller holds cat.availableFieldsMutex_.
bool LiveCatalogCoversScopeLocked(const GridContextFieldCatalog& cat, const std::string& projectKey) {
    return cat.fieldCatalogEverLoaded_ &&
           (projectKey.empty() || smatchet::components::CatalogIsRowProject(cat.currentCatalogProjectKey_, projectKey));
}

// True when two catalog scopes name the same project, or are both unscoped ("").
bool SameCatalogScope(const std::string& a, const std::string& b) {
    return a.empty() ? b.empty() : smatchet::components::CatalogIsRowProject(a, b);
}

} // namespace

void AppController::populatePaneCatalogAfterSync_(const std::string& paneId, const TrackerConfig& cfgCopy,
                                                  const std::string& projectKey, unsigned refetchAttempt) {
    // UI thread. Resolve the (just-synced) context; a fetch is only worthwhile when it lacks a
    // catalog for the project its view names — a same-backend pane already carrying a seeded
    // copy of that scope would only duplicate the focused catalog's own fetch traffic.
    std::map<std::string, std::unique_ptr<GridLiveContext>>::iterator it = gridContexts_.find(paneId);
    if (it == gridContexts_.end()) {
        return;
    }
    GridLiveContext& ctx = *it->second;
    GridLiveContext::PaneCatalogFetchState& state = ctx.paneCatalogFetch;
    // The latest request, stored before anything returns, even while a fetch runs or when the loaded catalog already
    // answers it: the apply re-fetches with it when the running fetch's result is unusable (another site, a stale
    // backend), and Retry runs it again, so it must name the view the pane shows now (after A -> B -> A, A, not B).
    state.Config = std::make_shared<const TrackerConfig>(cfgCopy);
    state.ProjectKey = projectKey;
    {
        std::lock_guard<std::mutex> lk(ctx.fieldCatalog.availableFieldsMutex_);
        // Loaded for this scope already (same-backend seed or a prior fetch): nothing to do. A catalog of another
        // scope stays visible while the scoped fetch runs.
        if (LiveCatalogCoversScopeLocked(ctx.fieldCatalog, projectKey)) {
            // The loaded catalog answers the current view, so a fetch still in flight for an earlier view (the pane
            // switched away and back) is superseded too: it must not land over this one. A failure recorded for that
            // earlier view is not this view's.
            ctx.fieldCatalog.PaneCatalogFetchSeq.fetch_add(1);
            state.Failed = false;
            state.LastError.clear();
            return;
        }
    }
    const std::string requestSite = smatchet::cache_keys::TrackerCacheBackendKey(cfgCopy);
    if (state.InFlight && state.InFlightSeq == ctx.fieldCatalog.PaneCatalogFetchSeq.load() &&
        state.InFlightProjectKey == projectKey && state.InFlightSiteKey == requestSite) {
        // A repeat of the request in flight (another kick, a Retry) while that fetch is still the pane's latest: it
        // waits for that fetch, whose apply re-fetches with the request stored above if its result turns out unusable.
        // A request for another scope or site, or one whose fetch a skip already superseded, starts its own fetch
        // below (PaneCatalogFetchSeq), so a fetch for the pane's previous view never lands last.
        return;
    }
    // Capture-then-check tokens (#1081), latched BEFORE the backend handle: SetBackend installs the
    // new backend and then bumps the generation, so a swap between these reads pairs an older
    // generation with the newer backend and the apply drops (safe), never the reverse. The epoch
    // moves when a tracker switch clears the catalog before its new backend is installed.
    const std::uint64_t capturedGeneration = ctx.backendGeneration_.load();
    const std::uint64_t capturedEpoch = ctx.fieldCatalog.CatalogEpoch.load();
    // Capture a STRONG handle to the pane's backend (ADR-0012): a live swap must not free it
    // mid-fetch. atomic_load — the shared_ptr instance read cannot race SetBackend's exchange.
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(&ctx.Backend);
    if (!backend || !backend->FieldCatalog()) {
        // No backend (kickPaneCatalogFetch_ parks that case until one is installed, so only a Retry or re-fetch gets
        // here without one), or one without a catalog endpoint (e.g. a fixture).
        // Recorded as failed so the grid's cue offers Retry instead of waiting on a fetch that never runs.
        state.Failed = true;
        state.LastError =
            backend ? "This tracker backend has no field catalog." : "The pane's tracker is not set up yet.";
        LOG_INFO("AppController::populatePaneCatalogAfterSync_ pane='%s' catalog fetch not started: %s", paneId.c_str(),
                 state.LastError.c_str());
        return;
    }
    // This fetch supersedes any still in flight for the pane (its view changed): only the latest one applies.
    const std::uint64_t fetchSeq = ctx.fieldCatalog.PaneCatalogFetchSeq.fetch_add(1) + 1;
    // InFlight is released on every path, as the Ui AGENTS.md rule on single-slot async loads requires: the worker
    // never throws (fetchPaneCatalogOnWorker_ catches everything), its result comes back as a completion, which the
    // dispatcher never evicts the way it drops ordinary posts, and a launch that throws (no thread to run it)
    // releases it here. LaunchBackgroundTask refuses silently only during shutdown, when nothing reads it.
    const std::uint64_t fetchToken = ++paneCatalogFetchSeq_;
    state.FetchToken = fetchToken;
    state.InFlight = true;
    state.InFlightProjectKey = projectKey;
    state.InFlightSiteKey = requestSite;
    state.InFlightSeq = fetchSeq;
    state.Failed = false; // LastError is kept, so a repeat of the same failure logs quietly
    const std::shared_ptr<const TrackerConfig> cfg = state.Config;
    try {
        LaunchBackgroundTask([this, paneId, backend, cfg, projectKey, fetchToken, fetchSeq, capturedGeneration,
                              capturedEpoch, refetchAttempt]() {
            /* PILLAR2_WORKER_ONLY */ // est-latency: catalog HTTP fetch off the UI thread
            PaneCatalogFetchResult result = fetchPaneCatalogOnWorker_(paneId, *backend, *cfg, projectKey);
            mainThreadDispatcher.PostCompletionToMainThread([this, paneId, fetchToken, fetchSeq, capturedGeneration,
                                                             capturedEpoch, cfg, refetchAttempt,
                                                             result = std::move(result)]() mutable {
                applyPaneCatalogOnMainThread_(paneId, fetchToken, fetchSeq, capturedGeneration, capturedEpoch, cfg,
                                              refetchAttempt, std::move(result));
            });
        });
    } catch (const std::exception& ex) {
        state.InFlight = false;
        state.Failed = true;
        state.LastError = std::string("The field catalog fetch could not start: ") + ex.what();
        LOG_WARN("AppController::populatePaneCatalogAfterSync_ pane='%s' %s", paneId.c_str(), state.LastError.c_str());
    }
}

AppController::PaneCatalogFetchResult AppController::fetchPaneCatalogOnWorker_(const std::string& paneId,
                                                                               ITrackerBackend& backend,
                                                                               const TrackerConfig& cfg,
                                                                               const std::string& projectKey) {
    PaneCatalogFetchResult out;
    out.ProjectKey = projectKey;
    try {
        auto fetched = backend.FieldCatalog()->FetchFieldCatalog(cfg, projectKey);
        if (fetched) {
            TrackerFieldCatalogResult catalog = std::move(fetched.value());
            out.Fields = std::move(catalog.Fields);
            out.Components = std::move(catalog.Components);
            out.IssueTypeMeta = std::move(catalog.IssueTypeMeta);
        } else {
            // Pillar 6: the pane falls back to the catalog snapshot saved for its tracker (the file read stays on
            // this worker); the apply marks it as saved, so the next sync still tries the live fetch. INFO, not
            // WARN: offline, every sync retry re-runs this fetch, and the apply WARNs once when the pane lands on a
            // saved copy or has none. Logged, not swallowed (policy).
            const std::string detail = fetched.error().Detail;
            LOG_INFO("AppController::populatePaneCatalogAfterSync_ fetch failed pane='%s' project='%s' err=%s",
                     paneId.c_str(), projectKey.c_str(), detail.c_str());
            // The snapshot keyed by the SAME project the fetch was for comes first; the unscoped one only stands
            // in when the project has none saved yet, and then restores as no project's catalog, so its
            // components never pass for this project's list.
            const std::vector<std::string> candidateKeys = smatchet::sync::CatalogSnapshotCandidates(
                projectKey, FieldCatalogCache::BuildFieldCatalogCacheKey(cfg, projectKey),
                FieldCatalogCache::BuildFieldCatalogCacheKey(cfg, std::string()));
            std::string snapErr;
            std::size_t loadedIndex = 0;
            if (!FieldCatalogCache::TryLoadFirstFieldCatalogSnapshot(candidateKeys, out.Fields, out.Components,
                                                                     out.IssueTypeMeta, snapErr, loadedIndex)) {
                LOG_INFO("AppController::populatePaneCatalogAfterSync_ pane='%s' has no saved catalog: %s",
                         paneId.c_str(), snapErr.c_str());
                out.Failed = true;
                out.Error = detail;
                return out;
            }
            out.ProjectKey = smatchet::sync::RestoredSnapshotScope(projectKey, loadedIndex, candidateKeys.size());
            out.RestoredWarning = std::string("Offline: restored the field catalog from local snapshot.") +
                                  smatchet::catalogoffline::kLastFetchFailedMarker + detail;
        }
        // The corrections the focused catalog gets (read-only timetracking, History / Comments columns), on the live
        // result and the restored snapshot alike (snapshots are saved uncorrected): without them a pane catalog that
        // replaces a seeded copy would make non-editable fields editable.
        ApplyCatalogCorrections(out.Fields, backend.Connectivity().GetTrackerType());
    } catch (const std::exception& ex) {
        LOG_WARN("AppController::populatePaneCatalogAfterSync_ pane='%s' catalog fetch threw: %s", paneId.c_str(),
                 ex.what());
        out = PaneCatalogFetchResult();
        out.Failed = true;
        out.Error = ex.what();
    } catch (...) {
        // A non-std throw would otherwise reach the background-task firewall, which posts nothing back and leaves
        // the pane's fetch marked in flight. Report it as a failure instead.
        LOG_WARN("AppController::populatePaneCatalogAfterSync_ pane='%s' catalog fetch threw a non-std exception.",
                 paneId.c_str());
        out = PaneCatalogFetchResult();
        out.Failed = true;
        out.Error = "The field catalog fetch failed with an unknown error.";
    }
    return out;
}

void AppController::applyPaneCatalogOnMainThread_(const std::string& paneId, std::uint64_t fetchToken,
                                                  std::uint64_t fetchSeq, std::uint64_t capturedGeneration,
                                                  std::uint64_t capturedEpoch,
                                                  const std::shared_ptr<const TrackerConfig>& cfg,
                                                  unsigned refetchAttempt, PaneCatalogFetchResult result) {
    // UI thread. Re-validate the CAPTURED context (issue #1457 / #1081 discipline): write only when the
    // pane's context still exists, the fetch is still its latest, its backend generation and catalog epoch
    // are unchanged since the fetch started, and no live catalog covering its scope landed meanwhile. Never
    // re-resolve focus at completion: write through the kick-time context or drop (the debt cluster's #975
    // entry).
    std::map<std::string, std::unique_ptr<GridLiveContext>>::iterator it = gridContexts_.find(paneId);
    if (it == gridContexts_.end()) {
        LOG_INFO("AppController::applyPaneCatalogOnMainThread_ pane='%s' catalog result dropped: the pane's context "
                 "was retired while it loaded.",
                 paneId.c_str());
        return;
    }
    GridLiveContext& ctx = *it->second;
    GridLiveContext::PaneCatalogFetchState& state = ctx.paneCatalogFetch;
    if (state.FetchToken != fetchToken) {
        // A newer fetch replaced it (the pane's view or site changed while it ran), or it is from the context this
        // pane id had before it was retired and revived: the fetch this context is waiting on is another one, and
        // its own apply settles the state.
        LOG_INFO("AppController::applyPaneCatalogOnMainThread_ pane='%s' catalog result dropped: a newer fetch "
                 "replaced it.",
                 paneId.c_str());
        return;
    }
    state.InFlight = false;
    // The scope this fetch was started for. Coverage is judged against it, never against the result's ProjectKey: a
    // failed result carries none, and a restored unscoped stand-in carries "", which any live catalog would "cover".
    const std::string requestedProject = state.InFlightProjectKey;
    if (ctx.fieldCatalog.PaneCatalogFetchSeq.load() != fetchSeq) {
        // A later request for the pane's current view superseded it (a newer fetch, or a skip because the loaded
        // catalog already answers the view): that request decides the catalog.
        state.Failed = false;
        LOG_INFO("AppController::applyPaneCatalogOnMainThread_ pane='%s' catalog for project '%s' dropped: a later "
                 "request for the pane's current view superseded it.",
                 paneId.c_str(), result.ProjectKey.c_str());
        return;
    }
    bool covered = false;
    {
        std::lock_guard<std::mutex> lk(ctx.fieldCatalog.availableFieldsMutex_);
        covered = LiveCatalogCoversScopeLocked(ctx.fieldCatalog, requestedProject);
    }
    const bool current = smatchet::PaneCatalogResultCurrent(capturedGeneration, ctx.backendGeneration_.load(),
                                                            capturedEpoch, ctx.fieldCatalog.CatalogEpoch.load());
    const std::string paneSite = ctx.CacheBackendKeyCopy();
    const bool resultForPaneSite = paneSite == smatchet::cache_keys::TrackerCacheBackendKey(*cfg);
    const std::shared_ptr<const TrackerConfig> latest = state.Config != nullptr ? state.Config : cfg;
    const bool latestForPaneSite = paneSite == smatchet::cache_keys::TrackerCacheBackendKey(*latest);
    const smatchet::PaneCatalogApplyVerdict verdict =
        smatchet::DecidePaneCatalogApply(true, current, covered, resultForPaneSite, latestForPaneSite, refetchAttempt);
    if (verdict == smatchet::PaneCatalogApplyVerdict::DropNotNeeded) {
        state.Failed = false;
        LOG_INFO("AppController::applyPaneCatalogOnMainThread_ pane='%s' catalog result dropped: a live catalog "
                 "for its scope landed while it loaded.",
                 paneId.c_str());
        return;
    }
    if (verdict == smatchet::PaneCatalogApplyVerdict::DropRefetch) {
        LOG_WARN("AppController::applyPaneCatalogOnMainThread_ pane='%s' catalog apply dropped: %s; the pane has no "
                 "catalog for its view, so it is fetched again with its latest request (re-fetch %u of %u).",
                 paneId.c_str(),
                 resultForPaneSite ? "the backend changed mid-fetch (#1081)" : "it was fetched for another site",
                 refetchAttempt + 1, smatchet::kMaxPaneCatalogRefetches);
        populatePaneCatalogAfterSync_(paneId, *latest, state.ProjectKey, refetchAttempt + 1);
        return;
    }
    if (verdict == smatchet::PaneCatalogApplyVerdict::DropOwed) {
        state.Failed = true;
        state.LastError = latestForPaneSite ? "The tracker backend kept changing while the field catalog loaded."
                                            : "The pane's tracker site changed while its field catalog loaded.";
        LOG_WARN("AppController::applyPaneCatalogOnMainThread_ pane='%s' catalog apply dropped: %s The pane has no "
                 "catalog; its cells stay read-only until Retry or its next sync kick.",
                 paneId.c_str(), state.LastError.c_str());
        return;
    }
    if (result.Failed) {
        // WARN once per distinct error: offline, every sync retry re-runs this fetch and fails the same way.
        const bool repeat = !result.Error.empty() && state.LastError == result.Error;
        state.Failed = true;
        state.LastError = result.Error;
        if (repeat) {
            LOG_INFO("AppController::applyPaneCatalogOnMainThread_ pane='%s' still has no field catalog (%s).",
                     paneId.c_str(), result.Error.c_str());
        } else {
            LOG_WARN("AppController::applyPaneCatalogOnMainThread_ pane='%s' has no field catalog: the fetch failed "
                     "and no saved copy exists (%s). Its cells stay read-only until Retry or its next sync kick.",
                     paneId.c_str(), result.Error.c_str());
        }
        return;
    }
    landPaneCatalog_(paneId, ctx, requestedProject, std::move(result));
}

void AppController::landPaneCatalog_(const std::string& paneId, GridLiveContext& ctx,
                                     const std::string& requestedProject, PaneCatalogFetchResult result) {
    GridLiveContext::PaneCatalogFetchState& state = ctx.paneCatalogFetch;
    const bool restored = !result.RestoredWarning.empty();
    const std::size_t fieldCount = result.Fields.size();
    std::string previousWarning;
    const PaneCatalogWriteOutcome outcome =
        writePaneCatalog_(ctx.fieldCatalog, requestedProject, result, previousWarning);
    if (outcome == PaneCatalogWriteOutcome::KeptExisting) {
        // The live fetch failed and its saved copy does not replace the catalog the pane already shows (another
        // scope's): that stays visible, but the request is still unanswered, so it stays failed and retryable.
        const std::string detail = smatchet::catalogoffline::ExtractLastFetchFailedDetail(result.RestoredWarning);
        state.Failed = true;
        state.LastError = detail.empty() ? std::string("The field catalog fetch failed.") : detail;
        LOG_INFO("AppController::applyPaneCatalogOnMainThread_ pane='%s' kept the catalog it shows: the fetch for "
                 "project '%s' failed and a saved copy never replaces a loaded catalog (%s).",
                 paneId.c_str(), requestedProject.c_str(), state.LastError.c_str());
        return;
    }
    state.Failed = false;
    state.LastError.clear();
    if (outcome == PaneCatalogWriteOutcome::AlreadyAnswered) {
        LOG_INFO("AppController::applyPaneCatalogOnMainThread_ pane='%s' catalog result dropped: the pane already has "
                 "a catalog that answers it (a live one for its scope, or this same saved copy).",
                 paneId.c_str());
        return;
    }
    // Bump AFTER the write is visible so a read-routing consumer that observes the new revision
    // also observes the new fields (the read-routing cache keys on this revision — the
    // same-backend staleness invalidation path).
    ctx.fieldCatalog.TrackerFieldCatalogRevision.fetch_add(1);
    if (restored && result.RestoredWarning != previousWarning) {
        // The live fetch failed and the pane now runs on its saved copy: WARN when it lands on that copy (a later
        // restore over the copy it already shows is dropped above), since the worker logs each attempt at INFO.
        LOG_WARN("AppController::applyPaneCatalogOnMainThread_ pane='%s' live field catalog fetch failed; using the "
                 "saved catalog (%zu fields, project '%s'). %s",
                 paneId.c_str(), fieldCount, result.ProjectKey.c_str(), result.RestoredWarning.c_str());
        return;
    }
    LOG_INFO(
        "AppController::applyPaneCatalogOnMainThread_ pane='%s' populated own catalog (%zu fields, project='%s'%s).",
        paneId.c_str(), fieldCount, result.ProjectKey.c_str(), restored ? ", from the saved snapshot" : "");
}

AppController::PaneCatalogWriteOutcome AppController::writePaneCatalog_(GridContextFieldCatalog& cat,
                                                                        const std::string& requestedProject,
                                                                        PaneCatalogFetchResult& result,
                                                                        std::string& previousWarning) {
    const bool restored = !result.RestoredWarning.empty();
    std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
    if (LiveCatalogCoversScopeLocked(cat, requestedProject)) {
        return PaneCatalogWriteOutcome::AlreadyAnswered; // a worker apply can land between the apply's check and here
    }
    // Offline, every sync kick re-runs the fetch and restores the same saved copy the pane already shows (a restored
    // catalog is "not loaded", so it does not count as covered): that request is answered, not failed.
    if (restored && cat.fieldCatalogRestored_ && SameCatalogScope(cat.currentCatalogProjectKey_, result.ProjectKey)) {
        return PaneCatalogWriteOutcome::AlreadyAnswered;
    }
    if (restored && !cat.AvailableFields->empty()) {
        return PaneCatalogWriteOutcome::KeptExisting; // a saved catalog never replaces the one the pane already has
    }
    previousWarning = cat.LastTrackerFieldCatalogWarning;
    cat.AvailableFields = std::make_shared<std::vector<TrackerField>>(std::move(result.Fields));
    cat.AvailableComponents = std::move(result.Components);
    cat.AvailableIssueTypeMeta = std::move(result.IssueTypeMeta);
    // Pinned with the catalog it describes: the components editor trusts the catalog's list for a row only when
    // this names the row's project (GetPaneFieldCatalogProjectKey / ComponentOptionsPickPure.h).
    cat.currentCatalogProjectKey_ = result.ProjectKey;
    // A restored snapshot leaves the catalog "not loaded", so the next sync kick retries the live fetch, while
    // fieldCatalogRestored_ still routes the pane's reads to it.
    cat.fieldCatalogEverLoaded_ = !restored;
    cat.fieldCatalogRestored_ = restored;
    cat.LastTrackerFieldCatalogError.clear();
    cat.LastTrackerFieldCatalogErrorTransient = false;
    cat.LastTrackerFieldCatalogWarning = result.RestoredWarning;
    return PaneCatalogWriteOutcome::Written;
}

AppController::PaneCatalogFetchStatus AppController::GetPaneCatalogFetchStatus(const std::string& paneId) const {
    PaneCatalogFetchStatus status;
    std::map<std::string, std::unique_ptr<GridLiveContext>>::const_iterator it = gridContexts_.find(paneId);
    if (it == gridContexts_.end()) {
        return status;
    }
    const GridLiveContext::PaneCatalogFetchState& state = it->second->paneCatalogFetch;
    status.OwnCatalogPopulated = IsPaneFieldCatalogPopulated(paneId);
    status.InFlight = state.InFlight;
    status.Failed = state.Failed;
    status.AwaitingBackend = state.AwaitingBackend;
    status.CanRetry = state.Config != nullptr && !state.AwaitingBackend; // a parked fetch resumes on its own
    status.LastError = state.LastError;
    return status;
}

void AppController::RetryPaneFieldCatalog(const std::string& paneId) {
    std::map<std::string, std::unique_ptr<GridLiveContext>>::iterator it = gridContexts_.find(paneId);
    if (it == gridContexts_.end()) {
        return;
    }
    GridLiveContext& ctx = *it->second;
    const std::shared_ptr<const TrackerConfig> cfg = ctx.paneCatalogFetch.Config;
    if (cfg == nullptr || ctx.paneCatalogFetch.InFlight || ctx.paneCatalogFetch.AwaitingBackend) {
        return; // nothing stored, a fetch is running, or one is parked until the backend is installed
    }
    if (smatchet::cache_keys::TrackerCacheBackendKey(*cfg) != ctx.CacheBackendKeyCopy()) {
        // The pane moved to another tracker site since that request was stored: fetching with it would load the old
        // site's catalog into this pane. The key only moves when a sync for the new site starts, so while that sync
        // is still running it is left alone: re-arming would make the next frame cancel and restart it. Its catalog
        // comes with it (a pane kick fetches after its swap; the focused fetch covers the focused pane).
        if (ctx.ticketSync_ && ctx.ticketSync_->HasPendingWork()) {
            LOG_INFO("AppController::RetryPaneFieldCatalog pane='%s': the pane's tracker site changed and its sync for "
                     "the new site is running; nothing to retry yet.",
                     paneId.c_str());
            return;
        }
        // No sync running: re-arm the pane's sync kick, the way a dropped kick does, so the frame loop kicks it again
        // with the pane's current configuration and that kick fetches the catalog.
        ctx.initialSyncKicked = false;
        ctx.lastSyncedJql.clear(); // cleared with the latch (GridLiveContext.h discipline)
        LOG_INFO("AppController::RetryPaneFieldCatalog pane='%s': the pane's tracker site changed; its next sync "
                 "kick fetches the catalog.",
                 paneId.c_str());
        return;
    }
    LOG_INFO("AppController::RetryPaneFieldCatalog pane='%s': fetching the field catalog again.", paneId.c_str());
    populatePaneCatalogAfterSync_(paneId, *cfg, ctx.paneCatalogFetch.ProjectKey, 0);
}

// --- Per-pane field-catalog READ routing (per-pane-catalog-value-read-routing) --------------
// These resolve THROUGH the pane's OWN GridLiveContext::fieldCatalog, which is populated by:
//   (a) EnsurePaneContextLive's same-backend one-time seed copy (existing), and
//   (b) the cross-backend per-pane catalog fetch kicked from applyPaneSyncKickOnMainThread_
//       (populatePaneCatalogAfterSync_ above), which writes into the CAPTURED context
//       (never fieldCatalog()) under the #1081 generation + epoch check it latches as the fetch
//       starts, after the pane's first sync has swapped its backend in.
// A pane with no live/populated context falls back to the focused catalog — identical to
// today's shared read, so the routing is never a regression, only an upgrade once populated.
// SMATCHET_DEVIATION(rule=duplication; reason=behavior-preserving TU split, the gridContexts_ find-or-default accessor idiom (shared with GetPaneTicketsRevision in AppController_PaneContexts.cpp) was intra-file before the split; owner=orchestrator; revisit=when a shared gridContexts_ lookup helper lands)
std::uint64_t AppController::GetPaneFieldCatalogRevision(const std::string& paneId) const {
    std::map<std::string, std::unique_ptr<GridLiveContext>>::const_iterator it = gridContexts_.find(paneId);
    return (it == gridContexts_.end()) ? 0 : it->second->fieldCatalog.TrackerFieldCatalogRevision.load();
}

bool AppController::IsPaneFieldCatalogPopulated(const std::string& paneId) const {
    std::map<std::string, std::unique_ptr<GridLiveContext>>::const_iterator it = gridContexts_.find(paneId);
    if (it == gridContexts_.end()) {
        return false;
    }
    const GridContextFieldCatalog& cat = it->second->fieldCatalog;
    std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_); // availableFieldsMutex_ is mutable
    return cat.fieldCatalogEverLoaded_ || cat.fieldCatalogRestored_;
}

std::shared_ptr<const std::vector<TrackerField>>
AppController::GetPaneAvailableFields(const std::string& paneId) const {
    std::string projectKey;
    return GetPaneAvailableFields(paneId, projectKey);
}

std::shared_ptr<const std::vector<TrackerField>>
AppController::GetPaneAvailableFields(const std::string& paneId, std::string& outProjectKey) const {
    std::map<std::string, std::unique_ptr<GridLiveContext>>::const_iterator it = gridContexts_.find(paneId);
    if (it != gridContexts_.end()) {
        const GridContextFieldCatalog& cat = it->second->fieldCatalog;
        std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_); // availableFieldsMutex_ is mutable
        if (cat.fieldCatalogEverLoaded_ || cat.fieldCatalogRestored_) {
            outProjectKey = cat.currentCatalogProjectKey_; // pinned with the snapshot, read in the same hold
            return cat.AvailableFields; // snapshot under the lock — the fetch worker swaps, never rewrites
        }
    }
    // No populated per-pane catalog: the focused catalog's snapshot (same data the shared read used).
    return GetAvailableFields(outProjectKey);
}

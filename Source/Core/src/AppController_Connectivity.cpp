#include "AppController.h"
#include "OfflineQueueService.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <string>

#include "ConfigManager.h"
#include "JiraClient.h"
#include "TrackerHttpUtils.h"
#include "Logger.h"
#include "StringUtil.h"

namespace {
constexpr auto kTrackerConnectivityProbeAggressiveInterval = std::chrono::seconds{20};
constexpr auto kTrackerConnectivityProbeRelaxedInterval = std::chrono::seconds{90};
constexpr auto kOfflineReplayDelayWhileTransportDown = std::chrono::seconds{25};
} // namespace

void AppController::DrainTrackerConnectivityProbeFuture() {
    if (!trackerConnectivityProbeInFlight_) {
        return;
    }
    try {
        if (trackerConnectivityProbeFuture_.valid()) {
            trackerConnectivityProbeFuture_.wait();
            (void)trackerConnectivityProbeFuture_.get();
        }
    } catch (const std::exception& ex) {
        LOG_DEBUG("CancelTrackerConnectivityProbe: shutdown swallow: %s", ex.what());
    } catch (...) {
        // catch-all-ok: shutdown path — swallow probe failures.
        LOG_DEBUG("CancelTrackerConnectivityProbe: shutdown swallow: unknown exception");
    }
    trackerConnectivityProbeInFlight_ = false;
}

AppController::TrackerConnectivityState AppController::MapReachabilityProbeKind(TrackerReachabilityProbeKind k) {
    switch (k) {
    case TrackerReachabilityProbeKind::AuthenticatedReachable:
        return TrackerConnectivityState::AuthenticatedReachable;
    case TrackerReachabilityProbeKind::ReachableAuthOrConfigError:
        return TrackerConnectivityState::ReachableAuthOrConfigError;
    case TrackerReachabilityProbeKind::TransportDown:
        return TrackerConnectivityState::TransportDown;
    case TrackerReachabilityProbeKind::ServiceUnavailable:
        return TrackerConnectivityState::ServiceUnavailable;
    }
    return TrackerConnectivityState::Unknown;
}

bool AppController::IsConnectivityDegradedForProbeInterval(TrackerConnectivityState nextProbeState) const {
    if (nextProbeState == TrackerConnectivityState::TransportDown ||
        nextProbeState == TrackerConnectivityState::ServiceUnavailable) {
        return true;
    }
    if (!LastTrackerTicketSyncWarning.empty() && IsTrackerTransportErrorText(LastTrackerTicketSyncWarning)) {
        return true;
    }
    const std::string& catalogErr = fieldCatalog().LastTrackerFieldCatalogError;
    if (!catalogErr.empty() && IsTrackerTransportErrorText(catalogErr)) {
        return true;
    }
    return false;
}

void AppController::PushOfflineReplayTimersDuringTransportOutage(std::chrono::steady_clock::time_point now) {
    if (!offlineQueue_) {
        return;
    }
    const auto pushTo = now + kOfflineReplayDelayWhileTransportDown;
    offlineQueue_->PushReplayTimersForward(pushTo);
}

void AppController::ApplyTrackerConnectivityProbeResult(const std::chrono::steady_clock::time_point now,
                                                        const TrackerReachabilityProbeResult& r) {
    const TrackerConnectivityState prev = lastTrackerConnectivityState_;
    const TrackerConnectivityState next = MapReachabilityProbeKind(r.Kind);
    lastTrackerConnectivityState_ = next;
    lastTrackerConnectivityDiagnostic_ = r.Diagnostic;

    if (prev != next) {
        LOG_INFO("AppController: Tracker connectivity probe state %d -> %d diag=%s", static_cast<int>(prev),
                 static_cast<int>(next), r.Diagnostic.c_str());
    }

    const bool nowAuthenticatedReachable = (next == TrackerConnectivityState::AuthenticatedReachable);
    const bool wasConnectivityDegraded =
        (prev == TrackerConnectivityState::TransportDown || prev == TrackerConnectivityState::ServiceUnavailable ||
         prev == TrackerConnectivityState::ReachableAuthOrConfigError);
    // First successful probe after startup can be Unknown -> AuthenticatedReachable while we still show
    // an offline/snapshot catalog banner from cache or a failed fetch; nudge a live catalog refresh.
    const bool coldStartCatalogBanner = (prev == TrackerConnectivityState::Unknown && nowAuthenticatedReachable &&
                                         !fieldCatalog().LastTrackerFieldCatalogWarning.empty());
    if (nowAuthenticatedReachable && (wasConnectivityDegraded || coldStartCatalogBanner)) {
        if (!trackerConnectivityRecoveryPending_) {
            trackerConnectivityRecoveryPending_ = true;
            LOG_INFO("AppController: Tracker authenticated reachability restored; UI recovery pending.");
        }
    }

    if (prev == TrackerConnectivityState::AuthenticatedReachable &&
        (next == TrackerConnectivityState::TransportDown || next == TrackerConnectivityState::ServiceUnavailable)) {
        std::string diag = r.Diagnostic;
        constexpr std::size_t kMaxDiagChars = 200;
        if (diag.size() > kMaxDiagChars) {
            diag.resize(kMaxDiagChars);
        }
        LastTrackerTicketSyncWarning = "Showing cached issues — lost connection to tracker: " + diag;
        LOG_WARN("AppController: Tracker probe reports connectivity loss: %s", diag.c_str());
    }

    if (next == TrackerConnectivityState::TransportDown || next == TrackerConnectivityState::ServiceUnavailable) {
        PushOfflineReplayTimersDuringTransportOutage(now);
    }

    const auto interval = IsConnectivityDegradedForProbeInterval(next) ? kTrackerConnectivityProbeAggressiveInterval
                                                                       : kTrackerConnectivityProbeRelaxedInterval;
    nextTrackerConnectivityProbeAt_ = now + interval;
}

void AppController::TickTrackerConnectivityMonitor(const TrackerConfig& cfg) {
    if (!focusedContext().Backend || shuttingDown_.load()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();

    if (trackerConnectivityProbeInFlight_) {
        if (trackerConnectivityProbeFuture_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            TrackerReachabilityProbeResult r;
            try {
                r = trackerConnectivityProbeFuture_.get();
            } catch (const std::exception& ex) {
                r.Kind = TrackerReachabilityProbeKind::TransportDown;
                r.Diagnostic = std::string("probe future exception: ") + ex.what();
                LOG_WARN("TrackerConnectivityProbe: future get failed: %s", ex.what());
            } catch (...) {
                r.Kind = TrackerReachabilityProbeKind::TransportDown;
                r.Diagnostic = "probe future exception: unknown";
                LOG_WARN("TrackerConnectivityProbe: future get failed: unknown exception");
            }
            trackerConnectivityProbeInFlight_ = false;
            ApplyTrackerConnectivityProbeResult(now, r);
        }
        return;
    }

    if (now < nextTrackerConnectivityProbeAt_) {
        return;
    }

    // No explicit auth check here; each backend handles its own config validation in ProbeReachability.

    try {
        // Latch a strong handle before dispatching: a live tracker swap (SetBackend) while the
        // probe runs would otherwise free the backend under the async worker (ADR 0012).
        std::shared_ptr<ITrackerBackend> backend = std::atomic_load(&focusedContext().Backend);
        if (!backend) {
            nextTrackerConnectivityProbeAt_ = now + kTrackerConnectivityProbeAggressiveInterval;
            return;
        }
        trackerConnectivityProbeFuture_ =
            std::async(std::launch::async, [backend, cfg]() { return backend->Connectivity().ProbeReachability(cfg); });
        trackerConnectivityProbeInFlight_ = true;
    } catch (const std::exception& ex) {
        LOG_WARN("TrackerConnectivityProbe: failed to launch: %s", ex.what());
        nextTrackerConnectivityProbeAt_ = now + kTrackerConnectivityProbeAggressiveInterval;
    } catch (...) {
        LOG_WARN("TrackerConnectivityProbe: failed to launch: unknown exception");
        nextTrackerConnectivityProbeAt_ = now + kTrackerConnectivityProbeAggressiveInterval;
    }
}

bool AppController::ConsumeFieldCatalogRefetchAfterLiveTicketSync() {
    return fieldCatalogRefetchAfterLiveTicketSyncPending_.exchange(false, std::memory_order_acq_rel);
}

void AppController::requestDeferredLiveTrackerBackendSuccessNotify_() const {
    if (!focusedContext().Backend) {
        return;
    }
    deferredLiveTrackerBackendSuccessNotify_.store(true, std::memory_order_release);
}

void AppController::applyLiveTrackerReachabilityAfterSuccessfulBackendRequest_() {
    if (!focusedContext().Backend) {
        return;
    }
    lastTrackerConnectivityState_ = TrackerConnectivityState::AuthenticatedReachable;
    LastTrackerTicketSyncWarning.clear();
    // Latch the catalog once: fieldCatalog() re-resolves focusedContextPtr_ per call; a focus
    // switch between two calls would split the check/clear across two contexts (Pillar 3).
    GridContextFieldCatalog& cat = fieldCatalog();
    if (!cat.LastTrackerFieldCatalogWarning.empty()) {
        cat.LastTrackerFieldCatalogWarning.clear();
        cat.TrackerFieldCatalogRevision.fetch_add(1);
        fieldCatalogRefetchAfterLiveTicketSyncPending_.store(true, std::memory_order_release);
    }
}

bool AppController::ConsumeDeferredLiveTrackerBackendSuccessNotifyIfAny() {
    if (!deferredLiveTrackerBackendSuccessNotify_.exchange(false, std::memory_order_acq_rel)) {
        return false;
    }
    applyLiveTrackerReachabilityAfterSuccessfulBackendRequest_();
    return true;
}

namespace {

constexpr char kWorkingOfflineSnapshotCatalog[] =
    "Working offline: Jira field catalog loaded from local snapshot until a live refresh succeeds.";

std::string TruncateTrackerBannerDetail(const std::string& s, std::size_t maxLen) {
    if (s.size() <= maxLen) {
        return s;
    }
    if (maxLen <= 3) {
        return "...";
    }
    return s.substr(0, maxLen - 3) + "...";
}

std::string CatalogOfflineTechnicalSuffix(const std::string& cw) {
    if (cw.empty()) {
        return std::string();
    }
    static const char* prefixes[] = {
        "Offline: using cached tracker field catalog. Last fetch failed: ",
        "Offline: restored tracker field catalog from local snapshot. Last fetch failed: ",
        "Offline: no field catalog snapshot could be loaded. Last fetch failed: ",
    };
    for (const char* p : prefixes) {
        const size_t pl = std::strlen(p);
        if (cw.size() >= pl && cw.compare(0, pl, p) == 0) {
            return cw.substr(pl);
        }
    }
    if (cw == kWorkingOfflineSnapshotCatalog) {
        return std::string();
    }
    return TruncateTrackerBannerDetail(cw, 100);
}

std::string TicketOfflineTechnicalSuffix(const std::string& tw) {
    if (tw.empty()) {
        return std::string();
    }
    static const char* prefixes[] = {
        "Showing cached issues — live refresh did not complete: ",
        "Showing cached issues — lost connection to tracker: ",
        // ASCII hyphen (some logs / older strings / copy-paste normalization).
        "Showing cached issues - live refresh did not complete: ",
        "Showing cached issues - lost connection to tracker: ",
    };
    for (const char* p : prefixes) {
        const size_t pl = std::strlen(p);
        if (tw.size() >= pl && tw.compare(0, pl, p) == 0) {
            return tw.substr(pl);
        }
    }
    return TruncateTrackerBannerDetail(tw, 100);
}

void AppendSessionCatalogNoteToBanner(std::string& out, const std::string* sessionNote) {
    if (!sessionNote || sessionNote->empty()) {
        return;
    }
    if (!out.empty()) {
        out += " ";
    }
    out += "(" + TruncateTrackerBannerDetail(*sessionNote, 90) + ")";
}

} // namespace

TrackerConnectivityBannerForUi
AppController::GetTrackerConnectivityBannerForUi(const std::string* sessionCatalogNote) const {
    TrackerConnectivityBannerForUi out;
    // Latch the catalog once: both refs below must come from the SAME context (Pillar 3).
    const GridContextFieldCatalog& cat = fieldCatalog();
    const std::string& ce = cat.LastTrackerFieldCatalogError;
    const std::string& cw = cat.LastTrackerFieldCatalogWarning;
    const std::string& tw = LastTrackerTicketSyncWarning;
    const bool haveSession = sessionCatalogNote && !sessionCatalogNote->empty();
    const bool haveCw = !cw.empty();
    const bool haveTw = !tw.empty();

    if (!ce.empty()) {
        out.Kind = TrackerConnectivityBannerForUi::Level::Error;
        out.Message = TruncateTrackerBannerDetail(ce, 240);
        if (haveTw) {
            const std::string ts = TicketOfflineTechnicalSuffix(tw);
            if (!ts.empty()) {
                out.Message += " · Issues: ";
                out.Message += TruncateTrackerBannerDetail(ts, 100);
            }
        }
        AppendSessionCatalogNoteToBanner(out.Message, sessionCatalogNote);
        return out;
    }

    if (!haveCw && !haveTw && haveSession) {
        out.Kind = TrackerConnectivityBannerForUi::Level::Warning;
        out.Message = "Offline: ";
        out.Message += TruncateTrackerBannerDetail(*sessionCatalogNote, 220);
        return out;
    }

    if (!haveCw && !haveTw) {
        return out;
    }

    out.Kind = TrackerConnectivityBannerForUi::Level::Warning;
    const std::string catSuffix = CatalogOfflineTechnicalSuffix(cw);
    const std::string ticketSuffix = TicketOfflineTechnicalSuffix(tw);
    const bool snapshotCatalogOnly = haveCw && cw == kWorkingOfflineSnapshotCatalog;

    std::string headline;
    if (haveCw && haveTw) {
        headline = "Offline: issue list and field catalog are not live with tracker.";
    } else if (haveCw) {
        headline = snapshotCatalogOnly ? "Offline: field catalog is a local snapshot until a live refresh succeeds."
                                       : "Offline: field catalog is from cache (not refreshed from tracker).";
    } else {
        headline = "Offline: issue list is from cache (live refresh did not complete).";
    }

    std::string detail;
    if (!catSuffix.empty() && !ticketSuffix.empty()) {
        if (catSuffix == ticketSuffix) {
            detail = catSuffix;
        } else {
            detail = TruncateTrackerBannerDetail(catSuffix, 70) + " · " + TruncateTrackerBannerDetail(ticketSuffix, 70);
        }
    } else if (!catSuffix.empty()) {
        detail = catSuffix;
    } else if (!ticketSuffix.empty()) {
        detail = ticketSuffix;
    }

    out.Message = headline;
    if (!detail.empty()) {
        out.Message += " — ";
        out.Message += TruncateTrackerBannerDetail(detail, 130);
    }
    AppendSessionCatalogNoteToBanner(out.Message, sessionCatalogNote);
    return out;
}

bool AppController::ConsumeTrackerConnectivityRecovery() {
    if (!trackerConnectivityRecoveryPending_) {
        return false;
    }
    trackerConnectivityRecoveryPending_ = false;
    LastTrackerTicketSyncWarning.clear();
    fieldCatalog().LastTrackerFieldCatalogWarning.clear();
    const auto now = std::chrono::steady_clock::now();
    if (offlineQueue_) {
        offlineQueue_->RestartReplayTimersNow(now);
    }
    LOG_INFO("AppController: consumed tracker connectivity recovery latch.");
    return true;
}

#include "ConnectivityMonitorService.h"

#include "IConnectivityDeps.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <future>
#include <memory>
#include <string>

#include "Config/ConfigManager.h" // TrackerConfig (by-value probe capture)
#include "ITrackerBackend.h"
#include "ITrackerConnectivity.h"
#include "TrackerHttpPure.h" // IsTrackerTransportErrorText (cpr-free). This TU links into the
                             // headless TSan subset (SmatchetTsanTests), where cpr is gated OFF
                             // (app-only — see SMATCHET_BUILD_APP in the root CMakeLists).
                             // Including TrackerHttpUtils.h dragged <cpr/cpr.h> into that target,
                             // so a cold FetchContent cache (no cpr-src) failed the build. The
                             // Pure header declares the only HTTP symbol this TU uses.
#include "AiErrorRedact.h"
#include "Logger.h"

namespace {
constexpr auto kTrackerConnectivityProbeAggressiveInterval = std::chrono::seconds{20};
constexpr auto kTrackerConnectivityProbeRelaxedInterval = std::chrono::seconds{90};
constexpr auto kOfflineReplayDelayWhileTransportDown = std::chrono::seconds{25};
} // namespace

ConnectivityMonitorService::ConnectivityMonitorService(IConnectivityDeps& deps) : deps_(deps) {}

void ConnectivityMonitorService::DrainProbeFuture() {
    if (!probeInFlight_) {
        return;
    }
    try {
        if (probeFuture_.valid()) {
            probeFuture_.wait();
            (void)probeFuture_.get();
        }
    } catch (const std::exception& ex) {
        LOG_DEBUG("CancelTrackerConnectivityProbe: shutdown swallow: %s", ex.what());
    } catch (...) {
        // catch-all-ok: shutdown path — swallow probe failures.
        LOG_DEBUG("CancelTrackerConnectivityProbe: shutdown swallow: unknown exception");
    }
    probeInFlight_ = false;
}

TrackerConnectivityState ConnectivityMonitorService::MapReachabilityProbeKind(TrackerReachabilityProbeKind k) {
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

bool ConnectivityMonitorService::IsConnectivityDegradedForProbeInterval(TrackerConnectivityState nextProbeState) const {
    if (nextProbeState == TrackerConnectivityState::TransportDown ||
        nextProbeState == TrackerConnectivityState::ServiceUnavailable) {
        return true;
    }
    if (!lastTicketSyncWarning_.empty() && IsTrackerTransportErrorText(lastTicketSyncWarning_)) {
        return true;
    }
    const std::string& catalogErr = deps_.FocusedFieldCatalogError();
    if (!catalogErr.empty() && IsTrackerTransportErrorText(catalogErr)) {
        return true;
    }
    return false;
}

void ConnectivityMonitorService::PushOfflineReplayTimersDuringTransportOutage(
    std::chrono::steady_clock::time_point now) {
    const auto pushTo = now + kOfflineReplayDelayWhileTransportDown;
    deps_.PushReplayTimers(pushTo);
}

void ConnectivityMonitorService::ApplyTrackerConnectivityProbeResult(const std::chrono::steady_clock::time_point now,
                                                                     const TrackerReachabilityProbeResult& r) {
    const TrackerConnectivityState prev = lastState_;
    const TrackerConnectivityState next = MapReachabilityProbeKind(r.Kind);
    lastState_ = next;
    lastDiagnostic_ = r.Diagnostic;

    if (prev != next) {
        LOG_INFO("ConnectivityMonitor: Tracker connectivity probe state %d -> %d diag=%s", static_cast<int>(prev),
                 static_cast<int>(next), r.Diagnostic.c_str());
    }

    const bool nowAuthenticatedReachable = (next == TrackerConnectivityState::AuthenticatedReachable);
    const bool wasConnectivityDegraded =
        (prev == TrackerConnectivityState::TransportDown || prev == TrackerConnectivityState::ServiceUnavailable ||
         prev == TrackerConnectivityState::ReachableAuthOrConfigError);
    // First successful probe after startup can be Unknown -> AuthenticatedReachable while we still show
    // an offline/snapshot catalog banner from cache or a failed fetch; nudge a live catalog refresh.
    const bool coldStartCatalogBanner = (prev == TrackerConnectivityState::Unknown && nowAuthenticatedReachable &&
                                         !deps_.FocusedFieldCatalogWarning().empty());
    if (nowAuthenticatedReachable && (wasConnectivityDegraded || coldStartCatalogBanner)) {
        if (!recoveryPending_) {
            recoveryPending_ = true;
            LOG_INFO("ConnectivityMonitor: Tracker authenticated reachability restored; UI recovery pending.");
        }
    }

    if (prev == TrackerConnectivityState::AuthenticatedReachable &&
        (next == TrackerConnectivityState::TransportDown || next == TrackerConnectivityState::ServiceUnavailable)) {
        std::string diag = r.Diagnostic;
        constexpr std::size_t kMaxDiagChars = 200;
        if (diag.size() > kMaxDiagChars) {
            diag.resize(kMaxDiagChars);
        }
        lastTicketSyncWarning_ = "Showing cached issues — lost connection to tracker: " + diag;
        LOG_WARN("ConnectivityMonitor: Tracker probe reports connectivity loss: %s", diag.c_str());
    }

    if (next == TrackerConnectivityState::TransportDown || next == TrackerConnectivityState::ServiceUnavailable) {
        PushOfflineReplayTimersDuringTransportOutage(now);
    }

    const auto interval = IsConnectivityDegradedForProbeInterval(next) ? kTrackerConnectivityProbeAggressiveInterval
                                                                       : kTrackerConnectivityProbeRelaxedInterval;
    nextProbeAt_ = now + interval;
}

void ConnectivityMonitorService::TickTrackerConnectivityMonitor(const TrackerConfig& cfg) {
    if (!deps_.FocusedBackendShared() || deps_.IsShuttingDown()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();

    if (probeInFlight_) {
        if (probeFuture_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            TrackerReachabilityProbeResult r;
            try {
                r = probeFuture_.get();
            } catch (const std::exception& ex) {
                r.Kind = TrackerReachabilityProbeKind::TransportDown;
                r.Diagnostic = std::string("probe future exception: ") + ex.what();
                LOG_WARN("TrackerConnectivityProbe: future get failed: %s", ex.what());
            } catch (...) {
                r.Kind = TrackerReachabilityProbeKind::TransportDown;
                r.Diagnostic = "probe future exception: unknown";
                LOG_WARN("TrackerConnectivityProbe: future get failed: unknown exception");
            }
            probeInFlight_ = false;
            ApplyTrackerConnectivityProbeResult(now, r);
        }
        return;
    }

    if (now < nextProbeAt_) {
        return;
    }

    // No explicit auth check here; each backend handles its own config validation in ProbeReachability.

    try {
        // Latch a strong handle before dispatching: a live tracker swap (SetBackend) while the
        // probe runs would otherwise free the backend under the async worker (ADR 0012).
        std::shared_ptr<ITrackerBackend> backend = deps_.FocusedBackendShared();
        if (!backend) {
            nextProbeAt_ = now + kTrackerConnectivityProbeAggressiveInterval;
            return;
        }
        probeFuture_ =
            std::async(std::launch::async, [backend, cfg]() { return backend->Connectivity().ProbeReachability(cfg); });
        probeInFlight_ = true;
    } catch (const std::exception& ex) {
        LOG_WARN("TrackerConnectivityProbe: failed to launch: %s", ex.what());
        nextProbeAt_ = now + kTrackerConnectivityProbeAggressiveInterval;
    } catch (...) {
        LOG_WARN("TrackerConnectivityProbe: failed to launch: unknown exception");
        nextProbeAt_ = now + kTrackerConnectivityProbeAggressiveInterval;
    }
}

bool ConnectivityMonitorService::ConsumeFieldCatalogRefetchAfterLiveTicketSync() {
    return fieldCatalogRefetchAfterLiveTicketSyncPending_.exchange(false, std::memory_order_acq_rel);
}

void ConnectivityMonitorService::RequestDeferredLiveTrackerBackendSuccessNotify() const {
    if (!deps_.FocusedBackendShared()) {
        return;
    }
    deferredLiveTrackerBackendSuccessNotify_.store(true, std::memory_order_release);
}

void ConnectivityMonitorService::applyLiveTrackerReachabilityAfterSuccessfulBackendRequest_() {
    if (!deps_.FocusedBackendShared()) {
        return;
    }
    lastState_ = TrackerConnectivityState::AuthenticatedReachable;
    lastTicketSyncWarning_.clear();
    // Clear the LIVE-focus catalog warning once: the adapter latches the focused catalog so the
    // check/clear/bump stay on the SAME context even if focus moves between deps calls (Pillar 3).
    if (!deps_.FocusedFieldCatalogWarning().empty()) {
        deps_.ClearFocusedFieldCatalogWarning();
        deps_.BumpFocusedFieldCatalogRevision();
        fieldCatalogRefetchAfterLiveTicketSyncPending_.store(true, std::memory_order_release);
    }
}

bool ConnectivityMonitorService::ConsumeDeferredLiveTrackerBackendSuccessNotifyIfAny() {
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
            // The suffix embeds the raw fetch error (cpr transport / backend text) — scrub
            // secret-shaped tokens before it reaches the banner (reuses the AI-side redactor).
            return smatchet::ai::pure::RedactProviderErrorBody(cw.substr(pl));
        }
    }
    if (cw == kWorkingOfflineSnapshotCatalog) {
        return std::string();
    }
    return TruncateTrackerBannerDetail(smatchet::ai::pure::RedactProviderErrorBody(cw), 100);
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
            return smatchet::ai::pure::RedactProviderErrorBody(tw.substr(pl));
        }
    }
    return TruncateTrackerBannerDetail(smatchet::ai::pure::RedactProviderErrorBody(tw), 100);
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

TrackerConnectivityBannerForUi ConnectivityMonitorService::GetBannerForUi(const std::string* sessionCatalogNote) const {
    TrackerConnectivityBannerForUi out;
    // LIVE-focus catalog reads through the deps adapter (the adapter latches the focused catalog so
    // both refs come from the SAME context — Pillar 3).
    const std::string& ce = deps_.FocusedFieldCatalogError();
    const std::string& cw = deps_.FocusedFieldCatalogWarning();
    const std::string& tw = lastTicketSyncWarning_;
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
        out.Message = TruncateTrackerBannerDetail(*sessionCatalogNote, 220);
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

bool ConnectivityMonitorService::ConsumeTrackerConnectivityRecovery() {
    if (!recoveryPending_) {
        return false;
    }
    recoveryPending_ = false;
    lastTicketSyncWarning_.clear();
    deps_.ClearFocusedFieldCatalogWarning();
    const auto now = std::chrono::steady_clock::now();
    deps_.RestartReplayTimers(now);
    LOG_INFO("ConnectivityMonitor: consumed tracker connectivity recovery latch.");
    return true;
}

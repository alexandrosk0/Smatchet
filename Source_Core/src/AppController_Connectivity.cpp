#include "AppController.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <mutex>
#include <string>

#include "ConfigManager.h"
#include "JiraClient.h"
#include "JiraHttpUtils.h"
#include "Logger.h"
#include "StringUtil.h"

namespace {
constexpr auto kJiraConnectivityProbeAggressiveInterval = std::chrono::seconds{20};
constexpr auto kJiraConnectivityProbeRelaxedInterval = std::chrono::seconds{90};
constexpr auto kOfflineReplayDelayWhileTransportDown = std::chrono::seconds{25};
} // namespace

void AppController::DrainJiraConnectivityProbeFuture() {
    if (!jiraConnectivityProbeInFlight_) {
        return;
    }
    try {
        if (jiraConnectivityProbeFuture_.valid()) {
            jiraConnectivityProbeFuture_.wait();
            (void)jiraConnectivityProbeFuture_.get();
        }
    } catch (...) {
        // Shutdown path: swallow probe failures.
    }
    jiraConnectivityProbeInFlight_ = false;
}

AppController::JiraConnectivityState AppController::MapReachabilityProbeKind(JiraReachabilityProbeKind k) {
    switch (k) {
    case JiraReachabilityProbeKind::AuthenticatedReachable:
        return JiraConnectivityState::AuthenticatedReachable;
    case JiraReachabilityProbeKind::ReachableAuthOrConfigError:
        return JiraConnectivityState::ReachableAuthOrConfigError;
    case JiraReachabilityProbeKind::TransportDown:
        return JiraConnectivityState::TransportDown;
    case JiraReachabilityProbeKind::ServiceUnavailable:
        return JiraConnectivityState::ServiceUnavailable;
    }
    return JiraConnectivityState::Unknown;
}

bool AppController::IsConnectivityDegradedForProbeInterval(JiraConnectivityState nextProbeState) const {
    if (nextProbeState == JiraConnectivityState::TransportDown ||
        nextProbeState == JiraConnectivityState::ServiceUnavailable) {
        return true;
    }
    if (!LastJiraTicketSyncWarning.empty() && IsJiraTransportErrorText(LastJiraTicketSyncWarning)) {
        return true;
    }
    const std::string& catalogErr = LastJiraFieldCatalogError;
    if (!catalogErr.empty() && IsJiraTransportErrorText(catalogErr)) {
        return true;
    }
    return false;
}

void AppController::PushOfflineReplayTimersDuringTransportOutage(std::chrono::steady_clock::time_point now) {
    const auto pushTo = now + kOfflineReplayDelayWhileTransportDown;
    std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
    nextOfflineReplayAt_ = (std::max)(nextOfflineReplayAt_, pushTo);
    nextOfflineFieldEditReplayAt_ = (std::max)(nextOfflineFieldEditReplayAt_, pushTo);
}

void AppController::ApplyJiraConnectivityProbeResult(const std::chrono::steady_clock::time_point now,
                                                     const JiraReachabilityProbeResult& r) {
    const JiraConnectivityState prev = lastJiraConnectivityState_;
    const JiraConnectivityState next = MapReachabilityProbeKind(r.Kind);
    lastJiraConnectivityState_ = next;
    lastJiraConnectivityDiagnostic_ = r.Diagnostic;

    if (prev != next) {
        LOG_INFO("AppController: Jira connectivity probe state %d -> %d diag=%s", static_cast<int>(prev),
                 static_cast<int>(next), r.Diagnostic.c_str());
    }

    const bool nowAuthenticatedReachable = (next == JiraConnectivityState::AuthenticatedReachable);
    const bool wasConnectivityDegraded =
        (prev == JiraConnectivityState::TransportDown || prev == JiraConnectivityState::ServiceUnavailable ||
         prev == JiraConnectivityState::ReachableAuthOrConfigError);
    // First successful probe after startup can be Unknown -> AuthenticatedReachable while we still show
    // an offline/snapshot catalog banner from cache or a failed fetch; nudge a live catalog refresh.
    const bool coldStartCatalogBanner =
        (prev == JiraConnectivityState::Unknown && nowAuthenticatedReachable && !LastJiraFieldCatalogWarning.empty());
    if (nowAuthenticatedReachable && (wasConnectivityDegraded || coldStartCatalogBanner)) {
        if (!jiraConnectivityRecoveryPending_) {
            jiraConnectivityRecoveryPending_ = true;
            LOG_INFO("AppController: Jira authenticated reachability restored; UI recovery pending.");
        }
    }

    if (prev == JiraConnectivityState::AuthenticatedReachable &&
        (next == JiraConnectivityState::TransportDown || next == JiraConnectivityState::ServiceUnavailable)) {
        std::string diag = r.Diagnostic;
        constexpr std::size_t kMaxDiagChars = 200;
        if (diag.size() > kMaxDiagChars) {
            diag.resize(kMaxDiagChars);
        }
        LastJiraTicketSyncWarning = "Showing cached issues — lost connection to Jira: " + diag;
        LOG_WARN("AppController: Jira probe reports connectivity loss: %s", diag.c_str());
    }

    if (next == JiraConnectivityState::TransportDown || next == JiraConnectivityState::ServiceUnavailable) {
        PushOfflineReplayTimersDuringTransportOutage(now);
    }

    const auto interval = IsConnectivityDegradedForProbeInterval(next) ? kJiraConnectivityProbeAggressiveInterval
                                                                       : kJiraConnectivityProbeRelaxedInterval;
    nextJiraConnectivityProbeAt_ = now + interval;
}

void AppController::TickJiraConnectivityMonitor(const JiraConfig& cfg) {
    if (!JiraBackend || shuttingDown_.load()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();

    if (jiraConnectivityProbeInFlight_) {
        if (jiraConnectivityProbeFuture_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            JiraReachabilityProbeResult r;
            try {
                r = jiraConnectivityProbeFuture_.get();
            } catch (...) {
                r.Kind = JiraReachabilityProbeKind::TransportDown;
                r.Diagnostic = "probe future exception";
            }
            jiraConnectivityProbeInFlight_ = false;
            ApplyJiraConnectivityProbeResult(now, r);
        }
        return;
    }

    if (now < nextJiraConnectivityProbeAt_) {
        return;
    }

    std::string authGate;
    if (!EnsureJiraAuthConfig(cfg, authGate)) {
        nextJiraConnectivityProbeAt_ = now + kJiraConnectivityProbeRelaxedInterval;
        return;
    }

    try {
        jiraConnectivityProbeFuture_ =
            std::async(std::launch::async, [cfg]() { return JiraClient::ProbeReachability(cfg); });
        jiraConnectivityProbeInFlight_ = true;
    } catch (...) {
        nextJiraConnectivityProbeAt_ = now + kJiraConnectivityProbeAggressiveInterval;
    }
}

bool AppController::ConsumeFieldCatalogRefetchAfterLiveTicketSync() {
    return fieldCatalogRefetchAfterLiveTicketSyncPending_.exchange(false, std::memory_order_acq_rel);
}

void AppController::requestDeferredLiveJiraBackendSuccessNotify_() const {
    if (!JiraBackend) {
        return;
    }
    deferredLiveJiraBackendSuccessNotify_.store(true, std::memory_order_release);
}

void AppController::applyLiveJiraReachabilityAfterSuccessfulBackendRequest_() {
    if (!JiraBackend) {
        return;
    }
    lastJiraConnectivityState_ = JiraConnectivityState::AuthenticatedReachable;
    LastJiraTicketSyncWarning.clear();
    if (!LastJiraFieldCatalogWarning.empty()) {
        LastJiraFieldCatalogWarning.clear();
        JiraFieldCatalogRevision.fetch_add(1);
        fieldCatalogRefetchAfterLiveTicketSyncPending_.store(true, std::memory_order_release);
    }
}

bool AppController::ConsumeDeferredLiveJiraBackendSuccessNotifyIfAny() {
    if (!deferredLiveJiraBackendSuccessNotify_.exchange(false, std::memory_order_acq_rel)) {
        return false;
    }
    applyLiveJiraReachabilityAfterSuccessfulBackendRequest_();
    return true;
}

namespace {

constexpr char kWorkingOfflineSnapshotCatalog[] =
    "Working offline: Jira field catalog loaded from local snapshot until a live refresh succeeds.";

std::string TruncateJiraBannerDetail(const std::string& s, std::size_t maxLen) {
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
        "Offline: using cached Jira field catalog. Last fetch failed: ",
        "Offline: restored Jira field catalog from local snapshot. Last fetch failed: ",
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
    return TruncateJiraBannerDetail(cw, 100);
}

std::string TicketOfflineTechnicalSuffix(const std::string& tw) {
    if (tw.empty()) {
        return std::string();
    }
    static const char* prefixes[] = {
        "Showing cached issues — live refresh did not complete: ",
        "Showing cached issues — lost connection to Jira: ",
        // ASCII hyphen (some logs / older strings / copy-paste normalization).
        "Showing cached issues - live refresh did not complete: ",
        "Showing cached issues - lost connection to Jira: ",
    };
    for (const char* p : prefixes) {
        const size_t pl = std::strlen(p);
        if (tw.size() >= pl && tw.compare(0, pl, p) == 0) {
            return tw.substr(pl);
        }
    }
    return TruncateJiraBannerDetail(tw, 100);
}

void AppendSessionCatalogNoteToBanner(std::string& out, const std::string* sessionNote) {
    if (!sessionNote || sessionNote->empty()) {
        return;
    }
    if (!out.empty()) {
        out += " ";
    }
    out += "(" + TruncateJiraBannerDetail(*sessionNote, 90) + ")";
}

} // namespace

JiraConnectivityBannerForUi AppController::GetJiraConnectivityBannerForUi(const std::string* sessionCatalogNote) const {
    JiraConnectivityBannerForUi out;
    const std::string& ce = LastJiraFieldCatalogError;
    const std::string& cw = LastJiraFieldCatalogWarning;
    const std::string& tw = LastJiraTicketSyncWarning;
    const bool haveSession = sessionCatalogNote && !sessionCatalogNote->empty();
    const bool haveCw = !cw.empty();
    const bool haveTw = !tw.empty();

    if (!ce.empty()) {
        out.Kind = JiraConnectivityBannerForUi::Level::Error;
        out.Message = TruncateJiraBannerDetail(ce, 240);
        if (haveTw) {
            const std::string ts = TicketOfflineTechnicalSuffix(tw);
            if (!ts.empty()) {
                out.Message += " · Issues: ";
                out.Message += TruncateJiraBannerDetail(ts, 100);
            }
        }
        AppendSessionCatalogNoteToBanner(out.Message, sessionCatalogNote);
        return out;
    }

    if (!haveCw && !haveTw && haveSession) {
        out.Kind = JiraConnectivityBannerForUi::Level::Warning;
        out.Message = "Offline: ";
        out.Message += TruncateJiraBannerDetail(*sessionCatalogNote, 220);
        return out;
    }

    if (!haveCw && !haveTw) {
        return out;
    }

    out.Kind = JiraConnectivityBannerForUi::Level::Warning;
    const std::string catSuffix = CatalogOfflineTechnicalSuffix(cw);
    const std::string ticketSuffix = TicketOfflineTechnicalSuffix(tw);
    const bool snapshotCatalogOnly = haveCw && cw == kWorkingOfflineSnapshotCatalog;

    std::string headline;
    if (haveCw && haveTw) {
        headline = "Offline: issue list and field catalog are not live with Jira.";
    } else if (haveCw) {
        headline = snapshotCatalogOnly ? "Offline: field catalog is a local snapshot until a live refresh succeeds."
                                       : "Offline: field catalog is from cache (not refreshed from Jira).";
    } else {
        headline = "Offline: issue list is from cache (live refresh did not complete).";
    }

    std::string detail;
    if (!catSuffix.empty() && !ticketSuffix.empty()) {
        if (catSuffix == ticketSuffix) {
            detail = catSuffix;
        } else {
            detail = TruncateJiraBannerDetail(catSuffix, 70) + " · " + TruncateJiraBannerDetail(ticketSuffix, 70);
        }
    } else if (!catSuffix.empty()) {
        detail = catSuffix;
    } else if (!ticketSuffix.empty()) {
        detail = ticketSuffix;
    }

    out.Message = headline;
    if (!detail.empty()) {
        out.Message += " — ";
        out.Message += TruncateJiraBannerDetail(detail, 130);
    }
    AppendSessionCatalogNoteToBanner(out.Message, sessionCatalogNote);
    return out;
}

bool AppController::ConsumeJiraConnectivityRecovery() {
    if (!jiraConnectivityRecoveryPending_) {
        return false;
    }
    jiraConnectivityRecoveryPending_ = false;
    LastJiraTicketSyncWarning.clear();
    LastJiraFieldCatalogWarning.clear();
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
        nextOfflineReplayAt_ = now;
        nextOfflineFieldEditReplayAt_ = now;
    }
    LOG_INFO("AppController: consumed Jira connectivity recovery latch.");
    return true;
}

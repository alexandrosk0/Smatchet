#include "SmatchetGridUiSupport.h"

#include "AppController.h"
#include "SmatchetLocalization.h"
#include "SmatchetUiSession.h"
#include "SmatchetToast.h"

#include <chrono>
#include <string>

namespace {
constexpr auto kTrackerWarningToastStartupGrace = std::chrono::seconds(3);
}

void MaybeToastTrackerConnectivityBanner(const AppController& app, UiDrawSession& d,
                                         const TrackerConnectivityBannerForUi& banner) {
    using Level = TrackerConnectivityBannerForUi::Level;
    if (banner.Kind == Level::None) {
        d.lastToastedTrackerBannerKind = Level::None;
        d.lastToastedTrackerBannerMessage.clear();
        return;
    }
    if (banner.Kind == Level::Warning) {
        const auto now = std::chrono::steady_clock::now();
        if (!d.trackerWarningToastStartupGateInitialized) {
            d.trackerWarningToastStartupGateInitialized = true;
            d.trackerWarningToastStartupGraceUntil = now + kTrackerWarningToastStartupGrace;
        }
        if (now < d.trackerWarningToastStartupGraceUntil) {
            const AppController::TrackerConnectivityState st = app.GetLastTrackerConnectivityState();
            if (st == AppController::TrackerConnectivityState::Unknown ||
                st == AppController::TrackerConnectivityState::AuthenticatedReachable) {
                return;
            }
        }
    }
    if (banner.Kind == d.lastToastedTrackerBannerKind && banner.Message == d.lastToastedTrackerBannerMessage) {
        return;
    }
    std::string body = banner.Message;
    if (banner.Kind == Level::Error) {
        body += "\n\n";
        body +=
            SmatchetLocalization::T("toast.tracker_reachable_required",
                                    "Grid edits and quick comment actions stay disabled until Tracker is reachable.");
        SmatchetToastManager::Instance().Push(SmatchetLocalization::T("toast.tracker", "Tracker"), body,
                                              ToastType::Error, 7000);
    } else {
        SmatchetToastManager::Instance().Push(SmatchetLocalization::T("toast.tracker", "Tracker"), body,
                                              ToastType::Warning, 5500);
    }
    d.lastToastedTrackerBannerKind = banner.Kind;
    d.lastToastedTrackerBannerMessage = banner.Message;
}

void MaybeToastGridBannerFromSession(UiDrawSession& d) {
    int curKind = 0;
    std::string curMsg;
    if (!d.gridEditError.empty()) {
        curKind = 1;
        curMsg = d.gridEditError;
    } else if (!d.gridEditSuccess.empty()) {
        curKind = 2;
        curMsg = d.gridEditSuccess;
    }
    if (curKind == 0) {
        d.lastToastedGridBannerKind = 0;
        d.lastToastedGridBannerMessage.clear();
        return;
    }
    if (curKind == d.lastToastedGridBannerKind && curMsg == d.lastToastedGridBannerMessage) {
        return;
    }
    if (curKind == 1) {
        SmatchetToastManager::Instance().Push(SmatchetLocalization::T("toast.active_project", "Active Project"), curMsg,
                                              ToastType::Error);
    } else {
        SmatchetToastManager::Instance().Push(SmatchetLocalization::T("toast.active_project", "Active Project"), curMsg,
                                              ToastType::Success);
    }
    d.lastToastedGridBannerKind = curKind;
    d.lastToastedGridBannerMessage = std::move(curMsg);
}

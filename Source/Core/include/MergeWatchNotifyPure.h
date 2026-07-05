#pragma once

#include "Ui/ToastHistoryPure.h"

#include <string>

/**
 * Pure request-validation core of the merge-watch notify endpoint (gap map Tier 1 #6 — the
 * SECURITY_AUDIT.md-flagged localhost network listener). Byte-identical lift of the
 * POST /merge-watch/notify handler's parse → shape/type validation → state allow-list →
 * sanitize → toast-plan pipeline out of SmatchetMergeWatchNotifyServer.cpp, so the entire
 * attacker-reachable decision surface is doctestable without cpp-httplib / AppController /
 * the dispatcher. The server shell owns only transport: it maps the plan onto the httplib
 * response and posts the toast to the UI thread when `Ok`.
 *
 * Owner: core (merge-watcher Phase 4b). Test surface: tests/Core/MergeWatchNotifyPure.test.cpp.
 */
namespace MergeWatchNotifyPure {

/** Decision output for one request body: the exact HTTP response, plus the toast when Ok. */
struct NotifyPlan {
    bool Ok = false;
    int HttpStatus = 400;
    std::string ResponseBody; // exact JSON body the endpoint returns
    std::string Title;        // toast title (only when Ok)
    std::string Message;      // sanitized, length-capped toast message (only when Ok)
    ToastType Type = ToastType::Warning;
};

/**
 * Validate one attacker-controlled request body and produce the response + toast plan.
 * Never throws; every rejection is a 400 with a stable, input-free error body (except the
 * bad-JSON path, which embeds ParseBounded's own stable message).
 */
NotifyPlan PlanMergeWatchNotify(const std::string& body);

} // namespace MergeWatchNotifyPure

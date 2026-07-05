#include "MergeWatchNotifyPure.h"

#include "Json/BoundedJsonParse.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <set>
#include <string>

// Byte-identical lift of SmatchetMergeWatchNotifyServer.cpp's request-validation pipeline
// (see the header comment for the extraction rationale). The helpers stay file-local; the
// shell (and tests) consume only PlanMergeWatchNotify.
namespace {

// Per Phase 4a's NOTIFY_STATES set in scripts/dev/merge-watcher.py.
// Server-side allow-list — reject any other state string so a compromised
// (or buggy) caller can't inject arbitrary toast text under an arbitrary
// state label.
const std::set<std::string>& KnownStates() {
    static const std::set<std::string> s = {
        "CI_FAIL", "GH_API_DOWN", "PR_CLOSED_OR_MERGED", "PAGINATION_OVERFLOW", "TIMEOUT", "TRIAGE_BUDGET_EXHAUSTED",
    };
    return s;
}

// Cap message length so a runaway payload can't flood the toast surface OR
// the dispatcher queue. 500 chars matches the per-line cap in
// SmatchetToastManager::Render's wrap path.
constexpr std::size_t kMaxMessageBytes = 500;

// Strip control characters (< 0x20 except \t \r \n) — ImGui renders text
// verbatim + control chars can hose layout. Doesn't escape HTML / markdown
// because ToastManager doesn't render either.
std::string SanitizeText(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 && uc != '\t' && uc != '\n' && uc != '\r') {
            out.push_back('?');
        } else {
            out.push_back(c);
        }
    }
    if (out.size() > kMaxMessageBytes) {
        out.resize(kMaxMessageBytes);
        out.append("...");
    }
    return out;
}

ToastType TypeForState(const std::string& state) {
    if (state == "CI_FAIL" || state == "PAGINATION_OVERFLOW" || state == "TRIAGE_BUDGET_EXHAUSTED") {
        return ToastType::Error;
    }
    if (state == "PR_CLOSED_OR_MERGED") {
        return ToastType::Success;
    }
    return ToastType::Warning;
}

} // namespace

namespace MergeWatchNotifyPure {

NotifyPlan PlanMergeWatchNotify(const std::string& body) {
    NotifyPlan plan;
    // Pillar 3: depth/node-bounded parse + validate before any toast dispatch. The
    // request body is attacker-controlled (localhost POST), so ParseBounded guards
    // against a hostile / oversized payload. It never throws; failure is a non-empty
    // parseErr holding a stable, input-free message (safe to embed in the response).
    std::string parseErr;
    nlohmann::json j = smatchet::json_safe::ParseBounded(body, parseErr);
    if (!parseErr.empty()) {
        plan.HttpStatus = 400;
        plan.ResponseBody = std::string("{\"error\":\"bad JSON: ") + parseErr + "\"}";
        return plan;
    }
    if (!j.is_object() || !j.contains("pr") || !j.contains("state") || !j.contains("message")) {
        plan.HttpStatus = 400;
        plan.ResponseBody = "{\"error\":\"missing required field (pr / state / message)\"}";
        return plan;
    }
    if (!j["pr"].is_number_integer() || !j["state"].is_string() || !j["message"].is_string()) {
        plan.HttpStatus = 400;
        plan.ResponseBody = "{\"error\":\"field type mismatch\"}";
        return plan;
    }
    const int pr = j["pr"].get<int>();
    const std::string state = j["state"].get<std::string>();
    const std::string rawMessage = j["message"].get<std::string>();
    if (KnownStates().find(state) == KnownStates().end()) {
        plan.HttpStatus = 400;
        plan.ResponseBody = "{\"error\":\"unknown state (allowed: CI_FAIL, GH_API_DOWN, "
                            "PR_CLOSED_OR_MERGED, PAGINATION_OVERFLOW, TIMEOUT, "
                            "TRIAGE_BUDGET_EXHAUSTED)\"}";
        return plan;
    }
    plan.Ok = true;
    plan.HttpStatus = 200;
    plan.ResponseBody = "{\"ok\":true}";
    plan.Message = SanitizeText(rawMessage);
    plan.Type = TypeForState(state);
    plan.Title = std::string("Smatchet watcher · PR #") + std::to_string(pr) + " · " + state;
    return plan;
}

} // namespace MergeWatchNotifyPure

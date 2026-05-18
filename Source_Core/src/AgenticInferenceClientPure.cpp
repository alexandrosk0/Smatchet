#include "AgenticInferenceClientPure.h"

#include "Logger.h"

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <set>
#include <string>

namespace AgenticInferenceClientPure {

namespace {

// Once-latched warning helper — keeps `LOG_WARN` from flooding the log under
// steady-state model misbehaviour (e.g. an LLM that consistently emits the
// same unknown action enum across thousands of triage rounds). The dedup
// set is process-local; restarting the app re-arms the latch.
std::set<std::string>& UnknownTopKeysSeen() {
    static std::set<std::string> s;
    return s;
}
std::set<std::string>& UnknownActionsSeen() {
    static std::set<std::string> s;
    return s;
}
// Once-latched warning for "too many proposals" — a runaway response should
// surface in the log, but a steady-state misbehaving model should not spam.
bool g_proposalCapWarned = false;
std::mutex& LatchMutex() {
    static std::mutex m;
    return m;
}

bool LatchAndCheckTopKey(const std::string& key) {
    std::lock_guard<std::mutex> g(LatchMutex());
    return UnknownTopKeysSeen().insert(key).second;
}
bool LatchAndCheckAction(const std::string& s) {
    std::lock_guard<std::mutex> g(LatchMutex());
    return UnknownActionsSeen().insert(s).second;
}

ProposedAction ActionFromString(const std::string& s) {
    if (s == "CommentAdd")
        return ProposedAction::CommentAdd;
    if (s == "LabelAdd")
        return ProposedAction::LabelAdd;
    if (s == "LabelRemove")
        return ProposedAction::LabelRemove;
    if (s == "AssigneeSet")
        return ProposedAction::AssigneeSet;
    if (s == "StateTransition")
        return ProposedAction::StateTransition;
    if (s == "DerivedTicketCreate")
        return ProposedAction::DerivedTicketCreate;
    if (s == "ImplementIssue")
        return ProposedAction::ImplementIssue;
    return ProposedAction::Unknown;
}

// Returns true iff `j` is a JSON string AND its trimmed value is non-empty.
// Empty strings are treated as missing — the LLM should not propose an
// action with an empty payload field, and downstream consumers (audit-trail,
// UI) cannot render an empty body.
bool IsNonEmptyString(const nlohmann::json& j) {
    if (!j.is_string())
        return false;
    const std::string& v = j.get_ref<const std::string&>();
    for (std::size_t i = 0; i < v.size(); ++i) {
        const char c = v[i];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            return true;
    }
    return false;
}

// Per-action payload validators. Each returns true on a valid payload and
// false (with `outReason` populated) on a missing-or-wrong-type required
// field. Extras inside the payload object are accepted.
bool ValidatePayload(ProposedAction action, const nlohmann::json& payload, std::string& outReason) {
    if (!payload.is_object()) {
        outReason = "payload not an object";
        return false;
    }
    switch (action) {
    case ProposedAction::CommentAdd: {
        if (!payload.contains("body") || !IsNonEmptyString(payload.at("body"))) {
            outReason = "CommentAdd missing/empty body";
            return false;
        }
        return true;
    }
    case ProposedAction::LabelAdd:
    case ProposedAction::LabelRemove: {
        if (!payload.contains("label") || !IsNonEmptyString(payload.at("label"))) {
            outReason = "LabelAdd/Remove missing/empty label";
            return false;
        }
        return true;
    }
    case ProposedAction::AssigneeSet: {
        if (!payload.contains("user") || !IsNonEmptyString(payload.at("user"))) {
            outReason = "AssigneeSet missing/empty user";
            return false;
        }
        return true;
    }
    case ProposedAction::StateTransition: {
        if (!payload.contains("state") || !payload.at("state").is_string()) {
            outReason = "StateTransition missing state";
            return false;
        }
        const std::string& st = payload.at("state").get_ref<const std::string&>();
        if (st != "open" && st != "closed") {
            outReason = "StateTransition state not in {open, closed}";
            return false;
        }
        return true;
    }
    case ProposedAction::DerivedTicketCreate: {
        if (!payload.contains("targetTracker") || !payload.at("targetTracker").is_string()) {
            outReason = "DerivedTicketCreate missing targetTracker";
            return false;
        }
        const std::string& tt = payload.at("targetTracker").get_ref<const std::string&>();
        if (tt != "jira" && tt != "plane") {
            outReason = "DerivedTicketCreate targetTracker not in {jira, plane}";
            return false;
        }
        if (!payload.contains("summary") || !IsNonEmptyString(payload.at("summary"))) {
            outReason = "DerivedTicketCreate missing/empty summary";
            return false;
        }
        if (!payload.contains("description") || !IsNonEmptyString(payload.at("description"))) {
            outReason = "DerivedTicketCreate missing/empty description";
            return false;
        }
        return true;
    }
    case ProposedAction::ImplementIssue: {
        if (!payload.contains("complexityHint") || !payload.at("complexityHint").is_string()) {
            outReason = "ImplementIssue missing complexityHint";
            return false;
        }
        const std::string& ch = payload.at("complexityHint").get_ref<const std::string&>();
        if (ch != "low" && ch != "medium" && ch != "high") {
            outReason = "ImplementIssue complexityHint not in {low, medium, high}";
            return false;
        }
        if (!payload.contains("approachOutline") || !IsNonEmptyString(payload.at("approachOutline"))) {
            outReason = "ImplementIssue missing/empty approachOutline";
            return false;
        }
        return true;
    }
    case ProposedAction::Unknown:
    default:
        outReason = "Unknown action — cannot validate payload";
        return false;
    }
}

} // namespace

std::string StripMarkdownCodeFence(const std::string& s) {
    // Conservative fence-strip — only acts on a leading triple-backtick fence
    // (optionally followed by a `json` language tag and newline) AND a
    // trailing triple-backtick fence. Anything else is returned verbatim.
    auto trim_left = [](const std::string& v) {
        std::size_t i = 0;
        while (i < v.size() && (v[i] == ' ' || v[i] == '\t' || v[i] == '\r' || v[i] == '\n'))
            ++i;
        return v.substr(i);
    };
    auto trim_right = [](const std::string& v) {
        if (v.empty())
            return v;
        std::size_t i = v.size();
        while (i > 0 && (v[i - 1] == ' ' || v[i - 1] == '\t' || v[i - 1] == '\r' || v[i - 1] == '\n'))
            --i;
        return v.substr(0, i);
    };
    std::string t = trim_left(trim_right(s));
    if (t.size() < 6)
        return s;
    if (t.compare(0, 3, "```") != 0)
        return s;
    if (t.size() < 3 || t.compare(t.size() - 3, 3, "```") != 0)
        return s;
    // Strip leading fence (with optional `json` tag) and trailing fence.
    std::size_t bodyBegin = 3;
    // Optional language tag — letters only, e.g. ```json or ```JSON.
    while (bodyBegin < t.size()) {
        const char c = t[bodyBegin];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
            ++bodyBegin;
        else
            break;
    }
    // Skip a single CR/LF after the opening fence.
    if (bodyBegin < t.size() && t[bodyBegin] == '\r')
        ++bodyBegin;
    if (bodyBegin < t.size() && t[bodyBegin] == '\n')
        ++bodyBegin;
    std::size_t bodyEnd = t.size() - 3;
    // Trim trailing CR/LF that precede the closing fence so the parser sees
    // a tight JSON body. Some emitters insert a newline before ``` (typical
    // markdown convention), others don't — both are valid input.
    while (bodyEnd > bodyBegin && (t[bodyEnd - 1] == '\r' || t[bodyEnd - 1] == '\n'))
        --bodyEnd;
    if (bodyEnd < bodyBegin)
        return s;
    return t.substr(bodyBegin, bodyEnd - bodyBegin);
}

const char* ActionToString(ProposedAction a) {
    switch (a) {
    case ProposedAction::CommentAdd:
        return "CommentAdd";
    case ProposedAction::LabelAdd:
        return "LabelAdd";
    case ProposedAction::LabelRemove:
        return "LabelRemove";
    case ProposedAction::AssigneeSet:
        return "AssigneeSet";
    case ProposedAction::StateTransition:
        return "StateTransition";
    case ProposedAction::DerivedTicketCreate:
        return "DerivedTicketCreate";
    case ProposedAction::ImplementIssue:
        return "ImplementIssue";
    case ProposedAction::Unknown:
    default:
        return "Unknown";
    }
}

bool ParseInferenceResponse(const std::string& jsonBody, std::vector<ProposalDraft>& outProposals,
                            std::string& outError) {
    outProposals.clear();
    outError.clear();

    const std::string stripped = StripMarkdownCodeFence(jsonBody);
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(stripped);
    } catch (const nlohmann::json::parse_error& e) {
        outError = std::string("JSON parse error: ") + e.what();
        return false;
    }

    if (!root.is_object()) {
        outError = "envelope is not a JSON object";
        return false;
    }
    if (!root.contains("proposals")) {
        outError = "missing `proposals` key";
        return false;
    }
    const nlohmann::json& proposalsNode = root.at("proposals");
    if (!proposalsNode.is_array()) {
        outError = "`proposals` is not an array";
        return false;
    }

    // Once-latched warning for unknown top-level keys (decision #3 — extras
    // are accepted but logged once each per process lifetime).
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (it.key() == "proposals")
            continue;
        if (LatchAndCheckTopKey(it.key())) {
            LOG_WARN("AgenticInferenceClientPure: unknown top-level envelope key '%s' — ignored",
                     it.key().c_str());
        }
    }

    // Bundle C CR#228:275 — cap the reserve at kMaxProposalsPerResponse to keep
    // a hostile / runaway LLM response from OOMing the process via the reserve
    // hint alone. Indexes past the cap are skipped (the loop bound is the
    // capped value). Once-latched warn so the operator sees the event but the
    // log isn't spammed by a stuck-in-loop model.
    const std::size_t capped = std::min(proposalsNode.size(), kMaxProposalsPerResponse);
    if (proposalsNode.size() > kMaxProposalsPerResponse) {
        std::lock_guard<std::mutex> g(LatchMutex());
        if (!g_proposalCapWarned) {
            LOG_WARN("AgenticInferenceClientPure: response carried %zu proposals — capped to %zu",
                     proposalsNode.size(), kMaxProposalsPerResponse);
            g_proposalCapWarned = true;
        }
    }
    outProposals.reserve(capped);
    for (std::size_t i = 0; i < capped; ++i) {
        const nlohmann::json& p = proposalsNode.at(i);
        if (!p.is_object()) {
            LOG_WARN("AgenticInferenceClientPure: proposal #%zu is not an object — dropping", i);
            continue;
        }
        if (!p.contains("action") || !p.at("action").is_string()) {
            LOG_WARN("AgenticInferenceClientPure: proposal #%zu missing/non-string `action` — dropping", i);
            continue;
        }
        const std::string actionStr = p.at("action").get<std::string>();
        const ProposedAction action = ActionFromString(actionStr);
        if (action == ProposedAction::Unknown) {
            if (LatchAndCheckAction(actionStr)) {
                LOG_WARN("AgenticInferenceClientPure: unknown action enum '%s' — dropping proposal #%zu",
                         actionStr.c_str(), i);
            }
            continue;
        }
        if (!p.contains("payload")) {
            LOG_WARN("AgenticInferenceClientPure: proposal #%zu (action=%s) missing payload — dropping", i,
                     actionStr.c_str());
            continue;
        }
        const nlohmann::json& payload = p.at("payload");
        std::string reason;
        if (!ValidatePayload(action, payload, reason)) {
            LOG_WARN("AgenticInferenceClientPure: proposal #%zu (action=%s) invalid: %s — dropping", i,
                     actionStr.c_str(), reason.c_str());
            continue;
        }
        ProposalDraft draft;
        draft.action = action;
        if (p.contains("rationale") && p.at("rationale").is_string())
            draft.rationale = p.at("rationale").get<std::string>();
        draft.payload = payload;
        outProposals.push_back(std::move(draft));
    }

    return true;
}

bool WouldExceedResponseCap(std::size_t currentBytes, std::size_t addBytes) {
    // Saturating add — if currentBytes + addBytes overflows, treat as "over the
    // cap" so an adversarial stream that wraps the size counter can't slip past
    // the gate. Pillar 3 defense — see AGENTS.md § Never crash.
    if (addBytes > kMaxLlmResponseBytes - std::min(currentBytes, kMaxLlmResponseBytes)) {
        return true;
    }
    return currentBytes + addBytes > kMaxLlmResponseBytes;
}

bool ShouldAdvancePollCursor(std::size_t failedCount) {
    return failedCount == 0;
}

} // namespace AgenticInferenceClientPure

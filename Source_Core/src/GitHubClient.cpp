#include "GitHubClient.h"

#include "AiErrorRedact.h"
#include "BackendAuditTrail.h"
#include "GitHubClientHelpers.h"
#include "Logger.h"

#include <cpr/cpr.h>

#include <sstream>
#include <utility>

namespace {

constexpr int kGitHubConnectTimeoutMs = 5000;
constexpr int kGitHubOverallTimeoutMs = 15000;

// Per agentic-flow-implementation.md § Decisions locked #2 — bearer-auth header
// is constructed inline INSIDE each write method, not promoted to a shared
// helper. The audit-trail source string is shared so audit consumers can filter.
constexpr const char* kGitHubAuditSource = "github_client";

std::string StripTrailingSlash(std::string s) {
    while (!s.empty() && s.back() == '/') {
        s.pop_back();
    }
    return s;
}

// PAT must never reach user-visible surfaces verbatim. The redactor is intentionally
// shared with the AI provider error path (defense-in-depth — provider 4xx bodies have
// been observed echoing the Authorization header).
std::string RedactForLog(const std::string& body) { return smatchet::ai::pure::RedactProviderErrorBody(body); }

// Build the standard GitHub header set with an inline bearer auth token. The
// `Authorization: Bearer <pat>` line is constructed at the call site and never
// logged verbatim — the redactor strips it from any error body before logging.
cpr::Header MakeGitHubAuthHeaders(const std::string& pat) {
    return cpr::Header{
        {"Accept", "application/vnd.github+json"},
        {"X-GitHub-Api-Version", "2022-11-28"},
        {"User-Agent", "Smatchet/agentic-flow"},
        {"Authorization", std::string("Bearer ") + pat},
    };
}

// (phase-7) Canonical GraphQL audit action for the resolve mutation. Lives as
// a named constant so audit consumers can filter on the literal.
constexpr const char* kAuditResolveReviewThread = "ResolveReviewThread";

// Single-line failure string assembled from the response status + GitHub's
// structured `{"message": "..."}` (if present) + the redacted body. Keeps the
// PAT out of every surface — the Authorization header substring would otherwise
// echo back through nginx-style upstream 401 pages.
std::string ComposeHttpErrorString(const std::string& verb, const std::string& urlSuffix, long statusCode,
                                   const std::string& cprErrorMessage, const std::string& responseBody) {
    std::ostringstream oss;
    oss << "GitHub " << verb << ' ' << urlSuffix << ": HTTP " << statusCode;
    if (!cprErrorMessage.empty()) {
        oss << " (" << cprErrorMessage << ")";
    }
    std::string structured;
    if (GitHubClientHelpers::ExtractGitHubErrorMessage(responseBody, structured)) {
        oss << ": " << structured;
    } else if (!responseBody.empty()) {
        const std::string redacted = RedactForLog(responseBody);
        oss << ": " << redacted;
    }
    return oss.str();
}

} // namespace

GitHubClient::GitHubClient(std::string baseUrl, std::string personalAccessToken)
    : pat_(std::move(personalAccessToken)) {
    // Empty input → canonical `https://api.github.com`. Non-empty input is
    // validated against `IsValidGitHubBaseUrl`; on failure we fall back to the
    // default + LOG_WARN so a typo in a future GitHub Enterprise config does
    // not silently rewrite outbound URLs to `javascript:` / `file:` / `data:`
    // etc. The validator currently accepts only the `https://` scheme.
    const std::string defaultUrl = "https://api.github.com";
    if (baseUrl.empty()) {
        baseUrl_ = defaultUrl;
    } else {
        std::string validationErr;
        if (GitHubClientHelpers::IsValidGitHubBaseUrl(baseUrl, validationErr)) {
            baseUrl_ = StripTrailingSlash(std::move(baseUrl));
        } else {
            LOG_WARN("GitHubClient: rejecting base URL ('%s') — falling back to '%s'", validationErr.c_str(),
                     defaultUrl.c_str());
            baseUrl_ = defaultUrl;
        }
    }
}

GitHubClient::~GitHubClient() = default;

std::string GitHubClient::GetTrackerType() const { return "GitHub"; }

TrackerReachabilityProbeResult GitHubClient::ProbeReachability(const TrackerConfig& /*cfg*/) {
    TrackerReachabilityProbeResult out;
    out.Kind = TrackerReachabilityProbeKind::TransportDown;
    out.Diagnostic = "GitHub ProbeReachability is not supported yet (lands in a later slice).";
    return out;
}

std::vector<CachedTicket> GitHubClient::FetchIssues(bool* outFullSyncCompleted, const TrackerConfig* /*configOverride*/,
                                                    const ViewsStore* /*viewsOverride*/, std::string* outFetchError,
                                                    std::string* /*outWarning*/) {
    if (outFullSyncCompleted) {
        *outFullSyncCompleted = false;
    }
    if (outFetchError) {
        *outFetchError = "FetchIssues is not supported on GitHub backend yet.";
    }
    return {};
}

bool GitHubClient::FetchIssuesForKeys(const TrackerConfig& /*cfg*/, const std::vector<std::string>& /*issueKeys*/,
                                      const ViewsStore& /*views*/, std::vector<CachedTicket>& outTickets,
                                      std::string& outError) {
    outTickets.clear();
    outError = "FetchIssuesForKeys is not supported on GitHub backend yet.";
    return false;
}

bool GitHubClient::UpdateIssueFields(const std::string& /*issueId*/, const nlohmann::json& /*fields*/,
                                     std::string& outError) {
    outError = "UpdateIssueFields is not supported on GitHub backend yet.";
    return false;
}

bool GitHubClient::UpdateField(const std::string& /*issueId*/, const TrackerField& /*field*/,
                               const std::vector<std::string>& /*values*/, std::string& outError) {
    outError = "UpdateField is not supported on GitHub backend yet.";
    return false;
}

bool GitHubClient::BuildFieldPayload(const TrackerField& /*field*/, const std::vector<std::string>& /*values*/,
                                     nlohmann::json& /*outPayload*/, std::string& outError) {
    outError = "BuildFieldPayload is not supported on GitHub backend yet.";
    return false;
}

std::string GitHubClient::ResolveDisplayValue(const std::string& /*fieldId*/, const TrackerField* /*field*/,
                                              const std::string& value) const {
    // No field catalog yet — identity is the safest displayable value pre-catalog.
    return value;
}

bool GitHubClient::FetchIssueComments(const std::string& issueKey, std::vector<TrackerIssueComment>& outComments,
                                      std::string& outError) {
    outComments.clear();
    outError.clear();

    if (pat_.empty()) {
        outError = "GitHub PAT not configured (set cfg.GitHubPat).";
        return false;
    }

    GitHubClientHelpers::ParsedIssueKey parsed;
    std::string parseErr;
    if (!GitHubClientHelpers::ParseGitHubIssueKey(issueKey, parsed, parseErr)) {
        outError = parseErr;
        return false;
    }

    std::ostringstream numOss;
    numOss << parsed.Number;
    const std::string url =
        baseUrl_ + "/repos/" + parsed.Owner + "/" + parsed.Repo + "/issues/" + numOss.str() + "/comments";

    cpr::Header headers{
        {"Accept", "application/vnd.github+json"},
        {"X-GitHub-Api-Version", "2022-11-28"},
        {"User-Agent", "Smatchet/agentic-flow"},
        {"Authorization", std::string("Bearer ") + pat_},
    };
    cpr::Response resp = cpr::Get(cpr::Url{url}, headers, cpr::ConnectTimeout{kGitHubConnectTimeoutMs},
                                  cpr::Timeout{kGitHubOverallTimeoutMs});

    if (resp.status_code != 200) {
        std::ostringstream oss;
        oss << "GitHub /comments HTTP " << resp.status_code;
        if (!resp.error.message.empty()) {
            oss << " (" << resp.error.message << ")";
        }
        // Body redaction is defense-in-depth: GitHub 401/403 bodies have been observed
        // echoing the Authorization header in nginx-style upstream error pages.
        if (!resp.text.empty()) {
            const std::string redacted = RedactForLog(resp.text);
            LOG_WARN("GitHubClient::FetchIssueComments failed: %s — body=%s", oss.str().c_str(), redacted.c_str());
        } else {
            LOG_WARN("GitHubClient::FetchIssueComments failed: %s", oss.str().c_str());
        }
        outError = oss.str();
        return false;
    }

    nlohmann::json arr;
    try {
        arr = nlohmann::json::parse(resp.text);
    } catch (const nlohmann::json::parse_error& e) {
        outError = std::string("GitHub /comments response is not valid JSON: ") + e.what();
        return false;
    }
    if (!arr.is_array()) {
        outError = "GitHub /comments response is not a JSON array.";
        return false;
    }

    outComments.reserve(arr.size());
    bool isoParseWarned = false;
    for (const auto& item : arr) {
        if (!item.is_object()) {
            continue;
        }
        TrackerIssueComment c;
        if (item.contains("id") && item["id"].is_number_integer()) {
            std::ostringstream idOss;
            idOss << item["id"].get<std::int64_t>();
            c.Id = idOss.str();
        }
        if (item.contains("user") && item["user"].is_object()) {
            const auto& user = item["user"];
            if (user.contains("login") && user["login"].is_string()) {
                c.Author = user["login"].get<std::string>();
            }
        }
        if (item.contains("body") && item["body"].is_string()) {
            c.Body = item["body"].get<std::string>();
        }
        if (item.contains("created_at") && item["created_at"].is_string()) {
            std::int64_t ts = 0;
            std::string isoErr;
            if (GitHubClientHelpers::ParseIso8601ToUnixSec(item["created_at"].get<std::string>(), ts, isoErr)) {
                c.CreatedAtSec = ts;
            } else if (!isoParseWarned) {
                LOG_WARN("GitHubClient::FetchIssueComments: created_at parse failed: %s", isoErr.c_str());
                isoParseWarned = true;
            }
        }
        if (item.contains("updated_at") && item["updated_at"].is_string()) {
            std::int64_t ts = 0;
            std::string isoErr;
            if (GitHubClientHelpers::ParseIso8601ToUnixSec(item["updated_at"].get<std::string>(), ts, isoErr)) {
                c.UpdatedAtSec = ts;
            } else {
                c.UpdatedAtSec = c.CreatedAtSec;
            }
        } else {
            c.UpdatedAtSec = c.CreatedAtSec;
        }
        outComments.push_back(std::move(c));
    }
    return true;
}

// ─── Write methods ──────────────────────────────────────────────────────────
//
// Common shape across the five methods, factored only by code clarity (not
// behavioural variation):
//   - PAT presence check        →  "GitHub PAT not configured (set cfg.GitHubPat)."
//   - Issue-key parse           →  parser's outError verbatim
//   - Action-specific validation (state transition target)
//   - BackendAuditTrail::AppendBegin
//   - cpr call with inline bearer header
//   - Status check + redacted error compose on failure
//   - BackendAuditTrail::AppendResult (both success and failure)
//
// Audit-trail entries carry the issue key as the `IssueKey` field. The action
// name maps 1:1 to the AgentProposal.proposedAction enum value so triage-side
// consumers can join proposals to applied audit events.
//
// The functions deliberately do not parse response bodies for success — the
// agentic-flow contract only needs success/fail + audit. If a future caller
// needs the resulting comment/label id, extend the audit entry's `Data` field
// rather than thread a new out-parameter.

bool GitHubClient::CommentAdd(const std::string& issueKey, const std::string& body, std::string& outError) {
    outError.clear();
    const std::string auditAction = "CommentAdd";
    const std::string auditOp = BackendAuditTrail::MakeOperationId("github-comment-add");
    // Body content is sensitive — audit-trail redactor handles `body` keys.
    nlohmann::json beginData = nlohmann::json::object();
    beginData["body"] = body;
    BackendAuditTrail::AppendBegin(auditAction, kGitHubAuditSource, issueKey, auditOp, beginData);

    if (pat_.empty()) {
        outError = "GitHub PAT not configured (set cfg.GitHubPat).";
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, issueKey, auditOp, false, outError);
        return false;
    }

    GitHubClientHelpers::ParsedIssueKey parsed;
    std::string parseErr;
    if (!GitHubClientHelpers::ParseGitHubIssueKey(issueKey, parsed, parseErr)) {
        outError = parseErr;
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, issueKey, auditOp, false, outError);
        return false;
    }

    const std::string suffix = GitHubClientHelpers::BuildIssueCommentsSuffix(parsed);
    const std::string url = baseUrl_ + suffix;
    const nlohmann::json payload = GitHubClientHelpers::BuildCommentAddBody(body);
    const std::string payloadDump = payload.dump();
    if (GitHubClientHelpers::ShouldRejectAsTooLarge(payloadDump.size())) {
        outError = "GitHub CommentAdd: body exceeds outbound size cap.";
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, issueKey, auditOp, false, outError);
        return false;
    }

    cpr::Response r = cpr::Post(cpr::Url{url}, MakeGitHubAuthHeaders(pat_), cpr::Body{payloadDump},
                                cpr::Header{{"Content-Type", "application/json"}},
                                cpr::ConnectTimeout{kGitHubConnectTimeoutMs}, cpr::Timeout{kGitHubOverallTimeoutMs});

    if (r.status_code < 200 || r.status_code >= 300) {
        outError = ComposeHttpErrorString("POST", suffix, r.status_code, r.error.message, r.text);
        LOG_WARN("GitHubClient::CommentAdd failed: %s", outError.c_str());
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, issueKey, auditOp, false, outError);
        return false;
    }
    BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, issueKey, auditOp, true, "");
    return true;
}

bool GitHubClient::LabelAdd(const std::string& issueKey, const std::string& label, std::string& outError) {
    outError.clear();
    const std::string auditAction = "LabelAdd";
    const std::string auditOp = BackendAuditTrail::MakeOperationId("github-label-add");
    nlohmann::json beginData = nlohmann::json::object();
    beginData["label"] = label;
    BackendAuditTrail::AppendBegin(auditAction, kGitHubAuditSource, issueKey, auditOp, beginData);

    if (pat_.empty()) {
        outError = "GitHub PAT not configured (set cfg.GitHubPat).";
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, issueKey, auditOp, false, outError);
        return false;
    }
    if (label.empty()) {
        outError = "Label name is empty.";
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, issueKey, auditOp, false, outError);
        return false;
    }

    GitHubClientHelpers::ParsedIssueKey parsed;
    std::string parseErr;
    if (!GitHubClientHelpers::ParseGitHubIssueKey(issueKey, parsed, parseErr)) {
        outError = parseErr;
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, issueKey, auditOp, false, outError);
        return false;
    }

    const std::string suffix = GitHubClientHelpers::BuildIssueLabelsSuffix(parsed);
    const std::string url = baseUrl_ + suffix;
    const nlohmann::json payload = GitHubClientHelpers::BuildLabelAddBody(label);
    const std::string payloadDump = payload.dump();
    if (GitHubClientHelpers::ShouldRejectAsTooLarge(payloadDump.size())) {
        outError = "GitHub LabelAdd: body exceeds outbound size cap.";
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, issueKey, auditOp, false, outError);
        return false;
    }

    cpr::Response r = cpr::Post(cpr::Url{url}, MakeGitHubAuthHeaders(pat_), cpr::Body{payloadDump},
                                cpr::Header{{"Content-Type", "application/json"}},
                                cpr::ConnectTimeout{kGitHubConnectTimeoutMs}, cpr::Timeout{kGitHubOverallTimeoutMs});

    if (r.status_code < 200 || r.status_code >= 300) {
        outError = ComposeHttpErrorString("POST", suffix, r.status_code, r.error.message, r.text);
        LOG_WARN("GitHubClient::LabelAdd failed: %s", outError.c_str());
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, issueKey, auditOp, false, outError);
        return false;
    }
    BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, issueKey, auditOp, true, "");
    return true;
}

bool GitHubClient::LabelRemove(const std::string& issueKey, const std::string& label, std::string& outError) {
    outError.clear();
    const std::string auditAction = "LabelRemove";
    const std::string auditOp = BackendAuditTrail::MakeOperationId("github-label-remove");
    nlohmann::json beginData = nlohmann::json::object();
    beginData["label"] = label;
    BackendAuditTrail::AppendBegin(auditAction, kGitHubAuditSource, issueKey, auditOp, beginData);

    if (pat_.empty()) {
        outError = "GitHub PAT not configured (set cfg.GitHubPat).";
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, issueKey, auditOp, false, outError);
        return false;
    }
    if (label.empty()) {
        outError = "Label name is empty.";
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, issueKey, auditOp, false, outError);
        return false;
    }

    GitHubClientHelpers::ParsedIssueKey parsed;
    std::string parseErr;
    if (!GitHubClientHelpers::ParseGitHubIssueKey(issueKey, parsed, parseErr)) {
        outError = parseErr;
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, issueKey, auditOp, false, outError);
        return false;
    }

    const std::string suffix = GitHubClientHelpers::BuildIssueLabelRemoveSuffix(parsed, label);
    const std::string url = baseUrl_ + suffix;

    cpr::Response r = cpr::Delete(cpr::Url{url}, MakeGitHubAuthHeaders(pat_),
                                  cpr::ConnectTimeout{kGitHubConnectTimeoutMs}, cpr::Timeout{kGitHubOverallTimeoutMs});

    if (r.status_code < 200 || r.status_code >= 300) {
        outError = ComposeHttpErrorString("DELETE", suffix, r.status_code, r.error.message, r.text);
        LOG_WARN("GitHubClient::LabelRemove failed: %s", outError.c_str());
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, issueKey, auditOp, false, outError);
        return false;
    }
    BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, issueKey, auditOp, true, "");
    return true;
}

bool GitHubClient::AssigneeSet(const std::string& issueKey, const std::string& user, std::string& outError) {
    outError.clear();
    const std::string auditAction = "AssigneeSet";
    const std::string auditOp = BackendAuditTrail::MakeOperationId("github-assignee-set");
    nlohmann::json beginData = nlohmann::json::object();
    beginData["user"] = user;
    BackendAuditTrail::AppendBegin(auditAction, kGitHubAuditSource, issueKey, auditOp, beginData);

    if (pat_.empty()) {
        outError = "GitHub PAT not configured (set cfg.GitHubPat).";
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, issueKey, auditOp, false, outError);
        return false;
    }
    if (user.empty()) {
        outError = "Assignee user is empty.";
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, issueKey, auditOp, false, outError);
        return false;
    }

    GitHubClientHelpers::ParsedIssueKey parsed;
    std::string parseErr;
    if (!GitHubClientHelpers::ParseGitHubIssueKey(issueKey, parsed, parseErr)) {
        outError = parseErr;
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, issueKey, auditOp, false, outError);
        return false;
    }

    const std::string suffix = GitHubClientHelpers::BuildIssueAssigneesSuffix(parsed);
    const std::string url = baseUrl_ + suffix;
    const nlohmann::json payload = GitHubClientHelpers::BuildAssigneeSetBody(user);
    const std::string payloadDump = payload.dump();
    if (GitHubClientHelpers::ShouldRejectAsTooLarge(payloadDump.size())) {
        outError = "GitHub AssigneeSet: body exceeds outbound size cap.";
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, issueKey, auditOp, false, outError);
        return false;
    }

    cpr::Response r = cpr::Post(cpr::Url{url}, MakeGitHubAuthHeaders(pat_), cpr::Body{payloadDump},
                                cpr::Header{{"Content-Type", "application/json"}},
                                cpr::ConnectTimeout{kGitHubConnectTimeoutMs}, cpr::Timeout{kGitHubOverallTimeoutMs});

    if (r.status_code < 200 || r.status_code >= 300) {
        outError = ComposeHttpErrorString("POST", suffix, r.status_code, r.error.message, r.text);
        LOG_WARN("GitHubClient::AssigneeSet failed: %s", outError.c_str());
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, issueKey, auditOp, false, outError);
        return false;
    }
    BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, issueKey, auditOp, true, "");
    return true;
}

// ─── Read endpoints (agentic triage seeds) ──────────────────────────────────
//
// Both methods follow the same shape as FetchIssueComments above: PAT presence
// check, issue-key parse (where applicable), inline bearer-auth header,
// status-code check + redacted error compose, JSON parse. Reads are NOT
// audit-trail entries — that surface is reserved for write methods per the
// existing Jira / Plane read-path precedent.

bool GitHubClient::FetchIssueBody(const std::string& issueKey, std::string& outBody, std::string& outError) {
    outBody.clear();
    outError.clear();

    if (pat_.empty()) {
        outError = "GitHub PAT not configured (set cfg.GitHubPat).";
        return false;
    }

    GitHubClientHelpers::ParsedIssueKey parsed;
    std::string parseErr;
    if (!GitHubClientHelpers::ParseGitHubIssueKey(issueKey, parsed, parseErr)) {
        outError = parseErr;
        return false;
    }

    const std::string suffix = GitHubClientHelpers::BuildIssueRootSuffix(parsed);
    const std::string url = baseUrl_ + suffix;

    cpr::Response r = cpr::Get(cpr::Url{url}, MakeGitHubAuthHeaders(pat_), cpr::ConnectTimeout{kGitHubConnectTimeoutMs},
                               cpr::Timeout{kGitHubOverallTimeoutMs});

    if (r.status_code != 200) {
        outError = ComposeHttpErrorString("GET", suffix, r.status_code, r.error.message, r.text);
        LOG_WARN("GitHubClient::FetchIssueBody failed: %s", outError.c_str());
        return false;
    }

    nlohmann::json obj;
    try {
        obj = nlohmann::json::parse(r.text);
    } catch (const nlohmann::json::parse_error& e) {
        outError = std::string("GitHub /issues/{n} response is not valid JSON: ") + e.what();
        return false;
    }
    if (!obj.is_object()) {
        outError = "GitHub /issues/{n} response is not a JSON object.";
        return false;
    }
    // GitHub returns `body: null` for issues created without a description. Treat as empty string.
    if (obj.contains("body") && obj["body"].is_string()) {
        outBody = obj["body"].get<std::string>();
    }
    return true;
}

bool GitHubClient::FetchIssueTitle(const std::string& issueKey, std::string& outTitle, std::string& outError) {
    // Mirrors FetchIssueBody structure — separate request to keep the read
    // surface uniform per (issueKey -> field) call. Could be folded into a
    // single FetchIssueTitleAndBody if perf becomes a concern; for the
    // current scheduled-poll cadence (>=60s) the extra round-trip per issue
    // is negligible compared to the LLM call that follows.
    outTitle.clear();
    outError.clear();

    if (pat_.empty()) {
        outError = "GitHub PAT not configured (set cfg.GitHubPat).";
        return false;
    }

    GitHubClientHelpers::ParsedIssueKey parsed;
    std::string parseErr;
    if (!GitHubClientHelpers::ParseGitHubIssueKey(issueKey, parsed, parseErr)) {
        outError = parseErr;
        return false;
    }

    const std::string suffix = GitHubClientHelpers::BuildIssueRootSuffix(parsed);
    const std::string url = baseUrl_ + suffix;

    cpr::Response r = cpr::Get(cpr::Url{url}, MakeGitHubAuthHeaders(pat_), cpr::ConnectTimeout{kGitHubConnectTimeoutMs},
                               cpr::Timeout{kGitHubOverallTimeoutMs});
    if (r.status_code != 200) {
        outError = ComposeHttpErrorString("GET", suffix, r.status_code, r.error.message, r.text);
        LOG_WARN("GitHubClient::FetchIssueTitle failed: %s", outError.c_str());
        return false;
    }

    nlohmann::json obj;
    try {
        obj = nlohmann::json::parse(r.text);
    } catch (const nlohmann::json::parse_error& e) {
        outError = std::string("GitHub /issues/{n} response is not valid JSON: ") + e.what();
        return false;
    }
    if (!obj.is_object()) {
        outError = "GitHub /issues/{n} response is not a JSON object.";
        return false;
    }
    if (obj.contains("title") && obj["title"].is_string()) {
        outTitle = obj["title"].get<std::string>();
    }
    return true;
}

bool GitHubClient::ListOpenIssuesForRepo(const std::string& owner, const std::string& repo,
                                         std::vector<std::string>& outKeys, std::string& outError,
                                         std::int64_t sinceUnixSec) {
    outKeys.clear();
    outError.clear();

    if (pat_.empty()) {
        outError = "GitHub PAT not configured (set cfg.GitHubPat).";
        return false;
    }
    if (owner.empty() || repo.empty()) {
        outError = "Owner and repo are required.";
        return false;
    }

    // per_page bound (30) keeps the downstream LLM inference cost predictable. When
    // `sinceUnixSec > 0` the scheduled-poll worker is paging by `updated_at` (GitHub's
    // since= filter on /issues), so only issues that changed since the previous poll
    // come back — the same 30-row cap then doubles as a per-poll work cap.
    std::string suffix = "/repos/" + owner + "/" + repo + "/issues?state=open&per_page=30";
    if (sinceUnixSec > 0) {
        suffix += "&since=";
        suffix += GitHubClientHelpers::FormatUnixSecAsIso8601(sinceUnixSec);
    }
    const std::string url = baseUrl_ + suffix;

    cpr::Response r = cpr::Get(cpr::Url{url}, MakeGitHubAuthHeaders(pat_), cpr::ConnectTimeout{kGitHubConnectTimeoutMs},
                               cpr::Timeout{kGitHubOverallTimeoutMs});

    if (r.status_code != 200) {
        outError = ComposeHttpErrorString("GET", suffix, r.status_code, r.error.message, r.text);
        LOG_WARN("GitHubClient::ListOpenIssuesForRepo failed: %s", outError.c_str());
        return false;
    }

    nlohmann::json arr;
    try {
        arr = nlohmann::json::parse(r.text);
    } catch (const nlohmann::json::parse_error& e) {
        outError = std::string("GitHub /issues response is not valid JSON: ") + e.what();
        return false;
    }
    if (!arr.is_array()) {
        outError = "GitHub /issues response is not a JSON array.";
        return false;
    }

    outKeys.reserve(arr.size());
    // GitHub's /issues endpoint returns BOTH issues and PRs; PR rows carry a
    // `pull_request` object. Skip those — the agentic triage flow is issue-only.
    for (const auto& item : arr) {
        if (!item.is_object())
            continue;
        if (item.contains("pull_request"))
            continue;
        if (!item.contains("number") || !item["number"].is_number_integer())
            continue;
        const std::int64_t number = item["number"].get<std::int64_t>();
        if (number <= 0)
            continue;
        outKeys.push_back(GitHubClientHelpers::FormatGitHubIssueKey(owner, repo, number));
    }
    return true;
}

bool GitHubClient::FetchRawFile(const std::string& owner, const std::string& repo, const std::string& path,
                                std::string& outContent, std::string& outError) {
    outContent.clear();
    outError.clear();

    if (owner.empty() || repo.empty() || path.empty()) {
        outError = "owner, repo, and path are required.";
        return false;
    }
    // PAT may be empty for public repos — the raw endpoint is anonymously
    // accessible on public-repo files. Keep going; auth header is attached
    // only when the PAT is set so private repos work too.

    // raw.githubusercontent.com is a separate host from api.github.com; it
    // does not vary with the configured baseUrl (GitHub Enterprise raw goes
    // to `<host>/raw/<owner>/<repo>/HEAD/<path>` — out of scope for this
    // slice; the agentic flow targets public github.com today).
    const std::string url = "https://raw.githubusercontent.com/" + owner + "/" + repo + "/HEAD/" + path;

    cpr::Header headers{
        {"Accept", "text/plain, */*"},
        {"User-Agent", "Smatchet/agentic-flow"},
    };
    if (!pat_.empty()) {
        headers["Authorization"] = std::string("Bearer ") + pat_;
    }

    cpr::Response r = cpr::Get(cpr::Url{url}, headers, cpr::ConnectTimeout{kGitHubConnectTimeoutMs},
                               cpr::Timeout{kGitHubOverallTimeoutMs});

    if (r.status_code == 404) {
        outError = "not found";
        LOG_DEBUG("GitHubClient::FetchRawFile: %s/%s/HEAD/%s -> 404 not found", owner.c_str(), repo.c_str(),
                  path.c_str());
        return false;
    }
    if (r.status_code < 200 || r.status_code >= 300) {
        const std::string suffix = std::string("/") + owner + "/" + repo + "/HEAD/" + path;
        outError = ComposeHttpErrorString("GET", suffix, r.status_code, r.error.message, r.text);
        LOG_WARN("GitHubClient::FetchRawFile failed: %s", outError.c_str());
        return false;
    }

    outContent = r.text;
    LOG_DEBUG("GitHubClient::FetchRawFile fetched %zu bytes from %s/%s/HEAD/%s", outContent.size(), owner.c_str(),
              repo.c_str(), path.c_str());
    return true;
}

// ─── H7 PR-thread + workflow methods ────────────────────────────────────────
//
// FetchPrComments shares the issues-comments endpoint shape so reuses the
// existing FetchIssueComments helper rather than re-fetching the same JSON
// parser. The five remaining methods follow the same shape as the H6 writes:
// PAT check → URL build → inline bearer header → status check + redacted
// error compose → optional response parse + audit-trail entry.

bool GitHubClient::FetchPrComments(const std::string& prKey, std::vector<TrackerIssueComment>& outComments,
                                   std::string& outError) {
    // GitHub treats PRs as a superset of issues — the issue-comments endpoint
    // returns the PR's conversation thread. The /pulls/{n}/comments variant
    // returns only diff-review comments, which is NOT what the watcher wants.
    // Delegating to FetchIssueComments keeps wire-parsing logic in one place.
    return FetchIssueComments(prKey, outComments, outError);
}

bool GitHubClient::CreatePullRequest(const CreatePullRequestRequest& req, std::string& outPrUrl,
                                     std::string& outError) {
    outPrUrl.clear();
    outError.clear();
    const std::string auditAction = "CreatePullRequest";
    const std::string auditOp = BackendAuditTrail::MakeOperationId("github-create-pr");
    // PR-context audit-key is `owner/repo` (no issue number — the PR doesn't
    // exist yet). Distinguishable from issue-keyed audit rows by absence of `#`.
    const std::string ctxKey = req.owner + "/" + req.repo;
    nlohmann::json beginData = nlohmann::json::object();
    beginData["head"] = req.headBranch;
    beginData["base"] = req.baseBranch;
    beginData["draft"] = req.draft;
    BackendAuditTrail::AppendBegin(auditAction, kGitHubAuditSource, ctxKey, auditOp, beginData);

    if (pat_.empty()) {
        outError = "GitHub PAT not configured (set cfg.GitHubPat).";
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, ctxKey, auditOp, false, outError);
        return false;
    }
    if (req.owner.empty() || req.repo.empty() || req.headBranch.empty() || req.baseBranch.empty()) {
        outError = "CreatePullRequest: owner/repo/headBranch/baseBranch are required.";
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, ctxKey, auditOp, false, outError);
        return false;
    }
    if (req.title.empty()) {
        outError = "CreatePullRequest: title is required.";
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, ctxKey, auditOp, false, outError);
        return false;
    }

    const std::string suffix = GitHubClientHelpers::BuildPullsCollectionSuffix(req.owner, req.repo);
    const std::string url = baseUrl_ + suffix;
    const nlohmann::json payload =
        GitHubClientHelpers::BuildCreatePullRequestBody(req.title, req.headBranch, req.baseBranch, req.body, req.draft);
    const std::string payloadDump = payload.dump();
    if (GitHubClientHelpers::ShouldRejectAsTooLarge(payloadDump.size())) {
        outError = "GitHub CreatePullRequest: body exceeds outbound size cap.";
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, ctxKey, auditOp, false, outError);
        return false;
    }

    cpr::Response r = cpr::Post(cpr::Url{url}, MakeGitHubAuthHeaders(pat_), cpr::Body{payloadDump},
                                cpr::Header{{"Content-Type", "application/json"}},
                                cpr::ConnectTimeout{kGitHubConnectTimeoutMs}, cpr::Timeout{kGitHubOverallTimeoutMs});

    if (r.status_code < 200 || r.status_code >= 300) {
        outError = ComposeHttpErrorString("POST", suffix, r.status_code, r.error.message, r.text);
        LOG_WARN("GitHubClient::CreatePullRequest failed: %s", outError.c_str());
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, ctxKey, auditOp, false, outError);
        return false;
    }

    std::string urlErr;
    if (!GitHubClientHelpers::ExtractCreatePullRequestHtmlUrl(r.text, outPrUrl, urlErr)) {
        outError = std::string("CreatePullRequest succeeded but response is malformed: ") + urlErr;
        LOG_WARN("GitHubClient::CreatePullRequest: %s", outError.c_str());
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, ctxKey, auditOp, false, outError);
        return false;
    }
    nlohmann::json resultData = nlohmann::json::object();
    resultData["prUrl"] = outPrUrl;
    BackendAuditTrail::AuditEvent ev;
    ev.Action = auditAction;
    ev.Source = kGitHubAuditSource;
    ev.IssueKey = ctxKey;
    ev.OperationId = auditOp;
    ev.Success = true;
    ev.Data = resultData;
    ev.Phase = "result";
    BackendAuditTrail::AppendEvent(ev);
    return true;
}

bool GitHubClient::FetchCheckRuns(const std::string& owner, const std::string& repo, const std::string& headSha,
                                  std::vector<CheckRun>& outRuns, std::string& outError) {
    outRuns.clear();
    outError.clear();

    if (pat_.empty()) {
        outError = "GitHub PAT not configured (set cfg.GitHubPat).";
        return false;
    }
    if (owner.empty() || repo.empty() || headSha.empty()) {
        outError = "owner/repo/headSha are required.";
        return false;
    }

    const std::string suffix = GitHubClientHelpers::BuildCheckRunsForCommitSuffix(owner, repo, headSha);
    const std::string url = baseUrl_ + suffix;

    cpr::Response r = cpr::Get(cpr::Url{url}, MakeGitHubAuthHeaders(pat_), cpr::ConnectTimeout{kGitHubConnectTimeoutMs},
                               cpr::Timeout{kGitHubOverallTimeoutMs});

    if (r.status_code != 200) {
        outError = ComposeHttpErrorString("GET", suffix, r.status_code, r.error.message, r.text);
        LOG_WARN("GitHubClient::FetchCheckRuns failed: %s", outError.c_str());
        return false;
    }

    nlohmann::json obj;
    try {
        obj = nlohmann::json::parse(r.text);
    } catch (const nlohmann::json::parse_error& e) {
        outError = std::string("GitHub /check-runs response is not valid JSON: ") + e.what();
        return false;
    }
    if (!obj.is_object() || !obj.contains("check_runs") || !obj["check_runs"].is_array()) {
        outError = "GitHub /check-runs response missing check_runs array.";
        return false;
    }
    const auto& arr = obj["check_runs"];
    outRuns.reserve(arr.size());
    for (const auto& item : arr) {
        if (!item.is_object()) {
            continue;
        }
        CheckRun run;
        if (item.contains("id") && item["id"].is_number_integer()) {
            run.id = item["id"].get<std::int64_t>();
        }
        if (item.contains("name") && item["name"].is_string()) {
            run.name = item["name"].get<std::string>();
        }
        if (item.contains("status") && item["status"].is_string()) {
            run.status = item["status"].get<std::string>();
        }
        // GitHub returns `conclusion: null` while a run is still in progress.
        if (item.contains("conclusion") && item["conclusion"].is_string()) {
            run.conclusion = item["conclusion"].get<std::string>();
        }
        if (item.contains("details_url") && item["details_url"].is_string()) {
            run.detailsUrl = item["details_url"].get<std::string>();
        }
        if (item.contains("started_at") && item["started_at"].is_string()) {
            std::int64_t ts = 0;
            std::string isoErr;
            if (GitHubClientHelpers::ParseIso8601ToUnixSec(item["started_at"].get<std::string>(), ts, isoErr)) {
                run.startedAtSec = ts;
            }
        }
        if (item.contains("completed_at") && item["completed_at"].is_string()) {
            std::int64_t ts = 0;
            std::string isoErr;
            if (GitHubClientHelpers::ParseIso8601ToUnixSec(item["completed_at"].get<std::string>(), ts, isoErr)) {
                run.completedAtSec = ts;
            }
        }
        outRuns.push_back(std::move(run));
    }
    return true;
}

bool GitHubClient::FetchCheckRunAnnotations(const std::string& owner, const std::string& repo, std::int64_t checkRunId,
                                            std::vector<CheckRunAnnotation>& outAnnotations, std::string& outError) {
    outAnnotations.clear();
    outError.clear();

    if (pat_.empty()) {
        outError = "GitHub PAT not configured (set cfg.GitHubPat).";
        return false;
    }
    if (owner.empty() || repo.empty() || checkRunId <= 0) {
        outError = "owner/repo/checkRunId are required.";
        return false;
    }

    const std::string suffix = GitHubClientHelpers::BuildCheckRunAnnotationsSuffix(owner, repo, checkRunId);
    const std::string url = baseUrl_ + suffix;

    cpr::Response r = cpr::Get(cpr::Url{url}, MakeGitHubAuthHeaders(pat_), cpr::ConnectTimeout{kGitHubConnectTimeoutMs},
                               cpr::Timeout{kGitHubOverallTimeoutMs});

    if (r.status_code != 200) {
        outError = ComposeHttpErrorString("GET", suffix, r.status_code, r.error.message, r.text);
        LOG_WARN("GitHubClient::FetchCheckRunAnnotations failed: %s", outError.c_str());
        return false;
    }

    nlohmann::json arr;
    try {
        arr = nlohmann::json::parse(r.text);
    } catch (const nlohmann::json::parse_error& e) {
        outError = std::string("GitHub /annotations response is not valid JSON: ") + e.what();
        return false;
    }
    if (!arr.is_array()) {
        outError = "GitHub /annotations response is not a JSON array.";
        return false;
    }
    outAnnotations.reserve(arr.size());
    for (const auto& item : arr) {
        if (!item.is_object()) {
            continue;
        }
        CheckRunAnnotation a;
        if (item.contains("path") && item["path"].is_string()) {
            a.path = item["path"].get<std::string>();
        }
        if (item.contains("start_line") && item["start_line"].is_number_integer()) {
            a.startLine = item["start_line"].get<int>();
        }
        if (item.contains("end_line") && item["end_line"].is_number_integer()) {
            a.endLine = item["end_line"].get<int>();
        }
        if (item.contains("annotation_level") && item["annotation_level"].is_string()) {
            a.annotationLevel = item["annotation_level"].get<std::string>();
        }
        if (item.contains("message") && item["message"].is_string()) {
            a.message = item["message"].get<std::string>();
        }
        if (item.contains("title") && item["title"].is_string()) {
            a.title = item["title"].get<std::string>();
        }
        outAnnotations.push_back(std::move(a));
    }
    return true;
}

bool GitHubClient::FetchActionsJobLogs(const std::string& owner, const std::string& repo, std::int64_t jobId,
                                       int tailLines, std::string& outLogTail, std::string& outError) {
    outLogTail.clear();
    outError.clear();

    if (pat_.empty()) {
        outError = "GitHub PAT not configured (set cfg.GitHubPat).";
        return false;
    }
    if (owner.empty() || repo.empty() || jobId <= 0) {
        outError = "owner/repo/jobId are required.";
        return false;
    }

    const std::string suffix = GitHubClientHelpers::BuildActionsJobLogsSuffix(owner, repo, jobId);
    const std::string url = baseUrl_ + suffix;

    // cpr follows redirects by default; GitHub returns 302 to a presigned S3
    // URL that serves the raw log text as text/plain. The follow lands a 200
    // with the raw body.
    cpr::Response r = cpr::Get(cpr::Url{url}, MakeGitHubAuthHeaders(pat_), cpr::ConnectTimeout{kGitHubConnectTimeoutMs},
                               cpr::Timeout{kGitHubOverallTimeoutMs});

    if (r.status_code != 200) {
        outError = ComposeHttpErrorString("GET", suffix, r.status_code, r.error.message, r.text);
        LOG_WARN("GitHubClient::FetchActionsJobLogs failed: %s", outError.c_str());
        return false;
    }
    outLogTail = GitHubClientHelpers::ClipLogTail(r.text, tailLines);
    return true;
}

bool GitHubClient::RerunWorkflowRun(const std::string& owner, const std::string& repo, std::int64_t runId,
                                    std::string& outError) {
    outError.clear();
    const std::string auditAction = "RerunWorkflowRun";
    const std::string auditOp = BackendAuditTrail::MakeOperationId("github-rerun-workflow");
    const std::string ctxKey = owner + "/" + repo;
    nlohmann::json beginData = nlohmann::json::object();
    beginData["runId"] = runId;
    BackendAuditTrail::AppendBegin(auditAction, kGitHubAuditSource, ctxKey, auditOp, beginData);

    if (pat_.empty()) {
        outError = "GitHub PAT not configured (set cfg.GitHubPat).";
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, ctxKey, auditOp, false, outError);
        return false;
    }
    if (owner.empty() || repo.empty() || runId <= 0) {
        outError = "owner/repo/runId are required.";
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, ctxKey, auditOp, false, outError);
        return false;
    }

    const std::string suffix = GitHubClientHelpers::BuildActionsRunRerunSuffix(owner, repo, runId);
    const std::string url = baseUrl_ + suffix;
    // GitHub accepts an empty `{}` body; an empty cpr::Body{} also works but a
    // valid JSON object keeps Content-Type honest for downstream proxies.
    const std::string payload = "{}";

    cpr::Response r = cpr::Post(cpr::Url{url}, MakeGitHubAuthHeaders(pat_), cpr::Body{payload},
                                cpr::Header{{"Content-Type", "application/json"}},
                                cpr::ConnectTimeout{kGitHubConnectTimeoutMs}, cpr::Timeout{kGitHubOverallTimeoutMs});

    if (r.status_code < 200 || r.status_code >= 300) {
        outError = ComposeHttpErrorString("POST", suffix, r.status_code, r.error.message, r.text);
        LOG_WARN("GitHubClient::RerunWorkflowRun failed: %s", outError.c_str());
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, ctxKey, auditOp, false, outError);
        return false;
    }
    BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, ctxKey, auditOp, true, "");
    return true;
}

bool GitHubClient::StateTransition(const std::string& issueKey, const std::string& state, std::string& outError) {
    outError.clear();
    const std::string auditAction = "StateTransition";
    const std::string auditOp = BackendAuditTrail::MakeOperationId("github-state-transition");
    nlohmann::json beginData = nlohmann::json::object();
    beginData["state"] = state;
    BackendAuditTrail::AppendBegin(auditAction, kGitHubAuditSource, issueKey, auditOp, beginData);

    if (pat_.empty()) {
        outError = "GitHub PAT not configured (set cfg.GitHubPat).";
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, issueKey, auditOp, false, outError);
        return false;
    }
    if (!GitHubClientHelpers::IsValidStateTransitionTarget(state)) {
        outError = "GitHub state must be exactly 'open' or 'closed' (got '" + state + "').";
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, issueKey, auditOp, false, outError);
        return false;
    }

    GitHubClientHelpers::ParsedIssueKey parsed;
    std::string parseErr;
    if (!GitHubClientHelpers::ParseGitHubIssueKey(issueKey, parsed, parseErr)) {
        outError = parseErr;
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, issueKey, auditOp, false, outError);
        return false;
    }

    const std::string suffix = GitHubClientHelpers::BuildIssueRootSuffix(parsed);
    const std::string url = baseUrl_ + suffix;
    const nlohmann::json payload = GitHubClientHelpers::BuildStateTransitionBody(state);
    const std::string payloadDump = payload.dump();
    if (GitHubClientHelpers::ShouldRejectAsTooLarge(payloadDump.size())) {
        outError = "GitHub StateTransition: body exceeds outbound size cap.";
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, issueKey, auditOp, false, outError);
        return false;
    }

    cpr::Response r = cpr::Patch(cpr::Url{url}, MakeGitHubAuthHeaders(pat_), cpr::Body{payloadDump},
                                 cpr::Header{{"Content-Type", "application/json"}},
                                 cpr::ConnectTimeout{kGitHubConnectTimeoutMs}, cpr::Timeout{kGitHubOverallTimeoutMs});

    if (r.status_code < 200 || r.status_code >= 300) {
        outError = ComposeHttpErrorString("PATCH", suffix, r.status_code, r.error.message, r.text);
        LOG_WARN("GitHubClient::StateTransition failed: %s", outError.c_str());
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, issueKey, auditOp, false, outError);
        return false;
    }
    BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, issueKey, auditOp, true, "");
    return true;
}

bool GitHubClient::ResolveReviewThread(const std::string& threadId, std::string& outError) {
    outError.clear();
    const std::string auditAction = kAuditResolveReviewThread;
    const std::string auditOp = BackendAuditTrail::MakeOperationId("github-resolve-review-thread");
    // Audit context-key is the thread id itself — the most stable identifier
    // we can produce without re-fetching the PR. Issue-keyed audit rows
    // distinguish on the presence of `#`; thread-id rows have neither.
    nlohmann::json beginData = nlohmann::json::object();
    beginData["threadId"] = threadId;
    BackendAuditTrail::AppendBegin(auditAction, kGitHubAuditSource, threadId, auditOp, beginData);

    if (pat_.empty()) {
        outError = "GitHub PAT not configured (set cfg.GitHubPat).";
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, threadId, auditOp, false, outError);
        return false;
    }
    if (threadId.empty()) {
        outError = "ResolveReviewThread: threadId is required.";
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, threadId, auditOp, false, outError);
        return false;
    }

    // Build the GraphQL request body. The mutation shape mirrors the
    // merge-gates plan's reviewThreads query — same wire-format the gate
    // poller reads from, so a successful resolve flips the bit the gate
    // looks at.
    const nlohmann::json reqBody = GitHubClientHelpers::BuildResolveReviewThreadBody(threadId);
    const std::string payloadDump = reqBody.dump();
    if (GitHubClientHelpers::ShouldRejectAsTooLarge(payloadDump.size())) {
        outError = "GitHub ResolveReviewThread: body exceeds outbound size cap.";
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, threadId, auditOp, false, outError);
        return false;
    }

    const std::string url = GitHubClientHelpers::BuildGraphqlUrl(baseUrl_);
    cpr::Response r = cpr::Post(cpr::Url{url}, MakeGitHubAuthHeaders(pat_), cpr::Body{payloadDump},
                                cpr::Header{{"Content-Type", "application/json"}},
                                cpr::ConnectTimeout{kGitHubConnectTimeoutMs}, cpr::Timeout{kGitHubOverallTimeoutMs});

    if (r.status_code < 200 || r.status_code >= 300) {
        outError = ComposeHttpErrorString("POST", "/graphql", r.status_code, r.error.message, r.text);
        LOG_WARN("GitHubClient::ResolveReviewThread failed: %s", outError.c_str());
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, threadId, auditOp, false, outError);
        return false;
    }
    // GraphQL returns 200 even on logical errors — defer the parse + shape
    // checks to the pure helper. A returned-false path is treated as a hard
    // failure; a returned-true with isResolved=false logs a warn (the
    // mutation succeeded structurally but the thread wasn't marked resolved,
    // which only happens if the thread was already resolved or permissions
    // dropped server-side).
    bool isResolved = false;
    std::string parseErr;
    if (!GitHubClientHelpers::ParseResolveReviewThreadResponse(r.text, isResolved, parseErr)) {
        outError = parseErr;
        LOG_WARN("GitHubClient::ResolveReviewThread parse failure: %s", outError.c_str());
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, threadId, auditOp, false, outError);
        return false;
    }
    if (!isResolved) {
        outError = "ResolveReviewThread returned isResolved=false.";
        LOG_WARN("GitHubClient::ResolveReviewThread: %s thread=%s", outError.c_str(), threadId.c_str());
        BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, threadId, auditOp, false, outError);
        return false;
    }
    BackendAuditTrail::AppendResult(auditAction, kGitHubAuditSource, threadId, auditOp, true, "");
    return true;
}

bool GitHubClient::LookupReviewThreadIdForComment(const std::string& restCommentId, std::string& outThreadId,
                                                  std::string& outError) {
    outThreadId.clear();
    outError.clear();
    if (pat_.empty()) {
        outError = "GitHub PAT not configured (set cfg.GitHubPat).";
        return false;
    }
    if (restCommentId.empty()) {
        outError = "LookupReviewThreadIdForComment: restCommentId is required.";
        return false;
    }
    // Encode REST id → canonical GraphQL node id via the helper.
    const std::string nodeId = GitHubClientHelpers::BuildPullRequestReviewCommentNodeId(restCommentId);
    const nlohmann::json reqBody = GitHubClientHelpers::BuildLookupReviewThreadBody(nodeId);
    const std::string payloadDump = reqBody.dump();
    if (GitHubClientHelpers::ShouldRejectAsTooLarge(payloadDump.size())) {
        outError = "LookupReviewThreadIdForComment: body exceeds outbound size cap.";
        return false;
    }

    const std::string url = GitHubClientHelpers::BuildGraphqlUrl(baseUrl_);
    cpr::Response r = cpr::Post(cpr::Url{url}, MakeGitHubAuthHeaders(pat_), cpr::Body{payloadDump},
                                cpr::Header{{"Content-Type", "application/json"}},
                                cpr::ConnectTimeout{kGitHubConnectTimeoutMs}, cpr::Timeout{kGitHubOverallTimeoutMs});

    if (r.status_code < 200 || r.status_code >= 300) {
        outError = ComposeHttpErrorString("POST", "/graphql", r.status_code, r.error.message, r.text);
        LOG_WARN("GitHubClient::LookupReviewThreadIdForComment failed: %s", outError.c_str());
        return false;
    }
    std::string parseErr;
    if (!GitHubClientHelpers::ParseLookupReviewThreadResponse(r.text, outThreadId, parseErr)) {
        outError = parseErr;
        return false;
    }
    return true;
}

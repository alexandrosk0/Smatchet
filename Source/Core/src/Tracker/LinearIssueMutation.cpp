#include "LinearClient.h"

#include "BackendAuditTrail.h"
#include "ConfigManager.h"
#include "IssueDraft.h"
#include "Json/BoundedJsonParse.h"
#include "LinearClientHelpers.h"
#include "LinearIssueSearch.h" // BuildLinearHeaders (cpr::Header — shared with the read path)
#include "LinearMutationPure.h"
#include "Logger.h"
#include "TrackerError.h"
#include "TrackerFieldSchema.h"
#include "TrackerHttpUtils.h"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

// Slice 3 of docs/plans/linear-tracker-backend.md — the Linear GraphQL
// mutation surface (issueUpdate / issueCreate / commentCreate), split out of the
// read-only LinearClient shell exactly like JiraIssueMutation.cpp /
// PlaneIssueMutation.cpp split their writes out of the client TU. A thin
// HTTP-touching adapter: the cpr-free request-builders + response-parsers live in
// LinearMutationPure.{h,cpp} (hermetically unit-tested); this TU is only the auth
// resolution + POST orchestration. Every POST routes through TrackerPostLogged.

// ISSUE-ID INVARIANT: CachedTicket.id (the issueId / issueKey these methods
// receive) is the human IDENTIFIER ("ENG-123"), but every Linear mutation/comment
// needs the issue UUID. Each write first RESOLVES the UUID via a lightweight
// `query($id:String!){ issue(id:$id){ id } }` — Linear's `issue(id)` accepts the
// identifier and returns the canonical UUID — then issues the mutation against
// that UUID (ResolveIssueUuid below).

using smatchet::linear::BuildLinearHeaders;

namespace {

const char* const kApiKeyMissingError =
    "Linear API key not configured (set Preferences > Tracker > Linear API key)";

// POST one GraphQL document + variables and return the parsed body. On any
// transport / HTTP / GraphQL-errors[] / success==false failure, `outError` is set
// (best-effort human message) and the call returns false. `outIssue` receives the
// mutation payload's nested `issue` object on success (empty for commentCreate).
// Pillar-2: only reachable from the create/update/offline-replay worker paths.
bool RunLinearMutation(const std::string& apiUrl, const std::string& apiKey, const std::string& document,
                       const nlohmann::json& variables, const char* mutationName, nlohmann::json& outIssue,
                       std::string& outError) {
    const std::string body = smatchet::linear::BuildGraphQLBody(document, variables);
    /* PILLAR2_WORKER_ONLY */ // est-latency: 15000ms
    const cpr::Response resp = TrackerPostLogged("LinearClient", apiUrl, BuildLinearHeaders(apiKey), body);

    // Bounded parse of the untrusted HTTP body (discarded on failure) — audit: unbounded-recursion-DoS.
    nlohmann::json parsed = smatchet::json_safe::ParseBoundedOrDiscarded(resp.text);
    std::string errorMessage;
    const bool hasErrors =
        !parsed.is_discarded() && smatchet::linear::LinearResponseHasErrors(parsed, errorMessage);
    if (resp.status_code != 200 || parsed.is_discarded() || hasErrors) {
        outError = !errorMessage.empty()
                       ? errorMessage
                       : smatchet::linear::ExtractLinearErrorMessage(static_cast<int>(resp.status_code), resp.text);
        return false;
    }
    if (!smatchet::linear::ParseMutationSucceeded(parsed, mutationName, outIssue)) {
        outError = std::string("Linear ") + mutationName + " returned success=false";
        return false;
    }
    return true;
}

// Resolve the issue UUID from a Linear identifier ("ENG-123") via the lightweight
// `issue(id)` query. Returns empty + sets `outError` on any failure. The mutation
// surface needs the UUID; the grid only carries the identifier (CachedTicket.id).
std::string ResolveIssueUuid(const std::string& apiUrl, const std::string& apiKey, const std::string& identifier,
                             std::string& outError) {
    nlohmann::json variables = nlohmann::json::object();
    variables["id"] = identifier;
    const std::string body = smatchet::linear::BuildGraphQLBody(smatchet::linear::ResolveIssueQueryDocument(), variables);
    /* PILLAR2_WORKER_ONLY */ // est-latency: 15000ms
    const cpr::Response resp = TrackerPostLogged("LinearClient", apiUrl, BuildLinearHeaders(apiKey), body);

    // Bounded parse of the untrusted HTTP body (discarded on failure) — audit: unbounded-recursion-DoS.
    // SMATCHET_DEVIATION(rule=duplication): the build-body → TrackerPostLogged → bounded-parse →
    // status/errors[] check is the standard Linear GraphQL call shape shared by the distinct
    // read/resolve callers (ResolveIssueUuid here, ResolveCatalogTeamId in LinearClient); each has
    // different downstream extraction, so per ADR-0015 an exemption is preferred over abstracting
    // the prefix across unrelated call sites.
    nlohmann::json parsed = smatchet::json_safe::ParseBoundedOrDiscarded(resp.text);
    std::string errorMessage;
    if (resp.status_code != 200 || parsed.is_discarded() ||
        smatchet::linear::LinearResponseHasErrors(parsed, errorMessage)) {
        outError = !errorMessage.empty()
                       ? errorMessage
                       : smatchet::linear::ExtractLinearErrorMessage(static_cast<int>(resp.status_code), resp.text);
        return "";
    }
    return smatchet::linear::ParseResolvedIssueUuid(parsed, identifier, outError);
}

} // namespace

// === ITrackerIssueMutations ===

TrackerError LinearClient::UpdateIssueFields(const std::string& issueId, const nlohmann::json& fields) {
    // `fields` is a ready IssueUpdateInput (built by BuildFieldPayload). Resolve
    // the issue UUID from the identifier, then issueUpdate against it. cfg-less
    // interface — resolve credentials from the settled on-disk config (issue #979,
    // same per-request pattern Jira/Plane mutations use).
    const smatchet::linear::LinearRequestAuth auth = ResolveAuth(nullptr);
    if (auth.ApiKey.empty()) {
        return TrackerErrorAuth(kApiKeyMissingError);
    }
    if (!fields.is_object() || fields.empty()) {
        return TrackerErrorInvalidRequest("LinearClient::UpdateIssueFields: empty IssueUpdateInput");
    }

    std::string resolveError;
    const std::string uuid = ResolveIssueUuid(auth.ApiUrl, auth.ApiKey, issueId, resolveError);
    if (uuid.empty()) {
        LOG_ERROR("LinearClient::UpdateIssueFields: resolve %s failed — %s", issueId.c_str(), resolveError.c_str());
        return TrackerErrorInvalidRequest(resolveError);
    }

    nlohmann::json variables = nlohmann::json::object();
    variables["id"] = uuid;
    variables["input"] = fields;
    nlohmann::json updatedIssue;
    std::string outError;
    if (!RunLinearMutation(auth.ApiUrl, auth.ApiKey, smatchet::linear::IssueUpdateMutationDocument(), variables,
                           "issueUpdate", updatedIssue, outError)) {
        LOG_ERROR("LinearClient::UpdateIssueFields: issueUpdate %s failed — %s", issueId.c_str(), outError.c_str());
        return TrackerErrorInvalidRequest(outError);
    }
    LOG_INFO("LinearClient::UpdateIssueFields: updated %s (uuid=%s)", issueId.c_str(), uuid.c_str());
    return TrackerError::Ok();
}

TrackerError LinearClient::UpdateField(const std::string& issueId, const TrackerField& field,
                                       const std::vector<std::string>& values) {
    // Set-replace single-field edit: build the IssueUpdateInput key for this field
    // (display→UUID option resolution lives in BuildIssueUpdateInput), then route
    // through UpdateIssueFields so the UUID-resolve + issueUpdate happen once.
    Result<nlohmann::json, TrackerError> payload = BuildFieldPayload(field, values);
    if (!payload) {
        return payload.error();
    }
    return UpdateIssueFields(issueId, payload.value());
}

Result<nlohmann::json, TrackerError> LinearClient::BuildFieldPayload(const TrackerField& field,
                                                                     const std::vector<std::string>& values) {
    // Pure — maps the Smatchet field id + set-replace values to a single
    // IssueUpdateInput key. Unknown ids → Err "field not editable on Linear".
    return smatchet::linear::BuildIssueUpdateInput(field, values);
}

Result<nlohmann::json, TrackerError> LinearClient::BuildCreatePayload(const IssueDraft& draft,
                                                                      const std::vector<TrackerField>& catalog) {
    // Build an IssueCreateInput from the draft. teamId is REQUIRED — resolve from
    // the live cfg.LinearTeamId (the draft scope rides ProjectKey for other
    // backends, but Linear's create scope is the configured team UUID). cfg-less
    // interface → settled on-disk config (issue #979). The build itself is pure.
    const TrackerConfig cfg = ConfigManager::Load();
    return smatchet::linear::BuildIssueCreateInput(draft, catalog, cfg.LinearTeamId);
}

Result<std::string, TrackerError> LinearClient::CreateIssue(const nlohmann::json& fields) {
    using CreateResult = Result<std::string, TrackerError>;
    const std::string auditOp = BackendAuditTrail::MakeOperationId("issue-create");
    BackendAuditTrail::AppendBegin("issue_create", "linear_client", std::string(), auditOp,
                                   nlohmann::json{{"diff", BackendAuditTrail::MakeFieldDiffUnknownBefore(fields)}});

    // cfg-less interface — resolve credentials from the settled on-disk config
    // (issue #979). `fields` is a ready IssueCreateInput from BuildCreatePayload.
    const smatchet::linear::LinearRequestAuth auth = ResolveAuth(nullptr);
    if (auth.ApiKey.empty()) {
        BackendAuditTrail::AppendResult("issue_create", "linear_client", std::string(), auditOp, false,
                                        kApiKeyMissingError);
        return CreateResult::Err(TrackerErrorAuth(kApiKeyMissingError));
    }
    if (!fields.is_object() || !fields.contains("teamId") || !fields.contains("title")) {
        const std::string msg = "LinearClient::CreateIssue: payload missing teamId/title (call BuildCreatePayload first)";
        BackendAuditTrail::AppendResult("issue_create", "linear_client", std::string(), auditOp, false, msg);
        return CreateResult::Err(TrackerErrorInvalidRequest(msg));
    }

    nlohmann::json variables = nlohmann::json::object();
    variables["input"] = fields;
    nlohmann::json createdIssue;
    std::string outError;
    if (!RunLinearMutation(auth.ApiUrl, auth.ApiKey, smatchet::linear::IssueCreateMutationDocument(), variables,
                           "issueCreate", createdIssue, outError)) {
        LOG_ERROR("LinearClient::CreateIssue: issueCreate failed — %s", outError.c_str());
        BackendAuditTrail::AppendResult("issue_create", "linear_client", std::string(), auditOp, false, outError);
        return CreateResult::Err(TrackerErrorInvalidRequest(outError));
    }

    // issueCreate{ issue{ identifier } } — the identifier ("ENG-123") is the
    // CachedTicket.id the grid keys by. A 2xx success with no identifier is a
    // "created, key unknown" case → Ok(empty), matching the Plane/GitHub contract.
    const std::string identifier = smatchet::linear::ParseCreatedIssueIdentifier(createdIssue);
    if (identifier.empty()) {
        LOG_WARN("LinearClient::CreateIssue: created but response carried no identifier");
        BackendAuditTrail::AppendResult("issue_create", "linear_client", std::string(), auditOp, true, std::string());
        return CreateResult::Ok(std::string());
    }
    LOG_INFO("LinearClient::CreateIssue: created Linear issue %s", identifier.c_str());
    BackendAuditTrail::AppendResult("issue_create", "linear_client", identifier, auditOp, true, std::string());
    return CreateResult::Ok(identifier);
}

// === ITrackerCollaboration ===

TrackerError LinearClient::AddIssueCommentPlain(const TrackerConfig& cfg, const std::string& issueKey,
                                                const std::string& plainText) {
    // cfg-carrying mutation — resolve credentials from the live cfg (matches
    // CreateIssue / UpdateField). Resolve the issue UUID from the identifier, then
    // commentCreate with the body. Direct-post: comments are exempt from the
    // offline-queue + audit-trail wiring (mirrors the Jira/GitHub comment post).
    const smatchet::linear::LinearRequestAuth auth = ResolveAuth(&cfg);
    if (auth.ApiKey.empty()) {
        return TrackerErrorAuth(kApiKeyMissingError);
    }

    std::string resolveError;
    const std::string uuid = ResolveIssueUuid(auth.ApiUrl, auth.ApiKey, issueKey, resolveError);
    if (uuid.empty()) {
        LOG_ERROR("LinearClient::AddIssueCommentPlain: resolve %s failed — %s", issueKey.c_str(),
                  resolveError.c_str());
        return TrackerErrorInvalidRequest(resolveError);
    }

    nlohmann::json variables = nlohmann::json::object();
    variables["input"] = smatchet::linear::BuildCommentCreateInput(uuid, plainText);
    nlohmann::json ignoredIssue;
    std::string outError;
    if (!RunLinearMutation(auth.ApiUrl, auth.ApiKey, smatchet::linear::CommentCreateMutationDocument(), variables,
                           "commentCreate", ignoredIssue, outError)) {
        LOG_ERROR("LinearClient::AddIssueCommentPlain: commentCreate %s failed — %s", issueKey.c_str(),
                  outError.c_str());
        return TrackerErrorInvalidRequest(outError);
    }
    LOG_INFO("LinearClient::AddIssueCommentPlain: commented on %s (uuid=%s)", issueKey.c_str(), uuid.c_str());
    return TrackerError::Ok();
}

#include "LinearClient.h"

#include "BackendAuditTrail.h"
#include "ConfigManager.h"
#include "IssueDraft.h"
#include "LinearClientHelpers.h"
#include "LinearIssueSearch.h"
#include "Logger.h"
#include "TrackerError.h"
#include "TrackerFieldSchema.h"
#include "TrackerHttpUtils.h"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

// Slice 3 of docs/plans/active/linear-tracker-backend.md — the Linear GraphQL
// mutation surface (issueUpdate / issueCreate / commentCreate), split out of the
// read-only LinearClient shell exactly like JiraIssueMutation.cpp /
// PlaneIssueMutation.cpp split their writes out of the client TU. A thin
// HTTP-touching adapter over the cpr-free pure helpers (auth resolution, GraphQL
// body building, error extraction) — every POST routes through TrackerPostLogged.
//
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

// === GraphQL documents (hand-written, mirroring LinearIssueSearch.cpp) ===

// Resolve a Linear issue UUID from its identifier ("ENG-123"). `issue(id)`
// accepts either the UUID or the identifier and returns the canonical node id.
const char* const kResolveIssueQuery = "query($id:String!){ issue(id:$id){ id } }";

const char* const kIssueUpdateMutation =
    "mutation($id: String!, $input: IssueUpdateInput!){ "
    "issueUpdate(id:$id, input:$input){ success issue{ id identifier } } }";

const char* const kIssueCreateMutation =
    "mutation($input: IssueCreateInput!){ "
    "issueCreate(input:$input){ success issue{ id identifier url } } }";

const char* const kCommentCreateMutation =
    "mutation($input: CommentCreateInput!){ commentCreate(input:$input){ success comment{ id } } }";

// === Pure helpers (no I/O) ===

// Resolve one input value to the Linear UUID it names. The grid hands select-like
// edits the option Id (UUID) already, but a value may also arrive as the display
// text; resolve both via the field's options (TrackerFieldOption: Id=UUID,
// Value=display) — mirrors the option-resolution PlaneClient::BuildFieldPayload
// performs. An unmatched value passes through unchanged (already a UUID, or the
// field carried no catalog options this session).
std::string ResolveOptionUuid(const TrackerField& field, const std::string& value) {
    for (std::size_t i = 0; i < field.AllowedValueOptions.size(); ++i) {
        if (field.AllowedValueOptions[i].Id == value) {
            return value; // already a UUID
        }
    }
    for (std::size_t i = 0; i < field.AllowedValueOptions.size(); ++i) {
        if (field.AllowedValueOptions[i].Value == value) {
            return field.AllowedValueOptions[i].Id; // display → UUID
        }
    }
    return value;
}

// Map a Linear priority value to the fixed Int 0–4 enum. Accepts the catalog Id
// ("0".."4") directly, or the display label ("Urgent"/"High"/"Medium"/"Low"/
// "No priority"). Returns false (→ caller omits priority / errors) when the input
// is neither, so a stray value never silently becomes priority 0.
bool MapPriorityValueToInt(const std::string& value, int& outPriority) {
    if (value == "0" || value == "No priority" || value == "None") {
        outPriority = 0;
        return true;
    }
    if (value == "1" || value == "Urgent") {
        outPriority = 1;
        return true;
    }
    if (value == "2" || value == "High") {
        outPriority = 2;
        return true;
    }
    if (value == "3" || value == "Medium") {
        outPriority = 3;
        return true;
    }
    if (value == "4" || value == "Low") {
        outPriority = 4;
        return true;
    }
    return false;
}

// Map a Smatchet field id + its set-replace values to a single IssueUpdateInput
// key/value (plan § Field→input mapping). `outInput` receives the one key on
// success; an unknown field id → Err "field not editable on Linear". UpdateField
// semantics are SET-REPLACE: `values` is the full intended set, so single-valued
// keys take values.front() (empty clears) and labelIds takes the whole array.
Result<nlohmann::json, TrackerError> BuildIssueUpdateInput(const TrackerField& field,
                                                          const std::vector<std::string>& values) {
    nlohmann::json input = nlohmann::json::object();
    const std::string& id = field.Id;
    const std::string first = values.empty() ? std::string() : values.front();

    if (id == "summary") {
        input["title"] = first;
    } else if (id == "description") {
        input["description"] = first; // Linear stores markdown verbatim
    } else if (id == "status") {
        input["stateId"] = ResolveOptionUuid(field, first);
    } else if (id == "assignee") {
        // Empty clears the assignee (IssueUpdateInput accepts null assigneeId).
        if (first.empty()) {
            input["assigneeId"] = nullptr;
        } else {
            input["assigneeId"] = ResolveOptionUuid(field, first);
        }
    } else if (id == "labels") {
        nlohmann::json labelIds = nlohmann::json::array();
        for (std::size_t i = 0; i < values.size(); ++i) {
            labelIds.push_back(ResolveOptionUuid(field, values[i]));
        }
        input["labelIds"] = labelIds; // set-replace: full intended set
    } else if (id == "priority") {
        int priorityInt = 0;
        if (first.empty()) {
            input["priority"] = 0; // cleared → No priority
        } else if (MapPriorityValueToInt(first, priorityInt)) {
            input["priority"] = priorityInt;
        } else {
            return Result<nlohmann::json, TrackerError>::Err(TrackerErrorInvalidRequest(
                std::string("LinearClient: unrecognised priority value '") + first + "'"));
        }
    } else if (id == "project") {
        if (first.empty()) {
            input["projectId"] = nullptr;
        } else {
            input["projectId"] = ResolveOptionUuid(field, first);
        }
    } else {
        return Result<nlohmann::json, TrackerError>::Err(TrackerErrorInvalidRequest(
            std::string("LinearClient: field '") + id + "' is not editable on Linear"));
    }
    return Result<nlohmann::json, TrackerError>::Ok(std::move(input));
}

// Split a labels CSV into a JSON array of resolved label UUIDs. Each trimmed,
// non-blank token is resolved display→UUID via `labelsField`'s options (pass-through
// when unmatched / field absent). Empty array when nothing remains.
nlohmann::json BuildLabelIdsFromCsv(const std::string& labelsCsv, const TrackerField* labelsField) {
    nlohmann::json labelIds = nlohmann::json::array();
    std::size_t start = 0;
    while (start <= labelsCsv.size()) {
        const std::size_t comma = labelsCsv.find(',', start);
        const std::size_t end = (comma == std::string::npos) ? labelsCsv.size() : comma;
        const std::string token = labelsCsv.substr(start, end - start);
        const std::size_t first = token.find_first_not_of(" \t\r\n");
        if (first != std::string::npos) {
            const std::size_t last = token.find_last_not_of(" \t\r\n");
            const std::string trimmed = token.substr(first, last - first + 1);
            labelIds.push_back(labelsField != nullptr ? ResolveOptionUuid(*labelsField, trimmed) : trimmed);
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return labelIds;
}

// True when a parsed mutation payload reports `data.<mutationName>.success == true`.
// Defensive on any shape (Pillar 3). `outIssue` receives the nested `issue` object
// when present (issueUpdate / issueCreate carry one; commentCreate carries
// `comment` instead and leaves `outIssue` null).
bool MutationSucceeded(const nlohmann::json& parsed, const char* mutationName, nlohmann::json& outIssue) {
    outIssue = nlohmann::json();
    if (!parsed.is_object() || !parsed.contains("data") || !parsed["data"].is_object()) {
        return false;
    }
    const nlohmann::json& data = parsed["data"];
    if (!data.contains(mutationName) || !data[mutationName].is_object()) {
        return false;
    }
    const nlohmann::json& payload = data[mutationName];
    nlohmann::json::const_iterator issueIt = payload.find("issue");
    if (issueIt != payload.end() && issueIt->is_object()) {
        outIssue = *issueIt;
    }
    return payload.value("success", false);
}

} // namespace

// === HTTP-touching adapter ===

namespace {

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

    nlohmann::json parsed = nlohmann::json::parse(resp.text, nullptr, false);
    std::string errorMessage;
    const bool hasErrors =
        !parsed.is_discarded() && smatchet::linear::LinearResponseHasErrors(parsed, errorMessage);
    if (resp.status_code != 200 || parsed.is_discarded() || hasErrors) {
        outError = !errorMessage.empty()
                       ? errorMessage
                       : smatchet::linear::ExtractLinearErrorMessage(static_cast<int>(resp.status_code), resp.text);
        return false;
    }
    if (!MutationSucceeded(parsed, mutationName, outIssue)) {
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
    const std::string body = smatchet::linear::BuildGraphQLBody(kResolveIssueQuery, variables);
    /* PILLAR2_WORKER_ONLY */ // est-latency: 15000ms
    const cpr::Response resp = TrackerPostLogged("LinearClient", apiUrl, BuildLinearHeaders(apiKey), body);

    nlohmann::json parsed = nlohmann::json::parse(resp.text, nullptr, false);
    std::string errorMessage;
    if (resp.status_code != 200 || parsed.is_discarded() ||
        smatchet::linear::LinearResponseHasErrors(parsed, errorMessage)) {
        outError = !errorMessage.empty()
                       ? errorMessage
                       : smatchet::linear::ExtractLinearErrorMessage(static_cast<int>(resp.status_code), resp.text);
        return "";
    }
    if (!parsed.is_object() || !parsed.contains("data") || !parsed["data"].is_object() ||
        !parsed["data"].contains("issue") || !parsed["data"]["issue"].is_object()) {
        outError = std::string("Linear issue '") + identifier + "' not found";
        return "";
    }
    const std::string uuid = parsed["data"]["issue"].value("id", std::string());
    if (uuid.empty()) {
        outError = std::string("Linear issue '") + identifier + "' resolved to an empty UUID";
    }
    return uuid;
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
    if (!RunLinearMutation(auth.ApiUrl, auth.ApiKey, kIssueUpdateMutation, variables, "issueUpdate", updatedIssue,
                           outError)) {
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
    return BuildIssueUpdateInput(field, values);
}

Result<nlohmann::json, TrackerError> LinearClient::BuildCreatePayload(const IssueDraft& draft,
                                                                      const std::vector<TrackerField>& catalog) {
    using PayloadResult = Result<nlohmann::json, TrackerError>;
    // Build an IssueCreateInput from the draft. teamId is REQUIRED — resolve from
    // the live cfg.LinearTeamId (the draft scope rides ProjectKey for other
    // backends, but Linear's create scope is the configured team UUID). cfg-less
    // interface → settled on-disk config (issue #979).
    const TrackerConfig cfg = ConfigManager::Load();
    const std::string teamId = cfg.LinearTeamId;
    if (teamId.empty()) {
        return PayloadResult::Err(TrackerErrorInvalidRequest(
            "Linear issue create requires a configured Team (set Preferences > Tracker > Linear team)"));
    }

    auto fieldOr = [&draft](const char* key) -> std::string {
        const std::unordered_map<std::string, std::string>::const_iterator it = draft.FieldValues.find(key);
        return it == draft.FieldValues.end() ? std::string() : it->second;
    };
    auto fieldForId = [&catalog](const std::string& id) -> const TrackerField* {
        for (std::size_t i = 0; i < catalog.size(); ++i) {
            if (catalog[i].Id == id) {
                return &catalog[i];
            }
        }
        return nullptr;
    };
    // Resolve a draft display value to its UUID via the matching catalog field's
    // options (Id=UUID, Value=display); pass through unchanged when absent.
    auto resolveViaCatalog = [&fieldForId](const std::string& id, const std::string& value) -> std::string {
        if (value.empty()) {
            return value;
        }
        const TrackerField* f = fieldForId(id);
        return f != nullptr ? ResolveOptionUuid(*f, value) : value;
    };

    const std::string summary = fieldOr("summary");
    if (summary.empty()) {
        return PayloadResult::Err(TrackerErrorInvalidRequest("Linear issue create requires a non-empty title (summary)"));
    }

    nlohmann::json input = nlohmann::json::object();
    input["teamId"] = teamId;
    input["title"] = summary;

    const std::string description = fieldOr("description");
    if (!description.empty()) {
        input["description"] = description; // markdown verbatim
    }
    const std::string priority = fieldOr("priority");
    if (!priority.empty()) {
        int priorityInt = 0;
        if (MapPriorityValueToInt(priority, priorityInt)) {
            input["priority"] = priorityInt;
        }
    }
    const std::string status = fieldOr("status");
    if (!status.empty()) {
        input["stateId"] = resolveViaCatalog("status", status);
    }
    const std::string assignee = fieldOr("assignee");
    if (!assignee.empty()) {
        input["assigneeId"] = resolveViaCatalog("assignee", assignee);
    }
    const std::string project = fieldOr("project");
    if (!project.empty()) {
        input["projectId"] = resolveViaCatalog("project", project);
    }
    const std::string labelsCsv = fieldOr("labels");
    if (!labelsCsv.empty()) {
        const nlohmann::json labelIds = BuildLabelIdsFromCsv(labelsCsv, fieldForId("labels"));
        if (!labelIds.empty()) {
            input["labelIds"] = labelIds;
        }
    }
    return PayloadResult::Ok(std::move(input));
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
    if (!RunLinearMutation(auth.ApiUrl, auth.ApiKey, kIssueCreateMutation, variables, "issueCreate", createdIssue,
                           outError)) {
        LOG_ERROR("LinearClient::CreateIssue: issueCreate failed — %s", outError.c_str());
        BackendAuditTrail::AppendResult("issue_create", "linear_client", std::string(), auditOp, false, outError);
        return CreateResult::Err(TrackerErrorInvalidRequest(outError));
    }

    // issueCreate{ issue{ identifier } } — the identifier ("ENG-123") is the
    // CachedTicket.id the grid keys by. A 2xx success with no identifier is a
    // "created, key unknown" case → Ok(empty), matching the Plane/GitHub contract.
    const std::string identifier = createdIssue.is_object() ? createdIssue.value("identifier", std::string()) : "";
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

    nlohmann::json input = nlohmann::json::object();
    input["issueId"] = uuid;
    input["body"] = plainText; // markdown verbatim
    nlohmann::json variables = nlohmann::json::object();
    variables["input"] = input;
    nlohmann::json ignoredIssue;
    std::string outError;
    if (!RunLinearMutation(auth.ApiUrl, auth.ApiKey, kCommentCreateMutation, variables, "commentCreate", ignoredIssue,
                           outError)) {
        LOG_ERROR("LinearClient::AddIssueCommentPlain: commentCreate %s failed — %s", issueKey.c_str(),
                  outError.c_str());
        return TrackerErrorInvalidRequest(outError);
    }
    LOG_INFO("LinearClient::AddIssueCommentPlain: commented on %s (uuid=%s)", issueKey.c_str(), uuid.c_str());
    return TrackerError::Ok();
}

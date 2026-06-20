#ifndef SMATCHET_LINEAR_MUTATION_PURE_H
#define SMATCHET_LINEAR_MUTATION_PURE_H

// Pure (cpr-free) request-builders + response-parsers for the Linear write
// surface (issueUpdate / issueCreate / commentCreate), split out of the
// cpr-bound LinearIssueMutation.cpp exactly like LinearClientHelpers /
// LinearIssueMappingPure split the read path. Keeping the GraphQL documents,
// the field-to-input mapping, priority/option resolution, and the success/entity
// response parse in their own TU lets the doctest rig pin the exact wire contract
// without a network stack — the hermetic half of the Linear write-path coverage
// (the live half is the credential-gated smoke). Null/missing-safe per Pillar 3.

#include "IssueDraft.h"
#include "SmatchetResult.h"
#include "TrackerError.h"
#include "TrackerFieldSchema.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace smatchet {
namespace linear {

// === GraphQL documents (exposed so tests pin the exact wire strings) ===
// `issue(id)` accepts either the UUID or the "ENG-123" identifier and returns the
// canonical node id — the UUID every mutation/comment needs.
const char* ResolveIssueQueryDocument();
const char* IssueUpdateMutationDocument();
const char* IssueCreateMutationDocument();
const char* CommentCreateMutationDocument();

// === Option / priority resolution ===

/// Resolve one input value to the Linear UUID it names. The grid usually hands a
/// select edit the option Id (UUID) already, but a value may arrive as display
/// text; resolve both via the field's options (TrackerFieldOption: Id=UUID,
/// Value=display). An unmatched value passes through unchanged (already a UUID,
/// or the field carried no catalog options this session).
std::string ResolveOptionUuid(const TrackerField& field, const std::string& value);

/// Map a Linear priority value to the fixed Int 0–4 enum. Accepts the catalog Id
/// ("0".."4") or the display label ("Urgent"/"High"/"Medium"/"Low"/"No priority").
/// Returns false when the input is neither, so a stray value never silently
/// becomes priority 0.
bool MapPriorityValueToInt(const std::string& value, int& outPriority);

// === Input builders ===

/// Map a Smatchet field id + its set-replace values to a single IssueUpdateInput
/// key/value. Unknown field id → Err "field not editable on Linear". SET-REPLACE:
/// `values` is the full intended set, so single-valued keys take values.front()
/// (empty clears) and labelIds takes the whole array.
Result<nlohmann::json, TrackerError> BuildIssueUpdateInput(const TrackerField& field,
                                                           const std::vector<std::string>& values);

/// Build an IssueCreateInput from a draft + catalog. `teamId` is REQUIRED (the
/// Linear create scope) and must be non-empty — the caller resolves it from
/// cfg.LinearTeamId. Err when teamId or the title (summary) is empty. Select-like
/// values resolve display→UUID via the matching catalog field's options.
Result<nlohmann::json, TrackerError> BuildIssueCreateInput(const IssueDraft& draft,
                                                           const std::vector<TrackerField>& catalog,
                                                           const std::string& teamId);

/// Build a CommentCreateInput { issueId, body }. `issueUuid` is the resolved node
/// UUID; `body` is stored as markdown verbatim.
nlohmann::json BuildCommentCreateInput(const std::string& issueUuid, const std::string& body);

/// Split a labels CSV into a JSON array of resolved label UUIDs. Each trimmed,
/// non-blank token resolves display→UUID via `labelsField`'s options (pass-through
/// when unmatched / field null). Empty array when nothing remains.
nlohmann::json BuildLabelIdsFromCsv(const std::string& labelsCsv, const TrackerField* labelsField);

// === Response parsers ===

/// True when a parsed mutation payload reports `data.<mutationName>.success`.
/// Defensive on any shape. `outIssue` receives the nested `issue` object when
/// present (issueUpdate / issueCreate carry one; commentCreate carries `comment`
/// instead and leaves `outIssue` null).
bool ParseMutationSucceeded(const nlohmann::json& parsed, const char* mutationName, nlohmann::json& outIssue);

/// Parse a resolve-issue response (`{ data: { issue: { id } } }`) into the UUID.
/// Returns empty + sets `outError` (referencing `identifier`) when the issue is
/// missing or the id is blank. Pure — the HTTP/errors[] handling stays in the
/// cpr adapter; this is only the data-shape half.
std::string ParseResolvedIssueUuid(const nlohmann::json& parsed, const std::string& identifier, std::string& outError);

/// Extract the created issue identifier ("ENG-123") from an issueCreate payload's
/// `issue` node (the value ParseMutationSucceeded wrote to `outIssue`). Empty when
/// absent — a "created, key unknown" case the caller maps to Ok(empty).
std::string ParseCreatedIssueIdentifier(const nlohmann::json& createdIssue);

} // namespace linear
} // namespace smatchet

#endif // SMATCHET_LINEAR_MUTATION_PURE_H

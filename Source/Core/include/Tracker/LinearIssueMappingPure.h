#ifndef SMATCHET_LINEAR_ISSUE_MAPPING_PURE_H
#define SMATCHET_LINEAR_ISSUE_MAPPING_PURE_H

// Pure-logic JSON -> CachedTicket mapping for Linear issue nodes, split out of
// the cpr-bound search TU (like GitHubIssueSearchMapping) so the doctest rig
// exercises it without HTTP. Null/missing-safe per Pillar 3: a malformed or
// absent nested field maps to a safe empty default, never a throw.

#include "CachedTicketTypes.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace smatchet {
namespace linear {

/// Map a single Linear `Issue` GraphQL node to a CachedTicket. Field ids match
/// the shared grid conventions (summary/description/status/assignee/labels/
/// author/created/updated) plus Linear-relevant priority/project/cycle/team/url.
/// `id` = the issue `identifier` (e.g. "ENG-123"); `created`/`updated` keep the
/// raw ISO-8601 strings (the date renderer formats them). Display names are used
/// for select-like fields; option UUIDs belong to the field catalog, not here.
CachedTicket MapLinearIssueNodeToCachedTicket(const nlohmann::json& node);

/// Map an `issues.nodes` array into a vector of CachedTicket. Nodes that are not
/// objects are skipped so the helper stays logger-free (the live caller can log
/// a summary count). Order is preserved.
std::vector<CachedTicket> MapLinearIssueNodesToTickets(const nlohmann::json& nodes);

} // namespace linear
} // namespace smatchet

#endif // SMATCHET_LINEAR_ISSUE_MAPPING_PURE_H

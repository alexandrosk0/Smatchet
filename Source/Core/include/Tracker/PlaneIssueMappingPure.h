#ifndef SMATCHET_PLANE_ISSUE_MAPPING_PURE_H
#define SMATCHET_PLANE_ISSUE_MAPPING_PURE_H

// Slice 2 of docs/plans/shipped/autonomous-debugging-no-creds.md — pure-logic JSON →
// CachedTicket mapping helpers extracted out of PlaneIssueSearch.cpp (which
// pulls cpr) so the doctest rig can exercise them without HTTP. Mirrors the
// GitHub backend's split (`GitHubIssueSearchMapping.{h,cpp}`).
// Surfaces:
//   - MapPlaneWorkItemJsonToCachedTicket: maps a single Plane `/work-items`
//     list entry to a CachedTicket. Honours the same Plane shape branches the
//     production fetcher does (state_detail/state/state, assignees+assignee_details,
//     cycle_details/cycle, label_details/labels, type_detail/type), preserves
//     description_html via fieldRichValues["description"], and synthesises the
//     visual `<identifier>-<sequence>` key.
//   - MapPlaneWorkItemsArrayToCachedTickets: applies the per-issue mapper to
//     each element of a `results` array, optionally returning a `key → uuid`
//     map for the caller's keyToId cache. Silently skips items that throw
//     during mapping (parity with the production fetcher's per-issue try/catch).
//   - BuildPlaneAuthHeaders: pure-logic header building (x-api-key,
//     Accept/Content-Type) — mirror of `PlaneClient::BuildPlaneHeaders` so the
//     header shape is testable without instantiating a PlaneClient.
//   - NextPaginationCursor: reads `next_page_results` + `next_cursor` from a
//     Plane list payload and returns the next cursor (empty when no more
//     pages). Drives pagination tests without a live server.

#include "CachedTicketTypes.h"

#include <nlohmann/json.hpp>

#include <string>
#include <unordered_map>
#include <vector>

struct TrackerConfig;

namespace smatchet {
namespace plane {

/// Look up a user's display name by Plane account id. Production code threads
/// the same data through a `std::vector<TrackerUser>` — the mapper accepts the
/// same vector indirectly via an opaque pair list to avoid coupling the pure
/// header to the full TrackerUser shape.
struct UserDisplayLookup {
    std::string AccountId;
    std::string DisplayName;
};

/// Per-issue JSON → CachedTicket mapper. `projectIdentifier` is the Plane
/// project's short identifier (e.g. "SMT"); when empty, the synthesised key
/// falls back to `#<sequence_id>` then UUID. `users` resolves assignee account
/// ids that the payload didn't already expand via `assignee_details`. Throws
/// nlohmann::json::exception on malformed input — the array variant catches.
CachedTicket MapPlaneWorkItemJsonToCachedTicket(const nlohmann::json& issue, const std::string& projectIdentifier,
                                                const std::vector<UserDisplayLookup>& users);

/// Batch wrapper around MapPlaneWorkItemJsonToCachedTicket. Skips items whose
/// mapping throws (production code logs via LOG_WARN; pure-logic helper stays
/// logger-free). When `outKeyToId` is non-null, fills it with the visual-key →
/// uuid associations for the caller's cache.
std::vector<CachedTicket>
MapPlaneWorkItemsArrayToCachedTickets(const nlohmann::json& results, const std::string& projectIdentifier,
                                      const std::vector<UserDisplayLookup>& users,
                                      std::unordered_map<std::string, std::string>* outKeyToId);

/// Pure-logic header builder. Mirrors `PlaneClient::BuildPlaneHeaders` so the
/// header shape is testable without instantiating a PlaneClient (which would
/// pull a recursive_mutex into the test rig).
std::unordered_map<std::string, std::string> BuildPlaneAuthHeaders(const std::string& planeApiKey);

/// Returns the next cursor token (empty when no more pages or when the payload
/// signals end-of-list). Reads both `next_page_results` (bool) and
/// `next_cursor` (string) from a Plane list-endpoint payload.
std::string NextPaginationCursor(const nlohmann::json& listPayload);

/// Pull a `key` string member from a Plane structured-query JSON blob (mirror of
/// the production `ExtractProjectFromPlaneQuery`). "" sentinel covers "no key
/// member" and "malformed JSON" — callers treat empty as "no key clause".
std::string ExtractKeyFromPlaneQuery(const std::string& planeQueryJson);

/// BACKLOG_CODE_REVIEW.md §B4-v2 — build the comma-joined `sequence_id__in` value
/// for Plane's work-items list filter from a set of requested issue keys.
/// Each visual key is `<identifier>-<sequence_id>` (e.g. `SMT-123`); the numeric
/// suffix after the LAST `-` is the Plane `sequence_id`. Only keys whose suffix is
/// a run of ASCII digits contribute. If ANY key fails to parse (a bare UUID, a
/// malformed key, an empty token), the whole filter is abandoned — returns "" so
/// the caller falls back to the unfiltered early-exit sweep and never silently
/// drops the unparseable key from the result set (correctness over speed).
/// Duplicate sequence_ids are de-duplicated; output order follows first-seen input
/// order (stable, so the URL is deterministic for a given key set). Empty input
/// returns "".
std::string BuildPlaneSequenceIdInFilter(const std::vector<std::string>& issueKeys);

} // namespace plane
} // namespace smatchet

#endif // SMATCHET_PLANE_ISSUE_MAPPING_PURE_H

#ifndef SMATCHET_TRACKER_PARENT_HIERARCHY_PURE_H
#define SMATCHET_TRACKER_PARENT_HIERARCHY_PURE_H

#include "CachedTicketTypes.h"

#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

/**
 * Pure parent/child hierarchy helpers over the cached ticket set.
 *
 * The tracker parsers emit the `parent` field as `"KEY - summary"` (or a bare `"KEY"`);
 * everything here keys off that one contract, so any backend whose parser fills
 * `parent` gets missing-parent fetch, story-group ordering, depth indent and the
 * hide-parents filter for free (design: docs/plans/active/parent-issue-hierarchy.md).
 *
 * No HTTP, no SQLite, no ImGui — unit-testable under the doctest rig. Every walk is
 * cycle-safe: a visited set plus `kMaxHierarchyDepth` bound the recursion so a parent
 * cycle in backend data can never recurse unbounded (UX Pillar 3).
 */
namespace ParentHierarchyPure {

/// Hard bound on ancestor walks; a chain deeper than this is treated as a cycle.
constexpr int kMaxHierarchyDepth = 64;

/// `"PROJ-1 - Summary"` / `"PROJ-1"` -> `"PROJ-1"`; whitespace trimmed; empty input -> empty.
/// Does NOT validate the key shape (see TrackerFieldPayloadPure::LooksLikeIssueKey for that).
std::string ParentKeyFromFieldValue(const std::string& raw);

/// Parent key of `ticket` from its `parent` field, or empty when the ticket has none.
std::string ParentKeyOf(const CachedTicket& ticket);

/// Every parent key referenced by any ticket (present or not), deduped, first-seen order.
std::vector<std::string> ReferencedParentIds(const std::vector<CachedTicket>& tickets);

/// Referenced parent keys that are NOT themselves in `tickets` — the keyed-fetch set.
/// Deduped, first-seen order (stable across runs so the fetch request is deterministic).
std::vector<std::string> MissingParentKeys(const std::vector<CachedTicket>& tickets);

/// Ids in `tickets` that at least one other ticket references as its parent.
std::unordered_set<std::string> PresentParentIds(const std::vector<CachedTicket>& tickets);

/// Per-ticket depth (index-aligned with `tickets`): number of PRESENT ancestors. A ticket
/// whose parent is absent from the set is a root (depth 0), matching FS. Cycles are cut
/// at `kMaxHierarchyDepth`.
std::vector<int> ComputeDepths(const std::vector<CachedTicket>& tickets);

/// Reorder `order` (indices into `tickets`, already column-sorted) so every parent is
/// immediately followed by its descendants. Roots keep their relative order; children
/// keep their relative order within their group. Out-of-range indices are dropped;
/// every in-range index appears exactly once (cycle members surface as roots).
std::vector<std::size_t> StoryGroupOrder(const std::vector<CachedTicket>& tickets,
                                         const std::vector<std::size_t>& order);

/// Indices of the PRESENT ancestors of `tickets[index]`, nearest parent first. Empty for a
/// root / out-of-range index. Bounded by `kMaxHierarchyDepth` and a visited set.
std::vector<std::size_t> AncestorChain(const std::vector<CachedTicket>& tickets, std::size_t index);

} // namespace ParentHierarchyPure

#endif // SMATCHET_TRACKER_PARENT_HIERARCHY_PURE_H

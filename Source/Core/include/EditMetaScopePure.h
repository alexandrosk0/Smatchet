#pragma once

// EditMetaScopePure — the (project, issue type) scope the saved edit permissions live under, and the rule for
// which fetched permissions may stand for a whole scope (Quality Pillar 6). Pure: no I/O, no Logger.h.
//
// A tracker answers "which fields can be edited" per issue. One issue can differ from its siblings (a Done
// issue lists nothing editable), so a result only stands for its project and issue type when it is
// *representative*: at least one field is editable. A non-representative result stays with its own issue.
// A scope is one (project, issue type) pair within one backend namespace.

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace smatchet {
namespace editmeta {

/// A saved scope row older than this is not restored: permissions drift (workflow and screen changes), so a
/// months-old copy must not keep answering CanEdit* offline.
constexpr std::int64_t kSavedEditPermissionsMaxAgeSec = 30LL * 24 * 60 * 60;
/// The startup warm tries at most this many tickets of one scope before giving up on a representative result.
constexpr std::size_t kMaxCandidatesPerScope = 3;
/// Fetches a warm may spend beyond the first candidate of each scope.
constexpr std::size_t kMaxExtraCandidateFetchesPerWarm = 8;
/// Scopes one warm covers; the rest wait for the next warm.
constexpr std::size_t kMaxScopesPerWarm = 64;
/// A scope whose candidates gave no representative result waits this long (seconds) before a retry.
constexpr int kNoRepresentativeBackoffSec = 1800;

/// The project an issue id belongs to ("PROJ-12" -> "PROJ"), or "" when the id carries none (a local draft,
/// a UUID, a GitHub "owner/repo#3" id).
std::string ProjectOfIssue(const std::string& issueId);

/// `lookup_cache` row key of a scope, "" when `issueTypeKey` is empty. `project` may be empty (a backend whose
/// permissions do not depend on the issue, so one scope per issue type serves every project).
std::string BuildScopeStoreKey(const std::string& project, const std::string& issueTypeKey);

/// Inverse of BuildScopeStoreKey. False for a malformed key (no separator, trailing escape, empty type).
bool SplitScopeStoreKey(const std::string& storeKey, std::string& project, std::string& issueTypeKey);

/// True when at least one field is editable: the result may stand for its whole scope.
bool IsRepresentativeEditPermissions(const std::unordered_map<std::string, bool>& fieldCanEdit);

} // namespace editmeta
} // namespace smatchet

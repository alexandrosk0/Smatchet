#pragma once

// EditMetaScopePure — the (project, issue type) scope the saved edit permissions live under, and the rule for
// which fetched permissions may stand for a whole scope (Quality Pillar 6). Pure and header-only: no I/O, no Logger.h.
//
// A tracker answers "which fields can be edited" per issue. One issue can differ from its siblings (a Done
// issue lists nothing editable), so a result only stands for its project and issue type when it is
// *representative*: at least one field is editable. A non-representative result stays with its own issue.
// A scope is one (project, issue type) pair within one backend namespace.

#include "LearnedWorkflowPure.h"
#include "Tracker/ProjectResolver.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>

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
inline std::string ProjectOfIssue(const std::string& issueId) { return ExtractIssueKeyPrefix(issueId); }

/// `lookup_cache` row key of a scope, "" when `issueTypeKey` is empty. `project` may be empty (an id that carries
/// none, on a backend whose answer does not vary per issue: one scope per issue type).
inline std::string BuildScopeStoreKey(const std::string& project, const std::string& issueTypeKey) {
    if (issueTypeKey.empty()) {
        return std::string();
    }
    return workflow::BuildScopedKey(project, issueTypeKey);
}

/// Inverse of BuildScopeStoreKey. False for a malformed key (no separator, trailing escape, empty type).
inline bool SplitScopeStoreKey(const std::string& storeKey, std::string& project, std::string& issueTypeKey) {
    std::string parts[2];
    std::size_t part = 0;
    for (std::size_t i = 0; i < storeKey.size(); ++i) {
        const char c = storeKey[i];
        if (c == '\\') {
            if (i + 1 >= storeKey.size()) {
                return false; // a trailing escape: not a key BuildScopeStoreKey wrote
            }
            parts[part] += storeKey[++i];
        } else if (c == '|') {
            if (part == 1) {
                return false; // a third part
            }
            part = 1;
        } else {
            parts[part] += c;
        }
    }
    if (part != 1 || parts[1].empty()) {
        return false;
    }
    project = std::move(parts[0]);
    issueTypeKey = std::move(parts[1]);
    return true;
}

/// True when at least one field is editable: the result may stand for its whole scope.
inline bool IsRepresentativeEditPermissions(const std::unordered_map<std::string, bool>& fieldCanEdit) {
    for (const auto& entry : fieldCanEdit) {
        if (entry.second) {
            return true;
        }
    }
    return false;
}

} // namespace editmeta
} // namespace smatchet

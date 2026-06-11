#ifndef SMATCHET_VCS_GITHUB_COMMITS_H
#define SMATCHET_VCS_GITHUB_COMMITS_H

#include "ConfigManager.h"
#include "Vcs/GitHubCommitsParse.h"
#include "Vcs/VcsSubmission.h"

#include <string>
#include <vector>

/**
 * GitHub commits-by-user VCS source for the User Info window's feed
 * (docs/plans/active/user-info-window.md, Slice 2). VCS-section plumbing, NOT
 * the tracker backend — auth reuses the tracker config's GitHubPat/GitHubBaseUrl
 * (persisted regardless of active TrackerType), so the git section works under
 * Jira/Plane panes too.
 */
namespace Vcs {

// ParseGitHubCommitListJson lives in Vcs/GitHubCommitsParse.h (re-exported via the
// include above) — the pure parse is a separate TU so the doctest rig links it
// without this file's cpr/HTTP fetch surface.

/**
 * Fetch the user's recent commits across every `owner/repo` in
 * `cfg.GitCommitRepos` (comma-separated) via the GitHub REST commits endpoint
 * (`?author=<email-or-login>`), merged newest-first and truncated to `maxN`.
 * Returns false only when the git source is unusable (empty PAT — the caller
 * hides the section + shows a status warning; empty repo list; empty author).
 * Per-repo HTTP failures are non-fatal: appended to `outError`, remaining
 * repos still fetched, partial rows kept (return stays true).
 * Blocking HTTP — call from a worker thread only (Pillar 2).
 */
bool GitHubCommitsForUser(const TrackerConfig& cfg, const std::string& authorEmailOrLogin, int maxN,
                          std::vector<VcsSubmission>& outCommits, std::string& outError);

} // namespace Vcs

#endif

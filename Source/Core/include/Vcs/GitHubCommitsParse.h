#ifndef SMATCHET_VCS_GITHUB_COMMITS_PARSE_H
#define SMATCHET_VCS_GITHUB_COMMITS_PARSE_H

#include "Vcs/VcsSubmission.h"

#include <string>
#include <vector>

/**
 * Pure-logic half of the GitHub commits-by-user VCS source (user-info-window
 * Slice 2). Split out of Vcs/GitHubCommits.h so the doctest rig links commit
 * JSON parsing WITHOUT dragging the cpr/HTTP surface (GitHubCommits.cpp) into
 * tests — tests must stay network-free. No cpr, no ConfigManager, no I/O.
 */
namespace Vcs {

/**
 * Pure parse of one `GET /repos/{owner}/{repo}/commits` JSON array body into
 * submission rows (order preserved — GitHub returns newest-first). Per row:
 * Id = sha, Url = html_url, Timestamp = commit.author.date (ISO-8601, leading
 * 10 chars YYYY-MM-DD), Author = commit.author.name falling back to
 * author.login, FirstLine = first line of commit.message. Rows without a sha
 * and malformed entries are skipped; an invalid / non-array body yields an
 * empty vector. No network — bucket-A test surface
 * (tests/Core/VcsSubmission.test.cpp).
 */
std::vector<VcsSubmission> ParseGitHubCommitListJson(const std::string& body);

} // namespace Vcs

#endif

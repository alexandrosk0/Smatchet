#pragma once

// The one GitHub REST header recipe, shared by every TU that talks to
// api.github.com (Tracker's GitHubClient family, Diagnostics' bug-report
// uploader, Vcs' commit feed). Bearer PAT (fine-grained or classic both
// work), JSON-only response, pinned API version (2022-11-28) so the server
// can't silently flip the response shape. Callers pass their own User-Agent
// so server-side logs can still tell the subsystems apart — that per-caller
// string is the only thing the retired per-TU mirror copies ever varied.

#include <cpr/cpr.h>

#include <string>

namespace smatchet {
namespace github {

inline cpr::Header GitHubRestHeaders(const std::string& pat, const char* userAgent) {
    return cpr::Header{
        {"Authorization", std::string("Bearer ") + pat},
        {"Accept", "application/vnd.github+json"},
        {"X-GitHub-Api-Version", "2022-11-28"},
        {"User-Agent", userAgent},
    };
}

} // namespace github
} // namespace smatchet

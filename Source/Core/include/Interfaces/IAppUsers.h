#ifndef SMATCHET_INTERFACES_IAPP_USERS_H
#define SMATCHET_INTERFACES_IAPP_USERS_H

// Narrow facet for tracker-user queries (search / watchers / voters) — AppController
// fan-in Phase 5 (docs/plans/appcontroller-fan-in-phase5-facets.md). AppController
// implements it; the users.* command TU depends on this instead of the full
// AppController.h. None of these are per-frame (each is a network round-trip).
// Rank-0 leaf (Interfaces/): the payload types TrackerUser / TrackerIssueVotes live in
// the rank-3 Tracker headers, which a rank-0 header can't include without a low->high
// back-edge, so they are forward-declared. Each read returns Result<T> BY VALUE — a
// function declaration does not require its return type (nor the vector element) to be
// complete, so naming Result<std::vector<TrackerUser>> / Result<TrackerIssueVotes> here
// does not instantiate them; the concrete override TU (AppController.cpp) and every call
// site include the full types.

#include "SmatchetResult.h"

#include <string>
#include <vector>

struct TrackerUser;
struct TrackerIssueVotes;

class IAppUsers {
  public:
    virtual ~IAppUsers() = default;

    // Read-side #21: has_value() → payload; error() carries the user-facing message. Pure
    // reads — no read-only gate; the backend/capability guards live in each override.
    virtual Result<std::vector<TrackerUser>> FetchIssueWatchers(const std::string& issueKey) const = 0;
    virtual Result<TrackerIssueVotes> FetchIssueVotes(const std::string& issueKey) const = 0;
    virtual Result<std::vector<TrackerUser>> SearchUsersByQuery(const std::string& query) const = 0;
    virtual Result<std::vector<TrackerUser>>
    FetchUsersByAccountIds(const std::vector<std::string>& accountIds) const = 0;
};

#endif // SMATCHET_INTERFACES_IAPP_USERS_H

#ifndef SMATCHET_INTERFACES_IAPP_USERS_H
#define SMATCHET_INTERFACES_IAPP_USERS_H

// Narrow facet for tracker-user queries (search / watchers / voters) — AppController
// fan-in Phase 5 (docs/plans/appcontroller-fan-in-phase5-facets.md). AppController
// implements it; the users.* command TU depends on this instead of the full
// AppController.h. None of these are per-frame (each is a network round-trip).
// Rank-0 leaf (Interfaces/): the collection element TrackerUser lives in the rank-3
// Tracker/TrackerFieldSchema.h, which a rank-0 header can't include without a low->high
// back-edge, so it is forward-declared. The out-parameter is a reference to
// std::vector<TrackerUser>, which does not require TrackerUser to be complete at the
// declaration point; the users.* TU includes the full type.

#include <string>
#include <vector>

struct TrackerUser;

class IAppUsers {
  public:
    virtual ~IAppUsers() = default;

    virtual bool FetchIssueWatchers(const std::string& issueKey, std::vector<TrackerUser>& outWatchers,
                                    std::string& outError) const = 0;
    virtual bool FetchIssueVotes(const std::string& issueKey, std::vector<TrackerUser>& outVoters,
                                 std::string& outError, int* outVoteCount = nullptr, bool* outHasVoted = nullptr,
                                 bool* outVotersInResponse = nullptr) const = 0;
    virtual bool SearchUsersByQuery(const std::string& query, std::vector<TrackerUser>& outUsers,
                                    std::string& outError) const = 0;
};

#endif // SMATCHET_INTERFACES_IAPP_USERS_H

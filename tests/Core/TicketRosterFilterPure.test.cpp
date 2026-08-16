// TicketRosterFilterPure.test.cpp — multi-pane cache scoping (pane-stale-delete-collision plan).
//
// Unit-tests the pure keep-set roster filter shared by TicketSyncService's session convergence and
// AppController's namespace-wide local-data refresh: rows outside the keep set are dropped, the
// survivors keep their relative order, an empty keep set clears the roster, and a keep set that
// already covers everything reports zero removals (the callers use that to skip a republish).

#include "Sync/TicketRosterFilterPure.h"

#include "doctest/doctest.h"

using smatchet::RetainTicketsInKeepSet;

namespace {
std::vector<CachedTicket> Roster(const std::vector<std::string>& ids) {
    std::vector<CachedTicket> rows;
    rows.reserve(ids.size());
    for (std::size_t i = 0; i < ids.size(); ++i) {
        CachedTicket t;
        t.id = ids[i];
        rows.push_back(t);
    }
    return rows;
}

std::vector<std::string> Ids(const std::vector<CachedTicket>& rows) {
    std::vector<std::string> ids;
    ids.reserve(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        ids.push_back(rows[i].id);
    }
    return ids;
}
} // namespace

TEST_CASE("RetainTicketsInKeepSet: drops sibling rows and preserves survivor order") {
    std::vector<CachedTicket> rows = Roster({"A-1", "B-9", "A-2", "B-7", "A-3"});
    const std::unordered_set<std::string> keep = {"A-1", "A-2", "A-3"};
    CHECK(RetainTicketsInKeepSet(rows, keep) == 2);
    CHECK(Ids(rows) == std::vector<std::string>({"A-1", "A-2", "A-3"}));
}

TEST_CASE("RetainTicketsInKeepSet: keep set covering everything removes nothing") {
    std::vector<CachedTicket> rows = Roster({"A-1", "A-2"});
    const std::unordered_set<std::string> keep = {"A-1", "A-2", "A-3"};
    CHECK(RetainTicketsInKeepSet(rows, keep) == 0);
    CHECK(Ids(rows) == std::vector<std::string>({"A-1", "A-2"}));
}

TEST_CASE("RetainTicketsInKeepSet: empty keep set clears the roster") {
    std::vector<CachedTicket> rows = Roster({"A-1", "A-2"});
    CHECK(RetainTicketsInKeepSet(rows, std::unordered_set<std::string>()) == 2);
    CHECK(rows.empty());
}

TEST_CASE("RetainTicketsInKeepSet: empty roster is a no-op") {
    std::vector<CachedTicket> rows;
    const std::unordered_set<std::string> keep = {"A-1"};
    CHECK(RetainTicketsInKeepSet(rows, keep) == 0);
    CHECK(rows.empty());
}

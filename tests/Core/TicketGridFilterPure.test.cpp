// Bucket-A doctest for the pure grid text-filter predicate extracted from
// SmatchetActiveProjectGridTable.cpp (TicketGridFilterPure.{h,cpp}). No ImGui / catalog
// class — plain CachedTicket in, bool out.

#include "TicketGridFilterPure.h"

#include <doctest/doctest.h>

#include <string>
#include <unordered_map>

namespace {

CachedTicket Ticket(const std::string& id, const std::unordered_map<std::string, std::string>& fields) {
    CachedTicket t;
    t.id = id;
    t.fieldValues = fields;
    return t;
}

} // namespace

TEST_CASE("TicketMatchesGridFilter — empty filter always matches") {
    CHECK(TicketMatchesGridFilter(Ticket("PROJ-1", {}), ""));
}

TEST_CASE("TicketMatchesGridFilter — matches the ticket id case-insensitively") {
    const CachedTicket t = Ticket("PROJ-42", {});
    CHECK(TicketMatchesGridFilter(t, "proj-42"));
    CHECK_FALSE(TicketMatchesGridFilter(t, "proj-43"));
}

TEST_CASE("TicketMatchesGridFilter — matches the summary field") {
    const CachedTicket t = Ticket("PROJ-1", {{"summary", "Fix the login page"}});
    CHECK(TicketMatchesGridFilter(t, "login"));
    CHECK_FALSE(TicketMatchesGridFilter(t, "logout"));
}

TEST_CASE("TicketMatchesGridFilter — matches a user field by display name") {
    const CachedTicket t =
        Ticket("PROJ-1", {{"summary", "Unrelated summary"}, {"assignee", "Alexandros Konstantinos"}});
    CHECK(TicketMatchesGridFilter(t, "Alexandros"));
    CHECK(TicketMatchesGridFilter(t, "konstantinos"));
    CHECK_FALSE(TicketMatchesGridFilter(t, "Someone Else"));
}

TEST_CASE("TicketMatchesGridFilter — full text: matches any field, not only summary/user fields") {
    const CachedTicket t = Ticket("PROJ-1", {{"summary", "Unrelated"},
                                             {"labels", "backend-migration"},
                                             {"priority", "High"},
                                             {"description", "Regression introduced in sprint 12"},
                                             {"customfield_1001", "Alexandros K."}});
    CHECK(TicketMatchesGridFilter(t, "migration"));
    CHECK(TicketMatchesGridFilter(t, "high"));
    CHECK(TicketMatchesGridFilter(t, "sprint 12"));
    CHECK(TicketMatchesGridFilter(t, "Alexandros"));
    CHECK_FALSE(TicketMatchesGridFilter(t, "nowhere"));
}

TEST_CASE("TicketMatchesGridFilter — a stored display name matches, and its account id does not") {
    // The tracker field-value parser flattens user objects to their display name (never the
    // raw account id) before they reach CachedTicket, so the filter sees the username. This
    // pins that a ticket carrying only the name does not match a lookup by the underlying id.
    const CachedTicket t = Ticket("PROJ-1", {{"reporter", "Alexandros Konstantinos"}});
    CHECK(TicketMatchesGridFilter(t, "Alexandros"));
    CHECK_FALSE(TicketMatchesGridFilter(t, "5b10a2844c20165700ede21g"));
}

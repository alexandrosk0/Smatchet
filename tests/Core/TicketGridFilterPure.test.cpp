// Bucket-A doctest for the pure grid text-filter predicate extracted from
// SmatchetActiveProjectGridTable.cpp (TicketGridFilterPure.{h,cpp}). No ImGui / catalog
// class — plain CachedTicket + a field-id -> TrackerField* lookup callback.

#include "TicketGridFilterPure.h"

#include "TrackerFieldSchema.h"

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

TrackerField UserField(const std::string& id) {
    TrackerField f;
    f.Id = id;
    f.Family = TrackerFieldFamily::UserSingle;
    f.IsUserType = true;
    return f;
}

TrackerField TextField(const std::string& id) {
    TrackerField f;
    f.Id = id;
    f.Family = TrackerFieldFamily::Text;
    return f;
}

} // namespace

TEST_CASE("TicketMatchesGridFilter — empty filter always matches") {
    const CachedTicket t = Ticket("PROJ-1", {});
    CHECK(TicketMatchesGridFilter(t, "", [](const std::string&) { return nullptr; }));
}

TEST_CASE("TicketMatchesGridFilter — matches the ticket id case-insensitively") {
    const CachedTicket t = Ticket("PROJ-42", {});
    CHECK(TicketMatchesGridFilter(t, "proj-42", [](const std::string&) { return nullptr; }));
    CHECK_FALSE(TicketMatchesGridFilter(t, "proj-43", [](const std::string&) { return nullptr; }));
}

TEST_CASE("TicketMatchesGridFilter — matches the summary field") {
    const CachedTicket t = Ticket("PROJ-1", {{"summary", "Fix the login page"}});
    CHECK(TicketMatchesGridFilter(t, "login", [](const std::string&) { return nullptr; }));
    CHECK_FALSE(TicketMatchesGridFilter(t, "logout", [](const std::string&) { return nullptr; }));
}

TEST_CASE("TicketMatchesGridFilter — matches a user-type field (e.g. assignee)") {
    const CachedTicket t =
        Ticket("PROJ-1", {{"summary", "Unrelated summary"}, {"assignee", "Alexandros Konstantinos"}});
    const TrackerField assignee = UserField("assignee");
    auto lookup = [&](const std::string& id) -> const TrackerField* { return id == "assignee" ? &assignee : nullptr; };
    CHECK(TicketMatchesGridFilter(t, "Alexandros", lookup));
    CHECK(TicketMatchesGridFilter(t, "konstantinos", lookup));
    CHECK_FALSE(TicketMatchesGridFilter(t, "Someone Else", lookup));
}

TEST_CASE("TicketMatchesGridFilter — matches any user field, not just a hardcoded assignee/reporter set") {
    // A backend-specific custom user field (e.g. a Jira "Approver" custom field) must also
    // match — the predicate identifies user fields via the catalog, not a fixed id list.
    const CachedTicket t = Ticket("PROJ-1", {{"customfield_1001", "Alexandros K."}});
    const TrackerField approver = UserField("customfield_1001");
    auto lookup = [&](const std::string& id) -> const TrackerField* {
        return id == "customfield_1001" ? &approver : nullptr;
    };
    CHECK(TicketMatchesGridFilter(t, "Alexandros", lookup));
}

TEST_CASE("TicketMatchesGridFilter — does not match a non-user field containing the same text") {
    // A plain text field containing a name-like string must not be treated as a user match
    // beyond the normal substring rule already covered by summary — this only asserts that
    // classifying a field as non-user doesn't spuriously widen the match via that field.
    const CachedTicket t = Ticket("PROJ-1", {{"labels", "alexandros-project"}});
    const TrackerField labels = TextField("labels");
    auto lookup = [&](const std::string& id) -> const TrackerField* { return id == "labels" ? &labels : nullptr; };
    // Still matches, but via the plain substring scan over fieldValues would only apply to
    // user fields — a non-user field is not scanned, so this must be false.
    CHECK_FALSE(TicketMatchesGridFilter(t, "alexandros", lookup));
}

TEST_CASE("TicketMatchesGridFilter — unknown field id (no catalog metadata) never matches via the user-field path") {
    const CachedTicket t = Ticket("PROJ-1", {{"mystery_field", "Alexandros"}});
    CHECK_FALSE(TicketMatchesGridFilter(t, "Alexandros", [](const std::string&) { return nullptr; }));
}

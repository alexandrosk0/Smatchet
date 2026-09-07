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

// Shape of a Jira attachment as the mapper stores it: the compact dump of the raw array,
// which the grid's attachment cell parses itself. It carries the author's account id,
// email and display name — none of which the grid ever renders as text.
const char* const kAttachmentDump =
    "[{\"self\":\"https://example.atlassian.net/rest/api/3/attachment/10001\",\"id\":\"10001\","
    "\"filename\":\"screenshot.png\",\"author\":{\"accountId\":\"5b10a2844c20165700ede21g\","
    "\"emailAddress\":\"alex@example.com\",\"displayName\":\"Alexandros Konstantinos\"},"
    "\"mimeType\":\"image/png\"}]";

} // namespace

TEST_CASE("TicketMatchesGridFilter — empty filter always matches") {
    CHECK(TicketMatchesGridFilter(Ticket("PROJ-1", {}), ""));
}

TEST_CASE("TicketMatchesGridFilter — matches the ticket id case-insensitively") {
    const CachedTicket t = Ticket("PROJ-42", {});
    CHECK(TicketMatchesGridFilter(t, "proj-42"));
    CHECK(TicketMatchesGridFilter(t, "OJ-4"));
    CHECK_FALSE(TicketMatchesGridFilter(t, "proj-43"));
}

TEST_CASE("TicketMatchesGridFilter — matches the summary field") {
    const CachedTicket t = Ticket("PROJ-1", {{"summary", "Fix the login page"}});
    CHECK(TicketMatchesGridFilter(t, "login"));
    CHECK(TicketMatchesGridFilter(t, "LOGIN PAGE"));
    CHECK_FALSE(TicketMatchesGridFilter(t, "logout"));
}

TEST_CASE("TicketMatchesGridFilter — matches a user field by display name") {
    const CachedTicket t =
        Ticket("PROJ-1", {{"summary", "Unrelated summary"}, {"assignee", "Alexandros Konstantinos"}});
    CHECK(TicketMatchesGridFilter(t, "Alexandros"));
    CHECK(TicketMatchesGridFilter(t, "konstantinos"));
    CHECK_FALSE(TicketMatchesGridFilter(t, "Someone Else"));
}

TEST_CASE("TicketMatchesGridFilter — full text: matches any displayable field") {
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

TEST_CASE("TicketMatchesGridFilter — a raw attachment payload is never searchable") {
    // Only the attachment blob is present, so any match here would be on hidden JSON: the
    // author's account id, email, display name, the mime type, or a JSON key.
    const CachedTicket t = Ticket("PROJ-1", {{"attachment", kAttachmentDump}});
    CHECK_FALSE(TicketMatchesGridFilter(t, "5b10a2844c20165700ede21g"));
    CHECK_FALSE(TicketMatchesGridFilter(t, "example.com"));
    CHECK_FALSE(TicketMatchesGridFilter(t, "Alexandros"));
    CHECK_FALSE(TicketMatchesGridFilter(t, "png"));
    CHECK_FALSE(TicketMatchesGridFilter(t, "self"));
}

TEST_CASE("TicketMatchesGridFilter — a user field matches by name while the same ticket's payload ids stay hidden") {
    const CachedTicket t = Ticket("PROJ-1", {{"reporter", "Alexandros Konstantinos"}, {"attachment", kAttachmentDump}});
    CHECK(TicketMatchesGridFilter(t, "Alexandros"));
    CHECK_FALSE(TicketMatchesGridFilter(t, "5b10a2844c20165700ede21g"));
    CHECK_FALSE(TicketMatchesGridFilter(t, "alex@example.com"));
}

TEST_CASE("TicketMatchesGridFilter — unrecognized-object fallback dumps are skipped, bracketed prose is not") {
    const CachedTicket t = Ticket("PROJ-1", {{"customfield_2002", "{\"kind\":\"opaque\",\"token\":\"deadbeef\"}"},
                                             {"summary", "[Bug] crash on start [urgent]"}});
    CHECK_FALSE(TicketMatchesGridFilter(t, "deadbeef"));
    CHECK_FALSE(TicketMatchesGridFilter(t, "opaque"));
    CHECK(TicketMatchesGridFilter(t, "crash"));
    CHECK(TicketMatchesGridFilter(t, "[Bug]"));
}

TEST_CASE("TicketMatchesGridFilter — non-ASCII text matches byte-exact and is not corrupted by the fold") {
    const CachedTicket t = Ticket("PROJ-1", {{"assignee", "Αλέξανδρος Κωνσταντόνης"}});
    CHECK(TicketMatchesGridFilter(t, "Αλέξανδρος"));
    CHECK(TicketMatchesGridFilter(t, "Κωνσταντόνης"));
    CHECK_FALSE(TicketMatchesGridFilter(t, "Γιώργος"));
}

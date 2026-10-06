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

TEST_CASE("TicketMatchesGridFilter — prose that merely opens with an empty bracket pair is still searchable") {
    // Regression: an opener-pair-only signature classified "[]. release notes" as a payload.
    const CachedTicket t = Ticket("PROJ-1", {{"summary", "[]. release notes"},
                                             {"description", "{} placeholder for the sprint report"},
                                             {"labels", "[\"quoted\"] is not a dump"}});
    CHECK(TicketMatchesGridFilter(t, "release notes"));
    CHECK(TicketMatchesGridFilter(t, "sprint report"));
    CHECK(TicketMatchesGridFilter(t, "not a dump"));
    // The genuinely empty dumps carry nothing searchable, so skipping them is harmless.
    const CachedTicket empty = Ticket("PROJ-2", {{"attachment", "[]"}, {"customfield_3", "{}"}});
    CHECK_FALSE(TicketMatchesGridFilter(empty, "]"));
    CHECK_FALSE(TicketMatchesGridFilter(empty, "}"));
}

TEST_CASE("TicketMatchesGridFilter — non-ASCII text matches byte-exact and is not corrupted by the fold") {
    const CachedTicket t = Ticket("PROJ-1", {{"assignee", "Αλέξανδρος Κωνσταντόνης"}});
    CHECK(TicketMatchesGridFilter(t, "Αλέξανδρος"));
    CHECK(TicketMatchesGridFilter(t, "Κωνσταντόνης"));
    CHECK_FALSE(TicketMatchesGridFilter(t, "Γιώργος"));
}

namespace {
// A Plane-shaped display: the State field stores a uuid, the grid shows its option name.
class StateNames final : public IGridFilterDisplay {
  public:
    const std::string& Shown(const std::string& fieldId, const std::string& stored) const override {
        if (fieldId == "status" && stored == uuid_) {
            return name_;
        }
        return stored;
    }

  private:
    std::string uuid_ = "3fa85f64-5717-4562-b3fc-2c963f66afa6";
    std::string name_ = "In Progress";
};
} // namespace

TEST_CASE("TicketMatchesGridFilter — an option stored by id matches by the name the grid shows") {
    const CachedTicket t = Ticket("PLN-1", {{"status", "3fa85f64-5717-4562-b3fc-2c963f66afa6"},
                                            {"uuid", "9b1deb4d-3b7d-4bad-9bdd-2b0d7b3dcb6d"}});
    const StateNames display;
    CHECK(TicketMatchesGridFilter(t, "progress", &display));
    // Neither the stored state uuid nor Plane's internal row uuid is text the user can see.
    CHECK_FALSE(TicketMatchesGridFilter(t, "5717", &display));
    CHECK_FALSE(TicketMatchesGridFilter(t, "3b7d", &display));
}

TEST_CASE("TicketMatchesGridFilter — internal sentinel keys never match") {
    const CachedTicket t = Ticket("GH-7", {{"_smatchet_is_pr", "1"}, {"summary", "Fix the build"}});
    CHECK_FALSE(TicketMatchesGridFilter(t, "1"));
    CHECK(TicketMatchesGridFilter(t, "build"));
    CHECK(IsInternalTicketFieldKey("uuid"));
    CHECK(IsInternalTicketFieldKey("_smatchet_is_pr"));
    CHECK_FALSE(IsInternalTicketFieldKey("summary"));
}

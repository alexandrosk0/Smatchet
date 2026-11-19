// Bucket-A doctest for the pure grid text-filter predicate extracted from
// SmatchetActiveProjectGridTable.cpp (TicketGridFilterPure.{h,cpp}). No ImGui / catalog
// class — plain CachedTicket in, bool out.

#include "TicketGridFilterPure.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

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

TEST_CASE("TicketMatchesGridFilter — Greek, Cyrillic and accented Latin fold case, not only ASCII") {
    const CachedTicket t =
        Ticket("PROJ-1", {{"assignee", "Αλέξανδρος Κωνσταντόνης"}, {"summary", "Ошибка входа: ÉCRAN figé"}});
    CHECK(TicketMatchesGridFilter(t, "αλέξανδρος"));
    CHECK(TicketMatchesGridFilter(t, "ΚΩΝΣΤΑΝΤΌΝΗΣ")); // capital sigma folds to the final sigma's letter
    CHECK(TicketMatchesGridFilter(t, "ошибка ВХОДА"));
    CHECK(TicketMatchesGridFilter(t, "écran FIGÉ"));
    CHECK_FALSE(TicketMatchesGridFilter(t, "ecran")); // folding is case only, never accents
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

TEST_CASE("TicketMatchesGridFilter — every word must match, in any order") {
    const CachedTicket t = Ticket("PROJ-1", {{"summary", "Login page crashes on submit"}});
    CHECK(TicketMatchesGridFilter(t, "login crash"));
    CHECK(TicketMatchesGridFilter(t, "crash login"));
    CHECK(TicketMatchesGridFilter(t, "SUBMIT Login"));
    // One word that occurs nowhere fails the whole filter.
    CHECK_FALSE(TicketMatchesGridFilter(t, "login signup"));
    CHECK_FALSE(TicketMatchesGridFilter(t, "signup login"));
}

TEST_CASE("TicketMatchesGridFilter — words may match different fields and the id") {
    const CachedTicket t = Ticket("PROJ-7", {{"summary", "Fix login"}, {"assignee", "Alexandros K."}});
    CHECK(TicketMatchesGridFilter(t, "login alexandros"));
    CHECK(TicketMatchesGridFilter(t, "alexandros login"));
    CHECK(TicketMatchesGridFilter(t, "proj-7 login"));
    CHECK(TicketMatchesGridFilter(t, "fix proj alex"));
    CHECK_FALSE(TicketMatchesGridFilter(t, "login proj-8"));
}

TEST_CASE("TicketMatchesGridFilter — runs of whitespace separate words and never match literally") {
    const CachedTicket t = Ticket("PROJ-1", {{"summary", "Login page crashes"}});
    CHECK(TicketMatchesGridFilter(t, "login   crash"));
    CHECK(TicketMatchesGridFilter(t, "login\tcrash"));
    CHECK(TicketMatchesGridFilter(t, "  login \r\n crash  "));
    // A filter of only whitespace has no words, so it filters nothing out.
    CHECK(TicketMatchesGridFilter(t, "   "));
    CHECK(TicketMatchesGridFilter(Ticket("PROJ-2", {}), " \t "));
}

TEST_CASE("TicketMatchesGridFilter — several words still cannot reach hidden payloads or stored ids") {
    const CachedTicket attach = Ticket("PROJ-1", {{"summary", "Upload fails"}, {"attachment", kAttachmentDump}});
    CHECK_FALSE(TicketMatchesGridFilter(attach, "screenshot png"));
    CHECK_FALSE(TicketMatchesGridFilter(attach, "upload alexandros"));
    CHECK(TicketMatchesGridFilter(attach, "upload fails"));

    const CachedTicket plane =
        Ticket("PLN-1", {{"status", "3fa85f64-5717-4562-b3fc-2c963f66afa6"}, {"summary", "Ship the build"}});
    const StateNames display;
    CHECK(TicketMatchesGridFilter(plane, "build progress", &display));
    CHECK_FALSE(TicketMatchesGridFilter(plane, "build 5717", &display));
}

TEST_CASE("TicketMatchesGridFilter — non-ASCII words each match byte-exact") {
    const CachedTicket t = Ticket("PROJ-1", {{"assignee", "Αλέξανδρος Κωνσταντόνης"}});
    CHECK(TicketMatchesGridFilter(t, "Κωνσταντόνης Αλέξανδρος"));
    CHECK_FALSE(TicketMatchesGridFilter(t, "Αλέξανδρος Γιώργος"));
}

TEST_CASE("SplitGridFilterTokens — lowercased words, whitespace dropped") {
    CHECK(SplitGridFilterTokens("").empty());
    CHECK(SplitGridFilterTokens(" \t\r\n ").empty());
    const std::vector<std::string> one = SplitGridFilterTokens("Login");
    REQUIRE(one.size() == 1);
    CHECK(one[0] == "login");
    const std::vector<std::string> three = SplitGridFilterTokens("  PROJ-7\tFix   Login ");
    REQUIRE(three.size() == 3);
    CHECK(three[0] == "proj-7");
    CHECK(three[1] == "fix");
    CHECK(three[2] == "login");
}

TEST_CASE("SplitGridFilterTokens — words are case-folded beyond ASCII") {
    const std::vector<std::string> words = SplitGridFilterTokens("ΑΛΦΑ Ёлка");
    REQUIRE(words.size() == 2);
    CHECK(words[0] == "αλφα");
    CHECK(words[1] == "ёлка");
}

TEST_CASE("TicketTitleMatchesGridFilterFuzzy — letters in order, title only") {
    smatchet::textmatch::Scratch scratch;
    const CachedTicket t = Ticket("PROJ-7", {{"summary", "Login page crashes"}, {"assignee", "Alexandros"}});
    using smatchet::textmatch::MakeQuery;
    CHECK(TicketTitleMatchesGridFilterFuzzy(t, MakeQuery("lgn crsh"), scratch));
    CHECK(TicketTitleMatchesGridFilterFuzzy(t, MakeQuery("CRASHES login"), scratch));
    CHECK(TicketTitleMatchesGridFilterFuzzy(t, MakeQuery(""), scratch));
    CHECK_FALSE(TicketTitleMatchesGridFilterFuzzy(t, MakeQuery("alx"), scratch));   // the assignee is not the title
    CHECK_FALSE(TicketTitleMatchesGridFilterFuzzy(t, MakeQuery("proj7"), scratch)); // nor is the id
    CHECK_FALSE(TicketTitleMatchesGridFilterFuzzy(t, MakeQuery("hsarc"), scratch)); // out of order
    CHECK_FALSE(TicketTitleMatchesGridFilterFuzzy(Ticket("PROJ-8", {}), MakeQuery("a"), scratch)); // no title
    // The strict pass still needs the words as written: the fallback is the grid's second tier.
    CHECK_FALSE(TicketMatchesGridFilter(t, "lgn crsh"));
}

TEST_CASE("SplitGridFilterTokens — words past the cap are ignored, so the match only gets looser") {
    std::string many;
    for (std::size_t i = 0; i < kGridFilterMaxTokens; ++i) {
        many += "login ";
    }
    many += "absent";
    CHECK(SplitGridFilterTokens(many).size() == kGridFilterMaxTokens);
    const CachedTicket t = Ticket("PROJ-1", {{"summary", "Fix the login page"}});
    CHECK(TicketMatchesGridFilter(t, many));
    // The same word inside the cap still has to match.
    CHECK_FALSE(TicketMatchesGridFilter(t, "absent login"));

    // A full set of words, with the one that occurs nowhere in the last slot.
    std::string full;
    for (std::size_t i = 0; i + 1 < kGridFilterMaxTokens; ++i) {
        full += "login ";
    }
    full += "absent";
    REQUIRE(SplitGridFilterTokens(full).size() == kGridFilterMaxTokens);
    CHECK_FALSE(TicketMatchesGridFilter(t, full));

    // The matcher applies the cap itself when handed more words than it tracks.
    std::vector<std::string> overCap(kGridFilterMaxTokens, "login");
    overCap.push_back("absent");
    CHECK(TicketMatchesGridFilterTokens(t, overCap));
    overCap[kGridFilterMaxTokens - 1] = "absent";
    CHECK_FALSE(TicketMatchesGridFilterTokens(t, overCap));
}

TEST_CASE("TicketMatchesGridFilterTokens — no words matches everything; all listed words are required") {
    const CachedTicket t = Ticket("PROJ-1", {{"summary", "Fix the login page"}});
    CHECK(TicketMatchesGridFilterTokens(t, {}));
    CHECK(TicketMatchesGridFilterTokens(t, {"login", "proj"}));
    CHECK_FALSE(TicketMatchesGridFilterTokens(t, {"login", "absent"}));
}

TEST_CASE("TicketMatchesGridFilter — internal sentinel keys never match") {
    const CachedTicket t = Ticket("GH-7", {{"_smatchet_is_pr", "1"}, {"summary", "Fix the build"}});
    CHECK_FALSE(TicketMatchesGridFilter(t, "1"));
    CHECK(TicketMatchesGridFilter(t, "build"));
    CHECK(IsInternalTicketFieldKey("uuid"));
    CHECK(IsInternalTicketFieldKey("_smatchet_is_pr"));
    CHECK_FALSE(IsInternalTicketFieldKey("summary"));
}

// The shared matcher's own cases live in one file and run in this TU (see that file's header).
#include "TextMatchPure.cases.h"

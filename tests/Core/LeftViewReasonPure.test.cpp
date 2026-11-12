// LeftViewReasonPure + ViewListingDiffPure: why an issue left a view, and which issues left. See
// docs/plans/left-view-history-grid.md.

#include "Sync/LeftViewReasonPure.h"
#include "Sync/RemoteVersionPure.h"
#include "Sync/ViewListingDiffPure.h"
#include "Ui/LeftViewHistoryPure.h"

#include <doctest/doctest.h>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace smatchet;

namespace {

TrackerFieldChange Change(const std::string& fieldId, const std::string& from, const std::string& to,
                          const std::string& author = std::string(), const std::string& at = std::string()) {
    TrackerFieldChange change;
    change.issueKey = "PROJ-1";
    change.fieldId = fieldId;
    change.fieldLabel = fieldId;
    change.fromValue = from;
    change.toValue = to;
    change.author = author;
    change.changedAt = at;
    return change;
}

SavedViewListing Listing(const std::string& query, const std::vector<std::string>& ids) {
    SavedViewListing listing;
    listing.known = true;
    listing.query = query;
    listing.ids = ids;
    return listing;
}

} // namespace

TEST_SUITE("LeftViewReasonPure") {

    TEST_CASE("QueryNamesField matches whole words, labels, cf[] and statusCategory") {
        CHECK(QueryNamesField("status != Done", "status", "Status"));
        CHECK(QueryNamesField("STATUS != Done", "status", "Status"));
        CHECK_FALSE(QueryNamesField("statusbar = 1", "status", "Status"));
        CHECK(QueryNamesField("statusCategory != Done", "status", "Status"));
        CHECK(QueryNamesField("cf[10020] = 5", "customfield_10020", "Sprint"));
        CHECK(QueryNamesField("Sprint in openSprints()", "customfield_10020", "Sprint"));
        CHECK(QueryNamesField("\"Story Points\" > 3", "customfield_1", "Story Points"));
        CHECK_FALSE(QueryNamesField("project = X", "assignee", "Assignee"));
    }

    TEST_CASE("a changed field the query names is the reason") {
        const std::vector<TrackerFieldChange> changes{Change("labels", "a", "b", "Bob"),
                                                      Change("status", "In Progress", "Done", "Alice")};
        const LeftViewReason reason = BuildLeftViewReason(changes, "status != Done", {});
        CHECK(reason.kind == LeftViewReasonKind::QueryFieldChanged);
        CHECK(reason.text == "Status: In Progress -> Done");
        CHECK(reason.author == "Alice");
        REQUIRE(reason.changes.size() == 1);
    }

    TEST_CASE("a change to a field the query does not name is only possibly related") {
        const LeftViewReason reason = BuildLeftViewReason({Change("labels", "a", "b", "Bob")}, "project = X", {});
        CHECK(reason.kind == LeftViewReasonKind::OtherFieldChanged);
        CHECK(reason.text.find("Possibly") == 0);
        CHECK(reason.text.find("does not name") != std::string::npos);
        CHECK(reason.author == "Bob");
    }

    TEST_CASE("no change is reported as such, never guessed at") {
        const LeftViewReason reason = BuildLeftViewReason({}, "project = X", {});
        CHECK(reason.kind == LeftViewReasonKind::NoChangeFound);
        CHECK(reason.text == "Left view (cause unknown): no compared field changed.");
    }

    TEST_CASE("an issue the tracker will not return is never called deleted without a 404") {
        CHECK(NotVisibleReason().text == "Deleted or no longer visible");
        CHECK(NotVisibleReason().kind == LeftViewReasonKind::NotVisible);
        CHECK(DeletedReason().kind == LeftViewReasonKind::Deleted);
    }

    TEST_CASE("labels come from the catalog when given, and long lists are capped") {
        const std::unordered_map<std::string, std::string> labels{{"customfield_1", "Sprint"}};
        std::vector<TrackerFieldChange> changes;
        changes.push_back(Change("customfield_1", "S1", "S2"));
        CHECK(BuildLeftViewReason(changes, "Sprint = S1", labels).text == "Sprint: S1 -> S2");
        changes.push_back(Change("status", "A", "B"));
        changes.push_back(Change("assignee", "A", "B"));
        changes.push_back(Change("priority", "A", "B"));
        const std::string text = BuildLeftViewReason(changes, "Sprint status assignee priority", labels).text;
        CHECK(text.find("+1 more") != std::string::npos);
    }

    TEST_CASE("DiffRowFields compares only shared fields and skips bookkeeping") {
        const std::unordered_map<std::string, std::string> before{
            {"status", "Open"}, {"updated", "1"}, {"assignee", "A"}, {"only_before", "x"}};
        const std::unordered_map<std::string, std::string> after{
            {"status", "Done"}, {"updated", "2"}, {"assignee", "A"}, {"only_after", "y"}};
        const std::vector<TrackerFieldChange> changes = DiffRowFields("PROJ-1", before, after);
        REQUIRE(changes.size() == 1);
        CHECK(changes[0].fieldId == "status");
        CHECK(changes[0].fromValue == "Open");
        CHECK(changes[0].toValue == "Done");
    }

    TEST_CASE("ChangesSince compares instants across offsets, newest first") {
        const std::vector<TrackerFieldChange> history{
            Change("status", "A", "B", "", "2026-01-01T10:00:00.000+0000"),
            Change("status", "B", "C", "", "2026-01-02T10:00:00.000+0000"),
            Change("updated", "1", "2", "", "2026-01-03T10:00:00.000+0000"),
        };
        std::int64_t sinceMs = 0;
        // 03:00 at -0800 is 11:00 UTC: after the first entry, though it sorts before it as text.
        REQUIRE(sync::ParseRemoteUpdatedMs("2026-01-01T03:00:00.000-0800", sinceMs));
        const std::vector<TrackerFieldChange> recent = ChangesSince(history, sinceMs);
        REQUIRE(recent.size() == 1);
        CHECK(recent[0].toValue == "C");
        CHECK(ChangesSince(history, 0).size() == 2);
        CHECK(ChangesSince(history, 0).front().toValue == "C");

        // The change the last-known row already shows is not news.
        REQUIRE(sync::ParseRemoteUpdatedMs("2026-01-02T10:00:00.000+0000", sinceMs));
        CHECK(ChangesSince(history, sinceMs).empty());

        // A malformed stamp never drops a real change.
        const std::vector<TrackerFieldChange> odd{Change("status", "A", "B", "", "yesterday")};
        CHECK(ChangesSince(odd, sinceMs).size() == 1);
    }

    TEST_CASE("last-known JSON round-trips, caps, and survives invalid UTF-8") {
        std::unordered_map<std::string, std::string> fields{{"status", "Open"}, {"empty", ""}};
        fields["long"] = std::string(5000, 'x');
        fields["bad"] = std::string("a\xFF"
                                    "b");
        const std::unordered_map<std::string, std::string> back =
            LastKnownFieldsFromJson(LastKnownFieldsToJson(fields));
        CHECK(back.at("status") == "Open");
        CHECK(back.find("empty") == back.end());
        CHECK(back.at("long").size() == 2048);
        CHECK(back.find("bad") != back.end());
        CHECK(LastKnownFieldsFromJson("not json").empty());
        CHECK(LastKnownFieldsToJson(fields, 2048, 10).size() < 100); // total cap
    }
}

TEST_SUITE("LeftViewHistoryPure") {

    TEST_CASE("the filter searches everything a row shows, ignoring case") {
        LeftViewRecord row;
        row.issueKey = "PROJ-1";
        row.summary = "Fix the login";
        row.viewName = "Sprint board";
        row.reason = "Status: Open -> Done";
        row.author = "Alice";
        CHECK(LeftViewRowMatches(row, "Jira \xC2\xB7 acme", ""));
        CHECK(LeftViewRowMatches(row, "", "proj-1"));
        CHECK(LeftViewRowMatches(row, "", "LOGIN"));
        CHECK(LeftViewRowMatches(row, "", "sprint"));
        CHECK(LeftViewRowMatches(row, "", "done"));
        CHECK(LeftViewRowMatches(row, "", "alice"));
        CHECK(LeftViewRowMatches(row, "Jira acme", "acme"));
        CHECK_FALSE(LeftViewRowMatches(row, "", "nope"));
    }

    TEST_CASE("details merge saved and live values under catalog labels") {
        const std::unordered_map<std::string, std::string> saved{{"status", "Open"}, {"customfield_1", "S1"}};
        const std::unordered_map<std::string, std::string> live{{"status", "Done"}, {"assignee", "Bob"}, {"x", ""}};
        const std::unordered_map<std::string, std::string> labels{{"customfield_1", "Sprint"}, {"status", "Status"}};
        const auto fields = MergeDetailFields(saved, live, labels);
        REQUIRE(fields.size() == 3);
        CHECK(fields[0].first == "Sprint");
        CHECK(fields[0].second == "S1");
        CHECK(fields[1].first == "Status");
        CHECK(fields[1].second == "Done");
        CHECK(fields[2].first == "assignee");
    }
}

TEST_SUITE("ViewListingDiffPure") {

    TEST_CASE("an issue the same query no longer returns has left") {
        const std::vector<std::string> left =
            DepartedFromListing(Listing("q", {"A", "B", "C"}), {"A", "C", "D"}, "q", {});
        REQUIRE(left.size() == 1);
        CHECK(left[0] == "B");
    }

    TEST_CASE("a first listing, a changed query or an empty answer records nothing") {
        CHECK(DepartedFromListing(SavedViewListing(), {"A"}, "q", {}).empty());
        CHECK(DepartedFromListing(Listing("old", {"A", "B"}), {"A"}, "new", {}).empty());
        CHECK(DepartedFromListing(Listing("q", {"A", "B"}), {}, "q", {}).empty());
        CHECK_FALSE(DepartedFromListing(Listing(" q\n", {"A", "B"}), {"A"}, "q", {}).empty()); // whitespace only
    }

    TEST_CASE("a row the pane still holds through the hierarchy walk has not left") {
        const std::unordered_set<std::string> held{"B"};
        CHECK(DepartedFromListing(Listing("q", {"A", "B"}), {"A"}, "q", held).empty());
    }

    TEST_CASE("each departure is recorded once across panes") {
        const SavedViewListing listing = Listing("q", {"A", "B"});
        const std::vector<std::string> still = KeysStillListed(listing, {"B", "Z"});
        REQUIRE(still.size() == 1);
        CHECK(still[0] == "B");
        const std::vector<std::string> without = ListingWithout(listing, still);
        REQUIRE(without.size() == 1);
        CHECK(without[0] == "A");
    }

    TEST_CASE("listing JSON round-trips, and malformed input is unknown") {
        const SavedViewListing back = ViewListingFromJson(ViewListingToJson("q", {"A", "B"}), 77);
        CHECK(back.known);
        CHECK(back.query == "q");
        CHECK(back.ids.size() == 2);
        CHECK(back.savedAtUnixSec == 77);
        CHECK_FALSE(ViewListingFromJson("not json", 1).known);
        CHECK_FALSE(ViewListingFromJson("{\"ids\":[]}", 1).known);
    }

    TEST_CASE("a key listed twice departs once") {
        CHECK(DepartedFromListing(Listing("q", {"A", "B", "B"}), {"A"}, "q", {}).size() == 1);
    }
}

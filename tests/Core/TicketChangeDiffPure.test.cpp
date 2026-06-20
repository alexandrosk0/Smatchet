// Bucket-A coverage for TicketChangeDiffPure (ticket-change-monitor plan, S1a) — the
// backend-agnostic salient-field snapshot differ + toast formatter. DiffChangedTickets
// compares two CachedTicket snapshots of the same view and reports salient-field deltas
// on tickets present in BOTH; FormatTicketChangeToast renders the notification body.
// No backend, pane, or toast manager involved.

#include "Sync/TicketChangeDiffPure.h"

#include "doctest/doctest.h"

#include <string>
#include <utility>
#include <vector>

using smatchet::DiffChangedTickets;
using smatchet::FormatTicketChangeToast;
using smatchet::IsSalientChangeField;
using smatchet::SalientFieldRole;
using smatchet::TicketChangeKind;
using smatchet::TicketChangeSummary;

namespace {

CachedTicket MakeTicket(const std::string& id, const std::vector<std::pair<std::string, std::string>>& fields) {
    CachedTicket t;
    t.id = id;
    for (const auto& kv : fields) {
        t.fieldValues[kv.first] = kv.second;
    }
    return t;
}

std::vector<SalientFieldRole> StatusAssigneeRoster() { return {{"status", "Status"}, {"assignee", "Assignee"}}; }

} // namespace

TEST_CASE("IsSalientChangeField: membership against the resolved roster") {
    const std::vector<SalientFieldRole> roster = StatusAssigneeRoster();
    CHECK(IsSalientChangeField("status", roster));
    CHECK(IsSalientChangeField("assignee", roster));
    CHECK_FALSE(IsSalientChangeField("description", roster));
    CHECK_FALSE(IsSalientChangeField("", roster));
    CHECK_FALSE(IsSalientChangeField("status", {}));
}

TEST_CASE("DiffChangedTickets: a single salient field change is reported") {
    const std::vector<CachedTicket> prev = {MakeTicket("A-1", {{"status", "To Do"}})};
    const std::vector<CachedTicket> next = {MakeTicket("A-1", {{"status", "In Progress"}})};
    const std::vector<TicketChangeSummary> out = DiffChangedTickets(prev, next, StatusAssigneeRoster());
    REQUIRE(out.size() == 1);
    CHECK(out[0].kind == TicketChangeKind::Modified);
    CHECK(out[0].issueId == "A-1");
    CHECK(out[0].changedFieldLabel == "Status");
    CHECK(out[0].fromValue == "To Do");
    CHECK(out[0].toValue == "In Progress");
    CHECK(out[0].author.empty()); // snapshot differ never knows the author
}

TEST_CASE("DiffChangedTickets: a non-salient field change is ignored") {
    const std::vector<CachedTicket> prev = {MakeTicket("A-1", {{"description", "old"}})};
    const std::vector<CachedTicket> next = {MakeTicket("A-1", {{"description", "new"}})};
    CHECK(DiffChangedTickets(prev, next, StatusAssigneeRoster()).empty());
}

TEST_CASE("DiffChangedTickets: keys present in only one snapshot are skipped") {
    const std::vector<CachedTicket> prev = {MakeTicket("A-1", {{"status", "To Do"}})};
    const std::vector<CachedTicket> next = {MakeTicket("A-2", {{"status", "Done"}})};
    CHECK(DiffChangedTickets(prev, next, StatusAssigneeRoster()).empty());
}

TEST_CASE("DiffChangedTickets: multiple salient fields on one ticket, roster order") {
    const std::vector<CachedTicket> prev = {MakeTicket("A-1", {{"status", "To Do"}, {"assignee", "alice"}})};
    const std::vector<CachedTicket> next = {MakeTicket("A-1", {{"status", "Done"}, {"assignee", "bob"}})};
    const std::vector<TicketChangeSummary> out = DiffChangedTickets(prev, next, StatusAssigneeRoster());
    REQUIRE(out.size() == 2);
    CHECK(out[0].changedFieldLabel == "Status"); // roster order: status before assignee
    CHECK(out[1].changedFieldLabel == "Assignee");
}

TEST_CASE("DiffChangedTickets: output follows next order across tickets") {
    const std::vector<CachedTicket> prev = {MakeTicket("A-1", {{"status", "To Do"}}),
                                            MakeTicket("A-2", {{"status", "To Do"}})};
    const std::vector<CachedTicket> next = {MakeTicket("A-2", {{"status", "Done"}}),
                                            MakeTicket("A-1", {{"status", "Done"}})};
    const std::vector<TicketChangeSummary> out = DiffChangedTickets(prev, next, StatusAssigneeRoster());
    REQUIRE(out.size() == 2);
    CHECK(out[0].issueId == "A-2"); // next-order: A-2 first
    CHECK(out[1].issueId == "A-1");
}

TEST_CASE("DiffChangedTickets: a missing field reads as empty, so set/clear are detected") {
    const std::vector<CachedTicket> prev = {MakeTicket("A-1", {})};
    const std::vector<CachedTicket> next = {MakeTicket("A-1", {{"status", "In Progress"}})};
    const std::vector<TicketChangeSummary> out = DiffChangedTickets(prev, next, StatusAssigneeRoster());
    REQUIRE(out.size() == 1);
    CHECK(out[0].fromValue.empty());
    CHECK(out[0].toValue == "In Progress");
}

TEST_CASE("FormatTicketChangeToast: empty changes yield an empty string") {
    CHECK(FormatTicketChangeToast({}).empty());
}

TEST_CASE("FormatTicketChangeToast: a modified field reads from -> to") {
    TicketChangeSummary c;
    c.kind = TicketChangeKind::Modified;
    c.issueId = "A-1";
    c.changedFieldLabel = "Status";
    c.fromValue = "To Do";
    c.toValue = "In Progress";
    CHECK(FormatTicketChangeToast({c}) == "A-1 Status: To Do -> In Progress");
}

TEST_CASE("FormatTicketChangeToast: author suffix when known") {
    TicketChangeSummary c;
    c.kind = TicketChangeKind::Modified;
    c.issueId = "A-1";
    c.changedFieldLabel = "Status";
    c.fromValue = "To Do";
    c.toValue = "Done";
    c.author = "alice";
    CHECK(FormatTicketChangeToast({c}) == "A-1 Status: To Do -> Done (by alice)");
}

TEST_CASE("FormatTicketChangeToast: empty from reads 'set to', empty to reads 'cleared'") {
    TicketChangeSummary setc;
    setc.issueId = "A-1";
    setc.changedFieldLabel = "Assignee";
    setc.toValue = "bob";
    CHECK(FormatTicketChangeToast({setc}) == "A-1 Assignee: set to bob");

    TicketChangeSummary clr;
    clr.issueId = "A-1";
    clr.changedFieldLabel = "Assignee";
    clr.fromValue = "bob";
    CHECK(FormatTicketChangeToast({clr}) == "A-1 Assignee: cleared");
}

TEST_CASE("FormatTicketChangeToast: membership kinds render their own phrasing") {
    TicketChangeSummary added;
    added.kind = TicketChangeKind::Added;
    added.issueId = "A-1";
    CHECK(FormatTicketChangeToast({added}) == "A-1 added to view");

    TicketChangeSummary left;
    left.kind = TicketChangeKind::LeftView;
    left.issueId = "A-2";
    CHECK(FormatTicketChangeToast({left}) == "A-2 left view");

    TicketChangeSummary del;
    del.kind = TicketChangeKind::Deleted;
    del.issueId = "A-3";
    CHECK(FormatTicketChangeToast({del}) == "A-3 deleted");
}

TEST_CASE("FormatTicketChangeToast: cap shows N lines then a '+remaining more' line") {
    std::vector<TicketChangeSummary> changes;
    for (int i = 1; i <= 3; ++i) {
        TicketChangeSummary c;
        c.kind = TicketChangeKind::Modified;
        c.issueId = "A-" + std::to_string(i);
        c.changedFieldLabel = "Status";
        c.fromValue = "To Do";
        c.toValue = "Done";
        changes.push_back(c);
    }
    // Default cap = 1: first line + "+2 more".
    CHECK(FormatTicketChangeToast(changes) == "A-1 Status: To Do -> Done\n+2 more");
    // cap = 2: first two lines + "+1 more".
    CHECK(FormatTicketChangeToast(changes, 2) == "A-1 Status: To Do -> Done\nA-2 Status: To Do -> Done\n+1 more");
    // cap >= size: all lines, no "+more".
    CHECK(FormatTicketChangeToast(changes, 5) ==
          "A-1 Status: To Do -> Done\nA-2 Status: To Do -> Done\nA-3 Status: To Do -> Done");
}

TEST_CASE("FormatTicketChangeToast: cap <= 0 is clamped to 1") {
    std::vector<TicketChangeSummary> changes;
    for (int i = 1; i <= 2; ++i) {
        TicketChangeSummary c;
        c.issueId = "A-" + std::to_string(i);
        c.changedFieldLabel = "Status";
        c.fromValue = "x";
        c.toValue = "y";
        changes.push_back(c);
    }
    CHECK(FormatTicketChangeToast(changes, 0) == "A-1 Status: x -> y\n+1 more");
}

// BulkImportStatusPure — the Bulk Import Status column rules (pure). Quality Pillar 6: a row the tracker
// could not be reached for is saved to the offline queue ("queued offline #<id>"); such a row is done and
// must never be sent again by a re-run, or the issue would be created twice.

#include "Ui/BulkImportStatusPure.h"

#include <doctest/doctest.h>

#include <string>

using namespace smatchet::ui::bulkimport;

TEST_CASE("BulkImportStatusPure: waiting rows are not terminal; every outcome is") {
    CHECK_FALSE(IsStatusTerminal(""));
    CHECK_FALSE(IsStatusTerminal(kQueuedStatus));
    CHECK_FALSE(IsStatusTerminal(kWaitingForCacheStatus));
    CHECK_FALSE(IsStatusTerminal(kSubmittingStatus));
    CHECK(IsStatusTerminal("ok PROJ-1"));
    CHECK(IsStatusTerminal("skipped (no changes)"));
    CHECK(IsStatusTerminal(QueuedOfflineStatus(12)));
    CHECK(IsStatusTerminal("Network/unreachable: timed out — retry when Jira is reachable."));
    CHECK(IsStatusTerminal("stopped"));
}

TEST_CASE("BulkImportStatusPure: a re-run resends only failed, stopped and never-run rows") {
    CHECK(QueuedOfflineStatus(12) == "queued offline #12");
    CHECK(IsStatusHandedOff("ok PROJ-1"));
    CHECK(IsStatusHandedOff("skipped (no changes)"));
    CHECK(IsStatusHandedOff(QueuedOfflineStatus(12)));
    CHECK_FALSE(IsStatusHandedOff(kQueuedStatus)); // waiting to be sent, not the offline queue
    CHECK_FALSE(IsStatusHandedOff("stopped"));
    CHECK_FALSE(IsStatusHandedOff("parse error: missing summary"));
    CHECK_FALSE(IsStatusHandedOff("Network/unreachable: timed out — retry when Jira is reachable."));
    CHECK_FALSE(IsStatusHandedOff(""));
}

TEST_CASE("BulkImportStatusPure: reparse preserves offline handoffs and retries changed drafts") {
    const std::vector<std::string> previousDrafts = {"draft A", "draft B", "draft C"};
    const std::vector<std::string> previousStatuses = {QueuedOfflineStatus(12), "ok PROJ-1", "network failure"};
    const auto statuses = PreserveHandedOffStatuses(previousDrafts, previousStatuses,
                                                    {"draft C", "draft B", "draft A", "changed draft A"});
    REQUIRE(statuses.size() == 4);
    CHECK(statuses[0].empty());
    CHECK(statuses[1] == "ok PROJ-1");
    CHECK(statuses[2] == QueuedOfflineStatus(12));
    CHECK(IsStatusHandedOff(statuses[2])); // the next Run must skip this create
    CHECK(statuses[3].empty());
}

TEST_CASE("BulkImportStatusPure: a create with an unknown outcome is never resent by a re-run") {
    const std::string unknown = UnknownOutcomeStatus("Operation timed out");
    CHECK(IsStatusTerminal(unknown));
    CHECK(IsStatusHandedOff(unknown));
    // Kept across an unchanged reparse; an edited row is a new draft and is sent normally.
    const auto statuses = PreserveHandedOffStatuses({"draft A"}, {unknown}, {"draft A", "draft A edited"});
    REQUIRE(statuses.size() == 2);
    CHECK(statuses[0] == unknown);
    CHECK(statuses[1].empty());
}

TEST_CASE("BulkImportStatusPure: reparse consumes each completed identical draft only once") {
    const auto statuses =
        PreserveHandedOffStatuses({"same", "same"}, {QueuedOfflineStatus(12), "failed"}, {"same", "same", "same"});
    REQUIRE(statuses.size() == 3);
    CHECK(statuses[0] == QueuedOfflineStatus(12));
    CHECK(statuses[1].empty());
    CHECK(statuses[2].empty());
}

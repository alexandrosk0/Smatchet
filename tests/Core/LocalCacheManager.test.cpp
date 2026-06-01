// LocalCacheManager unit tests — exercises the SQLite cache path against an in-memory DB.
// Covers SaveTicket transactional behaviour, prepared-statement reuse via stmt(), pending
// create / field-edit queue lifecycles, and dead-letter archive/restore/delete.

#include "../support/SqliteMemFixture.h"

#include "LocalCacheManager.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>

using smatchet_tests::SqliteMemFixture;

namespace {

CachedTicket MakeTicket(const std::string& id) {
    CachedTicket t;
    t.id = id;
    t.fieldValues["summary"] = "summary for " + id;
    t.fieldValues["status"] = "Open";
    return t;
}

} // namespace

TEST_CASE("LocalCacheManager: SaveTicket round-trips field values via TryGetTicket") {
    SqliteMemFixture fix;
    CachedTicket t = MakeTicket("ABC-1");
    t.fieldValues["assignee"] = "alice";
    fix.Ref().SaveTicket(t);

    CachedTicket got;
    REQUIRE(fix.Ref().TryGetTicket("ABC-1", got));
    CHECK(got.id == "ABC-1");
    CHECK(got.fieldValues["summary"] == "summary for ABC-1");
    CHECK(got.fieldValues["status"] == "Open");
    CHECK(got.fieldValues["assignee"] == "alice");
}

TEST_CASE("LocalCacheManager: TryGetTicket returns false for missing id") {
    SqliteMemFixture fix;
    CachedTicket got;
    CHECK_FALSE(fix.Ref().TryGetTicket("does-not-exist", got));
    CHECK(got.id == "does-not-exist"); // function does stamp the queried id onto out
    CHECK(got.fieldValues.empty());
}

TEST_CASE("LocalCacheManager: SaveTicket replaces the full field-value snapshot (delete+insert under one tx)" *
          doctest::test_suite("[high-risk]")) {
    SqliteMemFixture fix;
    CachedTicket initial = MakeTicket("ABC-1");
    initial.fieldValues["priority"] = "High";
    initial.fieldValues["labels"] = "alpha,beta";
    fix.Ref().SaveTicket(initial);

    // Second save with disjoint keys — old keys must vanish, not coexist.
    CachedTicket revised;
    revised.id = "ABC-1";
    revised.fieldValues["summary"] = "revised";
    revised.fieldValues["assignee"] = "bob";

    fix.Ref().SaveTicket(revised);

    CachedTicket got;
    REQUIRE(fix.Ref().TryGetTicket("ABC-1", got));
    CHECK(got.fieldValues.size() == 2);
    CHECK(got.fieldValues.count("priority") == 0);
    CHECK(got.fieldValues.count("labels") == 0);
    CHECK(got.fieldValues["summary"] == "revised");
    CHECK(got.fieldValues["assignee"] == "bob");
}

TEST_CASE("LocalCacheManager: SaveTicket stores rich values in parallel table") {
    SqliteMemFixture fix;
    CachedTicket t = MakeTicket("ABC-1");
    t.fieldRichValues["description"] = "{\"version\":1,\"type\":\"doc\"}";
    fix.Ref().SaveTicket(t);

    CachedTicket got;
    REQUIRE(fix.Ref().TryGetTicket("ABC-1", got));
    CHECK(got.fieldRichValues["description"] == "{\"version\":1,\"type\":\"doc\"}");
}

TEST_CASE("LocalCacheManager: SaveTicket skips empty rich values") {
    SqliteMemFixture fix;
    CachedTicket t = MakeTicket("ABC-1");
    t.fieldRichValues["description"] = ""; // empty rich payload: production code skips it
    t.fieldRichValues["body"] = "{\"v\":1}";
    fix.Ref().SaveTicket(t);

    CachedTicket got;
    REQUIRE(fix.Ref().TryGetTicket("ABC-1", got));
    CHECK(got.fieldRichValues.count("description") == 0); // empty skipped on insert
    CHECK(got.fieldRichValues["body"] == "{\"v\":1}");
}

TEST_CASE("LocalCacheManager: prepared statements survive repeated SaveTicket calls") {
    // Reuses the same cached SQLite::Statement slots (stmt_save_*); regression guard against
    // forgotten reset()/clearBindings() between binds.
    SqliteMemFixture fix;
    for (int i = 0; i < 25; ++i) {
        CachedTicket t = MakeTicket("ABC-" + std::to_string(i));
        t.fieldValues["iter"] = std::to_string(i);
        fix.Ref().SaveTicket(t);
    }
    CHECK(fix.Ref().GetAllTicketIds().size() == 25);
    CachedTicket got;
    REQUIRE(fix.Ref().TryGetTicket("ABC-7", got));
    CHECK(got.fieldValues["iter"] == "7");
}

TEST_CASE("LocalCacheManager: DeleteTicket removes ticket + field rows") {
    SqliteMemFixture fix;
    fix.Ref().SaveTicket(MakeTicket("ABC-1"));
    fix.Ref().SaveTicket(MakeTicket("ABC-2"));

    fix.Ref().DeleteTicket("ABC-1");

    CachedTicket got;
    CHECK_FALSE(fix.Ref().TryGetTicket("ABC-1", got));
    CHECK(fix.Ref().TryGetTicket("ABC-2", got));
}

TEST_CASE("LocalCacheManager: GetAllTickets returns all rows joined with their field values") {
    SqliteMemFixture fix;
    fix.Ref().SaveTicket(MakeTicket("ABC-1"));
    fix.Ref().SaveTicket(MakeTicket("ABC-2"));

    auto all = fix.Ref().GetAllTickets();
    REQUIRE(all.size() == 2);
    std::vector<std::string> ids{all[0].id, all[1].id};
    std::sort(ids.begin(), ids.end());
    CHECK(ids[0] == "ABC-1");
    CHECK(ids[1] == "ABC-2");
    for (const auto& t : all) {
        CHECK(t.fieldValues.count("summary") == 1);
    }
}

TEST_CASE("LocalCacheManager: ForEachTicket streams each ticket once") {
    SqliteMemFixture fix;
    fix.Ref().SaveTicket(MakeTicket("ABC-1"));
    fix.Ref().SaveTicket(MakeTicket("ABC-2"));
    fix.Ref().SaveTicket(MakeTicket("ABC-3"));

    int count = 0;
    std::vector<std::string> seenIds;
    fix.Ref().ForEachTicket([&](CachedTicket&& t) {
        ++count;
        seenIds.push_back(t.id);
        CHECK(t.fieldValues.count("summary") == 1);
    });
    CHECK(count == 3);
    CHECK(seenIds.size() == 3);
}

TEST_CASE("LocalCacheManager: cache_meta flag round-trip") {
    SqliteMemFixture fix;
    CHECK_FALSE(fix.Ref().HasCacheMetaFlag("legacy_sweep_v1"));
    fix.Ref().SetCacheMetaFlag("legacy_sweep_v1");
    CHECK(fix.Ref().HasCacheMetaFlag("legacy_sweep_v1"));
    // Idempotent set is fine — INSERT OR REPLACE used internally.
    fix.Ref().SetCacheMetaFlag("legacy_sweep_v1");
    CHECK(fix.Ref().HasCacheMetaFlag("legacy_sweep_v1"));
}

// --- pending_creates queue lifecycle ---------------------------------------------------------

TEST_CASE("LocalCacheManager: EnqueuePendingCreate + LoadPendingCreates round-trip") {
    SqliteMemFixture fix;
    const std::int64_t id1 = fix.Ref().EnqueuePendingCreate("{\"summary\":\"first\"}");
    const std::int64_t id2 = fix.Ref().EnqueuePendingCreate("{\"summary\":\"second\"}");
    CHECK(id1 > 0);
    CHECK(id2 > id1);

    auto rows = fix.Ref().LoadPendingCreates();
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].Id == id1);
    CHECK(rows[0].Payload == "{\"summary\":\"first\"}");
    CHECK(rows[0].Attempts == 0);
    CHECK(rows[0].LastError.empty());
    CHECK(rows[0].CreatedAtEpochSec > 0);
    CHECK(rows[1].Id == id2);
    CHECK(rows[1].Payload == "{\"summary\":\"second\"}");
}

TEST_CASE("LocalCacheManager: UpdatePendingCreate bumps attempts + last_error") {
    SqliteMemFixture fix;
    const std::int64_t id = fix.Ref().EnqueuePendingCreate("{}");
    fix.Ref().UpdatePendingCreate(id, 3, "HTTP 500");
    auto rows = fix.Ref().LoadPendingCreates();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].Attempts == 3);
    CHECK(rows[0].LastError == "HTTP 500");
}

TEST_CASE("LocalCacheManager: UpdatePendingCreatePayload replaces payload only") {
    SqliteMemFixture fix;
    const std::int64_t id = fix.Ref().EnqueuePendingCreate("{\"old\":1}");
    fix.Ref().UpdatePendingCreate(id, 2, "transient");
    fix.Ref().UpdatePendingCreatePayload(id, "{\"new\":1}");
    auto rows = fix.Ref().LoadPendingCreates();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].Payload == "{\"new\":1}");
    CHECK(rows[0].Attempts == 2);            // unchanged
    CHECK(rows[0].LastError == "transient"); // unchanged
}

TEST_CASE("LocalCacheManager: ArchivePendingCreate moves row to dead-letter with metadata" *
          doctest::test_suite("[high-risk]")) {
    SqliteMemFixture fix;
    const std::int64_t id = fix.Ref().EnqueuePendingCreate("{\"payload\":\"data\"}");
    fix.Ref().UpdatePendingCreate(id, 5, "tracker error");

    fix.Ref().ArchivePendingCreate(id, "max_attempts", "final tracker error");

    CHECK(fix.Ref().LoadPendingCreates().empty());
    auto dead = fix.Ref().LoadDeadPendingCreates();
    REQUIRE(dead.size() == 1);
    CHECK(dead[0].OriginalId == id);
    CHECK(dead[0].Payload == "{\"payload\":\"data\"}");
    CHECK(dead[0].Attempts == 5);
    CHECK(dead[0].LastError == "final tracker error"); // terminalError wins when non-empty
    CHECK(dead[0].TerminalReason == "max_attempts");
    CHECK(dead[0].CreatedAtEpochSec > 0);
    CHECK(dead[0].ArchivedAtEpochSec >= dead[0].CreatedAtEpochSec);
    CHECK(fix.Ref().GetDeadPendingCreateCount() == 1);
}

TEST_CASE("LocalCacheManager: ArchivePendingCreate keeps last_error when terminalError is empty") {
    SqliteMemFixture fix;
    const std::int64_t id = fix.Ref().EnqueuePendingCreate("{\"k\":1}");
    fix.Ref().UpdatePendingCreate(id, 5, "earlier tracker err");
    fix.Ref().ArchivePendingCreate(id, "max_attempts", "");

    auto dead = fix.Ref().LoadDeadPendingCreates();
    REQUIRE(dead.size() == 1);
    CHECK(dead[0].LastError == "earlier tracker err");
}

TEST_CASE("LocalCacheManager: RestoreDeadPendingCreate moves back to active with attempts=0") {
    SqliteMemFixture fix;
    const std::int64_t id = fix.Ref().EnqueuePendingCreate("{\"k\":1}");
    fix.Ref().UpdatePendingCreate(id, 5, "err");
    fix.Ref().ArchivePendingCreate(id, "max_attempts", "");

    REQUIRE(fix.Ref().RestoreDeadPendingCreate(id));

    auto active = fix.Ref().LoadPendingCreates();
    REQUIRE(active.size() == 1);
    CHECK(active[0].Payload == "{\"k\":1}");
    CHECK(active[0].Attempts == 0);
    CHECK(active[0].LastError.empty());
    CHECK(fix.Ref().GetDeadPendingCreateCount() == 0);
}

TEST_CASE("LocalCacheManager: RestoreDeadPendingCreate returns false when no archive matches") {
    SqliteMemFixture fix;
    CHECK_FALSE(fix.Ref().RestoreDeadPendingCreate(9999));
}

TEST_CASE("LocalCacheManager: DeletePendingCreate / DeleteDeadPendingCreate remove rows") {
    SqliteMemFixture fix;
    const std::int64_t id = fix.Ref().EnqueuePendingCreate("{}");
    fix.Ref().DeletePendingCreate(id);
    CHECK(fix.Ref().LoadPendingCreates().empty());

    const std::int64_t id2 = fix.Ref().EnqueuePendingCreate("{}");
    fix.Ref().ArchivePendingCreate(id2, "test", "");
    auto dead = fix.Ref().LoadDeadPendingCreates();
    REQUIRE(dead.size() == 1);
    fix.Ref().DeleteDeadPendingCreate(dead[0].DeadId);
    CHECK(fix.Ref().LoadDeadPendingCreates().empty());
}

TEST_CASE("LocalCacheManager: RunOneTimeLegacyDropPendingAtMaxAttempts is idempotent + flag-gated") {
    SqliteMemFixture fix;
    const std::int64_t under = fix.Ref().EnqueuePendingCreate("{}");
    const std::int64_t atMax = fix.Ref().EnqueuePendingCreate("{}");
    fix.Ref().UpdatePendingCreate(under, OfflineCreateQueue::kMaxReplayAttempts - 1, "");
    fix.Ref().UpdatePendingCreate(atMax, OfflineCreateQueue::kMaxReplayAttempts, "max-attempts");

    const size_t firstDropped = fix.Ref().RunOneTimeLegacyDropPendingAtMaxAttempts();
    CHECK(firstDropped == 1);
    CHECK(fix.Ref().LoadPendingCreates().size() == 1);

    // Subsequent call is a no-op because cache_meta flag is set.
    const std::int64_t atMax2 = fix.Ref().EnqueuePendingCreate("{}");
    fix.Ref().UpdatePendingCreate(atMax2, OfflineCreateQueue::kMaxReplayAttempts, "");
    const size_t secondDropped = fix.Ref().RunOneTimeLegacyDropPendingAtMaxAttempts();
    CHECK(secondDropped == 0);
    CHECK(fix.Ref().LoadPendingCreates().size() == 2);
}

// --- pending_field_edits queue lifecycle -----------------------------------------------------

TEST_CASE("LocalCacheManager: EnqueuePendingFieldEdit + LoadPendingFieldEdits round-trip") {
    SqliteMemFixture fix;
    const std::int64_t id = fix.Ref().EnqueuePendingFieldEdit("ABC-1", "summary", "{\"summary\":\"new\"}", "");
    CHECK(id > 0);
    auto rows = fix.Ref().LoadPendingFieldEdits();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].Id == id);
    CHECK(rows[0].IssueKey == "ABC-1");
    CHECK(rows[0].FieldId == "summary");
    CHECK(rows[0].FieldsPayloadJson == "{\"summary\":\"new\"}");
    CHECK(rows[0].OriginalRichValue.empty());
    CHECK_FALSE(rows[0].HasMergeConflict);
    CHECK(rows[0].ConflictContextJson.empty());
    CHECK(rows[0].Attempts == 0);
}

TEST_CASE("LocalCacheManager: EnqueuePendingFieldEdit stores original rich value") {
    SqliteMemFixture fix;
    fix.Ref().EnqueuePendingFieldEdit("ABC-1", "description", "{\"description\":\"new\"}",
                                      "{\"version\":1,\"type\":\"doc\"}");
    auto rows = fix.Ref().LoadPendingFieldEdits();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].OriginalRichValue == "{\"version\":1,\"type\":\"doc\"}");
}

TEST_CASE("LocalCacheManager: MarkFieldEditConflict + ResolveFieldEditConflict flow" *
          doctest::test_suite("[high-risk]")) {
    SqliteMemFixture fix;
    const std::int64_t id = fix.Ref().EnqueuePendingFieldEdit("ABC-1", "description", "{\"d\":\"mine\"}", "");

    fix.Ref().MarkFieldEditConflict(id, "{\"base\":\"\",\"mine\":\"a\",\"theirs\":\"b\"}");
    auto rows = fix.Ref().LoadPendingFieldEdits();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].HasMergeConflict);
    CHECK(rows[0].ConflictContextJson == "{\"base\":\"\",\"mine\":\"a\",\"theirs\":\"b\"}");

    fix.Ref().ResolveFieldEditConflict(id, "{\"d\":\"resolved\"}");
    rows = fix.Ref().LoadPendingFieldEdits();
    REQUIRE(rows.size() == 1);
    CHECK_FALSE(rows[0].HasMergeConflict);
    CHECK(rows[0].ConflictContextJson.empty());
    CHECK(rows[0].FieldsPayloadJson == "{\"d\":\"resolved\"}");
}

TEST_CASE("LocalCacheManager: ResolveFieldEditConflict clears original_rich_value" *
          doctest::test_suite("[high-risk]")) {
    SqliteMemFixture fix;
    const std::int64_t id =
        fix.Ref().EnqueuePendingFieldEdit("ABC-1", "description", "{\"d\":\"mine\"}", "{\"type\":\"doc\"}");

    fix.Ref().MarkFieldEditConflict(id, "{\"base\":\"b\",\"mine\":\"m\",\"theirs\":\"t\"}");
    fix.Ref().ResolveFieldEditConflict(id, "{\"d\":\"resolved\"}");

    auto rows = fix.Ref().LoadPendingFieldEdits();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].OriginalRichValue.empty());
    CHECK_FALSE(rows[0].HasMergeConflict);
    CHECK(rows[0].FieldsPayloadJson == "{\"d\":\"resolved\"}");
}

TEST_CASE("LocalCacheManager: UpdatePendingFieldEdit bumps attempts + last_error") {
    SqliteMemFixture fix;
    const std::int64_t id = fix.Ref().EnqueuePendingFieldEdit("ABC-1", "summary", "{}", "");
    fix.Ref().UpdatePendingFieldEdit(id, 4, "500 server error");
    auto rows = fix.Ref().LoadPendingFieldEdits();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].Attempts == 4);
    CHECK(rows[0].LastError == "500 server error");
}

TEST_CASE("LocalCacheManager: ArchivePendingFieldEdit moves row to dead-letter") {
    SqliteMemFixture fix;
    const std::int64_t id = fix.Ref().EnqueuePendingFieldEdit("ABC-1", "summary", "{\"summary\":\"x\"}", "rich-orig");
    fix.Ref().UpdatePendingFieldEdit(id, 5, "tracker err");

    fix.Ref().ArchivePendingFieldEdit(id, "max_attempts", "terminal err");

    CHECK(fix.Ref().LoadPendingFieldEdits().empty());
    auto dead = fix.Ref().LoadDeadPendingFieldEdits();
    REQUIRE(dead.size() == 1);
    CHECK(dead[0].OriginalId == id);
    CHECK(dead[0].IssueKey == "ABC-1");
    CHECK(dead[0].FieldId == "summary");
    CHECK(dead[0].FieldsPayloadJson == "{\"summary\":\"x\"}");
    CHECK(dead[0].Attempts == 5);
    CHECK(dead[0].LastError == "terminal err");
    CHECK(dead[0].TerminalReason == "max_attempts");
}

TEST_CASE("LocalCacheManager: DeletePendingFieldEdit removes row by id") {
    SqliteMemFixture fix;
    const std::int64_t id = fix.Ref().EnqueuePendingFieldEdit("ABC-1", "summary", "{}", "");
    fix.Ref().DeletePendingFieldEdit(id);
    CHECK(fix.Ref().LoadPendingFieldEdits().empty());
}

TEST_CASE("LocalCacheManager: DeleteDeadPendingFieldEdit removes archive by dead_id") {
    SqliteMemFixture fix;
    const std::int64_t id = fix.Ref().EnqueuePendingFieldEdit("ABC-1", "summary", "{}", "");
    fix.Ref().ArchivePendingFieldEdit(id, "test", "");
    auto dead = fix.Ref().LoadDeadPendingFieldEdits();
    REQUIRE(dead.size() == 1);
    fix.Ref().DeleteDeadPendingFieldEdit(dead[0].DeadId);
    CHECK(fix.Ref().LoadDeadPendingFieldEdits().empty());
}

// --- hostile-fixture / reopen ----------------------------------------------------------------

TEST_CASE("LocalCacheManager: Reopening on a fresh :memory: is consistent (idempotent init)") {
    SqliteMemFixture fix;
    fix.Ref().SaveTicket(MakeTicket("ABC-1"));
    fix.Reopen();
    // Fresh in-memory DB — no state expected. Sanity: schema init didn't throw + tables exist.
    CHECK(fix.Ref().GetAllTicketIds().empty());
    CHECK(fix.Ref().LoadPendingCreates().empty());
    CHECK(fix.Ref().LoadPendingFieldEdits().empty());
    CHECK(fix.Ref().LoadDeadPendingCreates().empty());
    CHECK(fix.Ref().LoadDeadPendingFieldEdits().empty());
}

// --- Phase 3(a): batched SaveTickets (one transaction) — docs/plans/shipped/memory-budget-and-lifetime-hardening.md ---

TEST_CASE("LocalCacheManager: SaveTickets persists every ticket in the batch") {
    SqliteMemFixture fix;
    std::vector<CachedTicket> batch = {MakeTicket("BATCH-1"), MakeTicket("BATCH-2"), MakeTicket("BATCH-3")};
    fix.Ref().SaveTickets(batch);
    for (const std::string id : {std::string("BATCH-1"), std::string("BATCH-2"), std::string("BATCH-3")}) {
        CachedTicket got;
        REQUIRE(fix.Ref().TryGetTicket(id, got));
        CHECK(got.GetFieldValue("summary") == "summary for " + id);
        CHECK(got.GetFieldValue("status") == "Open");
    }
}

TEST_CASE("LocalCacheManager: SaveTickets is equivalent to per-ticket SaveTicket") {
    SqliteMemFixture viaBatch;
    SqliteMemFixture viaLoop;
    std::vector<CachedTicket> batch = {MakeTicket("EQ-1"), MakeTicket("EQ-2")};
    viaBatch.Ref().SaveTickets(batch);
    for (const auto& t : batch) {
        viaLoop.Ref().SaveTicket(t);
    }
    for (const auto& t : batch) {
        CachedTicket a;
        CachedTicket b;
        REQUIRE(viaBatch.Ref().TryGetTicket(t.id, a));
        REQUIRE(viaLoop.Ref().TryGetTicket(t.id, b));
        CHECK(a.fieldValues == b.fieldValues);
        CHECK(a.fieldRichValues == b.fieldRichValues);
    }
}

TEST_CASE("LocalCacheManager: SaveTickets re-save replaces the field snapshot (prepared-stmt reuse across batch)") {
    SqliteMemFixture fix;
    CachedTicket t = MakeTicket("RS-1");
    t.fieldValues["extra"] = "first";
    fix.Ref().SaveTickets({t});
    // Re-save the same id in a new batch with a different field set — the delete+insert must
    // re-run correctly across the reused (reset) prepared statements.
    CachedTicket revised;
    revised.id = "RS-1";
    revised.fieldValues["summary"] = "revised";
    fix.Ref().SaveTickets({revised, MakeTicket("RS-2")});
    CachedTicket got;
    REQUIRE(fix.Ref().TryGetTicket("RS-1", got));
    CHECK(got.GetFieldValue("summary") == "revised");
    CHECK(got.GetFieldValue("extra").empty()); // old field gone
    CHECK(got.fieldValues.size() == 1);
    CachedTicket got2;
    REQUIRE(fix.Ref().TryGetTicket("RS-2", got2)); // second ticket in the same batch also landed
}

TEST_CASE("LocalCacheManager: SaveTickets on an empty batch is a no-op") {
    SqliteMemFixture fix;
    fix.Ref().SaveTickets({});
    CachedTicket got;
    CHECK_FALSE(fix.Ref().TryGetTicket("anything", got));
}

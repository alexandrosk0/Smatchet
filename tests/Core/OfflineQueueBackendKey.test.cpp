// Pending-queue backend_key + backend-scoped replay — multi-grid Slice 1c (ADR-0018 decision
// 4, docs/plans/active/multi-grid-tabs-slice1-design.md § 3.5 "Pending queues" +
// "Replay-matching rule").
//
// Covers the bucket-A cases the design mandates:
//   * ADD-COLUMN migration idempotence: a PRE-1c fixture DB (queue tables without backend_key,
//     written by a raw SQLite connection) opens cleanly, gains the column, and re-opens
//     cleanly (no duplicate ALTER).
//   * One-time stamp migration correctness: legacy rows take the configured backend's key
//     exactly once (cache_meta flag persists across reopen); an empty key never consumes the
//     flag; rows enqueued after the stamp keep their own key.
//   * Replay matching: with rows queued under two backend keys, context A's replay tick
//     touches only A-rows; B-rows stay queued untouched (never replayed against a wrong
//     backend, never dropped). An empty-key (corrupt) row is treated as no-match.
//   * Dead-letter rows carry the key, and a restore re-queues under the original key.
//
// The migration cases need a FILE-backed DB (raw legacy writer + LocalCacheManager must see
// the same data); `:memory:` is per-connection, so a unique temp file is used per case.

#include "../support/FakeOfflineQueueDeps.h"
#include "../support/OfflineQueueTestEnv.h"
#include "../support/SqliteMemFixture.h"

#include "IssueDraft.h"
#include "LocalCacheManager.h"
#include "OfflineQueueService.h"

#include <SQLiteCpp/SQLiteCpp.h>
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

using smatchet_tests::FakeOfflineQueueDeps;
using smatchet_tests::SqliteMemFixture;
using smatchet_tests::TestEnvGuard;

namespace {

// RAII temp-file DB path: unique per instance, removes the SQLite file family on destruction.
class TempDbFile {
  public:
    TempDbFile() {
        static std::atomic<int> counter{0};
        const char* base = std::getenv("TEMP");
        if (!base) {
            base = std::getenv("TMPDIR");
        }
        const long long stamp = static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count());
        path_ = std::string(base ? base : ".") + "/smatchet_queue_key_mig_" + std::to_string(stamp) + "_" +
                std::to_string(counter.fetch_add(1)) + ".db";
    }
    ~TempDbFile() {
        std::remove(path_.c_str());
        std::remove((path_ + "-wal").c_str());
        std::remove((path_ + "-shm").c_str());
    }
    const std::string& Path() const { return path_; }

  private:
    std::string path_;
};

// Write the PRE-1c queue schema with a raw connection — byte-for-byte what a pre-1c build
// persisted (no backend_key column anywhere).
void CreatePre1cQueueSchema(SQLite::Database& db) {
    db.exec("CREATE TABLE IF NOT EXISTS pending_creates ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, payload TEXT NOT NULL, "
            "attempts INTEGER NOT NULL DEFAULT 0, last_error TEXT, created_at INTEGER NOT NULL)");
    db.exec("CREATE TABLE IF NOT EXISTS pending_creates_dead ("
            "dead_id INTEGER PRIMARY KEY AUTOINCREMENT, original_id INTEGER NOT NULL, "
            "payload TEXT NOT NULL, attempts INTEGER NOT NULL, last_error TEXT, "
            "created_at INTEGER NOT NULL, archived_at INTEGER NOT NULL, terminal_reason TEXT NOT NULL)");
    db.exec("CREATE TABLE IF NOT EXISTS pending_field_edits ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, issue_key TEXT NOT NULL, field_id TEXT NOT NULL, "
            "fields_payload_json TEXT NOT NULL, original_rich_value TEXT, original_value TEXT, "
            "has_original_value INTEGER NOT NULL DEFAULT 0, attempts INTEGER NOT NULL DEFAULT 0, "
            "last_error TEXT, created_at INTEGER NOT NULL)");
    db.exec("CREATE TABLE IF NOT EXISTS pending_field_edits_dead ("
            "dead_id INTEGER PRIMARY KEY AUTOINCREMENT, original_id INTEGER NOT NULL, "
            "issue_key TEXT NOT NULL, field_id TEXT NOT NULL, fields_payload_json TEXT NOT NULL, "
            "original_rich_value TEXT, original_value TEXT, has_original_value INTEGER NOT NULL DEFAULT 0, "
            "attempts INTEGER NOT NULL, last_error TEXT, created_at INTEGER NOT NULL, "
            "archived_at INTEGER NOT NULL, terminal_reason TEXT NOT NULL)");
}

void SeedLegacyQueueRows(SQLite::Database& db) {
    db.exec("INSERT INTO pending_creates (payload, attempts, last_error, created_at) VALUES ('{}', 0, '', 100)");
    db.exec("INSERT INTO pending_creates_dead (original_id, payload, attempts, last_error, created_at, archived_at, "
            "terminal_reason) VALUES (1, '{}', 5, 'err', 100, 200, 'max_attempts')");
    db.exec("INSERT INTO pending_field_edits (issue_key, field_id, fields_payload_json, attempts, last_error, "
            "created_at) VALUES ('ABC-1', 'summary', '{}', 0, '', 100)");
    db.exec("INSERT INTO pending_field_edits_dead (original_id, issue_key, field_id, fields_payload_json, attempts, "
            "last_error, created_at, archived_at, terminal_reason) "
            "VALUES (1, 'ABC-2', 'summary', '{}', 5, 'err', 100, 200, 'max_attempts')");
}

bool TableHasColumn(SQLite::Database& db, const char* table, const char* col) {
    SQLite::Statement q(db, std::string("PRAGMA table_info(") + table + ")");
    while (q.executeStep()) {
        if (std::string(q.getColumn(1).getText()) == col)
            return true;
    }
    return false;
}

IssueDraft MakeDraft(const std::string& summary) {
    IssueDraft d;
    d.ProjectKey = "PROJ";
    d.IssueTypeId = "10001";
    d.IssueTypeName = "Story";
    d.FieldValues["summary"] = summary;
    return d;
}

void PrimeCreateHappy(FakeOfflineQueueDeps& deps) {
    deps.BackendImpl->SetBuildCreatePayloadResult(true, nlohmann::json{{"fields", {{"summary", "X"}}}});
    TrackerField f;
    f.Id = "summary";
    f.Name = "summary";
    f.Type = "string";
    deps.Fields = {f};
}

} // namespace

TEST_CASE("queue backend_key migration: pre-1c DB gains the column on open and re-opens cleanly (idempotent)" *
          doctest::test_suite("[high-risk]")) {
    TempDbFile tmp;
    {
        SQLite::Database raw(tmp.Path(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        CreatePre1cQueueSchema(raw);
        SeedLegacyQueueRows(raw);
        REQUIRE_FALSE(TableHasColumn(raw, "pending_creates", "backend_key"));
    }
    {
        LocalCacheManager mgr(tmp.Path()); // first open: guarded ADD COLUMN fires on all 4 tables
        auto rows = mgr.LoadPendingCreates();
        REQUIRE(rows.size() == 1);
        CHECK(rows.front().BackendKey.empty()); // legacy row: '' until the stamp migration runs
    }
    {
        LocalCacheManager mgr(tmp.Path()); // second open: column exists, ALTER skipped (no throw)
        CHECK(mgr.LoadPendingCreates().size() == 1);
    }
    SQLite::Database raw(tmp.Path(), SQLite::OPEN_READONLY);
    CHECK(TableHasColumn(raw, "pending_creates", "backend_key"));
    CHECK(TableHasColumn(raw, "pending_creates_dead", "backend_key"));
    CHECK(TableHasColumn(raw, "pending_field_edits", "backend_key"));
    CHECK(TableHasColumn(raw, "pending_field_edits_dead", "backend_key"));
}

TEST_CASE("queue backend_key stamp: legacy rows take the configured key once; flag persists across reopen" *
          doctest::test_suite("[high-risk]")) {
    TempDbFile tmp;
    {
        SQLite::Database raw(tmp.Path(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        CreatePre1cQueueSchema(raw);
        SeedLegacyQueueRows(raw);
    }
    {
        LocalCacheManager mgr(tmp.Path());
        // Empty key never consumes the one-time flag (corrupt/unwired-key guard).
        CHECK(mgr.RunOneTimePendingQueueBackendKeyStamp(std::string()) == 0);
        // Real key stamps all 4 tables (1 row each in the fixture).
        CHECK(mgr.RunOneTimePendingQueueBackendKeyStamp("Jira") == 4);
        // Same-process re-run: flag short-circuits.
        CHECK(mgr.RunOneTimePendingQueueBackendKeyStamp("Jira") == 0);

        CHECK(mgr.LoadPendingCreates().front().BackendKey == "Jira");
        CHECK(mgr.LoadDeadPendingCreates().front().BackendKey == "Jira");
        CHECK(mgr.LoadPendingFieldEdits().front().BackendKey == "Jira");
        CHECK(mgr.LoadDeadPendingFieldEdits().front().BackendKey == "Jira");

        // A row enqueued AFTER the stamp keeps its own key.
        (void)mgr.EnqueuePendingCreate("Plane", "{}");
    }
    {
        // Reopen: the flag persists, so a different key never re-stamps (the Plane row would
        // otherwise be untouched anyway — it is non-empty — but the flag must short-circuit).
        LocalCacheManager mgr(tmp.Path());
        CHECK(mgr.RunOneTimePendingQueueBackendKeyStamp("GitHub") == 0);
        const auto rows = mgr.LoadPendingCreates();
        REQUIRE(rows.size() == 2);
        CHECK(rows[0].BackendKey == "Jira");
        CHECK(rows[1].BackendKey == "Plane");
    }
}

TEST_CASE("queue backend_key replay matching: context A's tick replays only A-rows; B-rows stay queued" *
          doctest::test_suite("[high-risk]")) {
    TestEnvGuard guard;
    FakeOfflineQueueDeps deps; // CacheBackendKeyImpl defaults to "Jira" — this context is "A"
    PrimeCreateHappy(deps);
    deps.BackendImpl->EnqueueCreateIssueSuccess("PROJ-1");

    OfflineQueueService svc(deps);
    // A-row through the service (stamped with the context's key)...
    REQUIRE(svc.QueueCreateOffline(MakeDraft("jira row")) > 0);
    CHECK(deps.CacheImpl->LoadPendingCreates().front().BackendKey == "Jira");
    // ...plus a B-row and a corrupt empty-key row, queued directly at the cache layer.
    const std::int64_t planeId =
        deps.CacheImpl->EnqueuePendingCreate("Plane", IssueDraftHelpers::ToJson(MakeDraft("plane row")));
    const std::int64_t emptyId =
        deps.CacheImpl->EnqueuePendingCreate(std::string(), IssueDraftHelpers::ToJson(MakeDraft("corrupt row")));
    REQUIRE(svc.GetPendingCreateCount() == 3u);

    svc.RestartReplayTimersNow(std::chrono::steady_clock::now());
    svc.TickOfflineCreates();

    // Exactly the Jira row replayed; the Plane + empty-key rows stay queued UNTOUCHED
    // (attempts not bumped, never dead-lettered, never replayed against the wrong backend).
    CHECK(deps.BackendImpl->CreateIssueCallCount() == 1u);
    const auto remaining = deps.CacheImpl->LoadPendingCreates();
    REQUIRE(remaining.size() == 2);
    CHECK(remaining[0].Id == planeId);
    CHECK(remaining[0].BackendKey == "Plane");
    CHECK(remaining[0].Attempts == 0);
    CHECK(remaining[1].Id == emptyId);
    CHECK(remaining[1].Attempts == 0);
    CHECK(svc.GetDeadPendingCreateCount() == 0u);
}

TEST_CASE("queue backend_key replay matching: field-edit tick is backend-scoped too" *
          doctest::test_suite("[high-risk]")) {
    TestEnvGuard guard;
    FakeOfflineQueueDeps deps; // context key "Jira"
    deps.BackendImpl->EnqueueUpdateIssueFieldsSuccess();

    OfflineQueueService svc(deps);
    std::string err;
    REQUIRE(svc.QueueFieldEditOffline("PROJ-1", "summary", nlohmann::json{{"summary", "v"}}.dump(), err,
                                      std::string()) > 0);
    REQUIRE(err.empty());
    const std::int64_t planeId =
        deps.CacheImpl->EnqueuePendingFieldEdit("Plane", "OTHER-9", "summary", nlohmann::json{{"summary", "w"}}.dump());
    REQUIRE(svc.GetPendingFieldEdits().size() == 2u);

    svc.RestartReplayTimersNow(std::chrono::steady_clock::now());
    svc.TickOfflineFieldEdits();

    // Only the Jira edit was PUT; the Plane edit stays queued untouched.
    CHECK(deps.BackendImpl->UpdateIssueFieldsCallCount() == 1u);
    const auto remaining = deps.CacheImpl->LoadPendingFieldEdits();
    REQUIRE(remaining.size() == 1);
    CHECK(remaining.front().Id == planeId);
    CHECK(remaining.front().BackendKey == "Plane");
    CHECK(remaining.front().Attempts == 0);
    CHECK(svc.GetDeadPendingFieldEdits().empty());
}

TEST_CASE("queue backend_key: dead-letter rows carry the key and restore re-queues under the original key" *
          doctest::test_suite("[high-risk]")) {
    SqliteMemFixture fix;

    // Create path: archive carries the key; restore brings it back.
    const std::int64_t createId = fix.Ref().EnqueuePendingCreate("Plane", "{\"k\":1}");
    fix.Ref().ArchivePendingCreate(createId, "max_attempts", "terminal");
    auto deadCreates = fix.Ref().LoadDeadPendingCreates();
    REQUIRE(deadCreates.size() == 1);
    CHECK(deadCreates.front().BackendKey == "Plane");
    REQUIRE(fix.Ref().RestoreDeadPendingCreate(createId));
    auto active = fix.Ref().LoadPendingCreates();
    REQUIRE(active.size() == 1);
    CHECK(active.front().BackendKey == "Plane"); // original key, NOT any focused context's key
    CHECK(active.front().Attempts == 0);

    // Field-edit path: archive carries the key; restore brings it back (bases included).
    const std::int64_t editId =
        fix.Ref().EnqueuePendingFieldEdit("GitHub", "OWN/REPO#7", "summary", "{}", "rich base", "scalar base", true);
    fix.Ref().ArchivePendingFieldEdit(editId, "replay_rejected", "terminal");
    auto deadEdits = fix.Ref().LoadDeadPendingFieldEdits();
    REQUIRE(deadEdits.size() == 1);
    CHECK(deadEdits.front().BackendKey == "GitHub");
    REQUIRE(fix.Ref().RestoreDeadPendingFieldEdit(editId));
    CHECK(fix.Ref().LoadDeadPendingFieldEdits().empty());
    const auto activeEdits = fix.Ref().LoadPendingFieldEdits();
    REQUIRE(activeEdits.size() == 1);
    CHECK(activeEdits.front().BackendKey == "GitHub"); // original key, NOT any focused context's key
    CHECK(activeEdits.front().IssueKey == "OWN/REPO#7");
    CHECK(activeEdits.front().FieldId == "summary");
    CHECK(activeEdits.front().Attempts == 0);
    CHECK(activeEdits.front().OriginalRichValue == "rich base");
    CHECK(activeEdits.front().OriginalValue == "scalar base");
    CHECK(activeEdits.front().HasOriginalValue);
}

TEST_CASE("dead-letter restore via the SERVICE preserves the original backend key when the focused key differs "
          "(CR-951-1) and re-queues restored creates as fresh creates" *
          doctest::test_suite("[high-risk]")) {
    TestEnvGuard guard;
    FakeOfflineQueueDeps deps; // focused-context key is "Jira" — every dead row below is NOT Jira
    OfflineQueueService svc(deps);

    // Dead CREATE queued under "Plane", payload carrying a stale update identity.
    IssueDraft draft = MakeDraft("plane create");
    draft.ExistingIssueKey = "PLANE-9";
    draft.FieldValues["issuekey"] = "PLANE-9";
    draft.FieldValues["key"] = "PLANE-9";
    const std::int64_t createId = deps.CacheImpl->EnqueuePendingCreate("Plane", IssueDraftHelpers::ToJson(draft));
    deps.CacheImpl->ArchivePendingCreate(createId, "max_attempts", "terminal");
    REQUIRE(deps.CacheImpl->LoadPendingCreates().empty());

    const auto createSummary = svc.RestoreDeadPendingCreates({createId});
    CHECK(createSummary.Restored == 1);
    CHECK(createSummary.Failed == 0);
    CHECK(deps.CacheImpl->LoadDeadPendingCreates().empty());
    const auto restoredCreates = deps.CacheImpl->LoadPendingCreates();
    REQUIRE(restoredCreates.size() == 1);
    CHECK(restoredCreates.front().BackendKey == "Plane"); // NOT the deps' focused "Jira"
    CHECK(restoredCreates.front().Attempts == 0);
    // Fresh-create scrub preserved from the UI restore contract.
    IssueDraft restoredDraft;
    std::string parseErr;
    REQUIRE(IssueDraftHelpers::FromJson(restoredCreates.front().Payload, restoredDraft, parseErr));
    CHECK(restoredDraft.ExistingIssueKey.empty());
    CHECK(restoredDraft.FieldValues.count("issuekey") == 0);
    CHECK(restoredDraft.FieldValues.count("key") == 0);
    CHECK(restoredDraft.FieldValues.at("summary") == "plane create");

    // Dead FIELD EDIT queued under "GitHub" — the new key-preserving twin.
    const std::int64_t editId = deps.CacheImpl->EnqueuePendingFieldEdit(
        "GitHub", "OWN/REPO#7", "summary", "{\"summary\":\"v\"}", "rich base", "scalar base", true);
    deps.CacheImpl->ArchivePendingFieldEdit(editId, "replay_rejected", "terminal");
    REQUIRE(deps.CacheImpl->LoadPendingFieldEdits().empty());

    const auto editSummary = svc.RestoreDeadPendingFieldEdits({editId});
    CHECK(editSummary.Restored == 1);
    CHECK(editSummary.Failed == 0);
    CHECK(deps.CacheImpl->LoadDeadPendingFieldEdits().empty());
    const auto restoredEdits = deps.CacheImpl->LoadPendingFieldEdits();
    REQUIRE(restoredEdits.size() == 1);
    CHECK(restoredEdits.front().BackendKey == "GitHub"); // NOT the deps' focused "Jira"
    CHECK(restoredEdits.front().Attempts == 0);
    CHECK(restoredEdits.front().OriginalRichValue == "rich base");
    CHECK(restoredEdits.front().OriginalValue == "scalar base");
    CHECK(restoredEdits.front().HasOriginalValue);

    // Missing original id: counted Failed, nothing mutated.
    const auto missSummary = svc.RestoreDeadPendingFieldEdits({987654});
    CHECK(missSummary.Restored == 0);
    CHECK(missSummary.Failed == 1);
}

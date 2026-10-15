// SyncCacheContract — the executable spec for ISyncCache (ADR-0020, plan ilocalcache-seam #3).
//
// Authored against the REAL LocalCacheManager(":memory:") first (it passes by definition — it IS
// the contract). `FakeSyncCache` is appended as a second TEST_CASE_TEMPLATE type so the identical
// assertions run against both impls; the fake is then built until this suite is green, which is
// the per-method fake-fidelity gate. Every one of the 37 ISyncCache methods is touched here.
//
// This TU is SQLite-by-design (real-LCM half) — on the construction/direct-include purity gate's
// named-exemption allow-list.

#include "ISyncCache.h"
#include "LocalCacheManager.h"

#include "../support/FakeSyncCache.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace {

struct RealCacheMaker {
    static std::unique_ptr<ISyncCache> Make() { return std::make_unique<LocalCacheManager>(":memory:"); }
};

// The fake is the second contract subject — identical assertions run against both, so
// the fake cannot silently diverge from the production cache (the fidelity gate, grill Q3).
struct FakeCacheMaker {
    static std::unique_ptr<ISyncCache> Make() { return std::make_unique<smatchet_tests::FakeSyncCache>(); }
};

CachedTicket MakeTicket(const std::string& id, const std::string& summary) {
    CachedTicket t;
    t.id = id;
    t.fieldValues["summary"] = summary;
    return t;
}

std::vector<std::string> SortedIds(const std::vector<CachedTicket>& v) {
    std::vector<std::string> ids;
    ids.reserve(v.size());
    for (size_t i = 0; i < v.size(); ++i) {
        ids.push_back(v[i].id);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

} // namespace

// --- Tickets: backend-key namespacing + GetAllTickets set-equality + Try/Delete round-trip ---
TEST_CASE_TEMPLATE("ISyncCache: ticket storage is backend-key namespaced; GetAllTickets is a set", M, RealCacheMaker,
                   FakeCacheMaker) {
    auto c = M::Make();
    c->SaveTicket("Jira", MakeTicket("PROJ-1", "a"));
    c->SaveTickets("Jira", {MakeTicket("PROJ-2", "b"), MakeTicket("PROJ-3", "c")});
    c->SaveTicket("GitHub", MakeTicket("o/r#9", "z"));

    // Namespacing: a key sees only its own rows.
    CHECK(SortedIds(c->GetAllTickets("Jira")) == std::vector<std::string>{"PROJ-1", "PROJ-2", "PROJ-3"});
    CHECK(c->GetAllTickets("GitHub").size() == 1u);

    // GetAllTicketIds matches GetAllTickets (as a set).
    std::vector<std::string> ids = c->GetAllTicketIds("Jira");
    std::sort(ids.begin(), ids.end());
    CHECK(ids == std::vector<std::string>{"PROJ-1", "PROJ-2", "PROJ-3"});

    // TryGetTicket round-trips field values; misses return false.
    CachedTicket out;
    REQUIRE(c->TryGetTicket("Jira", "PROJ-2", out));
    CHECK(out.GetFieldValue("summary") == "b");
    CHECK_FALSE(c->TryGetTicket("Jira", "o/r#9", out)); // wrong namespace
    CHECK_FALSE(c->TryGetTicket("GitHub", "PROJ-2", out));

    c->DeleteTicket("Jira", "PROJ-2");
    CHECK(c->GetAllTickets("Jira").size() == 2u);
    CHECK_FALSE(c->TryGetTicket("Jira", "PROJ-2", out));
}

// --- Pending creates: FIFO load order + per-row backend-key capture (the #1081 latch) ---
TEST_CASE_TEMPLATE("ISyncCache: LoadPendingCreates is FIFO by enqueue id and carries the per-row backend key", M,
                   RealCacheMaker, FakeCacheMaker) {
    auto c = M::Make();
    const std::int64_t id1 = c->EnqueuePendingCreate("Jira", "p1");
    const std::int64_t id2 = c->EnqueuePendingCreate("Jira", "p2");
    const std::int64_t id3 = c->EnqueuePendingCreate("GitHub", "p3");
    CHECK(id1 < id2);
    CHECK(id2 < id3);

    std::vector<PendingCreate> rows = c->LoadPendingCreates();
    REQUIRE(rows.size() == 3u);
    // FIFO: rows arrive in ascending enqueue id, NOT grouped by key.
    CHECK(rows[0].Id == id1);
    CHECK(rows[1].Id == id2);
    CHECK(rows[2].Id == id3);
    CHECK(rows[0].Payload == "p1");
    // Per-row backend key captured at enqueue — never a global/replay-time re-read.
    CHECK(rows[0].BackendKey == "Jira");
    CHECK(rows[2].BackendKey == "GitHub");

    // UpdatePendingCreate bumps attempts + last error; UpdatePendingCreatePayload rewrites payload.
    c->UpdatePendingCreate(id1, 2, "boom");
    c->UpdatePendingCreatePayload(id2, "p2-rewritten");
    rows = c->LoadPendingCreates();
    CHECK(rows[0].Attempts == 2);
    CHECK(rows[0].LastError == "boom");
    CHECK(rows[1].Payload == "p2-rewritten");

    c->DeletePendingCreate(id3);
    CHECK(c->LoadPendingCreates().size() == 2u);
}

// --- Dead-letter creates: archive → newest-first load → restore (key preserved, attempts reset) ---
TEST_CASE_TEMPLATE("ISyncCache: create dead-letters load newest-first; restore re-queues under the same key", M,
                   RealCacheMaker, FakeCacheMaker) {
    auto c = M::Make();
    const std::int64_t a = c->EnqueuePendingCreate("Jira", "pa");
    const std::int64_t b = c->EnqueuePendingCreate("Jira", "pb");
    c->ArchivePendingCreate(a, "cap", "err-a");
    c->ArchivePendingCreate(b, "cap", "err-b");
    CHECK(c->LoadPendingCreates().empty()); // archived rows leave the active queue
    CHECK(c->GetDeadPendingCreateCount() == 2u);

    std::vector<DeadPendingCreate> dead = c->LoadDeadPendingCreates();
    REQUIRE(dead.size() == 2u);
    CHECK(dead[0].OriginalId == b); // newest-archived first (archived_at DESC, dead_id DESC)
    CHECK(dead[1].OriginalId == a);
    CHECK(dead[0].BackendKey == "Jira");

    // Restore the latest dead row for original `a` → back in the active queue, attempts reset, key kept.
    REQUIRE(c->RestoreDeadPendingCreate(a));
    std::vector<PendingCreate> active = c->LoadPendingCreates();
    REQUIRE(active.size() == 1u);
    CHECK(active[0].Attempts == 0);
    CHECK(active[0].BackendKey == "Jira");
    CHECK(c->GetDeadPendingCreateCount() == 1u);

    // Discard the remaining dead row.
    c->DeleteDeadPendingCreate(dead[0].DeadId);
    CHECK(c->GetDeadPendingCreateCount() == 0u);
}

// --- Pending field edits: FIFO + key + conflict mark/resolve + dead-letter restore ---
TEST_CASE_TEMPLATE("ISyncCache: field-edit queue FIFO, conflict mark/resolve, and dead-letter restore", M,
                   RealCacheMaker, FakeCacheMaker) {
    auto c = M::Make();
    const std::int64_t e1 = c->EnqueuePendingFieldEdit("Jira", "PROJ-1", "summary", "{\"summary\":\"v1\"}");
    const std::int64_t e2 = c->EnqueuePendingFieldEdit("GitHub", "o/r#2", "title", "{\"title\":\"v2\"}");
    CHECK(e1 < e2);

    std::vector<PendingFieldEditRecord> rows = c->LoadPendingFieldEdits();
    REQUIRE(rows.size() == 2u);
    CHECK(rows[0].Id == e1); // FIFO
    CHECK(rows[0].BackendKey == "Jira");
    CHECK(rows[0].IssueKey == "PROJ-1");
    CHECK(rows[1].BackendKey == "GitHub");

    c->UpdatePendingFieldEdit(e1, 1, "retry");
    c->MarkFieldEditConflict(e1, "{\"kind\":\"scalar\"}");
    rows = c->LoadPendingFieldEdits();
    CHECK(rows[0].Attempts == 1);
    CHECK(rows[0].HasMergeConflict);
    CHECK(rows[0].ConflictContextJson == "{\"kind\":\"scalar\"}");

    // Resolve clears the conflict flag and replaces the payload.
    c->ResolveFieldEditConflict(e1, "{\"summary\":\"resolved\"}");
    rows = c->LoadPendingFieldEdits();
    CHECK_FALSE(rows[0].HasMergeConflict);
    CHECK(rows[0].FieldsPayloadJson == "{\"summary\":\"resolved\"}");

    // Archive → dead-letter twin → restore round-trip.
    c->ArchivePendingFieldEdit(e2, "cap", "err");
    std::vector<DeadPendingFieldEdit> dead = c->LoadDeadPendingFieldEdits();
    REQUIRE(dead.size() == 1u);
    CHECK(dead[0].OriginalId == e2);
    CHECK(dead[0].BackendKey == "GitHub");
    REQUIRE(c->RestoreDeadPendingFieldEdit(e2));
    CHECK(c->LoadDeadPendingFieldEdits().empty());

    c->DeletePendingFieldEdit(e1);
    CHECK(c->LoadPendingFieldEdits().size() == 1u); // only the restored e2 remains
}

// --- Meta flags: set-once idempotent membership ---
TEST_CASE_TEMPLATE("ISyncCache: cache-meta flags are idempotent set-membership", M, RealCacheMaker, FakeCacheMaker) {
    auto c = M::Make();
    CHECK_FALSE(c->HasCacheMetaFlag("migrated"));
    c->SetCacheMetaFlag("migrated");
    c->SetCacheMetaFlag("migrated"); // idempotent — no throw, still set
    CHECK(c->HasCacheMetaFlag("migrated"));
    CHECK_FALSE(c->HasCacheMetaFlag("other"));
}

// --- Archive field mapping + fresh-create restore scrub ------------------------------------
TEST_CASE_TEMPLATE("ISyncCache: archive maps terminal fields; restore scrubs a fresh-create payload", M, RealCacheMaker,
                   FakeCacheMaker) {
    auto c = M::Make();
    // last_error: terminalError wins when non-empty, else the row's own LastError survives.
    const std::int64_t a = c->EnqueuePendingCreate("Jira", "not-json");
    c->UpdatePendingCreate(a, 3, "row-error");
    c->ArchivePendingCreate(a, "cap", "");
    const std::int64_t b = c->EnqueuePendingCreate("Jira", "{\"ExistingIssueKey\":\"PROJ-9\"}");
    c->ArchivePendingCreate(b, "rejected", "terminal-error");
    std::vector<DeadPendingCreate> dead = c->LoadDeadPendingCreates();
    REQUIRE(dead.size() == 2u);
    CHECK(dead[0].OriginalId == b); // newest-first
    CHECK(dead[0].TerminalReason == "rejected");
    CHECK(dead[0].LastError == "terminal-error");
    CHECK(dead[1].TerminalReason == "cap");
    CHECK(dead[1].LastError == "row-error");
    CHECK(dead[1].Attempts == 3);

    // Restore scrubs ExistingIssueKey (production IssueDraftHelpers::ScrubFreshCreatePayload —
    // a restored row must never replay as an UPDATE of a stale key)...
    REQUIRE(c->RestoreDeadPendingCreate(b));
    std::vector<PendingCreate> active = c->LoadPendingCreates();
    REQUIRE(active.size() == 1u);
    CHECK(active[0].Payload.find("PROJ-9") == std::string::npos);
    // ...and an unparseable payload restores verbatim (the replay tick dead-letters garbage).
    REQUIRE(c->RestoreDeadPendingCreate(a));
    active = c->LoadPendingCreates();
    REQUIRE(active.size() == 2u);
    CHECK(active[1].Payload == "not-json");
}

// --- Dead-letter key + merge-bases round-trip (pinned against BOTH impls here; service-level
// siblings live in OfflineQueueBackendKey.test.cpp) -----------------------------------------
TEST_CASE_TEMPLATE("ISyncCache: dead-letter rows carry the key; restore re-queues under the original key with bases", M,
                   RealCacheMaker, FakeCacheMaker) {
    auto c = M::Make();
    const std::int64_t createId = c->EnqueuePendingCreate("Plane", "{\"k\":1}");
    c->ArchivePendingCreate(createId, "max_attempts", "terminal");
    std::vector<DeadPendingCreate> deadCreates = c->LoadDeadPendingCreates();
    REQUIRE(deadCreates.size() == 1u);
    CHECK(deadCreates.front().BackendKey == "Plane");
    REQUIRE(c->RestoreDeadPendingCreate(createId));
    std::vector<PendingCreate> active = c->LoadPendingCreates();
    REQUIRE(active.size() == 1u);
    CHECK(active.front().BackendKey == "Plane"); // original key, NOT any focused context's key
    CHECK(active.front().Attempts == 0);

    const std::int64_t editId =
        c->EnqueuePendingFieldEdit("GitHub", "OWN/REPO#7", "summary", "{}", "rich base", "scalar base", true);
    c->ArchivePendingFieldEdit(editId, "replay_rejected", "terminal");
    std::vector<DeadPendingFieldEdit> deadEdits = c->LoadDeadPendingFieldEdits();
    REQUIRE(deadEdits.size() == 1u);
    CHECK(deadEdits.front().BackendKey == "GitHub");
    // Discard path: a second archived edit removed permanently via DeleteDeadPendingFieldEdit.
    const std::int64_t editId2 = c->EnqueuePendingFieldEdit("GitHub", "OWN/REPO#8", "title", "{}");
    c->ArchivePendingFieldEdit(editId2, "cap", "err");
    REQUIRE(c->LoadDeadPendingFieldEdits().size() == 2u);
    c->DeleteDeadPendingFieldEdit(c->LoadDeadPendingFieldEdits().front().DeadId); // newest = editId2's
    REQUIRE(c->LoadDeadPendingFieldEdits().size() == 1u);

    REQUIRE(c->RestoreDeadPendingFieldEdit(editId));
    CHECK(c->LoadDeadPendingFieldEdits().empty());
    std::vector<PendingFieldEditRecord> activeEdits = c->LoadPendingFieldEdits();
    REQUIRE(activeEdits.size() == 1u);
    CHECK(activeEdits.front().BackendKey == "GitHub");
    CHECK(activeEdits.front().IssueKey == "OWN/REPO#7");
    CHECK(activeEdits.front().FieldId == "summary");
    CHECK(activeEdits.front().Attempts == 0);
    CHECK(activeEdits.front().OriginalRichValue == "rich base");
    CHECK(activeEdits.front().OriginalValue == "scalar base");
    CHECK(activeEdits.front().HasOriginalValue);
}

// --- Resolve clears BOTH bases (production UPDATE column list; HasOriginalValue untouched) --
TEST_CASE_TEMPLATE("ISyncCache: ResolveFieldEditConflict clears flag, context, and both bases", M, RealCacheMaker,
                   FakeCacheMaker) {
    auto c = M::Make();
    const std::int64_t e = c->EnqueuePendingFieldEdit("Jira", "PROJ-1", "summary", "{}", "rich", "scalar", true);
    c->MarkFieldEditConflict(e, "{\"kind\":\"scalar\"}");
    c->ResolveFieldEditConflict(e, "{\"summary\":\"r\"}");
    std::vector<PendingFieldEditRecord> rows = c->LoadPendingFieldEdits();
    REQUIRE(rows.size() == 1u);
    CHECK_FALSE(rows[0].HasMergeConflict);
    CHECK(rows[0].ConflictContextJson.empty());
    CHECK(rows[0].OriginalRichValue.empty());
    CHECK(rows[0].OriginalValue.empty());
    CHECK(rows[0].FieldsPayloadJson == "{\"summary\":\"r\"}");
}

// --- Pending actions (comments / worklogs / watch; Quality Pillar 6) ---------------------------
TEST_CASE_TEMPLATE("ISyncCache: pending actions are FIFO, keep backend key and state, and update in place", M,
                   RealCacheMaker, FakeCacheMaker) {
    auto c = M::Make();
    const std::int64_t a = c->EnqueuePendingAction("Jira", "comment_add", "PROJ-1", "{\"body\":\"a\"}", "pending");
    const std::int64_t b = c->EnqueuePendingAction("Plane", "comment_add", "P-2", "{\"body\":\"b\"}", "ambiguous");
    CHECK(b > a);
    std::vector<PendingActionRecord> rows = c->LoadPendingActions();
    REQUIRE(rows.size() == 2u);
    CHECK(rows[0].Id == a);
    CHECK(rows[0].BackendKey == "Jira");
    CHECK(rows[0].Kind == "comment_add");
    CHECK(rows[0].IssueKey == "PROJ-1");
    CHECK(rows[0].PayloadJson == "{\"body\":\"a\"}");
    CHECK(rows[0].State == "pending");
    CHECK(rows[0].Attempts == 0);
    CHECK(rows[0].CreatedAtEpochSec > 0);
    CHECK(rows[1].BackendKey == "Plane");
    CHECK(rows[1].State == "ambiguous");

    c->UpdatePendingAction(a, "sending", 2, "timeout");
    rows = c->LoadPendingActions();
    CHECK(rows[0].State == "sending");
    CHECK(rows[0].Attempts == 2);
    CHECK(rows[0].LastError == "timeout");

    c->DeletePendingAction(a);
    rows = c->LoadPendingActions();
    REQUIRE(rows.size() == 1u);
    CHECK(rows[0].Id == b);
}

TEST_CASE_TEMPLATE("ISyncCache: a dead pending action keeps its state and key; restore resumes from it", M,
                   RealCacheMaker, FakeCacheMaker) {
    auto c = M::Make();
    const std::int64_t first = c->EnqueuePendingAction("Jira", "comment_add", "PROJ-1", "{\"body\":\"1\"}", "pending");
    const std::int64_t second =
        c->EnqueuePendingAction("GitHub", "comment_add", "o/r#3", "{\"body\":\"2\"}", "pending");
    c->UpdatePendingAction(first, "ambiguous", 4, "response lost");
    c->ArchivePendingAction(first, "max_attempts", std::string()); // keeps the row's last error
    c->ArchivePendingAction(second, "replay_rejected", "Issue is closed");
    CHECK(c->LoadPendingActions().empty());

    const std::vector<DeadPendingAction> dead = c->LoadDeadPendingActions();
    REQUIRE(dead.size() == 2u);
    CHECK(dead[0].Row.Id == second); // newest first
    CHECK(dead[0].TerminalReason == "replay_rejected");
    CHECK(dead[0].Row.LastError == "Issue is closed");
    CHECK(dead[0].Row.BackendKey == "GitHub");
    CHECK(dead[1].Row.Id == first);
    CHECK(dead[1].Row.State == "ambiguous");
    CHECK(dead[1].Row.Attempts == 4);
    CHECK(dead[1].Row.LastError == "response lost");
    CHECK(dead[1].Row.PayloadJson == "{\"body\":\"1\"}");
    CHECK(dead[1].ArchivedAtEpochSec > 0);

    CHECK(c->RestoreDeadPendingAction(first));
    CHECK_FALSE(c->RestoreDeadPendingAction(first)); // already restored
    const std::vector<PendingActionRecord> rows = c->LoadPendingActions();
    REQUIRE(rows.size() == 1u);
    CHECK(rows[0].Id > second); // a new id, like AUTOINCREMENT
    CHECK(rows[0].BackendKey == "Jira");
    CHECK(rows[0].State == "ambiguous");
    CHECK(rows[0].Attempts == 0);
    CHECK(rows[0].IssueKey == "PROJ-1");

    c->DeleteDeadPendingAction(dead[0].DeadId);
    CHECK(c->LoadDeadPendingActions().empty());
}

TEST_CASE_TEMPLATE("ISyncCache: a pending-action transition applies only from the expected state", M, RealCacheMaker,
                   FakeCacheMaker) {
    auto c = M::Make();
    const std::int64_t id = c->EnqueuePendingAction("Jira", "comment_add", "PROJ-1", "{\"body\":\"1\"}", "pending");
    CHECK_FALSE(c->TransitionPendingAction(id, "needs_review", "pending", 0, std::string())); // wrong state
    std::vector<PendingActionRecord> rows = c->LoadPendingActions();
    REQUIRE(rows.size() == 1u);
    CHECK(rows[0].State == "pending");

    CHECK(c->TransitionPendingAction(id, "pending", "sending", 2, "retrying"));
    rows = c->LoadPendingActions();
    CHECK(rows[0].State == "sending");
    CHECK(rows[0].Attempts == 2);
    CHECK(rows[0].LastError == "retrying");

    c->DeletePendingAction(id);
    CHECK_FALSE(c->TransitionPendingAction(id, "sending", "pending", 0, std::string())); // discarded
    CHECK(c->LoadPendingActions().empty());
}

TEST_CASE_TEMPLATE("ISyncCache: archiving a missing pending action throws", M, RealCacheMaker, FakeCacheMaker) {
    auto c = M::Make();
    CHECK_THROWS(c->ArchivePendingAction(12345, "replay_rejected", "x"));
    CHECK(c->LoadDeadPendingActions().empty());
}

// --- Delta-sync metadata: the tracker version each cached row was fetched at ---
namespace {
CachedTicket MetaRow(const std::string& id, const std::string& updated, const std::string& field,
                     const std::string& value) {
    CachedTicket t;
    t.id = id;
    t.fieldValues["updated"] = updated;
    t.fieldValues[field] = value;
    return t;
}
TicketSyncMeta MetaOf(const std::string& updated, std::vector<std::string> fields) {
    TicketSyncMeta m;
    m.RemoteUpdated = updated;
    m.Fields = std::move(fields);
    m.SyncedAtEpochSec = 77;
    return m;
}
} // namespace

TEST_CASE_TEMPLATE("ISyncCache: sync meta is stored per backend key and goes with its ticket", M, RealCacheMaker,
                   FakeCacheMaker) {
    auto c = M::Make();
    c->SaveTicketsWithSyncMeta("Jira", {MetaRow("A-1", "T1", "summary", "a"), MetaRow("A-2", "T1", "summary", "b")},
                               {MetaOf("T1", {"summary", "updated"}), MetaOf("T2", {"summary", "updated"})});
    c->SaveTicketsWithSyncMeta("GitHub", {MetaRow("A-1", "G1", "summary", "g")}, {MetaOf("G1", {"summary"})});

    const auto jira = c->GetTicketSyncMeta("Jira");
    REQUIRE(jira.size() == 2u);
    CHECK(jira.at("A-1").RemoteUpdated == "T1");
    CHECK(jira.at("A-2").RemoteUpdated == "T2");
    CHECK(jira.at("A-1").Fields == std::vector<std::string>{"summary", "updated"});
    CHECK(jira.at("A-1").SyncedAtEpochSec == 77);
    CHECK(c->GetTicketSyncMeta("GitHub").size() == 1u);
    CHECK(c->GetTicketSyncMeta("Plane").empty());

    // The rows themselves were saved by the same call.
    CachedTicket out;
    REQUIRE(c->TryGetTicket("Jira", "A-2", out));
    CHECK(out.GetFieldValue("summary") == "b");

    // Deleting a ticket takes its meta with it; the batch delete does the same.
    c->DeleteTicket("Jira", "A-1");
    CHECK(c->GetTicketSyncMeta("Jira").count("A-1") == 0u);
    c->DeleteTickets("Jira", {"A-2"});
    CHECK(c->GetTicketSyncMeta("Jira").empty());
    CHECK(c->GetTicketSyncMeta("GitHub").size() == 1u); // another namespace is untouched
}

TEST_CASE_TEMPLATE("ISyncCache: a plain save or a save without a stamp drops the stored meta", M, RealCacheMaker,
                   FakeCacheMaker) {
    auto c = M::Make();
    c->SaveTicketsWithSyncMeta("Jira", {MetaRow("A-1", "T1", "summary", "a")}, {MetaOf("T1", {"summary"})});
    // A plain save rewrites the row without knowing its version: the stored meta no longer vouches for it.
    c->SaveTickets("Jira", {MetaRow("A-1", "T1", "summary", "edited")});
    CHECK(c->GetTicketSyncMeta("Jira").empty());
    c->SaveTicketsWithSyncMeta("Jira", {MetaRow("A-1", "T1", "summary", "a")}, {MetaOf("T1", {"summary"})});
    c->SaveTicket("Jira", MetaRow("A-1", "T1", "summary", "edited again"));
    CHECK(c->GetTicketSyncMeta("Jira").empty());
    // A save with an unknown version drops it too.
    c->SaveTicketsWithSyncMeta("Jira", {MetaRow("A-1", "T1", "summary", "a")}, {MetaOf("T1", {"summary"})});
    c->SaveTicketsWithSyncMeta("Jira", {MetaRow("A-1", "", "summary", "x")}, {TicketSyncMeta()});
    CHECK(c->GetTicketSyncMeta("Jira").empty());
}

TEST_CASE_TEMPLATE("ISyncCache: saving at the same version merges the columns; a newer version replaces them", M,
                   RealCacheMaker, FakeCacheMaker) {
    auto c = M::Make();
    // Pane A fetched {summary, updated}; pane B then fetches {labels, updated} of the SAME version.
    c->SaveTicketsWithSyncMeta("Jira", {MetaRow("A-1", "T1", "summary", "s")}, {MetaOf("T1", {"summary", "updated"})});
    c->SaveTicketsWithSyncMeta("Jira", {MetaRow("A-1", "T1", "labels", "l")}, {MetaOf("T1", {"labels", "updated"})});

    CachedTicket out;
    REQUIRE(c->TryGetTicket("Jira", "A-1", out));
    CHECK(out.GetFieldValue("summary") == "s"); // kept: pane A's column survives pane B's save
    CHECK(out.GetFieldValue("labels") == "l");
    CHECK(c->GetTicketSyncMeta("Jira").at("A-1").Fields == std::vector<std::string>{"labels", "summary", "updated"});

    // A newer version replaces the row outright (no stale columns carried over).
    c->SaveTicketsWithSyncMeta("Jira", {MetaRow("A-1", "T2", "labels", "l2")}, {MetaOf("T2", {"labels", "updated"})});
    REQUIRE(c->TryGetTicket("Jira", "A-1", out));
    CHECK(out.GetFieldValue("summary").empty());
    CHECK(out.GetFieldValue("labels") == "l2");
    CHECK(c->GetTicketSyncMeta("Jira").at("A-1").Fields == std::vector<std::string>{"labels", "updated"});
}

TEST_CASE_TEMPLATE("ISyncCache: an older version never overwrites a stored row unless overwriteOlder", M,
                   RealCacheMaker, FakeCacheMaker) {
    const std::string newer = "2024-05-02T10:00:00.000+0000";
    const std::string older = "2024-05-01T10:00:00.000+0000";
    auto c = M::Make();
    c->SaveTicketsWithSyncMeta("Jira", {MetaRow("A-1", newer, "summary", "new")}, {MetaOf(newer, {"summary"})});

    // A late save of an older version is skipped: row and meta stay.
    c->SaveTicketsWithSyncMeta("Jira", {MetaRow("A-1", older, "summary", "old")}, {MetaOf(older, {"summary"})});
    CachedTicket out;
    REQUIRE(c->TryGetTicket("Jira", "A-1", out));
    CHECK(out.GetFieldValue("summary") == "new");
    CHECK(c->GetTicketSyncMeta("Jira").at("A-1").RemoteUpdated == newer);

    // The Full-sync bypass replaces it anyway.
    c->SaveTicketsWithSyncMeta("Jira", {MetaRow("A-1", older, "summary", "old")}, {MetaOf(older, {"summary"})},
                               /*overwriteOlder=*/true);
    REQUIRE(c->TryGetTicket("Jira", "A-1", out));
    CHECK(out.GetFieldValue("summary") == "old");
    CHECK(c->GetTicketSyncMeta("Jira").at("A-1").RemoteUpdated == older);
}

TEST_CASE_TEMPLATE("ISyncCache: an equal version merges and a far-future stored stamp does not block", M,
                   RealCacheMaker, FakeCacheMaker) {
    const std::string stamp = "2024-05-01T10:00:00.000+0000";
    auto c = M::Make();
    c->SaveTicketsWithSyncMeta("Jira", {MetaRow("A-1", stamp, "summary", "s")}, {MetaOf(stamp, {"summary"})});
    c->SaveTicketsWithSyncMeta("Jira", {MetaRow("A-1", stamp, "labels", "l")}, {MetaOf(stamp, {"labels"})});
    CachedTicket out;
    REQUIRE(c->TryGetTicket("Jira", "A-1", out));
    CHECK(out.GetFieldValue("summary") == "s");
    CHECK(out.GetFieldValue("labels") == "l");

    // A stored stamp more than a day ahead of the clock is unknown, so an older-looking row still lands.
    const std::string future = "2999-01-01T00:00:00.000+0000";
    c->SaveTicketsWithSyncMeta("Jira", {MetaRow("A-2", future, "summary", "wedged")}, {MetaOf(future, {"summary"})});
    c->SaveTicketsWithSyncMeta("Jira", {MetaRow("A-2", stamp, "summary", "real")}, {MetaOf(stamp, {"summary"})});
    REQUIRE(c->TryGetTicket("Jira", "A-2", out));
    CHECK(out.GetFieldValue("summary") == "real");
    CHECK(c->GetTicketSyncMeta("Jira").at("A-2").RemoteUpdated == stamp);
}

TEST_CASE_TEMPLATE("ISyncCache: GetTicketsByIds returns the existing rows and skips the rest", M, RealCacheMaker,
                   FakeCacheMaker) {
    auto c = M::Make();
    c->SaveTickets("Jira", {MakeTicket("A-1", "a"), MakeTicket("A-2", "b"), MakeTicket("A-3", "c")});
    c->SaveTicket("GitHub", MakeTicket("A-9", "other namespace"));
    const std::vector<CachedTicket> rows = c->GetTicketsByIds("Jira", {"A-3", "A-1", "A-9", "NOPE-1"});
    CHECK(SortedIds(rows) == std::vector<std::string>{"A-1", "A-3"});
    CHECK(c->GetTicketsByIds("Jira", {}).empty());
}

// SyncCacheContract — the executable spec for ISyncCache (ADR-0020, plan ilocalcache-seam #3).
//
// Authored against the REAL LocalCacheManager(":memory:") first (it passes by definition — it IS
// the contract). PR2 appends `FakeSyncCache` as a second TEST_CASE_TEMPLATE type so the identical
// assertions run against both impls; the fake is then built until this suite is green, which is
// the per-method fake-fidelity gate. Every one of the 28 ISyncCache methods is touched here.
//
// This TU is SQLite-by-design (real-LCM half) — on the construction/direct-include purity gate's
// named-exemption allow-list.

#include "ISyncCache.h"
#include "LocalCacheManager.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace {

// PR1: the only contract subject is the real cache. PR2 adds `FakeCacheMaker`.
struct RealCacheMaker {
    static std::unique_ptr<ISyncCache> Make() { return std::make_unique<LocalCacheManager>(":memory:"); }
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
TEST_CASE_TEMPLATE("ISyncCache: ticket storage is backend-key namespaced; GetAllTickets is a set", M, RealCacheMaker) {
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
                   RealCacheMaker) {
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
                   RealCacheMaker) {
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
    // NOTE (PR2 fidelity): extend with the fresh-create payload-scrub assertion
    // (IssueDraftHelpers::ScrubFreshCreatePayload — clears ExistingIssueKey inside the restore txn).

    // Discard the remaining dead row.
    c->DeleteDeadPendingCreate(dead[0].DeadId);
    CHECK(c->GetDeadPendingCreateCount() == 0u);
}

// --- Pending field edits: FIFO + key + conflict mark/resolve + dead-letter restore ---
TEST_CASE_TEMPLATE("ISyncCache: field-edit queue FIFO, conflict mark/resolve, and dead-letter restore", M,
                   RealCacheMaker) {
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
TEST_CASE_TEMPLATE("ISyncCache: cache-meta flags are idempotent set-membership", M, RealCacheMaker) {
    auto c = M::Make();
    CHECK_FALSE(c->HasCacheMetaFlag("migrated"));
    c->SetCacheMetaFlag("migrated");
    c->SetCacheMetaFlag("migrated"); // idempotent — no throw, still set
    CHECK(c->HasCacheMetaFlag("migrated"));
    CHECK_FALSE(c->HasCacheMetaFlag("other"));
}

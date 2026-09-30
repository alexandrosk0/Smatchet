// LocalCacheManager::RunOneTimeCacheIdentityRekey (#2268) — the one-time move from the legacy tracker-kind
// cache keys to the site-and-account keys: tickets, saved lookups and every offline queue follow; queued
// writes keep their ids and payloads; a second run is a no-op.

#include "../support/SqliteMemFixture.h"

#include "LocalCacheManager.h"

#include <doctest/doctest.h>

#include <string>
#include <utility>
#include <vector>

using smatchet_tests::SqliteMemFixture;

namespace {

const char* const kLegacy = "Jira";
const char* const kSite = "Jira@acme.atlassian.net#08168cd80dfd";
const char* const kOtherLegacy = "Plane";
const char* const kKind = "users";

CachedTicket Ticket(const std::string& id, const std::string& summary) {
    CachedTicket t;
    t.id = id;
    t.fieldValues["summary"] = summary;
    return t;
}

std::vector<std::pair<std::string, std::string>> JiraToSite() {
    return std::vector<std::pair<std::string, std::string>>{{kLegacy, kSite}};
}

} // namespace

TEST_SUITE("LocalCacheIdentityRekey") {

    TEST_CASE("tickets, lookups and every queue move from the legacy key to the site key") {
        SqliteMemFixture fx;
        LocalCacheManager& cache = fx.Ref();
        cache.SaveTicket(kLegacy, Ticket("OFF-1", "legacy summary"));
        REQUIRE(cache.UpsertLookup(kLegacy, kKind, "all", "[]"));
        const std::int64_t editId = cache.EnqueuePendingFieldEdit(kLegacy, "OFF-1", "summary", R"({"summary":"x"})");
        const std::int64_t createId = cache.EnqueuePendingCreate(kLegacy, R"({"fields":{}})");
        const std::int64_t actionId =
            cache.EnqueuePendingAction(kLegacy, "comment_add", "OFF-1", R"({"body":"hi","created":1})", "pending");
        REQUIRE(editId > 0);
        REQUIRE(createId > 0);
        REQUIRE(actionId > 0);
        // A row under another tracker's legacy key that has no pair stays where it is.
        cache.SaveTicket(kOtherLegacy, Ticket("PL-1", "plane"));

        CHECK(cache.RunOneTimeCacheIdentityRekey(JiraToSite()) >= 6);

        CHECK(cache.GetAllTickets(kLegacy).empty());
        const std::vector<CachedTicket> moved = cache.GetAllTickets(kSite);
        REQUIRE(moved.size() == 1);
        CHECK(moved[0].id == "OFF-1");
        CHECK(moved[0].GetFieldValue("summary") == "legacy summary");
        LookupCacheRow row;
        CHECK(cache.TryGetLookup(kSite, kKind, "all", row));
        CHECK_FALSE(cache.TryGetLookup(kLegacy, kKind, "all", row));

        const std::vector<PendingFieldEditRecord> edits = cache.LoadPendingFieldEdits();
        REQUIRE(edits.size() == 1);
        CHECK(edits[0].Id == editId);
        CHECK(edits[0].BackendKey == kSite);
        CHECK(edits[0].FieldsPayloadJson == R"({"summary":"x"})");
        const std::vector<PendingCreate> creates = cache.LoadPendingCreates();
        REQUIRE(creates.size() == 1);
        CHECK(creates[0].Id == createId);
        CHECK(creates[0].BackendKey == kSite);
        const std::vector<PendingActionRecord> actions = cache.LoadPendingActions();
        REQUIRE(actions.size() == 1);
        CHECK(actions[0].Id == actionId);
        CHECK(actions[0].BackendKey == kSite);
        CHECK(actions[0].PayloadJson == R"({"body":"hi","created":1})");

        CHECK(cache.GetAllTickets(kOtherLegacy).size() == 1);
    }

    TEST_CASE("dead-letter rows move too, so a restore re-queues under the site key") {
        SqliteMemFixture fx;
        LocalCacheManager& cache = fx.Ref();
        const std::int64_t createId = cache.EnqueuePendingCreate(kLegacy, R"({"fields":{}})");
        cache.ArchivePendingCreate(createId, "replay_rejected", "HTTP 400");
        REQUIRE(cache.LoadDeadPendingCreates().size() == 1);

        cache.RunOneTimeCacheIdentityRekey(JiraToSite());

        const std::vector<DeadPendingCreate> dead = cache.LoadDeadPendingCreates();
        REQUIRE(dead.size() == 1);
        CHECK(dead[0].BackendKey == kSite);
    }

    TEST_CASE("a collision keeps the row already under the site key and drops the legacy copy") {
        SqliteMemFixture fx;
        LocalCacheManager& cache = fx.Ref();
        cache.SaveTicket(kLegacy, Ticket("OFF-1", "legacy"));
        cache.SaveTicket(kSite, Ticket("OFF-1", "site"));

        cache.RunOneTimeCacheIdentityRekey(JiraToSite());

        CHECK(cache.GetAllTickets(kLegacy).empty());
        const std::vector<CachedTicket> site = cache.GetAllTickets(kSite);
        REQUIRE(site.size() == 1);
        CHECK(site[0].GetFieldValue("summary") == "site");
    }

    TEST_CASE("the move runs once per database file") {
        SqliteMemFixture fx;
        LocalCacheManager& cache = fx.Ref();
        cache.RunOneTimeCacheIdentityRekey(JiraToSite());
        // Rows written under the legacy key after the move (none should be) are left alone.
        cache.SaveTicket(kLegacy, Ticket("OFF-2", "late"));
        CHECK(cache.RunOneTimeCacheIdentityRekey(JiraToSite()) == 0);
        CHECK(cache.GetAllTickets(kLegacy).size() == 1);
    }

    TEST_CASE("an empty list only sets the flag") {
        SqliteMemFixture fx;
        LocalCacheManager& cache = fx.Ref();
        cache.SaveTicket(kLegacy, Ticket("OFF-1", "legacy"));
        CHECK(cache.RunOneTimeCacheIdentityRekey(std::vector<std::pair<std::string, std::string>>()) == 0);
        CHECK(cache.RunOneTimeCacheIdentityRekey(JiraToSite()) == 0);
        CHECK(cache.GetAllTickets(kLegacy).size() == 1);
    }

    TEST_CASE("pairs that would empty or keep a namespace are skipped") {
        SqliteMemFixture fx;
        LocalCacheManager& cache = fx.Ref();
        cache.SaveTicket(kLegacy, Ticket("OFF-1", "legacy"));
        const std::vector<std::pair<std::string, std::string>> bad{{kLegacy, ""}, {"", kSite}, {kLegacy, kLegacy}};
        CHECK(cache.RunOneTimeCacheIdentityRekey(bad) == 0);
        CHECK(cache.GetAllTickets(kLegacy).size() == 1);
        CHECK(cache.GetAllTickets(kSite).empty());
    }
}

// AppController::StartRecreateLocalCacheDatabase refuses to start when it cannot run, and a refusal leaves the
// "recreating" gate down so a later attempt is not blocked. Linked into SmatchetCommandsTests with the real
// (headless) AppController; the worker path itself is covered by the cache.recreate command tests (the facet fake)
// and by the spawned-instance smoke run in docs/plans/active/delta-sync.md.

#include <doctest/doctest.h>

#include "../support/OfflineQueueTestEnv.h"

#include "AppController.h"

#include <string>

TEST_CASE("AppController::StartRecreateLocalCacheDatabase refuses without a database path and keeps the gate down") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app; // never Initialize()d: no local cache database path is configured
    CHECK_FALSE(app.IsLocalCacheRecreating());

    bool called = false;
    const VoidResult started =
        app.StartRecreateLocalCacheDatabase([&called](const smatchet::sync::CacheRecreateOutcome&) { called = true; });
    REQUIRE_FALSE(started.has_value());
    CHECK(started.error() == "Local cache database path is not set.");
    CHECK_FALSE(app.IsLocalCacheRecreating()); // the refusal lowered the gate again
    CHECK_FALSE(called);                       // a refusal never calls back

    // A second refusal says the same: nothing latched.
    const VoidResult again = app.StartRecreateLocalCacheDatabase(nullptr);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error() == "Local cache database path is not set.");
}

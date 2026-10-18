#include <doctest/doctest.h>

#include "Sync/CacheRecreateReportPure.h"

#include <string>

using namespace smatchet::sync;

TEST_CASE("CacheRecreateStartRefusal: names why a recreate cannot start, and is empty when it can") {
    CHECK(CacheRecreateStartRefusal(false, false, false).empty());
    CHECK(CacheRecreateStartRefusal(true, false, false) == "Local cache database path is not set.");
    CHECK(CacheRecreateStartRefusal(false, true, false) == "Application is shutting down.");
    CHECK(CacheRecreateStartRefusal(false, false, true) == "The local cache is already being recreated.");
    // A missing path is reported before the others: it can never be fixed by waiting.
    CHECK(CacheRecreateStartRefusal(true, true, true) == "Local cache database path is not set.");
}

TEST_CASE("DescribeCacheRecreateOutcome: success is silent, a failure says whether the cache was wiped") {
    CacheRecreateOutcome ok;
    ok.Ok = true;
    ok.Wiped = true;
    CHECK(DescribeCacheRecreateOutcome(ok).empty());

    CacheRecreateOutcome untouched;
    untouched.Error = "Could not inspect cache.db: denied";
    const std::string notWiped = DescribeCacheRecreateOutcome(untouched);
    CHECK(notWiped.find("was not recreated") != std::string::npos);
    CHECK(notWiped.find("denied") != std::string::npos);

    CacheRecreateOutcome gone;
    gone.Wiped = true;
    gone.Error = "Failed to open new database: disk full";
    const std::string wiped = DescribeCacheRecreateOutcome(gone);
    CHECK(wiped.find("was wiped") != std::string::npos);
    CHECK(wiped.find("disk full") != std::string::npos);
    CHECK(wiped.find("was not recreated") == std::string::npos);
}

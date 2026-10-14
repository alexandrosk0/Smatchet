// Sync/LookupFreshnessPure.h - when a saved lookup row may be served without a live fetch.

#include "Sync/LookupFreshnessPure.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using smatchet::sync::CanSkipCatalogFetch;
using smatchet::sync::IsLookupRowFresh;
using smatchet::sync::SecondsUntilLookupStale;

TEST_CASE("IsLookupRowFresh: younger than the lifetime is fresh, at or past it is not") {
    CHECK(IsLookupRowFresh(1000, 1000, 60));
    CHECK(IsLookupRowFresh(1000, 1059, 60));
    CHECK_FALSE(IsLookupRowFresh(1000, 1060, 60));
    CHECK_FALSE(IsLookupRowFresh(1000, 99999, 60));
}

TEST_CASE("IsLookupRowFresh: an unknown save time, no lifetime or a clock that moved back is never fresh") {
    CHECK_FALSE(IsLookupRowFresh(0, 1000, 60));
    CHECK_FALSE(IsLookupRowFresh(-5, 1000, 60));
    CHECK_FALSE(IsLookupRowFresh(1000, 1010, 0));
    CHECK_FALSE(IsLookupRowFresh(1000, 1010, -1));
    CHECK_FALSE(IsLookupRowFresh(2000, 1000, 60));
}

TEST_CASE("SecondsUntilLookupStale counts down and is zero once stale") {
    CHECK(SecondsUntilLookupStale(1000, 1000, 60) == 60);
    CHECK(SecondsUntilLookupStale(1000, 1050, 60) == 10);
    CHECK(SecondsUntilLookupStale(1000, 1060, 60) == 0);
    CHECK(SecondsUntilLookupStale(0, 1000, 60) == 0);
}

TEST_CASE("CanSkipCatalogFetch: a fresh snapshot that resolves every view column skips the fetch") {
    const std::vector<std::string> catalog = {"summary", "status", "assignee"};
    CHECK(CanSkipCatalogFetch(1000, 1100, 600, {"summary", "", "id", "status"}, catalog, "PROJ", "PROJ"));
    CHECK_FALSE(CanSkipCatalogFetch(1000, 1100, 600, {"summary", "customfield_10001"}, catalog, "PROJ",
                                    "PROJ"));                                                // unknown column
    CHECK_FALSE(CanSkipCatalogFetch(1000, 2000, 600, {"summary"}, catalog, "PROJ", "PROJ")); // expired
    CHECK_FALSE(CanSkipCatalogFetch(0, 1100, 600, {"summary"}, catalog, "PROJ", "PROJ"));    // age unknown
    CHECK_FALSE(CanSkipCatalogFetch(1000, 1100, 600, {"summary"}, {}, "PROJ", "PROJ"));      // empty snapshot
    // The snapshot's project scope must be the one the view resolves to: a fresh UNSCOPED snapshot (the startup
    // fallback) never stands in for the project-scoped fetch, and a snapshot of another project never does.
    CHECK_FALSE(CanSkipCatalogFetch(1000, 1100, 600, {"summary"}, catalog, "", "PROJ"));
    CHECK_FALSE(CanSkipCatalogFetch(1000, 1100, 600, {"summary"}, catalog, "OTHER", "PROJ"));
    CHECK_FALSE(CanSkipCatalogFetch(1000, 1100, 600, {"summary"}, catalog, "PROJ", ""));
    CHECK(CanSkipCatalogFetch(1000, 1100, 600, {"summary"}, catalog, "", "")); // view resolves no project either
}

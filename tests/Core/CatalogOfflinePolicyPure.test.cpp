#include "CatalogOfflinePolicyPure.h"

#include <doctest/doctest.h>

using smatchet::catalogoffline::CatalogFailureBanner;
using smatchet::catalogoffline::DecideCatalogFailureBanner;
using smatchet::catalogoffline::ExtractLastFetchFailedDetail;
using smatchet::catalogoffline::IsStartupSnapshotWarning;

TEST_CASE("DecideCatalogFailureBanner never chooses a wipe for a retryable failure") {
    // Transient, has fields now → use cached.
    CHECK(DecideCatalogFailureBanner(true, true, false, false) == CatalogFailureBanner::WarningUsingCached);
    // Transient, no fields, snapshot loaded → restored snapshot.
    CHECK(DecideCatalogFailureBanner(true, false, true, false) == CatalogFailureBanner::WarningRestoredSnapshot);
    // Transient, no fields, no snapshot, but session had catalog → session had catalog.
    CHECK(DecideCatalogFailureBanner(true, false, false, true) == CatalogFailureBanner::WarningSessionHadCatalog);
    // Transient, no fields, no snapshot, never loaded → no catalog.
    CHECK(DecideCatalogFailureBanner(true, false, false, false) == CatalogFailureBanner::ErrorNoCatalog);
}

TEST_CASE("DecideCatalogFailureBanner keeps the catalog on a non-retryable failure") {
    // Non-retryable, has fields → keep catalog.
    CHECK(DecideCatalogFailureBanner(false, true, false, false) == CatalogFailureBanner::ErrorKeepCatalog);
    // Non-retryable, no fields, snapshot loaded → keep snapshot.
    CHECK(DecideCatalogFailureBanner(false, false, true, false) == CatalogFailureBanner::ErrorKeepCatalog);
    // Non-retryable, nothing at all → no catalog.
    CHECK(DecideCatalogFailureBanner(false, false, false, false) == CatalogFailureBanner::ErrorNoCatalog);
}

TEST_CASE("ExtractLastFetchFailedDetail reads every AppController warning wording") {
    const std::string jiraUsing =
        "Offline: using cached Jira field catalog. Last fetch failed: HTTP 0 (Couldn't connect)";
    CHECK(ExtractLastFetchFailedDetail(jiraUsing) == "HTTP 0 (Couldn't connect)");

    const std::string planeRestored =
        "Offline: restored Plane field catalog from local snapshot. Last fetch failed: HTTP 0 (Couldn't connect)";
    CHECK(ExtractLastFetchFailedDetail(planeRestored) == "HTTP 0 (Couldn't connect)");

    const std::string noSnapshot = "Offline: no field catalog snapshot could be loaded for this tracker context. Last "
                                   "fetch failed: HTTP 0 (Couldn't connect)";
    CHECK(ExtractLastFetchFailedDetail(noSnapshot) == "HTTP 0 (Couldn't connect)");

    const std::string startupSnapshot =
        "Working offline: tracker field catalog loaded from local snapshot until a live refresh succeeds.";
    CHECK(ExtractLastFetchFailedDetail(startupSnapshot).empty());
}

TEST_CASE("IsStartupSnapshotWarning matches every startup wording") {
    CHECK(IsStartupSnapshotWarning(
        "Working offline: tracker field catalog loaded from local snapshot until a live refresh succeeds."));
    CHECK(IsStartupSnapshotWarning(
        "Working offline: Jira field catalog loaded from local snapshot until a live refresh succeeds."));
    CHECK(IsStartupSnapshotWarning(
        "Working offline: Plane field catalog loaded from local snapshot until a live refresh succeeds."));

    CHECK_FALSE(IsStartupSnapshotWarning(
        "Offline: using cached Jira field catalog. Last fetch failed: HTTP 0 (Couldn't connect)"));
    CHECK_FALSE(IsStartupSnapshotWarning(""));
}

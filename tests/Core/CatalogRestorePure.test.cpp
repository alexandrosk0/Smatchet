#include <doctest/doctest.h>

#include "Sync/CatalogRestorePure.h"

#include <string>
#include <vector>

using namespace smatchet::sync;

TEST_CASE("CatalogSnapshotCandidates: scoped key first, unscoped fallback always last") {
    CHECK(CatalogSnapshotCandidates("PROJ", "scoped", "unscoped") == std::vector<std::string>{"scoped", "unscoped"});
    // A view that resolves no project looks only for the unscoped snapshot.
    CHECK(CatalogSnapshotCandidates("", "scoped", "unscoped") == std::vector<std::string>{"unscoped"});
}

TEST_CASE("RestoredSnapshotScope: only the unscoped fallback restores as no project's") {
    CHECK(RestoredSnapshotScope("PROJ", 0, 2) == "PROJ"); // the project's own snapshot loaded
    CHECK(RestoredSnapshotScope("PROJ", 1, 2) == "");     // the fallback loaded
    CHECK(RestoredSnapshotScope("", 0, 1).empty());       // no project resolved: the only candidate is unscoped
}

TEST_CASE("MayApplyRestoredCatalog: a restore never overrides a catalog that landed first or a moved pane") {
    CHECK(MayApplyRestoredCatalog(true, true));
    CHECK_FALSE(MayApplyRestoredCatalog(false, true)); // a live fetch filled the catalog while the file was read
    CHECK_FALSE(MayApplyRestoredCatalog(true, false)); // the pane switched tracker meanwhile
    CHECK_FALSE(MayApplyRestoredCatalog(false, false));
}

TEST_CASE("DecideStartupRestore: nothing saved, or no pane latched, applies nothing") {
    // The grid's own fetch brings the first catalog; the restore never touches the pane.
    CHECK(DecideStartupRestore(false, true, true, true) == StartupRestoreAction::NothingToApply);
    CHECK(DecideStartupRestore(true, false, true, true) == StartupRestoreAction::NothingToApply);
    CHECK(DecideStartupRestore(false, false, true, true) == StartupRestoreAction::NothingToApply);
    // Whatever the pane looks like, an absent restore stays absent.
    CHECK(DecideStartupRestore(false, true, false, false) == StartupRestoreAction::NothingToApply);
}

TEST_CASE("DecideStartupRestore: a restore never overrides a catalog that landed first") {
    CHECK(DecideStartupRestore(true, true, false, true) == StartupRestoreAction::Superseded);
}

TEST_CASE("DecideStartupRestore: a restore read for another tracker is dropped") {
    CHECK(DecideStartupRestore(true, true, true, false) == StartupRestoreAction::Superseded);
    CHECK(DecideStartupRestore(true, true, false, false) == StartupRestoreAction::Superseded);
}

TEST_CASE("DecideStartupRestore: a same-kind site switch during the read drops the restore") {
    // Same tracker kind, another site: the generation is unchanged but the cache key moved, and the site-change
    // reset has already emptied the pane's catalog. The latch must not hold, or the old site's fields land here.
    const bool latchHolds = PaneLatchHolds(7, 7, "Jira@another-site.example", "Jira@jira.example");
    CHECK_FALSE(latchHolds);
    CHECK(DecideStartupRestore(true, true, true, latchHolds) == StartupRestoreAction::Superseded);
    CHECK(DecideStartupRestore(true, true, true, PaneLatchHolds(7, 7, "Jira@jira.example", "Jira@jira.example")) ==
          StartupRestoreAction::Apply);
}

TEST_CASE("DecideStartupRestore: an empty pane on the same backend takes the saved catalog") {
    CHECK(DecideStartupRestore(true, true, true, true) == StartupRestoreAction::Apply);
}

TEST_CASE(
    "PaneLatchHolds: a restore latched at launch lands only while the pane still runs the same tracker and site") {
    CHECK(PaneLatchHolds(7, 7, "Jira@acme", "Jira@acme"));
    CHECK_FALSE(PaneLatchHolds(8, 7, "Jira@acme", "Jira@acme"));  // the backend was swapped, or the pane retired
    CHECK_FALSE(PaneLatchHolds(7, 7, "Jira@other", "Jira@acme")); // another site of the same tracker
    CHECK_FALSE(PaneLatchHolds(7, 7, "Plane", "Jira@acme"));      // another tracker
    CHECK_FALSE(PaneLatchHolds(8, 7, "Plane", "Jira@acme"));
}

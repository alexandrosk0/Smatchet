// Preferences → General → Storage snapshot invalidation (#2044). The section used to stat +
// parse smatchet_storage_mode.txt and re-resolve two paths on EVERY frame it was expanded; it
// now resolves once and re-resolves only when this predicate says the cache is stale. The
// predicate is the correctness of the fix (a frame-count throttle would still serve wrong data
// after a storage-mode change), so it is what gets covered here — via the production symbol
// SmatchetPreferencesUi_General.cpp itself calls.

#include "Ui/SmatchetStorageSnapshotPure.h"

#include <doctest/doctest.h>

#include <string>

using smatchet::prefs_storage::StorageSnapshotStale;

TEST_CASE("StorageSnapshotStale: a never-populated cache is stale") {
    // First draw of the section: nothing cached yet, so the disk resolve must run once.
    CHECK(StorageSnapshotStale(/*cachedValid=*/false, "", "C:/app/runtime/"));
    // Even when a dir happens to match, an explicitly-invalidated cache still refreshes — this
    // is the edge used after a successful SetStoragePreference and on Preferences-window close.
    CHECK(StorageSnapshotStale(/*cachedValid=*/false, "C:/app/runtime/", "C:/app/runtime/"));
}

TEST_CASE("StorageSnapshotStale: a valid cache for the same runtime-asset dir is reused") {
    // The steady-state case the fix exists for: every subsequent frame the section is visible
    // hits this branch and does NO disk I/O.
    CHECK_FALSE(StorageSnapshotStale(/*cachedValid=*/true, "C:/app/runtime/", "C:/app/runtime/"));
    CHECK_FALSE(StorageSnapshotStale(/*cachedValid=*/true, "", ""));
}

TEST_CASE("StorageSnapshotStale: a changed runtime-asset dir invalidates the cache") {
    // The marker file is resolved FROM the runtime-asset dir, so a different dir means a
    // different marker file — the cached preference and both displayed paths no longer apply.
    CHECK(StorageSnapshotStale(/*cachedValid=*/true, "C:/app/runtime/", "D:/portable/runtime/"));
    CHECK(StorageSnapshotStale(/*cachedValid=*/true, "", "C:/app/runtime/"));
    CHECK(StorageSnapshotStale(/*cachedValid=*/true, "C:/app/runtime/", ""));
    // Path comparison is exact — a trailing-separator difference resolves a different string
    // and is treated as stale rather than silently reusing the old marker path.
    CHECK(StorageSnapshotStale(/*cachedValid=*/true, "C:/app/runtime/", "C:/app/runtime"));
}

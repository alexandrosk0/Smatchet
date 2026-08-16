#pragma once

#include <string>

/// Pure invalidation predicate for the Preferences → General → Storage snapshot (#2044).
/// The section used to stat + parse `smatchet_storage_mode.txt` and re-resolve both displayed
/// paths on EVERY frame it was expanded (Pillar-2 CRITICAL). The values are now cached in
/// `UiDrawSession` and refreshed only when this predicate reports the cache stale. Staleness is
/// keyed on the runtime-asset dir the snapshot was resolved FOR — not a frame counter — since a
/// different dir resolves a different marker file. The other invalidation edges are explicit
/// `valid = false` writes (Preferences-window close; a successful `SetStoragePreference`).
namespace smatchet {
namespace prefs_storage {

/// True when the cached snapshot must be re-resolved: never populated / explicitly invalidated
/// (`cachedValid == false`), or resolved for a different runtime-asset directory.
inline bool StorageSnapshotStale(bool cachedValid, const std::string& cachedRuntimeAssetDir,
                                 const std::string& currentRuntimeAssetDir) {
    return !cachedValid || cachedRuntimeAssetDir != currentRuntimeAssetDir;
}

} // namespace prefs_storage
} // namespace smatchet

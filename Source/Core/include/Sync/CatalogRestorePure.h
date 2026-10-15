#pragma once

// Pure rules for restoring the saved field catalog off the UI thread (docs/plans/active/delta-sync.md
// § Review follow-up decisions, Quality Pillar 2). The snapshot file is read on a worker; these decide which
// keys it looks for, which project scope the restored catalog stands for, whether the result may still be
// applied when it lands, and whether the startup fetch decision has to wait for it. Header-only, no I/O.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace smatchet {
namespace sync {

/// Snapshot keys to try, most specific first: the project-scoped key when the active view resolves a project,
/// then the unscoped key, so some catalog is still restored offline for a project that has none saved yet.
inline std::vector<std::string> CatalogSnapshotCandidates(const std::string& projectKey, const std::string& scopedKey,
                                                          const std::string& unscopedKey) {
    std::vector<std::string> keys;
    if (!projectKey.empty()) {
        keys.push_back(scopedKey);
    }
    keys.push_back(unscopedKey);
    return keys;
}

/// The project scope a restored snapshot stands for: `projectKey` when the first (scoped) candidate loaded, "" when
/// the unscoped fallback did (it carries no project-scoped data, so it is no project's). `candidateCount` is the
/// size of the list `CatalogSnapshotCandidates` returned and `loadedIndex` the candidate that loaded.
inline std::string RestoredSnapshotScope(const std::string& projectKey, std::size_t loadedIndex,
                                         std::size_t candidateCount) {
    return loadedIndex + 1 == candidateCount ? std::string() : projectKey;
}

/// True when a restore that finished reading may still be applied: the pane's catalog is still empty (a live
/// fetch or another restore that landed first wins, never the older saved copy) and the pane still runs the
/// backend the restore was read for.
inline bool MayApplyRestoredCatalog(bool catalogEmptyNow, bool backendGenerationUnchanged) {
    return catalogEmptyNow && backendGenerationUnchanged;
}

/// True when work latched at launch for one pane (a failed refresh's snapshot restore, which is read on a worker
/// and lands later on the UI thread) still belongs to that pane when it lands: the pane's backend generation and its
/// cache namespace key are both what they were at launch. A tracker or site switch (generation, key) or a retired
/// pane (generation) makes the result another tracker's, which must not be written into this pane's catalog.
inline bool PaneLatchHolds(std::uint64_t generationNow, std::uint64_t generationLatched, const std::string& cacheKeyNow,
                           const std::string& cacheKeyLatched) {
    return generationNow == generationLatched && cacheKeyNow == cacheKeyLatched;
}

/// True while the startup fetch-or-skip decision has to wait: it reads the restored snapshot's age, project scope
/// and columns, so deciding before the restore lands would fetch live a catalog that was about to be served.
inline bool StartupCatalogDecisionMustWait(bool startupRestorePending) { return startupRestorePending; }

} // namespace sync
} // namespace smatchet

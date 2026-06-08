#pragma once

// GridPaneEvictionPolicy — the pure "which hidden pane's in-memory ticket snapshot to drop next"
// decision for the multi-grid hidden-pane LRU memory cap (plan multi-grid-tabs Slice 5a, plan
// item 21). VISIBLE / focused panes are NEVER evicted and never count toward the cap; a pane
// whose sync worker is live is NEVER evicted (the worker may be applying to its ActiveTickets —
// Pillar 3 never-crash). The chosen victim's rows stay durable in tickets_v2, so re-showing it
// re-seeds losslessly (Deliverable 1). Pure so it is unit-testable without an AppController, a
// SQLite cache, or an ImGui frame — mirrors CacheEvictionPolicy.h.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace smatchet {

struct HiddenPaneEvictionCandidate {
    std::string paneId;
    /// Visible / focused this frame — never evicted, never counted toward the cap.
    bool isVisible = false;
    /// A context whose streaming-sync worker is live — never evicted (a worker may mutate its
    /// ActiveTickets concurrently; freeing the rows would be a Pillar-3 use-after-free).
    bool hasActiveSync = false;
    /// Holds an in-memory ActiveTickets snapshot — only resident panes count toward the cap and
    /// only resident panes are evictable (an already-evicted husk is skipped).
    bool hasResidentRows = false;
    /// Monotonic recency stamp bumped each frame the pane is visible; smaller == less recently
    /// visible == evicted first.
    std::uint64_t lastVisibleOrder = 0;
};

/// Returns the paneId of the least-recently-visible HIDDEN, idle, resident pane to evict when the
/// number of hidden resident panes exceeds `maxHiddenResident`, else the empty string ("none").
/// Visible panes are excluded from both the count and the candidate set; busy-sync panes count
/// toward the cap but are not evictable (retried next frame once idle). Ties on lastVisibleOrder
/// break by smaller paneId for determinism. The caller loops (rebuilding candidates / flipping
/// the chosen victim's hasResidentRows) until this returns "" to evict down to the cap.
inline std::string ChooseHiddenPaneToEvict(const std::vector<HiddenPaneEvictionCandidate>& candidates,
                                           std::size_t maxHiddenResident) {
    std::size_t hiddenResident = 0;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (!candidates[i].isVisible && candidates[i].hasResidentRows) {
            ++hiddenResident;
        }
    }
    if (hiddenResident <= maxHiddenResident) {
        return std::string();
    }
    const HiddenPaneEvictionCandidate* victim = nullptr;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const HiddenPaneEvictionCandidate& c = candidates[i];
        if (c.isVisible || c.hasActiveSync || !c.hasResidentRows) {
            continue;
        }
        if (victim == nullptr || c.lastVisibleOrder < victim->lastVisibleOrder ||
            (c.lastVisibleOrder == victim->lastVisibleOrder && c.paneId < victim->paneId)) {
            victim = &c;
        }
    }
    return victim ? victim->paneId : std::string();
}

} // namespace smatchet

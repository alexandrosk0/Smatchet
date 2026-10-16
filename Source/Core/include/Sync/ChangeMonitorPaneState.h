#pragma once

// ChangeMonitorPaneState - what the change monitor remembers about one pane between its cycles
// (docs/plans/active/delta-sync.md § Phase 6). UI-thread-only bookkeeping, held by GridLiveContext and cleared
// when the pane's next reload finishes. Header-only, C++14, standard library only, so the context header can
// include it without pulling the reader interface in.

#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

namespace smatchet {
namespace sync {

/// What one probe cycle learned about its pane, for the main-thread apply hop to record.
struct ChangeProbeCycleNotes {
    /// The roster path cannot serve this pane: the walk was cut short (the view is larger than the reload's caps),
    /// or more keys are refused than one cycle can isolate. Neither changes before a reload, so asking again every
    /// cycle only wastes the capped walk or the failing keyed downloads.
    bool RosterIncomplete = false;
    /// More bodies had changed than one cycle downloads (kMaxMonitorBodiesPerCycle): the rest wait for a later
    /// cycle, so the cycle must not move the legacy path's changed-since anchor past them.
    bool BodiesCapped = false;
    /// Keys the tracker refused to serve this cycle.
    std::vector<std::string> RefusedKeys;
};

struct ChangeMonitorPaneState {
    /// A refused key stays in the roster diff every cycle (its stamp never matches a held row), and isolating it
    /// costs about 2 * log2(n) extra requests each time. The set is bounded so a hostile tracker cannot grow it.
    /// A function, not a `static constexpr` member: at C++14 such a member needs an out-of-line definition once it is
    /// bound by reference (a CHECK, std::min).
    static std::size_t MaxRememberedRefusals() { return 256; }

    bool RosterIncomplete = false;
    std::unordered_set<std::string> RefusedKeys;
    /// The ids the pane's last complete reload got from the view's own query (the rest of the grid is the hierarchy
    /// walk's parents and children). Unknown until a complete reload has finished. Survives ResetForReload: the next
    /// complete reload replaces it.
    std::unordered_set<std::string> ViewListed;
    bool ViewListedKnown = false;

    /// The view's own rows after a complete reload.
    void SetViewListing(const std::vector<std::string>& ids) {
        ViewListed.clear();
        ViewListed.insert(ids.begin(), ids.end());
        ViewListedKnown = true;
    }

    /// Rows the monitor added to the grid because the view's roster listed them.
    void NoteViewRowsAdded(const std::vector<std::string>& ids) {
        if (ViewListedKnown) {
            ViewListed.insert(ids.begin(), ids.end());
        }
    }

    /// Whether the next cycle should try the roster path: the feature is on, the backend can list versions, and
    /// an earlier cycle did not already find this pane's roster incomplete.
    bool ShouldTryRoster(bool deltaEnabled, bool backendSupportsRoster) const {
        return deltaEnabled && backendSupportsRoster && !RosterIncomplete;
    }

    void Absorb(const ChangeProbeCycleNotes& notes) {
        if (notes.RosterIncomplete) {
            RosterIncomplete = true;
        }
        for (const std::string& key : notes.RefusedKeys) {
            if (RefusedKeys.size() >= MaxRememberedRefusals()) {
                break;
            }
            RefusedKeys.insert(key);
        }
    }

    /// The pane finished a reload: its view, its size and its permissions may all have changed.
    void ResetForReload() {
        RosterIncomplete = false;
        RefusedKeys.clear();
    }

    /// The legacy changed-since window starts at the anchor, so a cycle that left changed rows undownloaded must
    /// not advance it: a later fall-back to that path would otherwise never see them.
    static bool AnchorMayAdvance(const ChangeProbeCycleNotes& notes) { return !notes.BodiesCapped; }
};

} // namespace sync
} // namespace smatchet

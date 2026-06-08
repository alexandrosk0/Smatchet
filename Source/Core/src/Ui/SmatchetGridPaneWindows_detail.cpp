// SmatchetGridPaneWindows_detail — pure, ImGui-free pane-management core
// (multi-grid-tabs Slice 2, ADR-0018). Split out of SmatchetGridPaneWindows.cpp
// so bucket-A tests (tests/Core/GridPaneRequests.test.cpp) cover the close/add/
// focus-reassignment invariants without linking the UI session TU — the same
// seam pattern as SmatchetGridHeaderUi_detail.cpp.

#include "SmatchetGridPaneWindows.h"

#include <string>
#include <vector>

namespace SmatchetGridPaneWindows {

std::string GenerateUniquePaneId(const std::vector<GridPane>& panes) {
    for (int n = 2;; ++n) {
        const std::string candidate = "pane-" + std::to_string(n);
        bool taken = false;
        for (const auto& pane : panes) {
            if (pane.id == candidate) {
                taken = true;
                break;
            }
        }
        if (!taken) {
            return candidate;
        }
    }
}

namespace detail {

PaneRequestApplyOutcome ApplyPaneAddAndCloseRequestsCore(std::vector<GridPane>& panes, std::string& focusedPaneId,
                                                         std::string& addRequestSourceId) {
    PaneRequestApplyOutcome outcome;

    // Close sweep — windows whose tab X was clicked wrote pane.open = false.
    // Min-1 invariant: never let the sweep empty the vector. The survivor keeps
    // its OWN identity (viewId/backendKey untouched); the focus hand-over is
    // REPORTED via FocusReassigned so the host replays it as a real focus switch
    // (review HIGH-2 — silently moving focus left the survivor's saved view
    // inactive and the steady-state sync then rebound it to the closed pane's view).
    for (size_t i = 0; i < panes.size();) {
        if (!panes[i].open && panes.size() > 1) {
            const bool wasFocused = (panes[i].id == focusedPaneId);
            panes.erase(panes.begin() + static_cast<std::ptrdiff_t>(i));
            if (wasFocused) {
                focusedPaneId = panes.front().id;
                outcome.FocusReassigned = true;
            }
            outcome.Changed = true;
        } else {
            panes[i].open = true; // re-arm the survivor (covers the last-pane case)
            ++i;
        }
    }

    // "+" request — duplicate the source pane (same backend + view; the new pane
    // shares the source's snapshot pointer, so opening costs no ticket copy).
    if (!addRequestSourceId.empty()) {
        const GridPane* src = FindGridPaneById(panes, addRequestSourceId);
        if (src == nullptr) {
            src = &panes.front();
        }
        GridPane dup;
        dup.id = GenerateUniquePaneId(panes);
        dup.title = src->title;
        dup.backendKey = src->backendKey;
        dup.viewId = src->viewId;
        dup.ticketsSnapshot = src->ticketsSnapshot;
        dup.snapshotRevision = src->snapshotRevision;
        panes.push_back(dup); // invalidates `src` — done reading it above
        focusedPaneId = panes.back().id;
        outcome.FocusReassigned = true;
        addRequestSourceId.clear();
        outcome.Changed = true;
    }

    return outcome;
}

bool PaneViewSelfRepairAllowed(const std::string& paneBackendKey, const std::string& cfgBackendKey) {
    // Empty pane key = pre-bootstrap placeholder pane — repair allowed (Pillar 3).
    // Otherwise only a pane in the currently-loaded backend bucket may have its
    // viewId rebound; a cross-backend pane's viewId is valid in its own bucket
    // (review HIGH-1 — the unconditional repair clobbered it every frame and the
    // debounced persist made the loss durable).
    return paneBackendKey.empty() || paneBackendKey == cfgBackendKey;
}

PaneColumnsSource ChoosePaneColumnsSource(const std::string& paneViewId, const std::string& resolvedViewId,
                                          const std::string& activeViewId, bool cachedColumnsValid,
                                          const std::string& cachedColumnsViewId) {
    // Strict ownership: resolvePaneView may hand back the ACTIVE view as a render
    // fallback for a cross-backend pane (its own bucket isn't loaded) — that view is
    // only "the pane's own" when the ids match exactly (empty pane id never owns).
    const bool viewIsPanesOwn = !paneViewId.empty() && resolvedViewId == paneViewId;
    if (viewIsPanesOwn) {
        return (paneViewId == activeViewId) ? PaneColumnsSource::SharedActive : PaneColumnsSource::OwnViewBuild;
    }
    // Unresolvable own view: keep the column set captured while it WAS resolvable. Frozen
    // until refocus reloads the pane's bucket. The cold-start hole (no session capture after a
    // restart) is closed by the pane context's resolvedOwnView upgrade in
    // SmatchetUI::resolvePaneColumns. Per-pane catalog VALUE routing stays deferred (it needs
    // per-context catalog population first — see docs/self-improvement/categories/debt.md).
    if (cachedColumnsValid && !paneViewId.empty() && cachedColumnsViewId == paneViewId) {
        return PaneColumnsSource::CachedFrozen;
    }
    return PaneColumnsSource::SharedFallback;
}

bool ShouldBuildColumnsFromOwnResolvedView(PaneColumnsSource source, const std::string& paneViewId,
                                           const std::string& ownResolvedViewId) {
    return source == PaneColumnsSource::SharedFallback && !paneViewId.empty() && ownResolvedViewId == paneViewId;
}

} // namespace detail

} // namespace SmatchetGridPaneWindows

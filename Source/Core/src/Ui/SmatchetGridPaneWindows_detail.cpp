// SmatchetGridPaneWindows_detail — pure, ImGui-free pane-management core
// (multi-grid-tabs Slice 2, ADR-0018). Split out of SmatchetGridPaneWindows.cpp
// so bucket-A tests (tests/Core/GridPaneRequests.test.cpp) cover the close/add/
// focus-reassignment invariants without linking the UI session TU — the same
// seam pattern as SmatchetGridHeaderUi_detail.cpp.

#include "SmatchetGridPaneWindows.h"

#include <algorithm>
#include <string>
#include <unordered_map>
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

std::string NextPaneId(const std::vector<GridPane>& panes, const std::string& currentId) {
    if (panes.empty()) {
        return std::string();
    }
    for (size_t i = 0; i < panes.size(); ++i) {
        if (panes[i].id == currentId) {
            return panes[(i + 1) % panes.size()].id;
        }
    }
    return panes.front().id; // current not found → safe fallback to the first pane.
}

std::string PrevPaneId(const std::vector<GridPane>& panes, const std::string& currentId) {
    if (panes.empty()) {
        return std::string();
    }
    for (size_t i = 0; i < panes.size(); ++i) {
        if (panes[i].id == currentId) {
            return panes[(i + panes.size() - 1) % panes.size()].id;
        }
    }
    return panes.front().id;
}

namespace detail {

std::string ResolveNewPaneView(const std::string& backendKey, const std::string& requestedViewId,
                               const std::unordered_map<std::string, ViewWorkspaceState>& viewBuckets) {
    const auto it = viewBuckets.find(backendKey);
    if (it == viewBuckets.end() || it->second.Views.empty()) {
        return std::string();
    }
    const ViewWorkspaceState& ws = it->second;
    if (!requestedViewId.empty()) {
        const auto found = std::find_if(ws.Views.begin(), ws.Views.end(),
                                        [&](const ViewDefinition& v) { return v.Id == requestedViewId; });
        if (found != ws.Views.end()) {
            return requestedViewId;
        }
    }
    if (!ws.ActiveViewId.empty()) {
        return ws.ActiveViewId;
    }
    return ws.Views.front().Id;
}

PaneRequestApplyOutcome
ApplyPaneAddAndCloseRequestsCore(std::vector<GridPane>& panes, std::string& focusedPaneId, PaneAddRequest& addRequest,
                                 const std::unordered_map<std::string, ViewWorkspaceState>& viewBuckets) {
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

    // "+" request — create a new pane from the source. Same-backend: duplicates
    // backend/view/snapshot (cheap shared_ptr copy). Cross-backend: sets the target
    // backend + resolves a view; NO snapshot inherit (different data).
    if (!addRequest.sourceId.empty()) {
        const GridPane* src = FindGridPaneById(panes, addRequest.sourceId);
        if (src == nullptr) {
            src = &panes.front();
        }
        GridPane dup;
        dup.id = GenerateUniquePaneId(panes);
        dup.title = src->title;
        // Inherit the source pane's LIVE dock node so the new window opens as a TAB next to
        // the pane whose "+" was clicked — following the source even after the user has
        // dragged it somewhere else. 0 (source floating / never drawn) means "no hand-off":
        // ImGui cannot tab a window into a node that does not exist, so the new pane falls
        // back to its own first-use placement. Plain ImGuiID-by-value keeps this core
        // ImGui-free. Tab selection is forced for a few frames because docking alone leaves
        // the new tab behind its sibling (imgui #2304).
        dup.pendingDockId = src->lastDockId;
        dup.selectTabFrames = 4;
        const bool crossBackend =
            !addRequest.targetBackendKey.empty() && addRequest.targetBackendKey != src->backendKey;
        if (crossBackend) {
            dup.backendKey = addRequest.targetBackendKey;
            dup.viewId = ResolveNewPaneView(addRequest.targetBackendKey, addRequest.targetViewId, viewBuckets);
        } else {
            dup.backendKey = src->backendKey;
            dup.viewId = src->viewId;
            dup.ticketsSnapshot = src->ticketsSnapshot;
            dup.snapshotRevision = src->snapshotRevision;
        }
        panes.push_back(dup); // invalidates `src` — done reading it above
        focusedPaneId = panes.back().id;
        outcome.FocusReassigned = true;
        addRequest = PaneAddRequest{};
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

unsigned int ResolveExtraPaneDockTarget(unsigned int& pendingDockId, unsigned int liveCentralNodeId,
                                        bool layoutResetSettling) {
    if (layoutResetSettling) {
        // The reset owns the placement for the whole settle window. Drop any "+" hand-off
        // unconditionally: the tree rebuild destroyed that source node, and docking into a
        // dead id mints an orphan root node rather than failing (SmatchetDockNodeIds.h).
        // Re-force the central node EVERY frame — the freshly loaded node is not
        // LastFrameAlive on the reset frame, so a single write is silently dropped by
        // ImGui's liveness guard. A 0 here (node not live yet) simply forces nothing this
        // frame; the caller re-asks next frame, still inside the settle window.
        pendingDockId = 0;
        return liveCentralNodeId;
    }
    if (pendingDockId != 0) {
        const unsigned int target = pendingDockId;
        pendingDockId = 0; // consume-once — from here the user owns the placement
        return target;
    }
    return 0;
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
    // SmatchetUI::resolvePaneColumns. Per-pane catalog VALUE routing now lands via
    // ChoosePaneCatalogSource + resolvePaneCatalog (per-context catalog population fetches the
    // pane's own backend catalog into GridLiveContext::fieldCatalog); backend Reader display
    // resolution (ResolveDisplayValue) remains focused-backend-routed (separate follow-up).
    if (cachedColumnsValid && !paneViewId.empty() && cachedColumnsViewId == paneViewId) {
        return PaneColumnsSource::CachedFrozen;
    }
    return PaneColumnsSource::SharedFallback;
}

bool ShouldBuildColumnsFromOwnResolvedView(PaneColumnsSource source, const std::string& paneViewId,
                                           const std::string& ownResolvedViewId) {
    return source == PaneColumnsSource::SharedFallback && !paneViewId.empty() && ownResolvedViewId == paneViewId;
}

PaneCatalogSource ChoosePaneCatalogSource(bool paneFocused, bool paneCatalogPopulated) {
    // Focused pane is the focused context — its catalog IS the shared focused catalog, so
    // there is nothing to route. A non-focused pane routes to its OWN catalog only once
    // populated; until then the shared focused catalog is the safe (unchanged) fallback.
    if (paneFocused || !paneCatalogPopulated) {
        return PaneCatalogSource::SharedFocused;
    }
    return PaneCatalogSource::OwnContext;
}

} // namespace detail

} // namespace SmatchetGridPaneWindows

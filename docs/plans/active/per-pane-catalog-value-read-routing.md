# Plan — Per-pane catalog value-read routing (PR-21)

> **Slug**: `per-pane-catalog-value-read-routing` (matches this file's basename without `.md`).
>
> **Status**: `active` — from `docs/plans/backlog-pr-roadmap.md` PR-21 (`per-pane-catalog-value-read-routing`). Owner: grid-engine. Behaviour change on the grid value-read path → needs bucket-E / visual validation to land.

## Context

In a multi-grid / side-by-side layout, each pane can be bound to a different tracker backend, but the field-value read path does not currently route through a per-pane field catalog: `resolvePaneCatalog` / `ChoosePaneCatalogSource` do **not** exist in the tree (verified 2026-07-14). A non-focused or cross-backend pane can therefore read/format field values against the wrong (focused or default) catalog, producing wrong column formatting/display for that pane's backend.

Intended outcome: *after this lands, every pane resolves and reads its field values against its own backend's catalog — seeded independently at context-live time and refreshed per-pane on first sync — so cross-backend side-by-side grids format each pane's fields correctly.*

## Approach

Two steps, in order. **Step 1 — populate each pane context's catalog independently.** At `EnsurePaneContextLive`, seed the pane's `fieldCatalog` cross-backend from `FieldCatalogCache` (so a freshly-live pane starts with the right shape), and route the pane's first sync into *that context's* catalog rather than into the shared/default catalog. **Step 2 — (re-)land the read routing.** Add `resolvePaneCatalog(paneId)` (returns the pane context's catalog, falling back to focused/default only when a pane has none yet) and `ChoosePaneCatalogSource` (the selection policy), and point the grid value-read/format call sites at them.

Trade-off: this is a hot-path change (grid cell formatting runs every frame per visible pane), so catalog resolution must be an O(1) per-pane lookup off already-live context state — no per-cell catalog rebuild, and the per-pane first-sync must stay off the UI thread with a visible cue, per Pillars 1–2.

## Files to modify

1. `Source/Core/src/AppController_PaneContexts.cpp` — `EnsurePaneContextLive`: seed the new context's `fieldCatalog` from `FieldCatalogCache` for the pane's backend; route first-sync into the context's catalog.
2. `Source/Core/include/GridLiveContext.h` — ensure the per-pane context carries its own `fieldCatalog` (confirm/extend) as the read-routing source of truth.
3. `Source/Core/src/Ui/SmatchetActiveProjectGridUi.cpp` + `Source/Core/src/Ui/SmatchetGridPaneWindows.cpp` — point the value-read/format call sites at `resolvePaneCatalog(paneId)` instead of the focused/default catalog.
4. New `resolvePaneCatalog` / `ChoosePaneCatalogSource` (in `AppController_CatalogAndFieldEdit.cpp` or a small grid-engine helper — grep to avoid a duplicate) — the resolution + source-selection policy.
5. `Source/Core/include/Tracker/FieldCatalogCache.h` consumers — reuse the existing cache API for the cross-backend seed; do not add a parallel cache.

## Existing utilities reused

- `FieldCatalogCache` — `Source/Core/include/Tracker/FieldCatalogCache.h` — the cross-backend catalog seed source; reuse for the per-pane seed.
- `AppController::EnsurePaneContextLive` — `Source/Core/src/AppController_PaneContexts.cpp` — the pane-context lifecycle hook the seed attaches to (multi-grid Slice 3).
- `AppController::paneContextOrFocused_` — `Source/Core/include/AppController.h:1141` — the existing focused-fallback accessor the resolver's fallback branch mirrors.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — adds a resolver + per-pane seed; extracts/splits nothing.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms)**: grid cell formatting is per-frame per-visible-pane; catalog resolution must be an O(1) per-pane lookup off live context state, not a per-cell rebuild. Validate against the side-by-side grid perf scenario.
- **Pillar 2 (UI-thread never blocks > 100 ms)**: the per-pane first-sync (catalog fetch for a newly-live pane's backend) is network I/O — it must run on a worker with a visible sync cue; the UI reads whatever catalog the context currently holds (seed → refined on sync completion). No sync-I/O on the ImGui path.
- **Pillar 3 (never crash)**: `resolvePaneCatalog` must tolerate a pane with no catalog yet (fallback to focused/default) and a retired-mid-frame context (the existing `paneContextOrFocused_` concurrency contract) — never deref a dead context.
- **Pillar 4 (accessibility)**: no direct impact; correct per-pane formatting improves data legibility.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A`)

Fires — the diff touches `Source/Core/src/Ui/` grid draw + `AppController` pane contexts.

1. **PR-fast CI** — scenario: `side-by-side-2-grid` (and `concurrent-sync` for the multi-pane live-sync path); these most directly exercise per-pane catalog reads. Confirm both are in `scripts/dev/perf-pr-fast-set.json` or add.
2. **Pillar 2 static scanner** — the per-pane first-sync is sync-I/O; it must be worker-thread with a `/* PILLAR2_WORKER_ONLY */ // est-latency: <N>ms` annotation. No new sync-I/O reachable from `ImGui::*`.
3. **Dispatcher drain** — does not touch `MainThreadDispatcher::Drain()` (catalog resolution is a pure lookup); confirm at implementation.
4. **Visible-cue bucket-E harness** — the per-pane first-sync adds a >100 ms path for a newly-live pane; assert the existing sync-stall visible cue covers it (a new pane shows the loading cue, not a frozen blank grid).
5. **Marker inventory** — if a `SMATCHET_UI_PERF_SCOPE` is added around the resolver, regen `docs/perf/MARKER_INVENTORY.md` in the same PR.

**Pre-push local check**: run the `side-by-side-2-grid` scenario gate-check vs baseline before opening the PR.

## Risks / non-goals

- **Risk: per-cell catalog rebuild tanks frame time.** Mitigation: resolve once per pane per frame (or cache on the context), never per cell.
- **Risk: a pane reads a stale seed before its first sync completes.** Accepted — the seed is the correct *shape* from `FieldCatalogCache`; sync refines it. Document the seed→refine ordering.
- **Risk: retired-context race on read.** Mitigation: route through the existing `paneContextOrFocused_` liveness contract; never hold a raw context pointer across the frame.
- **Non-goal: changing the catalog *data model*.** This routes reads to the right existing catalog; it does not redesign `FieldCatalogCache`.
- **Non-goal: write/edit routing.** Field-edit commit routing is separate; this plan is the value-*read* path only.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: a unit test for `ChoosePaneCatalogSource` / `resolvePaneCatalog` fallback policy (pane-with-catalog → own; pane-without → focused/default) as a pure function over context state.
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: a two-pane cross-backend fixture asserting each pane formats a backend-specific field against its own catalog (the behaviour-change proof).
- **Bash-driver scenario / screenshot / sanitizer**: `side-by-side-2-grid` perf scenario green vs baseline; screenshot diff on a cross-backend two-pane grid.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target).
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: `scripts/dev/test-docs.sh` suite green.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: confirm the resolver is O(1)/pane and the first-sync is worker-gated before finalising.
- **Manual residue**: the cross-backend two-pane visual validation is a one-time reviewer approval (grid behaviour change) — named here, not silent.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — no symbols are deferred by this plan.

- **Plane/GitHub-specific catalog quirks** — the resolver is backend-agnostic; per-backend catalog-shape fixes ride their own entries, not this routing change.

## Implementation log
*(populated post-ship — bullet per shipped commit)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*In the SAME PR that fills the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*(Delete this `## Archive` block as part of step 2.)*

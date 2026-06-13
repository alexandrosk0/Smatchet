# Plan — Pillar-2 UI-thread blocking elimination (future-destructor + sync-I/O cluster)

> **Slug**: `ui-freeze-pillar2-blocking` (matches this file's basename without `.md`).
>
> **Status**: `active`

## Context

A cluster of open GitHub Issues all violate **UX Pillar 2** (no UI-thread block > 100 ms without a visible cue). Two mechanisms:

- **(A) Blocking `std::future` destructors on the UI thread** — destroying a `future`/`vector<future>` whose worker is still running blocks until the worker drains.
  - **#734** (P2) — `bulkImportFutures.clear()` at `SmatchetBulkTicketsUi.cpp:132/226/311` blocks during *normal operation*.
  - **#1150** (P3) — `SmatchetUserInfoUi`'s 4 future members (`vcsFuture_`/`activityFuture_`/`groupsFuture_`/`membersFuture_`, `SmatchetUserInfoUi.h:108-111`) block at *app shutdown* (p4 annotate / network can stall teardown several seconds). NB: the block is currently the *safety mechanism* keeping the worker's captured `appPtr` valid (`SmatchetUI Ui` is destroyed before `AppController`) — any fix must preserve that no-UAF guarantee.
- **(B) Sync I/O reaching the ImGui render path** — **#1001** tracking issue, 5 sites alive on develop:
  - **#611** `SmatchetToolbarUi.cpp` RefreshTrackerAppendCache → `LoadPersistentViewsFromDisk` (ifstream+JSON under IoMutex+ScopedFileLock) from RenderBar (memoized; blocks on the memo-miss frame).
  - **#761** `AnnotateAnalysisUi_Window.cpp` DrawCallstackProcessControls runs `p4 changes -r -m1 //...@a,b` synchronously on confirm.
  - **#732** `SmatchetPreferencesUi_Templates.cpp` duration/work-log sub-tabs call `ConfigManager::Save` (RMW + DPAPI + disk under 2 mutexes) synchronously per reorder/delete/add click.
  - **#767** `SmatchetViewsDashboardUi_widgets.cpp` `ListCachedProjects()` (ifstream+parse+migrate+sort) every frame the project-pill popup is open.
  - **#892** `SmatchetPreferencesUi.cpp` DrawTrackerRecentProjects → `ListCachedProjects()` every frame the Tracker tab is open.

Intended outcome: after this lands, no UI-thread frame blocks on future-drain or sync I/O for these paths; each offloaded path shows a visible in-progress cue.

## Approach

Two workstreams under one plan (shared Pillar-2 goal + the same accepted offload toolkit), shippable as **separate PRs** (different subsystems, no shared seam beyond the helper).

**WS-A — non-blocking future ownership.** Introduce one reusable helper rather than hand-rolling per owner. Proposed: a process-scoped **`UiFutureGraveyard`** — owners `std::move` a finished-or-abandoned `future` into it instead of destroying it inline; a single background drainer (joined once at `AppController` teardown, mirroring `DrainUiDrawSessionFuturesBeforeAppTeardown`) waits on them off the UI thread. This makes the *clear* (#734) and *window close* (#1150 during-run) non-blocking, while preserving the no-UAF guarantee at process exit (the drainer is still joined before `AppController` dies). Workers that capture `appPtr` stay valid because teardown still joins. Open design question (grill target #1) below.

**WS-B — sync-I/O offload audit.** Apply the three already-accepted in-tree patterns site-by-site: `snapshot-on-open` into `UiDrawSession` (#767, #892 — the per-frame `ListCachedProjects()` reads), `MarkPrefsDirty` deferred-save (#732 — match the sibling tabs that already defer), `LaunchBackgroundTask`+`PostToMainThread` with a spinner cue (#761 p4 round-trip; #611 memo-miss `LoadPersistentViewsFromDisk`). No new pattern invented.

Trade-off named: WS-A's graveyard centralizes drain but still *blocks at shutdown* (acceptable — bounded, and #1150's slowness is the shutdown case; the win is non-blocking during normal operation). A fully cooperative cancellation (stop-token each worker polls) would also speed shutdown but requires touching every worker body — deferred unless grilling says shutdown latency must improve too.

## Files to modify

WS-A:
1. `Source/Core/include/Ui/UiFutureGraveyard.h` + `Source/Core/src/Ui/UiFutureGraveyard.cpp` — new helper (grep-confirmed absent: `rg -l UiFutureGraveyard Source/` → none).
2. `Source/Core/src/Ui/SmatchetBulkTicketsUi.cpp:132/226/311` — move-into-graveyard instead of `.clear()`.
3. `Source/Core/include/Ui/SmatchetUserInfoUi.h:108-111` + its `.cpp` — graveyard the 4 futures on window-close.
4. `Source/Core/src/Ui/SmatchetUI_Layout.cpp:248` (`DrainUiDrawSessionFuturesBeforeAppTeardown`) — also drain the graveyard at teardown.

WS-B (one site per row; each its own commit, batchable):
5. `SmatchetViewsDashboardUi_widgets.cpp` (#767) + `SmatchetPreferencesUi.cpp` (#892) — snapshot `ListCachedProjects()` on popup/tab open into `UiDrawSession`.
6. `SmatchetPreferencesUi_Templates.cpp` (#732) — swap sync `ConfigManager::Save` for `MarkPrefsDirty`.
7. `AnnotateAnalysisUi_Window.cpp` (#761) + `SmatchetToolbarUi.cpp` (#611) — `LaunchBackgroundTask`+`PostToMainThread` + spinner cue.

## Existing utilities reused

- `DrainUiDrawSessionFuturesBeforeAppTeardown` (`SmatchetUI_Layout.cpp:248`, decl `SmatchetUI.h:70`) — the shutdown-join model WS-A's graveyard drainer mirrors.
- `AppController::LaunchBackgroundTask` + `MainThreadDispatcher::PostToMainThread` — the worker-offload primitive (joined at shutdown via `JoinBackgroundTasks`).
- `MarkPrefsDirty` deferred-save (already used by sibling Preferences tabs) — the #732 fix.
- `UiDrawSession` snapshot fields — the snapshot-on-open target for #767/#892.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms)**: WS-B removes per-frame `ListCachedProjects()` disk reads (#767/#892) from the hot popup/tab path — net steady-state improvement. No new per-frame cost (snapshot is read once on open).
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: the entire point. WS-A removes future-destructor blocks; WS-B removes sync I/O from the render path. Every offloaded long op (#761 p4, #611 view-load) gets a spinner/in-progress cue.
- **Pillar 3 (never crash)**: WS-A must preserve the no-UAF guarantee — the graveyard drainer is joined before `AppController` teardown so a worker's captured `appPtr` never dangles. Sanitizer build must stay clean. This is the highest-risk area (lifetime).
- **Pillar 4 (accessibility)**: N/A — no new interactive surface (spinners are non-interactive cues).

## Perf-review-system gates (mandatory — diff touches `Source/Core/`)

1. **PR-fast CI** — scenarios: `bulk-import` (WS-A #734), `preferences-*` (#732/#892), `views-dashboard` (#767), `toolbar-*` (#611). Map each in `perf-gatekeeper.md` § Curated diff → scenario map; declare the subset in `perf-pr-fast-set.json` per PR.
2. **Pillar 2 static scanner** — WS-B explicitly *removes* sync-I/O-reachable-from-`ImGui::*` sites; the scanner should go from flagging these to clean. WS-A: confirm no new sync I/O reachable from a draw fn.
3. **Dispatcher drain** — WS-A interacts with `MainThreadDispatcher::Drain()` (PostToMainThread completions) and the teardown join — exercise both.
4. **Visible-cue bucket-E harness** — #761/#611 add a >100 ms offloaded path → each needs a visible-cue bucket-E test asserting the spinner renders while the worker runs.
5. **Marker inventory** — if any `SMATCHET_UI_PERF_SCOPE` markers are added, regen `docs/perf/MARKER_INVENTORY.md` in the same PR.

**Pre-push**: run `perf-workflow.md` § Gate-check vs baseline against the named scenarios before each PR.

## Risks / non-goals

- **Risk (Pillar-3, WS-A lifetime)**: a graveyard'd future whose worker captured `appPtr` must not outlive `AppController`. Mitigation: drainer joined in the existing teardown path *before* `AppController` dtor; sanitizer (ASan) bucket run on the WS-A PR. **This is the gate-the-design risk** — grill target #1.
- **Risk**: WS-B snapshot-on-open can show stale data if the underlying file changes while open. Accepted — these are cached-project / view lists; a refresh-on-reopen is fine (document it).
- **Non-goal**: fully cooperative worker cancellation (stop-token) to speed *shutdown* latency — deferred (touches every worker body). Revisit if grilling says shutdown speed matters.
- **Non-goal**: the inverse-asymmetry tracker findings (#943-related) — different subsystem, separate PR.

## Verification

- **Bucket A (pure-logic ctest)**: `UiFutureGraveyard` move/drain semantics (a stub future that signals when waited) — assert move-in is non-blocking, drain joins.
- **Bucket E (ImGui Test Engine)**: visible-cue tests for #761/#611 (spinner renders while worker runs); a #734 test that `bulkImportFutures` clear returns within one frame budget while a stub worker is still "running".
- **Bash-driver / sanitizer**: ASan bucket run on the WS-A PR (Pillar-3 lifetime) — no UAF at simulated mid-fetch shutdown (#1150 repro: trigger a User Info fetch, close the app, assert clean teardown).
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target).
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: `scripts/dev/test-docs.sh` green.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: grill the open design questions below with the user before WS-A implementation. Required — do not delete.
- **Manual residue**: none expected; #1150's mid-fetch-shutdown repro is automatable via the sanitizer bucket above.

## Open design questions (grill targets)

1. **WS-A shape** — process-scoped `UiFutureGraveyard` (drainer joined at teardown; non-blocking during run, still blocks at shutdown) **vs** per-worker cooperative cancellation (stop-token; also speeds shutdown but touches every worker)? Recommendation: graveyard first (smaller, reusable), cancellation deferred.
2. **#1150 priority** — is the *shutdown* stall worth fixing now (needs cancellation), or is making the *during-run* window-close non-blocking enough (graveyard alone leaves shutdown still bounded-blocking)?
3. **WS-B batching** — one PR for all 5 sites, or split (snapshot-on-open pair / MarkPrefsDirty / LaunchBackgroundTask pair)?
4. **Sequencing** — WS-A (the reusable helper) before WS-B, or independent/parallel?

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, `docs/self-improvement/categories/` for stray refs to anything deferred here.

- Cooperative worker cancellation (stop-token) — follow-up if shutdown latency must improve.
- A general Pillar-2 static-scanner expansion to catch future-destructor blocks (not just sync I/O) — possible tooling follow-up.

## Implementation log
*(populated post-ship)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

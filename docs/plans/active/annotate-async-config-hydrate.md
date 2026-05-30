# Plan — Move Annotate config hydrate off the UI thread

> **Slug**: `annotate-async-config-hydrate` (matches this file's basename without `.md`).
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety,
> § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

Backlog **N14** (`backlog/BACKLOG_CODE_REVIEW.md`), surfaced by CodeRabbit on PR #563 and deferred
out of the Phase-2 prefs-cleanup PR (#565) to keep that PR prefs-only/perf-neutral.

`AnnotateAnalysisUi::HydrateAnnotateCfgDiskOnce()` (`Source/Core/src/Ui/AnnotateAnalysisUi_Config.cpp`)
performs a **synchronous** `ConfigManager::LoadAnnotateAnalysis()` — which calls
`LoadMergedConfigJson()` → `LoadJsonFile()` (disk read + JSON parse) — on the **UI thread**, on the
first call. It is reached from three UI-thread sites, the load-bearing one being
`AnnotateRowHasNonEmptyCallstackField()`, which runs during **grid rendering**. The first grid frame
that hits this path blocks on disk I/O — a Pillar-2 violation (UI-thread sync I/O). It is mitigated
today only by the once-only `annotateCfgDiskHydrated` guard (so it stalls once, not per-frame).

Intended outcome: *after this lands, no Annotate code path performs synchronous config-file disk I/O
on the UI thread; the first hydrate happens on a worker and is marshaled back via the main-thread
dispatcher.*

## Approach

Convert `HydrateAnnotateCfgDiskOnce()` from a blocking load into a **non-blocking lazy async load with
main-thread marshal-back**, the same pattern the Annotate detail/worker code already uses
(`std::async` + `AppController::mainThreadDispatcher.PostToMainThread`).

On first call: if already hydrated → return; if a load is already in-flight → return (caller uses the
current/default `annotateCfg` for this frame); otherwise launch `std::async(std::launch::async, …)`
that reads `ConfigManager::LoadAnnotateAnalysis()` into a local, then `PostToMainThread([cfg]{ … })`
to assign `State().annotateCfg`, set `annotateCfgDiskHydrated = true`, and refresh the callstack-field
hint. **All writes to the shared `State().annotateCfg` stay on the UI thread** (they run inside
`Drain()`), so there is no data race; the worker lambda only touches a function-local result and the
stateless loader.

The trade-off (named): callers must tolerate a "not-yet-hydrated" frame or two. This is acceptable
because (a) `AnnotateRowHasNonEmptyCallstackField` already returns `false` for the no-field case, so a
1-frame-late affordance is invisible in practice; (b) the load is kicked on the first grid frame, so by
the time a user opens the Annotate window or right-clicks "Annotate…", the cfg is long since hydrated.
We deliberately do **not** go further (e.g. eager load at `AppController::Initialize`) — that would
broaden the blast radius beyond the Annotate subsystem for no extra Pillar-2 benefit.

## Files to modify

1. `Source/Core/src/Ui/AnnotateAnalysisUi_Internal.h` — add async-load state to `AnnotateState`
   (`std::atomic<bool> cfgLoadInFlight{false}`, `std::shared_future<AnnotateAnalysisConfig> cfgLoadFut`);
   change the `HydrateAnnotateCfgDiskOnce()` decl to take the dispatcher/`AppController&`.
2. `Source/Core/src/Ui/AnnotateAnalysisUi_Config.cpp` — rewrite `HydrateAnnotateCfgDiskOnce()` as the
   non-blocking launcher + completion marshal.
3. `Source/Core/src/Ui/AnnotateAnalysisUi.cpp` — thread `app`/dispatcher into the two direct callers
   (`AnnotateRowHasNonEmptyCallstackField` already has `app`; `OpenAnnotateAnalysisForGridIssue` has
   `app`) and into `ensureSettingsBuffersLoaded()` (called from `DrawAnnotatePreferencesTab(app)` and
   `DrawContent(app)` — both hold `app`).
4. `Source/Core/src/Ui/AnnotateAnalysisUi_Window.cpp` — `DrawContent` already has `app`; confirm the
   `ensureSettingsBuffersLoaded()` call passes it.
5. `Source/Core/include/Ui/AnnotateAnalysisUi.h` — update `ensureSettingsBuffersLoaded` signature if it
   gains an `AppController&`/dispatcher parameter.

## Existing utilities reused

- `MainThreadDispatcher::PostToMainThread(Task)` — `Source/Core/include/MainThreadDispatcher.h:35` —
  the canonical worker→UI marshal; no-ops after shutdown (safe for in-flight callbacks).
- `std::async(std::launch::async, …)` + `std::shared_future<>` — already the detail-load idiom in
  `Source/Core/src/Ui/AnnotateAnalysisUi_Worker.cpp:178`.
- `ConfigManager::LoadAnnotateAnalysis()` — `Source/Core/src/Config/ConfigManager.cpp` — unchanged;
  only its *call site timing* moves off the UI thread.
- `SetCallstackFieldIdHint(...)` — existing post-hydrate side-effect, re-invoked in the marshal-back.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: net positive — removes a one-shot disk-read
  stall from the first grid/annotate frame; steady-state unchanged (loader runs once, off-thread).
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: the entire point — eliminates the
  only synchronous config disk read on an `ImGui::*`-reachable path in the Annotate subsystem.
- **Pillar 3 (never crash)**: `annotateCfg` written only on the UI thread (inside `Drain()`); worker
  lambda touches only locals + stateless loader → no data race. Dispatcher no-ops post-shutdown, so a
  late-completing load can't write through a destroyed `AnnotateState`. The shared-future is joined/dropped
  in `~AnnotateState`. RAII throughout; no raw new/delete.
- **Pillar 4 (accessibility)**: N/A — no user-facing control or layout change.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A — <reason>`)

Diff touches `Source/Core/` → gate **fires**. This is an intentional Pillar-2 *improvement*, not a regression.

1. **PR-fast CI** — scenario `annotate-open-entry-tab` (exercises the Annotate open/hydrate path).
   Map: `agents/core/perf-gatekeeper.md` § Curated diff → scenario map; subset in
   `scripts/dev/perf-pr-fast-set.json`. Expect within-noise (work moved off-thread, not added).
2. **Pillar 2 static scanner** — **removes** a sync-I/O reachable from `ImGui::*`; the scanner must be
   silent over the changed tree after the move. New worker call annotated
   `/* PILLAR2_WORKER_ONLY */ // est-latency: <N>ms` per the convention.
3. **Dispatcher drain** — does **not** modify `MainThreadDispatcher::Drain()`; only enqueues one task.
4. **Visible-cue bucket-E harness** — N/A — removes (does not add) a sync-stall path; no new >100 ms path.
5. **Marker inventory** — no `SMATCHET_UI_PERF_SCOPE` markers added (no `MARKER_INVENTORY.md` regen).

**Pre-push local check**: `docs/guides/perf-workflow.md` § Gate-check vs baseline against
`annotate-open-entry-tab` before opening the PR; expect within-noise.

**Override**: not needed — improvement, no intentional regression.

## Risks / non-goals

- **Risk — 1-frame default flash in the prefs tab**: on first open before hydrate completes, the form
  could render default values for a frame. Mitigation: the prefs tab is opened by explicit user action,
  by which point the grid-frame-kicked load has completed; if observed, gate the form render on
  `annotateCfgDiskHydrated`. Accepted (cosmetic, ≤1–2 frames).
- **Risk — load failure / empty config**: `LoadAnnotateAnalysis()` already returns a defaulted struct on
  missing/garbage config (Pillar-3 behaviour from ConfigMigration tests); async wrapper preserves that.
- **Risk — late completion after window close / hot-reload**: `PostToMainThread` no-ops post-shutdown and
  `~AnnotateState` joins/drops the future; the marshal lambda re-checks nothing destructive.
- **Non-goal**: eager hydrate at app init (rejected — broader blast radius, no extra benefit).
- **Non-goal**: making `ConfigManager::Load*` itself async/thread-safe for other callers (out of scope;
  this plan only moves the Annotate call-site timing).

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: `tests/Core/AnnotateAnalysisConfig.test.cpp` (added in
  #565) still passes unchanged — proves the loader semantics (clamp, showRaw, multi-rule remaps) are
  untouched; only call timing moved. No new pure-logic surface (threading isn't unit-testable here).
- **Bucket E (ImGui Test Engine)**: N/A initially — no existing Annotate bucket-E harness; flagged as
  manual residue below with a deferred-automation action.
- **Bash-driver scenario / sanitizer**: run scenario `annotate-open-entry-tab` (CLI, `--spawn`) → expect
  `ok:true`; run under the ASan preset (`ninja-msvc-asan`) to confirm no data race / UAF on the
  worker→dispatcher hand-off. Optional `[temp-debug]` log/assert proving the first grid frame issues no
  synchronous `LoadJsonFile` (removed before merge).
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12`
  (dual-target).
- **Manual residue**: visual confirmation that the Annotate "…" grid affordance + prefs tab populate
  correctly after the async load (no permanent default-state). Deferred-automation action: add an
  `annotate-prefs-hydrate` bucket-E harness — log a `docs/self-improvement/categories/test.md` entry.

## Out of scope (flagged, not designed)

- Async-loading the *broad* `TrackerConfig` (`ConfigManager::Load()`) — separate concern, separate plan.
- Debouncing the per-edit `SaveAnnotateAnalysis()` writes (each prefs edit writes the whole file) — a
  distinct Pillar-2/IO-churn item; no-action here, candidate for a future backlog entry.

## Implementation log
*(populated post-ship)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*

# Plan — Move Annotate config I/O off the UI thread

> **Slug**: `annotate-async-config-hydrate` (matches this file's basename without `.md`).
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety,
> § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.
>
> **Status**: grilled (`grill-with-docs`, 2026-05-30) — design reshaped from the first draft; held for
> later execution. Sequencing: runs **after PR #565 merges** (shares `AnnotateAnalysisUi*` files), rebased
> on the updated `develop`.

## Context

Two CodeRabbit findings, same class — **synchronous config-file I/O on the UI thread** in the Annotate
subsystem — deferred out of their originating PRs:

- **Load / hydrate** (backlog **N14**, flagged on PR #563): `HydrateAnnotateCfgDiskOnce()`
  (`Source/Core/src/Ui/AnnotateAnalysisUi_Config.cpp`) does a synchronous `LoadAnnotateAnalysis()` →
  `LoadMergedConfigJson()` → `LoadJsonFile()` on first call, reached from `AnnotateRowHasNonEmptyCallstackField()`
  during **grid rendering**.
- **Save** (flagged on PR #565, `AnnotateAnalysisUi_Preferences.cpp:157-195`): prefs-form edits persist via
  synchronous `SaveAnnotateAnalysis()` (JSON encode + atomic file replace) from UI callbacks.

Persistence substrate (verified): a single whole-file JSON config, `smatchet_config.json`, read by
`ConfigManager::LoadMergedConfigJson()` (`Source/Core/src/Config/ConfigManager_PathUtils.cpp:603`) and
written atomically by `WriteConfigJson()` (`:630`, holds `GetIoMutexRef()` + `ScopedFileLock` +
`AtomicWriteTextFile`). **No DB / append-log / schema-version** — so no migration concern.

Intended outcome: *after this lands, the Annotate prefs-save path no longer blocks the UI thread on disk
I/O, and the hydrate read is either confirmed-negligible-and-annotated or moved off-thread — whichever the
measurement justifies.*

## Approach

Two independent paths, each settled by the grill against existing codebase precedent:

**Save path — mirror the shipped `ScheduleConfigSaveDetached` pattern.** The codebase already moves
`TrackerConfig` saves off the UI thread via `ScheduleConfigSaveDetached` (`SmatchetAiAssistantUi.cpp:91`):
value-snapshot + `std::thread(...).detach()` → `ConfigManager::Save()`. Add a sibling
`ScheduleAnnotateConfigSaveDetached(AnnotateAnalysisConfig snapshot)` and route every Annotate config save
through it. Detached is Pillar-3-safe here for the same reason as the original: `SaveAnnotateAnalysis` is
static and the snapshot is by-value (no owner to outlive — unlike `SmatchetChatPersistWorker`, which had to
join because it touched `LocalCacheManager`). The UI-thread-only side-effects in `PersistAnnotateCfg`
(`LogAnnotateP4PathsIfChanged`, `SetCallstackFieldIdHint`) stay synchronous; **only the file write detaches.**

**Load path — measure first, then decide.** The codebase documents an explicit policy
(`SmatchetAiAssistantUi.cpp` inline-vs-async hydration comment): small reads stay synchronous on the first
frame because a one-frame stall beats a background-thread + dispatcher round-trip; only above-threshold reads
go async. The Annotate hydrate is a small (~few-KB) **one-time** JSON read — the "keep sync" case by that
policy. So: instrument the hydrate read+parse latency. **If <~5 ms** (very likely), keep it synchronous with a
`/* PILLAR2_INLINE */ // est-latency: <N>ms` annotation citing the inline-hydration policy — done. **Only if it
measures slow** do we implement the lazy-async-load + `MainThreadDispatcher::PostToMainThread` marshal-back
(callers tolerate a not-yet-hydrated frame; writes to `State().annotateCfg` stay on the UI thread inside
`Drain()`).

Trade-off named (save path): mirroring the detached pattern inherits its **unlocked read-modify-write** —
`LoadMergedConfigJson()` (read) → modify → `WriteConfigJson()` (write) is not atomic across threads, so a
detached Annotate save can in principle lose-update against a concurrent `TrackerConfig` detached save or a
UI-thread save. This race **already ships and is tolerated** for `TrackerConfig`; saves are user-edit-gated and
~ms, so two subsystems committing edits within the same millisecond is practically impossible. We accept it for
consistency and scope the global fix out (see Out of scope).

## Files to modify

1. `Source/Core/src/Ui/AnnotateAnalysisUi_Internal.h` — declare
   `AnnotateInternal::ScheduleAnnotateConfigSaveDetached(const AnnotateAnalysisConfig&)`; (load-async fields
   only if measurement forces async).
2. `Source/Core/src/Ui/AnnotateAnalysisUi_Config.cpp` — define `ScheduleAnnotateConfigSaveDetached` (mirror of
   `ScheduleConfigSaveDetached`); route the autoselect-helper save + `ApplyShowRawCallstack` save through it;
   add the hydrate-latency instrumentation + (if fast) the `PILLAR2_INLINE` annotation on the existing sync read.
3. `Source/Core/src/Ui/AnnotateAnalysisUi_Preferences.cpp` — `PersistAnnotateCfg` swaps its synchronous
   `ConfigManager::SaveAnnotateAnalysis(...)` for `ScheduleAnnotateConfigSaveDetached(...)`; UI-thread
   side-effects stay. This single chokepoint covers all prefs-form edits (colors, cache size, remaps, text
   fields, combos, max-frames, ignore-keywords).
4. *(only if load measures slow)* `Source/Core/src/Ui/AnnotateAnalysisUi.cpp`, `_Window.cpp`,
   `Source/Core/include/Ui/AnnotateAnalysisUi.h` — thread `AppController&`/dispatcher into the hydrate callers.

## Existing utilities reused

- `ScheduleConfigSaveDetached` — `Source/Core/src/Ui/SmatchetAiAssistantUi.cpp:91` — the exact pattern to
  mirror (value-snapshot + detached thread + swallow-and-log on the worker).
- Inline-vs-async hydration policy + `PILLAR2_*` annotation convention — same file's hydration comment block.
- `ConfigManager::SaveAnnotateAnalysis` / `LoadAnnotateAnalysis` — `ConfigManager.cpp` — unchanged; only call
  timing moves.
- `MainThreadDispatcher::PostToMainThread` — `MainThreadDispatcher.h:35` — only if the load goes async.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms)**: net positive on save (removes JSON-encode + atomic-replace from the
  UI frame on edit-commit); load unchanged unless measured slow.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: the point — removes the save-path sync
  write from `ImGui::*` callbacks; load either confirmed-negligible-and-annotated or moved off-thread.
- **Pillar 3 (never crash)**: detached save captures cfg by value + calls a static; no owner to outlive, worker
  swallows+logs exceptions (mirrors the shipped pattern). If load goes async, `annotateCfg` is written only on
  the UI thread (inside `Drain()`); dispatcher no-ops post-shutdown; future joined in `~AnnotateState`.
- **Pillar 4 (accessibility)**: N/A — no user-facing control/layout change.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A — <reason>`)

Diff touches `Source/Core/` → gate **fires**. Intentional Pillar-2 *improvement*, not a regression.

1. **PR-fast CI** — scenario `annotate-open-entry-tab` (Annotate open + prefs-save path). Map:
   `agents/core/perf-gatekeeper.md` § Curated diff → scenario map; subset `scripts/dev/perf-pr-fast-set.json`.
   Expect within-noise (work moved off-thread / removed, not added).
2. **Pillar 2 static scanner** — **removes** sync-I/O reachable from `ImGui::*` on the save path; scanner must
   stay silent over the changed tree. The detached worker call + (if kept) the sync hydrate get the
   `/* PILLAR2_WORKER_ONLY */` / `/* PILLAR2_INLINE */ // est-latency: <N>ms` annotations.
3. **Dispatcher drain** — untouched on the save path; touched only if load goes async (one enqueue, no
   `Drain()` change).
4. **Visible-cue bucket-E harness** — N/A — removes (does not add) a sync-stall path.
5. **Marker inventory** — no `SMATCHET_UI_PERF_SCOPE` markers added.

**Pre-push local check**: `docs/guides/perf-workflow.md` § Gate-check vs baseline against
`annotate-open-entry-tab`; expect within-noise.

**Override**: not needed.

## Decisions (from grill, 2026-05-30)

1. **Scope = load + save** (CR #565 validated the save path is the same class as N14's load).
2. **Save mechanism = mirror `ScheduleConfigSaveDetached`** (not a new IoMutex-RMW refactor, not a new
   coalescing worker) — smallest change, consistent with shipped code, accepts the already-tolerated RMW race.
3. **Load = measure-first** — keep sync + Pillar-2 annotation if <~5 ms (per the documented inline-hydration
   policy); async only if measured slow.

## Risks / non-goals

- **Risk — unlocked-RMW lost-update across threads**: accepted; identical to the shipped `TrackerConfig`
  detached-save baseline; user-edit-gated + ~ms ⇒ practically unreachable. Global fix scoped out below.
- **Risk — load stays sync after all**: if measured fast, the literal "any sync I/O on UI thread" Critical
  guideline is satisfied-by-exception via the documented inline-hydration policy + a measured annotation, not
  by an async refactor. Acceptable; this is the codebase's own standard.
- **Non-goal — global `ConfigManager` RMW atomicity** (hold `GetIoMutexRef` across read-modify-write for *all*
  `Save*`): correct and would also fix the latent `TrackerConfig` race, but broadens into the shared config
  contract — separate hardening plan, not Annotate-scoped.
- **Non-goal — single coalescing config-save worker** (à la `SmatchetChatPersistWorker`): the codebase's stated
  end-state ("move to a coalescing worker if churn"), but over-infra for this low-frequency path now.
- **Non-goal — async the broad `TrackerConfig` load**.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: existing `tests/Core/AnnotateAnalysisConfig.test.cpp` (clamp +
  showRaw + multi-rule remap round-trip) must stay green — proves `Save/LoadAnnotateAnalysis` semantics are
  unchanged; only call timing moves. **Test-delta gate**: add a focused case asserting the detached-save helper
  reaches disk (drive `ScheduleAnnotateConfigSaveDetached` then join/poll for the persisted value with a bounded
  wait) so the `Source/Core` change carries a deterministic test delta.
- **Bash-driver scenario / sanitizer**: scenario `annotate-open-entry-tab` (`--spawn`) → `ok:true`; run under
  `ninja-msvc-asan` to confirm no data race / UAF on the detached-save thread (and the dispatcher hand-off if
  load goes async).
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12`.
- **Latency measurement (load)**: one-off instrumentation of `HydrateAnnotateCfgDiskOnce` read+parse; record
  the number in § Verification (actual) to justify keep-sync-vs-async.
- **Manual residue**: confirm prefs edits still persist across reopen with the detached save (no lost write on
  quick close). Deferred-automation: an `annotate-prefs-persist` bucket-E harness — log a
  `docs/self-improvement/categories/test.md` entry.

## Out of scope (flagged, not designed)

- Global `ConfigManager` RMW-atomicity hardening (fixes the latent cross-thread lost-update for *all* savers).
- A single coalescing config-save worker shared across subsystems.
- Async-loading the broad `TrackerConfig`.
- Debouncing rapid repeated edits (detached-per-save is fine at user-edit frequency; coalescing is the future
  worker's job).

## Implementation log
- Save path: added `AnnotateInternal::ScheduleAnnotateConfigSaveDetached` (`AnnotateAnalysisUi_Config.cpp`),
  mirroring `ScheduleConfigSaveDetached`; routed all three save chokepoints through it —
  `PersistAnnotateCfg` (`AnnotateAnalysisUi_Preferences.cpp`, covers every prefs-form edit), the
  `MaybeAutoselectTrackerField` helper, and `ApplyShowRawCallstack`. UI-thread-only side-effects
  (`LogAnnotateP4PathsIfChanged`, `SetCallstackFieldIdHint`) left synchronous.
- Load path: measured the hydrate (temp instrumentation) → kept synchronous + `PILLAR2_INLINE` annotation.
- Test: extended `tests/Core/AnnotateAnalysisConfig.test.cpp` with a `[high-risk]` concurrent-save case
  (8 threads hammering `SaveAnnotateAnalysis`) proving the file stays valid JSON under contention.

## Deviations from plan
- **Load stayed synchronous** (the planned "measure-first" outcome): measured **0.54 ms** for the whole-file
  JSON load+parse — far under the ~5 ms threshold — so no async/marshal-back machinery was built; the sync
  read got the documented `PILLAR2_INLINE` annotation instead. The async-load design in § Approach was the
  contingency and was not needed.
- Everything else implemented as designed; no IoMutex-RMW refactor, no coalescing worker (both stayed
  out-of-scope as planned).

## Verification (actual)
- Dual-target build (`SmatchetStandalone` + `SmatchetCore_DX12`): **pass** (exit 0).
- doctest full suite **847/0** (incl. new concurrent-save `[high-risk]` case + the existing clamp/showRaw/
  multi-rule round-trip cases): **pass**.
- `clang-format` (diff-scoped): clean. Delta-lint gate: **PASS** (no new strict-zone violations).
- Scenario `annotate-open-entry-tab` (`--spawn`): **ok:true**.
- Hydrate latency: **0.54 ms** measured (basis for keep-sync decision).
- ASan / data-race on the detached-save thread: covered by the PR's required **Sanitizer (ASAN via MSVC)**
  CI job + the 8-thread concurrent-save unit test (passed).
- Manual residue: prefs-edit-persists-across-reopen visual check still unautomated — deferred-automation
  `annotate-prefs-persist` bucket-E harness noted (low priority).

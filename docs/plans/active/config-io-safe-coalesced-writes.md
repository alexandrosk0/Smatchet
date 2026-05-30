# Plan — ConfigManager: safe + coalesced config writes

> **Slug**: `config-io-safe-coalesced-writes`.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety,
> § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.
>
> **Status**: draft for grill. Covers the three config-I/O follow-ups deferred from the N14 work
> (`annotate-async-config-hydrate`): (1) RMW-atomicity hardening, (2) a single coalescing config-save
> worker, (3) async the broad `TrackerConfig` load.

## Context

The N14 work moved Annotate config saves off the UI thread by mirroring the shipped
`ScheduleConfigSaveDetached` (`SmatchetAiAssistantUi.cpp`) — value-snapshot + detached `std::thread`.
That accepted a **pre-existing, tolerated race**: every `Save*` in `ConfigManager` does an *unlocked*
read-modify-write — `LoadMergedConfigJson()` (read) → modify one section → `WriteConfigJson()` (atomic
write). Two concurrent writers (now: two detached config-save threads + any UI-thread saver) can
lose-update: writer A reads, writer B reads+writes, writer A writes and clobbers B's section.

Substrate (verified): one whole-file JSON, `smatchet_config.json`. `WriteConfigJson`
(`ConfigManager_PathUtils.cpp:630`) holds `GetIoMutexRef()` (a **non-recursive `std::mutex`**,
`:348`) + a cross-process `ScopedFileLock` + `AtomicWriteTextFile` (temp-then-rename). The write is
individually atomic; the *transaction* (read…write) is not serialized across threads.

RMW `Save*` sites (item-1 scope): `ConfigManager.cpp` `Save(TrackerConfig)` (~`:456` write),
`SaveAnnotateAnalysis` (~`:535`→`:569`), and the third save at `:587`; `ConfigManager_Views.cpp`
(`:235`, `:286`). Detached-save consumers (item-2 scope): `ScheduleConfigSaveDetached` (3 sites in
`SmatchetAiAssistantUi.cpp`) + `ScheduleAnnotateConfigSaveDetached` (3 sites in the Annotate UI).

Intended outcome: *after this lands, concurrent config writes can never lose-update, and config saves
funnel through a single coalescing background writer (joined on shutdown) instead of per-save detached
threads.*

## Approach

**Item 1 — deadlock-safe RMW atomicity.** Introduce a distinct outer mutex
`GetConfigRmwMutexRef()` (NOT `GetIoMutexRef`). Every `Save*` transaction holds it across
`LoadMergedConfigJson → modify → WriteConfigJson`. `WriteConfigJson` keeps its own inner
`GetIoMutexRef` + `ScopedFileLock` for the atomic file write — **two different mutexes, so no
recursive-lock deadlock** (the trap if we naively reused `GetIoMutexRef`, which is non-recursive and
already held by `WriteConfigJson`). Readers calling `LoadMergedConfigJson` need no RMW lock — the
atomic temp-then-rename guarantees they always see a complete file. The RMW mutex serializes only the
write transactions, which is exactly what closes the lost-update window.

**Item 2 — single coalescing config-save worker.** Model on `SmatchetChatPersistWorker`
(`Start(deps, dispatcher)` / `Stop()` joined before owner destructs / `Enqueue(Op)` coalescing). One
background thread owns all config writes; `Op` is a tagged snapshot (`TrackerConfig` or
`AnnotateAnalysisConfig`). Coalescing = latest pending snapshot **per config-kind** wins (drop
intermediate edits). Each drained write goes through the item-1 atomic-RMW `Save*`. Replace the six
`Schedule*ConfigSaveDetached` call sites with `Enqueue`. Lifecycle owned by `AppController`
(`Start` at init, `Stop`/join before the dispatcher + config statics tear down — Pillar 3). This
realizes the codebase's stated direction ("move to a single coalescing worker if churn",
`SmatchetAiAssistantUi.cpp` comment).

**Item 3 — async the broad `TrackerConfig` load.** `ConfigManager::Load()` (`ConfigManager.cpp:576`)
is the broad parse. **Grill must validate necessity**: it appears to run at boot (pre-UI) + on cache
miss; if there is no hot UI-thread re-load path, item 3 is **N/A** (the same "measure-first → it's
boot-time, leave it" outcome N14's load path reached). If a hot re-load path exists, async it via the
worker/dispatcher. Treat item 3 as *contingent on the grill finding a real UI-thread load stall*.

## Files to modify

1. `Source/Core/src/Config/ConfigManager_PathUtils.cpp` / `ConfigManager_Internal.h` — add
   `GetConfigRmwMutexRef()`.
2. `Source/Core/src/Config/ConfigManager.cpp`, `ConfigManager_Views.cpp` — hold `GetConfigRmwMutexRef`
   across every `Save*` RMW transaction.
3. **New** `Source/Core/include/ConfigSaveWorker.h` + `Source/Core/src/ConfigSaveWorker.cpp` — the
   coalescing worker (mirror of `SmatchetChatPersistWorker`).
4. `Source/Core/src/AppController.cpp` (+ header) — `Start`/`Stop` the worker in the lifecycle.
5. `Source/Core/src/Ui/SmatchetAiAssistantUi.cpp` — replace `ScheduleConfigSaveDetached` (3 sites) +
   delete the helper.
6. `Source/Core/src/Ui/AnnotateAnalysisUi_Config.cpp` (+ Internal.h) — replace
   `ScheduleAnnotateConfigSaveDetached` (3 sites) with `Enqueue`; delete the helper.
7. *(item 3, only if grill finds a hot load)* the relevant `Load()` caller.

## Existing utilities reused

- `SmatchetChatPersistWorker` (`SmatchetChatPersistWorker.{h,cpp}`) — structural template for the
  coalescing worker (Start/Stop/Enqueue, join-on-shutdown, dispatcher marshal-back).
- `MainThreadDispatcher::PostToMainThread` — `MainThreadDispatcher.h:35` — for any UI-thread callback
  (e.g. post-save toast / error surfacing).
- `WriteConfigJson` / `LoadMergedConfigJson` / the existing `Save*` bodies — unchanged logic; only
  wrapped by the RMW mutex and called from the worker.

## UX Pillar callouts

- **Pillar 1 (perf)**: net positive — coalescing drops redundant writes; no steady-state path added.
- **Pillar 2 (UI never blocks)**: all config file I/O now off the UI thread (worker), superseding the
  detached-thread shims.
- **Pillar 3 (never crash)**: the worker is **joined on `Stop()` before** the dispatcher + config
  statics tear down (the detached threads it replaces had no join — a latent shutdown-race the chat
  worker was created to avoid). The new RMW mutex is a distinct lock from `GetIoMutexRef` → no
  deadlock; lock order is always outer-RMW → inner-IO, never reversed.
- **Pillar 4 (accessibility)**: N/A.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A — <reason>`)

Touches `Source/Core/` → **fires**. Improvement, not regression.

1. **PR-fast CI** — `priority-grid-scroll` (general UI) + `annotate-open-entry-tab` (config save path).
   Map: `agents/core/perf-gatekeeper.md`; subset `scripts/dev/perf-pr-fast-set.json`.
2. **Pillar 2 static scanner** — removes the remaining UI-thread config-write paths; scanner stays
   silent. Worker write annotated `/* PILLAR2_WORKER_ONLY */`.
3. **Dispatcher drain** — does not modify `Drain()`; the worker only enqueues marshal-backs.
4. **Visible-cue bucket-E harness** — N/A; removes sync-stall paths.
5. **Marker inventory** — no `SMATCHET_UI_PERF_SCOPE` markers added.

**Pre-push local check**: gate-check vs baseline on the named scenarios; expect within-noise.
**Override**: not needed.

## Risks / non-goals

- **Risk — lock-order / deadlock**: mitigated by using a *distinct* outer RMW mutex and a fixed lock
  order (outer-RMW then inner-IO); `WriteConfigJson` never takes the RMW mutex. Add a focused
  concurrency test (many threads × {TrackerConfig, Annotate} saves) asserting no loss + no hang.
- **Risk — worker lifecycle UAF at shutdown**: mitigated by join-on-`Stop()` before owner teardown
  (the explicit reason `SmatchetChatPersistWorker` exists); `Enqueue` no-ops after `Stop`.
- **Risk — coalescing drops an edit the user expected persisted**: latest-per-kind wins, and every
  edit updates the in-memory cfg first, so the final state is always written; only redundant
  intermediate writes are dropped. Acceptable.
- **Non-goal**: changing the on-disk JSON shape or adding schema versioning.
- **Non-goal (pending grill)**: item 3 if no hot UI-thread `Load()` exists.

## Verification

- **Bucket A (ctest, `test-rig`)**: new `tests/Core/ConfigSaveConcurrency.test.cpp` — N threads ×
  mixed `Save(TrackerConfig)` + `SaveAnnotateAnalysis`, assert (a) file always valid JSON, (b) no
  lost section (each kind reflects *some* writer, never a torn merge), (c) terminates (no deadlock,
  bounded join). Extends the #568 concurrent-save case. **Satisfies the Test-delta gate.**
- **Bash-driver / sanitizer**: `annotate-open-entry-tab` + an AI-prefs save scenario (`--spawn`) →
  `ok:true`; run under `ninja-msvc-asan` for the worker + RMW-mutex hand-offs.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12`.
- **Manual residue**: none expected (the #572 bucket-E prefs-persist harness already covers the
  edit→persist→reload UI path through the worker).

## Out of scope (flagged, not designed)

- On-disk schema versioning / migration.
- Async-loading `smatchet_views.json` (separate file, separate path).
- Reworking `ScopedFileLock` cross-process semantics.

## Implementation log
*(populated post-ship)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*

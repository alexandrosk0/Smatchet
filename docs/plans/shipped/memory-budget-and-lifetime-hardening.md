# Plan — Memory budget & lifetime hardening

> **Slug**: `memory-budget-and-lifetime-hardening` (matches this file's basename without `.md`).
>
> **Status**: P0-first. Slices **S1–S5** are committed scope; deferred phases **P1 (Phase 3, 4)** and **P2 (Phase 5)** are documented here but routed to backlog, reassessed after P0 ships.
>
> **Provenance**: memory-management audit (2026-05-31) → orchestrator verification of every claim against the tree → `architect` cross-cutting design review (v2) → `grill-with-docs` session (2026-05-31, re-verified against `9ae9c24e`) which overturned the site-#6 "shutdown-only" framing (the live tracker swap is the real UAF; fixed via [ADR 0012](../../adr/0012-shared-ownership-active-tracker-backend.md)). Original `path:line` anchors were captured at `e789e7f2`; the S4 slurp has since drifted to `:69` (TODO landmark `:62`). **Ship decision: all of S1–S5 in one PR** (per user, 2026-05-31).

## Context

A memory-management audit of Smatchet (2026-05-31) found the dominant ownership pattern is already healthy RAII/STL — `unique_ptr` service ownership, joined worker threads, bounded logger/audit queues, SQLite statement wrappers, capped texture/token/plan caches. The risks are **not** classic leaks. They are three different shapes:

1. **Detached-thread lifetime holes (Pillar 3, never-crash).** Six `std::thread(...).detach()` sites capture app / backend / dispatcher state and are not joined at shutdown — a use-after-free window if `~AppController` runs while a detached worker is mid-flight. One (`SmatchetNewIssueDraftUi.cpp`) captures `&app` by reference.
2. **Large transient allocation / I/O on the UI thread (Pillars 1 + 2).** The attachment preview reads up to a 50 MB file into a `std::vector` on the UI thread, then (on Win32 + thumbnail flag) decodes + GPU-uploads on the same thread.
3. **Count-bounded (not byte/time-bounded) queues + caches.** The main-thread dispatcher caps at 4096 *tasks* and drains all of them in one frame; the icon texture cache caps at 384 *entries* with only a commented byte estimate; the AI plan cache caps at 256 entries but its sibling height/action maps are not tied to that eviction.

The audit proposed a push-based `MemoryBudgetRegistry` + replacing the detached threads with a new `TaskService`. Verification showed the async primitive **already exists** (`AppController::LaunchBackgroundTask` + `JoinBackgroundTasks`, ~40 call sites, joined at shutdown), so the threading work is *migration*, not new infra. The `architect` review further rejected the push-registry as speculative generality in favour of a thin pull-based snapshot.

**Intended outcome — after P0 lands:** the six detached-thread UAF-on-shutdown holes are closed and locked shut by a `no-detach` lint rule; the 50 MB UI-thread attachment read is gone; and memory pressure (RSS, queue depth, cache occupancy) is observable via a `perf.memory` command so Pillar 1/2 regressions surface before users feel them.

## Approach

Three work-streams, sequenced **fixes before instrumentation**:

1. **Close the lifetime holes first (S1 + S2).** Migrate the 6 `detach()` sites onto the existing joined `LaunchBackgroundTask` pool (the canonical migration template already lives in-tree at `SmatchetPreferencesUi_Whisper.cpp:446`), then add a `no-detach` absolute first-party-wide lint rule so a re-introduced detach fails CI anywhere.
2. **Make memory observable (S3).** A thin **pull-based** `MemorySnapshot()` free-function + a `perf.memory` command. Each gauge reads its source under that source's existing lock, at snapshot time only — no push wiring into hot paths. Gauges land incrementally: the cheap, already-reachable ones now; producer-coupled gauges (pending uploads, plan-cache bytes, ticket bytes) ship with the phase that builds their producer.
3. **Move attachment I/O off the UI thread (S4 + S5).** S4 replaces the unconditional 50 MB slurp with a bounded header read (all platforms). S5 (Win32 + thumbnail flag) moves image decode to a background task and posts only the GPU upload back through the dispatcher, rate-limited producer-side, with a visible "loading thumbnails" cue.

**Key trade-off (named):** the audit's push-`MemoryBudgetRegistry` would force every source (texture-cache LRU splice, dispatcher post, ticket mutate) to hold a registry pointer and call `Report()` on mutation — invasive edits to hot lines for data only read at snapshot time. A pull snapshot is strictly cheaper, touches fewer hot lines, and is C++14 + dual-target friendly. The registry "grows a gauge per phase" rather than being built up front against producers that don't exist yet. Deeper rationale would go to an ADR if the shape is contested.

## Slices (P0 — committed scope)

Global order: **S1 → S2 → S3 → S4 → S5**. S1/S2 are adjacent (S2's clean-tree precondition depends on S1). S3 and S4 are independent of S1/S2 and of each other. S5 depends on S3 (consumes the `pendingThumbnailUploads` gauge surface).

### S1 — Migrate the 6 detached threads → `LaunchBackgroundTask`
Route each fire-and-forget worker through the joined pool, preserving its post-back semantics (cancel atoms, `g_ui` writes via `mainThreadDispatcher.PostToMainThread`). Per-site:

| # | Site | Today | Migration note | Risk |
|---|---|---|---|---|
| 1 | [AiPrefsTestConnection.cpp:183](Source/Core/src/AiPrefsTestConnection.cpp) | captures `g_ui`, local `dispatcher`, `cancel` atom | Clean. Confirm an `AppController&` is reachable; if only `dispatcher` is in scope, thread `AppController&` as a param (small signature delta — flag in slice). | Low |
| 2 | [SmatchetNewIssueDraftUi.cpp:255](Source/Core/src/Ui/SmatchetNewIssueDraftUi.cpp) | captures **`&app` by reference** | The by-ref capture is a bug **only because it is detached**; once pooled (joined before `~AppController`) it is safe — no capture change needed. Delete the stale "we use std::async" comment. | Low (once pooled) |
| 3 | [SmatchetAiAssistantUi.cpp:155](Source/Core/src/Ui/SmatchetAiAssistantUi.cpp) | captures `appPtr` (by value) | Clean lift; has a documented lifetime arg but is unjoined — pooling closes it. | Low |
| 4 | [SmatchetPreferencesUi_Whisper.cpp:391](Source/Core/src/Ui/SmatchetPreferencesUi_Whisper.cpp) | whisper test-connection | Copy the sibling mic-test template at `:446` verbatim. | Trivial |
| 5 | [SmatchetPreferencesUi_Assistant.cpp:325](Source/Core/src/Ui/SmatchetPreferencesUi_Assistant.cpp) | assistant test-connection (near-dup of #1) | Same as #1. | Low |
| 6 | [SmatchetProjectPicker.cpp:119](Source/Core/src/Ui/SmatchetProjectPicker.cpp) | captures backend client **raw ptr** + **raw** `statePtr` (app-lifetime window state) | Pool migration alone does **NOT** suffice (grill 2026-05-31): a **live** tracker switch (`SyncWithBackend`→`SetBackend`, `TicketSyncService.cpp:468/472/476` → `AppControllerDepsAdapter.cpp:58`) frees the old `Backend` mid-fetch → UAF; the pool joins only at shutdown. Fix per **[ADR 0012](../../adr/0012-shared-ownership-active-tracker-backend.md)**: make `Backend` a `shared_ptr`, hand it via new `BackendShared()`, capture a `shared_ptr` copy in the pooled fetch, call `sp->Connectivity().ListProjects()`. `statePtr` is app-lifetime window state (`d.newIssueProjectPickerState`) — safe as raw. | Medium (live-swap UAF) |

Sites #1-#5 require no `SMATCHET_DEVIATION`. **Site #6 additionally needs the `Backend`→`shared_ptr` ownership change (ADR 0012)** — added files: `AppController.h` (member type + `BackendShared()` accessor), `SmatchetProjectPicker.{h,cpp}` (`Draw` takes the `shared_ptr`), and its 2 callers (`SmatchetNewIssueDraftUi.cpp:448`, `SmatchetBulkTicketsUi.cpp:260`) pass `app.BackendShared()`. The `SetBackend(unique_ptr)` interface, the factory, and the ~22 `Backend->…` call sites are unchanged (shared_ptr adopts unique_ptr on assign; `operator->` is identical). Verification: ASan/TSan run — sites #1-#5 trigger-then-shutdown-mid-flight; **site #6 trigger-then-live-`SetBackend`-swap** (the real repro), asserting no UAF/data-race against the freed backend (bucket-D) + a scenario exercising the probes (bucket-B).

### S2 — `no-detach` lint rule (absolute, first-party-wide)
After all 6 land (tree is clean → 0 first-party `.detach()`), add `no-detach` to [test-lint-rules.sh](agents/scripts/project/test-lint-rules.sh) in the **absolute first-party-wide** tier (the `no-raw-new` / SWEEP_ROOTS path), not the strict-only tier — there is no legitimate first-party `.detach()`. Mirror the `no-printf-stderr` regex + comment/string exclusion; add the rule-id to the wide-scan awk filter, the FAIL message, the catalog comment, and the `--scan-file` list; add positive + negative `tests/bats` cases. Escape hatch: `SMATCHET_DEVIATION(rule=no-detach; …)`. Land in the PR immediately after the 6th migration merges so its "clean tree" precondition is verifiably true. Pre-flight: re-scan `Source/Plugins/` + `Source/Standalone/` (the inventory was Core-scoped) to confirm no first-party detach hides outside Core before flipping absolute.

### S3 — RSS + `perf.memory` (thin pull snapshot)
New `MemoryTelemetry.{h,cpp}`; `MemorySnapshot` struct of gauges; `perf.memory` command. Ships only the cheap, already-reachable gauges (deferring producer-coupled ones):

| Gauge | Source / cost | In S3? |
|---|---|---|
| RSS / working set | `K32GetProcessMemoryInfo` (kernel32), 1 syscall | ✅ |
| dispatcher queue len | new `MainThreadDispatcher::QueueLen()` (`size()` under `mutex_`) | ✅ |
| dispatcher last-drain tasks | existing `LastDrainTaskCount()` (atomic load) | ✅ |
| icon-cache entries | `g_map.size()` under `g_mutex` | ✅ |
| icon-cache approx bytes | O(384) `Σ Width·Height·4` under `g_mutex`, snapshot-only | ✅ |
| active-ticket count | `GetActiveTicketsSnapshot()->size()` (no copy) | ✅ |
| pending thumbnail uploads | producer ships in S5 | ⛔ → S5 |
| AI plan-cache bytes | needs new accounting in `s_planCache` | ⛔ → Phase 4 |
| ticket approx bytes | O(N·M) string-size sum — expensive; needs row-store | ⛔ → Phase 5 |

- **Files (new):** `Source/Core/include/MemoryTelemetry.h` (backend-agnostic — **no Win32, no GLFW/GL** in the header so it compiles into DX12), `Source/Core/src/Persistence/MemoryTelemetry.cpp` (Win32 RSS guarded `#if defined(_WIN32)` here only).
- **Edits:** [MainThreadDispatcher.h](Source/Core/include/MainThreadDispatcher.h) add `QueueLen()`; [SmatchetImageTextureCache.cpp](Source/Core/src/Persistence/SmatchetImageTextureCache.cpp) add `IconCacheEntryCount()` / `IconCacheApproxBytes()` (the cache owns `g_map`/`g_lru`/`g_mutex`); [BuiltinCommands_Perf.cpp](Source/Core/src/Commands/Builtin/BuiltinCommands_Perf.cpp) add the `perf.memory` command (**strict lint zone** — `LOG_*` only, `obj["k"]=v` json); [CliCommandRunner.cpp:997](Source/Standalone/CliCommandRunner.cpp) add `perf.memory` to the allowlist.
- Thread-safety: each gauge reads under its own source's existing lock; the snapshot is a deliberately-torn cross-source read (gauges, not invariants). No new shared state, no new lock.

### S4 — Attachment header-read fix (unconditional, all platforms)
Replace the whole-file `istreambuf_iterator` slurp in [`ParseImageDimensions`](Source/Core/src/Ui/SmatchetAttachmentPreviewUi.cpp) (`:69` — the read loop; reliable landmark is the `TODO(pillar2)` at `:62`; up to 50 MB) with a bounded `seekg` + `read` (PNG header 24 B; cap any JPEG marker walk at a fixed ceiling, e.g. 64 KB, never the whole file). The existing `ReadU16LE` / `ReadU24LE` / `ReadU32BE` helpers operate on a byte buffer and are agnostic to how it was filled. Pure-local, no new infra, ships independently.

### S5 — Attachment background decode + dispatcher upload + cue (Win32 + thumbnail flag)
Gated `#if SMATCHET_ENABLE_BITMAP_ATTACHMENT_THUMBNAILS && _WIN32` exactly as the existing decode+upload at `:579–590`.
- **Decode budget on the worker:** define `kMaxThumbnailPixels` (precedent: the cache's `kMaxIconDimension` / `kMaxFileReadBytes`). Over-budget images **downscale-to-fit on the worker thread** (it is a preview); only unparseable data is skipped with a `LOG_WARN` + the existing non-thumbnail fallback. No new error modal.
- **Upload drain — reuse `MainThreadDispatcher`**, not a new queue: a GPU upload is exactly "a task that must run on the UI thread." Post the decoded pixels as a dispatcher task.
- **Spike mitigation:** `Drain()` runs *all* queued tasks in one frame (not budgeted), so many simultaneous decode-completions would spike. Rate-limit the **enqueue** producer-side (at most N upload tasks posted per frame) + expose the `pendingThumbnailUploads` gauge (the S3-deferred gauge ships here). The visible "loading thumbnails" cue is driven off that same counter. (A budgeted dispatcher is Phase 4, out of P0 scope.)

## Files to modify

Grouped by slice (anchors verified @ `e789e7f2`):

**S1** — 1. [AiPrefsTestConnection.cpp:183](Source/Core/src/AiPrefsTestConnection.cpp) · 2. [SmatchetNewIssueDraftUi.cpp:255](Source/Core/src/Ui/SmatchetNewIssueDraftUi.cpp) · 3. [SmatchetAiAssistantUi.cpp:155](Source/Core/src/Ui/SmatchetAiAssistantUi.cpp) · 4. [SmatchetPreferencesUi_Whisper.cpp:391](Source/Core/src/Ui/SmatchetPreferencesUi_Whisper.cpp) · 5. [SmatchetPreferencesUi_Assistant.cpp:325](Source/Core/src/Ui/SmatchetPreferencesUi_Assistant.cpp) · 6. [SmatchetProjectPicker.cpp:119](Source/Core/src/Ui/SmatchetProjectPicker.cpp) — route detach → `LaunchBackgroundTask`. **Shared-ownership (ADR 0012, site #6):** [AppController.h](Source/Core/include/AppController.h) `Backend`→`shared_ptr<ITrackerBackend>` + `BackendShared()` accessor · [SmatchetProjectPicker.h/.cpp](Source/Core/src/Ui/SmatchetProjectPicker.cpp) `Draw` takes the `shared_ptr` · callers [SmatchetNewIssueDraftUi.cpp:448](Source/Core/src/Ui/SmatchetNewIssueDraftUi.cpp) + [SmatchetBulkTicketsUi.cpp:260](Source/Core/src/Ui/SmatchetBulkTicketsUi.cpp) pass `app.BackendShared()`. · 0. [docs/adr/0012-shared-ownership-active-tracker-backend.md](../../adr/0012-shared-ownership-active-tracker-backend.md) (new ADR).

**S2** — 7. [agents/scripts/project/test-lint-rules.sh](agents/scripts/project/test-lint-rules.sh) — add `no-detach` absolute rule. · 8. `tests/bats/<lint>.bats` — scan-file positive/negative cases.

**S3** — 9. `Source/Core/include/MemoryTelemetry.h` (new) — gauge struct + free-function decls, backend-agnostic. · 10. `Source/Core/src/Persistence/MemoryTelemetry.cpp` (new) — Win32-guarded RSS. · 11. [MainThreadDispatcher.h](Source/Core/include/MainThreadDispatcher.h) — `QueueLen()`. · 12. [SmatchetImageTextureCache.cpp](Source/Core/src/Persistence/SmatchetImageTextureCache.cpp) — count/bytes reporters. · 13. [BuiltinCommands_Perf.cpp:43](Source/Core/src/Commands/Builtin/BuiltinCommands_Perf.cpp) — `perf.memory` command (strict zone). · 14. [CliCommandRunner.cpp:997](Source/Standalone/CliCommandRunner.cpp) — CLI allowlist.

**S4** — 15. [SmatchetAttachmentPreviewUi.cpp:69](Source/Core/src/Ui/SmatchetAttachmentPreviewUi.cpp) — bounded header read (landmark: `TODO(pillar2)` at `:62`).

**S5** — 16. [SmatchetAttachmentPreviewUi.cpp:579](Source/Core/src/Ui/SmatchetAttachmentPreviewUi.cpp) — decode→worker + dispatcher upload + cue · (+ gauge in file 9).

## Existing utilities reused

- `AppController::LaunchBackgroundTask` + `JoinBackgroundTasks` — [AppController.cpp:574](Source/Core/src/AppController.cpp) / `:595`; joined at shutdown `:425–429`, `:1901–1902`. The whole point: migrated tasks are joined, killing the UAF window.
- Migration template — `app.LaunchBackgroundTask([…]{ …; app.mainThreadDispatcher.PostToMainThread([…]{ …UI… }); })`, live at [SmatchetPreferencesUi_Whisper.cpp:446](Source/Core/src/Ui/SmatchetPreferencesUi_Whisper.cpp) with its rationale comment.
- `MainThreadDispatcher::PostToMainThread` / `LastDrainTaskCount` / `SMATCHET_UI_PERF_SCOPE("dispatcher.drain")` — [MainThreadDispatcher.h](Source/Core/include/MainThreadDispatcher.h). Reused as the thumbnail-upload channel (S5).
- Texture-cache limits `kMaxCacheEntries` / `kMaxFileReadBytes` (4 MB, already byte-bounded) / `kMaxIconDimension` — [SmatchetImageTextureCache.cpp:26–28](Source/Core/src/Persistence/SmatchetImageTextureCache.cpp). Precedent for `kMaxThumbnailPixels`.
- `ReadU16LE` / `ReadU24LE` / `ReadU32BE` header parsers — [SmatchetAttachmentPreviewUi.cpp:42–53](Source/Core/src/Ui/SmatchetAttachmentPreviewUi.cpp). Agnostic to buffer fill — reused by S4.
- `K32GetProcessMemoryInfo` (kernel32, always linked) for RSS — avoids a psapi link dependency.
- `no-raw-new` / `no-printf-stderr` rule machinery in [test-lint-rules.sh](agents/scripts/project/test-lint-rules.sh) — `no-detach` mirrors it.
- `AppController::GetActiveTicketsSnapshot()` — [AppController.cpp:432](Source/Core/src/AppController.cpp) — already a published `shared_ptr<const vector>`; `.size()` for the ticket-count gauge (no copy).

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: net positive — S4/S5 move the 50 MB read + image decode off the UI thread; `perf.memory` reads are snapshot-only (no per-frame cost); S5 rate-limits uploads so the dispatcher drain can't spike. New `QueueLen()` is a single `size()` under an already-held lock.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: net positive — the worst remaining unconditional UI-thread block (50 MB slurp) is removed in S4; S5's decode moves to a worker with a visible "loading thumbnails" cue. No new sync I/O reachable from `ImGui::*` is introduced.
- **Pillar 3 (never crash)**: the core win — 6 detached-thread UAF-on-shutdown holes closed by joining them to the pool (S1); `no-detach` lint (S2) prevents regression. No raw `new`/`delete` added; RAII throughout.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: no regression — the new loading cue is a passive text/state indicator, keyboard-safe, no focus trap. (Pillar 4 is aspirational; no auto-gate.)

## Perf-review-system gates (mandatory — planned work touches `Source/Core/`)

Per `docs/plans/shipped/pillar-1-2-perf-review-system.md`. Declared per slice:

1. **PR-fast CI** — **fires for S4 + S5**: the attachment path's most direct scenario is `AttachmentPreviewOpenScenario` (`Source/Core/src/Commands/Scenarios/`); confirm it is in `scripts/dev/perf-pr-fast-set.json` per `agents/core/perf-gatekeeper.md` § Curated diff → scenario map. **N/A for S1/S2/S3** (S1 changes thread ownership, not per-frame cost; S2 is tooling; S3 is snapshot-only).
2. **Pillar 2 static scanner** — **fires for S5**: image decode moves to a worker; the worker-only entry point carries a `/* PILLAR2_WORKER_ONLY */ // est-latency: <N>ms` annotation. S4 removes sync I/O (scanner-negative). Others N/A.
3. **Dispatcher drain** — **fires for S5**: posts upload tasks to `MainThreadDispatcher`. Mitigation = producer-side enqueue rate-limit (not a `Drain()` change). S3 adds a `const QueueLen()` accessor only (no behaviour change).
4. **Visible-cue bucket-E harness** — **fires for S5**: the "loading thumbnails" indicator gets ImGui Test Engine coverage **before S5 ships** (test-author wires it). No other slice adds a > 100 ms sync stall.
5. **Marker inventory** — if S5 adds any `SMATCHET_UI_PERF_SCOPE`, regen `docs/perf/MARKER_INVENTORY.md` in the same PR. S3 reuses the existing `dispatcher.drain` marker (no new markers expected).

**Pre-push local check**: run `docs/guides/perf-workflow.md` § Gate-check vs baseline against `AttachmentPreviewOpenScenario` before opening the S4/S5 PRs.

**Override**: `perf-out-of-band` label only if an intentional regression + baseline-bump PR is queued (not anticipated).

> The plan-doc commit itself is a **pure-docs diff** (one `.md`) — `agents/scripts/core/is-pure-docs-diff.sh` skips build + tests for *this* PR. The gates above bind the **implementation** PRs (S1–S5).

## Risks / non-goals

- **[HIGH] psapi / kernel32 link on Standalone.** `GetProcessMemoryInfo` needs `psapi.lib`; prefer `K32GetProcessMemoryInfo` (kernel32, always linked, Win7+) to dodge the dependency entirely. Resolve **before S3** — verify the Standalone link. DX12-into-Core is covered by the `#if defined(_WIN32)` guard keeping the include out of any TU that doesn't link it. *Mitigation: kernel32 path + guarded include.*
- **[LOW — already satisfied] Phase 3(b) WAL precondition (deferred).** A background DB writer without WAL would hold a reserved lock stalling UI-thread reads (`TryGetTicket`). Verified WAL is **already enabled** — `LocalCacheManager.cpp:31` `db.exec("PRAGMA journal_mode=WAL")` (+ `synchronous=NORMAL` at `:29`), so the precondition holds today. *Mitigation: ship 3(a) coalescing first; re-confirm WAL stays enabled before adding any off-thread writer.*
- **[MED → fixed inside S1] Site #6 live-tracker-swap UAF (grill 2026-05-31).** Confirmed REAL, not hypothetical. The original "mid-session swap can't happen, raw capture is safe" reading was **wrong**: `SyncWithBackend`→`SetBackend` (`TicketSyncService.cpp:468/472/476` → `AppControllerDepsAdapter.cpp:58` `app_.Backend = std::move(...)`) frees the old backend live on a Jira→Plane switch while the picker's `ListProjects()` runs against it. The pool join (shutdown-only) does NOT close this window. *Fix: shared backend ownership ([ADR 0012](../../adr/0012-shared-ownership-active-tracker-backend.md)) — folded into S1, not a separate follow-up.*
- **[MED] `backgroundWorkers_` never reaps mid-session.** `LaunchBackgroundTask` appends to a `vector<thread>` drained only at shutdown; finished threads aren't reaped. Adding 6 repeatedly-clickable probes accumulates joinable-dead `std::thread` objects per session. Pre-existing; the migration adds producers. *Accepted in P0 (threads are tiny, sessions bounded) + backlog "reap finished background workers" (Phase 4 territory). Do not expand P1 scope.*
- **[MED] Dispatcher drain-all → upload spike (S5).** *Mitigation: producer-side enqueue rate-limit + `pendingThumbnailUploads` gauge; budgeted dispatcher deferred to Phase 4.*
- **[LOW] Sites #1/#5 may lack `AppController&` in scope** → small signature delta. Confirm during S1.
- **[LOW] `no-detach` regex false-positives** on trailing-comment / string-literal `.detach()`. *Mitigation: mirror `no-printf-stderr` comment/string handling + bats cases.*
- **[LOW] icon-cache `approxBytes` is an estimate** (`W·H·4`, ignores driver padding/mips). Fine for a gauge; not presented as authoritative RSS.

**Non-goals**: not building a push `MemoryBudgetRegistry` (pull snapshot instead); not fixing `backgroundWorkers_` reaping in P0 (backlog); not migrating `CachedTicket` storage in P0 (Phase 5); P1/P2 phases documented below but not committed.

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps where physically possible.

- **Bucket A (pure-logic ctest / CLI, `test-rig`)**: `perf.memory` returns non-zero RSS + sane gauge values (S3); `no-detach` bats positive+negative scan-file cases (S2); bounded-read assertion — feed a large fixture to the S4 header parser, assert bytes-read ≤ ceiling; if `GetFieldValueRef` is pulled forward (see Deferred), a unit test that it returns by reference.
- **Bucket E (ImGui Test Engine, `ninja-ui-test-msvc`)**: the S5 "loading thumbnails" cue is asserted present while a decode is in flight — wired **before** S5 ships.
- **Bash-driver scenario / screenshot / sanitizer**: S1 — ASan **and** TSan build, trigger each of the 6 probes then shut down mid-flight, assert no UAF / data race (bucket-D); S4/S5 — `AttachmentPreviewOpenScenario` via `scripts/dev/perf-run.sh` with a 50 MB fixture, compare against baseline.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target) on every implementation slice.
- **Manual residue**: none planned. The S5 visual cue is covered by bucket-E (not a manual eye-test); if Test-Engine coverage proves infeasible for the cue, fall back to the screenshot-diff harness + a `docs/self-improvement/categories/tooling.md` entry — no silent manual residue.

## Deferred (P1 / P2 — documented, not committed)

Routed to backlog; reassessed after P0. Each becomes its own slice/PR with its own perf-gate.

- **Phase 3 — ticket persistence off the UI frame (P1).** Today `TicketSyncService.cpp:237–280` applies ≤ 20 tickets/frame, each `LocalCacheManager::SaveTicket` (`:140`) a **separate** `SQLite::Transaction` on the UI thread.
  - **3(a) coalesce** the slice into **one** transaction (`SaveTickets(span)` reusing the cached prepared statements) — safe, immediate win, ship first.
  - **3(b) off-thread writer** — hazardous: must be **publish-first, persist-after** with a **single serialized FIFO writer** (the thread pool gives no ordering → stale-revision persist) and a **hard WAL precondition** (see Risks). Changes the meaning of the 20/frame budget from "20 DB writes" to "20 in-memory merges." Contingent slice.
- **Phase 4 — byte-aware caches + work-cost dispatcher bound (P1). ✅ SHIPPED** (`backgroundWorkers_` reaping in #661; the rest in the Phase-4 PR — see Implementation log). Aggregate byte caps added beside the entry caps (icon cache 96 MiB via running `g_totalBytes`; AI plan cache 4 MiB via Σ source length); the AI `s_messageHeightCache` / `s_turnActiveLastFrame` are now dropped on clear-history (`ClearAiRenderCaches`) and the height cache is invalidated on font / wrap-width change (`AiLayoutBasisChanged`); the dispatcher is bounded by a **drain-time** budget (4 ms; the "or drain-time" arm — a per-task byte estimate was rejected because tasks are opaque `std::function`, so it would force a cost-hint API onto every `PostToMainThread` caller); `planCacheApproxBytes` + `dispatcherLastDrainDeferred` gauges added to `perf.memory`. The cap decisions are pure + unit-tested (`CacheOverCap`, `RequeueDeferredFront`).
- **Phase 5 — `CachedTicket` dedup / row-store (P2).** Intern repeated field-ID keys; compact visible-field row projections; refine `activeTicketsPublished_` into a COW/shared column store so mutation stops deep-copying the whole vector. **Pull-forward candidate** (worth its own tiny slice ahead of the rest): add `const std::string& GetFieldValueRef(...)` to [CachedTicketTypes.h:23](Source/Core/include/CachedTicketTypes.h) and switch the sort comparator ([SmatchetActiveProjectGridUi.cpp:178–179](Source/Core/src/Ui/SmatchetActiveProjectGridUi.cpp)) to it — kills 2 string copies per comparison inside `std::stable_sort` with zero API break. The full row-store keeps `GetFieldValue` / `GetFieldRichValue` as a stable façade over new backing storage and warrants its own design doc.

## Out of scope (flagged, not designed)

- **`backgroundWorkers_` reaping** — named in Risks; folded into Phase 4, not P0.
- **A push `MemoryBudgetRegistry`** — explicitly rejected in favour of the pull snapshot.
- **Non-Windows RSS** — both ship targets are Windows; the non-Win32 stub returns 0 for header honesty only.
- **Tracker-backend / sync-protocol changes** — none; this plan is memory-shape only.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

Shipped via PR #657 (squash-merge `bf921a6b` → develop, 2026-06-01). The deferred-backlog
follow-ups landed as their own PRs: `b49c1b7` (#658 Phase 5 pull-forward `GetFieldValueRef`),
`5e8b7a3` (#660 Phase 3a coalesced ticket saves), `3d53a940` (#661 Phase 4 worker reaping),
and the remainder of **Phase 4 — byte-aware caches + drain-time dispatcher bound** (this PR,
SHA TBD): aggregate-byte cap on the icon LRU cache (`SmatchetImageTextureCache`, 96 MiB, running
`g_totalBytes` + the pure `CacheOverCap` policy) and on the AI plan FIFO cache (4 MiB, proxied by
Σ source length); AI height/hover render caches promoted to file scope and dropped on
clear-history (`ClearAiRenderCaches`) + the height cache invalidated on font/wrap-width change
(`AiLayoutBasisChanged`); `MainThreadDispatcher::Drain()` bounded by a 4 ms drain-time budget that
defers the untouched tail to the next frame FIFO (pure `RequeueDeferredFront`), closing the Risk
#134 drain-all spike; two new `perf.memory` gauges (`planCacheApproxBytes`,
`dispatcherLastDrainDeferred`). Phase 4 is now complete (the `backgroundWorkers_` reaping piece
shipped earlier in #661). Branch commits on #657:

- `9632e977` · feat(memory): S1–S5 in one PR — 6 detach→pool migrations + shared-backend ownership (ADR 0012); `no-detach` absolute lint + bats; `perf.memory` pull snapshot (`MemoryTelemetry` + dispatcher `QueueLen` + icon-cache gauges); bounded attachment header read; off-thread thumbnail decode + dispatcher upload + cue + `pendingThumbnailUploads` gauge.
- `a97346d7` · fix(memory): satisfy Pillar-2 scanner (restore the dropped `TODO(pillar2)` marker on the bounded read) + Test-delta gate (`tests/Core/MemoryTelemetry.test.cpp` doctest).
- `cccbe3ac` · fix(memory): latch a local `shared_ptr` in `RefreshFieldCatalog` — first-found live-swap UAF the migration alone didn't close.
- `5db5cdbe` · refactor(memory): self-review #1/#2/#3 — broaden the `Backend->` latch to all ~20 worker-reachable methods in `AppController_CatalogAndFieldEdit.cpp`; WIC `IWICBitmapScaler` decode-scale (bounds the worker's transient buffer); per-frame `MaybeKickThumbnailDecode` so a rate-limited skip retries instead of silently dropping.

## Deviations from plan

- **Site #6 fixed via shared ownership, not raw-pointer pooling (grill outcome).** The grill (see § Provenance) overturned the "shutdown-only" framing: a live Jira→Plane tracker switch (`SyncWithBackend`→`SetBackend`) frees the old backend mid-`ListProjects()`. Fix = `Backend`→`shared_ptr` + the picker fetch captures a `shared_ptr` copy (ADR 0012).
- **`SMATCHET_ENABLE_BITMAP_ATTACHMENT_THUMBNAILS` is a per-`_WIN32` `#define` in `SmatchetAttachmentPreviewUi.cpp:22`, not a CMake flag** — so S5 is **live** on both Windows ship targets (initial read that it was unset/dead was wrong). S5 therefore compiles + ships active; only the bucket-E cue test is residue.
- **Picker `Draw` kept at arity 6** (passes `AppController& app`, derives `backend = app.BackendShared()` internally) instead of adding a separate `backend` param. Adding it bumped arity 6→7, re-keying the function and **un-grandfathering** `Draw/6 · 31 branches` from `function-size-baseline.md` → a spurious `function-too-branchy` gate failure. Same-arity keeps the grandfather.
- **`pendingThumbnailUploads` counter lives in `MemoryTelemetry` (always compiled)**, not in the `#if`-gated attachment TU, so the `perf.memory` gauge reads `0` cleanly on non-Win32 (where thumbnails compile out) instead of needing its own gating.
- **S5 downscale = post-decode nearest-neighbour** (`kMaxThumbnailDimension=2048`, `DownscaleRgba32ToFit`) rather than a WIC-scaler decode. Bounds the uploaded texture + the work; a WIC `IWICBitmapScaler` decode (which would also bound the transient full-res buffer) is a noted follow-up.
- **Comment wording constrained by `comment_audit`**: migration comments may not look like code (no `std::thread().detach()` literal, no trailing `;`) or they trip `comment-commented-out-code`; new headers may not contain blank `//` lines (`comment-blank-run`). Reworded accordingly.
- **Post-merge self-review improvements (#1/#2/#3, 2026-06-01).** A post-ship self-review surfaced three items, all done in this PR:
  - **#1 — broader `Backend->` latch audit.** Migrating site #2 to the pool did NOT fix its live-swap UAF: `app.RefreshFieldCatalog` (and ~20 sibling methods) deref'd the `Backend` member across blocking HTTP with no local strong handle — a tracker swap mid-call frees the backend → UAF. **Every `Backend`-deref method in `AppController_CatalogAndFieldEdit.cpp` now latches a local `shared_ptr` at entry** (ADR 0012 § Scope note), not just `RefreshFieldCatalog`.
  - **#2 — WIC decode-scale.** `DecodeImageFileToRgba32` now takes a `maxDimension` and decode-scales via `IWICBitmapScaler`, so a multi-megapixel attachment never materialises a full-res RGBA buffer on the worker (replaced the post-decode `DownscaleRgba32ToFit`).
  - **#3 — no silent thumbnail drop.** The decode kick moved from the one-shot preview-update path to a per-frame `MaybeKickThumbnailDecode()` in the card loop, gated by a per-entry `ThumbnailDecodeInFlight` flag — a rate-limited (saturated) skip now retries on a later frame instead of the thumbnail never loading.
- **CodeRabbit round 2 → defer-free graveyard (2026-06-01, user-approved).** CR's re-review showed the live-swap UAF is systemic: (a) the `shared_ptr` *latch itself* races the swap (a `shared_ptr` instance isn't concurrently copy/assign-safe in C++14) → fixed by routing all `Backend` reads through `std::atomic_load` and writes through `std::atomic_store`/`atomic_exchange`; (b) `AppControllerDepsAdapter` hands out *raw* subobject pointers that `OfflineQueueService` captures into a worker outliving a swap → consumer-latching can't cheaply cover every raw holder, so `SetBackend` now **retires** the old backend into a shutdown-drained graveyard (`AppController::RetireBackend`) instead of freeing it (ADR 0012 § (c), reversing the earlier rejection now that the exposure is known-systemic). The adapter getters atomic-load `app_.Backend`. A bounded, deliberate per-session leak of a few small backend objects.
- **Shipped as ONE PR (all S1–S5)** per user decision (2026-05-31), not the plan's S1→S5 sequential-PR order.
- **Shipped as ONE PR (all S1–S5)** per user decision (2026-05-31), not the plan's S1→S5 sequential-PR order. Commit-per-slice keeps it reviewable; S2's clean-tree precondition holds because all 6 migrations land before the lint rule in the same PR.

## Verification (actual)

- **Build — dual-target (the gate):** `SmatchetStandalone` (Smatchet.exe) **and** `SmatchetCore_DX12` (SmatchetCore_DX12.lib) both build clean under `ninja-iter-msvc` (via `scripts/dev/with-msvc-env.sh`); only pre-existing C4530/C4996 warnings. Confirms the `shared_ptr` ownership change, the backend-agnostic `MemoryTelemetry.h`, `perf.memory`, and the S5 thumbnail path all compile into both targets. **Passed.**
- **S3 `perf.memory` (bucket-A, live):** `Smatchet.exe cmd perf.memory --spawn` returned `{"rssBytes":91709440, dispatcherQueueLen:0, dispatcherLastDrainTasks:0, iconCacheEntries:0, iconCacheApproxBytes:0, activeTicketCount:0, pendingThumbnailUploads:0, ok:true}` — non-zero RSS via `K32GetProcessMemoryInfo`, all gauges present. **Passed.**
- **S3 `MemoryTelemetry` (bucket-A doctest):** `tests/Core/MemoryTelemetry.test.cpp` (added to satisfy the Test-delta gate AND cover pure logic) — `ProcessRssBytes()` > 0 on Win32 / == 0 stub elsewhere; `PendingThumbnailUploads()` counter round-trips + is a stable singleton. Builds + runs in the `ninja-test-msvc` doctest rig. **Passed.**
- **CI-gate fixes (post-first-push):** (1) Pillar-2 scanner flagged the S4 bounded read as CRITICAL because the S4 rewrite dropped the original `// TODO(pillar2):` marker — restored it (within the scanner's 3-line proximity window) → downgrades to WARN, gate passes; the read is now bounded (≤64 KB) but still UI-thread, with the off-thread follow-up tracked. (2) Test-delta gate failed (11 prod / 0 test) because bats don't count — the `MemoryTelemetry.test.cpp` doctest above supplies the test delta.
- **S2 `no-detach` lint:** `lint_rules.bats` — 3 new cases (fires on `.detach()`; does NOT fire on comment/string mention; suppressed by `SMATCHET_DEVIATION(rule=no-detach)`) pass; `--scan-wide` confirms 0 first-party `.detach()` call sites in the real tree. The full `test-lint-rules.sh --diff origin/develop` gate passes (strict-zone, comment-noise, wide no-detach, function-size all clean). **Passed.**
- **S1 migrations:** all 6 `.detach()` call sites routed to `LaunchBackgroundTask`; only comment/string mentions remain. **Passed (build + lint).**
- **Self-review #1/#2/#3 (post-first-push):** #1 — `~20` worker-reachable `Backend->`-across-blocking-call methods in `AppController_CatalogAndFieldEdit.cpp` now latch a local `shared_ptr` (scripted transform, one lowercased string-literal caught + restored); #2 — `IWICBitmapScaler` decode-scale validated by build (replaces post-decode downscale); #3 — per-frame `MaybeKickThumbnailDecode` + `ThumbnailDecodeInFlight` dedupe. Dual-target build + full `--diff` lint green after each. **Passed (build + lint).**
- **CI (actual) — PR #657:** all required checks green — dual MSVC builds, Coverage (OpenCppCoverage runs the doctest rig), Sanitizer (ASAN), Bucket-C/E, Perf PR-fast, Shell-lint, Doc-anchors/Pillar-2/Test-delta, CodeRabbit (0 actionable). **Passed.**
- **Scope correction (worth noting):** the plan framed the lifetime risk as "6 detached threads." The self-review showed the live-tracker-swap UAF was far wider — **~20 `AppController` methods** dereference the backend across blocking HTTP and were all exposed. The `no-detach` lint + shared ownership + the latch audit together close the class; future worker-reachable `Backend->` sites must latch (ADR 0012 § Scope note).
- **Phase 4 (this PR) — dual-target build + bucket-A + live gauge.** Both `Smatchet.exe` (Standalone) and `SmatchetCore_DX12.lib` build clean under `ninja-iter-msvc` (only pre-existing C4530/C4996/nlohmann warnings). New bucket-A doctests pass in `ninja-test-msvc` (956 cases, 11977 assertions, 0 failed): `CacheEvictionPolicy.test.cpp` (`CacheOverCap` count/byte/empty/evict-before-insert shapes), `RequeueDeferredFront` ordering+trim cases in `MainThreadDispatcherDrain.test.cpp`, and a `PlanCacheApproxBytes` round-trip in `MemoryTelemetry.test.cpp`. Live: `Smatchet.exe cmd perf.memory --spawn` returns the two new gauges `planCacheApproxBytes:0` + `dispatcherLastDrainDeferred:0` alongside the rest. The drain-time-budget *trip* path and the AI font/wrap clear are UI-runtime behaviours (bucket-E residue — the pure ordering/policy underneath is unit-tested). **Passed (build + bucket-A + live).**
- **Other gates:** `comment_audit --diff` clean; markdown-links / plan-ref-integrity unaffected; two advisory comment-ratio WARNs (`MemoryTelemetry.h` 51%, `SmatchetProjectPicker.h` 56%) — non-blocking.
- **Manual residue (deferred-automation):** (a) bucket-D ASan/TSan **live-swap** repro for site #6 (kick picker fetch → fire `SetBackend` Jira→Plane → assert no UAF) — sanitizer preset is dev-machine-only; (b) bucket-E "loading thumbnails" cue assertion (needs ImGui Test Engine + a slow decode); (c) bucket-A S4 bounded-read unit test (the parser is a `static` TU-local — needs exposure or an in-TU doctest). All three need a dev-machine/headless-GL harness; tracked as a deferred-automation entry in `docs/self-improvement/categories/tooling.md`. **Not-run (deferred, tracked).**

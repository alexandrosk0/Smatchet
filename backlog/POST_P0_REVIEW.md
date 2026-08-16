# Post-fix code review — issues introduced by PRs #6 / #7 / #8

> **Deprecated as a work queue (2026-07-06).** Closed historical ledger (all items resolved as of 2026-07-05) — do not file new work here. New agent-facing items go to the live self-improvement backlog ([`docs/self-improvement/AGENT_SELF_IMPROVEMENT.md`](../docs/self-improvement/AGENT_SELF_IMPROVEMENT.md)); product bugs become GitHub Issues (ADR-0014).
>
> Companion to [`BACKLOG_CODE_REVIEW.md`](BACKLOG_CODE_REVIEW.md). This document tracks issues found by reviewing the **new code** added by the P0 sweep and follow-up — things the fixes themselves introduced or missed. Nothing here duplicates the 61 numbered items in `BACKLOG_CODE_REVIEW.md` §7.
>
> Method: three parallel reviewer agents (one per PR) inspected the substantive new code with full access to the original review, with explicit instructions to look for *new* hazards only. Each finding has file:line citations.
>
> Status legend matches `BACKLOG_CODE_REVIEW.md` §7:
> - ⏳ **OPEN** — not yet fixed.
> - 🟡 **PARTIAL** — first step shipped; follow-up tracked inline.
> - ✅ **DONE** — landed on `develop`; behavioural validation may still be pending.

---

## Current status — all 34 closed (2026-05-11 after PRs #9–#22; re-verified 2026-08-16)

| Severity | Total | Done | Partial | Open |
|----------|-------|------|---------|------|
| **P0** | 5 | 5 ✅ | 0 | **0** |
| **P1** | 17 | 17 ✅ | 0 | **0** |
| **P2** | 12 | 11 ✅ | 1 🟡 | **0** |

**Every numbered entry is closed on `develop`.** The one PARTIAL is item 24 (`FlushFileSink` is honest now but has no in-tree caller — not a bug as it stands; tracked for the eventual crash-handler wire-up).

> **Update 2026-07-05:** item 24 (the lone PARTIAL) is now **resolved on the graceful path** — `AppController::~AppController` calls `Logger::Instance().FlushFileSink()` after `JoinBackgroundTasks()`. The crash-handler half is intentionally omitted (superseded by the async-signal-safe `SmatchetCrashHandler`, which must not touch the logger mid-crash) — see **A4** in [`BACKLOG_CODE_REVIEW.md`](BACKLOG_CODE_REVIEW.md) for the full rationale. No other claim in this doc has gone stale.
>
> **Triage 2026-08-16:** re-checked — every numbered entry (1–34) is still closed; item 24's graceful half remains wired in `~AppController`. The "validation still pending" list below is **historical**: its build re-verify pins `0a79de5` and the six per-PR smokes are in the SUPERSEDED bucket of the [`MANUAL_TEST_QUEUE.md`](MANUAL_TEST_QUEUE.md) triage. No open items in this doc. Note that three of the fixes here later turned out to have residual holes, all since closed: **DR15** (a deadlock opened by #16), **DR16** (a duplicate-POST hole adjacent to B2), **DR17** (a stale-cache hole in A1) — see [`DEEP_REVIEW_2026-07-07.md`](DEEP_REVIEW_2026-07-07.md).

### Validation still pending on develop tip (`0a79de5`)

Code is in; these are human-only steps captured in `backlog/MANUAL_TEST_QUEUE.md` (added by [PR #22](https://github.com/alexandrosk0/Smatchet/pull/22)):

- Build re-verification on a fresh checkout of `0a79de5`.
- Six per-PR manual smokes covering items 9, 10, 12, 18, 20, 22.

Pre-existing cppcheck baseline noise was closed by [PR #21](https://github.com/alexandrosk0/Smatchet/pull/21) ([Smatchet#18](https://github.com/alexandrosk0/Smatchet/issues/18)).

### PR landings that produced this state

| PR | Title | Items closed |
|----|-------|--------------|
| [#9](https://github.com/alexandrosk0/Smatchet/pull/9) | `fix(logger): harden async file-sink lifecycle` | 1, 2, 6, 7, 8 (P0/P1) + 24 (P2 partial) |
| [#10](https://github.com/alexandrosk0/Smatchet/pull/10) | `fix(stability): close 7 shutdown / contention crash paths` | 3, 4, 5 (P0) + 11, 16, 17, 21 (P1) |
| [#11](https://github.com/alexandrosk0/Smatchet/pull/11) | `refactor(config): finish the split — drop json.hpp from public header` | 13, 14 (P1) |
| [#12](https://github.com/alexandrosk0/Smatchet/pull/12) | `review: P1 cleanup — 4 fixes from POST_P0_REVIEW` | 9, 10, 18, 22 (P1) + Impl member reorder hardening |
| [#13](https://github.com/alexandrosk0/Smatchet/pull/13) | `docs(controller): clarify automation-worker lifetime contract` | 19 (P1) |
| [#14](https://github.com/alexandrosk0/Smatchet/pull/14) | `docs(config): explain legacy-MCP migration ordering` | 15 (P1) |
| [#15](https://github.com/alexandrosk0/Smatchet/pull/15) | `fix(plane): snapshot TrackerConfig under cache lock in CreateIssue` | 12 (P1) |
| [#16](https://github.com/alexandrosk0/Smatchet/pull/16) | `fix(audit): fallback path when primary audit file is unwritable` | 20 (P1) |
| [#17](https://github.com/alexandrosk0/Smatchet/pull/17) | `polish: P2 batch — items 25/26/27/28/29/31/32` | 25, 26, 27, 28, 29, 31, 32 (P2) |
| [#20](https://github.com/alexandrosk0/Smatchet/pull/20) | `polish: P2 batch 2 — items 23/30/33/34` | 23, 30, 33, 34 (P2) |
| [#21](https://github.com/alexandrosk0/Smatchet/pull/21) | `chore(cppcheck): clear pre-existing baseline noise` | closes [#18](https://github.com/alexandrosk0/Smatchet/issues/18) |
| [#22](https://github.com/alexandrosk0/Smatchet/pull/22) | `docs(backlog): add MANUAL_TEST_QUEUE.md for pending human-only smokes` | tracks the validation list |

Last build verification on develop tip (`0a79de5`) is in `backlog/MANUAL_TEST_QUEUE.md`; per-PR builds were clean.

---

## P0 — bugs introduced by the fixes (do first)

These are real defects in the new code, not just polish. Each can fault at runtime.

1. ✅ **DONE (commit pending in `review/logger-hardening`).** `Logger::SetFileSinkPath` now holds `m_fileSinkLifecycleMutex` for the entire stop+restart sequence; `StopFileSinkWorkerLocked` is private and requires the caller to hold the same mutex, so concurrent `SetFileSinkPath` calls and dtor races can no longer assign over a still-joinable thread.

2. ✅ **DONE (commit pending in `review/logger-hardening`).** `Log()` re-checks `m_fileSinkPath` under `m_fileSinkMutex` before pushing; `StopFileSinkWorkerLocked` clears the path AND the queue under the same mutex. Worst case is a benign push onto a queue that the next `SetFileSinkPath` will clear — no UB, no leak across many sink cycles, no push to a queue with a dead consumer that lives forever.

3. ✅ **DONE (commit pending in `review/process-stability`).** `AutomationWorkerLoop` now wraps the per-job body (`InitLuaCore` + setup-script load + `RunAutomationJob`) in `try { … } catch (const std::exception&) { LOG_ERROR } catch (…) { LOG_ERROR }`. The worker keeps running for subsequent jobs — same liveness contract as a UI-thread exception handler.

4. ✅ **DONE (commit pending in `review/process-stability`).** `BackendAuditTrail::AuditWriter` now takes `AuditMutex()` around each `std::ofstream << line` write — the same mutex `ReadRecentEvents` already holds. Readers can no longer observe a partially-flushed line. Also: file-open failure and write failure are now logged once (rate-limited atomic flags) via the Logger ring (not the audit trail itself, to avoid log-on-log loops) so audit-data loss is observable.

5. ✅ **DONE (commit pending in `review/process-stability`).** Added `mutable std::mutex stmtMutex_` to `LocalCacheManager` and acquired it at the top of `SaveTicket` / `TryGetTicket` (the only callers of the cached prepared-statement slots that can fire from both the UI and Lua automation worker threads). Header comment documents the rationale: `SQLite::Statement` instances are not thread-safe even when the connection uses `OPEN_FULLMUTEX`.

---

## P1 — significant new code-health concerns

### From PR #6 (the P0 sweep)

6. ✅ **DONE (commit pending in `review/logger-hardening`).** `FlushFileSink()` is now synchronous: it bumps a monotonic `m_fileSinkFlushRequestedGen` under `m_fileSinkMutex` while holding `m_fileSinkLifecycleMutex`, notifies the worker, then waits on a dedicated `m_fileSinkAckCv` until `m_fileSinkFlushAckedGen >= targetGen` (or shutdown). The worker publishes ack at the end of each batch. Header docstring updated to describe the new contract.

7. ✅ **DONE (commit pending in `review/logger-hardening`).** `FileSinkWorker` no longer `return`s permanently on `!good()`. After `kFileSinkMaxConsecutiveErrors = 5` failed batches it closes the file, sleeps `kFileSinkErrorBackoffMs = 1000ms`, reopens, and resumes the loop. Persistent failure degrades to a no-op sink but never abandons the worker — `FlushFileSink` waiters still get released via the ack publish on every batch (success or failure).

8. ✅ **DONE (commit pending in `review/logger-hardening`).** `SetFileSinkPath` is now atomic under `m_fileSinkLifecycleMutex` end-to-end (idempotency check → stop → assign → spawn). No TOCTOU.

9. ✅ **DONE (commit `b610ec6` in `review/p1-cleanup`).** Heartbeat lambda now `wait_for`s on a per-Impl `shutdownCv` with a 1s timeout instead of sleeping unconditionally. `OnStop()` flips `shuttingDown` and notifies before `svr.stop()`, so every SSE worker returns within microseconds regardless of how many clients are connected. `OnStart()` resets the flag for start-after-stop cycles.

10. ✅ **DONE (commit `945d42a` in `review/p1-cleanup`).** `CellIdScope` constructor now accepts the column index alongside the field id and pushes the index when the field id is empty. `RenderFieldCell` takes the index from the grid caller's column loop; synthetic / errored-catalog rows no longer collapse onto a single ImGui id.

11. ✅ **DONE (commit pending in `review/process-stability`).** `BlameAnalysisUi::BlameState` now has an explicit destructor that signals `worker.Cancel.store(true)` and joins `worker.Thread` if joinable. The PR #8 pimpl move made step (b) easy: `~BlameAnalysisUi` releases the `unique_ptr<BlameState>`, which fires `~BlameState`, which joins synchronously before any `BlameState` member is destroyed. No more `std::terminate` at exit on a joinable worker.

12. ✅ **DONE.** Moved the `TrackerConfig` snapshot, header build, and `workspaceSlug` capture inside the `planeCacheMutex_` critical section so they're consistent with the cache lookup. HTTP POST still runs after the lock is released. The advertised "config + headers + project under the cache lock" contract now matches the code.

### From PR #7 (ConfigManager split)

13. ✅ **DONE (commit pending in `review/configmanager-finish`).** `ConfigManager.h` now pulls only `<nlohmann/json_fwd.hpp>` (~75 LOC of forward declarations) instead of the full `<nlohmann/json.hpp>` (~30k LOC of templated code). `CommentTemplate::to_json`/`from_json` are declared as friends in the header and defined in `ConfigManager.cpp`; nlohmann's `adl_serializer` still finds them via ADL at every call site. `<algorithm>`/`<cctype>`/`<iterator>` also dropped from the public header — were only needed by the now-moved inline view helpers (item 14). Two TUs that were relying on transitive includes got explicit includes added (`CppSyntaxHighlight.cpp` for `<algorithm>::std::any_of`, `SmatchetLocalization.cpp` for full `<nlohmann/json.hpp>`).

14. ✅ **DONE (commit pending in `review/configmanager-finish`).** All seven `SmatchetViewsDiskDetail::*` functions moved into the anonymous namespace of `ConfigManager.cpp`. Public seam stays as the existing `ConfigManager::ViewWorkspaceToViewsStore` / `ViewsStoreToViewWorkspace` statics (which forward to the anon-namespace impls). External grep confirms zero external callers had been using the namespace directly — the move is invisible to every other TU.

15. ✅ **DONE.** Investigated and concluded the divergence is intentional: env/CLI overrides are deliberately *not* persisted (they're meant to apply only to the current process), and the migration `Save(cfg)` must run before overrides so the on-disk file gets a clean re-encrypted token without ephemeral override values mixed in. Cache holding override-applied cfg vs disk holding pre-override cfg matches pre-split behavior. Added a block comment at the migration site documenting this ordering invariant so future readers don't try to "fix" it by moving Save below the override block (which would actually break the override semantics).

### From PR #8 (P0 partials + P1/P2 sweep)

16. ✅ **DONE (commit pending in `review/process-stability`).** `MainThreadDispatcher` got `std::atomic<bool> shuttingDown_` + `BeginShutdown()`; `PostToMainThread` checks the atom outside the lock and re-checks under it so late posts no-op cleanly. `~AppController` now calls `mainThreadDispatcher.BeginShutdown()` as the **first** step, before any worker joins, so late posts during shutdown are dropped instead of touching a soon-to-be-destroyed mutex. Queue now actually enforces `kMaxQueueSize = 4096` (was unbounded despite the docstring).

17. ✅ **DONE (commit pending in `review/process-stability`).** `Drain()` iterates destructively: `for (auto& t : tasks) { t(); t = nullptr; }`. Captures (especially `shared_ptr<X>`) are released after each task instead of being kept alive across the whole loop.

18. ✅ **DONE (commit `c400ab3` in `review/p1-cleanup`).** Added `std::atomic<uint64_t> Revision_` to `Views`, bumped on every mutation (`Activate`/`Create`/`UpdateActive`/`DeleteActive`/backend swap). `BumpRevision()` is also called from the grid's column-width/sort-spec persistence (which mutates via `GetActiveViewMutable`). `GridFrameContext`'s cache key now includes `viewsRevision`, so in-place view edits invalidate within one frame.

19. ✅ **DONE.** The code-level fix shipped in PR #10 (`fix(stability)`): `~AppController` now flips `automationWorkerShuttingDown_` under the job mutex, notifies, and `automationWorker_.join()`s *before* any member destruction begins. That join is the happens-before barrier — per-iteration `bgState` (with its `__smatchet_app = this`) cannot survive into member-dtor land. PR #13 then added the contract docstrings: `automationWorker_` in `AppController.h:679-688` and the `state["__smatchet_app"] = this` comment in `AppController_LuaBindings.cpp:529-538` now spell out the lifetime guarantees.

20. ✅ **DONE.** Queue cap (kWriterQueueMax = 512, drops oldest) was already in place from PR #8; rate-limited LOG_ERROR on first open / first write failure shipped in PR #10. This commit adds the remaining fallback-path leg: on primary-file open failure, the writer attempts `<userdata>/smatchet_backend_audit_fallback.jsonl` and writes there instead (with its own once-per-process LOG_ERROR on fallback write failure). When BOTH primary and fallback can't be opened, a dropped-event counter increments and LOG_ERRORs at log-spaced multiples (1 / 10 / 100 / 1000 / 10000) so a persistent failure stays visible without spamming. ReadRecentEvents continues to read only the primary file; the fallback is for post-mortem analysis when the primary is unwritable.

21. ✅ **DONE (commit pending in `review/process-stability`).** `RunLuaSetupScript` now takes `automationJobMutex_` for the `find` + `push_back`. `AutomationWorkerLoop` takes the same mutex briefly to snapshot the vector into a per-job local before iteration (under `try/catch` from #3 above) — workers iterate the snapshot, eliminating the data race on reallocation.

22. ✅ **DONE (commit `d13a0d7` in `review/p1-cleanup`).** On `Smatchet_ImplDX12_InitBackend` failure inside `UpdateRendererColorFormat` we now clear `ImplData->Initialized` and record the error in `LastInitError`. The existing lazy-retry path inside `BuildFrame` (~once per second while UI is visible) then re-attempts `Initialize()` with the cached options. All `DrawUI`/`RenderDrawData` paths already gate on `Initialized`, so no calls reach a torn-down backend.

---

## P2 — polish

### From PR #6

23. ✅ **DONE.** Added a `Warning` channel to `TrackerIssueFetchSummary` / `TrackerIssueFetchPack` / `StreamingSyncState`, with an optional `outWarning` parameter on `ITrackerClient::FetchIssues` (both Jira and Plane overrides updated). PlaneClient's page-cap message now goes to `Warning` instead of `FetchError`. `ApplyIssueFetchPack` and `TickStreamingApply` surface non-empty `Warning` as a `LastTrackerTicketSyncWarning` banner + "Sync Warning" toast (streaming path) while still firing `requestDeferredLiveTrackerBackendSuccessNotify_()` — the partial pull is no longer misclassified as a fetch failure.

24. 🟡 **PARTIAL (commit pending in `review/logger-hardening`).** `FlushFileSink` no longer lies (item 6 above) so the public surface is honest now. Still has no in-tree caller — a future wire-up into the crash handler / signal path or into `~AppController` (to ensure logs are flushed before SQLite/MCP shutdown) closes this. Not a bug as it stands.

25. ✅ **DONE.** All reads/writes of `isDeletingStale_` now go through explicit `.load()` / `.store()` to match the sibling atomics on the same expressions.

26. ✅ **DONE.** Verified that `lastCallstackIssueKey` is now a member of the `BlameState` struct (referenced as `State().lastCallstackIssueKey`); no `s_lastCallstack…` file-static remains in `BlameAnalysisUi.cpp`. Resolved by an earlier change that wasn't reflected in this row.

### From PR #7

27. ✅ **DONE.** Both `ScopedFileLock::Acquire` failure branches now `LOG_WARN` with the path and the OS error code before proceeding without exclusive access — Win32 covers both `CreateFileW` and `LockFileEx`, POSIX covers `open()` and `flock(LOCK_EX)`. Subsequent IO is unchanged; the lock degrades to advisory-only with observable telemetry.

28. ✅ **DONE.** `EnsureDirectoryExists` now routes through `Utf8ToWide` and calls `GetFileAttributesW` / `CreateDirectoryW`. Non-ASCII user-data paths (é/ñ/CJK) survive on systems whose active code page isn't UTF-8.

29. ✅ **DONE.** Changed to `constexpr char kDefaultImGuiDockLayoutIni[] = …;` so the array storage itself is constexpr and `sizeof` is meaningful at compile time.

### From PR #8

30. ✅ **DONE.** Added `virtual bool IPlugin::TryGetMcpStatusSnapshot(McpServerStatus&) const { return false; }` (gated by `SMATCHET_WITH_MCP`). `McpPlugin` overrides it to fill the snapshot and return true; `PluginHost::GetMcpServerStatus` now iterates plugins via the virtual hook with no RTTI. Same pattern as the `MatchesConfig` hook added under CODE_REVIEW item 20.

31. ✅ **DONE.** Captures the active index before erasing and picks `views[min(activeIndex, size-1)]` as the new active view — deleting the last view now selects its neighbour instead of jumping to the front.

32. ✅ **DONE.** Freshly prepared statements now skip the redundant `reset()` / `clearBindings()` pair; existing slots still take the reset path before reuse.

33. ✅ **DONE.** After the orphan-sink merge loop in the HTML→markdown converter, non-empty `tableRows` are now flushed via `appendPipeTable(tableRows)` (with `fellBack = true` so callers still prefer raw mode). Documents that end mid-table render as best-effort pipe-table output instead of vanishing.

34. ✅ **DONE (documented).** Investigation: the suggested fixes don't actually solve the problem. Caching compiled `sol::function` refs across jobs isn't viable — bytecode bound to a destroyed state can't be replayed, and persisting `bgState` across jobs would lose the per-job isolation guarantee that the fresh-state pattern provides. The right resolution is contractual: setup scripts must be defining-only (declare functions / tables / constants) and avoid side effects at module-load level. Added a block comment at the setup-script load site (`AppController_LuaBindings.cpp`) spelling out the contract and the reasoning so a future reader doesn't try to "optimise" the re-run.

---

## Summary

**34 new findings** beyond the original 61-item backlog:
- **5 P0** (Logger lifecycle races, missing try/catch in Lua worker, audit-writer race, SQLite statement non-thread-safety).
- **17 P1** (file-sink robustness, MCP shutdown latency, ImGui ID empty-fieldId, BlameUi terminate-at-exit, ConfigManager.h still pulls `json.hpp` unnecessarily, MainThreadDispatcher unbounded + unsynchronized destruction, GridFrameContext stale-on-view-edit, plus more).
- **12 P2** (polish: dead API surfaces, style consistency, UTF-8 path handling, error logging).

The **single most impactful follow-up** is item #13 — switching `ConfigManager.h` to `<nlohmann/json_fwd.hpp>` and moving the `to_json/from_json` definitions out, plus item #14 (move the unused-by-anyone-else `SmatchetViewsDiskDetail` namespace into the cpp anonymous namespace). Together those two changes finally cash in the build-time win that was the entire point of the ConfigManager split.

The next most impactful is the **Logger lifecycle hardening** (items 1, 2, 6, 7, 8) — the async file sink shipped functional but has multiple race conditions that can `std::terminate` or silently lose entries.

The third group is the **PR #8 process-stability set** — `AutomationWorkerLoop` missing try/catch (#3), `BackendAuditTrail` async-writer race (#4), `LocalCacheManager` prepared statements (#5), `MainThreadDispatcher` lifetime (#16), `__smatchet_app` background-state outlive (#19), `ShutdownState` for BlameAnalysisUi worker (#11). Each is a real shutdown- or contention-time crash path.

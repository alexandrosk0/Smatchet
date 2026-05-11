# Post-fix code review — issues introduced by PRs #6 / #7 / #8

> Companion to [`CODE_REVIEW.md`](CODE_REVIEW.md). This document tracks issues found by reviewing the **new code** added by the P0 sweep and follow-up — things the fixes themselves introduced or missed. Nothing here duplicates the 61 numbered items in `CODE_REVIEW.md` §7.
>
> Method: three parallel reviewer agents (one per PR) inspected the substantive new code with full access to the original review, with explicit instructions to look for *new* hazards only. Each finding has file:line citations.
>
> Status legend matches `CODE_REVIEW.md` §7:
> - ⏳ **OPEN** — not yet fixed.
> - 🟡 **PARTIAL** — first step shipped; follow-up tracked inline.
> - ✅ **DONE** — landed on `develop`; behavioural validation may still be pending.

---

## Current status (as of 2026-05-11, after PRs #9 / #10 / #11 merged)

| Severity | Total | Done | Partial | Open |
|----------|-------|------|---------|------|
| **P0** | 5 | 5 ✅ | 0 | **0** |
| **P1** | 17 | 12 ✅ | 0 | 5 |
| **P2** | 12 | 1 🟡 | 0 | 11 |

**P0 list is empty — no known bug introduced by the P0 sweep can still fault at runtime.**

Open work (in priority order):

- **P1 #9** — MCP SSE heartbeat blocks process shutdown for up to 1s per client.
- **P1 #10** — `CellIdScope` ID collision when `column.FieldId` is empty.
- **P1 #18** — `GridFrameContext` cache key misses in-place view edits (stale columns until catalog rev bump).
- **P1 #20** — `AuditWriter` silent on disk failure. Partially addressed in PR #10 (added rate-limited LOG_ERROR); the queue-with-cap-or-fallback-path follow-up is still open.
- **P1 #22** — `SmatchetImGuiHost::UpdateRendererColorFormat` torn-down backend on init failure.
- **10 P2** — items 23, 25-34. Polish, dead surfaces, UTF-8 path handling, UX nits, MarkdownConvert edge cases.

### PR landings that produced this state

| PR | Title | Items closed |
|----|-------|--------------|
| [#9](https://github.com/alexandrosk0/Smatchet/pull/9) | `fix(logger): harden async file-sink lifecycle` | 1, 2, 6, 7, 8 (P0/P1) + 24 (P2 partial) |
| [#10](https://github.com/alexandrosk0/Smatchet/pull/10) | `fix(stability): close 7 shutdown / contention crash paths` | 3, 4, 5 (P0) + 11, 16, 17, 21 (P1) |
| [#11](https://github.com/alexandrosk0/Smatchet/pull/11) | `refactor(config): finish the split — drop json.hpp from public header` | 13, 14 (P1) |

Final build verification on develop tip (`93f561b`): `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12` → **149/149 both targets clean**.

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

9. ⏳ **MCP SSE heartbeat blocks process shutdown for up to 1s per connected client** — `Plugins/Mcp/McpPlugin.cpp:621-635`. The lambda does `std::this_thread::sleep_for(1s)` before each heartbeat write. `impl_->svr.stop()` cannot reclaim a worker mid-sleep. With N clients that's up to N seconds of `~McpPlugin` latency. **Fix:** condvar-wait on a shutdown atom with a 1s timeout, or chunk the sleep into ~100ms ticks that re-check shutdown state.

10. ⏳ **`CellIdScope` collides cells when `column.FieldId` is empty** — `Source_Core/src/TicketFieldEditor.cpp:1414`. `ImGui::PushID("")` hashes identically for every empty-string column — synthetic / errored-catalog rows collapse onto one widget ID; edit state and popups leak between rows. **Fix:** `assert(!column.FieldId.empty())` or fall back to `ImGui::PushID((int)columnIndex)` when empty.

11. ✅ **DONE (commit pending in `review/process-stability`).** `BlameAnalysisUi::BlameState` now has an explicit destructor that signals `worker.Cancel.store(true)` and joins `worker.Thread` if joinable. The PR #8 pimpl move made step (b) easy: `~BlameAnalysisUi` releases the `unique_ptr<BlameState>`, which fires `~BlameState`, which joins synchronously before any `BlameState` member is destroyed. No more `std::terminate` at exit on a joinable worker.

12. ✅ **DONE.** Moved the `TrackerConfig` snapshot, header build, and `workspaceSlug` capture inside the `planeCacheMutex_` critical section so they're consistent with the cache lookup. HTTP POST still runs after the lock is released. The advertised "config + headers + project under the cache lock" contract now matches the code.

### From PR #7 (ConfigManager split)

13. ✅ **DONE (commit pending in `review/configmanager-finish`).** `ConfigManager.h` now pulls only `<nlohmann/json_fwd.hpp>` (~75 LOC of forward declarations) instead of the full `<nlohmann/json.hpp>` (~30k LOC of templated code). `CommentTemplate::to_json`/`from_json` are declared as friends in the header and defined in `ConfigManager.cpp`; nlohmann's `adl_serializer` still finds them via ADL at every call site. `<algorithm>`/`<cctype>`/`<iterator>` also dropped from the public header — were only needed by the now-moved inline view helpers (item 14). Two TUs that were relying on transitive includes got explicit includes added (`BlameSyntaxHighlight.cpp` for `<algorithm>::std::any_of`, `SmatchetLocalization.cpp` for full `<nlohmann/json.hpp>`).

14. ✅ **DONE (commit pending in `review/configmanager-finish`).** All seven `SmatchetViewsDiskDetail::*` functions moved into the anonymous namespace of `ConfigManager.cpp`. Public seam stays as the existing `ConfigManager::ViewWorkspaceToViewsStore` / `ViewsStoreToViewWorkspace` statics (which forward to the anon-namespace impls). External grep confirms zero external callers had been using the namespace directly — the move is invisible to every other TU.

15. ✅ **DONE.** Investigated and concluded the divergence is intentional: env/CLI overrides are deliberately *not* persisted (they're meant to apply only to the current process), and the migration `Save(cfg)` must run before overrides so the on-disk file gets a clean re-encrypted token without ephemeral override values mixed in. Cache holding override-applied cfg vs disk holding pre-override cfg matches pre-split behavior. Added a block comment at the migration site documenting this ordering invariant so future readers don't try to "fix" it by moving Save below the override block (which would actually break the override semantics).

### From PR #8 (P0 partials + P1/P2 sweep)

16. ✅ **DONE (commit pending in `review/process-stability`).** `MainThreadDispatcher` got `std::atomic<bool> shuttingDown_` + `BeginShutdown()`; `PostToMainThread` checks the atom outside the lock and re-checks under it so late posts no-op cleanly. `~AppController` now calls `mainThreadDispatcher.BeginShutdown()` as the **first** step, before any worker joins, so late posts during shutdown are dropped instead of touching a soon-to-be-destroyed mutex. Queue now actually enforces `kMaxQueueSize = 4096` (was unbounded despite the docstring).

17. ✅ **DONE (commit pending in `review/process-stability`).** `Drain()` iterates destructively: `for (auto& t : tasks) { t(); t = nullptr; }`. Captures (especially `shared_ptr<X>`) are released after each task instead of being kept alive across the whole loop.

18. ⏳ **`GridFrameContext` cache key misses in-place view edits** — `Source_Core/src/SmatchetUI.cpp:500-511`. Invalidation predicate is `(catalogRevision, activeViewId)`. `Views::UpdateActive` mutates the active `ViewDefinition` in place keeping its Id; after the user edits column order/fields/widths, `gridFrameCtx_.columns` is stale until catalog revision bumps or active view changes. Real correctness bug — grid renders the wrong columns for arbitrary frame counts. **Fix:** add a `viewsRevision` counter bumped in `Views::Save()` and include it in the cache key.

19. ✅ **DONE.** The code-level fix shipped in PR #10 (`fix(stability)`): `~AppController` now flips `automationWorkerShuttingDown_` under the job mutex, notifies, and `automationWorker_.join()`s *before* any member destruction begins. That join is the happens-before barrier — per-iteration `bgState` (with its `__smatchet_app = this`) cannot survive into member-dtor land. PR #13 then added the contract docstrings: `automationWorker_` in `AppController.h:679-688` and the `state["__smatchet_app"] = this` comment in `AppController_LuaBindings.cpp:529-538` now spell out the lifetime guarantees.

20. ⏳ **`AuditWriter` silently drops events on disk failure** — `Source_Core/src/BackendAuditTrail.cpp:185-194`. If `ofstream(path, app|binary)` fails (permissions, full disk, AV lock), the popped line is gone with no `LOG_ERROR`, no retry, no metric. Zero observability for audit-data loss. **Fix:** rate-limited error log on first failure; either re-queue with cap or write to a fallback path.

21. ✅ **DONE (commit pending in `review/process-stability`).** `RunLuaSetupScript` now takes `automationJobMutex_` for the `find` + `push_back`. `AutomationWorkerLoop` takes the same mutex briefly to snapshot the vector into a per-job local before iteration (under `try/catch` from #3 above) — workers iterate the snapshot, eliminating the data race on reallocation.

22. ⏳ **`SmatchetImGuiHost::UpdateRendererColorFormat` leaves DX12 backend torn down on init-failure** — `Source_Core/src/SmatchetImGuiHost.cpp:333-336`. `ImGui_ImplDX12_Shutdown()` always runs; `Smatchet_ImplDX12_InitBackend(...)` may fail and return false, leaving `ImplData->Initialized` still true while the backend is uninitialized. Subsequent `DrawUI`/`RenderDrawData` calls into a torn-down backend. **Fix:** on init failure, clear `Initialized` and restore the previous format or set a permanent-error state.

---

## P2 — polish

### From PR #6

23. ⏳ **`PlaneClient` page-cap warning surfaces as a failure banner** — `Source_Core/src/PlaneClient.cpp:489-495`. `summary.FetchError = warn;` then `AppController::TickStreamingApply` reads `FetchError` (`AppController.cpp:2204-2209`) and displays it as a *failure* even though 5,000 issues ingested successfully. **Fix:** add `summary.Warning` channel (or prefix `[partial]`) so the UI can distinguish.

24. 🟡 **PARTIAL (commit pending in `review/logger-hardening`).** `FlushFileSink` no longer lies (item 6 above) so the public surface is honest now. Still has no in-tree caller — a future wire-up into the crash handler / signal path or into `~AppController` (to ensure logs are flushed before SQLite/MCP shutdown) closes this. Not a bug as it stands.

25. ⏳ **`isDeletingStale_` style consistency** — `AppController.h:298`. Reads via implicit `operator bool` while sibling `activeStreamingSync_.Active.load()` uses `.load()` on the same line. Pick one form to avoid the next reader assuming `isDeletingStale_` is still a plain `bool`.

26. ⏳ **`s_lastCallstackIssueKey` missed in `BlameAnalysisUiState` consolidation** — `BlameAnalysisUi.cpp:137`. A single straggler file-static survived the merge into `State()`. **Fix:** move it into the struct.

### From PR #7

27. ⏳ **`ScopedFileLock::Acquire` failure path on Windows is silent** — `ConfigManager.cpp:193-196`. `LockFileEx` failure closes the handle without logging; subsequent IO proceeds without exclusive access. Pre-split bug carried forward. **Fix:** `LOG_WARN("ConfigManager: LockFileEx failed for '%s' err=%lu", lockPath_.c_str(), GetLastError());`. POSIX `flock` path has the same issue.

28. ⏳ **`EnsureDirectoryExists` uses `*A` Win32 APIs while every other Win32 path uses `Utf8ToWide` + `*W`** — `ConfigManager.cpp:86-93`. Non-ASCII paths (UTF-8 user-data dir with é/ñ/CJK) lose characters when the system code page isn't UTF-8. Pre-split bug carried forward. **Fix:** `GetFileAttributesW(Utf8ToWide(path).c_str())` / `CreateDirectoryW(...)`.

29. ⏳ **`kDefaultImGuiDockLayoutIni` declared as `constexpr const char*`** — `ConfigManager.cpp:366`. Pointer is constexpr but pointee storage is regular string-literal storage. **Fix:** `static constexpr char kDefaultImGuiDockLayoutIni[] = …;` — clearer intent, `sizeof` works.

### From PR #8

30. ⏳ **`PluginHost::GetMcpServerStatus` still uses `dynamic_cast<const McpPlugin*>`** — `PluginHost.cpp:84-93`. CODE_REVIEW item 20 added `IPlugin::MatchesConfig` and is marked done, but this status accessor was missed and still reaches into the concrete plugin via RTTI. **Fix:** add `virtual IPlugin::GetMcpStatusSnapshot(McpServerStatus&) {}` or an opaque-blob accessor.

31. ⏳ **`Views::DeleteActive` picks `Views.front()` after delete** — `Views.cpp:77-93`. UX nit: deleting the last view jumps to the first instead of the previous neighbour. **Fix:** capture index pre-erase; pick `min(idx, size-1)`.

32. ⏳ **`LocalCacheManager::stmt()` calls `reset()` + `clearBindings()` even on first creation** — `LocalCacheManager.cpp:117-122`. Harmless but redundant; freshly prepared statements have no bindings. **Fix:** branch on whether `slot` was already populated.

33. ⏳ **`MarkdownConvert` HTML path drops malformed nested tables silently** — `MarkdownConvert.cpp:1490-1499`. `appendPipeTable(tableRows)` fires only on `</table>` close; missing close-tag → rows lost (`fellBack` set, content gone). **Fix:** flush `tableRows` in the `while (outPtrStack.size() > 1)` cleanup at L1599.

34. ⏳ **Setup scripts re-load + re-invoke on every automation job** — `AppController_LuaBindings.cpp:859-869`. PR #8 kept the per-job fresh `sol::state` pattern; setup scripts with side effects (e.g. `tracker.create_issue(...)` at module-load) re-fire every job. **Fix:** cache compiled `sol::function` references and invoke without reloading; or run setup scripts once on the first job.

---

## Summary

**34 new findings** beyond the original 61-item backlog:
- **5 P0** (Logger lifecycle races, missing try/catch in Lua worker, audit-writer race, SQLite statement non-thread-safety).
- **17 P1** (file-sink robustness, MCP shutdown latency, ImGui ID empty-fieldId, BlameUi terminate-at-exit, ConfigManager.h still pulls `json.hpp` unnecessarily, MainThreadDispatcher unbounded + unsynchronized destruction, GridFrameContext stale-on-view-edit, plus more).
- **12 P2** (polish: dead API surfaces, style consistency, UTF-8 path handling, error logging).

The **single most impactful follow-up** is item #13 — switching `ConfigManager.h` to `<nlohmann/json_fwd.hpp>` and moving the `to_json/from_json` definitions out, plus item #14 (move the unused-by-anyone-else `SmatchetViewsDiskDetail` namespace into the cpp anonymous namespace). Together those two changes finally cash in the build-time win that was the entire point of the ConfigManager split.

The next most impactful is the **Logger lifecycle hardening** (items 1, 2, 6, 7, 8) — the async file sink shipped functional but has multiple race conditions that can `std::terminate` or silently lose entries.

The third group is the **PR #8 process-stability set** — `AutomationWorkerLoop` missing try/catch (#3), `BackendAuditTrail` async-writer race (#4), `LocalCacheManager` prepared statements (#5), `MainThreadDispatcher` lifetime (#16), `__smatchet_app` background-state outlive (#19), `ShutdownState` for BlameAnalysisUi worker (#11). Each is a real shutdown- or contention-time crash path.

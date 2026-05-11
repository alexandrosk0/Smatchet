# Post-fix code review — issues introduced by PRs #6 / #7 / #8

> Companion to [`CODE_REVIEW.md`](CODE_REVIEW.md). This document tracks issues found by reviewing the **new code** added by the P0 sweep and follow-up — things the fixes themselves introduced or missed. Nothing here duplicates the 61 numbered items in `CODE_REVIEW.md` §7.
>
> Method: three parallel reviewer agents (one per PR) inspected the substantive new code with full access to the original review, with explicit instructions to look for *new* hazards only. Each finding has file:line citations.
>
> Status legend matches `CODE_REVIEW.md` §7:
> - ⏳ **OPEN** — not yet fixed.
> - ✅ **DONE** — fixed in a later commit (reserved for when this file gets updated).

---

## P0 — bugs introduced by the fixes (do first)

These are real defects in the new code, not just polish. Each can fault at runtime.

1. ⏳ **`Logger::SetFileSinkPath` start-then-restart race can call `std::terminate`** — `Source_Core/src/Logger.cpp:196-216` (added in PR #6 `1c3bebf`). Two threads calling `SetFileSinkPath` concurrently both pass the "same path" early-exit check under separate lock acquisitions, both call `StopFileSinkWorker()`, both assign `m_fileSinkThread = std::thread(...)` — assigning to a still-joinable thread invokes `std::terminate`. `StopFileSinkWorker` also reads `m_fileSinkThread.joinable()` *without* any mutex, so a parallel `SetFileSinkPath` racing with `~Logger` may both observe joinable and both `join()` → UB. **Fix:** introduce a dedicated `m_fileSinkLifecycleMutex` held for the entire stop+restart sequence.

2. ⏳ **`Logger::Log` lock-release window leaks entries when sink stops mid-call** — `Source_Core/src/Logger.cpp:98-127`. Between releasing `m_mutex` (L112) and acquiring `m_fileSinkMutex` (L115), another thread can run `StopFileSinkWorker()`, clear `m_fileSinkPath`, and join the worker. The current call then pushes onto the queue of a no-longer-running worker; the entry is leaked until the next `SetFileSinkPath` (which clears the queue). **Fix:** acquire both mutexes in a fixed global order, or have the worker drain on shutdown.

3. ⏳ **`AutomationWorkerLoop` has no try/catch — any sol2/JSON exception terminates the process** — `Source_Core/src/AppController_LuaBindings.cpp:838-873` (rewritten in PR #8). `InitLuaCore(bgState)`, `bgState.load_file(...)`, `CreateSandboxEnvironment(...)`, and `RunAutomationJob(...)` can each throw (`sol::error`, `std::bad_alloc`, JSON parse). An exception escapes the lambda, unwinds out of the thread function, `std::terminate` runs. **Fix:** wrap the inner job body in `try { ... } catch (const std::exception&) { LOG_ERROR(...) } catch (...) { LOG_ERROR(...) }`.

4. ⏳ **`BackendAuditTrail` async writer races with `ReadRecentEvents` over the same file** — `Source_Core/src/BackendAuditTrail.cpp:172-186, 287-318` (async writer added in PR #8). The writer thread opens `path` with `std::ios::app | std::ios::binary` and does `file << line << '\n';` *without* holding `AuditMutex()`, while `ReadRecentEvents` reads the same path *with* the mutex. A reader can observe a partially-flushed line; the JSON parser's `catch (...)` then silently swallows it and the audit event is lost. **Fix:** writer acquires `AuditMutex()` around each line write (or use a writer-only mutex shared with the reader).

5. ⏳ **`LocalCacheManager` cached prepared statements are not thread-safe** — `Source_Core/include/LocalCacheManager.h:160-167`, `Source_Core/src/LocalCacheManager.cpp:117-203` (added in PR #8). The SQLite connection uses `OPEN_FULLMUTEX` so it's thread-safe, but `SQLite::Statement` instances are **not** (per SQLite docs). `LuaGetTicketGlue` runs on the automation worker thread and calls `Cache->TryGetTicket`; the UI thread also calls `TryGetTicket`. Both threads share the same `stmt_get_exists_` / `stmt_get_fields_` / `stmt_get_rich_` slots → SQLite UB. **Fix:** add a `std::mutex stmtMutex_` to the manager and lock around every `stmt()` call site, or document the class as UI-thread-only and route the Lua glue through a copy.

---

## P1 — significant new code-health concerns

### From PR #6 (the P0 sweep)

6. ⏳ **`Logger::FlushFileSink` lies in its contract — only calls `notify_one()`** — `Source_Core/src/Logger.cpp:218-222`. The header at `Logger.h:69` advertises "called from ~Logger" but `~Logger → StopFileSinkWorker` does the drain itself; callers relying on this method to flush before continuing silently lose data. **Fix:** make it synchronous (capture queue revision, wait on a new `m_drained` condvar) or rename to `RequestFileSinkFlush`.

7. ⏳ **`Logger::FileSinkWorker` doesn't recover from transient disk errors** — `Source_Core/src/Logger.cpp:240-254`. If `out.good()` flips false (disk full, file rotated/deleted externally), the worker `return`s silently and the sink is permanently disabled until `SetFileSinkPath("")` and a fresh `SetFileSinkPath(...)` are issued. **Fix:** on `!good()`, sleep + reopen with bounded retries.

8. ⏳ **`Logger::SetFileSinkPath` TOCTOU between two separate critical sections** — `Source_Core/src/Logger.cpp:197-214`. Lock → compare → unlock → Stop → lock → assign. Between the unlock and re-lock another thread can set the same path, making this call redundantly stop and restart the just-spawned worker (queue contents lost). **Fix:** unified lifecycle mutex held across the whole function.

9. ⏳ **MCP SSE heartbeat blocks process shutdown for up to 1s per connected client** — `Plugins/Mcp/McpPlugin.cpp:621-635`. The lambda does `std::this_thread::sleep_for(1s)` before each heartbeat write. `impl_->svr.stop()` cannot reclaim a worker mid-sleep. With N clients that's up to N seconds of `~McpPlugin` latency. **Fix:** condvar-wait on a shutdown atom with a 1s timeout, or chunk the sleep into ~100ms ticks that re-check shutdown state.

10. ⏳ **`CellIdScope` collides cells when `column.FieldId` is empty** — `Source_Core/src/TicketFieldEditor.cpp:1414`. `ImGui::PushID("")` hashes identically for every empty-string column — synthetic / errored-catalog rows collapse onto one widget ID; edit state and popups leak between rows. **Fix:** `assert(!column.FieldId.empty())` or fall back to `ImGui::PushID((int)columnIndex)` when empty.

11. ⏳ **`BlameAnalysisUiState` Meyers singleton holds a live `std::thread` that can terminate at process exit** — `Source_Core/src/BlameAnalysisUi.cpp:56-68, 132-135` (consolidation from PR #6). `s_state` is destroyed in reverse static-init order. If the callstack worker is still running when destruction fires, `~thread` on a joinable thread invokes `std::terminate`. The old file-static globals had the same hazard but were typically detached/joined explicitly via UI lifecycle hooks. The new singleton has no such hook. **Fix:** add `BlameAnalysisUi::ShutdownState()` invoked from `~AppController` that signals `worker.Cancel` and joins.

12. ⏳ **`PlaneClient::CreateIssue` snapshots `cfg` *before* the cache lock but reads cache state *under* it — inconsistent view** — `Source_Core/src/PlaneClient.cpp:1317-1342`. The lock-scope fix correctly pushes HTTP outside the lock, but `cfg` is captured pre-lock; if another thread mutates credentials between snapshot and HTTP POST, the POST uses old auth while the cache lookup under lock saw fresh state. Comment at L1314 advertises atomicity that isn't actually there. **Fix:** snapshot the entire `TrackerConfig` under the lock, or document the read-old-config-write-with-old-cache contract.

### From PR #7 (ConfigManager split)

13. ⏳ **The slim header can drop `<nlohmann/json.hpp>` entirely — the PR's stated rationale for keeping it is wrong** — `Source_Core/include/ConfigManager.h:16`. The header comment claims "json stays because the struct serializers (CommentTemplate to_json/from_json) need it", but only the *definitions* need the full include — *declarations* are satisfied by `<nlohmann/json_fwd.hpp>`. Moving the `to_json`/`from_json` bodies to `.cpp` would drop the ~30k LOC `json.hpp` parse from every consumer (33 first-party TUs). This was the **stated goal** of the split; the current state recovers ~73% but leaves the biggest win on the table. **Fix:** move `to_json`/`from_json` for `CommentTemplate` into `.cpp`, switch the header to `<nlohmann/json_fwd.hpp>`.

14. ⏳ **`SmatchetViewsDiskDetail::*` inline helpers in the slim header have exactly one caller** — `Source_Core/include/ConfigManager.h:208-350` (~140 LOC of inline JSON parse/serialize). `grep` for `SmatchetViewsDiskDetail::` returns only `ConfigManager.cpp`. Every consumer still re-parses them. **Fix:** move all seven inline functions into the anonymous namespace of `ConfigManager.cpp` — the existing public `ConfigManager::ViewWorkspaceToViewsStore` / `ViewsStoreToViewWorkspace` statics are the proper public seam.

15. ⏳ **Recursive `Save(cfg)` in legacy-MCP migration leaves disk+cache divergent for one `Load()`** — `Source_Core/src/ConfigManager.cpp:1057-1060`. Migration runs `Save(cfg)` *before* env/CLI overrides are applied, then the outer `Load()` applies overrides and caches the override-applied `cfg`. So disk persists no-overrides but cache holds overrides — divergent until the next disk write. Pre-split behavior preserved, but the comment at L1056 doesn't hint at it. **Fix:** move the migration `Save(cfg)` below the override block (after L1095) or explicitly invalidate the cache after the override block.

### From PR #8 (P0 partials + P1/P2 sweep)

16. ⏳ **`MainThreadDispatcher` destruction is unsynchronized + queue is unbounded despite docstring** — `Source_Core/include/MainThreadDispatcher.h:7-39`. The dispatcher is a public member of `AppController`. Detached/background workers post via `app.mainThreadDispatcher.PostToMainThread(...)`. There's no destructor that drains/joins, no shutdown flag, no synchronization with destruction; if `~AppController` runs while a worker is mid-`Post`, the `std::mutex` is destroyed during a `lock_guard` acquire. Also, docstring promises "Bounded" but `queue_.push_back` has no cap. **Fix:** add `std::atomic<bool> shuttingDown_` checked in `PostToMainThread`; drain on dtor OR document a join-before-destroy contract; enforce a max-size with drop policy.

17. ⏳ **`MainThreadDispatcher::Drain()` holds every task alive across the loop** — `Source_Core/include/MainThreadDispatcher.h:24-32`. `for (const auto& t : tasks) t();` keeps every `std::function`'s captures alive for the entire drain (so a task capturing a `shared_ptr<X>` extends `X`'s lifetime across all later tasks). **Fix:** iterate destructively (`for (auto& t : tasks) { t(); t = nullptr; }`) or pop-and-invoke per element.

18. ⏳ **`GridFrameContext` cache key misses in-place view edits** — `Source_Core/src/SmatchetUI.cpp:500-511`. Invalidation predicate is `(catalogRevision, activeViewId)`. `Views::UpdateActive` mutates the active `ViewDefinition` in place keeping its Id; after the user edits column order/fields/widths, `gridFrameCtx_.columns` is stale until catalog revision bumps or active view changes. Real correctness bug — grid renders the wrong columns for arbitrary frame counts. **Fix:** add a `viewsRevision` counter bumped in `Views::Save()` and include it in the cache key.

19. ⏳ **`__smatchet_app` set on every background `bgState` outlives controller during shutdown** — `Source_Core/src/AppController_LuaBindings.cpp:533, 822-824`. `ClearLuaTicketContextGlue` nils the main state's `__smatchet_app` but each `AutomationWorkerLoop` job re-sets `bgState["__smatchet_app"] = this`. If `~AppController` begins while a job is running, the worker's join must strictly precede member destruction. `shuttingDown_.load()` is the only guard with no acquire/release pairing against worker exit. (Pre-PR `gApp` had the same hazard; PR #8's registry capture didn't fix it.) **Fix:** add an explicit join+barrier in `~AppController` before any member destruction; document the contract on `MainThreadDispatcher.h` and the worker.

20. ⏳ **`AuditWriter` silently drops events on disk failure** — `Source_Core/src/BackendAuditTrail.cpp:185-194`. If `ofstream(path, app|binary)` fails (permissions, full disk, AV lock), the popped line is gone with no `LOG_ERROR`, no retry, no metric. Zero observability for audit-data loss. **Fix:** rate-limited error log on first failure; either re-queue with cap or write to a fallback path.

21. ⏳ **`activeSetupScripts_` read from worker thread without synchronization** — `Source_Core/src/AppController_LuaBindings.cpp:859, 1012`. `AutomationWorkerLoop` iterates the member while `RunLuaSetupScript` (UI thread) mutates it via `push_back`. Data race on `std::vector` reallocation. **Fix:** protect with `automationJobMutex_` (already held during job dequeue) or a dedicated mutex.

22. ⏳ **`SmatchetImGuiHost::UpdateRendererColorFormat` leaves DX12 backend torn down on init-failure** — `Source_Core/src/SmatchetImGuiHost.cpp:333-336`. `ImGui_ImplDX12_Shutdown()` always runs; `Smatchet_ImplDX12_InitBackend(...)` may fail and return false, leaving `ImplData->Initialized` still true while the backend is uninitialized. Subsequent `DrawUI`/`RenderDrawData` calls into a torn-down backend. **Fix:** on init failure, clear `Initialized` and restore the previous format or set a permanent-error state.

---

## P2 — polish

### From PR #6

23. ⏳ **`PlaneClient` page-cap warning surfaces as a failure banner** — `Source_Core/src/PlaneClient.cpp:489-495`. `summary.FetchError = warn;` then `AppController::TickStreamingApply` reads `FetchError` (`AppController.cpp:2204-2209`) and displays it as a *failure* even though 5,000 issues ingested successfully. **Fix:** add `summary.Warning` channel (or prefix `[partial]`) so the UI can distinguish.

24. ⏳ **`Logger::FlushFileSink` is dead public API** — `Logger.h:70`. No in-tree caller. Either wire into the crash handler / signal path or drop.

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

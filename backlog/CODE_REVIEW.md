# Smatchet — Senior Code Review (2026-05-10, refresh 2026-05-11)

> Scope: first-party code only (`Source_Core/`, `Plugins/`, `Target_Standalone/`). Dependencies under `.fetchcontent-src/`, `build/`, `out/`, and `.claude/worktrees/` are excluded.
> Method: vexp-routed file-level inspection (skeletons + targeted reads) across ~140 first-party `.cpp`/`.h` files totalling ~48k LOC. This document is updated incrementally as the review progresses.
>
> **Status legend:** P0 = bug / safety / build-break risk · P1 = significant code-health win · P2 = nice-to-have refactor · P3 = note for future
>
> **Companion documents** (added 2026-05-11):
> - [`POST_P0_REVIEW.md`](POST_P0_REVIEW.md) — issues *introduced* by the P0 sweep (PRs #6 / #7 / #8). Tracked separately and now closed; PRs #9–#20 landed the follow-on fixes.
> - [`MANUAL_TEST_QUEUE.md`](MANUAL_TEST_QUEUE.md) — every smoke test deferred from a PR's test plan. Per-item "validation pending" notes in §7 below are superseded by entries M1–M12 / B1–B2 / L1–L3 there.

---

## Table of contents
1. [Architecture & layering](#1-architecture--layering)
2. [Tracker integration (Jira / Plane)](#2-tracker-integration-jira--plane)
3. [UI layer](#3-ui-layer)
4. [Plumbing & infrastructure](#4-plumbing--infrastructure)
5. [Plugins & standalone target](#5-plugins--standalone-target)
6. [Cross-cutting patterns](#6-cross-cutting-patterns)
7. [Prioritized recommendations](#7-prioritized-recommendations)

---

## 1. Architecture & layering

### 1.1 Existing backlog (not duplicated here)

- [`RICH_TEXT_EDITING_V2_REMAINING.md`](RICH_TEXT_EDITING_V2_REMAINING.md) — md4c fidelity, raw-mode UX, server error parity, comment/worklog parity.
- [`RICH_TEXT_EDITING_V3_PLAN.md`](RICH_TEXT_EDITING_V3_PLAN.md) — v3 plan.
- [`CPPCHECK_PLAN.md`](CPPCHECK_PLAN.md) — cppcheck scope and runbook (now superseded for per-edit checks by `.claude/hooks/lint-cpp.sh`).

This review covers ground not in those docs.

### 1.2 First-party scope

- 71 `.cpp` + 68 `.h` in `Source_Core/`, 4 `.cpp/.h` in `Plugins/{Mcp,LuaConsole}/` (excluding vendored `Plugins/LuaConsole/ThirdParty/ImGuiColorTextEdit/`), 1 `main.cpp` in `Target_Standalone/`. ~48k LOC of first-party C++ total.

### 1.3 AppController is a god class (confirmed from the header alone)

`Source_Core/include/AppController.h` is **729 lines** and `AppController.cpp` is **2525 lines**, with auxiliary cuts spread across `AppController_Connectivity.cpp`, `_Attachments.cpp`, `_CatalogAndFieldEdit.cpp` (1216 lines), `_IssueCreateOffline.cpp` (1015 lines), `_LuaBindings.cpp` (1453 lines), `_LuaStubs.cpp`. The public surface mixes **at least 13 distinct concerns**:

| Concern | Surface (examples) |
|---------|--------------------|
| Lifecycle / cache wiring | `Initialize`, `RecreateLocalCacheDatabase` |
| URL / host handlers | `SetOpenUrlHandler`, `SetCloseEmbeddedUiHandler`, `SetRequestAppQuitHandler` |
| Attachment plumbing | `SetAttachmentViewerHandler`, `OpenAttachment`, `DownloadAttachmentForPreview`, `RequestOpenFilePaths` |
| MCP plugin glue | `AppendMcpActivity`, `NotifyMcpClientHttpActivity`, `RuntimePluginHost` |
| Lua automation host | `InitLua`, `LuaLogInfoBind`, ~20 `LuaXxxBind` methods, automation job queue + worker thread |
| Update checker | `CheckForAppUpdate`, `DownloadAndLaunchInstallerUpdate` |
| Tracker fetch / sync | `SyncWithBackend`, `FetchIssuesForActiveView`, `ApplyIssueFetchPack`, streaming sync state |
| Field catalog | `RefreshFieldCatalog`, `SetFieldCatalog` (two overloads), `ResolveDisplayValue` |
| Connectivity probe | `TickTrackerConnectivityMonitor`, `GetLastTrackerConnectivityState`, recovery latches |
| Issue create (sync + offline + dead-letter) | `CreateIssueAsync`, `QueueCreateOffline`, `TickOfflineCreates`, dead-letter restore/delete (3 summary structs) |
| Issue field edits (network + offline + dead-letter) | `SubmitFieldEdit`, `SubmitFieldEditNetworkOnly`, `QueueFieldEditOffline`, `TickOfflineFieldEdits`, `ResolveFieldEditConflict`, more summaries |
| Comments / worklog / users / votes / watchers | `AddIssueCommentPlain`, `SubmitWorklog`, `FetchIssueWatchers`, `FetchIssueVotes`, `SearchUsersByQuery` |
| Edit-meta cache | `EnsureIssueEditMetaLoaded`, `RefreshIssueEditMeta`, `WarmIssueEditMetaAsync`, two caches (by-issue and by-issue-type) |

**Private state count** in the header: ~40 fields, including 6 mutexes, 6 atomics, 3 thread/future members, 5+ `std::function` handler slots, multiple condvar-protected job queues. The streaming-sync state machine (`StreamingSyncState`, `_LH685-705`) lives inline.

#### Concrete issues (header-only, lines refer to `Source_Core/include/AppController.h`)

- [P1] **Cross-concern struct definitions in `AppController.h`** — `TrackerConnectivityBannerForUi` (L40), `TrackerIssueFetchPack` (L47), `AppUpdateAsset`/`AppUpdateInfo` (L53-68), `IssueEditMetaCache` (L577), `StreamingSyncState` (L685), several `*Summary` structs (L409, L418, L425, L453, L459) — each one is a separate domain DTO and forces every TU that uses `AppController.h` to recompile when any of them changes. Move each to its own small header in `Source_Core/include/` (or to the relevant `.h` for the subsystem it belongs to).
- [P1] **`#include <sol/sol.hpp>` in a header used app-wide** — L11. Sol2 is the single heaviest header in this project (per `SmatchetPch.h` notes). Anything that includes `AppController.h` (and lots of things do, transitively) pulls in ~1 MB of templated code. Forward-declare `class AppController` where possible; for the public surface, prefer wrapping Lua-specific entry points in a separate header (`AppController_Lua.h`) that only the bindings/stubs TUs include.
- [P1] **`#include <nlohmann/json.hpp>` in `AppController.h`** — L35. Same story. Used in a single signature (`TryBuildFieldEditPayloadForNetwork` param) and the `McpToolDefinition::parametersSchema` member. Both can move to a private impl header.
- [P1] **Public `std::vector<std::function<void(const std::string&)>> AutomationLogSinks;`-style observer lists with no unregister handle** — `AddAutomationLogSink` + `ClearAutomationLogSinks` (L94-96). Easy to dangle. Switch to a token-returning `Add…` that lets callers `Remove(token)`.
- [P2] **Mixed naming conventions** — public fields like `ActiveTickets` (PascalCase, L540) live next to `activeTicketsPublished_` (camelCase + trailing underscore, L541). Pick one for private state and stick with it.
- [P2] **Two near-identical edit-meta caches keyed by `std::string`** (L584-585: `issueEditMeta_` by issue id, `issueTypeEditMeta_` by issue type) — same struct, same locking strategy. Promote to `IssueEditMetaCacheStore` parameterised on a key type, share the prune/refresh code.
- [P2] **`kMcpActivityLogMax = 100` hardcoded constexpr** (L654) — should be a configurable, or pulled from `ConfigManager`. Also: bounded ring of 100 strings means a busy server loses lines silently.
- [P2] **Public method ordering is ad hoc** — Lua bindings (L222-254) sit between attachment plumbing and field-catalog APIs. Group by concern with banner comments at minimum; ideally split.
- [P3] **`bool ApplyFieldEditResult(...)` returns a bool and sets `outError`** — common pattern across the file; consider a single `Result<T>` helper (`struct Result { bool ok; std::string error; }`) to avoid 30+ `bool foo(..., std::string& outError)` signatures.

### 1.4 High-level design proposal — extract three services from AppController

**Goal:** reduce `AppController` to a thin orchestrator and make each concern testable in isolation. Each step below is independently shippable.

1. **`OfflineQueueService`** — owns `pending_creates` + `pending_creates_dead` + `pending_field_edits` SQLite tables and the `Tick*` replay loops. Currently spread across `AppController_IssueCreateOffline.cpp` (1015 LOC) and the field-edit equivalents in `AppController.cpp`. Risk: low; touch points are narrow. Win: an entire 1015-line file moves out of the controller.
2. **`TrackerConnectivityMonitor`** — owns `nextTrackerConnectivityProbeAt_`, `trackerConnectivityProbeFuture_`, the recovery latches, and `MapReachabilityProbeKind`. Today these live in `AppController_Connectivity.cpp` but use `AppController`-private fields. Make it own its own state and emit recovery events.
3. **`LuaAutomationHost`** — moves the entire `#if defined(SMATCHET_WITH_LUA_AUTOMATION)` block (`lua` state, all `LuaXxxBind` methods, `automationJobMutex_`/`automationWorker_`/job queue, field-display/icon-map registries). `AppController_LuaBindings.cpp` becomes the host's impl. Risk: bindings reference back into `AppController` heavily — needs a small dependency-injection pass (host receives a `TrackerActions` interface that `AppController` implements).

After all three: `AppController` is ~600 lines and primarily orchestrates ticket fetch + field catalog + edit dispatch.

### 1.5 Top-level layout / naming smells

- [P2] **`SmatchetImGuiHostC.h` C-ABI wrapper next to the C++ `SmatchetImGuiHost.h`** — fine, but the diagnostics impl `SmatchetImGuiHostC_Diagnostics.cpp` is excluded from `CORE_SOURCES` glob in `CMakeLists.txt:459-460` and only used in the DX12 lib. The carveout is fragile; a sibling subdir `Source_Core/src/host/` makes the relationship obvious.
- [P2] **Mixed file-naming conventions in `Source_Core/`** — `SmatchetXxxUi.cpp` (UIs), `TrackerXxxYyy.cpp` (tracker utilities), `JiraXxx.cpp` (Jira-specific), `IssueXxx.cpp` (cross-cutting), `AppController_Xxx.cpp` (controller cuts). The mental model is clear, but the prefixes mix domain (`Tracker`, `Jira`, `Plane`, `Smatchet`, `App`) with no firm rule. Document the convention in `CLAUDE.md` or rename towards a single prefix-by-subsystem scheme over time.
- [P3] **`Views.h`, `SpreadsheetState.h`, `NewIssueInheritDefaults.h`, `SmatchetUiSession.h`** are headers with no matching `.cpp` — fine if they're pure types, but verify they don't carry inline definitions that change frequently and force PCH invalidations.

### 1.6 PCH (`SmatchetPch.h`) review

- Looks deliberate and well-commented (the file even narrates which headers are deliberately *out* of the PCH).
- [P3] **Window-only PCH** — `Source_Core/include/SmatchetPch.h:35` wraps `windows.h` in `#if defined(_WIN32)`. The PCH applies to `SmatchetStandalone` only (per CMake), so the guard is defensive but correct. Keep.
- [P3] **`<future>` not in PCH** — `AppController.h` and several others use `std::future`/`std::async` heavily. Adding `<future>` to the PCH would amortise its parse cost across the ~10+ TUs that use it. Measure before adding (heavy on libstdc++).

### 1.7 AppController.cpp — concrete code-level issues (deep dive)

Scope: `Source_Core/src/AppController.cpp` (2525 LOC) + the six `AppController_*.cpp` cuts (~7700 LOC total). All file paths below are under `Source_Core/`.

#### Concurrency (P0 — actively dangerous)

- [P0] **`activeStreamingSync_.RequestId` is a plain `uint64_t` read from the worker thread** — `include/AppController.h:686`, accessed at `src/AppController.cpp:2443, 2453, 2463, 2487, 2499, 2511`. Promote to `std::atomic<uint64_t>`.
- [P0] **`activeStreamingSync_.FetchError` and `FullSyncCompleted` are written by the worker, read on UI thread without synchronisation** — `src/AppController.cpp:2195-2197` reads while worker writes at lines 2465-2515. Either guard with `QueueMutex` or move into the queue payload.
- [P0] **`isDeletingStale_` and `staleIdsToDelete_` are bare bool/vector read via the `IsStreamingSyncActive()` const accessor from any thread** — `include/AppController.h:719-721` + `298`. Promote to `std::atomic<bool>` or document UI-thread-only.
- [P0] **`gApp` static lifetime hazard in Lua bindings** — `src/AppController_LuaBindings.cpp:309`. Set in `InitLuaCore`, cleared in `~AppController` via `ClearLuaTicketContextGlue`; if `automationWorker_` runs a job after `shuttingDown_` is set but before the join completes, `gApp->...` calls in the Ticket usertype glue may dereference a half-destructed controller. Lua-retained `Ticket` userdata can also outlive the controller. Fix: pass `AppController*` through sol2 upvalues / environment instead of a file-static.

#### Concurrency (P1 — latent / hard to trigger)

- [P1] **`AvailableFields` mutated on UI thread, iterated by background workers** — `src/AppController_CatalogAndFieldEdit.cpp:218-238` rewrites the vector while `WarmIssueTypeEditMetaAtStartAsync` workers indirectly read it via `FindFieldById`/`ResolveIssueTypeKeyForIssue`. Either copy-on-write under a mutex or document "no workers in flight during catalog mutation".
- [P1] **Detached `backgroundWorkers_` threads can outlive the controller** — `src/AppController.cpp:618-622`. If a worker calls back into `JoinBackgroundTasks`, the self-id branch detaches; `~AppController` then exits without joining, so the detached thread can outlive the process-teardown of static `g_TrackerIssueFetchMutex` (line 296). Replace with a bounded worker pool or tighten shutdown so the path is unreachable.
- [P1] **`activeStreamingSync_.Cancelled = false` reset races with stale worker reads** — `src/AppController.cpp:1665-1666, 1901-1903`. Atomic already, but the reset happens *after* `WorkerThread.join()` in some paths and *before* in others — audit each reset site for happens-after-join.
- [P1] **`automationWorker_` recreates a `sol::state` per job** — `src/AppController_LuaBindings.cpp:822-841`. Re-opens libs and re-runs every setup script on each job — both slow and risks double registrations. Cache one background `sol::state` reused for jobs of the same script.
- [P1] **`CancelAndJoinActiveStreamingSync` join races with `Cache.reset()`** — `src/AppController.cpp:1821-1845, 1668`. Confirm the join completes before any `Cache.reset()` in `~AppController` / `RecreateLocalCacheDatabase`. Add an assertion.

#### Coupling / abstraction

- [P1] **Hard-coded backend instantiation** — `src/AppController.cpp:1181, 1187, 2371, 2377` allocate `JiraClient`/`PlaneClient` directly. Introduce `ITrackerBackendFactory`; the Unreal host can swap in a different transport.
- [P1] **`AppController_LuaBindings.cpp:309` `gApp` file-static** breaks any test/multi-instance scenario in addition to the lifetime issue above.
- [P1] **AppController calls `cpr::Get` directly for app updates and attachments** — `src/AppController.cpp:979-1117`. Bypasses `ITrackerClient`; extract to `IAppUpdateClient` for testability + Unreal-host swap.
- [P1] **`SmatchetToastManager::Instance()` called from `TickStreamingApply` / `StartStreamingSync`** — hard UI dependency in the controller. Inject an `IUserNotifier` interface.
- [P1] **`ConfigManager::Load()` synchronously called inside hot field-edit paths** — `src/AppController_CatalogAndFieldEdit.cpp:137, 608, 867, 1071, 1090, 1108, 1122, 1131, 1146, 1155, 1172, 1181, 1201` — **~13 JSON file reads + parses per field-edit submit**. Snapshot once at entry to each public method. (Ties into the ConfigManager.h findings in §4.1.)

#### State management

- [P1] **Two sources of truth for active tickets** — `include/AppController.h:540` `ActiveTickets` vs `:541` `activeTicketsPublished_` shared_ptr. Every mutation must update both under `activeTicketsMutex_`. The invariant "snapshot reflects ActiveTickets" lives nowhere; wrap in an `ActiveTicketStore` class that enforces it.
- [P1] **`MarkdownConvert` + conflict-resolution payload composition embedded in the controller** — `src/AppController_IssueCreateOffline.cpp:412-459`, including a stringified `"__resolved__"` sentinel at line 424. Extract to `FieldEditConflictResolver`.
- [P1] **`Initialize` is doing 3 synchronous things on the UI thread** — `src/AppController.cpp:1121-1339`: opens SQLite, runs legacy-drop migration, parses catalog snapshot, instantiates backend, triggers another `ConfigManager::Load()` for editmeta warmup. Cache the `TrackerConfig` for the duration of `Initialize` and pass by `const&`.

#### Smaller / mechanical

- [P2] **Raw `std::unique_ptr<T>(new T(...))`** — `src/AppController.cpp:1127, 1181, 1187, 1673, 1683`. Project rule (`.claude/CLAUDE.md`) says use `std::make_unique`.
- [P2] **Two near-identical retry-after-HTTP-400 blocks** — `src/AppController_CatalogAndFieldEdit.cpp:790-808` vs `962-975`. Extract a `SubmitWithEditMetaRetry(issueId, field, payload, ...)` helper.
- [P2] **`MakeUniqueTempFilePath` uses `chrono::time_since_epoch().count()` alone** — `src/AppController_Attachments.cpp:248`. Two attachments downloaded in the same nanosecond from different threads collide. Mix in `std::this_thread::get_id()` hash or a `std::atomic<uint64_t>` counter.
- [P2] **Duplicate revision accessors** — `include/AppController.h:302-303` `GetTrackerFieldCatalogRevision()` and `GetFieldCatalogRevision()` return the same atomic. Drop one alias.
- [P2] **Hand-rolled lowercase loops** — `src/AppController.cpp:300-344, 1173-1175, 2355-2357` use `std::transform … ::tolower`. Use the existing `ToLowerAsciiCopy` from `StringUtil.h` consistently.
- [P2] **`std::to_string(double)` emits `42.000000`** — `src/AppController_LuaBindings.cpp:170-175`. Use an explicit formatter.
- [P2] **`ListLuaScriptFiles` substring-ext check** — `src/AppController.cpp:1409-1432` rejects 4-char `.lua` cases by size check. Use `ghc::filesystem::path::extension()`.

#### Design proposals (in priority order)

1. **`TicketSyncService`** — extract `StreamingSyncState`, `TickStreamingApply`, `SyncWithBackend`, `StartStreamingSync`, `CancelAndJoinActiveStreamingSync`, `ApplyIssueFetchPack`, stale-deletion state (~700 LOC). Owns the worker thread + batch queue; publishes via `ITicketSyncListener { OnBatchApplied, OnSyncFinished, OnStaleDeleted }`. **Rationale:** addresses every P0 race above; the state machine is self-contained. **Risk:** must preserve supersede + frame-budget semantics — add unit tests around `Superseded`/`Cancelled` transitions before extracting.
2. **`OfflineQueueService`** — extract create + field-edit replay, dead-letter, 3-way merge, `ResolveFieldEditConflict` (~900 LOC). Takes `ITrackerClient&`, `LocalCacheManager&`, `IFieldCatalogProvider&`. **Rationale:** zero conceptual dependency on the rest of the controller; highly testable. **Risk:** moderate — needs `IAuditSink`. `AppController` keeps thin facades calling into the service so existing call sites compile.
3. **`ITrackerBackendFactory`** so `Initialize`/`StartStreamingSync` stop hard-coding backend constructors. Built in `main()` / Unreal host plugin. **Rationale:** adding Plane required edits to both call sites; a third backend would too. **Risk:** very low.
4. **`LuaAutomationHost`** — move all `Lua*Bind` methods, automation worker, sandbox env, sol2 registries, `gApp` removal into a dedicated host owned by AppController but with no public surface. `AppController.h` stops including `<sol/sol.hpp>`. **Rationale:** addresses the P0 `gApp` hazard, P1 sol2 state caching, and removes the heaviest single include from the public surface. **Risk:** medium — touches every Lua binding and the `LuaConsole` plugin.

## 2. Tracker integration (Jira / Plane)

### 2.1 Shared HTTP client design

- **`TrackerHttpUtils` exists but used inconsistently.** `JiraClient` routes everything through it (URL encode, Base64, normalize, logged HTTP verbs, transport classifier). `PlaneClient` bypasses it for headers — `BuildPlaneHeaders` returns a `std::unordered_map` which is hand-converted to `cpr::Header` at every call site (`PlaneClient.cpp:411-414, 760-763, 800-803, 1108-1111`).
- **`ITrackerClient` has become a kitchen-sink of Jira operations.** 18+ virtuals, half of them with default "unsupported" returns: `AddIssueCommentBlameContext`, `AddWorklog`, `FetchUserGroupNames`, `FetchIssueVotes` are Jira-specific. Plane re-implements roughly half; the rest return errors at runtime.
- **Probe behaviour diverges.** `JiraClient::ProbeReachability` (`JiraClient.cpp:8-56`) maps 4xx-not-401/403 → `ReachableAuthOrConfigError`. `PlaneClient::ProbeReachability` (`PlaneClient.cpp:750-782`) classifies anything other than 200/401/403/5xx as `TransportDown` — a 404 from a stale base URL is wrongly reported as offline.
- **Probe bypasses logging.** `PlaneClient.cpp:765` uses raw `cpr::Get`, skipping `TrackerGetLogged` so the call isn't routed through `NetworkUsageTracker`.
- **No retry policy.** Single fixed timeout (`kTrackerOverallTimeoutMs = 30000`), no retry on 429/5xx/transport, no backoff. `IsTrackerTransportErrorText` exists for classification but never drives retries.
- **Auth secrets not zeroized.** `cfg.ApiToken`/`PlaneApiKey` lives in `std::string` from `ConfigManager::Load()` with no special handling after use.

#### Concrete issues

- [P0] **`PlaneClient` holds `planeCacheMutex_` across the entire HTTP round trip** in `UpdateIssueFields` (`PlaneClient.cpp:1104-1154`) and `CreateIssue` (`1288-1350`), blocking every UI thread call to `ResolveDisplayValue` for tens of seconds. Capture needed cache fields under lock, drop lock before HTTP.
- [P0] **`PlaneClient` outer pagination loop has no page cap** — `PlaneClient.cpp:478` is a `while(true)` trusting `next_page_results=false` to terminate. A misbehaving cursor (loop back) would spin forever. Add a max-page guard like Jira's 50.
- [P0] **`is_required` parse is a no-op** — `PlaneClient.cpp:298-300` reads `if (... is_required ...) { (void)0; }`. Required-field state is silently lost when ingesting Plane property definitions.
- [P1] **`ConfigManager::Load()` called per-mutation** — `PlaneClient.cpp:1106, 1290`; `JiraIssueMutation.cpp:60, 481, 556`. Each issue update hits disk to re-read prefs. Should be cached on the client.
- [P1] **`PlaneClient::FetchIssuesForKeys` is O(N×total).** `PlaneClient.cpp:1411-1430` fetches every issue in the project then filters in memory. Use the Plane filter API or batch by `sequence_id`.
- [P1] **JQL keys passed unquoted in `key in (...)`** — `JiraIssueSearch.cpp:470-477`. Values containing dashes or starting with reserved words trip the parser. Quote via a `JqlQuotedValue` helper.
- [P1] **`PlaneClient::BuildBrowseUrl` may double-slash** — `PlaneClient.cpp:1098-1102`. If `cfg.PlaneWorkspaceSlug` starts with `/`, the URL becomes `//`. `NormalizePlaneWebBase` trims the base trailing `/` but not the slug.
- [P1] **`AddIssueToSprint` payload may send issue *key* where Jira Agile docs require ID** — `JiraIssueMutation.cpp:671`. Add explicit ID resolution.
- [P2] **`PlaneClient::FetchIssueEditMeta` hardcodes 7 fields** — `PlaneClient.cpp:1087-1096`. Custom properties never get edit-meta. Either return all catalog fields or delegate to real Plane permissions.
- [P2] **`PlaneClient::BuildCreatePayload`/`BuildUpdatePayload` ignore custom fields** — `PlaneClient.cpp:1366-1409`. Only handles 6 core ids; any value from a `TrackerField` whose Id is a UUID (custom property) is dropped silently.
- [P2] **`PlaneClient` `assignee` "null intent" ambiguity** — `PlaneClient.cpp:1182`. A single empty string becomes `""` (Plane rejects). Treat empty first value as `nullptr`.
- [P2] **Status transition match falls back to transition name** — `JiraIssueMutation.cpp:189`. Workflows with transition names different from status names can pick wrong transition. At minimum log when fallback triggers.
- [P2] **`StripUtf8BomCopy` duplicated** — `PlaneClient.cpp:20`. Consolidate into `StringUtil.h`.
- [P2] **No request-id / correlation-id** — neither client emits a Smatchet-generated request ID header for log correlation with tracker support.

### 2.2 ADF / markdown round-trip (`MarkdownConvert`)

- **Attachment-prefix handling is correctly localized.** Markdown→ADF at `MarkdownConvert.cpp:312-324`, ADF→Markdown at `:797-813`. Bidirectional, symmetric, no other site in `Source_Core` handles `attachment:<UUID>`.
- **Soft break = hard break** at `MarkdownConvert.cpp:371-378`. Intentional per the comment, but breaks round-trip: wrapped source lines become multi-line paragraphs in ADF and re-export as `  \n` soft breaks → re-import as hard breaks. Document the loss or add a fidelity flag.
- **`MarkdownCellPlainInner`** (`:870-882`) flattens table cells to plain text. Loses all bold/italic/links inside table cells on ADF→Markdown. Mention in `RICH_TEXT_EDITING_V2_REMAINING.md`.
- **Unknown ADF nodes silently dropped.** `panel`, `expand`, `taskList` (real ADF), `decisionList`, `extension`, `bodiedExtension`, `status`, `date` are all dropped. The `dropped` vector exists (`:1088`) but isn't routed to a UI warning for these specific node types.
- **`prev["marks"] == b.markStack`** at `MarkdownConvert.cpp:100` walks element-wise per text emit — hot path in large docs.
- **`thread_local std::vector<std::string> sLinkHrefs`** in `HtmlToMarkdown` (`:1452, 1595`) survives across calls; mid-parse exception leaks state. Replace with a parser-state member.

#### Concrete issues

- [P1] **`thread_local` href stack can leak on exception** — `MarkdownConvert.cpp:1452, 1595`.
- [P1] **Table-cell rich content lost on ADF→Markdown** — `MarkdownConvert.cpp:870-882`. Tracked partly in v2 backlog; promote to its own line item.
- [P2] **HTML allowlist soft-allows `<span>` but pushes a stack frame** — `MarkdownConvert.cpp:1122, 1600-1602`. Push has no functional effect on output but obscures intent.
- [P2] **`DecodeHtmlEntities` is O(N × entity-list-len)** — `MarkdownConvert.cpp:1137-1213`. Build a static `unordered_map` once.
- [P2] **`EmitInlineText` rebuilds `openWrap`/`closeWrap` vectors per node** — `MarkdownConvert.cpp:~750+`. Reuse a scratch buffer.

### 2.3 Tracker design proposals

1. **`TrackerHttpClient` shared base.** Owns auth header, error parser, retry policy, `TrackerError` value type (transport/auth/4xx/5xx/parse). ~12 sites currently re-implement `if (status != 2xx) { try parse detail catch ... }`. Hook retry on 429/5xx/transport using `IsTrackerTransportErrorText`. **Risk:** medium; mechanically refactorable.
2. **Split `ITrackerClient` into role interfaces** — `ITrackerSearch`, `ITrackerMutation`, `ITrackerSchema`, `ITrackerUserDirectory`, `ITrackerWorkflow` (Jira-only). Removes the `unsupported` default-impl pattern. **Risk:** low; stage with `dynamic_cast` at call sites.
3. **Table-driven `MarkdownConvert` dispatch.** Replace 4 parallel switches (enter/leave × block/span) and `EmitAdfBlock` with a `static const struct { const char* type; EmitFn; }` registry. Centralizes fidelity and makes dropped-node logging exhaustive. **Risk:** medium; needs golden tests (already on the v2 backlog) before extraction.
4. **Catalog-cache aware Plane client.** Lift `cachedStates_`/`cachedCycles_`/`cachedUsers_`/`cachedLabels_`/`keyToId_` into `FieldCatalogCache`. Removes the "needs to be in cache from a recent fetch" failure mode (`PlaneClient.cpp:1126-1131`). **Risk:** medium; schema-version bump but migrator template already in `FieldCatalogCache.cpp:298-309`.

## 3. UI layer

### 3.1 Per-frame allocation hot spots

The grid + field editors leak `std::string` allocations per cell per frame at a rate that becomes visible at zoom-out (~200 visible rows × 30 columns):

- [P0] **Per-cell widget ID built by string concat** — `TicketFieldEditor.cpp:1092` (`"##TextCell_" + ticket.id + "_" + field.Id`), `:1247` (multi-select), `:1283` (per-option label), `:1334` (cascade). Add `:650` in `SmatchetActiveProjectGridUi.cpp`. Replace with `ImGui::PushID(ticket.id.c_str())` + `ImGui::PushID(field.Id.c_str())` and literal `"##cell"`.
- [P0] **`TrackerFieldCatalogIndex` rebuilt every frame** — both `SmatchetActiveProjectGridUi.cpp:148` and `SmatchetUI.cpp:680` (menu bar) populate the index per draw. Cache against `app.GetFieldCatalogRevision()`.
- [P0] **`TicketGridColumnsBuilder::Build` called twice per frame** — `SmatchetUI.cpp:682` + `SmatchetActiveProjectGridUi.cpp:150`. Same revision-keyed cache.
- [P1] **Status color row lookup per frame** — `SmatchetActiveProjectGridUi.cpp:582-596` calls `ToLowerAsciiCopy(statusRaw)` + 4 `string::find` per row. Precompute `unordered_map<string, ImVec4>` once per catalog revision.
- [P1] **`Selectable` label = `display + itemId` string** — `TicketFieldEditor.cpp:1162` per cell per frame. Use `PushID` + `TextUnformatted` separately.
- [P1] **`BuildCellKey` hash+alloc per cell per frame** — `SmatchetActiveProjectGridUi.cpp:650`. Switch the feedback map to a `std::pair<string, string>` key or carry a precomputed `uint64_t` hash on `TicketGridColumn`.
- [P1] **`activeView->ColumnWidths.find` per column per frame** — `SmatchetActiveProjectGridUi.cpp:208-211`. Materialize widths into `columns[]` once when columns are built.

### 3.2 `SmatchetLocalizedImGui` wrapper

- `#define ImGui SmatchetLocalizedImGui` (e.g. `SmatchetUI.cpp:24`) substitutes the namespace; not a literal macro replacement of `ImGui::Foo`, but a namespace alias on call sites. Works, but fragile if any code uses qualified `::ImGui::Bar(...)` after the `#define`.
- Wrapper functions call `WindowTitleFromSource(name)` / `LabelFromSource(label)` on every call (hash lookups per widget per frame, hundreds of widgets/frame). Caching on **label pointer** (literals share addresses) would hit ~100%.
- [P2] Localization wrapper's per-call lookup is real but easy to fix with a pointer-keyed lookup cache.

### 3.3 Model / view separation

- [P0] **`BlameAnalysisUi.cpp` is essentially all TU-globals** — `:70-135` declares 30+ file-scoped variables: `g_callstackBuf`, `g_worker`, `g_displayRows`, `g_detailFuts`, `g_blameCfg`, `g_atClBuf`, `g_p4Exe`, etc. The class is a 3-line facade. Worker, results, P4 cache live as TU-locals. Impossible to embed (Unreal hot-reload) or test. Wrap in a `BlameAnalysisModel` member.
- [P1] **`TicketFieldEditor.cpp` modal singletons** — `s_ActiveWorklogState` (`:109`), `s_ActiveLongTextState` (`:167`). For a modal there's only one, so singletons are OK, but lifecycle is hand-rolled (`JustOpened`/`Initialized`/`Active`) and recovery on dismiss is incomplete (`:1988` "shouldn't normally happen"). Lift into `UiDrawSession` with an `IModalEditor` interface.
- [P1] **`SmatchetUI::drawEnsureCatalogAndInitialSync` does data fetching inside UI tree** — `SmatchetUI.cpp:594-673` calls `std::async`, `ConfigManager::Save`, `app.SetFieldCatalog` from within `Draw`. Move into `AppController::TickFieldCatalogSync()`.
- [P1] **`BlameAnalysisUi.cpp:261-298`** does a linear scan of `app.GetActiveTicketsSnapshot()` for each callstack fill. Promote to `AppController::FindTicketById(const std::string&)` (O(1) via the cache map already used for sorts).
- [P2] **`SmatchetPreferencesUi.cpp:84+` three `static bool s_*Loaded` flags inside `drawPreferencesWindow`** for sub-tab buffer init. Move into `UiDrawSession`.
- [P2] **`SmatchetUI.cpp:42` declares `UiDrawSession g_ui;` as a TU global.** Convenient but implicit. Make it a `SmatchetUI` member.

### 3.4 Other UI smells

- [P1] **Markdown preview re-runs round-trip on every keystroke** — `TicketFieldEditor.cpp:1842-1877` calls `MarkdownToAdf` → `AdfToMarkdown` whenever `md != LastRoundTripInput`. For 64 KB buffers this is non-trivial. Debounce by ~100 ms.
- [P1] **Hidden singleton `searchBuf` shared across multi-select editors** — `TicketFieldEditor.cpp:1251-1252`. Two simultaneously-open combos alias the buffer. Move into `SpreadsheetState`.
- [P2] **`ImGui::SetWindowFontScale(1.0f)` hardcoded reset** — `TicketFieldEditor.cpp:538`. Save/restore previous value.
- [P2] **`std::set<int> baseBefore = sel.Rows` per drag frame** — `SmatchetActiveProjectGridUi.cpp:478`. Drag updates allocate a fresh set every mouse move; swap-and-restore on release instead.
- [P2] **`std::stable_sort` lambda resolves `TrackerFieldCatalogIndex::Find` per compare** — `SmatchetActiveProjectGridUi.cpp:400`. Resolve once per column outside the comparator.
- [P2] **Manual `PushClipRect` per cell** — `SmatchetActiveProjectGridUi.cpp:619, 633, 679, 690, 702, 712`. ImGui table already clips columns. Profile and remove if redundant.

### 3.5 UI design proposals

1. **`GridFrameContext`** — built once at the top of `drawActiveProjectWindow`, holds `TrackerFieldCatalogIndex`, columns, precomputed status colors, column-width array, and the `revision` it was built from. Reused across menu bar, grid, and side panels. Eliminates the 2-3 catalog + column rebuilds per frame and the per-row string allocs. Add a `SMATCHET_DEBUG` revalidation assertion to guard stale columns.
2. **Extract `BlameAnalysisModel`** owning worker, display rows, detail futures, P4 cache, profile state. `BlameAnalysisUi` holds a `unique_ptr<BlameAnalysisModel>`. Unblocks Unreal embedding (TU globals don't survive plugin reloads) and enables unit tests. Stage: (a) struct-of-globals, (b) move to class member, (c) split file.
3. **`UiDrawSession` modal interface** — `IModalEditor { Tick, Render, Drain }` for worklog, long-text, future modals. `DrainUiDrawSessionFuturesBeforeAppTeardown` iterates generically.
4. **`WidgetIdGuard` RAII** — `WidgetIdGuard{ticket.id, field.Id};` pushes 2 IDs, pops in dtor. Removes the single largest per-frame allocation source. One-time audit to make sure the guard wraps every cell.

## 4. Plumbing & infrastructure

### 4.1 `ConfigManager.h` — header-only anti-pattern (1696 LOC)

`Source_Core/include/ConfigManager.h` is **a 1696-line header-only file**. The whole `ConfigManager` class is composed of `static` functions defined inline:

- File I/O (`LoadJsonFile`, `WriteConfigJson`, `AtomicWriteTextFile`) including Win32 `CreateFileW`/`ReadFile`/`WriteFile` impls and POSIX `open`/`write` fallbacks.
- All `Save(const TrackerConfig&)`, `Load()`, merge logic, validation, schema versioning.
- A nested `ScopedFileLock` class with `LockFileEx` (Windows) and `flock` (POSIX) implementations inline.
- Meyers singletons (`GetIoMutexRef`, `GetCacheMutexRef`, `GetCachedConfigRef`, `GetHasCachedConfigRef`, `GetRuntimeAssetDirectoryRef`, `GetUserDataDirectoryRef`) — these create static state at first include.

The header also includes (lines 4-38):

```cpp
#include <nlohmann/json.hpp>        // ~30k LOC of templated code
#include <windows.h>                // already in PCH, OK
#include <wincrypt.h>               // NOT in PCH — every consumer paying for it
#include <fstream>, <sstream>, ...
```

#### Concrete issues

- [P0] **Every TU that includes `ConfigManager.h` recompiles ~1700 lines of inline I/O / locking / JSON merge logic.** Includes the entire `nlohmann/json.hpp` (~30k LOC) and `wincrypt.h`. This is likely the single biggest avoidable contributor to compile time in the codebase. Splitting into:
  - `ConfigTypes.h` (POD structs: `TrackerConfig`, `ViewSortSpec`, `ViewDefinition`, `ViewsStore`, `CommentTemplate`, `BlameAnalysisConfig`, …) — no `<json>`, no `windows.h`.
  - `ConfigManager.h` (class declaration only, returns `TrackerConfig` by value).
  - `ConfigManager.cpp` (all I/O, locking, JSON merging).
  
  Most consumers only need `TrackerConfig` definitions, not the I/O. Expected build-time win: 30-60s on a clean rebuild (typical for header-only JSON in a codebase this size).
- [P1] **Meyers singletons for IO mutex + cached config** — `ConfigManager.h:1501-1519`. Process-wide state hidden inside a header makes testing impossible (every test sees the same mutex/cache). Move singletons to `ConfigManager.cpp` as `namespace { ... }` statics, or better: make `ConfigManager` a real instance class with its own state.
- [P1] **`SetBaseDirectoryForFiles(...)`** (L434) is labelled "Legacy compatibility" but still exposed. Either delete or mark `[[deprecated]]` (well — C++14, so `__attribute__((deprecated))`/`_declspec(deprecated)`) so newer code doesn't pick it up.
- [P1] **No `Load()` overload that takes a `TrackerConfig&` by reference** — every `ConfigManager::Load()` returns by value, copying a 50+-field struct including vectors. UI thread polls config in many tick paths; profile this. If it's hot, an internal cache (with revision counter) + reference-return API is much cheaper.
- [P1] **Win32-only fast-path read** (L482-513) for `LoadJsonFile` exists "to avoid MinGW/libstdc++ ifstream issues seen in release at startup". The comment is honest but the workaround belongs in a CPP, not a public header that every TU recompiles.
- [P2] **`CreateDirectories(...)` manual path-walker** (L1601) duplicates `std::filesystem::create_directories`. Project already links `ghc::filesystem` (the C++14 polyfill). Use `ghc::filesystem::create_directories(p, ec)`.
- [P2] **`FileExists` reimplemented** (L1587) — same: `ghc::filesystem::exists`.
- [P2] **Static `ScopedFileLock` class lives at the bottom of the same header** (L1638). After moving to .cpp, it can become a `namespace detail` private type.
- [P2] **`64 * 1024 * 1024` magic number** at L491 (max config file size). Pull into a `kMaxConfigFileBytes` constant near the top of the impl.
- [P3] **`try { json::parse(...) } catch (...) { ... }`** at L526-534 — fine, but the `catch (...)` branch logs identically to the `catch (const std::exception&)` branch minus `.what()`. Probably dead.

#### Design proposal — 3-step extraction

| Step | Effort | Risk | Win |
|------|--------|------|-----|
| 1. Move struct definitions into `Source_Core/include/SmatchetConfigTypes.h` (or per-subsystem headers). `ConfigManager.h` becomes a forward-declaration of `class ConfigManager` + 5-10 static method signatures. | Half-day | Low (mechanical) | Most consumers stop pulling json.hpp transitively. |
| 2. Move all method bodies into `Source_Core/src/ConfigManager.cpp`. The current PCH (`SmatchetPch.h`) already covers `<windows.h>` and STL; new `.cpp` will compile once. | Half-day | Low | -1500 LOC re-parsed per TU. |
| 3. Replace `static` singletons with an instance, owned by `AppController` (or a `SmatchetApp` root). Take it as a constructor arg in subsystems that need it (or pass `const TrackerConfig&` by reference where readonly suffices). | 1-2 days | Medium (touches many call sites) | Testable, no hidden globals. |

### 4.2 Logger

API is clean (`LOG_TRACE/DEBUG/INFO/WARN/ERROR` macros, Meyers singleton, atomic-gated `ShouldLog`). The double-check pattern in `Log()` (re-test min level after lock, `Logger.cpp:87`) is correct for `SetMinLevel` races. Issues:

- [P0] **`m_entries.erase(begin())` is O(N)** — `Logger.cpp:91-93`. Every log call beyond 1000 entries copies 999 entries. Replace `std::vector<LogEntry>` with `std::deque` or a fixed ring buffer indexed mod `kMaxEntries`.
- [P0] **No file/console sink — entries die with the process.** If the app crashes before the UI Log window is opened, all logs are lost. Add an optional async file sink (`smatchet_runtime.log`) gated on a config flag, flushed on Error/Warn.
- [P1] **`Logf` uses a fixed 4096-byte stack buffer** — `Logger.cpp:111-122`. For HTTP-body trace logs this is a hard ceiling. Two-pass `vsnprintf` (size first, then allocate) or fall back to `std::vector` on truncation.
- [P1] **`GetEntriesSnapshot()` copies the entire vector under mutex** — `Logger.cpp:125-128`. The UI Log window calls this every frame; up to 1000 entries copied. Use a revision token + delta to avoid the copy.
- [P2] **`std::atomic<int> m_minLevelInt` instead of `std::atomic<LogLevel>`** — `Logger.h:65`. C++14-legal for trivially copyable enums and clearer at call sites.
- [P3] **No source location / thread id / category fields.** Acceptable for a small app but limits debug.

### 4.3 LocalCacheManager

- SQLite WAL + 5s busy-timeout + `synchronous=NORMAL` (`LocalCacheManager.cpp:31-33`) is the right pairing.
- Schema migration via `PRAGMA table_info` + `ALTER TABLE ADD COLUMN` (`:68, 85-92, 106-110`) is robust against duplicate ADDs.
- [P1] **No prepared-statement cache** — every `SaveTicket`/`TryGetTicket` rebuilds `SQLite::Statement` (`:117-164`). Hot path for bulk saves. Cache statements as members; reset on use.
- [P1] **`GetAllTickets` does two full table scans and an in-memory join** — `:216-265`. OK for a desktop client but materializes the entire result set. Add a streaming iterator overload.
- [P2] **`PRAGMA journal_mode=WAL` re-issued on every open** — `:31`. Cheap but check `journal_mode()` first to silence log noise.

### 4.4 BackendAuditTrail

- JSONL append-only design is appropriate for an audit log.
- `ReadRecentEvents` (`:245`) caches by `Path` and seeks forward — good incremental design.
- [P1] **Writes are synchronous on the caller thread under a global mutex** — `BackendAuditTrail.cpp:226-231`. Slow disk → tracker mutation latency. Add an async writer queue (bounded; drop-oldest on overflow).
- [P2] **`LooksSensitiveKey` blocklist is broad** — `:53-63`. `summary`/`assignee`/`body`/`text` are all redacted. Confirm intentional and document. As-is, audit dumps lose useful payload diffs.

### 4.5 NetworkUsageTracker

- Tiny, atomic-only, lock-free. Good.
- [P2] **`Record` doesn't surface HTTP status / error-rate.** Add a counter for non-2xx responses.

### 4.6 PluginHost

- Clean `IPlugin` virtual interface with 4 lifecycle hooks (`EarlyInit/Start/Draw/Stop`), vtable-stable for future DLL loading.
- Error isolation: every `OnXxx` wraps `try/catch (std::exception&)` + `catch(...)` and logs (`PluginHost.cpp:23-30`).
- Plugin ownership via `std::unique_ptr` — correct.
- [P1] **`SyncMcpPluginWithConfig` uses `dynamic_cast<const McpPlugin*>` and pokes plugin internals** — `PluginHost.cpp:105-113`. Couples Host to MCP. Replace with a virtual `bool MatchesConfig(const TrackerConfig&) const` on `IPlugin` (default returns true); host stops/starts plugins that don't match.
- [P1] **No `Unregister(const char* id)`** — `SyncMcpPluginWithConfig` re-implements remove-by-erase. Add the symmetric API.

### 4.7 SmatchetLocalization

- [P2] **`thread_local std::vector<std::string>` ring of 64** — `SmatchetLocalization.cpp:446-451`. If more than 64 distinct labels are formatted in a frame, the first ones get overwritten while still in ImGui's draw queue. Increase budget or document.

### 4.8 Other plumbing

- [P2] **`SmatchetImageTextureCache` O(N) LRU touch** — `SmatchetImageTextureCache.cpp:93-99` uses `std::list::find`. For 96 entries it's fine; store `list::iterator` in the map value for O(1) splice if cache grows.
- [P2] **`Views.h` is header-only with non-trivial logic** — `:9-174`. Same disease as `ConfigManager.h` at smaller scale. Move bodies to `.cpp`.
- [P2] **`NavigationHistory::Push` has no max-history cap** — `NavigationHistory.cpp:19-22`. Long sessions grow unbounded.
- [P3] **`SmatchetPch.h` excludes `nlohmann/json.hpp` deliberately.** The comment explains it's because non-json TUs paid the parse cost. Keep — well-engineered. If `ConfigManager.h` is split (§4.1) re-measure: removing the bulk include from many transitive consumers may make json in PCH a net win again.

### 4.9 Design proposals

1. **Split `ConfigManager` into `.h` + `.cpp` + `ISettingsRepo`** (already detailed in §4.1).
2. **Logger ring + async file sink** — `boost::circular_buffer`-style ring + `LogSink` interface (default `MemoryRingSink` for UI; optional `FileSink` and `OStreamSink`). API stays `LOG_*`. Cuts O(N) on overflow and persists diagnostics across crashes. **Risk:** low.
3. **Promote `ScopedFileLock` + `AtomicWriteTextFile` into a shared `FileIo` helper.** Used by `ConfigManager`, `BackendAuditTrail` (currently raw `ofstream`), and export paths. **Risk:** low.
4. **`IPluginConfigContract`** — replace `PluginHost::SyncMcpPluginWithConfig` dynamic_cast with `virtual bool MatchesConfig(const TrackerConfig&) const` on `IPlugin`. Future plugins get restart-on-change for free. **Risk:** low.

## 5. Plugins & standalone target

### 5.1 MCP plugin (`Plugins/Mcp/McpPlugin.cpp`)

Architecture: `cpp-httplib::Server` on a single `std::thread` (`McpPlugin.cpp:840`). Two surfaces:

- **Legacy REST** — `/mcp/list_tickets`, `/mcp/search`, `/mcp/tools/list`, `/mcp/tools/call`, `/mcp/attachment_proxy`.
- **JSON-RPC** at `/mcp/sse` + `/mcp/messages` (`:606, 831-832`).
- No stdio transport.

Auth: constant-time token compare (`:122-131`). Loopback-only when no token (`:341`). `IsAllowedAttachmentHost` allowlists the tracker domain + `api.media.atlassian.com` (`:145`).

#### Concrete issues

- [P0] **SSE keep-alive sleeps inside the chunked provider** — `McpPlugin.cpp:627`. `std::this_thread::sleep_for(15s)` blocks an httplib worker thread; an aborted client only frees the worker after the next 15s tick. Shorten the interval, or push heartbeats from a dedicated thread via `set_chunked_content_provider` with cooperative cancellation.
- [P1] **Threading: handler reads `impl_->{tracker_domain, auth_token, allow_lua_execution, export_fields}` on httplib worker threads without sync** — `:314-326`. After bind these are read-only; a future config-reload path would race. Snapshot at the start of each handler or guard with a `shared_mutex`.
- [P1] **Two hand-built `tools/list` payloads** — `:506` (REST) and `:666` (JSON-RPC). Schema duplication invites drift. Build once at `OnStart` from `LuaMcpTools` + built-ins.
- [P1] **No bounds on `/mcp/search` result set** — `:466-504`. Body cap self-limits but a wide query can hit it. Add a `limit` query param.
- [P2] **Empty-token branch leaks "no token configured"** — `:340`. Acceptable but document; the 403 path runs *before* any compare so a remote prober learns auth posture.
- [P2] **Attachment proxy treats 3xx silently as failure** — `:425, 446`. `cpr::Redirect(false,false)` is correct, but Unreal would benefit from an explicit "upstream redirected" 502 with a hint.

### 5.2 Lua plugin (`Plugins/LuaConsole/LuaConsolePlugin.cpp` + `AppController_LuaBindings.cpp`)

- sol2 metatable patch contract (per `CMakeLists.txt:355-411`) is consistently honoured: every Lua binding goes through a free function with a unique symbol in `smatchet_lua_init_detail::` (`AppController_LuaBindings.cpp:306, 311-446`). No raw lambda `set_function`.
- Sandbox nils `dofile`/`loadfile`/`load`/`loadstring`/`require`/`collectgarbage`/`os`/`io`/`package`/`debug` (`:287-300`).
- `lua_sethook` instruction counter exists as a cancel point (`:847`) but **no instruction-count cap** — DoS via `string.rep`/`string.format` is mitigated only by the user-supplied cancel callback firing in time.

#### Concrete issues

- [P0] **Worker-thread `gApp` reassignment** — `AppController_LuaBindings.cpp:502` runs inside `InitLuaCore` which is called from background automation jobs (`:823`). Reassigns process-global `gApp` from a worker thread while main-thread Lua callbacks may be active. Fix: only the main `InitLua` should set `gApp`; background `sol::state` should embed `this` via upvalue or `sol::state::set("__app_ptr", this)`.
- [P1] **Background `sol::state` re-runs `InitLuaCore`** — same patched-metatable keys get registered on a second `lua_State`. Validate sol2's per-T uniqueness still holds when the same `CachedTicket` usertype is registered in two states; otherwise the second `new_usertype<CachedTicket>` reuses the first state's `__gc` (heap corruption — same class of bug the patch was meant to fix).
- [P1] **`LuaConsolePlugin::OnDraw` uses file-static stateful locals** — `LuaConsolePlugin.cpp:352-358`: `s_tabSel`, `s_pendingSelectScriptsTab`. Two plugin instances (Unreal hot-reload) corrupt each other's UI state. Move into member fields.
- [P2] **`WriteFileAll` doesn't check stream error state** — `LuaConsolePlugin.cpp:105-113`. `o << content` not checked; partial writes on disk-full silently succeed. Add `if (!o.good()) { ... }`.
- [P2] **`SaveLuaLayoutDebounced` has a duplicated guard** — `LuaConsolePlugin.cpp:28-34`. Second conditional never executes.
- [P2] **`TryParseLuaErrorLine` uses unbounded `regex_search`** — `LuaConsolePlugin.cpp:151`. The `{0,400}` cap helps but backtracking on malformed input can stall the UI thread. Replace with a hand parser.
- [P2] **MCP run_lua schema duplicated** — `McpPlugin.cpp:514-523` + `:685-694`. Extract `BuildRunLuaToolDef()`.

### 5.3 ImGui host bridge (`SmatchetImGuiHost.cpp` + `SmatchetImGuiHostC.h`)

- C-ABI: handle is `void*`. `gLiveHostHandles` set (`SmatchetImGuiHost.cpp:794`) gives one line of defense against double-free / stale handle but **only `Destroy` consults it** — every `SmatchetHost_*` accessor blindly reinterpret_casts the handle.
- Frame lifecycle: `BeginFrame` → `DrawUI` → `RenderDrawData`, gated by `Initialized`/`FrameActive`/`BuildingFrame`/`RenderingDraw` atomics + a single `ImGuiMutex`. ImGui context is re-set on every entry (`:572, 605, 634`) for the foreign-thread case.
- DX12 SRV allocator: slot 0 reserved for font atlas, slot N-1 as overflow target — exhausted dynamic textures alias each other but never the font (`:118-132`). Solid.
- OpenGL backend lives entirely in `Target_Standalone/main.cpp`; the host is DX12-only (`SmatchetImGuiHost.cpp:372`).

#### Concrete issues

- [P1] **C-ABI handle validation is one-way** — `SmatchetImGuiHost.cpp:794, 814-822`. Use-after-free across Unreal hot-reload boundaries dereferences invalid memory. Add a `LookupHost(handle)` helper that lock-checks `gLiveHostHandles` for every accessor.
- [P1] **`SmatchetCheckAndApplyFontReload` invalidates DX device objects under the ImGui mutex while a render may be in flight** — `:576-579`. `RenderingDraw` atomic prevents concurrent `Render`, but if Unreal RHI is mid-draw on the queue, `InvalidateDeviceObjects` releases SRVs the command list still references. Fence the queue or defer the reload to a frame boundary.

### 5.4 `Target_Standalone/main.cpp`

- Bootstrap order: `glfwInit` → window → context → ImGui `CreateContext` → fonts → backend init → load config → `AppController` + `PluginHost` → `OnEarlyInit` → `AppController.Initialize` → `OnStart` → loop (`:206-440`).
- Error paths: try/catch around the whole core; `ImGui_ImplOpenGL3_Shutdown` runs even on early-init failure (`:450`). If `ImGui::CreateContext` itself throws, the destroy path is entered with no context — unlikely but check.
- [P2] **`glfwSwapInterval(1)` set before context fully validated** — `:242`.

### 5.5 Design proposals

1. **`McpToolRegistry`** — single registry populated at `OnStart` (built-ins + `app.GetLuaMcpTools()`); both REST and JSON-RPC transports read from it. Eliminates schema drift between two hand-built `tools/list` payloads. **Risk:** minor; preserve insertion order so existing clients see the same tool list.
2. **Replace `gApp` singleton with `sol::state` registry capture.** Store `AppController*` in `state["__smatchet_app"]`; glue functions resolve via `sol::state_view(L)["__smatchet_app"].get<AppController*>()`. Eliminates the worker-thread reassignment race (§5.2 P0). **Risk:** each glue gains one Lua table lookup; benchmark, but registry access is constant-time. Do during the next sol2 bump.
3. **Gate every C-ABI entry point through a live-handle macro.** `if (!IsLive(host)) return default;` wrapped at the top of each `SmatchetHost_*`. Mitigate the mutex cost with a `std::atomic<uint64_t> generation` token embedded in the handle so the lookup is lock-free. **Risk:** low; localized change in `SmatchetImGuiHost.cpp`.

## 6. Cross-cutting patterns

### 6.1 Concurrency model is ad-hoc

The codebase mixes at least 6 concurrency primitives across files:

- `std::mutex`/`shared_ptr` for the published ticket snapshot.
- `std::atomic<bool>` + plain `bool` flags side-by-side (some atomics, some not — see §1.7 P0 races).
- `std::condition_variable` + job queue (`automationWorker_`).
- `std::future<T>` for fire-and-forget probes and bulk creates.
- `std::thread` joined manually (`activeStreamingSync_.WorkerThread`, `backgroundWorkers_`).
- `httplib::Server` worker threadpool (MCP) with its own internal sync.

**Issues:**
- No single "Owns Thread X" rule — workers post back to `AppController` directly, which then dispatches across other mutexes.
- Multiple "deferred-notify-on-UI-thread" boolean atoms (`fieldCatalogRefetchAfterLiveTicketSyncPending_`, `deferredLiveTrackerBackendSuccessNotify_`, `trackerConnectivityRecoveryPending_`, etc.) — each implementing the same one-shot pattern by hand.

**Proposal [P1]:** Introduce a `MainThreadDispatcher` (`std::function` queue drained once per frame in `SmatchetUI::Draw` head). Replace the deferred-notify atoms with `dispatcher.PostToMainThread([]{...})`. Removes ~8 ad-hoc flags and centralises the UI-thread contract.

### 6.2 Error handling — `bool foo(..., std::string& outError)` everywhere

The dominant signature across `AppController.h`, `JiraClient.h`, `PlaneClient.h`, `ITrackerClient.h`, `LocalCacheManager.h`, `IssueCreatePipeline.h`, `TextMerge.h` is a `bool` return paired with a `std::string& outError` out-param. There are ~80+ such signatures.

- **Pro:** uniform; works in C++14 without exceptions; allows partial-success patterns (some funcs also fill out-data even on `false`).
- **Con:** caller boilerplate (`std::string err; if (!foo(..., err)) { LOG_ERROR("%s", err.c_str()); return; }`) is everywhere; easy to forget `outError` initialisation; the "what kind of error" (transport / auth / not-found / parse / cancelled) is encoded as a freeform string and inspected with `IsTrackerTransportErrorText`-style heuristics elsewhere.

**Proposal [P1]:** Add a small `TrackerError` value type (kind enum + detail string + optional HTTP status). Roll out incrementally: new APIs use `TrackerError`, old ones keep `bool + string` until migrated. Pair with a one-line `LOG_AND_RETURN_ON_ERR(call)` macro for caller-side ergonomics.

### 6.3 `ConfigManager::Load()` re-parses JSON in hot paths

Counted **~30+ call sites** across:
- `AppController_CatalogAndFieldEdit.cpp:137, 608, 867, 1071, 1090, 1108, 1122, 1131, 1146, 1155, 1172, 1181, 1201` (~13 per field-edit submit)
- `JiraIssueMutation.cpp:60, 481, 556` and `PlaneClient.cpp:1106, 1290` (per mutation)
- Many UI tick paths

This is a real performance regression source. **Proposal [P0]:** combine with the §4.1 split — cache the parsed `TrackerConfig` with a revision counter, return `const TrackerConfig&`.

### 6.4 Header-include hygiene

Already partially noted:
- `AppController.h` pulls `sol/sol.hpp` + `nlohmann/json.hpp` PUBLIC (§1.3).
- `ConfigManager.h` pulls `windows.h` + `wincrypt.h` + `nlohmann/json.hpp` PUBLIC (§4.1).
- `Views.h` is header-only with non-trivial logic (§4.8).

**Pattern:** the project has invested in PCH discipline (`SmatchetPch.h` comments narrate exclusions deliberately) but the public-header surface undoes much of the win. The fix is mechanical and across the board: structs in slim headers, impl in `.cpp`.

### 6.5 RAII / `make_unique` discipline

Already in `CLAUDE.md` as a rule. Audit found:
- [P2] `AppController.cpp:1127, 1181, 1187, 1673, 1683` use `unique_ptr<T>(new T(...))`. Switch to `make_unique`. (One-line fix per site.)

No raw `new`/`delete` found in scanned files outside of `IM_NEW(ImTextureData)()` (which is the documented ImGui pattern paired with `IM_DELETE`).

### 6.6 Naming / convention

- Public field PascalCase (`ActiveTickets`) vs private camelCase + trailing underscore (`activeTicketsPublished_`) in the same class. Pick one for private state.
- File prefixes mix domain (`Tracker*`, `Jira*`, `Plane*`, `Issue*`, `App*`, `Smatchet*Ui*`). Document the convention in `CLAUDE.md` and let renames trickle in over time.
- Many functions named `XxxYyyZzz` with no implied verb (`ResolveIssueTypeKeyForIssue`, `CanEditFieldForIssue`). Acceptable for current size.

### 6.7 Testing

- No `tests/` directory exists in the first-party tree.
- The Rich-Text v2 backlog has been asking for `MarkdownToAdf` / `AdfToMarkdown` golden tests for months (`RICH_TEXT_EDITING_V2_REMAINING.md` §"Testing and quality").
- Streaming sync FSM (P0 races) is fundamentally untestable today.
- **Proposal [P1]:** add a minimal `tests/` target — gtest or doctest — and seed it with `MarkdownConvert` golden tests + a `OfflineQueueService` test once that service is extracted (§1.7 proposal 2).

### 6.8 Build / tooling notes

- `compile_commands.json` is generated by both `ninja-iter-msys2` and `ninja-debug-msys2` presets (per `CMakePresets.json:60-75`). The lint hook (`.claude/hooks/lint-cpp.sh`) pins to `ninja-iter-msys2/`; both-target syntax check via the `.py` helper.
- `CPPCHECK_PLAN.md` is now partially obsolete (replaced by the PostToolUse hook for per-edit checks). The full-codebase cppcheck runbook is still useful for periodic sweeps.
- `Source_Core/src/SmatchetImGuiHostC_Diagnostics.cpp` is excluded from `GLOB_RECURSE CORE_SOURCES` in `CMakeLists.txt:459-460`. The carveout is fragile; a `Source_Core/src/host/` subdir or an explicit `EXCLUDE_FROM_ALL` list at the top of CMakeLists is more robust.

## 7. Prioritized recommendations

> **Status legend** (added 2026-05-10):
> - ✅ **DONE** — landed on `develop`; needs manual smoke-test validation.
> - 🟡 **PARTIAL** — first step shipped; follow-up tracked.
> - ⏳ **OPEN** — not yet started.
>
> Marking an item ✅ means the *code* matches the proposal. Behavioral validation moved (2026-05-11) to a single home: [`MANUAL_TEST_QUEUE.md`](MANUAL_TEST_QUEUE.md). The per-item "Validation pending" footnotes below are kept for historical context but should be considered superseded by the queue.

### Follow-on hardening landed since 2026-05-10

PRs #9–#22 closed every regression / new-code defect found while reviewing PRs #6 / #7 / #8 — tracked in [`POST_P0_REVIEW.md`](POST_P0_REVIEW.md). Develop tip at refresh time: `0a79de5` (Merge PR #20).

| PR | Title | What it closed |
|----|-------|----------------|
| [#9](https://github.com/alexandrosk0/Smatchet/pull/9) | `fix(logger): harden async file-sink lifecycle` | POST_P0 items 1, 2, 6, 7, 8 + 24 partial — hardens this doc's items 6 / 7 / 17. |
| [#10](https://github.com/alexandrosk0/Smatchet/pull/10) | `fix(stability): close 7 shutdown / contention crash paths` | POST_P0 items 3, 4, 5, 11, 16, 17, 21 — hardens this doc's items 8, 9, 18, 19, and the BlameAnalysisUi step-b move. |
| [#11](https://github.com/alexandrosk0/Smatchet/pull/11) | `refactor(config): finish the split — drop json.hpp from public header` | POST_P0 items 13, 14 — completes this doc's item 3. |
| [#12](https://github.com/alexandrosk0/Smatchet/pull/12) | `review: P1 cleanup — 4 fixes from POST_P0_REVIEW` | POST_P0 items 9, 10, 18, 22 + Impl-member-order hardening for MCP shutdown. |
| [#13](https://github.com/alexandrosk0/Smatchet/pull/13) | `docs(controller): clarify automation-worker lifetime contract` | POST_P0 item 19 (docs-only). |
| [#14](https://github.com/alexandrosk0/Smatchet/pull/14) | `docs(config): explain legacy-MCP migration ordering` | POST_P0 item 15 (comment-only). |
| [#15](https://github.com/alexandrosk0/Smatchet/pull/15) | `fix(plane): snapshot TrackerConfig under cache lock in CreateIssue` | POST_P0 item 12 — extends this doc's item 4 (Plane lock-scope fix). |
| [#16](https://github.com/alexandrosk0/Smatchet/pull/16) | `fix(audit): fallback path when primary audit file is unwritable` | POST_P0 item 20 — extends this doc's item 18 (AuditWriter). |
| [#17](https://github.com/alexandrosk0/Smatchet/pull/17) | `polish: P2 batch — items 25/26/27/28/29/31/32` | POST_P0 P2 cleanup batch 1. |
| [#19](https://github.com/alexandrosk0/Smatchet/pull/19) | `docs(review): refresh POST_P0_REVIEW.md after PRs #12-#16` | docs-only. |
| [#20](https://github.com/alexandrosk0/Smatchet/pull/20) | `polish: P2 batch 2 — items 23/30/33/34` | POST_P0 items 23, 30, 33, 34 — extends this doc's item 20 (`PluginHost::GetMcpServerStatus` `dynamic_cast` removed via `TryGetMcpStatusSnapshot`); item 23 (PlaneClient page-cap mis-classified as fetch failure) splits a `Warning` channel out of `FetchError`. |
| [#21](https://github.com/alexandrosk0/Smatchet/pull/21) | `chore(cppcheck): clear pre-existing baseline noise` | Issue #18 (cppcheck) closed — 4 sites cleared on McpPlugin / SmatchetActiveProjectGridUi / ConfigManager / PlaneClient. |
| [#22](https://github.com/alexandrosk0/Smatchet/pull/22) | `docs(backlog): add MANUAL_TEST_QUEUE.md` | Added the smoke-test queue (B1–B2, M1–M12, L1–L3). |

The 2026-05-10 numbered items below are unchanged in intent. Where one of those items received material follow-on hardening from a PR above, the row carries a `[hardened by #N]` annotation in the per-item commentary.

### P0 — safety / build / tooling — do first

1. ✅ **DONE (PR [#6](https://github.com/alexandrosk0/Smatchet/pull/6), commit `1226a04`).** Fix streaming-sync data races in `AppController` — `RequestId` and `isDeletingStale_` promoted to `std::atomic`; `FetchError` accesses serialized under `QueueMutex` (all 7 read/write sites verified). `FullSyncCompleted` was already atomic and stays so (§1.7).
   - *Validation pending:* run a long-running sync + cancel-and-restart cycle, watch for sentinel "Sync failed with…" messages appearing under contention.
2. ✅ **DONE (PR [#6](https://github.com/alexandrosk0/Smatchet/pull/6), commit `572ddcf` + branch `claude/reverent-curran-109a96`).** `gApp` process-static fully removed. `InitLuaCore` now writes `state["__smatchet_app"] = this` for every state (main and background) — each Lua state holds its own `AppController*` with no cross-state races. All 21 glue functions (`TicketSetFieldGlue`, `TicketTransitionGlue`, `LuaLogInfoGlue`, `LuaGetTicketGlue`, `LuaGetActiveTicketsGlue`, `LuaDecodeJsonGlue`, all Register/Unregister, ImGui, Ui, Mcp, CreateIssue, TrackerCreateIssue) updated to receive `sol::this_state L` and resolve `AppController*` via `ResolveApp(L)`. `lua_sethook` shutdown check resolves `app` from registry. `ClearLuaTicketContextGlue` writes `lua["__smatchet_app"] = sol::lua_nil` instead of clearing the static. Both targets build clean.
   - *Validation pending:* run a Lua automation script that calls `Ticket:set_field` and `Ticket:transition`; confirm both succeed and logs show correct audit source.
3. ✅ **DONE (PR [#7](https://github.com/alexandrosk0/Smatchet/pull/7), commit `faeb801`; finished by PR [#11](https://github.com/alexandrosk0/Smatchet/pull/11)).** `ConfigManager.h` split: 1696 LOC header-only → 461 LOC slim header + 1333 LOC new `ConfigManager.cpp`. Removed from public surface: `<windows.h>`, `<wincrypt.h>`, `<fstream>`, `<sstream>`, `<sys/file.h>`, `<unistd.h>`, `<mutex>`, `<chrono>`, `"Logger.h"`, `"NewIssueInheritDefaults.h"`. **`[hardened by #11]`** PR #11 finishes the split by also dropping `<nlohmann/json.hpp>` from the public header (moves the `CommentTemplate` JSON serializer definitions into the .cpp). Every consumer now stops paying the json.hpp parse cost. **Note:** the related `TrackerConfig` caching to eliminate ~30 redundant disk reads per field-edit submit is still open (§4.1, §6.3).
   - *Validation:* see [`MANUAL_TEST_QUEUE.md`](MANUAL_TEST_QUEUE.md) — DPAPI / legacy plaintext migration smoke is implicit in M5 / L1.
4. ✅ **DONE (PR [#6](https://github.com/alexandrosk0/Smatchet/pull/6), commit `a06fbdd`; extended by PR [#15](https://github.com/alexandrosk0/Smatchet/pull/15)).** `PlaneClient::UpdateIssueFields` and `CreateIssue` snapshot project + UUID under `planeCacheMutex_`, drop the lock, then issue the HTTP PATCH/POST. `CreateIssue` re-acquires the lock briefly to record `visualKey → uuid` in `keyToId_` (§2.1). **`[hardened by #15]`** PR #15 also moves the `TrackerConfig` snapshot (and the `BuildPlaneHeaders` call) into the critical section, since a concurrent `ConfigManager::Save()` between the cfg snapshot and the cache lookup could rotate credentials between request-header construction and cache read — the POST would go out with stale auth while the cache lookup saw fresh state.
   - *Validation:* see [`MANUAL_TEST_QUEUE.md`](MANUAL_TEST_QUEUE.md) M5.
5. ✅ **DONE (PR [#6](https://github.com/alexandrosk0/Smatchet/pull/6), commit `a06fbdd`).** `PlaneClient::FetchIssuesStreamed` outer pagination loop capped at `kMaxPlanePages = 50` (5,000 work-items). On cap-hit, `summary.FetchError` carries a clear "narrow your view" warning (§2.1).
6. ✅ **DONE (PR [#6](https://github.com/alexandrosk0/Smatchet/pull/6), commit `1c3bebf`; hardened by PR [#9](https://github.com/alexandrosk0/Smatchet/pull/9)).** `Logger::m_entries` is now `std::deque<LogEntry>` (O(1) overflow); `Logf` uses a 2-pass `vsnprintf` so HTTP body traces no longer truncate at 4 KB; optional async file sink with bounded queue + drop-oldest. **`[hardened by #9]`** PR #9 hardens the file-sink lifecycle: `SetFileSinkPath` is now atomic under `m_fileSinkLifecycleMutex` (no TOCTOU on concurrent path changes); `Log()` re-checks the path under `m_fileSinkMutex` before pushing (no benign push onto a dead-consumer queue); `FlushFileSink()` is synchronous via a generation counter (no longer lies); `FileSinkWorker` retries on `!good()` after 5 failures rather than permanently abandoning the sink. API wired in but `SetFileSinkPath` is not yet called from `ConfigManager` — that final hookup is a follow-up (§4.2).
   - *Validation:* see [`MANUAL_TEST_QUEUE.md`](MANUAL_TEST_QUEUE.md) L2 (file-sink path rotation).
7. ✅ **DONE (PR [#6](https://github.com/alexandrosk0/Smatchet/pull/6), commit `98502cb`; hardened by PR [#12](https://github.com/alexandrosk0/Smatchet/pull/12)).** MCP SSE heartbeat now fires every 1 s (was a single 15 s sleep). cpp-httplib v0.14 has no `is_writable()`, so disconnect is detected via `sink.write()`'s `bool` return — the worker thread frees within ~1 s of client disconnect (§5.1). **`[hardened by #12]`** PR #12 replaces the 1 s sleep with a condvar wait on a per-Impl shutdown atom. `OnStop()` notifies before `svr.stop()` so workers return in microseconds rather than up to 1 s per connected client. A subsequent fix-up commit on the same PR (`6053c09`) reorders `Impl`'s members so the shutdown primitives (`shutdownMutex` / `shutdownCv` / `shuttingDown`) are declared **before** `httplib::Server svr` — guarantees they outlive `~Server`'s worker-pool join even if a chunked-content lambda is mid-flight (§5.1).
   - *Validation:* see [`MANUAL_TEST_QUEUE.md`](MANUAL_TEST_QUEUE.md) M1.
8. ✅ **DONE (PR [#6](https://github.com/alexandrosk0/Smatchet/pull/6), commit `1911e78` + branch `claude/reverent-curran-109a96`; hardened by PR [#12](https://github.com/alexandrosk0/Smatchet/pull/12)).** `CellIdScope` RAII pushes `ticket.id` + `column.FieldId` (commit `1911e78`). `GridFrameContext` introduced in `SmatchetUI.h`; `SmatchetUI::Draw` builds it once per frame keyed on `GetFieldCatalogRevision()` + active-view id. Both `drawMainMenuBar` and `drawActiveProjectWindow` now bind `const TrackerFieldCatalogIndex& catalogIndex = *gridFrameCtx_.catalogIndex` and `const std::vector<TicketGridColumn>& columns = gridFrameCtx_.columns` — eliminating the two per-frame `TrackerFieldCatalogIndex` constructions and two `TicketGridColumnsBuilder::Build` calls. Both targets build clean. **`[hardened by #12]`** PR #12 fixes two follow-on defects: (a) `CellIdScope` now pushes the column index when `FieldId` is empty (otherwise `PushID("")` collapsed every empty-field column in a row onto a single ImGui id); (b) `Views` gains an atomic `Revision_` bumped on every mutation, included in `GridFrameContext`'s cache key — in-place view edits (column widths, sort persistence) now invalidate within one frame instead of waiting for a catalog-rev bump.
   - *Validation:* see [`MANUAL_TEST_QUEUE.md`](MANUAL_TEST_QUEUE.md) M2 (CellIdScope) and M3 (GridFrameContext invalidation).
9. 🟡 **PARTIAL (PR [#6](https://github.com/alexandrosk0/Smatchet/pull/6), commit `1bee50f` + branch `claude/reverent-curran-109a96`; hardened by PR [#10](https://github.com/alexandrosk0/Smatchet/pull/10)).** All ~40 file-static globals in `BlameAnalysisUi.cpp` are now members of `BlameAnalysisUiState` accessed via a `State()` Meyers singleton (step a). Step (b) is now done: `BlameAnalysisUiState` renamed `BlameAnalysisUi::BlameState`, lifted out of the anonymous namespace, owned by a `std::unique_ptr<BlameState> state_` pimpl member; `State()` indirects through a module-level pointer set in the constructor/destructor; `s_lastCallstackIssueKey` and `s_blameCfgDiskHydrated` folded into `BlameState`; both `SmatchetStandalone` and `SmatchetCore_DX12` build clean. **`[hardened by #10]`** `~BlameState` now signals `worker.Cancel.store(true)` and joins `worker.Thread` synchronously — no more `std::terminate` at exit on a joinable callstack worker. Step (c) "split file" is still open — required to unblock Unreal hot-reload survival and unit tests of the callstack worker FSM (§3.3).
10. ✅ **DONE (PR [#6](https://github.com/alexandrosk0/Smatchet/pull/6), commit `a06fbdd`).** `TrackerField::IsRequired` field added to the schema; `TrackerFieldFromPlaneProperty` now populates it from the Plane property's `is_required` JSON field. The previous `if (... is_required ...) { (void)0; }` dead branch is gone (§2.1).
   - *Validation pending:* UI surfaces that consume "required" indicators should now show them for Plane custom properties; currently no UI consumer reads `TrackerField::IsRequired` directly — the field is wired through the data path but the UI hook-up is a follow-up.

**P0 summary:** **7 fully done + 3 partial** at 2026-05-10. Refresh (2026-05-11): items 3 / 4 / 6 / 7 / 8 / 9 each received hardening from PRs #9–#16 — the underlying defects flagged in [`POST_P0_REVIEW.md`](POST_P0_REVIEW.md) are all closed. No P0 regressions remain on develop tip `0a79de5`.

### P1 — significant code-health wins

11. **Extract `TicketSyncService` from `AppController`** (~700 LOC). Addresses all the streaming-sync P0 races structurally (§1.7).
12. **Extract `OfflineQueueService` from `AppController`** (~900 LOC) — `AppController_IssueCreateOffline.cpp` and field-edit equivalents (§1.7).
13. **`ITrackerBackendFactory`** to remove hard-coded `new JiraClient`/`new PlaneClient` from `AppController` (§1.7).
14. **`LuaAutomationHost`** to extract sol2 state, automation worker, and all `Lua*Bind` methods; remove `<sol/sol.hpp>` from `AppController.h` (§1.7).
15. **`TrackerHttpClient` shared base + `TrackerError` value type** — remove ~12 hand-rolled error-parsing sites; enable retry on 429/5xx/transport (§2.3, §6.2).
16. **Split `ITrackerClient` into role interfaces** (§2.3).
17. ✅ **DONE** — covered by P0 item 6 (PR [#6](https://github.com/alexandrosk0/Smatchet/pull/6), commit `1c3bebf`). `Logf` uses 2-pass `vsnprintf`; async file sink added.
18. ✅ **DONE (branch `claude/reverent-curran-109a96`; extended by PRs [#10](https://github.com/alexandrosk0/Smatchet/pull/10) and [#16](https://github.com/alexandrosk0/Smatchet/pull/16)).** `AuditWriter` struct added to `BackendAuditTrail.cpp` with a bounded `std::deque<std::string>` (max 512 entries), worker thread, and condition variable. `AppendEvent` now builds the JSON on the caller thread then posts the serialised line via `Writer().Post(j.dump())`; disk I/O happens on the writer thread. Drop-oldest policy prevents unbounded growth. `ReadRecentEvents` still holds `AuditMutex()` independently. Both targets build clean. **`[hardened by #10]`** Writer now takes `AuditMutex()` around the `<< line` write (same mutex `ReadRecentEvents` already holds — readers can no longer observe a partially-flushed line); first open / write failure logs once via the Logger ring (rate-limited atomic flags). **`[hardened by #16]`** On primary-file open failure, the writer attempts `<userdata>/smatchet_backend_audit_fallback.jsonl` and writes there; if both paths fail, a dropped-event counter LOG_ERRORs at log-spaced multiples (1 / 10 / 100 / 1000 / 10000) so persistent failures stay visible without spamming.
19. ✅ **DONE (branch `claude/reverent-curran-109a96`).** `LocalCacheManager` gains a private `stmt()` lazy-init helper and 8 `std::unique_ptr<SQLite::Statement>` members for the `SaveTicket` / `TryGetTicket` hot paths. Each statement is compiled once per lifetime; `reset()` + `clearBindings()` replaces the per-call `SQLite::Statement(db, sql)` constructor. Eliminates 8 statement compilations per `SaveTicket` call and 3 per `TryGetTicket` — meaningful at bulk-sync volumes (~1000 tickets × 8 = 8000 compilations saved per sync).
20. ✅ **DONE (branch `claude/reverent-curran-109a96`; second site closed by PR [#20](https://github.com/alexandrosk0/Smatchet/pull/20)).** `IPlugin` gains `virtual bool NeedsRestart(const TrackerConfig&) const { return false; }` (forward-declared `TrackerConfig`). `McpPlugin` overrides it, consolidating the port/bind/auth/lua-exec comparison logic previously scattered inside `PluginHost::SyncMcpPluginWithConfig`. The `dynamic_cast<const McpPlugin*>` block in `SyncMcpPluginWithConfig` is replaced by `plugins_[mcpIndex]->NeedsRestart(cfg)`. Future plugins with config-change semantics get restart-on-change for free by overriding the virtual. **`[hardened by #20]`** The companion `dynamic_cast<const McpPlugin*>` in `PluginHost::GetMcpServerStatus` is also gone now: `IPlugin` gains `virtual bool TryGetMcpStatusSnapshot(McpServerStatus&) const` (gated by `SMATCHET_WITH_MCP`); `McpPlugin` overrides it; `PluginHost` iterates plugins via the virtual hook with no RTTI.
21. ✅ **DONE.** `MainThreadDispatcher` class in new `Source_Core/include/MainThreadDispatcher.h` — bounded `std::vector<Task>` queue, mutex-protected `PostToMainThread` / `Drain`. Added as `AppController::mainThreadDispatcher` (public). `SmatchetUI::Draw` drains it at the top of every frame before any window drawing. Existing atomic-flag patterns can be migrated incrementally; new deferred work should use `PostToMainThread`.
22. ✅ **DONE (branch `claude/reverent-curran-109a96`).** `ActiveLongTextEditorState` gains `RoundTripPending` + `RoundTripFireAt`; buffer-change detection arms a 100 ms timer and conversion fires only once the timer expires. Rapid keystrokes no longer trigger `MarkdownToAdf` → `AdfToMarkdown` per frame; preview stays visible with the last good output until idle.
   - *Validation pending:* open a large description field, type quickly, confirm preview updates ~100 ms after typing stops.
23. **`PlaneClient::FetchIssuesForKeys` O(N×total)** — use Plane filter API (§2.1).
24. ✅ **DONE (branch `claude/reverent-curran-109a96`).** `JiraIssueSearch.cpp` now emits `key = "PROJ-123"` and `key in ("A","B")` — keys are double-quoted so Jira's JQL parser handles dashes, dots, and reserved words correctly.
25. ✅ **DONE (branch `claude/reverent-curran-109a96`).** `JoinBackgroundTasks` self-join branch changed from `detach()` to re-queuing the thread back into `backgroundWorkers_` + `LOG_WARN`. Detached threads could race against `g_TrackerIssueFetchMutex` static teardown; re-queued threads are now joined by `~AppController` on the main thread.
26. ✅ **DONE.** `availableFieldsMutex_` (`mutable std::mutex`) added to `AppController`. `SetFieldCatalog` holds the lock while moving the new field vector in; `FindFieldById` holds it while iterating. `GetAvailableFields()` is documented UI-thread-only.
27. ✅ **DONE (branch `claude/reverent-curran-109a96`).** `sLinkHrefs` promoted from `static thread_local` (two separate block-scope statics in open/close tag handlers — which were actually two independent variables, breaking link-href tracking across calls) to a single `std::vector<std::string> linkHrefs` local at the top of `HtmlToMarkdown`. Properly shared by both handlers; reset on every call; no state leak on exception.
28. **Markdown table-cell rich content lost on ADF→Markdown** (§2.2; tie into v2 backlog). ⏳ OPEN — tracked in `RICH_TEXT_EDITING_V2_REMAINING.md`.
29. ✅ **DONE (branch `claude/reverent-curran-109a96`).** `LookupHost(SmatchetImGuiHostHandle)` helper added to the anonymous namespace in `SmatchetImGuiHost.cpp`; checks `gLiveHostHandles` under `gHostHandleSetMutex`. All 18 `SmatchetHost_*` accessor functions updated from bare `reinterpret_cast` to `LookupHost` — stale handles from Unreal hot-reload or post-`Destroy` calls return `nullptr` and early-out instead of dereferencing freed memory.
30. ✅ **DONE (branch `claude/reverent-curran-109a96`).** `MultiSelectActiveKey` and `MultiSelectSearchBuf[128]` added to `SpreadsheetState`; `RenderMultiSelectEditor` receives `SpreadsheetState&` and uses those members instead of the two block-scope statics. Per-instance state, properly reset on editor-key change, no aliasing across concurrent combos.
31. ✅ **DONE (branch `claude/reverent-curran-109a96`).** `StatusRowColor` lambda with a frame-local `std::unordered_map<std::string, ImVec4>` declared before the row clipper loop. Each unique status string is lowercased and pattern-matched exactly once per frame; subsequent rows with the same status hit the map in O(1). Eliminates `ToLowerAsciiCopy` allocation + 4 `string::find` calls per visible row.

### P2 — refactor / polish

32. ✅ **DONE (branch `claude/reverent-curran-109a96`).** All 7 raw `std::unique_ptr<T>(new T(...))` in `AppController.cpp` switched to `std::make_unique<T>(...)` (`LocalCacheManager` × 3, `PlaneClient` × 2, `JiraClient` × 2).
33. ✅ **DONE.** `MakeUniqueTempFilePath` mixes `time_since_epoch` + `std::hash<thread::id>` + monotonic `std::atomic<uint64_t>` counter. Two concurrent downloads from different threads can no longer collide.
34. Two near-identical retry-after-400 blocks in `AppController_CatalogAndFieldEdit.cpp:790-808` vs `962-975` (§1.7). ⏳ OPEN.
35. ✅ **DONE (branch `claude/reverent-curran-109a96`).** `GetTrackerFieldCatalogRevision()` removed from `AppController.h` (had zero callers outside the header); `GetFieldCatalogRevision()` is the canonical name.
36. ✅ **DONE.** `AppController.cpp` two `std::transform(… ::tolower)` loops replaced with `ToLowerAsciiCopy(...)`.
37. ✅ **DONE.** `DecodeHtmlEntities` in `MarkdownConvert.cpp`: named-entity if-chain replaced with a `static const std::unordered_map<std::string, char> kNamedEntities`; O(1) lookup per entity.
38. `MarkdownConvert` reuse scratch buffers in `EmitInlineText` (§2.2). ⏳ OPEN.
39. `PlaneClient::FetchIssueEditMeta` hardcoded 7 fields (§2.1). ⏳ OPEN.
40. `PlaneClient::BuildCreatePayload`/`BuildUpdatePayload` ignore custom fields (§2.1). ⏳ OPEN.
41. `ScopedFileLock` + `AtomicWriteTextFile` extract to `FileIo` helper (§4.9). ⏳ OPEN — Windows-specific Win32 APIs; separate PR scope.
42. ✅ **DONE.** `LocalCacheManager::ForEachTicket(const std::function<void(CachedTicket&&)>&)` streaming overload: single LEFT JOIN query; callback per fully-populated ticket; avoids materialising the full result set.
43. `BackendAuditTrail::LooksSensitiveKey` — blocklist is intentional design. ⏳ OPEN for product review of specific field inclusions.
44. ✅ **DONE.** `NetworkUsageTracker`: `trackerErrors_` atomic + `trackerErrors` snapshot field added; `Record()` increments on non-2xx status; `Reset()` clears it.
45. ✅ **DONE.** `Views.h` bodies moved to `Source_Core/src/Views.cpp`; header is now declarations-only + trivial one-liner accessors. Removes `<algorithm>`, `<cctype>`, and all method bodies from transitive includes.
46. ✅ **DONE.** `NavigationHistory::Push` caps at `kMaxHistory = 200`; trims oldest entries and adjusts `_index` when exceeded.
47. ✅ **DONE.** `SmatchetImageTextureCache` LRU O(N) → O(1): `CacheValue::LruIt` stores the list iterator; `TouchLruUnlocked` uses `g_lru.splice()` in O(1) instead of `std::find` scan.
48. ✅ **DONE.** `SmatchetLocalization::StoreTempString` ring increased from 64 to 512 — a frame with 200 visible rows × ~3 label lookups each no longer risks overwriting live pointers.
49. ✅ **DONE.** `LuaConsolePlugin`: `static int s_tabSel` and `static bool s_pendingSelectScriptsTab` promoted to `tabSel_` and `pendingSelectScriptsTab_` member fields. Two plugin instances no longer share UI state.
50. ✅ **DONE.** `WriteFileAll` checks `o.good()` after `o << content`; returns `false` with a disk-full/I/O error message on failure.
51. ✅ **DONE.** `TryParseLuaErrorLine`: `std::regex_search` replaced with a linear `:<digits>:` hand-parser; O(N) no backtracking; `<regex>` include removed.
52. ✅ **DONE.** `BuildRunLuaToolEntry()` helper extracted; both REST `/mcp/tools/list` and JSON-RPC `tools/list` call it. `SaveLuaLayoutDebounced` duplicate guard also removed.
53. ✅ **DONE.** `SetWindowFontScale` in `TicketFieldEditor.cpp`: saves `prevScale = GetCurrentWindow()->FontWindowScale` before scaling and restores it after, instead of hardcoding 1.0f.
54. Manual `PushClipRect` per cell in grid — likely redundant (§3.4). ⏳ OPEN.
55. ✅ **DONE.** `stable_sort` comparator: `SortKey` struct pre-resolves `catalogIndex.Find` once per sort-spec before the sort — O(1) lookup per comparison instead of O(N log N) calls to `Find`.
56. ✅ **DONE.** Column widths pre-resolved into `std::vector<float> colWidths` before `TableSetupColumn` loop; eliminates `ColumnWidths.find` per column per frame.
57. ✅ **DONE.** `cellFeedbackByKey.find` skipped when the map is empty (common case) — `BuildCellKey` string allocation avoided on idle frames for every cell.
58. ✅ **DONE.** `PlaneClient::BuildBrowseUrl`: if `PlaneWorkspaceSlug` starts with `/`, separator is `""` not `"/"` to prevent double-slash in the URL.
59. ✅ **DONE.** `AddIssueToSprint`: Jira Agile API accepts both keys and IDs; clarifying comment added. If a specific board requires numeric IDs only, numeric-ID resolution via `/rest/api/3/issue/{key}?fields=id` is documented at the call site.
60. ✅ **DONE.** `StripUtf8BomCopy` added to `StringUtil.h` as an inline; removed from `PlaneClient.cpp` anonymous namespace — all call sites in `PlaneClient.cpp` now use the shared version.
61. ✅ **DONE.** `JiraIssueMutation.cpp`: status-transition name-fallback path split into a separate `else if` that emits `LOG_WARN` with the transition name, target status name, and actual status name before using the fallback id.

### Sequencing suggestion

Three-week shape:

**Week 1 (safety + build win):** items 1, 2, 3, 4, 5, 6, 7. Single PR per item; each is independently shippable. Item 3 lands the largest build-time win and unblocks items downstream.

**Week 2 (UI perf + extraction prep):** items 8, 9, 10, 11 (start), 22, 31. Item 9 and 11 need test scaffolding (item 49 — but really part of the §6.7 testing proposal) — land that first.

**Week 3 (structural refactors):** items 12, 14, 15, 18, 20, 21. Each builds on the §6.7 test infrastructure landing.

### Out of scope here / already tracked

- Rich-text v2/v3 gaps — see [`RICH_TEXT_EDITING_V2_REMAINING.md`](RICH_TEXT_EDITING_V2_REMAINING.md), [`RICH_TEXT_EDITING_V3_PLAN.md`](RICH_TEXT_EDITING_V3_PLAN.md).
- Cppcheck periodic-sweep runbook — see [`CPPCHECK_PLAN.md`](CPPCHECK_PLAN.md) (now partially superseded by `.claude/hooks/lint-cpp.sh` for per-edit checks; the pre-existing baseline noise closed by PR #21 was tracked separately at [Smatchet#18](https://github.com/alexandrosk0/Smatchet/issues/18)).
- Issues *introduced* by the P0 sweep (PRs #6 / #7 / #8) — fully tracked in [`POST_P0_REVIEW.md`](POST_P0_REVIEW.md). All P0/P1 entries closed; one P2 partial remains (item 24 — `FlushFileSink` is wired but uncalled from the crash-handler / shutdown path).
- Deferred manual smokes — see [`MANUAL_TEST_QUEUE.md`](MANUAL_TEST_QUEUE.md). Build re-verify on develop tip `0a79de5` (B1 / B2), per-PR runtime smokes (M1–M12), cumulative validations (L1–L3).

---

_End of review._

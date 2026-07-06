# Smatchet — Code Review Backlog (2026-05-16 full rewrite)

> Scope: first-party C++ in `Source/Core/`, `Source/Plugins/`, `Source/Standalone/`.
> Method: skeleton + targeted reads + symbol grep against develop tip `7597fd7+` (post PR #39).
> Previous doc (2026-05-10..11) accumulated 62 numbered items, 87% of which landed. This rewrite drops all `✅ DONE` items and re-audits the remainder against current code.
>
> Status legend: P0 = bug / safety / build-break · P1 = significant code-health win · P2 = nice-to-have · P3 = note.
>
> Companions:
> - [`POST_P0_REVIEW.md`](POST_P0_REVIEW.md) — issues introduced by the P0 sweep. All P0/P1 closed; one P2 partial (FlushFileSink shutdown wiring — listed below as B4).
> - [`MANUAL_TEST_QUEUE.md`](MANUAL_TEST_QUEUE.md) — deferred smoke tests.
> - [`RICH_TEXT_EDITING_V2_REMAINING.md`](RICH_TEXT_EDITING_V2_REMAINING.md), [`RICH_TEXT_EDITING_V3_PLAN.md`](RICH_TEXT_EDITING_V3_PLAN.md) — Markdown / ADF backlog.

---

## Status snapshot (since 2026-05-10 baseline)

| Category | Count | State |
|----------|-------|-------|
| P0 originals | 10 | 9 ✅ · 1 🟡 (#9 BlameAnalysis split now ✅ — see N2 below) |
| P1 originals (11–31) | 21 | 19 ✅ · 2 🟡 (item 14 LuaAutomationHost · item 15 TrackerError) |
| P2 originals (32–61) | 30 | 23 ✅ · 7 ⏳ |
| New (62+) | 1 | ✅ DONE (priority hot-path PR #40) |
| Post-P0 review | 34 | 33 ✅ · 1 🟡 (B4 FlushFileSink shutdown) |

**Net open before this rewrite:** 14 carry-overs + new findings from this pass.

---

## Reconciliation pass — 2026-07-05 (develop tip ~PR #1620)

> This doc was written on 2026-05-16; the tree has since advanced ~1500 merged PRs, with major decomposition of the god-objects it tracks. Each still-open/partial item below was re-checked against current source. Item headings now carry a status marker (✅ RESOLVED · 🟡 PARTIAL · ⏳ OPEN). Note the file relocations: `ConfigManager.cpp`→`Config/`, `BuiltinCommands.cpp`→`Commands/Builtin/`, `PlaneClient.cpp`→`Tracker/Plane*.cpp`, the field-edit pipeline→`FieldEditPipelineService.cpp`.

| Item | Verdict | Current state |
|---|---|---|
| A1 TrackerConfig caching | ✅ RESOLVED | `ConfigManager::Load()` returns `GetCachedConfigRef()` under `GetCacheMutexRef()` (valid-flag cache, invalidated on `Save()`) — kills the repeated disk read/parse. |
| A2 Logger file-sink wiring | ✅ RESOLVED (differently) | Wired at startup in `Source/Standalone/main.cpp` + `android_main.cpp` via `Logger::SetFileSinkPath` (env `SMATCHET_DEBUG_LOG` / default path), **not** from `ConfigManager`; no `LogFilePath` key. Functional complaint closed. |
| A4 FlushFileSink on shutdown | ✅ RESOLVED (graceful) | `~AppController` now calls `Logger::Instance().FlushFileSink()` after `JoinBackgroundTasks()`, persisting the whole shutdown-sequence log trail before late teardown. The crash-handler half is intentionally **not** done — `SmatchetCrashHandler` is async-signal-safe and must not take the file-sink mutex mid-crash (it uses its own async-safe crash sink). |
| B1 LuaAutomationHost extraction | 🟡 REFRAMED (won't-do-as-written) | Ownership migration abandoned for a different design: `LuaAutomationHost` is now a 17-LOC log-sink coordinator; sol2 moved to a pImpl (see N10); the binding TU split 3 ways; `friend class LuaAutomationHost` removed; new `ILuaBindingHost` interface. |
| B2 TrackerHttpClient migration | ✅ OBJECTIVE MET (2026-07-05) | Uniform retry on transient failures is achieved — implemented at the `TrackerXxxLogged` helper layer, so every tracker HTTP path inherits it. 2B (`JiraClient::ProbeReachability`) migrated + shared `ClassifyReachabilityProbe`; 2C/2D mutations settled single-attempt (retry owned by the offline-queue replay loop); 2E (Jira search reads) verified already-retried via `TrackerGetLogged`. Repo sweep found one raw `cpr` verb (Jira multipart attachment upload) — now also applies `MakeTrackerSslOptions()` (Android CA-bundle parity, WS2/#1068). Only the consumer-side `IsTrackerTransportErrorText` retirement (N12) remains. Characterization guard: `tests/Core/JiraClientHttp.test.cpp`. |
| B3 ITrackerClient split | ✅ RESOLVED (exceeded) | `ITrackerClient` gone; replaced by `ITrackerBackend` composing 6 role interfaces (`ITrackerIssueReader`/`Connectivity`/`FieldCatalog`/`IssueMutations`/`Collaboration`/`Activity`). "Unsupported default-impl" pattern removed. |
| B4 Plane FetchIssuesForKeys | ✅ RESOLVED | Early-exit pagination in `Tracker/PlaneIssueSearch.cpp` stops once all keys matched. Server-side `sequence_id__in` filter is the remaining B4-v2 follow-up. |
| B5 Markdown table-cell flatten | 🟡 IMPROVED | `MarkdownCellPlainInner` now joins cell blocks with `<br>` (GFM in-cell line break) and preserves list items instead of running paragraphs together / dropping lists. First ADF→Markdown table golden tests added. Deeper fidelity (code blocks, nested lists/tables in a cell) still deferred to RICH_TEXT_EDITING_V2. |
| C1 Retry-after-400 dup | ✅ RESOLVED | Consolidated into `FieldEditPipelineService::ApplyFieldUpdateWithEditMetaRetry`; both submit paths route through it. |
| C4 Plane customs dropped | ⏳ OPEN | `PlaneIssueMutation.cpp` `BuildCreatePayload` still ignores its `catalog` param; UUID custom props never emitted under `properties.<uuid>`. |
| C5 FileIo extraction | 🟡 PARTIAL | No `FileIo.{h,cpp}`. `AtomicWriteTextFile` promoted to a public `ConfigManager` static (shared by 4 callers); `ScopedFileLock` still confined to `ConfigManager_Internal.h`. |
| C6 LooksSensitiveKey blocklist | ⏳ OPEN | Unchanged (a product call, no code change resolves it). |
| N1 LuaBindings LOC | ✅ number stale | Now **1540** (not 2648) — the file shrank via the 3-way split, opposite the doc's "grew" narrative. |
| N3 CommandRegistry::FindLocked | ✅ RESOLVED | New alias-aware `Contains()` (locks internally, mirrors `FindLocked(name) != nullptr`); both `McpPlugin.cpp` worker-thread callers migrated to it — no registry pointer escapes to an httplib thread anymore. Regression-tested (alias resolution pinned). |
| N4 AppController.h size/friends | 🟡 MIXED | Now **1465 LOC** (+430); but friend-coupling largely resolved — three friends collapsed to one `GridContextDepsAdapter`; sol2 friend gone. No `TrackerActions` interface yet. |
| N5 ConfigManager.cpp size | ✅ number stale | Split into `Config/` (`ConfigManager.cpp` 1617 + `_PathUtils` + `_Views` + `_Panes` + `_Internal.h`). Per-concern split partly done. |
| N6 BuiltinCommands split | ✅ RESOLVED | Now a 72-LOC dispatcher + ~20 category files under `Commands/Builtin/`, exactly as proposed. |
| N8 OfflineQueueService friend | ✅ RESOLVED | Decoupled via `GridContextDepsAdapter` (see `AppController.h` comment). |
| N9 McpPlugin tools/list divergence | ✅ RESOLVED | Both REST + JSON-RPC paths registry-driven from `Commands().All()` + shared `BuildRunLuaToolEntry()`; can't diverge data-wise (cosmetic lambda dup remains). |
| N10 sol/sol.hpp public | ✅ RESOLVED | `AppController.h` no longer includes `<sol/sol.hpp>` — sol2 storage moved to a pImpl; only forward decls remain. |
| N12 IsTrackerTransportErrorText | ⏳ OPEN (unblocked 2026-07-05) | Still defined + used across ~14 files, shadowing `ClassifyTrackerResponse`. B2's uniform-retry objective is now met, so this consumer-side heuristic can be retired — next actionable B2 follow-on. |
| N13 TryGetMcpStatusSnapshot | ⏳ OPEN (acceptable) | Still a gated virtual on `IPlugin`; no capability-tag system. As the doc itself said, acceptable today. |

**Still genuinely open after this pass:** C4, C6, N12 (+ N13 acceptable, B2/B5/C5/N4 partial). A4 (graceful half) and N3 were closed 2026-07-05; B5 improved (multi-paragraph + list preservation) with fuller rich-cell fidelity deferred. Everything else on the A/B/C/N carry-over list is resolved. Items already ✅ in the doc (N2, C2, C7, N7, N11, N14) re-verified still true.

---

## A. P0 / safety tails — finish these first

### A1. `TrackerConfig` caching — kill ~19 disk re-reads per UI action — ✅ RESOLVED (2026-07-05)
> `ConfigManager::Load()` now returns a cached `GetCachedConfigRef()` under `GetCacheMutexRef()` (valid-flag cache, invalidated on `Save()`); the repeated disk read/parse is gone. Cited line numbers below are stale (logic moved to `Config/ConfigManager.cpp` + `FieldEditPipelineService.cpp`).

Old item 3 tail. `ConfigManager::Load()` still hit on every field-edit submit and per-mutation:
- `Source/Core/src/AppController_CatalogAndFieldEdit.cpp` — **17 sites** (line 70, 137, 493, 598, 612, 871, 1009, 1075, 1094, 1112, 1126, 1135, 1150, 1159, 1176, 1185, 1205).
- `Source/Core/src/AppController_LuaBindings.cpp:676, 921`.
- `Source/Core/src/PlaneClient.cpp:1106, 1290`, `Source/Core/src/JiraIssueMutation.cpp:60, 481, 556`.

Each `Load()` opens + reads + parses ~50-field JSON. Plumb a `const TrackerConfig&` snapshot from the public entry of each pipeline; add a `ConfigManager::CachedSnapshot()` with a revision counter for read-only fast-path callers.

**Severity:** P1 (it's perf + lock churn, not correctness). Listed P0 because §6.3 of old doc tagged it that.

### A2. `Logger::SetFileSinkPath` never wired from `ConfigManager` — ✅ RESOLVED (2026-07-05, differently)
> The file sink is no longer dark: it's wired at startup in `Source/Standalone/main.cpp` + `android_main.cpp` (env `SMATCHET_DEBUG_LOG` / default path), not from `ConfigManager::Load()`, and there is no `LogFilePath` key. The functional gap is closed; the doc's specific prescription was not followed.

Old item 6 tail. Header API + worker thread exist (`Source/Core/include/Logger.h:72`, `Source/Core/src/Logger.cpp:197`); `ConfigManager.cpp` never calls it. File sink stays dark unless a test path is set manually. Wire on `ConfigManager::Load()` post-parse with the chosen log path; honour a new `LogFilePath` config key (or default `<userdata>/smatchet_runtime.log`).

### A3. `TrackerField::IsRequired` not consumed by UI — shipped
Old item 10 tail. Schema populated for Plane (`PlaneClient.cpp` `TrackerFieldFromPlaneProperty`); zero UI consumer in `TicketFieldEditor.cpp` / `SmatchetNewIssueDraftUi.cpp`. Required-field state is dead data. Add a `*` glyph + tooltip on required-field labels in the new-issue draft and in-line editor; refuse submit / show validation on blank required. — shipped on branch `feat/required-field-ui-glyph` (PR pending).

### A4. `FlushFileSink` not called on shutdown / crash path — ✅ RESOLVED (graceful half, 2026-07-05)
POST_P0 item 24 (last open from that review). `Logger::FlushFileSink()` existed with no production caller. **Now wired** in `AppController::~AppController` immediately after `JoinBackgroundTasks()` — at that point every background thread that can emit a log line is joined, so the flush persists the entire shutdown-sequence trail to disk before member destruction and the riskier late-teardown steps run.

The original "call it from a `std::set_terminate` / `SIGSEGV` handler" ask is **intentionally not done** and is now superseded: this codebase grew a dedicated async-signal-safe crash handler (`Source/Standalone/SmatchetCrashHandler.cpp`) *after* this item was filed. That handler deliberately avoids the logger mid-crash — `FlushFileSink()` takes `m_fileSinkLifecycleMutex`/`m_fileSinkMutex` and waits on `m_fileSinkAckCv`, none of which is async-signal-safe (deadlock/UB risk in a signal context). Crash-time diagnostics are instead captured by the crash handler's own async-safe crash sink (marker + breadcrumb + minidump). So the abrupt-crash log batch is covered by a *different, safe* mechanism, and the graceful-shutdown drop-the-last-batch gap this item was really about is closed.

---

## B. P1 structural — open big wins

### B1. LuaAutomationHost Phase 1B → 1D (item 14) — 🟡 REFRAMED (2026-07-05, won't-do-as-written)
> The ownership migration below was abandoned for a different design: `LuaAutomationHost` is now a 17-LOC log-sink coordinator, sol2 moved to a pImpl (N10), the binding TU split 3 ways (`AppController_LuaBindings.cpp` **1540 LOC** + `_LuaBindingsCore.cpp` + `_LuaBindings_Draw.cpp`), the `friend` was removed, and a new `ILuaBindingHost` interface was introduced. The structural pain is addressed; the literal 1B/1C/1D extraction did not happen.

`Source/Core/src/AppController_LuaBindings.cpp` grew from 1453 → **2648 LOC** since the old review — Phase 1A migrated only log-sinks. Remaining:
- **1B** `InitLua` / `InitLuaCore` (~150–300 LOC). `<sol/sol.hpp>` stays PUBLIC on `AppController.h:11` until 1C; 1B alone is plumbing (low PR-value in isolation — see N1).
- **1C** ~18 `Lua*Bind` methods + free-function glue (~1500 LOC at current size). High-risk: patched-metatable contract per `CMakeLists.txt:355-411`.
- **1D** Automation worker thread (`AutomationWorkerLoop`, `automationJobMutex_`, shutdown contract).
- **2** Replace `friend class LuaAutomationHost;` with `TrackerActions` interface.

**Recommendation:** bundle 1B+1C as one PR with golden tests for each binding before move (testless extraction = lottery).

### B2. `TrackerHttpClient` migration follow-on (item 15) — 🟡 IN PROGRESS (harness-first restart 2026-07-05)
Phase 2A landed (PR #39) — helper + `PlaneClient::ProbeReachability`. ~30 hand-rolled error-status branches remain. Restarted "harness-first" 2026-07-05: `tests/Core/JiraClientHttp.test.cpp` now characterizes the real `JiraClient` HTTP-status→kind matrix over the `JiraCatalogHttpFixture` loopback (the harness already existed — used by `TrackerCatalogBuild`/`*Http` suites), so each migration batch has a regression guard. Migrate in batches:
- **2B** `JiraClient::ProbeReachability` — ✅ **done 2026-07-05** (routed through `ClassifyTrackerResponse`, mirrors Plane's Phase-2A switch; characterization test pins the status matrix). Reachability classification is now shared across both backends via `ClassifyReachabilityProbe` (extracted 2026-07-05 to kill the DRY clone flagged by the duplication lint; `PlaneClient`/`JiraClient`/`PlaneIssueSearch` all route through it).
- **2C** `PlaneClient::UpdateIssueFields` / `CreateIssue` / `FetchIssueEditMeta` — ✅ **decided 2026-07-05: mutations stay SINGLE-ATTEMPT, no per-call retry** (guardrail comments added at both call sites). Rationale:
  - `UpdateIssueFields` (PATCH) is driven by `OfflineQueueService::ReplayOneFieldEdit`, which already retries transient failures on its own tick with attempt bookkeeping. Wrapping the call in `TrackerHttpRequestWithRetry` would stack two retry loops and block the replay worker for the internal backoff. Direct (online) callers accept one attempt and surface a retryable `TrackerError`.
  - `CreateIssue` (POST) is **non-idempotent** — a retry after the server committed the create (5xx/timeout after receipt) would duplicate the issue. Durability for queued creates is owned by `OfflineQueueService::ReplayOneCreate` (pending-create latch de-dups).
  - `FetchIssueEditMeta` makes **no HTTP call** (`PlaneFieldCatalog.cpp` returns a static built-in field map), so there is nothing to retry. Nothing to migrate here.
  - Net: 2C is resolved by decision, not by new retry code. The single-attempt boundary is now documented in-code so a future contributor doesn't "helpfully" add a second retry layer.
- **2D** `JiraIssueMutation.cpp` mutation paths — same decision as 2C (mutations single-attempt; retry owned by the offline-queue replay loop). No code change; keeps Jira/Plane mutation semantics symmetric.
- **2E** `JiraIssueSearch.cpp` paginated fetches — ✅ **already retried 2026-07-05 (verified, no code change needed)**. The migration turned out to be implemented at the *helper* layer, not call-by-call: `TrackerGetLogged` (both overloads) already wraps `TrackerHttpRequestWithRetry` with the default idempotent predicate (Transport / 429 / 5xx), and every HTTP call in `JiraIssueSearch.cpp` (comment pages, JQL search pages, fetch-by-key, per-issue fallback, `myself` diagnose) routes through `TrackerGetLogged`. A repo-wide sweep for raw `cpr::Get/Post/Put/Patch` in `Source/Core/src/Tracker/` found **exactly one** verb that bypasses the helpers — the Jira multipart attachment upload (`JiraIssueMutation.cpp`), which can't use `TrackerPostLogged` (string-body only). That call already had the redirect guard + usage/log wiring; **2026-07-05 it also picked up `MakeTrackerSslOptions()`** (now exposed from `TrackerHttpUtils.h`) so it uses the same Android CA-bundle trust anchor (WS2 / Issue #1068) as every other tracker call instead of falling back to libcurl's default store. It stays single-attempt by design (non-idempotent POST).

**Net for B2: the migration objective — uniform retry on transient failures across every tracker HTTP path — is met.** Idempotent reads (GET) retry Transport/429/5xx via `TrackerGetLogged`; idempotent writes (PUT/PATCH) retry via `TrackerPutLogged`/`TrackerPatchLogged`; non-idempotent writes (POST + the multipart attachment) are Transport-only or single-attempt by design; and mutations additionally get durable retry from the offline-queue replay loop (2C/2D). The only remaining B2-adjacent work is the consumer-side **N12** cleanup: with the tracker clients now returning structured `TrackerError` everywhere, the `IsTrackerTransportErrorText` string heuristic can be retired.

### B3. Split `ITrackerClient` into role interfaces (item 16) — ✅ RESOLVED (2026-07-05, exceeded)
> `ITrackerClient` no longer exists; replaced by `ITrackerBackend` composing 6 role interfaces (`ITrackerIssueReader`/`Connectivity`/`FieldCatalog`/`IssueMutations`/`Collaboration`/`Activity`). The "unsupported default-impl" pattern is gone (optional roles return `nullptr` accessors).

`ITrackerSearch` / `ITrackerMutation` / `ITrackerSchema` / `ITrackerUserDirectory` / `ITrackerWorkflow` (Jira-only). Removes the "unsupported default-impl" pattern (~7 virtuals). Stage with `dynamic_cast` at call sites. Mechanical PR.

### B4. `PlaneClient::FetchIssuesForKeys` O(N×total) (item 23) — ✅ RESOLVED (2026-07-05; server-side filter = B4-v2 follow-up)
`PlaneIssueSearch.cpp:556` (file split from `PlaneClient.cpp`) still pulled every page then filtered in memory. Early-exit pagination now stops fetching once every requested key has been matched — cuts the hot prefetch-open-links path from `O(total)` to `O(pages_until_keys_found)`. Server-side `sequence_id__in` filter would be the next win (requires `FetchIssuesStreamed` URL-builder rework); leave as B4-v2 follow-up.

### B5. Markdown table-cell rich content lost on ADF→Markdown (item 28) — 🟡 IMPROVED (2026-07-05)
> `MarkdownCellPlainInner` (`Source/Core/src/Ui/MarkdownConvert.cpp`) now collects each cell block's inline text and joins blocks with an HTML `<br>` (GFM's single-line-cell line break), and represents `bulletList`/`orderedList` items with markers — so multiple paragraphs and lists survive instead of being merged into one run or silently dropped. Added the first ADF→Markdown table golden tests (`tests/Core/MarkdownConvertAdf.test.cpp`). Remaining (deferred to RICH_TEXT_EDITING_V2, needs full round-trip golden coverage): code blocks, nested lists, and nested tables inside a cell — GFM can't hold true block content in a cell, so those need a design decision on representation.
`MarkdownConvert.cpp` `MarkdownCellPlainInner` flattens. Tracked partly in `RICH_TEXT_EDITING_V2_REMAINING.md`; promote to its own ticket once round-trip golden tests land.

---

## C. P2 polish — leftovers

### C1. Two retry-after-HTTP-400 blocks (item 34) — ✅ RESOLVED (2026-07-05)
> Consolidated into `FieldEditPipelineService::ApplyFieldUpdateWithEditMetaRetry`; both submit paths route through it (`ErrorTextContainsHttpStatus` moved into the pipeline service).

`AppController_CatalogAndFieldEdit.cpp:837` and `:1009` both `if (!updateOk && ErrorTextContainsHttpStatus(outError, 400)) { ... refetch edit-meta ... retry }`. Extract `SubmitWithEditMetaRetry(issueId, field, payload, ...)` helper.

### C2. `MarkdownConvert::EmitInlineText` per-node vector allocs (item 38) — ✅ shipped (branch `feat/markdown-emitinlinetext-scratch`)
Rebuilds `openWrap` / `closeWrap` vectors per text node. Reuse a scratch buffer member. Done via `thread_local std::vector<const char*>` (capacity persists, mark markers are all string literals so no `std::string` heap churn).

### C3. `PlaneClient::FetchIssueEditMeta` hardcoded 7 fields (item 39) — 🟡 partial (branch `feat/plane-fetchissueeditmeta-broaden`)
`PlaneFieldCatalog.cpp:492` (file split from `PlaneClient.cpp`). Broadened to 9 built-ins matching what `BuildCreatePayload` / `BuildUpdatePayload` / `AddIssueToSprint` actually serialize (`+ type, parent`). Real per-issue permissions query deferred — Plane v1 has no capability endpoint; custom-property editability blocked on C4 (`properties.<uuid>` serialization).

### C4. `PlaneClient::BuildCreatePayload` / `BuildUpdatePayload` drop customs (item 40)
`PlaneClient.cpp:1459-1501` handles 6 core IDs (`summary`/`description`/`priority`/`status`/`type`/`parent`/`assignee`). Any `TrackerField.Id` that's a UUID (custom property) is silently dropped. Iterate `catalog` for custom props and emit under `properties.<uuid>` or whichever shape Plane v1 accepts.

### C5. Extract `ScopedFileLock` + `AtomicWriteTextFile` (item 41) — 🟡 PARTIAL (2026-07-05)
> No `FileIo.{h,cpp}` module. `AtomicWriteTextFile` was promoted to a public `ConfigManager` static (shared by `FieldCatalogCache`, `SmatchetUI`, `_Views`, `_Panes`); `ScopedFileLock` is still confined to `ConfigManager_Internal.h`.

Both still defined in `Source/Core/src/ConfigManager.cpp` anonymous namespace (lines 179+, ~700+). `BackendAuditTrail` uses raw `ofstream`; export paths re-implement atomic-write. Promote to `Source/Core/{src,include}/FileIo.{h,cpp}` so all three share. Win32-only work.

### C6. `BackendAuditTrail::LooksSensitiveKey` blocklist (item 43)
Redacts `summary` / `assignee` / `body` / `text` etc. Likely too broad — audit dumps lose useful diffs. Product call: trim the list or document why each entry stays.

### C7. Manual `PushClipRect` per grid cell (item 54) — ✅ shipped (branch `feat/grid-pushcliprect-audit`)
`Source/Core/src/SmatchetActiveProjectGridUi.cpp:843, 905, 930`. ImGui table already clips columns. Profile and remove if redundant. Removed; all three sites verified inside `BeginTable("TicketGrid")` scope.

---

## N. New findings (2026-05-16 pass)

### N1. `AppController_LuaBindings.cpp` at 2648 LOC — almost 2× since baseline (P1) — ✅ number stale (now **1540 LOC**, 2026-07-05)
> The file shrank via a 3-way split (`_LuaBindings.cpp` 1540 + `_LuaBindingsCore.cpp` 329 + `_LuaBindings_Draw.cpp` 1294) — the opposite of the "grew" narrative below. See B1.

Was 1453, now **2648**. Phase 1A of item 14 was supposed to start shrinking it; instead it grew. New surface includes `commands.invoke` glue, `RunAutoScript`, `RunFlatScriptAsync`, ~6 new `LuaUi*Bind`, window-op queue. The pre-existing extraction plan now has more to move; Phase 1C bundle is bigger than the doc predicted. Re-scope before committing.

### N2. `BlameAnalysisUi.cpp` split landed but not noted (✅)
Old item 9 step (c) was OPEN. Now done: `BlameAnalysisUi_Config.cpp` / `_Launch.cpp` / `_Modals.cpp` / `_Preferences.cpp` / `_Window.cpp` / `_Worker.cpp` + `BlameAnalysisUi_Internal.h`. Move from OPEN → done in tracker. Unreal hot-reload survival check still pending (no test infra).

### N3. `CommandRegistry::FindLocked` is a footgun (P2) — ✅ RESOLVED (2026-07-05)
> Closed via the second offered remedy (lock internally, return a value not a pointer). Added `CommandRegistry::Contains(name)` — alias-aware, takes the registry mutex internally, and returns a `bool` that exactly mirrors `FindLocked(name) != nullptr`. Both `McpPlugin.cpp` httplib-worker callers now use `Contains(name)`, so no `FindLocked` pointer escapes to a worker thread. `HasExact` was **not** the right substitute here: it is exact-only, so it would have broken the legacy-MCP aliases (`list_active_tickets` / `search_active_tickets`) by routing them to a fallback handler instead of the registry — a regression the new `Contains` test pins against. `FindLocked` is kept (renamed not needed) for the UI/Dispatch callers that legitimately dereference the command under the UI thread / internal lock.

`Source/Core/include/Commands/CommandRegistry.h:46` and `src/Commands/CommandRegistry.cpp:45-58`. Method name implies lock held but the impl is **lockless** — the comment says "Caller must serialize externally if they want stable pointers." External callers do NOT serialize: `Source/Plugins/Mcp/McpPlugin.cpp:629, :860` call it from httplib worker threads checking `!= nullptr` only. Race is benign today (pointer-equality test) but the API will bite future callers that dereference. Either:
- Rename to `FindUnlocked` + add `Has(name)` for the only existing real use, **or**
- Take the mutex internally and return a copy (`Optional<Command>`-style via `bool Find(name, Command& out)`).

### N4. `AppController.h` grew to 1035 LOC (was 729) (P1) — 🟡 MIXED (now **1465 LOC**; friends resolved, 2026-07-05)
> Size grew further (1465), but the friend-coupling half is largely resolved — the three friends collapsed into one `GridContextDepsAdapter`, and the sol2 friend is gone (N10). No `TrackerActions` interface yet (the "Phase 2" step remains).

Net +306 lines of public surface — `friend` declarations, forward decls, new structs (`CommandRegistry` accessors, `ScenarioRunner`, `MainThreadDispatcher`), `IsOnUiThread()`, etc. Old §1.3 P1 (cross-concern struct definitions forcing TU recompile) is now worse. With three services already extracted (`Ticket/Offline/Lua`), each service's own DTOs can move into its own header. Concrete refactor:
- `TrackerConnectivityBannerForUi` → already small, keep here.
- `TrackerIssueFetchPack` → move to `TicketSyncService.h`.
- `AppUpdateAsset` / `AppUpdateInfo` → new `AppUpdateClient.h`.
- All `*Summary` structs the §1.3 P1 callout listed → matching service header.
- Forward declarations of `OfflineQueueService` / `TicketSyncService` / `LuaAutomationHost` are fine; but `friend` of all three is a code-smell siren — every `private:` member is effectively public to ~70% of the codebase. The Phase 2 step (per old §1.7 design proposal #4: `TrackerActions` interface) is overdue.

### N5. `ConfigManager.cpp` grew to 1773 LOC (was 1333) (P2) — ✅ number stale (split into `Config/`, 2026-07-05)
> Split into a `Config/` directory: `ConfigManager.cpp` 1617 + `_PathUtils.cpp` + `_Views.cpp` + `_Panes.cpp` + `_Internal.h`. The per-concern split this item anticipated partly happened.

Post-split was supposed to be ~1333 LOC. Growth = +440 LOC. Spot-check: new config keys + bootstrap helpers. No structural issue, but the file is approaching the size threshold (~2000 LOC) where it should split per-concern (Tracker / Views / Config-file-IO / DPAPI). Track and split next time it hits 2000.

### N6. `BuiltinCommands.cpp` at 1898 LOC (P2) — ✅ RESOLVED (2026-07-05)
> Now a 72-LOC dispatcher + ~20 category files under `Commands/Builtin/` (`_View`/`_Perf`/`_Scenario`/`_Fields`/`_Tickets`/…), exactly the split proposed below.

New file — central registration of all CLI/Palette/MCP/Lua commands. Single `RegisterBuiltinCommands(reg, app)` function. At ~1900 LOC it's already a god-function risk. Split by category into `BuiltinCommands_View.cpp` / `_Perf.cpp` / `_Scenario.cpp` / `_Issue.cpp` / `_Field.cpp` etc. before it grows further; the existing `ViewCommands.cpp` precedent shows the pattern works.

### N7. `MarkdownConvert.cpp` at 1700 LOC, still no golden tests (P1) — ✅ DONE
~~Round-trip Markdown ↔ ADF / HTML continues to grow; bootstrap golden tests.~~ Closed since: `tests/Core/MarkdownConvert.test.cpp` covers the round-trip converters in `SmatchetTests`, and `tests/fuzz/fuzz_markdown_adf.cpp` fuzzes the ADF path. Flagged stale by `TEST_COVERAGE_GAP_MAP.md` § Hygiene notes.

### N8. `OfflineQueueService` still friend-coupled to AppController (P1) — ✅ RESOLVED (2026-07-05)
> `OfflineQueueService` + `TicketSyncService` friends were replaced by a single `GridContextDepsAdapter` friend during the item 11/12 Phase 2 extraction (see `AppController.h` comment).

`Source/Core/include/AppController.h:88-93` documents the friend-access boundary with a TODO to lift to interfaces. With Phase 1A→1C complete the service is at 1032 LOC of standalone logic plus AppController-private reach-throughs. Define the minimal access bundle (`IOfflineCacheAccess { Cache(), FindFieldById(), backendAuditTrail() }`) and convert. Same for `TicketSyncService` (currently friend-coupled).

### N9. `McpPlugin.cpp` REST `tools/list` + JSON-RPC `tools/list` may have diverged again (P2) — ✅ RESOLVED (2026-07-05)
> Both paths are registry-driven from `Commands().All()` + shared `BuildRunLuaToolEntry()` — no hardcoded duplicated payload; they can't diverge data-wise (only a cosmetic copy-pasted `std::transform` lambda remains).

Old §5.1 P1 was a duplicated payload (REST `:506` vs JSON-RPC `:666`). PR #41 / #52 added `BuildRunLuaToolEntry()` for the `run_lua` row. Re-check: are the rest of the tool-list rows also shared, or did the registry-driven approach drift? Audit current `McpPlugin.cpp:586` (REST) vs JSON-RPC handler.

### N10. `<sol/sol.hpp>` STILL PUBLIC on `AppController.h:11` (P1, item 14 dependency) — ✅ RESOLVED (2026-07-05)
> `AppController.h` no longer includes `<sol/sol.hpp>` — sol2 storage moved to a pImpl (`Impl`), so the ~100 header includers no longer drag sol2 through the compiler. Only forward decls remain.

Worth flagging on its own. Every TU including `AppController.h` (which is most of `Source/Core/`) drags ~1 MB of sol2 templates through the compiler. Phase 1C of item 14 is the only thing that unblocks this. The build-time win is real and measurable (`SmatchetPch.h` comments narrate sol2 as the heaviest header).

### N11. No `tests/` directory still exists (P0 for any future refactor) — ✅ DONE
~~Bootstrap a minimal doctest target before any further extraction.~~ Closed since: the doctest rig exists at scale (`tests/` holds 270+ test files across `SmatchetTests`, `SmatchetTsanTests`, UI, fuzz, Lua, and bats suites) and every candidate unit listed here (FuzzyMatch, MarkdownConvert, JqlProjectScope, TextMerge, CompactDateFormat) is covered. Flagged stale by `TEST_COVERAGE_GAP_MAP.md` § Hygiene notes; the remaining per-TU gaps are tracked there, not here.

### N12. `IsTrackerTransportErrorText` heuristic still classifies (P2)
`Source/Core/src/TrackerHttpUtils.cpp:161-236` — string-pattern-matching error text. `TrackerError` now classifies properly via HTTP status. `IsTrackerTransportErrorText` should disappear once item 15 migration completes; until then it shadows the new mechanism and produces inconsistent classifications when callers mix the two.

### N13. `IPlugin::TryGetMcpStatusSnapshot` couples the host to MCP (P3)
PR #20 replaced `dynamic_cast<McpPlugin*>` with `virtual bool TryGetMcpStatusSnapshot(McpServerStatus&)`. Cleaner, but the virtual is `#if SMATCHET_WITH_MCP`-gated on the base class — every plugin now ships a conditional v-table slot. Acceptable today; if more plugin-type-specific accessors land, switch to capability tags / `IPluginCapability* GetCapability(CapabilityId)` so the base interface stays MCP-agnostic.

### N14. Annotate config hydrate does sync disk I/O on the UI thread (P2, Pillar 2) — ✅ DONE
~~`AnnotateAnalysisUi::ensureSettingsBuffersLoaded()` → `HydrateAnnotateCfgDiskOnce()` calls `ConfigManager::LoadAnnotateAnalysis` synchronously on the UI thread.~~ **Resolved** across PR #568 (`annotate-async-config-hydrate`) + PR #574 (`config-io-safe-coalesced-writes`):
- **Load**: measured at **0.54 ms** (small once-guarded whole-file JSON read) — kept synchronous with a `PILLAR2_INLINE` annotation per the documented inline-vs-async hydration policy (a background-thread + dispatcher round-trip isn't worth it for a sub-ms one-time read).
- **Save**: the larger Pillar-2 exposure (per-edit `SaveAnnotateAnalysis` from UI callbacks, surfaced by CodeRabbit on #565) is now off the UI thread via the single coalescing `smatchet::config_save` worker, with `GetConfigRmwMutexRef` serializing the read-modify-write so concurrent writers can't lose updates.

---

## Sequencing (revised 2026-05-16)

**Now (low-risk, high-leverage):**
1. **N11** — bootstrap doctest `tests/` with 3–4 starter units. Unblocks everything else.
2. **A1** — `TrackerConfig` cached snapshot + revision counter. Single-PR mechanical refactor; measurable UI-latency win.
3. **A2** — wire `Logger::SetFileSinkPath` from `ConfigManager`. ~20-line change.
4. **A4** — call `FlushFileSink` from `~AppController` + crash handler. ~5-line change.

**Next (medium PRs):**
5. **B3** — split `ITrackerClient` into role interfaces. Mechanical.
6. **B2** — ✅ objective met (uniform retry via the `TrackerXxxLogged` helper layer; 2B/2C/2D/2E all resolved). Only **N12** (retire `IsTrackerTransportErrorText`) remains as follow-on.
7. **N4** — move service DTOs into their own headers; start chipping at `AppController.h` size.
8. **N6** — split `BuiltinCommands.cpp` per category.

**Defer until tests land (N11):**
9. **B1** — LuaAutomationHost 1B + 1C as a single PR. ~1700 LOC moves; needs golden tests.
10. **N7** / **B5** — Markdown rich-content fixes. Needs round-trip golden tests.

**Standing:**
- C1–C7, A3, B4: small enough to fit when adjacent work touches the file.
- N5, N9, N12, N13: re-check next pass.

---

## Out of scope here

- Rich-text gaps → [`RICH_TEXT_EDITING_V2_REMAINING.md`](RICH_TEXT_EDITING_V2_REMAINING.md), [`RICH_TEXT_EDITING_V3_PLAN.md`](RICH_TEXT_EDITING_V3_PLAN.md).
- Deferred runtime smokes → [`MANUAL_TEST_QUEUE.md`](MANUAL_TEST_QUEUE.md).
- Cppcheck periodic-sweep runbook → [`CPPCHECK_PLAN.md`](CPPCHECK_PLAN.md).
- Post-P0 review trail → [`POST_P0_REVIEW.md`](POST_P0_REVIEW.md).

_End of backlog._

# Plan — AppController cluster extraction (follow-up slices)

> **Slug**: `appcontroller-clusters-followup` (matches this file's basename without `.md`).
>
> **Status**: `shipped` — slice 1 (MCP client-activity) shipped (PR #1660); slice 2 (Lua-script-file handling) shipped (PR #1742, squash `1461bee3`); slice 3 (AI-context) shipped (PR #1743, squash `2c580c79`); slice 4 (host-integration) shipped (PR #1749, squash `a2ae5033`); slice 5 (ticket-prefetch) shipped (PR #1754, squash `b6a33c47`; follow-up leak fix PR #1757, squash `9ed93d33`); slice 6 (field-icon path resolver) shipped (PR #1763, squash `718205df`); slice 7 (local-cache DB block) shipped (PR #1765, squash `776d1b83`; rebased onto develop's #1751 DR6 atomic-swap — the moved bodies carry the `std::atomic_store(&Cache, …)` teardown, not the pre-#1751 `Cache.reset()`/`make_unique` forms). Slice 7 was the LAST cohesive cluster — `AppController.cpp` (2862 → 571 LOC across the two plans) has no further obviously-cohesive extractable cluster, so the plan is now `shipped`.

<!-- index-summary: Continuation of the shipped appcontroller-service-extraction plan — behavior-preserving extraction of the remaining cohesive AppController.cpp clusters (flagged there as § Out of scope) into focused companion TUs, toward the ≤ ~800 LOC target. -->

## Context

[`docs/plans/appcontroller-service-extraction.md`](../appcontroller-service-extraction.md)
shipped two slices (bootstrap → `AppController_Init.cpp`, pane-context →
`AppController_PaneContexts.cpp`) via PR #1653, dropping `AppController.cpp` **2862 → 1518
LOC**, then closed at its 2-PR cap. It flagged the **remaining clusters as § Out of scope
(not designed)**: further `AppController.cpp` clusters (MCP/activity plumbing,
Lua-script-file handling, AI-context), plus a separate `AppController.h` split. This plan
carries those forward, one cohesive slice per PR, toward the shipped plan's ≤ ~800 LOC target.

**Hard environment constraint (unchanged)**: `AppController.cpp` is in `CORE_SOURCES`, which
transitively needs `cpr`/`curl`; the tarball fetch is blocked by session egress policy, so
`posix-core-check` cannot configure and **no Core TU compiles in this container**. **CI
(Windows+MSVC full build + ctest + bucket UI lanes) is the sole correctness gate** for every
slice. Approach is therefore identical to the shipped plan: byte-identical body moves,
declarations stay in `AppController.h`, curated includes, one cohesive cluster per PR.

## Approach

Each slice moves a contiguous, cohesive block of `AppController::` method *definitions* out
of `AppController.cpp` into a new `AppController_<Area>.cpp` companion TU. Declarations stay
in `AppController.h` untouched — callers, linkage, and behavior are identical; only which
`.o` a symbol lands in changes. New TUs auto-join the DX12/Standalone build via the existing
`file(GLOB_RECURSE CORE_SOURCES …)` (Source/Core/CMakeLists.txt); the `tests/` target list is checked
per-slice (no test references the moved symbols → no test edit). Any file-local
(anon-namespace) helper used *exclusively* by the moved cluster moves with it, verified by
whole-tree grep before the cut.

## Slices

- **Slice 1 — MCP client-activity plumbing (SHIPPED, PR #1660).** Extract the whole
  `#if defined(SMATCHET_WITH_MCP)` cluster — the anon `PrefixMcpActivityLine` helper plus
  `AppendMcpActivity` / `CopyMcpActivityLog` / `NotifyMcpClientHttpActivity` /
  `GetMcpHttpTrafficEpoch` / `TryGetMcpLastClientHttpActivity` — into
  `AppController_McpActivity.cpp`. Anon helper confirmed cluster-private (2 grep hits, both
  moved). `AppController.cpp` 1519 → 1427 LOC. No CMake/test edit.

- **Slice 2 — Lua-script-file handling (SHIPPED, PR #1742).** Extract `ResolveLuaScriptPath` /
  `ListLuaScriptFiles` / `GetAutomationScriptContent` / `SaveAutomationScriptContent` into
  `AppController_LuaScriptFiles.cpp`. The four are topically cohesive but not contiguous
  (two around former lines 1007–1109, two around 1188–1230, with the field-icon path
  resolver between — that stays). Helper census: the bodies reference no anon-namespace or
  static file-local helper, so nothing else moves; the cluster is compiled unconditionally
  (not Lua-gated — matches the pre-move layout and the `AppController_LuaStubs.cpp` note).
  `AppController.cpp` 1442 → 1294 LOC. No CMake/test edit.

- **Slice 3 — AI-context cluster (SHIPPED, PR #1743, squash `2c580c79`).** Extract `AddAiContext` / `ClearAiContext` /
  `GetAiContext` / `PromptAi` into `AppController_AiContext.cpp`. The four are contiguous
  and always-on (unconditional signatures; each body internally guards its
  `impl_->aiAssistant_` delegation with `SMATCHET_WITH_AI` and no-ops otherwise —
  reproduced byte-identically, including the `#else (void)param;` tails, so the light
  build compiles the same bodies with the guarded branches dropped). Helper census: the
  bodies reference no anon-namespace or static file-local helper (only Impl's
  `aiAssistant_`, `GetGlobalAiAssistantUiState()`, and `LOG_WARN`), so nothing else
  moves. Unlike slice 2 this TU dereferences the pImpl, so it includes
  `AppControllerImpl.h`. `AppController.cpp` 1297 → 1237 LOC. No CMake/test edit.

- **Slice 4 — host-integration cluster (SHIPPED, PR #1749, squash `a2ae5033`).** Extract the contiguous former-line
  693–865 span — the host-callback setters an embedding shell registers plus the actions
  that invoke them (embedded-UI close, app-quit request, URL open, automation sink
  registration, scripting-window request consume, attachment handler setters, and the
  file-open-dialog request) — into `AppController_HostIntegration.cpp`. Platform gating:
  the URL-open body carries the pre-existing `_WIN32` / `__APPLE__` / else branches
  (system shell-open vs no-shell launcher), reproduced byte-identically along with the
  platform include block; no winsock preamble (the TU pulls no cpr/curl header). Helper
  census: the file-local no-shell launch helper is used only by the URL-open fallback,
  so it moves with the cluster; everything else the bodies touch is header-provided
  (host-callback members, log truncation). The automation sink delegators dereference
  the pImpl, so the TU includes `AppControllerImpl.h` and completes the Lua automation
  host unconditionally (the slice-3 lesson). `AppController.cpp` 1237 → 1023 LOC. No
  CMake/test edit. Remaining candidate clusters (not designed here): the ticket-prefetch
  block, the field-icon path resolver plus its two file-local helpers, and the
  local-cache database block with its db-file removal helper.

- **Slice 5 — ticket-prefetch cluster (SHIPPED, PR #1754, squash `b6a33c47`; follow-up in-flight-key leak fix PR #1757, squash `9ed93d33`).** Extract the contiguous former-line
  417–548 span — `PrefetchIssueTicketsForKeys` / `FetchAndCachePrefetchedTickets` /
  `IsBulkImportPrefetchInFlight` — into `AppController_TicketPrefetch.cpp`. Cohesion: the
  bulk-import prefetch subsystem dedupes requested keys against the in-flight set, launches
  a background worker, fetches and caches the tickets off the UI thread, then clears the
  in-flight set. Census: the bodies never dereference the pImpl (slice-2-style extraction —
  no `AppControllerImpl.h`, no `LuaAutomationHost.h`); reference none of the anon-namespace
  helpers (`RemoveLocalCacheDbFiles`, `g_TrackerIssueFetchMutex`,
  `FieldIconHasCaseInsensitivePrefix`, `FieldIconPathIsAllowed` all stay); no platform
  gating in the span. In-flight state (`bulkImportPrefetchKeysMutex_` /
  `bulkImportPrefetchKeysInFlight_`) stays in the class — the moved code only references it.
  Curated includes: `AppController.h` (with the deviation block — it transitively completes
  `ITrackerBackend`, `ITrackerIssueReader`, `TrackerConfig`/`ViewsStore`, `CachedTicket`,
  `GridLiveContext`), `LocalCacheManager.h` (the fetch-and-cache body calls `Cache->`),
  `TrackerHttpPure.h` (cpr-free `IsTrackerTransportErrorText`, not the curl-heavy
  `TrackerHttpUtils.h`), `ConfigManager.h`, `Logger.h`, plus `<atomic>` (the worker latches
  `Backend` via `std::atomic_load`), `<memory>`, `<mutex>`, `<string>`, `<unordered_set>`,
  `<utility>`, `<vector>`. `AppController.cpp` 1023 → 890 LOC. No CMake/test edit. Remaining
  candidate clusters after this slice: the local-cache database block with its db-file
  removal helper (~140 LOC), and the field-icon path resolver plus its two file-local
  helpers (~78 LOC).

- **Slice 6 — field-icon path resolver (SHIPPED, PR #1763, squash `718205df`).** Extract `ResolveFieldIconAssetPath`
  plus its two file-local helpers (`FieldIconHasCaseInsensitivePrefix` /
  `FieldIconPathIsAllowed`, the latter calling the former) into
  `AppController_FieldIconPath.cpp`. Helper census: whole-tree grep confirms both helpers
  are used exclusively by this resolver (zero callers outside `AppController.cpp`; inside
  it only the resolver and each other), so they move with it in a fresh anonymous
  namespace. Split-block note: the spec's second anon-namespace block was NOT
  helper-exclusive — it also declared `std::mutex g_TrackerIssueFetchMutex;`, which
  `FetchIssuesForActiveView` still locks, so the mutex STAYS behind in its own anon
  namespace (it belongs to the future local-cache/fetch slice) and only the two helpers
  moved. Census: the resolver never dereferences the pImpl (slice-2-style extraction — no
  `AppControllerImpl.h`, no `LuaAutomationHost.h`); no platform gating in either part.
  Member state (`luaScriptsDirectory_`, `kFieldIconAssetPathCacheCap`, the mutable memo
  `fieldIconAssetPathCache_`) stays in the class — the moved code references it through
  `this`. Curated includes: `AppController.h` (with the fan-in deviation block),
  `ConfigManager.h` (`ConfigManager::GetRuntimeAssetDirectory`), `StringUtil.h`
  (`TrimCopyAsciiWhitespace`), `ghc/filesystem.hpp`, `<string>`, `<system_error>` (no
  `Logger.h` — the bodies log nothing). `AppController.cpp` 893 → 742 LOC. No CMake/test
  edit. This crosses the plan's ≤ ~800 LOC target (742 < 800). The last obvious remaining
  cluster is the local-cache database block plus its `RemoveLocalCacheDbFiles` helper
  (~140 LOC, still in the first anon namespace).

- **Slice 7 — local-cache database block (SHIPPED, PR #1765, squash `776d1b83`).** Extract the file-local
  `RemoveLocalCacheDbFiles` helper (the whole first anonymous-namespace block) plus the three
  contiguous methods `GetResolvedLocalCacheDbPath` / `RecreateLocalCacheDatabase` /
  `EnsureLocalCacheForUiTest` into `AppController_LocalCacheDb.cpp`. Helper census: whole-tree
  grep confirms `RemoveLocalCacheDbFiles` has exactly one caller — `RecreateLocalCacheDatabase`
  (definition + one call site, both formerly in `AppController.cpp`) — so it moves with the
  cluster in a fresh anonymous namespace. The first anon block held ONLY that helper (verified);
  the SECOND anon block (`g_TrackerIssueFetchMutex`, locked by the staying
  `FetchIssuesForActiveView`, left behind by slice 6) STAYS untouched. `LoadAiChatMessages` sits
  just above `GetResolvedLocalCacheDbPath` and STAYS. pImpl census: none of the three methods
  dereferences `impl_->` (slice-2-style extraction — no `AppControllerImpl.h`, no
  `LuaAutomationHost.h`); they touch only class members and free functions. Gating census: none
  of the three carries a `#if`/`#else` build guard — all compile unconditionally
  (`EnsureLocalCacheForUiTest` opens a `:memory:` cache but is guarded at runtime by the bucket-E
  opt-in env var, not a build flag), reproduced byte-identically. External callers
  (`AppController_Init.cpp`, `AppController_LuaBindings_Tickets.cpp`, `UiTestScenario.cpp`,
  `BuiltinCommands_Sync.cpp`, `SmatchetPreferencesUi_Local.cpp`) call the public declarations in
  `AppController.h` — unaffected by the TU split (none `#include`s the `.cpp`). Curated includes:
  `AppController.h` (with the fan-in deviation block — transitively completes `TrackerConfig`,
  `GridLiveContext`/`CacheBackendKeyCopy`, `offlineQueue_`), `ConfigManager.h`
  (`ConfigManager::Load`/`Save`/`InvalidateCache`, `TrackerConfig`), `LocalCacheManager.h`
  (constructs `LocalCacheManager`, calls `Cache->` methods), `Logger.h` (`LOG_*`),
  `ghc/filesystem.hpp` (`fs::path` in the helper + path resolver), plus `<exception>`
  (`std::exception`), `<memory>` (`std::make_unique`), `<string>`, `<system_error>`
  (`std::error_code`). `AppController.cpp` 742 → 571 LOC. No CMake/test edit. This is the LAST
  cohesive extractable cluster — after slice 7, `AppController.cpp` has no further
  obviously-cohesive block to peel off and the plan is ready to move to `shipped`.

## Verification

- **Build gate (THE real verification here)**: CI Windows+MSVC full build (DX12 + Standalone +
  light) + ctest + bucket UI lanes must be green. Local compile is impossible in this container
  (see § Context). § Verification (actual) is populated post-ship per slice.
- Pre-push: `test-lint-rules.sh --diff origin/develop` (strict-zone / comment-noise) and
  `dup_audit.py --diff` clean; behavior-preserving move → `tests-out-of-band` (no runtime
  surface to add an assertion against).

## Out of scope

- **`AppController.h` (1465 LOC) split** — separate follow-up plan; not clustered here.
- **`GridContextDepsAdapter` friend-drop** — DEFERRED per the shipped plan (wide ownership /
  lifetime change this container cannot validate).

## Implementation log

- Slice 1 — PR #1660 (`17ea3235`) · MCP client-activity → `AppController_McpActivity.cpp`;
  `AppController.cpp` 1519 → 1427 LOC.
- Slice 2 — PR #1742 (`1461bee3`) · Lua-script-file handling → `AppController_LuaScriptFiles.cpp`;
  `AppController.cpp` 1442 → 1294 LOC.
- Slice 3 — PR #1743 (`2c580c79`) · AI-context cluster → `AppController_AiContext.cpp`;
  `AppController.cpp` 1297 → 1237 LOC.
- Slice 4 — PR #1749 (`a2ae5033`) · host-integration cluster → `AppController_HostIntegration.cpp`;
  `AppController.cpp` 1237 → 1023 LOC.
- Slice 5 — PR #1754 (`b6a33c47`) · ticket-prefetch cluster → `AppController_TicketPrefetch.cpp`;
  `AppController.cpp` 1023 → 890 LOC. Follow-up leak fix PR #1757 (`9ed93d33`) — clear
  in-flight prefetch keys on all exit paths of `FetchAndCachePrefetchedTickets`.
- Slice 6 — PR #1763 (`718205df`) · field-icon path resolver + its two file-local helpers →
  `AppController_FieldIconPath.cpp`; `AppController.cpp` 893 → 742 LOC. `g_TrackerIssueFetchMutex`
  (co-resident in the same source anon block but locked by the staying `FetchIssuesForActiveView`)
  kept behind in its own anon namespace. Crosses the ≤ ~800 LOC target.
- Slice 7 — PR #1765 · local-cache database block (`RemoveLocalCacheDbFiles` helper +
  `GetResolvedLocalCacheDbPath` / `RecreateLocalCacheDatabase` / `EnsureLocalCacheForUiTest`) →
  `AppController_LocalCacheDb.cpp`; `AppController.cpp` 742 → 571 LOC. Helper `RemoveLocalCacheDbFiles`
  cluster-private (2 grep hits, both moved). No `impl_->` in the moved bodies (slice-2-style — no
  `AppControllerImpl.h` / `LuaAutomationHost.h`); no build gating. `g_TrackerIssueFetchMutex`
  (second anon block) and `LoadAiChatMessages` left in place. LAST cohesive cluster — plan ready to
  move to `shipped` after this slice.

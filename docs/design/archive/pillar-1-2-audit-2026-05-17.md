# Pillar 1+2 audit — 2026-05-17
<!-- index-summary: Whole-codebase Pillar 1 (≤6.94 ms / ≤16.67 ms p99) + Pillar 2 (no sync I/O on UI thread) audit. 9 CRITICAL + 3 HIGH + 3 MEDIUM findings shipped via PR #190, PR #191, bundle PR `feat/h12-l16-m13-bundle`. H10/M14/L17 accepted no-fix; P2/P3/P4 watch-list (triggers documented in plan). -->

Read-only audit of all `Source_Core/` + `Plugins/` reachable from ImGui frame stacks, looking for Pillar 1 (≤ 6.94 ms mean / ≤ 16.67 ms p99) and Pillar 2 (no sync I/O on UI thread, > 100 ms work moves to worker) violations.

**Trigger**: PR #186 (`perf(grid): move cell-edit HTTP off the UI thread`) surfaced one CRITICAL violation by code-reading. User asked for a whole-codebase scan.

**Methodology**: exhaustive grep across `cpr::*`, `SQLite::*`, `std::ifstream` / `ofstream`, `ghc::filesystem::*`, `sol::protected_function_result`, `.join()`, `condition_variable::wait`, `.lock()`. For each match: open the function, walk up to verify call-graph reach from `SmatchetUI::Draw` / `*::Render` / context menus / button handlers. Worker hand-off validated by `std::async`, `LaunchBackgroundTask`, `std::thread(...).detach()`, or `MainThreadDispatcher::PostToMainThread` in between.

## Summary

| Bucket | Count |
|---|---|
| Pillar 2 CRITICAL (sync HTTP/IO reachable from ImGui frame, no worker hand-off) | **9** |
| Pillar 2 HIGH (sync IO via Modal/ContextMenu, blocks for seconds when fired) | 3 |
| Pillar 2 MEDIUM (one-shot per-click sync IO) | 3 |
| Pillar 2 LOW / observational | 2 |
| Pillar 1 HIGH (mean-frame impact) | 1 |
| Pillar 1 MEDIUM (p99 spike contributor) | 3 |

## Canonical fix pattern

Reference: `Source_Core/src/SmatchetGridFieldEditPipeline.cpp:105-141` (post-PR-#186).

- Enqueue from UI thread (cheap struct push)
- Dispatch via `AppController::LaunchBackgroundTask` (public since PR #186)
- Worker calls the blocking op (HTTP / disk / etc.)
- Completion lambda posted via `mainThreadDispatcher.PostToMainThread([captured-by-value]{ /* touch UI state */ });`
- Cancel atom (`std::shared_ptr<std::atomic<bool>>`) when the work can outlive the editor (per Phase B `assistantTurnGen` precedent)

## Pillar 2 — CRITICAL (9 findings)

### 1. Priority icon fetch in grid render

- **`Source_Core/src/SmatchetFieldIconRender.cpp:186`** — `HttpGetBinary` → `cpr::Get` (3s timeout)
- Path: `SmatchetUI::Draw → SmatchetActiveProjectGridUi::Render → TicketFieldEditor::RenderFieldCell:852 → SmatchetFieldIconRender::TryDrawFieldValueIcon:485 → LoadPriorityIconWithFallbacks:316 → LoadTextureForResolvedPath:248 → LoadOrFetchUrlImage:203 → HttpGetBinary:186`
- Trigger: any grid cell with priority icon that misses the URL-key cache AND the disk cache. Memoized after first success, but first frame for each new value stalls up to 3 s.
- Fix: dispatch fetch via `LaunchBackgroundTask`; store "loading" sentinel in memo to suppress re-issue; post texture back via `MainThreadDispatcher::PostToMainThread`.

### 2. Attachment preview download on gallery open

- **`Source_Core/src/SmatchetAttachmentPreviewUi.cpp:388`** — `app.DownloadAttachmentForPreview` (50 MB max, 120 s timeout)
- Trigger sites: line 388 (single fetch), 520 (eager preload of 6 entries on gallery open), 609 (per-frame queue for adjacent entries)
- Path: `SmatchetUI::Draw → DrawAttachmentPreviewWindow → QueuePriorityAttachmentPreviewRequests → DownloadAttachmentForPreview`
- Fix: convert handler to enqueue worker fetch; post `AttachmentPreviewUpdate` via `MainThreadDispatcher` so the per-frame call becomes a no-op once `PreviewRequestIssued = true`.

### 3. JQL @-mention autocomplete

- **`Source_Core/src/SmatchetAutocompleteUi.cpp:359`** — `app.SearchUsersByQuery` (cpr::Get)
- Path: `SmatchetUI::Draw → JQL field autocomplete (per frame) → DispatchAsyncUserSearchTickIfDue:359 → cpr::Get`
- Trigger: every JQL @-mention search debounce (220 ms after typing stops).
- Fix: `std::async(std::launch::async, [...]{ … })`; poll future on subsequent frames; existing `d.jqlAcpUserSearchFireAt` already supports the "scheduled work" pattern.

### 4. Grid context-menu quick-comment

- **`Source_Core/src/SmatchetGridUiSupport.cpp:194`** — `app->AddIssueCommentPlain` (cpr::Post)
- Trigger: user clicks a quick-comment template in the grid right-click menu.
- Latency: typical Jira POST 200-2000 ms; pathological = 30 s timeout.
- Fix: launch worker; toast on completion.

### 5. Blame profile open (sequential HTTP)

- **`Source_Core/src/BlameAnalysisUi_Modals.cpp:300`** — `app.SearchUsersByQuery`
- **`Source_Core/src/BlameAnalysisUi_Modals.cpp:313`** — `app.FetchUserGroupNames` (fires immediately after #5)
- Path: `SmatchetUI::Draw → BlameAnalysisUi::Render → blame-row click → OpenTrackerUserProfileForP4User → SearchUsersByQuery + FetchUserGroupNames` (back-to-back)

### 6. Blame assign modal prepare

- **`Source_Core/src/BlameAnalysisUi_Modals.cpp:330`** — `app.SearchUsersByQuery`
- Path: `BlameAnalysisUi::Render → right-click row → PrepareAssignModal:319 → SearchUsersByQuery:330`

### 7. Blame assign modal commit

- **`Source_Core/src/BlameAnalysisUi_Window.cpp:795`** — `app.AddIssueCommentBlameContext`
- **`Source_Core/src/BlameAnalysisUi_Window.cpp:825`** — `app.AddIssueCommentPlain`
- **`Source_Core/src/BlameAnalysisUi_Window.cpp:855`** — `app.SubmitFieldEdit`
- Trigger: blame assign-modal selectable click → 1-3 sequential cpr::Post calls.
- Fix for 5/6/7: `LaunchBackgroundTask` per click; show spinner in `State().lastUiStatus`; post result back.

### 8. Installer update download

- **`Source_Core/src/SmatchetUI.cpp:172`** — `app.DownloadAndLaunchInstallerUpdate` → `AppController.cpp:1007` `cpr::Get` (120 s timeout, full installer download)
- Path: `DrawAppUpdateModal:138 → "Download and Install" button:170 → DownloadAndLaunchInstallerUpdate:974 → cpr::Get:1007`
- Trigger: user clicks "Download and Install" in the update modal — UI freezes for entire download (tens of seconds typical).
- Fix: dispatch via `std::async`, poll, render progress bar in modal.

### 9. Quick-comment template (alternative path)

Counted as part of #4 (same `AddIssueCommentPlain` site reached from different menu entries). Total CRITICAL = 9 distinct call paths.

## Pillar 2 — HIGH (3 findings)

### H10. Win32 file-picker modal

- **`Source_Core/src/SmatchetUI.cpp:356` + `Source_Core/src/Win32PickFiles.cpp:106`** — `dialog->Show()` blocks UI thread until user dismisses.
- System-modal — gives its own UI cue. After dialog returns, `ConfigManager::Save(g_ui.cfg)` fires (sync JSON write).
- Recommended: leave the dialog itself (acceptable per visual-cue contract); consider posting the file-pick on a worker thread if the dialog ever launches slowly.

### H11. `ConfigManager::Save(d.cfg)` flooding from Preferences

- **`Source_Core/src/SmatchetPreferencesUi.cpp`** — 30+ call sites
- Trigger: every settings toggle. Each Save = JSON dump-4 + ScopedFileLock + parent dir check + write temp + flush + rename. Typically 1-5 ms; pathological on slow disk 50+ ms.
- Cumulative not single-spike — death by a thousand cuts.
- Fix: coalesce — `d.cfgDirty = true` flag, save once at end of frame (or 500 ms debounce). See also Pillar 1 P1 below.

### H12. Bulk-tickets file read/write

- **`Source_Core/src/SmatchetBulkTicketsUi.cpp:150, 551`** — `ReadEntireFile` / `WriteEntireFile`
- Trigger: button click; one-shot per user action. File could be megabytes (bulk import of 1000s of tickets).
- Fix: `std::async` with future poll; display "Loading…" while in flight.

## Pillar 2 — MEDIUM (3 findings)

### M13. Long-text editor open

- **`Source_Core/src/TicketFieldEditor.cpp:185, 216`** — `nlohmann::json::parse` of rich-text in `OpenLongTextEditor`
- One-shot when user clicks a long-text cell. CPU-bound parse + ADF→Markdown convert. Annoying only on huge descriptions.

### M14. App-update check (observational)

- **`Source_Core/src/AppController.cpp:882`** — `CheckForAppUpdate` (cpr::Get + json::parse)
- **Safe** today: only called via `StartAppUpdateCheckAsync:72` (wrapped in `std::async`) and the deferred lambda at `SmatchetPreferencesUi.cpp:1348` (also async). No direct UI-thread call site found.
- Track as observational — any future synchronous caller would be CRITICAL.

### M15. Disk-cached icon read

- **`Source_Core/src/SmatchetFieldIconRender.cpp:225-244`** — `fs::exists`, `fs::file_size`, `std::ifstream` of disk-cached icon bytes
- Same path as #1, on the disk-cache-hit branch (memo missed, disk hit).
- Trigger: first frame after app restart for each priority icon; disk read of <512 KB.
- Fix: bundle with #1 into the same off-thread fetch.

## Pillar 2 — LOW / observational (2)

### L16. Image texture cache disk read

- **`Source_Core/src/SmatchetImageTextureCache.cpp:244`** — `std::ifstream` in `GetOrLoadFromFile`. First-touch only, memoized after.

### L17. Available-fields mutex

- **`Source_Core/src/AppController_CatalogAndFieldEdit.cpp:289`** — `FindFieldById` acquires `availableFieldsMutex_`. Ubiquitous from grid render. Mutex held briefly during `SetFieldCatalog`'s vector move (line 256-260) — short critical section. Currently SAFE; flagged so any future expansion of the locked region gets pushed back.

## Pillar 1 — HIGH (1 finding)

### P1. `ConfigManager::Save` cache invalidation cascade

Same 30+ Preferences call sites as H11 above. The Save call invalidates the `ConfigManager::Load()` cache (line 168-170). Next call site that reads `ConfigManager::Load()` (some on the same frame) re-parses the merged JSON file from disk (`LoadMergedConfigJson:603` reads `defaults` + `user` files).

Worst case: dragging a slider in Preferences fires Save every frame → cache invalidated every frame → Load on next frame re-parses ~5 KB JSON.

Fix: same as H11 — coalesce Save. Optionally, debounce cache invalidation.

## Pillar 1 — MEDIUM (3 findings)

### P2. Lua field-cell provider on cache miss

- **`Source_Core/src/AppController_LuaBindings.cpp:1841-1923`** — `TryRenderCachedLuaField`
- Lua provider runs on UI thread. Bounded by instruction-count hook, but wall-time unbounded — a provider that calls back into expensive C++ (catalog rebuild, image fetch) creates a spike. Cache absorbs most calls; miss after value-change in a grid sort/refilter can fire N misses in one frame.
- Recommendation: enable `perf_temp:LuaDrawList::Record` on a long capture (already scoped at line 1886); if `maxPerCallMs` exceeds 1 ms, document that Lua providers must stay pure.

### P3. O(N²) candidate de-dup in icon fallbacks

- **`Source_Core/src/SmatchetFieldIconRender.cpp:327-329`** — `std::any_of` over `candidates` for each `pushUnique`. N is tiny (3-5) so negligible today. Same shape in `JiraUserAndMeta` and `FieldCatalogCache` deduping logic — if those ever grow to N > 50, watch.

### P4. JSON pretty-print in config save

- **`Source_Core/src/ConfigManager_PathUtils.cpp:634`** — `nlohmann::json::dump(4)` in `ConfigManager::WriteConfigJson`. Pretty-print + indent scales with full config size. Currently ~5 KB; trivial. Will degrade silently as config grows.

## Excluded from findings (already-async paths, no fix needed)

- `SmatchetUI.cpp:75` — `StartAppUpdateCheckAsync` wraps `cpr::Get` in `std::async`
- `SmatchetPreferencesUi.cpp:1348` — same
- `SmatchetNewIssueDraftUi.cpp:223` — `std::thread([&app, ...]{ app.RefreshFieldCatalog(...); }).detach()`
- `TrackerGridFieldDisplay.cpp:695, 749` — Watchers / Votes via `std::async`
- `AppController_Connectivity.cpp:153` — probe via `std::async`
- `AppController_IssueCreateOffline.cpp:94` — `CreateIssueAsync` returns `std::future`

## Out of scope per audit mandate

- `SmatchetGridFieldEditPipeline.cpp` — PR #186 fix confirmed correct, used as reference pattern
- `AiAssistantController.cpp` — Phase B pattern confirmed correct
- `tests/` — off-UI-thread by definition
- `Logger.cpp:130` — worker; UI submission is push-to-bounded-queue under short mutex, safe
- `TicketSyncService` / `OfflineQueueService` / `BackendAuditTrail` — already on workers
- `BlameAnalysisUi_Worker` — worker-thread by design; only `_Modals` / `_Window` flagged
- `Plugins/Mcp/McpPlugin.cpp:296` — `cpr::Get` runs in cpp-httplib server worker thread, not UI

## Disposition

### Shipped 2026-05-17

- **CRITICAL findings #1–9** (all 9 sync HTTP/IO call paths) — PR [#191](https://github.com/alexandrosk0/Smatchet/pull/191) at sha `8b779bc`. Worker dispatch via `LaunchBackgroundTask` + `MainThreadDispatcher::PostToMainThread`, cancel atoms on installer download path.
- **M15** (disk-cached icon read) — bundled into PR #191 via `LoadTextureForResolvedPath` refactor that defers disk reads off the icon-render path.
- **H11 + P1** (`ConfigManager::Save` cascade + cache invalidation) — PR [#190](https://github.com/alexandrosk0/Smatchet/pull/190) at sha `a3298ca`. 31 per-widget Save sites → `MarkPrefsDirty(d)` + 100 ms end-of-frame debounce. Final sync Save on shutdown via `DrainUiDrawSessionFuturesBeforeAppTeardown`.
- **H12 + L16 + M13** (bulk-tickets file r/w, image-texture-cache file reads, long-text editor JSON parse) — PR `feat/h12-l16-m13-bundle`. H12: Load file + Save to file buttons disable while a worker reads/writes, button label flips to "Loading..." / "Saving..."; modal-close flips a cancel atom so the worker's post-back bails. L16: URL-disk-cache-hit branch and file-path branch in `SmatchetFieldIconRender.cpp` now dispatch via `LaunchBackgroundTask`; `IconFileReadInFlightSet` mirrors the URL-fetch suppression set; worker reads bytes, posts back to UI thread for GPU upload via `GetOrLoadFromMemory`. M13: threshold-gated — rich values ≤ 32 KB stay on the sync path (no UX regression for normal Jira descriptions); above the cutoff, `nlohmann::json::parse` + `MarkdownConvert::AdfToMarkdown` run on a worker, the modal shows a placeholder "Loading description..." banner until a generation-checked post-back swaps the real seed in.

### Accepted — no fix needed

- **H10** Win32 file picker — system-modal supplies its own visual cue per Pillar-2 envelope.
- **M14** app-update check — every call site already wrapped in `std::async`; observational only.
- **L17** available-fields mutex — critical section is short (vector move under `SetFieldCatalog`); observational only.

### Watch list — re-evaluate when triggers fire

The three items below are below the merge-block bar today (small N, small payloads). They stay tracked so a future scenario crossing the trigger condition gets caught by the next audit pass.

- **P2** Lua field-cell provider spike. **Trigger**: `perf_temp:LuaDrawList::Record` capture (already scoped at `AppController_LuaBindings.cpp:1886`) shows `maxPerCallMs > 1.0` on any representative scenario. **Action when fired**: document in the Lua bindings contract that providers must stay pure (no catalog rebuild / image fetch / blocking C++ callbacks); pathological providers move to a worker via the same Pattern A. **Why not now**: cache absorbs almost all calls; current N misses on sort/refilter are sub-millisecond.
- **P3** O(N²) icon-fallback dedup (`SmatchetFieldIconRender.cpp:327-329`). **Trigger**: a future grid view ever holds **> 50 distinct priority candidates per cell**, OR the same shape appears in `JiraUserAndMeta` / `FieldCatalogCache` and N there crosses 50. **Action**: switch `std::any_of` over `candidates` to `std::unordered_set<std::string>` membership check. **Why not now**: N ≤ 5 today; the constant-factor win of `std::any_of` on tiny vectors beats hash-set setup.
- **P4** JSON `dump(4)` in config save (`ConfigManager_PathUtils.cpp:634`). **Trigger**: `smatchet_config.json` file size **> 64 KB** measured on disk in any real install. **Action**: switch the hot-write path to `dump()` (no indent); keep `dump(4)` for the Settings → Export flow where pretty-print is user-visible. **Why not now**: current config is ~5 KB; pretty-print cost is sub-millisecond.

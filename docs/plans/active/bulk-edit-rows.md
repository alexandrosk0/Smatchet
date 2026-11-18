# Plan — Bulk-edit one field across selected grid rows

> **Slug**: `bulk-edit-rows` (matches this file's basename without `.md`).
>
> **Status**: `active` — driving the three bulk-edit PRs (P1 → P2 → P3).
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

The user asked for a way "to edit multiple rows at once, like changing the assignee in 5 rows." Today the ticket grid already supports multi-row selection (shift / ctrl / drag on the ID column, Ctrl+A), but every edit is one cell on one ticket, so a five-row reassignment is five separate edits.

After this lands: the user selects N rows, right-clicks any cell of a column, picks "Set <Field> for N rows…", chooses a value once, and every selected ticket is updated. Each row is saved, queued offline, audited, and reported on its own. A summary toast says how many rows were saved, queued, failed, or skipped.

Design decisions were taken by the user on 2026-11-14 (architect options A/B/C; the user picked B, the context-menu picker):

| # | Decision | User's pick |
|---|---|---|
| D1 | Entry point | Context-menu picker: right-click a cell of a selected row → "Set <F> for N rows…" |
| D2 | Field types | All of them: select / user / priority, status transitions, labels / components with add / remove, and text / date / number |
| D3 | Multi-value mode | Replace / Add / Remove (follows from D2) |
| D4 + D8 | Guardrails | Pre-check every row and skip non-editable ones, listing them; confirm when N > 10; a disallowed value still fails per row with its backend message |
| D5 | Selection after the edit | Keep the selection by issue id across re-sorts |
| D6 | Undo | Best-effort Revert: re-apply each row's captured before-value as a new bulk job |
| D7 | Throughput | Up to 4 distinct issues saving in parallel, plus Jira Cloud's native bulk-edit API when online |
| — | Phasing | Three PRs (P1 → P2 → P3), one plan |
| — | Command name | `ticket.set_field_bulk` and `ticket.revert_bulk`, keeping every write under `ticket.*` (grill, 2026-11-14) |

Origin: the "Review other sessions" orchestrator brief for session `bulk-edit-rows`. Sibling work: PR #2432 (`docs/plans/shared-dropdown-recents.md`), which this plan builds on.

## Approach

**Shared job core.** A new pure header `BulkFieldEditPure.h` defines `BulkFieldEditJob`. When the job starts, it snapshots everything it needs: the selected **issue ids** (resolved from sort-order rows through `ResolveGridRowsToTicketIndices`, because sort-order indices stop meaning anything once the first edit lands and bumps `snapshotRevision`), the field, the values, the mode (Replace / Add / Remove), the pane's `PendingActionTarget`, each row's before-value, and a per-row state (`Pending / Saving / Saved / Queued / Failed(msg) / Skipped(reason)`).

The job **fans out** into one ordinary `PendingFieldEdit` per row, tagged with a new `BulkJobId`, and pushes them into the existing `queuedFieldEdits`. The existing pump (`PumpGridFieldEdits`) and the existing commit path then handle each row: `FieldEditPipelineService::CommitOrQueue` → `RouteWrite`, the editmeta retry, the offline queue, ADR-0016 conflict replay, the `field_edit_diff` audit, and the cell badges. Nothing adds a second commit branch, which the Ui `AGENTS.md` grid invariant forbids. `ApplyCommitResultOnUiThread` **folds** each row result back into the job by `BulkJobId`. When the last row lands, the job pushes one summary toast. Queued rows count as "queued", never as "failed". Per `Source/Core/src/Ui/AGENTS.md` § Toasts say what happened, "queued" is reported only for rows whose `CommitOrQueue` actually enqueued, and the toast reads "Saving k / N…" while rows are in flight.

**Entry.** `DrawGridCellRightClickPopups` gains a "Set <Field> for N rows…" item whenever the clicked row is inside a selection of two or more rows. `GridKeyCellMenuCtx` is widened so data cells receive the selection and the column's `TrackerField` too. Today only the key cell receives it. The item opens a new popup, `SmatchetBulkFieldEditPopup`. For select, user and priority fields it calls `smatchet::dropdown::DrawBody` from #2432 with `RecentsScope = MakeScopeKey(<pane cache key>, field.Id)`, so a bulk pick both feeds and reads the same Recent list as single-cell edits. The header shows "N rows · mixed (k values)", or "N rows · <value>" when every row holds the same value. Above 10 rows, a confirm step ("Apply to N rows? K will be skipped") runs before fan-out.

**Phase 3 throughput.** The pump's single `hasInFlightEdit` gate becomes a bounded in-flight set of up to 4 rows. It never admits two edits for the same (backend, issue), so the order of edits per issue is unchanged. For an online Jira Cloud backend, a job with no Add/Remove on rich or ADF fields is sent instead as one `POST /rest/api/3/bulk/issues/fields` request, and the returned task is polled on a worker. If that request fails with a retryable error or the backend goes offline, the job falls back to per-row fan-out, so offline-first (Pillar 6) still holds: the fast path is an optimisation that never replaces the queue.

**Commands.** `ticket.set_field_bulk` exposes the same job core to the palette, CLI, MCP and Lua through the registry, so no sol2 binding is needed. To respect Pillar 2, the handler never loops N synchronous `SubmitFieldEditOrQueue` calls on the dispatching thread. When it is dispatched on the UI thread (the palette, a Lua ticket action) it runs the rows on a tracked background worker, returns `{started, rows, field}` at once and shows the totals as a toast. On any other thread (CLI, MCP) it runs the rows on the calling thread and returns the per-row results. The split is decided by `IsOnUiThread()` (§ Open questions Q1).

## Interface contracts

- **`PendingFieldEdit`** (`Source/Core/include/Ui/SmatchetUiSession.h:115`): add `std::uint64_t BulkJobId = 0;`, where 0 means "not part of a bulk job". It is not persisted to the offline queue. A replayed row reports through the normal replay path, not the job.
- **`UiDrawSession`** (`SmatchetUiSession.h`, next to `queuedFieldEdits` at :1072): add `std::map<std::uint64_t, BulkFieldEditJob> bulkFieldEditJobs;` and `std::uint64_t nextBulkJobId = 1;`. These are in-memory only.
- **`GridRectSelection`** (`Source/Core/include/SpreadsheetState.h:19-66`, P1 / D5): add `std::set<std::string> KeepIssueIds;`. It is filled when a bulk job starts. After a snapshot-revision `ClearAll()` (`SmatchetActiveProjectGridTable.cpp:1120-1123`), the selection is rebuilt from it against the new sort order, then cleared on the next user selection gesture. The type is in-memory only, with no save-format impact.
- **`GridKeyCellMenuCtx`** (`Source/Core/include/Ui/SmatchetGridUiSupport.h:29`): add `const TrackerField* field = nullptr;` and `const std::string* paneCacheKey = nullptr;`. These are pointers, not owned strings, because `Source/Core/src/Ui/AGENTS.md` § Per-cell grid code allocates nothing per row. The pane builds one ctx before the row loop, and each cell sets only `field` and `clickedRow` (the rest are plain values). Data-cell call sites (`SmatchetActiveProjectGridCells.cpp` :413 / :478 / :496) start passing a ctx. Today only the key cell (:333) does.
- **`BulkFieldEditJob`** (new, `Source/Core/include/Ui/BulkFieldEditPure.h`, namespace `smatchet::grid::bulk`). It is pure, with no ImGui, cpr or SQLite includes:
  ```cpp
  enum class BulkRowState { Pending, Saving, Saved, Queued, Failed, Skipped };
  enum class BulkMode { Replace, Add, Remove };
  struct BulkRow { std::string IssueId; std::string BeforeValue; std::vector<std::string> BeforeValues;
                   BulkRowState State = BulkRowState::Pending; std::string Message; };
  struct BulkFieldEditJob { std::uint64_t Id = 0; TrackerField Field; std::vector<std::string> Values;
                            BulkMode Mode = BulkMode::Replace; std::vector<BulkRow> Rows; bool IsRevert = false; };
  struct BulkSummary { int Saved = 0, Queued = 0, Failed = 0, Skipped = 0; bool Done = false; };
  BulkSummary Summarize(const BulkFieldEditJob&);
  std::vector<std::string> ComputeValuesForRow(BulkMode, const std::vector<std::string>& before,
                                               const std::vector<std::string>& picked); // Add/Remove merge
  std::string MixedValueLabel(const BulkFieldEditJob&);            // "mixed (k values)" / the single value
  bool NeedsConfirm(int rowCount, int threshold /* = 10 */);
  BulkFieldEditJob MakeRevertJob(const BulkFieldEditJob& done, std::uint64_t newId); // P2
  ```
- **Command** (`Source/Core/src/Commands/Builtin/BuiltinCommands_TicketMutations.cpp`): `RegisterCommand({"ticket.set_field_bulk", …})` with the parameters `ids: Json (array of string)`, `field: String`, `value: String` or `values: Json`, `mode?: String (replace|add|remove)` and `dry_run?: Bool`. `Destructive = true`, so it needs `--yes` / `__confirm`, the same as `ticket.set_field`. It returns `{rows:[{id, state, message}], saved, queued, failed, skipped}` (or `{started, rows, field}` from the UI thread), and `dry_run` returns `{wouldDo:{rows:[{id, from, to, skip?}]}}`. P2 adds `ticket.revert_bulk {jobId}`. This is an MCP schema addition only: no existing tool changes shape.
- **`ITrackerIssueMutations`** (`Source/Core/include/ITrackerIssueMutations.h`, P3 only) gains:
  ```cpp
  virtual bool SupportsBulkFieldEdit() const { return false; }
  virtual Result<std::vector<BulkRowResult>, TrackerError>
      BulkUpdateSameFields(const std::vector<std::string>& issueIds, const nlohmann::json& fields) {
      return Result<…>::Err(TrackerError::NotSupported(...)); }
  ```
  Only `JiraClient` overrides it, and only when the deployment is Cloud. `PlaneClient` keeps the default, so the Jira specifics stay inside `src/Tracker/` (Tracker `AGENTS.md`).

**Plumbing, hop by hop (P1):**
1. Right-click on a data cell → `DrawGridCellRightClickPopups(…, keyMenu)` with ctx `{sel, field, paneCacheKey, sortedIdx, tickets}`.
2. Menu item → `ui->bulkEditPopupRequest = {paneId, field, issueIds}`. A new small struct in `SmatchetUiSession.h` holds this request.
3. The popup draws next frame → user pick → `StartBulkFieldEdit(app, ui, pane, field, values, mode)` (new, in `SmatchetGridFieldEditPipeline.cpp`).
4. That runs a pre-flight `app.CanEditFieldForIssue` per id, which is memory-only (verified in P1 slice 1), so it is safe for N calls on the UI thread. For every row it captures the ADR-0016 scalar base: `edit.OriginalValue = ticket.GetFieldValue(field.Id)` and `edit.HasOriginalValue = true`, the same as `QueueEdit` does at `TicketFieldEditor.cpp:380-381`. This lets a queued bulk row that replays after someone else changed the value raise a conflict instead of overwriting it silently. This adds a second capture point next to `QueueEdit`, so P1 slice 1 extracts a shared `CaptureScalarBase(const CachedTicket&, const TrackerField&, PendingFieldEdit&)` into `BulkFieldEditPure` (or a small pure header) and calls it from both places. That keeps ADR-0016's single choke point per kind. It then calls `EnqueueGridFieldEdits(d, edits, readOnly)` with `BulkJobId` set.
5. The existing `PumpGridFieldEdits` → `RunCommitWorker` → `CommitOrQueueFieldEdit` path saves each row.
6. `ApplyCommitResultOnUiThread` → `FoldBulkRowResult(d, edit.BulkJobId, issueId, outcome)` → when `Summarize(job).Done`, push the summary toast.

## Files to modify

**P1 — job core, context menu, choice fields, guardrails, keep-selection, command**

1. `Source/Core/include/Ui/BulkFieldEditPure.h` + `Source/Core/src/Ui/BulkFieldEditPure.cpp` (new): the job model, the Add/Remove merge, the summary, the mixed-value label and the confirm threshold. Pure. Before creating it, `rg -l BulkFieldEdit Source/Core` returned 0 hits.
2. [`Source/Core/include/Ui/SmatchetUiSession.h:115`](../../../Source/Core/include/Ui/SmatchetUiSession.h): `PendingFieldEdit::BulkJobId`, the `bulkFieldEditJobs` map and the popup request struct.
3. [`Source/Core/src/SmatchetGridFieldEditPipeline.cpp`](../../../Source/Core/src/SmatchetGridFieldEditPipeline.cpp): `StartBulkFieldEdit`, the fold in `ApplyCommitResultOnUiThread` (:45), and the summary toast.
4. [`Source/Core/include/Ui/SmatchetGridUiSupport.h:29`](../../../Source/Core/include/Ui/SmatchetGridUiSupport.h) + [`Source/Core/src/Ui/SmatchetGridUiSupport.cpp:205`](../../../Source/Core/src/Ui/SmatchetGridUiSupport.cpp): widen the ctx, add the "Set <F> for N rows…" item, and add the "Select failed rows" helper.
5. [`Source/Core/src/Ui/SmatchetActiveProjectGridCells.cpp`](../../../Source/Core/src/Ui/SmatchetActiveProjectGridCells.cpp): pass the ctx at the data-cell popup call sites (:413 / :478 / :496).
6. `Source/Core/include/Ui/SmatchetBulkFieldEditPopup.h` + `Source/Core/src/Ui/SmatchetBulkFieldEditPopup.cpp` (new): the popup, the `dropdown::DrawBody` picker, the mode toggle for multi-value fields, the confirm step and the skip list.
7. [`Source/Core/src/Ui/SmatchetActiveProjectGridUi.cpp`](../../../Source/Core/src/Ui/SmatchetActiveProjectGridUi.cpp): draw the popup once per pane, at the tail where `LatchPendingActionTargetForPane` is applied (:292).
8. [`Source/Core/include/SpreadsheetState.h:19`](../../../Source/Core/include/SpreadsheetState.h) + [`Source/Core/src/Ui/SmatchetActiveProjectGridTable.cpp:1120`](../../../Source/Core/src/Ui/SmatchetActiveProjectGridTable.cpp): add `KeepIssueIds` and rebuild the selection after a re-sort (D5).
9. [`Source/Core/src/Commands/Builtin/BuiltinCommands_TicketMutations.cpp`](../../../Source/Core/src/Commands/Builtin/BuiltinCommands_TicketMutations.cpp): `ticket.set_field_bulk`.
10a. `docs/guides/cli.md` (Command catalogue), `docs/guides/lua.md` (`commands` module) and `docs/guides/mcp.md` (Built-in Tools): document the new command, as Commands `AGENTS.md` § Keep the user docs in step requires.
10. [`Source/Core/src/SmatchetLocalization.cpp`](../../../Source/Core/src/SmatchetLocalization.cpp): the `grid.bulk.*` keys (menu item, header, confirm, summary, skip reasons).
11. `tests/Core/BulkFieldEditPure.test.cpp` (new) + `tests/CMakeLists.txt`: add it to `add_executable(SmatchetTests …)`.
12. `tests/ui/bulk_field_edit.test.cpp` (new) + `tests/ui/CMakeLists.txt` + `tests/ui/ui_tests_registry.cpp`: the bucket-E test.

**P2 — text / date / number, Revert**

13. `SmatchetBulkFieldEditPopup.cpp`: add text, date and number editors. They reuse the existing small inline inputs, not the grid cell editors, and the `TrackerDateTimeFieldEditor` pure parsing helpers. ADF long-text stays out of scope (§ Out of scope).
14. `BulkFieldEditPure.{h,cpp}`: `MakeRevertJob`. Only rows that reached `Saved` / `Queued` are reverted. Each row's before-value is held as option **ids**, not display names (§ Open questions Q2).
15. `SmatchetGridFieldEditPipeline.cpp` + `SmatchetGridUiSupport.cpp`: add "Revert last bulk edit (N rows)" to the context menu. The summary toast's `RowAction` opens it.
16. `BuiltinCommands_TicketMutations.cpp`: `ticket.revert_bulk {jobId}`.

**P3 — parallel saves, Jira Cloud bulk API**

17. `SmatchetGridFieldEditPipeline.cpp:253` (`PumpGridFieldEdits`): replace `hasInFlightEdit` with a bounded in-flight set (`limits::Get().GridEditMaxInFlight`, default 4) keyed by (backend, issue).
18. `Source/Core/include/ITrackerIssueMutations.h`: add the two defaulted virtuals (§ Interface contracts).
19. `Source/Core/src/Tracker/JiraIssueMutation.cpp` (+ its header): `BulkUpdateSameFields`. POST `bulk/issues/fields`, then poll `bulk/queue/{taskId}` on the worker with a cancel token, and map each failed issue to a `BulkRowResult`.
20. `Source/Core/src/FieldEditPipelineService.cpp` + `include/FieldEditPipelineService.h`: add `CommitBulkSameValueOrQueue(rows, field, values, target)`. It runs `RouteWrite` once for the job, takes the Jira bulk path only when online and `SupportsBulkFieldEdit()`, writes one `field_edit_diff` pair per row, and on a retryable error or offline falls back to the per-row `CommitOrQueue`, which also queues. The grid pipeline calls this service entry from its worker (`SmatchetGridFieldEditPipeline.cpp`). It never calls a tracker client directly: the Ui `AGENTS.md` grid invariant forbids a grid-side commit branch that skips the offline queue and audit. In the same PR, P3 updates that invariant's "one edit in flight" wording to "at most `GridEditMaxInFlight` distinct issues in flight" (`Source/Core/src/Ui/AGENTS.md:63`).

### File-level changes

| File | Symbol | CMake variable |
|---|---|---|
| `Source/Core/include/Ui/SmatchetUiSession.h` | `PendingFieldEdit` | — |
| `Source/Core/src/SmatchetGridFieldEditPipeline.cpp` | `PumpGridFieldEdits` | — |
| `Source/Core/src/SmatchetGridFieldEditPipeline.cpp` | `EnqueueGridFieldEdits` | — |
| `Source/Core/include/Ui/SmatchetGridUiSupport.h` | `GridKeyCellMenuCtx` | — |
| `Source/Core/src/Ui/SmatchetGridUiSupport.cpp` | `DrawGridCellRightClickPopups` | — |
| `Source/Core/src/Ui/SmatchetGridUiSupport.cpp` | `ResolveGridRowsToTicketIndices` | — |
| `Source/Core/include/SpreadsheetState.h` | `GridRectSelection` | — |
| `Source/Core/include/ITrackerIssueMutations.h` | `UpdateIssueFields` | — |
| `tests/CMakeLists.txt` | `SmatchetTests` (the `add_executable` target) | — |

Core `.cpp` files are picked up by `file(GLOB_RECURSE CORE_SOURCES …)`, so no list edit is needed for them. Only the test executables need list edits.

## Existing utilities reused

- `ResolveGridRowsToTicketIndices`: `Source/Core/src/Ui/SmatchetGridUiSupport.cpp:477`. Maps selected sort-order rows to ticket indices, as Bulk Export and copy-as-CSV already do.
- `GridRectSelection::RowSelected` / `HasAnySelection`: `SpreadsheetState.h:48-51`. The menu item shows when the selection holds two or more rows.
- `EnqueueGridFieldEdits` / `PumpGridFieldEdits`: `SmatchetGridFieldEditPipeline.cpp:187` / `:253`. These are the only commit path. Bulk edit adds rows; it is not a new branch.
- `FieldEditPipelineService::CommitOrQueue` / `RouteWrite`: `src/FieldEditPipelineService.cpp:394` / `:414`. Online, offline and retryable routing, the editmeta retry, and the `field_edit_diff` audit, applied per row.
- `OfflineQueueService::QueueFieldEditOffline` / `ReplayOneFieldEdit`: `src/Sync/OfflineQueueService.cpp:621` / `:1268`. Per-row offline queueing, replay and the ADR-0016 conflict gate.
- `LatchPendingActionTargetForPane`: `SmatchetActiveProjectGridUi.cpp:292`. Every row is written to the pane's backend.
- `AppController::CanEditFieldForIssue`: called at `SmatchetActiveProjectGridCells.cpp:481`. The per-row pre-flight check.
- `SmatchetToastManager::PushClickable` + `ToastRowAction`: `include/Ui/SmatchetToast.h`. The summary toast and its click action.
- `smatchet::dropdown::DrawBody` + `Options::RecentsScope`: `include/FilterDropdown.h` on PR #2432's branch. The shared value picker and recent picks.
- `ticket.set_field`: `BuiltinCommands_TicketMutations.cpp:52-97`. The pattern for dry-run, `FindFieldById`, `LatchPendingActionTarget()` and the `{ok, queued, offlineId}` result.
- `FieldEditAuditSource::ScopedOverride`: `include/Persistence/FieldEditAuditSource.h:13`. The command path tags rows `mcp` / `lua`.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — the plan extracts nothing. The new popup TU is new code, not a split. `SmatchetGridFieldEditPipeline.cpp` (352 lines) grows by about 150 lines in P1 and about 120 more in P3. If it passes the project's TU soft cap, P3 moves the bulk launcher into `SmatchetGridBulkEditPipeline.cpp`, using a closure rule: everything `StartBulkFieldEdit` calls that is not already in another TU.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: the popup is drawn only while open. A running job costs O(1) per frame in a map lookup during the fold. The pre-flight is O(N) once, at the click, in memory.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: all network I/O stays on `LaunchBackgroundTask` workers through the existing pump. Each row shows the "…" saving badge, and the popup or toast shows "k / N". The command path never loops synchronous submits on the UI thread (§ Approach, Commands). In P3, Jira task polling runs on the worker with a cancel token.
- **Pillar 3 (never crash)**: the job holds issue ids, never row indices or ticket pointers, so a cache refresh or re-sort cannot dangle. A fold for an unknown `BulkJobId` is a no-op that logs through `LOG_WARN`.
- **Pillar 4 (accessibility)**: the menu item and popup can be driven by keyboard through ImGui navigation, and the confirm step has a default-focused [Apply]. The popup text uses the same font-scaled widgets as the rest of the grid.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

1. **PR-fast CI**: **fires**. The changed path is the per-frame grid draw plus the edit pump. The scenario is the grid-scroll/edit one that `scripts/dev/perf-scenario-map.json` maps to `SmatchetActiveProjectGrid*` and `SmatchetGridFieldEditPipeline.cpp`. The `perf-gatekeeper` skill resolves the exact name at PR time.
2. **Pillar 2 static scanner**: **fires** in P3 only. The Jira `bulk/queue/{taskId}` poll loop is sync I/O and is marked `/* PILLAR2_WORKER_ONLY */ // est-latency: 30000ms`, because it can be reached only from `LaunchBackgroundTask`. P1 and P2 add no sync I/O.
3. **Dispatcher drain**: N/A. `MainThreadDispatcher::Drain()` is not touched. Completions use the existing `PostCompletionToMainThread`.
4. **Visible-cue bucket-E harness**: N/A. No new sync-stall path. Every row has the existing saving badge.
5. **Marker inventory**: N/A unless P3 adds a `SMATCHET_UI_PERF_SCOPE("grid.bulk.fold")`. If it does, regenerate `docs/perf/MARKER_INVENTORY.md` in that PR.

**Pre-push local check**: before each PR, run `docs/guides/perf-workflow.md` § Gate-check vs baseline (Step 7) against the named scenario.

**Override**: none expected.

## Risks / non-goals

- **Selection cleared by the first landed edit**: mitigated. The job snapshots ids, and `KeepIssueIds` rebuilds the visible selection (D5).
- **Accidental mass edit**: mitigated by the confirm step above 10 rows, the skip list, `ticket.set_field_bulk` being Destructive, and P2's Revert.
- **Cross-project option sets** (an assignee not assignable in another project, components that differ per project): the options come from the clicked row's catalog. A row where the value is not allowed fails at the backend with its own message (D4). Accepted: the user picked "try and report per row".
- **Status = N workflow transitions**: each row can be refused by its workflow and is reported per row. Accepted.
- **Revert fidelity**: Revert is best-effort. It re-applies the captured before-value, and a row changed by someone else in between gets the normal ADR-0016 conflict handling. Documented in the toast or menu tooltip.
- **P3 parallelism ordering**: the in-flight set is keyed by (backend, issue), so two edits for the same issue never overlap and per-issue order is unchanged. P3 needs a TSan run (§ Verification).
- **P3 Jira bulk API bypasses the per-row queue and conflict gate**: mitigated. It is online-only, and on a retryable error or offline it falls back to fan-out. It writes one `field_edit_diff` pair per row from the returned per-issue result, so the audit stays per row. It is not used on Jira Server or Data Center.
- **Save formats**: none change. Job state is in memory. `BulkJobId` is not persisted, and the offline queue schema is untouched.
- **Backend leakage**: P1 and P2 add none. P3 adds only defaulted virtuals, and the Jira code stays in `src/Tracker/`.
- **Dual-target**: everything is in `Source/Core`, so `SmatchetCore_DX12` compiles it as well. There is no `SMATCHET_WITH_MCP` gating, because the command goes through the registry.
- **Localization churn**: `grid.bulk.*` keys only, added through `SmatchetLocalization::T` / `Label` / `Format` with the English fallback kept in step with the table. `Command::Title`, `Summary` and `Params` stay plain English, because MCP publishes them verbatim.
- **PR #2432 conflict**: P1 needs #2432 merged, or is stacked on its branch. Shared files are `SmatchetUiSession.h` (different hunks: :115 and :1072 versus #2432 near :1130) and `SpreadsheetState.h` (`GridRectSelection` :19-66 versus #2432's removal at :114-126). This plan does not touch `TicketFieldEditor.cpp` or `TrackerLabelsEditor.cpp`. Conflict risk is low.
- **Bulk rows are not cancellable (P1)**: the palette/Lua worker has no cancel token (backlog: `docs/self-improvement/categories/debt/2026-11-16-bulk-edit-no-cancel-token.md`), and shutdown drops bulk rows still waiting in `queuedFieldEdits` (a `LOG_WARN` counts them; tracked as GitHub Issue #2448, a lost user write). Rows already enqueued offline are safe; unsent ones are not persisted.
- **Per-project option resolution**: a multi-value add/remove skips a row whose current value is not in the clicked row's option list (`grid.bulk.skip_unresolved`) instead of guessing, so a row from another project with different options is reported, not corrupted.
- **Non-goals**: multi-field apply in one gesture, ADF long-text bulk edit, a full undo stack, and bulk edit from the Views dashboard.

## Implementation handoff

**P1 slice 1 — job core + pipeline fold** (`grid-engine`, `Source/Core/src/Ui/AGENTS.md`)
- **Write set:** `BulkFieldEditPure.{h,cpp}`, `SmatchetUiSession.h`, `SmatchetGridFieldEditPipeline.cpp`, `tests/Core/BulkFieldEditPure.test.cpp`, `tests/CMakeLists.txt`.
- **Pre-resolved decisions:** there is no second commit branch, because fan-out goes into `queuedFieldEdits`. Verify that `CanEditFieldForIssue` is memory-only before using it N times.
- **Order:** first.

**P1 slice 2 — context menu + popup + keep-selection** (`grid-engine`)
- **Write set:** `SmatchetGridUiSupport.{h,cpp}`, `SmatchetActiveProjectGridCells.cpp`, `SmatchetActiveProjectGridUi.cpp`, `SmatchetActiveProjectGridTable.cpp`, `SpreadsheetState.h`, `SmatchetBulkFieldEditPopup.{h,cpp}`, `SmatchetLocalization.cpp`.
- **Pre-resolved decisions:** reuse `dropdown::DrawBody` from #2432 (no fork). Build `RecentsScope` from the pane's cache key, not `FocusedCacheBackendKey()`; #2432 already defers that bug.
- **Order:** after slice 1 and after #2432 lands, or stacked on it.

**P1 slice 3 — command** (`command-system`, `Source/Core/src/Commands/AGENTS.md`)
- **Write set:** `BuiltinCommands_TicketMutations.cpp`.
- **Pre-resolved decisions:** Destructive. The id list uses `ParamType::Json`, because there is no Array type. Dispatch threading is per § Open questions Q1.
- **Order:** after slice 1.

**P1 slice 4 — tests** (`test-author`): bucket E `tests/ui/bulk_field_edit.test.cpp`, plus CLI dry-run checks.

**P2 (text / date / number, Revert)** (`grid-engine` + `command-system`)
- **Write set:** files 13–16.
- **Order:** after P1 merges.

**P3 (parallel pump, Jira bulk)** (`grid-engine` for files 17 and 20; `tracker-backend` for files 18 and 19)
- These are split by subsystem. The backend virtual and the Jira implementation land in one commit before the pipeline switch.
- **Order:** after P2 merges.

## Verification

- **Bucket A (pure-logic doctest):** `tests/Core/BulkFieldEditPure.test.cpp` covers:
  - id snapshot and dedup
  - `ComputeValuesForRow` Replace / Add / Remove, including duplicates and an empty before-value
  - `Summarize` counts and `Done`
  - `MixedValueLabel` with one value and with k values
  - `NeedsConfirm` boundaries (10 / 11)
  - in P2, `MakeRevertJob` skipping Failed and Skipped rows

  Offline routing uses `FakeNetworkMode::TransportDown` → N rows end Queued.
- **Bucket E (ImGui Test Engine, `ninja-ui-test-msvc`):** `bulk_field_edit.test.cpp`:
  1. Seed 5 tickets and shift-select rows 1–5.
  2. Right-click the Assignee cell, pick a value, and check that 5 rows are saved, the summary toast shows 5 saved, and the selection is still 5 rows after the re-sort.
  3. Repeat with 12 rows and check that the confirm step appears.
- **Bash-driver / CLI (bucket A-CLI):** `smatchet ticket.set_field_bulk --ids '["A-1","A-2"]' --field assignee --value X --dry-run` returns the from → to diff. With `--yes` it returns `rows[].state`, and the audit log shows 2 `field_edit_diff` pairs. P3 adds a TSan run of the parallel pump plus fold.
- **Build gate:** `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target).
- **Doc validation (blocks plan-doc PRs — keep this bullet):** the canonical `scripts/dev/test-docs.sh` suite passes.
- **Plan stress-test — `grill-with-docs` (keep this bullet):** run `grill-with-docs` against this plan before P1 code starts, and record the outcome in § Deviations.
- **Manual residue:** none planned. The visual check of the popup layout is covered by the bucket-E test's screenshot step. If it ends up manual, add a `docs/self-improvement/categories/tooling.md` entry.

## Open questions

- **Q1 (resolved in P1 slice 3):** the threading model is undocumented, but `IMainThreadPoster::IsOnUiThread()` answers it directly, and `IsAutomationSource` is NOT a thread test (a Lua ticket-action script dispatches on the UI thread). The command runs the rows on `LaunchBackgroundTask` when the caller is on the UI thread and returns `{started:true}`; anywhere else it runs them on the calling thread.
- **Q2 (probe, P2):** does `CachedTicket` keep option **ids** for select, user and priority fields, or only display names? Revert needs ids. If only names are kept, P2 resolves names to ids through the catalog at job start, and records rows it cannot resolve as not revertible.
- **Q3 (probe, P3):** does the Jira `bulk/queue/{taskId}` result give a per-issue failure list that maps back to issue keys? If not, P3 re-fetches the job's issues to find which rows actually changed.
- **Q4 (user, P2):** the summary toast can hold one click action. P2 makes it "Select failed rows" when any row failed and "Revert" otherwise, and keeps both in the context menu. Confirm this or prefer something else when P2 starts.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- **Multi-field apply in one gesture** (the "bulk-edit dialog", option C): a follow-up plan if wanted. It would build on the same job core.
- **Fill-from-cell / Ctrl+Enter** (option A): a follow-up, on the same job core.
- **ADF / long-text bulk edit**: no action. The modal editor's 3-way merge is per ticket.
- **Full multi-step undo stack**: no action. Revert covers the single last job.
- **Bulk edit in the Views dashboard**: no action.

## Implementation log
- P1 shipped in #2434 (merged into #2432's branch) and landed on `develop` with #2432 (`60e4ea0e2`).
- Bulk-edit finish PR (this branch, `claude/bulk-edit-finish-4acd0e`), after the P1 code review:
  - Bucket-E cases for the labels multi-value picker (Replace / Add / Remove toggle, Apply, Enter in the tick-box list, Enter with nothing ticked) and CI wiring of `test-ui-bulk-field-edit` into the grid bucket-E driver loop.
  - Grid preview checks editability with the job's latched backend key and issue type, not the focused pane's.
  - The toast's first-error text is cut on a UTF-8 character boundary (`TruncateUtf8WithEllipsis`, shared with `LeftViewReasonPure`).
  - `ticket.set_field_bulk` mode validation moved into `ValidateBulkMode`.
  - `PickProjectComponents` shared by the grid components cell and the bulk popup (was a hand copy).
  - Enter in the tick-box list of the bulk modal applies the pick (`FilterDropdown` `EventKind::Done`); the draw-in-the-popup contract is documented.
  - Quick-create Esc is held back only by popups opened from its own window.

## Deviations from plan
- **Pre-implementation `grill-with-docs` (2026-11-14):** five gaps fixed before any code was written. (1) ADR-0016 scalar-base capture for every bulk row, through a shared `CaptureScalarBase`. (2) The per-cell menu ctx holds no owned strings (Ui § Per-cell allocation). (3) The P3 Jira bulk path goes through `FieldEditPipelineService`, not a grid-side commit branch, and P3 updates the Ui § Grid one-in-flight wording. (4) The cli, lua and mcp guides are updated. (5) Toast wording follows Ui § Toasts say what happened. The user chose the command name `ticket.set_field_bulk`.
- **Progress toast is one-shot (P1):** "Saving N rows..." at start and one summary toast at the end, not a live "Saving k / N" counter. The toast manager has no update-in-place API; the live counter moves to P2 if wanted.
- **Summary-text helpers live in `BulkFieldEditPure`:** `BulkSummaryText` / `BulkSummaryToastType` are shared by the grid's summary toast and the command worker's toast, and the unit-test binary links the Commands TUs but not the grid pipeline.
- **Command thread decision** uses `IsOnUiThread()` (see Q1), not the command source.
- **P1 code review (2026-11-15), fixed in P1:** `PlanBulkRows` (`Source/Core/src/Commands/Builtin/BuiltinCommands_TicketMutations.cpp:327`) skips "already has this value" rows; `add` / `remove` with empty `values` is rejected; a missing `Threading` is a `HandlerError`; the async branch needs a UI thread with a frame loop (the in-process one-shot CLI has none and runs synchronously, so it cannot drop rows at shutdown); `IAppTicketMutations` gained `GetLastTrackerConnectivityState` / `IsShuttingDown` (`Source/Core/include/Interfaces/IAppTicketMutations.h:45`) for the worker.
- **Selection hit-test uses `NoPopupHierarchy`:** a popup opened from the grid window counts as its child, so a right-click menu click was read as a click on the grid and cleared the selection. Both the cell hit-test and the click-outside clear now ignore popup children. A left click on a cell while a popup is open now only closes the popup (deliberate).
- **Deliberately deferred (not P1):** quitting mid-job drops the rows not yet sent (logged with `LOG_WARN`; persisting them is a follow-up); per-row `TrackerField` copies / a double `Prepare` for very large jobs (measure with perf-gatekeeper before changing); a cancel token for the palette worker; labels add / remove merge case-insensitively on purpose.
- **Review follow-ups deferred with a record:** quitting mid-job drops unsent rows (GitHub Issue #2448); no cancel token for the palette worker or a running job (`docs/self-improvement/categories/debt/2026-11-16-bulk-edit-no-cancel-token.md`, planned with Revert in P2).
- **Ctrl+F while a popup is open (not in this PR):** `grid.search.focus` calls `SetWindowFocus()` while the bulk dialog (or any popup) is open, which closes the popup. The fix is in `SmatchetGridHeaderUi.cpp`, which plan-lock `pane-picker-jira-sites` (branch `claude/plus-dropdown-two-jira-261a42`) claimed during this work, so it is deferred. Tracked as GitHub Issue #2449, with a ready-made bucket-E test that fails without the fix.
- **`DrawActions` in `SmatchetQuickCreateIssueUi.cpp`** has 27 decision points (cap 30, soft warning at the cap's edge); left as is.
- **Plan stays in `active/`:** only P1 ships here; P2 and P3 are still ahead.

## Verification (actual)
- Doctests (`ninja-test-msvc`, `SmatchetTests.exe`): 4218 / 4218 passed, including the `TruncateUtf8WithEllipsis` and `BulkFieldEditPure` cases.
- Bucket-E (`ninja-ui-test-msvc`): `test-ui-bulk-field-edit` 10 / 10, `test-ui-filter-dropdown` 19 / 19, `test-ui-data-dependent-windows` 12 / 12.
- `bash scripts/dev/pre-ship.sh`: all gates passed except the review ack, which is recorded after the code-review pass.
- Not run: the `SmatchetCore_DX12` dual-target build gate (no change touches a GLFW/OpenGL path; Core headers stay GL-free); a bucket-E case for Ctrl+F while a popup is open (item deferred to Issue #2449; the test was written and failed without the fix, as expected, and is attached to the Issue).
- Manual residue: none.

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*The `git mv` is the step that reliably gets dropped (empirically ~62% of post-ship plans drifted stale-in-place). Bind it to the impl-log write: in the SAME PR that populates the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
   > **Keep the literal `<slug>` placeholder in this committed step — do NOT
   > expand it to this plan's real filename.** Writing the actual basename here
   > manufactures a `docs/plans/shipped/<name>.md` path that points at a file
   > still living in `active/` (the move hasn't happened yet), which
   > `test-plan-ref-integrity.sh` reports as a dangling self-reference. The gate
   > carves out the *placeholder* form on the Archive `git mv` line; the
   > expanded form defeats that carve-out. Run the literal command with your
   > slug substituted at the shell — never bake the expansion into the file.
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*No ref-sweep — references use the tier-less form `docs/plans/<slug>.md` (the gates resolve it against any tier; PR #890), so the move can't break them. Write new plan references tier-less.*

*(Delete this `## Archive` block as part of step 2 — once moved to `shipped/`, the file is reference material and the checklist has served its purpose.)*

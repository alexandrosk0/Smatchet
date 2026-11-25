# Plan — Input UX Wave 2 leftover: drafts for Quick Create, the new-issue row and the worklog Time remaining
<!-- plan-date: 2026-11-24 -->

> **Slug**: `input-ux-w2-create-drafts` (matches this file's basename without `.md`).
>
> **Status**: `shipped`

## Context

Audit finding #51 (`docs/audits/INPUT_UX_AUDIT.md`): a half-written new issue lives only in memory. Esc, closing the window, a view change (which auto-discards the inline draft row with a toast) or a crash loses it. Wave 2's drafts-and-undo piece (`docs/plans/shipped/input-ux-w2-drafts-undo.md`, PR #2439) added the debounced draft store (`DraftStore.h`, `smatchet_drafts.json`) and used it for comments, the conflict resolver and the Log work dialog. It left out the two create surfaces, whose files another plan-lock held at the time:

- Quick Create (`SmatchetQuickCreateIssueUi.cpp`).
- The inline new-issue row (`SmatchetNewIssueDraftUi.cpp`).

The Log work draft keeps the spent time and the comment, but not a Time remaining the user typed (`TicketFieldEditor_Worklog.cpp`). The backlog entry `debt/2026-11-22-drafts-undo-deferred.md` lists all three.

Intended outcome: what the user typed on those surfaces is kept in the draft store. It comes back the next time the surface opens, with the same "Restored your unsaved …" line and Clear button as the Log work dialog. It is erased once the issue is created or queued, or the user discards it on purpose.

## Approach

- **Quick Create.**
  - Surfaces `quickcreate.summary` and `quickcreate.description`, scope `popup` (one popup). Only the two texts
    are kept; the project and type come from the open's own seed.
  - Each frame the popup is up, the two buffers are compared with what was last handed to the store. A change is
    `Put`; empty text erases. The description is kept only once the user edited it (the context prefill writes it
    otherwise).
  - `SeedOnOpen` restores both, unless the open passed its own `summary=` / `description=` text. Then neither comes
    back, and syncing waits until the user edits a field, so the caller's text never overwrites the kept entry. A
    restored description sets `quickCreateDescUserEdited`, so the context prefill does not replace it.
  - Erased on a successful create, on a queued create, and on the confirmed Discard. Clear on the restored line
    empties both fields, erases the entry and gathers the context description again, as a fresh open does.
  - A create that may have gone through (`IsAmbiguousCreateFailure`) leaves the text kept and sets a marker,
    `quickcreate.maybe_created`; the next open's restored line says to check the tracker before creating it again.
  - Closing the window with the X, or Esc/Cancel with nothing typed, keeps whatever is stored. Nothing typed means
    nothing stored.
- **New-issue row.**
  - Surface `newissue.row`, scope `grid|<connection>` (`NewIssueRowDraftScope`, from `FocusedCacheBackendKey`):
    the project and type it holds belong to that tracker connection. The stored text is a small JSON object:
    `project`, `issuetype_id`, `issuetype_name`, and `fields`, a map from field id to the text the user put in.
    A field counts when its text is not blank and differs from the text it opened with (`RowFieldTextTyped`, the
    test `DraftHasTypedText` also uses).
  - Whether a row is kept is decided at its first frame. A row opened for typing ("+ New Issue" in the header,
    `StartNewIssueDraft`, or in the grid's inactive row, both of which ask for the caret in the summary) is kept and
    gets a kept entry restored. A row the offline queue's "Open as draft" loads asks for no caret, so it is neither
    restored nor kept: the queue holds that one.
  - Synced once per frame while a kept row is up, but the JSON is built only when a row text input reported an edit
    or the project/type changed, and `Put` only when the JSON changed. Dumped with the `replace` error handler, so
    typed bytes that are not UTF-8 never throw. Not while a create runs: its result settles the entry.
  - Restore puts the project/type back (mirrored into the type dropdown's `FieldValues["issuetype"]`, as the queue
    does) and holds the kept field texts until each field's input first draws (`EnsureDraftEditBuf`). A field the
    view does not show keeps its text and still counts as typed, so Esc asks. Each field's seed stays the value it
    opened with, so the restored text counts as typed. Parsed with `json_safe::ParseBounded` (gate
    `bare-json-parse-untrusted`); a malformed entry is erased and ignored.
  - Erased on create success and on queue-offline, also when a view change closed the row while that create ran,
    and on `DiscardNewIssueDraft` (Cancel, "Discard draft", Esc on an empty draft). A create that may have gone
    through sets the marker `newissue.row.maybe_created`, and the next restore says to check the tracker.
  - Not erased by the view-change auto-discard (`CancelUnfinishedNewIssueForGridChange`), which is the loss #51
    names. Its toast now says "+ New Issue brings it back" when the text is kept, and keeps the old "discarded"
    text otherwise (a queue-loaded row, or one whose create is still running).
  - Staged attachments and picks in the row's other dropdowns are not kept: attachments are paths to files picked in
    this session.
- **Worklog Time remaining.**
  - Surface `worklog.remaining`, scope the issue, next to the two existing worklog surfaces.
  - Stored only while `TimeRemainingManuallyEdited`. The auto-derived value is recomputed at open, so keeping it
    would pin a stale estimate.
  - Restored with the flag set, cleared by the dialog's Clear (which also puts back the issue's current estimate),
    and erased with the other two by `EraseWorklogDraft`.
  - The keep/drop decision is a pure helper in `SmatchetWorklogSubmitPure.h` (`WorklogRemainingDraftText`),
    unit-tested.

## Interface contracts

N/A: no tracker, command, MCP, Lua or storage-schema change. New draft-store surface names only (`quickcreate.summary`, `quickcreate.description`, `quickcreate.maybe_created`, `newissue.row`, `newissue.row.maybe_created`, `worklog.remaining`); the store's file format is unchanged. Three new UI-internal functions in `SmatchetGridUiSupport.h` (`NewIssueRowDraftScope`, `KeepNewIssueRowDraftOnClose`, `ForgetNewIssueRowDraft`) for the view-change path and the tests.

## Files to modify

1. `Source/Core/src/Ui/SmatchetQuickCreateIssueUi.cpp`: sync, restore, erase, the restored-notice line, the
   may-have-been-created marker.
2. `Source/Core/src/Ui/SmatchetNewIssueDraftUi.cpp`: the keep decision, the row's JSON sync, the restore, erase in
   the create/queue/discard paths, the marker.
3. `Source/Core/src/Ui/SmatchetGridUiSupport.cpp` and `Source/Core/include/Ui/SmatchetGridUiSupport.h`: the
   view-change path keeps the row's text and says so; the three functions above.
4. `Source/Core/src/TicketFieldEditor_Worklog.cpp` and `TicketFieldEditor_Worklog.h`: the third surface;
   `WorklogDialogTestSnapshot` gains `TimeRemaining`.
5. `Source/Core/include/Ui/SmatchetWorklogSubmitPure.h` + `tests/Core/SmatchetWorklogSubmitPure.test.cpp`:
   `WorklogRemainingDraftText` and its cases.
6. `tests/ui/data_dependent_windows_smoke.test.cpp`: bucket-E cases for Quick Create and the row (existing
   registrations, no registry or CMake edit).
7. `docs/plans/shipped/input-ux-w2-create-drafts.md` (this plan, moved on ship), `docs/plans/INDEX.md`,
   `docs/self-improvement/categories/debt/2026-11-22-drafts-undo-deferred.md` (drop the two delivered bullets, add
   the Time remaining bucket-E residue and the queue settle gap below).
8. `.github/workflows/build-and-test.yml`: the jira-fixture job's driver loop gains
   `test-ui-data-dependent-windows`, so the fixture-gated draft-row cases run in CI (they skipped in every lane).

## Existing utilities reused

- `smatchet::drafts::{Put, Get, Erase}` (`DraftStore.h`), the Log work dialog's restored-notice pattern (`DrawWorklogDraftNotice`).
- `DraftHasTypedText` / `NewIssueDraftHasUserContent`, `EnsureDraftEditBuf`, `json_safe::ParseBounded`.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A: nothing is extracted. About 150 lines are added across the two create TUs (both under the 1,200-line ceiling).

## UX Pillar callouts

- **Pillar 1**: Quick Create compares two strings per frame. The row compares three strings per frame and builds its small JSON string only after an edit, and calls `Put` only on change. The store debounces the disk write on its own worker.
- **Pillar 2**: no I/O on the UI thread; `Put` is an in-memory map update (the file write is on the store's worker).
- **Pillar 3**: a malformed stored row entry is parsed with the bounded parser, then erased and ignored; it never throws into the frame. Typed bytes that are not UTF-8 are dumped with the `replace` handler, so building the entry never throws either.
- **Pillar 6**: unchanged queue semantics. A create queued offline erases the draft, because the queue now holds the content; a row loaded from the queue is never kept, so it is not held in two places.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A — <reason>`)

1. **PR-fast CI**: fires; no perf scenario drives Quick Create or the draft row.
2. **Pillar 2 static scanner**: N/A, no sync I/O added.
3. **Dispatcher drain**: N/A, not touched.
4. **Visible-cue bucket-E harness**: N/A, no new stall path.
5. **Marker inventory**: N/A, no markers.

## Risks / non-goals

- **Risk: a restored draft surprises.** The restored line names it and Clear drops it. It only appears after the user typed and left without creating.
- **Risk: a stale project in a restored row** (the project was renamed or removed). The project picker shows it as today and the create reports the error; Cancel discards it. The entry is per connection, so another tracker's project never lands in the row.
- **Risk: filing an issue twice.** The entry is erased when the create lands or is queued, also after a view change closed the row; a create that may have gone through marks it, and the restored line says to check first.
- **Known, Low (accepted):** a row opened while a closed row's create still runs is not restored; if that create then fails, the new row's first keystroke replaces the failed one's kept text. Once a Quick Create opened with its own text is edited, it replaces the older kept text (one popup, one entry). Kept texts for columns the view does not show count as typed but are not sent with Create.
- **Non-goal**: keeping staged attachments, and keeping Quick Create's project/type (the popup reseeds them from the last ticket each open).

## Implementation handoff

N/A: single slice in `Source/Core/src/Ui/` and the worklog TU.

## Verification

- **Bucket A** (`SmatchetWorklogSubmitPure.test.cpp`): `WorklogRemainingDraftText` keeps typed text only while manually edited.
- **Bucket E** (`DataDependentWindowsSmoke`):
  - `QuickCreate_DraftKeptAndRestored`: type a summary and description, close with the window's X, reopen. Both
    come back with the restored line; Clear empties them and the draft; a confirmed Discard erases it.
  - `QuickCreate_DraftArgsAndMaybeCreated`: an open with `summary=` shows the caller's text and leaves the kept
    entry alone until the user types; a create that may have gone through marks it, the next open restores it with
    the warning, and Clear drops both. Reopened while its create still runs, it shows the sent text with nothing
    restored over it, and the create's result still erases the entry. Sent with the caller's text and edited
    while that create runs, its result leaves the older kept draft alone.
  - `NewIssueDraftRow_DraftKeptAndRestored`: open the row with the grid's "+ New Issue", type a summary and an
    environment, close it as a view change does (`CancelUnfinishedNewIssueForGridChange`). The header's
    "+ New Issue" (`StartNewIssueDraft`) brings both back, Esc still asks, and the confirmed discard erases it.
  - `NewIssueDraftRow_KeptDraftSettledByCreate`: bytes that are not UTF-8 are still kept; a finished create
    erases the entry; a create that may have gone through, finishing after a view change closed the row, marks it
    and the next "+ New Issue" says so; a row loaded as the queue loads it is neither restored nor kept.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12`; unit `ninja-test-msvc`.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: `scripts/dev/test-docs.sh`.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: the coordinator session fixed the scope (item 4 of the Wave 2 leftovers). The terms (draft, surface, scope, restored line) follow `DraftStore.h` and the drafts-undo plan.
- **Manual residue**: the Log work Time remaining restore has no bucket-E case here, because its test host `tests/ui/duration_inline_edit_commit.test.cpp` is held by the `input-ux-w3-grid-s2` plan-lock. It is backlogged in the debt entry.

## Open questions

None.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- The bucket-E case for the Time remaining restore (test host locked, see Verification).
- CI running the fixture-gated `DataDependentWindowsSmoke` cases (the backlog entry in Files row 7).
- fr-FR rows for the new strings (with the other Wave 2 French rows).

## Implementation log

- 2026-11-24: the three surfaces landed as one PR. First the worklog Time remaining (pure helper and unit cases),
  then Quick Create, then the draft row and the view-change toast; the bucket-E cases went in beside the existing
  `DataDependentWindowsSmoke` keys cases.
- The code-review agent's first round found 1 critical, 3 high, 7 medium and 5 low issues. All are fixed in this
  PR (see Deviations), and a re-check ran on the result.

## Deviations from plan

- **The row's keep decision moved to its first frame.** The plan restored in `DrawNewIssueInactiveCell` only. Review
  found that the header's "+ New Issue" (`StartNewIssueDraft`, also the pane replay) skipped the restore, and the
  first sync then erased the kept entry. It also found that a row the offline queue's "Open as draft" loads was
  synced, so the same issue would be held twice. `SmatchetGridHeaderUi.cpp` is held by another session's
  plan-lock, so the opener was not edited. Instead the row decides at its first frame: a caret request
  (`newIssueFocusSummaryPending`, set by both "+ New Issue" paths and not by the queue) means kept and restored.
- **Scope is per connection** (`grid|<connection>`), not `grid`: a kept project and type belong to one tracker.
- **Settling by the create's result.** A create that lands after a view change closed the row erases its own
  entry, and one that may have gone through sets a `maybe_created` marker on both surfaces. The restored line then
  warns. This was not in the plan.
- **Quick Create opened with its own text** restores neither field and leaves the kept entry alone until the user
  edits. The plan restored the field the open did not pass.
- **Review fixes** (from the code-review agent): the replace error handler on `dump()`; a dirty flag so the JSON
  is built only after an edit; the restored type mirrored into the dropdown; kept texts held until their input
  draws (a hidden column keeps its text, and it counts as typed); a shared typed-text predicate; Quick Create's
  Clear gathers the context again; the view-change toast says "kept" only when it is.
- **Files beyond the list**: `SmatchetGridUiSupport.h` (three functions) and the CI workflow (Files row 8).
- **CI gap closed here**: the fixture-gated draft-row cases skipped in every CI lane, so the jira-fixture job now
  runs `test-ui-data-dependent-windows` too.
- **Re-check fixes**: a Quick Create opened with its own text and never edited neither erases nor marks the kept
  draft it did not show (Create, queue and Discard all leave it); a sync that empties the kept text also drops
  the `maybe_created` marker.
- **Delta-check fixes**: reopening Quick Create while its create still runs no longer seeds it. Seeding brought the
  kept (sent) text back and cleared the in-flight flag, so the result was never polled, the entry never settled,
  and Create could be pressed again. The reopen now shows the sent text as it was and keeps polling; the open's own
  arguments are dropped (logged). An edit while the create runs does not lift the suspended sync, so the result
  settles the entry as it was when sent; such an edit is kept from the next keystroke after a failed result. The
  restored line and its Clear are hidden while a create runs. The marker erase runs only when a synced text changes to empty, not every frame. The
  jira-fixture step's name now lists the data-dependent windows driver.
- **Left as backlog**: the queue's "Open as draft" consumes a finished row create without settling the kept entry
  (`SmatchetOfflineQueueUi.cpp`, held by the conflict-discard lock; a one-frame window). In the debt entry.
- **Test seam**: hiding Quick Create by clearing its flag while the summary holds the caret makes ImGui write the
  old text back at the next open. That is not something a user can do, since the X click moves the caret first. The
  cases drop the caret first (`DeactivateFieldForTest`).

## Verification (actual)

- Builds: `ninja-iter-msvc` (`SmatchetStandalone` and `SmatchetCore_DX12`) clean; `ninja-ui-test-msvc` clean.
- Bucket A: `SmatchetWorklogSubmitPure` 3/3 assertions.
- Bucket E `DataDependentWindowsSmoke`: 16/16 on the default driver (`test-ui-command-palette-inline-typing.sh`)
  and 16/16 on the fixture driver (`test-ui-jira-deterministic-backend.sh`, which runs the draft-row cases).
- Mutants, each killed on the fixture driver:
  - the queue row kept (the caret test removed);
  - no erase on a finished create;
  - no `maybe_created` marker;
  - kept texts never filled into their buffers;
  - Quick Create's sync not suspended for an open with its own text;
  - (re-check) a never-edited Quick Create opened with its own text erasing the kept draft on Discard (the
    suspended check in `EraseQuickCreateDraft` removed);
  - (delta check) Quick Create seeding again when reopened while its create runs (the in-flight guard at open
    removed);
  - (delta check) an edit while the create runs lifting the suspended sync (the in-flight guard on the summary's
    edit removed);
  - (first round) the row restore call removed;
  - (first round) Quick Create's Discard not erasing the entry.
- CI driver `scripts/dev/test-ui-data-dependent-windows.sh` (the step added to the jira-fixture job): 16/16.
- `scripts/dev/test-docs.sh`: passes with the flip.

# Plan — Input UX Wave 2 leftover: drafts for Quick Create, the new-issue row and the worklog Time remaining

> **Slug**: `input-ux-w2-create-drafts` (matches this file's basename without `.md`).
>
> **Status**: `active`

## Context

Audit finding #51 (`docs/audits/INPUT_UX_AUDIT.md`): a half-written new issue lives only in memory. Esc, closing the window, a view change (which auto-discards the inline draft row with a toast) or a crash loses it. Wave 2's drafts-and-undo piece (`docs/plans/shipped/input-ux-w2-drafts-undo.md`, PR #2439) added the debounced draft store (`DraftStore.h`, `smatchet_drafts.json`) and used it for comments, the conflict resolver and the Log work dialog. It left out the two create surfaces, whose files another plan-lock held at the time:

- Quick Create (`SmatchetQuickCreateIssueUi.cpp`).
- The inline new-issue row (`SmatchetNewIssueDraftUi.cpp`).

The Log work draft keeps the spent time and the comment, but not a Time remaining the user typed (`TicketFieldEditor_Worklog.cpp`). The backlog entry `debt/2026-11-22-drafts-undo-deferred.md` lists all three.

Intended outcome: what the user typed on those surfaces is kept in the draft store. It comes back the next time the surface opens, with the same "Restored your unsaved …" line and Clear button as the Log work dialog. It is erased once the issue is created or queued, or the user discards it on purpose.

## Approach

- **Quick Create.**
  - Surfaces `quickcreate.summary` and `quickcreate.description`, scope `popup` (one popup).
  - Each frame the popup is up, the two buffers are compared with what was last handed to the store. A change is `Put`; empty text erases.
  - `SeedOnOpen` restores both when the open passed no summary/description argument (arguments are caller content and win). A restored description sets `quickCreateDescUserEdited`, so the context prefill does not replace it.
  - Erased on a successful create, on a queued create, and on the confirmed Discard.
  - Closing the window with the X, or Esc/Cancel with nothing typed, keeps whatever is stored. Nothing typed means nothing stored.
- **New-issue row.**
  - Surface `newissue.row`, scope `grid`. The stored text is a small JSON object: `project`, `issuetype_id`, `issuetype_name`, and `fields`, a map from field id to the live text of every edit buffer the user typed into (the same test `DraftHasTypedText` uses: summary/description non-blank, or any other buffer that differs from the text it opened with).
  - Synced once per frame while the row is active, but only when the JSON differs from the last stored one. No draft text means it is erased.
  - "+ New Issue" seeds the draft as today, then overlays a kept entry. The project/type are put back, and each kept field gets its edit buffer pre-created with the restored text. Its seed stays the value it opened with, so Esc still asks and the text counts as typed.
  - Erased on create success, on queue-offline, and on `DiscardNewIssueDraft` (Cancel, "Discard draft", Esc on an empty draft).
  - Not erased by the view-change auto-discard (`SmatchetGridUiSupport.cpp`), which is the loss #51 names. The next "+ New Issue" brings it back.
  - Parsed with `json_safe::ParseBounded` (gate `bare-json-parse-untrusted`). A malformed entry is erased and ignored.
  - Staged attachments are not kept: they are paths to files picked in this session.
- **Worklog Time remaining.**
  - Surface `worklog.remaining`, scope the issue, next to the two existing worklog surfaces.
  - Stored only while `TimeRemainingManuallyEdited`. The auto-derived value is recomputed at open, so keeping it would pin a stale estimate.
  - Restored with the flag set, cleared by the dialog's Clear (which also puts back the issue's current estimate), and erased with the other two by `EraseWorklogDraft`.
  - The keep/drop decision is a pure helper in `SmatchetWorklogSubmitPure.h` (`WorklogRemainingDraftText`), unit-tested.

## Interface contracts

N/A: no tracker, command, MCP, Lua or storage-schema change. New draft-store surface names only (`quickcreate.summary`, `quickcreate.description`, `newissue.row`, `worklog.remaining`); the store's file format is unchanged.

## Files to modify

1. `Source/Core/src/Ui/SmatchetQuickCreateIssueUi.cpp`: sync, restore, erase, the restored-notice line.
2. `Source/Core/src/Ui/SmatchetNewIssueDraftUi.cpp`: the row's JSON sync, restore overlay in `DrawNewIssueInactiveCell`, erase in the create/queue/discard paths, the restored-notice line, a test seam.
3. `Source/Core/src/TicketFieldEditor_Worklog.cpp` and `TicketFieldEditor_Worklog.h`: the third surface; `WorklogDialogTestSnapshot` gains `TimeRemaining`.
4. `Source/Core/include/Ui/SmatchetWorklogSubmitPure.h` + `tests/Core/SmatchetWorklogSubmitPure.test.cpp`: `WorklogRemainingDraftText` and its cases.
5. `tests/ui/data_dependent_windows_smoke.test.cpp`: bucket-E cases for Quick Create and the row (existing registrations, no registry or CMake edit).
6. `docs/plans/shipped/input-ux-w2-create-drafts.md` (this plan, moved on ship), `docs/plans/INDEX.md`, and `docs/self-improvement/categories/debt/2026-11-22-drafts-undo-deferred.md` (drop the two bullets).

## Existing utilities reused

- `smatchet::drafts::{Put, Get, Erase}` (`DraftStore.h`), the Log work dialog's restored-notice pattern (`DrawWorklogDraftNotice`).
- `DraftHasTypedText` / `NewIssueDraftHasUserContent`, `EnsureDraftEditBuf`, `json_safe::ParseBounded`.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A: nothing is extracted. About 150 lines are added across the two create TUs (both under the 1,200-line ceiling).

## UX Pillar callouts

- **Pillar 1**: Quick Create compares two strings per frame. The row builds a small JSON string per frame only while it is active, and calls `Put` only on change. The store debounces the disk write on its own worker.
- **Pillar 2**: no I/O on the UI thread; `Put` is an in-memory map update (the file write is on the store's worker).
- **Pillar 3**: a malformed stored row entry is parsed with the bounded parser, then erased and ignored; it never throws into the frame.
- **Pillar 6**: unchanged queue semantics. A create queued offline erases the draft, because the queue now holds the content.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A — <reason>`)

1. **PR-fast CI**: fires; no perf scenario drives Quick Create or the draft row.
2. **Pillar 2 static scanner**: N/A, no sync I/O added.
3. **Dispatcher drain**: N/A, not touched.
4. **Visible-cue bucket-E harness**: N/A, no new stall path.
5. **Marker inventory**: N/A, no markers.

## Risks / non-goals

- **Risk: a restored draft surprises.** The restored line names it and Clear drops it. It only appears after the user typed and left without creating.
- **Risk: a stale project in a restored row** (the project was renamed or removed). The project picker shows it as today and the create reports the error; Clear resets it.
- **Non-goal**: keeping staged attachments, and keeping Quick Create's project/type (the popup reseeds them from the last ticket each open).

## Implementation handoff

N/A: single slice in `Source/Core/src/Ui/` and the worklog TU.

## Verification

- **Bucket A** (`SmatchetWorklogSubmitPure.test.cpp`): `WorklogRemainingDraftText` keeps typed text only while manually edited.
- **Bucket E** (`DataDependentWindowsSmoke`):
  - `QuickCreate_DraftKeptAndRestored`: type a summary and description, close with the window's X, reopen. Both come back with the restored line; Clear empties them and the draft; a confirmed Discard erases it.
  - `NewIssueDraftRow_DraftKeptAndRestored`: open the row with "+ New Issue", type a summary and an environment, drop the row as a view change does (`ClearNewIssueDraftForTest` keeps the store). "+ New Issue" brings both back, Esc still asks, and Cancel's discard erases the entry.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12`; unit `ninja-test-msvc`.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: `scripts/dev/test-docs.sh`.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: the coordinator session fixed the scope (item 4 of the Wave 2 leftovers). The terms (draft, surface, scope, restored line) follow `DraftStore.h` and the drafts-undo plan.
- **Manual residue**: the Log work Time remaining restore has no bucket-E case here, because its test host `tests/ui/duration_inline_edit_commit.test.cpp` is held by the `input-ux-w3-grid-s2` plan-lock. It is backlogged in the debt entry.

## Open questions

None.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- The bucket-E case for the Time remaining restore (test host locked, see Verification).
- fr-FR rows for the new strings (with the other Wave 2 French rows).

## Implementation log

## Deviations from plan

## Verification (actual)

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index from the repository root: `bash agent-layer/agents/scripts/core/test-plan-index.sh --fix`.*

*(Delete this `## Archive` block as part of step 2.)*

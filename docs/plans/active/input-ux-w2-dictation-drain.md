# Plan — Input UX Wave 2 leftover: one dictation reload drain, and a posted comment restored as a draft
<!-- plan-date: 2026-11-25 -->

> **Slug**: `input-ux-w2-dictation-drain` (matches this file's basename without `.md`).
>
> **Status**: `active`

## Context

Two Issues were filed by the code-review agent during the #2439 (drafts and undo) develop-merge review:

- **#2458: dictation into a field other than the AI Assistant can be overwritten.** `DictationInsertionRouter::InsertIntoFocusedInputText` splices into the registered buffer and arms one global `pendingReloadItemId_`. While an `InputText` is active, ImGui edits its own copy of the text, so the UI thread has to call `ImGui::GetInputTextState(id)->ReloadUserBufAndMoveToEnd()` before the next keystroke reaches that widget.
  - Two panels drain it today: the AI Assistant (`SmatchetAiAssistantUi.cpp`) and, since #2439, the comment box (`SmatchetCommentsModalUi.cpp`). Each drains the one global id before its own input.
  - The worklog description (`TicketFieldEditor_Worklog.cpp`), the annotate analysis input (`AnnotateAnalysisUi_Window.cpp`) and every other field routed through `SmatchetLocalizedImGui` have no drain. When neither drain panel is drawn that frame, nothing reloads them. The next keystroke then writes ImGui's stale copy over the dictated text.
- **#2459: reopening an issue while its comment is posting shows the posted text as a restored draft.**
  1. The post runs on a worker while the stored draft still holds its text.
  2. Close and reopen the same issue's comments before the result lands: the open restores the draft, which is the text being posted, and says "Draft restored".
  3. The result erases the stored draft (`EraseIfText`), but `CallbackIsStale` (a new `OpenGen`) skips clearing the box. The box keeps the posted text marked as a restored draft, which invites posting it twice.

Intended outcome:

- Dictated text survives the next keystroke in whichever routed field has the caret.
- A comment that has been posted never sits in the box as a restored draft.

## Approach

- **One drain (#2458).**
  - `SmatchetLocalizedImGui::ApplyPendingDictationReload()`, defined next to `HookDictationOnLastItem` in `SmatchetDictationHook.cpp`, takes the pending id and reloads that widget's `ImGuiInputTextState`. `GetInputTextState(id)` returns state only for the widget that owns it, so the drain is right for any field.
  - `SmatchetUI` calls it once per frame, straight after the in-frame `mainThreadDispatcher.Drain()` (where the Whisper post-back splices) and before any panel draws. Splices posted by the standalone loop's drain before `NewFrame` are covered by the same call.
  - The two panel drains are removed, so there is one site for every field.
  - `WhisperAiAssistantAutosendScenario` consumed the id itself to prove the reload was armed. The central drain now consumes it first, so the router keeps the last consumed id for tests (`TakeLastConsumedReloadItemIdForTest`), and the scenario reads that instead.
- **Posted draft (#2459).**
  - A pure helper, `SmatchetCommentsModalGen::PostedBodyRestoredAsDraft`, decides when a stale post result must still clear the box. All of these must hold:
    - the result is not a failure;
    - the modal is open on the same draft scope;
    - the box is still marked `DraftRestored`;
    - the box holds exactly the posted body.
  - `ApplyCommentPostResult` applies it in its stale branch: it clears the box, `DraftRestored` and `DraftSynced`. Text the user typed since stays, because the box no longer equals the posted body.

## Interface contracts

- `DictationInsertionRouter::TakeLastConsumedReloadItemIdForTest()`: a test-and-clear of the last id `ConsumePendingReloadItemId` returned. It is implemented in both the Whisper and the stub TU (the stub returns 0). No other router contract changes.
- `SmatchetLocalizedImGui::ApplyPendingDictationReload()`: UI thread only, inside an ImGui frame.
- No tracker, command, MCP, Lua or storage change.

## Files to modify

1. `Source/Core/include/DictationInsertionRouter.h`, `Source/Core/src/DictationInsertionRouter_Whisper.cpp`, `Source/Core/src/DictationInsertionRouter_Stubs.cpp`: the sticky last-consumed id and its accessor; the comment on the drain.
2. `Source/Core/include/SmatchetLocalizedImGui.h`, `Source/Core/src/SmatchetDictationHook.cpp`: `ApplyPendingDictationReload`.
3. `Source/Core/src/Ui/SmatchetUI.cpp`: the one call after the in-frame dispatcher drain.
4. `Source/Core/src/Ui/SmatchetAiAssistantUi.cpp`, `Source/Core/src/Ui/SmatchetCommentsModalUi.cpp`: remove the panel drains. `SmatchetCommentsModalUi.cpp` also applies the #2459 helper.
5. `Source/Core/src/Commands/Scenarios/WhisperAiAssistantAutosendScenario.cpp`: read the sticky id.
6. `Source/Core/include/Ui/SmatchetCommentsModalGenPure.h`: `PostedBodyRestoredAsDraft`.
7. Tests:
   - `tests/Core/DictationInsertionRouter.test.cpp`: the sticky id.
   - `tests/Core/SmatchetCommentsModalGenPure.test.cpp`: the helper.
   - `tests/support/FakeTrackerClient.h`: a hold on `AddIssueCommentPlain`, so a post can be kept in flight.
   - `tests/ui/field_viewer_modal.test.cpp`: the comment-box dictation case gains a keystroke after the splice, which pins the reload. A new case covers the reopen-while-posting order.
8. `docs/plans/shipped/input-ux-w2-dictation-drain.md` (this plan, moved on ship), `docs/plans/INDEX.md`, `docs/self-improvement/categories/debt/2026-11-22-drafts-undo-deferred.md` (the related-Issues line).

## Existing utilities reused

- `DictationInsertionRouter::ConsumePendingReloadItemId`, `ImGuiInputTextState::ReloadUserBufAndMoveToEnd`.
- `SmatchetCommentsModalGen::CallbackIsStale`, `smatchet::drafts::EraseIfText`.
- The `OfflineFirst` comment cases' helpers (`OpenFieldViewerForField`, `WaitForComposerCaret`, `GetCommentsModalSnapshotForTests`).

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A: nothing is extracted. Two drain blocks move into one helper of about ten lines.

## UX Pillar callouts

- **Pillar 1**: one atomic exchange per frame; the reload runs only on a frame after a splice.
- **Pillar 2**: no I/O. The fake tracker's hold blocks the post worker only, never the UI thread.
- **Pillar 3**: `GetInputTextState` returns null for a widget with no state, and the drain does nothing then.
- **Pillar 6**: N/A. A queued comment takes the same path as a sent one.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A — <reason>`)

1. **PR-fast CI**: fires. The per-frame cost is one atomic exchange.
2. **Pillar 2 static scanner**: N/A, no sync I/O added.
3. **Dispatcher drain**: touched only by the call placed after it; the drain itself is unchanged.
4. **Visible-cue bucket-E harness**: N/A, no new stall path.
5. **Marker inventory**: N/A, no markers.

## Risks / non-goals

- **Risk: a splice that lands after the drain in the same frame.** A splice can arrive after the drain from a call on the UI thread outside the dispatcher, such as a scenario tick or a test. It is reloaded on the next frame, before any field draws. A keystroke in that same frame could still see the stale copy. That is the same window the panel drains had.
- **Non-goal**: splice at the caret. Insertion still lands at the end, as before.

## Implementation handoff

N/A: single slice.

## Verification

- **Bucket A**:
  - `DictationInsertionRouter`: `TakeLastConsumedReloadItemIdForTest` reports the id a consume returned, once.
  - `SmatchetCommentsModalGenPure`: `PostedBodyRestoredAsDraft` for every condition.
- **Bucket E**:
  - `OfflineFirst/Comments_DictatedTextIsKeptAsDraft`: a key typed after the splice keeps the dictated text. The comment box no longer has its own drain, so this proves the central one.
  - New `OfflineFirst/Comments_ReopenWhilePostingClearsPostedDraft`: post with the fake tracker holding the call, close, reopen (the draft shows), release, and the box comes back empty and not marked restored.
- **Scenario**: `whisper-ai-assistant-autosend` still passes.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12`; unit `ninja-test-msvc`; UI `ninja-ui-test-msvc`.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: `scripts/dev/test-docs.sh`.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: the coordinator session fixed the scope (item 6 of the Wave 2 leftovers). The terms follow the Issues and `docs/plans/shipped/input-ux-w2-drafts-undo.md`.
- **Manual residue**: real-microphone dictation into the worklog description. The bucket-E case drives the router directly, as the existing one does.

## Open questions

None.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- Splice at the caret (see Non-goal).

## Implementation log

## Deviations from plan

## Verification (actual)

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index from the repository root: `bash agent-layer/agents/scripts/core/test-plan-index.sh --fix`.*

*(Delete this `## Archive` block as part of step 2.)*

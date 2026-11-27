# Plan — Input UX Wave 2: the four Esc sites the dialog contract left (audit § 4.1, § 3)
<!-- plan-date: 2026-11-27 -->

> **Slug**: `input-ux-w2-dialog-contract-esc-sites` (matches this file's basename without `.md`).
>
> **Status**: `active` — follow-up to `docs/plans/input-ux-w2-dialog-contract.md` (shipped: PRs #2437, #2468, #2469, test housekeeping #2520). It closes the bullet "Row 25, four Esc sites not on the scope" of `docs/self-improvement/categories/debt/2026-11-22-dialog-contract-deferred.md`.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

The dialog contract (`Ui/SmatchetDialogScope.h`) reads a modal's Esc and Ctrl+Enter once, from `ImGui` state the scope can trust (it reads last frame's field because ImGui's nav cancel clears the active item inside `NewFrame`), and mutes app shortcuts and returns focus around it. Slice 4 left four modals with their own Esc code, because none was a straight substitution:

| Site | File | What it does today |
|---|---|---|
| Offline conflict resolver | `Source/Core/src/Ui/SmatchetOfflineQueueUi.cpp` (`ReadConflictModalKeys`) | its own `ResolveEscape` call plus a private "had the keyboard last frame" flag; "typing" means the one resolved field, not any field |
| Comments post box | `Source/Core/src/Ui/SmatchetCommentsModalUi.cpp` (`DrawCommentsPostInput`) | reads Esc itself: leaves the box keeping the text, and in a blank box asks the shell to close (`CloseRequested`) |
| Field viewer shell | `Source/Core/src/Ui/SmatchetFieldViewerModalUi.cpp` (`RenderFieldViewerModal`) | closes on Esc only when `!io.WantTextInput` (a heuristic), so the box's own Esc decides while a field is active |
| Long-text editor | `Source/Core/src/TicketFieldEditor_Modal.cpp` (`DrawLongTextFooter`) | Esc asks to discard even from the text area |

Intended outcome — after this lands: every modal's Esc is decided by one rule (`dialogkeys::ResolveEscape`, through `DialogReadKeys`) or carries a comment naming why it is exempt; no site reads the key with a heuristic that can disagree with the scope.

Per-site decision (settled here, not left to the implementer):

- **Conflict resolver — migrate**, with one scope option: `DialogReadKeys` takes the field that counts as "being typed in". Its two read-only panes (`##CRMine`, `##CRTheirs`) can hold the active item; with "any field" an Esc pressed after clicking into one of them would leave that pane instead of showing the "Esc keeps your typed resolution" hint.
- **Field viewer shell and Comments post box — migrate together.** The shell reads Esc with `DialogReadKeys(nothingTyped = !Content->HasUnsavedInput())` and the box loses its own Esc block and `CloseRequested`. The outcomes are the same as today for the Comments content (first Esc leaves a box holding text, a blank box closes the viewer, a close with a draft asks), but they come from the scope's rule instead of `WantTextInput`, and `IFieldViewerContent::ConsumeCloseRequest` has no caller left.
- **Long-text editor — documented exception, no behaviour change.** Esc asks to discard even from the text area. The question is the guard: `Keep editing` returns the caret and nothing is lost, so a layered first Esc would add a key press to every dirty cancel without protecting more. The exception is a comment at the Esc read, and `docs/guides/keyboard-shortcuts.md` already states it. Revisit only if the user wants audit § 3's layered multiline Esc here (one line: pass `nothingTyped = !dirty` to `DialogReadKeys`).

## Approach

1. Add an optional last parameter `ImGuiID typingField = 0` to `DialogReadKeys` and a matching `DialogScopeState::TypingField`. With 0 the behaviour is unchanged (any field of the dialog counts). With a field id only that item counts as "being typed in", both for this frame's `ActiveId` and for `FieldActiveLastFrame` (which `EndDialog` records with the same definition), and only that item is left on Esc / Ctrl+Enter.
2. Conflict resolver: `BeginDialog` / `EndDialog` replace `BeginPopupModal` / `EndPopup` and the private `g_conflictOwnedKeyboardLastFrame`; `ReadConflictModalKeys` takes Esc and Ctrl+Enter from `DialogReadKeys(state, nothingTyped, resolvedFieldId)` and keeps Alt+M / Alt+T and its caret request (re-armed after Use Mine / Use Theirs, which the scope's one-shot `DialogFocusHere` does not model).
3. Field viewer shell: `BeginDialog` / `EndDialog`; Esc from `DialogReadKeys(state, !HasUnsavedInput())`; `Cancel` takes the existing close path (a viewer with unsaved input asks first). Comments: delete the box's Esc block, `CommentsModalState::CloseRequested` and the `ConsumeCloseRequest` override; delete `IFieldViewerContent::ConsumeCloseRequest`.
4. Long-text editor: comment only.
5. Bucket-E cases for each site that changes (below), each mutation-checked.

## Interface contracts

- `Source/Core/include/Ui/SmatchetDialogScope.h`: `DialogKeys DialogReadKeys(DialogScopeState& s, bool nothingTyped, ImGuiID typingField = 0);` and `DialogScopeState::TypingField` (ImGuiID, 0 = any field). Source-compatible: every existing call keeps its meaning.
- `Source/Core/include/Ui/SmatchetFieldViewerModalUi.h`: remove `IFieldViewerContent::ConsumeCloseRequest()`. Its only override is the Comments content's; grep before deleting (`git grep -n ConsumeCloseRequest`).
- No tracker, command, Lua, MCP, config, SQLite or view-storage change.

Plumbing, hop by hop: `RenderFieldViewerModal` → `BeginDialog(s_viewerDialog, …)` → `DialogReadKeys(s_viewerDialog, !HasUnsavedInput())` → `wantClose` → existing discard path. `DrawConflictModal` → `BeginDialog(s_conflictDialog, …)` → `ReadConflictModalKeys` → `DialogReadKeys(s_conflictDialog, nothingTyped, GetID(resolvedLabel))`.

## Files to modify

1. `Source/Core/include/Ui/SmatchetDialogScope.h` — the optional parameter and state field.
2. `Source/Core/src/Ui/SmatchetDialogScope.cpp` — `DialogReadKeys` and `EndDialog` use the typing-field definition.
3. `Source/Core/src/Ui/SmatchetOfflineQueueUi.cpp` — the resolver on the scope.
4. `Source/Core/src/Ui/SmatchetFieldViewerModalUi.cpp` and `Source/Core/include/Ui/SmatchetFieldViewerModalUi.h` — the shell on the scope; `ConsumeCloseRequest` removed.
5. `Source/Core/src/Ui/SmatchetCommentsModalUi.cpp` — the box's Esc block, `CloseRequested` and the `ConsumeCloseRequest` override removed.
6. `Source/Core/src/TicketFieldEditor_Modal.cpp` — a comment at the Esc read naming the exception.
7. `tests/ui/field_viewer_modal.test.cpp`, `tests/ui/offline_conflict_modal_panes.test.cpp` — bucket-E cases (both files are already registered; no CMake or registry edit).
8. `docs/guides/keyboard-shortcuts.md`, `Source/Core/src/Ui/AGENTS.md` — one line each: the typing-field option and the long-text exception.
9. `docs/self-improvement/categories/debt/2026-11-22-dialog-contract-deferred.md` — the four-sites bullet leaves it.

### File-level changes

| File | Symbol |
|---|---|
| `Source/Core/src/Ui/SmatchetDialogScope.cpp` | `DialogReadKeys` |
| `Source/Core/src/Ui/SmatchetOfflineQueueUi.cpp` | `ReadConflictModalKeys` |
| `Source/Core/src/Ui/SmatchetFieldViewerModalUi.cpp` | `RenderFieldViewerModal` |
| `Source/Core/src/Ui/SmatchetCommentsModalUi.cpp` | `DrawCommentsPostInput` |
| `Source/Core/src/TicketFieldEditor_Modal.cpp` | `DrawLongTextFooter` |

## Existing utilities reused

- `smatchet::ui::BeginDialog` / `EndDialog` / `DialogReadKeys` (`Ui/SmatchetDialogScope.h`) and `dialogkeys::ResolveEscape` (`Ui/DialogKeysPure.h`): the rule itself; nothing is re-decided here.
- The Wave 1 bucket-E cases that already pin these Esc behaviours (`Comments_EscKeepsTypedText`, `Comments_EscOnFilledBoxNobodyTypedIn`, `Comments_DiscardConfirmKeysKeep`, `FieldViewer_HistoryOpensAndEscCloses`, `LongText_*`, `ConflictModal_ScalarAndUnverifiedPerKindPanes`): they are the regression net and must stay green unchanged.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — nothing is extracted; the change deletes more than it adds.

## UX Pillar callouts

- **Pillar 1 (perf)**: no per-frame allocation added; `DialogReadKeys` already runs once per open dialog per frame.
- **Pillar 2 (UI never blocks)**: no I/O.
- **Pillar 3 (never crash)**: no window pointer is kept; the scope state is a plain struct owned next to each dialog. `ClosePopupToLevel` is not called.
- **Pillar 4 (accessibility)**: Esc means the same thing in these modals as everywhere else; no key press is lost or added (long-text keeps its one-Esc confirm).

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

1. **PR-fast CI** — fires as usual; none of the four modals is open in a PR-fast scenario, so no budget moves.
2. **Pillar 2 static scanner** — N/A: no sync I/O added.
3. **Dispatcher drain** — N/A: untouched.
4. **Visible-cue bucket-E harness** — N/A: no stall path.
5. **Marker inventory** — N/A: no markers.

**Pre-push local check**: the CI PR-fast lane is authoritative (this container is Linux; `project.config.json` § environments `linux-container`).

## Risks / non-goals

- **A lost Wave 1 behaviour.** Esc in these four modals was verified one case at a time in Wave 1. Mitigation: the plan keeps the cases above green unchanged, runs them before and after, and adds cases only where the rule moves.
- **Whitespace-only comment draft.** The box treated a whitespace-only box as "no text" (close request); the scope uses `HasUnsavedInput()` (any character), so the first Esc now leaves the box and the second closes through the discard question. Accepted: it matches how any other typed text behaves, and it cannot lose text.
- **Resolver caret re-request.** `DialogFocusHere` is one-shot; the resolver re-arms its caret after Use Mine / Use Theirs, so it keeps `TakeConflictCaretRequest`. Not migrated; the reason is a comment.
- **Dual target.** Plain `Ui/` code, no GLFW / GL include; `SmatchetCore_DX12` compiles it like any other `Ui/` TU.
- Non-goals: the shortcut-capture popups (#77, #81) and Esc Esc in Zen mode (#93), which are their own debt bullets; the annotate assignee modal; the long-text editor's Esc rule.

## Implementation handoff

Single slice, owner subsystem `Source/Core/src/Ui/` (leaf `AGENTS.md`), plus `Source/Core/src/TicketFieldEditor_Modal.cpp` (a comment only). Write set = § Files to modify. Pre-resolved decisions: per-site choice above; no new strings (so no `SmatchetLocalization.cpp` edit); no `tests/CMakeLists.txt` edit (plan-lock `input-ux-w3-grid-s4`). Order: the scope option first (it is the only shared change), then the resolver, then shell + Comments together (they interlock), then the long-text comment, then tests.

## Verification

- **Bucket A (doctest)**: none new. `ResolveEscape` is unchanged and covered in `tests/Core/DialogKeysPure.test.cpp`; the typing-field option is a state definition read from ImGui, which only bucket E can observe.
- **Bucket E (ImGui Test Engine, `ui_test.run --spawn`)**: run the Wave 1 groups before and after (`OfflineFirst` with the offline-first fixture, `OfflineConflictModalPanes`, `DialogContract`, `FuncSizeMainUi`). New cases: (1) resolver — typed resolution, caret moved into the read-only Mine pane, Esc keeps the resolver open and shows the unsaved hint; Esc in the resolved field with nothing typed closes; (2) field viewer — Esc with the caret in an empty Comments box closes the viewer in one press, Esc with text in the box leaves it and a second Esc asks, and a whitespace-only box takes the same two presses; (3) the long-text editor is pinned by its existing `LongText_*` cases (no change). Each new case mutation-checked against the production line it covers.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` in CI; locally `posix-core-check` and a Linux `SmatchetStandalone` UI-test build.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test this plan against the domain model before finalising; record the outcome in § Verification (actual).
- **Manual residue**: none.

## Open questions

- Should the long-text editor take audit § 3's layered Esc (first Esc leaves the text area, second asks)? This plan says no, on the reasoning above. That is the author's call, not the user's: the PR body flags it, and flipping it is one argument (`nothingTyped = !dirty`) plus the `LongText_*` cases.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- Shortcut-capture popups (#77, #81), Esc Esc in Zen mode (#93), the annotate assignee modal's per-frame `ConfigManager::Load()`, the stale comments in `tests/CMakeLists.txt`: each stays its own bullet in the debt entry.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*The `git mv` is the step that reliably gets dropped. In the SAME PR that populates the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
   > **Keep the literal `<slug>` placeholder in this committed step — do NOT
   > expand it to this plan's real filename.**
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*No ref-sweep — references use the tier-less form `docs/plans/<slug>.md`.*

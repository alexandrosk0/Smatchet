# Plan — Input UX Wave 2 · 4.1 A dialog contract

> **Slug**: `input-ux-w2-dialog-contract` (matches this file's basename without `.md`).
>
> **Status**: `active` — Wave 2 piece 4.1 of [`docs/audits/INPUT_UX_AUDIT.md`](../../audits/INPUT_UX_AUDIT.md) § 4.1. Stacked on PR #2433 (Wave 1) until it merges.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

The input-UX audit (§ 4.1) found that dialogs open with no caret, most ignore Esc, confirms have no default button and reach the destructive one first, and app-wide shortcuts still fire under an open dialog. § 3 of the audit fixes the contract: a form dialog puts the caret in its first field, Ctrl+Enter saves, Esc cancels and focus goes back to the opener; a confirm puts focus on the safe button when it is destructive (on the primary otherwise), Enter activates the focused button, Esc cancels; global shortcuts stay off while a dialog is open.

Wave 1 (PR #2433) fixed five of the eighteen findings one dialog at a time (#37, #48, #73, #96, #109) and added the pure `smatchet::dialogkeys` decisions (`Ui/DialogKeysPure.h`). This plan builds the shared piece and moves the remaining dialogs onto it.

After this lands: every migrated dialog follows the § 3 contract through one helper, a dialog left on its own still mutes global shortcuts, and the helper is the default for new dialogs.

Findings, as of this plan (finding text in audit § 7):

| # | State | Where it closes |
|---|---|---|
| #37, #48, #73, #96, #109 | closed by #2433 | — |
| #92 | closed by #2433 (worklog Ctrl+Enter / Esc / caret) | — |
| #3, #43, #69 | owned by PR #2432 (shared `FilterDropdown`, first-frame focus + Enter-pick) | out of this write set; verified after #2432 merges |
| #85, #86, #89, #91, #94 | open | this plan, slices 2–3 |
| #88 | open | this plan, slice 1 |
| #120, #121 | open | this plan, slice 3 (toolbar editor) |
| #24 | open | this plan, slice 4, only once lock `input-ux-wave1-palette` is released |

## Approach

One RAII scope object, `smatchet::ui::DialogScope`, constructed as the first statement inside a `BeginPopupModal` body. On construction it reads this frame's keys once, before any widget draws, and keeps its cross-frame state in the popup window's own `ImGuiStorage` (so a dialog needs no new member state). It decides with the pure functions in `DialogKeysPure.h`: Esc → cancel (the existing two-stage `ResolveEscape` rule), Ctrl+Enter → the default action, and for confirms plain Enter → the focused button. Keys that are already down on the frame the dialog appears stay latched until they are released, so the Enter that opened a confirm is never read as "confirm" (the High bug the dropped local attempt hit). The scope stamps a per-frame "dialog live" marker that `SmatchetUI::dispatchKeybindings` reads, together with ImGui's own top-most-modal check, to mute global shortcuts (zoom and full-screen stay live). A small fixed registry (no allocation) remembers the window and item that had focus before the dialog and puts the focus back after the dialog leaves the popup stack, re-activating a text field the user was typing in.

`DrawConfirmFooter(scope, buttons)` draws the confirm row through the localized wrapper, paints a destructive button red (`SmatchetPushDestructiveButtonColors`), puts focus on the safe button (or the primary when nothing is destructive) on frame one, and maps Enter / Esc to a `ConfirmChoice`. Button order and labels stay as they are today, so ids, translations and screenshots do not move; only the focus, the keys and the red styling change.

Every new label reuses a source string that already has a French row; this keeps `SmatchetLocalization.cpp` (held by three other branches' locks) out of the write set. Bucket-E tests live in an already-registered UI test TU for the same reason (`tests/ui/CMakeLists.txt` and `ui_tests_registry.cpp` are locked elsewhere).

## Interface contracts

New shared header `Source/Core/include/Ui/SmatchetDialogScope.h` (needs this plan per `Source/Core/src/Ui/AGENTS.md` § Before you edit). No tracker, command, Lua, MCP, config, view-storage or SQLite surface changes.

```cpp
namespace smatchet { namespace ui {
struct DialogScopeOptions {
    bool confirm = false;        // plain Enter activates the focused / default button
    bool nothingTyped = true;    // false: the first Esc in a typed field only leaves it
};
class DialogScope {               // construct first thing inside a BeginPopupModal body
  public:
    explicit DialogScope(const DialogScopeOptions& options);
    ~DialogScope();               // records end-of-frame state in the popup's storage
    bool Appearing() const;       // frame one
    bool CancelRequested() const; // Esc resolved to Cancel this frame
    bool SubmitRequested() const; // Ctrl+Enter (not latched) while the dialog has the keyboard
    void FocusNextItem();         // caret request for the next item; waits while Enter is held
    void SkipFocusRestore();      // the dialog's action moves focus itself
};
enum class ConfirmChoice : unsigned char { None, Primary, Secondary, Cancel };
struct ConfirmButtons {           // labels are English source labels or already-translated T() text
    const char* primary; bool primaryDestructive; bool primaryEnabled = true;
    const char* secondary = nullptr; bool secondaryDestructive = false;
    const char* cancel;
};
ConfirmChoice DrawConfirmFooter(DialogScope& scope, const ConfirmButtons& buttons);
void DialogScopesNewFrame();      // once per frame, top of SmatchetUI::Draw: focus record + restore
bool GlobalShortcutsMuted();      // a modal is open or a scope was live last frame
int DialogScopeDuplicateBeginsForTests();
}}
```

Pure additions to `Source/Core/include/Ui/DialogKeysPure.h` (`smatchet::dialogkeys`): `KeyStaysLatched`, `ResolveSubmitChord`, `FooterFocus` + `ResolveConfirmEnter`, `ShortcutClass` + `ClassifyShortcut` + `ShortcutAllowedWhileMuted`, `FocusRestoreAction` + `ResolveFocusRestore`, `IsDuplicateBegin`.

Plumbing: `SmatchetUI::Draw` → `DialogScopesNewFrame()` (before `dispatchKeybindings`) → `dispatchKeybindings` consults `GlobalShortcutsMuted()` + `ClassifyShortcut(commandId)` per matched binding. Dialog bodies → `DialogScope` → `DrawConfirmFooter`. No other hop.

## Files to modify

New:
1. `Source/Core/include/Ui/SmatchetDialogScope.h` — the scope + footer API above.
2. `Source/Core/src/Ui/SmatchetDialogScope.cpp` — ImGui-internal mechanics (popup storage, key latch, nav-activation suppression, focus registry). Picked up by `CORE_SOURCES` glob.

Modified (slice 1 — helper):
3. `Source/Core/include/Ui/DialogKeysPure.h` — pure decisions.
4. `Source/Core/src/Ui/SmatchetUI.cpp` — `DialogScopesNewFrame()` call; `dispatchKeybindings` mute; update dialog (Esc = Later, focus restore).
5. `tests/Core/SmatchetWorklogSubmitPure.test.cpp` — doctest cases for the new pure decisions (this TU already hosts the `dialogkeys` tests).

Modified (slice 2 — confirms, #85/#86/#94):
6. `Source/Core/src/Ui/SmatchetViewsDashboardUi.cpp` — Discard changes? / Delete view? on the footer; the embedded (mobile) Views page no longer draws `drawViewsModals` a second time per frame (the shell already does).
7. `Source/Core/src/Ui/SmatchetOfflineQueueUi.cpp` — Discard queued work?
8. `Source/Core/src/Ui/SmatchetOfflineQueueUi_Actions.cpp` — Discard queued change?
9. `Source/Core/src/Ui/SmatchetPreferencesUi_General.cpp` — Delete local database?
10. `Source/Core/src/Ui/SmatchetPreferencesUi_Keybindings.cpp` — Reset keyboard shortcuts?
11. `Source/Core/src/Ui/SmatchetPreferencesUi.cpp` — Unsaved changes close guard.
12. `Source/Core/src/Ui/SmatchetAiAssistantUi.cpp` — Clear the conversation.
13. `Source/Core/src/Ui/SmatchetUI_MainMenu.cpp` — Reset window layout?

Modified (slice 3 — forms, #89/#91/#120/#121):
14. `Source/Core/src/Ui/SmatchetIconPickerUi.cpp` — caret in the search box, Esc cancels.
15. `Source/Core/src/Ui/SmatchetToolbarUi.cpp` (+ `SmatchetToolbarUi_detail.h`, `Source/Core/include/Ui/SmatchetToolbarUi.h`) — caret on open and after Add, Ctrl+Enter saves, Esc / Cancel with edits asks first.
16. `Source/Core/src/Ui/SmatchetBugReportUi.cpp` — its per-TU Esc moves to the scope.
17. `Source/Core/src/Tracker/TrackerDateTimeFieldEditor.cpp` — the picker's Esc reads with repeat off (a held Esc no longer closes the dialog under it).

Slice 4 (conditional):
18. `Source/Core/src/Commands/CommandPaletteUi.cpp` — focus back to the opener on close (#24). Only once `input-ux-wave1-palette` is released; otherwise deferred (§ Deviations).

Tests and docs:
19. `tests/ui/funcsize_main_ui_smoke.test.cpp` — the `DialogContract` bucket-E category (hosted in this registered TU; see § Risks).
20. `docs/guides/keyboard-shortcuts.md` — the dialog keys.
21. `Source/Core/src/Ui/AGENTS.md` — one invariant: new dialogs open a `DialogScope`.

### File-level changes

| File | Symbol |
|---|---|
| `Source/Core/src/Ui/SmatchetUI.cpp` | `dispatchKeybindings` |
| `Source/Core/src/Ui/SmatchetViewsDashboardUi.cpp` | `drawViewsModals` |
| `Source/Core/src/Ui/SmatchetViewsDashboardUi.cpp` | `drawMobileViewsModals` |
| `Source/Core/src/Ui/SmatchetOfflineQueueUi.cpp` | `DrawOfflineDiscardConfirmModal` |
| `Source/Core/src/Ui/SmatchetPreferencesUi_Keybindings.cpp` | `DrawResetDefaultsConfirm` |
| `Source/Core/src/Ui/SmatchetUI_MainMenu.cpp` | `drawLayoutResetConfirmModal` |
| `Source/Core/src/Ui/SmatchetIconPickerUi.cpp` | `SmatchetIconPickerUi::Draw` |
| `Source/Core/src/Ui/SmatchetToolbarUi.cpp` | `RenderEditor` |
| `Source/Core/include/Ui/DialogKeysPure.h` | `ResolveEscape` |

## Existing utilities reused

- `smatchet::dialogkeys::ResolveEscape` / `ResolveCaretRequest` (`Source/Core/include/Ui/DialogKeysPure.h`) — the Esc and caret rules Wave 1 settled; the scope calls them rather than restating them.
- `SmatchetPushDestructiveButtonColors` / `SmatchetPopDestructiveButtonColors` (`Source/Core/include/Ui/SmatchetDestructiveButton.h`) — the one red.
- `SmatchetLocalizedImGui::Button` / `SmatchetLocalization::LabelFromSource` — the footer keeps each button's `label##English` id and translation.
- `smatchet::ui::OpenCenteredModalPopup` (`Ui/SmatchetModalPopupOpen.h`) — unchanged; the scope sits after the matching `BeginPopupModal`.
- `ImGui::GetTopMostPopupModal` — catches a dialog that has not moved to the scope yet.
- The worklog's nav-activation suppression on Ctrl+Enter (`TicketFieldEditor_Worklog.cpp` `ReadWorklogKeys`) — generalised in the scope.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — nothing is extracted from an over-cap file; the new TU is new code.

## UX Pillar callouts

- **Pillar 1 (perf)**: per open dialog per frame a handful of storage reads and key polls; the focus registry is a fixed 8-slot array, no allocation. `dispatchKeybindings` adds one popup-stack walk per matched binding only.
- **Pillar 2 (UI never blocks)**: no I/O; nothing new reaches a worker.
- **Pillar 3 (never crash)**: the helper never calls `ClosePopupToLevel` (it asserts with no popup open) and never holds a window pointer across frames (window ids are re-resolved with `FindWindowByID`); a second `DialogScope` on the same popup in one frame is inert and counted, never double-acting.
- **Pillar 4 (accessibility)**: the point of the change — caret on open, Esc, Enter, a visible focus ring on the safe button, focus back where it was.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

1. **PR-fast CI** — fires as usual; no scenario exercises a dialog in the steady-state frame (dialogs are closed in every PR-fast scenario), so no budget move is expected.
2. **Pillar 2 static scanner** — N/A: no sync I/O added.
3. **Dispatcher drain** — N/A: `MainThreadDispatcher::Drain()` untouched.
4. **Visible-cue bucket-E harness** — N/A: no new stall path.
5. **Marker inventory** — N/A: no `SMATCHET_UI_PERF_SCOPE` markers added.

**Pre-push local check**: the CI PR-fast lane is authoritative; this container has no Windows perf rig (`project.config.json` § environments `linux-container`).

## Risks / non-goals

- **Held keys at open** — the Enter that opened a confirm read as "confirm" on frame one (High in the dropped local attempt). Mitigation: keys down on the appearing frame stay latched until released; bucket-E `DialogContract/Confirm_OpeningEnterDoesNotConfirm` and doctest `KeyStaysLatched` pin it.
- **Mobile Views modals drawn twice per frame** — `drawMobileViewsModals` (shell) and the embedded Views page both called `drawViewsModals` (High in the dropped attempt: one Enter acted twice). Mitigation: the embedded page skips it, the scope treats a second begin of one popup in a frame as inert and counts it; bucket-E asserts the count stays 0 with the Views confirm up.
- **Nav-cancel already cleared ActiveId** — ImGui's `NavUpdateCancelRequest` clears `ActiveId` inside `NewFrame` on Esc, so "was a field active" reads `g.ActiveIdPreviousFrame` against the id the scope stored last frame.
- **`ClosePopupToLevel` asserts with no popup open** — the helper closes only through `CloseCurrentPopup` inside the body.
- **Lock contention** — `SmatchetLocalization.cpp`, `tests/CMakeLists.txt`, `tests/ui/CMakeLists.txt` and `tests/ui/ui_tests_registry.cpp` are in the write sets of `shared-dropdown-recents` (#2432), `bulk-edit-rows` (#2434) and `input-ux-wave1-palette` (#2433). Mitigation: no new strings, tests hosted in already-wired TUs; push waits for #2433 to merge (stacked diff), never an override.
- **Dual target** — the new TU is plain Core `Ui/` code with no GLFW/GL include; `SmatchetCore_DX12` compiles it like any other `Ui/` TU.
- Non-goals: the shared text field (4.2), drafts / undo (4.3), keys for primary actions outside dialogs (4.4), parsing (4.5), pointer-gesture equivalents (4.6), the matcher (4.7); key hints inside button labels (they need new strings; see § Out of scope).

## Implementation handoff

Single owner subsystem: `Source/Core/src/Ui/` (leaf `AGENTS.md`), plus one line in `Source/Core/src/Tracker/TrackerDateTimeFieldEditor.cpp` (strict zone; the change is a repeat flag, no new code shape) and, conditionally, `Commands/CommandPaletteUi.cpp`.

1. Slice 1 — helper + pure logic + shortcut mute + doctest. Lands first; everything else calls it.
2. Slice 2 — confirms onto `DrawConfirmFooter`, the mobile double-draw fix.
3. Slice 3 — form dialogs (icon picker, toolbar editor, bug report, update dialog, date picker Esc).
4. Slice 4 — palette focus restore, only if its lock is free at push time.

Pre-resolved decisions: button order and labels unchanged (ids, translations and goldens stay); Esc in a confirm is always the safe button, never the destructive one; a two-button non-destructive confirm defaults to the primary; the three-button Views and Preferences guards default to the non-destructive primary ("Save & switch", "Save & Sync").

## Verification

- **Bucket A (doctest, `tests/Core/SmatchetWorklogSubmitPure.test.cpp`)**: every new `dialogkeys` decision, each case mutation-checked (flip the decision, see the case fail).
- **Bucket E (`DialogContract` category in `tests/ui/funcsize_main_ui_smoke.test.cpp`)**: first-field focus; Esc cancels; Ctrl+Enter submits; Enter confirms the focused button; the safe button has focus on a destructive confirm; the opening Enter does not confirm; global shortcuts are muted under a dialog and zoom is not; focus and the text caret come back after close; Views delete confirm draws once per frame on the mobile Views page; toolbar editor asks before dropping edits. Each case mutation-checked.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual target) runs in CI (Windows); locally `posix-core-check` compiles every Core TU with clang.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: run against this plan before finalising; outcome recorded in § Verification (actual).
- **Manual residue**: none planned; bucket E covers the keyboard paths.

## Open questions

None — the lock-contention choices are settled by the brief (narrow or wait, never override).

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- Key hints on dialog buttons ("Every dialog button that has a key shows it", audit § 3): needs new strings in `SmatchetLocalization.cpp`; follow-up once that file's locks clear.
- A dedicated `tests/ui/dialog_contract.test.cpp` and `tests/Core/DialogKeysPure.test.cpp`: move the cases there once the test lists' locks clear.
- Remaining Esc-dead modals not listed above (About, bulk import project picker, annotate assign / profile, Lua unsaved switch, AI endpoint consent): their keys are muted and focus-restored through the modal check; moving each onto the scope is a follow-up debt entry.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*The `git mv` is the step that reliably gets dropped (empirically ~62% of post-ship plans drifted stale-in-place). Bind it to the impl-log write: in the SAME PR that populates the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
   > **Keep the literal `<slug>` placeholder in this committed step — do NOT
   > expand it to this plan's real filename.**
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*No ref-sweep — references use the tier-less form `docs/plans/<slug>.md`.*

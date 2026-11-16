# Plan — Input UX Wave 2: one dialog contract (audit § 4.1)

> **Slug**: `input-ux-w2-dialog-contract` (matches this file's basename without `.md`).
>
> **Status**: `active` — slices 1 and 2 (in part) shipped in the stacked PR; slices 3 and 4 open.
>
> **Source**: `docs/audits/INPUT_UX_AUDIT.md` § 4.1 "A dialog contract" and § 3 "Proposed keyboard contract" (the audit lands with PR #2433; cite findings as `INPUT_UX_AUDIT.md #N`). Sibling Wave 2 plans: `input-ux-w2-text-field` (§ 4.2), `input-ux-w2-drafts-undo` (§ 4.3), `input-ux-w2-action-keys` (§ 4.4), `input-ux-w2-tolerant-parsing` (§ 4.5), `input-ux-w2-gesture-keys` (§ 4.6), `input-ux-w2-one-matcher` (§ 4.7).

## Context

Every modal dialog in the desktop client decides its own keys. Esc has three meanings (leave the field, cancel at once, or nothing); most `BeginPopupModal` sites ignore Esc entirely because ImGui 1.92.8's nav-cancel deliberately skips modals (`NavUpdateCancelRequest` closes the top popup only when it lacks `ImGuiWindowFlags_Modal`). Confirm dialogs have no default button and draw the destructive action first, so the first Tab lands on "Delete". Only two of the destructive confirms are red. App-wide shortcuts (`SmatchetUI::dispatchKeybindings`) still fire under an open modal, so Ctrl+B or Ctrl+, change state behind a "Delete view?" prompt and can invalidate its pending target (`viewsPendingDeleteId`).

Wave 1 (PR #2433, PR #2435, and the shared dropdown in PR #2432) already fixed the daily dialogs one at a time: the worklog dialog, long-text editor, field viewer, comments composer, offline conflict resolver, assistant panel and Preferences now take the caret on open and follow a layered Esc, through the pure decisions in `Ui/DialogKeysPure.h` (`ResolveEscape`, `ResolveCaretRequest`, `DismissedPopupComesBack`) and `OpenOrBringBackModalPopup` in `Ui/SmatchetModalPopupOpen.h`. The shared `FilterDropdown` (PR #2432) focuses its filter on open, which covers the labels popup, project picker and multi-select combos. What is left is the long tail of modals that nobody migrated, the confirm dialogs, the global-shortcut leak and focus return on close, plus one helper so the next dialog gets the contract for free.

**Intended outcome — after this lands:** every first-party modal opens with the caret in its first field (or focus on the safe button of a destructive confirm), Esc cancels it under one rule, Enter / Ctrl+Enter run its default action, app-wide shortcuts are muted while it is up, and closing it hands focus back to where the user was. Findings closed: #3 (icon picker part), #24, #85, #86, #88, #89, #91, #94, #120, #121 (key half); #37, #43, #48, #69, #73, #92, #96, #109 are verified closed by Wave 1 and re-pointed at the shared helper where the edit is mechanical.

## Approach

Add one ImGui-side dialog scope, `Source/Core/include/Ui/SmatchetDialogScope.h` (+ `Source/Core/src/Ui/SmatchetDialogScope.cpp`), layered on the Wave 1 pieces rather than beside them. A caller-owned `DialogScopeState` per dialog carries the open latch, the pending caret request, "owned the keyboard last frame", the opener's window and active-item ids, and the bring-back frame. `BeginDialog` / `EndDialog` wrap `SmatchetLocalizedImGui::BeginPopupModal` / `EndPopup`; between them the dialog calls `DialogFocusFirstField(scope)` right before its primary input and reads one `DialogKeys` result (Esc action, submit, Ctrl+Enter) instead of calling `IsKeyPressed` itself. All decisions stay in `DialogKeysPure.h` (extended, not forked), so they are bucket-A testable without a frame. A `ConfirmFooter` helper draws the button pair in the platform order (safe action first, then primary; destructive primary red via `SmatchetPushDestructiveButtonColors`), puts default focus on the safe button for a destructive confirm and on the primary otherwise, and maps Enter → focused button, Esc → cancel.

Global mute is one check in `dispatchKeybindings`: when `ImGui::GetTopMostPopupModal()` is non-null, skip every binding except a short pure allow-list (`ui.zoom.*`, `app.fullscreen.toggle`). The command palette pseudo-binding is muted too: the palette is not a modal, and opening it over a confirm is the exact leak #88 describes. Focus return records the opener in `DialogOpen` and, on the close frame, relies on ImGui's own `ClosePopupToLevel(…, restore_focus_to_window_under_popup=true)` for window + nav item, adding only the missing piece: re-activating a text field that held the caret when the dialog opened (`ImGui::ActivateItemByID` next frame, bounded by `FocusRequestStaysArmed`).

Trade-off: a wrapper (`BeginDialog`) over a free function called after each `BeginPopupModal` (`ModalEscCancelRequested()`). The wrapper is chosen because focus return and the "owned last frame" bookkeeping need a hook on both open and close; a per-site free function would recreate the Wave 1 per-TU copies this plan exists to delete.

## Interface contracts

> **As shipped in slice 1, the API differs from the sketch below.** `SmatchetDialogScope.h` is the source of truth. The differences and the reason for each are in § Deviations from plan.

New shared header under `Source/Core/include/Ui/` (widens a shared contract per `Source/Core/src/Ui/AGENTS.md` § Before you edit — this plan is that approval request).

```cpp
// Source/Core/include/Ui/SmatchetDialogScope.h  (ImGui-side; includes SmatchetLocalizedImGui.h, DialogKeysPure.h)
namespace smatchet { namespace ui {

enum class DialogKind : unsigned char { Form, Confirm, DestructiveConfirm };

struct DialogScopeState {              // caller-owned, one per dialog, lives as long as the dialog's owner
    bool openRequested = false;        // set by DialogOpen, consumed by BeginDialog
    bool caretPending = false;         // DialogFocusFirstField takes it when ResolveCaretRequest says Take
    bool ownedKeyboardLastFrame = false;
    int lastReopenFrame = -1;          // for DismissedPopupComesBack
    ImGuiID openerWindowId = 0;        // NavWindow->ID when DialogOpen ran
    ImGuiID openerActiveId = 0;        // GetActiveID() when DialogOpen ran (non-zero: a text field had the caret)
    int focusRestoreRetries = 0;       // FocusRequestStaysArmed budget
    bool restorePending = false;
};

struct DialogKeys {
    dialogkeys::EscapeAction esc = dialogkeys::EscapeAction::None;
    bool submit = false;               // Enter on a single-line field with EnterReturnsTrue semantics, or Ctrl+Enter anywhere
};

// Records the opener and requests the popup (centred via OpenCenteredModalPopup when widthFrac > 0).
void DialogOpen(DialogScopeState& s, const char* popupId, float widthFrac = 0.f, float heightFrac = 0.f);
// Wraps SmatchetLocalizedImGui::BeginPopupModal. Returns false when not open; arms the restore on the close edge.
bool BeginDialog(DialogScopeState& s, const char* popupId, bool* pOpen, ImGuiWindowFlags flags);
// Call immediately before the dialog's primary input widget.
void DialogFocusFirstField(DialogScopeState& s);
// Call once per frame inside the dialog, after its fields. `nothingTyped`: the dialog holds no unsaved text.
DialogKeys DialogReadKeys(DialogScopeState& s, bool nothingTyped);
void EndDialog(DialogScopeState& s);   // EndPopup + ownedKeyboardLastFrame bookkeeping
// Call every frame from the dialog owner, outside BeginDialog; services a pending focus return.
void DialogServiceFocusReturn(DialogScopeState& s);

// Draws [Cancel-label] [Primary-label]; default focus per DialogKind; Enter = focused button; Esc = Cancel.
dialogkeys::ConfirmPick ConfirmFooter(DialogScopeState& s, DialogKind kind, const char* primaryLabel,
                            const char* cancelLabel = nullptr /* "Cancel" via SmatchetLocalization */);

// True while any modal is up (ImGui::GetTopMostPopupModal() != nullptr). For dispatchKeybindings.
bool AnyModalDialogOpen();

}} // namespace smatchet::ui
```

Additions to `Source/Core/include/Ui/DialogKeysPure.h` (pure, no ImGui):

```cpp
enum class ConfirmDefault : unsigned char { Safe, Primary };
enum class ConfirmPick : unsigned char { None, Primary, Cancel };
inline ConfirmDefault ResolveConfirmDefault(bool destructive);              // destructive → Safe
// Enter activates the focused button; with nothing focused it activates the default. Never Primary on a
// destructive confirm unless the primary button itself holds focus.
inline ConfirmPick ResolveConfirmEnter(bool enterPressed, bool safeFocused, bool primaryFocused, bool destructive);
// Ctrl+Enter submits from anywhere in a form; plain Enter submits only from a single-line field that asked for it.
inline bool ResolveSubmit(bool enterPressed, bool ctrlHeld, bool fieldIsSingleLineSubmit, bool submitEnabled, bool inFlight);
// Commands that still fire under a modal.
inline bool GlobalShortcutAllowedUnderModal(const char* commandId);          // "ui.zoom.*", "app.fullscreen.toggle"
```

No change to `ITrackerBackend`, `RegisterCommand`, Lua bindings, MCP schemas, view storage, config or SQLite formats. No new localization keys beyond the footer defaults (`dialog.cancel`, `dialog.keep_editing`) and the key-hint suffixes, routed through `SmatchetLocalization::T`.

Plumbing (keybinding mute): `SmatchetUI::Draw` → `dispatchKeybindings(app, d)` (`SmatchetUI.cpp`) → per-binding loop → new `if (AnyModalDialogOpen() && !GlobalShortcutAllowedUnderModal(pk.commandId.c_str())) continue;` placed after the alias de-dup and before the `ui.command_palette` pseudo-binding. No signature widens.

## Files to modify

Line numbers are against the Wave 1 tip `b12bdb424` (PR #2433); this plan is implemented on top of that branch (see § Implementation handoff, Order).

**Shared helper (slice 1)**

1. `Source/Core/include/Ui/DialogKeysPure.h` — add `ResolveConfirmDefault`, `ResolveConfirmEnter`, `ResolveSubmit`, `GlobalShortcutAllowedUnderModal`.
2. `Source/Core/include/Ui/SmatchetDialogScope.h` — new; the API above. (`rg -l 'DialogScope' Source/Core/` returns nothing on develop or the Wave 1 tip.)
3. `Source/Core/src/Ui/SmatchetDialogScope.cpp` — new; picked up by `GLOB_RECURSE CORE_SOURCES`.
4. `Source/Core/src/Ui/SmatchetUI.cpp:1090` `dispatchKeybindings` — the modal mute (#88).
5. `tests/Core/DialogKeysPure.test.cpp` — new bucket-A file; add to `add_executable(SmatchetTests …)` in `tests/CMakeLists.txt`.

**Confirm dialogs (slice 2) — `ConfirmFooter`, Esc, default focus, red primary (#85, #86, #91, #94)**

6. `Source/Core/src/Ui/SmatchetViewsDashboardUi.cpp:966` "Discard changes?" (three buttons: Save & switch is primary, Discard & switch is a destructive secondary — footer gets an optional third button) and `:998` "Delete view?".
7. `Source/Core/src/Ui/SmatchetOfflineQueueUi.cpp:1706` "Discard queued work?".
8. `Source/Core/src/Ui/SmatchetOfflineQueueUi_Actions.cpp:209` "Discard queued change?".
9. `Source/Core/src/Ui/SmatchetPreferencesUi.cpp:1695` "Unsaved changes" close guard.
10. `Source/Core/src/Ui/SmatchetPreferencesUi_General.cpp:354` "Delete local database?".
11. `Source/Core/src/Ui/SmatchetPreferencesUi_Keybindings.cpp:176` "Reset keyboard shortcuts?".
12. `Source/Core/src/Ui/SmatchetPreferencesUi_Assistant.cpp:466` "Allow custom AI endpoint?" (non-destructive; default Cancel anyway — it widens a trust boundary).
13. `Source/Core/src/Ui/SmatchetUI_MainMenu.cpp:482` "Reset window layout?".
14. `Source/Core/src/Ui/SmatchetAiAssistantUi.cpp:1452` "##ConfirmClearChat" and `:1118` outbound-consent (consent defaults to Cancel).
15. `Source/Plugins/LuaConsole/LuaConsolePlugin.cpp:847` "##lua_unsaved_switch".
16. `Source/Core/src/TicketFieldEditor_Modal.cpp:549` long-text discard and `Source/Core/src/Ui/SmatchetFieldViewerModalUi.cpp:147` field-viewer discard — already Esc-keeps; migrate only the footer drawing so wording and colour match.

**Form and info dialogs (slice 3) — caret on open, Esc, Enter/Ctrl+Enter, focus return (#3, #89, #91, #120, #121)**

17. `Source/Core/src/Ui/SmatchetIconPickerUi.cpp:34` — focus `##iconsearch` on open; Enter picks the single / highlighted match; Esc cancels.
18. `Source/Core/src/Ui/SmatchetToolbarUi.cpp:873` toolbar editor — caret on open and after Add command (#120); Ctrl+Enter saves, Esc follows the scope (#121 key half; the dirty guard is the drafts plan's).
19. `Source/Core/src/Ui/SmatchetUI.cpp:208` "Update Available" and `Source/Core/src/Ui/SmatchetAboutUi.cpp:456` About — Esc closes; default focus on the primary.
20. `Source/Core/src/Ui/SmatchetBulkTicketsUi.cpp:372` bulk-import project modal.
21. `Source/Core/src/Ui/AnnotateAnalysisUi_Window.cpp:1076` "annotate_assign" and `:1124` "Jira user profile".
22. `Source/Core/src/Tracker/TrackerDateTimeFieldEditor.cpp:432` — `IsKeyPressed(ImGuiKey_Escape)` with repeat → scope (a held Esc no longer closes the dialog beneath, #91).
23. `Source/Core/src/Ui/SmatchetActiveProjectGridUi.cpp:639` "Save view as new" — already the model; migrate for focus return only.
24. `Source/Core/src/Commands/CommandPaletteUi.cpp` `Open` / `Close` — record and restore the opener (#24). The palette is not a modal; it uses `DialogScopeState` for the opener bookkeeping only.

**Wave 1 sites (slice 4) — re-point at the scope where mechanical, no behaviour change**

25. `Source/Core/src/TicketFieldEditor_Worklog.cpp:633`, `Source/Core/src/TicketFieldEditor_Modal.cpp:815`, `Source/Core/src/Ui/SmatchetFieldViewerModalUi.cpp:109`, `Source/Core/src/Ui/SmatchetOfflineQueueUi.cpp:1649`, `Source/Core/src/Ui/SmatchetCommentsModalUi.cpp` — replace the per-TU `ResolveEscape` / `ResolveCaretRequest` plumbing with `DialogReadKeys` / `DialogFocusFirstField`. Skip any site where the swap is not a straight substitution; record it in § Deviations.

**Docs**

26. `docs/guides/keyboard-shortcuts.md` — the dialog rows of the § 3 contract (Form dialog, Confirm dialog) and "shortcuts are muted while a dialog is open".
27. `Source/Core/src/Ui/AGENTS.md` § Before you edit — one bullet: a new modal goes through `BeginDialog` / `ConfirmFooter`.

### File-level changes

| Symbol | File |
|---|---|
| `smatchet::dialogkeys::ResolveConfirmDefault` | `Source/Core/include/Ui/DialogKeysPure.h` |
| `smatchet::dialogkeys::ResolveConfirmEnter` | `Source/Core/include/Ui/DialogKeysPure.h` |
| `smatchet::dialogkeys::ResolveSubmit` | `Source/Core/include/Ui/DialogKeysPure.h` |
| `smatchet::dialogkeys::GlobalShortcutAllowedUnderModal` | `Source/Core/include/Ui/DialogKeysPure.h` |
| `smatchet::ui::BeginDialog` / `EndDialog` / `DialogOpen` / `DialogReadKeys` / `DialogFocusFirstField` / `DialogServiceFocusReturn` / `ConfirmFooter` / `AnyModalDialogOpen` | `Source/Core/include/Ui/SmatchetDialogScope.h` |

## Existing utilities reused

- `smatchet::dialogkeys::ResolveEscape`, `ResolveCaretRequest`, `DismissedPopupComesBack` — `Source/Core/include/Ui/DialogKeysPure.h` (Wave 1): the Esc / caret / bring-back rules; the scope calls them, it does not restate them.
- `smatchet::ui::OpenCenteredModalPopup`, `OpenOrBringBackModalPopup` — `Source/Core/include/Ui/SmatchetModalPopupOpen.h`: `DialogOpen` delegates to them, keeping the `SmatchetLocalizedImGui::OpenPopup` id-hash rule.
- `smatchet::ui::FocusRequestStaysArmed` — `Source/Core/include/Ui/SmatchetFocusRequest.h:35`: bounds the focus-return retry.
- `SmatchetPushDestructiveButtonColors` / `SmatchetPopDestructiveButtonColors` — `Source/Core/include/Ui/SmatchetDestructiveButton.h:13`: the one red.
- `ImGui::GetTopMostPopupModal` (imgui_internal, 1.92.8) — modal-up test for the mute.
- `smatchet::ui::HotkeyCaptureArmedRecently` — the existing early-return in `dispatchKeybindings`; the mute sits next to it.
- `FilterDropdown` (PR #2432) — already focuses its filter on open; this plan does not touch it.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — no extraction; the plan adds a helper and migrates call sites to it.

## UX Pillar callouts

- **Pillar 1 (perf)**: no impact — a handful of bool checks and one `GetTopMostPopupModal()` (walks the open-popup stack, usually depth ≤ 2) per frame.
- **Pillar 2 (UI-thread never blocks)**: no impact — no I/O; every dialog's existing async work is untouched.
- **Pillar 3 (never crash)**: `DialogScopeState` holds ids, never `ImGuiWindow*`, so a window destroyed while a dialog is up cannot dangle; `ActivateItemByID` on a stale id is a no-op.
- **Pillar 4 (accessibility)**: the point of the plan — every dialog completes from the keyboard, the safe button has the visible nav focus ring on a destructive confirm, and key hints are printed on the buttons.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

1. **PR-fast CI** — fires; nearest scenario is the frame-loop baseline in `scripts/dev/perf-pr-fast-set.json` (no scenario opens a modal; the mute check runs every frame in `dispatchKeybindings`, so the idle-frame scenario is the one to watch). Name the exact scenario id from `scripts/dev/perf-scenario-map.json` at implementation time.
2. **Pillar 2 static scanner** — N/A: no sync I/O added.
3. **Dispatcher drain** — N/A: `MainThreadDispatcher::Drain()` untouched.
4. **Visible-cue bucket-E harness** — N/A: no new stall path.
5. **Marker inventory** — N/A: no `SMATCHET_UI_PERF_SCOPE` markers added.

**Pre-push local check**: `docs/guides/perf-workflow.md` § Gate-check vs baseline against the scenario named in gate 1.

## Risks / non-goals

- **Collides with Wave 1 in nearly every file.** PR #2433 / #2435 / #2432 rewrite the worklog, comments, offline-queue, Preferences, assistant, toolbar, palette and picker files. Mitigation: implement on top of the merged Wave 1 (Order below); slice 4 is optional and dropped site by site if not a straight substitution.
- **ImGui Esc on the frame it closes a child popup.** ImGui's nav-cancel closes a non-modal popup (a combo, a date picker) before the dialog draws, so the same Esc can also reach the dialog. Mitigation: `ownedKeyboardLastFrame` in `ResolveEscape` (Wave 1 rule) plus `IsKeyPressed(…, repeat=false)` everywhere.
- **Button order change.** Moving the safe button first changes muscle memory on the confirms in slice 2. Accepted: it is the platform order the audit asks for, and the default focus means Enter is never destructive.
- **Muting the palette under a modal.** A user who opens the palette from a dialog loses that. Accepted: no dialog depends on it; `GlobalShortcutAllowedUnderModal` is one place to widen.
- **Focus return to a widget that no longer exists** (the row was re-sorted, the pane closed). Mitigation: bounded retries via `FocusRequestStaysArmed`; ImGui ignores an unknown id.
- **Dual-target.** `SmatchetCore_DX12` compiles `Source/Core/`; the helper uses only `imgui.h` / `imgui_internal.h` already in both. Build both targets.
- **Localization.** Footer labels and hints route through `SmatchetLocalization::T`; French rows added with the strings (Wave 1 precedent `b12bdb424`).
- **Non-goals:** dirty-guard / draft persistence on Esc (`input-ux-w2-drafts-undo`); the shared text field (`input-ux-w2-text-field`); printing every key on every button outside dialogs (`input-ux-w2-action-keys`); fuzzy matching in pickers (`input-ux-w2-one-matcher`); the hotkey-capture popups (#77, #81, Preferences surface); the mobile shell and the Unreal-embedded input path.

## Implementation handoff

Owner subsystem for every slice: `Source/Core/src/Ui/` (`Source/Core/src/Ui/AGENTS.md`), except slice 2's Lua console row (`Source/Plugins/LuaConsole`) and slice 3's date-time row (`Source/Core/src/Tracker/AGENTS.md` — a keys-only change, no catalog / parser / payload touch).

- **Slice 1 — helper + mute.** Write set: rows 1–5, 26, 27. Pre-resolved: Esc rule = Wave 1 `ResolveEscape` unchanged; repeat=false; the palette pseudo-binding is muted under a modal. Bucket-A tests land in this slice.
- **Slice 2 — confirm dialogs.** Write set: rows 6–16. Pre-resolved: safe button first; destructive default focus = safe; non-destructive default focus = primary, except the AI endpoint and outbound-consent prompts (trust boundary → Cancel); safe label is "Cancel", or "Keep editing" only on a dirty-close guard (#94). Inline inventory: the 12 `BeginPopupModal` confirm sites listed in rows 6–16.
- **Slice 3 — form and info dialogs + palette focus return.** Write set: rows 17–24.
- **Slice 4 — Wave 1 re-point (optional).** Write set: row 25. Behaviour-preserving; each site's existing bucket-E test must stay green unchanged.
- **Order.** PR #2432, #2433 and #2435 merge first (they own the files and `DialogKeysPure.h`). Slice 1 before 2–4; 2 and 3 are independent of each other once 1 lands; 4 last. One PR per slice, or 1+2 together if slice 1 alone has no user-visible change worth reviewing on its own.

## Verification

- **Bucket A (pure-logic doctest)**: `tests/Core/DialogKeysPure.test.cpp` — `ResolveConfirmDefault` (destructive → Safe), `ResolveConfirmEnter` (nothing focused → default; destructive with safe focused → Cancel; primary focused → Primary), `ResolveSubmit` (Ctrl+Enter from multiline; plain Enter only from a submit field; blocked while in flight or disabled), `GlobalShortcutAllowedUnderModal` (zoom / fullscreen allowed; palette, sidebar toggle, preferences refused). Mutation check: flip each rule, the matching case fails.
- **Bucket E (ImGui Test Engine, `ninja-ui-test-msvc`)**: one test per confirm kind and per form dialog — open, assert the focused id (safe button / first field), press Esc → closed with no action taken, reopen, press Enter → the default ran; for "Delete view?" assert Enter does not delete. A `DialogMute` test: open "Delete view?", press Ctrl+Shift+P and Ctrl+B, assert the palette did not open and the layout flag did not change. A focus-return test: caret in the grid search box, open the palette, Esc, assert the search box is active again.
- **Bash-driver scenario / screenshot**: bucket-C screenshot diff of the confirm footers (button order and red primary) through the existing screenshot lane; regenerate goldens in the same PR.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target).
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: run against this plan before slice 1 starts; record the outcome here.
- **Manual residue**: none planned. If the ImGui Test Engine cannot read the nav focus ring state, the assertion falls back to `GetFocusID()`; any residue gets a `docs/self-improvement/categories/tooling.md` entry.

## Open questions

- Does `ActivateItemByID` on the frame after `CloseCurrentPopup` reliably put the caret back in an `InputText` under 1.92.8, or does ImGui's own restore race it? — probe: the slice 3 focus-return bucket-E test, written first.
- Should the palette stay reachable under a modal for a user who drives everything from it? — user; the plan's default is muted.
- "Discard changes?" in the Views dashboard has three buttons (Save & switch, Discard & switch, Cancel). Keep three, with Save & switch as default, or split? — owner subsystem; default here is keep three.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- Dirty guard on Esc / Cancel for the toolbar editor (#121 lost-work half) and preferences — `input-ux-w2-drafts-undo`.
- Undo toast replacing "cannot be undone" confirms (#90) — `input-ux-w2-drafts-undo`.
- Key hints on every primary button outside dialogs (#59, #95) — `input-ux-w2-action-keys`.
- Shortcut-capture popups (#77, #81) — follow-up; they are captures, not dialogs.
- Esc Esc leaving Zen mode while a popup is open (#93) — follow-up in the cross-cutting surface; touches `SmatchetUI.cpp:999`.

## Implementation log
*(bullet per shipped commit: `<sha> · <one-line summary>`)*

- `112aa561e` · wip(plan): this plan.
- `c1b6f43f1` · merge of `origin/claude/frictionless-user-input-ux-ea174b` (PR #2433, head `b12bdb424`) for `Ui/DialogKeysPure.h`.
- Slice 1 + the slice 2 confirms + the icon picker (slice 3, row 17) · `SmatchetDialogScope` helper, `ConfirmFooter`, the modal shortcut mute, 11 confirm sites, tests and docs (this PR's feature commit).

## Deviations from plan

**Scope of this PR.** Slice 1 and slice 2 ship together (the § Implementation handoff allows 1+2 in one PR), plus row 17 (icon picker) from slice 3. It is stacked on PR #2433, not on #2435 / #2432. Dialogs in files those two PRs change stay for a later PR.

**API changes against § Interface contracts**

- **Button order is `[Primary][Alternate][Cancel]`**, action first, not safe first. The existing confirms already put the action first, and P2-M16 keeps that order. Moving every button would retrain users for no gain. Enter safety comes from the default focus instead: a destructive or consent primary starts with the keyboard on Cancel.
- **`DialogKind` dropped.** `ConfirmFooterSpec` takes `PrimaryDestructive`, `AlternateDestructive`, `SafeByDefault` (consent prompts) and `PrimaryEnabled` flags. Three kinds could not say "the alternate is destructive" ("Discard & switch") or "non-destructive but default to Cancel" (consent).
- **`ConfirmPick` gains `Alternate`** for the three-button confirms (Views "Discard changes?", Preferences "Unsaved changes", Lua console unsaved switch). The open question on "Discard changes?" is settled: three buttons, Save & switch is the default.
- **Pure signatures:** `ResolveConfirmDefault(bool primaryDestructive, bool safeByDefault)` and `ResolveConfirmEnter(bool enterPressed, bool dialogOwnsKeyboard, bool navItemTakesEnter, ConfirmDefault def)`. ImGui itself activates the button under a visible keyboard cursor on Enter, so the pure rule only decides the case where no button does.
- **`ResolveSubmit` not added.** Ctrl+Enter is read by `DialogReadKeys`. Plain Enter in a single-line field stays the field's own `EnterReturnsTrue`, as the icon picker uses it. A pure rule with no caller is dead code; it lands with the first form dialog that needs it.
- **`DialogOpen`, `DialogServiceFocusReturn` and the opener / restore fields dropped** from `DialogScopeState`, which now holds `Open`, `FocusPending`, `OwnedKeyboardLastFrame` and `FieldActiveLastFrame`. Focus return (#24, #3) and its open question (`ActivateItemByID` after `CloseCurrentPopup`) move to the slice 3 PR, which has the palette and the form dialogs that need it. `BeginDialog` detects a new open by itself (`IsWindowAppearing`), so call sites keep their own `OpenPopup`.
- **`DialogFocusFirstField` is named `DialogFocusHere`.** `ConfirmFooter` uses it for the default button, not only for a first field.
- **The mute is the first check after `MatchHotkey`** in `dispatchKeybindings`, not after the alias de-dup. A muted wheel combo then leaves the wheel to the dialog, and a muted combo does not count as its action's firing in the de-dup.
- **Spec factories added:** `DestructiveFooter`, `ConsentFooter` and `UnsavedChangesFooter` build the common `ConfirmFooterSpec`s. Without them the call sites were copy-paste clones (`dup_audit`, DRY Pillar 5).
- **Enter and Ctrl+Enter are gated like Esc** (`ResolveConfirmEnter` takes `dialogOwnedKeyboardLastFrame`; `DialogReadKeys` gates Submit the same way). ImGui focuses a new popup on the frame it opens, so the Enter that opened a confirm also answered it: the AI first-send consent closed unseen, and the Lua console saved over the script without asking. The code-review pass found this.
- **Mobile Views: the modals draw once per frame.** `drawViewsDashboardWindow(embedded)` no longer calls `drawViewsModals`; the shell's `drawMobileViewsModals` already does, every frame. Two calls under two ID stacks reset the shared dialog state each frame, so the opening focus was re-placed every frame. The code-review pass found this too.

**Tests, localization and locked files**

- **Bucket-A cases live in `tests/Core/SmatchetWorklogSubmitPure.test.cpp`** (9 new cases), not in a new `tests/Core/DialogKeysPure.test.cpp`. A new file needs a `tests/CMakeLists.txt` row, and that file is in the `bulk-edit-rows` plan-lock. That file already holds the Wave 1 `DialogKeysPure.h` cases. Move them when the lock frees.
- **Bucket-E tests live in `tests/ui/mobile_views_confirm_modal.test.cpp`** (3 new `DialogContract/*` tests), not in a new file. `tests/ui/CMakeLists.txt` and `tests/ui/ui_tests_registry.cpp` are locked by `bulk-edit-rows` and `input-ux-wave1-palette`. The tests cover Delete view (safe focus, Enter and Esc cancel), Discard changes (default on the action, Esc cancels and keeps edits) and the shortcut mute (Ctrl+Shift+P, with a no-dialog control run first). Ctrl+B is not asserted: the mobile shell has no sidebar layout flag to read.
- **No new localization rows.** `SmatchetLocalization.cpp` is locked by `bulk-edit-rows` and `input-ux-wave1-palette`. Every footer keeps the label it had before, which is already in the table. No `dialog.cancel` / `dialog.keep_editing` keys were added.
- **No key-hint suffixes on buttons** — `input-ux-w2-action-keys` owns them.
- **No bucket-C screenshot diff of the footers.** Button order did not change. Only the button that starts with keyboard focus did, and the bucket-E `NavId` asserts cover that.

**Deferred migrations** (later PRs; each row names what holds it)

- Row 16: `TicketFieldEditor_Modal.cpp` long-text discard and `SmatchetFieldViewerModalUi.cpp` field-viewer discard. Both already keep text on Esc. The footer-only swap goes with slice 4, which changes the same two files.
- Row 18: toolbar editor (`SmatchetToolbarUi.cpp`) — slice 3, it needs Ctrl+Enter save plus a caret after Add command.
- Row 19: "Update Available" (`SmatchetUI.cpp`) and About (`SmatchetAboutUi.cpp`) — slice 3.
- Row 20: bulk-import project modal (`SmatchetBulkTicketsUi.cpp`) — slice 3.
- Row 21: `annotate_assign` and "Jira user profile" (`AnnotateAnalysisUi_Window.cpp`) — slice 3.
- Row 22: date-time picker held-Esc (`TrackerDateTimeFieldEditor.cpp`) — slice 3.
- Row 23: "Save view as new" (`SmatchetActiveProjectGridUi.cpp`) — locked by `bulk-edit-rows`, and its only change is focus return (slice 3).
- Row 24: palette focus return (`CommandPaletteUi.cpp`, #24) — locked by `input-ux-wave1-palette`, and #2435 changes `BuiltinCommands_Ui.cpp`. Slice 3, after #2433 and #2435 merge.
- Row 25: Wave 1 re-point (slice 4) — after #2432 and #2435 merge (`TicketFieldEditor.cpp` is #2432's).
- Dialogs in #2435's files (`SmatchetNewIssueDraftUi.cpp`, `SmatchetGridHeaderUi.cpp`, `SmatchetQuickCreateIssueUi.cpp`, `SmatchetMobileShellUi.cpp`, `SmatchetGridPaneWindows.cpp`) and #2432's (`FilterDropdown*`, `TicketFieldEditor.cpp`) — after those PRs merge.

## Verification (actual)

- **Build (dual-target)**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` — passed.
- **Bucket A**: `ninja-test-msvc` `SmatchetTests.exe` — 4161 / 4161 cases passed (includes the 10 new `ResolveConfirmDefault` / `ResolveConfirmEnter` / `GlobalShortcutAllowedUnderModal` cases).
- **Bucket E**: `ninja-ui-test-msvc`, `ui_test.run --spawn`: `DialogContract` 5 / 5 passed (spawn log checked: no SKIP), `MobileViewsConfirmModal` 1 / 1. Mutation check: with the Enter gate removed, `DialogContract/OpeningEnterDoesNotAnswerConfirm` fails. Regression filters over the touched dialogs (`Preferences`, `AiAssistant`, `OfflineQueue`, `Views`, `Keybind`, `CommandPalette`) all pass. `UserInfoWindow/NarrowLayoutRendersWithoutAssert` fails locally on this branch and also on the stack-base exe (`b12bdb424`), so it is not from this change; it passes in CI.
- **Doc validation**: `scripts/dev/test-docs.sh` — see the PR body.
- **`grill-with-docs`**: not run. The peer coordinator session fixed scope and stacking. The deviations above record each decision the plan left open.
- **Manual residue**: none. The red destructive buttons and the action-first order are unchanged from before.

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*The `git mv` is the step that reliably gets dropped. Bind it to the impl-log write: in the SAME PR that populates the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*(Delete this `## Archive` block as part of step 2.)*

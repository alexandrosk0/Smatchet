- 2026-11-22 · input-ux-w2-dialog-contract · [debt] · P3 — Dialog contract leftovers: shortcut-capture popups, Zen-mode Esc and four row-25 Esc sites

Details:
`docs/plans/input-ux-w2-dialog-contract.md` (Wave 2 piece 4.1) shipped in PRs #2437, #2468 and #2469. These items
were left out, each for a reason the plan records:

- **Shortcut-capture popups (#77, #81).** The Keybindings capture popups read every key as the shortcut being
  bound, so they are captures, not dialogs, and do not take the dialog scope. They still need their own Esc rule
  (cancel the capture) and a visible hint.
- **Esc Esc leaving Zen mode while a popup is open (#93).** In the cross-cutting surface (`SmatchetUI.cpp`, the
  Zen-mode Esc handler); the popup should take the first Esc.
- **Row 25, four Esc sites not on the scope.** None is a straight substitution (plan § Implementation log, Row 25):
  the offline-queue conflict resolver (`SmatchetOfflineQueueUi.cpp`, one resolved field, not any field), the
  Comments post box (`SmatchetCommentsModalUi.cpp`, the Esc belongs to the box), the long-text editor
  (`TicketFieldEditor_Modal.cpp`, Esc asks to discard even from the text area) and the field viewer
  (`SmatchetFieldViewerModalUi.cpp`, Esc ignored while a text input is active). Each needs a scope option or a
  documented exception.
- **Annotate assignee modal** calls `ConfigManager::Load()` every frame (a Pillar 2 smell found in passing): load
  once on open.
- **Stale comments in `tests/CMakeLists.txt`.** The comments above `Core/SmatchetWorklogSubmitPure.test.cpp` (the
  tsan list, "the shared dialog keys") and above `Core/DialogKeysPure.test.cpp` (both lists, "caret return") predate
  the move of the dialog-key cases into `DialogKeysPure.test.cpp`. Reword them when `tests/CMakeLists.txt` is free
  (plan-lock `input-ux-w3-grid-s4` holds it).
- **Review lows (slice 3):** the caret is not returned to an outer modal's field when a nested modal closes, and
  Update Available can still be dismissed by a stray Enter (Later is the safe choice there).

Concrete next action:
Give the four Esc sites a `DialogScope` option or a comment naming why each is exempt, with a bucket-E case per
site that changes. #77 / #81 and #93 each get their own Esc rule and a bucket-E check. Delete each bullet as it
lands.

Status: open
Last-reviewed: 2026-11-22

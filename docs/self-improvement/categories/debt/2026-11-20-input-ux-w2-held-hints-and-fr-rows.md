- 2026-11-20 · input-ux-w2-action-keys · [debt] · P3 — The Quick-create Create button hint, the French rows for the Wave 2 key strings and a shortcut-guide row are still to land

Details:
`docs/plans/shipped/input-ux-w2-action-keys.md` (Wave 2 piece 4.4, PR #2443) shipped with two items left out
because another branch held their files. The plan-locks are gone; the lock-deferrals PR (branch
`claude/vigorous-khayyam-8e2d16`) landed the views sidebar Enter and the view-switch key tooltips. Three items are
left, each because another PR edited its file when it was written:

- **Quick-create Create button hint.** The Create button should print its key the way every other submit button
  does (`smatchet::ui::KeyHintButton`, `Ui/SmatchetSubmitKeys.h`). File: `Source/Core/src/Ui/SmatchetQuickCreateIssueUi.cpp`,
  edited by open PR #2450 (bulk edit).
- **fr-FR rows.** The Wave 2 key strings render their English fallbacks: the 4.4 keys (`keys.ctrl_enter` and the
  rest named in the plan) plus the four tooltip keys the lock-deferrals PR added, `views.sidebar_search_tooltip`,
  `views.next_view_key`, `views.previous_view_key` and `grid.view_selector.tooltip`. File:
  `Source/Core/src/SmatchetLocalization.cpp`. It was edited by PR #2440 (gesture keys), which merged while the
  lock-deferrals PR was in review, so this one is unblocked.
- **Shortcut guide row.** `docs/guides/keyboard-shortcuts.md` has no row for the views search's Enter yet;
  open PR #2439 (Tab order) edits that file. The row to add after the "Report a bug" row:
  `| Views dashboard | \`Enter\` in the views search box | Open the first view listed (the best match). Its
  tooltip and the grid header's view selector name the \`view.next\` / \`view.previous\` keys |`.

Concrete next action:
Now: add the fr-FR rows for the keys above in their own PR, which the visual-validation rule covers like any
product-text change. After #2439 merges: add the shortcut guide row above. After #2450 merges: draw Quick-create's
Create through `KeyHintButton` with `CtrlEnterKeyName()` and add a bucket-E check next to the existing Quick-create
cases (`KeyHintDrawnForTest`). Delete each bullet as it lands; archive the entry when all three are done.

Status: open
Last-reviewed: 2026-11-20

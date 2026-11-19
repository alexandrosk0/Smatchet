- 2026-11-17 · dropdown-recents-w1 · [debt] · P3 — Deferred review findings from PR #2432 (4 Medium, 9 Low): coverage gaps and small contract drift

Details:
The pre-merge adversarial code review of PR #2432 (head 9e03b4a0b00e) returned 13 findings, 0 critical, 0 high.
All were deferred on purpose. Grouped by theme:

Test coverage:
- No bucket-E case for the bulk-edit multi-value (tick-box) picker in `Source/Core/src/Ui/SmatchetBulkFieldEditPopup.cpp`.
- No bucket-E case for the splice caller, for the footer hints, or for the seeding glue.

Duplication / structure:
- `ResolveOptionList` duplicates what `PickRowComponents` already does (DRY, Pillar 5).
- `DrawActions` is at 27 branches (cap 30) and `RegisterSetFieldBulkCommand` is 109 lines (cap 120): split before the next feature touches them.

Dialog and popup contract:
- The bulk confirm dialog sits outside the #2437 dialog contract; the #2435 discard confirms are outside it too.
- Ctrl+F calls `SetWindowFocus`, which closes non-modal popups.
- `HostIsModal` is not documented in `Source/Core/include/FilterDropdown.h` and needs a contract note there, and Enter in a modal-hosted tick-box dropdown does nothing.
- The quick-create `IsPopupOpen` check looks at any window, not just the quick-create popup.

Behaviour edges:
- `CanEditFieldForIssue` reads the focused pane's backend key rather than the issue's own backend.
- The 160-byte UTF-8 trim was flagged in review (Low); re-read it and confirm it cannot split a multi-byte sequence.
- The #2434 deferrals have no backlog entries of their own.

Concrete next action:
Take them in the order coverage, DRY, contract notes. Delete each bullet as it lands; close this entry when empty.
The bulk-edit follow-ups belong to the `bulk-edit-finish` session, so check with it before touching the bulk popup.

Status: open
Last-reviewed: 2026-11-17

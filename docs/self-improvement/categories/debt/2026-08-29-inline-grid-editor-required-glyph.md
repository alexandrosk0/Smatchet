# Inline grid-cell editor lacks the required-field glyph

- **Category**: debt · **Priority**: P3 (fold-in) · **Filed**: 2026-08-29 (migrated from
  `backlog/BACKLOG_CODE_REVIEW.md` A3 residual on ledger retirement)
- **Where**: `Source/Core/src/TicketFieldEditor.cpp` (inline grid-cell editor).

## Problem

The required-field UI shipped everywhere else: the red-`*` glyph + `field.required_tooltip`
render in `Source/Core/src/Ui/SmatchetNewIssueDraftUi.cpp` (required-ness fans in from the
catalog's `IsRequired` and the issue type's `RequiredFieldIds`) and in
`Source/Core/src/TicketFieldEditor_Modal.cpp`, with blank-required submit gated via the
`missing` set. The inline grid-cell editor is the one remaining surface with no required
marker.

## Proposed shape

Fold the same glyph + tooltip into the inline editor the next time
`TicketFieldEditor.cpp` is touched — a fold-in, not a standalone PR.

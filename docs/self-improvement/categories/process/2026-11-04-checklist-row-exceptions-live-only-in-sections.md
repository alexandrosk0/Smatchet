# A checklist severity row whose scope or exception lives only in its `§` section is misread by the code-review row walk

- **Category**: process
- **Priority**: P2
- **Date**: 2026-11-04
- **Observed on**: Smatchet#2393. That PR pairs the empty-cache checklist row with layer the-unwilling-agentic-bunch#20, which makes the code-review agent walk the checklist's severity rows.
- **Status**: open

## What happened

Layer `agent-layer/agents/core/code-review.md` (version 10), process step 4, walks each row of the checklist's `## Severity in this project` table against the lines the diff adds, changes or removes. A matched row is a finding "unless the row's own text names an exception". Two rows of `docs/guides/review-checklist.md` kept part of their rule only in the section their `(§ …)` points to:

- **The empty-cache High row** had lost its "unless intended and stated" exception. That is the root cause the code-review eval found.
- **The human-eyes verification Critical row** kept both its scope and its exception in § Verification residue. The scope is the test plan, PR body and linked plan, which are not diff lines. The exception is a bucket-E follow-up already tracked in the backlog.

Read literally, the walk then files a false Critical on the tracked bucket-E case, which blocks the ack, or it falsely clears a residue that sits only in the PR body. Smatchet#2393 moved both rows' scope and exception inline. Nothing stops the next new row from repeating the pattern.

## Concrete next action

Pick one of these:

1. **Change the layer** (`agents/core/code-review.md` step 4): a row includes the section its `(§ …)` names. That removes the inline-duplication pressure.
2. **Add a host doc gate** that fails a severity-table row which cites a `§` section whose body states an exception ("Exception:", "unless") the row text lacks.

Option 1 is the smaller change. Re-run the code-review eval's catalog case afterwards, because the eval measured the inline-row form.

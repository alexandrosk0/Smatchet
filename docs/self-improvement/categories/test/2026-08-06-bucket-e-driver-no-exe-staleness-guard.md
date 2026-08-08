# bucket-E drivers run a stale exe silently

- **Category**: test
- **Priority**: P2
- **Date**: 2026-08-06
- **Source**: window-expand-button (bucket-E red chased for two build cycles)

## Friction

`scripts/dev/test-ui-window-expand.sh` (and every sibling bucket-E driver) runs
`build/ninja-ui-test-msvc/Smatchet.exe` unconditionally. If the source edit under test
landed after the last build, the driver reports a *product* verdict for a binary that does
not contain the change — and the failure text is indistinguishable from a real one.

Two debug cycles on this feature were spent disproving a hypothesis that had, in fact,
already been fixed in the working tree but not in the exe. Instrumentation added in the same
window produced zero log lines, which read as "the code path never runs" rather than "the
code was never compiled".

`docs/agent-rules/debug-techniques.md` § exe-staleness documents the manual check. A manual
check that must be remembered at exactly the moment the agent is deepest in a wrong
hypothesis is the weakest possible placement.

## Proposed fix

Add a staleness guard to the shared bucket-E driver preamble: compare the exe's mtime
against the newest mtime under `Source/` + `tests/ui/`, and hard-FAIL (exit 2, same class as
"binary missing") with the offending file named. Cheap — one `find -newer`. Belongs next to
the existing exit-2 "binary missing" check so every driver inherits it.

## Status

Open.

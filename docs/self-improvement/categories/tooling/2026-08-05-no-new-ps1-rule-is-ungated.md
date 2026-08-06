# "No new .ps1" rule is documented but ungated

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-08-05
- **Observed on**: PR #1960 (kill-PowerShell plan closeout)

## What happened

`docs/harness/SETUP.md` § Windows-only shims now states the invariant that the five
remaining `.ps1` files are the complete set and that "Adding a **new** `.ps1` anywhere
else is a regression". Nothing enforces it:

```
grep -rn "ps1" agents/scripts/project/test-lint-rules.sh scripts/dev/test-docs.sh
```

returns nothing. The whole point of the 7-slice kill-PowerShell plan was to stop the
toolchain re-growing; a prose-only invariant will not.

## Proposed action

Add a delta-gated `lint-rules.d/` rule (`no-new-ps1`) asserting that the tracked `.ps1`
set equals the five files named in the SETUP.md table, with the usual
`SMATCHET_DEVIATION(rule=no-new-ps1; ...)` escape for a genuinely Windows-only addition.
Cheap: `git ls-files '*.ps1'` diffed against the documented list. Pair it with a check
that each kept shim still carries its `# Last remaining PowerShell file` marker line and
stays ASCII-only / no BOM / LF, which SETUP.md also states and also does not enforce.

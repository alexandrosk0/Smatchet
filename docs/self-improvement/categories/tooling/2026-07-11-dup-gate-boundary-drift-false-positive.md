# dup_audit delta gate false-flags a clone in UNCHANGED files (winnow boundary drift)

**Category**: tooling · **Priority**: P2 · **Status**: applied (fixed in the N12 slice-3 PR)

## What happened

N12 slice-3 (a Tracker-only refactor) tripped `dup_audit.py --diff origin/develop` with:

```
[dup] FAIL CodeColorView.cpp:97 <-> CppSyntaxLex.cpp:113 — 74-token copy-paste clone
```

Neither file was in the diff. Probe of the module confirmed both files were **byte-identical**
at the merge-base and at HEAD, yet `find_clones` surfaced an *extra* maximal-clone boundary
`(97,113)` ntok 74 at HEAD that it did not surface at base `(98,124)` ntok 71. Winnowing is
corpus-sensitive: adding tokens in an unrelated changed file shifts which shingles seed the
extension for a pre-existing clone between two unchanged files, so the drifted boundary gets a
base-absent `content_hash` and slips past the hash-only grandfathering.

## Impact

A blocking DRY gate can fail a PR over duplication in files the author never touched, keyed on the
happenstance of what other files the diff perturbs. Every "gate, don't trust" property assumes the
gate flags the author's own new duplication; this violated it.

## Fix (applied)

`new_clones_vs` now grandfathers a clone whose **every** occurrence is in a file unchanged (by
normalized token stream) vs the base — you cannot duplicate code *into* a file without changing it,
so an all-unchanged-files clone is definitionally pre-existing, whatever boundary the winnow drift
selected. Sound: it cannot mask a genuinely-new clone (a new clone has ≥1 occurrence in a changed
file). Pinned by a `--selftest` case that stubs `find_clones` to emit a drifted base-absent hash for
an unchanged pair and asserts it stays grandfathered (fails on the old code, passes on the new).

## Preventing recurrence

The selftest locks the invariant. A stronger follow-up (not done — low value): make winnowing seed
selection deterministic w.r.t. corpus so boundaries don't drift at all; deferred as the guard fully
closes the false-positive class.

# Splitting a script that relocates shared symbols: verify the whole blast radius, not just the file's own suite

- **Date**: 2026-07-13
- **Author**: orchestrator
- **Category**: process
- **Priority**: P2

## What

`merge-gates.sh` was split into an entry point + `merge-gates.d/` modules (#1823),
relocating two load-bearing symbols out of the entry file: the block allow-list
constant `MERGE_GATES_BLOCK_ALLOWLIST_RE` (→ `00-common.sh`) and the `GATE_FILTER`
jq projection (→ `10-gate-filter.sh`). The split agent verified `merge_gates.bats`
locally (164/164) and reported "all gates green" — but the PR landed on the CI bats
lane **RED** and drew a **Security/Major CodeRabbit finding**. Two distinct classes
of breakage that the file's own test suite could never surface:

1. **Sibling drift-guards that read the original file.** `safe_admin_merge.bats`
   grepped `merge-gates.sh` directly for the relocated constant; `postmortem_owed.bats`
   built a fixture repo that copied `merge-gates.sh` but not the new `merge-gates.d/`
   modules, so the fail-closed module loader aborted and `postmortem-owed.sh` refused
   (false negative). Both are in *different* bats files (`safe_merge`, `safe_admin_merge`,
   `postmortem_owed`) — none run by a `merge_gates.bats`-only local check.

2. **Integrity/freshness guards that fingerprint the original file.** The #1428
   self-freshness guard hashed only `${BASH_SOURCE[0]}` (the entry point). After the
   split the load-bearing gate logic lived in the modules, so a stale/tampered
   *module* passed the freshness check while enforcing out-of-date gate logic — a real
   security regression. The same gap existed in `merge-watcher.py`'s
   `_GATE_LOGIC_RELPATHS` drift detector. CodeRabbit caught it; the local verify did not.

## Why it matters

The blast radius of "split a file and move a shared symbol" is **everything that reads
or fingerprints that file**, not "the file's own unit test." A `<file>.bats`-only local
run is a false-confidence signal for a relocation change: by construction it exercises
the moved-into-place behaviour, never the *cross-file assumptions* about where the
symbol lives.

## Concrete next action (process rule)

When a change **relocates a shared symbol / constant / function** out of a file (any
split, extraction, or god-file partition), before pushing:

1. **Grep the repo for readers of the moved symbol AND the source path**, e.g.
   `rg -n 'MERGE_GATES_BLOCK_ALLOWLIST_RE|GATE_FILTER|<oldpath>' tests/ agents/scripts/`
   — every hit that greps/copies/sources the old file is a candidate breakage.
2. **Run the full sibling suite set**, not just the file's own `*.bats`. For the
   `agents/scripts/core/` gate family that means `merge_gates.bats` **plus**
   `safe_merge.bats`, `safe_admin_merge.bats`, `postmortem_owed.bats`,
   `merge_watcher.bats`, and `test-oob-label-impl.sh` — the suites that read the gate
   files transitively.
3. **Update every integrity/freshness/drift guard that fingerprints the old file** to
   cover the new modules (here: the #1428 shell freshness guard's `_fresh_relpaths` +
   `merge-watcher.py`'s `_GATE_LOGIC_RELPATHS`), and add a **static regression guard**
   asserting each module appears in those lists (added to `merge_gates.bats` in #1823's
   follow-up so a future module can't silently escape).

Candidate automation: a `pre-ship` helper that, given a diff which deletes a
top-level assignment/def and adds it under a sibling `*.d/` module, auto-greps for
external readers of that symbol and warns if any live outside the changed files.

## Status

open (documented; the pre-ship auto-grep helper is the implementable follow-up —
the full-sibling-suite + guard-update discipline is the immediate manual rule)

# `test-orphan-bats` runs only in the full suite / CI, not the pre-ship fast path — an unwrapped `.bats` reddens develop a merge later

- 2026-07-10 · orchestrator (self-improvement campaign ship session) · [tooling] · P2 — a new bats suite shipped without its `test-*.sh` wrapper; the orphan-bats gate caught it only in CI, on the *next* PR, masking which change introduced the red

## Friction

The mutation-smoke gate slice (#1698) added `tests/bats/mutation_smoke.bats`
without a `test-*.sh` wrapper naming its path. `test-orphan-bats.sh` (which
enforces that every bats suite has a runner, so an added suite can't silently
never-run) **is** auto-enrolled in `scripts/dev/test-all.sh` — verified:
`test-all.sh:113` globs `agents/scripts/core/test-*.sh` and orphan-bats lives
there — but `scripts/dev/pre-ship.sh`, the fast pre-push gate, does **not** run
it (verified: `grep -c orphan scripts/dev/pre-ship.sh` → 0).

So the orphan escaped the local pre-push loop and surfaced only as a red
`Agentic self-tests (bats)` lane on the **next** PR's CI (#1702), one merge after
the change that caused it — the red pointed at an innocent PR and cost a
diagnosis round to trace back to #1698. A trap: the mirror script
`scripts/dev/test-mutation-smoke.sh` *looks* like it covers the suite but it
validates the harness/corpus, not the bats file, so it does not satisfy the
wrapper requirement.

## Proposal

1. Add a fast `bash agents/scripts/core/test-orphan-bats.sh` call to
   `scripts/dev/pre-ship.sh` (the check is near-instant — no build, just a glob +
   grep over wrappers) so an unwrapped suite is caught before push, not a merge
   later on an unrelated PR's CI.
2. Playbook one-liner: **adding a `tests/bats/*.bats` requires its
   `test-<name>-bats.sh` wrapper (naming the suite by `tests/bats/<name>.bats`
   path) in the SAME PR** — a harness/corpus mirror script does not count.

Est ~15 min total. This session fixed the instance by adding
`scripts/dev/test-mutation-smoke-bats.sh` (#1702), but the pre-ship gap remains
and will bite the next suite added without a wrapper.

**Update (2026-07-10): implemented.** Added a
`bash agents/scripts/core/test-orphan-bats.sh` stage to `scripts/dev/pre-ship.sh`
(next to the test-list consistency check), so a wrapper-less bats suite is caught
before push. Archive to `applied.md` on the next sweep.

## Format

- Details: see § Friction. Verified against the committed tree at develop head.
- Concrete next action: see § Proposal (1)–(2) — done.
- Status: open
- Last-reviewed: 2026-07-10

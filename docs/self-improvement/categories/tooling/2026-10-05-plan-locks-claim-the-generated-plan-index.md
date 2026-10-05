# Plan locks claim the generated `docs/plans/INDEX.md`, so every plan archive or move collides with any open plan's lock

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-10-05
- **Observed on**:
  - PR #2309: merged 2026-10-05T18:56:37Z under `plan-lock-out-of-band`, over lock `hook-tree-resolution`.
  - PR #2213: the same overlap, over lock `gate-selftest-msys-execbit`; see its [postmortem](../../postmortems.md#2026-10-04--pr-2213--override-bare-plan-lock-out-of-band-over-a-12-day-stale-plan-lock-gate-red-that-a-re-run-would-have-cleared).
- **Status**: open

## What happened

`docs/plans/INDEX.md` is generated. `bash agents/scripts/core/test-plan-index.sh --fix` rewrites it from the plan tiers, and the required `test-plan-index` check fails any PR whose `INDEX.md` does not match `docs/plans/{active,deferred,shipped}/`. So every PR that adds, archives or moves a plan must change it.

A plan lock's write set lists the plan's own lifecycle paths, and plan locks routinely list `docs/plans/INDEX.md` too, because the owning session will archive its plan at the end. `plan-lock-gate.sh` compares paths only. While any one such lock is open, every other PR that touches a plan reads red on `Plan-lock gate`. Its only way through is an override, or waiting for an unrelated plan to finish.

#2309 archived the offline-first plan (`git mv` to `shipped/`, then `test-plan-index.sh --fix`). Its one red was:

```text
plan-lock-gate: 'docs/plans/INDEX.md' overlaps the write set of plan-lock 'hook-tree-resolution', held by a different branch.
```

The lock was live, about a day old, held by `fix/hook-tree-resolution`, and that branch was not on GitHub. The overlap could not be avoided without dropping the archive from the PR. A generated file loses nothing to a concurrent change: the second PR to merge reruns the generator. The maintainer chose `plan-lock-out-of-band`, which was applied at 18:52:31Z and removed after the merge at 18:56:40Z.

## Concrete next action

Treat generated paths as never lockable:

- `lock-claim.sh` and `lock-claim-update.sh` drop them from a write set and say so.
- `plan-lock-gate.sh` drops them from both the PR's changed files and every lock's write set before it compares.

Keep the list in `project.config.json` (for example `plan_locks.generated_paths: ["docs/plans/INDEX.md", "docs/plans/_plan-locks.generated.md"]`), so the portable scripts read it rather than hard-code it. Bats coverage:

- a lock claiming `INDEX.md` does not redden a PR that only regenerates it;
- a real overlap elsewhere in the same PR still does.

Replayed on #2309, the gate is green and no override is needed. Replayed on #2213, it is green too, independent of that entry's stale-lock re-check.

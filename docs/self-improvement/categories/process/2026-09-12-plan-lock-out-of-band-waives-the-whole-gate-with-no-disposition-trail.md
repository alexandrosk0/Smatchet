# `plan-lock-out-of-band` waives the whole Plan-lock gate, and records no reason for it

- **Category**: process
- **Priority**: P2
- **Date**: 2026-09-12
- **Observed on**: PR #2160 (merged `6a1a28115146` 2026-09-06T04:57:59Z with `Plan-lock gate` = failure)
- **Status**: open

## What happened

`Plan-lock gate` is fail-closed and, by its own header, "unbypassable except the
`plan-lock-out-of-band` label" (`agents/scripts/core/plan-lock-gate.sh:18`). The poller honours that
label as a bare boolean:

```jq
# agents/scripts/core/merge-gates.d/10-gate-filter.sh:36
| ($labels | any(. == "plan-lock-out-of-band")) as $planlock
```

Presence alone downgrades a red `Plan-lock gate` to WARN. Nothing records **which** lock was
crossed, **which** path overlapped, or **why** crossing it was safe.

The sibling hatch already solved this. `cr-out-of-band` ALONE is deliberately **not** honoured: the
downgrade additionally requires `$crdisposition` — a `cr-disposition:`-prefixed label OR a
`cr-disposition:<reason>` marker line in the PR body (`10-gate-filter.sh:76-77`) — and
`merge-gates.sh:1486` refuses the waiver when that attestation is missing (PR-3
`cr-out-of-band-disposition-trail`). The plan-lock hatch never received the same treatment, so the
repo enforces an attestation trail for one override and not for the other.

## Why it matters

1. **One label, three unrelated conditions.** `plan-lock-gate.sh` offers the same label for a
   genuine write-set overlap (`:50`), for "lock table unavailable/undetermined … the fail-closed
   gate refuses to pass on an unverifiable lock state" (`:44`), and for an unresolvable base ref
   (`:73`). A coordinated overlap and a broken lock fetch are cleared by the identical token, and
   afterwards nothing distinguishes them.
2. **Whole-gate, not per-path.** The downgrade is all-or-nothing. Applied to clear one benign
   collision on a shared append-only doc, it equally waives any *other* overlapping path in the same
   PR — including a genuine source-file collision nobody looked at.
3. **The evidence self-destructs.** On #2160 the label was applied 2026-09-05T17:58:08Z and removed
   2026-09-06T04:58:42Z, 43 seconds after the merge. GitHub strips override labels post-merge, and
   ADR-0017's merge-snapshot ledger — the designed backstop — has no row for #2160 (see
   [`2026-08-19-safe-merge-arms-automerge-and-execs-away-before-writing-a-snapshot-row.md`](../tooling/2026-08-19-safe-merge-arms-automerge-and-execs-away-before-writing-a-snapshot-row.md):
   the default merge path writes none). Both records of *why* this gate was waived are gone.
4. **Undocumented.** `docs/agent-rules/merge-gates.md` documents the other hatches and does not
   mention `plan-lock-out-of-band` at all.

## Concrete next action

1. **Require a disposition trail, mirroring the shipped CR pattern.** Stop honouring
   `plan-lock-out-of-band` alone; require a `plan-lock-disposition:`-prefixed label OR a
   `plan-lock-disposition:<reason>` marker line in the PR body naming the lock slug and why crossing
   it is safe. Add `$planlockdisposition` beside `$crdisposition` in
   `merge-gates.d/10-gate-filter.sh`, and refuse the downgrade without it using the
   `merge-gates.sh:1486` refusal as the template.
   Enumerator: `grep -n 'planlock|crdisposition' agents/scripts/core/merge-gates.d/10-gate-filter.sh`.
2. **Split the infra condition off the coordination condition.** An unverifiable lock state
   (`plan-lock-gate.sh:44`, `:73`) is an infra failure, not an operator decision; it should not be
   clearable by the token that attests "I coordinated this overlap". Give it its own label, or make
   the disposition reason mandatory-and-distinct on that path.
3. **Document the hatch** in `docs/agent-rules/merge-gates.md` alongside the others.

**Enumerator + replay**: add a case to `tests/bats/merge_gates.bats` asserting that a red
`Plan-lock gate` plus `plan-lock-out-of-band` and NO `plan-lock-disposition:` does **not** downgrade,
and that adding the disposition does. Replayed against the gate as it stands, the first half fails —
the bare label downgrades today, which is the #2160 shape exactly.

Triggered-follow-up: when=pr-count:base=develop;since=2026-09-12;n=15; action=check whether the plan-lock hatch requires a disposition trail, and whether any PR merged on a bare plan-lock-out-of-band since; baseline=1 bare-label escape (#2160) with the reason unrecoverable, 2026-09-06; fired=never

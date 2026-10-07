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
   [`2026-08-19-safe-merge-arms-automerge-and-execs-away-before-writing-a-snapshot-row.md`](../applied.md):
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

## Follow-up measurement — 2026-10-04 (PR #2280 postmortem)

Re-measured: 52 PRs have merged into `develop` since 2026-09-12. **The hatch is unchanged, and it has been used twice more.**

- **Still a bare boolean.** `merge-gates.d/10-gate-filter.sh:36` still reads `($labels | any(. == "plan-lock-out-of-band")) as $planlock`. `grep -rn plan-lock-disposition` finds the token only in this entry and the ledger, not in any gate. None of actions 1-3 has landed.
- **2 merges since then crossed a red `Plan-lock gate` under the label** (`gh pr list --state merged --search "label:plan-lock-out-of-band merged:>=2026-09-12"`):
  - **#2213** merged 2026-09-24T13:48:26Z. Its head `1df75c8a` had `Plan-lock gate` red at 2026-09-12T00:46:43Z, and its body has no disposition line. This is the #2160 shape again; its `postmortems.md` entry (2026-10-04) finds the red was 12 days stale and would have passed on a re-run. It merged outside `postmortem-owed.sh`'s 20-merge window, which is also blinded by an HTTP 504; see [`2026-10-04-postmortem-owed-graphql-504-reads-as-clean.md`](../applied.md).
  - **#2280** merged 2026-10-04T04:47:49Z. Its author wrote a `plan-lock-disposition:` line voluntarily, which the gate does not yet ask for. The line covers `crash-retire-terminate-prefs-enddisabled`: a lock left orphaned by #2286's merge, with the override authorised by the maintainer.
- **The whole-gate waiver bit, as point 2 predicted.** The same red `Plan-lock gate` run (job `111353340607`) flagged a **second** overlap: `tests/CMakeLists.txt` against the live lock `sanitizer-nightly-run-tests`, held by `fix/sanitizer-nightly-run-tests` (#2288). The disposition never mentions it, and the label cleared it anyway.
  - It did no harm this time. #2288 does not edit `tests/CMakeLists.txt`, because its lock claims more than its diff, and #2288 is still mergeable.
  - That was luck, not review. A per-slug disposition, with every red overlap required to be named, would have forced someone to look at it.

So the volunteer behaviour this entry asks for showed up once, and the gate still could not tell a full disposition from a partial one. Action 1 should require the disposition to name **every** slug the failing run reported. `plan-lock-gate.sh` already prints one `overlaps the write set of plan-lock '<slug>'` line per overlap, so that is the enumerator. A bats case: a red run naming two slugs, plus a disposition naming one, must not downgrade.

Upstream of #2280's override is a separate hole: lock release on close keys only on a `lock-slug:` body line. That is filed as [`2026-10-04-lock-release-on-close-keys-only-on-a-body-line.md`](../tooling/2026-10-04-lock-release-on-close-keys-only-on-a-body-line.md).

Triggered-follow-up: when=pr-count:base=develop;since=2026-09-12;n=15; action=check whether the plan-lock hatch requires a disposition trail, and whether any PR merged on a bare plan-lock-out-of-band since; baseline=1 bare-label escape (#2160) with the reason unrecoverable, 2026-09-06; fired=2026-10-04

Re-scoped 2026-10-04 (backlog-sweep-2026-10): Phase 1 landed (backlog-sweep PR #2296: a plan-lock-disposition label or PR-body marker is required — shared disposition() jq helper with cr-disposition; documented in merge-gates.md § Per-PR overrides). Remaining: the per-slug completeness rule (a disposition must name every slug the failing run reported); splitting infra-unverifiable reds off the coordination label; and safe-admin-merge.sh still honours a bare plan-lock-out-of-band.

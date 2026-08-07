---
name: pre-implementation-review
version: 1
description: Gate review of a work item's 1-specification.md + 2-design.md + 3-plan.md as a coherent set, before any implementation. Use when invoked as a review-panel leg for the pre gate, when the user asks for a "pre-implementation review", "review the spec/design/plan", or "gate the item", or before implementing an open work item under docs/work/items/. A clean pass plus the user's sign-off is what unlocks implementation.
---

# Pre-implementation review

Reviews a work item's `1-specification.md` + `2-design.md` + `3-plan.md` **as a coherent set** —
coherence & readiness, not re-litigation. No implementation begins before this review passes and the
user signs off ([work-items.md](../../../../docs/agent-rules/work-items.md) § Gates). Ported from
Whip-Process `Procedures/PreImplementationReview.md`; contract wording carried verbatim. Shared
panel mechanics (reviewer identity, rounds, independence, output template):
[review-panels.md](../../../../docs/agent-rules/review-panels.md).

Normally launched as a panel leg by `agents/scripts/core/run-review.sh --gate pre --subject <NN|slug>
--round N` — the launcher hands you the exact output path; you do not choose your own harness, model,
or round. Run solo (user-invoked, no launcher), derive the filename per review-panels.md and say so
in the header.

## Do exactly this

1. **Review the last commit.** Start from `git show HEAD` — read every file it touches **in full**,
   not just the hunks. Then **widen to the whole item's three artifacts** even if the commit touched
   only one: the gate judges coherence across all three, so all three are in scope regardless of
   what just changed. Resolve the work item: an index/slug from the invocation → its
   `docs/work/items/NN-slug/`; else the folder the commit touches; if that resolution is ambiguous,
   ask rather than guess. Review independently (review-panels.md § Review independently) — form your
   own findings before reading any sibling `4-review-*` file.

2. **State only problems** — no praise; verify each finding against the actual artifact text, then
   stop. The exact failure modes this gate exists to catch:

   - the **plan does not fulfill the spec end-to-end** — a spec requirement no plan step delivers;
   - **design or plan drifted** from the spec's intent, or from each other;
   - the plan is **not buildable in its stated order** — a step depends on the output of a later one;
   - a **downstream cascade introduced a contradiction** — an earlier finding's fix updated one
     artifact and left a now-false claim standing in another;
   - the plan's `## QA` section **under-covers the spec** — a manually-verifiable success criterion
     with no QA step, or a QA step that merely restates an automated test (an item with no manual-QA
     surface rightly has no `## QA` at all — absence alone is not a finding);
   - a QA step is wrong **as a scenario**: filed under a scenario it doesn't belong to (it will
     drain there verbatim at close), a scenario setup precondition that is unreachable, or steps
     split across scenarios that one playthrough would cover.

   Do **not** re-open settled decisions — decisions the artifacts record and the user has already
   accepted are the oracle, not findings. Give every finding a **disposition**:
   `fix` / `defer (→ Spawned.md)` / `accept` (with rationale).

3. **Write the review file** — `4-review-[round]-[harness]-[model].md` in the item folder, per
   review-panels.md. Title: `NN · <item title> — Pre-implementation review (pass N)`; the
   "against …" line names the three artifacts **as a set**:

   > Problems only. Reviewed commit `<hash>` (`NN-slug`) against `1-specification.md`,
   > `2-design.md`, and `3-plan.md` as a set. No praise, no restatement.

   If the pass finds nothing actionable, the body is a single line saying so — that is the stop
   signal.

## After the pass

The panel's findings are resolved by one addresser per the `address-review-feedback` skill →
`4-resolution-[round]-[harness]-[model].md`; findings cascade **downstream** (spec → design → plan)
per work-items.md § Handling changes. The gate's outcome is the user's sign-off — implementation
stays blocked until it lands.

# Work items

The human-gated work loop for product features — how an item moves from idea to close. Ported from
Whip-Process (`Process.md` + `Conventions.md`); where the source wording encodes a contract it is
carried across verbatim rather than paraphrased. Panel-review mechanics live in
[review-panels.md](review-panels.md); the work-item-vs-plan boundary and location rules live in
[process-rules.md](process-rules.md); how the ship-loop behaves inside an item is
[ship-loops.md](ship-loops.md) exception 7.

One item (feature) at a time. Each item lives in its own numbered folder under `docs/work/items/`,
named `NN-slug` (two-digit number + kebab-case slug), holding the numbered stages in sequence:

- `1-specification.md` — what is being built and why
- `2-design.md` — the technical design / approach
- `3-plan.md` — the step-by-step implementation plan, including a `## QA` section **when the item has
  manual verification to author**. An item with none — a docs or tooling change, or no manual-QA
  surface — simply omits it.

The two review gates then add per-reviewer **review** and **resolution** files (see
[Review](#review) and [Addressing feedback](#addressing-feedback), and
[review-panels.md](review-panels.md) for the file mechanics). The numeric prefixes (`1-` … `5-`) are
an **artifact sequence** — file ordering within the item folder, not step numbers.

Plus the **ungated** working doc `Spawned.md` — the item's capture sheet for tracking work it spawns
(deferrals, ideas, backlog entries, bugs). A per-item `QA.md` sheet exists **only when the plan
carried `§QA`** — generated at implementation end; a **script to work through, not a form to fill**:
it records no results, and nothing gates on it. Each drains at close (`Spawned.md` into the canonical
ledgers — see [Tracking](#tracking)) and is then deleted with the folder; unlike the staged files
they aren't reviewed or frozen. (Whip also keeps a *standing* project-wide QA checklist that per-item
`(durable)` QA lines drain into; Smatchet has no manual-QA checklist yet, so that drain target does
not exist here — flagged, not invented. If a manual-QA surface appears, port the standing-checklist
convention then.)

Once an item closes, the whole folder collapses into a single `docs/work/closed/NN-slug.md` summary
file — see [Closing](#closing).

## The loop

An item moves through these **stages** — *(optional) idea → (optional) research → spec → design →
plan → implementation → retro → close*. Steps are referenced by name, and row order carries the
sequence:

| Step | Description | Artifact |
|------|-------------|----------|
| Idea capture *(pre-step)* | a proposal the user is unsure about enters `docs/work/IDEAS.md` | IDEAS entry |
| Research *(pre-step, optional)* | investigate; output cited from the entry | research notes |
| Research review *(pre-step, optional)* | lightweight, scaled read of the research; skipped with research | note on the IDEAS entry |
| Approval *(pre-step)* | graduate to `docs/work/BACKLOG.md`, else drop | — |
| Goal-setting | the user sets the item's goal + priorities (candidates from DEFERRED / BACKLOG / bug issues) | — |
| Specification | what is being built and why | `1-specification.md` |
| Design | the approach | `2-design.md` |
| Plan | the step-by-step plan | `3-plan.md` |
| Pre-implementation review | gate the spec+design+plan as a coherent set, then resolve findings (cascade) | `4-review-*` · `4-resolution-*` |
| Implementation | build the approved plan; not done until the verification suite passes (scaled to what changed; a docs-only item runs the docs suite) | code |
| Post-implementation review | gate the diff, then resolve findings; also sweeps spawned work into `Spawned.md` | `5-review-*` · `5-resolution-*` |
| Retro | a candid retro on *how the user and the agent worked* | — (in-chat) |
| Close | drain `Spawned.md` → ledgers, collapse the folder to one summary | `docs/work/closed/NN-slug.md` |

Specification, design, and plan each follow [the cycle](#the-cycle) (each builds on the prior
draft); their sign-off is the shared **pre-implementation review** gate, not per stage.

**The verification suite grows with the system.** New behaviour or a fixed logic bug carries a test
(an invariant where one fits), and an item that **reshapes existing behaviour** also carries a
**characterization/anchor test against the known-good behaviour** it replaces. Forward invariants and
re-recorded goldens guard regressions but enshrine birth defects — anchoring to prior
observed-correct behaviour is the cheap guard against them.

**QA — manual verification, not a stage.** The automated suite gates; QA is the human counterpart,
and it is **not a step of the loop and gates nothing**: manual-QA results may arrive late or never,
and nothing that may never arrive can gate a close. At implementation end the agent **generates** the
per-item `QA.md` from plan `§QA` — bare bullets, grouped into the scenarios `§QA` already declared,
indexed from 1 within each. It records **no results**: a sheet holding partial results is worse than
one holding none, because it implies coverage that did not happen. A defect found while working the
steps is captured in `Spawned.md` like any other. An item with nothing to verify manually omits
`§QA` entirely, and every QA check skips it.

A **bug fix** (taken from a confirmed GitHub Issue — Smatchet tracks product bugs as issues, not a
ledger file) runs the loop **scaled to the fix**: a trivial fix may skip the upfront
spec/design/plan artifacts and their gate and go straight to implementation → post-implementation
review → retro → close; a substantial bug carries the full set. Either way it closes to a
`docs/work/closed/NN-slug.md` summary and its issue is closed with a link to the item.

A spec needn't settle every decision — it may **defer specific questions to the design stage**,
recording them as *open questions* that `2-design.md` resolves. The spec is still complete; those
are conscious hand-offs to design, not loose ends.

## The cycle

1. **Discussion** — the user and the agent align on intent. The agent takes the inputs (the goal, the
   user's priorities, and any upstream artifacts — approved closed items, plus this item's prior
   drafts), asks every clarifying question it has, and both agree on scope before the agent drafts.
2. **Iteration** — the agent drafts the artifact; the user and the agent review it together; the
   agent revises on the user's feedback; repeat until it's right.
3. **Sign-off** — the user explicitly signs off **at the review gate** (the pre-implementation review
   for the spec/design/plan set; the post-implementation review for the build). Sign-off is the
   gate's *outcome*, not a precondition for it — it unlocks the next **phase** (implementation, then
   close); the next stage meanwhile builds on the prior settled artifacts.

**Spike unproven assumptions before designing on them.** When an artifact would hinge on an unproven
engine/feasibility assumption, prove it with a throwaway spike **before** writing the design around
it — don't design on paper around untested behavior. Optional and trigger-gated, scaled to the risk;
the spike is throwaway (never merged as product code), but its **conclusion** is cited from the
design's Risk section. This is the proactive twin of *spike before re-designing*
([Handling changes](#handling-changes)).

**Mock UI visual design before authoring it.** For UI items, **visual design is its own pass** —
settled via a **rendered mockup** before any widget authoring, the perceptual analog of the spike
above. The mock is throwaway; its conclusion lands in the design.

## Review

Two review moments, same discipline. In both, the thing under review is **by design not yet final** —
the review is the gate that clears it. Each reviewer writes its own file; the file-naming,
harness/model/round, and parallelism mechanics live in [review-panels.md](review-panels.md).

Each file **records only problems**: issues found, each with its disposition (fix / defer / accept)
— no praise, status summaries, or restating what already works.

- **Pre-implementation review — coherence & readiness, not re-litigation.** Reviews spec + design +
  plan *together*: does the plan fulfill the spec end-to-end; did design/plan **drift** from the
  spec's intent or from each other; is the plan actually buildable in its stated order; did a
  downstream cascade introduce a contradiction. It does **not** re-open decisions already settled.
  Performed by a **multi-model panel** plus the user's sign-off; **scaled to the item** (a line for a
  trivial one; a real pass for a substantial one). Findings cascade **downstream** per
  [Handling changes](#handling-changes).
- **Post-implementation review — the work product.** Reviews the diff; also sweeps the item's
  spawned tracking work into `Spawned.md`.

**Verify, then stop.** Verify each finding against the actual artifacts/code before acting — a
stale, duplicate, or incorrect finding gets a disposition (and a note), not a change. The loop ends
when a pass surfaces no new *actionable* finding; chasing marginal nits past that point is waste.

## Addressing feedback

A review panel produces findings; one **addresser** resolves them — the resolution counterpart to
the review gates. The panel is deliberately many *independent* passes that don't consolidate; dedup,
reconciling conflicts, and the **binding** disposition are the addresser's job (a reviewer's
disposition is a *suggestion*). The same discipline applies at each entry point, the downstream
target differing:

- **After pre-implementation review** — resolve into the spec/design/plan (cascade per
  [Handling changes](#handling-changes)).
- **After post-implementation review** — resolve into the diff, build-clean.

(Whip has two further entry points — a standing-code *audit* and an itemless *generic review*.
Neither is ported: Smatchet's ad-hoc commit review is covered by the CodeRabbit + Bugbot merge
gates, and the audit maps to later phases of the absorb plan. If either is ported later, it reuses
this addresser discipline unchanged.)

The binding disposition for each deduped finding is the checklist a close **diffs against**, so a
`fix` can't silently fail to land. Scaled to the item: a trivial round's resolution is one line.

## Gates

**No implementation begins before the pre-implementation review passes and the user signs off.**

These two sign-off points — pre-implementation and post-implementation — are the **only** places the
ship-loop pauses inside an open work item; everything between them runs autonomously
([ship-loops.md](ship-loops.md) exception 7).

## Handling changes

Because each phase builds on the one above, a change cascades **downstream**: update each later
artifact in order, re-reviewing — don't skip ahead. E.g. a design change → `2-design.md` →
`3-plan.md` → implementation (which pauses until the artifacts above are re-reviewed and accepted).
Record significant changes and their rationale in the `4-review-*.md` files.

**Trigger — re-evaluate vs. absorb.** Test each change: does it *contradict what an approved
artifact asserts*, or merely *implement* it? It trips the cascade + re-review above when it
invalidates an approved **decision** (a mechanism / structure / seam that proved wrong or got
replaced), moves **scope** (the in/out boundary, or a module / dependency / contract the plan didn't
sanction), changes a **spec acceptance criterion**, or **reverses a prior item's decision**. Below
that level — a compile/type/include fix, local naming, an added guard, the mechanics of an assertion
— it's **absorbed inline** and just noted (review/commit), no round trip. When the agent can't tell
which side a change is on, a one-line *"this changes decision X — re-evaluate or absorb?"* beats
both a needless cascade and silent drift. The point is to refresh a verdict that's gone stale: if a
change makes an approved/reviewed claim false, that sign-off is void; if it's below the level the
artifacts reasoned about, it isn't.

Two qualifiers keep this lean: **spike before re-designing** — when an approved approach proves
infeasible, prove a working one with a throwaway spike first, then rewrite the artifacts around the
*proven* approach (the reactive twin of *spike unproven assumptions* in [The cycle](#the-cycle));
and **scale the re-review to the change** — a contradicted decision re-reviews only that slice, a
scope change re-confirms the spec, not the full panel every time.

## Tracking

Three ledgers under `docs/work/` track product work that isn't an active item, split by **how it
enters** and **whether it's approved**:

- **[IDEAS.md](../work/IDEAS.md)** (`IDEA-NN`) — proposals the user is **unsure** about, **not yet
  approved**.
- **[BACKLOG.md](../work/BACKLOG.md)** (`BL-NN`) — work the user is **committed** to: requested with
  confidence, or graduated from IDEAS. Already approved; a ready promotion candidate, no trigger.
- **[DEFERRED.md](../work/DEFERRED.md)** (`DEF-NN`) — scope cuts and review leftovers that fall out
  **during work items**. Already approved (the item's process vetted the cut); trigger-gated (each
  says *when* to take it on).

Whip's fourth ledger (`Bugs.md`, `BUG-NN` — confirmed defects) is **not** ported: Smatchet tracks
product bugs as GitHub Issues. Where the source lifecycle says "Bugs", read "a confirmed bug issue";
the reproduce-first rule carries over — no triage gate, reproduce, then log the issue.

Goal-setting draws from **BACKLOG, DEFERRED, and confirmed bug issues** (all promotable). Ideas are
the upstream funnel — they can't be promoted to a work item directly; an idea graduates to BACKLOG
first (the ideas pre-step). **Graduation is a move** — delete the `IDEA-NN` and repoint its
cross-references to the new `BL-NN`, no tombstone (the same discipline as *retire at close* below).

**Per-item capture — `Spawned.md`.** Work an item spawns is **not** written into the canonical
ledgers mid-flight. It's captured in the item folder's `Spawned.md` — an ungated sheet grouped by
destination (Deferred / Ideas / Backlog / Bugs), each entry a title + one-liner + source, with **no
canonical ID** — assigned at close.

**Lifecycle.** **Capture** into the item's `Spawned.md` — a deferral when simple-now / complex-later
is chosen, a backlog item when the user commits to one (or an idea graduates), a bug when a defect
is confirmed. **Sweep** at post-implementation review — the `5-review-*.md` files confirm every such
choice reached `Spawned.md`. **Migrate at close** — drain `Spawned.md`: assign each entry its
canonical `DEF/IDEA/BL-NN` (bugs become GitHub Issues) and move it into the matching ledger.
**Consult** at goal-setting — a taken-up entry becomes `Promoted → NN-slug`. **Retire at close** —
**remove** every entry the item **resolved** (the one it took up, plus any it implemented or
dropped); the durable record lives in that item's `docs/work/closed/NN-slug.md` (Shipped / Spawned
work) + git history, and any surviving cross-reference is repointed to the resolving item. Each
ledger holds only **open** and **`Promoted → NN-slug`** work — a completed entry is **deleted, not
left as a `Done`/`Dropped` tombstone**.

**Citing a resolved or renumbered id.** Every `DEF/BL/IDEA` id cited in a doc must still exist in
its ledger. To mention an id that is gone, use one of exactly two accepted forms (a lint enforcing
them lands with the Phase 2 tooling):

- A closed-item link — `[DEF-NNN](<path to docs/work/closed/NN-slug.md>)` — the id is the **link
  text**, the target path contains `closed/`, and **the file must exist**; a link that doesn't
  resolve fails rather than excusing.
- **`DEF-NNN (retired)`** — the parenthetical immediately after the id, when no successor can be
  named. Bare prose ("… is retired") does not satisfy the check.

Source files cite **no** ids at all. The ledger is the source of truth; artifacts may cite a
`DEF-NN`/`BL-NN`/`IDEA-NN`.

## Current item

Several items may be open at once at different early stages (research / spec / design / plan); at
most one is in **active development** (implementation → close) — the **oldest open item**, finished
before the next begins implementing. A session works **one named item**; reviewers take their
**code** from the last commit and their **item** from the invocation. The `NN` is allocated when the
item's folder is created (the next free number) — never pre-claimed; a forward reference to future
work cites its ledger ID instead.

## Closing

At close the item's folder collapses into a single frozen summary at `docs/work/closed/NN-slug.md`:
drain `Spawned.md` into the ledgers (assigning canonical IDs), retire the resolved ledger entries,
diff the close against the binding dispositions (so a `fix` can't silently fail to land), write the
summary, delete the folder. A closed item's summary is **frozen at close** — not updated afterward;
anything further, including a follow-up, starts a new numbered folder.

## Retro

At item close the agent gives a short, candid retro on **how the user and the agent collaborated** —
what worked, what cost time or rework, and concrete changes for next time. Review covers the *work
product* (defects + dispositions); retro covers the *process*. No artifact — it's delivered in the
conversation, scaled to the item; durable lessons feed the self-improvement loop
([AGENTS.md](../../AGENTS.md) § Self-improvement loop), and once one proves repeatable it's promoted
into a doc rule.

## Priorities

Priorities steer trade-offs and the questions asked during discussion, in two forms: **ranked
priorities** (competing concerns; higher wins) and **hard constraints** (non-negotiables). The
project-wide baseline is the quality-pillar hierarchy ([AGENTS.md](../../AGENTS.md) § Quality
Pillars); each item's `1-specification.md` records only its **emphasis or overrides** (right after
`## Goal`), else inherits the baseline.

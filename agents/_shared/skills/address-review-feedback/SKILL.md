---
name: address-review-feedback
version: 1
description: Resolve a review panel's findings as the single addresser — dedup, reconcile, bind one disposition per finding, land fixes, and record the resolution file the close diffs against. Use after a pre- or post-implementation review round on a work item ("address the review", "resolve the findings", "write the resolution"), or whenever multiple review files for one round await resolution.
---

# Address review feedback

A review panel produces findings; one **addresser** resolves them. The panel is deliberately many
*independent* passes that don't consolidate — dedup, reconciling conflicts, and the **binding**
disposition are the addresser's job (a reviewer's disposition is a *suggestion*). Ported from
Whip-Process `Procedures/AddressReviewFeedback.md`; contract wording carried verbatim. Policy
(problems only, verify then stop, dispositions):
[work-items.md § Addressing feedback](../../../../docs/agent-rules/work-items.md).

Two entry points here, differing only in the downstream target and the resolution filename:

| Entry point | Findings live in | Resolution file (in the item folder) | Downstream target |
|---|---|---|---|
| Pre-implementation review | `4-review-*` | `4-resolution-[round]-[harness]-[model].md` | the spec/design/plan artifacts (cascade) |
| Post-implementation review | `5-review-*` | `5-resolution-[round]-[harness]-[model].md` | the implementation diff, kept build-clean |

(Whip has two further entry points — a standing-code *audit* and an itemless *generic review*. The
audit maps to the `historical-code-review` skill; generic was not ported. Either reuses this
addresser discipline unchanged.)

**`[round]` is the review round being resolved** — the same number as the gathered review files,
**not** an addresser pass count. Revising your own resolution amends it **in place**; only a new
review round gets a new resolution file.

## Do exactly this

1. **Gather the round's findings.** Default to the **highest `[round]`** among the gate's review
   files, unless the invocation names one. If counters diverged (an earlier round was never
   resolved), gather **every unresolved round** into one resolution named for the highest — a lower
   round already resolved is not re-opened.

2. **Dedup and reconcile.** Merge findings several reviewers raised (cite each source file). Where
   reviewers conflict, the addresser decides and records why — this is exactly the consolidation the
   independent panel deliberately leaves undone.

3. **Verify each finding against the CURRENT artifacts/code** — the working tree and the live
   artifact text, not the reviewer's quote. Findings can be stale, duplicate, or wrong; a
   non-actionable finding gets a disposition (`accept`, with the reason it doesn't hold) and a note,
   **not** a change.

4. **Bind one disposition per deduped finding** — `fix` / `defer (→ Spawned.md)` / `accept`:
   - **fix** — pre entry point: edit the artifact and **cascade downstream** (spec → design → plan;
     work-items.md § Handling changes). Post entry point: change the diff and keep the build clean.
   - **defer** — capture in the item's `Spawned.md` (title + one-liner + source); the canonical
     `DEF/IDEA/BL-NN` is assigned at close, not here (bugs become GitHub Issues).
   - **accept** — record the rationale.

5. **Record the resolution file** — one entry per deduped finding: which reviewer(s) raised it, the
   binding disposition, and the resolution (what changed, `file:line` / artifact §; or the
   `Spawned.md` line; or the accept rationale — verified). This file is the checklist the close
   **diffs against** — the guard against a `fix` silently never landing.

```text
# <NN-slug> — Review resolution (round N, <pre|post>)

---

## R1 — <finding, one line>
- **Raised by:** 4-review-1-claude-opus §3; 4-review-1-codex-sol §2
- **Disposition:** fix | defer (→ Spawned.md) | accept
- **Resolution:** <what changed (file:line / artifact §) | Spawned.md entry | accept rationale — verified>
```

Scaled to the item: a trivial round's resolution is one line.

6. **Aggregate the panel verdict and record the ack** (post entry point only). Every `5-review-*`
   leg ends with a `## Verdict` trailer (adversarial-code-review skill § Panel leg). After the
   `fix` dispositions land and the build is clean, run the pipeline:

   ```bash
   python agents/scripts/core/panel_verdicts.py --subject NN --round N \
       --authoring-harness <your harness tag, e.g. claude> > samples.json
   python scripts/dev/verifier-sidecar.py aggregate samples.json > verdict.json
   bash agents/scripts/core/review-ack.sh --record --branch --verdict verdict.json
   ```

   `--authoring-harness` is **not optional in practice** — pass the harness you are running on. A
   leg on that harness is not an independent backend no matter which model tag it carries, so it is
   dropped from the score (its veto is still kept: self-reported bad news is trusted, good news is
   not). If the adapter exits **3**, every leg was your own harness and none vetoed — there is
   nothing independent to score, so skip the two lines below and record a presence-only ack
   (`review-ack.sh --record --branch`, no `--verdict`). That is a normal degraded-roster outcome,
   not a failure; note it in the close.

   `--branch` pins the ack to the committed branch diff as it stands when you record — i.e. after
   the fixes, which is the state heading into the close. A leg without a trailer is skipped with a
   warning (the panel degrades); a malformed trailer fails the adapter — fix the leg file, never
   drop it, it may hide a veto. Only `hard_veto` blocks downstream (`review-ack.sh --check` exits
   3); the aggregate score is advisory until calibrated.

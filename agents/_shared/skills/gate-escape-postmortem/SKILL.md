---
name: gate-escape-postmortem
description: Run a blameless postmortem on a gate escape — something that shipped to develop that a gate should have caught (a non-SUCCESS check at merge, an override label, a Revert, an overdue deviation, or a named post-merge bug). Identify the escaped class, do blameless root-cause, name the concrete PREVENTING GATE (mandatory — the entry cannot close without it), author a subagent-eval case when the miss was agent-reviewable, file that gate as a normal self-improvement category entry, and append to docs/self-improvement/postmortems.md. Use when the postmortem-owed SessionStart nudge fires, or the user says "postmortem", "gate escape", "why did this ship", "post-merge bug". Escalate deep C++ root-cause to debug-detective. Read-write — edits the ledger + a category entry.
version: 1
---

# gate-escape-postmortem

The invocation shortcut for the gate-escape postmortem. Plan + rationale:
[`docs/plans/shipped/gate-escape-postmortem.md`](../../../../docs/plans/shipped/gate-escape-postmortem.md).
This skill is the incident **finder**; the existing self-improvement apply-loop
(`docs/self-improvement/categories/*` + `AGENT_SELF_IMPROVEMENT.md` threshold +
triage) is the **applier**. No second system.

## When

- The `postmortem-owed` SessionStart nudge fired (`agents/scripts/core/postmortem-owed.sh`).
- User says "postmortem" / "gate escape" / "why did this ship" / "post-merge bug".
- An override label / `Revert` / overdue deviation marker (the project's
  `lint.deviation_keyword`) shipped and the
  escape has no `postmortems.md` entry yet.

## Workflow

1. **Identify the escaped class.** From the trigger, name what shipped that a
   gate should have caught — and *which gate* (a non-required CI job that was red,
   a missing required check, an override used, a check that doesn't exist yet).
   Run `bash agents/scripts/core/postmortem-owed.sh --list` to see the open set.
   **Record the provenance of any gate-tool output you cite** — the tree and commit
   the tool ran from (e.g. "`merge-gates.sh` from `<integration-tree>` @ `ff0ee7a6`").
   Gate scripts live in the repo, so a long-lived session branch runs a *months-old*
   copy that emits plausible, correctly-formatted verdicts about rules `develop` no
   longer has. One postmortem asserted the sanctioned merge path was structurally
   unusable off exactly such a run (`gate-tooling-run-from-stale-session-branch`).
   Undated tool output is not evidence; reproduce from a worktree based on
   `origin/develop` before the finding goes in the ledger.
2. **Blameless root cause.** Why did the gate not catch it? Name the *gate hole*,
   never an agent/person. If the root cause needs deep C++ investigation (a
   product bug a check should have caught), **escalate to `debug-detective`** and
   resume with its diagnosis — that is the one branch that exceeds this skill's
   bounded rubric.
3. **Name the PREVENTING GATE (mandatory).** The concrete new gate / rule / test /
   lint that catches the *class*, e.g. "make the doc-validation contexts
   required", "add markdownlint to the pre-push gate", "tighten the perf scenario
   map to cover X". A legitimate, correct override closes with
   `### Preventing gate: none — override legitimate (<reason>)` — itself a
   recorded decision. **An entry cannot close without this field.**
4. **Author the eval case (when agent-reviewable) — mandatory field.** If the
   escaped class is one a *reviewer agent* could have caught (a code smell, a
   logic bug, or a policy violation that `code-review` / `coderabbit-triage` /
   a review agent scores — as opposed to a pure CI-config, infra, or
   missing-required-context gap), the RCA **is** an eval-case spec: the missed
   defect is the input, "a competent reviewer flags `<X>`" is the reference
   outcome. Add it as a candidate case for the subagent-eval corpus
   ([`subagent-eval-agentic-coverage.md`](../../../../docs/plans/active/subagent-eval-agentic-coverage.md)),
   **suggestion-only** — a human attaches the reference outcome + promotes it;
   never auto-promoted (same curation gate as the harvest flywheel,
   [`subagent-eval-flywheel.md`](../../../../docs/plans/deferred/subagent-eval-flywheel.md)).
   Not every escape qualifies; when the gate hole is not agent-reviewable,
   record `### Eval case: none — not agent-reviewable (<reason>)`. This closes
   the postmortem→eval flywheel at the exact point the harness demonstrably
   missed a defect. **An entry cannot close without this field.**
5. **File the gate into the existing loop.** Write the preventing-gate action as
   a new one-entry-per-file note at `docs/self-improvement/categories/<cat>/<YYYY-MM-DD>-<slug>.md`
   for the matching `<cat>` in `{tooling,process,test,infra,security}` (existing
   format + priority + threshold; one entry per file — see `AGENT_SELF_IMPROVEMENT.md`
   § Format) — so the established apply-loop applies it. Do NOT auto-apply the gate
   here; suggestion-only.
6. **Append to the ledger.** Add one entry to `docs/self-improvement/postmortems.md`
   (newest first) in the shape its header documents — including the `### Eval case`
   field from step 4 — with `### Filed as` linking the category entry from step 5.
   Reference every escaped `PR #N` so `postmortem-owed.sh` dedupes them.
7. **Ship PR-only.** The ledger + category edits are docs → a normal PR (never a
   direct push to develop).

## Out of scope

- Auto-applying the preventing gate (the normal loop applies it).
- A per-PR retrospective (covered by plan § Deviations).
- A CI-blocking "postmortem required" gate (deliberately advisory — blocking
  would re-introduce the ceremony tax).
- Deep C++ root-cause (escalate to `debug-detective`).

---
name: adversarial-code-review
version: 1
description: Adversarial code review of a diff or PR to surface real bugs the author would actually want to fix, with a high bar and no theater. Use when the user asks to "review this", "review the diff", "review my changes", "review this PR", "any bugs in this?", "adversarial review", "tear this apart", "code review", or similar. Also use proactively before opening a PR or after a substantial set of changes. Also the reviewer procedure for a work item's post-implementation review gate (run-review.sh --gate post) — see § Panel leg.
---

# Adversarial Code Review

Review a code change as if you will be the one paged when it breaks. Surface real bugs, not proof that you looked busy. A review with two genuine findings beats a review with twelve performative ones.

The author already knows what they wrote. Find what they missed.

## Step 1: Understand The Intent

Before reading the diff, learn what the change is supposed to do. Read the PR description, commit messages, linked issue, plan doc, or most recent commit body.

Build a hypothesis: "the author claims this change does X." The strongest finding is often that the code does not match the stated intent: a renamed flag not flipped, a fix that handles only some described cases, or a refactor that quietly changes behavior.

If intent is unrecoverable, say so in the final report and review on structural grounds only.

## Step 2: Get The Diff

Use git when reviewing local work:

```bash
git remote show origin
MERGE_BASE=$(git merge-base origin/${TARGET_BRANCH:-main} HEAD)
git diff $MERGE_BASE HEAD
git diff HEAD
git diff --cached
git status --short
```

If the target branch is ambiguous, use the default branch from `git remote show origin` before guessing. Read committed, staged, unstaged, and relevant untracked files as one review surface.

For a PR, inspect the PR description, branch diff, review comments if relevant, and the exact head commit under review.

## Step 3: Decide What Counts As A Finding

Flag an issue only if all are true:

- It meaningfully affects correctness, performance, security, reliability, data integrity, or long-term maintainability.
- It is discrete and actionable: one specific problem with one specific fix path.
- It was introduced by this change, or this change materially worsens an existing problem.
- A reasonable author would likely fix it if shown the evidence.
- You can prove it from the diff and surrounding code, not merely speculate.
- It matches the rigor level of the codebase.
- It is not just an intentional design choice you dislike.

If nothing clears this bar, say **No findings**. A clean review is a valid outcome.

## Step 4: What To Look For

Read the diff line by line, then inspect surrounding code only where needed to prove or dismiss a concern.

Prioritize:

- Intent mismatch: docs, issue, UI, config, test names, or commit message promise behavior the code does not deliver.
- Boundary bugs: empty input, first item, last item, single item, duplicate item, missing item, deleted item, oversized input.
- State bugs: stale caches, partial updates, inconsistent persisted state, rollback gaps, unsaved user changes, reentrancy.
- Error paths: swallowed failures, cleanup skipped on early return, success reported after partial failure, retry loops without bounds.
- Compatibility breaks: changed public contracts, serialization shape, CLI output, file paths, env vars, feature flags, migrations.
- Concurrency and async ordering: races, lifetime hazards, cancellation gaps, callbacks after destruction, UI-thread blocking.
- Security and privacy: injection, path traversal, auth bypass, token leakage, unsafe logging, permission expansion.
- Performance regressions: new work in hot loops, synchronous I/O on UI paths, accidental O(n^2), unnecessary allocations in tight paths.
- Test deception: tests asserting the implementation rather than behavior, snapshots masking wrong output, missing regression coverage for the stated fix.
- Build and packaging gaps: new files not included, platform-specific includes leaking into shared code, generated metadata not updated.

For each suspicion, try to disprove it. If you cannot name the exact failing path, affected caller, or broken invariant, keep reading before reporting.

## Step 5: Verify Findings

Before reporting a finding:

- Quote or reference the exact file and line.
- Explain the failing scenario in concrete terms.
- Point to the surrounding caller, state transition, or contract that makes it real.
- Identify the smallest plausible fix.
- Run a targeted command when it can confirm the issue cheaply, or state that the finding is from code inspection only.

Do not report:

- Style-only preferences.
- Broad refactor suggestions.
- Missing tests without a concrete uncovered bug.
- "Could be cleaner" comments.
- Pre-existing issues the patch did not worsen.
- Hypothetical future requirements.
- Findings that rely on hidden assumptions you did not verify.

## Output Format

Lead with findings, ordered by severity. Keep summaries secondary.

Use this shape:

```markdown
Findings
- [P1] <short title> — <file>:<line>
  <Why this is a real bug, the scenario that triggers it, and the fix.>

Open Questions
- <Only questions that affect correctness of the review. Omit if none.>

Verification
- <Commands run, or "Code inspection only".>
```

Severity:

- `P0`: security/data loss/crash likely in common production use.
- `P1`: correctness, reliability, or serious performance bug in a realistic path.
- `P2`: real bug or maintainability trap in a narrower path.
- `P3`: low-risk issue the author may still want to fix.

If there are no findings:

```markdown
Findings
- No findings.

Verification
- <What you inspected or ran.>
```

## Panel Leg: Post-Implementation Review Of A Work Item

When launched as a review-panel leg (`agents/scripts/core/run-review.sh --gate post --subject <NN|slug> --round N`) — or asked to post-implementation-review an open item under `docs/work/items/NN-slug/` — the steps above run with these deltas (ported from Whip-Process `Procedures/PostImplementationReview.md`; mechanics: `docs/agent-rules/review-panels.md`):

- **The intent is the artifact set.** Step 1's intent source is the item's `1-specification.md` + `2-design.md` + `3-plan.md`; the review judges the implementation diff against them. Start from `git show HEAD` and read the changed files **in full**, not just the hunks — judge the code as it now stands, in context. If the diff doesn't point to a single `docs/work/items/NN-slug/`, ask rather than guess.
- **Read-only pass.** No builds, no test runs — judge test coverage by reading the tests, verify findings by reading the code.
- **Extra lenses on top of Step 4:** **plan fidelity** (delivers the full scope, nothing silently dropped, no drift), **match to spec/design**, and **test coverage for new or reshaped behaviour**.
- **Review independently** — form your own findings before reading any sibling `5-review-*` file; the panel's worth is independent passes.
- **Sweep spawned work** — confirm every deferral / idea / backlog entry / bug this item spawned reached its `Spawned.md` (work-items.md § Tracking), including any defect found while working the QA steps.
- **Output** replaces the § Output Format shape with the panel file: `5-review-[round]-[harness]-[model].md` in the item folder, at the exact path the launcher hands you (never self-named). Title `NN · <item title> — Post-implementation review (pass N)`; the "against …" line names the three artifacts. Problems only, each with a disposition `fix` / `defer (→ Spawned.md)` / `accept`; a pass with nothing actionable is a single line saying so.
- **Verdict trailer.** End the review file with a `## Verdict` heading followed by one fenced `json` block — the panel-verdict adapter (`agents/scripts/core/panel_verdicts.py`) collects it as one verifier sample, so the fields are the sidecar's sample fields: `criteria` (name → score in [0,1] over `task_satisfaction`, `correctness`, `evidence_quality`, `regression_risk`, `security`, `project_invariants`, `scope_discipline`, `verification_completeness`), optional `confidence` in [0,1], `hard_veto` (JSON boolean — never a string), `veto_reason` (say why when vetoing). Set `hard_veto` only for a security hole, an invariant breach, or a break in a deterministic gate — never for stylistic doubt; the continuous score is advisory until calibrated. A findings-free pass still carries a trailer (high scores, `"hard_veto": false`):

  ````markdown
  ## Verdict

  ```json
  {"criteria": {"task_satisfaction": 0.9, "correctness": 0.85, "evidence_quality": 0.8,
    "regression_risk": 0.9, "security": 0.95, "project_invariants": 0.9,
    "scope_discipline": 0.9, "verification_completeness": 0.8},
   "confidence": 0.7, "hard_veto": false, "veto_reason": ""}
  ```
  ````

- **No ack recording by a leg** — a leg never runs review-ack.sh. The addresser (address-review-feedback skill) aggregates the panel's verdict trailers and records the ack once the round is resolved; the user's sign-off closes the loop.

## Record The Ack (staged-diff reviews in this repo)

`scripts/git-hooks/pre-commit` refuses a commit whose staged diff is substantive C++ and carries no review acknowledgement pinned to that exact content (`docs/agent-rules/process-rules.md` § Code-review before every commit). When you reviewed the staged diff (`git diff --cached`) and no P0/P1 finding is outstanding, record it so the commit can proceed:

```bash
bash agents/scripts/core/review-ack.sh --record --staged
```

If you emit a verifier object (`overall_score`, `confidence`, `hard_veto`, per-criterion scores), aggregate and attach it so the gate sees the verdict:

```bash
python3 scripts/dev/verifier-sidecar.py aggregate samples.json > /tmp/verdict.json
bash agents/scripts/core/review-ack.sh --record --staged --verdict /tmp/verdict.json
```

Only `hard_veto` blocks the commit — set it for a security issue or invariant breach, never for a stylistic doubt. The continuous score is advisory until calibrated.

Do not record while a P0/P1 stands, and never record a review you did not run — the fingerprint proves only that the diff is unchanged since something was acknowledged. Any later staged edit re-arms the gate.

## Review Discipline

Be concise and specific. Do not congratulate, hedge, or pad.

One strong finding is enough. Zero findings is better than noise.

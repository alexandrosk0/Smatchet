- 2026-08-11 · claude-code · [process] · P2 — the `[pre-first-push gate]`'s self-review step is the only one with no backstop, so skipping it is invisible; skipped on PR #1996 and it cost ~8 CodeRabbit cycles, both bots' rate limits, and four locally-knowable defects reaching CI

  Details: [`ship-loops.md`](../../../agent-rules/ship-loops.md) § `[pre-first-push gate]`
  makes a local self-review mandatory before the first push, "never deferring
  locally-knowable findings to CI/CR", and the
  [`adversarial-code-review`](../../../../agents/_shared/skills/adversarial-code-review/SKILL.md)
  skill says to use it "proactively before opening a PR". On PR #1996 the gate's
  other steps were either run or genuinely n/a (no strict-zone C++ touched, so
  the dual-target `/WX` build, `ctest`, and the leaf-`AGENTS.md` self-review did
  not apply). **The review step was simply not run.** It happened 14 commits
  later, only because the user asked "have you code reviewed the changes?".

  **Measured cost on that one PR**, all of it the churn `reduce-coderabbit-review-spend`
  Slice 1 exists to prevent:

  - ~8 completed CodeRabbit review cycles across the PR's 16 commits.
  - CodeRabbit's adaptive per-developer limit hit repeatedly (27–49 min waits,
    three `@coderabbitai review` requests answered only by the plan/rate-limit
    note). The rate-limiting that dominated the session was substantially
    self-inflicted by this PR's own churn.
  - Cursor Bugbot's usage cap hit on at least three separate heads.
  - **Four locally-knowable defects reached CI/CR** that the review found the
    moment it finally ran: the develop-side half-set glob enumeration, the
    silently-dead grace cutoff, and two docs this PR itself falsified. None
    needed CI, a reviewer, or a running gate to find — only reading the diff.
    (A fifth of the same class, the CWD-relative glob expansion, was caught by
    CodeRabbit as a *trivial* nitpick and escalated on inspection. A pre-push
    review would plausibly have found it too, but the credit is CodeRabbit's,
    not the self-review's — and this entry got that attribution wrong on its
    first draft, caught only by re-checking before merge.)

  **The structural point, which outlives this PR.** The automatic backstop
  (`scripts/git-hooks/pre-push` step D) mirrors the *mechanical* required checks
  — lint rules, doc anchors, markdown links, portable purity, clang-format,
  shell-lint. It cannot mirror a judgement step. So of the four gate items, three
  either self-enforce or are visibly n/a, and the fourth **leaves no trace either
  way**: a PR whose review was skipped and a PR whose review was clean look
  identical from outside.

  That is precisely the `required-check-that-never-reports-is-invisible` shape
  the same PR was fixing in CI — a check that produces no signal reads as a pass.
  Worth noting the gate was skipped *by the session working on that entry*, which
  suggests the failure is structural rather than a lapse of attention.

  Candidate fixes, cheapest first:

  - **Record the verdict.** Require the PR body's test plan to carry a line for
    the pre-first-push review (e.g. `adversarial-code-review: N findings, all
    fixed` or an explicit `n/a — trivial diff`). Makes absence visible without
    enforcing anything; the `Intent section` workflow already parses the body, so
    there is a home for the assertion.
  - **Cite it like gate evidence.** PR #1996 added the rule that gate-tool output
    must record the tree + commit it ran from. The same discipline applied here
    would make "the review ran, against this diff" checkable rather than assumed.
  - **Do not** try to enforce it in the pre-push hook. The step is a judgement
    call; a hook can confirm a claim was made, never that the review was real.
    An enforcement that can only check the claim would manufacture exactly the
    kind of green this backlog keeps filing entries about.

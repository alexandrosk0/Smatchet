---
category: test
priority: P2
status: open
discovered: 2026-06-24
discovered-by: orchestrator (Claude Code)
pr: 1552
---

# Non-hermetic `test-pre-push-merged-pr-guard.sh` mis-fires on PR merge-ref checkout

## Symptom

`Agentic self-tests (bats)` RED on PR #1552 with `test-pre-push-merged-pr-guard.sh`
reporting **6 passed / 6 failed** — but **only in CI**, green on every head-only
local run. The 6 failures (assertions 4, 5, 6a/6b/6c, 7, 8a/8b, 9) all showed the
wrong exit code / a missing recovery banner.

## Root cause

The test was **non-hermetic**. GitHub Actions `pull_request` checks out the
**merge ref** (`refs/pull/N/merge` = PR head auto-merged with `develop`), not the
PR head SHA. That tree's `scripts/git-hooks/pre-push` contains develop's
**gate D** (the local-CI delta-gate) and the **plan-lock probe (C)** — two probes
that run *before* the merged-PR guard the test actually exercises.

The test invokes the hook through a stripped `env -i` sandbox (only PATH + HOME).
In that sandbox:
- gate D's `test-lint-rules.sh` exits non-zero because its tooling
  (python / clang-format / shellcheck) is absent from the minimal PATH → the hook
  reads that as a violation and **refuses early** (exit 1) before reaching the guard.
- the plan-lock probe reads the **real** branch via `git symbolic-ref` (not the
  stubbed `git rev-parse`), so a real feature-branch checkout with plan-locked
  changed files can also refuse early.

On a head-only checkout the PR branch's own hook lacked gate D, so the test passed
locally — the failure surfaced **only** once CI merged develop's gate-D hook into
the tested tree. Classic merge-ref-reintroduces-develop-code trap.

## Fix (shipped on #1552)

Neutralise the two sibling pre-gh probes inside the test's `run_hook` `env -i`
block so the merged-PR guard is tested in isolation (each probe owns its own test):

```bash
out=$(env -i \
    PATH="$sandbox_path" \
    HOME="$HOME" \
    SMATCHET_SKIP_PRESHIP_GATE=1 \
    SMATCHET_ALLOW_UNLOCKED_PUSH=1 \
    SMATCHET_ALLOW_MERGED_PR_PUSH="${extra_env#SMATCHET_ALLOW_MERGED_PR_PUSH=}" \
    bash "$HOOK" 2>&1)
```

Verified 12/12 against a throwaway merge-ref worktree whose hook has gate D (the
exact CI condition).

## Lesson / preventing recurrence

A bucket-A unit test that drives a **whole script with sibling stages** must
neutralise every stage it is *not* testing — otherwise an unrelated stage gaining
a new environment dependency (here: gate D landing on develop) silently breaks the
test on the merge-ref checkout while local head-only runs stay green.

Proposed guard: a lint/checklist rule that any `env -i` hook-invocation in an
agentic self-test must explicitly set the escape var for **each** independent
stage of the script under test (grep the hook for `exit 1` stage-guards, assert a
matching `SMATCHET_*` neutraliser in the test). Cheap, catches the next
gate-added-to-a-shared-hook regression at author time rather than at merge-ref CI.

Related: the merge-ref-vs-head-SHA distinction is already documented
(merge ref reintroduces develop-only code into the tested tree); this is a concrete
instance worth citing when that doc is next touched.

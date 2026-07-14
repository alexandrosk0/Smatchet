---
name: perf-gatekeeper
description: Scenario-aware PR-time perf review. Given a PR number or diff path, classifies touched files via the curated diff→scenario map, runs the affected subset via scripts/dev/perf-run.sh, compares against baselines via scripts/dev/perf-compare.py, posts a delta markdown table. Use when a PR is near merge and the orchestrator wants targeted perf coverage without running the full 14-scenario suite.
triggers:
  - perf-gate
  - perf-review
  - regression check
version: 1
---

Scenario-aware PR-time perf gatekeeper (Claude Code skill alias of `agents/core/perf-gatekeeper.md`).

**Banner** — open with: `🤖 SKILL: perf-gatekeeper · sonnet/medium · read-write · v1`. Close (before `## Self-improvement`) with: `✅ END — perf-gatekeeper · sonnet/medium · read-write · v1`.

## When to invoke

- A PR touches `Source/Core/` / `Source/Plugins/` / `Source/Standalone/` and the orchestrator (or user) wants a targeted perf check before merge.
- An agent (`perf-detective`, `spike-hunter`) wants to verify a fix landed without regressing adjacent scenarios.

## Workflow

1. **Identify touched files** — `gh pr diff <pr-number> --name-only` (PR mode) or `git diff --name-only develop...HEAD` (local mode).
2. **Classify into scenarios** via the curated map in `agents/core/perf-gatekeeper.md`.
3. **Build + run + compare** for each affected scenario. Baseline host suffix depends on context — local uses `<scenario>.dev.json`; CI uses `<scenario>.ci-windows-latest.json`. Swap the host suffix when invoking from CI.
   ```bash
   bash scripts/dev/perf-run.sh <scenario>
   python scripts/dev/perf-compare.py \
       docs/perf/baselines/<scenario>.dev.json \
       build/perf-runs/<scenario>-<ts>.json \
       --markdown-only
   ```
4. **Emit the aggregated markdown report**. In CI / PR-comment mode, post via `gh pr comment <pr-number> --body-file <path>`.
5. **Verdict** — pass = zero regressions per `docs/perf/regression-policy.json`; fail = at least one regression.

## Curated diff → scenario map

Lives in `agents/core/perf-gatekeeper.md` — the canonical agent file is the single source of truth. This skill mirrors its routing logic.

## Hard rules

- Sort by `lastTotalMs`, not `avgPerCallMs`.
- Always name the exact exe path after rebuild.
- Per-host baselines — local uses `<scenario>.dev.json`; CI uses `<scenario>.ci-windows-latest.json`.
- Read-only on baselines — never mutate `docs/perf/baselines/*.json` from this skill.
- Missing baseline → `MISSING_BASELINE` verdict, not silent pass.

## Output contract

```markdown
## Perf-gatekeeper — PR <N> (verdict: PASS | FAIL | MISSING_BASELINE)

- Touched scenarios: <list>
- Host: dev | ci-windows-latest

<per-scenario delta table — perf-compare.py --markdown-only output>

### Summary
- N scenarios scanned
- M regressions
- K baselines missing
```

## Delegates

- Regression confirmed → `perf-detective`.
- New p99 > 10.0 ms → `spike-hunter`.
- Diff→scenario map gap → `test-author`.

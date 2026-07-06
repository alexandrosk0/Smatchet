# perf-full's gh/git steps lack `shell: bash` → scheduled full-suite perpetually RED (silent)

- **Category:** infra
- **Priority:** P2
- **Date:** 2026-07-06
- **Status:** open

## What I found

While shipping the perf-gate step-5 recapture dispatch, the recapture step failed with a PowerShell `ParserError` on bash syntax. Root cause: on `windows-2022`, a `run:` step with **no `shell:`** defaults to **PowerShell**, and `perf-full.yml` has no job-level `defaults.run.shell`. Only three steps declare `shell: bash` (Mesa install / Discover scenarios / Run full suite). **Every other bash step lacks it**:

- `Open scenario-run-failure issue`
- `Open regression issue`
- `Open baseline-bump PR for improvements`

All three use bash (`gh issue create \` line-continuations, `git checkout -b "$branch"`, `while IFS= read`) and therefore **fail under PowerShell whenever they fire** (`Missing expression after unary operator '--'`).

## Why it's been invisible

`Open scenario-run-failure issue` fires on `run_failure_count > 0`. The full suite discovers ~14 scenarios via `scenario.list`; **8 non-baselined ones always fail to spawn on the runner** (MCP-ready / `app.quit` timeouts), so `run_failure_count > 0` on **every** run → the step fires → fails under PowerShell → **the job is RED every time**. Evidence: Perf full-suite `schedule` runs on 2026-06-30 / 07-01 / 07-02 / 07-03 are all `failure`. Nobody noticed — it's an unwatched scheduled job with no required-check surface.

Net: the perf-run-failure / regression / improvement-bump **auto-issue + auto-PR mechanisms are silently dead** — they've never actually created an issue or PR because they crash before `gh` runs.

## Blast radius of the naive fix

Just adding `shell: bash` to `Open scenario-run-failure issue` makes it *work* — and it uses a per-run-id unique title (`perf-run-failure: full-suite run <id>`), so it would then create **one new issue per run** (daily + every dispatch) = spam. A correct fix must pair `shell: bash` with (a) dedup / stable-title-upsert on the issue step, and (b) a decision on the 8 always-failing scenarios: are they genuinely broken on CI, should they be excluded from the full-suite set, or is their spawn-timeout expected (and thus not issue-worthy)?

## Fix (proposed, not done here)

1. Add `shell: bash` (or a job-level `defaults.run.shell: bash`) to all bash steps — **but** first:
2. Make `Open scenario-run-failure issue` idempotent (stable title + `gh issue list`-then-comment, not create-per-run).
3. Investigate the 8 non-baselined scenario spawn failures (`ai-assistant-send-s2-s4-s5`, `code-syntax-coloring`, `command-contract-sweep`, `command-palette-fuzzy`, `dock-gap-sentinel`, `mobile-texture-guard`, `theme-switch-roundtrip`, `ui-test`) — fix or exclude.
4. Consider surfacing the scheduled full-suite conclusion somewhere watched (it's been red for ≥ a week unnoticed).

The step-5 recapture dispatch works around this: its step declares `shell: bash` and runs under `!cancelled()`, so it creates the baseline PR even while the job stays red from the pre-existing broken issue step.

## Self-improvement

Empty.

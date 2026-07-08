# perf-full's gh/git steps lack `shell: bash` → scheduled full-suite perpetually RED (silent)

- **Category:** infra
- **Priority:** P2
- **Date:** 2026-07-06
- **Status:** RESOLVED (2026-07-08)

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

## Addendum (2026-07-06) — second latent failure in the same steps

Even with `shell: bash`, the PR/issue-creating steps hit a **second** wall: this repo disables **Settings → Actions → General → "Allow GitHub Actions to create and approve pull requests"**, so `gh pr create` (and `gh issue create`) via `GITHUB_TOKEN` fail with `GraphQL: GitHub Actions is not permitted to create or approve pull requests (createPullRequest)`. So `Open baseline-bump PR for improvements` would fail here **even after** the shell fix. Two fixes possible: (a) enable that repo setting (one-time, admin), or (b) have the workflow only **push the branch** and let a human/orchestrator open the PR. The step-5 recapture step took path (b). The improvement-bump step still needs one of them.

## Resolution (2026-07-08)

Fixed in `.github/workflows/perf-full.yml`. Note the `shell: bash` gap on the
three follow-up steps (`Open regression issue`, `Open scenario-run-failure
issue`, `Open baseline-bump PR`) was **already** patched in an interim commit
(they carry `shell: bash` + `!cancelled()` on origin/develop). Three remaining
gaps were closed:

1. **Scenario scope (the real red-maker).** The "Discover scenarios" step now
   intersects the `scenario.list` universe with the **committed** baseline set
   (`git ls-files docs/perf/baselines/*.ci-windows-latest.json`) — the same
   source of truth the recapture step uses. Only the 6 baselined scenarios run;
   the ~8 non-baselined ones are logged via `::notice::` (no silent drop) and
   skipped. Root cause of the 8 failures (confirmed, not fixed because they are
   **expected non-perf-runnable via `scenario.run --spawn`**, not broken):
   - **screenshot-required (bucket-C)** — `command-palette-fuzzy`,
     `dock-gap-sentinel`, `code-syntax-coloring`, `theme-switch-roundtrip`:
     each `OnStart` hard-requires `--screenshotPath`, which the perf path never
     passes → immediate start failure.
   - **test-engine (bucket-E)** — `ui-test`: needs `SMATCHET_BUILD_UI_TESTS=ON`
     (the `ninja-ui-test-msvc` preset), OFF in `ninja-iter-msvc`; emits no perf
     `rows`.
   - **not-a-perf-scenario** — `command-contract-sweep`, `mobile-texture-guard`,
     `ai-assistant-send-s2-s4-s5`: run fine but `OnFinish` returns bespoke
     result JSON (violations / recovery flags / streaming sub-results) with no
     `UiPerfMonitor` `rows`, so the perf harness's `data.rows` guard rejects
     them. (AI is compiled ON in the iter build, uses a loopback fake server —
     no network/creds; the failure was purely the output-shape mismatch.)
   With scope = baseline set, `run_failure_count > 0` now means a **real**
   plumbing break of an in-scope scenario.

2. **Idempotent issues.** `Open regression issue` + `Open scenario-run-failure
   issue` now use a **stable title** (no per-run-id) + find-then-comment
   (`gh issue list ... --search "$title in:title"` → `gh issue comment` else
   `gh issue create`), mirroring the sibling nightlies (sanitizer / fuzz /
   monkey). One tracking issue that gains a "Recurred: <run-url>" comment per
   recurrence — never spam-one-per-run.

3. **Baseline-bump via push-only.** `Open baseline-bump PR for improvements`
   renamed to `Push baseline-bump branch for improvements`: dropped the blocked
   `gh pr create` (repo disables "Allow GitHub Actions to create and approve
   pull requests"), now pushes the `bot/perf-baseline-bump-<run_id>` branch,
   stages only the tracked bumped baselines, and emits a `::notice::` for the
   orchestrator to open the review PR — same pattern as the recapture step.

Validation (cannot run Windows/CI locally): real YAML parser (17 steps OK) +
`bash -n` on all 7 `run:` blocks + `shellcheck -S warning` clean on the 4
touched blocks.

## Self-improvement

Empty.

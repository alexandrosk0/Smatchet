# Perf-gate revival (Pillar 1) — execution playbook

> **STATUS: COMPLETE — GATE REQUIRED (2026-06-07).** All steps done: headless
> launch proven on CI (steps 1–3), 4 human-approved `ci-windows-latest`
> baselines committed (step 4), `mean_min_abs_delta_ms=0.05` noise floor
> calibrated from the first armed run (step 5 round 1; `mean_abs_ceiling_ms`
> itself stays null pending more CI observations), merge-gates allow-list
> (6a), Pattern-C always-report shape (step 7 — **deviation**: Pattern C
> instead of the planned Pattern-B skip-companion, because the old filter's
> `!**/*.md` negation has no `paths-ignore` inverse — a Source-markdown-only
> PR would wedge a Pattern-B pair), and `Perf PR-fast (windows-2022)` added
> to `branch_protection.required_contexts` + applied (6b, user-confirmed).
> Open follow-ups (filed in categories/): `p99Ms` missing from the emitter
> (tooling P2); `mean_abs_ceiling_ms` value + perScenario overrides after
> more observed runs; `calls=1` top-row filtering. Derived from
> `build-quality-velocity-hardening.md` item **#8/#13** (2026-06-06).

## Implementation log

**2026-06-07 — steps 7 + 6b (gate REQUIRED, branch `feat/perf-gate-required`):**

- **Step 7 (always-report — Pattern C, not Pattern B):** removed the
  workflow-level positive `paths:` filter from `perf-pr-fast.yml`; added a
  fast `changes` detect job (fail-safe `perf=true` on uncertainty) gating the
  measurement job via `if:` — a skipped required job counts as success, so
  docs-only PRs never wedge. Pattern-B skip-companion rejected: GitHub
  supports `!` negation only under `paths`, so the old filter's `!**/*.md`
  has no `paths-ignore` inverse and a Source-markdown-only PR would trigger
  neither half of the pair (deadlock).
- **Step 6b (GitHub-required):** `Perf PR-fast (windows-2022)` appended to
  `project.config.json` `branch_protection.required_contexts` +
  `ci.required_checks`; applied to the live branch protection via
  `setup-branch-protection.sh` after merge (user-confirmed 2026-06-07).

**2026-06-07 — steps 4 + 6a (gate armed, branch `feat/perf-gate-baselines`):**

- **Step 4 (GOLDEN):** committed the 4 `ci-windows-latest` baselines
  (`idle`, `priority-grid-scroll`, `cell-edit-burst`, `ai-chat-history-render`)
  captured by run `27080545207` on commit `37ca9d6c` — **explicit human
  approval given 2026-06-07** (golden-image-approval flow; cross-host rule
  D1 honoured: runner-captured, never hand-ported). Sanity: top scope
  `SmatchetUI::Draw` 0.35–0.42 ms avg — far under the 6.94 ms budget.
  Committing these flips the live compare branch AND re-arms the plumbing
  hard-fail in both perf workflows.
- **Step 6a:** `merge-gates.sh` allow-list regex extended
  (`Coverage|Sanitizer|Bucket-` → `…|Perf PR-fast`) + a `merge_gates.bats`
  case (red non-required `Perf PR-fast (windows-2022)` blocks; 113-test
  suite green). `perf-out-of-band` remains the override hatch.
- **CodeRabbit round (PR #937):** null-safe policy render in
  `emit_markdown` + strict `data.rows` guard in `perf-run.sh` (both
  regression-tested).

**2026-06-06 — steps 1 + 2 + 5-code (SAFE-NOW batch, branch `feat/perf-gate-revival`):**

- **Step 1 (Mesa software-GL):** copied the bucket-C `Cache Mesa opengl32.dll`
  + `Install Mesa software OpenGL ICD` steps (pal1000/mesa-dist-win **24.2.5**,
  cache key `mesa-opengl32-24.2.5`, lockstep with `build-and-test.yml`) into
  `perf-pr-fast.yml` + `perf-full.yml`; set `GALLIUM_DRIVER=llvmpipe` +
  `LIBGL_ALWAYS_SOFTWARE=1` on every step that spawns the exe — including
  perf-full's `Discover scenarios from scenario.list` step (it runs
  `scenario.list --spawn`, which the plan's original step list missed).
- **Step 2 (scenario set):** removed `command-palette-fuzzy` from
  `perf-pr-fast-set.json` (bucket-C-only, hard-requires `--screenshotPath`);
  kept `idle`, `priority-grid-scroll`, `cell-edit-burst`,
  `ai-chat-history-render`; fixed the stale "Five scenarios" description.
- **Step 5-code (mean budget knob):** added `mean_abs_ceiling_ms` to
  `DEFAULT_POLICY` (`perf-compare.py`) + `regression-policy.json` default,
  shipped **null/disabled** pending step-5 calibration. Gates on
  `avgPerCallMs` (the snapshot rows' mean field — `lastTotalMs` is a single
  frame's total, not a mean) beside the p99 branch, honouring
  `min_baseline_calls`. `avgPerCallMs` delta column added to the markdown
  table; policy line shows `mean ≤ X ms` only when enabled.
- **Verified:** self-compare exits 0 with knob null; forced ceiling fires
  `avgPerCallMs … exceeds Pillar 1 mean budget` + exit 1 only on rows meeting
  `min_baseline_calls` (calls ≥ 10).

**2026-06-06 — step-3 live run + root-cause round 2 (same PR):**

- First live `Perf PR-fast` run with the Mesa step: **all 4 scenarios still
  failed** (~250 ms each, zero output, WARN-downgraded). Root cause found and
  **reproduced locally**: Mesa ≥ 22 splits the driver — `opengl32.dll` is a
  137 KB thin loader hard-requiring `libgallium_wgl.dll` (53 MB driver) +
  `libglapi.dll`; copying the loader alone makes the exe die at process start
  (`STATUS_DLL_NOT_FOUND`, silent). Local proof: loader-only → instant abort;
  all three DLLs → `perf-run.sh idle` runs 600 frames headless, `ok:true`.
- **Same bug is live in bucket-C/E** (`build-and-test.yml`) and masked by
  `continue-on-error: true` — bucket-C on this PR ran `Passed: 0  Failed: 3`,
  exit 1, check green. **Gate-escape postmortem owed** (bucket-C/E green-but-
  broken). Fixed all 4 Mesa blocks (3-DLL copy, strict final cp, cache key →
  `mesa-dlls-24.2.5-v2`).
- **p99 finding (was "unverified")**: now confirmed — `scenario.run` rows
  carry NO `p99Ms` (`avgPerCallMs`/`emaAvgMs`/`lastTotalMs`/`maxMs`/`calls`
  only). The `p99_abs_ceiling_ms` policy check is structurally inert until the
  C++ perf-snapshot emitter adds p99 — follow-up item, not this PR.

## Deviations from plan

- **Step 7 shipped as Pattern C, not Pattern B**: the planned `perf-pr-fast-skip.yml` companion cannot express the inverse of the old filter's `!**/*.md` negation (`paths-ignore` accepts no `!`), so a Source-markdown-only PR would have triggered neither workflow (wedged required check). Pattern C (always-trigger + fail-safe `changes` detect job + `if:`-skip) has no such hole and matches `build-and-test.yml` precedent.
- **Step 1 needed a round 2 the plan didn't anticipate**: Mesa >= 22 splits the driver — copying `opengl32.dll` alone (the bucket-C recipe the plan said to copy) is itself broken; `libgallium_wgl.dll` + `libglapi.dll` must ship beside it. The same bug was live-but-masked in bucket-C/E (postmortems.md 2026-06-07; preventing gate `bucket-lane-launch-smoke`, infra P1).
- **A third launch blocker surfaced**: `perf-run.sh` treated the CLI exit code as authoritative; the runner's post-scenario `app.quit` handshake flakes non-zero AFTER the result file is written. The result file (+ JSON/rows parse guard) is now the outcome contract.
- **Step 5 split**: `mean_min_abs_delta_ms=0.05` noise floor landed from first-armed-run data (a +12% / 3 µs false positive); `mean_abs_ceiling_ms` itself stayed null pending more observed runs. **RESOLVED 2026-07-06** by the [`perf-gate-step5-calibration`](perf-gate-step5-calibration.md) follow-up: 6 green ci-windows-latest runs (all scopes ≤ 0.72 ms avg, 9.6× under budget) → armed `mean_abs_ceiling_ms = 6.94`, empty `perScenario`.
- **p99 half of Pillar 1 found structurally inert**: `scenario.run` rows carry no `p99Ms`; filed tooling P2 (emitter change) — not fixed in this plan's scope. **RESOLVED**: the `p99Ms` emitter (`GetLastFrameRows(includeP99=true)`) landed, and the step-5 recapture (#1659) refreshed all baselines so their rows carry `p99Ms`.

## Verification (actual)

- Headless launch: two-phase local repro (loader-only -> instant `STATUS_DLL_NOT_FOUND` death; 3 DLLs -> 600-frame headless run `ok:true`) + live CI proof (run `27080545207`: 4/4 scenarios, `run_failure_count=0`) — PASSED.
- Baselines: 4 `ci-windows-latest` files validated (schema fields, host, 31–34 rows, sane numbers) + explicit human approval (golden gate) — PASSED.
- Armed compare: first armed run flagged a 3 µs false positive (caught by design); post-floor run: 4/4 "No regressions", "All scenarios within policy" — PASSED.
- `mean_abs_ceiling_ms` knob: self-compare exit 0 disabled; forced ceiling fires + respects `min_baseline_calls`; all-null policy renders without TypeError — PASSED.
- `perf-run.sh` guards: shellcheck clean; empty-`data.rows`-with-top-level-`rows` correctly FAILS; observed-flake JSON passes — PASSED.
- Merge-gates 6a: `merge_gates.bats` 113 ok / 0 fail incl. the new non-required-Perf-PR-fast-blocks case — PASSED.
- Pattern C live proof: PR #946's own `Perf PR-fast` run executes the new detect->measure shape against committed baselines — observed on the PR.
- Post-merge residue: `setup-branch-protection.sh` applied from updated develop; next docs-only PR must show the required check reporting as skipped-success (ci-required-check-pattern.md § Invariant check).

## Why this exists

Quality **Pillar 1** (steady-state UI work ≤ **6.94 ms** / 144 Hz; p99 ≤ **16.67 ms** /
60 Hz floor) is supposed to be CI-enforced by `.github/workflows/perf-pr-fast.yml`
(job **`Perf PR-fast (windows-2022)`**) + the scheduled `perf-full.yml`. It is
**not.** The gate exists and parses but is a **guaranteed-pass no-op** today.

## The 5 compounding reasons the gate WAS a no-op *(historical analysis, 2026-06-06 — every item below is fixed; see § Implementation log)*

1. **Zero `ci-windows-latest` baselines exist.** Both workflows compare against
   `docs/perf/baselines/<scen>.ci-windows-latest.json` (`perf-pr-fast.yml:137`,
   `perf-full.yml:109`) but `baselines/` holds only 12 `*.dev.json` and **zero**
   `*.ci-windows-latest.json` (`git log --all` for that glob is empty). Every
   scenario takes the **bootstrap branch** → `missing_baseline_count++` (not
   `regression_count`) → Gate decision exits 0.
2. **The missing-baseline path is non-blocking** (`perf-pr-fast.yml:138-150,202-204`):
   a missing baseline emits `::notice::`, `regression_count` stays 0, the gate passes.
3. **The plumbing-failure hard-fail is itself downgraded while no baseline exists**
   (`perf-pr-fast.yml:219-228`): a `scenario.run` failure is demoted from `exit 1`
   to `::warning::` when zero `*.ci-windows-latest.json` exist — **masking the
   unproven headless launch** (see below).
4. **`Perf PR-fast (windows-2022)` is not a required check.**
   `project.config.json` `branch_protection.required_contexts` = `[Test-delta gate,
   Windows + MSVC, Windows + MSVC (light), Shell lint (shellcheck), Doc anchors +
   agent contract]`. A non-required red perf check does **not** block a squash-merge.
   *(Updated post-#933:)* `merge-gates.sh` now blocks a FAILING check when it is
   required **or** its name matches the curated non-required allow-list regex
   `Coverage|Sanitizer|Bucket-` (case-insensitive, "advisory" excluded) — but
   **`Perf PR-fast` is not in that allow-list**, so a red perf check still passes
   the poller today. The regex is the designed extension point (see step 6a).
5. **No skip-companion workflow.** Making the check required without a
   `perf-pr-fast-skip.yml` (per `ci-required-check-pattern.md` Pattern B) would
   **deadlock every docs-only PR** (the path-filtered-required-check deadlock).

Plus a **policy gap**: the **6.94 ms absolute mean budget is not enforced anywhere**
in `perf-compare.py` / `regression-policy.json` — they enforce only relative
`mean_delta_pct=10.0`, absolute `p99_abs_ceiling_ms=16.67`, `max_abs_ceiling_ms=50.0`,
`min_baseline_calls=10`. The literal `6.94` is enforced **only** by
`scripts/dev/test-grid-edit-perf-postfix.sh:56` (grid-edit path, under the required
Test-delta gate). And those literals **do not read `project.config.json` at
runtime** — editing the config does not change enforced thresholds; the literals
must be updated in lockstep.

### Budget — single source of truth
`project.config.json` `perf` block: `frame_budget_ms=6.94` (= 1000/144),
`fps_floor_ms=16.67` (= 1000/60), `freeze_ms=100` (Pillar 2),
`baselines_dir=docs/perf/baselines`, `policy=docs/perf/regression-policy.json`.
Echoed in `AGENTS.md`, `docs/CONTEXT.md`, `.coderabbit.yaml`; exported to shell as
`PERF_BUDGET_MS`/`PERF_FLOOR_MS`/`PERF_FREEZE_MS` by `scripts/dev/project-config.sh`.

## Headless-launch root cause (the load-bearing fix)

There is **no headless/offscreen GL mode** and no `SMATCHET_HEADLESS` flag in the
tree. "Headless" = a hidden GLFW window (`GLFW_VISIBLE=GLFW_FALSE`,
`StandaloneAppBootstrap.cpp:304`) that **still needs a working `opengl32` ICD** to
reach `ImGui_ImplOpenGL3_CreateDeviceObjects`. The perf workflows provision **no
Mesa software-GL** (zero `mesa`/`GALLIUM_DRIVER`/`LIBGL_ALWAYS_SOFTWARE` matches),
so the `--spawn` ephemeral child fails device-object creation, never brings MCP up,
`WaitForMcpReady` times out, no `outPath` is written, `perf-run.sh:128-130` errors.

**The fix is pure workflow — no C++ change.** bucket-C/E already run the same exe
under Mesa llvmpipe (`build-and-test.yml:374-409,466-502`). Copy the two Mesa steps
(`Cache Mesa opengl32.dll` + `Install Mesa software OpenGL ICD`, curl
`pal1000/mesa-dist-win 24.2.5`, extract `opengl32.dll` into the perf build dir) into
both perf workflows and set step-level `GALLIUM_DRIVER=llvmpipe` +
`LIBGL_ALWAYS_SOFTWARE=1` on the scenario-run step. The `--spawn` child inherits the
step env via `CreateProcessA`. **Do not** build a true offscreen/no-GL harness — that
is a `StandaloneAppBootstrap.cpp` refactor far larger than the Mesa-parity fix.

## Staged revival *(historical SAFE-NOW / PARKED tags as written at park time — ALL steps complete as of 2026-06-07; step 7 shipped as Pattern C, not the Pattern-B skip-companion described below — see § Deviations)*

1. **[SAFE-NOW] Mesa software-GL wiring** in `perf-pr-fast.yml` + `perf-full.yml`
   (the headless fix above). Inert/non-blocking until baselines exist (WARN
   downgrades stay), so safe to merge as prep.
2. **[SAFE-NOW] Fix the scenario set:** remove `command-palette-fuzzy` from
   `scripts/dev/perf-pr-fast-set.json` — it is **bucket-C-only** (needs
   `--screenshotPath`, cannot run headless) and would become a hard `run_failure`
   `exit 1` the instant any ci baseline lands. Keep `idle`, `priority-grid-scroll`,
   `cell-edit-burst`, `ai-chat-history-render`.
3. **[PARKED — needs a live CI run] Prove the launch:** after steps 1-2, trigger
   `perf-pr-fast.yml` (a code PR) or `perf-full.yml` (`workflow_dispatch`) and
   confirm each scenario emits JSON with **no** `run_failure_count` and that the
   fresh snapshot rows include `p99Ms` (existing `.dev.json` baselines have
   `anyP99=False`, so p99 enforcement is **unverified** until this).
4. **[PARKED — GOLDEN, human-approved] Capture + commit baselines:** download the
   `perf-pr-fast-snapshots` artefact, validate against `perf-baseline-schema.json`
   (`captureHost == "ci-windows-latest"`, non-empty `rows[]`), **sanity-check the
   numbers are not already-regressed**, route through `golden-image-approval.md`
   (surface → await **explicit human approval** → only then `git add`), commit the
   4 kept-scenario files via a PR (never direct-push). Committing them flips the
   live compare branch AND re-arms the plumbing hard-fail.
   **Cross-host is forbidden** (D1): baselines MUST be captured on the runner, never
   hand-ported from `*.dev.json`.
5. **[SAFE-NOW code / PARKED calibration] Enforce the 6.94 ms mean budget:** add
   `mean_abs_ceiling_ms` to `regression-policy.json` default + `DEFAULT_POLICY` in
   `perf-compare.py:54-59` + a check in `evaluate()` beside the p99 branch
   (`perf-compare.py:209-212`). **Calibration caveat:** a blanket `6.94` will likely
   need per-scenario `perScenario` overrides for legitimately-heavy scopes (e.g.
   `SmatchetUI::Draw`) so the gate is not perpetually red on CI hardware — set the
   value/overrides only after observing real runs (step 4).
6. **Make it blocking — two rungs, escalating:**
   - **6a. [PARKED — do with/after step 4] Poller allow-list:** extend the
     `merge-gates.sh` non-required allow-list regex (`Coverage|Sanitizer|Bucket-`,
     added by #933 — comment there marks it the extension point) to include
     `Perf PR-fast`, + a `merge_gates.bats` case. Blocks watcher/poller merges on a
     red perf check **without** GitHub branch protection — no skip-companion needed
     (the poller only sees checks that actually ran). The `perf-out-of-band`
     downgrade for `Perf PR-fast*` already exists in `merge-gates.sh` as the
     override hatch. Does NOT cover a raw `gh api .../merge` — that's what 6b adds.
     Only safe once baselines exist (before that the check is a guaranteed-pass
     no-op anyway, so the rung is inert-but-harmless if merged early).
   - **6b. [PARKED — branch-protection change, escalate] GitHub-required:** add
     `Perf PR-fast (windows-2022)` to `project.config.json`
     `branch_protection.required_contexts` + `ci.required_checks`, apply via
     `setup-branch-protection.sh`. Needs the step-7 skip-companion first.
     **Confirm with the user** before wedging merge flow.
7. **[PARKED — do before/with step 6b; not needed for 6a] Skip-companion:** add
   `.github/workflows/perf-pr-fast-skip.yml` (Pattern B) emitting the exact job name
   for non-perf-path PRs, else docs-only PRs deadlock.
8. **[PARKED — closeout] Update** `build-quality-velocity-hardening.md` #8/#13
   impl-log + Verification.

## Flake mitigations (apply during step 4-5 calibration)

- **Median-of-N runs** (the single-run `consecutive_run_required` knob was removed):
  add an N=3/5 loop in `perf-pr-fast.yml` / `perf-run.sh`, feed the **median**
  snapshot to `perf-compare.py`.
- **Warmup discard:** drop the first (cold-runner) run before taking the median.
- **Tolerance band:** keep relative `mean_delta_pct:10.0`; set `mean_abs_ceiling_ms`
  with **headroom** over the captured baseline, or use `perScenario` overrides for
  heavy scopes.
- **Pin the runner + Mesa:** `windows-2022` (not `windows-latest`), Mesa
  `24.2.5` cache key in lockstep with the buckets.
- `min_baseline_calls:10` already filters noisy low-call rows — keep it.
- Confirm the sccache + FetchContent cache fix (#9/#18, commit `fd4b37e8`) is
  hitting so build cost does not bleed into measurement.

## Blockers (why this parked) *(historical — all resolved 2026-06-07: launch proven on CI, baselines human-approved + committed, first-run noise observed + floor calibrated)*

- **Cannot capture `ci-windows-latest` baselines locally** — must be produced on the
  GH `windows-2022` runner under Mesa (cross-host forbidden by D1). Needs ≥1 real CI
  run after the Mesa fix.
- **Cannot prove the headless `--spawn` launch** without running it on the runner
  (it has never reached the compare branch).
- **Cannot self-approve the golden baseline artefacts** (`golden-image-approval.md`).
- **Cannot fix the flake constants** (N, warmup, ceiling headroom, per-scenario
  overrides) without observing CI noise across several runs.
- **p99 enforcement is unverified** until a CI run confirms `scenario.run` emits
  `p99Ms`.

## Parked-state handoff *(superseded — executed in full 2026-06-06/07; kept for the record)*

**Safe to do in the un-park session, in order:** steps 1, 2, 5-code (mean ceiling,
defaulted-not-required) — all merge-safe because the WARN-on-no-baseline /
WARN-on-run-failure downgrades (`perf-pr-fast.yml:219-228`) stay IN PLACE as the
safety net that keeps the unfinished gate from false-blocking.
**Then the human gates:** step 3 (live CI run) → step 4 (capture → **human approval**
→ commit baselines) → step 5-calibration → step 6a (poller allow-list — cheap,
no branch-protection change) → step 7 (skip-companion) → step 6b (flip
required — **confirm with user**) → step 8 (closeout).

## Files in play

`.github/workflows/perf-pr-fast.yml`, `.github/workflows/perf-full.yml`,
`.github/workflows/perf-pr-fast-skip.yml` (new), `scripts/dev/perf-pr-fast-set.json`,
`scripts/dev/perf-compare.py`, `docs/perf/regression-policy.json`,
`docs/perf/baselines/{idle,priority-grid-scroll,cell-edit-burst,ai-chat-history-render}.ci-windows-latest.json`
(new, GOLDEN), `project.config.json`, `scripts/dev/perf-run.sh` (optional N-run
median / direct in-process scenario.run), `agents/scripts/core/merge-gates.sh` +
`tests/bats/merge_gates.bats` (step 6a allow-list), `build-quality-velocity-hardening.md`.

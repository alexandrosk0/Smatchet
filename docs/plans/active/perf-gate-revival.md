# Perf-gate revival (Pillar 1) — execution playbook

> **STATUS: UN-PARKED — SAFE-NOW steps shipped (2026-06-06).** Steps 1, 2 and
> 5-code landed (see § Implementation log). Remaining: step 3 (live-CI proof —
> expected on this PR's own perf-pr-fast run), step 4 (baseline capture →
> **human approval** → commit), step 5-calibration, 6a, 7, 6b, 8 — per
> § Parked-state handoff. Derived from `build-quality-velocity-hardening.md`
> item **#8/#13** via a 5-reader investigation (2026-06-06).

## Implementation log

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

## Why this exists

Quality **Pillar 1** (steady-state UI work ≤ **6.94 ms** / 144 Hz; p99 ≤ **16.67 ms** /
60 Hz floor) is supposed to be CI-enforced by `.github/workflows/perf-pr-fast.yml`
(job **`Perf PR-fast (windows-2022)`**) + the scheduled `perf-full.yml`. It is
**not.** The gate exists and parses but is a **guaranteed-pass no-op** today.

## The 5 compounding reasons the gate is a no-op

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

## Staged revival (each step tagged SAFE-NOW / PARKED)

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

## Blockers (why this parks)

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

## Parked-state handoff (what "later" picks up)

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

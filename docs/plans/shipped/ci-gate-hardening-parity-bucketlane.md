# Plan — CI gate hardening: required-context parity selftest + bucket-lane launch-smoke
<!-- plan-date: 2026-06-13 -->

> **Slug**: `ci-gate-hardening-parity-bucketlane` (matches this file's basename without `.md`).
>
> **Status**: `shipped` — all cited PRs merged (see Implementation log); archived 2026-06-16 via plan-archival sweep.
>
> **Usage**: design-only plan authored per `docs/plans/active/_plan-template.md`. Closes two related "gate, don't trust" P1 backlog items in `docs/self-improvement/categories/infra.md` (2026-06-05 required-context parity; 2026-06-07 bucket-lane-launch-smoke).

## Context

Two CI gate-escape classes have already wedged or blinded `develop`, and nothing prevents either from recurring:

1. **required-context ⇄ unconditional-workflow parity** (infra.md `:214`). `Doc anchors + agent contract` was a *required* branch-protection context (`project.config.json` § `branch_protection.required_contexts`) whose emitting workflow `doc-validation.yml` was `paths:`-filtered. A PR touching none of those paths never produced the context, so GitHub held it `BLOCKED` forever — deadlocking #880/#881/#882 (pure product/test diffs). PR #884 fixed the one workflow (runs on every PR + self-gates), but the *next* required check added with a `paths:` filter recreates the deadlock. There is no selftest asserting required-contexts emit unconditionally.

2. **bucket-lane-launch-smoke** (infra.md `:40`). Advisory (`continue-on-error: true`) exe-running lanes can't distinguish dead-harness from flaky-tests. bucket-C ran `Passed: 0  Failed: 3` / exit 1 with a **GREEN** check for 2 weeks (#441 → #937) because Mesa provisioning shipped only the thin `opengl32.dll` loader → every exe died at process boot → `continue-on-error` swallowed total harness death exactly like a flaky test. The lane's entire purpose (visual/UI regression coverage) was silently void.

Both items share a **name→workflow resolution map**: "given a check-name string, which workflow + job emits it, and what are that job's `on.pull_request` triggers." infra.md `:246` (a third, related allow-list-present-assertion item) also names this shared map. Building it once is the central design decision of this plan.

Intended outcome — after this lands: (a) adding a required context whose workflow is `paths:`-filtered fails a doc-validation selftest before merge; (b) an advisory exe-running lane whose harness is dead (exe can't boot, or `Passed==0 && Failed>0`) hard-fails instead of green-washing.

## Approach

**Shared infra first — the name→workflow resolution map.** A new helper `agents/scripts/core/lib/ci-check-resolve.sh` (sourced library, not a standalone gate) parses every `.github/workflows/*.yml` and builds, for each `jobs.<id>.name` (the rendered check-name GitHub displays) → `{workflow_file, job_id, on_triggers, paths_filter, self_gate_marker}`. It resolves a check-name string to that record, or returns "unresolvable." Implementation: `yq` if available, else a constrained Python `yaml.safe_load` over the workflow files (Python is already a hard dep of `test-agent-contract.sh`, `:25`). One library, two consumers (this plan's two gates + the future allow-list-present item). The map handles the two name-shapes GitHub uses: a job with an explicit `name:` renders that string; a job without one renders `<job_id>`. Matrix-expanded names (`name: Foo (${{ matrix.x }})`) are flagged as "templated — match by prefix" so a required context naming a concrete matrix leg still resolves.

**Gate 1 — required-context parity selftest.** New `agents/scripts/core/test-required-context-parity.sh`, auto-enrolled by `doc-validation.yml` (added to its `paths:` watch-list + a run step, mirrored in the workflow's documented enrollment block) AND run locally by the `scripts/dev/test-all.sh` `test-*.sh` glob. For each `branch_protection.required_contexts` entry in `project.config.json`: resolve it via the shared map; **fail closed** if unresolvable (an unresolvable required context is itself the deadlock risk). If resolved, assert the emitting job's `on.pull_request` has **NO** `paths:` / `paths-ignore:` filter — OR the job carries a documented self-gate marker (a magic comment `# ci-required-context: self-gated` on the job, the convention the #884 fix uses where the workflow runs unconditionally but the job internally short-circuits). A `--selftest` mode (the established convention, e.g. `test-config-globs.sh`) feeds synthetic fixtures: one clean required context, one path-filtered (must fail), one unresolvable (must fail), one self-gate-marked (must pass).

**Gate 2 — bucket-lane launch-smoke (two mechanisms).**
(a) **Workflow step** — in `build-and-test.yml`, before each advisory bucket step (`bucket-c-screenshot-diff` `:456`, `bucket-e-ui-tests` `:568`, and Plan-1's `mobile-texture-guard-smoke`), insert a **non-`continue-on-error`** "launch-smoke" step *after* the Mesa-install step: run the freshly provisioned exe once with a short, side-effect-free command (`Smatchet.exe cmd app.version --spawn --yes`, ≤10 s via `timeout`). If the exe cannot start (the exact `opengl32.dll`-thin-loader death), this step hard-fails the job — making "harness is dead" a loud failure instead of a swallowed one. Because the step is NOT `continue-on-error`, a boot failure turns the advisory job RED (the job stays advisory for *test* outcomes, but a dead harness is not a test outcome). The launch-smoke step shape is shared across all three lanes — extract once as a reusable composite-action or a documented snippet.
(b) **Driver hard-exit** — inside `scripts/dev/test-screenshot-diff.sh` (`:198` prints `Passed: $PASSED Failed: $FAILED`) and the bucket-E scenario driver path, add: a lane that finishes `Passed==0 && Failed>0` is **broken, not flaky** → hard-exit nonzero in a way the `continue-on-error` cannot mask. Since `continue-on-error` masks the job result regardless, the real teeth are: emit a distinct `::error::` annotation + a sentinel the workflow's *next* non-advisory step asserts on (e.g. write a `lane-status` file; a tiny non-advisory "lane-integrity" step fails if `Passed==0 && Failed>0`). This is the bash-side complement that makes "passed nothing" indistinguishable-from-flaky impossible.

**Why fail-closed on unresolvable.** The whole class is "a check that silently never reports." A parity selftest that *skips* names it can't resolve would reintroduce the exact blind spot. Unresolvable ⇒ fail, forcing the author to either fix the name or document the self-gate.

## Files to modify

1. `agents/scripts/core/lib/ci-check-resolve.sh` **(new)** *(grep first: `rg -l 'ci-check-resolve|check.?resolve' agents/scripts/`)* — shared name→workflow resolution library; sourced by both gates.
2. `agents/scripts/core/test-required-context-parity.sh` **(new)** — Gate 1 selftest; `--selftest` + default check modes; fail-closed on unresolvable; reads `project.config.json` required_contexts.
3. `.github/workflows/doc-validation.yml` — enroll Gate 1: add `agents/scripts/core/test-required-context-parity.sh` (+ the new lib) to the `paths:` watch-list (`:33`-`:48` block), add a run step (mirror the `test-config-globs.sh` two-line `--selftest` + check shape at `:298`), and update the workflow's header enrollment comment (`:9`-`:48`).
4. `.github/workflows/build-and-test.yml` — Gate 2(a): insert a non-`continue-on-error` launch-smoke step into `bucket-c-screenshot-diff` (after Mesa install `:455`), `bucket-e-ui-tests` (after Mesa install `:567`); shared snippet. Gate 2(b): add a non-advisory "lane-integrity" assertion step that reads the driver's sentinel.
5. `scripts/dev/test-screenshot-diff.sh:198` — after the `Passed: … Failed: …` line, write a lane-status sentinel + `::error::` annotation + nonzero exit when `Passed==0 && Failed>0` (distinct exit code from ordinary test-fail so the workflow can tell "broken" from "some tests failed").
6. The bucket-E scenario driver (`Smatchet.exe scenario.run --name=ui-test`) — the same `Passed==0 && Failed>0` ⇒ hard-exit guard applied inside the scenario-engine summary path so a zero-pass run is non-flaky-fatal. *(grep first for the scenario-summary print: `rg -n 'Passed|scenario.*summary' Source/Core/src/`)*
7. `tests/bats/required_context_parity.bats` **(new)** — bats for Gate 1: fixtures for clean / path-filtered / unresolvable / self-gate-marked required contexts; assert pass/fail per case. Follows the `merge_gates.bats` stub-on-PATH + fixtures pattern.
8. `tests/bats/bucket_lane_launch_smoke.bats` **(new)** — bats for Gate 2: feed `test-screenshot-diff.sh` a `Passed==0 Failed>0` summary (stubbed scenario runner) and assert the broken-lane exit code + sentinel; assert a `Passed>0` run does NOT trip it.
9. `tests/fixtures/ci_parity_*.{json,yml}` **(new)** — synthetic `project.config.json` slice + synthetic workflow files for the parity `--selftest` and bats.
10. `agents/scripts/core/test-gate-selftests.sh` — register the new gate's `--selftest` in the meta-selftest enrollment if that script enumerates gates (confirm by reading it; the contract-card pattern in AGENTS.md asserts gate tokens).

## Existing utilities reused

- `--selftest` + check two-mode convention — `agents/scripts/core/test-config-globs.sh` (run pattern at `doc-validation.yml:298`) — copy the mode dispatch.
- Doc-validation auto-enrollment — `.github/workflows/doc-validation.yml:33`-`:48` watch-list + `:250`+ run steps — add the new gate the same way.
- bats stub-on-PATH + jq fixtures harness — `tests/bats/merge_gates.bats` setup block — reuse for both new bats files.
- Mesa-install cached-DLL block — `build-and-test.yml` bucket-C `:422` / bucket-E `:535` — the launch-smoke step is inserted immediately after it.
- `Passed: $PASSED  Failed: $FAILED` summary — `scripts/dev/test-screenshot-diff.sh:198` — the hook point for the broken-lane guard.
- Python `yaml.safe_load` (or `yq`) — Python is a hard dep already (`test-agent-contract.sh:25`).

## UX Pillar callouts

- **Pillar 1 (perf)**: no impact — CI-only gates, no product code on a perf path. (Gate 2(b) edits a CLI/driver summary path, not a steady-state render path.)
- **Pillar 2 (UI-thread)**: no impact — no UI code.
- **Pillar 3 (never crash)**: indirect positive — Gate 2 ensures the bucket-C/E *crash-coverage* lanes actually run (a dead harness no longer green-washes), restoring the Pillar-3 sanitizer/UI regression coverage they exist for.
- **Pillar 4 (accessibility)**: no impact.

## Perf-review-system gates

**N/A — diff does not touch `Source/Core/` product render/data paths in a perf-relevant way.** File #6 touches the scenario-engine *summary* print (a once-per-run CLI path, not a per-frame path); files #1-5,7-10 are shell/yaml/bats/fixtures. No `SMATCHET_UI_PERF_SCOPE`, no `MainThreadDispatcher::Drain()`, no new `ImGui::*`-reachable sync I/O. Justified N/A.

## Risks / non-goals

- **Risk: YAML parsing fragility** (matrix-expanded names, reusable/composite workflows, `name:` with `${{ }}` expressions). Mitigation: the resolution map flags templated names as prefix-matchable and fails-closed only on genuine non-resolution; bats fixtures cover the templated case.
- **Risk: self-gate marker becomes a rubber stamp** (authors slap the marker on to bypass parity). Mitigation: the marker is a documented convention requiring the job to internally short-circuit (the #884 shape); a follow-up could assert the marked job actually contains a skip-guard, but that is out of scope here.
- **Risk: `continue-on-error` still masks the driver hard-exit.** This is *why* Gate 2 uses a SEPARATE non-advisory step/sentinel rather than relying on the advisory job's own exit — the design explicitly routes the "broken" signal around `continue-on-error`.
- **Risk: launch-smoke exe-command choice has side effects.** Mitigation: use a read-only `app.version`-class command with `--spawn --yes` + a `timeout` wrapper; no network, no file mutation.
- **Non-goal — promoting the advisory lanes to required.** infra.md `:246` notes that's an alternative; this plan hardens them while keeping advisory status. No-action here.
- **Non-goal — the allow-list-present-assertion item** (infra.md `:246`). It reuses the shared map this plan builds; named as a follow-up so the map is built once.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: N/A — these are shell/yaml gates, not C++ pure logic. (File #6's scenario-summary guard, if it touches a pure helper, gets a bucket-A case in `tests/Core/`.)
- **Bucket E**: N/A — no ImGui Test Engine surface.
- **Bash-driver / bats**: `tests/bats/required_context_parity.bats` (clean passes; path-filtered, unresolvable fail; self-gate-marked passes) + `tests/bats/bucket_lane_launch_smoke.bats` (`Passed==0 Failed>0` trips broken-lane exit; `Passed>0` does not). Both run via `bats tests/bats/`.
- **Selftest**: `bash agents/scripts/core/test-required-context-parity.sh --selftest` green; the default-mode check passes against the LIVE `project.config.json` + current workflows (a precondition — if it fails on landing, a real parity violation already exists and must be fixed first).
- **Negative-test fixture (build-log-regex lesson)**: each bats includes a deliberately-broken input asserting exit≠0, so a false-pass regression is caught at authoring time.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` — only if file #6 touches compiled C++; otherwise the build is unaffected (shell/yaml/bats only) and this reduces to the doc/lint gates.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: `scripts/dev/test-docs.sh` green, including the newly enrolled `test-required-context-parity.sh` step.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test against the merge-gates + CI domain model and the #880/#937 RCAs before finalising; record the outcome. Required for every plan.
- **Manual residue**: none expected — both gates are fully automated. If the YAML resolver can't cover a real workflow shape, name the deferred-automation plan + a `docs/self-improvement/categories/infra.md` entry. No silent residue.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to the deferred items below presented as current, and revise.

- **Allow-list-present-assertion** (infra.md `:246`) — reuses this plan's shared resolution map; separate follow-up so the map ships first.
- **Marker-enforcement** (assert a self-gate-marked job actually contains a skip-guard) — follow-up hardening; out of scope.
- **Promoting bucket-C/E to required GitHub contexts** — out of scope; this plan keeps them advisory-but-honest.

## Implementation log
- `393bb748` · #1180 — required-context parity selftest + bucket-lane-launch-smoke (infra P1): 22 files incl. `agents/scripts/core/lib/ci-check-resolve.sh`, `test-required-context-parity.sh` (`--selftest`/`--check`), `doc-validation.yml` enrollment, launch-smoke steps in `build-and-test.yml`, broken-lane guard in `scripts/dev/test-screenshot-diff.sh`, bats fixtures.
- `458facdc` · #1187 — launch-smoke advisory tuning (kept the shipped launch-smoke advisory/WARN-first).
- `67098d12` · #1183 — mobile texture-guard dependency follow-up.

## Deviations from plan
- The 3 explicitly out-of-scope items remain deferred by design (not shipped): allow-list-present assertion (infra.md), self-gate marker-enforcement, and promoting bucket-C/E to required GitHub contexts. All stay as follow-ups; the shared resolution map shipped so the allow-list item can build on it later.
- #1187 only tuned the shipped launch-smoke to advisory/WARN-first; the broken-lane hard-fail teeth (sentinel + non-advisory lane-integrity assertion) remained, but the launch-smoke step itself was kept advisory rather than gating.

## Verification (actual)
- Archival audit (2026-06-16) confirmed present in tree: `agents/scripts/core/lib/ci-check-resolve.sh`, `agents/scripts/core/test-required-context-parity.sh` (with `--selftest`/`--check` modes), `doc-validation.yml` enrollment of the new gate, the launch-smoke steps in `build-and-test.yml`, and the broken-lane guard in `scripts/dev/test-screenshot-diff.sh`, plus the bats fixtures.
- Both infra.md P1 backlog entries (required-context parity; bucket-lane-launch-smoke) flipped to applied.
- Gate selftests / bats / CI runs verified present in tree (archival audit 2026-06-16), not re-run.

# Plan — Promote Coverage + ASan + UBSan to branch-protection required contexts (testing-surface Slice C)

**Status:** plan — revised after reviewer decisions (scope: +per-PR UBSan; flip: agent runs it; staging: continuous). Phase 1 (safe, non-wedging) implementation may proceed; Phase 2 (live-ruleset flip) is gated on develop-green + reported before/after.
**Branch:** `ci/coverage-sanitizer-required-contexts` · worktree `C:\Dev\trees\coverage-sanitizer-required`
**Parent:** [`testing-surface-roadmap.md`](testing-surface-roadmap.md) Slice **C** (§6 P0). First of the two gating-policy slices (C → B).

## Reviewer decisions (this revision)

1. **Scope = also add per-PR UBSan.** Beyond promoting the two existing checks, stand up a **new** per-PR Clang ASan+UBSan check (UBSan does not exist per-PR today — it runs only in nightly cron).
2. **Phase-2 ruleset flip = the agent runs it.** Confirmed `gh api repos/alexandrosk0/Smatchet --jq .permissions` → `admin: true`, so this session can run `setup-branch-protection.sh` itself; it reports the 6→9 result before/after.
3. **Staging = continuous.** Once Phase 1 merges and develop is confirmed green on all three contexts, proceed straight into Phase 2 (no pause), per decision 2's owner.

## Why this is non-trivial (gating policy — affects every session)

A branch-protection **required context** is enforced by GitHub itself on every PR into `develop`. Get it wrong and **all** concurrent sessions wedge: a required check that never reports, or that is red on `develop` HEAD, makes every PR unmergeable. So the rollout is staged + precondition-gated.

## Recon — what the roadmap assumed vs. the actual tree

The roadmap scoped Slice C as "refactor `coverage.yml` + the ASan/UBSan jobs to Pattern A so they always report and can join `required_contexts`." **The workflow refactor is already done for the two existing checks**; the real gaps are a **config↔live drift**, a **missing per-PR UBSan check**, and **missing escape hatches**. Findings:

1. **`coverage.yml` is already Pattern A and already in config.** No `on.paths` filter; single `windows-coverage` job self-gates via a `Detect coverage-relevant changes` step; `merge_group:` trigger; `continue-on-error: false --threshold 65` (BLOCKING). `project.config.json § branch_protection.required_contexts` **lists** `Coverage (windows-2022 + OpenCppCoverage)`.

2. **…but the LIVE ruleset never got Coverage.** `gh api .../branches/develop/protection/required_status_checks` returns **6** contexts — `Coverage (…)` absent. The apply step (`setup-branch-protection.sh`, a manual-only full-replace PUT that runs in **no** workflow) was never re-run after Coverage was added to config. **This drift is the root cause of the #1227 gate-escape** (a red Coverage merged because GitHub never required it; the merge-gates poller's allow-list only catches it on the poller path, which a direct `gh pr merge`/admin merge bypasses).

3. **`Sanitizer (ASAN via MSVC)` is in neither config nor live** — yet already shaped for promotion. It is the `sanitizer-asan` job in `build-and-test.yml` (job-level `if: needs.changes.outputs.source_core_cpp == 'true'`, **not** a workflow `paths:` filter → reports `skipped`=success on irrelevant PRs); the `changes` detect step **forces `source_core_cpp=true` on `merge_group`**. Textbook **Pattern C**. No workflow restructure needed. The 5 escaped red-Sanitizer PRs (#1232/#1233/#1229/#1220 + class) merged via the poller-bypass path.

4. **Per-PR UBSan does not exist.** UBSan is Clang-only (`/fsanitize=undefined`; MSVC ASan is ASan-only). The only UBSan surface today is `sanitizer-nightly.yml` (cron `Sanitizer nightly (Clang ASan+UBSan)`, preset `ninja-clang-asan`). `SMATCHET_SANITIZER` accepts `asan/tsan/msan` only — on **Clang**, `asan` bundles `-fsanitize=undefined` (preset displayName: "ASAN + UBSan via Clang. Full sanitizer suite including UBSan"). So a per-PR UBSan check reuses `ninja-clang-asan` (ASan+UBSan combined) — **no CMake change**.

5. **The nightly is 6/6 green (2026-06-09 → 06-14).** The Clang ASan+UBSan surface on develop is clean → a per-PR check on the same preset will be green on develop HEAD → **no pre-existing-UB backlog to fix first, no wedge risk on promotion.**

6. **No name collision.** Nightly job = `Sanitizer nightly (Clang ASan+UBSan)`; the new per-PR check = `Sanitizer (Clang ASan+UBSan)`; the MSVC one = `Sanitizer (ASAN via MSVC)`. All distinct.

**Net:** Slice C = (a) reconcile config→live so Coverage is enforced; (b) add `Sanitizer (ASAN via MSVC)` to config + live with a `sanitizer-out-of-band` escape; (c) add a **new** per-PR `Sanitizer (Clang ASan+UBSan)` job (reuse the proven-green nightly preset, Core-delta gated, Pattern C shape) + config + live + a `ubsan-out-of-band` escape.

## Goal

Three contexts become **live** branch-protection required checks on `develop` — `Coverage (windows-2022 + OpenCppCoverage)`, `Sanitizer (ASAN via MSVC)`, `Sanitizer (Clang ASan+UBSan)` — each with an in-workflow `*-out-of-band` label escape, promoted only against a green `develop` HEAD, with no merge-queue/path-filter deadlock. Live ruleset 6 → 9.

## Design decisions (review these)

1. **Per-PR UBSan reuses `ninja-clang-asan` (ASan+UBSan together) — RECOMMENDED.** Zero new preset, zero CMake change, identical to the proven-green nightly, and a bonus second ASan compiler (Clang catches what MSVC ASan misses). Cost: a second ~45–60 min windows-2022 sanitizer build per **Core-touching** PR (same `source_core_cpp` delta-gate as the MSVC ASan job — docs/Standalone/yaml PRs skip it; nightly still covers them). *Reviewer veto-point (β):* a dedicated UBSan-only preset (add a `ubsan` value to `cmake/Sanitizers.cmake` → `-fsanitize=undefined` without ASan) would build faster and give a pure-UBSan signal, but needs a new CMake surface + fresh validation and drops the second-compiler ASan coverage. Say if you prefer β; the plan implements α unless vetoed.

2. **Escape-hatch mechanism = conditional `continue-on-error` + outcome-warning** (not coverage's bash-threshold branch — ASan/UBSan failures are crashes, not threshold checks). A `Resolve <name>-out-of-band` step reads `toJson(github.event.pull_request.labels)` → `oob=0/1`; the test steps set `continue-on-error: ${{ steps.oob.outputs.oob == '1' }}` (false on push/merge_group where labels are empty → normal blocking), and a follow-on `if: steps.<id>.outcome == 'failure'` step emits `::warning::`. The label is a merge-time valve for a *known* flake while the recovery follow-up is queued; it must come off post-merge (same discipline as `tests-out-of-band`).

3. **Two-phase rollout, ruleset flip last** (see § Rollout). Phase 1 (PR): the new Clang ASan+UBSan job + both escape hatches + the `project.config.json` required_contexts edits + the two new labels + doc fixes — all safe, no live-ruleset change, nothing enforced. Phase 2 (after Phase 1 merges **and** all three checks are green on `develop` HEAD): run `setup-branch-protection.sh` to flip the live ruleset 6→9.

4. **Promotion precondition: all three checks green on `develop` HEAD.** Phase 2 is gated on a fresh `gh api .../commits/<develop-sha>/check-runs` showing all three SUCCESS (or legitimately skipped). The new Clang job's first develop run (post-Phase-1-merge) establishes its baseline; the nightly's 6/6 green strongly predicts green. If any is red on develop, fix that first.

5. **No change to `merge-gates.sh`.** It already lists `Coverage|Sanitizer` in `MERGE_GATES_BLOCK_ALLOWLIST_RE` (the `Sanitizer` substring matches both the MSVC and Clang check names). Post-promotion the poller and GitHub agree.

## Files to modify

| File | Change | Phase |
|---|---|---|
| `.github/workflows/build-and-test.yml` | (a) add a new `sanitizer-clang-asan-ubsan` job, name `Sanitizer (Clang ASan+UBSan)` — mirror the `sanitizer-asan` job (preset `ninja-clang-asan`, LLVM-on-PATH step, `needs: [changes, windows-msvc]`, `if: source_core_cpp == 'true'`, Core-delta gate, `ubsan-out-of-band` escape); (b) add the `sanitizer-out-of-band` escape to the existing `sanitizer-asan` job | 1 |
| `project.config.json` | add `"Sanitizer (ASAN via MSVC)"` + `"Sanitizer (Clang ASan+UBSan)"` to `branch_protection.required_contexts` (Coverage already present → 9 total) | 1 |
| *(repo labels)* | `gh label create sanitizer-out-of-band` + `gh label create ubsan-out-of-band` (the four other `*-out-of-band` labels already exist) | 1 |
| `docs/guides/testing-surface.md` | §3 Coverage row "blocks its own job" → "blocks + now a required context"; add Sanitizer (MSVC) + Sanitizer (Clang ASan+UBSan) rows; note the config↔live re-apply requirement | 1 |
| `docs/agent-rules/ci-required-check-pattern.md` | add a "config↔live drift" note: editing `required_contexts` is inert until `setup-branch-protection.sh` is re-run; cite #1227 | 1 |
| *(live GitHub ruleset)* | `bash agents/scripts/core/setup-branch-protection.sh` — flips live `develop` protection 6 → 9 (admin token; not a file edit) | 2 |

## Rollout

**Phase 1 — PR (safe; nothing enforced).**
1. Add the new `Sanitizer (Clang ASan+UBSan)` job + the two escape hatches to `build-and-test.yml`.
2. Add the two Sanitizer contexts to `project.config.json`.
3. Create the two new labels.
4. Doc fixes.
5. Ship-loop → PR → gates → merge. (This PR does not enforce the new contexts; `setup-branch-protection.sh` is not run here. The new Clang job runs on the PR itself, proving it green before it ever becomes required.)

**Phase 2 — live ruleset flip (continuous, after Phase 1 merges).**
6. Confirm `develop` HEAD green on all three contexts (`gh api .../commits/<develop-sha>/check-runs`).
7. Run `bash agents/scripts/core/setup-branch-protection.sh` (this session has admin). Verify `gh api .../protection/required_status_checks` shows 9 contexts; report before/after.
8. Smoke: a docs-only PR → all three report (Coverage no-ops green; both sanitizers `skipped`=success) and the PR is mergeable. A Core-touching PR → all three run.

## Flake-budget / escape hatches

- **Coverage:** `coverage-out-of-band` (exists) → below-threshold downgraded to WARN/green.
- **Sanitizer (ASAN via MSVC):** `sanitizer-out-of-band` (new) → non-clean ASan run downgraded.
- **Sanitizer (Clang ASan+UBSan):** `ubsan-out-of-band` (new) → non-clean ASan/UBSan run downgraded.
- Each label is a merge-time valve for a *known* flake; none is a standing waiver (label off post-merge).

## Risks

- **Wedge-on-red:** promoting a check red on `develop` HEAD wedges all PRs. Mitigated by the Phase-2 green precondition (decision 4); the new Clang job is de-risked by the 6/6-green nightly on the same preset.
- **New-check first-run UB:** a brand-new UBSan surface can flag pre-existing UB. Mitigated: the identical nightly preset has been green 6 consecutive days, so the per-PR check inherits a clean baseline. If its first develop run *does* red, Phase 2 holds until the UB is fixed (do not promote a red check).
- **Per-PR cost:** +1 sanitizer build (~45–60 min) per Core-touching PR. Accepted cost of decision 1; Core-delta gated so non-Core PRs skip it.
- **Merge-queue ref:** all three already trigger on `merge_group` (coverage.yml `merge_group:`; build-and-test.yml detect-step forces `source_core_cpp=true`) — no queue deadlock.
- **Skipped-required semantics:** Pattern C relies on GitHub treating an `if:`-skipped required job as success — the behaviour the existing MSVC required contexts already depend on; the Phase-2 docs-only smoke test verifies it for both sanitizers.

## Discharges owed postmortems

The SessionStart banner shows 5 owed gate-escape postmortems whose mandatory `### Preventing gate` is precisely this slice: #1227 (red Coverage) and #1232/#1233/#1229/#1220 (red Sanitizer) all escaped because these checks were not GitHub-required. Slice C is the systemic gate. Cite the shipped slice as the discharging fix in `postmortems.md`.

## Verification

- **Phase 1:** `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` (YAML/JSON/docs — no C++), `scripts/dev/test-docs.sh` (doc anchors), and the PR's own CI re-runs `build-and-test.yml` — the new `Sanitizer (Clang ASan+UBSan)` job runs on the PR (Core-delta permitting; if the PR is docs/yaml-only it will skip, so include a touch that flips `source_core_cpp` or trigger via `workflow_dispatch`/a temporary probe to prove the new job green before relying on it). Validate the escape branch on a labeled smoke PR.
- **Phase 2:** `gh api .../protection/required_status_checks` shows 9 contexts; docs-only smoke PR mergeable; Core-touching smoke PR runs all three.

## Perf-gate section

N/A — diff is CI YAML + JSON config + docs only. No `Source/Core/` code, no runtime path, no per-frame work.

## Open question for the reviewer

**Decision 1 α vs β** is the only remaining fork — implement per-PR UBSan by reusing `ninja-clang-asan` (α, recommended: proven-green, no CMake change, +1 sanitizer build/PR) or by adding a dedicated UBSan-only preset (β: cheaper build, pure-UBSan signal, new CMake surface). The plan implements **α** unless vetoed.

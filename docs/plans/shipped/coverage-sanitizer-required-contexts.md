# Plan — Promote Coverage + ASAN + UBSan to branch-protection required contexts (testing-surface Slice C)
<!-- plan-date: 2026-06-15 -->

**Status**: `shipped` — all cited PRs merged (see Implementation log); archived 2026-06-16 via plan-archival sweep.
**Branch:** `ci/coverage-sanitizer-required-contexts` · worktree `C:\Dev\trees\coverage-sanitizer-required`
**Parent:** [`testing-surface-roadmap.md`](testing-surface-roadmap.md) Slice **C** (§6 P0). First of the two gating-policy slices (C → B).

## Reviewer decisions (locked)

1. **Scope = also cover per-PR UBSan.** Confirmed by recon: a per-PR UBSan check already exists (`Sanitizer (UBSan via Clang)`, the `sanitizer-ubsan-pr` job, preset `ninja-clang-asan` = Clang ASan+UBSan). The earlier "stand up a NEW job" premise was **wrong** — the job is already there and reliably green. Slice C *promotes* it (no new job).
2. **Phase-2 ruleset flip = the agent runs it.** Confirmed `gh api repos/alexandrosk0/Smatchet --jq .permissions` → `admin: true`, so this session runs `setup-branch-protection.sh` itself; it reports the 6→9 result before/after.
3. **Staging = continuous.** Once Phase 1 merges and develop is confirmed green on all three contexts, proceed straight into Phase 2 (no pause).
4. **(α/β fork) per-PR UBSan reuses `ninja-clang-asan`** (α) — already the case; no new preset, no CMake change. β (a UBSan-only preset) is moot.

## What deep recon overturned (twice)

The roadmap scoped Slice C as "refactor `coverage.yml` + the ASan/UBSan jobs to Pattern A so they always report and can join `required_contexts`." Recon found the **workflow refactor is already done for all three checks** — the real gaps are different:

1. **`coverage.yml` is already Pattern A + already in config.** No `on.paths` filter; the single `windows-coverage` job self-gates via a `Detect coverage-relevant changes` step; `merge_group:` trigger; `--threshold 65` BLOCKING. `project.config.json § branch_protection.required_contexts` lists `Coverage (windows-2022 + OpenCppCoverage)`.

2. **…but the LIVE ruleset never got Coverage.** `gh api .../branches/develop/protection/required_status_checks` returns **6** contexts — `Coverage (…)` absent. The apply step (`setup-branch-protection.sh`, a manual-only full-replace PUT that runs in **no** workflow) was never re-run after Coverage was added to config. **This config↔live drift is the root cause of the #1227 gate-escape** (a red Coverage merged because GitHub never required it; the merge-gates poller's allow-list only catches it on the poller path, which a direct `gh pr merge`/admin merge bypasses).

3. **`Sanitizer (ASAN via MSVC)` exists and is correctly Pattern-C shaped — but DETERMINISTICALLY REDS every Core PR.** The `sanitizer-asan` job (`build-and-test.yml`, job-level `if: source_core_cpp == 'true'` — not a workflow `paths:` filter → reports `skipped`=success on irrelevant PRs; `changes` forces `source_core_cpp=true` on `merge_group`) has a `Run ctest under ASAN` step that runs `ctest --output-on-failure` with **no exclude**. That runs the `smatchet_tests` ctest entry (the full doctest rig, ~15k assertions) under **Debug** ASan, where the adversarial `CallstackParser::ParseCallstackText survives adversarial inputs` case **stack-overflows** (deep recursion × ASan fat frames). The very next step, `Run sanitized doctest rig`, runs the *same exe* with `--test-case-exclude="CallstackParser::ParseCallstackText survives adversarial inputs"` and is the genuine instrumented surface. So the ctest step's full-rig run is **redundant with** the rig step **and** is the deterministic-red. **This is the root cause of the owed ASAN gate-escape postmortems** (#1240/#1235/#1230/#1233/#1229/#1220 — identical `stack-overflow … __msvc_string_view.hpp _Char_traits::move` in `Test #1: smatchet_tests`). The team already classified this exact case as an ASan-instrumentation artifact (not a memory bug; `categories/{test,infra}.md`).

4. **`Sanitizer (UBSan via Clang)` exists and is reliably green.** The `sanitizer-ubsan-pr` job (preset `ninja-clang-asan` = Clang `-fsanitize=address,undefined`) runs `ctest --output-on-failure` and passes — because `ninja-clang-asan` is **RelWithDebInfo** (optimized frames don't overflow on the adversarial recursion the Debug MSVC build does). Same `source_core_cpp` Core-delta gate. Its comment currently says "This job is NOT a required branch-protection check" — Slice C makes it one.

5. **Neither sanitizer is in config or live; both lack an escape hatch.** Once a check is GitHub-*required*, a merge-gates label can no longer waive a red run — the only escape is the check itself reporting GREEN. So each promoted check needs an **in-workflow** `*-out-of-band` label escape before promotion, or a known flake wedges every Core PR with no valve.

**Net Slice C =** (a) **fix** the ASAN `Run ctest under ASAN` deterministic-red (`-E '^smatchet_tests$'` — the rig step is its instrumented replacement); (b) add a `sanitizer-out-of-band` escape to `sanitizer-asan` and a `ubsan-out-of-band` escape to `sanitizer-ubsan-pr`; (c) add `Sanitizer (ASAN via MSVC)` + `Sanitizer (UBSan via Clang)` to `branch_protection.required_contexts` (Coverage already present → 9); (d) doc fixes; (e) create the two labels; (f) **Phase 2** — run `setup-branch-protection.sh` to flip live 6→9.

## Goal

Three contexts become **live** branch-protection required checks on `develop` — `Coverage (windows-2022 + OpenCppCoverage)`, `Sanitizer (ASAN via MSVC)`, `Sanitizer (UBSan via Clang)` — each with an in-workflow `*-out-of-band` escape, promoted only against a green `develop` HEAD, with no merge-queue/path-filter deadlock. Live ruleset 6 → 9.

## Design decisions (review these)

1. **ASAN ctest fix = `ctest --output-on-failure -E '^smatchet_tests$'` on the `Run ctest under ASAN` step (ASan-MSVC job only).** The `smatchet_tests` entry is the full doctest rig; the *next* step (`Run sanitized doctest rig`) already runs that exe with the adversarial case excluded and IS the load-bearing instrumented surface. Dropping `smatchet_tests` from the ctest step removes the redundant overflow-prone full run while keeping the 5 `android_openssl_failfast_*` ctest probes (verified registered unconditionally → ctest still finds tests, no "No tests were found" error). The adversarial case keeps running under the *normal* (non-ASan) ctest where it passes legitimately — this exclude is ASan-scoped only (the workflow step, not the `add_test` registration). *Why not exclude it at the rig step instead:* the rig step already excludes it; the bug is the **separate** unconstrained ctest entry.
2. **Do NOT touch the UBSan job's ctest.** It is green as-is (RelWithDebInfo). Only add its escape hatch + flip its "NOT required" comment. (A symmetry refactor — split its ctest into `-E smatchet_tests` + an excluded rig step like the MSVC job — is a possible future hardening, deferred; changing a green required-to-be job adds risk for no current gain. Noted in § Deviations.)
3. **Escape-hatch mechanism = a `Resolve <name>-out-of-band` step → conditional `continue-on-error` + outcome-warning** (not coverage's bash-threshold branch — ASan/UBSan failures are crashes, not threshold checks). The resolve step reads `toJson(github.event.pull_request.labels)` → `oob=0/1`; the sanitizer test step(s) set `continue-on-error: ${{ steps.oob.outputs.oob == '1' }}` (false on push/merge_group where labels are empty → normal blocking), and a follow-on `if: steps.<id>.outcome == 'failure' && steps.oob.outputs.oob == '1'` step emits `::warning::`. The label is a merge-time valve for a *known* flake while the recovery follow-up is queued; it must come off post-merge (same discipline as `tests-out-of-band`).
4. **Two-phase rollout, ruleset flip last** (see § Rollout). Phase 1 (PR): ASAN ctest fix + both escape hatches + `project.config.json` required_contexts edits + the two new labels + doc fixes — all safe, no live-ruleset change, nothing enforced. Phase 2 (after Phase 1 merges **and** all three checks are green on `develop` HEAD): run `setup-branch-protection.sh` to flip live 6→9.
5. **Promotion precondition: all three checks green on `develop` HEAD.** Phase 2 is gated on a fresh `gh api .../commits/<develop-sha>/check-runs` showing all three SUCCESS (or legitimately skipped). The ASAN fix makes the MSVC check green on Core PRs; the UBSan check is already green; Coverage is already green. If any is red on develop, fix that first.
6. **No change to `merge-gates.sh`.** It already lists `Coverage|Sanitizer` in `MERGE_GATES_BLOCK_ALLOWLIST_RE` (the `Sanitizer` substring matches both the MSVC and Clang check names). Post-promotion the poller and GitHub agree.

## Files to modify

| File | Change | Phase |
|---|---|---|
| `.github/workflows/build-and-test.yml` | (a) `sanitizer-asan` job — `Run ctest under ASAN`: add `-E '^smatchet_tests$'` + comment (deterministic-red fix); (b) add a `Resolve sanitizer-out-of-band label` step + `continue-on-error` on the two ASAN test steps + a downgrade-warning step; (c) `sanitizer-ubsan-pr` job — add a `Resolve ubsan-out-of-band label` step + `continue-on-error` on its ctest step + a downgrade-warning step, and flip the "NOT a required branch-protection check" comment | 1 |
| `project.config.json` | add `"Sanitizer (ASAN via MSVC)"` + `"Sanitizer (UBSan via Clang)"` to **both** `branch_protection.required_contexts` (authoritative) **and** `ci.required_checks` (kept in sync) — byte-exact names so the parity gate resolves them (Coverage already present → 9 total) | 1 |
| *(repo labels)* | `gh label create sanitizer-out-of-band` + `gh label create ubsan-out-of-band` (the four other `*-out-of-band` labels already exist) | 1 |
| `docs/guides/testing-surface.md` | §3: Coverage row "not in required_contexts" → "now a required context"; Sanitizer row "not branch-required" → "required (ASAN via MSVC + UBSan via Clang)"; the "only 6 required contexts" line → 9; add the config↔live re-apply note | 1 |
| `docs/agent-rules/ci-required-check-pattern.md` | add a "config↔live drift" note: editing `required_contexts` is inert until `setup-branch-protection.sh` is re-run; cite #1227 | 1 |
| *(live GitHub ruleset)* | `bash agents/scripts/core/setup-branch-protection.sh` — flips live `develop` protection 6 → 9 (admin token; not a file edit) | 2 |

## Rollout

**Phase 1 — PR (safe; nothing enforced).**
1. Fix the ASAN `Run ctest under ASAN` step (`-E '^smatchet_tests$'`).
2. Add both escape hatches (`sanitizer-asan` + `sanitizer-ubsan-pr`).
3. Add the two Sanitizer contexts to `project.config.json` (both arrays).
4. Create the two labels.
5. Doc fixes.
6. Ship-loop → PR → gates → merge. (This PR does not enforce the new contexts; `setup-branch-protection.sh` is not run here. **Validation caveat:** a docs/yaml-only diff sets `source_core_cpp=false` → both sanitizer jobs SKIP, so the ASAN-fix-green can't be proven by this PR's own CI. Prove it by including a no-op Core touch on the branch OR a `workflow_dispatch`/temporary probe that flips `source_core_cpp`, so the ASAN job actually runs and shows green BEFORE Phase 2 relies on it.)

**Phase 2 — live ruleset flip (continuous, after Phase 1 merges).**
7. Confirm `develop` HEAD green on all three contexts (`gh api .../commits/<develop-sha>/check-runs`).
8. Run `bash agents/scripts/core/setup-branch-protection.sh` (this session has admin). Verify `gh api .../protection/required_status_checks` shows 9 contexts; report before/after.
9. Smoke: a docs-only PR → all three report (Coverage no-ops green; both sanitizers `skipped`=success) and the PR is mergeable. A Core-touching PR → all three run.

## Flake-budget / escape hatches

- **Coverage:** `coverage-out-of-band` (exists) → below-threshold downgraded to WARN/green.
- **Sanitizer (ASAN via MSVC):** `sanitizer-out-of-band` (new) → non-clean ASan run downgraded.
- **Sanitizer (UBSan via Clang):** `ubsan-out-of-band` (new) → non-clean ASan/UBSan run downgraded.
- Each label is a merge-time valve for a *known* flake; none is a standing waiver (label off post-merge).

## Risks

- **Wedge-on-red:** promoting a check red on `develop` HEAD wedges all PRs. Mitigated by the ASAN ctest fix (removes the deterministic red) + the Phase-2 green precondition (decision 5).
- **`-E '^smatchet_tests$'` strands the ctest step with no tests:** mitigated — the 5 `android_openssl_failfast_*` probes are registered unconditionally (verified) and remain, so ctest finds tests.
- **Per-PR cost:** the two sanitizer builds (~45–60 min each) already run on Core PRs today; Slice C makes them *required*, not *new*, so no added wall-clock — only added gating.
- **Merge-queue ref:** all three already trigger on `merge_group` (coverage.yml `merge_group:`; build-and-test.yml detect-step forces `source_core_cpp=true`) — no queue deadlock.
- **Skipped-required semantics:** Pattern C relies on GitHub treating an `if:`-skipped required job as success — the behaviour the existing MSVC required contexts already depend on; the Phase-2 docs-only smoke test verifies it for both sanitizers.

## Discharges owed postmortems

The SessionStart banner shows owed gate-escape postmortems whose mandatory `### Preventing gate` is precisely this slice: #1227 (red Coverage) and #1240/#1235/#1230/#1233/#1229/#1220 (red Sanitizer ASAN). The Sanitizer ones share a single **root cause** — the `Run ctest under ASAN` deterministic stack-overflow — which decision 1 fixes directly; promoting the (now-green) checks to required is the systemic gate that stops the next red from escaping. Cite the shipped slice as the discharging fix in `postmortems.md`.

## Verification

- **Phase 1:** `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` (YAML/JSON/docs — no C++), `scripts/dev/test-docs.sh` (doc anchors), `bash agents/scripts/core/test-required-context-parity.sh` (both new contexts resolve to their jobs), and the PR's own CI — with the Core-touch/`workflow_dispatch` caveat above so the ASAN job actually runs green. Validate the escape branch on a labeled smoke PR.
- **Phase 2:** `gh api .../protection/required_status_checks` shows 9 contexts; docs-only smoke PR mergeable; Core-touching smoke PR runs all three.

## Perf-gate section

N/A — diff is CI YAML + JSON config + docs only. No `Source/Core/` code, no runtime path, no per-frame work.

## Deviations

- vs the original (pre-recon) plan: there is **no new UBSan job** — it already exists (`Sanitizer (UBSan via Clang)`). The real Phase-1 product of this slice is the ASAN `Run ctest under ASAN` deterministic-red fix + escape hatches + config promotion, not a job stand-up. The α/β preset fork is moot (α was already the implementation).
- UBSan-job ctest symmetry refactor (mirror the MSVC job's `-E smatchet_tests` + excluded rig step) deferred — the job is green; changing it adds risk for no current gain. Backlog candidate if the adversarial case ever flakes under Clang instrumentation.

## Implementation log

- `af475041` · Phase 1 (#1253) — fixed the `Run ctest under ASAN` deterministic-red, added both in-workflow `*-out-of-band` escape hatches (`sanitizer-asan` + `sanitizer-ubsan-pr`), and added both `Sanitizer (ASAN via MSVC)` + `Sanitizer (UBSan via Clang)` contexts to `project.config.json` (`branch_protection.required_contexts` + `ci.required_checks`).
- Phase 2 (final deliverable) — flipped the live `develop` branch-protection `required_status_checks` 6 → 9 via `setup-branch-protection.sh`, now requiring `Coverage` + both Sanitizer contexts.

## Deviations from plan

- None beyond those already recorded in § Deviations above (no new UBSan job; UBSan ctest symmetry refactor deferred).

## Verification (actual)

- Live `develop` branch-protection ruleset now lists **9** required contexts including all three targets (`Coverage (windows-2022 + OpenCppCoverage)`, `Sanitizer (ASAN via MSVC)`, `Sanitizer (UBSan via Clang)`) — confirmed via live `gh api .../protection/required_status_checks`; corroborated by sibling `sanitizer-required-context.md` + `process.md`.
- Both Sanitizer contexts present in `project.config.json` (`branch_protection.required_contexts` + `ci.required_checks`) — verified present in tree (archival audit 2026-06-16), not re-run.

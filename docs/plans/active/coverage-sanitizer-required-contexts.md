# Plan — Promote Coverage + Sanitizer to branch-protection required contexts (testing-surface Slice C)

**Status:** plan — awaiting review (NO code until approved)
**Branch:** `ci/coverage-sanitizer-required-contexts` · worktree `C:\Dev\trees\coverage-sanitizer-required`
**Parent:** [`testing-surface-roadmap.md`](testing-surface-roadmap.md) Slice **C** (§6 P0). First of the two gating-policy slices (C → B); the roadmap flagged C/B for re-confirmation after the additive H+A+D+E1 block — that re-confirmation happened (user: "Plan Slice C next").

## Why this is non-trivial (gating policy — affects every session)

A branch-protection **required context** is enforced by GitHub itself on every PR into `develop`. Get it wrong and **all** concurrent sessions wedge: a required check that never reports, or that is red on `develop` HEAD, makes every PR unmergeable ("Expected — Waiting for status"). So this slice is plan-first and the rollout is staged + precondition-gated, not a single push.

## Recon — what the roadmap assumed vs. the actual tree

The roadmap scoped Slice C as "refactor `coverage.yml` + the ASan/UBSan jobs to Pattern A (no `paths:` filter; internal change-detection) so they always report and can join `required_contexts`." **Most of that workflow refactor is already done.** The real, narrower gap is a **config↔live drift** plus a **missing escape hatch**. Findings:

1. **`coverage.yml` is already Pattern A and already in config.** No `on.paths` filter; single `windows-coverage` job self-gates via a `Detect coverage-relevant changes` step (green no-op when irrelevant); has a `merge_group:` trigger; `continue-on-error: false --threshold 65` (BLOCKING, graduated 2026-06-04). Its header documents the promotion to a required context as **done**, and `project.config.json § branch_protection.required_contexts` **does list** `Coverage (windows-2022 + OpenCppCoverage)`.

2. **…but the LIVE ruleset never got Coverage.** `gh api repos/alexandrosk0/Smatchet/branches/develop/protection/required_status_checks` returns **6** contexts — `Coverage (…)` is **absent**. Config-as-code declared it; the apply step (`setup-branch-protection.sh`, a manual-only full-replace PUT — it runs in **no** workflow) was never re-run. **This drift is the root cause of the #1227 gate-escape** (a red Coverage merged because GitHub never required it; the merge-gates poller's allow-list only catches it on the poller path, which a direct `gh pr merge`/admin merge bypasses).

3. **`Sanitizer (ASAN via MSVC)` is in neither config nor live** — yet it is already shaped for promotion. It is the `sanitizer-asan` job in `build-and-test.yml` (job-level `if: needs.changes.outputs.source_core_cpp == 'true'`, **not** a workflow `paths:` filter → reports `skipped`=success on irrelevant PRs), the `changes` detect step **forces `source_core_cpp=true` on `merge_group`** (so it reports a terminal status on the queued merge), and the merge-gates allow-list already names `Sanitizer` as meant-to-block. This is textbook **Pattern C** (`docs/agent-rules/ci-required-check-pattern.md`). So **no workflow restructure is needed** to make Sanitizer required either. The 5 escaped red-Sanitizer PRs (#1232/#1233/#1229/#1220 + the recurring class) all merged via the poller-bypass path — exactly what a real required context closes.

4. **Nightly does not collide.** `sanitizer-nightly.yml`'s job is named `Sanitizer nightly (Clang ASan+UBSan)` — distinct from the PR check `Sanitizer (ASAN via MSVC)`. Promoting the PR check does not touch the nightly.

**Net:** Slice C is *not* a workflow refactor. It is (a) reconcile config→live so Coverage is actually enforced, (b) add Sanitizer to config + live, and (c) give Sanitizer the in-workflow escape hatch that Coverage already has — because once a check is a GitHub required context, a merge-gates *label* can no longer save a red run (GitHub blocks regardless); the only escape is the check itself reporting green.

## Goal

`Coverage (windows-2022 + OpenCppCoverage)` and `Sanitizer (ASAN via MSVC)` become **live** branch-protection required contexts on `develop`, each with an in-workflow `*-out-of-band` label escape, promoted only against a known-green `develop` HEAD, with no merge-queue/path-filter deadlock.

## Design decisions (review these)

1. **Add a `sanitizer-out-of-band` in-workflow escape**, mirroring `coverage-out-of-band` in `coverage.yml`. When the PR carries the label, the `Run ctest under ASAN` / sanitized-rig steps downgrade a non-clean run to a `::warning::` and the job exits 0 (green), so a flaky/known ASan red can still merge while the recovery follow-up is queued. **Why required, not optional:** after promotion, GitHub enforces the actual check status; the existing merge-gates `Sanitizer` allow-list (poller-only) cannot waive a GitHub-required red. The only post-promotion escape is the check going green — so the hatch must live *in the workflow*. *Alternative considered:* admin-merge override only — rejected (admin override is for stale-BLOCKED states, not a routine flake valve, and leaves no audit label).

2. **Two-phase rollout, ruleset flip last** (see § Rollout). Phase 1 (PR): the `sanitizer-out-of-band` escape + the `project.config.json` required_contexts edit + doc fixes — all safe, no live-ruleset change. Phase 2 (after Phase 1 merges **and** both checks are confirmed green on `develop` HEAD): run `setup-branch-protection.sh` to flip the live ruleset. Flipping against a red `develop` would insta-wedge every open PR; staging removes that window.

3. **Promotion precondition: both checks green on `develop` HEAD.** Phase 2 is gated on a fresh `gh` check-run query for the current `develop` SHA showing `Coverage (…)` and `Sanitizer (ASAN via MSVC)` both SUCCESS (or legitimately skipped). If either is red on develop, fix that first — do not promote a red check.

4. **No change to `merge-gates.sh`.** It already lists `Coverage|Sanitizer` in `MERGE_GATES_BLOCK_ALLOWLIST_RE`. Post-promotion the poller and GitHub agree (a red check blocks both ways; an out-of-band label greens the check, which the poller then sees as green). The allow-list stays as defence-in-depth for any future non-required check.

5. **`setup-branch-protection.sh` is the apply mechanism; the admin-token PUT is the one human-gated step.** It is a manual-only, idempotent full-replace PUT needing a repo-admin token. The plan does not automate it into CI in this slice (that is a separate hardening — see § Follow-ups). Whoever runs Phase 2 must hold an admin-scoped `gh` token.

## Files to modify

| File | Change | Phase |
|---|---|---|
| `.github/workflows/build-and-test.yml` | add `sanitizer-out-of-band` label detection to the `sanitizer-asan` job (read `github.event.pull_request.labels`, downgrade a non-clean ASan run to `::warning::` + exit 0), mirroring coverage.yml's pattern | 1 |
| `project.config.json` | add `"Sanitizer (ASAN via MSVC)"` to `branch_protection.required_contexts` (Coverage already present) | 1 |
| `docs/guides/testing-surface.md` | §3 Coverage row "blocks its own job" → "blocks + now a required context"; add a Sanitizer row; note the config↔live re-apply requirement (the drift root cause) | 1 |
| `docs/agent-rules/ci-required-check-pattern.md` | add a short "config↔live drift" note: editing `required_contexts` is inert until `setup-branch-protection.sh` is re-run; cite #1227 | 1 |
| *(live GitHub ruleset)* | `bash agents/scripts/core/setup-branch-protection.sh` — flips live `develop` protection from 6 → 8 required contexts (admin token; not a file edit) | 2 |

## Rollout

**Phase 1 — PR (safe; no live-ruleset change).**
1. Add the `sanitizer-out-of-band` escape to `build-and-test.yml`.
2. Add `Sanitizer (ASAN via MSVC)` to `project.config.json` required_contexts.
3. Doc fixes (testing-surface.md §3, ci-required-check-pattern.md drift note).
4. Ship-loop → PR → gates → merge. (This PR itself does not yet enforce the new contexts — it only stages config + escape; `setup-branch-protection.sh` is not run here.)

**Phase 2 — live ruleset flip (after Phase 1 merges).**
5. Confirm `develop` HEAD is green on **both** `Coverage (…)` and `Sanitizer (ASAN via MSVC)` (fresh `gh api .../commits/<develop-sha>/check-runs`).
6. Run `bash agents/scripts/core/setup-branch-protection.sh` with a repo-admin token. Verify with `gh api .../protection/required_status_checks` showing 8 contexts.
7. Smoke: open a trivial docs-only PR → confirm both new contexts report (Coverage no-ops green; Sanitizer reports `skipped`=success) and the PR is mergeable (no "Waiting for status" wedge). Open a Core-touching PR → confirm both actually run.

## Flake-budget / escape hatches

- **Coverage:** `coverage-out-of-band` (already in coverage.yml) downgrades below-threshold to WARN/green.
- **Sanitizer:** `sanitizer-out-of-band` (new, this slice) downgrades a non-clean ASan run to WARN/green.
- Both labels are merge-time valves for a *known* flake while the recovery follow-up is queued; neither is a standing waiver (the label must come off post-merge, same discipline as `tests-out-of-band`).
- The two `*-out-of-band` labels must exist in the repo label set before Phase 2 (create if absent).

## Risks

- **Wedge-on-red:** promoting a check that is red on `develop` HEAD wedges all PRs. Mitigated by the Phase-2 green precondition (decision 3).
- **Merge-queue ref:** if `develop` ever goes behind a GitHub merge queue, both checks already trigger on `merge_group` (coverage.yml `merge_group:`; build-and-test.yml detect-step forces `source_core_cpp=true`) — no queue deadlock. Confirmed in recon, no action needed.
- **Admin-token availability:** Phase 2 needs repo-admin scope. If the running session lacks it, hand the one-line `setup-branch-protection.sh` invocation to the user — call this out at Phase-2 time rather than assuming.
- **Skipped-required semantics:** Pattern C relies on GitHub treating an `if:`-skipped required job as success. This is the documented behaviour the existing MSVC required contexts already depend on; the Phase-2 docs-only smoke test (step 7) verifies it for Sanitizer specifically before trusting it.

## Discharges owed postmortems

The SessionStart banner shows 5 owed gate-escape postmortems whose mandatory `### Preventing gate` is precisely this slice: #1227 (red Coverage) and #1232/#1233/#1229/#1220 (red Sanitizer) all escaped because these checks were not GitHub-required. Slice C is the systemic gate those postmortems point to. The plan-doc + shipped slice should be cited as the discharging fix in `postmortems.md` (the postmortems themselves are a separate owed deliverable, but Slice C is their resolution).

## Verification

- Phase 1: `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` (YAML/docs only — no C++), `scripts/dev/test-docs.sh` (doc anchors), and a workflow-syntax sanity check (the PR's own CI re-runs `build-and-test.yml`, exercising the new label branch on a labeled smoke PR).
- Phase 2: `gh api .../protection/required_status_checks` shows 8 contexts; docs-only smoke PR mergeable; Core-touching smoke PR runs both checks.

## Perf-gate section

N/A — diff is CI YAML + JSON config + docs only. No `Source/Core/` code, no runtime path, no per-frame work.

## Open questions for the reviewer

1. **Phase 2 admin token** — do you want me to run `setup-branch-protection.sh` (if this session's `gh` has admin scope), or hand you the one-liner to run? (It is the single irreversible-ish, all-sessions-affecting step.)
2. **Bundle or split** — Phase 1 (escape + config + docs) as one PR, Phase 2 (ruleset flip) as a manual step after it merges — is that staging acceptable, or do you want Phase 2 folded into the same session immediately after Phase 1 merges?
3. **Scope check** — the roadmap named "ASan/UBSan jobs" (plural). The PR-time UBSan surface is Clang, which today runs **only** in `sanitizer-nightly.yml` (cron, not per-PR) and `tsan-linux-nightly.yml`. There is no per-PR UBSan check to promote. Confirm Slice C = the two per-PR checks (`Coverage`, `Sanitizer (ASAN via MSVC)`) and that adding a per-PR UBSan gate is out of scope (→ backlog / a later slice).

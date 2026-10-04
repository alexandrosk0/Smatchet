# Native auto-merge waits only on required contexts, so every arm path except `safe-merge.sh` merges past a red non-required check

- **Category**: infra
- **Priority**: P1
- **Date**: 2026-10-04
- **Observed on**: PR #2286 (merged `b4e71bec` 2026-10-04T01:59:46Z; `Bucket-E UI tests (Mesa headless GL)` had been **failure** since 01:56:45Z). Same class as PRs #1406, #1414, #1415, #1438 and #1566 (`postmortems.md` 2026-06-19, 2026-06-20 and 2026-06-27 entries).
- **Status**: open

## What happened

Timeline on head `dfa2e0ce`, from `gh api repos/alexandrosk0/Smatchet/issues/2286/timeline` and `.../commits/dfa2e0ce…/check-runs`:

| UTC | event |
|---|---|
| 01:17:42 | PR opened |
| 01:43:54 | `auto_squash_enabled`: native GitHub auto-merge armed through the desktop app's `set_auto_merge` PR tool. Six checks were still running: ASAN, Bucket-E Jira, **Bucket-E UI**, UBSan, Mobile texture-guard (advisory) and CodeQL analyze |
| 01:56:45 | `Bucket-E UI tests (Mesa headless GL)` → **failure** (`TrackerFirstRun/TestConnectionClickKeepsDisabledStackBalanced`: "Unable to locate item: 'Preferences/**/Test connection'") |
| 01:59:29 | `Sanitizer (UBSan via Clang)` → success. This was the last of the 22 branch-protection required contexts |
| 01:59:46 | GitHub merged the PR as `b4e71bec`, 17 s after the last required check and 3 min after the red |
| 02:00:25 | the `Build and test` workflow run (which hosts Bucket-E) completed `failure` |

`Bucket-E UI tests (Mesa headless GL)` is not in `project.config.json` § `branch_protection.required_contexts`. GitHub auto-merge only waits on that set. Block-on-any-red is enforced only by `merge-gates.sh`, and that poller only runs when the merge goes through `safe-merge.sh`, `git-janitor`, or the merge-watcher. `CodeQL analyze (c-cpp)` was also still **pending** at the merge, so the merge crossed a non-advisory check that had not finished, as well as the red one.

Before arming, the session wrote that GitHub auto-merge only waits for the checks marked required, and that auto-merge should be turned off if a non-required check went red. Nothing acted on that, and the arming session's transcript shows no activity after 01:44:25Z. Develop's own `Bucket-E UI tests` run on `b4e71bec` failed at 02:30:51Z. Develop stayed red until `385f3833` (#2280, merged 04:47:49Z) fixed the test, about 2 h 48 min later.

## Why it matters

This is the fourth `postmortems.md` incident of the class, and the sixth PR. The earlier ones were #1406/#1414/#1415, #1438 and #1566. #1566's remedy was to promote one lane to required. The others were fixed on the **arming side**:

- the `merge-gates.md` rule that forbids a bare `gh pr merge --auto` and a direct REST `PUT`;
- the `safe-merge.sh` wrapper;
- the out-of-band `## Intent` authoring rule.

Each fix covered the arm path, or the lane, that had just been seen. This one used an arm path that no rule names: the harness's own auto-merge tool. Others are in reach of an agent today: a GitHub MCP auto-merge tool, the GitHub web UI's "Enable auto-merge" button, and `gh` itself. Each one creates the same server-side `autoMergeRequest`, and that request only waits on required contexts. A rule per arm path can never be complete. The only place that sees every arm path is the merge condition GitHub itself evaluates.

A red required check is inherited by every open PR's merge ref under block-on-any-red. That happened here: the repo was blocked until #2280 landed.

## Concrete next action

1. **Primary — one required aggregate context that GitHub's auto-merge must wait on.** Add a workflow, for example `.github/workflows/all-checks-green.yml`, on `pull_request` (`opened`, `synchronize`, `reopened`, `labeled`, `unlabeled`, `ready_for_review`). It runs a single job, `All checks green (block-on-any-red)`, which:
   - polls `GET repos/{repo}/commits/{head_sha}/check-runs` and `.../status` until every other check is terminal;
   - fails fast on the first non-advisory red;
   - succeeds only when everything is terminal and green.

   Add that job name to `branch_protection.required_contexts`. Reuse the poller's semantics so the aggregate and `merge-gates.sh` agree on what "red" means. That means sourcing or porting the CI half of `merge-gates.d/10-gate-filter.sh`:
   - newest-suite duplicate collapse;
   - the `advisory` name exemption;
   - the `*-out-of-band` downgrades, re-evaluated on a `labeled` event.

   The job excludes itself by name. Runtime is about the length of the slowest lane (~40 min), mostly asleep in one `ubuntu-latest` job. Actions minutes are free on a public repo, but the job occupies a concurrency slot. Use `timeout-minutes` with fail-closed behaviour: a timeout is red and blocks, and a rerun clears it.
2. **Rejected alternative, kept so it is not re-proposed — a `workflow_run`-triggered disarm** (`gh pr merge --disable-auto` when a run concludes `failure`, modelled on `automated-pr-guard.yml`). Replayed against #2286 it fires **too late**: `workflow_run` fires once per *workflow*. `Build and test` completed at 02:00:25Z, 39 s after the merge, because UBSan and Mobile texture-guard share its run. It also cannot see a check that is still pending, which is the `CodeQL analyze` shape above.
3. **Cheaper partial step, only if (1) is deferred:** add an `alls-green` style job to `build-and-test.yml` itself. It would `needs:` every job with `if: always()`, fail if any needed non-advisory job failed, and be added to the required contexts. That covers #2286 exactly, since Bucket-E sits in `Build and test`. It does not cover checks in other workflows: `CodeQL analyze`, `TSan Linux subset`, `Intent section`, `Plan-lock gate` and others.
4. **Arming-side companion:** name harness-native auto-merge tools in `docs/agent-rules/merge-gates.md` § Sanctioned non-admin merge path, as equivalent to a bare `--auto`. Add a Claude Code `PreToolUse` hook that denies the desktop `set_auto_merge` tool (enable) and GitHub-MCP auto-merge tools, and points to `safe-merge.sh`. This is defence in depth only. It covers one harness, so it cannot replace (1).

Preconditions for (1):
- The new context must always report. A required check that never reports deadlocks every PR (`docs/agent-rules/ci-required-check-pattern.md`), so the workflow takes no `paths:` filter.
- Land it only on a green develop tip. Adding a required context while any lane is red blocks every open PR.

**Enumerator + replay** (per `AGENT_SELF_IMPROVEMENT.md`): the enumerator is the head commit's check-run list, `GET repos/alexandrosk0/Smatchet/commits/dfa2e0ce6c52711d0825e5aa772818785050887f/check-runs` (52 runs), deduplicated by name to the newest suite. Replayed on that list:

- **01:43:54Z (arm):** six runs are non-terminal, so the aggregate is still running and the required context is pending.
- **01:56:45Z:** `Bucket-E UI tests (Mesa headless GL)` concludes `failure`. Its name contains no `advisory`, so the aggregate fails and the required context goes red.
- **01:59:29Z:** UBSan turns green, but auto-merge stays held on the red aggregate, and #2286 does not merge.

The bats companion is a `tests/bats/` case that feeds the aggregate's filter a fixture of that list. It asserts `failure` with Bucket-E red, `pending` with CodeQL in progress, and `success` with only the advisory texture-guard lane non-green.

Related: [`postmortems.md`](../../postmortems.md) (2026-06-19 #1406/#1414/#1415, 2026-06-20 #1438, 2026-06-27 #1566 entries: the same class, each closed on the arming side), [`docs/agent-rules/merge-gates.md`](../../../agent-rules/merge-gates.md) § Sanctioned non-admin merge path.

Triggered-follow-up: when=pr-count:base=develop;since=2026-10-04;n=20; action=check whether a required aggregate context landed, and whether any PR has merged with a non-advisory check red or pending since; baseline=1 escape (#2286, red Bucket-E UI tests + pending CodeQL analyze at merge) armed through a harness auto-merge tool, 2026-10-04; fired=never

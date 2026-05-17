# git-ref plan-locks — atomic remote-agent coordination

**Status**: draft (Phase 0 pending) · **Slug**: `git-ref-plan-locks` · **Owner**: orchestrator · **Created**: 2026-05-17

## Problem

`docs/design/_plan-locks.md` is a single markdown file edited by every agent that claims, transitions, or releases a plan-lock. As the number of concurrent remote agents grows, this file becomes a contention point:

- **Race on the file itself.** Two agents editing the markdown concurrently produce merge conflicts on the lock-coordination artefact, not on the work they're trying to coordinate.
- **Non-atomic state transitions.** `claimed → in-flight → shipped` are three separate hand-edits scattered across PRs. Easy to forget the flip; `_plan-locks.md` drifts from reality.
- **Per-flip PR overhead.** Pure status-flip PRs (e.g. PR #187) ship docs-only edits just to keep the file current. Reviewer load + CI cost per flip.
- **No enforcement.** Nothing prevents an agent from writing to a path another slice has claimed. The file is convention-only.
- **Pruning friction.** Shipped entries accumulate; manual sweeps required.

The four orthogonal subproblems (claim / discovery / collision-catch / cleanup) are all served by one markdown file, badly.

## Proposed solution

Use **git refs under `refs/locks/<slug>`** as the atomic claim primitive. Each ref points to a tiny commit whose tree contains a single `claim.json` blob with the write set, owner, and timestamps. Coordination becomes:

- **Claim** — `git push origin <commit>:refs/locks/<slug>` with `0000...` old-sha. Git's ref-update protocol gives free compare-and-swap; concurrent claims resolve deterministically.
- **Discovery** — `git ls-remote origin 'refs/locks/*'` returns all in-flight claims in one network call.
- **Collision catch** — final PR merge against develop catches any missed claim via standard git merge-conflict detection (and optionally GitHub merge queue when added).
- **Cleanup** — GitHub Action triggered on `pull_request: closed && merged == true` parses `lock-slug: <slug>` from the PR body and deletes the ref. Staleness sweep posts an Issue for refs older than 14 days.

A derived `_plan-locks.md` keeps human-readable visibility by regenerating from refs on every change.

### Why git refs

- **Atomic by construction.** Git's ref-update protocol provides compare-and-swap at the server. No external lock service needed.
- **Zero file contention.** Refs live outside the working tree. Agents never edit the same file to claim.
- **Scales linearly with agent count.** N agents → N refs. No O(N²) conflict surface.
- **Standard git plumbing.** No bespoke service, no new auth surface beyond normal push.
- **Audit trail.** Ref reflog + commit history on each lock ref preserves who claimed what when.
- **Failure-safe.** Crashed agents leave a stale ref; staleness sweep flags it as an Issue rather than silently breaking coordination.

### Layered defense

| Layer | Mechanism | Catches |
|---|---|---|
| 1 | Git ref CAS via `push` | Concurrent claim race |
| 2 | Pre-flight `git ls-remote` + write-set intersection | Known collision before work starts |
| 3 | PR merge against develop | Missed claim, stale claim, scope-creep collision |
| 4 | (Optional) GitHub merge queue | Two PRs racing to merge same line |
| 5 | (Optional) CODEOWNERS on sensitive paths | Unauthorised edits to coordination-critical files |
| 6 | Cleanup action on PR merge | Forgotten release |
| 7 | Daily staleness sweep | Abandoned claims, crashed agents |

No single point of trust. Each layer is independently reversible.

## Non-goals

- Adding GitHub merge queue (separate work item; mention only as future enhancement).
- Replacing existing branch protection on `develop` (untouched).
- Cross-repo coordination (single-repo scope).
- Replacing the planning workflow itself — only the locking primitive changes.

## Phased rollout

Each phase is shippable independently and reversible by revert.

### Phase 0 — Pre-flight investigation

Verify the environment supports the design before building.

- Confirm GitHub Rulesets enabled on `alexandrosk0/Smatchet` (supports custom ref patterns for the optional Phase 7 hardening).
- Confirm `gh` CLI auth scopes include `contents: write` for the cleanup-bot path.
- Smoke test: `git push origin <sha>:refs/locks/_smoke && git push origin --delete refs/locks/_smoke` succeeds against the live remote with no special config.
- Inventory current `_plan-locks.md` entries — count `in-flight`, `claimed`, `shipped`. Baseline for migration audit in Phase 6.

Deliverable: append a `## Phase 0 results` section to this document.

### Phase 0 results (2026-05-17)

All checks complete on `feat/git-ref-plan-locks` branch.

#### Environment

| Check | Result | Notes |
|---|---|---|
| `gh auth status` scopes | `gist, read:org, repo, workflow` | `repo` scope includes `contents: write` — sufficient for cleanup action and manual ref pushes |
| Default branch | `develop` | Confirmed via `gh api repos/alexandrosk0/Smatchet` |
| Repo visibility | **public** | Caveat below |
| GitHub Rulesets | `[]` (none configured) | Phase 7 ruleset hardening is greenfield — no existing rules to preserve or migrate |

#### Smoke test — `refs/locks/*` push lifecycle

```
$ git push origin b3e44f6:refs/locks/_smoke
 * [new reference]   b3e44f6 -> refs/locks/_smoke

$ git ls-remote origin 'refs/locks/*'
b3e44f63123246e2fbf6f9f393a6d3b8da4189ed	refs/locks/_smoke

$ git push origin :refs/locks/_smoke
 - [deleted]         refs/locks/_smoke

$ git ls-remote origin 'refs/locks/*'
(empty)
```

PASS. Push of arbitrary commit sha to a custom `refs/locks/*` namespace, ls-remote discovery, and deletion all work end-to-end with the existing `repo`-scoped token. No branch-protection rule, ruleset, or workflow config blocked any step. Atomic CAS via the standard git ref-update protocol applies by default — no additional opt-in required.

#### `_plan-locks.md` inventory baseline

| Status | Count |
|---|---|
| `in-flight` | **1** |
| `claimed` | **1** |
| `shipped` | 36 |
| `abandoned` | 3 |
| **Total entries** | **41** |
| File size | 727 lines |

Phase 6 migration scope: **2 entries** (`in-flight` + `claimed`). 36 `shipped` entries already do not require coordination and can be deferred for cleanup or pruned outright pre-cutover. 3 `abandoned` entries are dead and ignored by `locks-migrate-from-markdown.sh`.

#### New risks surfaced

- **Public-repo metadata exposure** — `refs/locks/*` claim blobs are world-readable on a public Smatchet repo. Contents are slug + owner agent id + write set + timestamps. No secrets, but write-set paths could telegraph in-flight refactors to anyone watching the repo. Acceptable trade-off; documented here so it isn't a surprise later. Mitigation: do not embed sensitive context in claim blobs (no API keys, no internal URLs, no embargoed feature names).
- **No existing rulesets** — Phase 7 ruleset work has zero migration cost but also zero existing safeguard. Until Phase 7 ships, any push with `repo` scope can create or delete any `refs/locks/*` ref. Mitigation: cleanup action source is short + reviewable; staleness sweep posts Issues on anomalies rather than silently mutating; agent-token scopes already gate write access at the org level.

#### Decision

Greenlight Phases 1–6. No environmental blockers. The optional Phase 7 ruleset item is also greenlight (no prior rules to merge with) but stays opportunistic.

### Phase 1 — Primitive scripts

Build the claim primitive standalone. No agent reads it yet.

Files (new):

- `scripts/dev/lock-claim.sh` — args: `<slug> <write-set-file>`. Builds claim blob, commits on detached tree, pushes to `refs/locks/<slug>`. Atomic CAS via standard non-fast-forward rejection. Retries only on transient errors (3 retries, exponential backoff); CAS rejection is terminal.
- `scripts/dev/lock-claim-update.sh` — args: `<slug> <new-write-set-file>`. Force-with-lease updates existing ref. For scope growth mid-slice.
- `scripts/dev/lock-release.sh` — args: `<slug>`. Deletes remote ref. Idempotent. For abandoned local branches.
- `scripts/dev/locks-show.sh` — no args. Renders all `refs/locks/*` as a stdout table (or JSON when `LOCKS_SHOW_FORMAT=json`).
- `scripts/dev/_lock-json.py` — internal Python helper. Builds + parses + formats `claim.json` payloads. Pure stdlib. Underscore-prefix marks it as not part of the supported `scripts/dev/` surface.
- `scripts/dev/test-lock-primitives.sh` — exercises claim / claim-update / release / show in a sandboxed bare repo with two clones. Asserts CAS rejects concurrent claim and stale-lease update hijack. Auto-enrolled by `scripts/dev/test-all.sh`.

Acceptance: tests pass. Two concurrent `lock-claim.sh` invocations on the same slug → exactly one succeeds. `locks-show.sh` prints both before either releases.

PR title: `chore(plan-locks): introduce git-ref primitive scripts (phase 1)`

### Phase 2 — Derived markdown view

Keep `_plan-locks.md` agent-readable while truth migrates to refs.

Files (new):

- `.github/workflows/locks-render.yml` — triggers on push to any `refs/locks/*` ref (and nightly + workflow-dispatch). Runs the render script, opens or updates a sync PR. No direct push to develop.
- `scripts/dev/locks-render-markdown.sh` — reads all `refs/locks/*` via `git ls-remote` + `git cat-file`, emits Markdown from a template. Idempotent.

Files (modified):

- `docs/design/_plan-locks.md` — banner inserted at the top: *"Auto-generated from `refs/locks/*` — do not hand-edit below the line."* Hand-edited content below the line will be lost on next regeneration (intentional after Phase 6 cutover; before cutover, banner is informational only).

Acceptance: pushing a test claim ref triggers the action; PR opens within 5 minutes; merging it produces a develop-tip `_plan-locks.md` reflecting the ref state.

PR title: `chore(plan-locks): add ref→markdown sync action (phase 2)`

### Phase 3 — Cleanup action

Auto-release on PR merge.

Files (new):

- `.github/workflows/lock-cleanup.yml` — fires on `pull_request: closed && merged == true`. Parses PR body for a `lock-slug: <slug>` line. If present and the ref exists, deletes the ref. Logs no-op otherwise. Uses default `GITHUB_TOKEN` with `contents: write`.

Files (modified):

- `.github/PULL_REQUEST_TEMPLATE.md` — append an optional `lock-slug:` line in a comment block.

Token-scope note: the default `GITHUB_TOKEN` has repo-wide `contents: write`. Narrowing to `refs/locks/*` only requires a fine-grained PAT or GitHub App and is deferred to Phase 7 hardening. Mitigation today: the action source is short, reviewable, and runs only on the merge event.

Acceptance: open a synthetic PR with `lock-slug: _test-cleanup` in the body, merge it, observe the action delete `refs/locks/_test-cleanup` within 60 seconds.

PR title: `chore(plan-locks): add merge-time cleanup action (phase 3)`

### Phase 4 — Staleness sweep

Catch abandoned claims without deleting silently.

Files (new):

- `.github/workflows/lock-staleness.yml` — daily cron + workflow-dispatch. Lists `refs/locks/*`, parses `claim.json.started`, opens or updates an Issue per ref older than 14 days. Issue title: `Stale plan-lock: <slug>`. Body lists owner, write set, age, originating plan. Action does **not** delete.

Acceptance: backdate a test ref's `started` field via `lock-claim-update.sh`, trigger the cron via workflow-dispatch, verify an Issue is opened.

PR title: `chore(plan-locks): add staleness sweep (phase 4)`

### Phase 5 — Agent prompt migration

Switch agents from reading `_plan-locks.md` to running the live ref-aware scripts.

Files (modified):

- `AGENTS.md` § Orchestrator delegation packet — replace the standard claim wording. From: *"Read `docs/design/_plan-locks.md` first. Refuse if your write set overlaps an `in-flight` or `claimed` entry; surface to the orchestrator."* To: *"Run `bash scripts/dev/locks-show.sh` first (live ref state). The auto-generated `docs/design/_plan-locks.md` is a snapshot view that may lag by minutes. Claim via `bash scripts/dev/lock-claim.sh <slug> <write-set-file>` before the first edit. Refuse if your write set overlaps any active claim; surface to the orchestrator. On scope growth, run `lock-claim-update.sh`. The PR body must include the line `lock-slug: <slug>` so the cleanup action releases on merge."*
- Per-agent prompt files under `agents/*.md` that reference `_plan-locks` — grep + update each. Inventory taken in Phase 0.

Acceptance: dispatch one routine slice end-to-end through the new flow. Verify claim → work → push → PR-merge → ref deleted → markdown view regenerated. Zero hand-edits to `_plan-locks.md` during the slice.

PR title: `docs(agents): migrate plan-lock prompts to git-ref primitive (phase 5)`

### Phase 6 — Backfill + cutover

Convert existing `_plan-locks.md` claims to refs without disrupting active work.

Files (new):

- `scripts/dev/locks-migrate-from-markdown.sh` — one-shot. Parses current `_plan-locks.md`, calls `lock-claim.sh` for every `in-flight | claimed` entry. Skips `shipped | abandoned`.

Files (modified):

- `docs/design/_plan-locks.md` — strip all hand-edited content below the banner. From this PR forward, the file is fully auto-generated; future edits flow through refs only.

Cutover procedure:

1. Coordinate with all active slice owners on a cutover date.
2. Run `locks-migrate-from-markdown.sh` locally on develop tip; push refs.
3. Verify `git ls-remote origin 'refs/locks/*' | wc -l` equals the count of active markdown entries.
4. Open the cutover PR (this Phase). On merge, the render action regenerates the file from refs; result should be byte-identical to the pre-cutover markdown (modulo timestamps).

Acceptance: every `in-flight` / `claimed` entry as of migration day has a corresponding `refs/locks/<slug>`. Render action output matches manual file at cutover.

PR title: `chore(plan-locks): backfill existing claims into refs and cut over to ref-truth (phase 6)`

This is the single point of no return. Mitigations: (a) coordinate before merge, (b) keep `locks-migrate-from-markdown.sh` in tree for re-runs if a ref gets clobbered, (c) markdown view continues to render so visibility is preserved.

### Phase 7 — Optional hardening (opportunistic)

Build only if collision frequency warrants. Each is an independent micro-PR.

- **CODEOWNERS** for sensitive paths (`Source_Core/include/ITrackerClient.h`, `Source_Core/include/Commands/`, `cmake/`).
- **GitHub Ruleset** restricting `refs/locks/*` creation to authenticated push and deletion to the cleanup-action identity only.
- **Fine-grained PAT** for the cleanup action narrowing scope to `refs/locks/*` deletion.
- **GitHub merge queue** on develop for final collision catch.

## Verification

Filled in during each phase.

- Phase 0: results section appended below.
- Phase 1: `bash scripts/dev/test-lock-primitives.sh` green (8/8). Also auto-discovered by `bash scripts/dev/test-all.sh --filter lock-primitives`.
- Phase 2: ref push triggers render PR within 5 min; merged PR produces matching `_plan-locks.md`.
- Phase 3: synthetic merge → ref deleted within 60 s.
- Phase 4: backdated ref → Issue opened within 24 h.
- Phase 5: one real slice end-to-end with zero hand-edits to the markdown.
- Phase 6: ref count == active entry count at cutover; rendered output byte-identical to pre-cutover (modulo timestamps).

## Rollback

Each phase reversible by revert of its phase-PR.

- Phases 1–4: harmless on revert. Scripts and actions become dead code; existing markdown workflow continues.
- Phase 5: revert restores the old prompt wording. Refs continue to exist but agents stop consulting them; markdown view continues to render and stays the source of truth.
- Phase 6 (cutover): revert restores the hand-edited `_plan-locks.md`. Refs continue to exist but become advisory. Manual sweep clears them via `lock-release.sh`.

Worst-case operational impact of any single phase failing: revert merge, return to status quo, refs become inert. No data loss.

## Risk register

| Risk | Likelihood | Mitigation |
|---|---|---|
| GitHub Rulesets unavailable for `refs/locks/*` | medium | Phase 0 verifies; if missing, Phase 7 ruleset deferred indefinitely. Accept wider write scope on cleanup bot |
| Agent forgets `lock-slug:` in PR body | high | Cleanup is no-op (ref stays). Staleness sweep posts Issue at 14 days. Eventually consistent, never wrong |
| Ref → markdown action stalls or breaks | low | `locks-show.sh` always reads live truth. Markdown is convenience only |
| Agent runs `lock-claim.sh` outside repo / wrong remote | low | Script asserts `git rev-parse --show-toplevel` is a Smatchet checkout and `origin` URL matches expected pattern |
| Concurrent claim on same slug | by-design | Git CAS rejects loser. Loser refetches, re-evaluates intersection |
| Cleanup bot token compromised | low | Action source short + reviewable. Worst case = unnecessary merge-queue rebases. No code-push capability |
| Lock hijack via `lock-claim-update.sh` from non-holder | medium | **Known limitation surfaced by Phase 1 audit.** The schema's `owner` field is advisory; the ref layer enforces CAS only, not identity. Any actor with push rights to `refs/locks/*` can take over any lock by force-with-lease against its current sha. Mitigations: (a) Phase 7 GitHub ruleset can restrict ref writes to specific bot or owner identities; (b) Phase 3 cleanup action will validate `lock-slug:` against PR-body declared owner before releasing; (c) social convention + audit trail in the ref reflog. Genuine concurrent-update race protection (two well-meaning agents updating the same lock simultaneously) IS provided by force-with-lease — only the hostile-takeover case is unguarded |
| `_plan-locks.md` byte-mismatch at cutover | medium | Render template tuned during Phase 2 + 5 dry runs. Cutover PR diff reviewed manually before merge |
| Migration script misses an entry | low | Phase 0 inventory + Phase 6 ref-count assertion catches discrepancy before cutover-PR merge |

## Timing recommendation

Ship Phase 0 + 1 immediately. Zero impact on agent prompts; builds the primitive and validates the environment.

Hold Phases 2–6 until one of:

- `_plan-locks.md` conflicts occur in real operations more than ~once per week, **or**
- Concurrent agent count climbs past 4 sustained, **or**
- A docs-only flip-PR (PR #187 shape) is opened in two consecutive merge windows.

Below those thresholds the markdown workflow works fine and the migration cost outweighs the benefit.

Phase 7 is reactive — ship each item when the corresponding pain surfaces.

## Implementation log

- 2026-05-17 · `b3e44f6` · plan drafted (`wip(plan): git-ref-plan-locks`).
- 2026-05-17 · `773d42a` · Phase 0 pre-flight complete; environment greenlit Phases 1–6.
- 2026-05-17 · Phase 1 landed locally on `feat/git-ref-plan-locks`: primitive scripts + Python helper + sandboxed test (8/8). Live end-to-end smoke against `origin` for `refs/locks/tmp-smoke-live` PASS (claim → show → release → idempotent re-release).

## Deviations from plan

- **`jq` swapped for a Python helper** (`scripts/dev/_lock-json.py`). `jq` is not installed on the user's Smatchet dev box; Python 3.x is. Plan originally specified `jq` calls inline in each `.sh` script. Pure stdlib Python is more portable across Smatchet's developer environments (MSYS2 UCRT64 + Windows) and removes an external dependency.
- **Python detection probe** added because `python3` on Windows often resolves to the Microsoft Store stub (`exit 49`). Scripts probe candidates `python`, `python3` and accept only those that pass `sys.version_info[0] >= 3`.
- **Test path** moved from `tests/scripts/lock-primitives.test.sh` to `scripts/dev/test-lock-primitives.sh` so the existing `scripts/dev/test-all.sh` auto-discovery glob (`scripts/dev/test-*.sh`) picks it up without needing changes to the runner.
- **Test case 5** (stale-lease hijack) required `git fetch origin '+refs/locks/*:refs/locks/*'` in the second clone before `commit-tree -p <sha>` could reference the ref's tip as a parent. Documented in the test inline.

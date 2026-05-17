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

Build each item only when its cost is paid back by observed friction. All four items are independent micro-PRs.

#### 7a — CODEOWNERS for sensitive paths (SHIPPED 2026-05-17)

In-tree only. Adds `.github/CODEOWNERS` mapping the lock primitive scripts (`scripts/dev/lock-*.sh`, `scripts/dev/_lock-json.py`), the three workflow files (`.github/workflows/lock*.yml`, `locks-render.yml`), the plan doc, the archive, and the pre-existing cross-target invariant paths (`Source_Core/include/ITrackerClient.h`, `Source_Core/include/Commands/`, `cmake/`, `CMakeLists.txt`, `CMakePresets.json`, `AGENTS.md`, `.claude/CLAUDE.md`) to `@alexandrosk0`.

**Status today**: informational only. Branch protection on develop must be reconfigured to require code-owner review for the file to be enforced. Owner approval-loop limitation in a single-owner repo (GitHub forbids self-approval depending on org settings); enforcement deferred until a second owner exists.

**Recipe for enabling enforcement**: Settings → Branches → develop branch protection → "Require review from Code Owners" checkbox. No code change needed once the human + second-reviewer prerequisites are in place.

#### 7b — GitHub Ruleset restricting `refs/locks/*` (recipe only)

Build when: a `refs/locks/*` ref is mutated by an actor who shouldn't have (caught via reflog audit or anomalous Issue from staleness sweep).

**Recipe**: Settings → Rules → Rulesets → New ruleset, target ref pattern `refs/locks/*`. Add bypass list = `github-actions[bot]` (so `lock-cleanup.yml` keeps working). Add "restrict creations" + "restrict updates" + "restrict deletions" rules. Alternatively, the same via API: `gh api repos/$REPO/rulesets -X POST -f name=plan-locks ...` (full body in `scripts/dev/lock-ruleset-template.json` if/when needed).

#### 7c — Fine-grained PAT for cleanup action (recipe only)

Build when: a security audit flags the default `GITHUB_TOKEN`'s broad `contents: write` scope as too wide for what `lock-cleanup.yml` actually needs.

**Recipe**: create a fine-grained PAT scoped to `contents: write` on this single repo, restricted to `refs/locks/*` (the API supports per-ref scoping via repository content selection). Store as `LOCK_CLEANUP_PAT` repo secret. Replace `${{ secrets.GITHUB_TOKEN }}` with `${{ secrets.LOCK_CLEANUP_PAT }}` in `lock-cleanup.yml`. Rotation policy: 90 days, automated via `gh secret set` from a scheduled cron in a separate workflow.

#### 7d — GitHub merge queue on develop (recipe only)

Build when: a "merged green but broke develop" incident happens, OR PR throughput climbs past ~5 PRs / day.

**Recipe**: Settings → Branches → develop → "Require merge queue". Configure required status checks list (`build-and-test`, `coverage-gate`, `lock-cleanup` if it has a job we want to gate on, plus any others). Merge queue auto-rebases each PR before final merge and re-runs the required checks. PR throughput trade-off: queue is single-threaded per branch, slow CI degrades throughput.

## Verification

Filled in during each phase.

- Phase 0: results section appended below.
- Phase 1: `bash scripts/dev/test-lock-primitives.sh` green (8/8). Also auto-discovered by `bash scripts/dev/test-all.sh --filter lock-primitives`.
- Phase 2: local `bash scripts/dev/locks-render-markdown.sh` renders empty-state stub and the populated state of a temporary `tmp-render-smoke` claim correctly. Initial `docs/design/_plan-locks.generated.md` committed. Workflow `locks-render.yml` end-to-end run deferred to first dispatch post-merge — `act` simulation skipped due to GitHub-specific `gh` + permissions context.
- Phase 3: workflow YAML lints clean under GitHub Actions parser conventions (manual review of the `gh api`-based delete path). End-to-end run deferred to first real merged PR carrying a `lock-slug:` line — exercised at Phase 6 cutover at the latest. Dry-run alternative: open a draft PR with a synthetic `lock-slug: tmp-cleanup-smoke` line + a corresponding `refs/locks/tmp-cleanup-smoke` ref, then close-and-merge the draft.
- Phase 4: workflow YAML lints clean under GitHub Actions parser conventions (manual review). Empty-state path (no refs, no Issues) covered by the `refs` variable early-return. End-to-end run deferred to first scheduled cron post-merge or a manual `workflow_dispatch --field threshold_days=0` invocation against a known stale-looking ref.
- Phase 5: `grep -rln "plan-lock\\|_plan-locks" agents/` returns no matches — no per-agent prompt changes needed. `AGENTS.md § Orchestrator delegation packet` updated; new wording instructs agents to use `bash scripts/dev/locks-show.sh` for live state. Smoke-tested locally: `bash scripts/dev/locks-show.sh` returns table view of live `refs/locks/*` on origin (currently empty). End-to-end verification of the new workflow happens organically when the next slice is dispatched.
- Phase 6: pre-flight inventory showed exactly 1 entry needing migration. `bash scripts/dev/lock-claim.sh git-ref-plan-locks /tmp/ws-cutover.txt` claimed `refs/locks/git-ref-plan-locks` on origin at sha `89c2520`. `bash scripts/dev/locks-show.sh` returned a table row for the new claim. `bash scripts/dev/locks-render-markdown.sh > docs/design/_plan-locks.generated.md` emitted a 1-lock view (16 paths in write set). `git mv` of the markdown file preserved git history. Archive's strip surgery left exactly 42 shipped + 3 abandoned entries (zero in-flight). `bash scripts/dev/test-lock-primitives.sh` → `Passed: 8 Failed: 0`. `lock-cleanup.yml` will fire when this PR merges, deleting the ref; subsequent `locks-render.yml` cron will regenerate `_plan-locks.generated.md` to its empty-state stub.
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
- 2026-05-17 · `0d67b8e` · Phase 1 — primitive scripts + Python helper + sandboxed test (8/8). Live end-to-end smoke against `origin` for `refs/locks/tmp-smoke-live` PASS (claim → show → release → idempotent re-release).
- 2026-05-17 · `84073bc` · post-Phase-1 audit: hostile-takeover risk row added to risk register; dogfood plan-lock entry added to `docs/design/_plan-locks.md`.
- 2026-05-17 · Phase 2 landed locally on `feat/git-ref-plan-locks`: render script + workflow + banner + initial generated file. Live render against `origin` PASS (empty state + populated state via temporary `tmp-render-smoke` claim). Workflow not yet smoke-tested end-to-end — first dispatch happens after the branch lands. PR [#194](https://github.com/alexandrosk0/Smatchet/pull/194) opened as draft.
- 2026-05-17 · Phase 3 landed locally on `feat/git-ref-plan-locks-phase-3` (stacked on PR #194): cleanup workflow `.github/workflows/lock-cleanup.yml` parses `lock-slug:` line from PR body on merge and deletes the corresponding ref via `gh api DELETE`. PR template appended with optional `lock-slug:` instruction. PR [#195](https://github.com/alexandrosk0/Smatchet/pull/195) opened as draft. End-to-end run deferred to first real PR-merge post-merge of this branch.
- 2026-05-17 · Phase 4 landed locally on `feat/git-ref-plan-locks-phase-4` (stacked on PR #195): staleness sweep workflow `.github/workflows/lock-staleness.yml`. Daily cron + `workflow_dispatch` (with `threshold_days` input). Reads `claim.started` / `claim.updated`, uses `max(started, updated)` for age comparison, opens or updates `Stale plan-lock: <slug>` Issue labelled `plan-lock-stale`. Action does not delete refs. PR [#198](https://github.com/alexandrosk0/Smatchet/pull/198) opened as draft.
- 2026-05-17 · Stack review caught one **CRITICAL** (shell-into-python injection in `lock-staleness.yml` via attacker-controlled `claim.json.started`) and two **MEDIUM** (`git fetch ... \|\| true` swallowing network errors in both `locks-render.yml` and `lock-staleness.yml`). All three fixed in `aa7dd41` (render fail-loud) and `5342180` (staleness injection + fail-loud). Stack rebased onto develop after 5 upstream commits landed; `_plan-locks.md` conflict resolved by inserting `git-ref-plan-locks` entry above the upstream-added shipped entries. All three PRs back to **MERGEABLE**.
- 2026-05-17 · Phase 5 landed locally on `feat/git-ref-plan-locks-phase-5` (stacked on PR #198): `AGENTS.md § Orchestrator delegation packet` migrated from "read `_plan-locks.md`" to "run `bash scripts/dev/locks-show.sh`". Updated wording specifies the claim path (`lock-claim.sh`), scope-growth path (`lock-claim-update.sh`), and the `lock-slug:` PR-body requirement for auto-release. Legacy markdown path stays valid during the transition window. Per-agent `agents/*.md` files contain no references to `_plan-locks` — no per-agent prompt changes needed. PR [#200](https://github.com/alexandrosk0/Smatchet/pull/200) opened as draft.
- 2026-05-17 · Pre-Phase-6 truth audit: confirmed exactly **1** entry needs migration (`git-ref-plan-locks` itself); the earlier "1 claimed" count was a false positive matching protocol prose at `_plan-locks.md:46`. Two most-recent shipped PRs (#196, #197) verified merged on GitHub. One orphan feature branch on origin (`feat/h12-l16-m13-bundle`) unrelated to plan-locks. Live `refs/locks/*` greenfield (zero refs).
- 2026-05-17 · Phase 6 landed locally on `feat/git-ref-plan-locks-phase-6` (stacked on PR #200) — **the cutover**. Chose option C from the audit (rename + archive) per user direction. Steps: (1) hand-migrated the lone in-flight entry to `refs/locks/git-ref-plan-locks` via `bash scripts/dev/lock-claim.sh git-ref-plan-locks /tmp/ws-cutover.txt` — claim sha `89c2520`; (2) `git mv docs/design/_plan-locks.md docs/design/_plan-locks-archive.md`; (3) stripped the in-flight section from the archive (only shipped + abandoned entries remain — 42 + 3); (4) replaced the banner with a FROZEN notice pointing readers at `locks-show.sh` + `_plan-locks.generated.md` + plan doc; (5) regenerated `_plan-locks.generated.md` to reflect the new live ref (1 active); (6) `AGENTS.md` updated to drop the "legacy markdown path stays valid" transition clause; (7) PR template extended to distinguish the `lock-slug:` trigger key (final cutover PR only) from `holds-lock:` (informational on stacked-intermediates).
- 2026-05-17 · **Stack-merge to develop.** PR #194 squash-merged at `f703f6d`. Cascade-close of #195 occurred because GitHub auto-closes PRs whose base branch is deleted; recreated as new PR #203 with the same branch rebased onto develop via `git rebase --onto`. Pre-emptively retargeted #198 / #200 / #202 to develop before merging their predecessors to prevent further cascade-closes. Each remaining PR rebased onto develop via `git rebase --onto origin/develop HEAD~N` to skip duplicate (now-squashed) parent commits. Merge order: #194 (`f703f6d`) → #203 (`bc8b460`) → #198 (`60012c3`) → #200 (`eff5483`) → #202 (`21449f8`). `lock-cleanup.yml` fired on all 3 merges post-Phase-3 landing (`completed success` in ~8 s each); only #202 carried the trigger key `lock-slug:` and actually deleted `refs/locks/git-ref-plan-locks`. Post-merge `git ls-remote origin 'refs/locks/*'` returns empty — full lifecycle exercised end-to-end.
- 2026-05-17 · Phase 7a — CODEOWNERS for lock infra. **First real claim via the new flow**: `bash scripts/dev/lock-claim.sh phase-7-codeowners /tmp/ws-phase-7.txt` (`AGENT_ID=orchestrator`, `LOCK_BRANCH=feat/git-ref-plan-locks-phase-7-codeowners`) at sha `c9ce331`. Wrote `.github/CODEOWNERS` mapping the 7 lock primitive scripts + 3 lock workflows + plan doc + archive + build system + `ITrackerClient.h` + `Commands/` + `AGENTS.md` + `CLAUDE.md` to `@alexandrosk0`. Informational until branch protection is reconfigured to require code-owner review; the file documents the project's notion of "sensitive" and gives a single edit point when additional owners arrive.

## Deviations from plan

- **`jq` swapped for a Python helper** (`scripts/dev/_lock-json.py`). `jq` is not installed on the user's Smatchet dev box; Python 3.x is. Plan originally specified `jq` calls inline in each `.sh` script. Pure stdlib Python is more portable across Smatchet's developer environments (MSYS2 UCRT64 + Windows) and removes an external dependency.
- **Python detection probe** added because `python3` on Windows often resolves to the Microsoft Store stub (`exit 49`). Scripts probe candidates `python`, `python3` and accept only those that pass `sys.version_info[0] >= 3`.
- **Test path** moved from `tests/scripts/lock-primitives.test.sh` to `scripts/dev/test-lock-primitives.sh` so the existing `scripts/dev/test-all.sh` auto-discovery glob (`scripts/dev/test-*.sh`) picks it up without needing changes to the runner.
- **Test case 5** (stale-lease hijack) required `git fetch origin '+refs/locks/*:refs/locks/*'` in the second clone before `commit-tree -p <sha>` could reference the ref's tip as a parent. Documented in the test inline.
- **Phase 6 — cutover shape, option C (rename + archive)** rather than the original plan's option A (strip + auto-gen on the same file). Option A would have deleted 42 entries of shipped-slice audit-trail history. Option C preserves history in a frozen `_plan-locks-archive.md` while `_plan-locks.generated.md` is the live mirror. No splice-marker complexity needed.
- **Phase 6 — no migration helper script written**. The original plan budgeted `scripts/dev/locks-migrate-from-markdown.sh` for backfilling N markdown entries to refs. Actual input cardinality at cutover time was N=1 (this plan itself), making a parameterised script dead code. Hand-migrated via direct `lock-claim.sh` invocation. If a future archive restore ever needs to repopulate refs from markdown, the parser logic can be revived from this commit.
- **Phase 6 — `lock-slug:` ↔ `holds-lock:` split** added to the PR template to handle stacked-PR sets that share a single lock. Cleanup workflow regex only matches `lock-slug:` so intermediate PRs in a stack must use `holds-lock:` to avoid premature release on the first intermediate merge. PRs #195, #198, #200 were retroactively updated to the `holds-lock:` form to enforce this invariant on the in-flight stack.

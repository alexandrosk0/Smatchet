# git-ref plan-locks — atomic remote-agent coordination
<!-- plan-date: 2026-05-17 -->

**Status**: shipped (Phases 0-6 + 7a) · **Slug**: `git-ref-plan-locks` · **Owner**: orchestrator · **Created**: 2026-05-17

## Pending operator actions

The plan is fully shipped to develop. Four items below require a human action (UI click or admin command) before the system is in its final configured state. Each links to the section with full procedure.

| # | Action | Status | Trigger | Where the procedure lives |
|---|---|---|---|---|
| 1 | **Create `LOCK_RENDER_PAT` fine-grained PAT + add as repo secret.** Without this the `locks-render.yml` workflow fails at the `gh pr create` step and leaves orphan `bot/plan-locks-sync` branches on origin every cron fire. | **DONE 2026-05-18** — verified via two consecutive `conclusion: success` workflow runs after configuration. | — | § Operational requirements → `LOCK_RENDER_PAT` secret |
| 2 | **Calendar reminder to rotate `LOCK_RENDER_PAT` at +90 days** (the recommended expiry set at creation time). | OPEN — set the reminder once and forget | At PAT expiry. Workflow will start failing again the day after expiry. | § Operational requirements → `LOCK_RENDER_PAT` secret → "Rotation" |
| 3 | **Run `bash agents/scripts/core/setup-locks-ruleset.sh`** to enable Phase 7b ref-namespace hardening. | OPEN — opportunistic | Build when: (a) a `refs/locks/*` ref is mutated by an actor who shouldn't have (audit via reflog / staleness Issue), or (b) the repo gains a second collaborator with `push` access. | § Phase 7b |
| 4 | **Enable GitHub merge queue on develop** (Phase 7d). | **CLOSED (unavailable) 2026-07-14** — the merge queue is an org-only feature (Team/Enterprise), absent from both classic branch protection and rulesets on this user-owned repo; the underlying goal was met another way (`strict=false` on required checks — PRs merge on their own green head, post-merge CI backstop). See `docs/plans/build-quality-velocity-hardening.md` item #14 + its § Deviations. Re-open only if the repo moves under an org. | — | § Phase 7d |

Manual stop-gap available while item 1 is pending: `bash scripts/dev/local/manual-locks-render-sync.sh` (full lifecycle: regen → push → PR → merge → cleanup). Use sparingly; configuring the PAT is the real fix.

Already-shipped items (no further action required): primitive scripts (Phase 1), render workflow (Phase 2), cleanup workflow (Phase 3), staleness workflow + sweep script (Phase 4), agent prompts migrated (Phase 5), markdown cutover to refs (Phase 6), CODEOWNERS (Phase 7a). Phase 7c (cleanup PAT) was deprioritised indefinitely after investigation showed it offered zero benefit over `GITHUB_TOKEN` — see § Phase 7c.

## Problem

`docs/plans/active/_plan-locks.md` is a single markdown file edited by every agent that claims, transitions, or releases a plan-lock. As the number of concurrent remote agents grows, this file becomes a contention point:

- **Race on the file itself.** Two agents editing the markdown concurrently produce merge conflicts on the lock-coordination artefact, not on the work they're trying to coordinate.
- **Non-atomic state transitions.** `claimed → in-flight → shipped` are three separate hand-edits scattered across PRs. Easy to forget the flip; `_plan-locks.md` drifts from reality.
- **Per-flip PR overhead.** Pure status-flip PRs (e.g. PR #187) ship docs-only edits just to keep the file current. Reviewer load + CI cost per flip.
- **No enforcement.** Nothing prevents an agent from writing to a path another slice has claimed. The file is convention-only.
- **Pruning friction.** Shipped entries accumulate; manual sweeps required.

The four orthogonal subproblems (claim / discovery / collision-catch / cleanup) are all served by one markdown file, badly.

## Terminology

The vocabulary in this doc + `AGENTS.md` § Orchestrator delegation packet + `agents/scripts/core/lock-*.sh` uses four terms that look adjacent but are **role-distinct**. Cross-link: [`docs/CONTEXT.md`](../CONTEXT.md) § Plan locks for the glossary form.

| Term | Role | Form on disk / wire |
|---|---|---|
| **Plan-lock** | the **object** — a claim on a planned write set | `refs/locks/<slug>` on origin (one ref per active slice); rendered as a row in `docs/plans/active/_plan-locks.generated.md` |
| **Lock-slug** | the **identifier field** — kebab-case name of the slice | URL-safe ASCII; ref-name suffix; script argument; line key `lock-slug: <slug>` in PR bodies |
| **Lock-claim** | the **action** of acquiring + the **script** that performs it | `bash agents/scripts/core/lock-claim.sh <slug> <write-set-file>`; writes a `claim.json` blob into the ref; fails on ref-update conflict |
| **Holds-lock** | the **predicate** — "agent X holds-lock Y" | derived field on the rendered table from `bash agents/scripts/core/locks-show.sh`; reads `claim.json.owner` |

The four do not overlap. A `plan-lock` is the object; `lock-slug` is its identifier; `lock-claim` is the action that brings one into being; `holds-lock` is the predicate that names its current owner. If a future doc / commit / script needs a new term in this space, pick one of the existing four if it fits, or add a fifth row here.

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

## Operational requirements

One-time repo configuration that must exist for the workflows to function correctly. These are intentionally NOT auto-applied — they require human approval of the trust boundary they cross.

### `LOCK_RENDER_PAT` secret — required by `locks-render.yml`

GitHub disallows the default `GITHUB_TOKEN` from creating or approving pull requests since 2022 (security default). The render workflow needs `pull-requests: write` to open or update the `bot/plan-locks-sync` PR. Two compliant paths exist; we use the fine-grained PAT path so the relaxation surface stays narrow to a single workflow step.

**One-time setup procedure:**

1. Go to **https://github.com/settings/personal-access-tokens** (user-level, not org-level).
2. Click **Generate new token** → **Fine-grained tokens**.
3. **Token name**: `Smatchet locks-render PR creation`.
4. **Expiration**: 90 days. Set a calendar reminder to rotate.
5. **Resource owner**: `alexandrosk0` (the repo owner).
6. **Repository access** → **Only select repositories** → **Smatchet**.
7. **Repository permissions**:
   - **Pull requests**: **Read and write**.
   - Leave every other permission at **No access** (least privilege — the PAT cannot push code, merge PRs, delete refs, edit Issues, etc.; it can only create / read PRs).
8. **Generate token**. Copy the `github_pat_*` string immediately (it is shown only once).
9. Go to **https://github.com/alexandrosk0/Smatchet/settings/secrets/actions** → **New repository secret**.
10. **Name**: `LOCK_RENDER_PAT`. **Value**: paste the token. Save.

The workflow auto-detects the secret. Without it, the `Open or update sync PR` step logs a warning and falls back to `GITHUB_TOKEN` (which then fails loudly with the GraphQL error — same failure mode the user would have hit before this section existed, just now with an actionable warning).

**Rotation**: at PAT expiry, generate a new token under the same name and overwrite the secret value. The workflow picks up the new value on its next fire.

**Why not the repo-wide "Allow GitHub Actions to create and approve pull requests" checkbox?** That checkbox flips the default for every workflow with `pull-requests: write`. Smatchet has only one such workflow today (`locks-render.yml`), but the PAT approach future-proofs against accidentally widening the trust surface when a future workflow gets the same permission.

### Branch protection on `develop` — left as-is

The plan-locks system does not require changes to develop's existing branch protection. Phase 7d (merge queue) would add a separate gate but is opportunistic.

### Public-repo metadata exposure — known, accepted

`refs/locks/*` claim blobs are world-readable on a public repo. Contents are slug + owner + branch + write set + timestamps. No secrets. Do not embed sensitive context (API keys, internal URLs, embargoed feature names) in claim payloads. Documented at Phase 0 results.

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

> **Path note (relocated post-#1274):** the Phase-1 primitives below shipped under `scripts/dev/` but now live at `agents/scripts/core/` (the lock infra moved with PR #1274). The `scripts/dev/…` paths in this Phases-1–6 inventory are the as-built historical record, **not** live refs — to invoke a script today, read it from `agents/scripts/core/`.

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

- `docs/plans/active/_plan-locks.md` — banner inserted at the top: *"Auto-generated from `refs/locks/*` — do not hand-edit below the line."* Hand-edited content below the line will be lost on next regeneration (intentional after Phase 6 cutover; before cutover, banner is informational only).

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

- `AGENTS.md` § Orchestrator delegation packet — replace the standard claim wording. From: *"Read `docs/plans/active/_plan-locks.md` first. Refuse if your write set overlaps an `in-flight` or `claimed` entry; surface to the orchestrator."* To: *"Run `bash scripts/dev/locks-show.sh` first (live ref state). The auto-generated `docs/plans/active/_plan-locks.md` is a snapshot view that may lag by minutes. Claim via `bash scripts/dev/lock-claim.sh <slug> <write-set-file>` before the first edit. Refuse if your write set overlaps any active claim; surface to the orchestrator. On scope growth, run `lock-claim-update.sh`. The PR body must include the line `lock-slug: <slug>` so the cleanup action releases on merge."*
- Per-agent prompt files under `agents/*.md` that reference `_plan-locks` — grep + update each. Inventory taken in Phase 0.

Acceptance: dispatch one routine slice end-to-end through the new flow. Verify claim → work → push → PR-merge → ref deleted → markdown view regenerated. Zero hand-edits to `_plan-locks.md` during the slice.

PR title: `docs(agents): migrate plan-lock prompts to git-ref primitive (phase 5)`

### Phase 6 — Backfill + cutover

Convert existing `_plan-locks.md` claims to refs without disrupting active work.

Files (new):

- `scripts/dev/locks-migrate-from-markdown.sh` — one-shot. Parses current `_plan-locks.md`, calls `lock-claim.sh` for every `in-flight | claimed` entry. Skips `shipped | abandoned`.

Files (modified):

- `docs/plans/active/_plan-locks.md` — strip all hand-edited content below the banner. From this PR forward, the file is fully auto-generated; future edits flow through refs only.

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

#### 7b — GitHub Ruleset restricting `refs/locks/*` (script shipped 2026-05-18, run-when-needed)

Build when: any of (a) `refs/locks/*` ref is mutated by an actor who shouldn't have (caught via reflog audit or anomalous staleness-sweep Issue), (b) the repo gains a second collaborator with `push` access, (c) a security audit demands defence-in-depth beyond convention.

**Script**: `bash agents/scripts/core/setup-locks-ruleset.sh`. Idempotent — creates the ruleset on first run, updates in place on subsequent runs (look-up by name `plan-locks`).

**What the ruleset enforces**:
- Target ref pattern `refs/locks/*`.
- Restrict creations + updates + deletions to bypass-list actors only.
- Bypass list: `RepositoryRole/admin` (the repo owner) + `Integration/github-actions` (so `lock-cleanup.yml` can still delete refs via the default `github-actions[bot]` identity on PR-merge).

**Trade-off**: contributors with plain push access can no longer claim a plan-lock from their own clone via `lock-claim.sh`. Acceptable for "hard to bypass" stance; not acceptable if the workflow expects every contributor to self-serve claim. Single-owner repos today don't hit this trade-off.

**Best-effort first-cut caveat**: the script uses the `target: branch` ruleset shape with a `refs/locks/*` ref-name condition. GitHub's docs nominally support arbitrary ref patterns this way but the live API behaviour for non-`refs/heads` / non-`refs/tags` targets has edge cases. Run the script + immediately attempt a `lock-claim.sh tmp-ruleset-test` from a non-bypass actor to verify the rule actually fires. If it doesn't, fall back to the UI path: Settings → Rules → Rulesets → New ruleset.

**Rollback**: `gh api -X DELETE repos/$REPO/rulesets/<id>` where `<id>` comes from `gh api repos/$REPO/rulesets`.

#### 7c — Fine-grained PAT for cleanup action (DEPRIORITIZED 2026-05-18)

Originally planned to narrow `lock-cleanup.yml`'s `GITHUB_TOKEN`-wide `contents: write` scope. Investigation showed **GitHub fine-grained PATs cannot scope `contents: write` to a ref namespace** (no `refs/locks/*`-only permission exists; `contents` is whole-repo or nothing). A PAT for the cleanup workflow would therefore have the same trust surface as the default token, with extra rotation cost and zero benefit.

The right tool for narrowing trust at the `refs/locks/*` level is **Phase 7b ruleset** (above), which restricts the operation at the server side regardless of which token attempts it.

**Status**: deprioritized indefinitely. Reopen only if GitHub introduces ref-namespace-scoped content permissions in the fine-grained PAT model.

**Aside**: Phase 7a's `LOCK_RENDER_PAT` is a different story — that PAT exists to bypass GitHub's default-deny on PR creation, not to narrow `contents` scope. PR-creation permission is a distinct grant the PAT model does support cleanly.

#### 7d — GitHub merge queue on develop (recipe only)

Build when: a "merged green but broke develop" incident happens, OR PR throughput climbs past ~5 PRs / day.

**Recipe** (UI path): Settings → Branches → develop → "Require merge queue". Configure required status checks list (`build-and-test`, `coverage-gate`, plus any others). Merge queue auto-rebases each PR against develop tip + re-runs required checks before final merge. PR throughput trade-off: queue is single-threaded per branch; slow CI degrades throughput.

**Recipe** (API path):
```bash
gh api -X PATCH repos/alexandrosk0/Smatchet/branches/develop/protection \
  -F required_status_checks.strict=true \
  -F 'required_status_checks.contexts[]=build-and-test' \
  -F 'required_status_checks.contexts[]=coverage-gate' \
  -F required_linear_history=true \
  -F enforce_admins=false \
  -f restrictions= \
  -f required_pull_request_reviews.dismiss_stale_reviews=true
gh api -X PUT repos/alexandrosk0/Smatchet/rulesets \
  --input <<<'{
    "name": "develop-merge-queue",
    "target": "branch",
    "enforcement": "active",
    "conditions": {"ref_name": {"include": ["refs/heads/develop"], "exclude": []}},
    "rules": [{"type": "merge_queue", "parameters": {"merge_method": "SQUASH"}}]
  }'
```

(API shape may need adjusting per GitHub Actions API drift — verify the response.)

## Verification

Filled in during each phase.

- Phase 0: results section appended below.
- Phase 1: `bash scripts/dev/test-lock-primitives.sh` green (8/8). Also auto-discovered by `bash scripts/dev/test-all.sh --filter lock-primitives`.
- Phase 2: local `bash scripts/dev/locks-render-markdown.sh` renders empty-state stub and the populated state of a temporary `tmp-render-smoke` claim correctly. Initial `docs/plans/active/_plan-locks.generated.md` committed. Workflow `locks-render.yml` end-to-end run deferred to first dispatch post-merge — `act` simulation skipped due to GitHub-specific `gh` + permissions context.
- Phase 3: workflow YAML lints clean under GitHub Actions parser conventions (manual review of the `gh api`-based delete path). End-to-end run deferred to first real merged PR carrying a `lock-slug:` line — exercised at Phase 6 cutover at the latest. Dry-run alternative: open a draft PR with a synthetic `lock-slug: tmp-cleanup-smoke` line + a corresponding `refs/locks/tmp-cleanup-smoke` ref, then close-and-merge the draft.
- Phase 4: workflow YAML lints clean under GitHub Actions parser conventions (manual review). Empty-state path (no refs, no Issues) covered by the `refs` variable early-return. End-to-end run deferred to first scheduled cron post-merge or a manual `workflow_dispatch --field threshold_days=0` invocation against a known stale-looking ref.
- Phase 5: `grep -rln "plan-lock\\|_plan-locks" agents/` returns no matches — no per-agent prompt changes needed. `AGENTS.md § Orchestrator delegation packet` updated; new wording instructs agents to use `bash scripts/dev/locks-show.sh` for live state. Smoke-tested locally: `bash scripts/dev/locks-show.sh` returns table view of live `refs/locks/*` on origin (currently empty). End-to-end verification of the new workflow happens organically when the next slice is dispatched.
- Phase 6: pre-flight inventory showed exactly 1 entry needing migration. `bash scripts/dev/lock-claim.sh git-ref-plan-locks /tmp/ws-cutover.txt` claimed `refs/locks/git-ref-plan-locks` on origin at sha `89c2520`. `bash scripts/dev/locks-show.sh` returned a table row for the new claim. `bash scripts/dev/locks-render-markdown.sh > docs/plans/active/_plan-locks.generated.md` emitted a 1-lock view (16 paths in write set). `git mv` of the markdown file preserved git history. Archive's strip surgery left exactly 42 shipped + 3 abandoned entries (zero in-flight). `bash scripts/dev/test-lock-primitives.sh` → `Passed: 8 Failed: 0`. `lock-cleanup.yml` will fire when this PR merges, deleting the ref; subsequent `locks-render.yml` cron will regenerate `_plan-locks.generated.md` to its empty-state stub.
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
- 2026-05-17 · `84073bc` · post-Phase-1 audit: hostile-takeover risk row added to risk register; dogfood plan-lock entry added to `docs/plans/active/_plan-locks.md`.
- 2026-05-17 · Phase 2 landed locally on `feat/git-ref-plan-locks`: render script + workflow + banner + initial generated file. Live render against `origin` PASS (empty state + populated state via temporary `tmp-render-smoke` claim). Workflow not yet smoke-tested end-to-end — first dispatch happens after the branch lands. PR [#194](https://github.com/alexandrosk0/Smatchet/pull/194) opened as draft.
- 2026-05-17 · Phase 3 landed locally on `feat/git-ref-plan-locks-phase-3` (stacked on PR #194): cleanup workflow `.github/workflows/lock-cleanup.yml` parses `lock-slug:` line from PR body on merge and deletes the corresponding ref via `gh api DELETE`. PR template appended with optional `lock-slug:` instruction. PR [#195](https://github.com/alexandrosk0/Smatchet/pull/195) opened as draft. End-to-end run deferred to first real PR-merge post-merge of this branch.
- 2026-05-17 · Phase 4 landed locally on `feat/git-ref-plan-locks-phase-4` (stacked on PR #195): staleness sweep workflow `.github/workflows/lock-staleness.yml`. Daily cron + `workflow_dispatch` (with `threshold_days` input). Reads `claim.started` / `claim.updated`, uses `max(started, updated)` for age comparison, opens or updates `Stale plan-lock: <slug>` Issue labelled `plan-lock-stale`. Action does not delete refs. PR [#198](https://github.com/alexandrosk0/Smatchet/pull/198) opened as draft.
- 2026-05-17 · Stack review caught one **CRITICAL** (shell-into-python injection in `lock-staleness.yml` via attacker-controlled `claim.json.started`) and two **MEDIUM** (`git fetch ... \|\| true` swallowing network errors in both `locks-render.yml` and `lock-staleness.yml`). All three fixed in `aa7dd41` (render fail-loud) and `5342180` (staleness injection + fail-loud). Stack rebased onto develop after 5 upstream commits landed; `_plan-locks.md` conflict resolved by inserting `git-ref-plan-locks` entry above the upstream-added shipped entries. All three PRs back to **MERGEABLE**.
- 2026-05-17 · Phase 5 landed locally on `feat/git-ref-plan-locks-phase-5` (stacked on PR #198): `AGENTS.md § Orchestrator delegation packet` migrated from "read `_plan-locks.md`" to "run `bash scripts/dev/locks-show.sh`". Updated wording specifies the claim path (`lock-claim.sh`), scope-growth path (`lock-claim-update.sh`), and the `lock-slug:` PR-body requirement for auto-release. Legacy markdown path stays valid during the transition window. Per-agent `agents/*.md` files contain no references to `_plan-locks` — no per-agent prompt changes needed. PR [#200](https://github.com/alexandrosk0/Smatchet/pull/200) opened as draft.
- 2026-05-17 · Pre-Phase-6 truth audit: confirmed exactly **1** entry needs migration (`git-ref-plan-locks` itself); the earlier "1 claimed" count was a false positive matching protocol prose at `_plan-locks.md:46`. Two most-recent shipped PRs (#196, #197) verified merged on GitHub. One orphan feature branch on origin (`feat/h12-l16-m13-bundle`) unrelated to plan-locks. Live `refs/locks/*` greenfield (zero refs).
- 2026-05-17 · Phase 6 landed locally on `feat/git-ref-plan-locks-phase-6` (stacked on PR #200) — **the cutover**. Chose option C from the audit (rename + archive) per user direction. Steps: (1) hand-migrated the lone in-flight entry to `refs/locks/git-ref-plan-locks` via `bash scripts/dev/lock-claim.sh git-ref-plan-locks /tmp/ws-cutover.txt` — claim sha `89c2520`; (2) `git mv docs/plans/active/_plan-locks.md docs/plans/active/_plan-locks-archive.md`; (3) stripped the in-flight section from the archive (only shipped + abandoned entries remain — 42 + 3); (4) replaced the banner with a FROZEN notice pointing readers at `locks-show.sh` + `_plan-locks.generated.md` + plan doc; (5) regenerated `_plan-locks.generated.md` to reflect the new live ref (1 active); (6) `AGENTS.md` updated to drop the "legacy markdown path stays valid" transition clause; (7) PR template extended to distinguish the `lock-slug:` trigger key (final cutover PR only) from `holds-lock:` (informational on stacked-intermediates).
- 2026-05-17 · **Stack-merge to develop.** PR #194 squash-merged at `f703f6d`. Cascade-close of #195 occurred because GitHub auto-closes PRs whose base branch is deleted; recreated as new PR #203 with the same branch rebased onto develop via `git rebase --onto`. Pre-emptively retargeted #198 / #200 / #202 to develop before merging their predecessors to prevent further cascade-closes. Each remaining PR rebased onto develop via `git rebase --onto origin/develop HEAD~N` to skip duplicate (now-squashed) parent commits. Merge order: #194 (`f703f6d`) → #203 (`bc8b460`) → #198 (`60012c3`) → #200 (`eff5483`) → #202 (`21449f8`). `lock-cleanup.yml` fired on all 3 merges post-Phase-3 landing (`completed success` in ~8 s each); only #202 carried the trigger key `lock-slug:` and actually deleted `refs/locks/git-ref-plan-locks`. Post-merge `git ls-remote origin 'refs/locks/*'` returns empty — full lifecycle exercised end-to-end.
- 2026-05-17 · Phase 7a — CODEOWNERS for lock infra. **First real claim via the new flow**: `bash scripts/dev/lock-claim.sh phase-7-codeowners /tmp/ws-phase-7.txt` (`AGENT_ID=orchestrator`, `LOCK_BRANCH=feat/git-ref-plan-locks-phase-7-codeowners`) at sha `c9ce331`. Wrote `.github/CODEOWNERS` mapping the 7 lock primitive scripts + 3 lock workflows + plan doc + archive + build system + `ITrackerClient.h` + `Commands/` + `AGENTS.md` + `CLAUDE.md` to `@alexandrosk0`. Informational until branch protection is reconfigured to require code-owner review. PR #204 merged at `a390c2b`; cleanup workflow auto-deleted the ref in 8 s.
- 2026-05-17 · **Hotfix `fix/lock-staleness-yaml-parse`** — post-merge audit showed `.github/workflows/lock-staleness.yml` had been silently failing every fire since Phase 4 landed. Root cause: the inline multi-line Python heredocs at column 1 inside a `run: |` block broke YAML literal block-scalar parsing (block scalar requires every content line ≥ block indent; the Python source was un-indented at column 1, so YAML treated those lines as top-level YAML and the workflow file failed parse). Symptom on GitHub: every run labelled by file path instead of `name:` field, empty `jobs[]`, conclusion `failure`. Fix: lifted all bash + python logic into `scripts/dev/lock-staleness-sweep.sh`, leaving the YAML as a thin invoker. Added two new subcommands to `scripts/dev/_lock-json.py`: `latest-ts` (max of started + updated) and `iso-to-epoch` (env-var passthrough, preserves the Phase 4 security fix against shell-into-python interpolation). Verified locally: all three workflow YAMLs parse clean; `latest-ts` + `iso-to-epoch` + RCE-attempt smoke all behave correctly; `test-lock-primitives.sh` still 8/8 green.
- 2026-05-18 · **Hotfix `fix/locks-render-pat`** — first live `locks-render.yml` fire on develop failed at the `gh pr create` step with `GraphQL: GitHub Actions is not permitted to create or approve pull requests`. Root cause: GitHub disallows the default `GITHUB_TOKEN` from creating PRs since 2022 (security default). Fix: split the `Commit, push, open or update PR` step into two — `Commit + force-push sync branch` (uses `GITHUB_TOKEN` for `contents: write`) and `Open or update sync PR` (uses `LOCK_RENDER_PAT` fine-grained PAT for `pull-requests: write`). Workflow gracefully falls back with a `::warning::` log when the PAT secret is unset. Added `## Operational requirements` section with the one-time PAT setup procedure (least-privilege, 90-day rotation, repo-scoped). Cleaned up the orphan `bot/plan-locks-sync` branch left on origin by the failed run. PR #236 merged at `5bfc1d5`; end-to-end exercise of the fix happens on the next render-cron after the PAT secret is configured.
- 2026-05-18 · **Backlog batch `chore/locks-backlog-batch`** — closes three remaining items. (1) `_lock-json.py` `_utc_now_z` switched from deprecated `datetime.datetime.utcnow()` to `datetime.datetime.now(datetime.timezone.utc)` — preserves the literal `Z` suffix via `strftime`. (2) `_lock-json.py` `format_table` rewritten to auto-size columns from the widest value in the data; no more 32-char SLUG truncation when a slug approaches the 64-char schema max. (3) `scripts/dev/setup-locks-ruleset.sh` ships Phase 7b as a one-shot idempotent script (creates or updates a `plan-locks` ruleset targeting `refs/locks/*`, bypass list = repo admins + `github-actions[bot]` Integration). Plan doc Phase 7 section rewritten: 7b ships as script + caveat about live-API edge cases for non-standard ref targets; 7c (cleanup PAT) deprioritised indefinitely (PAT model can't scope `contents` to a ref namespace — same trust surface as `GITHUB_TOKEN`, zero benefit); 7d (merge queue) gets both UI and API recipe paths.
- 2026-05-18 · **Operator-actions index `chore/operator-actions-summary`** — added a single `## Pending operator actions` table near the top of the plan doc, indexing the four items still requiring a human action (PAT setup, rotation reminder, ruleset script, merge queue) with explicit trigger conditions + links to the existing detail sections. Status header bumped from "draft (Phase 0 pending)" to "shipped (Phases 0-6 + 7a)". PR #243.
- 2026-05-18 · **LOCK_RENDER_PAT configured by operator** — first two `locks-render.yml` runs post-config both `conclusion: success`. Workflow opened sync PR #245 (`1 file changed, 1 insertion, 1 deletion`). PAT path verified end-to-end. Phase 7a now fully operational.
- 2026-05-18 · **Hotfix `fix/locks-render-stable-output`** — PR #245 was timestamp-only diff revealing a design oversight: `locks-render-markdown.sh` embedded `**Snapshot taken**: <now()>` in every regen. Every 30-min cron fire produced a new file content even with unchanged `refs/locks/*`, opening a fresh sync PR forever (noise). Fix: dropped the snapshot timestamp entirely; replaced with a "When was this last regenerated?" note pointing readers at `git log -1 --format=%cI docs/plans/active/_plan-locks.generated.md` (the canonical signal). Regenerated `_plan-locks.generated.md` in this PR to match the post-merge empty-state shape so the next cron fire produces no diff (no follow-up sync PR opens). PR #245 closed as superseded.

## Deviations from plan

- **`jq` swapped for a Python helper** (`scripts/dev/_lock-json.py`). `jq` is not installed on the user's Smatchet dev box; Python 3.x is. Plan originally specified `jq` calls inline in each `.sh` script. Pure stdlib Python is more portable across Smatchet's developer environments (MSYS2 UCRT64 + Windows) and removes an external dependency.
- **Python detection probe** added because `python3` on Windows often resolves to the Microsoft Store stub (`exit 49`). Scripts probe candidates `python`, `python3` and accept only those that pass `sys.version_info[0] >= 3`.
- **Test path** moved from `tests/scripts/lock-primitives.test.sh` to `scripts/dev/test-lock-primitives.sh` so the existing `scripts/dev/test-all.sh` auto-discovery glob (`scripts/dev/test-*.sh`) picks it up without needing changes to the runner.
- **Test case 5** (stale-lease hijack) required `git fetch origin '+refs/locks/*:refs/locks/*'` in the second clone before `commit-tree -p <sha>` could reference the ref's tip as a parent. Documented in the test inline.
- **Phase 6 — cutover shape, option C (rename + archive)** rather than the original plan's option A (strip + auto-gen on the same file). Option A would have deleted 42 entries of shipped-slice audit-trail history. Option C preserves history in a frozen `_plan-locks-archive.md` while `_plan-locks.generated.md` is the live mirror. No splice-marker complexity needed.
- **Phase 6 — no migration helper script written**. The original plan budgeted `scripts/dev/locks-migrate-from-markdown.sh` for backfilling N markdown entries to refs. Actual input cardinality at cutover time was N=1 (this plan itself), making a parameterised script dead code. Hand-migrated via direct `lock-claim.sh` invocation. If a future archive restore ever needs to repopulate refs from markdown, the parser logic can be revived from this commit.
- **Phase 6 — `lock-slug:` ↔ `holds-lock:` split** added to the PR template to handle stacked-PR sets that share a single lock. Cleanup workflow regex only matches `lock-slug:` so intermediate PRs in a stack must use `holds-lock:` to avoid premature release on the first intermediate merge. PRs #195, #198, #200 were retroactively updated to the `holds-lock:` form to enforce this invariant on the in-flight stack.

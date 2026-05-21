# Plan — Perforce as local agent VCS (dual with git)

> **Slug**: `git-to-perforce-migration` (file's basename; retained for ref stability — see § Naming).
> **Status**: draft (locked decisions, unimplemented).
> **Originating prompt**: 2026-05-15 "transition the project from git to perforce", scope-clarified 2026-05-21 to dual-VCS rather than migration.

## Context

Smatchet's agentic flows lean heavily on git primitives — worktrees for parallel-agent isolation, branches as task units, refs as plan-locks, PR + GitHub Actions as the ship-line. Git is good at the ship-line side but weak at the local-WIP side: stashes are local-only, no exclusive file lock, no atomic counter, no server-side shelving across machines.

Goal: **add Perforce as a parallel local source-control layer for agentic workflows; keep git/GitHub as the canonical remote and PR-to-GitHub as the ship-line.** Existing single-git workflows continue to work unchanged for any agent / session that opts out. Perforce primitives — named server-side shelves, exclusive locks (`+l`), pending CLs as agent task units, atomic counters as plan-locks, stream depots as the isolation primitive replacing git worktrees — become opt-in tools for sessions that want them.

One-sentence outcome: after this lands, an orchestrator can spawn parallel subagents into isolated p4 task streams instead of git worktrees, ship each subagent's output as a GitHub PR via the existing flow, and never lose git/GitHub access for sessions that prefer the plain workflow.

## Approach

Two physical layouts, one logical project:

1. **Canonical tree at `C:\Dev\Smatchet`** is both a `.git/` workspace (branch `develop`) and a Perforce client root on stream `//smatchet/main`. Both `.gitignore` + `.p4ignore` exclude the other VCS's metadata. The Edit tool writes files; both `git status` and `p4 reconcile -n` can see the change, neither auto-opens. The agent decides which verb to use at submit time.
2. **Task streams replace git worktrees for parallel agent isolation.** Each spawned subagent gets a child stream `//smatchet/tasks/<agent-id>` mapped to its own physical folder under `.claude/streams/<agent-id>/`. The agent works there using p4 verbs only; on completion, the orchestrator integrates the task stream back to `//smatchet/main`, then runs `git diff` reconciliation at the canonical root, commits to a git branch, pushes, and opens a draft PR — the same `agent/<id>/<slug>` branch shape the orchestrator already uses for git-worktree-isolated subagents (so AGENTS.md § Force-push carve-out + `smatchet-merge-watcher` registration both apply unchanged).

Trade-off the choice forces: **the two-VCS model adds reconciliation steps at the canonical root** (`p4 reconcile -n` + `git status` should agree). Mitigated by a `pre-push` hook that fails if `p4 reconcile -n` reports unsubmitted p4-side changes that aren't in the git index. The alternative was a sync-bridge (git-p4) topology, rejected for opacity — agents can't reason cleanly about state that's mediated by a daemon.

Both VCS layers are **opt-in per session** via `SMATCHET_AGENT_VCS=git|p4` (default `git`). An agent that never sets the env continues to behave exactly as today; agents that set `p4` get streams + shelves + locks.

## Files to modify

Numbered list. Per-file rationale.

### New files
1. `docs/perforce/SETUP.md` — install Helix Core, configure client, typemap, ignore, charset.
2. `docs/perforce/AGENT_FLOWS.md` — when to use p4 primitives vs git; task-stream lifecycle; lock discipline.
3. `docs/perforce/RUNBOOK.md` — daily checkpoint cron, shelve GC, stream pruning, recovery.
4. `.p4ignore` (at repo root) — translation of `.gitignore` with p4 syntax differences (Phase 1).
5. `scripts/dev/p4-task-stream.sh` — allocate `//smatchet/tasks/<agent-id>` + populate folder.
6. `scripts/dev/p4-task-stream-gc.sh` — purge stale task streams older than N days.
7. `scripts/dev/p4-reconcile-check.sh` — pre-push hook helper; fails when p4 has open work not represented in git index.
8. `scripts/dev/lock-claim-p4.sh` — mirror of `lock-claim.sh` using `p4 counter -i` atomics.
9. `scripts/dev/lock-release-p4.sh` — mirror of `lock-release.sh` for p4-counter backend.
10. `agents/p4-janitor.md` — companion to `agents/git-janitor.md` for the p4 side (shelve GC, stream prune, `p4 verify`).

### Modified files
11. `scripts/dev/lock-claim.sh:1` — read `SMATCHET_LOCK_BACKEND` env; dispatch to `lock-claim-p4.sh` when `=p4-counter`.
12. `scripts/dev/lock-claim-update.sh:1` — same backend switch on the renew path.
13. `scripts/dev/_lock-json.py:1` — same backend switch on the read path.
14. `scripts/git-hooks/pre-push:1` — extend with `p4 reconcile -n` check when client is configured.
15. `scripts/setup-harness.sh:1` — detect `p4` on PATH + a configured `P4PORT`; warn (don't fail) if absent and `SMATCHET_AGENT_VCS=p4`.
16. `docs/harness/claude-code/hooks/` — add optional `pretool-edit-p4-lock-check.sh` template (gets junctioned into `.claude/hooks/` by `setup-harness.sh`). `.claude/` itself is gitignored — never committed.
17. `AGENTS.md:?` — new short section § Dual-VCS topology (p4 opt-in primitives) cross-linking the new docs. Existing git-centric sections stay verbatim.
18. `docs/CONTEXT.md:?` — add glossary entries: `task stream`, `shelf`, `pending CL`, `p4 counter` under a new § Source control section.
19. `docs/dev/offline-builds.md:127` — update cross-reference (this plan no longer "supersedes" the doc).
20. `agents/git-janitor.md` — add cross-link to `agents/p4-janitor.md` (no behavior change; git path unchanged).

`.gitignore` needs no change — `.claude/streams/` is already covered by the existing wholesale `.claude/` ignore at `.gitignore:63`.
`CMakeLists.txt` needs no change — FetchContent stays; git is canonical.

### Out of scope, explicitly **not** touched
- Existing `.github/` workflows (11 of them) — git/GitHub remains the ship-line, all CI continues to fire on PRs.
- Merge-gates poller (`scripts/dev/merge-gates.sh`) — operates on GitHub PRs, unchanged.
- `smatchet-merge-watcher` host daemon (`docs/design/smatchet-merge-watcher.md`) — out-of-band CI/CodeRabbit polling, GitHub-PR-shaped, unchanged.
- FetchContent vendoring — git stays, no need to vendor.
- Existing `refs/locks/*` plan-locks — keep working; p4 counter is purely additive.
- AGENTS.md merge-gates, plan-locks, force-push carve-out — all unchanged.

## Existing utilities reused

- `scripts/dev/lock-claim.sh:1` — extend with backend switch, don't rewrite.
- `scripts/dev/_lock-json.py:1` — read path gains a backend switch.
- `scripts/git-hooks/pre-push:1` — appended to, not replaced.
- `MainThreadDispatcher::Post(...)` (`Source_Core/include/MainThreadDispatcher.h`) — no use here; cited for completeness because any future Smatchet-side p4 status UI would route through it.
- `Source_Core/src/P4Blame.cpp` — Smatchet already speaks the `p4` CLI for blame analysis. Confirms the `p4` binary is a supported dependency on dev boxes; no new external dep.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: no impact — this plan is dev-tooling only; no `Source_Core/` runtime code path changes.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no impact — `p4` invocations are out-of-process tooling run by orchestrator scripts, never from the Smatchet UI thread.
- **Pillar 3 (never crash)**: no impact — no new C++ code in Smatchet's runtime.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: N/A — no UI surface.

## Perf-review-system gates

N/A — diff does not touch `Source_Core/`. Plan changes are limited to scripts, agent docs, `.gitignore` / `.p4ignore`, AGENTS.md, and new files under `docs/perforce/`. No PR-fast scenario fires; no Pillar 2 static scanner trigger; no dispatcher drain; no new bucket-E surface; no `SMATCHET_UI_PERF_SCOPE` markers added.

## Phases

Implementation breaks into seven ordered phases. Each phase ships independently; each is reversible (no destructive cutover — git stays working throughout).

### Phase 0 — Helix Core bring-up
1. Install `p4d` LTS (e.g. `r24.2`) on the developer Windows host.
2. Install `p4` CLI + P4V on the dev machine. `p4 info` reaches `ssl:localhost:1666`.
3. Create one user matching the git author (`alexkonstantonis@gmail.com`).
4. Server config: `unicode=1`, `case-sensitive=2`, `P4CHARSET=utf8`.
5. Configure `p4 typemap`: `binary+w *.png *.jpg *.dll *.exe *.lib`, `text+x *.sh *.py`, `unicode *.json *.md`, `text+w *.cpp *.h *.cmake`. Critically `.git/...` excluded entirely.
6. Daily checkpoint via Windows Scheduled Task. Offsite checkpoint copy to OneDrive / NAS / cloud bucket — recommended but **not blocking** because the depot is non-canonical (git remains the truth).

Exit: `p4 info` succeeds; a throwaway test CL submits and re-syncs.

### Phase 1 — Dual-VCS canonical tree
1. Create a stream depot (NOT classic) at `//smatchet`, with mainline `//smatchet/main` rooted at `C:\Dev\Smatchet`. Stream depot is required upfront because Phase 2 needs task streams as children.
2. Create client `smatchet_main_<user>`. View: `//smatchet/main/... //smatchet_main_<user>/...`. Root: `C:\Dev\Smatchet`.
3. Author `.p4ignore` at repo root (translation of `.gitignore` — Perforce `!` negation, no `**`, also exclude `.git/`). Note: `.gitignore` itself is **tracked** in git and gets baseline-imported to p4; do not list it in `.p4ignore` (the ignore file only governs untracked candidates for `p4 add` / `p4 reconcile`).
4. `p4 reconcile //smatchet/main/...` opens the whole tree (typemap + `.p4ignore` exclude `.git/`, build output, harness adapters); submit as a single baseline CL `chore(p4): baseline import from git develop@<SHA>`. **Optional** — most teams will accept a baseline-only import without history. Full `git p4` history import is filed as a follow-up plan (slug TBD); not required for the agentic-WIP use case.
5. Verify: a clean working tree shows empty `git status` AND empty `p4 opened`. After the baseline submit, both `.gitignore` and `.p4ignore` are tracked-and-unchanged — `git status` doesn't list them and neither does `p4 reconcile -n` (nothing was modified).

Exit: round-trip — edit a file, `git add` + commit, then `p4 reconcile` + submit; both VCSes see the same file as modified at their respective tips.

### Phase 2 — Task-stream allocator for agent isolation
1. Define stream template `task-stream-template` of type `task` parented to `//smatchet/main`. Inherits all paths read/write; isolated history.
2. New script `scripts/dev/p4-task-stream.sh <agent-id>`:
   - `p4 stream -t task -P //smatchet/main //smatchet/tasks/<agent-id>`
   - `p4 client -t task-client-template task_<agent-id>` with root `C:\Dev\Smatchet\.claude\streams\<agent-id>`
   - `p4 sync //smatchet/tasks/<agent-id>/...@head`
3. `.gitignore` add `.claude/streams/` (sibling of existing `.claude/worktrees/`).
4. New script `scripts/dev/p4-task-stream-gc.sh --older-than-days N`:
   - `p4 streams //smatchet/tasks/...` → filter by mtime → `p4 stream -d` after confirming no pending CLs.
5. Wire into orchestrator worktree allocation: when `SMATCHET_AGENT_VCS=p4` is set, the orchestrator's per-subagent isolation step calls `p4-task-stream.sh` to produce `.claude/streams/<agent-id>/` instead of running `git worktree add .claude/worktrees/<agent-id> ...`. The subagent's working directory is the p4 task-stream root; nothing else about subagent invocation changes.

Exit: spawn a stub child with `SMATCHET_AGENT_VCS=p4`, observe the child receives a populated p4 task stream; on exit the GC purges it.

### Phase 3 — Submit-to-PR bridge
The piece that closes the loop. A subagent finishes work on a task stream; the orchestrator turns that into a GitHub PR.

1. New script `scripts/dev/p4-task-stream-to-pr.sh <agent-id> <pr-title>`:
   - `p4 integrate //smatchet/tasks/<agent-id>/... //smatchet/main/...`
   - `p4 resolve -as` (auto-accept safe) + bail loudly on conflicts (escalate to user).
   - At canonical root: `p4 sync //smatchet/main/...@head`
   - At canonical root: `git checkout -b agent/<agent-id>/<short-slug>` (the `agent/<id>/<slug>` branch shape carries from the orchestrator's pre-existing subagent-branch convention; preserves AGENTS.md § Force-push carve-out for spawned-agent recovery so emergency recovery flows still match).
   - `git add -A` (the synced p4 changes appear as git-modified files because the canonical tree is dual-tracked)
   - `git commit` using the integrated CL's description as the commit-message body
   - `git push -u origin agent/<agent-id>/<short-slug>`
   - `gh pr create --draft --title "<pr-title>" --body "<...>"`
   - Print the PR URL to stdout (caller — orchestrator or `smatchet-merge-watcher` — picks it up; `register <pr>` flows through the watcher per AGENTS.md § Post-ship turn-end protocol).
2. Downstream merge-gates / CodeRabbit / CI workflows fire as they do today; merge-watcher (if registered) drives the PR to green per its own contract.

This is the only **load-bearing** new script — the rest is opt-in tooling. Test coverage via a new bats rig (`tests/bats/p4_submit_to_pr.bats`) mirroring `tests/bats/merge_gates.bats` — `gh api` calls mocked, `p4` calls run against a throwaway local depot.

### Phase 4 — p4-counter plan-locks backend
1. `SMATCHET_LOCK_BACKEND=git-ref|p4-counter`, default `git-ref`.
2. New scripts `lock-claim-p4.sh` + `lock-release-p4.sh` use `p4 counter -i smatchet_lock_<name>` for atomic test-and-set (returns the post-increment value; claim succeeds if previous was `0`).
3. `_lock-json.py` reads either backend; rendering layer unchanged.
4. `docs/design/_plan-locks.generated.md` rendering is backend-agnostic (it shows logical locks, not the storage).
5. Existing 3 GitHub workflows (`lock-cleanup.yml`, `lock-staleness.yml`, `locks-render.yml`) continue to operate on `refs/locks/*` for git-backed locks; do not extend to p4-counter (would need a runner with `P4PORT` access to the local server, not feasible from GitHub-hosted runners).

Exit: switch backend env, claim + release a lock, observe `_plan-locks.generated.md` renders identically.

### Phase 5 — Exclusive-lock discipline (`p4 edit +l`)
Optional Phase. Surface only when the user finds two agents touching the same file without coordination.

1. Document the `p4 edit -t +l <file>` pattern in `docs/perforce/AGENT_FLOWS.md`.
2. Optional `PreToolUse:Edit` hook template at `docs/harness/claude-code/hooks/pretool-edit-p4-lock-check.sh` — `setup-harness.sh` junctions it into `.claude/hooks/` (which is itself gitignored). Hook checks `p4 opened -m1 <file>` and warns if the file is `+l`-locked by another client. Off by default; on under `SMATCHET_AGENT_VCS=p4`.
3. The hook is a warning, not a hard block — agents can ignore for emergency fixes.

Exit: deliberately have two agents try to edit the same `+l`-locked file; one is warned.

### Phase 6 — Agent + AGENTS.md wiring
1. Author `agents/p4-janitor.md` (companion, not replacement, of `git-janitor.md`).
2. AGENTS.md gets a new short section § Dual-VCS topology (≤30 lines) cross-linking the new docs.
3. `docs/CONTEXT.md` glossary additions under new § Source control section.

### Phase 7 — Verification + adoption
1. Round-trip scenario: orchestrator allocates a subagent task stream with `SMATCHET_AGENT_VCS=p4`, subagent works in `.claude/streams/<id>/`, the orchestrator (or user) invokes `p4-task-stream-to-pr.sh`, PR opens against `develop`, registered with `smatchet-merge-watcher`, PR merges, branch + task stream both GC'd.
2. Round-trip scenario: same flow with `SMATCHET_AGENT_VCS=git` (default), behavior bit-identical to today (regression gate — no p4 calls at all).
3. Multi-agent scenario: two subagents in parallel task streams; integrate both back to `//smatchet/main`; auto-resolve where safe; both ship.

## Risks / non-goals

- **Risk: dual-VCS drift at the canonical root** — `git status` and `p4 reconcile -n` should agree about what's modified. Mitigation: pre-push hook in `scripts/git-hooks/pre-push` runs `p4-reconcile-check.sh`; fails the push if p4 has uncommitted file edits the git index doesn't know about. Hook is no-op for sessions without a configured `P4PORT`.
- **Risk: stale task streams sprawl** — each spawned agent allocates one; if cleanup script misses a few, they accumulate. Mitigation: weekly cron via Windows Scheduled Task runs `p4-task-stream-gc.sh --older-than-days 14`.
- **Risk: `p4d` SPOF on dev box (shelves / locks lost)** — accepted. Shelves are non-canonical (only `git push` produces canonical state). Lock state is best-effort coordination, not data. Loss is annoyance, not data loss.
- **Risk: collaborator without `p4` installed** — `SMATCHET_AGENT_VCS=git` (the default) means every agent works without any p4 dependency. p4 is **never** required.
- **Risk: integration conflicts on `p4 integrate`** — surface to user via Phase 3 bail-loud rule. Mitigation: short-lived task streams + frequent integrate keep diffs small.
- **Risk: vexp daemon misbehaves against a dual-VCS workspace** — open question, verify during Phase 1 dry-run. Worst case: vexp loses indexing on streams folders, falls back to text-search per AGENTS.md § Semantic-search exceptions.
- **Risk: `p4 reconcile` over a large tree is slow** — Smatchet's `Source_Core/` + `tests/` + `docs/` aren't enormous; expect sub-second `reconcile -n`. Verify in Phase 1.
- **Non-goal: full git history import to p4.** Out of scope; baseline-only import suffices for the agentic-WIP use case. A history-import plan can come later if there's a reason.
- **Non-goal: Helix Swarm code review.** Out of scope; PRs on GitHub remain the review surface.
- **Non-goal: git-fusion / bidirectional sync.** Explicitly rejected — too opaque.
- **Non-goal: replacing `gh pr create` / CI / merge-gates / coverage-gate / perf-gate / `smatchet-merge-watcher`.** All survive unchanged.

## Verification

- **Bucket A (pure-logic ctest)**: N/A — no C++ added.
- **Bucket E (ImGui Test Engine)**: N/A — no UI surface.
- **Bash-driver scenarios**:
  - `scripts/dev/test-p4-task-stream-roundtrip.sh` (new) — Phase 2 exit gate.
  - `scripts/dev/test-p4-submit-to-pr.sh` (new) — Phase 3 exit gate, uses a fake gh harness (no real PR opened).
  - `scripts/dev/test-p4-lock-claim.sh` (new) — Phase 4 exit gate, asserts `lock-claim-p4.sh` and `lock-claim.sh` produce equivalent `_plan-locks.generated.md` output.
  - `scripts/dev/test-dual-vcs-status-agreement.sh` (new) — modifies one file, asserts both `git status --porcelain` and `p4 reconcile -n` flag it consistently.
- **Build gate**: `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12` — must pass before + after the plan ships (no C++ changes, but the dual-target invariant remains).
- **Manual residue**: Phase 0 (server install) is irreducibly manual. Documented in `docs/perforce/SETUP.md`. Backlog entry `docs/backlog/agent-self-improvement/tooling.md`: "automate p4d bring-up via a `scripts/dev/setup-p4d.ps1` Idempotent installer."

## Out of scope (flagged, not designed)

- **Helix Swarm** — code review on shelved CLs instead of GitHub PRs. Follow-up plan if PR-flow ever becomes a bottleneck.
- **Bidirectional git ↔ p4 sync** — git-fusion or equivalent. Rejected per Approach trade-off.
- **Full git history import to p4** — baseline-only is enough for agentic-WIP. Follow-up plan if archaeology motivates.
- **p4 server on second machine** — local `p4d` is fine for one developer. Promote to dedicated server when a second contributor joins.
- **Vendoring FetchContent into p4 depot** — git stays canonical, FetchContent stays git, no vendor work.
- **Archiving GitHub** — never, GitHub IS the canonical remote.

## Naming

Slug is `git-to-perforce-migration` for historical continuity (this doc was first committed under that name on 2026-05-15 as a full-migration plan). After 2026-05-21 scope-correction the doc describes a dual-VCS opt-in layer, not a migration. Slug retained because external references exist (`docs/dev/offline-builds.md:127`). Future renames are cheap if the slug becomes confusing.

## Implementation log
*(populated post-ship per AGENTS.md § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*

## Superseded — 2026-05-15 full-migration plan

This doc's original framing (2026-05-15, commit `9d36aab`) was a full git → Perforce migration: archive GitHub, vendor every FetchContent dep, retire all git/gh-touching agents + workflows. The 2026-05-21 drift report against develop tip `95d51a5` surfaced that this would have torched five load-bearing systems (plan-locks, merge-gates, agentic-handoff, perf-CI, coverage-CI) without replacements. Drift report commit: `1a3c9109`.

The user clarified that the actual goal was an opt-in **local agentic-WIP layer** with git/GitHub preserved as the ship-line. This rewrite (2026-05-21) replaces the migration framing with the dual-VCS topology above. Original migration plan body preserved in git history at `9d36aab`; do not rebuild it from this doc.

**Post-merge stale-ref cleanup (2026-05-21 evening)**: PR #361 ("agentic ripout doc cleanup") independently removed the `handoff-implementer` + `pr-iterator` agents, `docs/agentic/USAGE.md`, `ClaudeCodeLocalRunner`, the `SEED.json` / `RUN_RESULT.json` sentinel-envelope, and the `stub-claude` test harness. Phases 2, 3, 7 of this plan referenced those primitives by name; they have been re-anchored to (a) the orchestrator's pre-existing per-subagent worktree allocation (which survives), (b) the `smatchet-merge-watcher` host daemon (the replacement out-of-band poll path per `docs/design/smatchet-merge-watcher.md`), and (c) a bats rig modelled on `tests/bats/merge_gates.bats` for Phase 3 coverage in place of the deleted stub harness.

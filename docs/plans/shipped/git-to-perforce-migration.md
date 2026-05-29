# Plan — Perforce as local agent VCS (dual with git)

> **Slug**: `git-to-perforce-migration` (file's basename; retained for ref stability — see § Naming).
> **Status**: in-progress — Phase 0 + Phase 1 implemented (see § Implementation log); Phase 2+ in flight.
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
19. `docs/guides/offline-builds.md:127` — update cross-reference (this plan no longer "supersedes" the doc).
20. `agents/git-janitor.md` — add cross-link to `agents/p4-janitor.md` (no behavior change; git path unchanged).

`.gitignore` needs no change — `.claude/streams/` is already covered by the existing wholesale `.claude/` ignore at `.gitignore:63`.
`CMakeLists.txt` needs no change — FetchContent stays; git is canonical.

### Out of scope, explicitly **not** touched
- Existing `.github/` workflows (11 of them) — git/GitHub remains the ship-line, all CI continues to fire on PRs.
- Merge-gates poller (`scripts/dev/merge-gates.sh`) — operates on GitHub PRs, unchanged.
- `smatchet-merge-watcher` host daemon (`docs/plans/shipped/smatchet-merge-watcher.md`) — out-of-band CI/CodeRabbit polling, GitHub-PR-shaped, unchanged.
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
1. Install `p4d` LTS (e.g. P4D 2025.2 or later) on the developer Windows host. Smatchet ships against the remote-LAN topology authored in PR #373; see [`docs/perforce/SETUP.md`](../perforce/SETUP.md) for the authoritative recipe.
2. Install `p4` CLI + P4V on the dev machine. `p4 info` reaches the configured `P4PORT` (default `localhost:1666` for the original single-host plan; `Brick:1666` or equivalent for the shipped remote-LAN topology).
3. Create one user matching the git author (`alexkonstantonis@gmail.com`).
4. Server config: `case-sensitive` (`-C0`); unicode mode optional — Smatchet's shipped instance skipped it (single-user, ASCII-safe UTF-8; one-way switch). Enable later via `p4d -xi` if a real charset bug surfaces.
5. Configure `p4 typemap` with **explicit `+w` (writeable) modifier on every text type** — critical for the dual-VCS edit-from-either-side contract. Without `+w`, p4 sets Windows ReadOnly on every reconciled file, blocking the next `git`-only edit with `EPERM`. Shipped typemap:
   - `binary+w *.png *.jpg *.dll *.exe *.pdb *.lib *.ttf *.zip`
   - `text+wx *.sh *.py *.bat *.cmd`
   - `text+w *.cpp *.h *.cmake *.lua *.json *.yml *.md *.toml *.graphql`
   `.git/...` excluded entirely.
6. Daily checkpoint via Windows Scheduled Task. Offsite checkpoint copy to OneDrive / NAS / cloud bucket — recommended but **not blocking** (depot is non-canonical; git remains the truth). Operational details + restore recipe in [`docs/perforce/RUNBOOK.md`](../perforce/RUNBOOK.md).

Exit: `p4 info` succeeds; a throwaway test CL submits and re-syncs.

### Phase 1 — Dual-VCS canonical tree
1. Create a stream depot (NOT classic) at `//smatchet`, with mainline `//smatchet/main` rooted at the canonical repo path. Stream depot is required upfront because Phase 2 needs task streams as children.
2. Create client `smatchet_main_<user>`. View: `//smatchet/main/... //smatchet_main_<user>/...`. Root: the user's actual repo path (shipped instance uses `C:\Development\Smatchet`; substitute your own).
3. Author `.p4ignore` at repo root (translation of `.gitignore` — Perforce `!` negation, `**` globstar supported since p4 2014.2, also exclude `.git/`). Note: `.gitignore` itself is **tracked** in git and gets baseline-imported to p4; do not list it in `.p4ignore` (the ignore file only governs untracked candidates for `p4 add` / `p4 reconcile`).
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
4. `docs/plans/active/_plan-locks.generated.md` rendering is backend-agnostic (it shows logical locks, not the storage).
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
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` — must pass before + after the plan ships (no C++ changes, but the dual-target invariant remains).
- **Manual residue**: Phase 0 (server install) is irreducibly manual. Documented in `docs/perforce/SETUP.md`. Backlog entry `docs/backlog/agent-self-improvement/tooling.md`: "automate p4d bring-up via a `scripts/dev/setup-p4d.ps1` Idempotent installer."

## Out of scope (flagged, not designed)

- **Helix Swarm** — code review on shelved CLs instead of GitHub PRs. Follow-up plan if PR-flow ever becomes a bottleneck.
- **Bidirectional git ↔ p4 sync** — git-fusion or equivalent. Rejected per Approach trade-off.
- **Full git history import to p4** — baseline-only is enough for agentic-WIP. Follow-up plan if archaeology motivates.
- **p4 server on second machine** — local `p4d` is fine for one developer. Promote to dedicated server when a second contributor joins.
- **Vendoring FetchContent into p4 depot** — git stays canonical, FetchContent stays git, no vendor work.
- **Archiving GitHub** — never, GitHub IS the canonical remote.

## Naming

Slug is `git-to-perforce-migration` for historical continuity (this doc was first committed under that name on 2026-05-15 as a full-migration plan). After 2026-05-21 scope-correction the doc describes a dual-VCS opt-in layer, not a migration. Slug retained because external references exist (`docs/guides/offline-builds.md:127`). Future renames are cheap if the slug becomes confusing.

## Implementation log

- `61795b8` · Phase 1 Step 3 — `.p4ignore` translation from `.gitignore` (PR #374).
- 2026-05-21 (local, no git commit) · **Phase 0 — server bring-up** on Windows host `MainBot`: Helix Core 2025.2 (`p4d -r C:\depot`) already installed by user; auto-user `alexk` email + full-name updated to `alexkonstantonis@gmail.com` / `Alexandros Konstantonis`; stream depot `//smatchet` (depth 1) created; mainline stream `//smatchet/main` created with `ParentView: inherit`; client `smatchet_main_alexk` rooted at `C:\Development\Smatchet` created; typemap configured (binary+w for png/jpg/dll/exe/pdb/lib/ttf/zip; **text+wx** for sh/py/bat/cmd; **text+w** for cpp/h/cmake/lua/json/yml/md/toml/graphql — `+w` is critical for dual-VCS, see Deviations). Persistent user-env: `P4PORT=localhost:1666`, `P4USER=alexk`, `P4CLIENT=smatchet_main_alexk`, `P4IGNORE=.p4ignore`.
- 2026-05-21 (p4 change `@2`) · **Phase 1 Steps 1-2, 4-5**: `p4 reconcile //smatchet/main/...` opened 820 files; submitted as change @2 ("baseline import from git feat/p4-migration-phase-1-p4ignore@61795b8"). 9 254 445 bytes total. Dry-run pre-flight confirmed `.p4ignore` correctly excludes `.git/`, `build/`, `.claude/`, `.vs/`, `.fetchcontent-src/`. No leakage.
- `9fb61ba` · **Phase 0 — runbook for remote LAN topology** (PR #373, user-authored in parallel). Topology pivoted from single dev-box localhost p4d to remote-Windows-on-LAN at the user's `Brick` machine (which already hosted an unrelated Unreal p4d:1666). `docs/perforce/SETUP.md` documents the wipe-installer-defaults + `p4d -xi` unicode + LAN-only firewall pattern. Supersedes the never-merged single-host runbook draft (PR #377, closed).
- `ae6f004` · **Phase 4 — p4-counter plan-locks backend** (PR #383). Two new scripts (`scripts/dev/lock-claim-p4.sh`, `scripts/dev/lock-release-p4.sh`) using `p4 counter --from=<old> --to=<new>` compare-and-swap atomicity. 4-line `exec`-dispatch added to existing `lock-claim.sh` / `lock-release.sh` so `SMATCHET_LOCK_BACKEND=p4-counter` switches transparently; default stays `git-ref` with zero behavioural change. Metadata stored in sibling counter `smatchet_lock_<slug>_meta` as JSON claim record. 6/6 lifecycle scenarios verified end-to-end against live `p4d`.
- `ba652be` · **Phase 5 — exclusive-lock discipline + `AGENT_FLOWS.md`** (PR #384). `docs/perforce/AGENT_FLOWS.md` (~140 lines, the dual-VCS playbook the plan listed as Phase 0/1 deliverable but never authored) covers task-stream lifecycle, lock discipline, shelf-vs-stash, filetype hygiene, cross-link reconciliation, when-not-to-use-Perforce. Plus `docs/harness/claude-code/hooks/pretool-edit-p4-lock-check.sh` — opt-in `PreToolUse:Edit` hook template that warns (or with `SMATCHET_P4_LOCK_HOOK_BLOCK=1`, blocks) when Edit targets a file held under `+l` by a different client. Off by default; activated via `setup-harness.sh` with `SMATCHET_AGENT_VCS=p4`.

- `2fa3730` · **Phase 2 — task-stream allocator + GC scripts** (PR #380). `scripts/dev/p4-task-stream.sh` allocates `//smatchet/tasks/<id>` from a `task-stream-template` parented to `//smatchet/main` and populates `.claude/streams/<id>/`. `scripts/dev/p4-task-stream-gc.sh --older-than-days N` purges streams with no pending CLs older than the threshold. CR re-review cleared on `4bff831`.
- `f506556` · **Phase 3 — submit-to-PR bridge script** (PR #382). `scripts/dev/p4-task-stream-to-pr.sh <agent-id> <pr-title>` integrates the task stream up to `//smatchet/main`, syncs the canonical tree, creates an `agent/<id>/<short-slug>` git branch with the integrated CL's description as the commit body, pushes, and opens a draft PR via `gh pr create`. CR re-review cleared on `0823ff6`.
- `7f00cbe` · **Phase 6 — agent + AGENTS.md + CONTEXT.md wiring** (PR #388). `agents/p4-janitor.md` companion; `AGENTS.md § Dual-VCS topology` short section cross-linking the new docs; `docs/CONTEXT.md § Source control (dual-VCS)` glossary additions.
- `3395fc2` · **Phase 7 — verification scenario driver** (PR #389). `scripts/dev/test-p4-dual-vcs.sh` covers 9 assertions across 3 scenarios (single-agent p4 round-trip, default-git regression gate, parallel-streams multi-agent).
- 2026-05-22 (Mainbot, in-session work) · **Phase 0 — server-side hardening + checkpoint task** (no git commit — server-side state only, documented here + in `docs/perforce/SETUP.md` PR #404). Three closeouts in one session:
  - **LAN firewall rule added** — `New-NetFirewallRule -DisplayName 'Perforce p4d_smatchet (LAN)' -Direction Inbound -Protocol TCP -LocalPort 1666 -Profile Private,Domain` on Mainbot. The Wi-Fi NIC's `NetworkCategory` was also reclassified `Public` → `Private` so the rule actually applies (runbook step 7 was previously skipped; LAN clients were timing out silently with `WSAETIMEDOUT` on `connect: 192.168.2.23:1666`).
  - **Service renamed `Perforce` → `p4d_smatchet`** via `New-Service … p4s.exe` + `p4 set -S p4d_smatchet P4ROOT=C:\depot P4PORT=1666 P4LOG=… P4JOURNAL=…`. Discovered the runbook's original `sc.exe create binPath=p4d.exe -r …` recipe doesn't work on Helix Core 2025.2 — `p4d.exe` has no Windows SCM dispatcher, so `Start-Service` fails with "Cannot start service." Corrected recipe is `p4s.exe` (the service wrapper) + per-service Perforce env vars. Doc fix shipped in PR #404 (`ada5071`); a Deviations bullet captures the lesson + the Mainbot-specific install state (server root `C:\depot\` vs runbook's `C:\depot-smatchet\`, case-insensitive — locked since first start).
  - **Daily checkpoint scheduled task created** — `p4d_smatchet_checkpoint`, runs `p4d -r C:\depot -jc` at 3 AM as SYSTEM, `StartWhenAvailable: true`. Closes the Phase 0 Step 6 Deviation ("deferred per plan, recommended-not-blocking"). Next-run gate set for 2026-05-23 03:00; first successful run will flip `LastTaskResult` from `267011` (not-yet-run) to `0`.
  - **Deferred @2-baseline retype verified done** — backlog entry archived to `docs/backlog/agent-self-improvement/applied.md`. Live HEAD inventory across 820 revs: 689 `text+w` / 116 `text+wx` / 14 `binary+w` / 1 bare `text` (the bare holdout is a delete-tombstone with no `p4 sync` impact).

## Deviations from plan

- **`+w` modifier on every text type in typemap (critical dual-VCS fix)**: plan listed bare `text` types (`text *.cpp`, `text *.md`, etc.). After change @2 submitted, every reconciled file got Windows `ReadOnly` attribute set per p4's default "checked-in = read-only" rule — which **breaks the dual-VCS edit-from-either-side contract** (the next `git`-only edit attempt failed with `EPERM` on `docs/plans/shipped/git-to-perforce-migration.md` itself, that's how we found it). Recovery: bulk `attrib -R /S /D` over the tree to clear the bits, then re-submitted typemap with `text+w` / `text+wx` modifiers so future `p4 add`s never set ReadOnly. Plan body amended in PR #402 (`c9a433f`) to list the full `+w` / `+wx` typemap in Phase 0 Step 5. Retroactive `p4 reopen -t text+w //smatchet/main/...` against the existing @2 revs was NOT done; Phases 2-7 shipped against the corrected typemap for new `p4 add`s but the @2 baseline revs still carry the old types. ✓ Resolved — backlog entry archived to `docs/backlog/agent-self-improvement/applied.md` (2026-05-22). Safe to defer until fresh-client `p4 sync` re-applies ReadOnly; no blocking impact on current single-contributor setup.
- **Case sensitivity** (Phase 0 Step 4): plan listed `case-sensitive=2` — invalid value (legal values are `0` insensitive / `1` sensitive). Server was initialized insensitive (Windows / NTFS default); kept as-is. Case-sensitive mode would break the dual-VCS invariant on a case-insensitive filesystem.
- **Unicode mode** (Phase 0 Step 4): plan said `unicode=1`. Skipped. Rationale: single-user dev box; all content ASCII-safe UTF-8 (Locales/*.json round-trip cleanly as `text`); one-way switch adds per-client `P4CHARSET` declarations forever for marginal benefit. Empty depot keeps the re-enable path cheap if a real charset bug ever surfaces. Revisit if a second user with non-ASCII content joins.
- **Daily checkpoint scheduled task** (Phase 0 Step 6): ✓ Resolved — Windows Scheduled Task `p4d_smatchet_checkpoint` created on Mainbot, runs `p4d -r C:\depot -jc` at 3 AM as SYSTEM with `StartWhenAvailable: true`; next-run gate set for 2026-05-23 03:00. See implementation log 2026-05-22 entry above.
- **Client root path** (Phase 1 Step 2): plan said `C:\Dev\Smatchet`; actual repo at `C:\Development\Smatchet`. Client `smatchet_main_alexk` uses the actual path. Plan body amended in PR #402 (`c9a433f`) to drop the hardcoded path and direct readers to substitute their own canonical repo path.
- **Server root**: user installed `p4d` with `c:\depot\` (NOT the canonical-tree path). Plan didn't pin a server root; this is fine — depot files live in `c:\depot\db.*` and `c:\depot\smatchet.p4s/`, separate from the client workspace.
- **`docs/perforce/SETUP.md` runbook**: not authored in PR #375. Subsequently authored by user in PR #373 (merged) for a **remote-LAN topology** (Brick host) rather than the single-host localhost pattern this plan assumed. A parallel single-host draft (PR #377) was opened by the orchestrator but closed as superseded — #373's topology is the authoritative reference.
- **Phase 4 CAS atomicity — bootstrap-on-unset** (PR #383): plan said "use `p4 counter -i` atomics" without specifying CAS semantics. Empirically: `p4 counter --from=0 --to=1 <name>` fails with "Current value is unset" on the first-ever claim because p4 treats "non-existent counter" as DISTINCT from "value 0". Mitigated by a one-shot bootstrap path — on `unset` error, unconditionally `p4 counter <name> 0` then retry CAS. Race-safe (concurrent bootstraps both set to 0; only one subsequent CAS 0→1 wins; cannot stomp an existing claim because `unset` only fires when no counter exists).
- **Phase 4 release-path failure masking** (caught by CodeRabbit on #383 before merge): the `current=$("$p4" counter ... 2>/dev/null || echo 0)` pattern coerced real p4 server-unreachable failures into the phantom "already released" success path. Fixed by distinguishing "command failed" from "command succeeded, counter is 0/absent" — read errors exit 3 with a clear message; the 0/absent case stays the no-op success path.
- **Phase 5 lock marker — `*exclusive*` not `*locked*`** (PR #384): `+l` filetype modifier produces `*exclusive*` in `p4 opened -a` output. `*locked*` is the marker for files locked via deliberate `p4 lock` action (a different mechanism). The hook matches both for completeness — initial draft only matched `*locked*` and would have silently missed every `+l` lock.
- **Phase 5 hook activation** (PR #384): the hook template ships under `docs/harness/claude-code/hooks/` (gitignored once installed). Activation is opt-in via `setup-harness.sh` with `SMATCHET_AGENT_VCS=p4` exported — plan didn't specify the install path; chose `docs/harness/claude-code/hooks/` to match existing lint-hook conventions.

## Verification (actual)

- **Phase 0 exit** (`p4 info` succeeds; throwaway CL submits + re-syncs): ✅ via change @2 baseline submit; `p4 info` shows the right user/client/server/case-handling.
- **Phase 1 Step 5 exit** (clean tree shows empty `git status` AND empty `p4 opened`): ✅ — `p4 opened` returns "File(s) not opened on this client"; `p4 reconcile -n //smatchet/main/...` returns "no file(s) to reconcile". (`git status` shows `?? log` — unrelated stale file from a 2026-05-21 `/remote-control` test; not Phase-1-relevant.)
- **Edit-driven round-trip** (the exit gate Phase 1 actually mandates): ✅ — transient `.p4-roundtrip-probe.txt` was flagged by both `git status --porcelain` (`?? .p4-roundtrip-probe.txt`) AND `p4 reconcile -n` (`opened for add`); clean-up restores empty state on both sides.
- **Dual-VCS ReadOnly regression check**: ✅ initial change @2 submit triggered ReadOnly on every text file (root cause: bare `text` types in typemap). Recovered via bulk `attrib -R` + typemap re-submit with `+w`. Retroactive retype shipped on 2026-05-21 in changes @3 ("retype @2 baseline t…") + @4 ("catch-all text+w typ…"); HEAD inventory on 2026-05-22 confirms 689 `text+w` / 116 `text+wx` / 14 `binary+w` / 1 bare `text` across 820 revs — the lone holdout is `//smatchet/main/log#2`, a delete-tombstone (delete change @5) with no `p4 sync` impact. Backlog entry archived to `docs/backlog/agent-self-improvement/applied.md`.
- **Phase 4 exit** (claim CAS + release + idempotent re-release; default backend unaffected): ✅ 6/6 lifecycle scenarios pass against live `p4d` — first-ever-claim bootstrap → re-claim by another agent fails with existing meta → release → re-claim → idempotent release → default `git-ref` backend takes the early-exit path with zero p4 calls. Detailed run output in PR #383.
- **Phase 5 exit** (deliberate two-agent contention on a `+l`-locked file; one is warned): ✅ verified via a second main-stream client (`smatchet_main_test` rooted at `/tmp`) opening `README.md` with `p4 edit -t +l`; hook fired against the canonical client with both warning-mode (`WARNING` to stderr + exit 0) and block-mode (`SMATCHET_P4_LOCK_HOOK_BLOCK=1` → exit 2) paths. Negative cases (`SMATCHET_AGENT_VCS=git`, unlocked file) silently exit 0. Detailed run output in PR #384.
- **Phase 2 exit** (spawn stub child with `SMATCHET_AGENT_VCS=p4`, observe populated task stream; GC purges on exit): ✅ — `scripts/dev/test-p4-dual-vcs.sh` § Scenario 1 covers allocation + GC end-to-end against live `p4d`. PR #380 verification log.
- **Phase 3 exit** (submit-to-PR bridge end-to-end): ✅ — `test-p4-dual-vcs.sh` § Scenario 2 walks the integrate → sync → `git push` → `gh pr create` chain with a fake-gh harness. PR #382 verification log.
- **Phase 6 exit** (AGENTS.md + CONTEXT.md additions present + cross-linked): ✅ — anchor checker (`scripts/dev/test_doc_anchors.py`) passes against the new sections; `agents/p4-janitor.md` discoverable per the agents.md spec.
- **Phase 7 exit** (3 round-trip scenarios green): ✅ — `bash scripts/dev/test-p4-dual-vcs.sh` exits 0 across all 9 assertions on the shipped `Brick`-LAN topology.

### Gap audit (2026-05-22)

Plan promised 8 modified-file deltas + 2 new files that did not ship with the per-phase PRs. Closed in two follow-up PRs:

- **PR B** (`docs(plan): perforce dual-VCS plan revision sweep + ReadOnly backlog`, PR #402, `c9a433f`) — plan-doc revision sweep: folded the four "pending merge" stubs (#380 / #382 / #388 / #389) into the bulleted Implementation log with shipped SHAs; amended the plan body to match Deviations (typemap `+w` modifiers, case-sensitive valid range, non-hardcoded paths); added the P3 backlog entry tracking the unfinished `p4 reopen -t text+w` on the @2 baseline revs. This very `### Gap audit` subsection was authored by PR B.
- **PR A** (`docs(p4): close 7 mechanical gaps in perforce dual-VCS plan (replaces #401)`, PR #403, `eb0cde0`, on branch `chore/perforce-gap-closeout-a`) — closes the 7 missing file deltas. PR #401 was the first attempt on `wip/perforce-plan-gap-closeout-a` but hit a `tooling.md` append-conflict with PR B at merge time; rebased onto develop tip post-#402 and re-shipped as PR #403 on a fresh branch (force-push to `wip/*` blocked per AGENTS.md force-push carve-out — only `claude/<id>` API-500 recovery is exempt). #401 closed as superseded.

What PR A actually shipped:

- ✅ `docs/perforce/RUNBOOK.md` authored.
- ✅ `scripts/dev/p4-reconcile-check.sh` authored.
- ✅ `scripts/git-hooks/pre-push` invokes the reconcile check.
- ✅ `scripts/dev/lock-claim-update.sh` adds `SMATCHET_LOCK_BACKEND=p4-counter` dispatch (refuses with exit 2 + diagnostic; full p4-side update path filed as a P3 backlog entry).
- ✅ `scripts/setup-harness.sh` adds dual-VCS opt-in check.
- ✅ `docs/guides/offline-builds.md:127` cross-reference de-staled.
- ✅ `agents/git-janitor.md` adds the `See also` cross-link to `agents/p4-janitor.md`.
- ⏭ `scripts/dev/_lock-json.py` — no change required (the helper is backend-agnostic and already shared by both backends).

## Superseded — 2026-05-15 full-migration plan

This doc's original framing (2026-05-15, commit `9d36aab`) was a full git → Perforce migration: archive GitHub, vendor every FetchContent dep, retire all git/gh-touching agents + workflows. The 2026-05-21 drift report against develop tip `95d51a5` surfaced that this would have torched five load-bearing systems (plan-locks, merge-gates, agentic-handoff, perf-CI, coverage-CI) without replacements. Drift report commit: `1a3c9109`.

The user clarified that the actual goal was an opt-in **local agentic-WIP layer** with git/GitHub preserved as the ship-line. This rewrite (2026-05-21) replaces the migration framing with the dual-VCS topology above. Original migration plan body preserved in git history at `9d36aab`; do not rebuild it from this doc.

**Post-merge stale-ref cleanup (2026-05-21 evening)**: PR #361 ("agentic ripout doc cleanup") independently removed the `handoff-implementer` + `pr-iterator` agents, `docs/agentic/USAGE.md`, `ClaudeCodeLocalRunner`, the `SEED.json` / `RUN_RESULT.json` sentinel-envelope, and the `stub-claude` test harness. Phases 2, 3, 7 of this plan referenced those primitives by name; they have been re-anchored to (a) the orchestrator's pre-existing per-subagent worktree allocation (which survives), (b) the `smatchet-merge-watcher` host daemon (the replacement out-of-band poll path per `docs/plans/shipped/smatchet-merge-watcher.md`), and (c) a bats rig modelled on `tests/bats/merge_gates.bats` for Phase 3 coverage in place of the deleted stub harness.

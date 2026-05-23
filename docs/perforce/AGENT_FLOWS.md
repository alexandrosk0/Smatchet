# Perforce agent flows — when to use which verb

> **Plan**: [`docs/design/git-to-perforce-migration.md`](../design/git-to-perforce-migration.md) Phases 2–5.
> **Audience**: agents (or humans driving agents) deciding whether to reach for `git` or `p4` for a given operation.
> **Prerequisite**: dual-VCS bring-up complete per [`docs/perforce/SETUP.md`](SETUP.md).

Smatchet runs git/GitHub as the **ship-line** (canonical, PR review, CI, merge-watcher) and Perforce as an **opt-in local layer** for agentic-WIP primitives. Sessions opt in with `SMATCHET_AGENT_VCS=p4`; sessions that don't keep behaving exactly as they did pre-Perforce.

This doc is the playbook for what to use when.

## TL;DR

| If you need to… | git verb | p4 verb | Owner |
|---|---|---|---|
| Ship code to `develop` for review | `git push` + `gh pr create` | (same — git remains the ship-line) | every agent |
| Spawn an isolated parallel subagent | `git worktree add .claude/worktrees/<id>` | `bash scripts/dev/p4-task-stream.sh <id>` | orchestrator |
| Atomic plan-lock claim | `bash scripts/dev/lock-claim.sh` | `SMATCHET_LOCK_BACKEND=p4-counter bash scripts/dev/lock-claim.sh` | orchestrator |
| Stash WIP locally | `git stash` | `p4 shelve` (server-side, surfaces in P4V) | individual agent |
| Edit a file exclusively (block other agents) | (no equivalent) | `p4 edit -t +l <file>` | individual agent |
| Read a file's revision history | `git log -- <file>` | `p4 filelog <depot-path>` | any |
| Diff before submit | `git diff` | `p4 diff` | any |
| Merge subagent work back to mainline | (manual) | `bash scripts/dev/p4-task-stream-to-pr.sh <id> <title>` | orchestrator |

The general principle: **anything that needs to leave the dev box (PRs, CI, merge-watcher) goes through git**. Anything local + ephemeral + agentic (parallel isolation, locks, shelves, exclusive-edit) can go through p4 for the affordances p4 has that git doesn't.

## Task-stream lifecycle (Phase 2 + 3)

A task stream is the p4 sibling of a git worktree: a labeled scratch space for one subagent's work, parented to `//smatchet/main`, lazily branched, GC'd when stale.

### Allocate

```bash
ws=$(bash scripts/dev/p4-task-stream.sh <agent-id>)
echo "subagent working dir: $ws"
```

Behind the scenes:
1. Creates stream `//smatchet/task-<agent-id>` (single-segment name because depot `//smatchet` has `StreamDepth: //smatchet/1`).
2. Creates client `task_<agent-id>` rooted at `.claude/streams/<agent-id>/`.
3. `p4 populate` seeds the task stream's depot path from `//smatchet/main` (task streams hold zero revs until populated; bare sync returns "No such file(s).").
4. `p4 sync` pulls all parent revs into the workspace (~820 files / ~9 MB today).

Idempotent — re-invocation reuses existing stream + client; sync brings to current head.

### Subagent works in the task stream

Inside `$ws`, the subagent uses any p4 verb: `p4 edit`, `p4 add`, `p4 submit`, etc. Submits land as CLs scoped to the task stream's depot path (`//smatchet/task-<id>/...`), NOT to `//smatchet/main`. Main is unaffected until integration.

### Integrate + ship as PR

```bash
bash scripts/dev/p4-task-stream-to-pr.sh <agent-id> "PR title here"
```

Behind the scenes:
1. Preconditions: git tree clean, on `develop`, local matches origin, no pending CLs on main client.
2. `p4 copy --from <agent-id>` (or `merge --from` if mainline diverged in parallel) → resolve → submit on `//smatchet/main`.
3. `git checkout -b agent/<agent-id>/<kebab-slug>` (branch shape matters: preserves AGENTS.md § Force-push carve-out).
4. `git add -A` → commit (body references the p4 submit CL).
5. `git push -u origin <branch>`.
6. `gh pr create --draft --base develop`.
7. PR URL emitted on stdout for the orchestrator / `smatchet-merge-watcher` to register.

`--dry-run` skips steps 5–6 (no remote side effects) — useful for testing.

### GC

```bash
bash scripts/dev/p4-task-stream-gc.sh --older-than-days 14
```

Scans `//smatchet/task-*`, parses each stream's `Update:` timestamp, deletes streams older than the threshold whose clients have **zero pending CLs** (safety). On-disk cleanup limited to client roots under `.claude/streams/` (refuses to rm-rf anywhere else).

## Lock discipline

Two complementary mechanisms — a **plan-lock** (slice-scoped, coordinates which agent owns a write-set) and an **exclusive file lock** (file-scoped, blocks concurrent edits at the p4 level).

### Plan-lock (slice-scoped)

Same API as git-ref backend; flip the backend env to use p4-counter:

```bash
SMATCHET_LOCK_BACKEND=p4-counter bash scripts/dev/lock-claim.sh <slug> <write-set-file>
# ... do the slice work ...
SMATCHET_LOCK_BACKEND=p4-counter bash scripts/dev/lock-release.sh <slug>
```

Backed by `p4 counter --from=<old> --to=<new> smatchet_lock_<slug>` compare-and-swap. Metadata (`{owner, branch, plan, write_set, …}`) stored in sibling counter `smatchet_lock_<slug>_meta`. See [`docs/design/git-to-perforce-migration.md`](../design/git-to-perforce-migration.md) § Phase 4 for the atomic-primitive details.

Same env vars as the git-ref backend (`AGENT_ID`, `LOCK_BRANCH`, `LOCK_PLAN`, `LOCK_NOTES`). Default backend stays `git-ref` — only sessions that set the env see the p4 path.

### Exclusive file lock (`p4 edit -t +l`)

For the rare case where two agents are racing to edit the same file. Open the file with the `+l` filetype modifier:

```bash
p4 edit -t +l Source_Core/src/SmatchetApp.cpp
```

Effects:
- p4 server marks the file as exclusively locked by the current client.
- Any other client attempting `p4 edit <same-file>` gets `file - already locked by alex@smatchet_main_alex` and refuses to open.
- Lock releases automatically on `p4 submit` (the edit lands) or `p4 revert` (the edit is abandoned).
- Lock survives client disconnect — if the holding agent crashes, the lock persists until `p4 unlock -f` (admin) or explicit revert.

**Use sparingly.** `+l` defeats merging — by design. Reserve for files where concurrent edits cause demonstrable conflicts (e.g. large generated files, single-writer config). For most C++ source, git-style merge-on-conflict is the right model.

**Hook**: `docs/harness/claude-code/hooks/pretool-edit-p4-lock-check.sh` (this PR) is an opt-in `PreToolUse:Edit` hook for Claude Code that warns when the Edit tool is about to modify a file currently locked by another client. The hook is a **warning, not a hard block** — agents can ignore for emergency fixes. Off by default; opt in via `setup-harness.sh` adding the junction into `.claude/hooks/`.

## Shelf vs stash

| | git stash | p4 shelve |
|---|---|---|
| Scope | local-only (no `.git/` sync) | server-side (`p4 shelve` pushes to depot) |
| Visibility | only the originating clone sees it | every client on the same server can `p4 unshelve` |
| Survives clone replacement | no | yes |
| Has metadata (description, author) | minimal (`stash message`) | full CL description, author, datetime |
| Has stack semantics | yes (stash list, pop, drop) | no — flat list of pending CLs |

When to reach for which:
- **git stash** when the work is throwaway / strictly local / won't be picked up by another machine.
- **p4 shelve** when the work might need to move between machines (e.g. crashed on dev box, resume from another).

## Filetype hygiene

Every text file in the depot should carry the `+w` modifier (always-writable). Without `+w`, p4 sets Windows `ReadOnly` on the on-disk file post-submit, which **breaks the dual-VCS edit-from-either-side contract** (a git-only `Edit` then fails with `EPERM`).

The typemap in [`docs/perforce/SETUP.md`](SETUP.md) § 5 is the source-of-truth for `+w` rules. If you find a depot file without `+w`:

```powershell
& $p4 edit -t text+w <file>
if ($LASTEXITCODE -eq 0) { & $p4 submit -d "retype <file> to text+w (dual-VCS hygiene)" }
```

(Bash equivalent: `p4 edit -t text+w <file> && p4 submit -d "retype <file>"` — PowerShell 5.1 has no `&&` so it needs the explicit `$LASTEXITCODE` check.)

The catch-all `text+w //...` typemap line should keep this from happening for new adds.

## Cross-link reconciliation

The canonical workspace at `C:\Development\Smatchet` is BOTH a git working tree (branch `develop`) AND a p4 client (`smatchet_main_<user>` on stream `//smatchet/main`). Editing a file with any verb makes BOTH VCSes see it as modified.

Useful sanity check before any submit / commit:

```bash
git status --porcelain                       # what does git think?
p4 reconcile -n //smatchet/main/... 2>&1     # what does p4 think?
```

The two outputs should agree on what's modified (modulo files only one VCS tracks — `.git/`, `.claude/`, etc., which only git or only p4 sees due to their respective ignore files).

If they disagree, something has gone wrong with the dual-VCS state — investigate before committing to either side. The most common cause is a previous `p4 sync -f` overwriting a git-tracked file without git knowing, or a `git checkout` between branches changing file content that p4 still thinks matches its depot rev.

## When NOT to use Perforce in Smatchet

- **CI**: GitHub Actions runs against git. P4 is invisible to it.
- **CodeRabbit**: reviews GitHub PRs only.
- **`smatchet-merge-watcher`**: polls GitHub PR state; p4-only changes aren't visible.
- **Issue tracking**: GitHub Issues / Jira / Plane (whichever the user uses) — p4 has its own `p4 jobs` system but Smatchet doesn't wire it.
- **Public discoverability**: the GitHub repo is the public face; p4 depot is private to the dev box.

For all of these, ship via git/GitHub. The Perforce layer is purely for agentic-WIP primitives that git can't express.

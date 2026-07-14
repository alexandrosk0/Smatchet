# Perforce agent flows — when to use which verb

> **Plan**: [`docs/plans/shipped/git-to-perforce-migration.md`](../plans/shipped/git-to-perforce-migration.md) Phases 2–5.
> **Audience**: agents (or humans driving agents) deciding whether to reach for `git` or `p4` for a given operation.
> **Prerequisite**: dual-VCS bring-up complete per [`docs/perforce/SETUP.md`](SETUP.md).

Smatchet runs git/GitHub as the **ship-line** (canonical, PR review, CI, merge-watcher) and Perforce as an **opt-in local layer** for agentic-WIP primitives. Sessions opt in with `SMATCHET_AGENT_VCS=p4`; sessions that don't keep behaving exactly as they did pre-Perforce.

This doc is the playbook for what to use when.

## Topology

Concern-oriented summary of which side owns which agentic-WIP primitive — absorbed from `AGENTS.md` § Dual-VCS topology per [`docs/plans/shipped/agents-md-reduction.md`](../plans/shipped/agents-md-reduction.md) D3.

| Concern | git path | p4 path |
|---|---|---|
| Per-subagent isolation | `git worktree add .claude/worktrees/<id>` | `bash agents/scripts/project/p4-task-stream.sh <agent-id>` |
| Plan-lock backend | `refs/locks/<slug>` (default) | `SMATCHET_LOCK_BACKEND=p4-counter` |
| Submit subagent work as PR | (manual) | `bash agents/scripts/project/p4-task-stream-to-pr.sh <id> <title>` |
| Stale-stream GC | (cron via `agents/core/git-janitor.md`) | `agents/core/p4-janitor.md` + `agents/scripts/project/p4-task-stream-gc.sh` |
| Exclusive file lock | (no equivalent) | `p4 edit -t +l <file>` (+ optional `pretool-edit-p4-lock-check.sh` hook) |
| Ship-line (PR review + CI + merge) | ALWAYS git/GitHub | (never p4 — GitHub Actions can't reach a local `p4d`) |
| GitHub → p4d backup mirror | (canonical — GitHub) | `//repo/smatchet` graph depot via Git Connector (one-way, non-authoritative — [`MIRROR.md`](MIRROR.md)) |

The verb-level TL;DR below extends this concern view with per-operation guidance. Existing git-centric AGENTS.md sections (merge-gates, plan-locks default, force-push carve-out, destructive-git-op preflight, ship-loops) are unchanged — the Perforce layer is purely additive, never required, never authoritative, never on the ship-line.

## TL;DR

| If you need to… | git verb | p4 verb | Owner |
|---|---|---|---|
| Ship code to `develop` for review | `git push` + `gh pr create` | (same — git remains the ship-line) | every agent |
| Spawn an isolated parallel subagent | `git worktree add .claude/worktrees/<id>` | `bash agents/scripts/project/p4-task-stream.sh <id>` | orchestrator |
| Atomic plan-lock claim | `bash agents/scripts/core/lock-claim.sh` | `SMATCHET_LOCK_BACKEND=p4-counter bash agents/scripts/core/lock-claim.sh` | orchestrator |
| Stash WIP locally | `git stash` | `p4 shelve` (server-side, surfaces in P4V) | individual agent |
| Edit a file exclusively (block other agents) | (no equivalent) | `p4 edit -t +l <file>` | individual agent |
| Read a file's revision history | `git log -- <file>` | `p4 filelog <depot-path>` | any |
| Diff before submit | `git diff` | `p4 diff` | any |
| Merge subagent work back to mainline | (manual) | `bash agents/scripts/project/p4-task-stream-to-pr.sh <id> <title>` | orchestrator |

The general principle: **anything that needs to leave the dev box (PRs, CI, merge-watcher) goes through git**. Anything local + ephemeral + agentic (parallel isolation, locks, shelves, exclusive-edit) can go through p4 for the affordances p4 has that git doesn't.

## Task-stream lifecycle (Phase 2 + 3)

A task stream is the p4 sibling of a git worktree: a labeled scratch space for one subagent's work, parented to `//smatchet/main`, lazily branched, GC'd when stale.

### Allocate

```bash
ws=$(bash agents/scripts/project/p4-task-stream.sh <agent-id>)
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
bash agents/scripts/project/p4-task-stream-to-pr.sh <agent-id> "PR title here"
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
bash agents/scripts/project/p4-task-stream-gc.sh --older-than-days 14
```

Scans `//smatchet/task-*`, parses each stream's `Update:` timestamp, deletes streams older than the threshold whose clients have **zero pending CLs** (safety). On-disk cleanup limited to client roots under `.claude/streams/` (refuses to rm-rf anywhere else).

## Lock discipline

Two complementary mechanisms — a **plan-lock** (slice-scoped, coordinates which agent owns a write-set) and an **exclusive file lock** (file-scoped, blocks concurrent edits at the p4 level).

### Plan-lock (slice-scoped)

Same API as git-ref backend; flip the backend env to use p4-counter:

```bash
SMATCHET_LOCK_BACKEND=p4-counter bash agents/scripts/core/lock-claim.sh <slug> <write-set-file>
# ... do the slice work ...
SMATCHET_LOCK_BACKEND=p4-counter bash agents/scripts/core/lock-release.sh <slug>
```

Backed by `p4 counter --from=<old> --to=<new> smatchet_lock_<slug>` compare-and-swap. Metadata (`{owner, branch, plan, write_set, …}`) stored in sibling counter `smatchet_lock_<slug>_meta`. See [`docs/plans/shipped/git-to-perforce-migration.md`](../plans/shipped/git-to-perforce-migration.md) § Phase 4 for the atomic-primitive details.

Same env vars as the git-ref backend (`AGENT_ID`, `LOCK_BRANCH`, `LOCK_PLAN`, `LOCK_NOTES`). Default backend stays `git-ref` — only sessions that set the env see the p4 path.

### Exclusive file lock (`p4 edit -t +l`)

For the rare case where two agents are racing to edit the same file. Open the file with the `+l` filetype modifier:

```bash
p4 edit -t +l Source/Core/src/SmatchetApp.cpp
```

Effects:
- p4 server marks the file as exclusively locked by the current client.
- Any other client attempting `p4 edit <same-file>` gets `file - already locked by alex@smatchet_main_alex` and refuses to open.
- Lock releases automatically on `p4 submit` (the edit lands) or `p4 revert` (the edit is abandoned).
- Lock survives client disconnect — if the holding agent crashes, the lock persists until `p4 unlock -f` (admin) or explicit revert.

**Use sparingly.** `+l` defeats merging — by design. Reserve for files where concurrent edits cause demonstrable conflicts (e.g. large generated files, single-writer config). For most C++ source, git-style merge-on-conflict is the right model.

**Hook**: `docs/harness/claude-code/hooks/pretool-edit-p4-lock-check.sh` is a `PreToolUse:Edit|Write|MultiEdit` hook for Claude Code that warns when the Edit tool is about to modify a file currently locked by another client. The hook is a **warning, not a hard block** — agents can ignore for emergency fixes. It is **always deployed** by `setup-harness.sh` and registered in `settings.json.tmpl`, but **self-gates on `SMATCHET_AGENT_VCS=p4`**: it exits 0 immediately in the default git mode, so it only does work when the session opts into p4. Upgrade warning → block with `SMATCHET_P4_LOCK_HOOK_BLOCK=1`.

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
- **The `//repo/smatchet` graph-depot mirror is read-only and one-way** ([`MIRROR.md`](MIRROR.md)): GitHub → p4d only. Never push from `//repo/smatchet` back to GitHub, never treat it as a review/merge source of truth. It is a backup/visibility copy; losing it is annoyance, not data loss.

For all of these, ship via git/GitHub. The Perforce layer is purely for agentic-WIP primitives that git can't express.

## P4-gated ship-loop

When `SMATCHET_AGENT_VCS=p4`, the orchestrator follows a P4-gated ship-loop instead of the default git ship-loop. Documented at [`AGENTS.md`](../../AGENTS.md) § P4-gated ship-loop; this section is the full phase sequence + invariants. Plan: [`docs/plans/shipped/p4-gated-ship-loop.md`](../plans/shipped/p4-gated-ship-loop.md). ADR: [`docs/adr/0008-p4-gated-ship-loop.md`](../adr/0008-p4-gated-ship-loop.md).

### Session-init in p4-mode

```bash
# Probe p4 reachability first. Refuse to silently downgrade.
if ! p4 info >/dev/null 2>&1; then
    # AskUserQuestion: fall back to git ship-loop / abort / follow SETUP.md and retry
    ...
fi

# Plan-lock backend auto-flips to p4-counter, ONLY when unset (no colon —
# empty-string setting is preserved so test-p4-dual-vcs.sh scenario 2
# (line 149) keeps passing).
export SMATCHET_LOCK_BACKEND="${SMATCHET_LOCK_BACKEND-p4-counter}"
```

### Sub-variant selection (ask the user)

| Situation | p4 stream | Promote-to-PR |
|---|---|---|
| **Default** | `//smatchet/main` via canonical client directly | `p4 submit` (approved CL) + `git push` + `gh pr create` |
| **User-approved task stream** | `bash agents/scripts/project/p4-task-stream.sh <id>` | `bash agents/scripts/project/p4-task-stream-to-pr.sh <id> "<title>" --promote-reviewed-cl <CL>` |

**Default**: always `//smatchet/main`. Task streams are never chosen automatically — orchestrator asks via `AskUserQuestion` before allocating one.

**Suggest a task stream** when: multiple slices planned OR write set spans multiple subsystems.

**Never suggest** for single-slice, single-subsystem work. Ask once at task start; do not re-ask mid-task.

### Phase sequence — small-change loop (single slice, main stream)

1. **`p4 iterate` on `//smatchet/main`** — `p4 edit` / `p4 add` / `p4 reconcile` into a pending CL. Keep the final review candidate **pending**; do not submit yet.
2. **Smoke build** (`cmake --build --preset ninja-iter-msvc --target SmatchetStandalone`) — confirm compilable BEFORE shelf. Failure → fix → re-build, no shelf yet. Pass → continue.
3. **Shelf for review** (`p4 shelve -c <pending-CL>`) — present shelf to user via `AskUserQuestion`. Rejected → iterate → re-shelve with `p4 shelve -f -c <pending-CL>`. Approved → continue.
4. **Full tests** — `doctor.sh`, `ninja-test-msvc` + `ctest`, dual-target sentinels, `lint-flush.sh`, coverage-delta (Source/Core touch), doc-anchors / agent-contract (AGENTS.md / agents/** touch), `test-all.sh`, `ninja-ui-test-msvc` (visual touch), `perf-run.sh` + `perf-compare.py` (scenario-map hit). Failure → fix → re-test (no re-review). Pass → continue.
5. **Promote to PR** — `p4 submit -c <approved-CL>` lands on `//smatchet/main`. Then build the git PR commit with the **plumbing-commit recipe** below (§ Promote-to-PR in a shared tree). Do **not** `git checkout -b` / `git add -A` / `git commit` in the canonical client tree: those mutate HEAD + the working tree, which (a) trips `guard-shared-tree.sh` under concurrent sessions and (b) the inevitable `checkout` back to `develop` reverts the just-submitted files on disk, diverging the working tree from the p4 depot head until the PR merges. Post-ship `AskUserQuestion` fires with option 3 pre-selected.

### Phase sequence — multi-slice loop (task stream, user-approved)

For each slice (repeat until all slices done):

- **Iterate in task stream** — edit → `p4 submit` to `//smatchet/task-<id>/...`.
- **Inter-slice slice-boundary gate** — at-most-once per [`AGENTS.md`](../../AGENTS.md) § Build / ctest cadence: `ninja-test-msvc` + `ctest` + `lint-flush.sh` + `test-all.sh`. `code-review` agent NOT dispatched here. Failure → fix in p4 → re-gate (still one slice; cadence respected within the retry loop).

After all slices pass slice-boundary gates:

- **End-gate** — full battery of sentinels + coverage-delta + doc-anchors + agent-contract + bucket-E + perf. Runs ONCE here, NOT per slice. Failures iterate in p4 (no shelf yet; user sees nothing until end-gate green).
- **Prepare main-stream review CL** — `bash agents/scripts/project/p4-task-stream-to-pr.sh <id> "<title>" --prepare-review-cl`. Integrates task stream → `//smatchet/main` into a pending CL, resolves auto-safe, shelves it, prints CL number on stdout.
- **Code-review pass** — `code-review` agent dispatched ONCE (cumulative diff across all slices). Findings → fix inline → re-run end-gate → re-prepare review CL → re-dispatch code-review.
- **Shelf-for-user-validation** — `AskUserQuestion`: "All slices done, tests green, code-review clean. Shelf <CL> ready — review in P4V and confirm." Feedback → fix in p4 → `p4 shelve -f -c <CL>` → re-present. Approved → continue.
- **Promote** — `bash agents/scripts/project/p4-task-stream-to-pr.sh <id> "<title>" --promote-reviewed-cl <CL>`. Submits the approved CL to `//smatchet/main`, then creates git branch + commit + push + draft PR. **In a shared canonical tree** the helper's git-publish step must use the § Promote-to-PR plumbing-commit recipe (not `git checkout -b` + `git commit`) for the same reason as the small-change loop — see the recipe's residual note. Post-ship `AskUserQuestion` (option 3 pre-selected).

### Stranded-CL recovery — `--prepare-review-cl` / `--promote-reviewed-cl`

If a session dies between `--prepare-review-cl` and `--promote-reviewed-cl`, the pending main-stream CL persists across sessions. On resume:

- `--promote-reviewed-cl <CL>` validates: (1) CL exists, (2) CL is pending (not submitted), (3) CL belongs to the current client, (4) CL description matches the task stream's task-id (`task-stream-id: <agent-id>` tag in the description body).
- On mismatch, the script refuses with exit 5 and prints the manual cleanup recipe:
  ```bash
  p4 shelve -d -c <CL>
  P4CLIENT=smatchet_main_$P4USER p4 revert -c <CL> //smatchet/main/...
  p4 change -d <CL>
  ```
- The script never auto-cleans — the user decides whether the stranded CL contains valuable work to recover or is safe to drop.

### Pure-docs slice + p4-mode

The § Pure-docs slice skip rule applies as written. Shelf-review gate still fires — the user is the final reviewer in p4-mode even for docs-only changes. The pending CL is still required (a p4 shelf cannot wrap a submitted CL).

### Trivial-visual-only envelope + p4-mode

The envelope from [`AGENTS.md`](../../AGENTS.md) § Trivial-visual-only change envelope applies. P4 race-recovery substitutes for `git stash`:

1. `p4 sync //smatchet/main/...` before opening any file for edit.
2. `p4 edit -t +l <file>` on hot files (exclusive lock blocks concurrent edits at the p4 server — see § Exclusive file lock above).
3. On conflict surface: `p4 resolve -am` (auto-merge), then `p4 resolve` (manual), then fall back to user.

The shelf step subsumes the Pillar-4 visual-validation pause — the user reviewing the shelf IS the visual sign-off.

### Promote-to-PR in a shared tree — plumbing-commit recipe

The canonical p4-mode promote step. The git ship-line needs a branch with the change, but the canonical client tree (`C:\Dev\Smatchet`) is the **shared integration tree** — usually with sibling sessions live in it. Two hard constraints collide there: `guard-shared-tree.sh` blocks any HEAD/working-tree-mutating git op (`checkout`/`switch`/`reset`/…) while a sibling is live, and the p4-mode invariant forbids `git worktree add` ([`ship-loops.md`](../agent-rules/ship-loops.md) § P4-gated ship-loop). So promote must publish a branch **without touching HEAD or the working tree**.

After `p4 submit`, the on-disk files already hold the submitted content (p4 does not revert the workspace on submit) and git HEAD is still on `develop`. Build the PR commit in a throwaway index with git plumbing — no `checkout`/`add`/`commit`, so the guard never fires, no sibling is rug-pulled, and the working tree stays at the p4 depot head:

```bash
branch="agent/<id>/<kebab-slug>"   # or claude/<id>/<slug> — branch shape preserves AGENTS.md § Force-push carve-out
title="<PR title>"
base="origin/develop"

# 1. Files the submitted CL changed (disk vs develop). git diff covers tracked
#    edits/deletes; ls-files --others covers p4-added files git doesn't track yet.
changed="$(git diff --name-only "$base" --; git ls-files --others --exclude-standard)"

# 2. Assemble the PR commit in a temp index — never touches the real index/HEAD/worktree.
tmp_index="$(mktemp)"; export GIT_INDEX_FILE="$tmp_index"
git read-tree "$base"
printf '%s\n' "$changed" | while IFS= read -r f; do
  [ -n "$f" ] || continue
  if [ -e "$f" ]; then git update-index --add -- "$f"
  else                 git update-index --remove -- "$f"; fi
done
tree="$(git write-tree)"
commit="$(git commit-tree "$tree" -p "$base" -m "$title")"
unset GIT_INDEX_FILE; rm -f "$tmp_index"

# 3. Publish via refs only — `git branch` (not checkout) + push. Guard never fires.
git branch "$branch" "$commit"
git push -u origin "$branch"
gh pr create --draft --base develop --head "$branch" --title "$title" \
  --body "Promotes p4 submit CL <CL> (//smatchet/main). PR commit built via git plumbing; working tree stays at depot head."
```

Why not the override (`SMATCHET_ALLOW_SHARED_SWITCH=1`): it unblocks the guard but the `git checkout -b` it permits still flips the working tree, and the eventual `checkout develop` reverts the just-submitted files on disk under any sibling session — the exact rug-pull the guard exists to prevent. The plumbing recipe needs no override.

Why the guard needs no p4-mode exemption: the recipe uses no op the guard watches for (`checkout`/`switch`/`pull`/`reset`/`merge`/`rebase`/`stash pop`), so the correct promote path passes the guard untouched. The guard's "Do feature work in a worktree" message only ever surfaces if a session ignores this recipe and runs a raw `git checkout -b` — treat that message, in p4-mode, as "use the plumbing recipe," not "add a worktree."

**Residual** — `agents/scripts/project/p4-task-stream-to-pr.sh` still does a raw `git checkout -b` in its git-publish step (the multi-slice `--promote-reviewed-cl` path); it must be ported to this plumbing recipe to be shared-tree-safe. Tracked in `docs/self-improvement/categories/process.md` (PR #1125 / shelf CL 374 entry).

## Destructive p4 ops pre-flight

Companion to [`AGENTS.md`](../../AGENTS.md) § Destructive git ops in shared worktrees. Before running any destructive Perforce verb (`p4 revert -k`, `p4 obliterate`, `p4 unshelve -f`), run the five-step pre-flight:

1. **`p4 -ztag info`** — confirm client + user. Tagged output is easier to parse + harder to misread than the human-formatted default.
2. **`p4 opened -c default //smatchet/...`** — inventory opened files on the default change. Any non-empty result means another in-flight edit could be clobbered.
3. **`p4 shelve -c <CL>`** any pending unrelated CLs that the destructive op might touch. Shelving preserves the work server-side; if the destructive op proves wrong, `p4 unshelve` restores it.
4. **Run the destructive op only after 1–3 succeed.**
5. **`p4 changes -c <client>`** to confirm post-op state matches expectation.

`p4 revert` on a newly-added file removes the file from the workspace AND from the pending CL. `p4 obliterate` is admin-only and destroys submitted history — coordinate explicitly before invoking it.

`p4 unshelve -f` overwrites the current workspace state with shelf content, blowing away any in-flight uncommitted edits. Pair it with step 3 (shelve current state first) before resort.

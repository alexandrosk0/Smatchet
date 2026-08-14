---
name: git-cleanup-procedures
description: The deterministic VCS procedures of an end-of-session git cleanup — path resolution, pre-flight audit, the poll-until-stable mergeability helper, pre-flight cross-checks (worktree/salvage/lock), per-PR squash-merge + branch-delete mechanics, the protected-branch guards, the stale-branch sweep, diverged-branch recovery, bringing develop to latest, the regression-gate build commands, the orphan-scenario sweep, and the final-report template. Invoked by git-janitor (which keeps the judgment: hard refusals, merge-gate orchestration, the FF-clean decision). Use when running post-merge cleanup, sweeping stale branches, or bringing develop to latest.
triggers:
  - git-cleanup
  - post-merge cleanup
  - stale-branch sweep
  - bring to develop
  - poll-until-stable
  - worktree cleanup
version: 1
---

<!-- Skill-only helper (no agent twin; registered in SKILL_ONLY_HELPERS). This is the
     EXTRACTED procedures body of agents/core/git-janitor.md — the verbatim VCS shell.
     The agent keeps the refusals + merge-gate orchestration + FF-clean decision and
     points here per step. reduce-agent-prompt-bloat Slice 3. Cross-harness: Codex/Cursor
     read the agent's summary + this path; Claude Code loads this skill on demand. -->

# git-cleanup-procedures (skill)

The verbatim VCS shell `git-janitor` runs. The agent owns *what to merge, what to refuse, and when* (dependency order, hard refusals, merge-gate rc-handling, the FF-clean decision); this skill owns *how* — the exact `git` / `gh` commands. `git` only touches the git/GitHub ship-line; never p4 (that's `p4-janitor`).

## Path resolution — `<main-repo>` / `<worktree>`

Resolve placeholders at session start:

```bash
MAIN_REPO="$(git rev-parse --show-toplevel)"          # current repo (the one you're invoked from)
git worktree list                                     # full list
# If MAIN_REPO is itself a worktree, the canonical main repo is the first line whose
# path does NOT match the current worktree.
```

If only one worktree exists, drop the `-C <worktree>` lines from every command below. If multiple worktrees exist, the agent confirms which is in scope before running cleanup (janitor doesn't guess). `git worktree list` output is required reading; surface it in the final report.

## Pre-flight (run in order before any mutation)

```bash
# 0. List worktrees so the agent knows what's in scope.
git worktree list

# 1. Pull all remote state.
git -C "$MAIN_REPO" fetch origin --prune
[ -n "${WORKTREE:-}" ] && git -C "$WORKTREE" fetch origin --prune

# 2. Audit uncommitted state in every worktree.
git -C "$MAIN_REPO" status --short
[ -n "${WORKTREE:-}" ] && git -C "$WORKTREE" status --short

# 3. Audit local branches with unmerged commits.
git -C "$MAIN_REPO" branch --no-merged origin/develop
[ -n "${WORKTREE:-}" ] && git -C "$WORKTREE" branch --no-merged origin/develop

# 4. Audit open PRs.
gh pr list --base develop --json number,title,headRefName,mergeable,mergeStateStatus

# 5. Resolve orchestrator identity for the merge-gates user-comment filter.
gh auth status >/dev/null || { echo "gh auth failed — HALT" >&2; exit 1; }
ORCH_USER=$(gh api user --jq .login)
export ORCH_USER

# 6. Source the merge-gates poller (declares poll_merge_gates, gh_pr_ready_idempotent, ask_user_question).
source "$MAIN_REPO/agents/scripts/core/merge-gates.sh"
```

If step 2 reports uncommitted modifications outside the safe-ignore set (`build/`, `.fetchcontent-*/`), HALT and surface the file list (a hard refusal — see the agent).

## Poll-until-stable helper

GitHub computes `mergeable` + `mergeStateStatus` lazily — the first read after a sibling merge often returns `UNKNOWN`. Poll until stable; call before any merge decision:

```bash
pr_state() {
    local n="$1"
    local tries=0 max=10        # 10 × 2s = 20s ceiling
    local m s
    while [ "$tries" -lt "$max" ]; do
        read -r m s < <(gh pr view "$n" --json mergeable,mergeStateStatus \
            --jq '"\(.mergeable) \(.mergeStateStatus)"')
        case "$s" in
            CLEAN|DIRTY|BLOCKED|BEHIND|HAS_HOOKS|UNSTABLE) echo "$m $s"; return 0 ;;
            UNKNOWN|"")                                    sleep 2; tries=$((tries+1)) ;;
            *)                                             echo "$m $s"; return 0 ;;
        esac
    done
    echo "$m $s"; return 1       # caller sees still-UNKNOWN + non-zero exit
}
```

Caller pattern. `read < <()` does NOT propagate the function's exit code (it reflects `read`'s own success), so capture stdout to a variable first, branch on the function's exit, then split:

```bash
if ! PR_OUT=$(pr_state "$N"); then
    echo "PR #$N stuck UNKNOWN after 20s — HALT, surface to user" >&2
    exit 1
fi
read -r MERGEABLE STATE <<< "$PR_OUT"
case "$MERGEABLE/$STATE" in
    MERGEABLE/CLEAN)                ;;                                                              # ready
    MERGEABLE/UNSTABLE|*/HAS_HOOKS)  echo "PR #$N: $STATE, proceed-with-caution" >&2 ;;
    MERGEABLE/BLOCKED)               echo "PR #$N: BLOCKED (required check failing) — HALT" >&2; exit 1 ;;
    CONFLICTING/*)                   echo "PR #$N: CONFLICTING — user must resolve" >&2; exit 1 ;;
    *)                               echo "PR #$N: $MERGEABLE/$STATE — HALT" >&2; exit 1 ;;
esac
```

`UNKNOWN` after 20s is itself halt-worthy — GitHub's mergeability usually settles within seconds; sustained `UNKNOWN` indicates an upstream problem (repo migration, abuse rate-limit, replication outage). Don't merge through it.

## Pre-flight cross-checks (Step 0) — before any destructive op

1. **Worktree bookkeeping audit** — parse `git worktree list --porcelain`, inspect each entry's `gitdir` line. The **main worktree** has no `gitdir` line; skip it from orphan checks. For each additional worktree, the `gitdir` points at `.git/worktrees/<id>/` — if that dir is missing, the working dir is orphaned. Conversely, flag on-disk dirs under `.claude/worktrees/<id>/` with no matching `worktree` line. Phantom dirs warrant a manual `rm -rf` after confirming with the user — basename parity against `.git/worktrees/` is NOT reliable.
2. **Detached-HEAD salvage tag** — for any worktree on detached HEAD, `git -C <path> log --oneline HEAD ^origin/develop ^origin/main` to inventory unique commits. If non-empty, create `salvage/<worktree-name>-<short-summary>` BEFORE any prune/remove so a parallel-agent's WIP isn't GC'd.
3. **Lock staleness sweep** — `bash agents/scripts/core/lock-staleness-sweep.sh` to surface plan-locks whose PR merged but the auto-release token didn't fire (squash-merge edge case). Run before the final report.

## Standard cleanup loop — per-PR mechanics

For each open PR targeting `develop`, in **dependency order** (oldest unmerged first; same-file PRs → older first). The agent owns step 3 (merge-gate rc-handling); the mechanical steps:

```bash
# Step 2 — best-effort pre-flip draft→ready (CR auto_review.drafts:false skips drafts).
gh_pr_ready_idempotent "$N" || echo "WARN: pre-flip failed; poller will retry flip at poll start" >&2

# Step 4 — re-confirm ready (defence-in-depth; idempotent no-op if step 2 flipped).
gh_pr_ready_idempotent "$N" || { echo "gh pr ready failed — HALT" >&2; exit 1; }

# Step 5 — squash-merge via API (works regardless of which branch is checked out on disk).
gh api -X PUT repos/<owner>/<repo>/pulls/<N>/merge -f merge_method=squash   # capture .sha for the log

# Step 6 — delete the remote branch.
gh api -X DELETE repos/<owner>/<repo>/git/refs/heads/<headRefName>
```

- **Step 1** verify merge state via the poll-until-stable helper (require `MERGEABLE`+`CLEAN`).
- **Step 3** run merge gates — the rc-handling decision mapping (0 pass / 1-3 ask-user / 4 closed / 5 pagination / 7 Actions-outage / 8 cancelled-required-rerun / `*` HALT) is the agent's reasoning; the executable case block is below.
- **Step 7** append to plan revision if the PR shipped a slice (`- <sha-short> · <PR-title>` under `## Implementation log`; commit `docs(plan): log <slug> #<N>` on a fresh branch + PR, or batch).
- **Step 8** re-check the next PR's mergeability (merging A may flip B to `CONFLICTING`).
- **Step 9** post-merge backlog sweep (apply threshold-met `docs/self-improvement/` entries; one small PR each).
- **Step 10** verification-automation handoff check (flag manual-verification language for `test-author`).
- **Step 10.5** orphan-scenario sweep (below).

## Merge-gates rc-handling (executable form)

The verbatim step-3 case block. The decision mapping it implements lives in the agent (git-janitor § Standard cleanup loop step 3) — keep the two in lockstep when either changes:

```bash
if [ "${SKIP_MERGE_GATES:-false}" != "true" ]; then
    # Authorized merge → flip draft→ready at poll start (ADR 0006 amendment).
    MERGE_GATES_FLIP_READY=true poll_merge_gates "$OWNER" "$REPO" "$N"
    rc=$?
    case "$rc" in
        0) ;;                                                        # gates passed
        1|2|3)
            choice=$(ask_user_question "Gates blocked (code=$rc)." \
                       "Skip gates and merge anyway" \
                       "Keep waiting (double MAX_POLLS, reset timer)" \
                       "Abandon")
            case "$choice" in
                "Skip gates and merge anyway") echo "WARN: user skipped gates: code=$rc" >&2 ;;
                "Keep waiting"*) MERGE_GATES_MAX_POLLS=$((MERGE_GATES_MAX_POLLS*2)) \
                                MERGE_GATES_FLIP_READY=true \
                                poll_merge_gates "$OWNER" "$REPO" "$N" || exit 1 ;;
                *) exit 1 ;;
            esac
            ;;
        4) ask_user_question "PR no longer mergeable (CLOSED/MERGED)." "Abandon"; exit 1 ;;
        5)
            choice=$(ask_user_question "Pagination overflow — manual review required." \
                       "Abandon (manual review)" \
                       "Skip and merge anyway (acknowledge risk)")
            case "$choice" in
                "Skip and merge anyway"*) echo "WARN: user skipped gates: code=5 (pagination)" >&2 ;;
                *) exit 1 ;;
            esac
            ;;
        7)
            # Actions unavailable (outage escalation): required contexts absent for
            # the OUTAGE_POLLS streak AND zero runs created repo-wide in the probe
            # window. NO skip option — an --admin merge past absent checks is exactly
            # what postmortem-owed.sh's required-ABSENT detector flags.
            choice=$(ask_user_question "Actions unavailable (code=7) — see the ESCALATE diagnosis above." \
                       "Wait for Actions to drain, then re-poll" \
                       "Re-fire CI (push or close-reopen), then re-poll" \
                       "Abandon")
            case "$choice" in
                "Wait for Actions to drain"*)
                    MERGE_GATES_FLIP_READY=true \
                    poll_merge_gates "$OWNER" "$REPO" "$N" || exit 1 ;;
                "Re-fire CI"*)
                    gh pr close "$N" && gh pr reopen "$N"
                    MERGE_GATES_FLIP_READY=true \
                    poll_merge_gates "$OWNER" "$REPO" "$N" || exit 1 ;;
                *) exit 1 ;;
            esac
            ;;
        8)
            # Cancelled-while-pending (required-missing-cancelled): the required
            # context's pending run was cancelled by the concurrency-group
            # collapse and left NO check-run — waiting cannot clear it. The
            # BLOCK output above names the exact `gh run rerun <id>` line(s).
            # No skip option (same absent-checks rationale as code 7).
            choice=$(ask_user_question "Required run cancelled while pending (code=8) — see the gh run rerun line(s) above." \
                       "Rerun the named run(s), then re-poll" \
                       "Abandon")
            case "$choice" in
                "Rerun the named run(s)"*)
                    # Operator/agent executes the printed `gh run rerun <id>`
                    # line(s) from the BLOCK output, then:
                    MERGE_GATES_FLIP_READY=true \
                    poll_merge_gates "$OWNER" "$REPO" "$N" || exit 1 ;;
                *) exit 1 ;;
            esac
            ;;
        *)
            # Defensive catch-all. poll_merge_gates returns 0-5, 7 or 8 today; any
            # unexpected rc must HALT — never silently fall through to auto-merge.
            echo "poll_merge_gates: unexpected rc=$rc — HALT" >&2; exit 1
            ;;
    esac
fi
```

## Protected-branch guards — run before ANY branch delete

Either hit refuses the delete:

```bash
# 1. Config allowlist (project-specific value, portable mechanism).
. scripts/dev/project-config.sh                 # exports PC_VCS_PROTECTED_BRANCHES (space-joined)
read -ra _protected <<< "${PC_VCS_PROTECTED_BRANCHES:-}"
is_protected() { local b="$1" p; for p in "${_protected[@]}"; do [ "$b" = "$p" ] && return 0; done; return 1; }

# 2. Code-reference net (fully generic, zero config) — a branch whose literal name appears
#    in committed source is runtime infra; refuse regardless of the allowlist.
git grep -qlF "$b" origin/develop -- Source/ tools/ scripts/ && echo "PROTECTED (code-referenced): $b — skip"
```

The branch literal lives ONLY in `project.config.json` (`vcs.protected_branches`), never in a portable file — so `test-portable-purity` stays green.

## FF-clean docs-batch exception — execution

The agent owns the decision (preconditions + why). Execution bash:

```bash
# Verify preconditions 1-3 (caller already ran 4 + 5).
ahead_behind=$(git -C "$MAIN_REPO" rev-list --left-right --count origin/develop...develop)
case "$ahead_behind" in
    0$'\t'[1-9]*) ;;                                  # 0 behind, ≥1 ahead → OK
    *) echo "not FF-clean ($ahead_behind); fall back to PR-only"; exit 1 ;;
esac

# Path audit across the ahead-range.
touched=$(git -C "$MAIN_REPO" diff --name-only origin/develop..develop)
disallow='^(Source/Core/|Source/Plugins/|Source/Standalone/|Source/UnrealPlugins/|cmake/|CMakeLists\.txt$|CMakePresets\.json$)'
if printf '%s\n' "$touched" | grep -E -- "$disallow" >/dev/null; then
    echo "blacklisted path touched; fall back to PR-only"; exit 1
fi

git -C "$MAIN_REPO" push origin develop                                  # FF push
```

Pure-docs sub-exception discriminator (skips `test-all.sh` when the diff is strictly docs):

```bash
bash agents/scripts/core/is-pure-docs-diff.sh develop && echo "pure-docs (skip test-all.sh)" || echo "needs full gate"
```

`is-pure-docs-diff.sh <base>` exits 0 if `git diff --name-only origin/<base>...HEAD` is strictly within the doc allow-list.

## Diverged branch recovery (squash-merged + further commits → rebase conflicts)

Recover via file-restore, NOT rebase:

```bash
git log --oneline origin/develop..origin/<branch>          # 1. gap: commits not yet on develop
git diff origin/develop..origin/<branch> --stat            # 2. file delta
git checkout -b <new-clean-branch> origin/develop          # 3. recover
git checkout origin/<branch> -- <each-changed-file>
# working tree now holds the final post-squash delta as a single staged change
```

Verify build, open a fresh PR (title reflects what's *new* vs the merged PR), close the stale branch's PR pointing at the recovery PR. Cherry-pick replays N intermediate states that conflict with the squash; file-restore gives one clean commit equal to the delta.

## Stale-branch sweep

> **Cherry / ahead-count trap.** Under squash-merge, `git rev-list develop..<branch>` and `git cherry develop <branch>` are **useless as merge-safety signals** — a fully-merged branch still reports "N ahead" and `+` for every commit (squash rewrote the patch-ids). The only reliable signals: (a) `gh pr list --head <b> --state all` returning `MERGED`, (b) a content-in-develop spot-check (`git diff origin/develop <branch> -- <file>` byte-identical, or `git grep` for the commit's substance on `origin/develop`), and (c) the squash sha itself reachable — `git merge-base --is-ancestor <PR mergeCommit.oid> origin/develop` (the squash commit, **never** the branch tip, which is never an ancestor after squash). Signal (c) is the strongest single check; the post-squash-merge fast-path below relies on it.

Enumerate both spaces (local `git branch --format='%(refname:short)'` minus develop/main; remote `git branch -r ...` minus origin/HEAD,develop,main; de-dupe by short name). Classify by PR state:

```bash
classify_branch() {                              # echoes: MERGED | CLOSED | OPEN | NO-PR
    gh pr list --head "$1" --state all --json state --jq '.[0].state // "NO-PR"' 2>/dev/null
}
```

| Class | Action |
|---|---|
| **MERGED** | Safe-delete (worktree→local→remote), AFTER the protected-branch guards. |
| **OPEN** | Keep — active PR. |
| **CLOSED** (unmerged) | Verify content-in-develop (may have shipped via a superseding PR). Superseded → safe-delete. Else → `AskUserQuestion`. |
| **NO-PR** | Keep if (committed < ~24 h ago) OR (local-only, no upstream). Else → `AskUserQuestion`. Never auto-delete. |

Worktree-before-branch ordering + dirty-gate + protected guard:

```bash
. scripts/dev/project-config.sh
read -ra _protected <<< "${PC_VCS_PROTECTED_BRANCHES:-}"
is_protected() { local x="$1" p; for p in "${_protected[@]}"; do [ "$x" = "$p" ] && return 0; done; return 1; }
if is_protected "$b" || git grep -qlF "$b" origin/develop -- Source/ tools/ scripts/; then
    echo "PROTECTED: $b — skip"; continue
fi
wt=$(git worktree list --porcelain | awk -v b="$b" '/^worktree /{p=$2} $0=="branch refs/heads/"b{print p}')
if [ -n "$wt" ]; then
    if [ -n "$(git -C "$wt" status --porcelain)" ]; then
        echo "DIRTY worktree $wt holds $b — skip, surface to user"; continue   # never delete dirty
    fi
    git -C "$MAIN_REPO" worktree remove "$wt"
fi
git -C "$MAIN_REPO" branch -D "$b"                                             # local
git -C "$MAIN_REPO" push origin --delete "$b" 2>/dev/null || true             # remote (if it had an upstream)
```

A clean, confirmed-merged worktree is zero-risk — remove inline. Dirty/unconfirmed → skip + surface, never force. **Finish:** `git -C "$MAIN_REPO" fetch origin --prune`, then report the sweep verdict table (name · class · action).

## Bringing `develop` to latest

`gh api ... merge` returns `merged:true` before the new sha replicates — the replication-lag belt re-fetches when local diverges.

```bash
git -C "$MAIN_REPO" fetch origin
git -C "$MAIN_REPO" checkout develop
git -C "$MAIN_REPO" pull --ff-only

# Replication-lag belt.
if [ "$(git -C "$MAIN_REPO" rev-parse develop)" != "$(git -C "$MAIN_REPO" rev-parse origin/develop)" ]; then
    git -C "$MAIN_REPO" fetch origin && git -C "$MAIN_REPO" pull --ff-only
fi

# Worktree: detach to origin/develop (can't share develop checkout across worktrees).
if [ -n "${WORKTREE:-}" ]; then
    git -C "$WORKTREE" fetch origin
    git -C "$WORKTREE" checkout --detach origin/develop
fi
```

**Post-squash-merge fast-path — the 5-step pre-flight does NOT apply** to a branch just squash-merged this session and untouched by another agent (it holds nothing at risk). Confirm both (a) `gh pr view <N> --json state` is `MERGED` and (b) the PR's `mergeCommit.oid` is reachable from `origin/<base>`, then `branch -D` directly (squash makes a different sha, so `branch -d` rejects — use `-D`). The 5-step still binds for any branch whose merge-state you have not positively confirmed.

```bash
git -C "$MAIN_REPO" branch -D <branch-name>
[ -n "${WORKTREE:-}" ] && git -C "$WORKTREE" branch -D <branch-name>
git -C "$MAIN_REPO" worktree remove "$WORKTREE"     # worktree-remove from inside the worktree rejects; run from main
```

## Orphan-scenario sweep (step 10.5; end-of-session only, advisory)

A scenario is orphan when **all three** hold: (i) no recent PR cite, (ii) not in any curated set, (iii) no failing-test reference. Any single hit disqualifies it.

```bash
for f in Source/Core/src/Commands/Scenarios/*Scenario.cpp; do
    name=$(basename "$f" .cpp | sed 's/Scenario$//' | sed 's/\([A-Z]\)/-\L\1/g' | sed 's/^-//')
    recent=$(git log --grep="$name" --since="60.days.ago" --oneline | wc -l)
    curated=$({ grep -lF "$name" scripts/dev/perf-pr-fast-set.json agents/core/perf-gatekeeper.md 2>/dev/null; find tests/golden -type f 2>/dev/null | grep -F "$name" || true; } | wc -l)
    intests=$(grep -rlF "$name" tests/ 2>/dev/null | wc -l)
    [ "$recent" -eq 0 ] && [ "$curated" -eq 0 ] && [ "$intests" -eq 0 ] && echo "ORPHAN: $name ($f)"
done
```

Surface every match via one combined `AskUserQuestion` (per orphan: `keep` / `archive to docs/reference/<name>.md` / `delete .cpp + registry line`). Default = **keep** (advisory, not destructive). Carry the three command outputs verbatim so the user can audit.

## Regression gate

```bash
# Dual-target build is the minimum.
cmake --build --preset ninja-iter-msvc --target SmatchetStandalone
cmake --build --preset ninja-iter-msvc --target SmatchetCore_DX12

# Lua-off variant catches stub-build drift (cmake -B is idempotent).
cmake -B build/lua-off-check -DSMATCHET_WITH_LUA_AUTOMATION=OFF -G Ninja
cmake --build build/lua-off-check --target SmatchetStandalone

# Unified test runner (auto-discovers every scripts/dev/test-*.sh).
bash scripts/dev/test-all.sh
```

A build failure means a squash-merge produced a non-buildable develop → HALT and surface (user / build-doctor authors the fix). `test-all.sh`: exit 0 all pass; 1 regression; 2 missing binary (build problem upstream — HALT). Any non-zero = HALT.

## Final report template

```
=== git-janitor — end-of-session cleanup ===

Pre-flight:
  main repo:    <commit-msg-short>  (clean / DIRTY → halted)
  worktree:     <commit-msg-short>  (clean / DIRTY → halted)
  open PRs:     N

Merged this round:
  #<N>  <title>  →  squash <sha-short>
  ...

Branches deleted (remote + local):
  - <name>
  ...

Stale-branch sweep:
  <name>  <MERGED|CLOSED|OPEN|NO-PR>  <deleted|kept: reason|PROTECTED|asked-user>
  ...
  (protected branches always shown as kept: PROTECTED so the user sees they were considered + spared)

Backlog sweep: <N applied / 0>

Regression gate:
  SmatchetStandalone  PASS
  SmatchetCore_DX12   PASS
  lua-off variant     PASS
  scripts/dev/*.sh    N/N PASS

develop now at:  <sha-short>  <title>

Residue requiring user action:
  - git -C "$MAIN_REPO" worktree remove "$WORKTREE"   (if you want this worktree gone)
  - test-author follow-up: PR #<N> shipped manual verification step — flagged
  - <other manual items>

Worktrees in scope: <git worktree list output>
```

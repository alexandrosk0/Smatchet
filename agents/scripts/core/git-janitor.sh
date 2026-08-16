#!/usr/bin/env bash
# agents/scripts/core/git-janitor.sh — automate the deterministic post-merge cleanup steps.
#
# Wraps the manual flow at agents/core/git-janitor.md § Standard cleanup loop into a
# single command. Refuses to act on uncommitted user work; refuses to
# force-push; refuses to delete branches that aren't merged on GitHub.
#
# Usage:
#   bash agents/scripts/core/git-janitor.sh --post-merge <pr-number>
#
# What it does (post-merge mode):
#   1. Verify clean working tree (no uncommitted modifications outside
#      .fetchcontent-* / build/*).
#   2. Verify the PR is MERGED on GitHub (refuse if not).
#   3. fetch + prune remotes.
#   4. Switch to develop, fast-forward to origin/develop.
#   5. Delete the local PR branch (after GitHub remote-delete on squash-merge).
#   5.5. Backfill the merge-snapshot ledger row when the merge actor left none
#      (human/UI merges — merge-pipeline-02; verdict BACKFILLED, actor
#      git-janitor, age-capped via SMATCHET_JANITOR_SNAPSHOT_MAX_AGE_HOURS,
#      default 6h, 0 = uncapped; best-effort, never fails the cleanup).
#   6. Run `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12`
#      as the final regression gate.
#   7. Print a concise report.
#
# Exit codes:
#   0 — clean cleanup; dual-target build passed (or deferred: other live sessions).
#   1 — refused to act (uncommitted work, PR not merged, build failed).
#   2 — usage error / required tool missing.
#
# Env knobs (concurrent-session confinement, Step 3.5):
#   CLAUDE_SESSION_ID / SMATCHET_JANITOR_SELF_SESSION — caller's own session id,
#       excluded from the live-session count so the orchestrator never blocks itself.
#   SMATCHET_JANITOR_DEFER — unset/1 (default): hard-defer when another session is
#       live; 0: force cleanup anyway.

set -euo pipefail

usage() {
    echo "Usage: bash agents/scripts/core/git-janitor.sh --post-merge <pr-number>" >&2
    exit 2
}

[ "${1:-}" = "--post-merge" ] || usage
PR_NUMBER="${2:-}"
[ -n "$PR_NUMBER" ] || usage
[[ "$PR_NUMBER" =~ ^[0-9]+$ ]] || { echo "git-janitor: pr-number must be numeric (got: $PR_NUMBER)" >&2; exit 2; }

command -v gh >/dev/null 2>&1 || { echo "git-janitor: gh CLI required on PATH" >&2; exit 2; }
command -v cmake >/dev/null 2>&1 || { echo "git-janitor: cmake required on PATH" >&2; exit 2; }

# ---------- Step 1: clean working tree --------------------------------------
DIRTY="$(git status --porcelain | grep -Ev '^.. (\.fetchcontent-|build/)' || true)"
if [ -n "$DIRTY" ]; then
    echo "git-janitor: REFUSE — uncommitted work in tree:" >&2
    printf '%s\n' "$DIRTY" >&2
    echo "Commit or stash before running --post-merge cleanup." >&2
    exit 1
fi

# ---------- Step 2: verify PR is merged --------------------------------------
echo "[git-janitor] checking PR #${PR_NUMBER} state..."
# Use gh's built-in --jq to extract fields — no python dependency. The script
# pre-flight only checks for gh + cmake; python may be absent on python3-only
# hosts and would have yielded empty PR_STATE / silent fallthrough.
PR_STATE="$(gh pr view "$PR_NUMBER" --json state --jq '.state' 2>/dev/null || echo "")"
PR_BRANCH="$(gh pr view "$PR_NUMBER" --json headRefName --jq '.headRefName' 2>/dev/null || echo "")"
if [ -z "$PR_STATE" ]; then
    echo "git-janitor: gh pr view #${PR_NUMBER} failed (state empty); cannot verify merge state." >&2
    exit 1
fi
if [ "$PR_STATE" != "MERGED" ]; then
    echo "git-janitor: REFUSE — PR #${PR_NUMBER} state=${PR_STATE} (expected MERGED). Aborting." >&2
    exit 1
fi
echo "[git-janitor] PR #${PR_NUMBER} (${PR_BRANCH}) is MERGED on GitHub."

# ---------- Step 3: fetch + prune --------------------------------------------
echo "[git-janitor] fetching + pruning remotes..."
git fetch --all --prune

# ---------- Step 3.5: concurrent-session confinement -------------------------
# Steps 4-5 mutate THIS tree's HEAD/branches (checkout develop, ff-merge,
# branch -D). If OTHER interactive Claude sessions are live in this tree, those
# ops rug-pull them (the documented multi-session collision). Count live
# entries in the per-tree registry, EXCLUDING the caller's own session
# (CLAUDE_SESSION_ID; override via SMATCHET_JANITOR_SELF_SESSION) so the
# autonomous ship-loop orchestrator that invoked this janitor never blocks
# itself. Hard-defer by DEFAULT when any OTHER session is live;
# SMATCHET_JANITOR_DEFER=0 forces cleanup anyway (legacy =1 still defers).
# Registry files are key=value lines (branch=/sha=/ppid=/ts=) named by session
# id (session-tree-banner.sh). See docs/agent-rules/process-rules.md
# § Concurrent interactive sessions.
#
# Liveness + pruning use the shared authoritative-pid primitives in
# session-registry-lib.sh (sr_entry_is_live / sr_prune_dead_stale) — the same
# real-pid liveness the banner + drift guard use, so the janitor agrees byte-for-
# byte (DRY Quality Pillar) instead of re-deriving the now-superseded inline
# ts-OR-kill-0 shim that over-blocked on a just-exited sibling. If the lib is
# somehow absent the script degrades to a conservative fresh-ts-only count
# (never false-prunes a live session, never false-allows a HEAD-moving op).
JANITOR_TREE="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
SELF_SESSION="${SMATCHET_JANITOR_SELF_SESSION:-${CLAUDE_SESSION_ID:-}}"
NOW_TS="$(date -u +%s)"

_SR_LIB="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd)/session-registry-lib.sh"
[ -f "$_SR_LIB" ] || _SR_LIB="$JANITOR_TREE/agents/scripts/core/session-registry-lib.sh"
if [ -f "$_SR_LIB" ]; then
    # shellcheck source=agents/scripts/core/session-registry-lib.sh
    . "$_SR_LIB"
else
    # Degraded fallbacks: fresh-ts-only liveness; prune is a no-op (never delete
    # an entry we cannot prove dead via the authoritative-pid rule).
    sr_entry_is_live() {
        local fts; fts="$(sed -n 's/^ts=//p' "$1" 2>/dev/null | head -n1)"
        case "$fts" in ''|*[!0-9]*) return 1 ;; esac
        [ $(($2 - fts)) -lt "${SMATCHET_REGISTRY_FRESH_SECS:-1800}" ]
    }
    sr_prune_dead_stale() { :; }
fi

# Cross-worktree dead+stale prune: scan EVERY worktree's .active-sessions/, not
# just this tree's. The registry writer's landing site is pwd-non-deterministic
# (session-tree-banner.sh: CLAUDE_PROJECT_DIR unset -> $(pwd)), so dead entries
# accrete across sibling worktrees and only this sweep clears them. Conservative:
# sr_prune_dead_stale removes ONLY entries with an authoritative pid proven dead
# AND ts-stale — never a live session, never the caller's own entry.
git worktree list --porcelain 2>/dev/null | while IFS= read -r _wl_line; do
    case "$_wl_line" in
        "worktree "*)
            _wt_regdir="${_wl_line#worktree }/.claude/.active-sessions"
            [ -d "$_wt_regdir" ] || continue
            sr_prune_dead_stale "$_wt_regdir" "$SELF_SESSION" "$NOW_TS"
            ;;
    esac
done

# Live-sibling count for THIS tree (the tree whose HEAD steps 4-5 will move).
SESS_REGDIR="$JANITOR_TREE/.claude/.active-sessions"
LIVE_SESSIONS=0
if [ -d "$SESS_REGDIR" ]; then
    for f in "$SESS_REGDIR"/*; do
        [ -f "$f" ] || continue
        [ -n "$SELF_SESSION" ] && [ "$(basename "$f")" = "$SELF_SESSION" ] && continue
        sr_entry_is_live "$f" "$NOW_TS" && LIVE_SESSIONS=$((LIVE_SESSIONS + 1))
    done
fi
if [ "$LIVE_SESSIONS" -gt 0 ]; then
    echo "git-janitor: ${LIVE_SESSIONS} other live session(s) registered in ${JANITOR_TREE} (excluding self)." >&2
    echo "             Steps 4-5 change HEAD here and can corrupt a concurrent session." >&2
    if [ "${SMATCHET_JANITOR_DEFER:-1}" = "0" ]; then
        echo "git-janitor: WARNING — SMATCHET_JANITOR_DEFER=0 forces cleanup despite ${LIVE_SESSIONS} live sibling(s); HEAD may move under them." >&2
    else
        echo "[git-janitor] DEFER (default): ${LIVE_SESSIONS} other live session(s) in integration tree — skipping HEAD-mutating cleanup. Set SMATCHET_JANITOR_DEFER=0 to force, or re-run when idle." >&2
        exit 0
    fi
fi

# ---------- Step 4: switch to develop, fast-forward --------------------------
CURRENT_BRANCH="$(git rev-parse --abbrev-ref HEAD)"
if [ "$CURRENT_BRANCH" != "develop" ]; then
    echo "[git-janitor] switching from ${CURRENT_BRANCH} to develop..."
    git checkout develop
fi
echo "[git-janitor] fast-forwarding develop to origin/develop..."
git merge --ff-only origin/develop || {
    echo "git-janitor: FAIL — develop is not FF-clean against origin/develop; resolve manually." >&2
    exit 1
}

# ---------- Step 5: delete the local PR branch -------------------------------
if [ -n "$PR_BRANCH" ] && git show-ref --quiet "refs/heads/$PR_BRANCH"; then
    echo "[git-janitor] deleting local branch ${PR_BRANCH}..."
    # Use -D since squash-merge leaves the local tip orphaned (not ancestor of develop tip).
    git branch -D "$PR_BRANCH" || {
        echo "git-janitor: WARN — failed to delete local branch ${PR_BRANCH} (worktree owns it?)" >&2
    }
else
    echo "[git-janitor] local branch ${PR_BRANCH:-<unknown>} already gone (or never existed locally)."
fi

# ---------- Step 5.5: merge-snapshot ledger backfill (ADR-0017) ---------------
# The sanctioned merge actors append their own ledger row at the decision
# instant; a HUMAN/UI merge — or an automerge-arming session that died before
# the merge event — leaves a ledger hole (merge-pipeline-02). This janitor runs
# minutes after a merge, so it is the post-merge hook that closes the hole:
# when the just-cleaned PR has NO row for pr+mergeCommit, compose one from the
# live PR state (labels persist on merged PRs outside the watcher path; the
# rollup is minutes-fresh — strictly fresher than the SessionStart live
# fallback that otherwise covers the hole) and append it with verdict
# BACKFILLED + actor git-janitor. BACKFILLED (never GATES_PASSED) marks the row
# post-hoc-composed; the detector keys on redChecks/overrideLabels, which ARE
# captured (via safe-admin-merge.sh's shared projections). Age cap: a merge
# older than SMATCHET_JANITOR_SNAPSHOT_MAX_AGE_HOURS (default 6; 0 = uncapped)
# is LEFT AS A HOLE — ADR-0017: a stale line is worse than a hole, and an
# unparseable mergedAt fails closed to "too old". Best-effort throughout: no
# failure here ever fails the cleanup (the || invocation also suspends -e
# inside, so each parse degrades instead of aborting).
backfill_merge_snapshot() {
    local jdir view mc merged_at head_sha ledger red_csv override_csv ma age_cap
    command -v jq >/dev/null 2>&1 || { echo "[git-janitor] ledger backfill skipped (jq not on PATH; live fallback covers)."; return 0; }
    jdir="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
    # Ledger path: anchored to the SCRIPT's repo root (jdir/../../..), the same
    # anchor merge-snapshot-append.sh uses — NOT $JANITOR_TREE (the cwd's
    # toplevel), which can be a different worktree; the dedup check and the
    # append must always target the same file, so the resolved path is also
    # passed to the helper explicitly.
    ledger="${MERGE_SNAPSHOT_LEDGER:-$(cd "$jdir/../../.." && pwd)/docs/self-improvement/merge-snapshots.jsonl}"
    # Fast path BEFORE any network call: a GitHub PR merges at most once, so a
    # pr-number match alone proves a row exists (fixed-string grep; the
    # helper's pr+mergeCommit idempotency guard stays the authoritative dedup).
    if [ -f "$ledger" ] && grep -qF "\"pr\":${PR_NUMBER}," "$ledger"; then
        echo "[git-janitor] ledger row for PR #${PR_NUMBER} already present (merge actor wrote it) — no backfill needed."
        return 0
    fi
    view="$(gh pr view "$PR_NUMBER" --json mergeCommit,mergedAt,headRefOid,labels,statusCheckRollup 2>/dev/null || echo "")"
    mc="$(jq -r '.mergeCommit.oid // ""' <<<"$view" 2>/dev/null || echo "")"
    merged_at="$(jq -r '.mergedAt // ""' <<<"$view" 2>/dev/null || echo "")"
    head_sha="$(jq -r '.headRefOid // ""' <<<"$view" 2>/dev/null || echo "")"
    if [ -z "$mc" ] || [ -z "$head_sha" ]; then
        echo "[git-janitor] ledger backfill skipped (mergeCommit/headRefOid unavailable; live fallback covers)."
        return 0
    fi
    # An empty mergedAt must be checked EXPLICITLY: GNU `date -d ""` succeeds
    # (today 00:00 UTC), so feeding it through the age math would fail OPEN
    # before 06:00 UTC and stamp the row with the janitor's run time — exactly
    # the post-hoc-stale row ADR-0017 forbids.
    if [ -z "$merged_at" ]; then
        echo "[git-janitor] ledger backfill skipped (mergedAt unavailable — undatable fails closed; live fallback covers PR #${PR_NUMBER})." >&2
        return 0
    fi
    age_cap="${SMATCHET_JANITOR_SNAPSHOT_MAX_AGE_HOURS:-6}"
    # NOW_TS is assigned under set -e at startup, but validate it anyway: an
    # empty/garbage value would make $((NOW_TS - ma)) treat it as 0 and accept
    # arbitrarily old merges (fail-open). Undatable "now" fails closed too.
    case "$NOW_TS" in
        ''|*[!0-9]*)
            echo "[git-janitor] ledger backfill skipped (current timestamp unavailable — undatable fails closed; live fallback covers PR #${PR_NUMBER})." >&2
            return 0 ;;
    esac
    if [ "$age_cap" != "0" ]; then
        # GNU date first, BSD/macOS fallback (mirrors safe-admin-merge.sh's
        # grace math) — without it every mergedAt is "undatable" on BSD hosts
        # and the backfill silently never fires.
        ma="$(date -u -d "$merged_at" +%s 2>/dev/null \
              || date -u -j -f "%Y-%m-%dT%H:%M:%SZ" "$merged_at" +%s 2>/dev/null \
              || echo "")"
        if [ -z "$ma" ] || [ $(( NOW_TS - ma )) -gt $(( age_cap * 3600 )) ]; then
            echo "[git-janitor] ledger backfill skipped — merge older than ${age_cap}h (or undatable): a post-hoc row this stale is worse than a hole (ADR-0017); postmortem-owed live fallback covers PR #${PR_NUMBER}." >&2
            return 0
        fi
    fi
    # The redChecks/overrideLabels projections live in safe-admin-merge.sh.
    # Source it inside a SUBSHELL: although its CLI entry point is guarded,
    # sourcing still executes its top level, which can `exit 2` fail-closed
    # (e.g. the merge-gates allowlist failing to load) — and `exit` in a file
    # sourced HERE would kill the whole janitor past the point it already
    # mutated HEAD. The subshell contains any exit; a non-zero rc (source
    # failure OR jq failure, pipefail) skips the append entirely — degrading
    # to empty arrays and appending anyway would write an authoritative-looking
    # "clean" row that suppresses postmortem-owed's live fallback.
    if ! red_csv="$( ( set -o pipefail; . "$jdir/safe-admin-merge.sh" >/dev/null 2>&1
                       downgraded_red_checks "$view" | paste -sd, - ) 2>/dev/null )"; then
        echo "[git-janitor] WARN — redChecks projection failed; ledger row NOT written (live fallback covers PR #${PR_NUMBER})." >&2
        return 0
    fi
    if ! override_csv="$( ( . "$jdir/safe-admin-merge.sh" >/dev/null 2>&1
                            override_labels_csv "$view" ) 2>/dev/null )"; then
        echo "[git-janitor] WARN — overrideLabels projection failed; ledger row NOT written (live fallback covers PR #${PR_NUMBER})." >&2
        return 0
    fi
    if MERGE_SNAPSHOT_LEDGER="$ledger" SNAPSHOT_MERGED_AT="$merged_at" \
        bash "$jdir/merge-snapshot-append.sh" \
        "$PR_NUMBER" "$mc" "$head_sha" BACKFILLED "$red_csv" "$override_csv" git-janitor; then
        echo "[git-janitor] ledger row BACKFILLED for PR #${PR_NUMBER} (redChecks: ${red_csv:-none}; overrides: ${override_csv:-none}). Commit it with your next develop-bound commit (chore(ledger) if nothing else is in flight)."
    else
        echo "[git-janitor] WARN — ledger backfill append failed; live fallback covers PR #${PR_NUMBER}." >&2
    fi
    return 0
}
# ||-context suspends -e inside the function; the function itself always
# returns 0 (every failure path degrades + logs), so `|| true` is honest —
# there is no reachable error branch to report here.
backfill_merge_snapshot || true

# ---------- Step 6: dual-target regression build -----------------------------
echo "[git-janitor] running dual-target regression build..."
if ! cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12 2>&1 | tail -20; then
    echo "git-janitor: FAIL — dual-target build failed. Develop is broken." >&2
    exit 1
fi

# ---------- Step 7: report ---------------------------------------------------
echo
echo "[git-janitor] === cleanup complete ==="
echo "  PR:             #${PR_NUMBER} (MERGED)"
echo "  Branch:         ${PR_BRANCH} (local deleted, remote already cleaned)"
echo "  Develop:        $(git rev-parse --short HEAD) (== origin/develop)"
echo "  Build:          PASS (SmatchetStandalone + SmatchetCore_DX12)"
exit 0

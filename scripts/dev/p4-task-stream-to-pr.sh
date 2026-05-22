#!/usr/bin/env bash
# p4-task-stream-to-pr.sh — integrate a Perforce task stream back to main
# and ship the resulting changes as a GitHub PR.
#
# The piece that closes the loop. A subagent finishes work in its task
# stream (allocated by `bash scripts/dev/p4-task-stream.sh`); this script
# walks that work back to `//smatchet/main`, then into the canonical git
# working tree (rooted at the same workspace via `smatchet_main_*`), then
# onto a new git branch + push + GitHub PR.
#
# Phase 3 of `docs/design/git-to-perforce-migration.md`.
#
# Usage:
#   bash scripts/dev/p4-task-stream-to-pr.sh <agent-id> <pr-title> [--dry-run]
#
# Arguments:
#   agent-id  — the task stream's agent id (the one used at allocate time)
#   pr-title  — quoted PR title; the script derives a kebab-case branch
#               slug from it (`agent/<agent-id>/<slug>`)
#
# Options:
#   --dry-run — do every step EXCEPT remote actions (`git push` AND
#               `gh pr create`). Useful for integration testing so the
#               PR list doesn't get polluted with test PRs and no stray
#               branch lands on origin. The p4 integrate + submit ARE
#               still real (p4 has no native dry-run for submit).
#               Prints what the git-push + gh invocations would have been.
#
# Required environment (see docs/perforce/SETUP.md § 1):
#   P4PORT, P4USER
#
# Optional environment:
#   P4_STREAM_PARENT  — integrate target (default: //smatchet/main)
#   P4_TASK_PREFIX    — task-stream path prefix (default: //smatchet/task-)
#   P4_MAIN_CLIENT    — client bound to the integrate target (default:
#                       smatchet_main_${P4USER})
#   P4_BIN            — p4 executable (default: `p4`)
#   GH_BIN            — gh executable (default: `gh`)
#   GIT_BASE_BRANCH   — base branch for the PR (default: develop)
#
# Exit codes:
#   0 — PR opened (or --dry-run plan printed); URL on stdout
#   1 — p4 integrate/resolve/submit failed (conflicts or server error)
#   2 — argument / environment error
#   3 — git state precondition failed (dirty tree, wrong branch, missing remote)
#   4 — gh pr create failed (or auth missing)
#
# Hard rules:
#   - Refuses to run with a dirty git working tree (would conflate p4 imports
#     with unrelated user changes).
#   - Refuses to run if `p4 resolve -as` leaves unresolved files (manual
#     resolution required; the orchestrator can re-invoke after fixup).
#   - On `gh pr create` failure, leaves the git branch pushed so the user
#     can open the PR manually via the printed URL.

set -euo pipefail

usage() {
    echo "usage: bash scripts/dev/p4-task-stream-to-pr.sh <agent-id> <pr-title> [--dry-run]" >&2
    exit 2
}

dry_run=0
positional=()
while [ "$#" -gt 0 ]; do
    case "$1" in
        --dry-run) dry_run=1; shift ;;
        -h|--help) sed -n '2,52p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        --) shift; while [ "$#" -gt 0 ]; do positional+=("$1"); shift; done ;;
        -*) echo "p4-task-stream-to-pr: unknown flag '$1'" >&2; usage ;;
        *) positional+=("$1"); shift ;;
    esac
done

[ "${#positional[@]}" -eq 2 ] || usage
agent_id="${positional[0]}"
pr_title="${positional[1]}"

if ! printf '%s' "$agent_id" | grep -qE '^[a-z0-9][a-z0-9-]{0,63}$'; then
    echo "p4-task-stream-to-pr: invalid agent-id '$agent_id' — must match [a-z0-9][a-z0-9-]{0,63}" >&2
    exit 2
fi
[ -n "$pr_title" ] || { echo "p4-task-stream-to-pr: pr-title must be non-empty" >&2; exit 2; }

# --- env resolution -------------------------------------------------------
: "${P4PORT:?p4-task-stream-to-pr: P4PORT not set; see docs/perforce/SETUP.md § 1}"
: "${P4USER:?p4-task-stream-to-pr: P4USER not set; see docs/perforce/SETUP.md § 1}"
export P4PORT P4USER

p4="${P4_BIN:-p4}"
gh="${GH_BIN:-gh}"
parent="${P4_STREAM_PARENT:-//smatchet/main}"
task_prefix="${P4_TASK_PREFIX:-//smatchet/task-}"
main_client="${P4_MAIN_CLIENT:-smatchet_main_${P4USER}}"
base_branch="${GIT_BASE_BRANCH:-develop}"

stream="${task_prefix}${agent_id}"

# --- branch slug derivation -----------------------------------------------
# kebab-case from pr-title: lowercase, non-alnum → -, collapse runs, trim ends, cap 64
slug=$(printf '%s' "$pr_title" \
    | tr '[:upper:]' '[:lower:]' \
    | sed -e 's/[^a-z0-9]/-/g' -e 's/-\{1,\}/-/g' -e 's/^-//' -e 's/-$//' \
    | cut -c1-64)
[ -n "$slug" ] || { echo "p4-task-stream-to-pr: derived empty slug from pr-title" >&2; exit 2; }
branch="agent/${agent_id}/${slug}"

# --- precondition: git state clean + on base ------------------------------
repo_root="$(git rev-parse --show-toplevel 2>/dev/null)" || {
    echo "p4-task-stream-to-pr: not inside a git checkout" >&2; exit 3
}
cd "$repo_root"

if [ -n "$(git status --porcelain)" ]; then
    echo "p4-task-stream-to-pr: git working tree is dirty — commit or stash before re-running" >&2
    git status --short >&2
    exit 3
fi

current_branch=$(git rev-parse --abbrev-ref HEAD)
if [ "$current_branch" != "$base_branch" ]; then
    echo "p4-task-stream-to-pr: must be on '${base_branch}' (currently on '${current_branch}')" >&2
    exit 3
fi
git fetch origin "$base_branch" --quiet || {
    echo "p4-task-stream-to-pr: git fetch origin ${base_branch} failed" >&2; exit 3
}
if ! git diff --quiet "origin/${base_branch}"; then
    echo "p4-task-stream-to-pr: local ${base_branch} differs from origin — pull first" >&2
    exit 3
fi

# --- precondition: p4 server reachable + task stream exists ---------------
if ! "$p4" info >/dev/null 2>&1; then
    echo "p4-task-stream-to-pr: cannot reach p4 server at ${P4PORT}" >&2; exit 1
fi
if ! "$p4" streams "$stream" 2>/dev/null | grep -q "Stream ${stream}"; then
    echo "p4-task-stream-to-pr: task stream ${stream} not found (allocate with p4-task-stream.sh first)" >&2
    exit 2
fi

# Pending CLs on the main client = leftover state from a prior failed run.
# Abort + tell the user how to clean up — auto-revert would silently
# discard work that might be unrelated to this invocation.
#
# Two-step counting (NOT `... | wc -l || echo 0`): under `set -o pipefail`,
# the fallback `echo 0` only fires if the WHOLE pipeline fails. When p4
# succeeds but produces zero lines, the captured value is the wc-l output
# alone ("0"). When p4 FAILS, the value becomes "wc-output\n0" — multi-line,
# which breaks the integer test downstream with "integer expression expected".
if pending_out=$("$p4" changes -s pending -c "$main_client" 2>/dev/null); then
    pending_on_main=$(printf '%s' "$pending_out" | grep -c . || true)
else
    pending_on_main=0
fi
if [ "${pending_on_main:-0}" -gt 0 ]; then
    echo "p4-task-stream-to-pr: ${main_client} has ${pending_on_main} pending CL(s); leftover from prior run?" >&2
    "$p4" changes -s pending -c "$main_client" >&2
    echo "p4-task-stream-to-pr: clean up with: p4 revert -c <cl> ${parent}/... && p4 change -d <cl>" >&2
    exit 1
fi

# --- p4 copy task stream → main ------------------------------------------
# Task-stream → mainline uses `p4 copy --from <stream-name>` (no resolve
# needed when main hasn't moved). p4 will say "needs 'merge' not 'copy' in
# this direction" if parallel changes landed on main — fall back to
# `p4 merge --from` in that case (surfaces resolve required to caller).
# `p4 integrate <src>/... <dst>/...` fails outright with "Must use a stream
# view to merge into //smatchet/main".
stream_name="${stream##*/}"
echo "p4-task-stream-to-pr: p4 copy --from ${stream_name} (into ${parent} via ${main_client})" >&2
copy_out=$(P4CLIENT="$main_client" "$p4" copy --from "$stream_name" 2>&1 || true)
echo "$copy_out" >&2

if echo "$copy_out" | grep -qiE "needs 'merge' not 'copy'"; then
    echo "p4-task-stream-to-pr: copy refused (mainline moved) — retrying as merge" >&2
    merge_out=$(P4CLIENT="$main_client" "$p4" merge --from "$stream_name" 2>&1 || true)
    echo "$merge_out" >&2
    if echo "$merge_out" | grep -qiE 'all revision\(s\) already integrated|no files to merge|no such file'; then
        echo "p4-task-stream-to-pr: nothing to merge (empty task stream)" >&2
        exit 1
    fi
elif echo "$copy_out" | grep -qiE 'all revision\(s\) already integrated|no permission|no such file|no files to copy'; then
    echo "p4-task-stream-to-pr: nothing to copy (already up-to-date or empty task stream)" >&2
    exit 1
fi

# Resolve any conflicts auto-safe; then check for unresolved.
# `p4 resolve -n` prints "No file(s) to resolve." when clean — case-
# insensitive grep is the reliable way to detect it (previous `grep -vc`
# pattern double-negated and reported phantom conflicts).
echo "p4-task-stream-to-pr: p4 resolve -as (auto-accept safe)" >&2
P4CLIENT="$main_client" "$p4" resolve -as >&2 || true

resolve_check=$(P4CLIENT="$main_client" "$p4" resolve -n 2>&1 || true)
if printf '%s\n' "$resolve_check" | grep -qiE '^no file\(s\) to resolve|^file\(s\) up-to-date'; then
    : # clean — proceed
else
    unresolved_count=$(printf '%s\n' "$resolve_check" | grep -cE '^//|must resolve|must be resolved' || true)
    if [ "${unresolved_count:-0}" -gt 0 ]; then
        echo "p4-task-stream-to-pr: ${unresolved_count} file(s) unresolved after p4 resolve -as — manual fix required" >&2
        printf '%s\n' "$resolve_check" >&2
        exit 1
    fi
fi

# --- p4 submit the integrated changes -------------------------------------
submit_desc="merge from ${stream}: ${pr_title}"
echo "p4-task-stream-to-pr: submitting integrated CL to ${parent}" >&2
submit_out=$(P4CLIENT="$main_client" "$p4" submit -d "$submit_desc" 2>&1 || true)
echo "$submit_out" >&2
if ! echo "$submit_out" | grep -qE 'Change [0-9]+ submitted'; then
    echo "p4-task-stream-to-pr: p4 submit appears to have failed (no 'Change N submitted' line)" >&2
    exit 1
fi
submit_change=$(echo "$submit_out" | sed -nE 's/.*Change ([0-9]+) submitted.*/\1/p' | tail -1)

# --- git: branch + commit + push -----------------------------------------
# After p4 submit, the on-disk client root has the integrated content; git
# now sees those as modified files.
if [ -z "$(git status --porcelain)" ]; then
    echo "p4-task-stream-to-pr: WARNING — p4 submit succeeded but git sees no diff. Skipping git/PR steps." >&2
    exit 0
fi

echo "p4-task-stream-to-pr: creating git branch ${branch}" >&2
git checkout -b "$branch" >&2
git add -A
commit_body="Mirrors Perforce change @${submit_change} (integrated from ${stream}).

Auto-generated by scripts/dev/p4-task-stream-to-pr.sh.

Co-Authored-By: ${P4USER}@p4 <noreply@anthropic.com>"
git commit -m "$pr_title" -m "$commit_body" >&2

# --- git push + gh pr create (skipped in --dry-run) ----------------------
pr_body="Auto-generated submit-to-PR bridge from Perforce task stream \`${stream}\` (p4 change @${submit_change}).

Source: \`bash scripts/dev/p4-task-stream-to-pr.sh ${agent_id} \"${pr_title}\"\`

See [docs/design/git-to-perforce-migration.md](docs/design/git-to-perforce-migration.md) § Phase 3."

if [ "$dry_run" = 1 ]; then
    echo "p4-task-stream-to-pr: DRY-RUN — skipping git push + gh pr create" >&2
    echo "  local branch ${branch} committed at $(git rev-parse --short HEAD); not pushed" >&2
    echo "  would invoke: git push -u origin ${branch}" >&2
    echo "  would invoke: gh pr create --draft --base ${base_branch} --title '${pr_title}'" >&2
    # Non-URL sentinel on stdout so tooling can distinguish dry-run from a real PR URL
    printf 'DRY-RUN:%s\n' "$branch"
    exit 0
fi

echo "p4-task-stream-to-pr: pushing ${branch} to origin" >&2
git push -u origin "$branch" >&2

echo "p4-task-stream-to-pr: opening draft PR via gh" >&2
pr_url=$("$gh" pr create --draft --base "$base_branch" --title "$pr_title" --body "$pr_body" 2>&1 || true)
if ! printf '%s' "$pr_url" | grep -qE '^https://github.com/.*pull/[0-9]+'; then
    echo "p4-task-stream-to-pr: gh pr create did not return a PR URL; check auth (gh auth status)" >&2
    echo "$pr_url" >&2
    echo "branch ${branch} is pushed; create PR manually" >&2
    exit 4
fi

# Print PR URL to stdout — caller (orchestrator / merge-watcher) picks it up
printf '%s\n' "$pr_url"

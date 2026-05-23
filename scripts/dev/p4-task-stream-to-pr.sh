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
# Modes:
#   default (one-shot) — integrate → submit → git push → gh pr create in
#                        one run. Preserves the original Phase-3 behaviour
#                        for callers that don't need a human review gate.
#   --prepare-review-cl — integrate → create a NAMED pending CL on
#                        //smatchet/main (NOT submitted) → shelve. Prints
#                        the CL number on stdout. Used by the p4-gated
#                        ship-loop (docs/design/p4-gated-ship-loop.md) so a
#                        human can review the change in P4V before submit.
#   --promote-reviewed-cl <CL> — resume from a CL prepared above. Validates
#                        the CL (exists, pending, current client, matching
#                        `task-stream-id: <agent-id>` tag), submits it,
#                        then runs the git checkout + commit + push + PR.
#                        Refuses on mismatch — prints cleanup commands.
#
# Usage:
#   bash scripts/dev/p4-task-stream-to-pr.sh <agent-id> <pr-title> [--dry-run]
#   bash scripts/dev/p4-task-stream-to-pr.sh <agent-id> <pr-title> --prepare-review-cl
#   bash scripts/dev/p4-task-stream-to-pr.sh <agent-id> <pr-title> --promote-reviewed-cl <CL> [--dry-run]
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
#               Applies to one-shot AND --promote-reviewed-cl modes.
#               Prints what the git-push + gh invocations would have been.
#   --prepare-review-cl
#               Integrate + shelve only; no submit, no git, no PR.
#   --promote-reviewed-cl <CL>
#               Submit the named pending CL + run the git/PR phase.
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
#   0 — PR opened (or --dry-run plan printed); URL on stdout. In prepare
#       mode, the pending CL number is printed on stdout.
#   1 — p4 integrate/resolve/submit failed (conflicts or server error)
#   2 — argument / environment error
#   3 — git state precondition failed (dirty tree, wrong branch, missing remote)
#   4 — gh pr create failed (or auth missing)
#   5 — promote-mode CL validation failed (stranded / mismatched / submitted)
#
# Hard rules:
#   - Refuses to run with a dirty git working tree (would conflate p4 imports
#     with unrelated user changes) — except in prepare mode, where the git
#     tree is not touched (preconditions skip the git-clean check).
#   - Refuses to run if `p4 resolve -as` leaves unresolved files (manual
#     resolution required; the orchestrator can re-invoke after fixup).
#   - On `gh pr create` failure, leaves the git branch pushed so the user
#     can open the PR manually via the printed URL.
#   - Promote mode never auto-cleans a stranded CL — prints the manual
#     `p4 shelve -d -c <CL> && p4 change -d <CL>` recipe and refuses.

set -euo pipefail

usage() {
    cat >&2 <<'USAGE'
usage:
  bash scripts/dev/p4-task-stream-to-pr.sh <agent-id> <pr-title> [--dry-run]
  bash scripts/dev/p4-task-stream-to-pr.sh <agent-id> <pr-title> --prepare-review-cl
  bash scripts/dev/p4-task-stream-to-pr.sh <agent-id> <pr-title> --promote-reviewed-cl <CL> [--dry-run]
USAGE
    exit 2
}

dry_run=0
mode="one_shot"
promote_cl=""
positional=()
while [ "$#" -gt 0 ]; do
    case "$1" in
        --dry-run) dry_run=1; shift ;;
        --prepare-review-cl)
            if [ "$mode" != "one_shot" ]; then
                echo "p4-task-stream-to-pr: --prepare-review-cl and --promote-reviewed-cl are mutually exclusive" >&2
                usage
            fi
            mode="prepare"; shift ;;
        --promote-reviewed-cl)
            if [ "$mode" != "one_shot" ]; then
                echo "p4-task-stream-to-pr: --prepare-review-cl and --promote-reviewed-cl are mutually exclusive" >&2
                usage
            fi
            mode="promote"; shift
            [ "$#" -ge 1 ] || { echo "p4-task-stream-to-pr: --promote-reviewed-cl requires a CL number" >&2; usage; }
            promote_cl="$1"; shift
            if ! printf '%s' "$promote_cl" | grep -qE '^[0-9]+$'; then
                echo "p4-task-stream-to-pr: --promote-reviewed-cl CL must be a positive integer ('$promote_cl')" >&2
                exit 2
            fi
            ;;
        -h|--help) awk 'NR==1{next} /^set -euo/{exit} {sub(/^# ?/,"")} {print}' "$0"; exit 0 ;;
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
task_tag="task-stream-id: ${agent_id}"

# --- branch slug derivation -----------------------------------------------
# kebab-case from pr-title: lowercase, non-alnum → -, collapse runs, trim ends, cap 64
slug=$(printf '%s' "$pr_title" \
    | tr '[:upper:]' '[:lower:]' \
    | sed -e 's/[^a-z0-9]/-/g' -e 's/-\{1,\}/-/g' -e 's/^-//' -e 's/-$//' \
    | cut -c1-64)
[ -n "$slug" ] || { echo "p4-task-stream-to-pr: derived empty slug from pr-title" >&2; exit 2; }
branch="agent/${agent_id}/${slug}"

# --- precondition: p4 server reachable ------------------------------------
# (Run before the git-clean check so failures surface fast.)
if ! "$p4" info >/dev/null 2>&1; then
    echo "p4-task-stream-to-pr: cannot reach p4 server at ${P4PORT}" >&2; exit 1
fi
# Task-stream existence is required for one-shot + prepare (both integrate
# from the stream). Promote-mode is the resume path: the integration
# already happened during prepare, and the task stream may have been GC'd
# in the interim. Skip the check for promote-mode so stranded-CL recovery
# works even after the task stream is gone.
if [ "$mode" != "promote" ]; then
    if ! "$p4" streams "$stream" 2>/dev/null | grep -q "Stream ${stream}"; then
        echo "p4-task-stream-to-pr: task stream ${stream} not found (allocate with p4-task-stream.sh first)" >&2
        exit 2
    fi
fi

# --- precondition: git state checks --------------------------------------
# Two independent preconditions, used in different combinations per mode:
#   require_git_on_base_at_origin — on develop + HEAD == origin/develop
#   require_git_clean_tree        — `git status --porcelain` empty
#
# One-shot needs both: tree must be clean (no concurrent uncommitted user
# work) AND HEAD aligned with origin (so the branch we cut from is current).
#
# Promote skips clean-tree: prepare INTENTIONALLY leaves the workspace
# dirty (modified files opened on the pending CL on //smatchet/main are
# also visible to git as a modified working tree). The dirty state IS the
# change we're about to ship; requiring clean would refuse the legitimate
# flow.
#
# Prepare skips both: workspace is going to be modified anyway, and the
# loop is designed to be interruptible by other git-side activity between
# slices.
require_git_repo() {
    repo_root="$(git rev-parse --show-toplevel 2>/dev/null)" || {
        echo "p4-task-stream-to-pr: not inside a git checkout" >&2; exit 3
    }
    cd "$repo_root"
}

require_git_on_base_at_origin() {
    require_git_repo
    current_branch=$(git rev-parse --abbrev-ref HEAD)
    if [ "$current_branch" != "$base_branch" ]; then
        echo "p4-task-stream-to-pr: must be on '${base_branch}' (currently on '${current_branch}')" >&2
        exit 3
    fi
    git fetch origin "$base_branch" --quiet || {
        echo "p4-task-stream-to-pr: git fetch origin ${base_branch} failed" >&2; exit 3
    }
    local local_head remote_head
    local_head=$(git rev-parse HEAD)
    remote_head=$(git rev-parse "origin/${base_branch}")
    if [ "$local_head" != "$remote_head" ]; then
        echo "p4-task-stream-to-pr: local HEAD (${local_head:0:7}) differs from origin/${base_branch} (${remote_head:0:7}) — pull first" >&2
        exit 3
    fi
}

require_git_clean_tree() {
    require_git_repo
    if [ -n "$(git status --porcelain)" ]; then
        echo "p4-task-stream-to-pr: git working tree is dirty — commit or stash before re-running" >&2
        git status --short >&2
        exit 3
    fi
}

# --- helper: count pending CLs on main client ----------------------------
# Two-step counting (NOT `... | wc -l || echo 0`): under `set -o pipefail`,
# the fallback `echo 0` only fires if the WHOLE pipeline fails. When p4
# succeeds but produces zero lines, the captured value is the wc-l output
# alone ("0"). When p4 FAILS, the value becomes "wc-output\n0" — multi-line,
# which breaks the integer test downstream with "integer expression expected".
count_pending_on_main() {
    if pending_out=$("$p4" changes -s pending -c "$main_client" 2>/dev/null); then
        printf '%s' "$pending_out" | grep -c . || true
    else
        echo 0
    fi
}

# --- helper: integrate task stream → main (copy, fallback to merge) ------
# Leaves files opened on the default change of the main client. Caller
# decides whether to submit (one-shot), or move to a named CL + shelve
# (prepare).
integrate_task_stream() {
    local stream_name="${stream##*/}"
    echo "p4-task-stream-to-pr: p4 copy --from ${stream_name} (into ${parent} via ${main_client})" >&2
    local copy_out
    copy_out=$(P4CLIENT="$main_client" "$p4" copy --from "$stream_name" 2>&1 || true)
    echo "$copy_out" >&2

    if echo "$copy_out" | grep -qiE "needs 'merge' not 'copy'"; then
        echo "p4-task-stream-to-pr: copy refused (mainline moved) — retrying as merge" >&2
        local merge_out
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

    local resolve_check
    resolve_check=$(P4CLIENT="$main_client" "$p4" resolve -n 2>&1 || true)
    if printf '%s\n' "$resolve_check" | grep -qiE '^no file\(s\) to resolve|^file\(s\) up-to-date'; then
        return 0
    fi
    local unresolved_count
    unresolved_count=$(printf '%s\n' "$resolve_check" | grep -cE '^//|must resolve|must be resolved' || true)
    if [ "${unresolved_count:-0}" -gt 0 ]; then
        echo "p4-task-stream-to-pr: ${unresolved_count} file(s) unresolved after p4 resolve -as — manual fix required" >&2
        printf '%s\n' "$resolve_check" >&2
        exit 1
    fi
}

# --- helper: do the git branch / commit / push / gh pr create dance ------
# Used by one-shot and promote modes after a successful p4 submit. Param 1
# is the p4 submit change number (for the commit body reference).
ship_as_pr() {
    local submit_change="$1"

    # After p4 submit, the on-disk client root has the integrated content;
    # git now sees those as modified files (or no diff if everything was
    # already mirrored).
    if [ -z "$(git status --porcelain)" ]; then
        echo "p4-task-stream-to-pr: WARNING — p4 submit succeeded but git sees no diff. Skipping git/PR steps." >&2
        return 0
    fi

    echo "p4-task-stream-to-pr: creating git branch ${branch}" >&2
    git checkout -b "$branch" >&2
    git add -A
    local commit_body="Mirrors Perforce change @${submit_change} (integrated from ${stream}).

Auto-generated by scripts/dev/p4-task-stream-to-pr.sh.

Co-Authored-By: ${P4USER}@p4 <noreply@anthropic.com>"
    git commit -m "$pr_title" -m "$commit_body" >&2

    local pr_body="Auto-generated submit-to-PR bridge from Perforce task stream \`${stream}\` (p4 change @${submit_change}).

Source: \`bash scripts/dev/p4-task-stream-to-pr.sh ${agent_id} \"${pr_title}\"\`

See [docs/design/git-to-perforce-migration.md](docs/design/git-to-perforce-migration.md) § Phase 3."

    if [ "$dry_run" = 1 ]; then
        echo "p4-task-stream-to-pr: DRY-RUN — skipping git push + gh pr create" >&2
        echo "  local branch ${branch} committed at $(git rev-parse --short HEAD); not pushed" >&2
        echo "  would invoke: git push -u origin ${branch}" >&2
        echo "  would invoke: gh pr create --draft --base ${base_branch} --title '${pr_title}'" >&2
        printf 'DRY-RUN:%s\n' "$branch"
        return 0
    fi

    echo "p4-task-stream-to-pr: pushing ${branch} to origin" >&2
    git push -u origin "$branch" >&2

    echo "p4-task-stream-to-pr: opening draft PR via gh" >&2
    local pr_url
    pr_url=$("$gh" pr create --draft --base "$base_branch" --title "$pr_title" --body "$pr_body" 2>&1 || true)
    if ! printf '%s' "$pr_url" | grep -qE '^https://github.com/.*pull/[0-9]+'; then
        echo "p4-task-stream-to-pr: gh pr create did not return a PR URL; check auth (gh auth status)" >&2
        echo "$pr_url" >&2
        echo "branch ${branch} is pushed; create PR manually" >&2
        exit 4
    fi
    printf '%s\n' "$pr_url"
}

# =========================================================================
# Mode dispatch
# =========================================================================

case "$mode" in
    one_shot)
        require_git_clean_tree
        require_git_on_base_at_origin
        pending_on_main=$(count_pending_on_main)
        if [ "${pending_on_main:-0}" -gt 0 ]; then
            echo "p4-task-stream-to-pr: ${main_client} has ${pending_on_main} pending CL(s); leftover from prior run?" >&2
            "$p4" changes -s pending -c "$main_client" >&2
            echo "p4-task-stream-to-pr: clean up with: p4 revert -c <cl> ${parent}/... && p4 change -d <cl>" >&2
            exit 1
        fi

        integrate_task_stream

        # Submit the integrated changes via the default change.
        submit_desc="merge from ${stream}: ${pr_title}"
        echo "p4-task-stream-to-pr: submitting integrated CL to ${parent}" >&2
        submit_out=$(P4CLIENT="$main_client" "$p4" submit -d "$submit_desc" 2>&1 || true)
        echo "$submit_out" >&2
        if ! echo "$submit_out" | grep -qE 'Change [0-9]+ submitted'; then
            echo "p4-task-stream-to-pr: p4 submit appears to have failed (no 'Change N submitted' line)" >&2
            exit 1
        fi
        submit_change=$(echo "$submit_out" | sed -nE 's/.*Change ([0-9]+) submitted.*/\1/p' | tail -1)

        ship_as_pr "$submit_change"
        ;;

    prepare)
        # Prepare mode: integrate + create a named pending CL + shelve.
        # No git touch — orchestrator iterates the shelf with the user
        # before calling --promote-reviewed-cl.
        pending_on_main=$(count_pending_on_main)
        if [ "${pending_on_main:-0}" -gt 0 ]; then
            echo "p4-task-stream-to-pr: ${main_client} has ${pending_on_main} pending CL(s) — resolve before --prepare-review-cl" >&2
            "$p4" changes -s pending -c "$main_client" >&2
            echo "p4-task-stream-to-pr: if leftover from prior run, clean up with: p4 shelve -d -c <cl>; p4 revert -c <cl> ${parent}/...; p4 change -d <cl>" >&2
            exit 1
        fi

        integrate_task_stream

        # Build the change spec for a new pending CL. p4 change -o emits
        # a form with `Description:` followed by a placeholder line. We
        # replace the placeholder block with the PR title + task-stream-id
        # tag (the tag is the contract that promote-mode validates against).
        #
        # H4 defensive: pass pr_title / task_tag / desc_body via ENV +
        # ENVIRON["..."] rather than `-v t="$pr_title"`. The -v form is
        # safe under bash's `"$var"` quoting for the simple cases that
        # land here today, but env-var pass is robust against:
        #   - awk's `-v` escape-sequence interpretation of `\n`/`\t`/`\\`
        #     (which would silently mangle a title containing a literal
        #     backslash);
        #   - any future caller that forgets the outer `"$..."` quotes
        #     when invoking the script (the env pattern stays correct
        #     even if the calling site degrades).
        # Env-name choice: SMATCHET_PT_* prefix (not bare PR_TITLE/TASK_TAG)
        # to avoid colliding with whatever the caller happens to have in
        # their env. The export is scoped to this single awk invocation.
        desc_first="${pr_title}"
        desc_body="Integrated from ${stream} — awaiting shelf review (no submit yet)."
        # CR feedback on PR #423: the env-var prefix above the pipe only sets
        # vars on `p4 change -o` (the LHS), not on `awk` (the RHS). In bash,
        # `FOO=bar cmd1 | cmd2` scopes FOO to cmd1 only — awk's ENVIRON[FOO]
        # would have been empty, dropping Description/tag/body entirely. Fix:
        # put the env-prefix directly on `awk`, where it's read.
        new_cl_out=$(
            P4CLIENT="$main_client" "$p4" change -o \
            | SMATCHET_PT_TITLE="$desc_first" \
              SMATCHET_PT_TAG="$task_tag" \
              SMATCHET_PT_BODY="$desc_body" \
              awk '
                /^Description:/ {
                    print
                    # Tab-indented body per p4 change-spec format. Trailing
                    # blank line keeps the standard p4 section separator
                    # between Description and the next field (Files: / Jobs:).
                    printf "\t%s\n\n\t%s\n\n\t%s\n\n",
                          ENVIRON["SMATCHET_PT_TITLE"],
                          ENVIRON["SMATCHET_PT_TAG"],
                          ENVIRON["SMATCHET_PT_BODY"]
                    in_desc=1
                    next
                }
                in_desc && /^[ \t]/ { next }
                in_desc && /^$/ { next }
                in_desc && /^[A-Z]/ { in_desc=0; print; next }
                !in_desc { print }
            ' | P4CLIENT="$main_client" "$p4" change -i 2>&1)
        echo "$new_cl_out" >&2

        new_cl=$(printf '%s\n' "$new_cl_out" | sed -nE 's/.*Change ([0-9]+) created.*/\1/p' | tail -1)
        if [ -z "$new_cl" ]; then
            echo "p4-task-stream-to-pr: failed to create named pending CL on ${main_client}" >&2
            exit 1
        fi

        # Move opened files from default change → new named CL.
        echo "p4-task-stream-to-pr: reopening default-change files into CL ${new_cl}" >&2
        P4CLIENT="$main_client" "$p4" reopen -c "$new_cl" "${parent}/..." >&2 || {
            echo "p4-task-stream-to-pr: p4 reopen failed; pending CL ${new_cl} stranded" >&2
            exit 1
        }

        # Shelve the named pending CL.
        echo "p4-task-stream-to-pr: shelving CL ${new_cl}" >&2
        shelve_out=$(P4CLIENT="$main_client" "$p4" shelve -c "$new_cl" 2>&1 || true)
        echo "$shelve_out" >&2
        if ! printf '%s\n' "$shelve_out" | grep -qE 'Change [0-9]+ files? shelved|shelved'; then
            echo "p4-task-stream-to-pr: p4 shelve produced no recognisable 'shelved' line; verify manually" >&2
        fi

        # Print CL number on stdout — caller (orchestrator) picks it up
        # and passes to --promote-reviewed-cl <CL> later.
        printf '%s\n' "$new_cl"
        ;;

    promote)
        # Promote mode: validate the named pending CL, submit it, then
        # run the git/PR phase. Refuses on any validation mismatch.
        cl_spec=$(P4CLIENT="$main_client" "$p4" change -o "$promote_cl" 2>&1) || {
            echo "p4-task-stream-to-pr: CL ${promote_cl} does not exist (or is inaccessible)" >&2
            exit 5
        }

        cl_status=$(printf '%s\n' "$cl_spec" | awk '/^Status:/ { print $2; exit }')
        if [ "$cl_status" != "pending" ]; then
            echo "p4-task-stream-to-pr: CL ${promote_cl} status is '${cl_status}', not 'pending' — refusing to promote" >&2
            exit 5
        fi

        cl_client=$(printf '%s\n' "$cl_spec" | awk '/^Client:/ { print $2; exit }')
        if [ "$cl_client" != "$main_client" ]; then
            echo "p4-task-stream-to-pr: CL ${promote_cl} belongs to client '${cl_client}', not '${main_client}'" >&2
            exit 5
        fi

        if ! printf '%s\n' "$cl_spec" | grep -qF "$task_tag"; then
            echo "p4-task-stream-to-pr: CL ${promote_cl} description missing tag '${task_tag}' — refusing to promote" >&2
            echo "p4-task-stream-to-pr: if this CL is stranded from a different agent-id, clean up manually with:" >&2
            echo "    p4 shelve -d -c ${promote_cl} && p4 revert -c ${promote_cl} ${parent}/... && p4 change -d ${promote_cl}" >&2
            exit 5
        fi

        # Validation passed — verify git side is on base + at-origin (but
        # NOT clean: prepare's integrate left modified files in the
        # workspace, and those ARE the change we're shipping).
        require_git_on_base_at_origin

        echo "p4-task-stream-to-pr: submitting reviewed CL ${promote_cl}" >&2
        submit_out=$(P4CLIENT="$main_client" "$p4" submit -c "$promote_cl" 2>&1 || true)
        echo "$submit_out" >&2
        if ! printf '%s\n' "$submit_out" | grep -qE 'Change [0-9]+ submitted|renamed change [0-9]+ and submitted'; then
            echo "p4-task-stream-to-pr: p4 submit -c ${promote_cl} appears to have failed (no submitted line)" >&2
            exit 1
        fi
        # p4 either keeps the CL number ("Change N submitted.") or renames
        # it ("Change N renamed change M and submitted."). Extract the
        # final number for the commit reference.
        submit_change=$(printf '%s\n' "$submit_out" \
            | sed -nE 's/.*Change [0-9]+ renamed change ([0-9]+) and submitted.*/\1/p; s/.*Change ([0-9]+) submitted.*/\1/p' \
            | tail -1)
        [ -n "$submit_change" ] || submit_change="$promote_cl"

        ship_as_pr "$submit_change"
        ;;
esac

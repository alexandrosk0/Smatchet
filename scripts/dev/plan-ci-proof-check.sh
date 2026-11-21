#!/usr/bin/env bash
# scripts/dev/plan-ci-proof-check.sh — refuse a branch whose plan names a CI check as
# its proof on the PR when that check will skip (or did skip) on the PR.
#
# A plan's `## Verification` can name a CI check as the proof for the change:
#     - CI: "Sanitizer (UBSan via Clang)" green on this PR ...
# A delta-gated check that skips on the PR concludes `skipped`, and a skipped required
# check is a pass to branch protection and to merge-gates.sh, so the proof the plan
# asked for silently never runs. #2400 is the case: its plan asked for exactly that
# check green "on this PR", the Core-delta gate skipped the lane on #2400, and the lane
# first ran (and crashed) on three unrelated PRs (infra 2026-11-12-delta-gated-lane-
# skips-the-pr-that-reconfigures-it, action 4).
#
# Usage:
#   bash scripts/dev/plan-ci-proof-check.sh [--live] [<base-ref>]   # default origin/develop
#
# Scope: proof lines this branch ADDS to a plan under docs/plans/{active,shipped}/, vs
# the merge-base with <base-ref> (committed or not). "This PR" in a plan means the PR
# that wrote the line, so an unchanged proof line is not re-checked: a later docs-only
# edit of the plan, or its git mv to shipped/ (rename detection, -M), does not inherit
# the original PR's proof. Only the `## Verification` section is read (not
# `## Verification (actual)`, which records what happened). A named check counts when
# the line reads `CI: "<check>" green on this PR` (or `the PR`; the name may be in
# backticks instead of quotes).
#
# Two checks per named CI check:
#   predicted  For a check build-and-test.yml gates on a `changes` output, run the same
#              classifier the workflow runs (scripts/ci/detect-changes.sh) over
#              <base-ref>...HEAD. If that output is false, the check will skip: FAIL.
#              Offline, pure git. A check with no known gate is reported, not failed.
#   --live     Also read the open PR's head check runs through `gh pr view`; a named
#              check that concluded SKIPPED: FAIL. Off by default: before a push the PR
#              head is the previous push, and the prediction above is what the next run
#              will do. No gh / no PR: reported, not failed.
#
# Exit codes:
#   0 — no changed plan names a CI proof, or every named proof will run (and did not skip)
#   1 — a named proof check will skip / did skip on this PR
#   2 — usage error
set -uo pipefail

live=0
base_ref=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --live) live=1 ;;
        -h|--help)
            sed -n '2,36p' "$0"
            exit 0
            ;;
        -*)
            echo "plan-ci-proof-check: unknown option $1" >&2
            exit 2
            ;;
        *)
            [ -z "$base_ref" ] || { echo "plan-ci-proof-check: one <base-ref> only" >&2; exit 2; }
            base_ref="$1"
            ;;
    esac
    shift
done
base_ref="${base_ref:-origin/develop}"

script_dir="$(cd "$(dirname "$0")" && pwd -P)"
detect="$script_dir/../ci/detect-changes.sh"

# gate_output_for <check-name> — the `changes` output build-and-test.yml gates the check
# on, or nothing when the check is not delta-gated (or not known to this script).
# Keep in step with the `if:` lines of those jobs.
gate_output_for() {
    case "$1" in
        "Sanitizer (UBSan via Clang)"|"Sanitizer (ASAN via MSVC)") echo source_core_cpp ;;
        "Windows + MSVC"|"Comment-noise + high-integrity gate") echo code ;;
    esac
}

# verification_checks <plan-file> <added-lines> — print each check name the
# `## Verification` section names as proof on the PR, one per line, keeping only lines
# whose number is in <added-lines> (space-separated; "all" keeps every line).
# shellcheck disable=SC2016  # awk / sed programs; the backticks are literal
verification_checks() {
    awk -v added=" $2 " '
        /^## / { in_v = ($0 ~ /^## Verification[[:space:]]*$/); next }
        in_v && (added == " all " || index(added, " " NR " ")) { print }
    ' "$1" | sed -nE 's/.*CI:[[:space:]]*["`]([^"`]+)["`][[:space:]]+green on (this|the) PR.*/\1/p'
}

# added_lines <plan-file> — the head-side line numbers this branch adds to <plan-file>
# (space-separated), from the -U0 hunk headers of the rename-aware diff computed once
# below; "all" for an untracked plan.
added_lines() {
    if grep -qxF -- "$1" <<< "$untracked"; then
        echo all
        return 0
    fi
    printf '%s\n' "$plan_diff" | awk -v want="$1" '
        /^\+\+\+ / { path = substr($0, 5); sub(/^b\//, "", path); next }
        /^@@ / && path == want {
            n = $3; sub(/^\+/, "", n)
            split(n, p, ",")
            count = (2 in p) ? p[2] + 0 : 1
            for (l = p[1] + 0; l < p[1] + count; l++) printf "%d ", l
        }
    '
}

if ! merge_base="$(git merge-base "$base_ref" HEAD 2>/dev/null)"; then
    echo "plan-ci-proof-check: WARN — no merge-base with $base_ref; cannot tell which plans this branch changes (skipped)." >&2
    exit 0
fi

# The patch text is parsed below, so its format is pinned against user git config
# (color.diff=always, diff.mnemonicPrefix's w/ prefix, an external diff driver): a
# reshaped patch would match no path and silently pass every plan.
GIT_DIFF=(git -c core.quotePath=false diff --no-color --no-ext-diff --src-prefix=a/ --dst-prefix=b/ -M --diff-filter=d)
plans="$("${GIT_DIFF[@]}" --name-only "$merge_base" -- 'docs/plans/active/*.md' 'docs/plans/shipped/*.md' 2>/dev/null || true)"
plan_diff="$("${GIT_DIFF[@]}" -U0 "$merge_base" -- 'docs/plans/active/*.md' 'docs/plans/shipped/*.md' 2>/dev/null || true)"
untracked="$(git ls-files --others --exclude-standard -- 'docs/plans/active/*.md' 'docs/plans/shipped/*.md' 2>/dev/null || true)"
plans="$(printf '%s\n%s\n' "$plans" "$untracked" | sed '/^$/d' | sort -u)"

if [ -z "$plans" ]; then
    echo "plan-ci-proof-check: no changed plan doc — nothing to check."
    exit 0
fi

# Both lookups are filled once, in this shell (a fill inside $(...) would be lost), and
# only when a plan names a check that needs them.
predicted=""
predicted_state="unread"
ensure_predicted() {
    [ "$predicted_state" = "unread" ] || return 0
    predicted="$(bash "$detect" "${base_ref}...HEAD" 2>/dev/null)"
    predicted_state="read"
}
predicted_value() {
    printf '%s\n' "$predicted" | sed -n "s/^$1=//p"
}

live_rollup=""
live_state="unread"
ensure_live() {
    [ "$live_state" = "unread" ] || return 0
    if command -v gh >/dev/null 2>&1 &&
        live_rollup="$(gh pr view --json statusCheckRollup \
            --jq '.statusCheckRollup[] | [.name, (.conclusion // .state // "")] | @tsv' 2>/dev/null)"; then
        live_state="read"
    else
        live_state="unavailable"
    fi
}
live_conclusion() {
    [ "$live_state" = "read" ] || return 1
    printf '%s\n' "$live_rollup" | awk -F '\t' -v name="$1" '$1 == name { print $2; exit }'
}

fail=0
named=0
while IFS= read -r plan; do
    [ -f "$plan" ] || continue
    while IFS= read -r check; do
        [ -n "$check" ] || continue
        named=$((named + 1))
        gate="$(gate_output_for "$check")"
        if [ -n "$gate" ]; then
            ensure_predicted
            value="$(predicted_value "$gate")"
            if [ "$value" = "false" ]; then
                cat >&2 <<EOF
plan-ci-proof-check: FAIL — $plan names "$check" green on this PR as its proof,
  but build-and-test.yml gates that check on changes.$gate, and this diff sets
  $gate=false: the check will conclude skipped, and a skipped check is a pass.
  Either make the diff run the check (scripts/ci/detect-changes.sh holds the
  trigger set; a lane-config path missing there is the bug to fix), or change the
  plan's proof to one that runs (e.g. a workflow_dispatch of sanitizer-nightly.yml
  on this branch).
EOF
                fail=1
            else
                echo "plan-ci-proof-check: $plan — \"$check\" will run on this PR ($gate=${value:-unknown})."
            fi
        else
            echo "plan-ci-proof-check: $plan — \"$check\" is not a known delta-gated check; not predicted."
        fi
        if [ "$live" -eq 1 ]; then
            ensure_live
            if conclusion="$(live_conclusion "$check")"; then
                case "$conclusion" in
                    SKIPPED|skipped)
                        echo "plan-ci-proof-check: FAIL — \"$check\" concluded SKIPPED on the PR head; the proof $plan asks for did not run." >&2
                        fail=1
                        ;;
                    "") echo "plan-ci-proof-check: \"$check\" has not reported on the PR head yet." ;;
                    *) echo "plan-ci-proof-check: \"$check\" on the PR head: $conclusion." ;;
                esac
            else
                echo "plan-ci-proof-check: no PR head readable through gh; live check skipped." >&2
            fi
        fi
    done < <(verification_checks "$plan" "$(added_lines "$plan")")
done <<< "$plans"

if [ "$fail" -ne 0 ]; then
    exit 1
fi
echo "plan-ci-proof-check: PASS — $named CI proof(s) named by changed plans; none will skip."
exit 0

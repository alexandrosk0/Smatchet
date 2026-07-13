#!/usr/bin/env bash
# agents/scripts/core/merge-gates.d/00-common.sh
# ----------------------------------------------------------------------------
# Shared header constants + top-level helpers for merge-gates.sh.
#
# Sourced by the merge-gates.sh entry point (via the explicit load list) AFTER
# it has defined SCRIPT_DIR. Behaviour-preserving relocation of the former
# top-level block: the meant-to-block allow-list constant, the prompt-shim
# lazy-source, and the standalone gh_pr_ready_idempotent function. No logic
# change — this is a pure split of the monolith (mirrors lint-rules.d/00-common.sh).
# ----------------------------------------------------------------------------

# ----------------------------------------------------------------------------
# Meant-to-block scope — SINGLE SOURCE OF TRUTH.
# BLOCK-ON-ANY-RED (all-gates-blocking flip): the regex matches EVERY check
# name, so any non-required check that reaches a failing terminal state blocks
# the merge exactly like a required one, and any still-pending check holds
# GATES_PASSED. This is the mechanised form of the AGENTS.md invariant
# "Never merge past ANY red check — required or not" (previously curated:
# Coverage|Sanitizer|Perf PR-fast|Android security gate|Fuzz smoke|
# Bucket launch-smoke|Intent section|Plan-lock gate, grown one #923-class
# gate-escape at a time — this flip retires the curation).
# The ONE remaining exemption is the `advisory`-NAME exclusion in the jq below:
# a check whose name literally contains "advisory" does not block. After the
# all-gates-blocking rename sweep NO lane carries that token; it survives as the
# sanctioned, name-visible convention for any future deliberately-advisory lane
# (an invisible poller-side list is exactly what this flip removes).
# Prereqs that made block-all safe (each lane genuinely green first):
#  * emulator smoke — cold-boot fix removed the ~23% snapshot-restore boot race;
#  * Android NDK — httplib zstd auto-detect pinned OFF (#1604, hermetic);
#  * fuzz smoke — the stochastic fuzz STEP stays continue-on-error on PRs
#    (#1301 design) so only the deterministic build/ctest reds the check;
#  * bucket-C — the per-scenario golden-diff STEP stays advisory-by-design
#    (per-developer GPU goldens; lane-integrity + launch-smoke carry the teeth),
#    so the CHECK reds only on real infra/dead-harness breakage.
# Override hatches, unchanged: tests-/perf-/intent-/plan-lock-out-of-band labels
# downgrade their named checks; SKIP_MERGE_GATES=true is the global bypass.
# Other tooling applies the IDENTICAL scope by sourcing this file
# (safe-admin-merge.sh, postmortem-owed.sh) — change it HERE and every consumer
# follows.
# shellcheck disable=SC2034  # spliced into the GATE_FILTER by the entry point (cross-file).
MERGE_GATES_BLOCK_ALLOWLIST_RE="."

# Source prompt shim so `ask_user_question` is callable from the caller's
# integration flow. Lazy — only if available.
if [ -f "$SCRIPT_DIR/merge-gates-prompt.sh" ]; then
    # shellcheck source=agents/scripts/core/merge-gates-prompt.sh
    source "$SCRIPT_DIR/merge-gates-prompt.sh"
fi

# ----------------------------------------------------------------------------
# gh_pr_ready_idempotent <pr_number>
# ----------------------------------------------------------------------------
# H2: positive-check fallback. `gh pr ready` returns non-zero with an English
# stderr message ("not in draft state" / "already marked ready") when called
# against an already-non-draft PR. Matching on English text breaks if `gh`
# updates its wording, ships a localised build, or the user is on a locale-
# overridden CLI. Fall back to a positive state probe via
# `gh pr view --json isDraft`: if the PR is observably non-draft, the
# original `gh pr ready` failure was the benign "already ready" case and we
# can return 0. Any other failure surfaces as exit 6.
gh_pr_ready_idempotent() {
    local prNumber="${1:?gh_pr_ready_idempotent: pr_number required}"
    local out
    if ! out=$(gh pr ready "$prNumber" 2>&1); then
        # Fast path — known English phrases. Cheaper than the extra API call
        # and preserves backward compatibility with the prior contract.
        case "$out" in
            *"not in draft state"*|*"already marked ready"*)
                return 0
                ;;
        esac
        # Positive-check fallback: probe the PR's actual draft state. If it's
        # already non-draft, the `gh pr ready` failure was benign. Robust
        # against `gh` wording changes + locale variation + CLI version drift.
        local is_draft
        if is_draft=$(gh pr view "$prNumber" --json isDraft --jq .isDraft 2>/dev/null); then
            if [ "$is_draft" = "false" ]; then
                return 0
            fi
        fi
        echo "$out" >&2
        return 6
    fi
}

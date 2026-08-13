#!/usr/bin/env bash
# agents/scripts/core/merge-gates.sh
# ----------------------------------------------------------------------------
# Merge-gates poller for the orchestrator + git-janitor ship-loop.
#
# Polls four conditions on a PR via one `gh api graphql` call:
#   1. CI — every required check passes (CheckRun terminal SUCCESS/NEUTRAL/SKIPPED;
#      StatusContext state == SUCCESS) PLUS every non-required check whose name
#      does not contain "advisory" (block-on-any-red; the all-gates-blocking
#      flip of the former curated allow-list) — see $failing below +
#      postmortems.md 2026-06-06 "#923".
#   2. CodeRabbit — latest review on current headRefOid is not CHANGES_REQUESTED;
#      zero unresolved non-outdated review threads contain a CodeRabbit comment.
#      A CR rate-limit skip (TEMPORARY, distinct from a terminal "Review skipped")
#      auto-downgrades to WARN on a PURE-DOCS PR (no label) but PAUSES a CODE PR
#      for CR re-review (PR-2). See the rate-limit handling block + the
#      cr-disposition label note below.
#   3. User comments — zero unresolved non-outdated review threads with any
#      non-bot non-self comment; zero conversation-tab comments from non-bot
#      non-self authors
#   4. Cursor Bugbot (cursor[bot]) — zero unresolved non-outdated review threads
#      authored by cursor[bot] (its line-anchored findings). Bugbot summary
#      reviews are always COMMENTED, so the gate guards on open findings only;
#      a "couldn't run"/"usage limit" status comment, a silent Bugbot, or a
#      stale prior-commit review never wedge merge (no-wedge + grace hatches).
#
# Plus: pullRequest.state == OPEN, reviewDecision in {APPROVED, null},
# all connection pageInfo.hasNextPage == false.
#
# Rollup dedup: required CheckRuns with the same `.name` are deduped to the
# entry with the latest `.startedAt` so stale FAILUREs from rerun jobs don't
# falsely block. StatusContexts are deduped by `.context` (GitHub overwrites).
#
# Per-PR label overrides (AGENTS.md § Merge gates § Per-PR overrides):
#   tests-out-of-band → downgrades `Test-delta gate` FAIL → WARN
#   perf-out-of-band  → downgrades `Perf PR-fast (...)` FAIL → WARN
#   intent-out-of-band → downgrades `Intent section` FAIL → WARN
#   plan-lock-out-of-band → downgrades `Plan-lock gate` FAIL → WARN
#   cr-out-of-band    → downgrades a CodeRabbit block → WARN (CR gate only;
#                       CI + user-comment gates still bind). REQUIRES a paired
#                       `cr-disposition:<reason>` attestation (label OR PR-body
#                       marker) — cr-out-of-band ALONE is NOT honoured (PR-3
#                       cr-out-of-band-disposition-trail). The CR rate-limit skip
#                       on a CODE PR is the original case of this rule (PR-2
#                       cr-rate-limit-code-pr-auto-pause).
#   cr-disposition:*  → operator attestation (a `cr-disposition:`-prefixed label
#                       OR a `cr-disposition:<reason>` marker line in the PR body)
#                       paired with cr-out-of-band to record WHY CR review was
#                       waived. Mandatory for every cr-out-of-band downgrade.
#   bugbot-out-of-band → downgrades a Cursor Bugbot block → WARN (Bugbot gate
#                       only; CI + CR + user-comment gates still bind)
# Downgraded failures are logged on stderr but do NOT contribute to ci_fail
# (CI downgrades) / do NOT block the CR gate (cr-out-of-band) / do NOT block
# the Bugbot gate (bugbot-out-of-band).
#
# Auto-exemption (NO label): a PR whose diff is ENTIRELY under
# docs/self-improvement/** (the agent system's own backlog / postmortem ledger)
# auto-skips the CR (#2) + Bugbot (#4) gates — CR via the .coderabbit.yaml
# path_filter ("Review skipped" fast-pass) plus a belt-and-suspenders downgrade
# here, Bugbot via the $selfImpOnly tuple field (27). CI (#1) + user-comment (#3)
# gates still bind. This stops the bots *blocking* merge; stopping Bugbot from
# *posting* needs a Cursor-dashboard path-ignore (no in-repo Bugbot config). See
# plan docs/plans/self-improvement-pr-review-exemption.md.
#
# Usage:
#   source agents/scripts/core/merge-gates.sh
#   poll_merge_gates <owner> <repo> <pr_number>
# OR:
#   agents/scripts/core/merge-gates.sh <owner> <repo> <pr_number>
#
# Env knobs:
#   ORCH_USER                    — orchestrator GitHub login (required)
#   MERGE_GATES_POLL_INTERVAL    — seconds between polls (default 60)
#   MERGE_GATES_MAX_POLLS        — max poll count (default 90; raised from 60 at the all-gates-blocking flip — the pending-hold now spans every lane incl. the 45-min bucket-E cap)
#   MERGE_GATES_TIMEOUT_SECONDS  — wall-clock budget (default derives from MAX_POLLS x POLL_INTERVAL, i.e. 5400 at the 90x60 defaults, so the two knobs can't cap each other)
#   MERGE_GATES_QUERY_FILE       — override GraphQL document path
#   MERGE_GATES_FLIP_READY       — when "true", flip PR ready-for-review at
#                                  poll start (authorized-merge callers only)
#   MERGE_GATES_REQUIRED_CONTEXTS — override the branch-protection required-context
#                                  set (newline- or comma-separated names). When
#                                  set (even to ""), bypasses the project.config.json
#                                  read. Empty → the required-absent detector is
#                                  inert. Used by tests to inject a fixture-matching
#                                  set; operationally rarely needed.
#   MERGE_GATES_CONFIG_FILE      — override path to project.config.json for the
#                                  required-context read (default: repo-root config).
#   MERGE_GATES_IGNORE_MERGESTATE — when "true", skip the mergeStateStatus guard so
#                                  GATES_PASSED is NOT refused on BLOCKED/BEHIND.
#                                  Default unset/false → enforce the block. Set true
#                                  for the documented admin-merge escape (AGENTS.md
#                                  § Merge gates: a positively-confirmed STALE-BLOCKED
#                                  PR where everything is actually green).
#   MERGE_GATES_FRESHNESS        — gate-logic self-freshness guard. off |
#                                  warn (default) | block. When "block", refuse GATES_PASSED if
#                                  THIS script's on-disk blob differs from
#                                  origin/develop:agents/scripts/core/merge-gates.sh
#                                  (fail-closed) — an unattended merger running a
#                                  STALE checkout would otherwise enforce out-of-date
#                                  gate logic (the #1428 gate escape: a host tree
#                                  parked behind develop merged past a RED non-required
#                                  "Intent section" its old allow-list lacked).
#                                  smatchet-merge-watcher sets "block"; "warn" (the
#                                  default) prints the divergence without blocking;
#                                  "off" disables the check entirely.
#                                  Default is "warn", NOT "off"
#                                  (gate-tooling-run-from-stale-session-branch, process
#                                  P1): the failure mode is a HUMAN-invoked poll out of a
#                                  long-lived session tree, where nothing in the output
#                                  distinguishes a stale-script BLOCK from a real one. A
#                                  7-week-old copy hard-blocked PR #1953 on a CR rule
#                                  that current develop auto-exempts, and the phantom
#                                  block was written up as a product-gate defect before a
#                                  fresh-worktree re-run disproved it. Defaulting to off
#                                  meant only the watcher ever got the caveat — precisely
#                                  the caller that is never stale. warn never blocks, so
#                                  offline / detached / no-remote stays usable.
#                                  Test-only overrides MERGE_GATES_FRESH_RUN_BLOB /
#                                  MERGE_GATES_FRESH_DEV_BLOB bypass the git compare.
#   MERGE_GATES_CR_GRACE_POLLS   — CR review grace window (default 10 polls)
#   MERGE_GATES_BB_GRACE_POLLS   — Bugbot re-review grace window for a STALE
#                                  (prior-commit-only) cursor[bot] review before
#                                  grace-expiry pass (default 10 polls)
#   MERGE_GATES_CR_INSTALLED     — override CR-installed auto-detection
#   MERGE_GATES_STALE_REREVIEW_POLLS — consecutive STALE polls on same HEAD
#                                  before auto-posting `@coderabbitai review`
#                                  (default 5; 0 disables the STALE nudge only).
#   MERGE_GATES_NONE_NUDGE_POLLS — consecutive blocking-NONE polls on same HEAD
#                                  before auto-posting `@coderabbitai review`
#                                  (default 0 = DISABLED; auto_review already
#                                  reviews every push). Decoupled from the STALE
#                                  knob — reduce-coderabbit-review-spend Slice 2.
#   MERGE_GATES_PRIOR_NUDGE_HEAD — seed the once-per-HEAD nudge guard +
#   MERGE_GATES_PRIOR_STALE_HEAD    STALE/NONE streaks from a prior invocation.
#   MERGE_GATES_PRIOR_STALE_STREAK  Set by smatchet-merge-watcher (which persists
#   MERGE_GATES_PRIOR_NONE_HEAD     them in the registry) so the once-per-HEAD
#   MERGE_GATES_PRIOR_NONE_STREAK   nudge + the STALE/NONE streaks survive
#                                  MERGE_GATES_MAX_POLLS=1 cycles. The poll emits
#                                  the updated values on a `GATE_CARRY` stdout
#                                  line before `return 1`.
#   MERGE_GATES_PRIOR_OUTAGE_HEAD   — seed the Actions-outage escalation state
#   MERGE_GATES_PRIOR_OUTAGE_STREAK   (consecutive unexplained required-absent
#   MERGE_GATES_PRIOR_OUTAGE_SINCE    polls on this head + the probe-window
#                                  anchor). Same GATE_CARRY round-trip as the
#                                  streaks above: without it the outage streak
#                                  resets every MERGE_GATES_MAX_POLLS=1 cycle
#                                  and the exit-7 escalation can never fire
#                                  under the watcher.
#
# Manual CR re-review trigger: post `@coderabbitai review` as a PR comment
# (`gh pr comment <pr> --body "@coderabbitai review"`) when CR's review is
# STALE on a new HEAD and isn't auto-firing.
#
# Return codes (poll_merge_gates):
#   0 — gates passed
#   1 — gates still blocked at MAX_POLLS
#   2 — timeout (≥MERGE_GATES_TIMEOUT_SECONDS wall-clock)
#   3 — gh API down (3 consecutive failures)
#   4 — PR closed or merged externally
#   5 — pagination overflow (any connection has more pages)
#   7 — Actions outage escalation: required contexts absent on the head for
#       MERGE_GATES_OUTAGE_POLLS consecutive polls while ZERO workflow runs
#       were created repo-wide since the absence streak began (the window
#       re-anchors after each confirmed-alive probe) — the head cannot go
#       green by waiting (admin-merge-past-absent-checks-undetected fix (3):
#       the ship-loop's defined move is to STOP and escalate, never override;
#       see docs/agent-rules/ship-loops.md § CI unavailable).
#
# Return codes (gh_pr_ready_idempotent):
#   0 — PR is now ready (or already was)
#   6 — unknown failure (caller halts; do not auto-merge)
# ----------------------------------------------------------------------------

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_QUERY_FILE="$SCRIPT_DIR/merge-gates.graphql"

# ----------------------------------------------------------------------------
# Sourced gate-condition modules — resolved relative to THIS script (SCRIPT_DIR
# via BASH_SOURCE, so it works whether the file is executed or `source`d, as the
# bats suite does). Explicit load list, fail-closed if a module is missing
# (NOT a glob) — mirrors agents/scripts/project/lint-rules.d/. The modules carry:
#   00-common.sh      — the meant-to-block allow-list constant, the prompt-shim
#                        lazy-source, and gh_pr_ready_idempotent (top-level).
#   10-gate-filter.sh — the one giant `gh api graphql --jq` GATE_FILTER program
#                        (the 33-field projection) as a template emitter.
# The four gate-condition verdicts (CI / CodeRabbit / Bugbot / user-comments)
# stay INLINE in poll_merge_gates: they share one tightly-coupled per-poll local
# state (cr_pass, cr_open_blocks, streak counters, the nudge_coderabbit closure)
# that cannot be threaded through a function boundary without a subtle
# behaviour change — the exact risk this split must not take (bats is the net).
# lib/script-freshness.sh is loaded on the same fail-closed terms as the gate
# modules: it supplies the self-staleness detector, and a merger that silently
# lost its ability to notice it is running out-of-date gate logic is exactly the
# failure this guard exists to prevent — so a missing helper must stop the script,
# not degrade to "fresh".
MERGE_GATES_D="$SCRIPT_DIR/merge-gates.d"
for _mg_mod in "$SCRIPT_DIR"/lib/script-freshness.sh \
               "$MERGE_GATES_D"/00-common.sh "$MERGE_GATES_D"/10-gate-filter.sh; do
    if [ ! -f "$_mg_mod" ]; then
        echo "merge-gates: ERROR: missing module $_mg_mod" >&2
        # sourced (bats) → return; executed → exit. Fail-closed either way.
        # shellcheck disable=SC2317  # reachable only when a module is missing.
        return 2 2>/dev/null || exit 2
    fi
    # shellcheck source=/dev/null
    . "$_mg_mod"
done
unset _mg_mod

# ----------------------------------------------------------------------------
# poll_merge_gates <owner> <repo> <pr_number>
# ----------------------------------------------------------------------------
poll_merge_gates() {
    local owner="${1:?poll_merge_gates: owner required}"
    local repo="${2:?poll_merge_gates: repo required}"
    local prNumber="${3:?poll_merge_gates: pr_number required}"
    # Deps preflight scoped to function call — file is documented sourceable
    # (see header § Usage). Top-level `exit` would kill the caller's shell.
    command -v gh >/dev/null 2>&1 || { echo "gh required" >&2; return 2; }
    # No standalone `jq` needed — the poll parses the GraphQL response via
    # gh's bundled jq engine (`gh api --jq`). gh is the only hard dep.

    # SKIP_MERGE_GATES=true at session init bypasses all gates. Documented in
    # AGENTS.md § Merge gates and docs/agent-rules/merge-gates.md § Override.
    # Until this guard landed (PR for C1 in docs/reference/agentic-infrastructure-2026-05-23.md),
    # the override was a pure documentation contract — every caller was trusted
    # to gate the call itself. A miswired delegated invocation could quietly
    # poll regardless. Read FIRST (before ORCH_USER + every other prereq) so
    # the bypass is unconditional — a skipped gate doesn't need ORCH_USER to
    # be set, doesn't need the query file to exist, doesn't need anything.
    if [ "${SKIP_MERGE_GATES:-}" = "true" ]; then
        echo "GATES_SKIPPED (SKIP_MERGE_GATES=true)"
        return 0
    fi

    if [ -z "${ORCH_USER:-}" ]; then
        echo "poll_merge_gates: ORCH_USER not set (run: ORCH_USER=\$(gh api user --jq .login))" >&2
        return 3
    fi

    # MERGE_GATES_FLIP_READY=true flips the PR ready-for-review BEFORE polling starts.
    # Authorized-merge callers (orchestrator + smatchet-merge-watcher) opt in so that
    # CodeRabbit's auto_review.drafts:false config doesn't bypass review on draft PRs.
    # Without this, CR's placeholder StatusContext SUCCESS could let a draft PR pass
    # through the grace window without any real review activity (C4 draft-PR bypass).
    # Plain poll-only callers (status checks, dry-runs) leave this unset; the gate's
    # CR-installed grace window still blocks NONE for installed repos.
    if [ "${MERGE_GATES_FLIP_READY:-}" = "true" ]; then
        gh_pr_ready_idempotent "$prNumber" || \
            echo "WARN: gh_pr_ready_idempotent returned non-zero; PR may still be draft." >&2
    fi

    # Gate-logic self-freshness guard (#1428). An unattended merger (the watcher)
    # runs THIS script from its host checkout; if that checkout is parked behind
    # origin/develop, it enforces STALE gate logic — e.g. an old allow-list missing
    # "Intent section" merged a PR past that RED non-required check. Compare this
    # file's on-disk blob to origin/develop's blob; in block mode refuse a pass when
    # they differ OR the comparison can't be made (fail-closed). Computed ONCE here
    # (before the poll loop) and consulted in the GATES_PASSED conjunction below.
    # Default "warn": a BLOCK must never be readable without a staleness caveat when
    # the checkout is behind (see the header note). warn never sets self_stale, so no
    # caller's pass/fail verdict changes — only the operator's ability to distrust a
    # stale one. The watcher still sets "block"; "off" remains available for callers
    # that must avoid the ref-refresh entirely.
    local fresh_mode="${MERGE_GATES_FRESHNESS:-warn}"
    # Reject typos up front — an unrecognised value would otherwise fall through the
    # "!= off" gate into warn-only handling, silently weakening enforcement (#1428 CR).
    case "$fresh_mode" in
        off|warn|block) ;;
        *)
            echo "poll_merge_gates: MERGE_GATES_FRESHNESS must be one of off|warn|block (got: '$fresh_mode')" >&2
            return 3
            ;;
    esac
    local self_stale=false
    if [ "$fresh_mode" != "off" ]; then
        # Gate logic now spans the entry point PLUS its sourced modules
        # (merge-gates.d/00-common.sh holds the block allow-list; 10-gate-filter.sh
        # holds the GATE_FILTER). A stale/tampered MODULE would enforce out-of-date
        # gate logic while the entry file still matches origin/develop — so freshness
        # must fingerprint all three, not just BASH_SOURCE[0] (#1428 CR follow-up:
        # the merge-gates.d/ split moved load-bearing logic out of the entry file).
        # lib/script-freshness.sh is in the set for the same reason and one sharper
        # one: it IS the detector, so a stale copy of it is the single blind spot
        # that could hide every other file's staleness. Self-fingerprinting closes
        # that — a behind-develop detector reports itself behind.
        local _self_relpath="agents/scripts/core/merge-gates.sh (+ merge-gates.d/ modules, lib/script-freshness.sh)"
        local _fresh_relpaths=(
            "agents/scripts/core/merge-gates.sh"
            "agents/scripts/core/merge-gates.d/00-common.sh"
            "agents/scripts/core/merge-gates.d/10-gate-filter.sh"
            "agents/scripts/core/lib/script-freshness.sh"
        )
        # Detection is delegated to lib/script-freshness.sh (the bounded fetch, the
        # combined fingerprint, the fail-closed blanking) so `pre-ship.sh` and the
        # other core gates inherit the same logic instead of each hand-rolling it —
        # staleness there fails in the WORSE direction, producing false GREENS.
        # The MESSAGES stay here: this gate names its own remedy and cites its own
        # history (#1428), which a shared helper cannot do for every caller.
        # MERGE_GATES_FRESH_RUN_BLOB / _DEV_BLOB remain this script's documented
        # test seam; they are mapped onto the helper's positional override.
        local _run_blob _dev_blob
        SCRIPT_FRESHNESS_ROOT_HINT="$SCRIPT_DIR" \
            script_freshness_verdict "${MERGE_GATES_FRESH_RUN_BLOB:-}" "${MERGE_GATES_FRESH_DEV_BLOB:-}" \
                "${_fresh_relpaths[@]}"
        _run_blob="$SCRIPT_FRESHNESS_RUN_BLOB"
        _dev_blob="$SCRIPT_FRESHNESS_DEV_BLOB"
        # The helper reports `unverifiable` by blanking both fingerprints, which is
        # exactly the shape the two branches below already discriminate on — so the
        # fail-closed policy stays expressed here, in the gate that owns it.
        if [ "$SCRIPT_FRESHNESS_VERDICT" = "unverifiable" ]; then
            _run_blob=""
            _dev_blob=""
        fi
        if [ -z "$_run_blob" ] || [ -z "$_dev_blob" ]; then
            if [ "$fresh_mode" = "block" ]; then
                echo "BLOCK: merge-gates freshness unverifiable (no git checkout / no origin/develop:$_self_relpath blob); refusing GATES_PASSED (MERGE_GATES_FRESHNESS=block, fail-closed). See postmortems.md #1428." >&2
                self_stale=true
            else
                echo "WARN: merge-gates freshness unverifiable (no git checkout / no origin/develop:$_self_relpath blob); MERGE_GATES_FRESHNESS=warn — not blocking." >&2
            fi
        elif [ "$_run_blob" != "$_dev_blob" ]; then
            if [ "$fresh_mode" = "block" ]; then
                echo "BLOCK: merge-gates.sh or a merge-gates.d/ gate module differs from origin/develop (combined fingerprint '$_run_blob' != develop '$_dev_blob') — this merger would enforce out-of-date gate logic. Refresh the checkout to origin/develop and restart. Refusing GATES_PASSED (fail-closed). See postmortems.md #1428." >&2
                self_stale=true
            else
                echo "WARN: merge-gates.sh or a merge-gates.d/ gate module differs from origin/develop (combined fingerprint '$_run_blob' != develop '$_dev_blob'); gate logic may be out of date (MERGE_GATES_FRESHNESS=warn — not blocking)." >&2
            fi
        fi
    fi

    local POLL_INTERVAL="${MERGE_GATES_POLL_INTERVAL:-60}"
    # 60 -> 90 (all-gates-blocking): $blocking now waits on EVERY lane, and the
    # slowest (bucket-E, 45-min job cap + runner queue time) could overrun the
    # old 60-min budget and fire a spurious GATES_TIMEOUT on a healthy PR.
    local MAX_POLLS="${MERGE_GATES_MAX_POLLS:-90}"
    # Default timeout DERIVES from the poll budget (poll count x interval) so the
    # two knobs cannot silently cap each other — a fixed 3600 while MAX_POLLS grew
    # to 90 kept firing GATES_TIMEOUT at ~60 polls anyway (CR finding on the
    # all-gates-blocking flip). An explicit MERGE_GATES_TIMEOUT_SECONDS still wins.
    local TIMEOUT_SECONDS="${MERGE_GATES_TIMEOUT_SECONDS:-$(( MAX_POLLS * POLL_INTERVAL ))}"
    local QUERY_FILE="${MERGE_GATES_QUERY_FILE:-$DEFAULT_QUERY_FILE}"
    # When CR state is STALE_WITH_FINDINGS / STALE_UNKNOWN on the same HEAD for
    # this many consecutive polls, post `@coderabbitai review` once per HEAD to
    # nudge CR into re-reviewing. Default 5 polls (~5 min at default interval).
    # Set to 0 to disable the STALE auto-trigger. (The CR=NONE early-nudge is a
    # SEPARATE knob — MERGE_GATES_NONE_NUDGE_POLLS, below — so the redundant
    # first-poll NONE nudge can be off while this STALE backstop stays on.)
    local STALE_REREVIEW_POLLS="${MERGE_GATES_STALE_REREVIEW_POLLS:-5}"
    if ! [[ "$STALE_REREVIEW_POLLS" =~ ^[0-9]+$ ]]; then
        echo "poll_merge_gates: MERGE_GATES_STALE_REREVIEW_POLLS must be a non-negative integer (got: $STALE_REREVIEW_POLLS)" >&2
        return 3
    fi
    # CR=NONE early-nudge threshold — consecutive blocking-NONE polls on the same
    # HEAD before posting one `@coderabbitai review`. Default 0 = DISABLED:
    # `.coderabbit.yaml` auto_review already reviews every push, so the nudge was
    # redundant and (firing on the first NONE poll) raced ahead of auto_review.
    # Set > 0 to re-enable after an N-poll grace that lets auto_review post first.
    # The grace-then-pass fall-through + the STALE trigger remain the backstops.
    # (reduce-coderabbit-review-spend Slice 2.)
    local NONE_NUDGE_POLLS="${MERGE_GATES_NONE_NUDGE_POLLS:-0}"
    if ! [[ "$NONE_NUDGE_POLLS" =~ ^[0-9]+$ ]]; then
        echo "poll_merge_gates: MERGE_GATES_NONE_NUDGE_POLLS must be a non-negative integer (got: $NONE_NUDGE_POLLS)" >&2
        return 3
    fi
    # Number of consecutive polls the gate will wait for CodeRabbit when the repo has
    # `.coderabbit.yaml` checked in (= CR is installed for this repo). After this many
    # polls without a review or a `CodeRabbit` SUCCESS StatusContext, NONE falls back
    # to pass with a logged warning so the loop is never wedged by a stuck integration.
    local CR_GRACE_POLLS="${MERGE_GATES_CR_GRACE_POLLS:-10}"
    # Bugbot (cursor[bot]) re-review grace: when Bugbot reviewed a PRIOR commit
    # but has not reviewed the current head yet (bb_state == STALE), wait this
    # many polls for it to re-review the new head before grace-expiry pass.
    # Mirrors CR_GRACE_POLLS. A silent Bugbot with NO artefact (bb_state ABSENT)
    # is never waited on — only a confirmed-engaged-but-stale Bugbot graces.
    local BB_GRACE_POLLS="${MERGE_GATES_BB_GRACE_POLLS:-10}"
    # Actions-outage escalation threshold — consecutive required-absent polls
    # (non-conflicted head) before probing whether ANY workflow run has been
    # created repo-wide since polling began. Zero runs created + required
    # contexts absent = a head that cannot go green by waiting (the #1941
    # shape: Actions jammed repo-wide, 75 runs stuck queued, close/reopen
    # produced 0 runs; the operator's options collapsed to "wait forever" or
    # "--admin override"). Escalating with that diagnosis IS the third option
    # (admin-merge-past-absent-checks-undetected fix (3); AI_POLICY.md
    # § Escalate, don't assume). Default 15 (~15 min at the 60 s interval).
    # The ~27 min check-suite creation LAG is disambiguated by the probe, not
    # the threshold: under mere backlog runs ARE still being created
    # repo-wide, so the probe returns >0 and the poller keeps polling — only
    # a repo that has stopped creating runs entirely escalates. Zero is
    # REJECTED, not a disable switch: exit 7 is a no-skip safety exit, and a
    # 0 here would silently downgrade the Actions-unavailable state to rc 1/2
    # where the halt prompt offers "Skip gates and merge anyway" (same
    # fail-closed posture as MERGE_GATES_FRESHNESS: an env var must not be
    # able to weaken enforcement). Effectively-never is available via a large
    # value.
    local OUTAGE_POLLS="${MERGE_GATES_OUTAGE_POLLS:-15}"
    if ! [[ "$OUTAGE_POLLS" =~ ^[0-9]+$ ]] || [ "$OUTAGE_POLLS" -eq 0 ]; then
        echo "poll_merge_gates: MERGE_GATES_OUTAGE_POLLS must be a positive integer (got: $OUTAGE_POLLS) — 0 is rejected because it would silently disable the no-skip exit-7 outage escalation" >&2
        return 3
    fi
    # Outage state survives MERGE_GATES_MAX_POLLS=1 watcher cycles via the same
    # PRIOR_*/GATE_CARRY round-trip as the STALE/NONE streaks — in-process
    # locals alone would reset every cycle and the threshold could never be
    # reached under the watcher. Keyed by head: a new push restarts CI's
    # run-creation clock, so the streak and probe window restart with it.
    local outage_head="${MERGE_GATES_PRIOR_OUTAGE_HEAD:-}"
    local outage_streak="${MERGE_GATES_PRIOR_OUTAGE_STREAK:-0}"
    [[ "$outage_streak" =~ ^[0-9]+$ ]] || outage_streak=0
    local outage_since="${MERGE_GATES_PRIOR_OUTAGE_SINCE:-}"
    [[ "$outage_since" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$ ]] || outage_since=""
    # Lazy per-run cache for the base branch's required_conversation_resolution
    # setting ("true"/"false"/"unknown"; empty = not yet read). Read at most
    # once per run, and only on the poll that actually needs it (the
    # BLOCKED-with-all-other-gates-green cause naming below). Env seam
    # MERGE_GATES_CONV_RES_REQUIRED overrides for tests, mirroring
    # pr-blocked-why.sh's PR_BLOCKED_WHY_CONV_RES_REQUIRED.
    local conv_res_cache=""
    # The probe window ($outage_since above) anchors when the absence STREAK
    # starts, not when polling starts: `created=>=<streak start>` scoped
    # repo-wide. A quiet-but-healthy repo can only stay at zero created runs
    # while nothing pushes; the head under poll was itself pushed BEFORE the
    # absence was first observed, so its runs (if Actions is alive) were
    # created before the window and don't count — which is exactly right: the
    # question is whether Actions is creating runs NOW, not whether it once
    # did. A confirmed-alive probe re-anchors the window (streak reset below),
    # so a single early run cannot permanently mask a jam that begins later —
    # a fixed poll-start window would answer "did Actions EVER create a run
    # since polling began" for the rest of the budget.

    if [ ! -f "$QUERY_FILE" ]; then
        echo "poll_merge_gates: query file not found: $QUERY_FILE" >&2
        return 3
    fi

    # Detect whether CodeRabbit is installed for this repo by probing for a checked-in
    # `.coderabbit.yaml` (or `.coderabbit.yml`). The `auto_review.drafts: false` default
    # plus CR's eventual-consistency means a freshly-opened PR can race the poller —
    # NONE on Poll 1 is a race, not "CR not installed". Override via env if needed.
    #
    # H12: separate 404 (file truly absent → cr_installed=false) from other
    # errors (auth, network, transient — fail safe, cr_installed=true). The
    # previous probe treated any non-zero `gh api` exit as "absent", which
    # silently disabled the CR gate on auth failures or transient network
    # blips. Fail-safe direction = assume installed so the gate blocks on
    # unknown CR state instead of waving through.
    local cr_installed
    if [ -n "${MERGE_GATES_CR_INSTALLED:-}" ]; then
        cr_installed="$MERGE_GATES_CR_INSTALLED"
    else
        local yaml_err="" yaml_rc=0 yml_err="" yml_rc=0
        yaml_err=$(gh api "repos/$owner/$repo/contents/.coderabbit.yaml" 2>&1 >/dev/null) || yaml_rc=$?
        if [ "$yaml_rc" -eq 0 ]; then
            cr_installed=true
        elif echo "$yaml_err" | grep -q "HTTP 404"; then
            # .yaml confirmed 404; try .yml
            yml_err=$(gh api "repos/$owner/$repo/contents/.coderabbit.yml" 2>&1 >/dev/null) || yml_rc=$?
            if [ "$yml_rc" -eq 0 ]; then
                cr_installed=true
            elif echo "$yml_err" | grep -q "HTTP 404"; then
                cr_installed=false  # both files confirmed 404 — truly absent
            else
                echo "WARN: gh api .coderabbit.yml probe failed with non-404 error; assuming CR installed (fail safe)" >&2
                cr_installed=true
            fi
        else
            echo "WARN: gh api .coderabbit.yaml probe failed with non-404 error; assuming CR installed (fail safe)" >&2
            cr_installed=true
        fi
    fi

    # Read GraphQL document into a variable. `gh api graphql -f query=@file`
    # does NOT read the file — it sends the literal `@filename` string, which
    # the GraphQL parser then chokes on at the leading `@` (directive marker).
    # The canonical pattern is to pass the document body as a string field.
    local query_body
    query_body=$(<"$QUERY_FILE")

    # ----------------------------------------------------------------------
    # Required-context ground-truth — branch_protection.required_contexts from
    # project.config.json (the authoritative set GitHub branch protection
    # enforces). The poll cross-checks this against the head rollup so that a
    # required check which NEVER RAN on the head (absent from the rollup — e.g.
    # a GITHUB_TOKEN bot push that GitHub's recursion-guard prevents from
    # re-triggering CI) is NOT scored a vacuous `CI: 0/0 pass`. Such an absent
    # required context blocks (like a pending check) instead of waving through.
    #
    # Read UTF-8-safe: standalone `jq` (the only reader here that decodes the
    # config's UTF-8 em-dash in "Windows + MSVC (Smatchet light — …)" correctly
    # — python's default open() would mojibake it under cp1252 on Windows).
    # `jq` is a soft dep for THIS read only; if it (or the config) is missing
    # the set is empty and the detector is inert (fail-safe: never wedge a merge
    # because the config couldn't be read — the isRequired-based ci_fail/ci_pend
    # plus the mergeStateStatus guard below remain the backstops).
    #
    # MERGE_GATES_REQUIRED_CONTEXTS (newline/comma-separated) overrides the file
    # read entirely when set — even to "" (empty → inert). Tests inject a
    # fixture-matching set this way; production leaves it unset and reads config.
    local req_ctx_raw=""
    if [ -n "${MERGE_GATES_REQUIRED_CONTEXTS+x}" ]; then
        # Set (possibly empty) — honour the override, skip the file read.
        req_ctx_raw="${MERGE_GATES_REQUIRED_CONTEXTS//,/$'\n'}"
    else
        local config_file="${MERGE_GATES_CONFIG_FILE:-$SCRIPT_DIR/../../../project.config.json}"
        if [ -f "$config_file" ] && command -v jq >/dev/null 2>&1; then
            req_ctx_raw=$(jq -r '.branch_protection.required_contexts[]? // empty' "$config_file" 2>/dev/null) || req_ctx_raw=""
        elif [ ! -f "$config_file" ]; then
            echo "WARN: required-context config not found ($config_file); required-absent detector inert this run." >&2
        elif ! command -v jq >/dev/null 2>&1; then
            echo "WARN: jq not found; required-absent detector inert this run (config UTF-8-safe read needs jq)." >&2
        fi
    fi
    # Build a JSON-array literal of the required-context names to splice into the
    # gh --jq filter (same pattern as __ORCH_USER__). Built with jq so names with
    # spaces ("Windows + MSVC") and the em-dash are encoded safely; empty input
    # yields []. If jq is unavailable the detector is already inert above, but
    # guard the encode too so the filter always gets valid JSON.
    local req_ctx_json='[]'
    if [ -n "$req_ctx_raw" ] && command -v jq >/dev/null 2>&1; then
        req_ctx_json=$(printf '%s\n' "$req_ctx_raw" | jq -R . | jq -sc 'map(select(length > 0))') || req_ctx_json='[]'
    fi

    # STALE-recovery state — count consecutive STALE polls for the same HEAD.
    # Reset whenever HEAD advances. After STALE_REREVIEW_POLLS, post one
    # `@coderabbitai review` comment to nudge CR into re-reviewing (idempotent
    # per-HEAD — the trigger fires at most once per head SHA).
    local stale_streak="${MERGE_GATES_PRIOR_STALE_STREAK:-0}"
    [[ "$stale_streak" =~ ^[0-9]+$ ]] || stale_streak=0
    local stale_head="${MERGE_GATES_PRIOR_STALE_HEAD:-}"
    # NONE early-nudge streak — consecutive blocking-NONE polls on the same HEAD
    # (mirror of stale_streak). Carries across MAX_POLLS=1 watcher cycles via the
    # GATE_CARRY line so the per-HEAD grace survives the merge-watcher's 1-poll loop.
    local none_streak="${MERGE_GATES_PRIOR_NONE_STREAK:-0}"
    [[ "$none_streak" =~ ^[0-9]+$ ]] || none_streak=0
    local none_head="${MERGE_GATES_PRIOR_NONE_HEAD:-}"
    # Shared once-per-HEAD guard for the `@coderabbitai review` auto-nudge —
    # both the STALE re-review trigger and the NONE early-nudge dedup on this so
    # at most one comment is posted per head SHA (they never double-post).
    local rereview_posted_head="${MERGE_GATES_PRIOR_NUDGE_HEAD:-}"

    # nudge_coderabbit <head_sha> <reason> — post `@coderabbitai review` once per
    # HEAD. Idempotent: subsequent calls for the same head SHA are no-ops. On a
    # failed `gh pr comment` the guard is left unset so a later poll retries.
    nudge_coderabbit() {
        local _head="$1" _reason="$2"
        [ "$rereview_posted_head" = "$_head" ] && return 0
        echo "WARN: $_reason; posting @coderabbitai review to nudge re-review." >&2
        if gh pr comment "$prNumber" --repo "$owner/$repo" --body "@coderabbitai review" >/dev/null 2>&1; then
            rereview_posted_head="$_head"
            echo "INFO: @coderabbitai review trigger posted on HEAD ${_head:0:8}." >&2
        else
            echo "WARN: gh pr comment failed posting @coderabbitai review; will retry next poll." >&2
        fi
    }

    local start gh_fails=0
    start=$(date +%s)

    # Option B: parse the GraphQL response with gh's BUNDLED jq (`gh api --jq`)
    # — no standalone `jq` binary required (gh is the only dep). One filter
    # computes every gate field and emits them as a fixed-order, one-per-line
    # stream (33 lines) that the poll loop reads with `mapfile`. The exact jq
    # sub-expressions are the same ones the per-field `jq` calls used before;
    # they're just composed into one program. ORCH_USER is spliced in as a
    # string literal because `gh --jq` (unlike standalone jq) takes no --arg.
    # Field order (index): 0 state · 1 headSha · 2 overflow · 3 testsOob ·
    # 4 perfOob · 5 ciTotal · 6 ciFail · 7 ciPend · 8 ciWarnDowngraded ·
    # 9 dgNames · 10 crState · 11 crFirstLine · 12 crOpen · 13 crStatusState ·
    # 14 crThreadCommentsOnHead · 15 userComments · 16 reviewDecision ·
    # 17 mergeStateStatus · 18 crOob (cr-out-of-band label) ·
    # 19 crSizeSkipped (CR posted a "review skipped — too many files" comment) ·
    # 20 crContextPresent · 21 reqAbsentNames (", "-joined config-required
    # contexts absent from the head rollup) · 22 reqAbsentCount (their count) ·
    # 23 crReviewSkipped (bool: CR StatusContext SUCCESS + description "Review
    # skipped" and NOT the too-many-files size-skip — a TERMINAL generic skip) ·
    # 24 bbState (latest cursor[bot] review state on head, e.g. COMMENTED;
    # else TERMINAL = a "couldn't run"/"usage limit" conversation comment, else
    # STALE = cursor[bot] reviewed a prior commit only, else ABSENT = no
    # cursor[bot] review anywhere) · 25 bbOpen (count of unresolved non-outdated
    # cursor[bot] inline-finding review threads — Bugbot gate #4) · 26 bbOob
    # (bugbot-out-of-band label) · 27 selfImpOnly (bool: PR diff entirely under
    # docs/self-improvement/** → auto-skip CR + Bugbot gates) ·
    # 28 pureDocs (bool: PR diff strictly within the is-pure-docs-diff.sh
    # allow-list — docs/ / backlog/ / agents/scripts/ / *.md) ·
    # 29 crRateLimited (bool: CR posted a rate-limit signal on a comment OR the
    # CodeRabbit StatusContext description — a TEMPORARY skip, not a terminal pass) ·
    # 30 crDisposition (bool: a `cr-disposition:`-prefixed label is present OR a
    # `cr-disposition:<reason>` marker line is in the PR body — the explicit
    # operator attestation REQUIRED before any `cr-out-of-band` downgrade is
    # honoured; PR-3 cr-out-of-band-disposition-trail) ·
    # 31 thrUnresolvedTotal (UNFILTERED unresolved non-outdated review-thread
    # count — what required_conversation_resolution actually gates on; the
    # user-comment gate's filtered view excludes bot threads, #1937) ·
    # 32 thrUnresolvedUser (the user-authored subset of 31; bot = 31 - 32).
    # The trailing fields must all be non-empty so the `data=$(gh …)` command
    # substitution (trailing-newline collapse) never strips one and deflates the
    # 33-field count (tripping the fail-closed assertion). reqAbsentCount (22),
    # crReviewSkipped (23), bbState (24, ABSENT-default), bbOpen (25, numeric),
    # bbOob (26), selfImpOnly (27), pureDocs (28), crRateLimited (29),
    # crDisposition (30) and the two numeric thread counts (31/32) are all
    # non-empty tokens, so they are safe at the tail.
    # GATE_FILTER — the 33-field jq projection (see field-order map above).
    # Copied byte-for-byte from the _MG_GATE_FILTER_TEMPLATE global that
    # merge-gates.d/10-gate-filter.sh defines (single-quoted literal → no
    # command-substitution newline trim); placeholders spliced below as before.
    local GATE_FILTER="$_MG_GATE_FILTER_TEMPLATE"
    GATE_FILTER="${GATE_FILTER//__ORCH_USER__/$ORCH_USER}"
    # Splice the required-context JSON array (built UTF-8-safe above) as a jq
    # literal. Unlike ORCH_USER (a bare login spliced into a string compare),
    # this is a full JSON array value — valid jq on its own (e.g. `[]` or
    # `["Test-delta gate","Windows + MSVC"]`).
    GATE_FILTER="${GATE_FILTER//__REQUIRED_CONTEXTS__/$req_ctx_json}"
    # Splice the meant-to-block allow-list regex from the single-source constant
    # (MERGE_GATES_BLOCK_ALLOWLIST_RE, defined at file top). Spliced into a jq
    # `test("…"; "i")` string literal — the regex has no jq/double-quote-special
    # chars, so a plain substitution is safe.
    GATE_FILTER="${GATE_FILTER//__BLOCK_ALLOWLIST_RE__/$MERGE_GATES_BLOCK_ALLOWLIST_RE}"

    local p
    for ((p=0; p<MAX_POLLS; p++)); do
        local data
        if ! data=$(gh api graphql \
                       -f owner="$owner" \
                       -f repo="$repo" \
                       -F pr="$prNumber" \
                       -f query="$query_body" \
                       --jq "$GATE_FILTER" 2>&1); then
            gh_fails=$((gh_fails+1))
            echo "Poll $((p+1)): gh failed ($gh_fails/3): $data"
            if [ "$gh_fails" -ge 3 ]; then
                echo "GH_API_DOWN"
                return 3
            fi
            # Wall-clock check on failure path — intermittent failures that
            # never hit 3-in-a-row must not exceed MERGE_GATES_TIMEOUT_SECONDS.
            local elapsed_fail=$(( $(date +%s) - start ))
            if [ "$elapsed_fail" -ge "$TIMEOUT_SECONDS" ]; then
                echo "GATES_TIMEOUT"
                return 2
            fi
            if [ "$p" -lt $((MAX_POLLS-1)) ]; then
                sleep "$POLL_INTERVAL"
            fi
            continue
        fi
        gh_fails=0

        # Parse the gh --jq field stream — 33 fixed-order lines (see GATE_FILTER
        # field map above). gh --jq errors already routed through the gh-fail
        # path above; this guards a truncated/partial body → fail closed (retry).
        local fields
        # Strip CR — Windows jq builds (and gh's bundled jq on Windows) emit
        # CRLF, which would leave a trailing \r on every field (e.g. pr_state
        # "OPEN\r" != "OPEN" → spurious return-4).
        data="${data//$'\r'/}"
        mapfile -t fields <<<"$data"
        if [ "${#fields[@]}" -ne 33 ]; then
            # Exactly 33 expected. Any other count (a field value with an embedded
            # newline would inflate it, misaligning fields[n]) → fail closed (CR #511).
            gh_fails=$((gh_fails+1))
            echo "Poll $((p+1)): gate filter returned ${#fields[@]} fields (expected 33); transient ($gh_fails/3)"
            if [ "$gh_fails" -ge 3 ]; then echo "GH_API_DOWN"; return 3; fi
            local elapsed_short=$(( $(date +%s) - start ))
            if [ "$elapsed_short" -ge "$TIMEOUT_SECONDS" ]; then echo "GATES_TIMEOUT"; return 2; fi
            if [ "$p" -lt $((MAX_POLLS-1)) ]; then sleep "$POLL_INTERVAL"; fi
            continue
        fi

        # PR state early-exit. Empty/UNKNOWN → return 4 (no-longer-mergeable).
        local pr_state="${fields[0]:-UNKNOWN}"
        if [ "$pr_state" != "OPEN" ]; then
            echo "PR_${pr_state:-UNKNOWN}"
            return 4
        fi

        local head_sha="${fields[1]}"

        # Pagination overflow — fail closed (return 5). The filter already OR's
        # every hasNextPage; a malformed response routes through the gh-fail path.
        local overflow="${fields[2]}"
        if [ "$overflow" = "true" ]; then
            echo "PAGINATION_OVERFLOW"
            return 5
        fi

        # Labels (tests-out-of-band / perf-out-of-band) — the downgrade was
        # already applied inside the filter's ci_fail / ci_warn_downgraded; these
        # are surfaced only for the WARN line below.
        local has_tests_oob="${fields[3]}" has_perf_oob="${fields[4]}"

        # cr-out-of-band label — when present, a CR block (CHANGES_REQUESTED,
        # COMMENTED+actionable>0, DISMISSED, STALE_WITH_FINDINGS, STALE_UNKNOWN,
        # or NONE-after-grace) is downgraded to a WARN (pass). Mirrors the
        # tests/perf-out-of-band CI downgrade pattern but scoped to the CR gate
        # only (CI + user-comment gates still bind). Read here; applied in the
        # CR decision branch below after cr_pass is computed.
        local has_cr_oob="${fields[18]}"

        # cr_size_skipped — true when CodeRabbit posted a PR conversation comment
        # saying it skipped review because the PR exceeds its file limit (marker:
        # `skip review by coderabbit.ai`). This is NOT a review object, so
        # reviewDecision stays NONE and cr_state computes to NONE — meaning the
        # passive NONE grace-then-pass backstop would otherwise wave a huge,
        # entirely-unreviewed PR straight through. The NONE branch below treats a
        # size-skip as a hard block (unless cr-out-of-band waives it) and
        # suppresses the futile @coderabbitai review auto-nudge. Only consulted
        # when no real CR review exists on any commit (cr_state == NONE) so a
        # later genuine review on head always takes precedence over a stale skip
        # comment. Empty (filter/parse miss) → treated as "not skipped" (the
        # NONE-grace path still binds; fail-closed is preserved by cr_pass=false
        # within grace), but a malformed body inflates the field count and routes
        # through the fail-closed assertion above before we reach here.
        local cr_size_skipped="${fields[19]}"

        # CI — required-only, latest-per-name dedup + label downgrades, all done
        # in the filter. Empty → -1, which fails closed at the integer checks.
        local ci_total="${fields[5]:--1}"
        local ci_fail="${fields[6]:--1}"
        local ci_pend="${fields[7]:--1}"
        local ci_warn_downgraded="${fields[8]:--1}"
        local dg_names="${fields[9]}"

        # Surface every downgraded check on stderr so the operator sees what the
        # label hid. Mirrors the "Skip gates and merge anyway" LOG_WARN pattern.
        if [ "$ci_warn_downgraded" -gt 0 ]; then
            echo "WARN: out-of-band label(s) downgraded ${ci_warn_downgraded} failing check(s) to WARN: ${dg_names}" >&2
        fi

        # CodeRabbit — four-bucket discrimination with body-aware actionable parsing.
        # The filter computed cr_state + the review body's FIRST LINE (current-head
        # review preferred, else most-recent stale — same selection as before).
        # P1 fix per docs/self-improvement/categories/process.md: body's first
        # line carries "Actionable comments posted: N" — N>0 means CR found real
        # bugs the user should review before any force-merge / timeout-pass.
        local cr_state="${fields[10]}"
        local cr_first_line="${fields[11]}"
        # Extract N from "Actionable comments posted: N". -1 = header not found.
        # First line only (CR's convention) — nested findings may quote the phrase.
        local cr_actionable=-1
        if [ -n "$cr_first_line" ]; then
            local match
            match=$(printf '%s' "$cr_first_line" | grep -oE 'Actionable comments posted:[[:space:]]*[0-9]+' || true)
            if [ -n "$match" ]; then
                cr_actionable=$(printf '%s' "$match" | grep -oE '[0-9]+')
            fi
        fi
        # -1 (filter/parse miss) fails closed at the `cr_open -eq 0` pass check.
        local cr_open="${fields[12]:--1}"
        # CR completion signal on the head rollup. CodeRabbit emits this as EITHER a
        # StatusContext named "CodeRabbit" OR a CheckRun ("CodeRabbit" / "CR findings
        # (0 actionable)"); the GraphQL projection (field 13) now normalizes both to
        # SUCCESS when CR has finished cleanly — previously only the StatusContext was
        # read, so a CheckRun-only signal computed ABSENT and a clean PR (reviewDecision
        # NONE) needlessly burned the full CR_GRACE_POLLS window instead of fast-passing.
        # SUCCESS is a positive signal; "ABSENT" if neither shape is present yet.
        local cr_status_state="${fields[13]}"
        # C4 prong 2: count of CR review-thread comments anchored to the current
        # head — positive evidence CR actively reviewed this commit (vs a bare
        # placeholder StatusContext). The NONE branch uses it to gate the pass.
        local cr_thread_comments_on_head="${fields[14]:--1}"
        # 1 when a CodeRabbit context (StatusContext OR CheckRun, ANY state incl.
        # in-progress) is present on the head — i.e. CR is already engaged (reviewing
        # or done). Suppresses the redundant NONE early-nudge: CR auto-reviews every
        # new head, so poking `@coderabbitai review` while it is already reviewing is
        # noise (the "multiple pokes" symptom on auto-commit head churn).
        local cr_context_present="${fields[20]:-0}"

        # crReviewSkipped — true when CR's "CodeRabbit" StatusContext is SUCCESS
        # with description "Review skipped" and NOT the too-many-files size-skip
        # (filter field 23). A TERMINAL generic skip: CR processed the PR and
        # declined an incremental review (docs-only / path-filtered / trivial
        # diff). The NONE branch uses it to fast-pass instead of burning the
        # status-SUCCESS grace waiting for an inline review that never lands
        # (PR #976). Empty (parse miss) → false, so the NONE-grace path still
        # binds (fail-safe — no spurious terminal pass).
        local cr_review_skipped="${fields[23]:-false}"

        # Required-context ground-truth cross-check (P1 fix —
        # docs/self-improvement/categories/tooling.md:328). Count + names of the
        # branch_protection.required_contexts that are ABSENT from the head
        # rollup entirely (never ran — e.g. a GITHUB_TOKEN bot push GitHub's
        # recursion-guard kept from re-triggering CI). The isRequired-based
        # ci_fail/ci_pend can't see these (an absent context has no rollup node
        # to carry isRequired=true), so a required check that never ran would
        # otherwise score a vacuous `CI: 0/0 pass`. An absent required context
        # is NOT a pass — it blocks like a pending check. Present-but-failing is
        # already caught by ci_fail (the context IS in the rollup), so this only
        # counts truly-absent names → no double-count. -1 (filter/parse miss)
        # fails closed at the `req_absent -eq 0` pass check below. Count is the
        # LAST field (index 22) — always numeric, never the empty trailing field
        # that command substitution would strip; names (index 21) precede it.
        local req_absent_names="${fields[21]}"
        local req_absent="${fields[22]:--1}"

        # Bugbot (cursor[bot]) gate #4 inputs (fields 24-26). bb_state: a review
        # state (e.g. COMMENTED) when Bugbot reviewed the current head; TERMINAL
        # for a "couldn't run"/"usage limit" conversation comment (spend-cap
        # no-verdict); STALE for a prior-commit-only review (mid-re-review);
        # ABSENT for no cursor[bot] review anywhere (Bugbot-free / not engaged).
        # bb_open: unresolved non-outdated cursor[bot] inline-finding threads
        # (-1 = filter/parse miss, fails closed). has_bb_oob: the
        # bugbot-out-of-band override label. Decision bucket is computed below,
        # just before the Poll status line.
        local bb_state="${fields[24]:-ABSENT}"
        local bb_open="${fields[25]:--1}"
        local has_bb_oob="${fields[26]:-false}"

        # self_imp_only — true iff the PR diff is entirely under
        # docs/self-improvement/** (field 27). Drives the CR + Bugbot auto-skip
        # (user ask 2026-06-20; plan self-improvement-pr-review-exemption): these
        # PRs are low-risk, never-compiled ledger edits that shouldn't be
        # code-reviewed or block on a review bot. Empty/parse-miss → false
        # (fail-safe = NOT exempt → full gates). NOTE: this only stops Bugbot from
        # *blocking* — stopping it from *posting* needs a Cursor-dashboard
        # path-ignore (docs/agent-rules/merge-gates.md § Bugbot gate).
        local self_imp_only="${fields[27]:-false}"

        # pure_docs — true iff the PR diff is strictly within the
        # is-pure-docs-diff.sh allow-list (docs/ / backlog/ / agents/scripts/ /
        # *.md). Field 28. Drives the rate-limit auto-downgrade (deliverable 1):
        # a CR rate-limit skip on a pure-docs PR is harmless to fast-pass.
        # Empty/parse-miss → false (fail-safe = NOT pure-docs → treated as code).
        local pure_docs="${fields[28]:-false}"
        # cr_rate_limited — true iff CR signalled a rate limit on a comment or its
        # StatusContext description (field 29). A TEMPORARY skip (CR re-reviews on
        # quota recovery), distinct from the terminal cr_review_skipped pass.
        # Drives both the pure-docs auto-downgrade (deliverable 1) and the CODE-PR
        # pause/disposition gate (deliverable 2). Empty/parse-miss → false.
        local cr_rate_limited="${fields[29]:-false}"
        # cr_disposition — a `cr-disposition:`-prefixed label OR a
        # `cr-disposition:<reason>` PR-body marker is present (field 30). Required
        # ALONGSIDE cr-out-of-band to waive ANY CR block (PR-3): cr-out-of-band
        # alone is NOT honoured — the disposition records why CR review was waived.
        local cr_disposition="${fields[30]:-false}"

        # UNFILTERED unresolved-non-outdated review-thread counts (fields 31/32:
        # total, user-authored). The user-comment gate deliberately excludes bot
        # threads, but branch protection's required_conversation_resolution
        # counts EVERY unresolved thread — so the poller could read all-clear on
        # a PR GitHub will never merge (#1937: ten open CR threads, ~90 min
        # burned, cause found by hand). These feed the BLOCKED-cause naming
        # below only; they gate nothing themselves. -1 = filter/parse miss →
        # the naming stays silent (never invent a count).
        local thr_total="${fields[31]:--1}"
        local thr_user_cnt="${fields[32]:--1}"
        # bats-only seam: force the field-32 parse-miss (-1) path — a fixture
        # cannot make the filter emit -1 for one field while field 31 parses.
        [ -n "${MERGE_GATES_TEST_THR_USER_CNT:-}" ] && thr_user_cnt="$MERGE_GATES_TEST_THR_USER_CNT"

        # User comments (non-bot, non-self) — -1 fails closed at `user -eq 0`.
        local user="${fields[15]:--1}"

        # reviewDecision
        local review_decision="${fields[16]:-NONE}"
        local review_pass=false
        case "$review_decision" in
            APPROVED|NONE) review_pass=true ;;
        esac

        local cr_pass=false
        local cr_state_print="$cr_state"
        # Set true only when the cr-out-of-band label actually waives a real CR
        # block below (state verdict or open CR threads) — i.e. the override was
        # LOAD-BEARING. Distinct from mere label presence (has_cr_oob): a moot
        # cr-out-of-band on an already-passing CR gate never sets this. Feeds the
        # GATE_SNAPSHOT line so the override-merge ledger records the CR gate as
        # genuinely bypassed (mandatory-merge-snapshot-on-override-merge).
        local cr_overridden=false
        # Set true only by the NONE branch when CR posted a "review skipped — too
        # many files" comment. Hoisted here so it's always defined for the
        # cr-out-of-band downgrade + nudge-suppression checks below, regardless of
        # which case branch runs.
        local cr_size_skip_block=false
        case "$cr_state" in
            APPROVED)
                # Approval on the current head is always a pass, regardless of body shape.
                cr_pass=true
                cr_state_print="APPROVED"
                ;;
            COMMENTED)
                # On-head COMMENTED. Block when CR reported actionable findings (N>0);
                # pass when CR explicitly said 0 actionable; pass when no Actionable
                # header found (body is empty / non-CR-shape / older CR template).
                if [ "$cr_actionable" -gt 0 ]; then
                    cr_pass=false
                    cr_state_print="COMMENTED (${cr_actionable} actionable — block)"
                else
                    cr_pass=true
                    if [ "$cr_actionable" = "0" ]; then
                        cr_state_print="COMMENTED (0 actionable)"
                    else
                        cr_state_print="COMMENTED (no Actionable header)"
                    fi
                fi
                ;;
            STALE)
                # CR reviewed a prior commit. Discriminate via Actionable count from
                # that stale body. STALE_WITH_FINDINGS NEVER passes on timeout (would
                # discard real CR feedback the user hasn't seen). STALE_CLEAN passes
                # on timeout (the prior review was clean; current commit likely still
                # clean modulo new edits). STALE_UNKNOWN treated as STALE_WITH_FINDINGS
                # to be safe — caller can't distinguish "0 actionable" from "no header".
                #
                # H16: STALE_RESOLVED — when CR's prior review found N>0 findings BUT
                # all CR review threads are now resolved AND CR's StatusContext on the
                # current head is SUCCESS, CR has re-evaluated the current commit
                # (resolving threads is its accept signal) without re-issuing a fresh
                # review body. This is the dominant case for small fixup commits where
                # CR accepts the addressing change via thread-resolution rather than
                # posting a new "Actionable comments posted: 0" review. The merge-gate
                # used to wedge here forever; now we treat it as pass. Requires BOTH
                # signals (open=0 AND status=SUCCESS) — open=0 alone could mean the
                # user manually resolved (no CR judgement); status=SUCCESS alone could
                # be a stale placeholder. Together they're a CR-driven accept.
                if [ "$cr_actionable" -gt 0 ] && [ "$cr_open" -eq 0 ] && [ "$cr_status_state" = "SUCCESS" ]; then
                    cr_pass=true
                    cr_state_print="STALE_RESOLVED (${cr_actionable} actionable on prior commit, all threads resolved + status SUCCESS — pass)"
                elif [ "$cr_actionable" -gt 0 ]; then
                    cr_pass=false
                    cr_state_print="STALE_WITH_FINDINGS (${cr_actionable} actionable on prior commit — block + surface review)"
                elif [ "$cr_actionable" = "0" ]; then
                    cr_pass=true
                    cr_state_print="STALE_CLEAN (0 actionable on prior commit — pass)"
                else
                    cr_pass=false
                    cr_state_print="STALE_UNKNOWN (no Actionable header — treat as block per safe-default policy)"
                fi
                # STALE auto-recovery: when CR sits on a STALE blocking state for
                # ≥STALE_REREVIEW_POLLS on the same HEAD, post `@coderabbitai review`
                # once per HEAD to nudge a re-review (idempotent — dedups on head_sha).
                if [ "$cr_pass" = false ] && [ "$STALE_REREVIEW_POLLS" -gt 0 ]; then
                    if [ "$stale_head" = "$head_sha" ]; then
                        stale_streak=$((stale_streak + 1))
                    else
                        stale_head="$head_sha"
                        stale_streak=1
                    fi
                    if [ "$stale_streak" -ge "$STALE_REREVIEW_POLLS" ]; then
                        nudge_coderabbit "$head_sha" "STALE on HEAD ${head_sha:0:8} for $stale_streak polls"
                    fi
                fi
                ;;
            NONE)
                # cr_size_skipped takes precedence over the entire NONE
                # grace/status fall-through chain below. CR explicitly told us it
                # skipped review because the PR is too large — there will never be
                # a review or inline evidence on this head, so the passive
                # grace-then-pass backstop must NOT wave it through (that hole let
                # a 638-file reorg merge with zero CR review). Hard-block here and
                # short-circuit; the cr-out-of-band downgrade below can still waive
                # it, and the early-nudge is suppressed (re-triggering review on a
                # size-skipped PR just makes CR skip again + spams the thread).
                # Gated on cr_installed for symmetry with the rest of NONE — a
                # skip comment only exists when CR is installed anyway.
                #
                # Precedence note: this lives inside `case NONE` so a genuine CR
                # review on the current head (cr_state COMMENTED/APPROVED/
                # CHANGES_REQUESTED) or a stale review on a prior commit (STALE*)
                # never reaches here — the most-recent real CR signal always wins
                # over an older size-skip comment. cr_state==NONE means no review
                # object exists on ANY commit, so the skip comment is the only CR
                # signal and is authoritative.
                if [ "$cr_size_skipped" = "true" ] && [ "$cr_installed" = true ]; then
                    cr_pass=false
                    cr_size_skip_block=true
                    cr_state_print="NONE+size-skip (CR skipped review — too many files)"
                    echo "BLOCK: CodeRabbit skipped review — too many files (exceeds CR file limit); split the PR (coderabbit review --dir <path> / --base) or apply the 'cr-out-of-band' label to merge without CR review." >&2
                elif [ "$cr_installed" != true ]; then
                    # Repo doesn't have CodeRabbit installed — NONE is the steady state.
                    cr_pass=true
                elif [ "$cr_review_skipped" = "true" ]; then
                    # Generic terminal "Review skipped" — CR's StatusContext is
                    # SUCCESS with description "Review skipped" (NOT the
                    # too-many-files size-skip, which the first arm caught and
                    # short-circuited). CR processed the PR and deliberately
                    # declined an incremental review (docs-only / path-filtered /
                    # trivial diff per .coderabbit.yaml). This is TERMINAL, not
                    # pending: no inline review will ever land, so the
                    # status-SUCCESS-waiting-for-inline grace below would burn the
                    # full CR_GRACE_POLLS window for nothing (PR #976: a docs-only
                    # PR sat ~10 cycles before the passive grace-then-pass fired).
                    # Treat as an immediate pass. Ordered AFTER the size-skip arm
                    # so a too-many-files skip can never reach here even if CR's
                    # description text overlaps.
                    cr_pass=true
                    cr_state_print="NONE+review-skipped (CR terminal skip — no incremental review for this diff)"
                elif [ "$cr_status_state" = "SUCCESS" ] && [ "$cr_thread_comments_on_head" -gt 0 ]; then
                    # C4 prong 2: status-SUCCESS PLUS at least one CR review-thread
                    # comment on the current head. CR has actively reviewed this
                    # commit — placeholder status is corroborated by real review
                    # activity. This is the safe pass path. The previous rule
                    # (status-SUCCESS alone) let draft-PR bypass slip through:
                    # CR's auto_review.drafts:false skipped the review but its
                    # StatusContext placeholder still fired SUCCESS.
                    cr_pass=true
                    cr_state_print="NONE+status-SUCCESS+inline-evidence (${cr_thread_comments_on_head} CR comment(s) on head)"
                elif [ "$cr_status_state" = "SUCCESS" ] && [ "$p" -ge "$CR_GRACE_POLLS" ]; then
                    # Status-SUCCESS but zero inline evidence on current head. Two
                    # possible causes: (a) a status-only CR config (rare; CR's
                    # default emits both status + review), or (b) CR's placeholder
                    # fired without a real review (the C4 bypass). After the grace
                    # window we fall through to pass so the loop never wedges on a
                    # status-only config, but the WARN names the suspicious shape.
                    echo "WARN: CodeRabbit status=SUCCESS but no inline CR comments on head after grace ($CR_GRACE_POLLS polls); possible status-only config OR C4 bypass." >&2
                    cr_pass=true
                    cr_state_print="NONE+status-SUCCESS+no-inline-evidence (grace expired — assume status-only)"
                elif [ "$cr_status_state" = "SUCCESS" ]; then
                    # Status fired SUCCESS, no inline evidence yet, still within
                    # grace. Wait — a real CR review on a freshly-flipped-ready PR
                    # often lands a poll or two after the placeholder. Distinct
                    # from NONE+pending so the operator can see the placeholder.
                    cr_state_print="NONE+status-SUCCESS-waiting-for-inline (poll $((p+1))/$CR_GRACE_POLLS)"
                elif [ "$p" -ge "$CR_GRACE_POLLS" ]; then
                    # Grace window elapsed; CR never started. Log + fall through to pass
                    # so the loop is never wedged by a stuck integration.
                    echo "WARN: CodeRabbit grace window ($CR_GRACE_POLLS polls) expired without a review or SUCCESS status; treating NONE as pass." >&2
                    cr_pass=true
                    cr_state_print="NONE+grace-expired"
                else
                    cr_state_print="NONE+pending (poll $((p+1))/$CR_GRACE_POLLS)"
                fi
                # NONE early-nudge: CR is installed but has posted no review on
                # this head yet (still within grace → cr_pass=false). Post
                # `@coderabbitai review` once per HEAD so CR resolves before the
                # grace window expires rather than relying on the passive
                # grace-then-pass backstop. Reuses the shared once-per-HEAD guard
                # (won't double-post with the STALE trigger). Gated on the same
                # STALE_REREVIEW_POLLS knob (0 disables). Fires on the first
                # blocking NONE poll — no streak required, since the goal is to
                # wake CR up early. The grace fall-through above stays as the
                # backstop for a genuinely stuck integration.
                #
                # Suppressed when cr_size_skip_block: a size-skip is the one NONE
                # shape where nudging is futile — re-triggering review on a PR that
                # exceeds CR's file limit just makes CR skip again and spams the
                # thread. The nudge stays for genuine NONE (CR still thinking).
                # Also suppressed when CR is already engaged on this head
                # (cr_context_present): CR auto-reviews every new head, so nudging
                # while it is mid-review is a redundant poke — the nudge is for a
                # genuinely silent CR, not one already working. Without this, an
                # auto-commit that moves the head (e.g. a bot INDEX-autosync) drew a
                # second `@coderabbitai review` on top of CR's own auto-review.
                if [ "$cr_pass" = false ] && [ "$cr_installed" = true ] && \
                   [ "$cr_size_skip_block" != true ] && [ "$cr_context_present" != 1 ] && \
                   [ "$NONE_NUDGE_POLLS" -gt 0 ]; then
                    # Streak-gated (NOT first-poll): let auto_review post first.
                    if [ "$none_head" = "$head_sha" ]; then
                        none_streak=$((none_streak + 1))
                    else
                        none_head="$head_sha"
                        none_streak=1
                    fi
                    if [ "$none_streak" -ge "$NONE_NUDGE_POLLS" ]; then
                        nudge_coderabbit "$head_sha" "CR=NONE on HEAD ${head_sha:0:8} for $none_streak polls (auto_review not yet posted)"
                    fi
                fi
                ;;
        esac

        # Reset STALE streak whenever the state leaves the BLOCKING-STALE
        # family. Any non-STALE cr_state breaks consecutive — and so do
        # passing STALE variants (STALE_CLEAN / STALE_RESOLVED) where
        # cr_pass=true, since the re-review trigger only makes sense for
        # blocking-STALE polls. Without `|| cr_pass=true`, an intermittent
        # STALE_WITH_FINDINGS -> STALE_RESOLVED -> STALE_WITH_FINDINGS
        # pattern would accumulate streak across the passing intervals.
        if [ "$cr_state" != "STALE" ] || [ "$cr_pass" = true ]; then
            stale_streak=0
            stale_head=""
        fi
        # NONE early-nudge streak resets whenever the state leaves blocking-NONE
        # (CR engaged / passed). Mirrors the STALE reset above.
        if [ "$cr_state" != "NONE" ] || [ "$cr_pass" = true ]; then
            none_streak=0
            none_head=""
        fi

        # Required-context ground-truth: surface any branch-protection-required
        # context that never ran on the head (absent from the rollup). Printed
        # like ci_fail/ci_pend so the operator sees exactly which required check
        # is missing a run — the false-pass signal that scored CI: 0/0 on #856.
        #
        # CAUSE ATTRIBUTION (admin-merge-past-absent-checks-undetected, process P1):
        # a CONFLICTED head produces this same all-contexts-absent shape, because
        # GitHub declines to build a head whose mergeStateStatus is DIRTY at all.
        # Reporting the generic "never ran" hint for it sent one session through a
        # full 90-poll timeout before the real cause (a conflict in
        # docs/plans/INDEX.md) was found by hand — while the actionable field was in
        # the poller's own GraphQL response from poll 1. Branch on it and name the
        # fix. Still a BLOCK either way: the merge is refused for the same reason
        # (required contexts have no terminal conclusion); only the diagnosis differs.
        if [ "$req_absent" -gt 0 ] && [ "${fields[17]:-UNKNOWN}" = "DIRTY" ]; then
            echo "BLOCK: required-missing: ${req_absent_names} (head is CONFLICTED — mergeStateStatus=DIRTY, so GitHub will not build it and these contexts can never report. Merge origin/develop into the branch and resolve the conflict; CI re-fires on the new head. This is NOT a CI fault and polling will not clear it)." >&2
            # A conflict fully explains the absence — the outage question does
            # not arise, and its streak must not accumulate behind the
            # conflict diagnosis.
            outage_streak=0
            outage_since=""
        elif [ "$req_absent" -gt 0 ]; then
            echo "BLOCK: required-missing: ${req_absent_names} (required by branch protection but absent from the head rollup — never ran; e.g. a GITHUB_TOKEN bot push that did not re-trigger CI). NOTE check-suite creation LAGS a push (~27 min measured under backlog), so absence on an early poll may still be pending; it is blocked until the contexts actually report, never merged on the assumption they will." >&2
            # Actions-outage escalation (fix (3), admin-merge-past-absent-
            # checks-undetected): after OUTAGE_POLLS consecutive polls in this
            # unexplained-absence state, ask whether Actions has created ANY
            # workflow run repo-wide since polling began. Zero runs created
            # means waiting cannot clear this block — either Actions is jammed
            # (the #1941 shape) or nothing ever triggers CI for this head —
            # and the defined move is to stop and escalate, NOT to time out at
            # MAX_POLLS with a per-check message, and NOT to --admin merge.
            # A probe failure keeps polling (escalation is an early exit;
            # never take it on unverified evidence), as does any nonzero
            # created-count (backlogged-but-alive Actions keeps creating runs,
            # which is what separates the ~27 min creation lag from an
            # outage).
            if [ "$outage_head" != "$head_sha" ]; then
                # New head: the push restarts CI's run-creation clock, so
                # a streak carried from an earlier head must not count
                # against this one.
                outage_head="$head_sha"
                outage_streak=0
                outage_since=""
            fi
            outage_streak=$((outage_streak + 1))
            [ -n "$outage_since" ] || outage_since="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
            if [ "$outage_streak" -ge "$OUTAGE_POLLS" ]; then
                local runs_created
                runs_created=$(gh api "repos/${owner}/${repo}/actions/runs" \
                                 -X GET -f "created=>=${outage_since}" -f per_page=1 \
                                 --jq '.total_count' 2>/dev/null) || runs_created=""
                # A 200 with an unexpected body shape yields the string
                # "null" (gh exits 0) — that is an UNVERIFIED probe, not
                # evidence of life; route it to the WARN branch with any
                # other non-numeric output.
                [[ "$runs_created" =~ ^[0-9]+$ ]] || runs_created=""
                if [ "$runs_created" = "0" ]; then
                    echo "ESCALATE: ACTIONS UNAVAILABLE — ${req_absent} required context(s) absent on head ${head_sha:0:8} for ${outage_streak} consecutive polls AND zero workflow runs created repo-wide since ${outage_since}. This head cannot go green by waiting: either Actions is not creating runs (outage/jam — the #1941 shape) or nothing re-triggers CI for this head. Defined move (ship-loops.md § CI unavailable): stop polling, surface this diagnosis, and re-run once Actions drains (or re-fire CI with a push / close-reopen). Do NOT --admin merge past absent checks — postmortem-owed.sh's required-context-ABSENT detector flags that merge and a snapshot + deviation entry are owed." >&2
                    return 7
                elif [ -z "$runs_created" ]; then
                    echo "WARN: outage probe failed (gh api actions/runs); cannot distinguish outage from backlog this poll — continuing to poll." >&2
                else
                    # Actions confirmed alive: reset the streak and
                    # re-anchor the window at the next absent poll — the
                    # next probe fires a full threshold later against a
                    # fresh window (catches a jam that begins after an
                    # early run), instead of re-probing every remaining
                    # poll against evidence that only ages.
                    outage_streak=0
                    outage_since=""
                fi
            fi
        elif [ "$req_absent" -lt 0 ]; then
            echo "WARN: required-context check returned ${req_absent} (filter/parse miss); failing closed on the required-absent gate this poll." >&2
            outage_streak=0
            outage_since=""
        else
            outage_streak=0
            outage_since=""
        fi

        # ── Bugbot (cursor[bot]) gate #4 ──────────────────────────────────────
        # Mirrors the CR machinery but guards on bb_open (unresolved cursor[bot]
        # inline findings) ONLY — Bugbot summary reviews are always COMMENTED, so
        # there is no review-state verdict to read. Decision ORDER is load-bearing:
        # open findings BLOCK *before* the terminal-signal / out-of-band / grace
        # escape hatches are consulted, so a "couldn't run"/"usage limit" comment
        # can NEVER wave real findings through (plan Finding 1 — the no-wedge
        # guarantee covers a *no-verdict* Bugbot, not findings-then-cap). The
        # three no-wedge hatches (TERMINAL short-circuit, bugbot-out-of-band, and
        # the BB_GRACE_POLLS-bounded STALE wait) guarantee a usage-capped or silent
        # Bugbot can never block a merge longer than the grace window.
        #   bb_block feeds the GATES_PASSED composite below (the actual consume
        #   point — without that conjunct a Bugbot BLOCK is computed yet the
        #   aggregate still passes when CI + CR are green; plan Finding 2).
        # bb_open < 0 (filter/parse miss) fails closed. bb_state_print avoids the
        # tokens the merge-watcher's Poll-line matchers key on ("block",
        # "actionable", …) so a Bugbot cell never spoofs a CR-finding route.
        local bb_block=false
        local bb_state_print="$bb_state"
        if [ "$bb_open" -lt 0 ]; then
            bb_block=true
            bb_state_print="parse-miss (bb_open=$bb_open — fail-closed)"
        elif [ "$bb_open" -gt 0 ]; then
            if [ "$has_bb_oob" = "true" ]; then
                bb_state_print="${bb_state} (${bb_open} unresolved — bugbot-out-of-band → WARN)"
                echo "WARN: bugbot-out-of-band label downgraded Bugbot block (${bb_open} unresolved cursor[bot] finding(s)) to WARN" >&2
            elif [ "$self_imp_only" = "true" ]; then
                # Self-improvement doc PR — auto-downgrade the Bugbot block to WARN
                # (mirrors bugbot-out-of-band, but keyed on detection, no label).
                bb_state_print="${bb_state} (${bb_open} unresolved — self-improvement doc PR → WARN)"
                echo "WARN: self-improvement doc PR (diff entirely under docs/self-improvement/**) — Bugbot gate auto-skipped (${bb_open} unresolved cursor[bot] finding(s) downgraded to WARN)" >&2
            else
                bb_block=true
                bb_state_print="${bb_state} (${bb_open} unresolved)"
                echo "BLOCK: Cursor Bugbot has ${bb_open} unresolved finding(s) on the head; resolve the cursor[bot] review thread(s) or apply the 'bugbot-out-of-band' label to merge without Bugbot review." >&2
            fi
        elif [ "$bb_state" = "TERMINAL" ]; then
            # Bugbot couldn't run / hit its usage limit and left no findings on the
            # head — a spend-cap state must never wedge merge. No-wedge short-circuit
            # (skips grace), mirroring the CR review-skipped fast-pass. Reached only
            # when bb_open == 0, so it never overrides real findings (plan Finding 1).
            bb_state_print="TERMINAL (Bugbot couldn't run / usage limit — no-wedge pass)"
        elif [ "$has_bb_oob" = "true" ]; then
            # Operator waiver with no open findings → pass + short-circuit grace.
            bb_state_print="bugbot-out-of-band (pass)"
        elif [ "$self_imp_only" = "true" ] && [ "$bb_state" = "STALE" ]; then
            # Self-improvement doc PR whose only Bugbot review is on a PRIOR commit:
            # short-circuit the grace wait — these PRs never wait on a Bugbot
            # re-review (ordered before the STALE-grace branch). ABSENT / clean-on-head
            # self-improvement PRs need no skip; they fall through to the normal
            # clean-pass print below, so the auto-skip label only appears when it is
            # actually load-bearing.
            bb_state_print="self-improvement-doc-pr (Bugbot STALE grace auto-skipped)"
        elif [ "$bb_state" = "STALE" ] && [ "$p" -lt "$BB_GRACE_POLLS" ]; then
            # Bugbot reviewed a PRIOR commit but not the current head yet — wait a
            # full CR-style grace window for it to re-review the new head.
            bb_block=true
            bb_state_print="STALE-grace (no Bugbot review on head — poll $((p+1))/$BB_GRACE_POLLS)"
        else
            # PASS: a clean on-head review (bb_open==0), STALE grace expired, or no
            # cursor[bot] artefacts at all (Bugbot-free PR / not engaged → never wedge).
            case "$bb_state" in
                ABSENT) bb_state_print="ABSENT (no Bugbot activity)" ;;
                STALE)  bb_state_print="STALE-grace-expired (pass)" ;;
                *)      bb_state_print="${bb_state} (clean)" ;;
            esac
        fi

        # H1: APPROVED CR review passes unconditionally per AGENTS.md § Merge
        # gates § CodeRabbit ("APPROVED → pass unconditionally (approval trumps
        # body)"). Previously the pass-check always required cr_open == 0, so
        # an APPROVED review on the current head + any unresolved non-outdated
        # CR thread (even one CR itself left for context) wedged the gate.
        # Decompose into an explicit `cr_open_blocks` so the intent is legible.
        # Computed BEFORE the Poll status line so the rate-limit handling below
        # (which may rewrite cr_state_print) is reflected on that line.
        local cr_open_blocks=false
        if [ "$cr_state" != "APPROVED" ] && [ "$cr_open" -ne 0 ]; then
            cr_open_blocks=true
        fi

        # ── CR rate-limit handling (PR-2) ─────────────────────────────────────
        # A CR rate-limit skip is TEMPORARY (CR re-reviews once its quota recovers),
        # so it must override whatever PASS the case-block computed via the generic
        # terminal cr_review_skipped path — otherwise a rate-limit "Review skipped"
        # on a CODE PR would falsely fast-pass with zero review. Two outcomes:
        #   • pure-docs PR  → auto-downgrade to WARN/pass, NO label needed. Markdown
        #     is never compiled, so a deferred CR review on a docs-only diff is
        #     harmless (deliverable 1: cr-review-skipped-pure-docs-auto-downgrade).
        #   • CODE PR       → block this poll (PAUSE/RETRY): the merge-gate keeps
        #     polling so CR recovers + re-reviews within the grace window.
        #     cr-out-of-band ALONE will NOT waive this; the operator must ALSO
        #     attest an explicit `cr-disposition:` label (enforced in the
        #     cr-out-of-band downgrade below). (deliverable 2:
        #     cr-rate-limit-code-pr-auto-pause.)
        # $cr_rate_limit_block scopes the disposition requirement to exactly this
        # case so a normal cr-out-of-band on a non-rate-limited block is unaffected.
        # Runs BEFORE the Poll line so cr_state_print reflects the rate-limit verdict.
        local cr_rate_limit_block=false
        if [ "$cr_rate_limited" = "true" ]; then
            if [ "$pure_docs" = "true" ]; then
                cr_pass=true
                cr_open_blocks=false
                cr_state_print="${cr_state_print} +rate-limit pure-docs-auto-downgrade (WARN)"
                echo "WARN: CodeRabbit rate-limited on a pure-docs PR (diff within docs/ / backlog/ / agents/scripts/ / *.md) — CR gate auto-downgraded to WARN (no label needed; markdown is never compiled). PR-2 cr-review-skipped-pure-docs-auto-downgrade." >&2
            elif [ "$cr_state" = "NONE" ]; then
                # CODE PR with NO current-head CR verdict: the rate-limit skip is
                # the only CR signal for this head, so block (PAUSE/RETRY) until CR
                # recovers + re-reviews within the grace window.
                cr_pass=false
                cr_rate_limit_block=true
                cr_state_print="${cr_state_print} +rate-limit CODE-PR-pause (block; pending CR re-review)"
                echo "BLOCK: CodeRabbit rate-limited on a CODE PR — pausing for CR to re-review on quota recovery. To merge before then, apply BOTH 'cr-out-of-band' AND a 'cr-disposition:<reason>' label (the disposition attests you consciously merged past an incomplete review). PR-2 cr-rate-limit-code-pr-auto-pause." >&2
            else
                # CODE PR that ALSO has a real current-head CR verdict
                # (APPROVED / COMMENTED / CHANGES_REQUESTED / STALE*): a rate-limit
                # comment is STALE — it survives from a PRIOR push and must NOT
                # override the legitimate current-head review. Leave cr_pass /
                # cr_open_blocks as the case-block computed them; only annotate the
                # print so the operator sees the rate-limit signal was ignored.
                cr_state_print="${cr_state_print} +rate-limit stale-on-prior-push (non-blocking; current-head ${cr_state} verdict wins)"
            fi
        fi

        printf 'Poll %d/%d — CI: %d/%d pass (%d fail, %d pending, %d warn-downgraded, %d req-missing) | CodeRabbit: %s (%d open) | Bugbot: %s (%d open) | User: %d | reviewDecision: %s\n' \
            $((p+1)) "$MAX_POLLS" $((ci_total - ci_fail - ci_pend - ci_warn_downgraded)) "$ci_total" \
            "$ci_fail" "$ci_pend" "$ci_warn_downgraded" "$req_absent" \
            "$cr_state_print" "$cr_open" "$bb_state_print" "$bb_open" "$user" "$review_decision"

        # merge-pipeline-04 residual: did CodeRabbit actually engage THIS head? A
        # cr-out-of-band downgrade (even WITH a cr-disposition attestation) may only
        # waive a CR gate that really ran — never a phantom one. Positive engagement
        # signals: a real review verdict (APPROVED / COMMENTED / CHANGES_REQUESTED /
        # a STALE prior-commit review), a CodeRabbit context on the head
        # (cr_context_present, StatusContext OR CheckRun, any state), a SUCCESS
        # StatusContext, an explicit terminal "Review skipped", a too-many-files
        # size-skip, or a rate-limit skip (the case cr-out-of-band exists for). With
        # NONE of these present, CR never ran and a disposition label alone must not
        # dismiss the gate — otherwise a bogus attestation on a CR-untouched PR
        # bypasses review entirely (the original merge-pipeline-04 hole, residual
        # after PR-3 added the disposition requirement).
        local cr_ran=false
        case "$cr_state" in
            APPROVED|COMMENTED|CHANGES_REQUESTED|STALE*) cr_ran=true ;;
        esac
        if [ "$cr_context_present" = 1 ] || [ "$cr_status_state" = "SUCCESS" ] \
           || [ "$cr_review_skipped" = true ] || [ "$cr_rate_limited" = true ] \
           || [ "$cr_size_skip_block" = true ]; then
            cr_ran=true
        fi

        # cr-out-of-band label: when present, downgrade a CR block to a WARN
        # (pass) — mirrors the tests/perf-out-of-band CI-downgrade pattern but
        # scoped to the CR gate ONLY. Covers both CR-gate signals: the state
        # verdict (cr_pass=false: CHANGES_REQUESTED, COMMENTED+actionable>0,
        # DISMISSED, STALE_WITH_FINDINGS, STALE_UNKNOWN, NONE-after-grace) and
        # unresolved CR-authored review threads (cr_open_blocks). CI
        # (ci_fail / ci_pend) and the user-comment gate (user) are NOT touched —
        # the label only waives CodeRabbit's own conditions. Like the other
        # out-of-band labels, it MUST NOT stay on the PR post-merge.
        if [ "$has_cr_oob" = "true" ] && { [ "$cr_pass" != true ] || [ "$cr_open_blocks" = true ]; }; then
            # PR-3 cr-out-of-band-disposition-trail: a cr-out-of-band downgrade now
            # ALWAYS requires a paired `cr-disposition:<reason>` attestation (a
            # `cr-disposition:` LABEL or a grep-able `cr-disposition:` marker line
            # in the PR body — $cr_disposition folds both). Without it the override
            # is NOT honoured and the CR block stands. This generalises the PR-2
            # rate-limit-only requirement to EVERY cr-out-of-band waiver so a generic
            # override can never silently bypass CR review with no recorded reason
            # (the override-merge ledger then always carries the disposition trail).
            if [ "$cr_disposition" != true ]; then
                if [ "$cr_rate_limit_block" = true ]; then
                    # Rate-limit-specific guidance (PR-2): the block is a transient
                    # skip — pause for CR to re-review, or attest a disposition.
                    echo "WARN: cr-out-of-band present but NOT honoured — a CR rate-limit skip on a CODE PR also requires a 'cr-disposition:<reason>' label or PR-body marker. Add one to merge past the rate-limited review, or wait for CR to re-review. PR-2 cr-rate-limit-code-pr-auto-pause." >&2
                else
                    echo "WARN: cr-out-of-band present but NOT honoured — a cr-out-of-band downgrade also requires a 'cr-disposition:<reason>' label or PR-body marker recording why CR review was waived. Add one to merge past the CR block (${cr_state_print}). PR-3 cr-out-of-band-disposition-trail." >&2
                fi
            elif [ "$cr_ran" != true ]; then
                # merge-pipeline-04 residual: disposition present, but CR never ran on
                # this head (no review, no CR context, no SUCCESS status, no terminal
                # "Review skipped", no size-skip, no rate-limit skip). A disposition
                # cannot waive a review that never happened — the CR block STANDS
                # (cr_pass stays false). Re-trigger '@coderabbitai review' or wait.
                echo "WARN: cr-out-of-band + cr-disposition present but NOT honoured — CodeRabbit never ran on head ${head_sha:0:8} (no review / CR context / SUCCESS status / 'Review skipped' / rate-limit skip). A disposition cannot waive a review that never happened; re-trigger '@coderabbitai review' or wait for CR. merge-pipeline-04 cr-out-of-band-requires-cr-ran." >&2
            else
                if [ "$cr_size_skip_block" = true ]; then
                    # Tailored message for the size-skip block — names the actual
                    # cause (CR skipped review, too many files) rather than the
                    # generic "CR block" so the operator's log is unambiguous.
                    echo "WARN: cr-out-of-band + cr-disposition — CR skipped review (too many files) overridden" >&2
                elif [ "$cr_rate_limit_block" = true ]; then
                    # cr-out-of-band + cr-disposition both present → honoured.
                    echo "WARN: cr-out-of-band + cr-disposition label downgraded CR rate-limit block (${cr_state_print}) to WARN" >&2
                else
                    echo "WARN: cr-out-of-band + cr-disposition label downgraded CR block (${cr_state_print}) to WARN" >&2
                fi
                cr_pass=true
                cr_open_blocks=false
                cr_overridden=true
            fi
        fi

        # Self-improvement doc PR — belt-and-suspenders CR-gate auto-skip beside
        # the .coderabbit.yaml path_filter that already makes CR post "Review
        # skipped" (the NONE+review-skipped fast-pass). Covers the rare residual CR
        # block (e.g. a STALE review left from before the path_filter landed).
        # Does NOT set cr_overridden — this is a documented auto-exemption, not a
        # label override; GATE_SNAPSHOT ledger attribution is deferred (see plan
        # § Out of scope). CI + user-comment gates are untouched, same as cr-oob.
        if [ "$self_imp_only" = "true" ] && { [ "$cr_pass" != true ] || [ "$cr_open_blocks" = true ]; }; then
            echo "WARN: self-improvement doc PR — CR gate auto-skipped (CR block '${cr_state_print}' downgraded to WARN)" >&2
            cr_pass=true
            cr_open_blocks=false
        fi

        # mergeStateStatus guard (secondary P1 fix). GitHub's own mergeability
        # verdict. BLOCKED = a required check is failing/missing or a required
        # review is absent; BEHIND = the head is behind base under strict
        # branch protection. Either means GitHub itself would reject the merge,
        # so GATES_PASSED MUST refuse — this catches the absent-required-check
        # false-pass even when the config-name cross-check above is inert (e.g.
        # jq/config unavailable). UNSTABLE is explicitly NOT blocked: non-required
        # checks pending/failing is normal and we merge on UNSTABLE routinely.
        # MERGE_GATES_IGNORE_MERGESTATE=true skips this guard for the documented
        # admin-merge escape (a positively-confirmed STALE-BLOCKED PR that is
        # actually all-green — AGENTS.md § Merge gates).
        #
        # DIRTY joins the set (CR #1996): a conflicted head is unmergeable by
        # construction — GitHub refuses the merge AND declines to build the head, so
        # its required contexts can never report. Previously only the required-absent
        # cross-check caught it, and then only when that detector was armed: an
        # all-green DIRTY head (one whose contexts DID report before the conflict
        # appeared) reached GATES_PASSED, and the merge then failed at the REST call.
        # That is the same false-pass class this change exists to close. DIRTY is a
        # COMPUTED verdict, not a not-yet-known one — GitHub reports UNKNOWN while
        # mergeability is still being determined — so this cannot fire on a pending
        # computation. MERGE_GATES_IGNORE_MERGESTATE remains the escape.
        local gh_merge_state="${fields[17]:-UNKNOWN}"
        local mergestate_blocks=false
        if [ "${MERGE_GATES_IGNORE_MERGESTATE:-}" != "true" ]; then
            case "$gh_merge_state" in
                BLOCKED|BEHIND|DIRTY) mergestate_blocks=true ;;
            esac
        fi

        if [ "$ci_fail" -eq 0 ] && [ "$ci_pend" -eq 0 ] && \
           [ "$req_absent" -eq 0 ] && [ "$mergestate_blocks" = false ] && \
           [ "$cr_pass" = true ] && [ "$cr_open_blocks" = false ] && \
           [ "$bb_block" = false ] && [ "$self_stale" = false ] && \
           [ "$user" -eq 0 ] && [ "$review_pass" = true ]; then
            # Diagnostic: surface GitHub's mergeStateStatus alongside our pass
            # decision so the operator can correlate when GH says BLOCKED while
            # our gates pass (typically branch-protection summary-only / stale
            # GH mergeability cache — REST squash-merge still works). Note: with
            # the guard above, a non-overridden BLOCKED/BEHIND never reaches here
            # — so this INFO now only fires for the overridden case or other
            # non-CLEAN/UNSTABLE states (e.g. UNKNOWN handled separately).
            if [ "$gh_merge_state" != "CLEAN" ] && [ "$gh_merge_state" != "UNSTABLE" ] && [ "$gh_merge_state" != "UNKNOWN" ]; then
                echo "INFO: merge-gates pass; GitHub mergeStateStatus=$gh_merge_state may be stale or branch-protection summary-only (MERGE_GATES_IGNORE_MERGESTATE override active). REST squash-merge contract still applies." >&2
            fi
            # GATE_SNAPSHOT — emitted ONLY on the PASS path, naming the checks an
            # override label actually bypassed at the decision instant so the
            # merge actor can write a lossless merge-snapshot row
            # (mandatory-merge-snapshot-on-override-merge; ADR-0017).
            #   cr_override=1 iff cr-out-of-band waived a real CR block above.
            #   downgraded=<rest of line> is the CI checks tests-/perf-out-of-band
            #     turned FAIL→WARN (dg_names — the SAME $downgraded the GATE_FILTER
            #     computed, no forked logic). It is jq `join(", ")` output so it
            #     contains spaces+commas; it is therefore the LAST field and spans
            #     the entire remainder of the line (the watcher reads everything
            #     after "downgraded=" verbatim — no whitespace tokenisation).
            # Both empty/0 on a clean (no-override) pass, so the snapshot writer
            # leaves redChecks=[] and never double-flags a moot label. Distinct
            # prefix like GATE_CARRY; the watcher parses it on the merged path only.
            local cr_override_flag=0
            [ "$cr_overridden" = true ] && cr_override_flag=1
            printf 'GATE_SNAPSHOT cr_override=%s downgraded=%s\n' \
                "$cr_override_flag" "${dg_names:-}"
            echo "GATES_PASSED"
            return 0
        fi

        # When the ONLY thing blocking is mergeStateStatus, name it explicitly —
        # otherwise the operator sees an all-green Poll line with no obvious cause.
        if [ "$mergestate_blocks" = true ]; then
            echo "BLOCK: GitHub mergeStateStatus=$gh_merge_state (set MERGE_GATES_IGNORE_MERGESTATE=true only for a positively-confirmed STALE-BLOCKED PR that is actually all-green — AGENTS.md § Merge gates)." >&2
            # Silent-BLOCKED cause naming (poller-bot-thread-filter-diverges-
            # from-branch-protection, tooling P2): the user-comment gate excludes
            # bot threads, but required_conversation_resolution counts every
            # unresolved thread — so BLOCKED with every poller gate green and
            # open threads is almost always the conversation gate, and
            # cr-out-of-band steers INTO it (waiving the CR gate is exactly when
            # CR threads stay unresolved). Name it on this poll instead of
            # letting the operator burn the budget and find the threads by hand
            # (#1937: ten CR threads, ~90 min). Counts come from the same
            # GraphQL response; only the branch-protection read is extra, cached
            # per run, and a probe miss reports "may require" rather than
            # staying silent (fail toward naming the likely cause — same
            # posture as pr-blocked-why.sh's unknown handling).
            if [ "$gh_merge_state" = "BLOCKED" ] && [ "${thr_total:-0}" -gt 0 ] && \
               [ "$ci_fail" -eq 0 ] && [ "$ci_pend" -eq 0 ] && [ "$req_absent" -eq 0 ] && \
               [ "$cr_pass" = true ] && [ "$cr_open_blocks" = false ] && \
               [ "$bb_block" = false ] && [ "$self_stale" = false ] && \
               [ "$user" -eq 0 ] && [ "$review_pass" = true ]; then
                if [ -z "$conv_res_cache" ]; then
                    if [ -n "${MERGE_GATES_CONV_RES_REQUIRED+x}" ]; then
                        conv_res_cache="${MERGE_GATES_CONV_RES_REQUIRED:-unknown}"
                    else
                        # Protection must be read for the PR's ACTUAL base
                        # branch — a release/hotfix PR based off a branch other
                        # than the config-named one would otherwise be judged
                        # by the wrong branch's conversation-resolution
                        # setting. Config (then "develop") is the fallback
                        # when the base read fails, not the primary source.
                        local conv_base=""
                        conv_base=$(gh api "repos/${owner}/${repo}/pulls/${prNumber}" \
                            --jq '.base.ref' 2>/dev/null) || conv_base=""
                        if [ -z "$conv_base" ] && command -v jq >/dev/null 2>&1; then
                            conv_base=$(jq -r '.branch_protection.branch // "develop"' \
                                "${MERGE_GATES_CONFIG_FILE:-$SCRIPT_DIR/../../../project.config.json}" 2>/dev/null) || conv_base=""
                        fi
                        [ -n "$conv_base" ] || conv_base="develop"
                        conv_res_cache=$(gh api "repos/${owner}/${repo}/branches/${conv_base}/protection" \
                            --jq 'if .required_conversation_resolution.enabled == true then "true" else "false" end' 2>/dev/null) \
                            || conv_res_cache="unknown"
                        [ -n "$conv_res_cache" ] || conv_res_cache="unknown"
                    fi
                fi
                if [ "$conv_res_cache" != "false" ]; then
                    # A field-32 parse miss (thr_user_cnt=-1) must WITHHOLD the
                    # user/bot breakdown, not default the user count to 0 and
                    # attribute every thread to bots — the total alone still
                    # names the cause (the never-invent-a-count posture the
                    # field-31 comment promises).
                    local thr_split=""
                    if [ "$thr_user_cnt" -ge 0 ] 2>/dev/null; then
                        local thr_bot_n=$(( thr_total - thr_user_cnt ))
                        [ "$thr_bot_n" -ge 0 ] || thr_bot_n=0
                        thr_split=" (${thr_user_cnt} user, ${thr_bot_n} bot)"
                    fi
                    local conv_verb="requires"
                    [ "$conv_res_cache" = "unknown" ] && conv_verb="may require (protection probe failed)"
                    echo "BLOCK: mergeStateStatus=BLOCKED with all other gates green; ${thr_total} unresolved review thread(s)${thr_split} and branch protection ${conv_verb} conversation resolution. Bot threads count too — cr-out-of-band waives the poller's CR gate only, never branch protection. Resolve the threads (scripts/dev/pr-blocked-why.sh classifies them vs HEAD) or the merge will never unblock." >&2
                fi
            fi
        fi

        local elapsed=$(( $(date +%s) - start ))
        if [ "$elapsed" -ge "$TIMEOUT_SECONDS" ]; then
            echo "GATES_TIMEOUT"
            return 2
        fi

        if [ "$p" -lt $((MAX_POLLS-1)) ]; then
            sleep "$POLL_INTERVAL"
        fi
    done

    # Emit the cross-poll guard/streak state so a single-poll caller (the
    # watcher runs with MERGE_GATES_MAX_POLLS=1) can persist it and seed the
    # next cycle via MERGE_GATES_PRIOR_* — the in-process locals reset every
    # invocation otherwise (same class as the registry-persisted CR-NONE grace
    # counter). Distinct `GATE_CARRY` prefix; the per-iteration `Poll …` line
    # above always precedes it on this return path, so a caller parsing the
    # status line (first `Poll ` line) is unaffected.
    printf 'GATE_CARRY nudge_head=%s stale_head=%s stale_streak=%s none_head=%s none_streak=%s outage_head=%s outage_streak=%s outage_since=%s\n' \
        "$rereview_posted_head" "$stale_head" "$stale_streak" "$none_head" "$none_streak" \
        "$outage_head" "$outage_streak" "$outage_since"
    return 1
}

# ----------------------------------------------------------------------------
# gh_pr_ready_idempotent — moved to merge-gates.d/00-common.sh (sourced at top).
# ----------------------------------------------------------------------------

# ----------------------------------------------------------------------------
# CLI entry point — only when invoked directly (not sourced).
# ----------------------------------------------------------------------------
if [ "${BASH_SOURCE[0]}" = "${0}" ]; then
    poll_merge_gates "$@"
fi

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
#   MERGE_GATES_MAX_POLLS        — max poll count (default 60)
#   MERGE_GATES_TIMEOUT_SECONDS  — wall-clock budget (default 3600)
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
#   MERGE_GATES_FRESHNESS        — gate-logic self-freshness guard. off (default) |
#                                  warn | block. When "block", refuse GATES_PASSED if
#                                  THIS script's on-disk blob differs from
#                                  origin/develop:agents/scripts/core/merge-gates.sh
#                                  (fail-closed) — an unattended merger running a
#                                  STALE checkout would otherwise enforce out-of-date
#                                  gate logic (the #1428 gate escape: a host tree
#                                  parked behind develop merged past a RED non-required
#                                  "Intent section" its old allow-list lacked).
#                                  smatchet-merge-watcher sets "block"; "warn" prints
#                                  the divergence without blocking (local-dev default
#                                  if opted in); "off" disables the check entirely.
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
#
# Return codes (gh_pr_ready_idempotent):
#   0 — PR is now ready (or already was)
#   6 — unknown failure (caller halts; do not auto-merge)
# ----------------------------------------------------------------------------

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_QUERY_FILE="$SCRIPT_DIR/merge-gates.graphql"

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
MERGE_GATES_BLOCK_ALLOWLIST_RE="."

# Source prompt shim so `ask_user_question` is callable from the caller's
# integration flow. Lazy — only if available.
if [ -f "$SCRIPT_DIR/merge-gates-prompt.sh" ]; then
    # shellcheck source=agents/scripts/core/merge-gates-prompt.sh
    source "$SCRIPT_DIR/merge-gates-prompt.sh"
fi

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
    # Default off so existing callers + local dev are unaffected unless they opt in;
    # the watcher sets MERGE_GATES_FRESHNESS=block.
    local fresh_mode="${MERGE_GATES_FRESHNESS:-off}"
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
        local _self_relpath="agents/scripts/core/merge-gates.sh"
        local _run_blob _dev_blob
        if [ -n "${MERGE_GATES_FRESH_RUN_BLOB:-}" ]; then
            # Test-only override — bypass git, use injected blobs so the bats suite
            # exercises the guard deterministically without a git/network dependency.
            _run_blob="$MERGE_GATES_FRESH_RUN_BLOB"
            _dev_blob="${MERGE_GATES_FRESH_DEV_BLOB:-}"
        else
            local _root
            _root="$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel 2>/dev/null)"
            if [ -n "$_root" ]; then
                # Bounded ref refresh (refs only; never touches the worktree). A failed
                # fetch must NOT silently compare against a stale local origin/develop —
                # blank _dev_blob so the unverifiable branch below fails closed (#1428 CR).
                local _fetch_ok=true
                git -C "$_root" fetch -q --no-tags origin develop >/dev/null 2>&1 || _fetch_ok=false
                _run_blob="$(git -C "$_root" hash-object "${BASH_SOURCE[0]}" 2>/dev/null)"
                _dev_blob="$(git -C "$_root" rev-parse -q --verify "origin/develop:$_self_relpath" 2>/dev/null)"
                if [ "$_fetch_ok" != true ]; then
                    _dev_blob=""
                fi
            else
                _run_blob=""
                _dev_blob=""
            fi
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
                echo "BLOCK: merge-gates.sh differs from origin/develop (running ${_run_blob:0:12} != develop ${_dev_blob:0:12}) — this merger would enforce out-of-date gate logic. Refresh the checkout to origin/develop and restart. Refusing GATES_PASSED (fail-closed). See postmortems.md #1428." >&2
                self_stale=true
            else
                echo "WARN: merge-gates.sh differs from origin/develop (running ${_run_blob:0:12} != develop ${_dev_blob:0:12}); gate logic may be out of date (MERGE_GATES_FRESHNESS=warn — not blocking)." >&2
            fi
        fi
    fi

    local POLL_INTERVAL="${MERGE_GATES_POLL_INTERVAL:-60}"
    local MAX_POLLS="${MERGE_GATES_MAX_POLLS:-60}"
    local TIMEOUT_SECONDS="${MERGE_GATES_TIMEOUT_SECONDS:-3600}"
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
    # stream (31 lines) that the poll loop reads with `mapfile`. The exact jq
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
    # honoured; PR-3 cr-out-of-band-disposition-trail).
    # The trailing fields must all be non-empty so the `data=$(gh …)` command
    # substitution (trailing-newline collapse) never strips one and deflates the
    # 31-field count (tripping the fail-closed assertion). reqAbsentCount (22),
    # crReviewSkipped (23), bbState (24, ABSENT-default), bbOpen (25, numeric),
    # bbOob (26), selfImpOnly (27), pureDocs (28), crRateLimited (29) and
    # crDisposition (30) are all non-empty tokens, so they are safe at the tail.
    local GATE_FILTER
    GATE_FILTER='
.data.repository.pullRequest as $pr
| ($pr.headRefOid // "") as $sha
| ([$pr.labels.nodes[]?.name]) as $labels
# selfImpOnly — TRUE iff the PR diff is ENTIRELY under docs/self-improvement/**
# (the self-improvement backlog / postmortem ledger). Such a PR auto-skips the
# CR + Bugbot gates (user ask 2026-06-20; plan self-improvement-pr-review-exemption).
# Fail-safe to FALSE on any uncertainty: an empty file list (length 0, the vacuous
# all() guarded out), a >100-file page (pageInfo.hasNextPage, cannot see every
# path), or absent files (legacy fixtures) all yield FALSE for full gates. Computed
# here; consumed by the Bugbot bucket + the CR belt-and-suspenders downgrade below.
# (NB: no apostrophes in this single-quoted jq filter string.)
| (($pr.files.nodes // []) | map(.path)) as $changedPaths
| (($pr.files.pageInfo.hasNextPage // false)) as $filesOverflow
| (($changedPaths | length) > 0
   and ($filesOverflow | not)
   and ($changedPaths | all(startswith("docs/self-improvement/")))) as $selfImpOnly
| ($labels | any(. == "tests-out-of-band")) as $tests
| ($labels | any(. == "perf-out-of-band")) as $perf
| ($labels | any(. == "intent-out-of-band")) as $intent
| ($labels | any(. == "plan-lock-out-of-band")) as $planlock
| ($labels | any(. == "cr-out-of-band")) as $cr
# crDisposition — an explicit operator attestation supplied EITHER as a label
# prefixed `cr-disposition:` (e.g. `cr-disposition:rate-limit-acked`) OR as a
# grep-able `cr-disposition:<reason>` marker line in the PR BODY. Whenever
# `cr-out-of-band` would downgrade a CR block, this disposition is REQUIRED —
# `cr-out-of-band` alone is NOT honoured (PR-3 cr-out-of-band-disposition-trail,
# generalising the PR-2 cr-rate-limit-code-pr-auto-pause requirement to EVERY
# cr-out-of-band downgrade): it proves the operator consciously waived CR review
# with a recorded reason rather than reflexively slapping a generic override on.
# Body match: `cr-disposition:` followed by any non-empty reason on the line
# (regex tolerates leading whitespace / list markers). (NB: no apostrophes in
# this single-quoted jq filter string.)
| (($labels | any(startswith("cr-disposition:")))
   or (($pr.body // "") | test("cr-disposition:[[:space:]]*[^[:space:]]"; "i"))) as $crdisposition
| ($labels | any(. == "bugbot-out-of-band")) as $bb
| ((($pr.commits.nodes[0].commit.statusCheckRollup.contexts.nodes) // [])
   | map(. + {_k: (if .__typename == "CheckRun" then ["CheckRun", (.name // "")]
                   else ["StatusContext", (.context // "")] end)})
   | group_by(._k) | map(sort_by(.startedAt // "") | .[-1]) | map(del(._k))) as $ctx
| ([$ctx[] | select(.isRequired == true)]) as $req
# $blocking — the set the gate must wait on: REQUIRED contexts PLUS every
# non-required, non-advisory-named check (block-on-any-red — the allow-list
# regex now matches all names). The $failing set below already unions these
# (the #923 fix), but the PENDING count historically counted only $req — so a
# non-required blocking check still IN_PROGRESS (not yet terminal) was
# invisible: not failing (not terminal) and not pending (not required) →
# GATES_PASSED fired before ASAN/Coverage/Bucket finished and the merge beat
# the sanitizer to the line (#1237/#1232/#1227/#1220/#1198 ASAN/Coverage
# escapes). Counting $blocking (not $req) for pending closes it.
| ([$ctx[] | select(
      (.isRequired == true)
      or ((if .__typename == "CheckRun" then (.name // "") else (.context // "") end)
          | (test("__BLOCK_ALLOWLIST_RE__"; "i")
             and (ascii_downcase | contains("advisory") | not))))]) as $blocking
| (__REQUIRED_CONTEXTS__) as $reqNames
| ([$ctx[] | (if .__typename == "CheckRun" then (.name // "") else (.context // "") end)]) as $ctxNames
| ([$reqNames[] | select(. as $n | ($ctxNames | any(. == $n)) | not)]) as $reqAbsent
| ([$ctx[] | select(
      ((.__typename == "CheckRun" and .status == "COMPLETED" and ((.conclusion // "") | IN("FAILURE","TIMED_OUT","CANCELLED","ACTION_REQUIRED","STARTUP_FAILURE"))) or
       (.__typename == "StatusContext" and ((.state // "") | IN("FAILURE","ERROR"))))
      and
      # A failing check blocks if it is REQUIRED (unchanged), OR it is ANY
      # non-required check whose name does not contain "advisory"
      # (block-on-any-red — the spliced regex matches every name; see the
      # MERGE_GATES_BLOCK_ALLOWLIST_RE comment for the full rationale + the
      # curated-era history: #923 Coverage escape closed 2026-06-06; Perf
      # PR-fast 2026-06-07; Android security gate 2026-06-09 — the advisory
      # mobile jobs let a green develop ship mobile breakage #1021/#1064; Fuzz
      # smoke 2026-06-16 — #1301 merged past a RED fuzz-smoke whose libFuzzer
      # driver failed to compile, paired with the continue-on-error guard on
      # the stochastic fuzz STEP so only the deterministic build/ctest reds the
      # check). The curated list grew one gate-escape at a time; the flip
      # retires the curation so the next escape class is impossible by
      # construction. A red on an "advisory"-named check (none exist post-
      # rename; the token is the sanctioned future escape) still passes.
      ((.isRequired == true)
       or ((if .__typename == "CheckRun" then (.name // "") else (.context // "") end)
           | (test("__BLOCK_ALLOWLIST_RE__"; "i")
              and (ascii_downcase | contains("advisory") | not)))))]) as $failing
| ([$failing[] | select(
      ($tests and .__typename == "CheckRun" and .name == "Test-delta gate") or
      ($perf  and .__typename == "CheckRun" and ((.name // "") | startswith("Perf PR-fast"))) or
      ($intent and .__typename == "CheckRun" and .name == "Intent section") or
      ($planlock and .__typename == "CheckRun" and .name == "Plan-lock gate"))]) as $downgraded
| ([$pr.reviews.nodes[] | select(.author.login == "coderabbitai" or .author.login == "coderabbitai[bot]")]) as $crall
| (if ($crall | length) == 0 then "NONE"
   else (([$crall[] | select(.commit.oid == $sha)]) as $cur
         | if ($cur | length) == 0 then "STALE" else ($cur | sort_by(.submittedAt) | .[-1].state) end) end) as $crstate
| (if ($crall | length) == 0 then ""
   else (([$crall[] | select(.commit.oid == $sha)]) as $cur
         | if ($cur | length) > 0 then ($cur | sort_by(.submittedAt) | .[-1].body // "")
           else ($crall | sort_by(.submittedAt) | .[-1].body // "") end) end) as $crbody
| ([$pr.comments.nodes[]?
    | select(.author.login == "coderabbitai" or .author.login == "coderabbitai[bot]")
    | (.body // "")]
   | any(
       contains("skip review by coderabbit.ai")
       or (test("##[[:space:]]*Review skipped"; "i") and (ascii_downcase | contains("too many files"))))) as $crskip
# crReviewSkipped — the GENERIC terminal "Review skipped" (docs-only /
# path-filtered / trivial diff per .coderabbit.yaml): the "CodeRabbit"
# StatusContext is SUCCESS and its description says "Review skipped" WITHOUT
# "too many files" (the size-skip variant, handled by $crskip + the NONE size
# branch). This is TERMINAL — CR processed the PR and declined an incremental
# review, so no inline review will ever land and the NONE+status-SUCCESS grace
# must NOT wait it out (PR #976 burned ~10 cycles on a docs-only PR). Distinct
# from $crskip (a too-many-files BLOCK read from a CONVERSATION COMMENT); this
# reads the STATUS description and is a PASS signal.
| ([$pr.commits.nodes[0].commit.statusCheckRollup.contexts.nodes[]?
    | select(.__typename == "StatusContext" and .context == "CodeRabbit"
             and (.state == "SUCCESS"))
    | (.description // "")]
   | any((test("review skipped"; "i"))
         and ((test("too many files"; "i")) | not))) as $crreviewskipped
# crRateLimited — CR declined / deferred the review because it hit its rate
# limit (PR-2 cr-review-skipped-pure-docs-auto-downgrade /
# cr-rate-limit-code-pr-auto-pause). Distinct from $crreviewskipped (a clean
# path-filtered/docs skip, a PASS) and $crskip (the too-many-files size skip):
# a rate-limit skip is TEMPORARY — CR will re-review once its quota recovers, so
# it must NOT be treated as a terminal pass on a CODE PR. Read from BOTH the CR
# conversation-comment bodies AND the "CodeRabbit" StatusContext description
# (CR surfaces the rate-limit on either surface). Regex tolerates "rate limit",
# "rate-limited", "rate limited", and the common "try again later" phrasing.
# (NB: no apostrophes in this single-quoted jq filter string.)
| (([$pr.comments.nodes[]?
     | select(.author.login == "coderabbitai" or .author.login == "coderabbitai[bot]")
     | (.body // "")]
    + [$pr.commits.nodes[0].commit.statusCheckRollup.contexts.nodes[]?
       | select(.__typename == "StatusContext" and .context == "CodeRabbit")
       | (.description // "")])
   | any(test("rate.?limit"; "i") or test("try again later"; "i"))) as $crratelimited
# pureDocs — the PR diff is strictly within the is-pure-docs-diff.sh allow-list
# (docs/ , backlog/ , agents/scripts/ , or any *.md ANYWHERE). Mirrors that
# script over the PR file list so the poller can apply the IDENTICAL pure-docs
# verdict without a local checkout. Used by the rate-limit auto-downgrade
# (deliverable 1): a rate-limit skip on a pure-docs PR is harmless to fast-pass
# (markdown is never compiled), while a rate-limit skip on a CODE PR must pause /
# require an explicit disposition (deliverable 2). Fail-safe FALSE on an empty
# file list, a >100-file page (cannot see every path), or absent files.
| (($changedPaths | length) > 0
   and ($filesOverflow | not)
   and ($changedPaths | all(test("^(docs/|backlog/|agents/scripts/|.*[.]md$)")))) as $pureDocs
# Bugbot (cursor[bot]) — mirrors the $crall/$crstate machinery. $bball = all
# reviews authored by cursor[bot] (its summary review, always state COMMENTED).
# $bbterminal = TRUE when a cursor[bot] CONVERSATION (issue) comment body carries
# a couldn.t-run / usage-limit status message. The regex uses couldn.t (a jq `.`
# wildcard, NOT a literal apostrophe) so it (a) stays inside the bash
# single-quoted filter — a literal apostrophe here would close the string — and
# (b) tolerates a straight or unicode apostrophe. $bbstate folds the head-review
# state, the terminal signal, the engaged-but-stale signal, and the no-activity
# signal into one token — decision order is load-bearing (see the bash bucket):
# a head review wins, else TERMINAL (no-wedge), else STALE (prior-commit review
# → grace), else ABSENT (Bugbot-free / not engaged → never waited on).
| ([$pr.reviews.nodes[] | select(.author.login == "cursor" or .author.login == "cursor[bot]")]) as $bball
| ([$pr.comments.nodes[]?
    | select(.author.login == "cursor" or .author.login == "cursor[bot]")
    | (.body // "")]
   | any(test("couldn.t run"; "i") or test("usage limit"; "i"))) as $bbterminal
| (([$bball[] | select(.commit.oid == $sha)]) as $bbcur
   | if ($bbcur | length) > 0 then ($bbcur | sort_by(.submittedAt) | .[-1].state)
     elif $bbterminal then "TERMINAL"
     elif ($bball | length) > 0 then "STALE"
     else "ABSENT" end) as $bbstate
| (
    ($pr.state // "UNKNOWN"),
    $sha,
    (((($pr.commits.nodes[0].commit.statusCheckRollup.contexts.pageInfo.hasNextPage // false)
       or ($pr.reviews.pageInfo.hasNextPage // false)
       or ($pr.reviewThreads.pageInfo.hasNextPage // false)
       or ($pr.comments.pageInfo.hasNextPage // false)
       or ($pr.labels.pageInfo.hasNextPage // false)
       or (any($pr.reviewThreads.nodes[]?; .comments.pageInfo.hasNextPage // false)))) | tostring),
    ($tests | tostring),
    ($perf | tostring),
    ($req | length),
    ([$failing[] | select(. as $f | ($downgraded | any(.name == $f.name and .__typename == $f.__typename)) | not)] | length),
    ([$blocking[] | select(
        (.__typename == "CheckRun" and .status != "COMPLETED") or
        (.__typename == "StatusContext" and ((.state // "") | IN("PENDING","EXPECTED"))))] | length),
    ($downgraded | length),
    ([$downgraded[].name] | join(", ")),
    $crstate,
    (($crbody | split("\n"))[0] // ""),
    ([$pr.reviewThreads.nodes[] | select(.isResolved == false and .isOutdated == false
        and any(.comments.nodes[]; .author.login == "coderabbitai" or .author.login == "coderabbitai[bot]"))] | length),
    ([$pr.commits.nodes[0].commit.statusCheckRollup.contexts.nodes[]?
      | if (.__typename == "StatusContext" and .context == "CodeRabbit") then .state
        elif (.__typename == "CheckRun"
              and ((.name == "CodeRabbit") or (.name == "CR findings (0 actionable)"))
              and (.conclusion != null))
          then (if ((.conclusion) | IN("SUCCESS","NEUTRAL","SKIPPED")) then "SUCCESS" else .conclusion end)
        else empty end] | (.[0] // "ABSENT")),
    ([$pr.reviewThreads.nodes[]? | .comments.nodes[]?
      | select((.author.login == "coderabbitai" or .author.login == "coderabbitai[bot]") and (.commit.oid // "") == $sha)] | length),
    (([$pr.comments.nodes[] | select(.author.__typename != "Bot" and ((.author.login // "") | ascii_downcase) != ("__ORCH_USER__" | ascii_downcase))] | length)
     + ([$pr.reviewThreads.nodes[] | select(.isResolved == false and .isOutdated == false
          and any(.comments.nodes[]; .author.__typename != "Bot" and ((.author.login // "") | ascii_downcase) != ("__ORCH_USER__" | ascii_downcase)))] | length)),
    ($pr.reviewDecision // "NONE"),
    ($pr.mergeStateStatus // "UNKNOWN"),
    ($cr | tostring),
    ($crskip | tostring),
    (if ([$pr.commits.nodes[0].commit.statusCheckRollup.contexts.nodes[]?
          | select((.__typename == "StatusContext" and .context == "CodeRabbit")
                or (.__typename == "CheckRun"
                    and ((.name == "CodeRabbit") or (.name == "CR findings (0 actionable)"))))] | length) > 0
     then 1 else 0 end),
    ($reqAbsent | join(", ")),
    ($reqAbsent | length),
    ($crreviewskipped | tostring),
    $bbstate,
    ([$pr.reviewThreads.nodes[] | select(.isResolved == false and .isOutdated == false
        and any(.comments.nodes[]; .author.login == "cursor" or .author.login == "cursor[bot]"))] | length),
    ($bb | tostring),
    ($selfImpOnly | tostring),
    ($pureDocs | tostring),
    ($crratelimited | tostring),
    ($crdisposition | tostring)
  )
'
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

        # Parse the gh --jq field stream — 31 fixed-order lines (see GATE_FILTER
        # field map above). gh --jq errors already routed through the gh-fail
        # path above; this guards a truncated/partial body → fail closed (retry).
        local fields
        # Strip CR — Windows jq builds (and gh's bundled jq on Windows) emit
        # CRLF, which would leave a trailing \r on every field (e.g. pr_state
        # "OPEN\r" != "OPEN" → spurious return-4).
        data="${data//$'\r'/}"
        mapfile -t fields <<<"$data"
        if [ "${#fields[@]}" -ne 31 ]; then
            # Exactly 31 expected. Any other count (a field value with an embedded
            # newline would inflate it, misaligning fields[n]) → fail closed (CR #511).
            gh_fails=$((gh_fails+1))
            echo "Poll $((p+1)): gate filter returned ${#fields[@]} fields (expected 31); transient ($gh_fails/3)"
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
        if [ "$req_absent" -gt 0 ]; then
            echo "BLOCK: required-missing: ${req_absent_names} (required by branch protection but absent from the head rollup — never ran; e.g. a GITHUB_TOKEN bot push that did not re-trigger CI)." >&2
        elif [ "$req_absent" -lt 0 ]; then
            echo "WARN: required-context check returned ${req_absent} (filter/parse miss); failing closed on the required-absent gate this poll." >&2
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
        local gh_merge_state="${fields[17]:-UNKNOWN}"
        local mergestate_blocks=false
        if [ "${MERGE_GATES_IGNORE_MERGESTATE:-}" != "true" ]; then
            case "$gh_merge_state" in
                BLOCKED|BEHIND) mergestate_blocks=true ;;
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
    printf 'GATE_CARRY nudge_head=%s stale_head=%s stale_streak=%s none_head=%s none_streak=%s\n' \
        "$rereview_posted_head" "$stale_head" "$stale_streak" "$none_head" "$none_streak"
    return 1
}

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

# ----------------------------------------------------------------------------
# CLI entry point — only when invoked directly (not sourced).
# ----------------------------------------------------------------------------
if [ "${BASH_SOURCE[0]}" = "${0}" ]; then
    poll_merge_gates "$@"
fi

#!/usr/bin/env bash
# agents/scripts/core/merge-gates.sh
# ----------------------------------------------------------------------------
# Merge-gates poller for the orchestrator + git-janitor ship-loop.
#
# Polls four conditions on a PR via one `gh api graphql` call:
#   1. CI — every required check passes (CheckRun terminal SUCCESS/NEUTRAL/SKIPPED;
#      StatusContext state == SUCCESS) PLUS any non-required check on the
#      meant-to-block allow-list (name ~ Coverage|Sanitizer, non-advisory)
#      — see $failing below + postmortems.md 2026-06-06 "#923".
#   2. CodeRabbit — latest review on current headRefOid is not CHANGES_REQUESTED;
#      zero unresolved non-outdated review threads contain a CodeRabbit comment
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
#   cr-out-of-band    → downgrades a CodeRabbit block → WARN (CR gate only;
#                       CI + user-comment gates still bind)
#   bugbot-out-of-band → downgrades a Cursor Bugbot block → WARN (Bugbot gate
#                       only; CI + CR + user-comment gates still bind)
# Downgraded failures are logged on stderr but do NOT contribute to ci_fail
# (CI downgrades) / do NOT block the CR gate (cr-out-of-band) / do NOT block
# the Bugbot gate (bugbot-out-of-band).
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
# Meant-to-block allow-list — SINGLE SOURCE OF TRUTH.
# A non-required RED check still BLOCKS a merge when its name matches this
# regex AND is not explicitly "advisory" (see the $failing jq sub-expression
# below, which splices __BLOCK_ALLOWLIST_RE__ from this constant). Closes the
# #923 gate-escape (watcher auto-merged past a RED non-required "Coverage").
# Other tooling that must apply the IDENTICAL allow-list (e.g.
# safe-admin-merge.sh) sources this file and reads MERGE_GATES_BLOCK_ALLOWLIST_RE
# rather than duplicating the regex — change it HERE and every consumer follows.
#   Coverage / Sanitizer / Perf PR-fast / Android security gate / Fuzz smoke / Intent section
# (history: Perf PR-fast 2026-06-07; Android security gate 2026-06-09;
#  Fuzz smoke 2026-06-16 — gates the #1301 class (a fuzz-relevant PR that breaks
#  the DETERMINISTIC configure/build/ctest of the libFuzzer drivers). Only safe
#  because fuzz-smoke.yml's time-boxed fuzz STEP is continue-on-error on PRs
#  (advisory) while those build steps stay hard-fail — so the one check name
#  reds on a real build break but NOT on stochastic crash discovery. Adding it
#  here WITHOUT that step-guard would re-introduce the Bucket-style poller jam;
#  the two ship together (see fuzz-smoke.yml + postmortems.md 2026-06-16 #1301);
#  Bucket launch-smoke (Mesa GL) ADDED 2026-06-18 — the GRADUATED, DEDICATED form
#  of the Mesa dead-harness gate. The broad `Bucket-` token is DELIBERATELY NOT
#  here: the continue-on-error bucket-C / bucket-E jobs ALSO run flaky
#  screenshot-diff / ImGui-Test-Engine lanes (infra.md `bucket-mesa-exe-boot` P1 /
#  `ci-infra-flake-reds-masquerade-as-real-breakage` item (c)), so matching
#  `Bucket-` would re-block merges on stochastic rendering flake — the exact jam
#  the 2026-06-15 advisory-flip solved. Instead the dead-harness boot check now
#  lives in its OWN hard-fail job named `Bucket launch-smoke (Mesa GL)` (see
#  build-and-test.yml; NO continue-on-error), and ONLY that stable name is on the
#  allow-list. bucket-C/E stay advisory; this one blocks. #1370 fixed the `--spawn`
#  teardown exit-code that previously red-walled the smoke, so it is now reliably
#  green and safe to block on. postmortem-owed.sh sources this constant (no
#  separate copy to keep in sync since the de-dup), so one edit here covers both.)
#  "Intent section" added 2026-06-18 (pr-intent-capture-hardening #5, ADR-0022):
#  the doc-validation Intent gate now exits non-zero on a missing/empty `## Intent`;
#  routed onto the blocking path here rather than project.config.json
#  branch_protection (which would need merge_group reporting or deadlock the queue).
#  Override hatch: the `intent-out-of-band` label.
MERGE_GATES_BLOCK_ALLOWLIST_RE="Coverage|Sanitizer|Perf PR-fast|Android security gate|Fuzz smoke|Bucket launch-smoke [(]Mesa GL[)]|Intent section"

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
    # stream (27 lines) that the poll loop reads with `mapfile`. The exact jq
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
    # (bugbot-out-of-band label).
    # bbOob is LAST because it is always a non-empty "true"/"false" — a trailing
    # EMPTY field (reqAbsentNames is "" in the common case) would be stripped by
    # the `data=$(gh …)` command substitution (trailing-newline collapse),
    # deflating the 27-field count and tripping the fail-closed assertion.
    # reqAbsentCount (22), crReviewSkipped (23), bbState (24, ABSENT-default) and
    # bbOpen (25, numeric) are also non-empty so they are safe ahead of bbOob.
    local GATE_FILTER
    GATE_FILTER='
.data.repository.pullRequest as $pr
| ($pr.headRefOid // "") as $sha
| ([$pr.labels.nodes[]?.name]) as $labels
| ($labels | any(. == "tests-out-of-band")) as $tests
| ($labels | any(. == "perf-out-of-band")) as $perf
| ($labels | any(. == "intent-out-of-band")) as $intent
| ($labels | any(. == "cr-out-of-band")) as $cr
| ($labels | any(. == "bugbot-out-of-band")) as $bb
| ((($pr.commits.nodes[0].commit.statusCheckRollup.contexts.nodes) // [])
   | map(. + {_k: (if .__typename == "CheckRun" then ["CheckRun", (.name // "")]
                   else ["StatusContext", (.context // "")] end)})
   | group_by(._k) | map(sort_by(.startedAt // "") | .[-1]) | map(del(._k))) as $ctx
| ([$ctx[] | select(.isRequired == true)]) as $req
# $blocking — the set the gate must wait on: REQUIRED contexts PLUS the
# non-required meant-to-block allow-list (Coverage / Sanitizer /
# Perf PR-fast / Android security gate / Fuzz smoke, non-advisory). The $failing set below
# already unions these (the #923 fix), but the PENDING count historically
# counted only $req — so a non-required allow-listed check still IN_PROGRESS
# (not yet terminal) was invisible: not failing (not terminal) and not pending
# (not required) → GATES_PASSED fired before ASAN/Coverage/Bucket finished and
# the merge beat the sanitizer to the line (#1237/#1232/#1227/#1220/#1198
# ASAN/Coverage escapes). Counting $blocking (not $req) for pending closes it.
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
      # A failing check blocks if it is REQUIRED (unchanged), OR it is a
      # non-required check that is meant-to-block: its name matches the curated
      # allow-list AND is not explicitly "advisory". This closes the gate-escape
      # where the watcher auto-merged #923 past a RED non-required "Coverage"
      # check (postmortems.md 2026-06-06 "#923", option B). The allow-list is
      # deliberately tight (Coverage / Sanitizer / Perf PR-fast /
      # Android security gate / Fuzz smoke) — a non-allow-listed non-required red (e.g. the
      # `non-required-fail` test fixture, or "Duplication scanner (advisory)")
      # still passes, preserving the prior "non-required → pass" contract.
      # Extend the regex to gate more checks. "Perf PR-fast" added 2026-06-07
      # (perf-gate-revival step 6a) — armed now that ci-windows-latest baselines
      # exist; the perf-out-of-band downgrade below remains the override hatch.
      # "Android security gate" added 2026-06-09 (mobile-mvp-completion WS1,
      # Issues #1067/#1068): the advisory mobile jobs (posix-core / android-ndk /
      # apk) let a green develop ship mobile breakage (precedent #1021/#1064), so
      # the manifest-allowBackup + OpenSSL-fail-fast regression gate is routed
      # onto the blocking path here, NOT left advisory. "Fuzz smoke" added
      # 2026-06-16 (#1301 merged past a RED fuzz-smoke whose libFuzzer driver
      # FAILED TO COMPILE — a real broken develop build the poller waved through
      # because the check is non-required): paired with a continue-on-error guard
      # on the stochastic time-boxed fuzz STEP in the workflow so only the
      # deterministic build/ctest reds the check (see fuzz-smoke.yml).
      ((.isRequired == true)
       or ((if .__typename == "CheckRun" then (.name // "") else (.context // "") end)
           | (test("__BLOCK_ALLOWLIST_RE__"; "i")
              and (ascii_downcase | contains("advisory") | not)))))]) as $failing
| ([$failing[] | select(
      ($tests and .__typename == "CheckRun" and .name == "Test-delta gate") or
      ($perf  and .__typename == "CheckRun" and ((.name // "") | startswith("Perf PR-fast"))) or
      ($intent and .__typename == "CheckRun" and .name == "Intent section"))]) as $downgraded
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
    ($bb | tostring)
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

        # Parse the gh --jq field stream — 27 fixed-order lines (see GATE_FILTER
        # field map above). gh --jq errors already routed through the gh-fail
        # path above; this guards a truncated/partial body → fail closed (retry).
        local fields
        # Strip CR — Windows jq builds (and gh's bundled jq on Windows) emit
        # CRLF, which would leave a trailing \r on every field (e.g. pr_state
        # "OPEN\r" != "OPEN" → spurious return-4).
        data="${data//$'\r'/}"
        mapfile -t fields <<<"$data"
        if [ "${#fields[@]}" -ne 27 ]; then
            # Exactly 27 expected. Any other count (a field value with an embedded
            # newline would inflate it, misaligning fields[n]) → fail closed (CR #511).
            gh_fails=$((gh_fails+1))
            echo "Poll $((p+1)): gate filter returned ${#fields[@]} fields (expected 27); transient ($gh_fails/3)"
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

        printf 'Poll %d/%d — CI: %d/%d pass (%d fail, %d pending, %d warn-downgraded, %d req-missing) | CodeRabbit: %s (%d open) | Bugbot: %s (%d open) | User: %d | reviewDecision: %s\n' \
            $((p+1)) "$MAX_POLLS" $((ci_total - ci_fail - ci_pend - ci_warn_downgraded)) "$ci_total" \
            "$ci_fail" "$ci_pend" "$ci_warn_downgraded" "$req_absent" \
            "$cr_state_print" "$cr_open" "$bb_state_print" "$bb_open" "$user" "$review_decision"

        # H1: APPROVED CR review passes unconditionally per AGENTS.md § Merge
        # gates § CodeRabbit ("APPROVED → pass unconditionally (approval trumps
        # body)"). Previously the pass-check always required cr_open == 0, so
        # an APPROVED review on the current head + any unresolved non-outdated
        # CR thread (even one CR itself left for context) wedged the gate.
        # Decompose into an explicit `cr_open_blocks` so the intent is legible.
        local cr_open_blocks=false
        if [ "$cr_state" != "APPROVED" ] && [ "$cr_open" -ne 0 ]; then
            cr_open_blocks=true
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
            if [ "$cr_size_skip_block" = true ]; then
                # Tailored message for the size-skip block — names the actual
                # cause (CR skipped review, too many files) rather than the
                # generic "CR block" so the operator's log is unambiguous.
                echo "WARN: cr-out-of-band — CR skipped review (too many files) overridden" >&2
            else
                echo "WARN: cr-out-of-band label downgraded CR block (${cr_state_print}) to WARN" >&2
            fi
            cr_pass=true
            cr_open_blocks=false
            cr_overridden=true
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

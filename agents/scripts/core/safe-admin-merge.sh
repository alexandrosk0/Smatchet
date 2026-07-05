#!/usr/bin/env bash
# agents/scripts/core/safe-admin-merge.sh
# ----------------------------------------------------------------------------
# Safe admin-merge guard — the orchestrator MUST call this instead of a bare
# `gh pr merge <pr> --squash --admin`.
#
# The preventing gate for the 2026-06-13 #1180 escape (postmortems.md): the
# orchestrator admin-merged #1180 while Bucket-C/E (on the merge-gates
# `Bucket-*` block allow-list) were RED, because the pre-merge "0 failures"
# re-confirm was an informational `echo` and `gh pr merge --admin` ran as a
# separate unconditional statement — the printed "FAIL" could not stop the
# merge, and `--admin` bypassed branch protection. The legitimate carve-out
# (admin-merge a STALE-BLOCKED PR only when everything is actually green) was
# asserted in prose, not enforced by an exit code.
#
# (Historical note: `Bucket-*` was DROPPED from $MERGE_GATES_BLOCK_ALLOWLIST_RE
# on 2026-06-15 while the Mesa-GL lanes could not boot the CI exe; the
# all-gates-blocking flip re-armed it — the constant is now "." (every
# non-advisory-named check gates), so a RED bucket lane blocks this guard
# again, exactly the teeth the #1180 escape needed.)
#
# This guard makes the green assertion an EXIT CODE, not text:
#   * reads the head's statusCheckRollup (`gh pr view --json statusCheckRollup`)
#   * EXITS NON-ZERO without merging if ANY required-or-allow-listed check is
#     non-green (failing OR still pending) — only a genuinely stale-BLOCKED PR
#     whose every gating check is green is allowed through, and only THEN does
#     it run `gh pr merge --squash --admin`.
#
# Single-source-of-truth: the meant-to-block allow-list regex is NOT duplicated
# here — it is SOURCED from merge-gates.sh ($MERGE_GATES_BLOCK_ALLOWLIST_RE).
# Change the allow-list there and this guard follows automatically.
#
# A check BLOCKS the admin-merge when BOTH:
#   (1) it is gating — REQUIRED (name ∈ branch_protection.required_contexts) OR
#       in the blocking scope (name matches $MERGE_GATES_BLOCK_ALLOWLIST_RE —
#       "." post-flip, i.e. every name — AND does not contain "advisory")
#   (2) it is non-green — a CheckRun that is not COMPLETED, or whose conclusion
#       is not in {SUCCESS, NEUTRAL, SKIPPED}; or a StatusContext whose state is
#       not SUCCESS. (Pending counts as non-green: a stale-BLOCKED-green PR has
#       no pending gating checks left.)
# The only non-gating red left is an advisory-NAMED check — mirroring the
# merge-gates $failing contract exactly.
#
# CodeRabbit-completion gate (added 2026-06-17 — the #1332 "Review failed:
# Pull request was closed or merged during review" race): a CI-green PR can be
# admin-merged BEFORE CodeRabbit even starts its async review pass, so CR posts
# "Review failed (merged during review)" after the fact. The CI-blocker check
# above is blind to this — an ABSENT CR review reads as NONE, not "failing", so
# it is not a gating row. This guard therefore ALSO waits for CodeRabbit when CR
# is installed for the repo (a checked-in `.coderabbit.yaml`/`.yml`): it REFUSES
# the admin-merge until CR's review has completed on the head, mirroring the
# poll_merge_gates CR contract. The merge is allowed when ANY of:
#   * CodeRabbit is not installed for the repo (NONE is the steady state); OR
#   * the `CodeRabbit` rollup signal (StatusContext OR CheckRun) is terminal-
#     green on the head — CR finished its pass (reviewed or skipped); OR
#   * the `cr-out-of-band` label waives the wait (the explicit operator escape); OR
#   * the head commit is older than the grace window and CR never resolved
#     (SAFE_ADMIN_MERGE_CR_GRACE_MINUTES, default 20) — a stuck/disabled CR must
#     not wedge the ship-loop forever, so this degrades to a logged pass.
# Unlike poll_merge_gates (a polling loop with a poll-count grace + inline-thread
# corroboration via raw GraphQL), this one-shot guard uses the terminal `CodeRabbit`
# rollup state as the completion signal and a head-age grace as the backstop —
# `gh pr view --json statusCheckRollup` exposes no StatusContext `description`, so
# the size-skip / inline-evidence nuances stay in poll_merge_gates; `cr-out-of-band`
# is the lever for those edge cases here.
#
# `*-out-of-band` PR labels downgrade the matching check, same as the poller:
#   tests-out-of-band  → "Test-delta gate"   ·  perf-out-of-band → "Perf PR-fast*"
#   intent-out-of-band → "Intent section" (mirrors poll_merge_gates $intent downgrade)
#   cr-out-of-band     → waives the CodeRabbit-completion wait above (it still does
#                        NOT downgrade any CI check — CR is its only scope).
#
# Usage:
#   agents/scripts/core/safe-admin-merge.sh <pr>
#   agents/scripts/core/safe-admin-merge.sh --selftest
#
# Env knobs:
#   SAFE_ADMIN_MERGE_DRY_RUN     — when "true", print the merge command instead
#                                  of executing it (the gate still runs).
#   SAFE_ADMIN_MERGE_REQUIRED_CONTEXTS — override the required-context set
#                                  (newline/comma-separated). When set (even ""),
#                                  bypasses the project.config.json read. Tests
#                                  inject a fixture-matching set this way.
#   SAFE_ADMIN_MERGE_CONFIG_FILE — override project.config.json path.
#   SAFE_ADMIN_MERGE_STUB_ROLLUP — TEST-ONLY: a JSON blob used in place of the
#                                  `gh pr view` call (the full --json object:
#                                  {statusCheckRollup,state,labels,commits}). Lets
#                                  --selftest + bats feed a synthetic rollup with
#                                  zero `gh` involvement.
#   SAFE_ADMIN_MERGE_CR_INSTALLED — override the CodeRabbit-installed probe
#                                  ("true"/"false"). When set, bypasses the
#                                  checked-in `.coderabbit.yaml` file probe.
#                                  Tests set "false" to isolate the CI-blocker
#                                  cases from the CR-completion gate.
#   SAFE_ADMIN_MERGE_CR_GRACE_MINUTES — head-age grace before a still-absent CR
#                                  review degrades to a logged pass (default 20).
#                                  0 DISABLES the backstop (never auto-passes a
#                                  never-shown CR).
#   SAFE_ADMIN_MERGE_CR_GRACE_EXPIRED — TEST-ONLY: force the grace verdict
#                                  ("true"/"false"), bypassing the head-age math.
#
# Return codes:
#   0 — guard passed; admin-merge performed (or dry-run printed)
#   1 — REFUSED: a gating check is non-green, OR CodeRabbit has not completed its
#       review on the head (no merge performed)
#   2 — usage / dependency error (gh or jq missing, bad args)
#   3 — PR not in a mergeable precondition (not OPEN)
#
# selftest: asserts-failure
# ----------------------------------------------------------------------------

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Source merge-gates.sh ONLY for the single-source allow-list constant. The
# source is side-effect-free (functions + constants; its CLI entry-point is
# guarded by `[ "${BASH_SOURCE[0]}" = "${0}" ]`, false when sourced).
# shellcheck source=agents/scripts/core/merge-gates.sh
source "$SCRIPT_DIR/merge-gates.sh"

if [ -z "${MERGE_GATES_BLOCK_ALLOWLIST_RE:-}" ]; then
    echo "safe-admin-merge: merge-gates.sh did not export MERGE_GATES_BLOCK_ALLOWLIST_RE — refusing (fail-closed)." >&2
    exit 2
fi

# ----------------------------------------------------------------------------
# read_required_contexts — newline-separated list of branch-protection required
# context names. Override via SAFE_ADMIN_MERGE_REQUIRED_CONTEXTS (even ""),
# else read project.config.json with jq (UTF-8-safe, matches merge-gates.sh).
# ----------------------------------------------------------------------------
read_required_contexts() {
    if [ -n "${SAFE_ADMIN_MERGE_REQUIRED_CONTEXTS+x}" ]; then
        printf '%s\n' "${SAFE_ADMIN_MERGE_REQUIRED_CONTEXTS//,/$'\n'}"
        return 0
    fi
    local config_file="${SAFE_ADMIN_MERGE_CONFIG_FILE:-$SCRIPT_DIR/../../../project.config.json}"
    if [ -f "$config_file" ] && command -v jq >/dev/null 2>&1; then
        jq -r '.branch_protection.required_contexts[]? // empty' "$config_file" 2>/dev/null || true
    fi
}

# ----------------------------------------------------------------------------
# evaluate_rollup <rollup-json> — the PURE core, fully testable with no `gh`.
# Reads a `gh pr view --json statusCheckRollup,state,labels` object on stdin-arg
# and the required-context list on $2 (newline-separated). Emits the list of
# BLOCKING check names on stdout, one per line — a name blocks when it is a
# gating row that is non-green, OR a required context that is ABSENT from the
# rollup entirely (a required check missing from the rollup must never be read
# as green — that was a fail-open hole). The caller treats ANY emitted name as
# a blocker.
#
# Exit status reflects evaluation SUCCESS, not the blocker count: exit 0 on a
# successful evaluation (whether or not blockers were emitted); NON-ZERO only
# when the evaluation itself failed (jq parse/runtime error on a malformed
# rollup). The caller MUST fail closed — refuse the merge — on a non-zero exit.
#
# All decision logic lives in jq so a single program decides gating + greenness
# identically to the merge-gates $failing sub-expression.
# ----------------------------------------------------------------------------
evaluate_rollup() {
    local view_json="$1" req_list="$2"
    local req_json='[]'
    if [ -n "$req_list" ]; then
        req_json=$(printf '%s\n' "$req_list" | jq -R . | jq -sc 'map(select(length > 0))') || req_json='[]'
    fi

    printf '%s' "$view_json" | jq -r \
        --argjson req "$req_json" \
        --arg allow "$MERGE_GATES_BLOCK_ALLOWLIST_RE" '
        # Label-driven downgrades (mirror merge-gates $downgraded).
        ([.labels[]?.name] // []) as $labels
        | ($labels | any(. == "tests-out-of-band")) as $testsOob
        | ($labels | any(. == "perf-out-of-band")) as $perfOob
        | ($labels | any(. == "intent-out-of-band")) as $intentOob
        # Dedup-to-latest-run BEFORE judging green/red (mirrors merge-gates.sh
        # $ctx). A check name can appear MULTIPLE times in statusCheckRollup when a
        # job is re-run / superseded: an older CANCELLED / FAILURE run plus a newer
        # SUCCESS run. Judging every raw row would let the stale non-green run veto
        # a check whose LATEST run is green — a false REFUSAL (e.g. a doc-validation
        # CANCELLED by a body-edit re-trigger that then re-ran green). Group by
        # (typename, name/context) and keep only the latest run per group — latest =
        # max completedAt, then max startedAt as a tie-break (a still-running rerun
        # of a finished job has no completedAt; startedAt still orders it newest).
        # StatusContexts have neither timestamp, but GitHub already overwrites a
        # StatusContext in place by context, so each appears once and the group is a
        # singleton (the sort is a harmless no-op).
        #
        # The missing-timestamp default is a FAR-FUTURE sentinel ("9999-..."), NOT
        # "": a null completedAt means the run is still IN PROGRESS (a re-trigger of
        # a finished job), and a still-running re-trigger is genuinely the NEWEST
        # run — so it must sort LAST (win as the latest), not first. With a ""
        # default the in-progress row sorted FIRST lexicographically and a completed
        # older SUCCESS could be wrongly picked as "latest" over the live re-run.
        # The same far-future sentinel is used for both completedAt and startedAt so
        # the tie-break stays consistent (an in-progress run with neither timestamp
        # still sorts newest).
        | (((.statusCheckRollup) // [])
            | map(. + {_k: (if .__typename == "CheckRun" then ["CheckRun", (.name // "")]
                            else ["StatusContext", (.context // "")] end)})
            | group_by(._k)
            | map(sort_by((.completedAt // "9999-12-31T23:59:59Z"), (.startedAt // "9999-12-31T23:59:59Z")) | .[-1] | del(._k))) as $latest
        # Resolve each deduped rollup row to a (name, green?) pair; bind as $rows so
        # the absent-required cross-check below can see which names are present.
        | ($latest
            | map(
                (if .__typename == "CheckRun" then (.name // "")
                 else (.context // "") end) as $name
                | (if .__typename == "CheckRun"
                   then (.status == "COMPLETED"
                         and ((.conclusion // "") | ascii_upcase | IN("SUCCESS","NEUTRAL","SKIPPED")))
                   else ((.state // "") | ascii_upcase | . == "SUCCESS") end) as $green
                | {name: $name, green: $green}
              )) as $rows
        | ([$rows[].name]) as $present
        # Blockers among rows that EXIST: gating (required OR allow-listed-non-
        # advisory) and non-green, after out-of-band downgrades. Bind the row to
        # $r so $req-iteration inside `any` does not shadow `.name`.
        | ([ $rows[]
             | . as $r
             | ($r + {gating:
                 (($req | any(. == $r.name))
                  or (($r.name | test($allow; "i"))
                      and (($r.name | ascii_downcase | contains("advisory")) | not)))}) as $g
             | ($g + {gating:
                 ($g.gating
                  and (($testsOob and $g.name == "Test-delta gate") | not)
                  and (($perfOob and ($g.name | startswith("Perf PR-fast"))) | not)
                  and (($intentOob and $g.name == "Intent section") | not))})
             | select(.gating and (.green | not))
             | .name ]) as $rowBlockers
        # Absent-required cross-check (fail-closed): a required context missing
        # from the rollup is a blocker, NOT a silent green — same out-of-band
        # downgrades apply.
        | ([ $req[]
             | select(. as $rn | ($present | any(. == $rn)) | not)
             | select(($testsOob and . == "Test-delta gate") | not)
             | select(($perfOob and (. | startswith("Perf PR-fast"))) | not)
             | select(($intentOob and . == "Intent section") | not) ]) as $absentReq
        | ($rowBlockers + $absentReq)
        | unique
        | .[]
    '
}

# ----------------------------------------------------------------------------
# detect_cr_installed — "true" if CodeRabbit is configured for this repo, else
# "false". SAFE_ADMIN_MERGE_CR_INSTALLED overrides the probe (tests). The probe
# is a checked-in `.coderabbit.yaml`/`.yml` at the repo root: unlike the poller's
# `gh api contents` probe (the poller may run detached), this guard always runs
# inside the repo working tree, so a local file test is sufficient and offline.
# ----------------------------------------------------------------------------
detect_cr_installed() {
    if [ -n "${SAFE_ADMIN_MERGE_CR_INSTALLED+x}" ]; then
        printf '%s' "$SAFE_ADMIN_MERGE_CR_INSTALLED"
        return 0
    fi
    local root="$SCRIPT_DIR/../../.."
    if [ -f "$root/.coderabbit.yaml" ] || [ -f "$root/.coderabbit.yml" ]; then
        printf 'true'
    else
        printf 'false'
    fi
}

# ----------------------------------------------------------------------------
# detect_cr_grace_expired <view_json> — "true" when the head commit is older
# than the grace window (so a still-absent/stuck CR degrades to a logged pass),
# else "false". SAFE_ADMIN_MERGE_CR_GRACE_EXPIRED overrides the math (tests).
# Head staleness is read from the commit whose oid == .headRefOid (NOT merely the
# newest committedDate, which a truncated `--json commits` page can mis-report).
# SAFE_ADMIN_MERGE_CR_GRACE_MINUTES=0 DISABLES the backstop entirely (returns
# "false" always) — a 0-minute window is "no grace", not "instant grace".
# Fail-safe direction = "false" (cannot prove staleness → keep blocking) on any
# missing date / unparseable timestamp / `date` failure: the backstop only fires
# on a CONFIRMED-stale head, never on an unknown one.
# ----------------------------------------------------------------------------
detect_cr_grace_expired() {
    if [ -n "${SAFE_ADMIN_MERGE_CR_GRACE_EXPIRED+x}" ]; then
        printf '%s' "$SAFE_ADMIN_MERGE_CR_GRACE_EXPIRED"
        return 0
    fi
    local view_json="$1"
    local grace_min="${SAFE_ADMIN_MERGE_CR_GRACE_MINUTES:-20}"
    [[ "$grace_min" =~ ^[0-9]+$ ]] || grace_min=20
    # grace_min == 0 DISABLES the backstop. A 0-minute window would mark every
    # past head instantly stale and let evaluate_cr PASS on a never-shown CR —
    # re-opening the #1332 merge-beats-review race. Treat 0 as "no backstop;
    # wait for a real CR verdict (or the cr-out-of-band label)".
    if [ "$grace_min" -eq 0 ]; then printf 'false'; return 0; fi
    local committed
    # Prefer the commit whose oid == headRefOid (the TRUE PR head): `gh pr view
    # --json commits` is capped (~250), so sort-by-committedDate can omit the
    # head on a large PR and mis-time the grace. Fall back to the newest
    # committedDate only when headRefOid is absent or not in the returned page.
    committed=$(printf '%s' "$view_json" \
        | jq -r '(.headRefOid // "") as $head
                 | (.commits // []) as $c
                 | (($c | map(select(.oid == $head)) | (.[0].committedDate // empty))
                    // ($c | sort_by(.committedDate) | last | .committedDate)
                    // empty)' 2>/dev/null) || committed=""
    if [ -z "$committed" ]; then printf 'false'; return 0; fi
    local now head_epoch
    now=$(date -u +%s 2>/dev/null) || { printf 'false'; return 0; }
    # GNU date parses ISO-8601 with -d; BSD date (macOS) needs -j -f. Try GNU
    # first, fall back to BSD, then fail closed (cannot prove staleness → block).
    head_epoch=$(date -u -d "$committed" +%s 2>/dev/null) \
        || head_epoch=$(date -u -j -f "%Y-%m-%dT%H:%M:%SZ" "$committed" +%s 2>/dev/null) \
        || { printf 'false'; return 0; }
    if [ $(( (now - head_epoch) / 60 )) -ge "$grace_min" ]; then
        printf 'true'
    else
        printf 'false'
    fi
}

# ----------------------------------------------------------------------------
# evaluate_cr <view_json> <cr_installed:true|false> <grace_expired:true|false>
# — the PURE CodeRabbit-completion verdict, fully testable with no `gh`/`date`.
# Emits ONE line: a "PASS <reason>" or "BLOCK <reason>" token. The caller reads
# the leading token: PASS → continue to merge; BLOCK → refuse (exit 1). Exit 0
# on a successful eval; NON-ZERO only on a jq parse/runtime error (caller fails
# closed). First-match-wins ordering mirrors poll_merge_gates' CR arms:
#   cr-out-of-band override  >  not-installed  >  CR terminal-green  >
#   CR present-but-not-green (BLOCK, ignores grace)  >  grace-expired backstop
#   (CR absent + stale head → PASS)  >  CR absent (BLOCK).
# The grace backstop deliberately sits AFTER the present-but-not-green arm: an
# in-progress CR review must never be raced by the head-age backstop (that is
# exactly the #1332 merge-beats-review hole); grace only rescues an ABSENT CR.
# ----------------------------------------------------------------------------
evaluate_cr() {
    local view_json="$1" cr_installed="$2" grace_expired="$3"
    printf '%s' "$view_json" | jq -r \
        --arg installed "$cr_installed" \
        --arg grace "$grace_expired" '
        ([.labels[]?.name] // []) as $labels
        | ($labels | any(. == "cr-out-of-band")) as $crOob
        # CodeRabbit rollup rows — StatusContext "CodeRabbit" OR CheckRun
        # "CodeRabbit" / "CR findings (N actionable)" (both shapes CR emits).
        | (((.statusCheckRollup) // [])
            | map(select(
                (if .__typename == "CheckRun" then (.name // "") else (.context // "") end)
                | test("CodeRabbit|CR findings"; "i")))) as $cr
        | ($cr | length > 0) as $present
        # Terminal-green = CR finished its pass (reviewed or skipped): a CheckRun
        # COMPLETED in {SUCCESS,NEUTRAL,SKIPPED}, or a StatusContext state SUCCESS.
        | ([ $cr[] | select(
                (.__typename == "CheckRun"
                   and .status == "COMPLETED"
                   and ((.conclusion // "") | ascii_upcase | IN("SUCCESS","NEUTRAL","SKIPPED")))
                or (.__typename == "StatusContext"
                   and (((.state // "") | ascii_upcase) == "SUCCESS"))) ]
            | length > 0) as $green
        | if $crOob then "PASS cr-out-of-band label waives the CodeRabbit wait"
          elif ($installed != "true") then "PASS CodeRabbit not installed for this repo"
          elif ($present and $green) then "PASS CodeRabbit review complete on head"
          elif $present then "BLOCK CodeRabbit review still in progress on head (context present, not yet green)"
          elif ($grace == "true") then "PASS CodeRabbit grace expired (head stale, CR never showed — backstop pass)"
          else "BLOCK CodeRabbit has not reviewed this head yet"
          end
    '
}

run_selftest() {
    local fails=0

    # CASE 1 — REFUSES on RED checks under block-on-any-red (all-gates-blocking
    # flip): BOTH `Coverage` (required-era allow-list) AND `Bucket-C` (never on
    # the curated list; the 2026-06-15 advisory era pinned it non-blocking) must
    # now block — every non-advisory-named red gates the admin-merge. The
    # advisory-NAME escape is covered by CASE 2's `Duplication scanner (advisory)`.
    local red_rollup
    red_rollup='{"state":"OPEN","labels":[],"statusCheckRollup":[
      {"__typename":"CheckRun","name":"Coverage","status":"COMPLETED","conclusion":"FAILURE"},
      {"__typename":"CheckRun","name":"Bucket-C (ImGui Test Engine)","status":"COMPLETED","conclusion":"FAILURE"},
      {"__typename":"StatusContext","context":"Windows + MSVC","state":"SUCCESS"}]}'
    local blockers
    blockers=$(evaluate_rollup "$red_rollup" "Windows + MSVC")
    if [ -n "$blockers" ] && printf '%s' "$blockers" | grep -q 'Coverage' \
       && printf '%s' "$blockers" | grep -q 'Bucket-C'; then
        echo "selftest CASE1 PASS — refuses on RED Coverage AND RED Bucket-C (block-on-any-red)"
    else
        echo "selftest CASE1 FAIL — Coverage AND Bucket-C must both block (got: '$blockers')" >&2
        fails=$((fails + 1))
    fi

    # CASE 2 — ALLOWS a stale-BLOCKED PR whose every gating check is green.
    # All required + allow-listed checks SUCCESS; an advisory red is ignored.
    local green_rollup
    green_rollup='{"state":"OPEN","labels":[],"statusCheckRollup":[
      {"__typename":"CheckRun","name":"Bucket-C (ImGui Test Engine)","status":"COMPLETED","conclusion":"SUCCESS"},
      {"__typename":"CheckRun","name":"Coverage","status":"COMPLETED","conclusion":"SUCCESS"},
      {"__typename":"StatusContext","context":"Windows + MSVC","state":"SUCCESS"},
      {"__typename":"StatusContext","context":"Test-delta gate","state":"SUCCESS"},
      {"__typename":"CheckRun","name":"Duplication scanner (advisory)","status":"COMPLETED","conclusion":"FAILURE"}]}'
    blockers=$(evaluate_rollup "$green_rollup" $'Windows + MSVC\nTest-delta gate')
    if [ -z "$blockers" ]; then
        echo "selftest CASE2 PASS — allows stale-BLOCKED-all-green (advisory red ignored)"
    else
        echo "selftest CASE2 FAIL — should be clean (got blockers: '$blockers')" >&2
        fails=$((fails + 1))
    fi

    # CASE 3 — a PENDING allow-listed check counts as non-green (blocks).
    local pending_rollup
    pending_rollup='{"state":"OPEN","labels":[],"statusCheckRollup":[
      {"__typename":"CheckRun","name":"Sanitizer","status":"IN_PROGRESS","conclusion":null}]}'
    blockers=$(evaluate_rollup "$pending_rollup" "")
    if printf '%s' "$blockers" | grep -q 'Sanitizer'; then
        echo "selftest CASE3 PASS — pending Sanitizer blocks"
    else
        echo "selftest CASE3 FAIL — pending Sanitizer should block (got: '$blockers')" >&2
        fails=$((fails + 1))
    fi

    # CASE 4 — out-of-band label downgrades a RED gated check to non-blocking.
    local oob_rollup
    oob_rollup='{"state":"OPEN","labels":[{"name":"perf-out-of-band"}],"statusCheckRollup":[
      {"__typename":"CheckRun","name":"Perf PR-fast (windows-2022)","status":"COMPLETED","conclusion":"FAILURE"}]}'
    blockers=$(evaluate_rollup "$oob_rollup" "")
    if [ -z "$blockers" ]; then
        echo "selftest CASE4 PASS — perf-out-of-band downgrades RED Perf PR-fast"
    else
        echo "selftest CASE4 FAIL — perf-out-of-band check should not block (got: '$blockers')" >&2
        fails=$((fails + 1))
    fi

    # CASE 4b — intent-out-of-band downgrades a RED allow-listed "Intent section".
    local intent_oob_rollup
    intent_oob_rollup='{"state":"OPEN","labels":[{"name":"intent-out-of-band"}],"statusCheckRollup":[
      {"__typename":"CheckRun","name":"Intent section","status":"COMPLETED","conclusion":"FAILURE"}]}'
    blockers=$(evaluate_rollup "$intent_oob_rollup" "")
    if [ -z "$blockers" ]; then
        echo "selftest CASE4b PASS — intent-out-of-band downgrades RED Intent section"
    else
        echo "selftest CASE4b FAIL — intent-out-of-band check should not block (got: '$blockers')" >&2
        fails=$((fails + 1))
    fi

    # CASE 5 — a REQUIRED context ABSENT from the rollup blocks (fail-closed).
    # "Windows + MSVC" is required but no row reports it -> must not read green.
    local absent_rollup
    absent_rollup='{"state":"OPEN","labels":[],"statusCheckRollup":[
      {"__typename":"StatusContext","context":"Linux + Clang","state":"SUCCESS"}]}'
    blockers=$(evaluate_rollup "$absent_rollup" "Windows + MSVC")
    if printf '%s' "$blockers" | grep -q 'Windows + MSVC'; then
        echo "selftest CASE5 PASS — absent required context blocks (no silent green)"
    else
        echo "selftest CASE5 FAIL — absent required 'Windows + MSVC' should block (got: '$blockers')" >&2
        fails=$((fails + 1))
    fi

    # CASE 6 — a malformed rollup makes evaluate_rollup EXIT NON-ZERO so the
    # caller fails closed (jq parse error must never read as zero-blockers-green).
    if evaluate_rollup 'this is not json' "" >/dev/null 2>&1; then
        echo "selftest CASE6 FAIL — malformed rollup should exit non-zero" >&2
        fails=$((fails + 1))
    else
        echo "selftest CASE6 PASS — malformed rollup exits non-zero (caller fails closed)"
    fi

    # ---- CodeRabbit-completion gate (evaluate_cr, pure) --------------------
    local cr_green_rollup cr_absent_rollup cr_pending_rollup verdict
    cr_green_rollup='{"state":"OPEN","labels":[],"statusCheckRollup":[
      {"__typename":"StatusContext","context":"CodeRabbit","state":"SUCCESS"}]}'
    cr_absent_rollup='{"state":"OPEN","labels":[],"statusCheckRollup":[
      {"__typename":"StatusContext","context":"Windows + MSVC","state":"SUCCESS"}]}'
    cr_pending_rollup='{"state":"OPEN","labels":[],"statusCheckRollup":[
      {"__typename":"StatusContext","context":"CodeRabbit","state":"PENDING"}]}'

    # CASE 7 — CR installed but absent on the head → BLOCK (the #1332 race).
    verdict=$(evaluate_cr "$cr_absent_rollup" "true" "false")
    if [[ "$verdict" == BLOCK* ]] && [[ "$verdict" == *"has not reviewed"* ]]; then
        echo "selftest CASE7 PASS — absent CR review blocks (#1332 race closed)"
    else
        echo "selftest CASE7 FAIL — absent CR should BLOCK (got: '$verdict')" >&2
        fails=$((fails + 1))
    fi

    # CASE 8 — CR StatusContext terminal-SUCCESS → PASS (review complete).
    verdict=$(evaluate_cr "$cr_green_rollup" "true" "false")
    if [[ "$verdict" == PASS* ]] && [[ "$verdict" == *"review complete"* ]]; then
        echo "selftest CASE8 PASS — terminal-green CR passes"
    else
        echo "selftest CASE8 FAIL — green CR should PASS (got: '$verdict')" >&2
        fails=$((fails + 1))
    fi

    # CASE 9 — CR present but not yet green (still reviewing) → BLOCK.
    verdict=$(evaluate_cr "$cr_pending_rollup" "true" "false")
    if [[ "$verdict" == BLOCK* ]] && [[ "$verdict" == *"in progress"* ]]; then
        echo "selftest CASE9 PASS — in-progress CR blocks"
    else
        echo "selftest CASE9 FAIL — pending CR should BLOCK (got: '$verdict')" >&2
        fails=$((fails + 1))
    fi

    # CASE 10 — cr-out-of-band label waives the wait even with CR absent → PASS.
    local cr_oob_rollup
    cr_oob_rollup='{"state":"OPEN","labels":[{"name":"cr-out-of-band"}],"statusCheckRollup":[
      {"__typename":"StatusContext","context":"Windows + MSVC","state":"SUCCESS"}]}'
    verdict=$(evaluate_cr "$cr_oob_rollup" "true" "false")
    if [[ "$verdict" == PASS* ]] && [[ "$verdict" == *"cr-out-of-band"* ]]; then
        echo "selftest CASE10 PASS — cr-out-of-band waives the CR wait"
    else
        echo "selftest CASE10 FAIL — cr-out-of-band should PASS (got: '$verdict')" >&2
        fails=$((fails + 1))
    fi

    # CASE 11 — CR not installed for the repo → PASS (NONE is steady state).
    verdict=$(evaluate_cr "$cr_absent_rollup" "false" "false")
    if [[ "$verdict" == PASS* ]] && [[ "$verdict" == *"not installed"* ]]; then
        echo "selftest CASE11 PASS — CR-not-installed passes"
    else
        echo "selftest CASE11 FAIL — not-installed should PASS (got: '$verdict')" >&2
        fails=$((fails + 1))
    fi

    # CASE 12 — CR absent but head stale past grace → PASS (stuck-CR backstop).
    verdict=$(evaluate_cr "$cr_absent_rollup" "true" "true")
    if [[ "$verdict" == PASS* ]] && [[ "$verdict" == *"grace expired"* ]]; then
        echo "selftest CASE12 PASS — grace-expired backstop passes"
    else
        echo "selftest CASE12 FAIL — grace-expired should PASS (got: '$verdict')" >&2
        fails=$((fails + 1))
    fi

    # CASE 13 — CR present-but-not-green with grace EXPIRED → still BLOCK.
    # Regression guard for the #1358 High finding: the head-age backstop must NOT
    # override an in-progress CR review (else admin-merge races CodeRabbit — the
    # exact #1332 hole this gate closes). Grace rescues only an ABSENT CR.
    verdict=$(evaluate_cr "$cr_pending_rollup" "true" "true")
    if [[ "$verdict" == BLOCK* ]] && [[ "$verdict" == *"in progress"* ]]; then
        echo "selftest CASE13 PASS — in-progress CR blocks even past grace"
    else
        echo "selftest CASE13 FAIL — pending CR past grace should BLOCK (got: '$verdict')" >&2
        fails=$((fails + 1))
    fi

    # CASE 14 — SAFE_ADMIN_MERGE_CR_GRACE_MINUTES=0 DISABLES the backstop:
    # detect_cr_grace_expired must read "false" even on an ancient head, so a
    # 0-minute window cannot auto-pass a never-shown CR (#1358 zero-grace finding).
    # These two cases exercise the real head-age MATH, so they must neutralize an
    # ambient SAFE_ADMIN_MERGE_CR_GRACE_EXPIRED override (the bats harness sets it)
    # that would otherwise short-circuit detect_cr_grace_expired before the math.
    local stale_head_json grace_out
    stale_head_json='{"headRefOid":"abc","commits":[{"oid":"abc","committedDate":"2020-01-01T00:00:00Z"}]}'
    grace_out=$(unset SAFE_ADMIN_MERGE_CR_GRACE_EXPIRED; SAFE_ADMIN_MERGE_CR_GRACE_MINUTES=0 detect_cr_grace_expired "$stale_head_json")
    if [ "$grace_out" = "false" ]; then
        echo "selftest CASE14 PASS — grace_min=0 disables the stale-head backstop"
    else
        echo "selftest CASE14 FAIL — grace_min=0 should disable backstop (got: '$grace_out')" >&2
        fails=$((fails + 1))
    fi

    # CASE 15 — staleness follows headRefOid, NOT the newest committedDate, so a
    # truncated `--json commits` page can't mis-time the grace (#1358 truncation
    # finding). headRefOid points at an ancient commit while a fresh "now" commit
    # also appears: with the default 20-min window the head must read STALE.
    local head_oid_json now_iso
    now_iso=$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null)
    head_oid_json='{"headRefOid":"old","commits":[
      {"oid":"new","committedDate":"'"$now_iso"'"},
      {"oid":"old","committedDate":"2020-01-01T00:00:00Z"}]}'
    grace_out=$(unset SAFE_ADMIN_MERGE_CR_GRACE_EXPIRED SAFE_ADMIN_MERGE_CR_GRACE_MINUTES; detect_cr_grace_expired "$head_oid_json")
    if [ "$grace_out" = "true" ]; then
        echo "selftest CASE15 PASS — staleness uses headRefOid, not newest commit"
    else
        echo "selftest CASE15 FAIL — head oid (ancient) should read stale (got: '$grace_out')" >&2
        fails=$((fails + 1))
    fi

    # CASE 16 — dedup-to-latest: a check with an OLDER CANCELLED run plus a NEWER
    # SUCCESS run is judged GREEN (the latest run wins), NOT refused on the stale
    # CANCELLED row. Regression guard for the false-refusal where a doc-validation
    # run cancelled by a body-edit re-trigger vetoed a check whose re-run was green.
    local dedup_rollup
    dedup_rollup='{"state":"OPEN","labels":[],"statusCheckRollup":[
      {"__typename":"CheckRun","name":"Coverage","status":"COMPLETED","conclusion":"CANCELLED","startedAt":"2026-06-20T10:00:00Z","completedAt":"2026-06-20T10:05:00Z"},
      {"__typename":"CheckRun","name":"Coverage","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-20T11:00:00Z","completedAt":"2026-06-20T11:05:00Z"},
      {"__typename":"StatusContext","context":"Windows + MSVC","state":"SUCCESS"}]}'
    blockers=$(evaluate_rollup "$dedup_rollup" "Windows + MSVC")
    if [ -z "$blockers" ]; then
        echo "selftest CASE16 PASS — older CANCELLED run deduped; latest SUCCESS reads GREEN"
    else
        echo "selftest CASE16 FAIL — latest SUCCESS Coverage should not block (got: '$blockers')" >&2
        fails=$((fails + 1))
    fi

    # CASE 16b — dedup direction is correct: an OLDER SUCCESS plus a NEWER FAILURE
    # must still BLOCK (the latest run is red — dedup must not flip a real failure
    # green by picking the wrong run).
    local dedup_red_rollup
    dedup_red_rollup='{"state":"OPEN","labels":[],"statusCheckRollup":[
      {"__typename":"CheckRun","name":"Coverage","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-20T10:00:00Z","completedAt":"2026-06-20T10:05:00Z"},
      {"__typename":"CheckRun","name":"Coverage","status":"COMPLETED","conclusion":"FAILURE","startedAt":"2026-06-20T11:00:00Z","completedAt":"2026-06-20T11:05:00Z"},
      {"__typename":"StatusContext","context":"Windows + MSVC","state":"SUCCESS"}]}'
    blockers=$(evaluate_rollup "$dedup_red_rollup" "Windows + MSVC")
    if printf '%s' "$blockers" | grep -q 'Coverage'; then
        echo "selftest CASE16b PASS — latest FAILURE still blocks (dedup keeps the newest run)"
    else
        echo "selftest CASE16b FAIL — latest FAILURE Coverage should block (got: '$blockers')" >&2
        fails=$((fails + 1))
    fi

    # CASE 16c — null-completedAt-in-progress + older-SUCCESS dedup direction:
    # an OLDER COMPLETED/SUCCESS run plus a NEWER still-running re-trigger (null
    # completedAt, IN_PROGRESS) must keep the LIVE re-run as the latest — so the
    # check reads non-green (pending) and BLOCKS. Regression guard for the CR
    # #1537 finding: with a "" fallback the null-completedAt in-progress row sorted
    # FIRST lexicographically, the older SUCCESS was wrongly picked as "latest",
    # and an admin-merge raced a re-running gate. The far-future sentinel sorts the
    # in-progress run LAST (newest), correctly preserving the block.
    local inprog_rollup
    inprog_rollup='{"state":"OPEN","labels":[],"statusCheckRollup":[
      {"__typename":"CheckRun","name":"Coverage","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-20T10:00:00Z","completedAt":"2026-06-20T10:05:00Z"},
      {"__typename":"CheckRun","name":"Coverage","status":"IN_PROGRESS","conclusion":null,"startedAt":"2026-06-20T11:00:00Z","completedAt":null},
      {"__typename":"StatusContext","context":"Windows + MSVC","state":"SUCCESS"}]}'
    blockers=$(evaluate_rollup "$inprog_rollup" "Windows + MSVC")
    if printf '%s' "$blockers" | grep -q 'Coverage'; then
        echo "selftest CASE16c PASS — null-completedAt in-progress re-run sorts newest; pending Coverage blocks"
    else
        echo "selftest CASE16c FAIL — live re-running Coverage should block over older SUCCESS (got: '$blockers')" >&2
        fails=$((fails + 1))
    fi

    if [ "$fails" -eq 0 ]; then
        echo "PASS — safe-admin-merge --selftest (18/18)"
        return 0
    fi
    echo "FAIL — safe-admin-merge --selftest ($fails failing case(s))" >&2
    return 1
}

main() {
    local arg="${1:-}"
    case "$arg" in
        --selftest) run_selftest; exit $? ;;
        ""|-h|--help)
            sed -n '2,60p' "${BASH_SOURCE[0]}"
            [ -z "$arg" ] && exit 2 || exit 0 ;;
    esac

    local pr="$arg"
    if ! [[ "$pr" =~ ^[0-9]+$ ]]; then
        echo "safe-admin-merge: <pr> must be a PR number (got: '$pr')" >&2
        exit 2
    fi

    command -v jq >/dev/null 2>&1 || { echo "safe-admin-merge: jq required" >&2; exit 2; }

    local view_json
    if [ -n "${SAFE_ADMIN_MERGE_STUB_ROLLUP:-}" ]; then
        view_json="$SAFE_ADMIN_MERGE_STUB_ROLLUP"
    else
        command -v gh >/dev/null 2>&1 || { echo "safe-admin-merge: gh required" >&2; exit 2; }
        if ! view_json=$(gh pr view "$pr" --json statusCheckRollup,state,labels,commits,headRefOid 2>&1); then
            echo "safe-admin-merge: gh pr view failed: $view_json" >&2
            exit 2
        fi
    fi

    # Precondition — PR must be OPEN.
    local state
    state=$(printf '%s' "$view_json" | jq -r '.state // "UNKNOWN"' 2>/dev/null) || state="UNKNOWN"
    if [ "$state" != "OPEN" ]; then
        echo "safe-admin-merge: PR #$pr is not OPEN (state=$state) — refusing." >&2
        exit 3
    fi

    local req_list blockers eval_rc
    req_list=$(read_required_contexts)
    blockers=$(evaluate_rollup "$view_json" "$req_list")
    eval_rc=$?
    if [ "$eval_rc" -ne 0 ]; then
        echo "safe-admin-merge: rollup evaluation failed (jq rc=$eval_rc, likely malformed statusCheckRollup) — refusing (fail-closed)." >&2
        exit 2
    fi

    if [ -n "$blockers" ]; then
        echo "REFUSED — PR #$pr has non-green gating check(s); NOT admin-merging:" >&2
        printf '  RED/PENDING: %s\n' "$blockers" >&2
        echo "A bare 'gh pr merge --admin' would have bypassed branch protection and shipped past these. Fix or use the named *-out-of-band override label." >&2
        exit 1
    fi

    # CodeRabbit-completion gate — CI is green, but a CI-green PR can still be
    # admin-merged before CR finishes its async review (the #1332 race). Refuse
    # until CR has completed on the head (or is overridden/not-installed/stale).
    local cr_installed cr_grace cr_verdict cr_rc cr_decision
    cr_installed=$(detect_cr_installed)
    cr_grace=$(detect_cr_grace_expired "$view_json")
    cr_verdict=$(evaluate_cr "$view_json" "$cr_installed" "$cr_grace")
    cr_rc=$?
    if [ "$cr_rc" -ne 0 ] || [ -z "$cr_verdict" ]; then
        echo "safe-admin-merge: CodeRabbit-gate evaluation failed (jq rc=$cr_rc) — refusing (fail-closed)." >&2
        exit 2
    fi
    cr_decision=${cr_verdict%% *}
    case "$cr_decision" in
        PASS)
            echo "CR-GATE PASS — ${cr_verdict#PASS }" ;;
        BLOCK)
            echo "REFUSED — PR #$pr: ${cr_verdict#BLOCK }; NOT admin-merging." >&2
            echo "CodeRabbit's review has not completed on this head — merging now would race it (the #1332 'Review failed: merged during review'). Wait for CR to finish, or apply the 'cr-out-of-band' label to merge without it." >&2
            exit 1 ;;
        *)
            echo "safe-admin-merge: unexpected CR verdict '$cr_verdict' — refusing (fail-closed)." >&2
            exit 2 ;;
    esac

    echo "GREEN — PR #$pr: every required-or-allow-listed check is green (genuine stale-BLOCKED carve-out)."
    if [ "${SAFE_ADMIN_MERGE_DRY_RUN:-}" = "true" ]; then
        echo "DRY-RUN: would run: gh pr merge $pr --squash --admin"
        exit 0
    fi
    exec gh pr merge "$pr" --squash --admin
}

if [ "${BASH_SOURCE[0]}" = "${0}" ]; then
    main "$@"
fi

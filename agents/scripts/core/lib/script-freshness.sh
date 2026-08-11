#!/usr/bin/env bash
# script-freshness.sh — shared gate-logic staleness detector (sourced, not run).
# ----------------------------------------------------------------------------
# WHY THIS EXISTS (gate-tooling-run-from-stale-session-branch, process P1)
#   Gate scripts live IN the repo, so the copy that runs is whatever the calling
#   checkout happens to hold. `claude/<id>/*` session branches are long-lived by
#   design and nothing pulls `agents/scripts/**` forward on them, so the longer a
#   session lives the further the gate logic it executes drifts from the logic
#   actually guarding `develop`. Nothing in a gate's output distinguishes a
#   stale-script verdict from a real one: every line is a plausible, correctly
#   formatted BLOCK (or PASS).
#
#   Observed: `merge-gates.sh` run from a session branch ~7 weeks behind
#   hard-blocked a PR on a CodeRabbit rule that current `develop` auto-exempts.
#   The phantom block was taken as evidence about `develop` and written up as a
#   product-gate defect before a fresh-worktree re-run disproved it.
#
#   The FALSE-RED direction above is the visible one. The dangerous direction is
#   the opposite: a stale `pre-ship.sh` produces false **GREENS** — it passes a
#   diff that current `develop` would reject, and nobody looks twice at a green.
#   That is why this helper is shared rather than living inside the merge poller.
#
# CONTRACT — script_freshness_verdict
#   Usage:
#       script_freshness_verdict <run_override> <dev_override> <relpath>...
#
#   <run_override>/<dev_override> are TEST SEAMS: when <run_override> is
#   non-empty the git layer is bypassed entirely and the two values are compared
#   as the fingerprints. Pass "" "" for the real git path. Callers map their own
#   documented env var onto these so each keeps its own seam name.
#
#   <relpath>... are repo-relative paths making up the caller's gate-logic set.
#   Pass EVERY file whose content changes the verdict — an entry point alone is
#   not enough once load-bearing logic lives in sourced modules (a stale module
#   beside a current entry point is exactly as wrong, and invisible).
#
#   Sets three globals and returns 0 (it never fails the caller — the caller owns
#   the policy decision):
#       SCRIPT_FRESHNESS_VERDICT   fresh | stale | unverifiable
#       SCRIPT_FRESHNESS_RUN_BLOB  combined on-disk fingerprint ("" if unknown)
#       SCRIPT_FRESHNESS_DEV_BLOB  combined origin/develop fingerprint ("" if unknown)
#
#   `unverifiable` is a DISTINCT verdict from `stale`, deliberately: "the gate
#   logic is out of date" and "I could not find out" call for different operator
#   responses, and collapsing them either cries wolf offline or hides a real
#   drift. Fail-closed is the CALLER's choice to make from `unverifiable`, not
#   this helper's (the merge poller refuses a pass on it; an advisory nudge
#   degrades to silence).
#
#   This helper prints NOTHING. Message prose stays with each caller so a gate
#   can name its own remedy and cite its own history; only the subtle part — the
#   bounded fetch, the combined fingerprint, and the fail-closed blanking — is
#   shared. That split is deliberate: prose duplication is cheap and readable,
#   whereas a second hand-rolled fetch is where the hangs and the silent
#   stale-compare bugs come from.
#
# This file is sourced; it defines functions and does NOT `set -e`
# (the sourcing script owns shell options).
# ----------------------------------------------------------------------------

# Bounded, non-interactive refresh of origin/develop. Refs only — never touches
# the worktree. Returns non-zero on any failure so the caller can blank the
# comparison rather than silently measuring against a stale local ref.
#
# This MUST NOT be able to hang. It runs on every invocation of every gate that
# uses it, including interactive ones, and in the merge poller it runs before the
# poll budget is even initialised — so a stall here is bounded by nothing. Three
# independent stops, because no single one is portable across git-bash and Linux CI:
#   - GIT_TERMINAL_PROMPT=0 + ssh BatchMode — never block on a credential or
#     host-key prompt (the classic silent hang on a fresh checkout).
#   - http.lowSpeedLimit/Time — abort a transfer stalled under 1KB/s for 10s.
#     Pure git config, works everywhere, needs no external binary.
#   - `timeout 30` where coreutils provides it — a hard wall-clock cap over both.
script_freshness_fetch() {  # script_freshness_fetch <repo_root> → 0 ok / 1 failed
    local _root="$1"
    local -a _cmd=(git -C "$_root"
        -c "http.lowSpeedLimit=1000" -c "http.lowSpeedTime=10"
        fetch -q --no-tags origin develop)
    if command -v timeout >/dev/null 2>&1; then
        GIT_TERMINAL_PROMPT=0 GIT_SSH_COMMAND="${GIT_SSH_COMMAND:-ssh -oBatchMode=yes}" \
            timeout 30 "${_cmd[@]}" >/dev/null 2>&1 || return 1
    else
        GIT_TERMINAL_PROMPT=0 GIT_SSH_COMMAND="${GIT_SSH_COMMAND:-ssh -oBatchMode=yes}" \
            "${_cmd[@]}" >/dev/null 2>&1 || return 1
    fi
    return 0
}

script_freshness_verdict() {  # <run_override> <dev_override> <relpath>...
    local _run_override="${1:-}" _dev_override="${2:-}"
    shift 2 || true

    SCRIPT_FRESHNESS_VERDICT="unverifiable"
    SCRIPT_FRESHNESS_RUN_BLOB=""
    SCRIPT_FRESHNESS_DEV_BLOB=""

    if [ "$#" -eq 0 ]; then
        return 0   # nothing declared → nothing knowable
    fi

    if [ -n "$_run_override" ]; then
        SCRIPT_FRESHNESS_RUN_BLOB="$_run_override"
        SCRIPT_FRESHNESS_DEV_BLOB="$_dev_override"
    else
        # EVERY command substitution here carries an explicit `|| var=""`. The
        # sourcing script owns shell options and several callers run under `set -e`,
        # where a bare `var="$(cmd)"` assignment PROPAGATES cmd's exit status and
        # kills the caller outright. (merge-gates.sh's inline original was accidentally
        # immune: it used `local var="$(cmd)"`, and `local`'s own success masks the
        # substitution's failure — a mask this extraction loses, which is precisely
        # how it first took down pre-ship.sh's selftest on a repo whose files were
        # absent.) A missing file must degrade to `unverifiable`, never to a dead shell.
        local _root
        _root="$(git -C "${SCRIPT_FRESHNESS_ROOT_HINT:-.}" rev-parse --show-toplevel 2>/dev/null)" || _root=""
        if [ -n "$_root" ]; then
            local _fetch_ok=true
            script_freshness_fetch "$_root" || _fetch_ok=false
            # Combined fingerprint over the whole declared set. A missing local
            # file or a missing develop blob leaves an empty component; any such
            # gap blanks BOTH sides so the result is `unverifiable` rather than a
            # comparison of partial fingerprints that could coincidentally match.
            local _rp _rh _dh _incomplete=false
            for _rp in "$@"; do
                _rh="$(git -C "$_root" hash-object "$_root/$_rp" 2>/dev/null)" || _rh=""
                _dh="$(git -C "$_root" rev-parse -q --verify "origin/develop:$_rp" 2>/dev/null)" || _dh=""
                if [ -z "$_rh" ] || [ -z "$_dh" ]; then _incomplete=true; fi
                SCRIPT_FRESHNESS_RUN_BLOB="$SCRIPT_FRESHNESS_RUN_BLOB $_rh"
                SCRIPT_FRESHNESS_DEV_BLOB="$SCRIPT_FRESHNESS_DEV_BLOB $_dh"
            done
            # A failed fetch is treated exactly like a missing blob: without it the
            # local origin/develop may be arbitrarily old, so a "matches" result
            # would be meaningless and a "differs" result unattributable.
            if [ "$_incomplete" = true ] || [ "$_fetch_ok" != true ]; then
                SCRIPT_FRESHNESS_RUN_BLOB=""
                SCRIPT_FRESHNESS_DEV_BLOB=""
            fi
        fi
    fi

    if [ -z "$SCRIPT_FRESHNESS_RUN_BLOB" ] || [ -z "$SCRIPT_FRESHNESS_DEV_BLOB" ]; then
        SCRIPT_FRESHNESS_VERDICT="unverifiable"
    elif [ "$SCRIPT_FRESHNESS_RUN_BLOB" != "$SCRIPT_FRESHNESS_DEV_BLOB" ]; then
        SCRIPT_FRESHNESS_VERDICT="stale"
    else
        SCRIPT_FRESHNESS_VERDICT="fresh"
    fi
    return 0
}

# Advisory one-liner for gates that only ever WARN (pre-ship.sh and friends).
# Prints to stderr and always returns 0 — a freshness note must never be the
# reason a lint gate fails, only the reason its GREEN is not taken on faith.
# Silent when fresh, and silent when unverifiable UNLESS the caller opts in via
# SCRIPT_FRESHNESS_VERBOSE=1 (offline is the common local case; nagging about it
# on every run trains people to ignore the line that matters).
warn_if_script_stale() {  # <label> <relpath>...
    local _label="$1"; shift
    script_freshness_verdict "" "" "$@"
    case "$SCRIPT_FRESHNESS_VERDICT" in
        stale)
            echo "WARN: ${_label} differs from origin/develop — this checkout is running OUT-OF-DATE gate logic, so a PASS here may not reflect what CI enforces. Re-run from a worktree based on origin/develop before trusting this result (docs/agent-rules/process-rules.md § Run gate tooling from a tree freshly based on origin/develop)." >&2
            ;;
        unverifiable)
            # `if`, not `[ … ] && echo`: as the branch's last command a false test
            # would make the case (and under `set -e` the caller) see a non-zero
            # status. Same class of trap as the assignments above.
            if [ "${SCRIPT_FRESHNESS_VERBOSE:-0}" = "1" ]; then
                echo "WARN: ${_label} freshness unverifiable (no git checkout / no origin/develop blob / fetch failed) — cannot confirm this checkout runs current gate logic." >&2
            fi
            ;;
    esac
    return 0
}

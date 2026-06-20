#!/usr/bin/env bash
# plan-history-guard.sh — shared shallow-clone guard for history-dependent
# plan-index generators (sourced, not run).
# ----------------------------------------------------------------------------
# WHY THIS EXISTS
#   test-plan-index.sh derives each shipped-plan row's "Approx. date" from
#   `git log --follow --format=%ad --date=short` — the FIRST-commit date of the
#   plan file. CI's required `test-plan-index` check and the advisory
#   `autosync-plan-index` job BOTH check out with `fetch-depth: 0` (full
#   history), so their dates are authoritative.
#
#   On a SHALLOW clone (`git rev-parse --is-shallow-repository == true`),
#   `git log --follow` walks only the truncated history: the first-commit date
#   is wrong (or empty → the "—" placeholder), so a local `--fix` produces an
#   INDEX that differs from what CI regenerates. The author commits it, then the
#   required check reds the PR on a date-column diff they can't reproduce — the
#   shallow-clone-corrupts-git-log-date-generators failure.
#
# CONTRACT — is_shallow_or_refuse <mode>
#   <mode> is the caller's run mode: "fix" | "check" (any non-"fix" treated as
#   read-only / validate).
#
#   Non-shallow repo  -> return 0 (no-op; the common case).
#   Shallow + fix     -> AUTO-UNSHALLOW, then return 0; if the unshallow fetch
#                        fails (offline / no remote), HARD-REFUSE: exit 2 with a
#                        remedy. Rationale below.
#   Shallow + check   -> emit a WARN to stderr and return 0 (a read-only
#                        validate must not mutate the repo; the required CI
#                        check runs on full history and stays authoritative).
#
# AUTO-UNSHALLOW vs HARD-REFUSE — the choice (deliverable 1):
#   We AUTO-`git fetch --unshallow` under --fix because (a) it is exactly what
#   the high-integrity delta gate already does on shallow CI checkouts
#   (.github/workflows/build-and-test.yml — "if is-shallow … fetch --unshallow"),
#   (b) it is cheap + safe for this repo, and (c) it is the ONLY way a local
#   --fix can be byte-identical to CI (whose dates come from full history). If
#   the fetch itself fails we do NOT silently fall through to a corrupt index —
#   we hard-refuse (exit 2) so the drift is impossible rather than committed.
# ----------------------------------------------------------------------------

# Emit on stderr; never to stdout (stdout carries the "Passed: N Failed: M"
# machine line some callers parse).
_phg_warn() { printf '%s\n' "$*" >&2; }

# is_shallow_or_refuse <mode>
# Returns 0 on success (non-shallow, or shallow successfully unshallowed, or
# shallow+check WARN). Calls `exit 2` directly on a shallow --fix that cannot
# be unshallowed (a hard refusal — the caller wants the whole run aborted).
is_shallow_or_refuse() {
    local mode="${1:-check}"

    # `git rev-parse --is-shallow-repository` prints "true"/"false"; on very old
    # gits the flag is unknown and the command errors — treat any non-"true" as
    # non-shallow (fail-open: a guard for an old git must not block work).
    local shallow
    shallow="$(git rev-parse --is-shallow-repository 2>/dev/null || echo false)"
    if [ "$shallow" != "true" ]; then
        return 0
    fi

    if [ "$mode" = "fix" ]; then
        _phg_warn "plan-history-guard: shallow clone detected — git-log dates would drift from CI."
        _phg_warn "plan-history-guard: auto-unshallowing (git fetch --unshallow) so --fix matches CI…"
        if git fetch --no-tags --unshallow origin >/dev/null 2>&1; then
            _phg_warn "plan-history-guard: unshallow OK — full history restored."
            return 0
        fi
        # Could not unshallow (offline / no 'origin' / already-being-fetched).
        # HARD-REFUSE rather than emit a date-drifted index CI will reject.
        _phg_warn "plan-history-guard: REFUSING --fix on a shallow clone (could not unshallow)."
        _phg_warn "  git-log --follow dates would differ from CI (which runs on full history),"
        _phg_warn "  producing an INDEX the required test-plan-index check rejects on a diff you"
        _phg_warn "  cannot reproduce locally."
        _phg_warn "  Remedy: git fetch --unshallow   (or re-clone without --depth), then re-run."
        exit 2
    fi

    # check / validate mode: read-only — never mutate the repo. WARN only.
    _phg_warn "plan-history-guard: WARN — shallow clone; git-log dates may differ from CI"
    _phg_warn "  (CI runs on full history). If --check drifts, run: git fetch --unshallow"
    return 0
}

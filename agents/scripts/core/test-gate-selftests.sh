#!/usr/bin/env bash
# test-gate-selftests.sh — the gate that gates the gates (Gap C / close-gate-gaps Slice 3).
# ----------------------------------------------------------------------------
# Class of bug this kills (#918/#663/#784/#978): a gate whose `--selftest` only
# ever exercises the PASS path, so a silently-broken gate ships "green." A
# universal bad-input meta-gate is impossible (each gate's bad input is
# semantic), so this checks SHAPE, not semantics: every first-party script that
# EXPOSES a `--selftest` flag must ALSO carry a grep-able
#     # selftest: asserts-failure
# marker (a behaviour-inert comment) proving its selftest exercises at least one
# genuine failure case. The `: n/a — <reason>` form is accepted for the rare
# pure dispatcher with no failure mode to assert. A future gate physically
# cannot ship a pass-only selftest without tripping this.
#
# SCOPE: agents/scripts/{core,project}/ + scripts/dev/, first-party only.
#   "Exposes --selftest" = a real flag handler (a `--selftest)` case arm, a
#   `= "--selftest"` test, or an argparse/argv handler) — NOT a mere mention in
#   a comment or a manifest string (so test-docs.sh's STEPS manifest and
#   test-session-registry-bats.sh's doc comment are correctly NOT required to
#   carry the marker).
#
# Fail-CLOSED: a missing tool / unreadable file / no scripts found is an infra
# error (exit 2), never a silent pass.
#
# Modes:
#   (no args) | --check   scan the real tree; FAIL (1) listing every pass-only selftest.
#   --selftest            dogfood: synth a marker-less script -> assert FAIL; add
#                         the marker -> assert PASS. Asserts a failure case.
#
# Exit: 0 every exposer carries the marker · 1 a pass-only selftest · 2 infra error.
# Goes through test-shell-lint.sh (5 rules) + the shellcheck -S warning fail-set.
# ----------------------------------------------------------------------------
set -uo pipefail

# Repo-root-anchored (this script lives in agents/scripts/core/).
ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"

# Directories scanned for selftest-exposing scripts (relative to a root).
SCAN_DIRS=(agents/scripts/core agents/scripts/project scripts/dev)

# A file EXPOSES --selftest (vs merely mentioning it) when it has a real flag
# handler. Keep in sync with the comment above + tests/bats/gate_selftests.bats.
EXPOSE_RE='(--selftest\)|"--selftest"|== "--selftest"|= "--selftest"|add_argument\("--selftest"|argv\[0\] == "--selftest")'

# The required behaviour-inert marker (accepts the `: n/a — <reason>` form too).
MARKER_RE='# selftest: asserts-failure'

# exposers <root> — print, one per line, every first-party script under <root>'s
# SCAN_DIRS that EXPOSES a --selftest flag. Fail-closed: unreadable dirs are
# simply empty, but a total-zero result is treated as infra error by the caller.
exposers() {
    local root="$1" d f
    for d in "${SCAN_DIRS[@]}"; do
        [ -d "$root/$d" ] || continue
        for f in "$root/$d"/*; do
            [ -f "$f" ] || continue
            case "$f" in *.sh|*.py|*.bash) : ;; *) continue ;; esac
            [ -r "$f" ] || { echo "test-gate-selftests: unreadable file: $f" >&2; return 2; }
            grep -qE -- "$EXPOSE_RE" "$f" && printf '%s\n' "$f"
        done
    done
    return 0   # never leak the final grep's no-match (1) as the function's exit
}

# run_check <root> — assert every exposer carries the marker. Exit 0/1/2.
run_check() {
    local root="$1" list rc=0 f missing=()
    if ! command -v grep >/dev/null 2>&1; then
        echo "test-gate-selftests: grep not on PATH (required)" >&2; return 2
    fi
    list="$(exposers "$root")" || return 2
    if [ -z "$list" ]; then
        echo "test-gate-selftests: FAIL — zero --selftest-exposing scripts found under" >&2
        printf '    %s\n' "${SCAN_DIRS[@]}" >&2
        echo "    (expected many; a zero result means the scan is broken — fail-closed)." >&2
        return 2
    fi
    while IFS= read -r f; do
        [ -n "$f" ] || continue
        if ! grep -qF -- "$MARKER_RE" "$f"; then
            missing+=("$f")
        fi
    done <<EOF
$list
EOF
    if [ "${#missing[@]}" -ne 0 ]; then
        echo "test-gate-selftests: FAIL — these scripts expose --selftest but their selftest" >&2
        echo "  asserts no FAILURE case (no '# selftest: asserts-failure' marker):" >&2
        for f in "${missing[@]}"; do echo "    ${f#"$root"/}" >&2; done
        echo "  Backfill a real negative assertion (feed known-bad input -> expect non-zero/" >&2
        echo "  failure token) and mark it, or use '# selftest: asserts-failure: n/a — <reason>'" >&2
        echo "  for a genuine pure dispatcher. See close-gate-gaps Slice 3." >&2
        rc=1
    else
        local n; n="$(printf '%s\n' "$list" | grep -c .)"
        echo "test-gate-selftests: PASS — all $n --selftest-exposing scripts assert a failure case."
    fi
    return "$rc"
}

# run_selftest — dogfood against a temp root. A marker-less exposer must FAIL;
# adding the marker must PASS. Asserts a failure case (the marker-less branch).
run_selftest() {
    local tmp rc=0
    tmp="$(mktemp -d)" || { echo "test-gate-selftests selftest: mktemp failed" >&2; return 2; }
    trap 'rm -rf "$tmp"' RETURN
    mkdir -p "$tmp/agents/scripts/core"
    local synth="$tmp/agents/scripts/core/fake-gate.sh"
    # A script that EXPOSES --selftest but carries NO marker.
    cat > "$synth" <<'SH'
#!/usr/bin/env bash
case "${1:-}" in
    --selftest) echo ok; exit 0 ;;
esac
SH

    # selftest: asserts-failure — a marker-less exposer must be detected (the gate's whole job).
    if run_check "$tmp" >/dev/null 2>&1; then
        echo "test-gate-selftests selftest: FAIL — marker-less exposer was NOT flagged" >&2
        rc=1
    fi

    # Add the marker -> must now PASS.
    printf '# selftest: asserts-failure\n' >> "$synth"
    if ! run_check "$tmp" >/dev/null 2>&1; then
        echo "test-gate-selftests selftest: FAIL — marked exposer was wrongly flagged" >&2
        rc=1
    fi

    # n/a form must also satisfy the rule.
    cat > "$synth" <<'SH'
#!/usr/bin/env bash
# selftest: asserts-failure: n/a — pure dispatcher, no failure mode to assert.
case "${1:-}" in
    --selftest) echo ok; exit 0 ;;
esac
SH
    if ! run_check "$tmp" >/dev/null 2>&1; then
        echo "test-gate-selftests selftest: FAIL — n/a marker form was wrongly flagged" >&2
        rc=1
    fi

    if [ "$rc" -eq 0 ]; then echo "test-gate-selftests --selftest: PASS"; fi
    return "$rc"
}

case "${1:---check}" in
    --check)    run_check "$ROOT" ;;
    --selftest) run_selftest ;;
    -h|--help)  echo "usage: test-gate-selftests.sh [--check|--selftest]" ;;
    *)          echo "usage: test-gate-selftests.sh [--check|--selftest]" >&2; exit 2 ;;
esac

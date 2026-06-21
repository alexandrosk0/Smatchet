#!/usr/bin/env bash
# test-bats-ascii-names.sh — every bats @test TITLE must be pure ASCII.
# ----------------------------------------------------------------------------
# Class of bug this kills (green-wash): the Windows `bats` host silently SKIPs
# an @test whose TITLE contains a non-ASCII byte (it prints `bats: unknown test
# name`, counts the whole batch as one failure, and never runs those cases).
# CI's Linux bats parses the same titles fine, so the suite RUNS in CI but
# silently UNDER-RUNS locally — masking real local failures behind a count that
# looks green. ~120 existing @test titles used `→` / `—` and were affected.
#
# Only the @test TITLE is constrained. Arrows / em-dashes inside comments, test
# bodies, or `Status:`-style prose are fine — the grep is scoped to the
#     @test "<title>"
# line only (the title between the FIRST pair of double-quotes on an @test line).
#
# SCOPE: every *.bats under tests/bats/ AND agents/scripts/** AND scripts/dev/**.
#
# Blocking absolute-0 (no grandfathering): the existing offenders were swept to
# ASCII in the same change that introduced this gate, so HEAD is clean. A new
# unicode-titled @test is an exact, unambiguous signal — not WARN-worthy.
#
# Modes:
#   (no args) | --check   scan the real tree; FAIL (1) listing every offending
#                         <file>:<line>: @test "<title>".
#   --selftest            dogfood: synth a .bats with a unicode title -> assert
#                         FAIL; an ASCII-only one -> assert PASS. Asserts a
#                         failure case.
#
# Exit: 0 clean · 1 non-ASCII @test title(s) found · 2 infra error.
# Emits the canonical `Passed: N  Failed: M` line so test-all.sh aggregates it.
# Goes through test-shell-lint.sh + the shellcheck warning fail-set.
# ----------------------------------------------------------------------------
set -uo pipefail

# selftest: asserts-failure — the --selftest mode feeds a known-bad input (a
# .bats whose @test title carries U+2192) and asserts the checker flags it.

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"

# Roots scanned for *.bats (each included only if present).
SCAN_ROOTS=(tests/bats agents/scripts scripts/dev)

# Print every offending "<file>:<line>:<@test line>" under <root>; return 1 if
# any found, 0 if clean. A title is offending iff the @test line — restricted to
# the title between its first pair of double quotes — carries a non-ASCII byte.
#
# Implementation note: grep -P with [^\x00-\x7F] is the byte-level non-ASCII
# test. We first isolate @test lines, then re-match the TITLE only so a non-ASCII
# byte AFTER the closing quote (a trailing comment on the @test line) does not
# false-positive — only the quoted title is constrained.
_scan_root() {
    local root="$1" base="$2" rc=0 f rest lineno content after title
    [ -d "$root/$base" ] || return 0
    # Collect candidate @test lines that contain ANY non-ASCII byte, with
    # file:line prefix, across all *.bats under this base.
    # grep --null-data-style isn't portable; instead we use grep's
    # `--null` (-Z) separator between FILENAME and the rest so a Windows
    # drive-letter colon in the path can't be mistaken for the file:line
    # delimiter. Format per match: <path>\0<lineno>:<content>.
    while IFS= read -r -d '' f && IFS= read -r rest; do
        [ -n "$rest" ] || continue
        # rest = <lineno>:<content>
        lineno="${rest%%:*}"
        content="${rest#*:}"
        # Extract the title: substring between the first '"' and the next '"'.
        after="${content#*\"}"
        title="${after%%\"*}"
        # Offending iff the TITLE has a non-ASCII byte.
        if printf '%s' "$title" | LC_ALL=C grep -qP '[^\x00-\x7F]'; then
            echo "  - $f:$lineno: non-ASCII @test title -> $content" >&2
            rc=1
        fi
    done < <(LC_ALL=C grep -rnZP '^[[:space:]]*@test[[:space:]]+".*[^\x00-\x7F].*"' \
                 "$root/$base" --include='*.bats' 2>/dev/null)
    return "$rc"
}

run_check() {
    local root="$1" rc=0 base
    if ! command -v grep >/dev/null 2>&1; then
        echo "test-bats-ascii-names: grep not on PATH (required)" >&2
        echo "Passed: 0  Failed: 0  (skipped — grep missing)"
        return 2
    fi
    local found=0
    for base in "${SCAN_ROOTS[@]}"; do
        if ! _scan_root "$root" "$base"; then
            found=1
        fi
    done
    if [ "$found" -ne 0 ]; then
        echo "test-bats-ascii-names: FAIL — @test title(s) above contain non-ASCII bytes." >&2
        echo "  The Windows bats host silently SKIPs these (green-wash). Replace e.g." >&2
        echo "  arrows '->' for U+2192, ' - ' for U+2014 (em dash) in the TITLE only." >&2
        echo "Passed: 0  Failed: 1"
        rc=1
    else
        local n
        n="$(for base in "${SCAN_ROOTS[@]}"; do
                 [ -d "$root/$base" ] && find "$root/$base" -name '*.bats'
             done | wc -l | tr -d ' ')"
        echo "test-bats-ascii-names: PASS — all @test titles across $n bats suite(s) are ASCII."
        echo "Passed: 1  Failed: 0"
    fi
    return "$rc"
}

run_selftest() {
    local tmp rc=0
    tmp="$(mktemp -d)" || { echo "test-bats-ascii-names selftest: mktemp failed" >&2; return 2; }
    trap 'rm -rf "$tmp"' RETURN
    mkdir -p "$tmp/tests/bats"

    # A .bats whose @test TITLE carries U+2192 (a real offender) — must FAIL.
    # U+2192 is emitted via printf octal so this gate file itself stays ASCII.
    local arrow; arrow="$(printf '\xe2\x86\x92')"
    cat > "$tmp/tests/bats/synth_bad.bats" <<EOF
#!/usr/bin/env bats
@test "all gates pass ${arrow} return 0" {
  true
}
EOF
    if run_check "$tmp" >/dev/null 2>&1; then
        echo "test-bats-ascii-names selftest: FAIL — a unicode @test title was NOT flagged" >&2
        rc=1
    fi

    # A control: arrow only in a COMMENT and inside the BODY — title is ASCII.
    # Must NOT be flagged (only the title is constrained).
    cat > "$tmp/tests/bats/synth_good.bats" <<EOF
#!/usr/bin/env bats
# this comment may use ${arrow} freely
@test "all gates pass -> return 0" {
  echo "body may use ${arrow} too"
  true
}
EOF
    rm -f "$tmp/tests/bats/synth_bad.bats"
    if ! run_check "$tmp" >/dev/null 2>&1; then
        echo "test-bats-ascii-names selftest: FAIL — an ASCII-title suite was wrongly flagged" >&2
        echo "  (arrow in a comment or body must not count)" >&2
        rc=1
    fi

    if [ "$rc" -eq 0 ]; then
        echo "test-bats-ascii-names --selftest: PASS — unicode title flagged; comment/body arrow ignored."
    fi
    return "$rc"
}

case "${1:---check}" in
    --check)    run_check "$ROOT" ;;
    --selftest) run_selftest ;;
    -h|--help)  echo "usage: test-bats-ascii-names.sh [--check|--selftest]" ;;
    *)          echo "usage: test-bats-ascii-names.sh [--check|--selftest]" >&2; exit 2 ;;
esac

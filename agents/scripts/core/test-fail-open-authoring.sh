#!/usr/bin/env bash
# test-fail-open-authoring.sh — authoring-lint for the fail-OPEN gate-shape cluster (PR-5;
# sibling of test-gate-selftests.sh, which checks that a selftest exercises a FAILURE case).
# ----------------------------------------------------------------------------
# Class of bug this kills: a gate / test-driver authored in a shape that returns
# GREEN on a NON-result, so a silently-broken gate ships "passing." The historical
# fail-open cluster (tooling.md → fail-open-gate-cluster-*; postmortems on the
# build-log-regex single-separator escape, the non-recursive scan, the stale
# committed artefact) groups four recurring authoring shapes:
#
#   B — zero-match grep that PASSES. A `grep -q PATTERN file` (or a `$(grep ...)`
#       captured as a "clean" signal) whose NO-MATCH path is treated as success,
#       with no fail-closed `|| { echo ...; exit N; }` / `|| return N`. A grep that
#       matches zero lines exits 1, which an unguarded `&&`/`if` silently turns into
#       "nothing wrong" — the exact false-PASS class (a broken PATTERN matches
#       nothing and the gate goes green).
#   C — non-recursive scan. A `for f in <dir>/*` / `ls <dir>` / `glob('<dir>/*.py')`
#       over a directory KNOWN to nest (the gate's scan root) that never recurses
#       (`**`, `os.walk`, `find`, `git ls-files`, `-r`/`-R`), so a site in a subdir
#       is missed and the gate passes blind (the #119 tooltip-wrapwidth escape).
#   D — stale committed artefact as the gate INPUT. A gate that reads a committed
#       file (`cat docs/.../baseline...`, a checked-in `*.lock` / `*-snapshot.*`) as
#       its source of truth instead of LIVE-scanning the tree — the artefact drifts
#       and the gate validates yesterday's state. (The catalog/baseline files are
#       informational snapshots, NOT gate inputs — the live `--diff` is the gate.)
#   E — single-toolchain literal. A hardcoded compiler / toolchain path
#       (`/usr/bin/gcc`, `C:\\msys64\\...`, a bare `clang++ ` invocation with an
#       absolute path) that pins the gate to ONE toolchain, so the gate no-ops (or
#       fails open) on the other half of the dual MSVC+Clang matrix.
#
# This is a STATIC authoring-lint (a text proxy, not execution): it greps the gate
# scripts for each shape. A deliberate, reviewed instance escapes with an inline
#     # fail-open-ok: <reason>
# marker on the offending line (or the line above). The synthetic-driver --selftest
# plants one bad snippet per shape (B/C/D/E) and asserts each is flagged, then adds
# the escape marker and asserts it clears — proving the lint is live in both
# directions (asserts-failure).
#
# SCOPE: agents/scripts/{core,project}/ + scripts/dev/, first-party gate scripts
#   (*.sh / *.py / *.bash). ThirdParty / build excluded by living outside the dirs.
#
# Fail-CLOSED: a missing tool / unreadable file / zero scripts found is an infra
# error (exit 2), never a silent pass.
#
# WARN-FIRST / calibration (mirrors the `duplication` + `unused-symbol-under-config-guard`
# precedent): the real-tree `--check` is ADVISORY — it prints every detected shape to stderr
# but ALWAYS exits 0, because the static text proxy cannot distinguish a genuinely-dangerous
# fail-open shape from a benign idiom (a `grep -q X && continue` filter, a flat-dir `for f in
# dir/*`) without execution. The shapes graduate to a blocking `--check` once the FP rate is
# calibrated low. The authoritative, BLOCKING half is the `--selftest` synthetic driver, which
# proves the detection logic fires on a planted bad snippet of each shape and clears on the
# escape marker (asserts-failure) — that is the load-bearing regression guard.
#
# Modes:
#   (no args) | --check   scan the real tree; WARN (advisory) listing each shape; ALWAYS exit 0.
#   --check-strict        same scan but BLOCKING (exit 1 on any un-escaped shape) — for a future
#                         calibrated graduation / a curated subtree; not wired into CI yet.
#   --selftest            dogfood: synth one bad snippet per shape -> assert FLAGGED; add the
#                         escape marker -> assert CLEARED. Asserts a failure case (BLOCKING).
#
# Exit: 0 advisory --check (always) / clean --check-strict · 1 --check-strict shape / selftest
#       regression · 2 infra error.
#
# selftest: asserts-failure — --selftest plants a bad snippet per shape B/C/D/E and
# asserts each is flagged before the escape marker clears it.
# ----------------------------------------------------------------------------
set -uo pipefail

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"

SCAN_DIRS=(agents/scripts/core agents/scripts/project scripts/dev)

# This file is itself a gate script under a SCAN_DIR, and it necessarily MENTIONS
# every fail-open shape it hunts for (the doc-comment + the synthetic snippets). It
# must not flag itself. Self-exclude by basename.
SELF_BN="test-fail-open-authoring.sh"

# The inline escape marker for a deliberate, reviewed instance.
ESCAPE_RE='# fail-open-ok:'

# scan_one_file <file> — emit `<shape>\t<file>:<line>\t<rawline>` per fail-open shape,
# minus any line carrying the escape marker (on the line itself or the line above).
scan_one_file() {
    local f="$1"
    [ -r "$f" ] || { echo "test-fail-open-authoring: unreadable file: $f" >&2; return 2; }
    local lineno=0 prev=""
    while IFS= read -r line || [ -n "$line" ]; do
        lineno=$((lineno + 1))
        local raw="$line" cur="$line"
        # Escape: marker on this line OR the immediately preceding line.
        local escaped=0
        case "$cur" in *"$ESCAPE_RE"*) escaped=1 ;; esac
        case "$prev" in *"$ESCAPE_RE"*) escaped=1 ;; esac
        prev="$cur"
        # Skip pure-comment lines (the doc-comment prose must not self-trip).
        local s="${cur#"${cur%%[![:space:]]*}"}"
        case "$s" in '#'*) continue ;; esac
        [ "$escaped" -eq 1 ] && continue

        # --- Shape B: zero-match grep whose NO-MATCH path is a CLEAN/PASS signal. ---
        # The dangerous form is narrow: `grep ... && <clean-signal>` with NO `||` fail-closed
        # handler, where the consequence is an explicit pass token (echo PASS / exit 0 / return 0
        # / "clean"/"ok"). A `grep -q X && continue` (filter) or `&& grep -q Y` (assertion
        # conjunction) is NOT this shape and must not fire. Requiring the clean-signal token keeps
        # the FP rate near zero. (`||` anywhere on the line means a no-match path exists -> ok.)
        case "$cur" in
            *'||'*) : ;;                                                  # has a no-match handler — ok
            *'grep '*'&&'*)
                case "$cur" in
                    *'&&'*'continue'*|*'&&'*'grep '*|*'&&'*'break'*|*'&&'*'return 1'*|*'&& {'*) : ;;
                    *'&&'*'exit 0'*|*'&&'*'return 0'*|*'&&'*'echo PASS'*|*'&&'*'echo clean'*|*'&&'*'echo ok'*|*'&&'*'echo OK'*)
                        printf 'B-zero-match-grep\t%s:%s\t%s\n' "$f" "$lineno" "$raw" ;;
                esac ;;
        esac

        # --- Shape C: non-recursive python glob over a known-nesting tree. ---
        # The #119 tooltip-wrapwidth escape was a python `glob.glob('<dir>/*.<ext>')` (no `**`,
        # no os.walk) that missed a site in a subdir. We flag ONLY the python single-`*` glob
        # form (shell `for f in dir/*` is too commonly a deliberate flat-dir scan to flag without
        # a flood). A `glob('.../**/...', recursive=True)` or an os.walk on the same line is ok.
        case "$cur" in
            *'os.walk'*|*'**'*|*'recursive=True'*) : ;;                   # recursive — ok
            *'glob.glob('*"'"*'/*.'*"'"*')'*|*'glob('*"'"*'/*.'*"'"*')'*)
                printf 'C-nonrecursive-scan\t%s:%s\t%s\n' "$f" "$lineno" "$raw" ;;
        esac

        # --- Shape D: stale committed snapshot/baseline read as the gate's EXPECTED input. ---
        # Narrow: a `cat <committed-baseline/snapshot>` captured into a want/expected/golden
        # variable as the source of truth (the live `--diff` scan is the real gate; the snapshot
        # is informational only). `open(args.baseline)` (a CLI-passed comparison input) and a
        # snapshot WRITE (--refresh / --baseline-md / > $BASELINE) are NOT this shape.
        case "$cur" in
            *'--refresh'*|*'--baseline-md'*|*'> "$BASELINE'*|*'>"$BASELINE'*) : ;;   # WRITE — ok
            *'want='*'cat '*baseline*|*'expected='*'cat '*baseline*|*'golden='*'cat '*baseline*)
                printf 'D-stale-artefact-input\t%s:%s\t%s\n' "$f" "$lineno" "$raw" ;;
            *'want='*'cat '*'-snapshot'*|*'expected='*'cat '*'-snapshot'*)
                printf 'D-stale-artefact-input\t%s:%s\t%s\n' "$f" "$lineno" "$raw" ;;
        esac

        # --- Shape E: single-toolchain literal. ---
        # A hardcoded absolute compiler/toolchain path pins the gate to one toolchain
        # (breaks the dual MSVC+Clang matrix). Flag absolute *nix gcc/clang paths and
        # absolute msys64/LLVM bin paths used as an invocation.
        case "$cur" in
            *'/usr/bin/gcc'*|*'/usr/bin/g++'*|*'/usr/bin/clang'*)
                printf 'E-single-toolchain\t%s:%s\t%s\n' "$f" "$lineno" "$raw" ;;
            *'C:\\msys64\\'*'\\gcc'*|*'/c/msys64/'*'/gcc'*|*'/c/msys64/'*'/clang'*)
                printf 'E-single-toolchain\t%s:%s\t%s\n' "$f" "$lineno" "$raw" ;;
        esac
    done < "$f"
    return 0
}

# list_gate_scripts <root> — first-party gate scripts under SCAN_DIRS (excl. self).
list_gate_scripts() {
    local root="$1" d f
    for d in "${SCAN_DIRS[@]}"; do
        [ -d "$root/$d" ] || continue
        for f in "$root/$d"/*; do
            [ -f "$f" ] || continue
            case "$f" in *.sh|*.py|*.bash) : ;; *) continue ;; esac
            case "$f" in *"/$SELF_BN") continue ;; esac
            printf '%s\n' "$f"
        done
    done
}

# run_check <root> <strict:0|1> — scan; with strict=0 (advisory/WARN-first) findings go to stderr
# and the function ALWAYS returns 0; with strict=1 (blocking) a finding returns 1.
run_check() {
    local root="$1" strict="${2:-0}" list f rc=0 hits=""
    if ! command -v grep >/dev/null 2>&1; then
        echo "test-fail-open-authoring: grep not on PATH (required)" >&2; return 2
    fi
    list="$(list_gate_scripts "$root")"
    if [ -z "$list" ]; then
        echo "test-fail-open-authoring: FAIL — zero gate scripts found under" >&2
        printf '    %s\n' "${SCAN_DIRS[@]}" >&2
        echo "    (expected many; a zero result means the scan is broken — fail-closed)." >&2
        return 2
    fi
    local one
    while IFS= read -r f; do
        [ -n "$f" ] || continue
        one="$(scan_one_file "$f")" || return 2
        [ -n "$one" ] && hits="$hits$one"$'\n'
    done <<EOF
$list
EOF
    hits="$(printf '%s' "$hits" | grep -E . || true)"
    if [ -n "$hits" ]; then
        local tag="WARN (advisory; calibration; not blocking)"
        [ "$strict" = "1" ] && tag="FAIL"
        echo "test-fail-open-authoring: $tag — fail-open authoring shape(s) detected:" >&2
        printf '%s\n' "$hits" | sed "s@^@  @; s@\t@ @g; s@$root/@@" >&2
        echo "  B zero-match grep passing · C non-recursive scan · D stale committed artefact as input ·" >&2
        echo "  E single-toolchain literal. Fix the shape, or mark a reviewed instance with" >&2
        echo "  '# fail-open-ok: <reason>' on the line (or the line above). See PR-5." >&2
        [ "$strict" = "1" ] && rc=1
    else
        local n; n="$(printf '%s\n' "$list" | grep -c .)"
        echo "test-fail-open-authoring: PASS — $n gate scripts carry no un-escaped fail-open shape."
    fi
    return "$rc"
}

# run_selftest — synthetic driver: one planted bad snippet per shape B/C/D/E must be
# flagged; the same snippet with the escape marker must clear. Asserts a failure case.
run_selftest() {
    local tmp rc=0
    tmp="$(mktemp -d)" || { echo "test-fail-open-authoring selftest: mktemp failed" >&2; return 2; }
    trap 'rm -rf "$tmp"' RETURN

    # Shape B — zero-match grep whose no-match path is a CLEAN signal, no `||` handler.
    local fb="$tmp/fixture-b.sh"
    printf '%s\n' 'grep -q "PATTERN" "$f" && echo clean' > "$fb"
    if [ -z "$(scan_one_file "$fb")" ]; then
        echo "selftest: FAIL — shape B (zero-match grep -> clean signal) not flagged" >&2; rc=1; fi
    # Escape marker clears it.
    local fb2="$tmp/fixture-b2.sh"
    printf '%s\n' 'grep -q "PATTERN" "$f" && echo clean # fail-open-ok: literal-existence probe' > "$fb2"
    if [ -n "$(scan_one_file "$fb2")" ]; then
        echo "selftest: FAIL — shape B escape marker did not clear the finding" >&2; rc=1; fi
    # A benign `grep -q X && continue` filter must NOT be flagged.
    local fb3="$tmp/fixture-b3.sh"
    printf '%s\n' 'grep -q "PATTERN" "$f" && continue' > "$fb3"
    if [ -n "$(scan_one_file "$fb3")" ]; then
        echo "selftest: FAIL — shape B false-positive on a 'grep -q X && continue' filter" >&2; rc=1; fi
    # A grep WITH a `||` no-match handler must NOT be flagged.
    local fb4="$tmp/fixture-b4.sh"
    printf '%s\n' 'grep -q "PATTERN" "$f" && echo clean || { echo bad; exit 1; }' > "$fb4"
    if [ -n "$(scan_one_file "$fb4")" ]; then
        echo "selftest: FAIL — shape B false-positive on a grep with a || fail-closed handler" >&2; rc=1; fi

    # Shape C — non-recursive python glob over a nesting tree (the #119 escape shape).
    local fc="$tmp/fixture-c.py"
    printf '%s\n' "for p in glob.glob('Source/Core/*.cpp'): scan(p)" > "$fc"
    if [ -z "$(scan_one_file "$fc")" ]; then
        echo "selftest: FAIL — shape C (non-recursive glob) not flagged" >&2; rc=1; fi
    local fc2="$tmp/fixture-c2.py"
    printf '%s\n' "for p in glob.glob('Source/Core/*.cpp'): scan(p)  # fail-open-ok: root-only by design" > "$fc2"
    if [ -n "$(scan_one_file "$fc2")" ]; then
        echo "selftest: FAIL — shape C escape marker did not clear the finding" >&2; rc=1; fi
    # A recursive form (os.walk / **) must NOT be flagged.
    local fc3="$tmp/fixture-c3.py"
    printf '%s\n' 'for root, _, files in os.walk("Source"): pass' > "$fc3"
    if [ -n "$(scan_one_file "$fc3")" ]; then
        echo "selftest: FAIL — shape C false-positive on a recursive os.walk" >&2; rc=1; fi
    local fc4="$tmp/fixture-c4.py"
    printf '%s\n' "for p in glob.glob('Source/**/*.cpp', recursive=True): scan(p)" > "$fc4"
    if [ -n "$(scan_one_file "$fc4")" ]; then
        echo "selftest: FAIL — shape C false-positive on a recursive ** glob" >&2; rc=1; fi

    # Shape D — reading a committed baseline snapshot into a want/expected var as the gate input.
    local fd="$tmp/fixture-d.sh"
    printf '%s\n' 'want="$(cat docs/high-integrity/baseline.md)"' > "$fd"
    if [ -z "$(scan_one_file "$fd")" ]; then
        echo "selftest: FAIL — shape D (stale artefact input) not flagged" >&2; rc=1; fi
    local fd2="$tmp/fixture-d2.sh"
    printf '%s\n' 'want="$(cat docs/high-integrity/baseline.md)" # fail-open-ok: drift-checked by post-merge --refresh' > "$fd2"
    if [ -n "$(scan_one_file "$fd2")" ]; then
        echo "selftest: FAIL — shape D escape marker did not clear the finding" >&2; rc=1; fi
    # WRITING the snapshot (--refresh) must NOT be flagged.
    local fd3="$tmp/fixture-d3.sh"
    printf '%s\n' 'gen_catalog > "$BASELINE_FILE"  # --baseline-md refresh' > "$fd3"
    if [ -n "$(scan_one_file "$fd3")" ]; then
        echo "selftest: FAIL — shape D false-positive on a snapshot WRITE" >&2; rc=1; fi

    # Shape E — hardcoded single-toolchain absolute path.
    local fe="$tmp/fixture-e.sh"
    printf '%s\n' '/usr/bin/gcc -fsyntax-only "$f"' > "$fe"
    if [ -z "$(scan_one_file "$fe")" ]; then
        echo "selftest: FAIL — shape E (single-toolchain literal) not flagged" >&2; rc=1; fi
    local fe2="$tmp/fixture-e2.sh"
    printf '%s\n' '/usr/bin/gcc -fsyntax-only "$f" # fail-open-ok: lint-only syntax probe, not the build toolchain' > "$fe2"
    if [ -n "$(scan_one_file "$fe2")" ]; then
        echo "selftest: FAIL — shape E escape marker did not clear the finding" >&2; rc=1; fi

    if [ "$rc" -eq 0 ]; then echo "test-fail-open-authoring --selftest: PASS"; fi
    return "$rc"
}

case "${1:---check}" in
    --check)        run_check "$ROOT" 0 ;;
    --check-strict) run_check "$ROOT" 1 ;;
    --selftest)     run_selftest ;;
    -h|--help)      echo "usage: test-fail-open-authoring.sh [--check|--check-strict|--selftest]" ;;
    *)              echo "usage: test-fail-open-authoring.sh [--check|--check-strict|--selftest]" >&2; exit 2 ;;
esac

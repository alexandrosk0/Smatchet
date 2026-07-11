#!/usr/bin/env bash
# test-fail-open-authoring.sh — authoring-lint for the fail-OPEN gate-shape cluster (PR-5;
# sibling of test-gate-selftests.sh, which checks that a selftest exercises a FAILURE case).
# ----------------------------------------------------------------------------
# Class of bug this kills: a gate / test-driver authored in a shape that returns
# GREEN on a NON-result, so a silently-broken gate ships "passing." The historical
# fail-open cluster (tooling.md → fail-open-gate-cluster-*; postmortems on the
# build-log-regex single-separator escape, the non-recursive scan, the stale
# committed artefact; the Batches 13–17 historical-review recurrence) groups the
# recurring authoring shapes:
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
# Batches 13–17 recurrence extension (historical-review-findings.md; the reopened
# fail-open-meta-gate-authoring-check entry in tooling.md):
#
#   Z — zero-run driver (FILE-level; the 12+-site recurrence class). A driver that
#       parses PASSED/FAILED counts and gates success ONLY on `$FAILED` (e.g.
#       `if [ "$FAILED" != "0" ]; then exit 1; fi; exit 0`) with NO floor on
#       `$PASSED` anywhere in the file — so a filter that matches ZERO tests
#       (rename/deregistration) yields passed=0/failed=0 and the driver exits 0
#       having executed nothing. Reported at the FAILED-gate line; a `$PASSED`
#       comparison against a number anywhere in the file (`-eq 0`, `-lt 1`,
#       `= "0"`, `-gt 0` assert, ...) counts as the floor and clears it.
#   F — unanchored suppression probe. A denylist/suppression membership test done
#       as an UNANCHORED substring match: `grep -qiF "$var"` (a broad token also
#       suppresses sibling names — the postmortem-owed broken-lane shape) or
#       `[[ "$x" =~ $SOME_SKIP/DENY/EXCLUDE/SUPPRESS_RE ]]` with unanchored tokens
#       (the CI_SKIP_RE `test-plan-index` → `test-plan-index-robustness-bats.sh`
#       over-match). Anchor to the whole name, or escape-mark a reviewed probe.
#   G — assertion-masking cleanup tail in bats (scanned over tests/bats/*.bats
#       ONLY). bats scores a @test by its LAST command; an `rm -r ...` cleanup
#       after the assertions masks any failing `[[ ... ]]` above it. Use bats'
#       auto-cleaned $BATS_TEST_TMPDIR or a teardown() instead.
#   H — errexit-aborts-graceful-path (scanned over .github/workflows/*.yml ONLY).
#       A command-substitution assignment whose pipeline can legitimately no-match
#       (`x=$( ... | grep ... )`, `... | tail -1)`) under the step's `bash -e`
#       (+ pipefail) with no `||` guard: a no-match aborts the STEP before its own
#       graceful-degradation branch (the build-and-test.yml childlog/perf-run
#       shape) — spurious red, or a skipped intended ::warning:: path.
#   I — explicit zero-tally green. A "nothing ran / nothing to gate" message whose
#       very next line resolves to `exit 0` — the gate KNOWS it verified zero
#       units and still chooses green (the mutation-smoke scored==0 shape).
#
# This is a STATIC authoring-lint (a text proxy, not execution): it greps the gate
# scripts for each shape. A deliberate, reviewed instance escapes with an inline
#     # fail-open-ok: <reason>
# marker on the offending line (or the line above). The synthetic-driver --selftest
# plants one bad snippet per shape and asserts each is flagged, then adds the
# escape marker and asserts it clears — proving the lint is live in both
# directions (asserts-failure).
#
# SCOPE: agents/scripts/{core,project}/ + scripts/dev/, first-party gate scripts
#   (*.sh / *.py / *.bash) for shapes B/C/D/E/F/I/Z; tests/bats/*.bats for shape G
#   only; .github/workflows/*.yml for shape H only. ThirdParty / build excluded by
#   living outside the dirs.
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
# selftest: asserts-failure — --selftest plants a bad snippet per shape B/C/D/E/F/G/H/I/Z and
# asserts each is flagged before the escape marker clears it.
# ----------------------------------------------------------------------------
set -uo pipefail

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"

SCAN_DIRS=(agents/scripts/core agents/scripts/project scripts/dev)
BATS_DIR=tests/bats
WF_DIR=.github/workflows

# This file is itself a gate script under a SCAN_DIR, and it necessarily MENTIONS
# every fail-open shape it hunts for (the doc-comment + the synthetic snippets). It
# must not flag itself. Self-exclude by basename.
SELF_BN="test-fail-open-authoring.sh"

# The inline escape marker for a deliberate, reviewed instance.
ESCAPE_RE='# fail-open-ok:'

# scan_one_file <file> <mode> — emit `<shape>\t<file>:<line>\t<rawline>` per fail-open
# shape, minus any line carrying the escape marker (on the line itself or the line
# above). mode: full (B/C/D/E/F/I + file-level Z) · bats (G only) · wf (H only).
scan_one_file() {
    local f="$1" mode="${2:-full}"
    [ -r "$f" ] || { echo "test-fail-open-authoring: unreadable file: $f" >&2; return 2; }
    local lineno=0 prev="" prev_escaped=0
    # Shape-G state: inside a @test block (a teardown()/setup() rm is the CORRECT
    # pattern and must not be flagged; only an in-@test cleanup tail masks).
    local in_test=0
    # Shape-Z file-level state: the first FAILED-only success gate seen + whether a
    # $PASSED floor exists anywhere in the file.
    local z_gate_line=0 z_gate_raw="" z_gate_escaped=0 z_passed_floor=0
    while IFS= read -r line || [ -n "$line" ]; do
        lineno=$((lineno + 1))
        local raw="$line" cur="$line"
        # Escape: marker on this line OR the immediately preceding line.
        local escaped=0
        case "$cur" in *"$ESCAPE_RE"*) escaped=1 ;; esac
        case "$prev" in *"$ESCAPE_RE"*) escaped=1 ;; esac
        local prev_raw="$prev" prev_was_escaped="$prev_escaped"
        prev="$cur"
        prev_escaped="$escaped"
        # Skip pure-comment lines (the doc-comment prose must not self-trip).
        local s="${cur#"${cur%%[![:space:]]*}"}"
        case "$s" in '#'*) continue ;; esac

        if [ "$mode" = "bats" ]; then
            # --- Shape G: assertion-masking cleanup tail in a @test block. ---
            # bats scores a test by its LAST command; a cleanup `rm -r ...` directly
            # before the @test's closing `}` (column 0) masks a failing assertion
            # above it. Use $BATS_TEST_TMPDIR or teardown() instead — an rm inside
            # teardown()/setup() is the correct pattern and is not flagged.
            case "$cur" in '@test '*'{'*) in_test=1 ;; esac
            if [ "$cur" = "}" ]; then
                if [ "$in_test" -eq 1 ] && [ "$escaped" -eq 0 ] && [ "$prev_was_escaped" -eq 0 ]; then
                    case "$prev_raw" in
                        *'rm -r'*)
                            printf 'G-bats-cleanup-tail\t%s:%s\t%s\n' "$f" "$((lineno - 1))" "$prev_raw" ;;
                    esac
                fi
                in_test=0
            fi
            continue
        fi

        if [ "$mode" = "wf" ]; then
            # --- Shape H: errexit-aborts-graceful-path in a workflow step. ---
            # `x=$( ... | grep/tail/sed ... )` under bash -e (+ pipefail): a legit
            # no-match fails the assignment and aborts the step before its own
            # graceful handling. `||` anywhere on the line means it is guarded.
            [ "$escaped" -eq 1 ] && continue
            case "$cur" in
                *'||'*) : ;;
                *'=$('*'| grep '*|*'=$('*'| tail '*|*'=$('*'| sed '*|\
                *'="$('*'| grep '*|*'="$('*'| tail '*|*'="$('*'| sed '*)
                    printf 'H-errexit-graceful-path\t%s:%s\t%s\n' "$f" "$lineno" "$raw" ;;
            esac
            continue
        fi

        # ---------------- mode=full: shapes B/C/D/E/F/I + Z state ----------------

        # --- Shape Z state (file-level; emitted after the loop). ---
        # FAILED-only success gate: a comparison that fails the run on $FAILED.
        if [ "$z_gate_line" -eq 0 ]; then
            case "$cur" in
                *'$FAILED'*'!= "0"'*|*'$FAILED'*'-gt 0'*|*'$FAILED'*'-ne 0'*|*'$FAILED'*'!= 0'*)
                    z_gate_line="$lineno"; z_gate_raw="$raw"; z_gate_escaped="$escaped" ;;
            esac
        fi
        # $PASSED floor: any numeric comparison of $PASSED (or string-eq against "0").
        if [ "$z_passed_floor" -eq 0 ]; then
            case "$cur" in
                *'$PASSED'*'-eq '*|*'$PASSED'*'-ne '*|*'$PASSED'*'-lt '*|*'$PASSED'*'-le '*|*'$PASSED'*'-gt '*|*'$PASSED'*'-ge '*)
                    z_passed_floor=1 ;;
                *'$PASSED'*'= "0"'*|*'$PASSED'*'== "0"'*)
                    z_passed_floor=1 ;;
            esac
        fi

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

        # --- Shape F: unanchored suppression probe. ---
        # (1) `grep -qiF "$var"` — a case-insensitive fixed-string SUBSTRING membership
        # test of a configured token against a name; a broad token also matches sibling
        # names (the postmortem-owed broken-lane shape). (2) `=~ $<...SKIP/DENY/EXCL/
        # SUPPRESS...>` — a deny/skip regex var matched unanchored against a full path
        # (the CI_SKIP_RE over-match shape). Anchor to the whole name to clear.
        case "$cur" in
            *'grep -qiF "$'*|*'grep -qFi "$'*)
                printf 'F-unanchored-suppression\t%s:%s\t%s\n' "$f" "$lineno" "$raw" ;;
            *'=~ $'*SKIP*|*'=~ $'*DENY*|*'=~ $'*EXCL*|*'=~ $'*SUPPRESS*)
                printf 'F-unanchored-suppression\t%s:%s\t%s\n' "$f" "$lineno" "$raw" ;;
        esac

        # --- Shape I: explicit zero-tally green. ---
        # A "nothing ran / nothing to gate" diagnostic immediately followed by a line
        # that resolves to `exit 0`: the gate KNOWS its scored/tested set was empty and
        # still chooses green (the mutation-smoke scored==0 shape). Diff-scoped no-ops
        # ("nothing to lint in this diff") are a different, legitimate idiom and are
        # not matched.
        case "$cur" in
            *'exit 0'*)
                case "$prev_raw" in
                    *'exit 1'*|*'exit 2'*|*'return 1'*) : ;;   # the message line fails closed itself — ok
                    *'nothing to gate'*|*'no scorable'*|*'no tests ran'*|*'matched no tests'*|*'0 tests ran'*)
                        [ "$prev_was_escaped" -eq 0 ] && \
                            printf 'I-zero-tally-green\t%s:%s\t%s\n' "$f" "$lineno" "$raw" ;;
                esac ;;
        esac
    done < "$f"

    # --- Shape Z emission (file-level): FAILED-only gate with no $PASSED floor. ---
    if [ "$mode" = "full" ] && [ "$z_gate_line" -gt 0 ] && [ "$z_passed_floor" -eq 0 ] \
        && [ "$z_gate_escaped" -eq 0 ]; then
        printf 'Z-zero-run-driver\t%s:%s\t%s\n' "$f" "$z_gate_line" "$z_gate_raw"
    fi
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

# list_aux_files <root> <dir> <ext> — supplementary scan lists (bats / workflows).
list_aux_files() {
    local root="$1" dir="$2" ext="$3" f
    [ -d "$root/$dir" ] || return 0
    for f in "$root/$dir"/*."$ext"; do
        [ -f "$f" ] || continue
        printf '%s\n' "$f"
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
        one="$(scan_one_file "$f" full)" || return 2
        [ -n "$one" ] && hits="$hits$one"$'\n'
    done <<EOF
$list
EOF
    # Supplementary scans: bats suites (shape G) + workflows (shape H). Their dirs
    # existing but yielding zero files is fine (advisory groups, not the main list).
    while IFS= read -r f; do
        [ -n "$f" ] || continue
        one="$(scan_one_file "$f" bats)" || return 2
        [ -n "$one" ] && hits="$hits$one"$'\n'
    done <<EOF
$(list_aux_files "$root" "$BATS_DIR" bats)
EOF
    while IFS= read -r f; do
        [ -n "$f" ] || continue
        one="$(scan_one_file "$f" wf)" || return 2
        [ -n "$one" ] && hits="$hits$one"$'\n'
    done <<EOF
$(list_aux_files "$root" "$WF_DIR" yml)
EOF
    hits="$(printf '%s' "$hits" | grep -E . || true)"
    if [ -n "$hits" ]; then
        local tag="WARN (advisory; calibration; not blocking)"
        [ "$strict" = "1" ] && tag="FAIL"
        echo "test-fail-open-authoring: $tag — fail-open authoring shape(s) detected:" >&2
        printf '%s\n' "$hits" | sed "s@^@  @; s@\t@ @g; s@$root/@@" >&2
        echo "  B zero-match grep passing · C non-recursive scan · D stale committed artefact as input ·" >&2
        echo "  E single-toolchain literal · F unanchored suppression probe · G bats cleanup tail masks" >&2
        echo "  assertions · H errexit aborts the graceful path · I explicit zero-tally green · Z FAILED-only" >&2
        echo "  driver with no PASSED floor. Fix the shape, or mark a reviewed instance with" >&2
        echo "  '# fail-open-ok: <reason>' on the line (or the line above). See PR-5 + the Batches 13-17" >&2
        echo "  recurrence in docs/self-improvement/categories/tooling.md." >&2
        [ "$strict" = "1" ] && rc=1
    else
        local n; n="$(printf '%s\n' "$list" | grep -c .)"
        echo "test-fail-open-authoring: PASS — $n gate scripts carry no un-escaped fail-open shape."
    fi
    return "$rc"
}

# run_selftest — synthetic driver: one planted bad snippet per shape must be
# flagged; the same snippet with the escape marker must clear. Asserts a failure case.
run_selftest() {
    local tmp rc=0
    tmp="$(mktemp -d)" || { echo "test-fail-open-authoring selftest: mktemp failed" >&2; return 2; }
    trap 'rm -rf "$tmp"' RETURN

    # Shape B — zero-match grep whose no-match path is a CLEAN signal, no `||` handler.
    local fb="$tmp/fixture-b.sh"
    printf '%s\n' 'grep -q "PATTERN" "$f" && echo clean' > "$fb"
    if [ -z "$(scan_one_file "$fb" full)" ]; then
        echo "selftest: FAIL — shape B (zero-match grep -> clean signal) not flagged" >&2; rc=1; fi
    # Escape marker clears it.
    local fb2="$tmp/fixture-b2.sh"
    printf '%s\n' 'grep -q "PATTERN" "$f" && echo clean # fail-open-ok: literal-existence probe' > "$fb2"
    if [ -n "$(scan_one_file "$fb2" full)" ]; then
        echo "selftest: FAIL — shape B escape marker did not clear the finding" >&2; rc=1; fi
    # A benign `grep -q X && continue` filter must NOT be flagged.
    local fb3="$tmp/fixture-b3.sh"
    printf '%s\n' 'grep -q "PATTERN" "$f" && continue' > "$fb3"
    if [ -n "$(scan_one_file "$fb3" full)" ]; then
        echo "selftest: FAIL — shape B false-positive on a 'grep -q X && continue' filter" >&2; rc=1; fi
    # A grep WITH a `||` no-match handler must NOT be flagged.
    local fb4="$tmp/fixture-b4.sh"
    printf '%s\n' 'grep -q "PATTERN" "$f" && echo clean || { echo bad; exit 1; }' > "$fb4"
    if [ -n "$(scan_one_file "$fb4" full)" ]; then
        echo "selftest: FAIL — shape B false-positive on a grep with a || fail-closed handler" >&2; rc=1; fi

    # Shape C — non-recursive python glob over a nesting tree (the #119 escape shape).
    local fc="$tmp/fixture-c.py"
    printf '%s\n' "for p in glob.glob('Source/Core/*.cpp'): scan(p)" > "$fc"
    if [ -z "$(scan_one_file "$fc" full)" ]; then
        echo "selftest: FAIL — shape C (non-recursive glob) not flagged" >&2; rc=1; fi
    local fc2="$tmp/fixture-c2.py"
    printf '%s\n' "for p in glob.glob('Source/Core/*.cpp'): scan(p)  # fail-open-ok: root-only by design" > "$fc2"
    if [ -n "$(scan_one_file "$fc2" full)" ]; then
        echo "selftest: FAIL — shape C escape marker did not clear the finding" >&2; rc=1; fi
    # A recursive form (os.walk / **) must NOT be flagged.
    local fc3="$tmp/fixture-c3.py"
    printf '%s\n' 'for root, _, files in os.walk("Source"): pass' > "$fc3"
    if [ -n "$(scan_one_file "$fc3" full)" ]; then
        echo "selftest: FAIL — shape C false-positive on a recursive os.walk" >&2; rc=1; fi
    local fc4="$tmp/fixture-c4.py"
    printf '%s\n' "for p in glob.glob('Source/**/*.cpp', recursive=True): scan(p)" > "$fc4"
    if [ -n "$(scan_one_file "$fc4" full)" ]; then
        echo "selftest: FAIL — shape C false-positive on a recursive ** glob" >&2; rc=1; fi

    # Shape D — reading a committed baseline snapshot into a want/expected var as the gate input.
    local fd="$tmp/fixture-d.sh"
    printf '%s\n' 'want="$(cat docs/high-integrity/baseline.md)"' > "$fd"
    if [ -z "$(scan_one_file "$fd" full)" ]; then
        echo "selftest: FAIL — shape D (stale artefact input) not flagged" >&2; rc=1; fi
    local fd2="$tmp/fixture-d2.sh"
    printf '%s\n' 'want="$(cat docs/high-integrity/baseline.md)" # fail-open-ok: drift-checked by post-merge --refresh' > "$fd2"
    if [ -n "$(scan_one_file "$fd2" full)" ]; then
        echo "selftest: FAIL — shape D escape marker did not clear the finding" >&2; rc=1; fi
    # WRITING the snapshot (--refresh) must NOT be flagged.
    local fd3="$tmp/fixture-d3.sh"
    printf '%s\n' 'gen_catalog > "$BASELINE_FILE"  # --baseline-md refresh' > "$fd3"
    if [ -n "$(scan_one_file "$fd3" full)" ]; then
        echo "selftest: FAIL — shape D false-positive on a snapshot WRITE" >&2; rc=1; fi

    # Shape E — hardcoded single-toolchain absolute path.
    local fe="$tmp/fixture-e.sh"
    printf '%s\n' '/usr/bin/gcc -fsyntax-only "$f"' > "$fe"
    if [ -z "$(scan_one_file "$fe" full)" ]; then
        echo "selftest: FAIL — shape E (single-toolchain literal) not flagged" >&2; rc=1; fi
    local fe2="$tmp/fixture-e2.sh"
    printf '%s\n' '/usr/bin/gcc -fsyntax-only "$f" # fail-open-ok: lint-only syntax probe, not the build toolchain' > "$fe2"
    if [ -n "$(scan_one_file "$fe2" full)" ]; then
        echo "selftest: FAIL — shape E escape marker did not clear the finding" >&2; rc=1; fi

    # Shape F — unanchored suppression probe (both sub-forms).
    local ff="$tmp/fixture-f.sh"
    printf '%s\n' 'printf %s "$name" | grep -qiF "$lane" && matched=1' > "$ff"
    if ! scan_one_file "$ff" full | grep -q '^F-'; then
        echo "selftest: FAIL — shape F (grep -qiF substring probe) not flagged" >&2; rc=1; fi
    local ff2="$tmp/fixture-f2.sh"
    printf '%s\n' '[[ "$script" =~ $CI_SKIP_RE ]] && skip=1' > "$ff2"
    if ! scan_one_file "$ff2" full | grep -q '^F-'; then
        echo "selftest: FAIL — shape F (=~ \$SKIP_RE unanchored deny) not flagged" >&2; rc=1; fi
    local ff3="$tmp/fixture-f3.sh"
    printf '%s\n' '[[ "$script" =~ $CI_SKIP_RE ]] && skip=1 # fail-open-ok: tokens are anchored basenames' > "$ff3"
    if scan_one_file "$ff3" full | grep -q '^F-'; then
        echo "selftest: FAIL — shape F escape marker did not clear the finding" >&2; rc=1; fi
    # An anchored, case-sensitive whole-name equality must NOT be flagged.
    local ff4="$tmp/fixture-f4.sh"
    printf '%s\n' '[ "$name" = "$lane" ] && matched=1' > "$ff4"
    if scan_one_file "$ff4" full | grep -q '^F-'; then
        echo "selftest: FAIL — shape F false-positive on an anchored whole-name equality" >&2; rc=1; fi

    # Shape G — bats cleanup tail masking the assertions (bats mode only).
    local fg="$tmp/fixture-g.bats"
    printf '%s\n' '@test "x" {' '    [[ "$output" == *"hit"* ]]' '    rm -rf "$tmp"' '}' > "$fg"
    if ! scan_one_file "$fg" bats | grep -q '^G-'; then
        echo "selftest: FAIL — shape G (bats rm-cleanup tail) not flagged" >&2; rc=1; fi
    local fg2="$tmp/fixture-g2.bats"
    printf '%s\n' '@test "x" {' '    [[ "$output" == *"hit"* ]]' '    rm -rf "$tmp" # fail-open-ok: no assertions above' '}' > "$fg2"
    if scan_one_file "$fg2" bats | grep -q '^G-'; then
        echo "selftest: FAIL — shape G escape marker did not clear the finding" >&2; rc=1; fi
    # Assertion-last (cleanup in teardown) must NOT be flagged.
    local fg3="$tmp/fixture-g3.bats"
    printf '%s\n' '@test "x" {' '    rm -rf "$tmp/stale"' '    [[ "$output" == *"hit"* ]]' '}' > "$fg3"
    if scan_one_file "$fg3" bats | grep -q '^G-'; then
        echo "selftest: FAIL — shape G false-positive on an assertion-last test" >&2; rc=1; fi

    # Shape H — errexit-aborts-graceful-path in a workflow (wf mode only).
    local fh="$tmp/fixture-h.yml"
    printf '%s\n' '          childlog="$(sed -n 1p "$out" | grep -a "child stdout" | tail -1)"' > "$fh"
    if ! scan_one_file "$fh" wf | grep -q '^H-'; then
        echo "selftest: FAIL — shape H (unguarded piped assignment in workflow) not flagged" >&2; rc=1; fi
    local fh2="$tmp/fixture-h2.yml"
    printf '%s\n' '          childlog="$(sed -n 1p "$out" | grep -a "child stdout" | tail -1 || true)"' > "$fh2"
    if scan_one_file "$fh2" wf | grep -q '^H-'; then
        echo "selftest: FAIL — shape H false-positive on a ||-guarded piped assignment" >&2; rc=1; fi
    local fh3="$tmp/fixture-h3.yml"
    printf '%s\n' '          # fail-open-ok: step is continue-on-error' '          childlog="$(cat "$out" | grep -a "child stdout" | tail -1)"' > "$fh3"
    if scan_one_file "$fh3" wf | grep -q '^H-'; then
        echo "selftest: FAIL — shape H escape marker did not clear the finding" >&2; rc=1; fi

    # Shape I — explicit zero-tally green.
    local fi_="$tmp/fixture-i.sh"
    printf '%s\n' 'echo "gate: no scorable mutants ran — nothing to gate."' '[ "$bad" -gt 0 ] && exit 1 || exit 0' > "$fi_"
    if ! scan_one_file "$fi_" full | grep -q '^I-'; then
        echo "selftest: FAIL — shape I (explicit zero-tally green) not flagged" >&2; rc=1; fi
    local fi2="$tmp/fixture-i2.sh"
    printf '%s\n' 'echo "gate: no scorable mutants ran — nothing to gate." # fail-open-ok: advisory lane' '[ "$bad" -gt 0 ] && exit 1 || exit 0' > "$fi2"
    if scan_one_file "$fi2" full | grep -q '^I-'; then
        echo "selftest: FAIL — shape I escape marker did not clear the finding" >&2; rc=1; fi
    # A zero-tally that fails closed must NOT be flagged.
    local fi3="$tmp/fixture-i3.sh"
    printf '%s\n' 'echo "gate: no scorable mutants ran — nothing to gate."' 'exit 1' > "$fi3"
    if scan_one_file "$fi3" full | grep -q '^I-'; then
        echo "selftest: FAIL — shape I false-positive on a fail-closed zero-tally" >&2; rc=1; fi
    # A zero-run guard that fails closed INLINE, followed by the normal success exit,
    # must NOT be flagged (the compliant-driver shape).
    local fi4="$tmp/fixture-i4.sh"
    printf '%s\n' 'if [ "$PASSED" -eq 0 ]; then echo "FAIL: 0 tests ran"; exit 1; fi' 'exit 0' > "$fi4"
    if scan_one_file "$fi4" full | grep -q '^I-'; then
        echo "selftest: FAIL — shape I false-positive on an inline fail-closed zero-run guard" >&2; rc=1; fi

    # Shape Z — FAILED-only driver with no PASSED floor (file-level).
    local fz="$tmp/fixture-z.sh"
    printf '%s\n' 'echo "Passed: $PASSED  Failed: $FAILED"' 'if [ "$FAILED" != "0" ]; then exit 1; fi' 'exit 0' > "$fz"
    if ! scan_one_file "$fz" full | grep -q '^Z-'; then
        echo "selftest: FAIL — shape Z (FAILED-only driver, no PASSED floor) not flagged" >&2; rc=1; fi
    # A driver WITH a zero-run floor must NOT be flagged.
    local fz2="$tmp/fixture-z2.sh"
    printf '%s\n' 'if [ "$FAILED" != "0" ]; then exit 1; fi' 'if [ "$PASSED" -eq 0 ]; then echo "FAIL: 0 tests ran"; exit 1; fi' 'exit 0' > "$fz2"
    if scan_one_file "$fz2" full | grep -q '^Z-'; then
        echo "selftest: FAIL — shape Z false-positive on a driver with a PASSED floor" >&2; rc=1; fi
    # Escape marker on the FAILED-gate line clears it.
    local fz3="$tmp/fixture-z3.sh"
    printf '%s\n' 'if [ "$FAILED" != "0" ]; then exit 1; fi # fail-open-ok: aggregate wrapper, floor lives in test-all.sh' 'exit 0' > "$fz3"
    if scan_one_file "$fz3" full | grep -q '^Z-'; then
        echo "selftest: FAIL — shape Z escape marker did not clear the finding" >&2; rc=1; fi

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

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

# Self-exec of a mode-100644 script (asserts-failure-marker-does-not-prove-the-
# negative-is-reachable, tooling P1): `"$_SCRIPT_PATH" --check` execs a file
# with no +x bit, fails 126 Permission denied on every machine, and a negative
# asserting only "non-zero" is satisfied by that forever — test-plan-index.sh's
# negative lived its whole life that way. Re-invocation must go through
# `bash "$path"`. Judged PER COMMAND SEGMENT, not per line: a whole-line
# allow-check let `bash "$p" --x; "$p" --y` slide (the later bash masked the
# earlier raw exec) and misread `printf '%s' "$p" --x` as an exec (the path is
# an argument there, not a command). Lines are split at command boundaries
# (; | && ` $( parens braces), leading keywords/env-assignments are stripped,
# and only a segment that BEGINS with the quoted path is an exec of it — which
# also makes bash/sh/. prefixes pass with no separate allow-list.
SELF_EXEC_RE='"\$(_SCRIPT_PATH|SCRIPT_PATH|0)"[[:space:]]+--'

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

# _selfexec_hits <file> — print `lineno:line` for every non-comment line with a
# raw self-exec in COMMAND POSITION. Boundary split + keyword/assignment strip
# is a heuristic lexer, deliberately shallow: false splits only create extra
# segments (never hide one), and a segment must still BEGIN with the quoted
# path to count, so `printf '%s' "$p" --x` and prose in strings stay clean.
_selfexec_hits() {
    awk '
        BEGIN {
            # Leading tokens that keep what follows in command position:
            # control keywords / ! / exec-style wrappers; env assignments with
            # bare, double- or single-quoted values (FOO="a b" — the quoted
            # space defeated the bare [^space]* version); redirections
            # (2>/dev/null before the command). String form so \047 can carry
            # the single quote through the shell single-quoted awk program.
            strip = "^((if|elif|while|until|then|else|do|time|exec|command|env|nohup|!)[[:space:]]+" \
                    "|[A-Za-z_][A-Za-z0-9_]*=(\"[^\"]*\"|\047[^\047]*\047|[^[:space:]]*)[[:space:]]+" \
                    "|[0-9]*(<|>>|>)[^[:space:]]*[[:space:]]+)"
        }
        /^[[:space:]]*#/ { next }
        {
            n = split($0, seg, /&&|[|;`(){}]|\$\(/)
            for (i = 1; i <= n; i++) {
                s = seg[i]
                sub(/^[[:space:]]+/, "", s)
                while (sub(strip, "", s)) { }
                if (s ~ /^"\$(_SCRIPT_PATH|SCRIPT_PATH|0)"[[:space:]]+--/) {
                    printf "%d:%s\n", NR, $0
                    next
                }
            }
        }
    ' "$1"
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
    local selfexec=()
    while IFS= read -r f; do
        [ -n "$f" ] || continue
        if ! grep -qF -- "$MARKER_RE" "$f"; then
            missing+=("$f")
        fi
        # BLOCKING: raw self-exec — the negative it feeds can never reach the
        # behaviour it names (126 before the script runs).
        # Comment lines are excluded: docs legitimately QUOTE the bad shape
        # (this very script does, one screen up) without executing it.
        # Cheap prefilter first; the awk lexer only runs on files that match.
        if grep -qE -- "$SELF_EXEC_RE" "$f" && [ -n "$(_selfexec_hits "$f")" ]; then
            selfexec+=("$f")
        fi
    done <<EOF
$list
EOF
    if [ "${#selfexec[@]}" -ne 0 ]; then
        echo "test-gate-selftests: FAIL — raw self-exec of a mode-100644 script (fails 126" >&2
        echo "  Permission denied before any behaviour runs, silently satisfying bare-status" >&2
        echo "  negatives — the test-plan-index escape). Re-invoke via bash \"\$path\":" >&2
        for f in "${selfexec[@]}"; do
            echo "    ${f#"$root"/}:" >&2
            _selfexec_hits "$f" | head -3 | sed 's/^/      /' >&2
        done
        rc=1
    fi
    if [ "${#missing[@]}" -ne 0 ]; then
        echo "test-gate-selftests: FAIL — these scripts expose --selftest but their selftest" >&2
        echo "  asserts no FAILURE case (no '# selftest: asserts-failure' marker):" >&2
        for f in "${missing[@]}"; do echo "    ${f#"$root"/}" >&2; done
        echo "  Backfill a real negative assertion (feed known-bad input -> expect non-zero/" >&2
        echo "  failure token) and mark it, or use '# selftest: asserts-failure: n/a — <reason>'" >&2
        echo "  for a genuine pure dispatcher. See close-gate-gaps Slice 3." >&2
        rc=1
    fi
    # PASS prints only when EVERY rule held — a green line above a red exit was
    # exactly the contradiction the selfexec branch first shipped with (caught
    # by reading the combined output, not the code).
    if [ "$rc" -eq 0 ]; then
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

    # Raw self-exec must be flagged, and for the RIGHT reason — the output must
    # name the rule, since a bare rc check would also pass on an unrelated FAIL.
    # The bad shape is ASSEMBLED, never written literally: this very gate greps
    # files for it, and a literal fixture here would flag the gate itself (it
    # did — same self-flagging the INDEX-marker fixture avoids the same way).
    {
        printf '#!/usr/bin/env bash\n# selftest: asserts-failure\ncase "${1:-}" in\n'
        printf '    --selftest) "%s" --check; exit 0 ;;\nesac\n' '$_SCRIPT_PATH'
    } > "$synth"
    local out
    out="$(run_check "$tmp" 2>&1)" && {
        echo "test-gate-selftests selftest: FAIL — raw self-exec was NOT flagged" >&2
        rc=1
    }
    case "$out" in
        *"raw self-exec"*) ;;
        *) echo "test-gate-selftests selftest: FAIL — self-exec exposer rejected for the wrong reason:" >&2
           printf '%s\n' "$out" | sed 's/^/    /' >&2; rc=1 ;;
    esac

    # A raw self-exec MASKED by a legitimate bash invocation on the same line
    # must still be flagged — the allow-check is per command segment, not per
    # line (CodeRabbit finding on PR #2002; also the second edge in that PR's
    # recorded verdict). Assembled, same as above.
    {
        printf '#!/usr/bin/env bash\n# selftest: asserts-failure\ncase "${1:-}" in\n'
        printf '    --selftest) bash "%s" --check; "%s" --selftest; exit 0 ;;\nesac\n' '$_SCRIPT_PATH' '$_SCRIPT_PATH'
    } > "$synth"
    if run_check "$tmp" >/dev/null 2>&1; then
        echo "test-gate-selftests selftest: FAIL — raw self-exec masked by a same-line bash invocation was NOT flagged" >&2
        rc=1
    fi

    # A raw exec behind a QUOTED-value env assignment must be flagged — the
    # bare [^space]* strip broke at the quoted space and the leftover masked
    # the path (CodeRabbit round-2 finding on PR #2002). Assembled, as above.
    {
        printf '#!/usr/bin/env bash\n# selftest: asserts-failure\ncase "${1:-}" in\n'
        printf '    --selftest) FOO="a b" "%s" --check; exit 0 ;;\nesac\n' '$_SCRIPT_PATH'
    } > "$synth"
    out="$(run_check "$tmp" 2>&1)" && {
        echo "test-gate-selftests selftest: FAIL — raw self-exec behind a quoted env assignment was NOT flagged" >&2
        rc=1
    }
    case "$out" in
        *"raw self-exec"*) ;;
        *) echo "test-gate-selftests selftest: FAIL — quoted-assignment exposer rejected for the wrong reason:" >&2
           printf '%s\n' "$out" | sed 's/^/    /' >&2; rc=1 ;;
    esac

    # A raw exec behind a leading REDIRECTION must be flagged (same finding).
    {
        printf '#!/usr/bin/env bash\n# selftest: asserts-failure\ncase "${1:-}" in\n'
        printf '    --selftest) 2>/dev/null "%s" --check; exit 0 ;;\nesac\n' '$_SCRIPT_PATH'
    } > "$synth"
    out="$(run_check "$tmp" 2>&1)" && {
        echo "test-gate-selftests selftest: FAIL — raw self-exec behind a leading redirection was NOT flagged" >&2
        rc=1
    }
    case "$out" in
        *"raw self-exec"*) ;;
        *) echo "test-gate-selftests selftest: FAIL — redirection exposer rejected for the wrong reason:" >&2
           printf '%s\n' "$out" | sed 's/^/    /' >&2; rc=1 ;;
    esac

    # A TAB between the path and the flag must also be flagged — the grep
    # prefilter matched literal spaces only, so a tab-separated exec never
    # reached the awk lexer (CodeRabbit round-3 finding on PR #2002).
    {
        printf '#!/usr/bin/env bash\n# selftest: asserts-failure\ncase "${1:-}" in\n'
        printf '    --selftest) "%s"\t--check; exit 0 ;;\nesac\n' '$_SCRIPT_PATH'
    } > "$synth"
    out="$(run_check "$tmp" 2>&1)" && {
        echo "test-gate-selftests selftest: FAIL — tab-separated raw self-exec was NOT flagged" >&2
        rc=1
    }
    case "$out" in
        *"raw self-exec"*) ;;
        *) echo "test-gate-selftests selftest: FAIL — tab-separated exposer rejected for the wrong reason:" >&2
           printf '%s\n' "$out" | sed 's/^/    /' >&2; rc=1 ;;
    esac

    # The path as a mere ARGUMENT is not an exec of it and must pass — only a
    # segment that BEGINS with the quoted path counts as command position.
    {
        printf '#!/usr/bin/env bash\n# selftest: asserts-failure\ncase "${1:-}" in\n'
        printf "    --selftest) printf '%%s ' %s --check; exit 0 ;;\nesac\n" '"$_SCRIPT_PATH"'
    } > "$synth"
    if ! run_check "$tmp" >/dev/null 2>&1; then
        echo "test-gate-selftests selftest: FAIL — script path used as a printf ARGUMENT was wrongly flagged" >&2
        rc=1
    fi

    # The bash-prefixed form is the CORRECT shape and must pass; so must a
    # comment that merely quotes the bad shape (docs cite it legitimately).
    cat > "$synth" <<'SH'
#!/usr/bin/env bash
# selftest: asserts-failure
# never invoke as "$_SCRIPT_PATH" --check (126 on a 100644 file)
case "${1:-}" in
    --selftest) bash "$_SCRIPT_PATH" --check; exit 0 ;;
esac
SH
    if ! run_check "$tmp" >/dev/null 2>&1; then
        echo "test-gate-selftests selftest: FAIL — bash-prefixed self-invocation (or a quoting comment) was wrongly flagged" >&2
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

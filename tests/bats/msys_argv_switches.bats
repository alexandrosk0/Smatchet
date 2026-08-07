#!/usr/bin/env bats
#
# msys_argv_switches.bats — guards the MSYS argument-translation footgun.
#
# System under test: every bash script that invokes a NATIVE Windows exe with a
# `/switch` argument (`cmd.exe /c`, `ISCC /D...`, `signtool /fd`).
#
# The bug: Git Bash rewrites a whole argument that looks like an absolute POSIX
# path before exec. `/c` reaches the child as `C:/`; `/DMyAppVersion=0.1.0`
# reaches it as `C:/Program Files/Git/DMyAppVersion=0.1.0`. The switch is gone.
# Converting only the *value* (winpath) does not help — the leading `/D` is what
# triggers the rewrite. The fix is `MSYS_NO_PATHCONV=1` on the invocation (or a
# helper such as release-github.sh's `native_exec`).
#
# This shipped as a live develop breakage: PR #1957 ported release_github.ps1 to
# bash, and both installer-smoke backstop jobs went red with
# "You may not specify more than one script filename." — ISCC counted the
# mangled `/DMyAppVersion=...` as a second script path. PR checks never see it
# because the installer smokes are push-to-develop-only backstops.
#
# Bucket A (CLI) per AGENTS.md § Verification automation. Zero manual steps.

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    cd "$REPO_ROOT" || return 1
}

# A call site is safe when the invocation carries MSYS_NO_PATHCONV=1 or goes
# through a wrapper whose name says it suppresses translation (native_exec).
# `exe` is the shell expansion that names the binary; the regex is anchored at
# the start of a command so mentions inside strings / test expressions (e.g.
# `[ -n "$ISCC_EXE" ]`) are not mistaken for invocations.
GUARD_PREFIX_RE='(MSYS_NO_PATHCONV=1[[:space:]]+|native_exec[[:space:]]+)'

assert_guarded() {
    local file="$1" exe="$2"
    # A call site is any command whose first word is the exe expansion; the guard
    # prefix is optional here so unguarded sites are still collected.
    local call="^[[:space:]]*${GUARD_PREFIX_RE}?\"\\\$${exe}\"[[:space:]]"
    # Same pattern with the guard prefix MANDATORY. Matching the guard token
    # anywhere in the line would pass for a trailing comment or an unrelated
    # expression, so the check re-anchors on the command's first word.
    local guarded="^[[:space:]]*${GUARD_PREFIX_RE}\"\\\$${exe}\"[[:space:]]"
    local hits
    hits="$(grep -nE "$call" "$file" || true)"
    [ -n "$hits" ] || {
        echo "no call site matching /$call/ in $file — test is stale, update it" >&2
        return 1
    }
    local bad=""
    while IFS= read -r line; do
        # grep -n prefixes `NNN:`; strip it before re-anchoring on the command.
        printf '%s\n' "${line#*:}" | grep -qE "$guarded" \
            || bad="$bad$file:$line"$'\n'
    done <<< "$hits"
    if [ -n "$bad" ]; then
        echo "unguarded native-exe /switch invocation (needs MSYS_NO_PATHCONV=1):" >&2
        printf '%s' "$bad" >&2
        return 1
    fi
    return 0
}

@test "release-github.sh defines native_exec and it suppresses path conversion" {
    # Match the body itself (`MSYS_NO_PATHCONV=1 "$@"`), not merely the token
    # somewhere in the first three lines — a comment would satisfy that.
    run grep -A2 -E '^native_exec\(\)' scripts/publish/release-github.sh
    [ "$status" -eq 0 ]
    run bash -c "grep -A2 -E '^native_exec\\(\\)' scripts/publish/release-github.sh \
        | grep -qE '^[[:space:]]*MSYS_NO_PATHCONV=1[[:space:]]+\"\\\$@\"[[:space:]]*\$'"
    [ "$status" -eq 0 ]
}

@test "ISCC invocation is guarded against MSYS argument translation" {
    assert_guarded scripts/publish/release-github.sh ISCC_EXE
}

@test "signtool invocations are guarded against MSYS argument translation" {
    assert_guarded scripts/publish/release-github.sh SIGN_TOOL
}

@test "every cmd.exe invocation in tracked shell scripts is guarded" {
    # Matches an actual invocation (`cmd.exe` followed by a `/switch`), not a
    # mention inside a comment or a PowerShell string.
    local offenders=""
    while IFS= read -r f; do
        [ -f "$f" ] || continue
        while IFS= read -r line; do
            case "$line" in
                \#*|*\#\ *cmd.exe*) continue ;;
            esac
            # The guard must PREFIX the command — the token elsewhere on the
            # line (trailing comment, unrelated expression) does not count.
            printf '%s\n' "${line#*:}" \
                | grep -qE "^[[:space:]]*${GUARD_PREFIX_RE}cmd\.exe[[:space:]]" \
                || offenders="$offenders$f: $line"$'\n'
        done < <(grep -nE '(^|[^#]*[^"'"'"'[:alnum:]])cmd\.exe[[:space:]]+/[A-Za-z]' "$f" \
                 | grep -vE '^[0-9]+:[[:space:]]*#' || true)
    done < <(git ls-files '*.sh')
    if [ -n "$offenders" ]; then
        echo "unguarded cmd.exe /switch invocation (needs MSYS_NO_PATHCONV=1):" >&2
        printf '%s' "$offenders" >&2
        return 1
    fi
}

@test "MSYS really does mangle a bare /switch (behavioural, Windows-only)" {
    # MINGW/MSYS only — MSYS_NO_PATHCONV does not control Cygwin's own argument
    # conversion, so the second assertion below would be a false red there.
    case "$(uname -s)" in
        MINGW*|MSYS*) ;;
        *) skip "MSYS argument translation only exists under Git Bash on Windows" ;;
    esac
    command -v python >/dev/null 2>&1 || skip "python not on PATH"

    # Without the guard the switch is rewritten...
    run python -c 'import sys;print(sys.argv[1])' /DMyAppVersion=0.1.0
    [ "$status" -eq 0 ]
    [ "$output" != "/DMyAppVersion=0.1.0" ]

    # ...and with it the switch survives verbatim.
    run env MSYS_NO_PATHCONV=1 python -c 'import sys;print(sys.argv[1])' /DMyAppVersion=0.1.0
    [ "$status" -eq 0 ]
    [ "$output" = "/DMyAppVersion=0.1.0" ]
}

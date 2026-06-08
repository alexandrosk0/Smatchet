#!/usr/bin/env bash
# test-shell-lint.sh — five-rule self-review lint for shell scripts.
#
# Closes docs/self-improvement/categories/process.md 2026-05-28 P1 entry
# "Implementer-side self-review didn't catch real shell-script bugs".
# Plan: docs/plans/shipped/shell-script-self-review-lint.md.
#
# Rules (one per CR finding class from session 2026-05-28):
#   1. Dependency preflight — every allowlisted external must have
#      `command -v X` / `which X` / `type -p X` guard.
#   2. shellcheck clean (SC2086 / 2046 / 2128 / 2155 / 2068).
#   3. curl -f / --fail on every curl invocation.
#   4. sha256 verify within 10 lines after `curl -o <path>` / `--output <path>`.
#   5. --key=value ↔ --key value parity when the flag takes a value.
#
# Targets: scripts/dev/*.sh + agents/scripts/{core,project}/*.sh (post-#609
# layout; maxdepth 1, so scripts/dev/local/ is excluded). Lints itself.
#
# Bypass: SMATCHET_SKIP_SHELL_LINT=1 (logged when used).
#
# Exit codes:
#   0 — all-pass (Passed: N  Failed: 0), or shellcheck not on PATH (warn-only
#       fallback matches the cppcheck/clang-tidy missing-toolchain pattern)
#   1 — at least one violation (Passed: N  Failed: M)

set -euo pipefail
cd "$(dirname "$0")/../../.."

if [ "${SMATCHET_SKIP_SHELL_LINT:-0}" = "1" ]; then
    echo "test-shell-lint: SMATCHET_SKIP_SHELL_LINT=1 — skipping all checks" >&2
    echo "Passed: 0  Failed: 0"
    exit 0
fi

if ! command -v shellcheck >/dev/null 2>&1; then
    echo "test-shell-lint: WARN: shellcheck not on PATH; install via 'npm install -g shellcheck'" >&2
    echo "Passed: 0  Failed: 0"
    exit 0
fi

# 19-entry closed allowlist (see plan § Approach rule 1).
# `git` was grilled-in but dropped during implementation: it's effectively a
# shell builtin in any dev environment (BUILD.md § Prereqs makes it mandatory)
# and preflighting it generated 25 violations across the existing tree with
# near-zero practical value. Captured in plan § Deviations.
ALLOWLIST=(curl gh cmake python python3 jq 7z cppcheck clang-format clang-tidy shellcheck actionlint bats p4 cl.exe clang-cl link.exe cygpath tasklist)

# --target <path> lints a single script (used by bats fixture harness).
TARGETS=()
if [ "${1:-}" = "--target" ] && [ -n "${2:-}" ]; then
    TARGETS=("$2")
else
    while IFS= read -r f; do TARGETS+=("$f"); done \
        < <(find scripts/dev agents/scripts/core agents/scripts/project -maxdepth 1 -type f -name '*.sh' | sort)
fi

VIOLATIONS=()
emit() { VIOLATIONS+=("$1:$2: $3: $4"); }

# Strip comments + blank lines so heuristic checks don't fire on prose.
non_comment() { sed -E 's/(^|[^\\])#.*$/\1/' "$1"; }

# Rule 1 — dependency preflight.
# A "command use" is the tool appearing at a command-start position:
#   start-of-line (with optional indent), after `&&`/`||`/`|`/`;`, inside
#   `$(...)` / backticks, or after `if`/`while`/`until`/`then`/`else`/`!`/`&`.
# This excludes bare-word array list contexts (e.g. `ALLOWLIST=(curl gh git)`),
# and graceful-degradation shapes (same-line `|| <fallback>`, if/while/until
# conditions) are filtered out below — they self-handle a missing tool.
check_deps() {
    local script="$1"
    local nc
    nc=$(non_comment "$script")
    for tool in "${ALLOWLIST[@]}"; do
        local tool_escaped="${tool//./\\.}"
        # Match command-start tokens then the tool then a non-identifier follow.
        # Boundaries:
        #   ^[[:space:]]*                       — start of line
        #   [|&;][|&]?[[:space:]]*              — after pipe / semicolon / && / ||
        #   \$\([[:space:]]*                    — $( cmd-subst opener
        #   \`[[:space:]]*                      — backtick subst opener
        #   (if|while|until|then|else|!)[[:space:]]+   — control-flow lead
        local hits
        hits=$(printf '%s\n' "$nc" | grep -nE "(^[[:space:]]*|[|&;][|&]?[[:space:]]*|\\\$\\([[:space:]]*|\\\`[[:space:]]*|\\b(if|while|until|then|else|exec|!)[[:space:]]+)${tool_escaped}([[:space:]]|$)" || true)
        if [ -z "$hits" ]; then continue; fi
        # Drop preflight lines themselves.
        local real_use
        real_use=$(printf '%s\n' "$hits" | grep -vE "(command -v|which|type -p)[[:space:]]+${tool_escaped}\\b" || true)
        if [ -z "$real_use" ]; then continue; fi
        # Graceful-degradation shapes are NOT unguarded uses — drop them before
        # deciding (these handle a missing/failed tool inline, so a separate
        # `command -v` preflight is redundant). Two shapes:
        #   * same-line trailing fallback `<tool> <args> || <fallback>`: the
        #     tool's own failure is caught inline (e.g. the lock scripts'
        #     `python3 -c … || python -c … || printf`, setup-harness's
        #     `cygpath … || echo`). The span is quote-aware: a `;` inside a
        #     quoted arg (python's `-c 'import json,sys;…'`) does not end it,
        #     but a bare `;` statement boundary does — so an unrelated later
        #     command's `||` is not mistaken for the tool's own guard.
        #   * if/while/until CONDITION: the control-flow itself IS the
        #     availability test (e.g. clear-session-context's
        #     `if P4_INFO="$(p4 info 2>/dev/null)"; then`).
        real_use=$(printf '%s\n' "$real_use" \
            | grep -vE "${tool_escaped}[[:space:]]([^;'\"]|'[^']*'|\"[^\"]*\")*[|][|]" \
            | grep -vE '^[0-9]+:[[:space:]]*(if|while|until)[[:space:]]' \
            || true)
        if [ -z "$real_use" ]; then continue; fi
        # Does the script have ANY preflight for this tool? Strip comments
        # first so a comment mentioning "command -v <tool>" (e.g. in this
        # fixture's own header) doesn't fool the check.
        if printf '%s\n' "$nc" | grep -qE "(command -v|which|type -p)[[:space:]]+${tool_escaped}\\b"; then
            continue
        fi
        # First line's leading line-number (grep -n "<lno>:..."). Pure param
        # expansion — NOT `printf | head -1 | cut`: under `set -euo pipefail`,
        # head closing the pipe early SIGPIPEs printf, so the PLAIN assignment
        # returns 141 and set -e aborts the whole gate (exit 141). It trips only
        # when $real_use is multi-line (a tool used on many lines in a scanned
        # script), and is CI-only — msys bash ignores SIGPIPE, so it kept
        # passing 137/137 locally while breaking every PR's Shell-lint job.
        local lno first_line
        first_line="${real_use%%$'\n'*}"
        lno="${first_line%%:*}"
        emit "$script" "$lno" "SHELL_LINT_DEPS" "external '$tool' used without 'command -v $tool' (or which/type -p) preflight"
    done
}

# Rule 2 — shellcheck clean. `-S warning` plus `--include` for the specific
# codes the plan cites (SC2086 is info-level by default in shellcheck and
# would be filtered out by -S warning alone).
SHELLCHECK_CODES="SC2086,SC2046,SC2128,SC2155,SC2068"
check_shellcheck() {
    local script="$1"
    local out
    if out=$(shellcheck --include="$SHELLCHECK_CODES" "$script" 2>&1); then return; fi
    while IFS= read -r line; do
        # `In <path> line N:` — path may include Windows-style `C:/…` so
        # don't use `[^:]+` here (would stop at the drive-letter colon).
        if [[ "$line" =~ ^In\ .+\ line\ ([0-9]+): ]]; then
            local lno="${BASH_REMATCH[1]}"
            emit "$script" "$lno" "SHELL_LINT_SHELLCHECK" "shellcheck warning (run 'shellcheck $script' for detail)"
        fi
    done <<< "$out"
}

# Rule 3 — every curl invocation has -f / --fail.
check_curl_fail() {
    local script="$1"
    while IFS=: read -r lno content; do
        # Skip lines that match `curl` only as a literal / command-v probe /
        # comment (already stripped) / string mention.
        case "$content" in
            *"command -v curl"*|*"which curl"*|*"type -p curl"*) continue ;;
            *'"curl"'*|*"'curl'"*) continue ;;
        esac
        # Only fire when curl is the command (preceded by whitespace / start-of-
        # line / `&&` / `||` / `|` / `;`, followed by whitespace).
        if ! echo "$content" | grep -qE '(^|[[:space:]]|&&|\|\||\||;)curl([[:space:]]|$)'; then
            continue
        fi
        # Look on the curl line + next 2 lines (curl is often line-continued).
        local window
        window=$(sed -n "${lno},$((lno+2))p" "$script")
        if echo "$window" | grep -qE -- '(-[a-zA-Z]*f[a-zA-Z]*|--fail)\b'; then continue; fi
        emit "$script" "$lno" "SHELL_LINT_CURL_FAIL" "curl invocation missing -f / --fail"
    done < <(non_comment "$script" | grep -nE '\bcurl\b' || true)
}

# Rule 4 — sha256 within 10 lines after curl writes to a file.
check_sha256() {
    local script="$1"
    while IFS=: read -r lno content; do
        # curl -o <path> or curl --output <path>; only fire if the path looks
        # like a real filesystem write (excludes /dev/null and similar).
        if [[ "$content" =~ /dev/null ]]; then continue; fi
        local window
        window=$(sed -n "$((lno+1)),$((lno+10))p" "$script")
        if echo "$window" | grep -qE '(sha256sum[[:space:]]+-c|--checksum)'; then continue; fi
        emit "$script" "$lno" "SHELL_LINT_SHA256" "curl writes to file without sha256 verify within 10 lines"
    done < <(non_comment "$script" | grep -nE 'curl.*([[:space:]]-o[[:space:]]|--output[[:space:]])' || true)
}

# Rule 5 — --key) ↔ --key=*) parity when the flag takes a value.
check_flag_parity() {
    local script="$1"
    declare -A value_flags=()
    declare -A eq_flags=()
    declare -A flag_line=()
    while IFS=: read -r lno content; do
        if [[ "$content" =~ --([a-zA-Z][a-zA-Z0-9_-]*)\)([[:space:]]|$) ]]; then
            local flag="${BASH_REMATCH[1]}"
            # Body of this branch only — terminate scan at the first `;;` so
            # neighboring case-branches' `shift 2` / `$2` don't leak in. Awk
            # (not sed range) so the start line itself is checked for `;;`
            # (sed's `,/p/` skips the start-line match).
            local body
            # Cap at 10 lines INSIDE awk — NOT `awk … | head -10`: the SAME
            # SIGPIPE-under-pipefail class as the deps-rule fix above, and the
            # SECOND trigger of the exit-141 develop breakage (#995 fixed only
            # the deps-rule pipe). Here awk STREAMS a whole script file; on a CI
            # runner a large scanned script (e.g. setup-harness.sh, a 200-line
            # case-branch body with no early `;;`) keeps awk writing after
            # `head -10` closes the pipe → awk SIGPIPE → 141. No pipe → no
            # SIGPIPE; the first ≤10 lines (through the `;;` line) are identical.
            body=$(awk -v start="$lno" 'NR<start{next} {print; n++; if (n>=10 || /;;/) exit}' "$script")
            # Value-taking signal: shift 2 OR $2 reference in the branch body.
            if echo "$body" | grep -qE '(shift[[:space:]]+2|\$2|\$\{2)'; then
                value_flags["$flag"]=1
                flag_line["$flag"]="$lno"
            fi
        fi
        if [[ "$content" =~ --([a-zA-Z][a-zA-Z0-9_-]*)=\*\) ]]; then
            local flag="${BASH_REMATCH[1]}"
            eq_flags["$flag"]=1
            flag_line["${flag}_eq"]="$lno"
        fi
    done < <(non_comment "$script" | grep -nE -- '--[a-zA-Z]' || true)
    for flag in "${!value_flags[@]}"; do
        if [ -z "${eq_flags[$flag]:-}" ]; then
            emit "$script" "${flag_line[$flag]}" "SHELL_LINT_FLAG_PARITY" "--$flag takes a value but has no --$flag=* twin"
        fi
    done
    for flag in "${!eq_flags[@]}"; do
        if [ -z "${value_flags[$flag]:-}" ]; then
            emit "$script" "${flag_line[${flag}_eq]}" "SHELL_LINT_FLAG_PARITY" "--$flag=* exists but no bare --$flag) twin (value-taking)"
        fi
    done
}

PASSED=0
FAILED=0
for script in "${TARGETS[@]}"; do
    before="${#VIOLATIONS[@]}"
    check_deps "$script"
    check_shellcheck "$script"
    check_curl_fail "$script"
    check_sha256 "$script"
    check_flag_parity "$script"
    after="${#VIOLATIONS[@]}"
    if [ "$after" -gt "$before" ]; then
        FAILED=$((FAILED + 1))
    else
        PASSED=$((PASSED + 1))
    fi
done

for v in "${VIOLATIONS[@]}"; do
    echo "$v" >&2
done

echo "Passed: $PASSED  Failed: $FAILED"
[ "$FAILED" -eq 0 ] || exit 1

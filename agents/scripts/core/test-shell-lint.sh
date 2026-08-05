#!/usr/bin/env bash
# test-shell-lint.sh — nine-rule self-review lint for shell scripts.
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
#   6. pipefail SIGPIPE/truncation guard — in a script using `set -o pipefail`,
#      a top-level command-substitution assignment `var=$(... | head ...)` whose
#      pipeline ENDS in a truncating consumer (head / head -n / head -c). head
#      closes the pipe early → the producer gets SIGPIPE (141) → under pipefail
#      the `$(...)` RC is non-zero → with `set -e` the assignment aborts the
#      script (CI-only: msys bash ignores SIGPIPE, so it passes locally). Demand
#      an explicit mitigation (`|| true`, restructure). Closes the
#      `pipefail-masked-pipe-exit-needs-class-guard` backlog entry (the dormant
#      class behind the two #995 inline fixes — deps-rule + flag-parity pipes).
#   7. NUL-byte guard — fail any tracked first-party *.sh containing a NUL byte
#      (\x00). bash truncates a string at a NUL, so an embedded NUL silently
#      drops the rest of the script at execution while the file looks intact in
#      most editors. Catches corruption / mis-encoding (UTF-16, truncated write)
#      at lint time.
#   8. early-break-pipe SIGPIPE guard — generalizes rule 6 beyond `$(… | head)`:
#      under pipefail, a `producer | fn` pipeline where `fn` is a same-script
#      function that reads with `while … read` and `break`s early. The early
#      break closes the pipe → producer SIGPIPE (141) → script aborts (CI-only,
#      msys ignores SIGPIPE). Closes the #1593 follow-up (feed the reader from a
#      temp file). Requires while+read+break in the fn body, so a full-drain
#      reader is not flagged.
#   9. resolve-only python-interpreter picker — a probe that SELECTS among two
#      or more python names (`for c in python3 python`, or an
#      `if command -v python3 … elif command -v python` chain) using only
#      `command -v` / `which` / `type -p`, with no exec-validation of the
#      candidate. On Windows `python3` normally resolves to the Microsoft Store
#      App Execution Alias stub under %LOCALAPPDATA%\Microsoft\WindowsApps\: it
#      is on PATH, so a resolve-only probe picks it, but running it prints an
#      "install from the Microsoft Store" banner and exits non-zero. The picker
#      therefore selects the stub OVER a working later candidate — the failure
#      is silent-wrong (parsed nothing / skipped tests), not a clean not-found.
#      Fix: pair the resolve with an exec check (`"$c" -c ""` or
#      `"$c" --version`) in the same loop/chain. Single-candidate hard-require
#      guards (`command -v python3 || exit 2`) are deliberately NOT flagged —
#      they have nothing to mis-select and fail loudly.
#
# Targets: scripts/dev/*.sh + agents/scripts/{core,project}/*.sh (post-#609
# layout; maxdepth 1, so scripts/dev/local/ is excluded) + scripts/mobile/**
# (recursive — build helpers nest under openssl/) + docs/harness/<h>/hooks/*.sh
# (the PreToolUse/SessionStart guard scripts). Lints itself.
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

# Target set — the scripts this gate lints. Derived before the shellcheck
# preflight so --list-targets resolves even without shellcheck on PATH.
#   --target <path>   lint a single script (bats fixture harness)
#   --list-targets    print the resolved set and exit 0 (scope-coverage test)
# Flat (maxdepth 1): scripts/dev + agents/scripts/{core,project}. Recursive:
# scripts/mobile (build helpers nest under openssl/), agents/scripts/core/lib
# and agents/scripts/project/lint-rules.d (sourced libraries / rule modules —
# live one level down). Per-harness guards: docs/harness/<harness>/hooks/*.sh.
TARGETS=()
if [ "${1:-}" = "--target" ] && [ -n "${2:-}" ]; then
    TARGETS=("$2")
else
    while IFS= read -r f; do TARGETS+=("$f"); done < <(
        {
            find scripts/dev agents/scripts/core agents/scripts/project \
                -maxdepth 1 -type f -name '*.sh'
            if [ -d agents/scripts/core/lib ]; then find agents/scripts/core/lib -type f -name '*.sh'; fi
            if [ -d agents/scripts/project/lint-rules.d ]; then find agents/scripts/project/lint-rules.d -type f -name '*.sh'; fi
            if [ -d scripts/mobile ]; then find scripts/mobile -type f -name '*.sh'; fi
            if [ -d docs/harness ]; then find docs/harness -path '*/hooks/*.sh' -type f; fi
        } | sort -u
    )
fi
if [ "${1:-}" = "--list-targets" ]; then
    printf '%s\n' "${TARGETS[@]}"
    exit 0
fi

if ! command -v shellcheck >/dev/null 2>&1; then
    echo "test-shell-lint: WARN: shellcheck not on PATH; install via scoop (scoop install shellcheck), brew (brew install shellcheck), or apt (apt-get install shellcheck) — see https://github.com/koalaman/shellcheck#installing" >&2
    echo "Passed: 0  Failed: 0"
    exit 0
fi

# 19-entry closed allowlist (see plan § Approach rule 1).
# `git` was grilled-in but dropped during implementation: it's effectively a
# shell builtin in any dev environment (BUILD.md § Prereqs makes it mandatory)
# and preflighting it generated 25 violations across the existing tree with
# near-zero practical value. Captured in plan § Deviations.
ALLOWLIST=(curl gh cmake python python3 jq 7z cppcheck clang-format clang-tidy shellcheck actionlint bats p4 cl.exe clang-cl link.exe cygpath tasklist)

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
        # Here-string, NOT `printf | grep -q`: grep -q exits on the first match
        # and closes the pipe, SIGPIPE-ing printf; under `set -o pipefail` the
        # pipeline then returns non-zero, the `if` reads as "no preflight", and a
        # false SHELL_LINT_DEPS fires on any script whose preflight isn't the
        # last `gh`/tool token (e.g. merge-gates.sh's L117 preflight + a later
        # help-string `gh`). CI-only — msys bash ignores SIGPIPE, so it passed
        # locally while reddening every PR's Shell-lint job.
        if grep -qE "(command -v|which|type -p)[[:space:]]+${tool_escaped}\\b" <<<"$nc"; then
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

# Rule 4 — sha256 within 10 lines after curl writes to a file. Two accepted
# shapes: (a) `sha256sum -c` / `--checksum` against a manifest, and (b) the
# capture-compare idiom `actual=$(sha256sum FILE …)` followed by an equality
# test — both genuinely verify the download. `known-bad-4-no-sha.sh` (curl to
# file, zero sha256sum) still has neither and stays a finding.
check_sha256() {
    local script="$1"
    while IFS=: read -r lno content; do
        # curl -o <path> or curl --output <path>; only fire if the path looks
        # like a real filesystem write (excludes /dev/null and similar).
        if [[ "$content" =~ /dev/null ]]; then continue; fi
        local window
        window=$(sed -n "$((lno+1)),$((lno+10))p" "$script")
        if echo "$window" | grep -qE '(sha256sum[[:space:]]+-c|--checksum|\$\([[:space:]]*sha256sum|`[[:space:]]*sha256sum)'; then continue; fi
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

# Rule 6 — pipefail var=$(... | head ...) SIGPIPE/truncation class guard.
# Only fires when the script actually enables pipefail (otherwise an upstream
# producer's non-zero/SIGPIPE exit is discarded by the shell and the risk is
# absent). The dangerous shape is a top-level COMMAND-SUBSTITUTION assignment
# whose pipeline ENDS in a truncating consumer (`head`, `head -n`, `head -c`):
# head closes the pipe after N lines/bytes, the producer takes SIGPIPE (141),
# and under pipefail the `$(...)` returns 141 — with `set -e` (the common
# companion of pipefail) the plain assignment then aborts the whole script.
# This was a CI-only break twice (#995) because msys bash ignores SIGPIPE, so it
# kept passing locally while reddening every PR. Both prior instances were fixed
# INLINE; this rule guards the class so it can't silently reappear.
#
# False-positive controls (keep noise low):
#   * head must be the FINAL consumer — `… | head -1 | awk …` ends in awk, not
#     head, so the producer is not the thing being SIGPIPE'd by head and the
#     line is NOT flagged (matched by requiring `head[ flags] )` with no further
#     `|` before the `$(` closer).
#   * already-mitigated lines (`… | head …) || true` / `|| :`) are skipped — the
#     author has explicitly accepted/handled the non-zero RC.
check_pipefail_head() {
    local script="$1"
    local nc
    nc=$(non_comment "$script")
    # Gate on pipefail (set -o pipefail / set -euo pipefail / set -eo pipefail…).
    if ! grep -qE '(^|[[:space:]])set[[:space:]]+(-[a-z]*o[[:space:]]+pipefail|-o[[:space:]]+pipefail)' <<<"$nc"; then
        return
    fi
    # Held in a var (not inline in `[[ =~ ]]`): the `[^|)]` bracket trips bash's
    # conditional parser if written literally. Top-level (optionally local/export)
    # command-substitution assignment whose `$(...)` ends in a truncating head —
    # `[^|)]*` between head's args and the `)` keeps head the LAST pipeline stage
    # (a later `|` means head is not the consumer being SIGPIPE'd, so no flag).
    local re='^[[:space:]]*(local[[:space:]]+|export[[:space:]]+)?[A-Za-z_][A-Za-z0-9_]*=\$\(.*\|[[:space:]]*head([[:space:]]+[^|)]*)?\)'
    while IFS=: read -r lno content; do
        # Already-handled non-zero RC — author opted in.
        case "$content" in
            *'|| true'*|*'|| :'*) continue ;;
        esac
        if [[ "$content" =~ $re ]]; then
            emit "$script" "$lno" "SHELL_LINT_PIPEFAIL_HEAD" "var=\$(... | head) under pipefail: head can SIGPIPE the producer (141) and abort the assignment; add '|| true' or restructure"
        fi
    done < <(printf '%s\n' "$nc" | grep -nE '=\$\(.*\|[[:space:]]*head([[:space:]]|\))' || true)
}

# Rule 8 — producer | early-break reader under pipefail (the general SIGPIPE-141
# class Rule 6 only catches for the `$(... | head)` shape). This is the exact trap
# that reddened CI on #1593: a producer piped into a shell FUNCTION whose body
# reads with `while ... read` and `break`s early. The early break closes the read
# end of the pipe before the producer finishes writing; the producer takes SIGPIPE
# (128+13=141); under `set -o pipefail` the pipeline returns 141 and (with the
# common `set -e` companion) the whole script aborts with a bare
# `Process completed with exit code 141` instead of the intended message. msys
# bash ignores SIGPIPE, so — like Rule 6 — this passes locally and only reds in CI.
# The safe shape is a temp file (`producer >tmp; reader <tmp`), which keeps the
# producer's own exit status observable; a `reader < <(producer)` process
# substitution stops the SIGPIPE but drops that exit status (the mitigation's own
# trap on #1593's first pass), so it is NOT recommended and NOT treated as safe.
#
# Two-pass, conservative:
#   (1) collect same-script functions that are early-break readers — a function
#       whose body contains a `while`, a `read`, AND a `break`. All three are
#       required, so a full-drain `while read; do … done` (no early break, no
#       SIGPIPE) is never flagged.
#   (2) flag any top-level `producer | <that-fn>` pipeline. `|| true` / `|| :`
#       opt-outs are honoured (author accepted the RC), matching Rule 6.
# Only fires under pipefail (without it the producer's SIGPIPE exit is discarded).
check_early_break_pipe() {
    local script="$1"
    local nc
    nc=$(non_comment "$script")
    if ! grep -qE '(^|[[:space:]])set[[:space:]]+(-[a-z]*o[[:space:]]+pipefail|-o[[:space:]]+pipefail)' <<<"$nc"; then
        return
    fi
    # Pass 1 — early-break reader function names. Regexes are inlined against $0
    # (POSIX-awk boundary classes, no \b) — NOT via a helper taking `/re/` as an
    # arg, which awk evaluates against $0 first and passes the 0/1 result, not the
    # pattern. `{`/`}` are counted with gsub for a crude brace-depth so nested
    # blocks don't end the function early.
    local readers
    readers=$(printf '%s\n' "$nc" | awk '
        /^[[:space:]]*[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\(\)[[:space:]]*\{/ {
            name=$0; sub(/[[:space:]]*\(\).*/, "", name); sub(/^[[:space:]]*/, "", name)
            infn=1; depth=1; hasWhile=0; hasRead=0; hasBreak=0; next
        }
        infn {
            opens=gsub(/\{/, "{"); closes=gsub(/\}/, "}"); depth += opens - closes
            if ($0 ~ /(^|[^A-Za-z0-9_])while([^A-Za-z0-9_]|$)/) hasWhile=1
            if ($0 ~ /(^|[^A-Za-z0-9_])read([^A-Za-z0-9_]|$)/)  hasRead=1
            if ($0 ~ /(^|[^A-Za-z0-9_])break([^A-Za-z0-9_]|$)/) hasBreak=1
            if (depth<=0) { if (hasWhile && hasRead && hasBreak) print name; infn=0 }
        }
    ')
    if [ -z "$readers" ]; then return; fi
    # Pass 2 — pipelines feeding one of those readers. The fn name may be followed
    # by `)` (inside `$(producer | fn)`), whitespace, or end-of-line — so the
    # trailing boundary is any non-identifier char, not just whitespace/EOL.
    local fn re
    while IFS= read -r fn; do
        [ -n "$fn" ] || continue
        re="\\|[[:space:]]*${fn}([^A-Za-z0-9_]|\$)"
        while IFS=: read -r lno content; do
            case "$content" in
                *'|| true'*|*'|| :'*) continue ;;
            esac
            emit "$script" "$lno" "SHELL_LINT_EARLY_BREAK_PIPE" \
                "producer | ${fn} under pipefail: ${fn} reads with an early break, so the producer can SIGPIPE (141) and abort the script; write the producer to a temp file and feed ${fn} from it"
        done < <(printf '%s\n' "$nc" | grep -nE "$re" || true)
    done <<<"$readers"
}

# Rule 7 — NUL-byte guard. A tracked first-party `*.sh` is a text file; an embedded NUL
# (`\x00`) means it was corrupted / partially-binary-written / mis-encoded (UTF-16 with BOM,
# a truncated write, an accidental binary paste). bash treats a NUL as a string terminator, so
# a NUL silently truncates the script at the byte — the tail never executes, yet the file looks
# fine in most editors. Fail any script carrying a NUL so the corruption is caught at lint time
# rather than as a baffling "rest of the script didn't run" at execution.
check_nul_byte() {
    local script="$1"
    # LC_ALL=C + -a treat the file as binary so grep doesn't bail on the NUL; -P \x00 matches
    # the NUL byte. -q: presence-only.
    if LC_ALL=C grep -qaP '\x00' "$script" 2>/dev/null; then
        emit "$script" "1" "SHELL_LINT_NUL_BYTE" "script contains a NUL byte (\\x00) — corrupted / mis-encoded; bash truncates at the NUL"
    fi
}

# Rule 9 — resolve-only python-interpreter picker (Windows Store-alias stub).
# Fires ONLY on the picker shape: a probe choosing among >= 2 python candidate
# names with no exec-validation. That is the silent-wrong class — the stub wins
# the selection over a working later candidate, so the script proceeds with an
# interpreter that cannot run. A single-candidate `command -v python3 || exit 2`
# guard is not flagged: it selects nothing and fails loudly.
#
# Two shapes, both requiring >= 2 DISTINCT python names so ordinary one-tool
# preflights stay quiet:
#   (a) candidate loop — `for c in python3 python py; do … command -v "$c" …`
#   (b) literal chain  — `if command -v python3 … elif command -v python …`,
#       or a one-line `$(command -v python || command -v python3)`.
# An exec-validation anywhere in the shape's window clears it. The window check
# is deliberately generic (any `<cmd> -c` / `--version` / `-V`) rather than
# anchored to the loop variable: the canonical repo form validates a DIFFERENT
# variable than it probes (`_p="$(command -v "$_c")"` … `"$_p" -c ""`), and a
# stray unrelated `-c` in the window only makes this rule quieter, never louder.
#
# Here-strings throughout, never `printf | grep -q`: grep -q exits on the first
# match and SIGPIPEs the producer, which under this script's own `set -o
# pipefail` reads as "no match" — the exact CI-only trap rules 6 and 8 guard.
PY_NAME_RE='(python[0-9]*(\.[0-9]+)?|py)'
PY_EXEC_RE='[[:space:]](-c|--version|-V)([[:space:]]|$)'
check_py_probe() {
    local script="$1"
    local nc
    nc=$(non_comment "$script")
    local probe='(command -v|which|type -p)'

    # -- shape (a): candidate-loop picker.
    local lno content var list ncand win hits first rel
    while IFS=: read -r lno content; do
        var=$(sed -E 's/.*[[:space:]]?for[[:space:]]+([A-Za-z_][A-Za-z0-9_]*)[[:space:]]+in[[:space:]].*/\1/' <<<"$content")
        list=$(sed -E 's/.*[[:space:]]?for[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]+in[[:space:]]//' <<<"$content")
        # Distinct python names in the candidate list. Split to one word per
        # line and match each WHOLE word: a single `grep -oE` over the list
        # cannot do this, because -o consumes the delimiting space and so skips
        # every other adjacent candidate (`python3 python py` counts as 1).
        # Whole-word match also keeps a `*.py` glob or `python3-config` out.
        ncand=$(tr -s '[:space:]' '\n' <<<"$list" | sed -E 's/[;&|)].*$//' \
            | grep -xE "$PY_NAME_RE" | sort -u | grep -c . || true)
        [ "${ncand:-0}" -ge 2 ] || continue
        # Loop body window — the probe and any exec-validation live here.
        win=$(sed -n "${lno},$((lno + 8))p" <<<"$nc")
        hits=$(grep -nE "${probe}[[:space:]]+\"?\\\$\{?${var}\}?" <<<"$win" || true)
        [ -n "$hits" ] || continue
        if grep -qE "$PY_EXEC_RE" <<<"$win"; then continue; fi
        first="${hits%%$'\n'*}"
        rel="${first%%:*}"
        emit "$script" "$((lno + rel - 1))" "SHELL_LINT_PY_PROBE" \
            "python candidate loop resolves with 'command -v' but never runs the candidate: on Windows python3 is the Microsoft Store alias stub (on PATH, exits non-zero), so this picks a non-working interpreter — add an exec check (\"\$$var\" -c \"\")"
    done < <(grep -nE '(^|[[:space:]])for[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]+in[[:space:]]' <<<"$nc" || true)

    # -- shape (b): literal multi-candidate chain. Cluster probe lines that sit
    # within 3 lines of each other; a cluster naming >= 2 distinct python
    # interpreters is a picker.
    local -a cl_lnos=() cl_toks=()
    local toks prev_lno=-99 c_start=0 c_toks=""
    _flush_cluster() {
        local start="$1" last="$2" t="$3" n w
        [ -n "$t" ] || return 0
        n=$(tr ' ' '\n' <<<"$t" | sort -u | grep -c . || true)
        [ "${n:-0}" -ge 2 ] || return 0
        w=$(sed -n "${start},$((last + 2))p" <<<"$nc")
        if grep -qE "$PY_EXEC_RE" <<<"$w"; then return 0; fi
        emit "$script" "$start" "SHELL_LINT_PY_PROBE" \
            "python interpreter chosen among $n candidates by 'command -v' alone, with no exec check: on Windows python3 is the Microsoft Store alias stub (on PATH, exits non-zero), so the chain can select a non-working interpreter — add an exec check (\"\$PY\" -c \"\")"
    }
    while IFS=: read -r lno content; do
        toks=$(grep -oE "${probe}[[:space:]]+\"?${PY_NAME_RE}" <<<"$content" \
            | sed -E "s/^${probe}[[:space:]]+\"?//" | sort -u | tr '\n' ' ')
        [ -n "${toks// /}" ] || continue
        cl_lnos+=("$lno")
        cl_toks+=("$toks")
    done < <(grep -nE "${probe}[[:space:]]+\"?${PY_NAME_RE}\"?([[:space:]]|\)|;|\"|$)" <<<"$nc" || true)
    local i last_lno=0
    for i in "${!cl_lnos[@]}"; do
        lno="${cl_lnos[$i]}"
        if [ "$prev_lno" -ge 0 ] && [ $((lno - prev_lno)) -le 3 ]; then
            c_toks="$c_toks ${cl_toks[$i]}"
        else
            _flush_cluster "$c_start" "$last_lno" "$c_toks"
            c_start="$lno"
            c_toks="${cl_toks[$i]}"
        fi
        prev_lno="$lno"
        last_lno="$lno"
    done
    _flush_cluster "$c_start" "$last_lno" "$c_toks"
    unset -f _flush_cluster
}

PASSED=0
FAILED=0
for script in "${TARGETS[@]}"; do
    before="${#VIOLATIONS[@]}"
    # NUL guard runs FIRST and short-circuits: a NUL-bearing file is binary/corrupt, so the
    # text-oriented rules below would only emit "ignored null byte" command-substitution
    # warnings and unreliable matches. One NUL finding is enough.
    check_nul_byte "$script"
    if [ "${#VIOLATIONS[@]}" -gt "$before" ]; then
        FAILED=$((FAILED + 1))
        continue
    fi
    check_deps "$script"
    check_shellcheck "$script"
    check_curl_fail "$script"
    check_sha256 "$script"
    check_flag_parity "$script"
    check_pipefail_head "$script"
    check_early_break_pipe "$script"
    check_py_probe "$script"
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

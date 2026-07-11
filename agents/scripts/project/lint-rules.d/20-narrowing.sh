#!/usr/bin/env bash
# 20-narrowing.sh — clang-tidy narrowing-conversions scanner (opt-in) (sourced by test-lint-rules.sh, not run directly).

# clang-tidy narrowing-conversions over strict-zone .cpp TUs. Requires a
# compile_commands.json; warn-skips otherwise (matches the existing missing-
# toolchain fallback for cppcheck/shellcheck).
CLANG_TIDY="${CLANG_TIDY:-clang-tidy}"
find_compile_db() {
    # Prefer clang dbs — clang-tidy can't parse the MSVC db (PCH + stdlib paths).
    local d
    for d in build/ninja-iter-clang build/ninja-debug-clang build/ninja-iter-msvc build/ninja-test-msvc; do
        [ -f "$d/compile_commands.json" ] && { echo "$d"; return 0; }
    done
    return 1
}

scan_narrowing() {
    # $@ = strict-zone .cpp files. Emits narrowing-conversions triples.
    #
    # OPT-IN (SMATCHET_LINT_NARROWING=1). Default-off because clang-tidy needs a
    # *clang* compile_commands.json: against the MSVC db it errors on the MSVC
    # PCH ("not a valid PCH file") and can't resolve <string> (no clang stdlib
    # paths), yielding only clang-diagnostic-error noise. Until a PCH-free clang
    # db is provisioned in CI this rule is catalogued-only, never gated.
    # See docs/plans/shipped/high-integrity-cpp-enforcement.md § Deviations.
    if [ "${SMATCHET_LINT_NARROWING:-0}" != "1" ]; then
        echo "test-lint-rules: INFO: narrowing-conversions opt-in (SMATCHET_LINT_NARROWING=1 + clang db); skipped" >&2
        return 0
    fi
    if ! command -v "$CLANG_TIDY" >/dev/null 2>&1; then
        echo "test-lint-rules: WARN: clang-tidy not on PATH; narrowing-conversions skipped" >&2
        return 0
    fi
    local db
    if ! db="$(find_compile_db)"; then
        echo "test-lint-rules: WARN: no compile_commands.json (configure a build); narrowing-conversions skipped" >&2
        return 0
    fi

    # Collect the .cpp TUs to scan (the .h/.hpp the caller passes are not compile
    # units in the db; clang-tidy -p resolves only entries present in the db).
    local f cpp_files=()
    for f in "$@"; do
        case "$f" in *.cpp) cpp_files+=("$f") ;; *) ;; esac
    done
    [ "${#cpp_files[@]}" -eq 0 ] && return 0

    # Parallel clang-tidy fan-out. The old serial `for` loop paid clang-tidy's
    # ~5 s startup+parse once per TU (~100 strict TUs = several minutes; the
    # develop `high-integrity-narrowing` job paid it every merge — tooling.md P2).
    # Fan the TUs out across $jobs workers with `xargs -P`, aggregate raw output,
    # then apply the grep + sed-normalise + first-party/ThirdParty filter ONCE
    # below. Identical findings, several-fold faster wall time.
    #
    # Job count: SMATCHET_LINT_NARROWING_JOBS override > nproc > 4 fallback.
    local jobs="${SMATCHET_LINT_NARROWING_JOBS:-}"
    if [ -z "$jobs" ]; then
        jobs="$(nproc 2>/dev/null || echo 4)"
    fi
    case "$jobs" in ''|*[!0-9]*) jobs=4 ;; esac
    [ "$jobs" -lt 1 ] && jobs=1

    # Raw aggregate of every worker's clang-tidy output. A per-TU clean run that
    # emits no narrowing line is fine — the grep filter below drops the noise.
    # `|| true` on the inner clang-tidy keeps a non-zero per-TU rc (e.g. a
    # clang-diagnostic-error on an unrelated header) from aborting xargs.
    local raw; raw="$(mktemp 2>/dev/null || echo "${TMPDIR:-/tmp}/scan_narrowing.$$")"
    # shellcheck disable=SC2016  # $CLANG_TIDY / $db expand in the child sh, by design.
    printf '%s\0' "${cpp_files[@]}" \
        | CT="$CLANG_TIDY" DB="$db" xargs -0 -P "$jobs" -I '{}' \
            sh -c '"$CT" -p "$DB" --checks="-*,cppcoreguidelines-narrowing-conversions" --quiet "$1" 2>/dev/null || true' _ '{}' \
        > "$raw" 2>/dev/null || true

    # clang-tidy lines: <file>:<line>:<col>: warning: ... [cppcoreguidelines-narrowing-conversions]
    # `grep || true`: an aggregate with no narrowing match must NOT make this pipe
    # exit non-zero — under the caller's `set -e -o pipefail` that would abort the
    # whole strict scan (false PASS: catalog computed as empty).
    { grep -E 'narrowing-conversions\]$' "$raw" || true; } | while IFS= read -r w; do
        # Greedy path match so a Windows drive-letter colon (C:\…) is not
        # truncated the way `cut -d:` would; then backslash->slash and strip to
        # a repo-relative Source/… path so the triple basename + hash match the
        # line-rule shape (and stay path-portable across head/base).
        local loc; loc="$(printf '%s' "$w" | sed -E 's@^(.*):([0-9]+):[0-9]+: warning:.*@\1:\2@; s@\\@/@g; s@^.*/(Source/)@\1@')"
        # First-party strict sources only: drop anything that did not normalise
        # under Source/ (FetchContent / system headers) and any vendored
        # ThirdParty header — e.g. stb_image.h compiled into a strict TU via
        # STB_IMAGE_IMPLEMENTATION is not first-party narrowing debt.
        case "$loc" in Source/*) ;; *) continue ;; esac
        case "$loc" in *ThirdParty*) continue ;; esac
        printf 'narrowing-conversions\t%s\t%s\n' "$loc" "$w"
    done
    rm -f "$raw" 2>/dev/null || true
    return 0
}

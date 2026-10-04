#!/usr/bin/env bash
# test-mirrored-paths.sh — every file listed in docs/mirrored-paths.txt must exist
# in both the host and the agent layer, and the two copies must be byte-identical.
#
# WHY THIS EXISTS (plan agent-surface-extraction-repo, Phase B row 8g)
#   A few files are deliberately dual-homed: the host needs them before the layer
#   is checked out (the build sources scripts/dev/project-config.sh), and the layer
#   needs them to run its own CI standalone. Two copies drift unless something
#   compares them. This is that comparison. It loops over a LIST rather than naming
#   one file, and a listed path missing from either tree is a failure, so a mirror
#   that vanishes reds instead of silently passing.
#
#   Before the flip both roots are this checkout and the gate proves only that the
#   list names real files. After it, AGENT_LAYER_ROOT is the agent-layer/ submodule
#   and the comparison is real.
#
# USAGE
#   bash scripts/dev/test-mirrored-paths.sh             # compare PROJECT_ROOT and AGENT_LAYER_ROOT
#   bash scripts/dev/test-mirrored-paths.sh --selftest  # prove the gate reds on drift
#
# ENV  MIRRORED_PATHS_FILE  the list (default: $PROJECT_ROOT/docs/mirrored-paths.txt)
#
# EXIT 0 every listed path is identical in both trees; 1 a path differs or is
#      missing, or the list names no path; 2 the roots cannot be resolved.
#
# selftest: asserts-failure
set -uo pipefail

_tmp_self="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
_tmp_root="$(cd "$(dirname "$_tmp_self")/../.." && pwd)"

# check_mirrors <project_root> <layer_root> <list> — prints one line per path and a
# Passed/Failed summary; returns 1 on any failure or when no path was checked.
check_mirrors() {
    local host="$1" layer="$2" list="$3" path passed=0 failed=0
    if [ ! -f "$list" ]; then
        printf 'FAIL  mirror list not found: %s\n' "$list"
        printf 'Passed: 0  Failed: 1\n'
        return 1
    fi
    while IFS= read -r path || [ -n "$path" ]; do
        path="${path%$'\r'}"
        case "$path" in ''|'#'*) continue ;; esac
        if [ ! -f "$host/$path" ]; then
            printf 'FAIL  %s: missing from the host (%s)\n' "$path" "$host"
            failed=$((failed + 1))
        elif [ ! -f "$layer/$path" ]; then
            printf 'FAIL  %s: missing from the agent layer (%s)\n' "$path" "$layer"
            failed=$((failed + 1))
        elif ! cmp -s "$host/$path" "$layer/$path"; then
            printf 'FAIL  %s: the host copy differs from the agent layer'"'"'s (the layer copy is canonical)\n' "$path"
            failed=$((failed + 1))
        else
            passed=$((passed + 1))
        fi
    done < "$list"
    if [ "$passed" -eq 0 ] && [ "$failed" -eq 0 ]; then
        printf 'FAIL  %s names no path — refusing to report a vacuous pass\n' "$list"
        failed=1
    fi
    printf 'Passed: %d  Failed: %d\n' "$passed" "$failed"
    [ "$failed" -eq 0 ]
}

selftest() {
    local tmp rc=0
    tmp="$(mktemp -d "${TMPDIR:-/tmp}/test-mirrored-paths.XXXXXX")" || return 2
    mkdir -p "$tmp/host/scripts" "$tmp/layer/scripts"
    printf 'a\n' > "$tmp/host/scripts/x.sh"
    printf 'a\n' > "$tmp/layer/scripts/x.sh"
    printf '# list\n\nscripts/x.sh\n' > "$tmp/list"
    check_mirrors "$tmp/host" "$tmp/layer" "$tmp/list" >/dev/null \
        || { echo "selftest: identical copies were not accepted"; rc=1; }
    printf 'b\n' > "$tmp/layer/scripts/x.sh"
    check_mirrors "$tmp/host" "$tmp/layer" "$tmp/list" >/dev/null \
        && { echo "selftest: differing copies were accepted"; rc=1; }
    rm -f "$tmp/layer/scripts/x.sh"
    check_mirrors "$tmp/host" "$tmp/layer" "$tmp/list" >/dev/null \
        && { echo "selftest: a path missing from the layer was accepted"; rc=1; }
    printf '# nothing listed\n' > "$tmp/list"
    check_mirrors "$tmp/host" "$tmp/layer" "$tmp/list" >/dev/null \
        && { echo "selftest: an empty list was accepted"; rc=1; }
    check_mirrors "$tmp/host" "$tmp/layer" "$tmp/no-such-list" >/dev/null \
        && { echo "selftest: a missing list was accepted"; rc=1; }
    rm -rf "$tmp"
    if [ "$rc" -eq 0 ]; then
        echo "test-mirrored-paths: selftest PASS (accepts identical copies; reds on drift, a missing copy, an empty list, a missing list)"
    fi
    return "$rc"
}

case "${1:-}" in
    --selftest) selftest; exit $? ;;
    "") ;;
    *) echo "usage: $0 [--selftest]" >&2; exit 2 ;;
esac

# shellcheck source=scripts/dev/project-config.sh
PC_ROOTS_ONLY=1 . "$_tmp_root/scripts/dev/project-config.sh" \
    || { echo "test-mirrored-paths: cannot resolve the project and agent-layer roots" >&2; exit 2; }
check_mirrors "$PROJECT_ROOT" "$AGENT_LAYER_ROOT" "${MIRRORED_PATHS_FILE:-$PROJECT_ROOT/docs/mirrored-paths.txt}"

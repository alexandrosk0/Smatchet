#!/usr/bin/env bash
# test-no-new-ps1.sh — rule `no-new-ps1`: the tracked PowerShell files are
# exactly the Windows-only shims listed in the agent layer's
# docs/harness/SETUP.md, and each one stays safe for Windows PowerShell 5.1 to
# load.
# ----------------------------------------------------------------------------
# Class of bug this kills (tooling 2026-08-05, "No new .ps1" rule is ungated):
# the kill-PowerShell plan ported every helper to bash and left five shims that
# call Windows APIs with no bash equivalent. SETUP.md § Windows-only shims says
# that list is complete and that a new .ps1 anywhere is a regression, and that
# each kept shim is ASCII-only / no BOM / LF with a marker comment, but nothing
# checked any of it, so the toolchain could quietly regrow.
#
# Layout: SETUP.md and the shims are agent-layer content, so the checkout is
# checked as a whole — `git ls-files --recurse-submodules '*.ps1'` (the host
# tree plus the agent-layer/ mount) against the table in the layer's SETUP.md.
# The layer is the root's agent-layer/ mount once it is populated (it holds the
# layer's scripts/dev/project-config.sh, the marker every host script resolves
# it by), else the root itself (a fixture tree, or the standalone layer).
#
# Checks (on the tracked .ps1 files against the § Windows-only shims table):
#   * the tracked .ps1 basenames equal the table's names — an extra file, a
#     second copy of a listed name, or a listed file that is gone all FAIL;
#   * every listed file carries the `# Last remaining PowerShell file` marker;
#   * every tracked .ps1 is ASCII-only, has no UTF-8 BOM and no CR (PS 5.1
#     decodes a BOM-less file as ANSI, so one em-dash in a string literal kills
#     the script).
# Escape hatch: an unlisted .ps1 that genuinely must be PowerShell carries a
#   SMATCHET_DEVIATION(rule=no-new-ps1; reason=…; owner=…; revisit=…)
# line; it is then exempt from the set check (still encoding-checked). Prefer
# adding the file to the SETUP.md table with its reason instead.
#
# Fail-CLOSED: no SETUP.md (an agent-layer/ mount that is not checked out
# included), no § Windows-only shims table, or a table that parses to zero
# names is an infra error (exit 2), never a silent pass.
#
# Modes:
#   (no args) | --check   check the repo (or $NO_NEW_PS1_ROOT, a git work tree).
#   --selftest            build fixture trees under a temp root and assert each
#                         violation FAILs and the clean tree / deviation PASS.
#
# Exit: 0 clean · 1 violation · 2 infra error.
# selftest: asserts-failure
# ----------------------------------------------------------------------------
set -uo pipefail

SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
SETUP_DOC="docs/harness/SETUP.md"
SHIMS_HEADING="## Windows-only shims"
MARKER="# Last remaining PowerShell file"
DEVIATION_RE='SMATCHET_DEVIATION\(rule=no-new-ps1;[^)]*reason='

# allowed_names <setup-md> — the backticked first-column .ps1 names of the table
# under SHIMS_HEADING, one per line, up to the next `## ` heading.
allowed_names() {
    # shellcheck disable=SC2016  # the backticks are literal markdown, not expansions
    awk -v h="$SHIMS_HEADING" '$0 == h {inside = 1; next} /^## / {inside = 0} inside' "$1" \
        | sed -n 's/^|[[:space:]]*`\([^`/]*\.ps1\)`[[:space:]]*|.*/\1/p'
}

# encoding_problems <file> — print each PS 5.1 hazard the file has (empty = ok).
encoding_problems() {
    local f="$1"
    [ "$(head -c 3 "$f" | od -An -tx1 | tr -d ' \n')" = "efbbbf" ] && echo "UTF-8 BOM"
    LC_ALL=C grep -q $'\r' "$f" && echo "CR line endings"
    LC_ALL=C grep -q '[^[:print:][:space:]]' "$f" && echo "non-ASCII bytes"
    return 0
}

# layer_root_for <root> — the tree holding docs/harness/SETUP.md: the root's
# agent-layer/ mount once it holds the layer, else the root itself.
layer_root_for() {
    if [ -f "$1/agent-layer/scripts/dev/project-config.sh" ]; then
        printf '%s\n' "$1/agent-layer"
    else
        printf '%s\n' "$1"
    fi
}

# check_root <root> — run every check on <root> (a git work tree); 0/1/2.
check_root() {
    local root="$1" layer doc allowed tracked name path bad=0 problem
    layer="$(layer_root_for "$root")"
    doc="$layer/$SETUP_DOC"
    if [ "$layer" != "$root" ]; then SETUP_DOC="${layer#"$root"/}/$SETUP_DOC"; fi
    [ -f "$doc" ] || { echo "test-no-new-ps1: $SETUP_DOC not found under $root (an agent-layer/ mount not checked out? git submodule update --init agent-layer)" >&2; return 2; }
    allowed="$(allowed_names "$doc" | sort)"
    if [ -z "$allowed" ]; then
        echo "test-no-new-ps1: no .ps1 rows parsed from the '$SHIMS_HEADING' table in $SETUP_DOC — fail-closed" >&2
        return 2
    fi
    tracked="$(git -C "$root" ls-files --recurse-submodules '*.ps1' 2>/dev/null)" || {
        echo "test-no-new-ps1: git ls-files failed under $root" >&2; return 2; }

    # Set check, on basenames: the table names files, not paths. A deviated
    # unlisted file is left out of the comparison.
    local counted=""
    while IFS= read -r path; do
        [ -n "$path" ] || continue
        name="${path##*/}"
        if ! printf '%s\n' "$allowed" | grep -qxF "$name" \
            && grep -qE "$DEVIATION_RE" "$root/$path" 2>/dev/null; then
            echo "  note  $path is not in the table but carries a no-new-ps1 deviation"
            continue
        fi
        counted="${counted:+$counted$'\n'}$name"
    done <<< "$tracked"
    counted="$(printf '%s\n' "$counted" | sed '/^$/d' | sort)"
    while IFS= read -r name; do
        [ -n "$name" ] || continue
        echo "  FAIL  new PowerShell file: $(printf '%s\n' "$tracked" | grep -E "(^|/)${name//./\\.}\$" | tr '\n' ' ')— port it to bash (or Python), or list it in $SETUP_DOC § Windows-only shims" >&2
        bad=1
    done < <(comm -13 <(printf '%s\n' "$allowed" | sort -u) <(printf '%s\n' "$counted" | sort -u))
    while IFS= read -r name; do
        [ -n "$name" ] || continue
        echo "  FAIL  $name appears more than once — the table allows one copy" >&2
        bad=1
    done < <(printf '%s\n' "$counted" | uniq -d)
    while IFS= read -r name; do
        [ -n "$name" ] || continue
        echo "  FAIL  $SETUP_DOC lists $name but no such file is tracked — drop the row or restore the file" >&2
        bad=1
    done < <(comm -23 <(printf '%s\n' "$allowed" | sort -u) <(printf '%s\n' "$counted" | sort -u))

    # Per-file checks.
    while IFS= read -r path; do
        [ -n "$path" ] || continue
        [ -f "$root/$path" ] || continue
        if printf '%s\n' "$allowed" | grep -qxF "${path##*/}" \
            && ! grep -qF "$MARKER" "$root/$path"; then
            echo "  FAIL  $path lacks the '$MARKER' marker comment" >&2
            bad=1
        fi
        while IFS= read -r problem; do
            [ -n "$problem" ] || continue
            echo "  FAIL  $path has $problem — keep it ASCII-only, no BOM, LF (Windows PowerShell 5.1)" >&2
            bad=1
        done < <(encoding_problems "$root/$path")
    done <<< "$tracked"
    return "$bad"
}

if [ "${1:-}" = "--selftest" ]; then
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    fail=0
    # <dir> [<layer-dir>] — a clean tree: two listed shims, each well-formed.
    # <layer-dir> (relative) puts SETUP.md + the shims under a populated
    # agent-layer-style mount instead of at the root.
    fixture() {
        local L="$1${2:+/$2}"
        mkdir -p "$L/docs/harness" "$L/tools"
        [ -n "${2:-}" ] && mkdir -p "$L/scripts/dev" && : > "$L/scripts/dev/project-config.sh"
        # shellcheck disable=SC2016  # the backticks are literal markdown
        printf '# Setup\n\n%s\n\n| File | Why |\n|---|---|\n| `a.ps1` | x |\n| `b.ps1` | y |\n\n## Next\n\n| `c.ps1` | not in the table |\n' \
            "$SHIMS_HEADING" > "$L/docs/harness/SETUP.md"
        printf '#Requires -Version 5.1\n%s - see SETUP.md.\nWrite-Host ok\n' "$MARKER" > "$L/tools/a.ps1"
        printf '%s - see SETUP.md.\nWrite-Host ok\n' "$MARKER" > "$L/tools/b.ps1"
        git -C "$1" init -q
        git -C "$1" add -A
    }
    # Asserts the REASON as well as the exit code: a failure for some other
    # cause (a broken fixture) must not pass for the case under test.
    expect() {  # <want-rc> <label> <dir> [<output-fragment>]
        local rc=0 out
        out="$(NO_NEW_PS1_ROOT="$3" bash "$SELF" 2>&1)" || rc=$?
        if [ "$rc" -ne "$1" ]; then
            echo "  FAIL [want $1 got $rc] $2"; fail=1
        elif [ -n "${4:-}" ] && [[ "$out" != *"$4"* ]]; then
            echo "  FAIL [$1 for the wrong reason, wanted '$4'] $2"; fail=1
        else
            echo "  ok   [$1] $2"
        fi
    }
    n=0
    # Sets $d to a fresh fixture. Not called in $(...): the counter must persist.
    new_case() { n=$((n + 1)); d="$tmp/$n"; fixture "$d" "${1:-}"; }

    new_case; expect 0 "clean tree passes" "$d" "PASS"
    new_case; printf '%s\nWrite-Host new\n' "$MARKER" > "$d/tools/c.ps1"; git -C "$d" add -A
    expect 1 "an unlisted .ps1 (named only OUTSIDE the table section) fails" "$d" "new PowerShell file: tools/c.ps1"
    new_case; mkdir -p "$d/other"; cp "$d/tools/a.ps1" "$d/other/a.ps1"; git -C "$d" add -A
    expect 1 "a second copy of a listed name fails" "$d" "a.ps1 appears more than once"
    new_case; git -C "$d" rm -q --cached tools/b.ps1
    expect 1 "a listed file that is not tracked fails" "$d" "lists b.ps1 but no such file is tracked"
    new_case; printf 'Write-Host no-marker\n' > "$d/tools/a.ps1"
    expect 1 "a listed file without the marker fails" "$d" "tools/a.ps1 lacks the"
    new_case; printf '\xef\xbb\xbf%s\n' "$MARKER" > "$d/tools/a.ps1"
    expect 1 "a UTF-8 BOM fails" "$d" "has UTF-8 BOM"
    new_case; printf '%s\r\nWrite-Host crlf\r\n' "$MARKER" > "$d/tools/a.ps1"
    expect 1 "CRLF line endings fail" "$d" "has CR line endings"
    new_case; printf '%s\nWrite-Host "a \xe2\x80\x94 b"\n' "$MARKER" > "$d/tools/a.ps1"
    expect 1 "a non-ASCII em-dash fails" "$d" "has non-ASCII bytes"
    new_case; printf '# SMATCHET_DEVIATION(rule=no-new-ps1; reason=Windows-only API; owner=x; revisit=2099-01-01)\n' > "$d/tools/c.ps1"
    git -C "$d" add -A
    expect 0 "an unlisted .ps1 with a no-new-ps1 deviation passes" "$d" "carries a no-new-ps1 deviation"
    new_case; printf '# Setup\n\nno table here\n' > "$d/docs/harness/SETUP.md"
    expect 2 "a SETUP.md without the shims table is an infra error" "$d" "fail-closed"
    new_case agent-layer; expect 0 "a populated agent-layer/ mount supplies SETUP.md + the shims" "$d" "PASS"
    new_case agent-layer; printf '%s\nWrite-Host new\n' "$MARKER" > "$d/host-new.ps1"; git -C "$d" add -A
    expect 1 "a host .ps1 beside a mounted layer fails" "$d" "new PowerShell file: host-new.ps1"
    new_case agent-layer; rm -rf "$d/agent-layer/docs"
    expect 2 "a mount without SETUP.md is an infra error" "$d" "agent-layer/docs/harness/SETUP.md not found"

    [ "$fail" -eq 0 ] && { echo "test-no-new-ps1 --selftest: PASS"; echo "Passed: 1  Failed: 0"; exit 0; }
    echo "test-no-new-ps1 --selftest: FAIL"; echo "Passed: 0  Failed: 1"; exit 1
fi

case "${1:---check}" in
    --check) ;;
    *) echo "usage: $0 [--check] | --selftest" >&2; exit 2 ;;
esac

ROOT="${NO_NEW_PS1_ROOT:-$(git rev-parse --show-toplevel 2>/dev/null)}" || {
    echo "test-no-new-ps1: not inside a git work tree" >&2; exit 2; }
[ -n "$ROOT" ] || { echo "test-no-new-ps1: not inside a git work tree" >&2; exit 2; }

check_root "$ROOT"
rc=$?
case "$rc" in
    0) echo "test-no-new-ps1: PASS — tracked .ps1 files match $SETUP_DOC § Windows-only shims; all marked, ASCII, no BOM, LF."
       echo "Passed: 1  Failed: 0"; exit 0 ;;
    1) echo "test-no-new-ps1: FAIL — see above (rule no-new-ps1)." >&2
       echo "Passed: 0  Failed: 1"; exit 1 ;;
    *) echo "Passed: 0  Failed: 1"; exit 2 ;;
esac

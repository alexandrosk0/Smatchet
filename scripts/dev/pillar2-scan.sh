#!/usr/bin/env bash
# scripts/dev/pillar2-scan.sh — canonical Pillar 2 static scanner.
#
# Slice 2 of docs/plans/shipped/pillar-1-2-perf-review-system.md. For each first-party
# C++ source/header passed as an argument:
#   1. Decide whether the TU is UI-thread-reachable (heuristic).
#   2. If reachable, grep for known synchronous-I/O call patterns.
#   3. Emit CRITICAL for each hit NOT preceded within 3 lines by BOTH a
#      `/* PILLAR2_WORKER_ONLY */` marker AND an `// est-latency: <N>ms` (or
#      `<N>s`) comment.
#
# Harness-agnostic — pure bash. Invoked by:
#   - .claude/hooks/lint-cpp-pillar2.sh (Claude Code wrapper; per-file batch)
#   - bash scripts/dev/test-all.sh (auto-enrolled scan over changed files)
#   - manual: `bash scripts/dev/pillar2-scan.sh <file.cpp|file.h>...`
#
# Exit codes:
#   0 — no CRITICAL findings.
#   1 — at least one CRITICAL finding (script printed file:line evidence).
#   2 — usage error or no arguments.
#
# Why a text-search reachability heuristic (not full AST): the scanner runs
# inside the per-edit lint loop where a clang AST query would be too slow.
# False-positives (a `*Ui*.cpp` file with safely-worker-bound I/O) get
# annotated via the same PILLAR2_WORKER_ONLY marker — no whitelist drift.

set -uo pipefail

usage() {
    cat >&2 <<'USAGE'
Usage: bash scripts/dev/pillar2-scan.sh <file.cpp|file.h>...

  Scans each first-party C++ file for synchronous-I/O calls reachable from
  the UI thread without a PILLAR2_WORKER_ONLY + est-latency annotation.
USAGE
    exit 2
}

if [[ $# -eq 0 ]]; then
    usage
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT" || { echo "ERROR: cd to $REPO_ROOT failed" >&2; exit 2; }

# --- patterns ---------------------------------------------------------------
# Sync I/O reaching the UI thread is a Pillar 2 CRITICAL. Patterns mirror the
# AGENTS.md § UX Pillars § Pillar 2 enforceable invariants list.
#
# `cpr::*`        — HTTP/HTTPS calls (the most common offender).
# `SQLite::Database` / `SQLite::Statement::executeStep` — blocking SQLite calls.
# `popen` / `system` — process launches (p4 invocation via subprocess).
# `std::ifstream <var>(...)` — disk reads (open-and-read pattern).
# `std::mutex.*lock()` — explicit lock acquisitions reaching the UI thread.
SYNC_IO_REGEX='(cpr::(Get|Post|Put|Delete|Head|Options|Patch)\(|SQLite::(Database|Statement::executeStep)|popen\(|system\(|std::ifstream[[:space:]]+[[:alnum:]_]+[[:space:]]*\(|\.lock\(\))'

# Annotation: a line is "exempt" if WITHIN 3 LINES PRIOR there is BOTH:
#   /* PILLAR2_WORKER_ONLY */   — semantic OK-flag
#   // est-latency: <N>ms|s     — required-latency comment per C10
ANNOTATION_OK_REGEX='PILLAR2_WORKER_ONLY'
# POSIX-portable — \s isn't reliable across grep variants; [[:space:]] is. The
# numeric part tolerates `~` prefix (rough estimate) + decimals, and the unit
# is `ms` or `s` after optional whitespace.
ANNOTATION_LATENCY_REGEX='est-latency:[[:space:]]*~?[0-9.]+[[:space:]]*m?s'

# --- helpers ----------------------------------------------------------------

is_first_party() {
    case "$1" in
        Source/Core/*|Source/Plugins/*|Source/Standalone/*|tests/*) return 0 ;;
        *) return 1 ;;
    esac
}

# Excludes vendored third-party copies inside our tree.
is_vendored() {
    case "$1" in
        Source/Core/ThirdParty/*) return 0 ;;
        *) return 1 ;;
    esac
}

# UI-thread reachability heuristic.
# True when ANY of:
#   - filename matches *Ui*.cpp / *Ui*.h
#   - filename matches Smatchet*.cpp (top-level UI surface files)
#   - file includes <imgui.h> directly
is_ui_reachable() {
    local f="$1"
    case "$(basename "$f")" in
        *Ui*.cpp|*Ui*.h) return 0 ;;
        SmatchetUI*.cpp|SmatchetUI*.h) return 0 ;;
    esac
    if grep -qE '^[[:space:]]*#include[[:space:]]+["<]imgui\.h[">]' "$f" 2>/dev/null; then
        return 0
    fi
    return 1
}

# --- main loop --------------------------------------------------------------

CRITICAL_COUNT=0
SCANNED_COUNT=0
EXEMPT_COUNT=0

for arg in "$@"; do
    # Normalise: strip leading ./ if present.
    file="${arg#./}"
    # Skip non-existent files (Edit hook may queue deleted paths).
    [[ -f "$file" ]] || continue
    # First-party C++ only.
    is_first_party "$file" || continue
    is_vendored "$file" && continue
    # UI-thread reachability check.
    is_ui_reachable "$file" || continue

    SCANNED_COUNT=$((SCANNED_COUNT + 1))

    # Find every line matching the sync-I/O regex.
    # grep -n => line numbers. -E => extended regex.
    while IFS= read -r match; do
        [[ -z "$match" ]] && continue
        line_num="${match%%:*}"
        line_text="${match#*:}"

        # Skip false-positives where the regex matched inside a comment. A
        # comment cannot execute I/O — strip leading whitespace and check the
        # first one or two characters: // (line comment), /* (block-comment
        # opener), or * (continuation of a block comment). The /\** branch
        # is needed because `/*` followed by content (e.g. `/* std::ifstream
        # foo(p) */ // commented out`) does NOT match `//*` (needs second
        # `/`) nor `\**` (needs literal `*` at column 0).
        stripped="${line_text#"${line_text%%[![:space:]]*}"}"
        # Skip only genuine comment lines: // line comment, /* block open, `* text`
        # block continuation, or a bare `*/` close. The old `\**` matched ANY line
        # starting with '*', so a pointer-deref sink like `*out = popen(...)` was
        # silently skipped — a false negative that let sync I/O through the gate.
        case "$stripped" in
            //*|/\**|'* '*|'*/') continue ;;
        esac

        # Check the 3 preceding lines for the BOTH-OK pattern.
        start=$((line_num - 3))
        [[ $start -lt 1 ]] && start=1
        prefix="$(sed -n "${start},$((line_num - 1))p" "$file" 2>/dev/null)"

        if echo "$prefix" | grep -qE "$ANNOTATION_OK_REGEX" && \
           echo "$prefix" | grep -qE "$ANNOTATION_LATENCY_REGEX"; then
            EXEMPT_COUNT=$((EXEMPT_COUNT + 1))
            continue
        fi

        # Also tolerate a TODO marker that scanner emits as WARN (not CRITICAL).
        # WARN cases happen when a site is known-bad but tracked in backlog.
        if echo "$prefix" | grep -qE 'TODO\(pillar2\)'; then
            echo "WARN: $file:$line_num — sync-I/O tracked in backlog: $line_text" >&2
            continue
        fi

        # CRITICAL — emit file:line + the offending line text.
        echo "CRITICAL: $file:$line_num — sync I/O on UI-reachable path without PILLAR2_WORKER_ONLY annotation: $line_text" >&2
        CRITICAL_COUNT=$((CRITICAL_COUNT + 1))
    done < <(grep -nE "$SYNC_IO_REGEX" "$file" 2>/dev/null || true)
done

if [[ $CRITICAL_COUNT -gt 0 ]]; then
    echo >&2
    echo "pillar2-scan: $CRITICAL_COUNT CRITICAL finding(s) across $SCANNED_COUNT UI-reachable file(s); $EXEMPT_COUNT exempt." >&2
    echo >&2
    echo "Pillar 2 (AGENTS.md § UX Pillars § 2): synchronous I/O reaching the UI thread is code-review CRITICAL." >&2
    echo "Fix options:" >&2
    echo "  (a) Move the call to a worker thread + post results back via MainThreadDispatcher::PostToMainThread." >&2
    echo "  (b) If already on a worker (false positive), annotate the line with:" >&2
    echo "      /* PILLAR2_WORKER_ONLY */ // est-latency: <N>ms" >&2
    echo "      (Both required; either alone still fails the gate.)" >&2
    echo "  (c) If the migration is tracked, mark with: // TODO(pillar2): <issue-ref>  (downgrades to WARN.)" >&2
    exit 1
fi

exit 0

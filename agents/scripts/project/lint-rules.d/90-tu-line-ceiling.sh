#!/usr/bin/env bash
# 90-tu-line-ceiling.sh — tu-line-ceiling rule (advisory; sourced by test-lint-rules.sh, not run directly).

# tu-line-ceiling — ADVISORY (WARN-first, never touches $rc) nudge against god-file regrowth. A
# first-party translation unit (.cpp under Source/, excluding ThirdParty) that a change touches and
# that exceeds 1,200 lines gets a soft warning to consider a cohesive partition — the same ceiling
# the god-file-splits campaign (docs/plans/shipped/god-file-splits.md) left every split TU under.
# Diff-scoped to the CHANGED TUs (a whole-tree scan would just re-report the grandfathered whales —
# SmatchetAiAssistantUi.cpp, TicketFieldEditor.cpp, … — that predate the ceiling), so it fires only
# when you actually touch an oversized TU. Headers are out of scope (a large header is a different
# smell). Advisory by construction: it prints to stderr and never fails the gate, mirroring the
# function-size soft tier + the comment-ratio warning. A single
# `// SMATCHET_DEVIATION(rule=tu-line-ceiling; reason=...; owner=...; revisit=...)` anywhere in the
# file escapes (a whole-file size rule has no single line to anchor to — same grammar as the
# agent-too-long file-level escape).
TU_LINE_CEILING_RULES=(tu-line-ceiling)
TU_LINE_CEILING_LIMIT=1200

scan_tu_line_ceiling_file() {
    # $1 = a candidate file. Emits `tu-line-ceiling\t<f>:<lines>` when it is a first-party .cpp over
    # the ceiling and carries no suppressing SMATCHET_DEVIATION(rule=tu-line-ceiling). Path-agnostic
    # (the diff / whole-tree scoping lives in the callers); .cpp-only so headers never match.
    local f="$1"
    [ -f "$f" ] || return 0
    case "$f" in *.cpp) ;; *) return 0 ;; esac
    local n
    n=$(wc -l < "$f" 2>/dev/null | tr -d ' ')
    [ -n "$n" ] || return 0
    [ "$n" -gt "$TU_LINE_CEILING_LIMIT" ] || return 0
    # A whole-file escape: one SMATCHET_DEVIATION(rule=tu-line-ceiling) anywhere in the file suppresses.
    if grep -q 'SMATCHET_DEVIATION(rule=tu-line-ceiling' "$f" 2>/dev/null; then return 0; fi
    printf 'tu-line-ceiling\t%s:%s\n' "$f" "$n"
}

compute_tu_line_ceiling_violations() {
    # Whole-tree scan over first-party C++ TUs (debug + bats harness + `--scan-tu-ceiling`). Excludes
    # ThirdParty. NOTE: intentionally NOT wired into the blocking `--diff` path as a whole-tree set —
    # the diff gate is delta-scoped (see the tu-line-ceiling block in test-lint-rules.sh); this
    # whole-tree enumerator exists for the campaign-sweep / diagnostic use only.
    local files=() f
    while IFS= read -r f; do files+=("$f"); done < <(
        git ls-files 'Source/' 2>/dev/null | grep -E '\.cpp$' | grep -vE '(^|/)ThirdParty/' || true
    )
    [ "${#files[@]}" -gt 0 ] || return 0
    for f in "${files[@]}"; do scan_tu_line_ceiling_file "$f"; done
}

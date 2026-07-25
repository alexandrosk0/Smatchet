#!/usr/bin/env bats
# tests/bats/dead_export_audit.bats
# ----------------------------------------------------------------------------
# Bats coverage for agents/scripts/core/dead_export_audit.py — the
# gate-blind-spot-sweep Slice 1 cross-TU dead-export detector. ADVISORY (WARN-first,
# never blocking) per the DRY gate's pre-graduation precedent (ADR-0015), so these
# tests pin the DETECTION semantics rather than an exit code.
#
# Covers: --selftest; and, via throwaway git repos, the partition rule that decides
# dead-vs-alive —
#   * an export with no caller anywhere IS flagged;
#   * a called export is NOT flagged;
#   * a caller in a THIRD TU (not the definer) keeps a symbol alive;
#   * an `inline` header-only helper with a caller in a .cpp is NOT flagged
#     (the false-positive class the first cut shipped with);
#   * a class MEMBER function is never reported (virtual dispatch blind spot);
#   * a mention in a COMMENT does not keep a dead export alive;
#   * a mention in a STRING LITERAL DOES (the Lua/MCP dispatch guard);
#   * a caller behind `#if SMATCHET_WITH_*` keeps the callee alive;
#   * the baseline grandfathers a finding so --check stops WARNing about it.
#
# There is no bats on the Windows CI runner, so these run at the pre-push gate and
# in the ubuntu "Agentic self-tests (bats)" lane (match dup_audit.bats structure).
#
# Requires: bash, bats, a working python interpreter (python3/python/py), git.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    export AUD="$REPO_ROOT/agents/scripts/core/dead_export_audit.py"
    export GATE="$REPO_ROOT/agents/scripts/core/test-dead-export-audit.sh"
    PY=""
    for c in python3 python py; do
        if command -v "$c" >/dev/null 2>&1 && "$c" -c "" >/dev/null 2>&1; then PY="$c"; break; fi
    done
    export PY
    [ -n "$PY" ] || skip "no working python interpreter"
    TMP="$(mktemp -d)"
    export TMP
}

teardown() {
    [ -n "${TMP:-}" ] && rm -rf "$TMP"
    return 0
}

# Init a throwaway repo with the Core layout the detector scans. Callers drop
# headers/TUs in afterwards and re-`git add`.
_mk_repo() {
    local dir="$1"
    mkdir -p "$dir/Source/Core/include" "$dir/Source/Core/src" "$dir/tests"
    git init -q "$dir"
    git -C "$dir" config user.email t@t.t
    git -C "$dir" config user.name t
}

_commit() {
    git -C "$1" add -A
}

# Run the detector's --list mode inside the fixture repo.
_list() {
    (cd "$1" && "$PY" "$AUD" --list 2>&1)
}

# ---------- selftest ----------

@test "selftest passes (parser + partition invariants in sync)" {
    run "$PY" "$AUD" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"invariants hold"* ]]
}

@test "the shell gate's --selftest passes (planted dead export flagged end-to-end)" {
    run bash "$GATE" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"Failed: 0"* ]]
}

# ---------- the partition rule ----------

@test "an export declared in a Core header with NO caller is flagged" {
    _mk_repo "$TMP/r"
    printf '#pragma once\n#include <string>\nstd::string Lonely(const std::string& s);\n' \
        > "$TMP/r/Source/Core/include/F.h"
    printf '#include "F.h"\nstd::string Lonely(const std::string& s) { return s; }\n' \
        > "$TMP/r/Source/Core/src/F.cpp"
    _commit "$TMP/r"
    run _list "$TMP/r"
    [ "$status" -eq 0 ]
    [[ "$output" == *"Lonely"* ]]
}

@test "an export called from another TU is NOT flagged" {
    _mk_repo "$TMP/r"
    printf '#pragma once\n#include <string>\nstd::string Used(const std::string& s);\n' \
        > "$TMP/r/Source/Core/include/F.h"
    printf '#include "F.h"\nstd::string Used(const std::string& s) { return s; }\n' \
        > "$TMP/r/Source/Core/src/F.cpp"
    printf '#include "F.h"\nstd::string Go(const std::string& s) { return Used(s); }\n' \
        > "$TMP/r/Source/Core/src/Caller.cpp"
    _commit "$TMP/r"
    run _list "$TMP/r"
    [ "$status" -eq 0 ]
    [[ "$output" != *"Used"* ]]
}

@test "an inline header-only helper with a .cpp caller is NOT flagged" {
    # The false-positive class the first cut of the detector shipped with: a header
    # that already carries the body owes ZERO out-of-line definitions, so its single
    # occurrence in a .cpp is a CALLER, not the definition.
    _mk_repo "$TMP/r"
    printf '#pragma once\n#include <string>\ninline std::string Inl(std::string s) { return s; }\n' \
        > "$TMP/r/Source/Core/include/F.h"
    printf '#include "F.h"\nstd::string Go(std::string s) { return Inl(s); }\n' \
        > "$TMP/r/Source/Core/src/Caller.cpp"
    _commit "$TMP/r"
    run _list "$TMP/r"
    [ "$status" -eq 0 ]
    [[ "$output" != *"Inl"* ]]
}

@test "an inline header-only helper with NO caller IS flagged" {
    _mk_repo "$TMP/r"
    printf '#pragma once\n#include <string>\ninline std::string InlDead(std::string s) { return s; }\n' \
        > "$TMP/r/Source/Core/include/F.h"
    _commit "$TMP/r"
    run _list "$TMP/r"
    [ "$status" -eq 0 ]
    [[ "$output" == *"InlDead"* ]]
}

@test "a class MEMBER function is never reported (virtual-dispatch blind spot)" {
    _mk_repo "$TMP/r"
    printf '#pragma once\nclass W {\npublic:\n    void MemberOnly();\n};\n' \
        > "$TMP/r/Source/Core/include/F.h"
    printf '#include "F.h"\nvoid W::MemberOnly() {}\n' \
        > "$TMP/r/Source/Core/src/F.cpp"
    _commit "$TMP/r"
    run _list "$TMP/r"
    [ "$status" -eq 0 ]
    [[ "$output" != *"MemberOnly"* ]]
}

# ---------- comments dead / literals live ----------

@test "a mention in a COMMENT does not keep a dead export alive" {
    _mk_repo "$TMP/r"
    printf '#pragma once\n#include <string>\nstd::string Ghosted(const std::string& s);\n' \
        > "$TMP/r/Source/Core/include/F.h"
    printf '#include "F.h"\nstd::string Ghosted(const std::string& s) { return s; }\n' \
        > "$TMP/r/Source/Core/src/F.cpp"
    printf '#include "F.h"\n// TODO: wire Ghosted() up one day\nint Other() { return 0; }\n' \
        > "$TMP/r/Source/Core/src/Other.cpp"
    _commit "$TMP/r"
    run _list "$TMP/r"
    [ "$status" -eq 0 ]
    [[ "$output" == *"Ghosted"* ]]
}

@test "a mention in a STRING LITERAL keeps it alive (Lua/MCP dispatch guard)" {
    _mk_repo "$TMP/r"
    printf '#pragma once\n#include <string>\nstd::string Dispatched(const std::string& s);\n' \
        > "$TMP/r/Source/Core/include/F.h"
    printf '#include "F.h"\nstd::string Dispatched(const std::string& s) { return s; }\n' \
        > "$TMP/r/Source/Core/src/F.cpp"
    printf '#include "F.h"\nstatic const char* kName = "Dispatched";\n' \
        > "$TMP/r/Source/Core/src/Table.cpp"
    _commit "$TMP/r"
    run _list "$TMP/r"
    [ "$status" -eq 0 ]
    [[ "$output" != *"Dispatched"* ]]
}

@test "a caller behind #if SMATCHET_WITH_* keeps the callee alive" {
    _mk_repo "$TMP/r"
    printf '#pragma once\n#include <string>\nstd::string FlagGated(const std::string& s);\n' \
        > "$TMP/r/Source/Core/include/F.h"
    printf '#include "F.h"\nstd::string FlagGated(const std::string& s) { return s; }\n' \
        > "$TMP/r/Source/Core/src/F.cpp"
    printf '#include "F.h"\n#if SMATCHET_WITH_LUA\nstd::string Go(std::string s) { return FlagGated(s); }\n#endif\n' \
        > "$TMP/r/Source/Core/src/Gated.cpp"
    _commit "$TMP/r"
    run _list "$TMP/r"
    [ "$status" -eq 0 ]
    [[ "$output" != *"FlagGated"* ]]
}

@test "a caller in tests/ keeps an export alive" {
    _mk_repo "$TMP/r"
    printf '#pragma once\n#include <string>\nstd::string TestedOnly(const std::string& s);\n' \
        > "$TMP/r/Source/Core/include/F.h"
    printf '#include "F.h"\nstd::string TestedOnly(const std::string& s) { return s; }\n' \
        > "$TMP/r/Source/Core/src/F.cpp"
    printf '#include "F.h"\nint main() { return TestedOnly("x").empty(); }\n' \
        > "$TMP/r/tests/F.test.cpp"
    _commit "$TMP/r"
    run _list "$TMP/r"
    [ "$status" -eq 0 ]
    [[ "$output" != *"TestedOnly"* ]]
}

# ---------- baseline grandfathering ----------

@test "--check WARNs on a new finding and stops once it is baselined" {
    _mk_repo "$TMP/r"
    printf '#pragma once\n#include <string>\nstd::string Grandfathered(const std::string& s);\n' \
        > "$TMP/r/Source/Core/include/F.h"
    printf '#include "F.h"\nstd::string Grandfathered(const std::string& s) { return s; }\n' \
        > "$TMP/r/Source/Core/src/F.cpp"
    _commit "$TMP/r"

    run bash -c "cd '$TMP/r' && '$PY' '$AUD' --check"
    [ "$status" -eq 0 ]
    [[ "$output" == *"WARN"* ]]
    [[ "$output" == *"Grandfathered"* ]]

    mkdir -p "$TMP/r/docs/high-integrity"
    (cd "$TMP/r" && "$PY" "$AUD" --baseline-md) > "$TMP/r/docs/high-integrity/dead-export-baseline.md"
    run bash -c "cd '$TMP/r' && '$PY' '$AUD' --check"
    [ "$status" -eq 0 ]
    [[ "$output" != *"WARN"* ]]
    [[ "$output" == *"PASS"* ]]
}

@test "--check NOTEs a baselined symbol that is no longer dead (burn-down ratchet)" {
    _mk_repo "$TMP/r"
    mkdir -p "$TMP/r/docs/high-integrity"
    printf '#pragma once\n#include <string>\nstd::string Revived(const std::string& s);\n' \
        > "$TMP/r/Source/Core/include/F.h"
    printf '#include "F.h"\nstd::string Revived(const std::string& s) { return s; }\n' \
        > "$TMP/r/Source/Core/src/F.cpp"
    _commit "$TMP/r"
    (cd "$TMP/r" && "$PY" "$AUD" --baseline-md) > "$TMP/r/docs/high-integrity/dead-export-baseline.md"

    # Now give it a caller — the baselined entry is stale.
    printf '#include "F.h"\nstd::string Go(std::string s) { return Revived(s); }\n' \
        > "$TMP/r/Source/Core/src/Caller.cpp"
    _commit "$TMP/r"
    run bash -c "cd '$TMP/r' && '$PY' '$AUD' --check"
    [ "$status" -eq 0 ]
    [[ "$output" == *"NOTE"* ]]
    [[ "$output" == *"Revived"* ]]
}

# ---------- fail-closed ----------

@test "a tree with no Core headers is an infra error, not a silent pass" {
    _mk_repo "$TMP/r"
    rm -rf "$TMP/r/Source"
    printf 'placeholder\n' > "$TMP/r/README.md"
    _commit "$TMP/r"
    run bash -c "cd '$TMP/r' && '$PY' '$AUD' --check"
    [ "$status" -eq 2 ]
    [[ "$output" == *"fail closed"* ]]
}

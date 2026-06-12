#!/usr/bin/env bats
# tests/bats/lint_rules.bats
# ----------------------------------------------------------------------------
# Bats coverage for agents/scripts/project/test-lint-rules.sh (high-integrity C++ gate).
# Plan: docs/design/high-integrity-cpp-enforcement.md.
#
# Covers: each grep rule via --scan-file, SMATCHET_DEVIATION suppression +
# overdue detection, --selftest (AGENTS.md zone sync), --catalog format +
# determinism, and the --diff delta gate via a stubbed baseline triple-set.
#
# Requires: bash, bats. (narrowing-conversions is opt-in / clang-only — not
# exercised here; it has no deterministic fixture without a clang compile db.)
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    export LINT="$REPO_ROOT/agents/scripts/project/test-lint-rules.sh"
    export FIX="$REPO_ROOT/tests/fixtures/lint_rules"
}

# ---------- per-rule detection (--scan-file) ----------

@test "no-printf-stderr fires on unexempted std::printf" {
    run bash "$LINT" --scan-file "$FIX/known-bad-printf.cpp"
    [ "$status" -eq 0 ]
    [[ "$output" == *"no-printf-stderr"* ]]
}

@test "no-raw-new fires on raw new Type" {
    run bash "$LINT" --scan-file "$FIX/known-bad-raw-new.cpp"
    [ "$status" -eq 0 ]
    [[ "$output" == *"no-raw-new"* ]]
}

@test "define-imgui fires on #define ImGui macro-alias" {
    run bash "$LINT" --scan-file "$FIX/known-bad-define-imgui.cpp"
    [ "$status" -eq 0 ]
    [[ "$output" == *"define-imgui"* ]]
}

@test "no-detach fires on raw .detach()" {
    run bash "$LINT" --scan-file "$FIX/known-bad-detach.cpp"
    [ "$status" -eq 0 ]
    [[ "$output" == *"no-detach"* ]]
}

@test "no-detach does NOT fire on a comment or string-literal mention of .detach()" {
    run bash "$LINT" --scan-file "$FIX/detach-in-comment.cpp"
    [ "$status" -eq 0 ]
    [[ "$output" != *"no-detach"* ]]
}

@test "no-detach suppressed by SMATCHET_DEVIATION(rule=no-detach)" {
    run bash "$LINT" --scan-file "$FIX/detach-deviation.cpp"
    [ "$status" -eq 0 ]
    [[ "$output" != *"no-detach"* ]]
}

# ---------- no-glfw-in-core-headers (#24) ----------

@test "no-glfw-in-core-headers fires on a header with a GLFW include" {
    run bash "$LINT" --scan-file "$FIX/known-bad-glfw-header.h"
    [ "$status" -eq 0 ]
    [[ "$output" == *"no-glfw-in-core-headers"* ]]
}

@test "no-glfw-in-core-headers does NOT fire on a clean header (incl. prose mentions)" {
    run bash "$LINT" --scan-file "$FIX/known-good-core-header.h"
    [ "$status" -eq 0 ]
    [[ "$output" != *"no-glfw-in-core-headers"* ]]
}

@test "no-glfw-in-core-headers suppressed by SMATCHET_DEVIATION(rule=no-glfw-in-core-headers)" {
    run bash "$LINT" --scan-file "$FIX/glfw-deviation-header.h"
    [ "$status" -eq 0 ]
    [[ "$output" != *"no-glfw-in-core-headers"* ]]
}

@test "--scan-glfw FAILs on a GLFW include in a Source/Core/include header" {
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/Source/Core/include/Tracker"
    printf '#pragma once\n#include <GLFW/glfw3.h>\nstruct X {};\n' \
        > "$tmp/Source/Core/include/Tracker/Bad.h"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-glfw
    [ "$status" -eq 0 ]
    [[ "$output" == *"no-glfw-in-core-headers"* ]]
    [[ "$output" == *"Tracker/Bad.h"* ]]
    rm -rf "$tmp"
}

@test "--scan-glfw ignores GLFW in a .cpp and in Source/Standalone" {
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/Source/Core/src/Ui" "$tmp/Source/Standalone"
    # GLFW in a .cpp under Core is fine (DX12 doesn't compile .cpp window code into the lib).
    printf '#include <GLFW/glfw3.h>\nvoid f() {}\n' > "$tmp/Source/Core/src/Ui/Win.cpp"
    # GLFW header in Standalone is fine (not a Source/Core/include header).
    printf '#pragma once\n#include <GLFW/glfw3.h>\n' > "$tmp/Source/Standalone/glue.h"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-glfw
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    rm -rf "$tmp"
}

@test "--scan-glfw is clean on the real first-party tree" {
    run bash "$LINT" --scan-glfw
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "--diff enforces the no-glfw-in-core-headers absolute-0 gate (clean tree PASSes)" {
    run bash "$LINT" --full
    [ "$status" -eq 0 ]
    printf '%s\n' "$output" | sed -n 's/^  //p' > /tmp/lr_base_all
    SMATCHET_LINT_BASELINE_SET=/tmp/lr_base_all run bash "$LINT" --diff
    [ "$status" -eq 0 ]
    [[ "$output" == *"no GLFW/glad/OpenGL include in Source/Core/include headers"* ]]
}

# ---------- cmake-local-gate-ci-scope (infra:10 #1074) ----------

@test "--scan-cmake-ci FAILs on an unguarded knob-keyed FATAL_ERROR" {
    tmp="$(mktemp -d)"
    printf 'if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")\n  set(_p "msvc_toolset_pin")\n  if(NOT _p STREQUAL _cc)\n    message(FATAL_ERROR "toolset mismatch")\n  endif()\nendif()\n' \
        > "$tmp/CMakeLists.txt"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-cmake-ci
    [ "$status" -eq 0 ]
    [[ "$output" == *"cmake-local-gate-ci-scope"* ]]
    [[ "$output" == *"CMakeLists.txt"* ]]
    rm -rf "$tmp"
}

@test "--scan-cmake-ci ignores a CI-scoped (NOT DEFINED ENV{CI}) local-knob guard" {
    tmp="$(mktemp -d)"
    printf 'if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND NOT DEFINED ENV{CI})\n  set(_p "msvc_toolset_pin")\n  if(NOT _p STREQUAL _cc)\n    message(FATAL_ERROR "toolset mismatch")\n  endif()\nendif()\n' \
        > "$tmp/CMakeLists.txt"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-cmake-ci
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    rm -rf "$tmp"
}

@test "--scan-cmake-ci ignores a FATAL_ERROR with no local-knob in its window" {
    tmp="$(mktemp -d)"
    printf 'if(NOT LUA_FOUND)\n  message(FATAL_ERROR "Lua download failed")\nendif()\n' \
        > "$tmp/CMakeLists.txt"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-cmake-ci
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    rm -rf "$tmp"
}

@test "--scan-cmake-ci respects an in-window SMATCHET_DEVIATION" {
    tmp="$(mktemp -d)"
    printf 'if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")\n  set(_p "msvc_toolset_pin")\n  # SMATCHET_DEVIATION(rule=cmake-local-gate-ci-scope; reason=test; owner=x; revisit=2099-01-01)\n  message(FATAL_ERROR "toolset mismatch")\nendif()\n' \
        > "$tmp/CMakeLists.txt"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-cmake-ci
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    rm -rf "$tmp"
}

@test "--scan-cmake-ci is clean on the real tree (the toolset guard is CI-scoped)" {
    run bash "$LINT" --scan-cmake-ci
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

# ---------- SMATCHET_DEVIATION ----------

@test "deviation-overdue fires when calendar revisit has passed" {
    run bash "$LINT" --scan-file "$FIX/deviation-overdue.cpp"
    [ "$status" -eq 0 ]
    [[ "$output" == *"deviation-overdue"* ]]
}

@test "deviation suppresses its rule on the next line + overdue not flagged when current" {
    run bash "$LINT" --scan-file "$FIX/deviation-current.cpp"
    [ "$status" -eq 0 ]
    # rule=no-raw-new suppresses the new Thing(); revisit=2099 is not overdue.
    [[ "$output" != *"no-raw-new"* ]]
    [[ "$output" != *"deviation-overdue"* ]]
}

@test "deviation-overdue fixture suppresses no-raw-new (only overdue fires)" {
    run bash "$LINT" --scan-file "$FIX/deviation-overdue.cpp"
    [ "$status" -eq 0 ]
    [[ "$output" != *"no-raw-new"* ]]
}

# ---------- known-good ----------

@test "known-good fixture produces no findings" {
    run bash "$LINT" --scan-file "$FIX/known-good.cpp"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

# ---------- selftest: AGENTS.md zone-glob sync ----------

@test "--selftest passes (scanner zone globs match AGENTS.md)" {
    run bash "$LINT" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"in sync"* ]]
}

# ---------- comment_audit prose-vs-code discriminator (build-quality #7) ----------

@test "comment_audit.py --selftest passes (prose-vs-code fixtures)" {
    PY="$(command -v python3 || command -v python)"
    run "$PY" "$REPO_ROOT/agents/scripts/core/comment_audit.py" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"PASS"* ]]
}

# ---------- catalog: format + determinism ----------

@test "--catalog emits the rule-id sections + Totals" {
    run bash "$LINT" --catalog
    [ "$status" -eq 0 ]
    [[ "$output" == *"strict zone × no-printf-stderr"* ]]
    [[ "$output" == *"strict zone × deviation-overdue"* ]]
    [[ "$output" == *"## Totals"* ]]
}

@test "--catalog is byte-deterministic across runs (no timestamp drift)" {
    run bash -c "bash '$LINT' --catalog > /tmp/lr_c1 2>/dev/null; bash '$LINT' --catalog > /tmp/lr_c2 2>/dev/null; diff -q /tmp/lr_c1 /tmp/lr_c2"
    [ "$status" -eq 0 ]
}

# ---------- diff delta gate via stubbed baseline ----------

@test "--diff PASSes when HEAD triples are a subset of the baseline" {
    # Validate --full succeeded before trusting its output as the baseline.
    run bash "$LINT" --full
    [ "$status" -eq 0 ]
    printf '%s\n' "$output" | sed -n 's/^  //p' > /tmp/lr_base_all
    SMATCHET_LINT_BASELINE_SET=/tmp/lr_base_all run bash "$LINT" --diff
    [ "$status" -eq 0 ]
    [[ "$output" == *"PASS"* ]]
}

@test "--diff FAILs when HEAD has a triple absent from the baseline" {
    # Empty baseline -> every current strict-zone violation is 'new'.
    : > /tmp/lr_base_empty
    SMATCHET_LINT_BASELINE_SET=/tmp/lr_base_empty run bash "$LINT" --diff
    [ "$status" -eq 1 ]
    [[ "$output" == *"new strict-zone"* ]]
}

# ---------- first-party-wide absolute rules (no-raw-new / deviation-overdue) ----------

@test "--scan-wide fires no-raw-new for a NON-strict first-party file (Ui)" {
    # Ui is the 'light' zone — outside the strict zone, but still first-party, so
    # the wide gate must catch a raw new here.
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/Source/Core/src/Ui"
    printf 'struct W {};\nvoid f() { W* p = new W(); (void)p; }\n' > "$tmp/Source/Core/src/Ui/Widget.cpp"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-wide
    [ "$status" -eq 0 ]
    [[ "$output" == *"no-raw-new"* ]]
    [[ "$output" == *"Ui/Widget.cpp"* ]]
    rm -rf "$tmp"
}

@test "--scan-wide ignores ThirdParty and non-first-party (tests) raw new" {
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/Source/Core/ThirdParty/x" "$tmp/tests"
    printf 'struct V {};\nvoid g() { V* p = new V(); (void)p; }\n' > "$tmp/Source/Core/ThirdParty/x/vendor.cpp"
    printf 'struct T {};\nvoid h() { T* p = new T(); (void)p; }\n' > "$tmp/tests/t.cpp"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-wide
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    rm -rf "$tmp"
}

@test "--scan-wide respects an exemption marker on the raw new" {
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/Source/Standalone"
    printf 'struct H {};\nH* make() { return new H(); } // C-ABI handle\n' > "$tmp/Source/Standalone/main.cpp"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-wide
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    rm -rf "$tmp"
}

@test "--scan-wide is clean on the real first-party tree" {
    run bash "$LINT" --scan-wide
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "--diff enforces the first-party-wide no-raw-new / deviation-overdue gate" {
    run bash "$LINT" --full
    [ "$status" -eq 0 ]
    printf '%s\n' "$output" | sed -n 's/^  //p' > /tmp/lr_base_all
    SMATCHET_LINT_BASELINE_SET=/tmp/lr_base_all run bash "$LINT" --diff
    [ "$status" -eq 0 ]
    [[ "$output" == *"no first-party no-raw-new / deviation-overdue"* ]]
}

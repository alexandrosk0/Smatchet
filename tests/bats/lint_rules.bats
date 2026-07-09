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

# ---------- unused-symbol-under-config-guard (infra #863) ----------
# The config-skew -Werror,-Wunused-function class: a free-function DEFINITION
# unguarded while ALL its call sites are under a POSITIVE #if defined(SMATCHET_WITH_*).
# WARN-first in the diff gate (per the dup-gate precedent); --scan-unused-cfg emits
# the detected set for the bats harness. Fixtures replay the 61b17427~1 (pre-fix,
# detected) and #945 fixed (clean) shapes, plus the discriminators that keep the
# real-tree false-positive idioms (out-of-line member, #else-of-#if-!defined) quiet.

@test "--scan-unused-cfg detects the #863 shape (unguarded def, all refs under positive SMATCHET_WITH_* guard)" {
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/Source/Core/src"
    # 61b17427~1 shape: def at column 0 unguarded; both call sites inside the guard.
    printf 'void LogLuaScriptFileProbe(const char* label, const std::string& path) {\n    (void)label; (void)path;\n}\n\nvoid InitLua() {\n#if defined(SMATCHET_WITH_LUA_AUTOMATION)\n    LogLuaScriptFileProbe("SmatchetHooks.lua", "x");\n    LogLuaScriptFileProbe("Automation.lua", "y");\n#endif\n}\n' \
        > "$tmp/Source/Core/src/AppController.cpp"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-unused-cfg
    [ "$status" -eq 0 ]
    [[ "$output" == *"unused-symbol-under-config-guard"* ]]
    [[ "$output" == *"AppController.cpp:1"* ]]
    rm -rf "$tmp"
}

@test "--scan-unused-cfg is clean on the #945 fixed shape (def ALSO inside the guard)" {
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/Source/Core/src"
    # 61b17427 fixed shape: definition wrapped in the SAME #if as its call sites.
    printf 'void InitLua() {\n#if defined(SMATCHET_WITH_LUA_AUTOMATION)\n    LogLuaScriptFileProbe("SmatchetHooks.lua", "x");\n#endif\n}\n\n#if defined(SMATCHET_WITH_LUA_AUTOMATION)\nvoid LogLuaScriptFileProbe(const char* label, const std::string& path) {\n    (void)label; (void)path;\n}\n#endif\n' \
        > "$tmp/Source/Core/src/AppController.cpp"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-unused-cfg
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    rm -rf "$tmp"
}

@test "--scan-unused-cfg ignores an out-of-line member def (Type::method is never -Wunused-function)" {
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/Source/Core/src"
    # AppController::AddAiContext — a qualified member; its in-file refs are all
    # SMATCHET_WITH_AI-guarded but it is declared in a header + called cross-TU,
    # so it is NEVER -Wunused-function. (Real-tree false-positive caught pre-ship.)
    printf 'void AppController::AddAiContext(const AiContextBlock& block) {\n#if defined(SMATCHET_WITH_AI)\n    impl_->aiAssistant_->AddAiContext(block);\n#else\n    (void)block;\n#endif\n}\n' \
        > "$tmp/Source/Core/src/AppController.cpp"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-unused-cfg
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    rm -rf "$tmp"
}

@test "--scan-unused-cfg ignores a def with an unguarded reference (legit asymmetry)" {
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/Source/Core/src"
    printf 'void Helper(int x) {\n    (void)x;\n}\n\nvoid AlwaysCalls() {\n    Helper(1);\n}\n\nvoid InitLua() {\n#if defined(SMATCHET_WITH_LUA_AUTOMATION)\n    Helper(2);\n#endif\n}\n' \
        > "$tmp/Source/Core/src/Helper.cpp"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-unused-cfg
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    rm -rf "$tmp"
}

@test "--scan-unused-cfg respects an in-window SMATCHET_DEVIATION" {
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/Source/Core/src"
    printf '// SMATCHET_DEVIATION(rule=unused-symbol-under-config-guard; reason=test; owner=x; revisit=2099-01-01)\nvoid LogLuaScriptFileProbe(const char* label) {\n    (void)label;\n}\n\nvoid InitLua() {\n#if defined(SMATCHET_WITH_LUA_AUTOMATION)\n    LogLuaScriptFileProbe("x");\n#endif\n}\n' \
        > "$tmp/Source/Core/src/AppController.cpp"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-unused-cfg
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    rm -rf "$tmp"
}

@test "--scan-unused-cfg ignores a call in the #else of a #if !defined(SMATCHET_WITH_*) (negative-guard discriminator)" {
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/Source/Core/src"
    # The SmatchetWhisperSetupBanner idiom: the TU is CMake-gated ON, the real impl
    # (incl. the call) lives in the #else of `#if !defined(SMATCHET_WITH_WHISPER)`,
    # so the call is in the FLAG-ON branch — the def is never unused. Only POSITIVE
    # #if defined(...) true-branch refs count as guarded, so this must NOT fire.
    printf 'float ComputeHeight(float s) {\n    return 64.0f * s;\n}\n\nbool Render() {\n#if !defined(SMATCHET_WITH_WHISPER)\n    return false;\n#else\n    float h = ComputeHeight(1.0f);\n    return h > 0.0f;\n#endif\n}\n' \
        > "$tmp/Source/Core/src/Banner.cpp"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-unused-cfg
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    rm -rf "$tmp"
}

@test "--scan-unused-cfg runs clean (rc 0) on the real first-party tree" {
    # WARN-first calibration: the real tree carries a small, KNOWN advisory residual
    # (CliCommandRunner.cpp MCP-only helpers) that the per-file text proxy cannot
    # statically distinguish from the #863 shape — surfaced as WARNs in the diff
    # gate, never blocking. The scan mode must run to completion with rc 0; we assert
    # the residual stays bounded to CliCommandRunner (a regression that introduces a
    # NEW unrelated hit would change this set and is caught in review).
    run bash "$LINT" --scan-unused-cfg
    [ "$status" -eq 0 ]
    # Every emitted line (if any) is the known CliCommandRunner calibration residual.
    if [ -n "$output" ]; then
        run bash -c 'printf "%s\n" "$1" | grep -vE "CliCommandRunner\.cpp|unused-symbol-under-config-guard" || true' _ "$output"
        [ -z "$output" ]
    fi
}

# ---------- bare-json-parse-untrusted (BLOCKING; ALL first-party C++, repo-wide) ----------
# A bare nlohmann::json::parse( (or stream>>json slurp) not routed via ParseBounded crashes
# uncatchably on a depth bomb (#1271/#1287). Repo-wide default-deny — the old curated-TU
# allow-list lagged the code every time the class recurred (#1573/#1592/#1598); headers are
# in scope too. --scan-bare-json emits the detected set for the bats harness.

@test "--scan-bare-json fires on a bare json::parse in an ingress TU (McpPlugin)" {
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/Source/Plugins/Mcp"
    printf 'void f(const std::string& body) {\n    auto j = nlohmann::json::parse(body);\n    (void)j;\n}\n' \
        > "$tmp/Source/Plugins/Mcp/McpPlugin.cpp"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-bare-json
    [ "$status" -eq 0 ]
    [[ "$output" == *"bare-json-parse-untrusted"* ]]
    [[ "$output" == *"McpPlugin.cpp"* ]]
    rm -rf "$tmp"
}

@test "--scan-bare-json ignores a ParseBounded-routed ingress parse" {
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/Source/Plugins/Mcp"
    printf 'void f(const std::string& body) {\n    std::string err;\n    auto j = smatchet::json_safe::ParseBounded(body, err);\n    (void)j;\n}\n' \
        > "$tmp/Source/Plugins/Mcp/McpPlugin.cpp"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-bare-json
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    rm -rf "$tmp"
}

@test "--scan-bare-json fires on a bare parse in ANY first-party TU (repo-wide default-deny)" {
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/Source/Core/src"
    printf 'void f(const std::string& s) {\n    auto j = nlohmann::json::parse(s);\n    (void)j;\n}\n' \
        > "$tmp/Source/Core/src/SomeUnrelatedThing.cpp"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-bare-json
    [ "$status" -eq 0 ]
    [[ "$output" == *"bare-json-parse-untrusted"* ]]
    [[ "$output" == *"SomeUnrelatedThing.cpp"* ]]
    rm -rf "$tmp"
}

@test "--scan-bare-json fires on a bare parse in a HEADER" {
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/Source/Core/include"
    printf 'inline void f(const std::string& s) {\n    auto j = nlohmann::json::parse(s, nullptr, false);\n    (void)j;\n}\n' \
        > "$tmp/Source/Core/include/SomeInlineHelper.h"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-bare-json
    [ "$status" -eq 0 ]
    [[ "$output" == *"SomeInlineHelper.h"* ]]
    rm -rf "$tmp"
}

@test "--scan-bare-json fires on the stream>>json slurp form" {
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/Source/Core/src"
    printf 'void f(std::ifstream& file) {\n    nlohmann::json j;\n    file >> j;\n    (void)j;\n}\n' \
        > "$tmp/Source/Core/src/SlurpThing.cpp"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-bare-json
    [ "$status" -eq 0 ]
    [[ "$output" == *"SlurpThing.cpp:3"* ]]
    rm -rf "$tmp"
}

@test "--scan-bare-json ignores a >> into a non-json identifier" {
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/Source/Core/src"
    printf 'static nlohmann::json g_unrelated;\nvoid f(std::ifstream& file) {\n    int count = 0;\n    file >> count;\n    (void)count;\n}\n' \
        > "$tmp/Source/Core/src/NotASlurp.cpp"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-bare-json
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    rm -rf "$tmp"
}

@test "--scan-bare-json still fires when a trailing quoted comment mentions json::parse" {
    # Masking regression: the string-literal exemption must be evaluated on the comment-stripped
    # view — a real bare parse followed by `// logs "json::parse"` must NOT be skipped.
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/Source/Core/src"
    printf 'void f(const std::string& s) {\n    auto j = nlohmann::json::parse(s); // logs "json::parse" on failure\n    (void)j;\n}\n' \
        > "$tmp/Source/Core/src/MaskedParse.cpp"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-bare-json
    [ "$status" -eq 0 ]
    [[ "$output" == *"MaskedParse.cpp:2"* ]]
    rm -rf "$tmp"
}

@test "--scan-bare-json respects an in-window SMATCHET_DEVIATION" {
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/Source/Plugins/Mcp"
    printf 'void f(const std::string& body) {\n    // SMATCHET_DEVIATION(rule=bare-json-parse-untrusted; reason=test; owner=x; revisit=2099-01-01)\n    auto j = nlohmann::json::parse(body);\n    (void)j;\n}\n' \
        > "$tmp/Source/Plugins/Mcp/McpPlugin.cpp"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-bare-json
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    rm -rf "$tmp"
}

@test "--scan-bare-json is EMPTY on the real first-party tree (repo-wide clean invariant)" {
    # The repo-wide default-deny gate only works because the tree is clean: every remaining
    # bare parse was either routed through ParseBounded or carries an explicit
    # SMATCHET_DEVIATION. A non-empty set here means a bare parse landed without either —
    # fix it before it ships (this is the whole-tree backstop for the diff-scoped CI gate).
    run bash "$LINT" --scan-bare-json
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

# ---------- catch-all-swallow (BLOCKING; ALL first-party C++, absolute-0) ----------
# An EMPTY catch (...) body silently swallows every exception — exception-handling-policy.md
# hard rule 1 (review CRITICAL). The editor hook flags it per-edit; this is the CI gate.

@test "--scan-catch-all fires on an empty catch (...) body" {
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/Source/Core/src"
    printf 'void f() {\n    try {\n        g();\n    } catch (...) {}\n}\n' \
        > "$tmp/Source/Core/src/Swallower.cpp"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-catch-all
    [ "$status" -eq 0 ]
    [[ "$output" == *"catch-all-swallow"* ]]
    [[ "$output" == *"Swallower.cpp:4"* ]]
    rm -rf "$tmp"
}

@test "--scan-catch-all ignores a commented / logged / catch-all-ok body and a deviation" {
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/Source/Core/src"
    printf 'void a() {\n    try { g(); } catch (...) {\n        // best-effort stringify; failure renders "?"\n    }\n}\nvoid b() {\n    try { g(); } catch (...) {\n        LOG_WARN("g failed");\n    }\n}\nvoid c() {\n    try { g(); } catch (...) {} // catch-all-ok: thread-exit path\n}\nvoid d() {\n    // SMATCHET_DEVIATION(rule=catch-all-swallow; reason=test; owner=x; revisit=2099-01-01)\n    try { g(); } catch (...) {}\n}\n' \
        > "$tmp/Source/Core/src/Justified.cpp"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-catch-all
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    rm -rf "$tmp"
}

@test "--scan-catch-all is EMPTY on the real first-party tree (absolute-0 invariant)" {
    run bash "$LINT" --scan-catch-all
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

# ---------- unbounded-recursive-json-walker (WARN-first; the DW class) ----------

@test "--scan-json-walkers fires on an unbounded self-recursive json walker" {
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/Source/Core/src"
    printf 'static void Walk(const nlohmann::json& n) {\n    for (const auto& c : n) {\n        Walk(c);\n    }\n}\n' \
        > "$tmp/Source/Core/src/Walker.cpp"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-json-walkers
    [ "$status" -eq 0 ]
    [[ "$output" == *"unbounded-recursive-json-walker"* ]]
    [[ "$output" == *"Walker.cpp:1"* ]]
    rm -rf "$tmp"
}

@test "--scan-json-walkers ignores a depth-bounded walker and a deviation" {
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/Source/Core/src"
    printf 'static void Walk(const nlohmann::json& n, int depth) {\n    if (depth > 256) {\n        return;\n    }\n    for (const auto& c : n) {\n        Walk(c, depth + 1);\n    }\n}\n' \
        > "$tmp/Source/Core/src/Bounded.cpp"
    printf '// SMATCHET_DEVIATION(rule=unbounded-recursive-json-walker; reason=test; owner=x; revisit=2099-01-01)\nstatic void Walk2(const nlohmann::json& n) {\n    for (const auto& c : n) {\n        Walk2(c);\n    }\n}\n' \
        > "$tmp/Source/Core/src/Exempt.cpp"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-json-walkers
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    rm -rf "$tmp"
}

# ---------- unbounded-file-slurp (WARN-first) ----------

@test "--scan-slurps fires on rdbuf()/istreambuf slurps, respects a deviation" {
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/Source/Core/src"
    printf 'void f(std::ifstream& in) {\n    std::stringstream ss;\n    ss << in.rdbuf();\n}\nvoid g(std::ifstream& in) {\n    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());\n}\n' \
        > "$tmp/Source/Core/src/Slurper.cpp"
    printf 'void h(std::ifstream& in) {\n    std::stringstream ss;\n    // SMATCHET_DEVIATION(rule=unbounded-file-slurp; reason=test; owner=x; revisit=2099-01-01)\n    ss << in.rdbuf();\n}\n' \
        > "$tmp/Source/Core/src/ExemptSlurp.cpp"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-slurps
    [ "$status" -eq 0 ]
    [[ "$output" == *"Slurper.cpp:3"* ]]
    [[ "$output" == *"Slurper.cpp:6"* ]]
    [[ "$output" != *"ExemptSlurp.cpp"* ]]
    rm -rf "$tmp"
}

# ---------- ui-request-flag-off-thread (PR-5; strict-zone, absolute-0) ----------
# A g_ui request-flag write in a command-dispatch TU (Source/Core/src/Commands, excl.
# Scenarios) outside a RunOnUiThread* closure races the main loop polling those non-atomic
# fields (Pillar-3). --scan-ui-reqflag emits the detected set for the bats harness.

@test "--scan-ui-reqflag fires on a request-flag write outside a RunOnUiThread closure" {
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/Source/Core/src/Commands/Builtin"
    printf 'void Handler() {\n    g_ui.requestScreenshotPath = path;\n    g_ui.requestScreenshot = true;\n}\n' \
        > "$tmp/Source/Core/src/Commands/Builtin/BuiltinCommands_Bad.cpp"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-ui-reqflag
    [ "$status" -eq 0 ]
    [[ "$output" == *"ui-request-flag-off-thread"* ]]
    [[ "$output" == *"BuiltinCommands_Bad.cpp"* ]]
    rm -rf "$tmp"
}

@test "--scan-ui-reqflag ignores a write inside a RunOnUiThread closure" {
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/Source/Core/src/Commands/Builtin"
    printf 'CommandResult Handler(AppController& app) {\n    return RunOnUiThreadAsCommandResult(app, [path]() {\n        g_ui.requestScreenshotPath = path;\n        g_ui.requestScreenshot = true;\n        return CommandResult::Success();\n    });\n}\n' \
        > "$tmp/Source/Core/src/Commands/Builtin/BuiltinCommands_Good.cpp"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-ui-reqflag
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    rm -rf "$tmp"
}

@test "--scan-ui-reqflag exempts Scenarios (run on the UI thread by contract)" {
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/Source/Core/src/Commands/Scenarios"
    printf 'void OnStart() {\n    g_ui.requestWindowResize = true;\n}\n' \
        > "$tmp/Source/Core/src/Commands/Scenarios/MyScenario.cpp"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-ui-reqflag
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    rm -rf "$tmp"
}

@test "--scan-ui-reqflag ignores a read/compare of a request flag (not an assignment)" {
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/Source/Core/src/Commands"
    printf 'void Poll() {\n    if (g_ui.requestScreenshot) {\n        DoShot();\n    }\n}\n' \
        > "$tmp/Source/Core/src/Commands/Reader.cpp"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-ui-reqflag
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    rm -rf "$tmp"
}

@test "--scan-ui-reqflag respects an in-window SMATCHET_DEVIATION" {
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/Source/Core/src/Commands/Builtin"
    printf 'void Handler() {\n    // SMATCHET_DEVIATION(rule=ui-request-flag-off-thread; reason=test; owner=x; revisit=2099-01-01)\n    g_ui.requestScreenshot = true;\n}\n' \
        > "$tmp/Source/Core/src/Commands/Builtin/BuiltinCommands_Dev.cpp"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-ui-reqflag
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    rm -rf "$tmp"
}

@test "--scan-ui-reqflag is clean on the real first-party tree (handlers all marshal)" {
    run bash "$LINT" --scan-ui-reqflag
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

# ---------- pr-numbered-temporal-comments (WARN-first; comment-regrowth guard) ----------
# A comment pinning a DEV pull-request number (// PR 5 / PR #1104 / PR#1218 / PR12) is a temporal
# scaffold that rots once the PR squash-merges. --scan-pr-comments emits the detected set for bats.

@test "--scan-pr-comments fires on dev-PR-number comment shapes" {
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/Source/Core/src/Tracker"
    printf '// PR 6: legacy key removed.\n// PR #1104 review.\n// CR PR#1218.\n// PR12 latency fix.\n' \
        > "$tmp/Source/Core/src/Tracker/Thing.cpp"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-pr-comments
    [ "$status" -eq 0 ]
    [[ "$output" == *"pr-numbered-temporal-comments"* ]]
    [[ "$output" == *"Thing.cpp"* ]]
    # all four shapes detected
    [ "$(printf '%s\n' "$output" | grep -c 'pr-numbered-temporal-comments')" -eq 4 ]
    rm -rf "$tmp"
}

@test "--scan-pr-comments ignores product-domain PR usage (no number)" {
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/Source/Core/src/Tracker"
    printf '// GitHub PR-only columns from the per-PR enrichment loop.\n// the JQL shorthand type:pr maps to is:pr.\n// a [PR] prefix so users tell PRs apart.\n' \
        > "$tmp/Source/Core/src/Tracker/GitHubThing.cpp"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-pr-comments
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    rm -rf "$tmp"
}

@test "--scan-pr-comments ignores GitHub Issue / ADR refs and non-comment code lines" {
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/Source/Core/src/Tracker"
    printf '// capture-then-check (issue #1081); see ADR-0012.\nconst int kPR12Threshold = 5;\n' \
        > "$tmp/Source/Core/src/Tracker/Refs.cpp"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-pr-comments
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    rm -rf "$tmp"
}

@test "--scan-pr-comments respects an in-window SMATCHET_DEVIATION" {
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/Source/Core/src/Tracker"
    printf '// SMATCHET_DEVIATION(rule=pr-numbered-temporal-comments; reason=test; owner=x; revisit=2099-01-01)\n// PR 6: cited for the audit trail.\n' \
        > "$tmp/Source/Core/src/Tracker/Dev.cpp"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-pr-comments
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    rm -rf "$tmp"
}

@test "--scan-pr-comments is clean on the real first-party tree (post-sweep)" {
    run bash "$LINT" --scan-pr-comments
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

@test "comment_audit.py --fix strips a new blank-comment run but keeps prose AND code-like" {
    PY="$(command -v python3 || command -v python)"
    AUDIT="$REPO_ROOT/agents/scripts/core/comment_audit.py"
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/Source/Core/include"
    printf '#pragma once\n// header line one\n' > "$tmp/Source/Core/include/X.h"
    ( cd "$tmp" && git init -q && git config user.email a@b.c && git config user.name t \
        && git add -A && git commit -qm base ) >/dev/null 2>&1
    # Add a bare // (blank-run, auto-strippable) + prose + commented-out code (NOT auto-strippable).
    printf '//\n// kept prose paragraph here\n// foo(bar);\n' >> "$tmp/Source/Core/include/X.h"
    run bash -c "cd '$tmp' && '$PY' '$AUDIT' --fix HEAD"
    [ "$status" -eq 0 ]
    # The bare // is gone; the prose + the code-like line (manual reword) stay; header intact.
    run grep -c '^//$' "$tmp/Source/Core/include/X.h"
    [ "$output" -eq 0 ]
    grep -q 'kept prose paragraph' "$tmp/Source/Core/include/X.h"
    grep -q 'foo(bar);' "$tmp/Source/Core/include/X.h"
    grep -q 'header line one' "$tmp/Source/Core/include/X.h"
    rm -rf "$tmp"
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

# ---------- lint-rules.d module loading (monolith split) ----------
# The scanner sources its per-rule-family modules from lint-rules.d/ next to the
# entry point. Loading must FAIL CLOSED: a missing module means a silently
# partial rule set, which would pass dirty PRs as false-clean.

@test "entry point fails closed (rc 2) when a lint-rules.d module is missing" {
    tmp="$(mktemp -d)"
    cp "$LINT" "$tmp/test-lint-rules.sh"
    mkdir -p "$tmp/lint-rules.d"
    # Copy all modules EXCEPT one rule family.
    for m in "$REPO_ROOT/agents/scripts/project/lint-rules.d"/*.sh; do
        case "$m" in *55-catch-all.sh) continue ;; esac
        cp "$m" "$tmp/lint-rules.d/"
    done
    run bash "$tmp/test-lint-rules.sh" --root "$REPO_ROOT" --scan-catch-all
    [ "$status" -eq 2 ]
    [[ "$output" == *"missing rule module"* ]]
    rm -rf "$tmp"
}

@test "entry point loads modules from its own directory, not the --root target" {
    # The --diff base scan re-invokes the CURRENT scanner against a base worktree
    # that may predate the split (no lint-rules.d there) — modules must resolve
    # relative to the script, so a scan of a bare tree still works.
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/Source/Core/src"
    printf 'void f() {\n    try {\n        g();\n    } catch (...) {}\n}\n' > "$tmp/Source/Core/src/Swallow.cpp"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash "$LINT" --root "$tmp" --scan-catch-all
    [ "$status" -eq 0 ]
    [[ "$output" == *"catch-all-swallow"* ]]
    rm -rf "$tmp"
}

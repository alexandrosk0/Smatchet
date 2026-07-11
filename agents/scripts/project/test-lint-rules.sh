#!/usr/bin/env bash
# test-lint-rules.sh — tiered high-integrity C++ enforcement for Smatchet.
#
# Plan: docs/plans/shipped/high-integrity-cpp-enforcement.md (precursor reorg #505).
#
# Zones (single source of truth = AGENTS.md § Tiered enforcement; the globs
# below are asserted identical to AGENTS.md by --selftest):
#   strict — Source/Core/src/{Tracker,Sync,Persistence,Config,Commands}, Source/Plugins/Mcp
#            + matching include/ subdirs. Any rule violation here FAILS.
#   light  — Source/Core/src/Ui, Source/Standalone. Not gated (existing inline
#            exemption-comment vocabulary continues to apply).
#   exempt — ThirdParty, build, non-C++ trees. Not scanned.
#
# Rule ids (stable kebab-case; the linkage between scanner output, the catalog
# section headers, and SMATCHET_DEVIATION(rule=<id>) suppression):
#   no-printf-stderr       printf/fprintf/cerr/cout without an exemption marker
#   no-raw-new             raw `new T` (use make_unique) without an exemption marker
#   no-detach              `.detach()` (use AppController::LaunchBackgroundTask; first-party-wide)
#   no-glfw-in-core-headers  GLFW/glad/OpenGL #include in a Source/Core/include header
#                          (DX12 target compiles them too; absolute-0, no grandfathering)
#   cmake-local-gate-ci-scope  message(FATAL_ERROR ...) keyed on a LOCAL-dev project.config
#                          knob (msvc_toolset_pin) without a NOT DEFINED ENV{CI} scope —
#                          would FATAL every fresh-configure CI runner (absolute-0; #1074)
#   narrowing-conversions  clang-tidy cppcoreguidelines-narrowing-conversions (strict TUs)
#   define-imgui           `#define ImGui...` macro-alias trick
#   deviation-overdue      SMATCHET_DEVIATION whose calendar revisit= has passed
#   function-too-long      function body > 120 lines non-UI / > 200 lines ImGui-draw
#                          (repo-wide, delta-gated; tiered; function_size_audit.py)
#   function-too-branchy   function decision count > 30 (repo-wide, delta-gated, all functions)
#   (advisory)             [func-size] WARN on > 100 lines / > 20 branches — never blocks
#   agent-too-long         agent prompt > 250 lines / AGENTS.md > 150 (delta-gated; agent_size_audit.py)
#   (advisory)             [agent-size] WARN — soft tiers + the docs/agent-rules + skills sinks (~400)
#   bare-json-parse-untrusted  bare json::parse( / stream>>json slurp not routed via
#                          json_safe::ParseBounded (ALL first-party .cpp/.h/.hpp; blocking)
#   catch-all-swallow      EMPTY catch (...) body — no statement, no justifying comment
#                          (all first-party C++; absolute-0; exception-handling-policy hard rule 1)
#   no-ui-include-in-domain  quote-form #include "Ui/..." in a DOMAIN subsystem (Tracker/Sync/
#                          Persistence/Config + include mirrors + Plugins/Mcp; absolute-0 —
#                          layer-DAG inversion the header->header include-cycle gate can't see)
#   (advisory)             unbounded-recursive-json-walker — self-recursive fn over a
#                          nlohmann::json/sol::object param with no depth/budget token (WARN)
#   (advisory)             unbounded-file-slurp — rdbuf()/istreambuf whole-file read (WARN)
#
# Modes:
#   (no args) / --diff [<ref>]   delta gate: fail only on (rule,basename,hash)
#                                triples present on HEAD but not <ref>
#                                (default origin/develop). Grandfathers existing.
#   --catalog [--refresh]        dump the strict-zone violator set; --refresh
#                                writes docs/high-integrity/baseline.md
#   --scan-file <f>              scan a single file with all rules (bats harness);
#                                zone-agnostic, prints `<rule>\t<file>:<line>`
#   --full                       whole-tree strict-zone scan (human/debug)
#   --selftest                   assert AGENTS.md zone globs == this script's copy
#
# Overrides:
#   SMATCHET_LINT_BASELINE_SET=<file>  read baseline triples from a file instead
#                                      of computing from git (bats stub).
#   SMATCHET_LINT_BYPASS=1             advisory short-circuit (exit 0).
#
# Exit codes: 0 pass, 1 new strict-zone violation / baseline drift, 2 usage error.
#
# Layout: the rule scanners + shared helpers live in lint-rules.d/*.sh
# (per-rule-family modules, sourced below in lexical order). This file keeps
# the CLI contract, zone config wiring, and mode dispatch.

set -euo pipefail

# Absolute path to THIS script — so the --diff base scan re-invokes the *current*
# scanner logic (not origin/develop's older copy) against the base worktree.
SELF="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"

# --root <dir> scans an arbitrary tree (used to scan the --diff baseline worktree
# with the current scanner). Default root = repo root relative to this script.
if [ "${1:-}" = "--root" ]; then
    cd "$2"; shift 2
else
    cd "$(dirname "$0")/../../.."
fi
REPO_ROOT="$(pwd)"

BASELINE_FILE="docs/high-integrity/baseline.md"

# Load the per-rule-family modules from the directory of THIS script (not the
# scanned root — the --diff base scan re-invokes this scanner against a base
# worktree and must use the current modules). Fail CLOSED when a module is
# missing: a silently partial rule set would pass dirty PRs as false-clean.
LINT_RULES_D="$(dirname "$SELF")/lint-rules.d"
for _mod in "$LINT_RULES_D"/00-common.sh "$LINT_RULES_D"/10-line-rules.sh \
            "$LINT_RULES_D"/20-narrowing.sh "$LINT_RULES_D"/30-cmake-ci-scope.sh \
            "$LINT_RULES_D"/40-unused-config-guard.sh "$LINT_RULES_D"/50-bare-json.sh \
            "$LINT_RULES_D"/55-catch-all.sh "$LINT_RULES_D"/60-json-walker.sh \
            "$LINT_RULES_D"/65-file-slurp.sh "$LINT_RULES_D"/70-ui-request-flag.sh \
            "$LINT_RULES_D"/75-pr-comments.sh "$LINT_RULES_D"/80-interface-doc.sh \
            "$LINT_RULES_D"/85-ui-include-direction.sh; do
    if [ ! -f "$_mod" ]; then
        echo "test-lint-rules: ERROR: missing rule module $_mod" >&2
        exit 2
    fi
    # shellcheck source=/dev/null
    . "$_mod"
done

# ---------------------------------------------------------------------------
# Modes
# ---------------------------------------------------------------------------
[ "${SMATCHET_LINT_BYPASS:-0}" = "1" ] && { echo "[test-lint-rules] BYPASS"; exit 0; }

MODE="diff"; ARG=""; REFRESH=0
# --refresh is a modifier on --catalog; detect it independently of the
# subcommand branch (keeps the subcommand parse value-free).
for _a in "$@"; do [ "$_a" = "--refresh" ] && REFRESH=1; done
case "${1:-}" in
    --diff)        MODE=diff;     ARG="${2:-}" ;;
    --diff=*)      MODE=diff;     ARG="${1#--diff=}" ;;
    --catalog)     MODE=catalog ;;
    --funcsize-baseline) MODE=funcsizebaseline ;;
    --agentsize-baseline) MODE=agentsizebaseline ;;
    --dup-baseline) MODE=dupbaseline ;;
    --include-cycle-baseline) MODE=includecyclebaseline ;;
    --scan-file)   MODE=scanfile; ARG="${2:-}" ;;
    --scan-file=*) MODE=scanfile; ARG="${1#--scan-file=}" ;;
    --full)        MODE=full ;;
    --scan-wide)   MODE=scanwide ;;
    --scan-glfw)   MODE=scanglfw ;;
    --scan-cmake-ci) MODE=scancmakeci ;;
    --scan-unused-cfg) MODE=scanunusedcfg ;;
    --scan-bare-json) MODE=scanbarejson ;;
    --scan-catch-all) MODE=scancatchall ;;
    --scan-json-walkers) MODE=scanjsonwalkers ;;
    --scan-slurps) MODE=scanslurps ;;
    --scan-ui-reqflag) MODE=scanuireqflag ;;
    --scan-ui-include) MODE=scanuiinclude ;;
    --scan-pr-comments) MODE=scanprcomments ;;
    --selftest)    MODE=selftest ;;
    "")            MODE=diff ;;
    *) echo "usage: $0 [--diff[=]<ref>|--catalog [--refresh]|--funcsize-baseline|--agentsize-baseline|--dup-baseline|--include-cycle-baseline|--scan-file[=]<f>|--full|--scan-wide|--scan-glfw|--scan-cmake-ci|--scan-unused-cfg|--scan-bare-json|--scan-catch-all|--scan-json-walkers|--scan-slurps|--scan-ui-reqflag|--scan-ui-include|--scan-pr-comments|--selftest]" >&2; exit 2 ;;
esac

case "$MODE" in
  scanfile)
    [ -n "$ARG" ] || { echo "--scan-file needs a path" >&2; exit 2; }
    # shellcheck disable=SC2034  # raw (the matched source line) is dropped on purpose here.
    scan_file_rules "$ARG" | while IFS=$'\t' read -r rule loc raw; do printf '%s\t%s\n' "$rule" "$loc"; done
    ;;

  selftest)
    # Assert each STRICT_GLOBS entry appears in AGENTS.md § Tiered enforcement.
    miss=0
    for g in "${STRICT_GLOBS[@]}"; do
        # only check the canonical src/* + Mcp globs (include/ mirrors are implied)
        case "$g" in Source/Core/include/*) continue ;; esac
        if ! grep -qF "$g" AGENTS.md; then echo "SELFTEST FAIL: '$g' missing from AGENTS.md" >&2; miss=1; fi
    done
    # Assert each repo-wide comment-regrowth rule-id is documented in AGENTS.md (delta-gated list).
    for r in "${COMMENT_RULES[@]}"; do
        if ! grep -qF "$r" AGENTS.md; then echo "SELFTEST FAIL: comment rule '$r' missing from AGENTS.md" >&2; miss=1; fi
    done
    # Assert each repo-wide function-size rule-id is documented in AGENTS.md (delta-gated list).
    for r in "${FUNCSIZE_RULES[@]}"; do
        if ! grep -qF "$r" AGENTS.md; then echo "SELFTEST FAIL: function-size rule '$r' missing from AGENTS.md" >&2; miss=1; fi
    done
    # Assert the agent-prompt-size rule-id is documented in AGENTS.md (delta-gated list).
    for r in "${AGENTSIZE_RULES[@]}"; do
        if ! grep -qF "$r" AGENTS.md; then echo "SELFTEST FAIL: agent-size rule '$r' missing from AGENTS.md" >&2; miss=1; fi
    done
    # Assert the include-cycle rule-id is documented in AGENTS.md (delta-gated, BLOCKING gate).
    for r in "${INCLUDECYCLE_RULES[@]}"; do
        if ! grep -qF "$r" AGENTS.md; then echo "SELFTEST FAIL: include-cycle rule '$r' missing from AGENTS.md" >&2; miss=1; fi
    done
    # Assert the AppController fan-in rule-id is documented in AGENTS.md (delta-gated, BLOCKING gate).
    for r in "${FANIN_RULES[@]}"; do
        if ! grep -qF "$r" AGENTS.md; then echo "SELFTEST FAIL: fan-in rule '$r' missing from AGENTS.md" >&2; miss=1; fi
    done
    # Assert the PR-5 rule-ids are documented in AGENTS.md (bare-json WARN-first; ui-reqflag absolute-0).
    for r in "${BAREJSON_RULES[@]}" "${UIREQFLAG_RULES[@]}"; do
        if ! grep -qF "$r" AGENTS.md; then echo "SELFTEST FAIL: PR-5 rule '$r' missing from AGENTS.md" >&2; miss=1; fi
    done
    # Delegate the tiered-cap + UI-classification in-sync assertion to the audit script's own
    # --selftest (single source of truth = function_size_audit.py is_ui_function() vs AGENTS.md).
    # Assert the duplication rule-id (DRY Engineering Pillar 5) is documented in AGENTS.md.
    if ! grep -qF "duplication" AGENTS.md; then echo "SELFTEST FAIL: 'duplication' rule missing from AGENTS.md" >&2; miss=1; fi
    # Assert the no-glfw-in-core-headers rule-id is documented in AGENTS.md (absolute-0 gate).
    if ! grep -qF "no-glfw-in-core-headers" AGENTS.md; then echo "SELFTEST FAIL: 'no-glfw-in-core-headers' rule missing from AGENTS.md" >&2; miss=1; fi
    # Assert the cmake-local-gate-ci-scope rule-id is documented in AGENTS.md (absolute-0 gate).
    if ! grep -qF "cmake-local-gate-ci-scope" AGENTS.md; then echo "SELFTEST FAIL: 'cmake-local-gate-ci-scope' rule missing from AGENTS.md" >&2; miss=1; fi
    # cmake-local-gate-ci-scope: asserts-failure — an unguarded knob-keyed FATAL_ERROR must fire;
    # a CI-scoped one (NOT DEFINED ENV{CI}) must not; a SMATCHET_DEVIATION in the window must not.
    _cmci_tmp="$(mktemp 2>/dev/null || echo "${TMPDIR:-/tmp}/cmci_selftest.$$")"
    printf 'if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")\n  set(_p "msvc_toolset_pin")\n  if(NOT _p STREQUAL _cc)\n    message(FATAL_ERROR "toolset mismatch")\n  endif()\nendif()\n' > "$_cmci_tmp"
    if [ -z "$(scan_cmake_ci_scope_file "$_cmci_tmp")" ]; then
        echo "SELFTEST FAIL: cmake-local-gate-ci-scope did not fire on an unguarded knob-keyed FATAL_ERROR" >&2; miss=1; fi
    printf 'if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND NOT DEFINED ENV{CI})\n  set(_p "msvc_toolset_pin")\n  if(NOT _p STREQUAL _cc)\n    message(FATAL_ERROR "toolset mismatch")\n  endif()\nendif()\n' > "$_cmci_tmp"
    if [ -n "$(scan_cmake_ci_scope_file "$_cmci_tmp")" ]; then
        echo "SELFTEST FAIL: cmake-local-gate-ci-scope fired on a CI-scoped knob-keyed FATAL_ERROR" >&2; miss=1; fi
    printf 'if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")\n  set(_p "msvc_toolset_pin")\n  # SMATCHET_DEVIATION(rule=cmake-local-gate-ci-scope; reason=test; owner=x; revisit=2099-01-01)\n  message(FATAL_ERROR "toolset mismatch")\nendif()\n' > "$_cmci_tmp"
    if [ -n "$(scan_cmake_ci_scope_file "$_cmci_tmp")" ]; then
        echo "SELFTEST FAIL: cmake-local-gate-ci-scope fired despite an in-window SMATCHET_DEVIATION" >&2; miss=1; fi
    rm -f "$_cmci_tmp" 2>/dev/null || true
    # Assert the unused-symbol-under-config-guard rule-id is documented in AGENTS.md (absolute-0 gate).
    if ! grep -qF "unused-symbol-under-config-guard" AGENTS.md; then echo "SELFTEST FAIL: 'unused-symbol-under-config-guard' rule missing from AGENTS.md" >&2; miss=1; fi
    # unused-symbol-under-config-guard: asserts-failure — replays the #863 shape (61b17427~1):
    # an UNGUARDED column-0 free-function definition whose only call sites are inside a
    # #if defined(SMATCHET_WITH_LUA_AUTOMATION) block MUST fire; the fixed shape (#945 / 61b17427,
    # definition also wrapped in the guard) MUST be clean; an in-window SMATCHET_DEVIATION MUST escape.
    # The scanner is .cpp-gated, so the fixtures carry a .cpp extension.
    _uscg_tmp="$(mktemp --suffix=.cpp 2>/dev/null || echo "${TMPDIR:-/tmp}/uscg_selftest.$$.cpp")"
    case "$_uscg_tmp" in *.cpp) ;; *) mv -f "$_uscg_tmp" "$_uscg_tmp.cpp" 2>/dev/null && _uscg_tmp="$_uscg_tmp.cpp" ;; esac
    # Positive: #863 pre-fix shape — def at column 0, unguarded; both call sites guarded.
    printf 'void LogLuaScriptFileProbe(const char* label, const std::string& path) {\n    (void)label; (void)path;\n}\n\nvoid InitLua() {\n#if defined(SMATCHET_WITH_LUA_AUTOMATION)\n    LogLuaScriptFileProbe("SmatchetHooks.lua", "x");\n    LogLuaScriptFileProbe("Automation.lua", "y");\n#endif\n}\n' > "$_uscg_tmp"
    if [ -z "$(scan_unused_under_config_guard_file "$_uscg_tmp")" ]; then
        echo "SELFTEST FAIL: unused-symbol-under-config-guard did not fire on an unguarded def whose only refs are SMATCHET_WITH_*-guarded (the #863 shape)" >&2; miss=1; fi
    # Negative: #945 fixed shape — def ALSO inside the guard (symmetric). Clean.
    printf 'void InitLua() {\n#if defined(SMATCHET_WITH_LUA_AUTOMATION)\n    LogLuaScriptFileProbe("SmatchetHooks.lua", "x");\n#endif\n}\n\n#if defined(SMATCHET_WITH_LUA_AUTOMATION)\nvoid LogLuaScriptFileProbe(const char* label, const std::string& path) {\n    (void)label; (void)path;\n}\n#endif\n' > "$_uscg_tmp"
    if [ -n "$(scan_unused_under_config_guard_file "$_uscg_tmp")" ]; then
        echo "SELFTEST FAIL: unused-symbol-under-config-guard fired on the symmetric (def+refs both guarded) #945 fixed shape" >&2; miss=1; fi
    # Negative: a def with an UNGUARDED reference too (legit asymmetry) must not fire.
    printf 'void Helper(int x) {\n    (void)x;\n}\n\nvoid AlwaysCalls() {\n    Helper(1);\n}\n\nvoid InitLua() {\n#if defined(SMATCHET_WITH_LUA_AUTOMATION)\n    Helper(2);\n#endif\n}\n' > "$_uscg_tmp"
    if [ -n "$(scan_unused_under_config_guard_file "$_uscg_tmp")" ]; then
        echo "SELFTEST FAIL: unused-symbol-under-config-guard fired despite an unguarded reference (legit asymmetry)" >&2; miss=1; fi
    # Negative: in-window SMATCHET_DEVIATION above the unguarded def must escape.
    printf '// SMATCHET_DEVIATION(rule=unused-symbol-under-config-guard; reason=test; owner=x; revisit=2099-01-01)\nvoid LogLuaScriptFileProbe(const char* label) {\n    (void)label;\n}\n\nvoid InitLua() {\n#if defined(SMATCHET_WITH_LUA_AUTOMATION)\n    LogLuaScriptFileProbe("x");\n#endif\n}\n' > "$_uscg_tmp"
    if [ -n "$(scan_unused_under_config_guard_file "$_uscg_tmp")" ]; then
        echo "SELFTEST FAIL: unused-symbol-under-config-guard fired despite an in-window SMATCHET_DEVIATION" >&2; miss=1; fi
    rm -f "$_uscg_tmp" 2>/dev/null || true
    # --- bare-json-parse-untrusted — assert the rule is documented + fires on a bare parse in ANY
    # first-party TU (repo-wide default-deny), in a HEADER, and on the stream>>json slurp form; and
    # stays quiet for a ParseBounded route / a deviation. ---
    if ! grep -qF "bare-json-parse-untrusted" AGENTS.md; then echo "SELFTEST FAIL: 'bare-json-parse-untrusted' rule missing from AGENTS.md" >&2; miss=1; fi
    # selftest: asserts-failure — a bare json::parse in ANY TU must fire; ParseBounded / a
    # deviation must not.
    _bj_tmp="$(mktemp 2>/dev/null || echo "${TMPDIR:-/tmp}/bj_selftest.$$")"
    _bj_ingress="$(dirname "$_bj_tmp")/McpPlugin.cpp"
    printf 'void f(const std::string& body) {\n    auto j = nlohmann::json::parse(body);\n    (void)j;\n}\n' > "$_bj_ingress"
    if [ -z "$(scan_bare_json_parse_file "$_bj_ingress")" ]; then
        echo "SELFTEST FAIL: bare-json-parse-untrusted did not fire on a bare json::parse in an ingress TU (McpPlugin.cpp)" >&2; miss=1; fi
    printf 'void f(const std::string& body) {\n    std::string err;\n    auto j = smatchet::json_safe::ParseBounded(body, err);\n    (void)j;\n}\n' > "$_bj_ingress"
    if [ -n "$(scan_bare_json_parse_file "$_bj_ingress")" ]; then
        echo "SELFTEST FAIL: bare-json-parse-untrusted fired on a ParseBounded-routed ingress parse" >&2; miss=1; fi
    # Repo-wide default-deny: a NON-curated basename MUST fire too (the curated allow-list is
    # retired — it lagged the code every time the class recurred; #1573/#1592/#1598).
    _bj_other="$(dirname "$_bj_tmp")/SomeUnrelatedThing.cpp"
    printf 'void f(const std::string& s) {\n    auto j = nlohmann::json::parse(s);\n    (void)j;\n}\n' > "$_bj_other"
    if [ -z "$(scan_bare_json_parse_file "$_bj_other")" ]; then
        echo "SELFTEST FAIL: bare-json-parse-untrusted did not fire on a bare parse in a non-curated TU (repo-wide default-deny)" >&2; miss=1; fi
    # Headers are in scope (the JsonParseUtil.h class): a bare parse in a .h must fire.
    _bj_hdr="$(dirname "$_bj_tmp")/SomeInlineHelper.h"
    printf 'inline void f(const std::string& s) {\n    auto j = nlohmann::json::parse(s, nullptr, false);\n    (void)j;\n}\n' > "$_bj_hdr"
    if [ -z "$(scan_bare_json_parse_file "$_bj_hdr")" ]; then
        echo "SELFTEST FAIL: bare-json-parse-untrusted did not fire on a bare parse in a header" >&2; miss=1; fi
    # The `stream >> json` slurp form (declare-then-slurp within 8 lines) must fire.
    printf 'void f(std::ifstream& file) {\n    nlohmann::json j;\n    file >> j;\n    (void)j;\n}\n' > "$_bj_other"
    if [ -z "$(scan_bare_json_parse_file "$_bj_other")" ]; then
        echo "SELFTEST FAIL: bare-json-parse-untrusted did not fire on a stream>>json slurp" >&2; miss=1; fi
    # ...but a >> into a non-json identifier must NOT fire.
    printf 'void f(std::ifstream& file) {\n    int count = 0;\n    file >> count;\n    (void)count;\n}\n' > "$_bj_other"
    if [ -n "$(scan_bare_json_parse_file "$_bj_other")" ]; then
        echo "SELFTEST FAIL: bare-json-parse-untrusted fired on a >> into a non-json identifier" >&2; miss=1; fi
    # An in-line SMATCHET_DEVIATION above the parse must escape.
    printf 'void f(const std::string& body) {\n    // SMATCHET_DEVIATION(rule=bare-json-parse-untrusted; reason=test; owner=x; revisit=2099-01-01)\n    auto j = nlohmann::json::parse(body);\n    (void)j;\n}\n' > "$_bj_ingress"
    if [ -n "$(scan_bare_json_parse_file "$_bj_ingress")" ]; then
        echo "SELFTEST FAIL: bare-json-parse-untrusted fired despite an in-window SMATCHET_DEVIATION" >&2; miss=1; fi
    # A REAL bare parse with a trailing quoted-comment mention must STILL fire (the string-literal
    # exemption is evaluated on the comment-stripped view, so the comment cannot mask it).
    printf 'void f(const std::string& body) {\n    auto j = nlohmann::json::parse(body); // logs "json::parse" on failure\n    (void)j;\n}\n' > "$_bj_ingress"
    if [ -z "$(scan_bare_json_parse_file "$_bj_ingress")" ]; then
        echo "SELFTEST FAIL: bare-json-parse-untrusted was masked by a quoted trailing-comment mention" >&2; miss=1; fi
    rm -f "$_bj_tmp" "$_bj_ingress" "$_bj_other" "$_bj_hdr" 2>/dev/null || true
    # --- catch-all-swallow — assert the rule is documented + fires on an EMPTY catch (...) body and
    # stays quiet for a commented body / catch-all-ok / a LOG body / a deviation. ---
    for r in "${CATCHALL_RULES[@]}" "${JSONWALKER_RULES[@]}" "${SLURP_RULES[@]}"; do
        if ! grep -qF "$r" AGENTS.md; then echo "SELFTEST FAIL: recurring-findings rule '$r' missing from AGENTS.md" >&2; miss=1; fi
    done
    _ca_tmp="$(mktemp --suffix=.cpp 2>/dev/null || echo "${TMPDIR:-/tmp}/ca_selftest.$$.cpp")"
    case "$_ca_tmp" in *.cpp) ;; *) mv -f "$_ca_tmp" "$_ca_tmp.cpp" 2>/dev/null && _ca_tmp="$_ca_tmp.cpp" ;; esac
    # Positive: single-line and multi-line empty bodies must fire.
    printf 'void f() {\n    try {\n        g();\n    } catch (...) {}\n}\n' > "$_ca_tmp"
    if [ -z "$(scan_catch_all_swallow_file "$_ca_tmp")" ]; then
        echo "SELFTEST FAIL: catch-all-swallow did not fire on a single-line empty catch (...) body" >&2; miss=1; fi
    printf 'void f() {\n    try {\n        g();\n    } catch (...) {\n    }\n}\n' > "$_ca_tmp"
    if [ -z "$(scan_catch_all_swallow_file "$_ca_tmp")" ]; then
        echo "SELFTEST FAIL: catch-all-swallow did not fire on a multi-line empty catch (...) body" >&2; miss=1; fi
    # Negative: a justifying comment inside the body (policy escape) must NOT fire.
    printf 'void f() {\n    try {\n        g();\n    } catch (...) {\n        // best-effort stringify for logging; failure renders "?"\n    }\n}\n' > "$_ca_tmp"
    if [ -n "$(scan_catch_all_swallow_file "$_ca_tmp")" ]; then
        echo "SELFTEST FAIL: catch-all-swallow fired despite a justifying comment inside the body" >&2; miss=1; fi
    # Negative: the hook vocabulary `// catch-all-ok:` on the catch line must NOT fire.
    printf 'void f() {\n    try {\n        g();\n    } catch (...) {} // catch-all-ok: destructor path\n}\n' > "$_ca_tmp"
    if [ -n "$(scan_catch_all_swallow_file "$_ca_tmp")" ]; then
        echo "SELFTEST FAIL: catch-all-swallow fired despite a catch-all-ok marker" >&2; miss=1; fi
    # Negative: a body with a statement must NOT fire (the no-LOG tier stays editor-hook-advisory).
    printf 'void f() {\n    try {\n        g();\n    } catch (...) {\n        LOG_WARN("g failed");\n    }\n}\n' > "$_ca_tmp"
    if [ -n "$(scan_catch_all_swallow_file "$_ca_tmp")" ]; then
        echo "SELFTEST FAIL: catch-all-swallow fired on a non-empty catch body" >&2; miss=1; fi
    # Negative: an in-window SMATCHET_DEVIATION above the catch must escape.
    printf 'void f() {\n    try {\n        g();\n    // SMATCHET_DEVIATION(rule=catch-all-swallow; reason=test; owner=x; revisit=2099-01-01)\n    } catch (...) {}\n}\n' > "$_ca_tmp"
    if [ -n "$(scan_catch_all_swallow_file "$_ca_tmp")" ]; then
        echo "SELFTEST FAIL: catch-all-swallow fired despite an in-window SMATCHET_DEVIATION" >&2; miss=1; fi
    rm -f "$_ca_tmp" 2>/dev/null || true
    # --- unbounded-recursive-json-walker — asserts-failure + bounded/deviation negatives. ---
    _jw_tmp="$(mktemp --suffix=.cpp 2>/dev/null || echo "${TMPDIR:-/tmp}/jw_selftest.$$.cpp")"
    case "$_jw_tmp" in *.cpp) ;; *) mv -f "$_jw_tmp" "$_jw_tmp.cpp" 2>/dev/null && _jw_tmp="$_jw_tmp.cpp" ;; esac
    printf 'static void Walk(const nlohmann::json& n) {\n    for (const auto& c : n) {\n        Walk(c);\n    }\n}\n' > "$_jw_tmp"
    if [ -z "$(scan_json_walker_file "$_jw_tmp")" ]; then
        echo "SELFTEST FAIL: unbounded-recursive-json-walker did not fire on an unbounded self-recursive json walker" >&2; miss=1; fi
    printf 'static void Walk(const nlohmann::json& n, int depth) {\n    if (depth > 256) {\n        return;\n    }\n    for (const auto& c : n) {\n        Walk(c, depth + 1);\n    }\n}\n' > "$_jw_tmp"
    if [ -n "$(scan_json_walker_file "$_jw_tmp")" ]; then
        echo "SELFTEST FAIL: unbounded-recursive-json-walker fired on a depth-bounded walker" >&2; miss=1; fi
    printf '// SMATCHET_DEVIATION(rule=unbounded-recursive-json-walker; reason=test; owner=x; revisit=2099-01-01)\nstatic void Walk(const nlohmann::json& n) {\n    for (const auto& c : n) {\n        Walk(c);\n    }\n}\n' > "$_jw_tmp"
    if [ -n "$(scan_json_walker_file "$_jw_tmp")" ]; then
        echo "SELFTEST FAIL: unbounded-recursive-json-walker fired despite an in-window SMATCHET_DEVIATION" >&2; miss=1; fi
    rm -f "$_jw_tmp" 2>/dev/null || true
    # --- unbounded-file-slurp — asserts-failure + comment/deviation negatives. ---
    _sl_tmp="$(mktemp --suffix=.cpp 2>/dev/null || echo "${TMPDIR:-/tmp}/sl_selftest.$$.cpp")"
    case "$_sl_tmp" in *.cpp) ;; *) mv -f "$_sl_tmp" "$_sl_tmp.cpp" 2>/dev/null && _sl_tmp="$_sl_tmp.cpp" ;; esac
    printf 'void f(std::ifstream& in) {\n    std::stringstream ss;\n    ss << in.rdbuf();\n}\n' > "$_sl_tmp"
    if [ -z "$(scan_file_slurp_file "$_sl_tmp")" ]; then
        echo "SELFTEST FAIL: unbounded-file-slurp did not fire on an rdbuf() slurp" >&2; miss=1; fi
    printf 'void f(std::ifstream& in) {\n    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());\n}\n' > "$_sl_tmp"
    if [ -z "$(scan_file_slurp_file "$_sl_tmp")" ]; then
        echo "SELFTEST FAIL: unbounded-file-slurp did not fire on an istreambuf_iterator slurp" >&2; miss=1; fi
    printf 'void f(std::ifstream& in) {\n    // a comment mentioning ss << in.rdbuf() must not fire\n    int x = 0;\n    (void)x;\n}\n' > "$_sl_tmp"
    if [ -n "$(scan_file_slurp_file "$_sl_tmp")" ]; then
        echo "SELFTEST FAIL: unbounded-file-slurp fired on a comment mention" >&2; miss=1; fi
    printf 'void f(std::ifstream& in) {\n    std::stringstream ss;\n    // SMATCHET_DEVIATION(rule=unbounded-file-slurp; reason=test; owner=x; revisit=2099-01-01)\n    ss << in.rdbuf();\n}\n' > "$_sl_tmp"
    if [ -n "$(scan_file_slurp_file "$_sl_tmp")" ]; then
        echo "SELFTEST FAIL: unbounded-file-slurp fired despite an in-window SMATCHET_DEVIATION" >&2; miss=1; fi
    rm -f "$_sl_tmp" 2>/dev/null || true
    # --- ui-request-flag-off-thread (PR-5) — assert the rule is documented + fires on an unwrapped
    # request-flag write and stays quiet inside a RunOnUiThread closure / behind a deviation. ---
    if ! grep -qF "ui-request-flag-off-thread" AGENTS.md; then echo "SELFTEST FAIL: 'ui-request-flag-off-thread' rule missing from AGENTS.md" >&2; miss=1; fi
    # selftest: asserts-failure — a request-flag write outside RunOnUiThread must fire; inside it,
    # and behind a deviation, must not. (.cpp-gated, so the fixture carries a .cpp extension.)
    _ui_tmp="$(mktemp --suffix=.cpp 2>/dev/null || echo "${TMPDIR:-/tmp}/ui_selftest.$$.cpp")"
    case "$_ui_tmp" in *.cpp) ;; *) mv -f "$_ui_tmp" "$_ui_tmp.cpp" 2>/dev/null && _ui_tmp="$_ui_tmp.cpp" ;; esac
    # Positive: a direct off-thread write (no enclosing RunOnUiThread).
    printf 'void Handler() {\n    g_ui.requestScreenshotPath = path;\n    g_ui.requestScreenshot = true;\n}\n' > "$_ui_tmp"
    if [ -z "$(scan_ui_request_flag_file "$_ui_tmp")" ]; then
        echo "SELFTEST FAIL: ui-request-flag-off-thread did not fire on a request-flag write outside a RunOnUiThread closure" >&2; miss=1; fi
    # Negative: the same write inside a RunOnUiThread closure (the conformant marshalling seam).
    printf 'CommandResult Handler(AppController& app) {\n    return RunOnUiThreadAsCommandResult(app, [path]() {\n        g_ui.requestScreenshotPath = path;\n        g_ui.requestScreenshot = true;\n        return CommandResult::Success();\n    });\n}\n' > "$_ui_tmp"
    if [ -n "$(scan_ui_request_flag_file "$_ui_tmp")" ]; then
        echo "SELFTEST FAIL: ui-request-flag-off-thread fired on a write inside a RunOnUiThread closure" >&2; miss=1; fi
    # Negative: a READ / compare of a request flag (not an assignment) must NOT fire.
    printf 'void Poll() {\n    if (g_ui.requestScreenshot) {\n        DoShot();\n    }\n}\n' > "$_ui_tmp"
    if [ -n "$(scan_ui_request_flag_file "$_ui_tmp")" ]; then
        echo "SELFTEST FAIL: ui-request-flag-off-thread fired on a read/compare (not an assignment)" >&2; miss=1; fi
    # Negative: an in-window SMATCHET_DEVIATION above the write must escape.
    printf 'void Handler() {\n    // SMATCHET_DEVIATION(rule=ui-request-flag-off-thread; reason=test; owner=x; revisit=2099-01-01)\n    g_ui.requestScreenshot = true;\n}\n' > "$_ui_tmp"
    if [ -n "$(scan_ui_request_flag_file "$_ui_tmp")" ]; then
        echo "SELFTEST FAIL: ui-request-flag-off-thread fired despite an in-window SMATCHET_DEVIATION" >&2; miss=1; fi
    rm -f "$_ui_tmp" 2>/dev/null || true
    # --- no-ui-include-in-domain — assert the rule is documented + fires on a quote-form Ui/
    # include, and stays quiet for an angle-bracket / comment mention / a deviation. ---
    for r in "${UIINCLUDE_RULES[@]}"; do
        if ! grep -qF "$r" AGENTS.md; then echo "SELFTEST FAIL: include-direction rule '$r' missing from AGENTS.md" >&2; miss=1; fi
    done
    # selftest: asserts-failure — a quote-form `#include "Ui/..."` must fire; a comment mention,
    # an angle-bracket include, and an in-window SMATCHET_DEVIATION must not.
    _uii_tmp="$(mktemp --suffix=.cpp 2>/dev/null || echo "${TMPDIR:-/tmp}/uii_selftest.$$.cpp")"
    case "$_uii_tmp" in *.cpp) ;; *) mv -f "$_uii_tmp" "$_uii_tmp.cpp" 2>/dev/null && _uii_tmp="$_uii_tmp.cpp" ;; esac
    printf '#include "TrackerLabelsPure.h"\n#include "Ui/TouchCellEditGesture.h"\n' > "$_uii_tmp"
    if [ -z "$(scan_ui_include_direction_file "$_uii_tmp")" ]; then
        echo "SELFTEST FAIL: no-ui-include-in-domain did not fire on a quote-form Ui/ include" >&2; miss=1; fi
    printf '// the gesture gate used to live at Ui/TouchCellEditGesture.h\n#include <string>\n#include "TrackerLabelsPure.h"\n' > "$_uii_tmp"
    if [ -n "$(scan_ui_include_direction_file "$_uii_tmp")" ]; then
        echo "SELFTEST FAIL: no-ui-include-in-domain fired on a comment mention / non-Ui include" >&2; miss=1; fi
    printf '// SMATCHET_DEVIATION(rule=no-ui-include-in-domain; reason=test; owner=x; revisit=2099-01-01)\n#include "Ui/SmatchetToast.h"\n' > "$_uii_tmp"
    if [ -n "$(scan_ui_include_direction_file "$_uii_tmp")" ]; then
        echo "SELFTEST FAIL: no-ui-include-in-domain fired despite an in-window SMATCHET_DEVIATION" >&2; miss=1; fi
    rm -f "$_uii_tmp" 2>/dev/null || true
    # --- pr-numbered-temporal-comments — assert the rule is documented + fires on a dev-PR-number
    # comment, and stays quiet for product-domain "PR" (no number) / Issue refs / non-comment lines /
    # a deviation. ---
    if ! grep -qF "pr-numbered-temporal-comments" AGENTS.md; then echo "SELFTEST FAIL: 'pr-numbered-temporal-comments' rule missing from AGENTS.md" >&2; miss=1; fi
    _prc_tmp="$(mktemp --suffix=.cpp 2>/dev/null || echo "${TMPDIR:-/tmp}/prc_selftest.$$.cpp")"
    case "$_prc_tmp" in *.cpp) ;; *) mv -f "$_prc_tmp" "$_prc_tmp.cpp" 2>/dev/null && _prc_tmp="$_prc_tmp.cpp" ;; esac
    # Positive: each dev-PR-number comment shape must fire.
    printf '// PR 6: legacy key removed.\n// PR #1104 review MEDIUM-1.\n// CR PR#1218.\n// PR12 latency fix.\n' > "$_prc_tmp"
    _prc_hits="$(scan_pr_comment_file "$_prc_tmp" | wc -l | tr -d ' ')"
    if [ "$_prc_hits" != "4" ]; then
        echo "SELFTEST FAIL: pr-numbered-temporal-comments expected 4 dev-PR-number hits, got $_prc_hits" >&2; miss=1; fi
    # Negative: product-domain "PR" with NO number must NOT fire.
    printf '// GitHub PR-only columns populated by the per-PR enrichment loop.\n// the JQL shorthand type:pr maps to is:pr.\n// a visible [PR] prefix so users tell PRs apart.\n' > "$_prc_tmp"
    if [ -n "$(scan_pr_comment_file "$_prc_tmp")" ]; then
        echo "SELFTEST FAIL: pr-numbered-temporal-comments fired on product-domain PR usage (no number)" >&2; miss=1; fi
    # Negative: GitHub Issue refs / ADR refs must NOT fire (no PR prefix).
    printf '// capture-then-check (issue #1081); see ADR-0012.\n// drop the legacy field (#823).\n' > "$_prc_tmp"
    if [ -n "$(scan_pr_comment_file "$_prc_tmp")" ]; then
        echo "SELFTEST FAIL: pr-numbered-temporal-comments fired on a GitHub Issue / ADR ref" >&2; miss=1; fi
    # Negative: a NON-comment code line carrying a PR<digit>-looking token must NOT fire.
    printf 'const int kPR12Threshold = 5;\nReadOnlyMode = true;\n' > "$_prc_tmp"
    if [ -n "$(scan_pr_comment_file "$_prc_tmp")" ]; then
        echo "SELFTEST FAIL: pr-numbered-temporal-comments fired on a non-comment code line" >&2; miss=1; fi
    # Negative: an in-window SMATCHET_DEVIATION above the comment must escape.
    printf '// SMATCHET_DEVIATION(rule=pr-numbered-temporal-comments; reason=test; owner=x; revisit=2099-01-01)\n// PR 6: cited for the audit trail.\n' > "$_prc_tmp"
    if [ -n "$(scan_pr_comment_file "$_prc_tmp")" ]; then
        echo "SELFTEST FAIL: pr-numbered-temporal-comments fired despite an in-window SMATCHET_DEVIATION" >&2; miss=1; fi
    rm -f "$_prc_tmp" 2>/dev/null || true
    # --- interface-doc WARN (Gap B / Slice 2) — assert the rule WARNs on real drift, stays quiet
    # otherwise, and is documented. This exercises a FAILURE case (the pinned symbol changed without
    # a doc touch), satisfying both Slice 2's selftest contract and Slice 3's "assert-a-failure" rule.
    if ! grep -qF "interface-doc" AGENTS.md; then echo "SELFTEST FAIL: 'interface-doc' rule missing from AGENTS.md" >&2; miss=1; fi
    _idoc_pins=$'ITrackerIssueMutations::UpdateField'
    _idoc_hit=$'-    TrackerError UpdateField(const std::string& issueId, const TrackerField& field);\n+    Result<nlohmann::json, TrackerError> UpdateField(const std::string& issueId);'
    _idoc_miss=$'+    void SomeUnrelatedThing();'
    # selftest: asserts-failure — interface-doc must WARN on real drift (pinned symbol changed, doc untouched).
    if [ -z "$(interface_doc_emit "Source/Core/src/Tracker/AGENTS.md" 0 "$_idoc_pins" "$_idoc_hit" 2>&1 1>/dev/null)" ]; then
        echo "SELFTEST FAIL: interface-doc did not WARN when a doc-pinned symbol changed without a doc touch" >&2; miss=1; fi
    if [ -n "$(interface_doc_emit "Source/Core/src/Tracker/AGENTS.md" 1 "$_idoc_pins" "$_idoc_hit" 2>&1 1>/dev/null)" ]; then
        echo "SELFTEST FAIL: interface-doc WARNed despite the leaf doc being touched in the same diff" >&2; miss=1; fi
    if [ -n "$(interface_doc_emit "Source/Core/src/Tracker/AGENTS.md" 0 "$_idoc_pins" "$_idoc_miss" 2>&1 1>/dev/null)" ]; then
        echo "SELFTEST FAIL: interface-doc WARNed when the pinned symbol was absent from the header hunk" >&2; miss=1; fi
    st_py="$(resolve_python || true)"
    if [ -n "$st_py" ]; then
        if ! "$st_py" "$REPO_ROOT/agents/scripts/core/function_size_audit.py" --selftest; then miss=1; fi
        if ! "$st_py" "$REPO_ROOT/agents/scripts/core/dup_audit.py" --selftest; then miss=1; fi
        if ! "$st_py" "$REPO_ROOT/agents/scripts/core/agent_size_audit.py" --selftest; then miss=1; fi
        if ! "$st_py" "$REPO_ROOT/agents/scripts/core/include_cycle_audit.py" --selftest; then miss=1; fi
        if ! "$st_py" "$REPO_ROOT/agents/scripts/core/appcontroller_fan_in_audit.py" --selftest; then miss=1; fi
        if ! "$st_py" "$REPO_ROOT/agents/scripts/core/daemon_subprocess_timeout_audit.py" --selftest; then miss=1; fi
    else
        echo "test-lint-rules: WARN: no python interpreter; skipped function_size_audit.py / dup_audit.py / agent_size_audit.py / include_cycle_audit.py / appcontroller_fan_in_audit.py / daemon_subprocess_timeout_audit.py --selftest" >&2
    fi
    [ "$miss" -eq 0 ] && echo "selftest: AGENTS.md zone globs + comment + function-size + duplication rules in sync" || exit 1
    ;;

  full)
    compute_strict_triples | sed 's/^/  /'
    n=$(compute_strict_triples | wc -l)
    echo "strict-zone violations (grandfather candidates): $n"
    ;;

  scanwide)
    # First-party-wide no-raw-new / deviation-overdue set (debug + bats harness).
    # `--root <dir>` (handled above) points this at an arbitrary tree.
    compute_wide_violations
    ;;

  scanglfw)
    # no-glfw-in-core-headers absolute-0 set over Source/Core/include headers (debug +
    # bats harness). `--root <dir>` (handled above) points this at an arbitrary tree.
    compute_glfw_violations
    ;;

  scancmakeci)
    # cmake-local-gate-ci-scope absolute-0 set over CMakeLists.txt / cmake/*.cmake (debug +
    # bats harness). `--root <dir>` (handled above) points this at an arbitrary tree.
    compute_cmake_ci_scope_violations
    ;;

  scanunusedcfg)
    # unused-symbol-under-config-guard absolute-0 set over first-party .cpp TUs (debug +
    # bats harness). `--root <dir>` (handled above) points this at an arbitrary tree.
    compute_unused_under_config_guard_violations
    ;;

  scanbarejson)
    # bare-json-parse-untrusted set over ALL first-party C++ (.cpp/.h/.hpp) — whole-tree campaign
    # sweep (debug + bats harness). `--root <dir>` (handled above) points this at an arbitrary tree.
    compute_bare_json_parse_violations
    ;;

  scancatchall)
    # catch-all-swallow absolute-0 set over first-party C++ (debug + bats harness).
    # `--root <dir>` (handled above) points this at an arbitrary tree.
    compute_catch_all_violations
    ;;

  scanjsonwalkers)
    # unbounded-recursive-json-walker WARN set over first-party C++ — whole-tree campaign sweep
    # (debug + bats harness). `--root <dir>` (handled above) points this at an arbitrary tree.
    compute_json_walker_violations
    ;;

  scanslurps)
    # unbounded-file-slurp WARN set over first-party C++ — whole-tree campaign sweep (debug +
    # bats harness). `--root <dir>` (handled above) points this at an arbitrary tree.
    compute_file_slurp_violations
    ;;

  scanuireqflag)
    # ui-request-flag-off-thread set over Source/Core/src/Commands (excl. Scenarios) .cpp TUs
    # (debug + bats harness). `--root <dir>` (handled above) points this at an arbitrary tree.
    compute_ui_request_flag_violations
    ;;

  scanuiinclude)
    # no-ui-include-in-domain set over the domain subsystems (Tracker/Sync/Persistence/Config +
    # include mirrors + Plugins/Mcp) — debug + bats harness. `--root <dir>` (handled above) points
    # this at an arbitrary tree.
    compute_ui_include_direction_violations
    ;;

  scanprcomments)
    # pr-numbered-temporal-comments set over first-party C++ (.cpp/.h/.hpp) — whole-tree campaign
    # sweep (debug + bats harness). `--root <dir>` (handled above) points this at an arbitrary tree.
    compute_pr_comment_violations
    ;;

  catalog)
    triples="$(compute_strict_triples)"
    # NB: NO timestamp / commit-sha in the body — the develop post-merge job
    # enforces drift via `git diff --exit-code`, so the file must be a pure
    # function of the violation set (byte-identical when nothing changed).
    # "when did it last change" lives in git history, not the file.
    gen_catalog() {
        echo "# High-Integrity C++ — grandfathered baseline"
        echo
        echo "_Auto-generated. Do not hand-edit; run \`bash agents/scripts/project/test-lint-rules.sh --catalog --refresh\` and commit._"
        echo "_Refreshed on \`develop\` post-merge (fail-on-drift); gate uses live scan vs \`origin/develop\`, not this file._"
        local total=0 rule cnt
        for rule in narrowing-conversions no-printf-stderr no-raw-new define-imgui deviation-overdue; do
            echo
            cnt="$(printf '%s\n' "$triples" | awk -F'\t' -v r="$rule" '$1==r' | grep -c . || true)"
            echo "## strict zone × $rule ($cnt entries)"
            if [ "$cnt" -eq 0 ]; then echo "- (none)"; else
                printf '%s\n' "$triples" | awk -F'\t' -v r="$rule" '$1==r{print "- `"$2"` · `"$3"`"}' | sort
            fi
            total=$((total+cnt))
        done
        echo
        echo "## Totals"
        echo "- strict-zone violators grandfathered: $total"
    }
    if [ "$REFRESH" -eq 1 ]; then
        mkdir -p "$(dirname "$BASELINE_FILE")"
        gen_catalog > "$BASELINE_FILE"
        echo "[test-lint-rules] refreshed $BASELINE_FILE"
    else
        gen_catalog
    fi
    ;;

  funcsizebaseline)
    # Refresh the informational function-size grandfather snapshot. Kept in its OWN file (not
    # co-mingled with $BASELINE_FILE) so the strict-catalog determinism contract is untouched; the
    # gate itself is a live merge-base delta (function_size_audit.py --diff), not this file.
    fs_py="$(resolve_python || true)"
    [ -n "$fs_py" ] || { echo "test-lint-rules: ERROR: no python interpreter for --funcsize-baseline" >&2; exit 2; }
    FUNCSIZE_BASELINE_FILE="docs/high-integrity/function-size-baseline.md"
    mkdir -p "$(dirname "$FUNCSIZE_BASELINE_FILE")"
    "$fs_py" "$REPO_ROOT/agents/scripts/core/function_size_audit.py" --baseline-md > "$FUNCSIZE_BASELINE_FILE"
    echo "[test-lint-rules] refreshed $FUNCSIZE_BASELINE_FILE"
    ;;

  agentsizebaseline)
    # Refresh the informational agent-prompt/AGENTS.md size grandfather snapshot. Same contract as
    # --funcsize-baseline: the gate is a live merge-base delta (agent_size_audit.py --diff), not this
    # file. reduce-agent-prompt-bloat Slice 0.
    as_py="$(resolve_python || true)"
    [ -n "$as_py" ] || { echo "test-lint-rules: ERROR: no python interpreter for --agentsize-baseline" >&2; exit 2; }
    AGENTSIZE_BASELINE_FILE="docs/high-integrity/agent-size-baseline.md"
    mkdir -p "$(dirname "$AGENTSIZE_BASELINE_FILE")"
    "$as_py" "$REPO_ROOT/agents/scripts/core/agent_size_audit.py" --baseline-md > "$AGENTSIZE_BASELINE_FILE"
    echo "[test-lint-rules] refreshed $AGENTSIZE_BASELINE_FILE"
    ;;

  dupbaseline)
    # Refresh the informational duplication grandfather snapshot (DRY pillar). Same contract as
    # --funcsize-baseline: the gate is a live merge-base delta (dup_audit.py --diff), not this file.
    dup_py="$(resolve_python || true)"
    [ -n "$dup_py" ] || { echo "test-lint-rules: ERROR: no python interpreter for --dup-baseline" >&2; exit 2; }
    DUP_BASELINE_FILE="docs/high-integrity/dup-baseline.md"
    mkdir -p "$(dirname "$DUP_BASELINE_FILE")"
    "$dup_py" "$REPO_ROOT/agents/scripts/core/dup_audit.py" --baseline-md > "$DUP_BASELINE_FILE"
    echo "[test-lint-rules] refreshed $DUP_BASELINE_FILE"
    ;;

  includecyclebaseline)
    # Refresh the include-cycle grandfather/ratchet snapshot (core-include-dag Phase 0). Same
    # contract as --dup-baseline: the gate is a live merge-base delta (include_cycle_audit.py
    # --diff), not this file — but unlike dup this snapshot is also the RATCHET ledger (each later
    # phase deletes the line for the edge it kills).
    ic_py="$(resolve_python || true)"
    [ -n "$ic_py" ] || { echo "test-lint-rules: ERROR: no python interpreter for --include-cycle-baseline" >&2; exit 2; }
    INCLUDECYCLE_BASELINE_FILE="docs/high-integrity/include-cycle-baseline.md"
    mkdir -p "$(dirname "$INCLUDECYCLE_BASELINE_FILE")"
    "$ic_py" "$REPO_ROOT/agents/scripts/core/include_cycle_audit.py" --baseline-md > "$INCLUDECYCLE_BASELINE_FILE"
    echo "[test-lint-rules] refreshed $INCLUDECYCLE_BASELINE_FILE"
    ;;

  diff)
    BASE="${ARG:-origin/develop}"
    # narrowing-conversions is catalogue-only (not gated) — the base worktree has
    # no compile db, so it can't be diffed without false positives. Exclude it
    # from both sides of the set-diff. (See § Deviations in the plan.)
    head_set="$(compute_strict_triples | grep -v $'^narrowing-conversions\t' || true)"
    if [ -n "${SMATCHET_LINT_BASELINE_SET:-}" ] && [ -f "$SMATCHET_LINT_BASELINE_SET" ]; then
        base_set="$(sort -u "$SMATCHET_LINT_BASELINE_SET")"
    else
        # Compute the baseline triple-set against <ref> via a temp worktree so the
        # same scanner sees both trees without a checkout dance.
        # Fail closed (CR #507): an unresolved base or failed worktree must NOT
        # silently skip the gate in CI. Fetch origin/develop before invoking
        # locally if you hit this.
        if ! git rev-parse --verify --quiet "$BASE" >/dev/null 2>&1; then
            echo "test-lint-rules: ERROR: base '$BASE' unresolved; cannot compute delta gate" >&2
            exit 2
        fi
        wt="$(mktemp -d)"
        git worktree add -q --detach "$wt" "$BASE" 2>/dev/null || { echo "test-lint-rules: ERROR: worktree add for '$BASE' failed" >&2; exit 2; }
        # Run the CURRENT scanner (--root) against the base worktree so both sides
        # use identical logic, regardless of what scanner version the base tree ships.
        base_set="$(SMATCHET_LINT_BASELINE_SET="" bash "$SELF" --root "$wt" --full 2>/dev/null | sed -n 's/^  //p' | grep -v $'^narrowing-conversions\t' || true)"
        git worktree remove --force "$wt" 2>/dev/null || true
    fi
    # New triples = HEAD \ base.
    new_triples="$(comm -23 <(printf '%s\n' "$head_set" | sort -u) <(printf '%s\n' "$base_set" | sort -u) | grep -E . || true)"

    rc=0
    # --- strict-zone high-integrity rules (delta-gated) ---
    if [ -n "$new_triples" ]; then
        rc=1
        echo
        echo "FAIL: new strict-zone high-integrity violations vs $BASE:"
        printf '%s\n' "$new_triples" | sed 's/^/  /'
        echo "  Fix it, or add SMATCHET_DEVIATION(rule=...; reason=...; owner=...; revisit=...) above the line."
    else
        echo "[test-lint-rules] PASS — no new strict-zone violations vs $BASE"
    fi

    # --- comment-regrowth rules (repo-wide, delta-gated; reduce-source-comment-bloat Phase 4) ---
    # New noise comments (commented-out code / decorative banner / blank-comment run) fail ANYWHERE
    # in first-party C++ (never legitimate). comment_audit.py --diff classifies; a
    # `// SMATCHET_DEVIATION(rule=comment-...)` on the line above escapes a flagged line.
    # Fail CLOSED: a hard-fail gate that silently no-ops (missing python / missing script / crash)
    # would pass dirty PRs as false-clean. comment_audit.py's exit contract: 0=clean, 1=violations
    # (on stdout), >=2=infra error. resolve_python validates the interpreter actually
    # runs (skips the Windows python3 Store-alias stub that exits 49).
    cr_out=""
    cr_aud="$REPO_ROOT/agents/scripts/core/comment_audit.py"
    cr_py="$(resolve_python || true)"
    if [ -z "$cr_py" ]; then
        echo "test-lint-rules: ERROR: no python interpreter; cannot enforce comment-regrowth gate" >&2
        exit 2
    fi
    if [ ! -f "$cr_aud" ]; then
        echo "test-lint-rules: ERROR: missing $cr_aud; cannot enforce comment-regrowth gate" >&2
        exit 2
    fi
    # Capture exit code inside the `if` condition: a bare `x=$(cmd); rc=$?` would, under the CI
    # shell's `set -e`, abort the whole script the instant comment_audit.py exits non-zero (e.g. 1
    # = violations found) — before `rc=$?` ever runs. An assignment used as an `if` condition is
    # exempt from `set -e`, so this reliably captures 0 / 1 / >=2.
    if cr_out="$("$cr_py" "$cr_aud" --diff "$BASE")"; then cr_rc=0; else cr_rc=$?; fi
    if [ "$cr_rc" -ge 2 ]; then
        echo "test-lint-rules: ERROR: comment_audit.py --diff failed (exit $cr_rc) for base '$BASE'" >&2
        exit 2
    fi
    if [ -n "$cr_out" ]; then
        rc=1
        echo
        echo "FAIL: new comment-noise vs $BASE (commented-out code / decorative banner / blank-comment run):"
        printf '%s\n' "$cr_out" | sed 's/^/  /'
        echo "  Delete the noise, or add SMATCHET_DEVIATION(rule=<comment-id>; reason=...; owner=...; revisit=...) above it."
    else
        echo "[test-lint-rules] PASS — no new comment-noise vs $BASE"
    fi

    # --- first-party-wide absolute rules (no-raw-new, deviation-overdue) ---
    # Enforced at 0 across ALL first-party C++ (Source/Core, Source/Plugins,
    # Source/Standalone — the comment_audit.py SWEEP_ROOTS), not just the strict
    # zone: every raw `new` must use make_unique or carry an exemption marker, and
    # no SMATCHET_DEVIATION may sit past its revisit= date, ANYWHERE. Absolute (no
    # grandfathering) — the tree is clean today, so any hit is a regression. The
    # other two grep rules stay strict-only: both have legitimate first-party uses
    # outside the strict zone (no-printf-stderr → Standalone CLI stdout; define-imgui
    # → the ImGui localization-alias macro in Ui).
    wide_out="$(compute_wide_violations)"
    if [ -n "$wide_out" ]; then
        rc=1
        echo
        echo "FAIL: first-party no-raw-new / deviation-overdue / no-detach (enforced everywhere, not just the strict zone):"
        printf '%s\n' "$wide_out" | sed 's/^/  /'
        echo "  no-raw-new: use std::unique_ptr + make_unique (or marker // C-ABI handle / // custom-deleter / // pimpl)."
        echo "  no-detach: route the worker through AppController::LaunchBackgroundTask (joined at shutdown), not std::thread().detach()."
        echo "  Or revisit the overdue SMATCHET_DEVIATION / add SMATCHET_DEVIATION(rule=<id>; reason=...; owner=...; revisit=...) above the line."
    else
        echo "[test-lint-rules] PASS — no first-party no-raw-new / deviation-overdue / no-detach (whole tree)"
    fi

    # --- no-glfw-in-core-headers (Source/Core/include headers; ABSOLUTE-0) ---
    # GLFW/glad/OpenGL #include in a Source/Core/ header breaks the DX12 dual-target
    # build (SmatchetCore_DX12 compiles these headers with no GL/GLFW toolchain). The
    # tree is GLFW-clean today, so any hit is a regression — absolute-0, no grandfathering
    # (same model as no-raw-new). A SMATCHET_DEVIATION(rule=no-glfw-in-core-headers; ...)
    # above the include escapes.
    glfw_out="$(compute_glfw_violations)"
    if [ -n "$glfw_out" ]; then
        rc=1
        echo
        echo "FAIL: GLFW/glad/OpenGL include in a Source/Core/include header (breaks the DX12 dual-target build):"
        printf '%s\n' "$glfw_out" | sed 's/^/  /'
        echo "  Move the GL/GLFW include into a .cpp (or Source/Standalone), not a Source/Core/ header,"
        echo "  or add SMATCHET_DEVIATION(rule=no-glfw-in-core-headers; reason=...; owner=...; revisit=...) above the include."
    else
        echo "[test-lint-rules] PASS — no GLFW/glad/OpenGL include in Source/Core/include headers"
    fi

    # --- cmake-local-gate-ci-scope (CMakeLists.txt / cmake/*.cmake; ABSOLUTE-0) ---
    # A message(FATAL_ERROR ...) keyed on a LOCAL-dev project.config knob (msvc_toolset_pin)
    # without a NOT DEFINED ENV{CI} scope FATALs every fresh-configure CI runner (incident
    # #1074 red-walled all 5 Windows required checks). The tree's one such guard is correctly
    # CI-scoped today, so any unguarded hit is a regression (absolute-0, no grandfathering —
    # same model as no-glfw). A `# SMATCHET_DEVIATION(rule=cmake-local-gate-ci-scope; ...)`
    # within the guard window escapes.
    cmci_out="$(compute_cmake_ci_scope_violations)"
    if [ -n "$cmci_out" ]; then
        rc=1
        echo
        echo "FAIL: message(FATAL_ERROR ...) keyed on a LOCAL-dev knob (msvc_toolset_pin) without a NOT DEFINED ENV{CI} scope (FATALs every fresh-configure CI runner; incident #1074):"
        printf '%s\n' "$cmci_out" | sed 's/^/  /'
        echo "  Scope the guard to local dev: 'if(... AND NOT DEFINED ENV{CI})' (CI configures fresh + can't hit the stale-cache class),"
        echo "  or add '# SMATCHET_DEVIATION(rule=cmake-local-gate-ci-scope; reason=...; owner=...; revisit=...)' within the guard block."
    else
        echo "[test-lint-rules] PASS — no un-CI-scoped local-knob CMake FATAL_ERROR"
    fi

    # --- ui-request-flag-off-thread (Source/Core/src/Commands, excl. Scenarios; ABSOLUTE-0) ---
    # A write to a g_ui request-flag field (requestWindow* / requestScreenshot*) from a command
    # handler must be marshalled onto the UI thread via RunOnUiThread* — the standalone main loop
    # polls those non-atomic fields each frame, so an unsynchronised write from an MCP/Lua worker
    # thread is a data race (UB — Pillar-3 never-crash). The command handlers all marshal correctly
    # today (BuiltinCommands_Debug.cpp), so any unwrapped write is a regression (absolute-0, no
    # grandfathering — same model as no-glfw). Scenarios/ is exempt (IScenario lifecycle runs on the
    # UI thread by contract). A SMATCHET_DEVIATION(rule=ui-request-flag-off-thread; ...) above escapes.
    uireq_out="$(compute_ui_request_flag_violations)"
    if [ -n "$uireq_out" ]; then
        rc=1
        echo
        echo "FAIL: g_ui request-flag write outside a RunOnUiThread* closure in a command-dispatch TU (races the main loop that polls these non-atomic fields — Pillar-3 data race):"
        printf '%s\n' "$uireq_out" | sed 's/^/  /'
        echo "  Wrap the write in RunOnUiThreadAsCommandResult(app, [...]{ ...write... }) (see BuiltinCommands_Debug.cpp),"
        echo "  or add SMATCHET_DEVIATION(rule=ui-request-flag-off-thread; reason=...; owner=...; revisit=...) above the write."
    else
        echo "[test-lint-rules] PASS — no off-UI-thread g_ui request-flag write in command-dispatch TUs"
    fi

    # --- no-ui-include-in-domain (domain subsystems; ABSOLUTE-0) ---
    # A quote-form `#include "Ui/..."` in a DOMAIN subsystem (Tracker/Sync/Persistence/Config +
    # include mirrors + Plugins/Mcp) inverts the architecture layer DAG (Ui ranks above every domain
    # layer) and compile-couples backend logic to the render layer. The include-cycle gate's
    # back-edge check is header->header by design, so a domain .cpp -> Ui/ header edge escaped it
    # (the TouchCellEditGesture.h class). The domain dirs are Ui-clean today, so any hit is a
    # regression (absolute-0, no grandfathering — same model as no-glfw / ui-request-flag).
    # Commands/ is out of scope (sanctioned Scenario/view-visibility Ui seams — see the rule module).
    # A SMATCHET_DEVIATION(rule=no-ui-include-in-domain; ...) above the include escapes.
    uii_out="$(compute_ui_include_direction_violations)"
    if [ -n "$uii_out" ]; then
        rc=1
        echo
        echo "FAIL: Ui/ header included from a domain subsystem (layer inversion — domain code must never depend on the Ui layer):"
        printf '%s\n' "$uii_out" | sed 's/^/  /'
        echo "  Relocate the shared logic to a layer-neutral leaf header (Source/Core/include/ root — see TouchCellEditGesture.h),"
        echo "  or invert via an interface the Ui layer implements,"
        echo "  or add SMATCHET_DEVIATION(rule=no-ui-include-in-domain; reason=...; owner=...; revisit=...) above the include."
    else
        echo "[test-lint-rules] PASS — no Ui/ include in domain subsystems (Tracker/Sync/Persistence/Config/Mcp)"
    fi

    # --- unused-symbol-under-config-guard (CHANGED first-party .cpp; WARN-first) ---
    # A free-function definition unguarded while ALL its references sit inside a
    # POSITIVE #if defined(SMATCHET_WITH_*) block is dead in the feature-OFF build and
    # trips Clang's /WX -Werror,-Wunused-function (invisible to the PR-time MSVC
    # presets) — the #863 config-skew gate escape (fixed by 61b17427 / #945).
    #
    # WARN-FIRST / ADVISORY (calibration phase, mirrors the `duplication` + `interface-doc`
    # gates; never touches $rc) — and scoped to the files CHANGED in this diff, not the
    # whole tree. Rationale (plan § Risks + § Verification fallback): the heuristic is a
    # per-file text proxy, not the compiler, and the real tree carries a handful of
    # benign idioms it cannot statically distinguish from the #863 shape — a helper
    # called only in a SMATCHET_WITH_MCP path inside a TU that is itself MCP-gated, a
    # def whose only ref is in the #else of a `#if !defined(...)`. Shipping this as an
    # absolute-0 block would red-wall develop on those idioms; shipping it WARN-first
    # surfaces the #863 shape at PR time (the signal the gate exists for) without that
    # risk. The nightly Lua-OFF sanitizer build stays the authoritative backstop. It
    # graduates to a blocking rule once the FP rate is calibrated low (same path as the
    # DRY dup gate). A `// SMATCHET_DEVIATION(rule=unused-symbol-under-config-guard; ...)`
    # above the def suppresses. Diff-scoped keeps it fast (per-file scan is heavy on the
    # 2000+-line monoliths) and focuses the WARN on newly-introduced code.
    uscg_mb="$(git merge-base "$BASE" HEAD 2>/dev/null || echo "$BASE")"
    uscg_changed="$(git diff --name-only --diff-filter=d "$uscg_mb" 2>/dev/null \
        | grep -E '\.cpp$' | grep -vE '(^|/)ThirdParty/' || true)"
    if [ -n "$uscg_changed" ]; then
        uscg_warn=""
        while IFS= read -r uscg_f; do
            [ -n "$uscg_f" ] || continue
            uscg_warn="$uscg_warn$(scan_unused_under_config_guard_file "$uscg_f")"$'\n'
        done <<< "$uscg_changed"
        uscg_warn="$(printf '%s' "$uscg_warn" | grep -E . || true)"
        if [ -n "$uscg_warn" ]; then
            {
                echo "[unused-symbol-under-config-guard] WARN: a free-function definition appears unguarded while all its in-file references sit under a positive #if defined(SMATCHET_WITH_*) guard — it would be dead (Clang -Werror,-Wunused-function) in the feature-OFF build (the #863 config-skew class). Advisory (calibration); not blocking:"
                printf '%s\n' "$uscg_warn" | sed 's/^/  /'
                echo "  If real: wrap the definition in the SAME #if defined(SMATCHET_WITH_...) as its call sites (see 61b17427 / #945)."
                echo "  If a false positive (e.g. an out-of-line member, an #else-branch impl, or a cross-TU helper): add"
                echo "  // SMATCHET_DEVIATION(rule=unused-symbol-under-config-guard; reason=...; owner=...; revisit=...) above the definition."
            } >&2
        fi
    fi

    # --- bare-json-parse-untrusted (ALL first-party C++, repo-wide default-deny; BLOCKING) ---
    # A bare nlohmann::json::parse( — or a `stream >> json` slurp — that does NOT route through
    # smatchet::json_safe::ParseBounded crashes uncatchably on a deeply-nested payload: nlohmann
    # builds the DOM iteratively but the RECURSIVE ~json teardown overflows the stack before any
    # try/catch can fire (issues #1271/#1287). The 3-arg non-throwing form (..., nullptr, false)
    # still builds the DOM and still overflows, so only ParseBounded escapes. BLOCKING (sets $rc=1).
    # Repo-wide default-deny (replaced the curated BARE_JSON_INGRESS_TUS allow-list, which lagged
    # the code every time the class recurred — #1573/#1592/#1598 all fixed TUs the list did not
    # watch); headers are in scope too (the JsonParseUtil.h class). A SMATCHET_DEVIATION(rule=
    # bare-json-parse-untrusted; ...) above the parse suppresses (bytes the program itself
    # serialised). The tree is clean today, so any hit in a changed file is a regression.
    # Diff-scoped to the CHANGED files (fast path); the whole-tree set is available via
    # --scan-bare-json for a campaign sweep and is asserted empty by the gate's bats.
    barejson_mb="$(git merge-base "$BASE" HEAD 2>/dev/null || echo "$BASE")"
    barejson_changed="$(git diff --name-only --diff-filter=d "$barejson_mb" 2>/dev/null \
        | grep -E '\.(cpp|h|hpp)$' | grep -vE '(^|/)ThirdParty/' \
        | grep -E '^Source/(Core|Plugins|Standalone)/' || true)"
    barejson_out=""
    if [ -n "$barejson_changed" ]; then
        while IFS= read -r barejson_f; do
            [ -n "$barejson_f" ] || continue
            barejson_out="$barejson_out$(scan_bare_json_parse_file "$barejson_f")"$'\n'
        done <<< "$barejson_changed"
        barejson_out="$(printf '%s' "$barejson_out" | grep -E . || true)"
    fi
    if [ -n "$barejson_out" ]; then
        {
            echo "[bare-json-parse-untrusted] FAIL: a bare nlohmann::json::parse( (or stream>>json slurp) appears in first-party C++ without routing through smatchet::json_safe::ParseBounded — a deeply-nested payload stack-overflows the recursive ~json DOM teardown before any try/catch can fire (#1271/#1287). Blocking, repo-wide (the curated-TU allow-list is retired):"
            printf '%s\n' "$barejson_out" | sed 's/^/  /'
            echo "  Route the decode through smatchet::json_safe::ParseBounded(text, errOut[, maxBytes]) or ParseBoundedOrDiscarded(text) (#include \"Json/BoundedJsonParse.h\")."
            echo "  If the bytes are provably program-internal (not external input): add"
            echo "  // SMATCHET_DEVIATION(rule=bare-json-parse-untrusted; reason=...; owner=...; revisit=...) above the parse."
        } >&2
        rc=1
    fi

    # --- catch-all-swallow (ALL first-party C++; ABSOLUTE-0, BLOCKING) ---
    # An EMPTY catch (...) {} body silently swallows every exception — exception-handling-policy.md
    # hard rule 1 (review CRITICAL). The editor hook (lint-catch-all.py) flags it per-edit but is
    # not a merge gate; this closes that hole. The tree is at 0 today, so any hit is a regression
    # (absolute-0, no grandfathering — same model as no-glfw / no-raw-new). A comment inside the
    # body (documented silence), `// catch-all-ok: <reason>` on the catch line, or a
    # SMATCHET_DEVIATION(rule=catch-all-swallow; ...) above it escapes.
    catchall_out="$(compute_catch_all_violations)"
    if [ -n "$catchall_out" ]; then
        rc=1
        echo
        echo "FAIL: empty catch (...) body (swallows every exception silently — exception-handling-policy.md hard rule 1):"
        printf '%s\n' "$catchall_out" | sed 's/^/  /'
        echo "  Log with operation context (LOG_WARN/LOG_ERROR + function/key/backend), rethrow, or document the"
        echo "  silence with an inline comment / '// catch-all-ok: <reason>' per the policy's escape hatches,"
        echo "  or add SMATCHET_DEVIATION(rule=catch-all-swallow; reason=...; owner=...; revisit=...) above the catch."
    else
        echo "[test-lint-rules] PASS — no empty catch (...) body in first-party C++ (whole tree)"
    fi

    # --- unbounded-recursive-json-walker (CHANGED first-party C++; WARN-first) ---
    # The DW class from the security campaign (#1220/#1237): a self-recursive walker over a
    # nlohmann::json / sol::object parameter with no depth/budget bound overflows the stack on deep
    # nesting even when the parse was bounded. WARN-FIRST / ADVISORY (calibration, mirrors the
    # duplication precedent; never touches $rc) and diff-scoped to changed files — the two known
    # residuals (FieldCatalogCache::OptionFromJson, TrackerFieldValueParser::TrackerFieldOptionFromJson)
    # are transitively depth-bounded by ParseBounded's 256 cap upstream, so they warn only when
    # touched. Whole-tree sweep via --scan-json-walkers. A SMATCHET_DEVIATION(rule=
    # unbounded-recursive-json-walker; ...) above the definition suppresses.
    jw_changed="$(printf '%s\n' "$barejson_changed" | grep -E . || true)"
    jw_out=""
    if [ -n "$jw_changed" ]; then
        while IFS= read -r jw_f; do
            [ -n "$jw_f" ] || continue
            jw_out="$jw_out$(scan_json_walker_file "$jw_f")"$'\n'
        done <<< "$jw_changed"
        jw_out="$(printf '%s' "$jw_out" | grep -E . || true)"
    fi
    if [ -n "$jw_out" ]; then
        {
            echo "[unbounded-recursive-json-walker] WARN: a self-recursive function over a nlohmann::json / sol::object parameter carries no depth/budget bound — a deeply-nested value overflows the C++ stack even when the parse was bounded (the DW class; #1220/#1237). Advisory (calibration); not blocking:"
            printf '%s\n' "$jw_out" | sed 's/^/  /'
            echo "  Thread an int depth parameter (bail past ~256, mirroring kMaxAdfRecursionDepth) or rewrite to an explicit work-stack."
            echo "  If a false positive (bounded wrapper / mutual recursion misread): add"
            echo "  // SMATCHET_DEVIATION(rule=unbounded-recursive-json-walker; reason=...; owner=...; revisit=...) above the definition."
        } >&2
    fi

    # --- unbounded-file-slurp (CHANGED first-party C++; WARN-first) ---
    # A whole-file rdbuf()/istreambuf slurp with no byte cap reads an arbitrarily large file into
    # memory (the SECURITY_AUDIT #33 Win32-vs-POSIX cap asymmetry class). WARN-FIRST / ADVISORY
    # (calibration; never touches $rc), diff-scoped; residual sites warn only when touched.
    # Whole-tree sweep via --scan-slurps. A SMATCHET_DEVIATION(rule=unbounded-file-slurp; ...)
    # above the read suppresses.
    slurp_out=""
    if [ -n "$jw_changed" ]; then
        while IFS= read -r slurp_f; do
            [ -n "$slurp_f" ] || continue
            slurp_out="$slurp_out$(scan_file_slurp_file "$slurp_f")"$'\n'
        done <<< "$jw_changed"
        slurp_out="$(printf '%s' "$slurp_out" | grep -E . || true)"
    fi
    if [ -n "$slurp_out" ]; then
        {
            echo "[unbounded-file-slurp] WARN: a whole-file rdbuf()/istreambuf_iterator slurp has no byte cap — an oversized file is read fully into memory before any validation (SECURITY_AUDIT #33 class). Advisory (calibration); not blocking:"
            printf '%s\n' "$slurp_out" | sed 's/^/  /'
            echo "  Prefer a size-capped read (stat/tellg + reject over-limit, or ConfigManager::LoadJsonFile which caps at 64 MiB)."
            echo "  If the file is trusted/small by construction: add"
            echo "  // SMATCHET_DEVIATION(rule=unbounded-file-slurp; reason=...; owner=...; revisit=...) above the read."
        } >&2
    fi

    # --- pr-numbered-temporal-comments (CHANGED first-party C++ comments; WARN-first) ---
    # A comment that pins a DEVELOPMENT pull-request number (`// PR 5`, `// PR #1104`, `PR#1218`,
    # `PR12`) is a temporal scaffold — it rots the moment the PR squash-merges and the number is
    # forgotten. Rewrite to durable present-tense intent (drop the PR-number token; keep the meaning).
    # Product-domain "PR" usages with no number ("PR-only", "type:pr", "per-PR") do NOT match.
    # WARN-FIRST / ADVISORY (calibration; never touches $rc), diff-scoped to the CHANGED first-party
    # C++ files (mirrors the bare-json + unused-symbol WARNs). A SMATCHET_DEVIATION(rule=pr-numbered-
    # temporal-comments; ...) on the line above suppresses. Whole-tree sweep via --scan-pr-comments.
    prc_mb="$(git merge-base "$BASE" HEAD 2>/dev/null || echo "$BASE")"
    prc_changed="$(git diff --name-only --diff-filter=d "$prc_mb" 2>/dev/null \
        | grep -E '\.(cpp|h|hpp)$' | grep -vE '(^|/)ThirdParty/' || true)"
    prc_out=""
    if [ -n "$prc_changed" ]; then
        while IFS= read -r prc_f; do
            [ -n "$prc_f" ] || continue
            prc_out="$prc_out$(scan_pr_comment_file "$prc_f")"$'\n'
        done <<< "$prc_changed"
        prc_out="$(printf '%s' "$prc_out" | grep -E . || true)"
    fi
    if [ -n "$prc_out" ]; then
        {
            echo "[pr-numbered-temporal-comments] WARN: a comment pins a development PR number (// PR <n> / PR#<n> / PRn) — a temporal scaffold that rots once the PR squash-merges. Rewrite to durable present-tense intent (drop the PR-number token, keep the meaning). Advisory (calibration); not blocking:"
            printf '%s\n' "$prc_out" | sed 's/^/  /'
            echo "  Rephrase the comment to state what the code does / why, without the dev-PR number (keep GitHub Issue / ADR refs)."
            echo "  If a specific historical PR genuinely must be cited (e.g. an audit trail): add"
            echo "  // SMATCHET_DEVIATION(rule=pr-numbered-temporal-comments; reason=...; owner=...; revisit=...) above the comment."
        } >&2
    fi

    # --- function-size rules (repo-wide, delta-gated, TIERED; decompose-top-20-monoliths Slice 0) ---
    # New functions over the hard cap (>120 lines non-UI / >200 lines ImGui-draw / >30 branches) —
    # or existing ones that JUST crossed it — fail anywhere in first-party C++. function_size_audit.py
    # keys by (rule, basename, qualified-name) and diffs HEAD vs the merge-base of $BASE, so the
    # existing monoliths are grandfathered (a grandfathered function growing further stays
    # grandfathered — same model as the comment-regrowth rules; lowering non-UI to 120 does NOT
    # retroactively fire existing 120-200-line functions — they're in both the HEAD and base sets).
    # Fail CLOSED on infra error, identical contract to the comment gate (0 clean / 1 violations /
    # >=2 infra). A `// SMATCHET_DEVIATION(rule=function-too-long; ...)` above the signature
    # suppresses one. The advisory soft-warning tier (>100 lines / >20 branches) is printed to
    # STDERR by the audit script, so it surfaces to the user but never enters $fs_out / the exit
    # code. $cr_py is the validated interpreter from above.
    fs_aud="$REPO_ROOT/agents/scripts/core/function_size_audit.py"
    if [ ! -f "$fs_aud" ]; then
        echo "test-lint-rules: ERROR: missing $fs_aud; cannot enforce function-size gate" >&2
        exit 2
    fi
    if fs_out="$("$cr_py" "$fs_aud" --diff "$BASE")"; then fs_rc=0; else fs_rc=$?; fi
    if [ "$fs_rc" -ge 2 ]; then
        echo "test-lint-rules: ERROR: function_size_audit.py --diff failed (exit $fs_rc) for base '$BASE'" >&2
        exit 2
    fi
    if [ -n "$fs_out" ]; then
        rc=1
        echo
        echo "FAIL: new oversized functions vs $BASE (cap 120 lines non-UI / 200 lines ImGui-draw / 30 branches):"
        printf '%s\n' "$fs_out" | sed 's/^/  /'
        echo "  Decompose it (see docs/plans/shipped/decompose-top-20-monoliths.md § Approach), or add"
        echo "  SMATCHET_DEVIATION(rule=function-too-long; reason=...; owner=...; revisit=...) above the signature"
        echo "  (comma-separate the rule ids — rule=function-too-long,function-too-branchy — to suppress both caps)."
    else
        echo "[test-lint-rules] PASS — no new oversized functions vs $BASE"
    fi

    # --- include-cycle gate (Source/Core quote-include graph; delta-gated; BLOCKS) ---
    # A NEW SCC>1 include cycle OR a NEW low->high named-layer header back-edge in the Source/Core
    # quote-include graph fails the gate (core-include-dag Phase 0). include_cycle_audit.py keys each
    # violation by includer->resolved-target (path-pair, line-independent) and diffs HEAD vs the
    # merge-base of $BASE, so the current grandfathered edges (the ~5 baselined in
    # docs/high-integrity/include-cycle-baseline.md) stay green and only a genuinely-new cycle/back-edge
    # fires. Unlike the dup gate this FAILS CLOSED (exit 1 on a new violation) — the acyclicity ratchet
    # is the load-bearing deliverable that keeps the cleanup from regressing. A
    # `// SMATCHET_DEVIATION(rule=include-cycle; ...)` on the nearest non-blank line above the offending
    # #include suppresses it. Fail CLOSED on infra error too, identical contract to the function-size
    # gate (0 clean / 1 violations / >=2 infra). $cr_py is the validated interpreter from above.
    ic_aud="$REPO_ROOT/agents/scripts/core/include_cycle_audit.py"
    if [ ! -f "$ic_aud" ]; then
        echo "test-lint-rules: ERROR: missing $ic_aud; cannot enforce include-cycle gate" >&2
        exit 2
    fi
    if ic_out="$("$cr_py" "$ic_aud" --diff "$BASE")"; then ic_rc=0; else ic_rc=$?; fi
    if [ "$ic_rc" -ge 2 ]; then
        echo "test-lint-rules: ERROR: include_cycle_audit.py --diff failed (exit $ic_rc) for base '$BASE'" >&2
        exit 2
    fi
    if [ -n "$ic_out" ]; then
        rc=1
        echo
        printf '%s\n' "$ic_out" | sed 's/^/  /'
    else
        echo "[test-lint-rules] PASS — no new Source/Core include cycle / layer back-edge vs $BASE"
    fi

    # --- AppController.h fan-in ratchet (Source/-wide quote-includers; delta-gated; BLOCKS) ---
    # A NEW Source/-wide quote-form `#include "AppController.h"` includer above the merge-base count
    # fails the gate (appcontroller-fan-in Phase 1). appcontroller_fan_in_audit.py --diff is DOWN-only
    # and hard-FAILs (exit 1) on a regression; a `// SMATCHET_DEVIATION(rule=app-controller-fan-in; ...)`
    # above the offending #include escapes a genuinely-needed new includer. Fails CLOSED on infra error
    # (0 clean / 1 new includer / >=2 infra), same contract as the include-cycle gate above.
    fi_aud="$REPO_ROOT/agents/scripts/core/appcontroller_fan_in_audit.py"
    if [ ! -f "$fi_aud" ]; then
        echo "test-lint-rules: ERROR: missing $fi_aud; cannot enforce app-controller-fan-in gate" >&2
        exit 2
    fi
    if fi_out="$("$cr_py" "$fi_aud" --diff "$BASE")"; then fi_rc=0; else fi_rc=$?; fi
    if [ "$fi_rc" -ge 2 ]; then
        echo "test-lint-rules: ERROR: appcontroller_fan_in_audit.py --diff failed (exit $fi_rc) for base '$BASE'" >&2
        exit 2
    fi
    if [ -n "$fi_out" ]; then
        rc=1
        echo
        printf '%s\n' "$fi_out" | sed 's/^/  /'
    else
        echo "[test-lint-rules] PASS — no new AppController.h includer (fan-in ratchet) vs $BASE"
    fi

    # --- soft comment-ratio warning (ADVISORY — never changes exit code) ---
    ratio_warn_for "$BASE" || true

    # --- duplication BLOCKING (DRY Engineering Pillar 5; fails CLOSED) ---
    # dup_audit.py --diff graduated WARN→blocking on 2026-06-21 (ADR-0015 calibration complete): it
    # prints [dup] FAIL lines to stderr for NEW cross-file copy-paste clones and exits 1, which now
    # FAILS the gate ($rc=1). An infra error (>=2) also fails CLOSED. Exempt a genuine NEW clone with
    # a // SMATCHET_DEVIATION(rule=duplication; ...) marker on/above either occurrence.
    dup_aud="$REPO_ROOT/agents/scripts/core/dup_audit.py"
    if [ -f "$dup_aud" ] && [ -n "$cr_py" ]; then
        if ! "$cr_py" "$dup_aud" --diff "$BASE"; then
            echo "test-lint-rules: FAIL: NEW copy-paste clone(s) (rule=duplication, DRY Pillar 5) — de-duplicate or exempt with SMATCHET_DEVIATION(rule=duplication)" >&2
            rc=1
        fi
    fi

    # --- interface-doc WARN (Gap B / Slice 2; ADVISORY — never changes exit code) ---
    # Symbol-pinned: WARNs when a leaf-doc-embedded `Type::method` token appears in a changed
    # interface header's diff hunk without the doc being touched (the stale-contract-in-leaf-doc
    # class — e.g. the ITrackerIssueMutations::UpdateField signature slip). Pure-bash, no python,
    # no signature parsing; stderr-only, $rc untouched. See INTERFACE_DOC_MAP for the curated map.
    interface_doc_warn "$BASE"

    # --- agent-prompt / AGENTS.md size rule (delta-gated; reduce-agent-prompt-bloat Slice 0) ---
    # New agent prompts (agents/core, agents/project) over 250 lines — or AGENTS.md over 150, or an
    # existing one that JUST crossed its cap — fail. agent_size_audit.py keys by (rule, path) and
    # diffs HEAD vs the merge-base of $BASE, so the current whales (debug-detective, git-janitor,
    # test-author, AGENTS.md) are grandfathered (a grandfathered file shrinking — or even growing —
    # stays grandfathered; only a NEW over-cap file or an under->over crossing fires). The sink
    # classes (docs/agent-rules, agents/_shared/skills) are soft-warn-only and never enter $rc. Fail
    # CLOSED on infra error, identical contract to the function-size gate (0 clean / 1 violations /
    # >=2 infra). A line reading SMATCHET_DEVIATION(rule=agent-too-long; ...) anywhere in the file
    # suppresses it. Advisory [agent-size] WARN lines go to STDERR (never enter $as_out / exit code).
    # NB: this gate ALSO runs as its own required-check step in doc-validation.yml, which (unlike the
    # Windows build job that hosts this script) fires on docs-only PRs — the exact agent-regrowth
    # vector. Keeping it here too covers code PRs + local test-all.sh. $cr_py is the validated interp.
    as_aud="$REPO_ROOT/agents/scripts/core/agent_size_audit.py"
    if [ ! -f "$as_aud" ]; then
        echo "test-lint-rules: ERROR: missing $as_aud; cannot enforce agent-size gate" >&2
        exit 2
    fi
    if as_out="$("$cr_py" "$as_aud" --diff "$BASE")"; then as_rc=0; else as_rc=$?; fi
    if [ "$as_rc" -ge 2 ]; then
        echo "test-lint-rules: ERROR: agent_size_audit.py --diff failed (exit $as_rc) for base '$BASE'" >&2
        exit 2
    fi
    if [ -n "$as_out" ]; then
        rc=1
        echo
        echo "FAIL: new over-budget agent prompts / AGENTS.md vs $BASE (cap 250 lines prompt / 150 lines AGENTS.md):"
        printf '%s\n' "$as_out" | sed 's/^/  /'
        echo "  Extract procedure-bodies to a skill (see docs/agent-rules/AGENT-VS-SKILL.md), or add"
        echo "  SMATCHET_DEVIATION(rule=agent-too-long; reason=...; owner=...; revisit=...) in the file."
    else
        echo "[test-lint-rules] PASS — no new over-budget agent prompts / AGENTS.md vs $BASE"
    fi

    exit "$rc"
    ;;
esac

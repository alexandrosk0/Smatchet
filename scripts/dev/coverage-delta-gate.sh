#!/usr/bin/env bash
# coverage-delta-gate.sh — refuse Source/Core/ diffs without matching test deltas.
#
# Per-PR enforcement: if the current branch's diff against `develop` (or the
# configured base) touches any production file under Source/Core/src/ AND
# touches zero test files under tests/, the gate exits 1 with a diagnostic —
# UNLESS the diff's product-code change is provably no-new-runtime-surface (see
# the test-light exemption pre-check below), in which case it PASSES legitimately
# without needing the tests-out-of-band override.
#
# The CI workflow checks for the `tests-out-of-band` PR label and dismisses
# this gate when present; the bash script itself is label-unaware (label
# inspection requires the GitHub event payload, which is workflow-level). The
# label stays as a manual escape for genuine cases the classifier can't cover.
#
# Test-light exemption (no override, no postmortem) — auto-PASS a diff whose
# *every* added/modified line in first-party C/C++ product files
# (.cpp/.h/.hpp/.cc/.cxx under Source/Core, Source/Plugins, Source/Standalone,
# tests/) is provably no-new-runtime-surface. Classes (CONSERVATIVE — anything
# not on this list falls through to the normal coverage-delta gate):
#   * comment/marker-only  — //, /* */, doc-* continuation, // catch-all-ok: …
#   * logging-only         — LOG_{DEBUG,INFO,WARN,ERROR,TRACE}(…) calls
#   * static_assert-only   — static_assert(…) (compile-time; the build is the test)
#   * include/using-only   — #include / using directives
#   * catch-scaffold       — exception-handler structure (catch (…) { , try { ,
#                            and the brace/closing tokens) whose body is only the
#                            above (the swallow→log pattern: no rethrow, no logic)
#   * build-only           — no .cpp/.h/.hpp product change at all (CMake/yml/sh/…)
# A new function, a new branch, a changed condition, a new statement — NOT exempt.
# Motivation: a GitHub merge queue runs this required check on the merge_group
# ref where PR labels don't apply, so tests-out-of-band can't dismiss it there;
# the gate must PASS legitimately for genuinely-untestable correctness diffs.
# See docs/plans/build-quality-velocity-hardening.md #14 + postmortems.md 2026-06-06.
#
# Override mechanism for local runs:
#   SMATCHET_COVERAGE_GATE_BASE   base ref to diff against. Default: origin/develop
#                                 (falls back to develop, then HEAD~1).
#   SMATCHET_COVERAGE_GATE_BYPASS set to 1 to short-circuit (advisory mode).
#
# Self-test (both-direction fixtures, no network):
#   bash scripts/dev/coverage-delta-gate.sh --selftest
#
# Exit codes:
#   0 — gate satisfied (no Source/Core change, test files also changed, or the
#       test-light exemption fired)
#   1 — gate failed (Source/Core changed without test delta and not exempt) /
#       --selftest failure

set -euo pipefail

# ---------------------------------------------------------------------------
# Test-light exemption classifier
# ---------------------------------------------------------------------------
# Decide whether a single added/modified C/C++ line (already stripped of its
# leading diff '+' and surrounding whitespace) is no-new-runtime-surface.
# Returns 0 (exempt) / 1 (real surface). CONSERVATIVE: unknown ⇒ 1.
#
# Block-comment state is tracked by the caller (in_block_comment) because a
# multi-line /* … */ spans lines; this helper only judges single-line shapes.
_line_is_no_runtime_surface() {
    local line="$1"

    # Blank line — no surface.
    [ -z "$line" ] && return 0

    # Whole-line single-line block comment: /* … */
    case "$line" in
        '/*'*'*/') return 0 ;;
    esac

    # Line comment / doc-comment continuation / block-comment open or close.
    #   //…            line comment
    #   /*…            block comment open (may not close on this line)
    #   *… or */       continuation / close of a block comment
    case "$line" in
        '//'*) return 0 ;;
        '/*'*) return 0 ;;
        '*/'*) return 0 ;;
        '*'*)  return 0 ;;
    esac

    # Strip a trailing line-comment so an exempt token followed by `// note`
    # still classifies (e.g. `} catch (...) { // catch-all-ok: …`). Only strip
    # `//` (a `/*…*/` mid-line is unusual in product code and we stay strict).
    local code="${line%%//*}"
    # Trim trailing whitespace left by the strip.
    code="${code%"${code##*[![:space:]]}"}"
    [ -z "$code" ] && return 0

    # #include / #pragma once / using directive.
    case "$code" in
        '#include'*) return 0 ;;
        '#pragma once'*) return 0 ;;
        'using '*) return 0 ;;
    esac

    # static_assert(…) — compile-time; the build is the test.
    case "$code" in
        'static_assert('*) return 0 ;;
    esac

    # Logging-only — LOG_{DEBUG,INFO,WARN,ERROR,TRACE}(…). Must be the start of
    # the statement (a LOG_ embedded as an argument would have other tokens
    # before it, which we don't exempt here — conservative).
    case "$code" in
        'LOG_DEBUG('*|'LOG_INFO('*|'LOG_WARN('*|'LOG_ERROR('*|'LOG_TRACE('*) return 0 ;;
        # Continuation of a multi-line LOG_ call argument list (string literal /
        # closing paren on its own line). A bare closing `");` or a quoted
        # fragment is scaffold for the call above; real statements would carry
        # an identifier + operator. Be strict: only a lone `");`-ish tail or a
        # pure string-literal continuation.
        '");'|');') return 0 ;;
        '"'*'"'|'"'*'",'|'"'*'");') return 0 ;;
    esac

    # catch-scaffold — exception-handler structure with no logic of its own.
    # The swallow→log pattern adds a `} catch (...) {` / `catch (const T& e) {`
    # plus a logging body (handled above). Lone braces / try open also scaffold.
    case "$code" in
        'try'|'try {'|'} try {') return 0 ;;
        'catch'*'{'|'} catch'*'{') return 0 ;;
        '{'|'}'|'};') return 0 ;;
    esac

    # Anything else is real runtime surface.
    return 1
}

# Read a unified diff on stdin; emit "EXEMPT" or "FALLTHROUGH" on stdout.
# EXEMPT  ⇒ every added line in a first-party C/C++ product file is
#           no-new-runtime-surface (or there are zero such added lines —
#           build-only). The caller short-circuits to PASS.
# FALLTHROUGH ⇒ at least one added C/C++ product line is real surface; the
#           caller runs the unchanged coverage-delta logic.
#
# Scope: only .cpp/.h/.hpp/.cc/.cxx under Source/Core, Source/Plugins,
# Source/Standalone, tests/. Lines in other files (build/docs/scripts/non-product
# C++) are ignored for the purposes of this classifier — they carry no runtime
# surface the gate enforces, so they neither block nor force a fallthrough.
_classify_diff() {
    local cur_file=""
    local in_product_cpp=0
    local in_block_comment=0
    local saw_real_surface=0
    local raw line code

    while IFS= read -r raw; do
        case "$raw" in
            '+++ '*)
                # New-file header: +++ b/<path>  (or /dev/null on delete)
                cur_file="${raw#+++ }"
                cur_file="${cur_file#b/}"
                in_product_cpp=0
                in_block_comment=0
                case "$cur_file" in
                    Source/Core/*.cpp|Source/Core/*.h|Source/Core/*.hpp|Source/Core/*.cc|Source/Core/*.cxx|\
                    Source/Plugins/*.cpp|Source/Plugins/*.h|Source/Plugins/*.hpp|Source/Plugins/*.cc|Source/Plugins/*.cxx|\
                    Source/Standalone/*.cpp|Source/Standalone/*.h|Source/Standalone/*.hpp|Source/Standalone/*.cc|Source/Standalone/*.cxx|\
                    tests/*.cpp|tests/*.h|tests/*.hpp|tests/*.cc|tests/*.cxx)
                        in_product_cpp=1 ;;
                esac
                continue ;;
            '--- '*) continue ;;
            'diff --git '*) in_block_comment=0; continue ;;
            '@@'*) in_block_comment=0; continue ;;
        esac

        # Only added lines matter. Skip context / removed / metadata.
        case "$raw" in
            '+'*) ;;   # added line (the leading + is the diff marker)
            *) continue ;;
        esac
        # Drop the leading '+'.
        line="${raw#+}"
        # Only product C/C++ files contribute surface.
        [ "$in_product_cpp" -eq 1 ] || continue

        # Trim leading/trailing whitespace.
        line="${line#"${line%%[![:space:]]*}"}"
        line="${line%"${line##*[![:space:]]}"}"

        # In a block comment: stays a comment until the close `*/`.
        if [ "$in_block_comment" -eq 1 ]; then
            case "$line" in
                *'*/'*) in_block_comment=0 ;;
            esac
            continue
        fi
        # Opening of a block comment that does not close on this line.
        case "$line" in
            '/*'*'*/'*) : ;;            # opens and closes — handled by helper
            '/*'*) in_block_comment=1; continue ;;
        esac

        if ! _line_is_no_runtime_surface "$line"; then
            saw_real_surface=1
            break
        fi
    done

    if [ "$saw_real_surface" -eq 1 ]; then
        echo "FALLTHROUGH"
    else
        echo "EXEMPT"
    fi
}

# ---------------------------------------------------------------------------
# --selftest — both-direction fixtures (mirrors the real target PRs)
# ---------------------------------------------------------------------------
if [ "${1:-}" = "--selftest" ]; then
    fail=0
    # _expect <EXEMPT|FALLTHROUGH> <label> <<diff
    _expect() {
        local want="$1" label="$2" got
        got="$(_classify_diff)"
        if [ "$got" = "$want" ]; then
            echo "  ok   [$want] $label"
        else
            echo "  FAIL [$want != $got] $label"
            fail=1
        fi
    }

    echo "coverage-delta-gate --selftest:"

    # ---- EXEMPT cases (must auto-PASS without override) ----

    # #907-equivalent — static_assert-only.
    _expect EXEMPT "static_assert-only (#907)" <<'EOF'
diff --git a/Source/Core/src/Ui/SmatchetImGuiFonts.cpp b/Source/Core/src/Ui/SmatchetImGuiFonts.cpp
--- a/Source/Core/src/Ui/SmatchetImGuiFonts.cpp
+++ b/Source/Core/src/Ui/SmatchetImGuiFonts.cpp
@@ -15,6 +15,9 @@
+// WCHAR32 ABI parity guard. A desync silently narrows ImWchar to 16-bit.
+static_assert(sizeof(ImWchar) == 4, "IMGUI_USE_WCHAR32 desynced: ImWchar must be 32-bit.");
EOF

    # #906-equivalent — swallow→log: new catch clause whose body is only LOG_*.
    _expect EXEMPT "logging-only swallow->log w/ new catch (#906)" <<'EOF'
diff --git a/Source/Core/src/Tracker/PlaneIssueMutation.cpp b/Source/Core/src/Tracker/PlaneIssueMutation.cpp
--- a/Source/Core/src/Tracker/PlaneIssueMutation.cpp
+++ b/Source/Core/src/Tracker/PlaneIssueMutation.cpp
@@ -100,7 +100,7 @@
-        } catch (...) {
+        } catch (...) { // catch-all-ok: error body not JSON — fall back to raw text
@@ -324,7 +324,13 @@
+    } catch (const std::exception& ex) {
+        // Network/API tier: created server-side but body did not parse —
+        // surface it instead of swallowing.
+        LOG_WARN("PlaneClient::CreateIssue: response JSON failed to parse: %s", ex.what());
     } catch (...) {
+        LOG_WARN("PlaneClient::CreateIssue: response JSON failed to parse: unknown exception");
EOF

    # LOG_WARN added inside an existing catch (no new clause).
    _expect EXEMPT "LOG_WARN in existing catch" <<'EOF'
diff --git a/Source/Core/src/Sync/Foo.cpp b/Source/Core/src/Sync/Foo.cpp
--- a/Source/Core/src/Sync/Foo.cpp
+++ b/Source/Core/src/Sync/Foo.cpp
@@ -10,6 +10,7 @@
     } catch (const std::exception& e) {
+        LOG_WARN("Foo failed: %s", e.what());
     }
EOF

    # Comment/marker-only.
    _expect EXEMPT "comment/marker-only" <<'EOF'
diff --git a/Source/Core/src/Config/Bar.cpp b/Source/Core/src/Config/Bar.cpp
--- a/Source/Core/src/Config/Bar.cpp
+++ b/Source/Core/src/Config/Bar.cpp
@@ -5,3 +5,6 @@
+// Explain the existing branch below; no behaviour change.
+/* A block comment
+   spanning two lines. */
EOF

    # Include/using-only (#3-style include cleanup).
    _expect EXEMPT "include/using-only" <<'EOF'
diff --git a/Source/Core/include/AppController.h b/Source/Core/include/AppController.h
--- a/Source/Core/include/AppController.h
+++ b/Source/Core/include/AppController.h
@@ -37,7 +37,11 @@
-#include "JiraClient.h"
+// Include real homes directly (drop the heavy cpr dependency JiraClient.h dragged in).
+#include "ConfigManager.h"
+#include "ITrackerConnectivity.h"
+using tracker::TrackerConfig;
EOF

    # CMake-only (#917-equivalent: build/CI/scripts, no C++ TU).
    _expect EXEMPT "build-only / CMake-only (#917)" <<'EOF'
diff --git a/CMakeLists.txt b/CMakeLists.txt
--- a/CMakeLists.txt
+++ b/CMakeLists.txt
@@ -10,3 +10,5 @@
+FetchContent_Declare(lua GIT_TAG abc123)
+add_compile_definitions(SMATCHET_FOO=1)
diff --git a/scripts/dev/test-lua-mirror-smoke.sh b/scripts/dev/test-lua-mirror-smoke.sh
--- a/scripts/dev/test-lua-mirror-smoke.sh
+++ b/scripts/dev/test-lua-mirror-smoke.sh
@@ -0,0 +1,3 @@
+#!/usr/bin/env bash
+echo "smoke"
EOF

    # ---- FALLTHROUGH cases (must NOT exempt — real runtime surface) ----

    # New function with branches.
    _expect FALLTHROUGH "new function w/ branches" <<'EOF'
diff --git a/Source/Core/src/Tracker/Baz.cpp b/Source/Core/src/Tracker/Baz.cpp
--- a/Source/Core/src/Tracker/Baz.cpp
+++ b/Source/Core/src/Tracker/Baz.cpp
@@ -10,0 +11,6 @@
+int classify(int x) {
+    if (x > 0) {
+        return 1;
+    }
+    return 0;
+}
EOF

    # New if-statement dropped into existing code.
    _expect FALLTHROUGH "new if-statement" <<'EOF'
diff --git a/Source/Core/src/Sync/Qux.cpp b/Source/Core/src/Sync/Qux.cpp
--- a/Source/Core/src/Sync/Qux.cpp
+++ b/Source/Core/src/Sync/Qux.cpp
@@ -20,2 +20,5 @@
     int n = compute();
+    if (n < 0) {
+        n = 0;
+    }
EOF

    # LOG_ mixed with a new for-loop — the loop is real surface.
    _expect FALLTHROUGH "LOG_ mixed with new for-loop" <<'EOF'
diff --git a/Source/Core/src/Config/Mix.cpp b/Source/Core/src/Config/Mix.cpp
--- a/Source/Core/src/Config/Mix.cpp
+++ b/Source/Core/src/Config/Mix.cpp
@@ -5,1 +5,5 @@
+    LOG_INFO("starting");
+    for (int i = 0; i < count; ++i) {
+        total += items[i];
+    }
EOF

    # New templated function (the #915 MainThreadDispatcherDrain reality — a
    # mix of include-only + a real new function ⇒ NOT exempt as a whole).
    _expect FALLTHROUGH "include-only mixed with a new function (#915 reality)" <<'EOF'
diff --git a/Source/Core/include/MainThreadDispatcherDrain.h b/Source/Core/include/MainThreadDispatcherDrain.h
--- a/Source/Core/include/MainThreadDispatcherDrain.h
+++ b/Source/Core/include/MainThreadDispatcherDrain.h
@@ -8,2 +8,6 @@
+#include <algorithm>
+#include <iterator>
+void RequeueDeferredFront(std::vector<T>& deferred, std::vector<T>& arrived, std::size_t maxSize, std::vector<T>& out) {
+    std::move(deferred.begin(), deferred.end(), std::back_inserter(out));
+}
EOF

    # A real statement disguised next to a comment — strictness check.
    _expect FALLTHROUGH "comment + a real assignment" <<'EOF'
diff --git a/Source/Core/src/Commands/Cmd.cpp b/Source/Core/src/Commands/Cmd.cpp
--- a/Source/Core/src/Commands/Cmd.cpp
+++ b/Source/Core/src/Commands/Cmd.cpp
@@ -5,1 +5,3 @@
+// adjust the cap
+maxRetries = maxRetries + 1;
EOF

    if [ "$fail" -eq 0 ]; then
        echo "coverage-delta-gate --selftest: PASS"
        exit 0
    fi
    echo "coverage-delta-gate --selftest: FAIL"
    exit 1
fi

# ---------------------------------------------------------------------------
# Normal run
# ---------------------------------------------------------------------------
cd "$(dirname "$0")/../.."

if [ "${SMATCHET_COVERAGE_GATE_BYPASS:-0}" = "1" ]; then
    echo "[coverage-delta-gate] BYPASS active (SMATCHET_COVERAGE_GATE_BYPASS=1)"
    exit 0
fi

# Resolve the base ref. Prefer the merge-base against the named remote/develop
# so the diff reflects only the PR's contribution, not develop drift since
# branching.
BASE_REF="${SMATCHET_COVERAGE_GATE_BASE:-}"
if [ -z "$BASE_REF" ]; then
    for candidate in origin/develop develop HEAD~1; do
        if git rev-parse --verify --quiet "$candidate" >/dev/null 2>&1; then
            BASE_REF="$candidate"
            break
        fi
    done
fi
if [ -z "$BASE_REF" ]; then
    echo "[coverage-delta-gate] no usable base ref; skipping gate" >&2
    exit 0
fi

MERGE_BASE=$(git merge-base "$BASE_REF" HEAD 2>/dev/null || echo "$BASE_REF")

# Compute the diff once. --name-only --diff-filter=ACMR keeps adds, copies,
# modifies, renames (the cases that actually change content). Deletes intentionally
# excluded — removing a production file shouldn't require a new test.
mapfile -t CHANGED < <(git diff --name-only --diff-filter=ACMR "$MERGE_BASE"...HEAD 2>/dev/null || true)

if [ "${#CHANGED[@]}" -eq 0 ]; then
    echo "[coverage-delta-gate] no changed files vs $BASE_REF; gate passes"
    exit 0
fi

PROD_CHANGES=()
TEST_CHANGES=()

for f in "${CHANGED[@]}"; do
    case "$f" in
        # Production surface that the gate cares about. Source/Core/src/*.cpp is
        # the core enforcement target; *.h headers under Source/Core/include/ are
        # treated as docs-or-API-shape (different review surface) so they don't
        # require a paired test delta on their own.
        Source/Core/src/*.cpp)
            PROD_CHANGES+=("$f") ;;
        # Test surface — only actual test TUs count toward a delta. tests/support/*.h
        # (shared fixtures / helpers) was previously included but is trivially
        # dismissable (add an empty header to "satisfy" the gate). Restrict to the
        # per-test-file delta the gate was designed to enforce.
        tests/Core/*.test.cpp|tests/Lua/*.test.cpp|tests/Plugins/*.test.cpp|tests/Plugins/Mcp/*.test.cpp)
            TEST_CHANGES+=("$f") ;;
    esac
done

echo "[coverage-delta-gate] base ref:     $BASE_REF (merge-base $MERGE_BASE)"
echo "[coverage-delta-gate] prod changes: ${#PROD_CHANGES[@]}"
echo "[coverage-delta-gate] test changes: ${#TEST_CHANGES[@]}"

if [ "${#PROD_CHANGES[@]}" -eq 0 ]; then
    echo "[coverage-delta-gate] PASS — no production Source/Core/src/*.cpp changes"
    exit 0
fi

if [ "${#TEST_CHANGES[@]}" -gt 0 ]; then
    echo "[coverage-delta-gate] PASS — production + test files both changed"
    exit 0
fi

# Production-only change with no test delta. Before failing, run the test-light
# exemption pre-check: if every added/modified line in first-party C/C++ product
# files is provably no-new-runtime-surface, PASS legitimately (no override). This
# is what lets the gate run cleanly on a merge_group ref where PR labels (and so
# tests-out-of-band) don't apply. CONSERVATIVE — any real statement falls through.
EXEMPTION="$(git diff --diff-filter=ACMR "$MERGE_BASE"...HEAD -- \
        Source/Core Source/Plugins Source/Standalone tests 2>/dev/null \
    | _classify_diff)"
if [ "$EXEMPTION" = "EXEMPT" ]; then
    echo "[coverage-delta-gate] PASS — test-light exemption: every product-code"
    echo "[coverage-delta-gate]        change is no-new-runtime-surface"
    echo "[coverage-delta-gate]        (comment/log/static_assert/include/catch-scaffold)."
    exit 0
fi

# Production-only change with real new runtime surface and no test delta. The
# workflow may still dismiss via the tests-out-of-band label; the script's job
# is to signal the condition.
echo
echo "FAIL: Source/Core/ changes without test deltas."
echo
echo "Changed production files:"
for f in "${PROD_CHANGES[@]}"; do
    echo "  - $f"
done
echo
echo "Add tests under tests/Core/ (or tests/Lua/, tests/Plugins/) for the"
echo "changed units. Changes that add no new runtime surface (comment-only,"
echo "logging-only, static_assert-only, include-only, swallow->log catch) are"
echo "auto-exempted; if yours genuinely cannot be unit-tested, apply the"
echo "'tests-out-of-band' PR label to dismiss this gate."
exit 1

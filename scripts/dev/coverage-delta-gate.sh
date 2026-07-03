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
#   * forward-decl-only    — `class/struct/union/enum Foo;` name declarations
#                            (optionally template-prefixed) — a type name with no
#                            body and no object carries no runtime surface (the
#                            #1308 fan-in swaps a heavy include for a fwd-decl).
#   * include/using-only   — #include / using directives
#   * preprocessor-guard   — #if/#ifdef/#ifndef/#elif/#else/#endif conditional
#                            directives (compile-config selection; the wrapped
#                            code is classified on its own added lines, so a guard
#                            around NEW statements still falls through). NOT
#                            #define/#undef/#pragma (a macro can carry real logic).
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

    # Whole-line `//` comment.
    case "$line" in
        '//'*) return 0 ;;
    esac

    # A leading `/* … */` span: strip it and classify the RESIDUAL.
    # Historically this was `'/*'*) return 0`, which exempted
    # `/* note */ launchTask();` — real surface (#918 MEDIUM). The bare `'*'*`
    # and `'*/'*` continuation cases were ALSO removed: genuine block-comment
    # continuation lines are consumed by the `in_block_comment` state machine in
    # the caller BEFORE reaching this helper, so a line arriving here that starts
    # with `*` is a pointer-deref statement (`*out = compute();`, `*it = next();`),
    # NOT a comment — exempting it falsely PASSED the required test-delta gate on
    # output-pointer writes (#918 `'*'*` finding).
    case "$line" in
        '/*'*'*/'*)
            local rest="${line#*\*/}"
            rest="${rest#"${rest%%[![:space:]]*}"}"
            [ -z "$rest" ] && return 0   # comment-only — no surface
            line="$rest"                 # fall through to classify the residual code
            ;;
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

    # Preprocessor conditional guards — #if/#ifdef/#ifndef/#elif/#else/#endif.
    # A guard wrapping EXISTING code is compile-config selection (no runtime
    # surface); NEW code inside the guard arrives as its own added line and is
    # classified on its own merits, so a guard around new statements still falls
    # through. NOT #define/#undef/#pragma (a macro can carry real logic).
    case "$code" in
        # '#if'* subsumes '#ifdef'/'#ifndef'; '#elif'/'#else'/'#endif' are explicit.
        '#if'*|'#elif'*|'#else'*|'#endif'*) return 0 ;;
    esac

    # static_assert(…) — compile-time; the build is the test.
    case "$code" in
        'static_assert('*) return 0 ;;
    esac

    # Forward declaration — `class Foo;` / `struct Foo;` / `union Foo;` /
    # `enum [class|struct] Foo [: underlying];`, optionally template-prefixed
    # (`template <…> class Foo;`). A pure name declaration introduces a type name
    # with NO definition body and NO object, so it carries zero runtime surface
    # (the #1308 AppController fan-in swaps a heavy `#include` for a bare
    # `class LocalCacheManager;` fwd-decl). The trailing `;$` anchor keeps this
    # tight: a definition opener (`class Foo : public Bar {` / `enum E { … }`),
    # an elaborated-type object (`class Foo bar;`), or anything with `=`/`(`
    # all fail to match and fall through to real surface. The regex lives in a
    # single-quoted var referenced unquoted so `<`/`>`/`;` stay literal ERE (an
    # inline `\<` would mean a GNU word-boundary, not a literal angle bracket).
    local fwd_decl_re='^(template[[:space:]]*<[^{}]*>[[:space:]]*)?(class|struct|union|enum([[:space:]]+(class|struct))?)[[:space:]]+[A-Za-z_][A-Za-z0-9_]*([[:space:]]*:[[:space:]]*[A-Za-z_:][A-Za-z0-9_:]*)?[[:space:]]*;$'
    if [[ "$code" =~ $fwd_decl_re ]]; then
        return 0
    fi

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
# Net paren balance of a string: count of '(' minus count of ')'. Used to know when a
# wrapped LOG_*( ... ) statement has closed. Parens INSIDE a double-quoted string literal
# (e.g. a LOG format arg `LOG_ERROR("x (", y);`) must NOT count — otherwise the accumulator
# stays open past a balanced statement and swallows the next real-surface line. We strip
# quoted spans (honouring backslash-escaped quotes) before counting; an unterminated quote
# on the line leaves its tail stripped, which is the safe direction (a wrapped string literal
# carries no parens we care about and the close `);` arrives on a later line).
_paren_delta() {
    local s="$1" out="" i=0 n ch in_str=0 esc=0
    local bslash=$'\\'   # single literal backslash via ANSI-C quoting (avoids SC1003)
    n=${#s}
    while [ "$i" -lt "$n" ]; do
        ch="${s:i:1}"
        if [ "$in_str" -eq 1 ]; then
            if [ "$esc" -eq 1 ]; then
                esc=0
            elif [ "$ch" = "$bslash" ]; then
                esc=1
            elif [ "$ch" = '"' ]; then
                in_str=0
            fi
        else
            if [ "$ch" = '"' ]; then
                in_str=1
            else
                out="$out$ch"
            fi
        fi
        i=$(( i + 1 ))
    done
    local opens closes
    opens="${out//[^(]/}"
    closes="${out//[^)]/}"
    echo $(( ${#opens} - ${#closes} ))
}

# Given a line and the paren depth on ENTRY (>=1, the unbalanced opener carried over), find
# where the LOG_*( ... ) statement closes on this line and echo any trailing code that follows
# the closing `)` (and an immediately-following `;`/`,`). Parens inside string literals are
# ignored (same quote-tracking as _paren_delta). Echoes the empty string when the statement
# does NOT close on this line, or when nothing but whitespace trails the close. The caller
# re-classifies the returned tail as its own statement so `LOG_x(...); realStmt();` is not
# blanket-skipped.
_tail_after_log_close() {
    local s="$1" depth="$2" i=0 n ch in_str=0 esc=0 tail=""
    local bslash=$'\\'   # single literal backslash via ANSI-C quoting (avoids SC1003)
    n=${#s}
    while [ "$i" -lt "$n" ]; do
        ch="${s:i:1}"
        if [ "$in_str" -eq 1 ]; then
            if [ "$esc" -eq 1 ]; then
                esc=0
            elif [ "$ch" = "$bslash" ]; then
                esc=1
            elif [ "$ch" = '"' ]; then
                in_str=0
            fi
        else
            if [ "$ch" = '"' ]; then
                in_str=1
            elif [ "$ch" = '(' ]; then
                depth=$(( depth + 1 ))
            elif [ "$ch" = ')' ]; then
                depth=$(( depth - 1 ))
                if [ "$depth" -le 0 ]; then
                    # statement closes here; the tail is whatever follows, minus a leading
                    # statement terminator/separator that belongs to the LOG call.
                    tail="${s:i+1}"
                    tail="${tail#;}"
                    tail="${tail#,}"
                    # Trim leading whitespace.
                    tail="${tail#"${tail%%[![:space:]]*}"}"
                    echo "$tail"
                    return 0
                fi
            fi
        fi
        i=$(( i + 1 ))
    done
    echo ""
}

_classify_diff() {
    local cur_file=""
    local in_product_cpp=0
    local in_block_comment=0
    local saw_real_surface=0
    # Multi-line LOG_*( ... ) accumulation: when a logging call opens with unbalanced parens,
    # keep consuming added lines (treating them as part of the one logging-only-exempt unit)
    # until the paren depth returns to zero. A non-LOG real-surface line is never swallowed
    # because we only enter this state on a LOG_ opener — once balanced we resume normal
    # per-line classification.
    local in_log_stmt=0
    local log_depth=0
    local raw line code

    while IFS= read -r raw; do
        case "$raw" in
            '+++ '*)
                # New-file header: +++ b/<path>  (or /dev/null on delete)
                cur_file="${raw#+++ }"
                cur_file="${cur_file#b/}"
                in_product_cpp=0
                in_block_comment=0
                in_log_stmt=0
                log_depth=0
                case "$cur_file" in
                    Source/Core/*.cpp|Source/Core/*.h|Source/Core/*.hpp|Source/Core/*.cc|Source/Core/*.cxx|\
                    Source/Plugins/*.cpp|Source/Plugins/*.h|Source/Plugins/*.hpp|Source/Plugins/*.cc|Source/Plugins/*.cxx|\
                    Source/Standalone/*.cpp|Source/Standalone/*.h|Source/Standalone/*.hpp|Source/Standalone/*.cc|Source/Standalone/*.cxx|\
                    tests/*.cpp|tests/*.h|tests/*.hpp|tests/*.cc|tests/*.cxx)
                        in_product_cpp=1 ;;
                esac
                continue ;;
            '--- '*) continue ;;
            'diff --git '*) in_block_comment=0; in_log_stmt=0; log_depth=0; continue ;;
            '@@'*) in_block_comment=0; in_log_stmt=0; log_depth=0; continue ;;
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

        # Mid-LOG-statement continuation: keep consuming until parens balance. The whole
        # multi-line LOG_*( ... ) is one logging-only-exempt unit — its continuation lines
        # (format-string fragments, arg lists, the closing `);`) carry no runtime surface.
        # When the statement closes, any real code trailing the close paren on the same line
        # must still be classified — don't blanket-continue past it.
        if [ "$in_log_stmt" -eq 1 ]; then
            local _new_depth _tail
            _new_depth=$(( log_depth + $(_paren_delta "$line") ))
            if [ "$_new_depth" -le 0 ]; then
                _tail="$(_tail_after_log_close "$line" "$log_depth")"
                in_log_stmt=0
                log_depth=0
                if [ -n "$_tail" ]; then
                    line="$_tail"
                    # fall through to classify the trailing code below.
                else
                    continue
                fi
            else
                log_depth="$_new_depth"
                continue
            fi
        fi

        # A LOG_*( opener: if its parens are NOT balanced on this line, enter the multi-line
        # accumulation state (the statement wraps across 2-3 lines). A single-line LOG_*(...);
        # is already handled by _line_is_no_runtime_surface below. If real code trails the
        # closing paren on the SAME line (`LOG_x(...); realStmt();`), classify that tail.
        if [ "$in_log_stmt" -eq 0 ]; then
            case "$line" in
                'LOG_DEBUG('*|'LOG_INFO('*|'LOG_WARN('*|'LOG_ERROR('*|'LOG_TRACE('*)
                    log_depth=$(_paren_delta "$line")
                    if [ "$log_depth" -gt 0 ]; then
                        in_log_stmt=1
                        continue
                    fi
                    # balanced on one line: check for trailing real code after the close.
                    # Entry depth 0 — this line contains the opener's own `(`.
                    local _otail
                    _otail="$(_tail_after_log_close "$line" 0)"
                    log_depth=0
                    if [ -n "$_otail" ]; then
                        line="$_otail"
                        # fall through to classify the trailing code below.
                    fi
                    # else: pure logging line — fall through to _line_is_no_runtime_surface,
                    # which exempts it.
                    ;;
            esac
        fi

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

    # 2-line wrapped LOG_ERROR — the format string + args span two lines; the whole call is
    # one logging-only-exempt unit (the paren-balance accumulator joins them). Before the join
    # fix the continuation line `param);` was classified standalone and fell through.
    _expect EXEMPT "2-line wrapped LOG_ERROR" <<'EOF'
diff --git a/Source/Core/src/Sync/Wrap2.cpp b/Source/Core/src/Sync/Wrap2.cpp
--- a/Source/Core/src/Sync/Wrap2.cpp
+++ b/Source/Core/src/Sync/Wrap2.cpp
@@ -10,0 +11,2 @@
+    LOG_ERROR("sync failed for ticket %s with status %d",
+              ticketKey.c_str(), httpStatus);
EOF

    # 3-line wrapped LOG_ERROR — opener + a middle arg line + the closing `);`. All three
    # accumulate into one exempt logging statement.
    _expect EXEMPT "3-line wrapped LOG_ERROR" <<'EOF'
diff --git a/Source/Core/src/Sync/Wrap3.cpp b/Source/Core/src/Sync/Wrap3.cpp
--- a/Source/Core/src/Sync/Wrap3.cpp
+++ b/Source/Core/src/Sync/Wrap3.cpp
@@ -20,0 +21,3 @@
+    LOG_ERROR(
+        "create failed: %s (code %d)",
+        ex.what(), code);
EOF

    # A wrapped LOG_ERROR followed by REAL surface must still fall through — the accumulator
    # closes on the balanced `);`, then the assignment is classified on its own merits.
    _expect FALLTHROUGH "wrapped LOG_ERROR then a real assignment" <<'EOF'
diff --git a/Source/Core/src/Sync/WrapMix.cpp b/Source/Core/src/Sync/WrapMix.cpp
--- a/Source/Core/src/Sync/WrapMix.cpp
+++ b/Source/Core/src/Sync/WrapMix.cpp
@@ -30,0 +31,3 @@
+    LOG_ERROR("partial: %s",
+              detail.c_str());
+    retries = retries + 1;
EOF

    # A balanced single-line LOG_ERROR whose format string contains a literal `(` must NOT
    # keep the accumulator open — the paren is inside a string literal and carries no surface.
    # Before the _paren_delta string-literal fix this stayed open and swallowed the next line.
    _expect EXEMPT "LOG_ with literal paren in format string (string-literal paren fix)" <<'EOF'
diff --git a/Source/Core/src/Sync/WrapLit.cpp b/Source/Core/src/Sync/WrapLit.cpp
--- a/Source/Core/src/Sync/WrapLit.cpp
+++ b/Source/Core/src/Sync/WrapLit.cpp
@@ -40,0 +41,1 @@
+    LOG_ERROR("x (", y);
EOF

    # A LOG_ statement closing mid-line followed by REAL code on the same line must fall through:
    # the trailing statement is classified on its own merits, not blanket-skipped by the close.
    _expect FALLTHROUGH "LOG_ then trailing real code same line (trailing-code fix)" <<'EOF'
diff --git a/Source/Core/src/Sync/WrapTrail.cpp b/Source/Core/src/Sync/WrapTrail.cpp
--- a/Source/Core/src/Sync/WrapTrail.cpp
+++ b/Source/Core/src/Sync/WrapTrail.cpp
@@ -50,0 +51,1 @@
+    LOG_ERROR("partial: %s", detail.c_str()); retries = retries + 1;
EOF

    # The string-literal paren AND trailing-code fixes combined: a literal `)` inside the format
    # string must not be mistaken for the statement close, and the real trailing stmt must show.
    _expect FALLTHROUGH "LOG_ literal paren + trailing real code same line" <<'EOF'
diff --git a/Source/Core/src/Sync/WrapLitTrail.cpp b/Source/Core/src/Sync/WrapLitTrail.cpp
--- a/Source/Core/src/Sync/WrapLitTrail.cpp
+++ b/Source/Core/src/Sync/WrapLitTrail.cpp
@@ -60,0 +61,1 @@
+    LOG_ERROR("done )", n); count = count + 1;
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

    # #1082-equivalent — a #ifndef/#endif compile-config guard wrapped around an
    # EXISTING function (the def lines are diff CONTEXT, only the directives +
    # comments are added). DX12 dual-target -Wunused-function fix shape.
    _expect EXEMPT "preprocessor-guard around existing code (#1082)" <<'EOF'
diff --git a/Source/Core/src/Ui/SmatchetBugReportUi.cpp b/Source/Core/src/Ui/SmatchetBugReportUi.cpp
--- a/Source/Core/src/Ui/SmatchetBugReportUi.cpp
+++ b/Source/Core/src/Ui/SmatchetBugReportUi.cpp
@@ -38,6 +38,11 @@
+#ifndef SMATCHET_EMBEDDED_IN_UNREAL
+// Only used by the screenshot-attach path below, which is itself
+// #ifndef SMATCHET_EMBEDDED_IN_UNREAL — guard the def too, else the DX12
+// target compiles it out of every call site and trips -Wunused-function -Werror.
 std::string PendingShotStamp() {
     const auto now = std::chrono::steady_clock::now().time_since_epoch();
     return std::to_string(ms);
 }
+#endif
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

    # #1308-equivalent — forward-declaration-only header diff (the AppController
    # fan-in: drop a heavy `#include`, add a `class …;` fwd-decl + json_fwd).
    _expect EXEMPT "forward-declaration-only header (#1308)" <<'EOF'
diff --git a/Source/Core/include/AppController.h b/Source/Core/include/AppController.h
--- a/Source/Core/include/AppController.h
+++ b/Source/Core/include/AppController.h
@@ -37,7 +37,12 @@
-#include "LocalCacheManager.h"
+#include <nlohmann/json_fwd.hpp>
+// Forward-declare instead of pulling the heavy include (the #1308 fan-in).
+class LocalCacheManager;
+struct TrackerFieldCatalog;
+enum class SyncPhase : int;
+template <typename T> class Pool;
EOF

    # ---- FALLTHROUGH cases (must NOT exempt — real runtime surface) ----
    # selftest: asserts-failure — real runtime-surface diffs must NOT be exempted (the gate's block path).

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

    # #918 — output-pointer-deref writes must NOT be exempted by the old `'*'*`
    # comment-continuation case (real runtime surface).
    _expect FALLTHROUGH "pointer-deref output writes (#918 '*'*)" <<'EOF'
diff --git a/Source/Core/src/Sync/Deref.cpp b/Source/Core/src/Sync/Deref.cpp
--- a/Source/Core/src/Sync/Deref.cpp
+++ b/Source/Core/src/Sync/Deref.cpp
@@ -10,0 +11,3 @@
+    *out = compute();
+    *it = next();
+    *(p + i) = v;
EOF

    # #918 — `/* … */ <code>` on one line is real surface, not comment-only.
    _expect FALLTHROUGH "trailing code after a /* */ span (#918 MEDIUM)" <<'EOF'
diff --git a/Source/Core/src/Config/Trail.cpp b/Source/Core/src/Config/Trail.cpp
--- a/Source/Core/src/Config/Trail.cpp
+++ b/Source/Core/src/Config/Trail.cpp
@@ -5,0 +6,1 @@
+    /* note */ launchTask();
EOF

    # A #ifndef guard wrapping a NEW statement — the directives are exempt but
    # the wrapped assignment is real runtime surface, so the diff falls through
    # (proves the guard exemption can't be used to smuggle in untested logic).
    _expect FALLTHROUGH "preprocessor-guard wrapping a new statement" <<'EOF'
diff --git a/Source/Core/src/Sync/Guarded.cpp b/Source/Core/src/Sync/Guarded.cpp
--- a/Source/Core/src/Sync/Guarded.cpp
+++ b/Source/Core/src/Sync/Guarded.cpp
@@ -10,0 +11,3 @@
+#ifndef SMATCHET_EMBEDDED_IN_UNREAL
+    g_counter = computeStamp();
+#endif
EOF

    # A class DEFINITION opener (has a body `{`) is real surface — the fwd-decl
    # exemption must NOT swallow it.
    _expect FALLTHROUGH "class definition opener (not a fwd-decl)" <<'EOF'
diff --git a/Source/Core/include/Widget.h b/Source/Core/include/Widget.h
--- a/Source/Core/include/Widget.h
+++ b/Source/Core/include/Widget.h
@@ -5,0 +6,4 @@
+class Widget : public Base {
+    int compute() { return x_ + 1; }
+};
+Widget gWidget;
EOF

    # An elaborated-type-specifier OBJECT declaration (`class Foo bar;`) declares
    # a variable — real surface, NOT a forward declaration (no trailing object
    # name in a fwd-decl). Proves the `;$` anchor can't be gamed.
    _expect FALLTHROUGH "elaborated-type object decl (not a fwd-decl)" <<'EOF'
diff --git a/Source/Core/src/Sync/Elab.cpp b/Source/Core/src/Sync/Elab.cpp
--- a/Source/Core/src/Sync/Elab.cpp
+++ b/Source/Core/src/Sync/Elab.cpp
@@ -10,0 +11,2 @@
+    class LocalCacheManager cache;
+    struct Foo f = make();
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
        tests/Core/*.test.cpp|tests/Lua/*.test.cpp|tests/Plugins/*.test.cpp|tests/Plugins/Mcp/*.test.cpp|tests/ui/*.test.cpp)
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
# Write the diff to a temp file rather than piping it into `_classify_diff` directly.
# `_classify_diff` intentionally `break`s out of its read loop on the first real-surface
# line (see its body). Under a `|` pipe with `set -o pipefail`, an early-closing reader
# sends `git diff` SIGPIPE (128+13=141) once its stdout buffer fills, and pipefail
# propagates that 141 through the `EXEMPTION=$(...)` assignment, tripping `set -e` and
# killing the script BEFORE it reaches the "FAIL: ... test deltas" message below — a real
# diff that should cleanly fail the gate instead crashes it. A plain redirect into a file
# has no pipe to receive SIGPIPE (git diff always runs to completion), AND its exit status
# is captured directly — unlike an earlier process-substitution fix for this same SIGPIPE
# bug, which fixed the crash but lost git-diff-failure detection entirely (a bad
# `MERGE_BASE` or other git error would silently classify as EXEMPT on the resulting empty
# input instead of hard-failing the gate).
GIT_DIFF_TMPFILE="$(mktemp)"
trap 'rm -f "$GIT_DIFF_TMPFILE"' EXIT
if ! git diff --diff-filter=ACMR "$MERGE_BASE"...HEAD -- \
        Source/Core Source/Plugins Source/Standalone tests >"$GIT_DIFF_TMPFILE" 2>/dev/null; then
    echo "[coverage-delta-gate] FAIL — git diff failed (bad MERGE_BASE '$MERGE_BASE' or git error)" >&2
    exit 1
fi
EXEMPTION="$(_classify_diff < "$GIT_DIFF_TMPFILE")"
if [ "$EXEMPTION" = "EXEMPT" ]; then
    echo "[coverage-delta-gate] PASS — test-light exemption: every product-code"
    echo "[coverage-delta-gate]        change is no-new-runtime-surface"
    echo "[coverage-delta-gate]        (comment/log/static_assert/include/preprocessor-guard/catch-scaffold)."
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
echo "Add tests under tests/Core/ (or tests/Lua/, tests/Plugins/, tests/ui/) for the"
echo "changed units. Changes that add no new runtime surface (comment-only,"
echo "logging-only, static_assert-only, forward-declaration-only, include-only,"
echo "preprocessor-guard-only, swallow->log catch) are auto-exempted; if yours"
echo "genuinely cannot be"
echo "unit-tested, apply the"
echo "'tests-out-of-band' PR label to dismiss this gate."
exit 1

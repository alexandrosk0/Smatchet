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
#   * off-target platform arm — an added line whose enclosing #if/#elif/#else arm
#                            can only be compiled for Android (__ANDROID__): never
#                            built by the desktop/Linux test targets, validated
#                            instead by the Android NDK/APK cross-compile jobs
#                            (#1021). __APPLE__ / TARGET_OS_* arms are NOT exempt
#                            — no CI job builds macOS/iOS, so nothing would
#                            validate them; widen the macro set only alongside an
#                            Apple CI job. The non-_WIN32 #else arm IS built + run
#                            on Linux CI — NOT exempt.
#   * header→cpp body relocation — an in-header (inline) function definition
#                            removed and re-added byte-identical (whitespace-
#                            trimmed, `inline` dropped) as an out-of-line
#                            definition in a .cpp, plus its header declaration
#                            and the new TU's namespace opener (#1317).
#   The last two need nesting/pairing context, so the diff is generated with full
#   file context and _prefilter_diff drops exempt lines before _classify_diff.
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
# Scope: only .cpp/.h/.hpp/.cc/.cxx (and the included .c/.inl/.inc/.ipp) under
# Source/Core, Source/Plugins, Source/Standalone, tests/. Lines in other files (build/docs/scripts/non-product
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
                    tests/*.cpp|tests/*.h|tests/*.hpp|tests/*.cc|tests/*.cxx|\
                    Source/Core/*.c|Source/Core/*.inl|Source/Core/*.inc|Source/Core/*.ipp|\
                    Source/Plugins/*.c|Source/Plugins/*.inl|Source/Plugins/*.inc|Source/Plugins/*.ipp|\
                    Source/Standalone/*.c|Source/Standalone/*.inl|Source/Standalone/*.inc|Source/Standalone/*.ipp|\
                    tests/*.c|tests/*.inl|tests/*.inc|tests/*.ipp)
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

        # In a block comment: stays a comment until the close `*/`. If real code
        # trails the close on the same line (`... */ launchTask();`), it must still
        # be classified — don't blanket-continue past it (mirrors the LOG-statement
        # close handling below; the single-line `/* */ code` path already does this).
        if [ "$in_block_comment" -eq 1 ]; then
            case "$line" in
                *'*/'*)
                    in_block_comment=0
                    line="${line#*'*/'}"                       # drop through the close
                    line="${line#"${line%%[![:space:]]*}"}"    # ltrim the remainder
                    case "$line" in
                        ''|'//'*) continue ;;                  # nothing (or a line comment) follows
                    esac
                    ;;                                          # else fall through to classify trailing code
                *)
                    continue ;;                                 # still inside the block comment
            esac
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
# Full-context prefilter (platform-arm + header→cpp body relocation exemptions)
# ---------------------------------------------------------------------------
# _prefilter_diff <diff-file> — read a FULL-CONTEXT unified diff (git diff
# --unified=<huge>, so every hunk carries the whole post-image and the #if
# nesting of each added line is knowable) and print a reduced diff for
# _classify_diff: the file/hunk headers plus the '+' lines that are NOT exempted
# here. Context and '-' lines are dropped (the classifier never reads them), which
# also keeps the bash read loop cheap on big diffs. Because a full-context diff is
# ONE hunk per file, the classifier's carried state (inside a /* */ block, inside a
# wrapped LOG_*( ... )) would otherwise never reset — a reworded first line of an
# existing comment or LOG call would swallow every later '+' line of the file. So
# whenever a context line or a dropped '+' line sits between two printed '+'
# lines, a synthetic `@@` header is printed first: classifier state never spans
# post-image lines it cannot see (the default-context diff only reset at real
# hunk boundaries, so this is never looser than that).
#
# A small lexer walks the same post-image tracking /* */ comments and raw string
# literals (R"delim( ... )delim"). A '+' line that is only whitespace/comment is
# dropped (no surface; not a gap) — so an edit to a comment whose opener is a
# context line stays exempt. A '+' line that starts inside a raw string literal is
# string DATA and is replaced by a sentinel the classifier never exempts, as is a
# '+' line with a carriage return inside it. A C/C++ file git prints as binary
# ("Binary files ... differ", e.g. one NUL byte) is never exempt. A file
# whose tracking cannot be trusted — a hunk that ends with the #if stack open,
# closes an arm it never opened, ends inside a comment/raw string, splices a line
# with a trailing backslash outside a directive's own continuation, splices a
# directive where the join could change what the lexer sees (a split directive
# name, or a quote, comment token, '/' or '*' at the splice on a spliced directive
# line), puts a comment between '#' and the directive name or a directive after a
# closing */, uses a %: digraph directive, holds a control byte the compilers read
# differently (a carriage return inside a line, a form feed, a vertical tab, a
# backslash followed by whitespace), or puts '$', a non-ASCII byte or a pp-number
# (1.R, 1'R, 1e+R) before a raw string's R prefix — gets
# neither the comment drop nor the off-target drop: its lines reach the
# classifier as-is (falls through). Two exemptions, both conservative (anything
# unrecognised is printed, i.e. falls through):
#
#   1. Off-target platform arm. Walking the post-image (' ' + '+' lines) of each
#      first-party product C/C++ file, keep an #if/#ifdef/#ifndef/#elif/#else/
#      #endif stack. An arm is OFF-TARGET when its effective condition requires an
#      off-target platform macro: its own condition is an ||/&& combination of
#      ONLY __ANDROID__ atoms (`defined(X)`, `defined X`, bare `X`), or an
#      earlier arm of the same group was the pure negation of such a combination
#      (`#ifndef __ANDROID__ … #else`). Android is the only off-target platform
#      with a CI build (mobile-android-ndk / APK jobs); __APPLE__ / TARGET_OS_*
#      arms stay gated until a macOS/iOS job exists. A '+' line inside any
#      off-target arm is dropped. So `#ifdef _WIN32 … #else` stays gated (the
#      #else arm is the Linux/POSIX path CI builds and runs), as does
#      `#elif defined(__ANDROID__) || defined(__linux__)`. A directive line with a
#      backslash continuation or a multi-line comment is classified OTHER (gated).
#      A `#if` line that starts inside a /* */ comment or a raw string literal is
#      not a directive and is not tracked. The stack resets at every hunk header,
#      so a partial-context diff can only under-exempt.
#
#   2. Header→cpp body relocation. Pass 1 collects every complete, brace-balanced
#      function definition inside a run of REMOVED lines of a product header
#      (.h/.hpp), and every run of ADDED lines of a product .cpp/.cc/.cxx. A
#      definition re-added as a contiguous, byte-identical (per-line trimmed;
#      `inline` dropped from the signature line) block in a .cpp is a relocation:
#      those added lines are dropped, as are the header's added declaration of
#      the same signature (`<sig>;`) and a bare namespace opener in a .cpp that
#      received a relocated body. The body moved unchanged, so no new runtime
#      surface — the existing callers' tests still exercise it.
_PREFILTER_AWK="$(cat <<'AWK'
function trim(s) { sub(/^[ \t\r]+/, "", s); sub(/[ \t\r]+$/, "", s); return s }
function is_prod(p) { return p ~ /^(Source\/(Core|Plugins|Standalone)|tests)\/.*\.(cpp|h|hpp|cc|cxx|c|inl|inc|ipp)$/ }
function is_hdr(p) { return p ~ /\.(h|hpp)$/ }
function is_cpp(p) { return p ~ /\.(cpp|cc|cxx)$/ }
function path_of(raw,   p) { p = substr(raw, 5); sub(/^b\//, "", p); return p }
function drop_inline(s) { s = " " s " "; gsub(/[ \t]inline[ \t]/, " ", s); return trim(s) }
# Net { minus } outside string/char literals and a trailing // comment.
function brace_delta(s,   t, o, c) {
    t = s
    gsub(/\\./, "", t)
    gsub(/"[^"]*"/, "", t)
    gsub(SQ "[^" SQ "]*" SQ, "", t)
    sub(/\/\/.*$/, "", t)
    o = gsub(/\{/, "", t)
    c = gsub(/\}/, "", t)
    return o - c
}
# A function-definition signature line (already trimmed + inline-dropped):
# `<type tokens> name(...) {` or a one-line `... { ... }`.
function is_def_opener(s,   head) {
    if (s !~ /\(/) return 0
    if (s !~ /\{$/ && s !~ /\}$/) return 0
    head = s
    sub(/\(.*/, "", head)
    head = trim(head)
    if (head !~ /^[A-Za-z_~][A-Za-z0-9_:<>,*&~ \t]*$/) return 0
    if (head ~ /^(if|for|while|switch|catch|return|else|do|try|case|sizeof|new|delete|throw)([ \t]|$)/) return 0
    return head ~ /[ \t*&]/
}
# Platform condition classifier: OFF (requires an off-target macro), NEGOFF (pure
# negation of an OFF expression), or OTHER.
function off_only(s,   t) {
    t = s
    gsub(/[()]/, "", t)
    return t ~ /^@((\|\||&&)@)*$/
}
function wrapped(s,   i, d, ch) {
    if (substr(s, 1, 1) != "(" || substr(s, length(s), 1) != ")") return 0
    d = 0
    for (i = 1; i <= length(s); i++) {
        ch = substr(s, i, 1)
        if (ch == "(") d++
        else if (ch == ")") { d--; if (d == 0 && i < length(s)) return 0 }
    }
    return d == 0
}
function classify_cond(e,   s, inner, p, q) {
    s = e
    if (s ~ /\\$/) return "OTHER"
    while ((p = index(s, "/*")) > 0) {
        q = index(substr(s, p + 2), "*/")
        if (q == 0) return "OTHER"
        s = substr(s, 1, p - 1) " " substr(s, p + q + 3)
    }
    sub(/\/\/.*$/, "", s)
    gsub(/defined[ \t]*\([ \t]*__ANDROID__[ \t]*\)/, "@", s)
    gsub(/defined[ \t]+__ANDROID__/, "@", s)
    gsub(/__ANDROID__/, "@", s)
    gsub(/[ \t\r]/, "", s)
    if (s == "") return "OTHER"
    if (off_only(s)) return "OFF"
    if (substr(s, 1, 1) == "!") {
        inner = substr(s, 2)
        if (inner == "@") return "NEGOFF"
        if (wrapped(inner) && off_only(substr(inner, 2, length(inner) - 2))) return "NEGOFF"
    }
    return "OTHER"
}
function is_off_macro(m) { return m == "__ANDROID__" }
# Update the #if stack for one post-image line; returns 1 when it is a
# conditional directive (#define/#include/#pragma return 0 and are classified as
# ordinary lines, so one inside an off-target arm is still dropped).
function track_directive(body,   s, kw, rest, c) {
    s = trim(body)
    if (substr(s, 1, 1) != "#") return 0
    s = trim(substr(s, 2))
    kw = s
    sub(/[^A-Za-z].*$/, "", kw)
    if (kw !~ /^(if|ifdef|ifndef|elif|elifdef|elifndef|else|endif)$/) return 0
    rest = trim(substr(s, length(kw) + 1))
    if (kw == "if" || kw == "ifdef" || kw == "ifndef") {
        if (kw == "ifdef") c = is_off_macro(first_tok(rest)) ? "OFF" : "OTHER"
        else if (kw == "ifndef") c = is_off_macro(first_tok(rest)) ? "NEGOFF" : "OTHER"
        else c = classify_cond(rest)
        depth++
        arm[depth] = (c == "OFF")
        neg[depth] = (c == "NEGOFF")
    } else if (kw == "elif" || kw == "elifdef" || kw == "elifndef") {
        if (depth > 0) {
            if (kw == "elifdef") c = is_off_macro(first_tok(rest)) ? "OFF" : "OTHER"
            else if (kw == "elifndef") c = is_off_macro(first_tok(rest)) ? "NEGOFF" : "OTHER"
            else c = classify_cond(rest)
            arm[depth] = (neg[depth] || c == "OFF")
            if (c == "NEGOFF") neg[depth] = 1
        } else uflow = 1
    } else if (kw == "else") {
        if (depth > 0) arm[depth] = neg[depth]
        else uflow = 1
    } else if (kw == "endif") {
        if (depth > 0) depth--
        else uflow = 1
    }
    return 1
}
function first_tok(s) { sub(/[ \t\/].*$/, "", s); return s }
function in_off_arm(   i) { for (i = 1; i <= depth; i++) if (arm[i]) return 1; return 0 }
# Lexical state across the post-image lines of one hunk: lx_blk (inside a /* */
# comment) and lx_raw (inside a raw string literal, closed by ")" lx_rdel "\"").
function lex_reset() { lx_blk = 0; lx_raw = 0; lx_rdel = ""; lx_untrusted = 0; lx_macro = 0 }
# lex_line(s) — advance the lexical state across one line; sets lx_code = 1 when
# any non-whitespace byte lies outside a comment (string-literal bytes are code).
# Ordinary string/char literals cannot span lines, so they are skipped in place
# (a C++14 digit separator 1'000 is not a char literal).
function lex_line(s,   i, n, rest, p, c, pre, m, d, k) {
    lx_code = 0
    n = length(s)
    i = 1
    while (i <= n) {
        if (lx_blk) {
            k = index(substr(s, i), "*/")
            if (k == 0) return
            i += k + 1
            lx_blk = 0
            continue
        }
        if (lx_raw) {
            lx_code = 1
            k = index(substr(s, i), ")" lx_rdel "\"")
            if (k == 0) return
            i += k + length(lx_rdel) + 1
            lx_raw = 0
            continue
        }
        rest = substr(s, i)
        p = match(rest, /[\/"']/)
        if ((p ? substr(rest, 1, p - 1) : rest) ~ /[^ \t\r]/) lx_code = 1
        if (!p) return
        i += p - 1
        c = substr(s, i, 1)
        if (substr(s, i, 2) == "//") {
            # A trailing backslash splices the next line into this comment, which the
            # per-line lexer cannot follow: the file's tracking is not trusted.
            if (s ~ /\\[ \t\r]*$/) lx_untrusted = 1
            return
        }
        if (substr(s, i, 2) == "/*") { lx_blk = 1; i += 2; continue }
        lx_code = 1
        if (c == "/") { i++; continue }
        pre = substr(s, 1, i - 1)
        # GCC, Clang and MSVC take '$' and non-ASCII bytes as identifier characters, so before an R
        # prefix they make it part of an identifier; this lexer cannot follow that.
        if (c == "\"" && pre ~ /([$]|[^\t -~])(u8|u|U|L)?R$/) lx_untrusted = 1
        # Nor can it follow an R that continues a pp-number (1.R, 1'R, 1e+R): the quote after it opens an
        # ordinary string for the compilers.
        if (c == "\"" && pre ~ /(^|[^A-Za-z0-9_])[.]?[0-9]([A-Za-z0-9_.']|[eEpP][+-])*R$/) lx_untrusted = 1
        if (c == "\"" && pre ~ /(^|[^A-Za-z0-9_])(u8|u|U|L)?R$/) {
            m = substr(s, i + 1)
            d = index(m, "(")
            # A delimiter is up to 16 characters, any but space, the parentheses, backslash and the
            # tab, vertical-tab and form-feed controls ('"' is allowed: R""( ... )"").
            if (d > 0 && d <= 17 && substr(m, 1, d - 1) !~ /[ \t\v\f\\)]/) {
                lx_rdel = substr(m, 1, d - 1)
                lx_raw = 1
                i += d + 1
                continue
            }
        }
        # A digit separator continues a pp-number, so it is followed by a digit or a letter; any other
        # quote after a digit opens a character literal.
        if (c == SQ && pre ~ /(^|[^A-Za-z0-9_])[0-9][A-Za-z0-9_.']*$/ && substr(s, i + 1, 1) ~ /[A-Za-z0-9_]/) {
            i++
            continue
        }
        i++
        while (i <= n) {
            k = substr(s, i, 1)
            if (k == "\\") { i += 2; continue }
            i++
            if (k == c) break
        }
    }
}
# post_line(body) — feed one post-image line through the #if stack (only when it
# STARTS outside a comment / raw string — a `#if` there is text, not a directive)
# and then the lexer. Sets pl_raw (line starts inside a raw string literal);
# returns 1 for a conditional directive.
function post_line(body,   r, st, tl, splice, cont, isdir) {
    pl_raw = lx_raw
    # Shapes the per-line tracking cannot follow mark the file untrusted (its lines then fall
    # through to the classifier, never dropped):
    #   - a backslash line splice outside a preprocessor directive's continuation (the
    #     compiler joins the lines first, so a string, char literal or comment can run on);
    #   - a directive splice the tracking could misread: one that splits the directive
    #     name (`#el\` + `se`), or a spliced directive line holding a quote, a comment
    #     token, or a '/' or '*' just before the backslash (the join can open or close a
    #     string or comment the per-line lexer never sees);
    #   - a comment between '#' and the directive name (`# /* c */ else`);
    #   - a `%:` digraph directive.
    st = trim(body)
    tl = body
    sub(/[ \t\r]+$/, "", tl)
    splice = (tl ~ /\\$/)
    # A continuation line is the directive's own text (a `#else` there is macro body, not a
    # directive), and a line that starts inside a comment or raw string is no directive.
    cont = lx_macro
    isdir = !cont && !lx_blk && !lx_raw && (substr(st, 1, 1) == "#" || substr(st, 1, 2) == "%:")
    if (substr(st, 1, 2) == "%:" || st ~ /^#[ \t]*\/\*/) lx_untrusted = 1
    # Control characters the compilers read differently from this lexer: a carriage return inside a
    # line ends it, a form feed or vertical tab is whitespace (`\f#else` is a directive), and a
    # backslash followed by whitespace splices for GCC and Clang but not for MSVC before C++23.
    if (body ~ /\r[^\r]/ || body ~ /[\f\v]/ || body ~ /\\[ \t]+\r?$/) lx_untrusted = 1
    if (splice && !isdir && !cont) lx_untrusted = 1
    if (isdir && splice && st ~ /^#[ \t]*[A-Za-z0-9_]*\\$/) lx_untrusted = 1
    if ((splice && isdir) || cont) {
        if (index(body, "\"") || index(body, SQ) || index(body, "/*") || index(body, "*/") || index(body, "//") ||
            tl ~ /[\/*]\\$/) lx_untrusted = 1
    }
    lx_macro = splice && (isdir || cont)
    # A directive after a closing */ (`/* x */ #else`, `/* a */ # /* b */ else`, `/* x */ %:else`,
    # or the last line of a multi-line comment) is one the compiler honours but track_directive
    # never sees: the #if stack would be wrong, so the file's tracking is not trusted.
    # (Only a conditional name or a comment after the '#': an ImGui id path "**/##id" is no directive.)
    if (body ~ /\*\/[ \t]*(%:|#[ \t]*(\/\*|(if|ifdef|ifndef|elif|elifdef|elifndef|else|endif)([^A-Za-z0-9_]|$)))/) lx_untrusted = 1
    r = isdir ? track_directive(body) : 0
    lex_line(body)
    return r
}
# Pass-1 balance check, at each hunk end: tracking that leaves the #if stack open,
# closed an arm it never opened, or stops inside a comment / raw string cannot be
# trusted — that file gets neither the off-target nor the comment-only drop.
function end_hunk1() {
    if (prod1 && (depth != 0 || uflow || lx_blk || lx_raw || lx_untrusted)) untrusted[f1] = 1
    depth = 0
    uflow = 0
    lex_reset()
}
# Pass-2 output of one kept '+' line: a synthetic hunk header first when a context
# or dropped line separates it from the previous printed '+' line, so classifier
# state (block comment / wrapped LOG_) never spans lines it cannot see.
function emit(b) {
    # A carriage return inside the line ends it for the compilers: whatever follows is code the
    # classifier would read as part of the line before (a comment, a directive), so it is never exempt.
    if (b ~ /\r[^\r]/) b = RAWLINE
    if (gap) { print "@@ prefilter: post-image gap @@"; gap = 0 }
    print "+" b
}
# Pass-1 helpers: close the current removed-header / added-cpp run.
function end_runs() { in_rrun = 0; in_arun = 0 }
BEGIN {
    SQ = sprintf("%c", 39); nr = 0; na = 0
    RAWLINE = "__coverage_gate_raw_string_literal_line__;"
}
NR == FNR {
    if ($0 ~ /^diff --git / || $0 ~ /^@@/) { end_runs(); end_hunk1(); next }
    if ($0 ~ /^--- /) { end_runs(); next }
    if ($0 ~ /^\+\+\+ /) { end_runs(); f1 = path_of($0); prod1 = is_prod(f1); next }
    if (prod1 && (substr($0, 1, 1) == " " || substr($0, 1, 1) == "+")) post_line(substr($0, 2))
    if (substr($0, 1, 1) == "-" && is_prod(f1) && is_hdr(f1)) {
        if (!in_rrun) { nr++; rn[nr] = 0; in_rrun = 1 }
        rl[nr, ++rn[nr]] = trim(substr($0, 2))
        in_arun = 0
        next
    }
    if (substr($0, 1, 1) == "+" && is_prod(f1) && is_cpp(f1)) {
        if (!in_arun) { na++; an[na] = 0; af[na] = f1; in_arun = 1 }
        an[na]++
        al[na, an[na]] = trim(substr($0, 2))
        ak[na, an[na]] = FNR
        in_rrun = 0
        next
    }
    end_runs()
    next
}
FNR == 1 && !paired {
    paired = 1
    end_hunk1()
    # Extract complete definitions from each removed header run, then pair each
    # with a contiguous byte-identical added run segment in a .cpp.
    for (r = 1; r <= nr; r++) {
        i = 1
        while (i <= rn[r]) {
            sig = drop_inline(rl[r, i])
            if (!is_def_opener(sig)) { i++; continue }
            d = brace_delta(rl[r, i])
            j = i + 1
            while (d > 0 && j <= rn[r]) { d += brace_delta(rl[r, j]); j++ }
            if (d != 0) { i++; continue }
            k = j - i
            matched = 0
            for (a = 1; a <= na && !matched; a++) {
                for (st = 1; st + k - 1 <= an[a] && !matched; st++) {
                    if (used[a, st] || drop_inline(al[a, st]) != sig) continue
                    ok = 1
                    for (q = 1; q < k && ok; q++)
                        if (used[a, st + q] || al[a, st + q] != rl[r, i + q]) ok = 0
                    if (!ok) continue
                    for (q = 0; q < k; q++) { used[a, st + q] = 1; reloc[ak[a, st + q]] = 1 }
                    relfile[af[a]] = 1
                    s2 = sig
                    sub(/[ \t]*\{.*$/, "", s2)
                    relsig[s2] = 1
                    matched = 1
                }
            }
            i = j
        }
    }
}
{
    if ($0 ~ /^diff --git / || $0 ~ /^--- /) { print; next }
    if ($0 ~ /^\+\+\+ /) {
        f2 = path_of($0); prod = is_prod(f2); trusted = !(f2 in untrusted)
        depth = 0; gap = 0; lex_reset(); print; next
    }
    if ($0 ~ /^@@/) { depth = 0; gap = 0; lex_reset(); print; next }
    c1 = substr($0, 1, 1)
    if (c1 == " ") { if (prod) { post_line(substr($0, 2)); gap = 1 } next }
    if (c1 != "+") next
    if (!prod) { print; next }
    body = substr($0, 2)
    if (post_line(body)) { emit(body); next }
    if (FNR in reloc) { gap = 1; next }
    if (trusted && in_off_arm()) { gap = 1; next }
    if (trusted && !lx_code) next
    t = trim(body)
    if (relfile[f2] && t ~ /^namespace([ \t]+[A-Za-z_][A-Za-z0-9_:]*)?[ \t]*\{$/) { gap = 1; next }
    if (is_hdr(f2) && t ~ /;$/) {
        s2 = t
        sub(/[ \t]*;$/, "", s2)
        if (s2 in relsig) { gap = 1; next }
    }
    emit(pl_raw ? RAWLINE : body)
}
AWK
)"

_prefilter_diff() {
    awk "$_PREFILTER_AWK" "$1" "$1"
}

# _classify_diff_file <diff-file> — prefilter a full-context diff, then classify the
# reduced diff. Both stages read/write temp FILES, never a pipe: _classify_diff
# breaks out of its read loop early, and an early-closing pipe reader would SIGPIPE
# the producer (see the GIT_DIFF_TMPFILE note in the normal run below). Echoes
# EXEMPT / FALLTHROUGH; returns non-zero (echoing nothing) if the prefilter fails.
_classify_diff_file() {
    local reduced rc=0
    # git prints a file it takes for binary (one NUL byte, even inside a comment the compilers
    # ignore) as a single "Binary files ... differ" line: a C/C++ file in that form hides its whole
    # change from the classifier, so it is never exempt.
    if grep -qE '^Binary files .*\.(cpp|h|hpp|cc|cxx|c|inl|inc|ipp)"? differ$' "$1"; then
        echo FALLTHROUGH
        return 0
    fi
    reduced="$(mktemp)"
    _prefilter_diff "$1" >"$reduced" || rc=$?
    if [ "$rc" -eq 0 ]; then
        _classify_diff <"$reduced"
    fi
    rm -f "$reduced"
    return "$rc"
}

# ---------------------------------------------------------------------------
# --selftest — both-direction fixtures (mirrors the real target PRs)
# ---------------------------------------------------------------------------
if [ "${1:-}" = "--selftest" ]; then
    fail=0
    _st_diff="$(mktemp)"
    trap 'rm -f "$_st_diff"' EXIT
    # _expect <EXEMPT|FALLTHROUGH> <label> <<diff — the fixture runs through the
    # same prefilter + classifier pipeline as the real gate.
    _expect() {
        local want="$1" label="$2" got
        cat >"$_st_diff"
        got="$(_classify_diff_file "$_st_diff")"
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

    # #1021-equivalent — a real statement confined to a Bionic-only #elif arm (full
    # file context, as the gate generates it). Never compiled by the desktop/Linux
    # test targets, so it carries no testable surface there.
    _expect EXEMPT "statement inside an #elif defined(__ANDROID__) arm (#1021)" <<'EOF'
diff --git a/Source/Core/src/SubprocessCapture.cpp b/Source/Core/src/SubprocessCapture.cpp
--- a/Source/Core/src/SubprocessCapture.cpp
+++ b/Source/Core/src/SubprocessCapture.cpp
@@ -1,9 +1,10 @@
 #include "SubprocessCapture.h"
 #ifdef _WIN32
     std::string out = ReadPipeWin();
 #elif defined(__ANDROID__)
-    std::string out(ptr);
+    std::string out(ptr, length);
+    out.resize(trimmed);
 #else
     std::string out = ReadPipePosix();
 #endif
EOF

    # Nested + negated forms: a `defined __ANDROID__` arm inside an unrelated
    # guard, and the #else of `#ifndef __ANDROID__` (i.e. the Android-only arm).
    _expect EXEMPT "__ANDROID__ arm nested in a guard + #else of #ifndef __ANDROID__" <<'EOF'
diff --git a/Source/Core/src/Config/Paths.cpp b/Source/Core/src/Config/Paths.cpp
--- a/Source/Core/src/Config/Paths.cpp
+++ b/Source/Core/src/Config/Paths.cpp
@@ -1,12 +1,14 @@
 #if SMATCHET_WITH_FOO
 #  if defined __ANDROID__  // Bionic sandbox
+    root = SandboxRoot();
 #  endif
 #endif
 #ifndef __ANDROID__
     root = HomeDir();
 #else
+    root = AppFilesDir(env);
+#define ANDROID_ROOT_SET 1
 #endif
EOF

    # #1317-equivalent — an inline header body demoted to a declaration and moved
    # byte-identical (indentation aside) into a new TU inside a namespace.
    _expect EXEMPT "header inline body relocated byte-identical to a .cpp (#1317)" <<'EOF'
diff --git a/Source/Core/include/LocalizedImGui.h b/Source/Core/include/LocalizedImGui.h
--- a/Source/Core/include/LocalizedImGui.h
+++ b/Source/Core/include/LocalizedImGui.h
@@ -1,12 +1,8 @@
 #pragma once
 namespace LocalizedImGui {
-inline void HookOnLastItem(char* buf, std::size_t size) {
-    const bool active = ::ImGui::IsItemActive();
-    if (active) {
-        g_router.Register(buf, size);
-    }
-}
+// Defined out-of-line in DictationHook.cpp.
+void HookOnLastItem(char* buf, std::size_t size);
 inline bool InputText(const char* label, char* buf, size_t size) {
     return ::ImGui::InputText(label, buf, size);
 }
 }  // namespace LocalizedImGui
diff --git a/Source/Core/src/DictationHook.cpp b/Source/Core/src/DictationHook.cpp
--- /dev/null
+++ b/Source/Core/src/DictationHook.cpp
@@ -0,0 +1,12 @@
+#include "LocalizedImGui.h"
+
+namespace LocalizedImGui {
+
+void HookOnLastItem(char* buf, std::size_t size) {
+        const bool active = ::ImGui::IsItemActive();
+        if (active) {
+            g_router.Register(buf, size);
+        }
+}
+
+}  // namespace LocalizedImGui
EOF

    # Full-context diffs are one hunk per file, so the prefilter's lexer tracks
    # /* */ across CONTEXT lines: rewording non-adjacent lines of one doc-comment
    # block (the second edit's opener is a context line) stays comment-only.
    _expect EXEMPT "interleaved edits inside one /** */ block (opener is context)" <<'EOF'
diff --git a/Source/Core/src/Sync/Doc.cpp b/Source/Core/src/Sync/Doc.cpp
--- a/Source/Core/src/Sync/Doc.cpp
+++ b/Source/Core/src/Sync/Doc.cpp
@@ -1,7 +1,7 @@
 #include "Doc.h"
-/** Old summary.
+/** New summary.
  * unchanged detail
- * old note
+ * new note
  */
 int v = 1;
EOF

    # ---- FALLTHROUGH cases (must NOT exempt — real runtime surface) ----
    # selftest: asserts-failure — real runtime-surface diffs must NOT be exempted (the gate's block path).

    # A full-context diff is ONE hunk per file: classifier state entered on a
    # reworded first line of an existing /* */ block, or of a wrapped LOG_*( ,
    # must not persist past the context lines to a real statement far below
    # (default-context diffs reset at each @@; the prefilter emits a synthetic @@
    # at every post-image gap so this stays as strict as that).
    _expect FALLTHROUGH "reworded /* */ block opener + untested statement far below" <<'EOF'
diff --git a/Source/Core/src/Sync/Blk.cpp b/Source/Core/src/Sync/Blk.cpp
--- a/Source/Core/src/Sync/Blk.cpp
+++ b/Source/Core/src/Sync/Blk.cpp
@@ -1,10 +1,10 @@
 #include "Blk.h"
-/* Old first line of the block.
+/* New first line of the block.
  * second line
  */
 int v1 = 1;
 int v2 = 2;
 void g(int x) {
-    (void)x;
+    launchMissiles(x);
 }
EOF
    _expect FALLTHROUGH "reworded wrapped LOG_INFO( opener + untested statement far below" <<'EOF'
diff --git a/Source/Core/src/Sync/Lg.cpp b/Source/Core/src/Sync/Lg.cpp
--- a/Source/Core/src/Sync/Lg.cpp
+++ b/Source/Core/src/Sync/Lg.cpp
@@ -1,10 +1,10 @@
 #include "Lg.h"
 void f(int x) {
-    LOG_INFO("old {}",
+    LOG_INFO("new {}",
              x);
 }
 int v1 = 1;
 void g(int x) {
-    (void)x;
+    launchMissiles(x);
 }
EOF

    # A `#if defined(__ANDROID__)` that is comment text or raw-string data is not
    # a directive — tracking it left the #if stack open and dropped every later
    # '+' line of the file as "off-target".
    _expect FALLTHROUGH "#if defined(__ANDROID__) inside a /* */ comment is not a directive" <<'EOF'
diff --git a/Source/Core/src/Sync/Pp.cpp b/Source/Core/src/Sync/Pp.cpp
--- a/Source/Core/src/Sync/Pp.cpp
+++ b/Source/Core/src/Sync/Pp.cpp
@@ -1,8 +1,8 @@
 #include "Pp.h"
 /* Usage note:
 #if defined(__ANDROID__)
    (the guard above is illustrative)
  */
 void g(int x) {
-    (void)x;
+    launchMissiles(x);
 }
EOF
    _expect FALLTHROUGH "#if defined(__ANDROID__) inside a raw string literal is not a directive" <<'EOF'
diff --git a/Source/Core/src/Sync/Raw.cpp b/Source/Core/src/Sync/Raw.cpp
--- a/Source/Core/src/Sync/Raw.cpp
+++ b/Source/Core/src/Sync/Raw.cpp
@@ -1,7 +1,7 @@
 static const char* kShader = R"glsl(
 #if defined(__ANDROID__)
 )glsl";
 void g(int x) {
-    (void)x;
+    launchMissiles(x);
 }
EOF
    # Tracking the lexer cannot follow (a `//` comment continued by a trailing
    # backslash swallows the next line) leaves the #if stack unbalanced at end of
    # file: the off-target drop is untrusted for that file, so it falls through.
    _expect FALLTHROUGH "#if stack unbalanced at end of file (untrusted) falls through" <<'EOF'
diff --git a/Source/Core/src/Sync/Cont.cpp b/Source/Core/src/Sync/Cont.cpp
--- a/Source/Core/src/Sync/Cont.cpp
+++ b/Source/Core/src/Sync/Cont.cpp
@@ -1,6 +1,6 @@
 // historical note \
 #if defined(__ANDROID__)
 void g(int x) {
-    (void)x;
+    launchMissiles(x);
 }
EOF
    # A directive after a closing */ is honoured by the compiler but invisible to the
    # #if tracker: here the new line is in the desktop #else arm, not the Android one.
    _expect FALLTHROUGH "directive after a closing comment marks the file untrusted" <<'EOF'
diff --git a/Source/Core/src/Sync/Hidden.cpp b/Source/Core/src/Sync/Hidden.cpp
--- a/Source/Core/src/Sync/Hidden.cpp
+++ b/Source/Core/src/Sync/Hidden.cpp
@@ -1,5 +1,5 @@
 #ifdef __ANDROID__
 /* desktop below */ #else
-    (void)x;
+    launchMissiles(x);
 #endif
EOF
    # A `//` comment spliced onto the next line hides a /* from the compiler, so the
    # lexer's block-comment state is wrong from there on: nothing may be dropped as
    # comment-only.
    _expect FALLTHROUGH "comment splice before a /* marks the file untrusted" <<'EOF'
diff --git a/Source/Core/src/Sync/Splice.cpp b/Source/Core/src/Sync/Splice.cpp
--- a/Source/Core/src/Sync/Splice.cpp
+++ b/Source/Core/src/Sync/Splice.cpp
@@ -1,7 +1,7 @@
 // note \
 still the note /* not a comment opener
 void g(int x) {
-    (void)x;
+    launchMissiles(x);
 }
 int h() { return 0; } // */
EOF
    # Four more shapes the per-line tracking cannot follow; each marks the file untrusted.
    _expect FALLTHROUGH "line splice inside a string literal marks the file untrusted" <<'EOF'
diff --git a/Source/Core/src/Sync/StrSplice.cpp b/Source/Core/src/Sync/StrSplice.cpp
--- a/Source/Core/src/Sync/StrSplice.cpp
+++ b/Source/Core/src/Sync/StrSplice.cpp
@@ -1,8 +1,8 @@
 const char* s = "abc\
 def /* not a comment opener";
 void g(int x) {
-    (void)x;
+    launchMissiles(x);
 }
 /* a real comment */
EOF
    _expect FALLTHROUGH "comment closed through a splice marks the file untrusted" <<'EOF'
diff --git a/Source/Core/src/Sync/BlkSplice.cpp b/Source/Core/src/Sync/BlkSplice.cpp
--- a/Source/Core/src/Sync/BlkSplice.cpp
+++ b/Source/Core/src/Sync/BlkSplice.cpp
@@ -1,8 +1,8 @@
 /* comment *\
 /
 void g(int x) {
-    (void)x;
+    launchMissiles(x);
 }
 /* a real comment */
EOF
    _expect FALLTHROUGH "comment between # and the directive name marks the file untrusted" <<'EOF'
diff --git a/Source/Core/src/Sync/HashCmt.cpp b/Source/Core/src/Sync/HashCmt.cpp
--- a/Source/Core/src/Sync/HashCmt.cpp
+++ b/Source/Core/src/Sync/HashCmt.cpp
@@ -1,5 +1,5 @@
 #ifdef __ANDROID__
 # /* desktop below */ else
-    (void)x;
+    launchMissiles(x);
 #endif
EOF
    _expect FALLTHROUGH "%: digraph directive marks the file untrusted" <<'EOF'
diff --git a/Source/Core/src/Sync/Digraph.cpp b/Source/Core/src/Sync/Digraph.cpp
--- a/Source/Core/src/Sync/Digraph.cpp
+++ b/Source/Core/src/Sync/Digraph.cpp
@@ -1,5 +1,5 @@
 #ifdef __ANDROID__
 %:else
-    (void)x;
+    launchMissiles(x);
 #endif
EOF
    # Directive splices the per-line tracking would misread: the join opens a string or
    # closes a comment, splits the directive name, or hides a directive after a comment.
    _expect FALLTHROUGH "string spliced inside a #define marks the file untrusted" <<'EOF'
diff --git a/Source/Core/src/Sync/MStr.cpp b/Source/Core/src/Sync/MStr.cpp
--- a/Source/Core/src/Sync/MStr.cpp
+++ b/Source/Core/src/Sync/MStr.cpp
@@ -1,8 +1,8 @@
 #define S "abc\
 def /* not a comment opener"
 void g(int x) {
-    (void)x;
+    launchMissiles(x);
 }
 /* a real comment */
EOF
    _expect FALLTHROUGH "comment closed through a #define splice marks the file untrusted" <<'EOF'
diff --git a/Source/Core/src/Sync/MBlk.cpp b/Source/Core/src/Sync/MBlk.cpp
--- a/Source/Core/src/Sync/MBlk.cpp
+++ b/Source/Core/src/Sync/MBlk.cpp
@@ -1,8 +1,8 @@
 #define X 1 /* c *\
 /
 void g(int x) {
-    (void)x;
+    launchMissiles(x);
 }
 /* a real comment */
EOF
    _expect FALLTHROUGH "directive name split by a splice marks the file untrusted" <<'EOF'
diff --git a/Source/Core/src/Sync/DSplit.cpp b/Source/Core/src/Sync/DSplit.cpp
--- a/Source/Core/src/Sync/DSplit.cpp
+++ b/Source/Core/src/Sync/DSplit.cpp
@@ -1,6 +1,6 @@
 #ifdef __ANDROID__
 androidOnly();
 #el\
 se
-    (void)x;
+    launchMissiles(x);
 #endif
EOF
    _expect FALLTHROUGH "bare # spliced onto its directive name marks the file untrusted" <<'EOF'
diff --git a/Source/Core/src/Sync/HSplit.cpp b/Source/Core/src/Sync/HSplit.cpp
--- a/Source/Core/src/Sync/HSplit.cpp
+++ b/Source/Core/src/Sync/HSplit.cpp
@@ -1,6 +1,6 @@
 #ifdef __ANDROID__
 androidOnly();
 #\
 else
-    (void)x;
+    launchMissiles(x);
 #endif
EOF
    # The `#else` is the macro's body text, so the desktop arm is still open below it.
    _expect FALLTHROUGH "#else on a #define continuation line is macro text, not a directive" <<'EOF'
diff --git a/Source/Core/src/Sync/MElse.cpp b/Source/Core/src/Sync/MElse.cpp
--- a/Source/Core/src/Sync/MElse.cpp
+++ b/Source/Core/src/Sync/MElse.cpp
@@ -1,6 +1,6 @@
 #ifndef __ANDROID__
 #define X \
 #else
-    (void)x;
+    launchMissiles(x);
 #endif
EOF
    _expect FALLTHROUGH "%: digraph directive after a comment marks the file untrusted" <<'EOF'
diff --git a/Source/Core/src/Sync/CDig.cpp b/Source/Core/src/Sync/CDig.cpp
--- a/Source/Core/src/Sync/CDig.cpp
+++ b/Source/Core/src/Sync/CDig.cpp
@@ -1,6 +1,6 @@
 #ifdef __ANDROID__
 androidOnly();
 /* x */ %:else
-    (void)x;
+    launchMissiles(x);
 #endif
EOF
    _expect FALLTHROUGH "comment, #, comment, else marks the file untrusted" <<'EOF'
diff --git a/Source/Core/src/Sync/CHC.cpp b/Source/Core/src/Sync/CHC.cpp
--- a/Source/Core/src/Sync/CHC.cpp
+++ b/Source/Core/src/Sync/CHC.cpp
@@ -1,6 +1,6 @@
 #ifdef __ANDROID__
 androidOnly();
 /* a */ # /* b */ else
-    (void)x;
+    launchMissiles(x);
 #endif
EOF
    # A '#' line that starts inside a block comment is comment text, so its splice is no
    # directive continuation.
    _expect FALLTHROUGH "splice on a #-line inside a block comment marks the file untrusted" <<'EOF'
diff --git a/Source/Core/src/Sync/BHS.cpp b/Source/Core/src/Sync/BHS.cpp
--- a/Source/Core/src/Sync/BHS.cpp
+++ b/Source/Core/src/Sync/BHS.cpp
@@ -1,9 +1,9 @@
 /* notes:
 # see below *\
 /
 void g(int x) {
-    (void)x;
+    launchMissiles(x);
 }
 /* a real comment */
EOF
    # An ImGui id path in a string ("**/##id") is not a comment closer before a directive.
    _expect EXEMPT "ImGui id path string keeps the file trusted" <<'EOF'
diff --git a/Source/Core/src/Sync/IdPath.cpp b/Source/Core/src/Sync/IdPath.cpp
--- a/Source/Core/src/Sync/IdPath.cpp
+++ b/Source/Core/src/Sync/IdPath.cpp
@@ -1,6 +1,6 @@
 static const char* kId = "**/##AiAssistantInput";
 #ifdef __ANDROID__
-androidOnly(1);
+androidOnly(2);
 #endif
EOF
    # '/' + splice + '*' opens a comment the per-line lexer never sees; here it hides a #else, so
    # the new line is in the desktop arm.
    _expect FALLTHROUGH "comment opened through a #define splice marks the file untrusted" <<'EOF'
diff --git a/Source/Core/src/Sync/SlashSplice.cpp b/Source/Core/src/Sync/SlashSplice.cpp
--- a/Source/Core/src/Sync/SlashSplice.cpp
+++ b/Source/Core/src/Sync/SlashSplice.cpp
@@ -1,7 +1,8 @@
 #ifndef __ANDROID__
 #define X 1 /\
 * c
 #else
 */
+    launchMissiles(x);
 #endif
EOF
    # Spliced directive lines holding a quote or a comment token are not trusted, whether or not
    # the token closes before the splice.
    _expect FALLTHROUGH "char literal on a spliced #define marks the file untrusted" <<'EOF'
diff --git a/Source/Core/src/Sync/SqSplice.cpp b/Source/Core/src/Sync/SqSplice.cpp
--- a/Source/Core/src/Sync/SqSplice.cpp
+++ b/Source/Core/src/Sync/SqSplice.cpp
@@ -1,6 +1,6 @@
 #define C 'a' \
     + 1
 #ifdef __ANDROID__
-androidOnly(1);
+androidOnly(2);
 #endif
EOF
    _expect FALLTHROUGH "line comment on a #define continuation marks the file untrusted" <<'EOF'
diff --git a/Source/Core/src/Sync/LcSplice.cpp b/Source/Core/src/Sync/LcSplice.cpp
--- a/Source/Core/src/Sync/LcSplice.cpp
+++ b/Source/Core/src/Sync/LcSplice.cpp
@@ -1,6 +1,6 @@
 #define X \
     1 // one
 #ifdef __ANDROID__
-androidOnly(1);
+androidOnly(2);
 #endif
EOF
    _expect FALLTHROUGH "comment closer on a #define continuation marks the file untrusted" <<'EOF'
diff --git a/Source/Core/src/Sync/CcSplice.cpp b/Source/Core/src/Sync/CcSplice.cpp
--- a/Source/Core/src/Sync/CcSplice.cpp
+++ b/Source/Core/src/Sync/CcSplice.cpp
@@ -1,6 +1,6 @@
 #define X \
     y */ z
 #ifdef __ANDROID__
-androidOnly(1);
+androidOnly(2);
 #endif
EOF
    # A '#else' inside a comment or a raw string is text: the desktop arm is still open below it.
    _expect FALLTHROUGH "#else inside a block comment is not a directive" <<'EOF'
diff --git a/Source/Core/src/Sync/CmtElse.cpp b/Source/Core/src/Sync/CmtElse.cpp
--- a/Source/Core/src/Sync/CmtElse.cpp
+++ b/Source/Core/src/Sync/CmtElse.cpp
@@ -1,7 +1,7 @@
 #ifndef __ANDROID__
 /* notes
 #else
 */
-    (void)x;
+    launchMissiles(x);
 #endif
EOF
    _expect FALLTHROUGH "#else inside a raw string literal is not a directive" <<'EOF'
diff --git a/Source/Core/src/Sync/RawElse.cpp b/Source/Core/src/Sync/RawElse.cpp
--- a/Source/Core/src/Sync/RawElse.cpp
+++ b/Source/Core/src/Sync/RawElse.cpp
@@ -1,7 +1,7 @@
 #ifndef __ANDROID__
 const char* s = R"(
 #else
 )";
-    (void)x;
+    launchMissiles(x);
 #endif
EOF
    # '"' is a legal raw-string delimiter character: R""( ... )"" holds the comment opener.
    _expect FALLTHROUGH "raw string delimited by a quote keeps its contents as data" <<'EOF'
diff --git a/Source/Core/src/Sync/RawQ.cpp b/Source/Core/src/Sync/RawQ.cpp
--- a/Source/Core/src/Sync/RawQ.cpp
+++ b/Source/Core/src/Sync/RawQ.cpp
@@ -1,8 +1,9 @@
 int g(int x) {
     const char* s = R""(
 /* not a comment
 )"";
+    launchMissiles(x);
 /* real */
     return x;
 }
EOF
    # x$R is one identifier to the compilers, so "(" after it is an ordinary string.
    _expect FALLTHROUGH "'\$' before an R prefix marks the file untrusted" <<'EOF'
diff --git a/Source/Core/src/Sync/Dollar.cpp b/Source/Core/src/Sync/Dollar.cpp
--- a/Source/Core/src/Sync/Dollar.cpp
+++ b/Source/Core/src/Sync/Dollar.cpp
@@ -1,5 +1,6 @@
 #ifdef __ANDROID__
 const char* k = x$R"(";
 #else
 const char* j = ")";
+int launch = launchMissiles(5);
 #endif
EOF
    # Control bytes are written with printf so no editor strips them.
    _expect FALLTHROUGH "carriage return inside a line marks the file untrusted" < <(printf '%s\n' \
        'diff --git a/Source/Core/src/Sync/Cr.cpp b/Source/Core/src/Sync/Cr.cpp' \
        '--- a/Source/Core/src/Sync/Cr.cpp' '+++ b/Source/Core/src/Sync/Cr.cpp' '@@ -1,3 +1,4 @@' \
        ' void g(int x) {' "+    // note$(printf '\r')    launchMissiles(x);" '     (void)x;' ' }')
    _expect FALLTHROUGH "form feed before #else marks the file untrusted" < <(printf '%s\n' \
        'diff --git a/Source/Core/src/Sync/Ff.cpp b/Source/Core/src/Sync/Ff.cpp' \
        '--- a/Source/Core/src/Sync/Ff.cpp' '+++ b/Source/Core/src/Sync/Ff.cpp' '@@ -1,4 +1,5 @@' \
        ' #ifdef __ANDROID__' ' androidOnly();' " $(printf '\f')#else" '+launchMissiles(x);' ' #endif')
    _expect FALLTHROUGH "backslash then whitespace on a #define marks the file untrusted" < <(printf '%s\n' \
        'diff --git a/Source/Core/src/Sync/Ws.cpp b/Source/Core/src/Sync/Ws.cpp' \
        '--- a/Source/Core/src/Sync/Ws.cpp' '+++ b/Source/Core/src/Sync/Ws.cpp' '@@ -1,4 +1,5 @@' \
        ' #ifdef __ANDROID__' ' #define X \ ' ' #else' '+launchMissiles(x);' ' #endif')
    # A NUL byte makes git print the whole file as one "Binary files ... differ" line.
    _expect FALLTHROUGH "a C++ file git shows as binary is never exempt" <<'EOF'
diff --git a/Source/Core/src/Sync/Nul.cpp b/Source/Core/src/Sync/Nul.cpp
index 1111111..2222222 100644
Binary files a/Source/Core/src/Sync/Nul.cpp and b/Source/Core/src/Sync/Nul.cpp differ
EOF
    # 1.R is one pp-number to the compilers, so "(" after it is an ordinary string.
    _expect FALLTHROUGH "R continuing a pp-number marks the file untrusted" <<'EOF'
diff --git a/Source/Core/src/Sync/PpNum.cpp b/Source/Core/src/Sync/PpNum.cpp
--- a/Source/Core/src/Sync/PpNum.cpp
+++ b/Source/Core/src/Sync/PpNum.cpp
@@ -1,5 +1,6 @@
 #ifdef __ANDROID__
 const char* k = 1.R"(";
 #else
 const char* j = ")";
+int launch = launchMissiles(5);
 #endif
EOF
    # A quote after a digit that no digit or letter follows opens a character literal, not a
    # digit separator: here the string after it holds the comment opener.
    _expect FALLTHROUGH "a quote after a digit is a char literal unless a digit or letter follows" <<'EOF'
diff --git a/Source/Core/src/Sync/DigitSep.cpp b/Source/Core/src/Sync/DigitSep.cpp
--- a/Source/Core/src/Sync/DigitSep.cpp
+++ b/Source/Core/src/Sync/DigitSep.cpp
@@ -1,6 +1,6 @@
 #define M(a) a(1'"', "/*")
 void g(int x) {
-    (void)x;
+    launchMissiles(x);
 }
 /* real */
EOF
    _expect FALLTHROUGH "carriage return inside a directive line is never exempt" < <(printf '%s\n' \
        'diff --git a/Source/Core/src/Sync/CrDir.cpp b/Source/Core/src/Sync/CrDir.cpp' \
        '--- a/Source/Core/src/Sync/CrDir.cpp' '+++ b/Source/Core/src/Sync/CrDir.cpp' '@@ -1,2 +1,3 @@' \
        ' #ifdef __ANDROID__' ' androidOnly();' "+#endif$(printf '\r')    launchMissiles(x);")
    # An included implementation file is product code too: its lines are classified.
    _expect FALLTHROUGH "real code in an .inl file is not exempt" <<'EOF'
diff --git a/Source/Core/src/Sync/Impl.inl b/Source/Core/src/Sync/Impl.inl
--- a/Source/Core/src/Sync/Impl.inl
+++ b/Source/Core/src/Sync/Impl.inl
@@ -1,1 +1,2 @@
 // implementation, included by Sync.cpp
+launchMissiles(x);
EOF
    # A macro's own continuation lines are not a splice the tracking misreads: an
    # off-target arm holding a multi-line #define still drops its new lines.
    _expect EXEMPT "multi-line #define in an Android arm stays exempt" <<'EOF'
diff --git a/Source/Core/src/Sync/Macro.cpp b/Source/Core/src/Sync/Macro.cpp
--- a/Source/Core/src/Sync/Macro.cpp
+++ b/Source/Core/src/Sync/Macro.cpp
@@ -1,6 +1,6 @@
 #if defined(__ANDROID__)
 #define LOG_ANDROID(x) \
     do { log(x); } while (0)
-    androidOnly(1);
+    androidOnly(2);
 #endif
EOF
    # A line inside a raw string literal is string DATA (an embedded script or
    # shader) even when it looks like a comment / include / brace.
    _expect FALLTHROUGH "comment-looking line inside a raw string literal is data" <<'EOF'
diff --git a/Source/Core/src/Sync/Lua.cpp b/Source/Core/src/Sync/Lua.cpp
--- a/Source/Core/src/Sync/Lua.cpp
+++ b/Source/Core/src/Sync/Lua.cpp
@@ -1,4 +1,4 @@
 static const char* kScript = R"(
-// old
+// new
 )";
EOF

    # The SAME #1021 shape on the desktop (#ifdef _WIN32) side must still gate.
    _expect FALLTHROUGH "statement on the #ifdef _WIN32 (desktop) side" <<'EOF'
diff --git a/Source/Core/src/SubprocessCapture.cpp b/Source/Core/src/SubprocessCapture.cpp
--- a/Source/Core/src/SubprocessCapture.cpp
+++ b/Source/Core/src/SubprocessCapture.cpp
@@ -1,8 +1,9 @@
 #include "SubprocessCapture.h"
 #ifdef _WIN32
     std::string out = ReadPipeWin();
+    out.resize(trimmed);
 #elif defined(__ANDROID__)
     std::string out(ptr, length);
 #else
     std::string out = ReadPipePosix();
 #endif
EOF

    # The non-_WIN32 #else arm is compiled AND run on Linux CI — must still gate.
    _expect FALLTHROUGH "statement in the non-_WIN32 #else arm (built on Linux CI)" <<'EOF'
diff --git a/Source/Core/src/SubprocessCapture.cpp b/Source/Core/src/SubprocessCapture.cpp
--- a/Source/Core/src/SubprocessCapture.cpp
+++ b/Source/Core/src/SubprocessCapture.cpp
@@ -1,8 +1,9 @@
 #include "SubprocessCapture.h"
 #ifdef _WIN32
     std::string out = ReadPipeWin();
 #elif defined(__ANDROID__)
     std::string out(ptr, length);
 #else
     std::string out = ReadPipePosix();
+    out.resize(trimmed);
 #endif
EOF

    # No CI job builds macOS/iOS, so an __APPLE__ / TARGET_OS_* arm is validated by
    # nothing — it must gate like desktop code (only __ANDROID__ has a CI build).
    _expect FALLTHROUGH "__APPLE__ && TARGET_OS_IOS arm (no Apple CI job validates it)" <<'EOF'
diff --git a/Source/Core/src/Config/Paths.cpp b/Source/Core/src/Config/Paths.cpp
--- a/Source/Core/src/Config/Paths.cpp
+++ b/Source/Core/src/Config/Paths.cpp
@@ -1,5 +1,6 @@
 #if SMATCHET_WITH_FOO
 #  if defined(__APPLE__) && TARGET_OS_IOS  // iOS sandbox
+    root = SandboxRoot();
 #  endif
 #endif
EOF
    _expect FALLTHROUGH "#ifdef __APPLE__ arm (no Apple CI job validates it)" <<'EOF'
diff --git a/Source/Core/src/Config/Paths.cpp b/Source/Core/src/Config/Paths.cpp
--- a/Source/Core/src/Config/Paths.cpp
+++ b/Source/Core/src/Config/Paths.cpp
@@ -1,3 +1,4 @@
 #ifdef __APPLE__
+    root = BundleRoot();
 #endif
EOF

    # A mixed condition (Linux is a test target) and the #else of a POSITIVE
    # Android guard are both desktop-reachable — must gate.
    _expect FALLTHROUGH "__ANDROID__ || __linux__ arm, and the #else of #if defined(__ANDROID__)" <<'EOF'
diff --git a/Source/Core/src/HostIntegration.cpp b/Source/Core/src/HostIntegration.cpp
--- a/Source/Core/src/HostIntegration.cpp
+++ b/Source/Core/src/HostIntegration.cpp
@@ -1,6 +1,7 @@
 #if defined(__ANDROID__) || defined(__linux__)
+    OpenWithXdg(path);
 #endif
 #if defined(__ANDROID__)
 #else
 #endif
EOF
    _expect FALLTHROUGH "#else of a positive #if defined(__ANDROID__)" <<'EOF'
diff --git a/Source/Core/src/HostIntegration.cpp b/Source/Core/src/HostIntegration.cpp
--- a/Source/Core/src/HostIntegration.cpp
+++ b/Source/Core/src/HostIntegration.cpp
@@ -1,4 +1,5 @@
 #if defined(__ANDROID__)
 #else
+    LaunchDesktop(path);
 #endif
EOF

    # A relocation whose .cpp copy differs by one token is NOT byte-identical —
    # behaviour may have changed, so it must gate.
    _expect FALLTHROUGH "header body 'relocated' with an edited line" <<'EOF'
diff --git a/Source/Core/include/LocalizedImGui.h b/Source/Core/include/LocalizedImGui.h
--- a/Source/Core/include/LocalizedImGui.h
+++ b/Source/Core/include/LocalizedImGui.h
@@ -1,7 +1,3 @@
 #pragma once
-inline void HookOnLastItem(char* buf, std::size_t size) {
-    if (::ImGui::IsItemActive()) {
-        g_router.Register(buf, size);
-    }
-}
+void HookOnLastItem(char* buf, std::size_t size);
diff --git a/Source/Core/src/DictationHook.cpp b/Source/Core/src/DictationHook.cpp
--- /dev/null
+++ b/Source/Core/src/DictationHook.cpp
@@ -0,0 +1,6 @@
+#include "LocalizedImGui.h"
+void HookOnLastItem(char* buf, std::size_t size) {
+    if (::ImGui::IsItemActive() || ::ImGui::IsItemFocused()) {
+        g_router.Register(buf, size);
+    }
+}
EOF

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
# --no-renames: a rename's carried-over lines are content the gate must see, never a silent move.
# core.quotePath=false: a non-ASCII path stays a plain path the patterns below can match.
mapfile -t CHANGED < <(git -c core.quotePath=false diff --no-renames --name-only --diff-filter=ACMR \
    "$MERGE_BASE"...HEAD 2>/dev/null || true)

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
        # Test surface — only actual test TUs count toward a delta. tests/support/
        # and tests/fixtures/ (shared helpers) are excluded as trivially
        # dismissable (an empty helper would "satisfy" the gate). Any OTHER
        # tests/ subdirectory counts by the *.test.cpp naming convention — a
        # fixed per-directory list red-walled the first PR to add a NEW harness
        # dir (tests/monkey/, #1637); a harness dir earns credit by naming its
        # test TUs *.test.cpp, not by a hand-synced allowlist.
        tests/support/*|tests/fixtures/*) ;;
        tests/*.test.cpp)
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
# --unified=100000: full-file context, so _prefilter_diff can see the #if nesting
# of every added line and pair a removed header body with its relocated copy.
GIT_DIFF_TMPFILE="$(mktemp)"
trap 'rm -f "$GIT_DIFF_TMPFILE"' EXIT
if ! git -c core.quotePath=false diff --no-renames --unified=100000 --diff-filter=ACMR "$MERGE_BASE"...HEAD -- \
        Source/Core Source/Plugins Source/Standalone tests >"$GIT_DIFF_TMPFILE" 2>/dev/null; then
    echo "[coverage-delta-gate] FAIL — git diff failed (bad MERGE_BASE '$MERGE_BASE' or git error)" >&2
    exit 1
fi
if ! EXEMPTION="$(_classify_diff_file "$GIT_DIFF_TMPFILE")"; then
    echo "[coverage-delta-gate] FAIL — diff prefilter (awk) failed" >&2
    exit 1
fi
if [ "$EXEMPTION" = "EXEMPT" ]; then
    echo "[coverage-delta-gate] PASS — test-light exemption: every product-code"
    echo "[coverage-delta-gate]        change is no-new-runtime-surface"
    echo "[coverage-delta-gate]        (comment/log/static_assert/include/preprocessor-guard/catch-scaffold/"
    echo "[coverage-delta-gate]        off-target platform arm/header->cpp body relocation)."
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
echo "Add tests under tests/Core/ (or tests/Commands/, tests/Lua/, tests/Plugins/, tests/ui/) for the"
echo "changed units. Changes that add no new runtime surface (comment-only,"
echo "logging-only, static_assert-only, forward-declaration-only, include-only,"
echo "preprocessor-guard-only, swallow->log catch, lines inside an __ANDROID__"
echo "arm, a byte-identical header->cpp body relocation)"
echo "are auto-exempted; if yours"
echo "genuinely cannot be"
echo "unit-tested, apply the"
echo "'tests-out-of-band' PR label to dismiss this gate."
exit 1

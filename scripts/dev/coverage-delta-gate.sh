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
# *every* added, modified AND removed line in first-party product files (every file
# under Source/Core, Source/Plugins, Source/Standalone, tests/ but known data: docs,
# assets, fixtures, scripts, build files — _DATA_EXT_RE) is provably
# no-new-runtime-surface. Removing a line is a change too: deleting a guard
# (`if (!confirmed) return;`) changes behaviour exactly as adding one does. A file
# renamed between the product trees, or into or out of them, is never exempt (the
# compilers newly build, or stop building, every line it carries). Classes
# (CONSERVATIVE — anything not on this list falls through to the normal
# coverage-delta gate):
#   * comment/marker-only  — //, /* */, doc-* continuation, // catch-all-ok: …
#   * logging-only         — a LOG_{DEBUG,INFO,WARN,ERROR,TRACE}(…); statement that
#                            starts a statement (after ; { or }), including the lines
#                            of one that wraps; the arguments are not inspected
#   * static_assert-only   — static_assert(…); (compile-time; the build is the test)
#   * forward-decl-only    — `class/struct/union/enum Foo;` name declarations
#                            (optionally template-prefixed) — a type name with no
#                            body and no object carries no runtime surface (the
#                            #1308 fan-in swaps a heavy include for a fwd-decl).
#   * include/using-only   — #include / using directives
#   * preprocessor-guard   — #if/#ifdef/#ifndef/#elif/#else/#endif conditional
#                            directives whose condition names a configuration macro
#                            (compile-config selection; the wrapped code is
#                            classified on its own changed lines, so a guard around
#                            NEW statements still falls through). A condition that
#                            is a constant, or holds a constant operand of || / &&
#                            (`#if 0` -> `#if 1`, `#if FOO || 1`), turns existing code
#                            on or off: NOT exempt. Nor are #define/#undef/#pragma
#                            (a macro can carry real logic).
#   * catch-scaffold       — exception-handler structure (catch (…) { , try { ,
#                            and the brace/closing tokens) whose body is only the
#                            above (the swallow→log pattern: no rethrow, no logic)
#   * build-only           — no .cpp/.h/.hpp product change at all (CMake/yml/sh/…)
#   * off-target platform arm — a changed line whose enclosing #if/#elif/#else arm
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
#   Every class but the shape checks needs C++ lexing and pairing context, so the diff is
#   generated with full file context: _prefilter_diff lexes both sides of each file, drops
#   the exempt lines, and hands _classify_diff each remaining line with its comments removed
#   and its literals emptied.
# A new function, a new branch, a changed condition, a new or removed statement — NOT exempt.
# Motivation: a GitHub merge queue runs this required check on the merge_group
# ref where PR labels don't apply, so tests-out-of-band can't dismiss it there;
# the gate must PASS legitimately for genuinely-untestable correctness diffs.
# See docs/plans/build-quality-velocity-hardening.md #14 + postmortems.md 2026-06-06.
#
# Override mechanism for local runs:
#   SMATCHET_COVERAGE_GATE_BASE   base ref to diff against. Default: origin/develop
#                                 (falls back to develop, then HEAD~1). A ref that does
#                                 not resolve fails the gate.
#   SMATCHET_COVERAGE_GATE_BYPASS set to 1 to short-circuit (advisory mode).
#
# Self-test (both-direction fixtures, no network):
#   bash scripts/dev/coverage-delta-gate.sh --selftest
#
# Exit codes:
#   0 — gate satisfied (no Source/Core change, test files also changed, or the
#       test-light exemption fired)
#   1 — gate failed (Source/Core changed without test delta and not exempt, or a
#       git command failed) / --selftest failure

set -euo pipefail

# ---------------------------------------------------------------------------
# Test-light exemption classifier
# ---------------------------------------------------------------------------
# The shapes of a no-new-runtime-surface line. Each is one whole statement or directive:
# _prefilter_diff empties every string and character literal (so a ';', '{' or '}' here is
# code, never text) and removes comments.
_USING_RE='^using[[:space:]][^;{}]*;$'
_STATIC_ASSERT_RE='^static_assert[[:space:]]*\([^;{}]*\)[[:space:]]*;$'
_CATCH_RE='^(\}[[:space:]]*)?catch[[:space:]]*\([^;{}]*\)[[:space:]]*\{$'
_TRY_RE='^(\}[[:space:]]*)?try([[:space:]]*\{)?$'
_BRACE_RE='^(\{|\}|\};)$'
_INCLUDE_RE='^#[[:space:]]*include(_next)?([^A-Za-z0-9_]|$)'
_PRAGMA_ONCE_RE='^#[[:space:]]*pragma[[:space:]]+once$'
_CONDITIONAL_RE='^#[[:space:]]*(if|ifdef|ifndef|elif|elifdef|elifndef|else|endif)([^A-Za-z0-9_]|$)'
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
_FWD_DECL_RE='^(template[[:space:]]*<[^{}]*>[[:space:]]*)?(class|struct|union|enum([[:space:]]+(class|struct))?)[[:space:]]+[A-Za-z_][A-Za-z0-9_]*([[:space:]]*:[[:space:]]*[A-Za-z_:][A-Za-z0-9_:]*)?[[:space:]]*;$'
# A constant (a pp-number, true or false, already replaced by '#') that is a whole operand of
# ||, && or !: the condition no longer depends on the configuration there.
_CONST_OPERAND_RE='(^|[|][|]|&&|!|[(])[[:space:]]*[(]*[[:space:]]*#[[:space:]]*[)]*[[:space:]]*([|][|]|&&|[)]|$)'

# _guard_selects_config <directive> — 0 when a conditional directive selects code by the
# build configuration; 1 when its condition is constant, or turns code on or off whatever
# the configuration (`#if 0` -> `#if 1`, `#if FOO || 1`, `#if BAR && 0`).
_guard_selects_config() {
    local cond norm
    [[ "$1" =~ ^#[[:space:]]*(if|elif)([^A-Za-z0-9_].*)?$ ]] || return 0
    cond="${BASH_REMATCH[2]}"
    # Every pp-number, true and false becomes '#'; `defined` is an operator, not a macro.
    norm="$(LC_ALL=C sed -E ":a
s/(^|[^A-Za-z0-9_#])([0-9][A-Za-z0-9_.']*|true|false)([^A-Za-z0-9_]|$)/\\1#\\3/
ta
s/(^|[^A-Za-z0-9_])defined([^A-Za-z0-9_]|$)/\\1 \\2/g" <<<"$cond")"
    [[ "$norm" =~ [A-Za-z_] ]] || return 1
    [[ "$norm" =~ $_CONST_OPERAND_RE ]] && return 1
    return 0
}

# _line_is_no_runtime_surface <code> — decide one changed line, as _prefilter_diff prints it
# (trimmed, comments removed, literals emptied). Returns 0 (exempt) / 1 (real surface).
# CONSERVATIVE: unknown ⇒ 1.
_line_is_no_runtime_surface() {
    local code="$1"
    [ -z "$code" ] && return 0
    if [[ "$code" =~ $_CONDITIONAL_RE ]]; then
        _guard_selects_config "$code"
        return
    fi
    [[ "$code" =~ $_INCLUDE_RE ]] && return 0
    [[ "$code" =~ $_PRAGMA_ONCE_RE ]] && return 0
    [[ "$code" =~ $_USING_RE ]] && return 0
    [[ "$code" =~ $_STATIC_ASSERT_RE ]] && return 0
    [[ "$code" =~ $_FWD_DECL_RE ]] && return 0
    [[ "$code" =~ $_CATCH_RE ]] && return 0
    [[ "$code" =~ $_TRY_RE ]] && return 0
    [[ "$code" =~ $_BRACE_RE ]] && return 0
    return 1
}

# Read _prefilter_diff's output on stdin (one changed product line per line); emit "EXEMPT"
# or "FALLTHROUGH" on stdout.
# EXEMPT  ⇒ every changed line is no-new-runtime-surface (or there are none — build-only).
#           The caller short-circuits to PASS.
# FALLTHROUGH ⇒ at least one changed line is real surface; the caller runs the
#           unchanged coverage-delta logic.
_classify_diff() {
    local line
    while IFS= read -r line; do
        if ! _line_is_no_runtime_surface "$line"; then
            echo FALLTHROUGH
            return 0
        fi
    done
    echo EXEMPT
}

# Data files under the product trees, never built as C++ (docs, assets, fixtures, scripts, build
# files). One list for the bash checks and the awk prefilter (passed in as -v dataext).
_DATA_EXT_RE='md|txt|json|png|jpg|jpeg|gif|svg|ico|bmp|wav|ttf|otf|woff|woff2|a|lib|so|dll|tsv|csv|bats|py|sh|ps1|cmake|in|rc|yml|yaml|toml|xml|html'
# A configure_file template of a C/C++ file (Foo.cpp.in) is built once configured: not data.
_CXX_TEMPLATE_RE='c|cc|cpp|cxx|h|hh|hpp|hxx|inl|ipp|tpp|inc'

# _product_root <path> — the product tree a path lies in (Source/Core, Source/Plugins,
# Source/Standalone, tests), or nothing.
_product_root() {
    case "$1" in
        Source/Core/*) echo Source/Core ;;
        Source/Plugins/*) echo Source/Plugins ;;
        Source/Standalone/*) echo Source/Standalone ;;
        tests/*) echo tests ;;
    esac
}

# _is_product_path <path> — a file under the product trees whose lines a compiler may build. Fail
# closed: anything there that is not known data counts, whatever its extension (or none), because any
# file can be #included.
_is_product_path() {
    [ -n "$(_product_root "$1")" ] || return 1
    case "$1" in
        tests/fuzz/corpus/*|*/.gitkeep|*/.gitignore) return 1 ;;
    esac
    [[ "$1" =~ \.(${_CXX_TEMPLATE_RE})\.in$ ]] && return 0
    [[ ! "$1" =~ \.(${_DATA_EXT_RE})$ ]]
}

# _rename_class <path> — the product tree a path is built in, or "none" for a path outside them
# (or data inside them). A rename that changes it changes what the compilers build.
_rename_class() {
    if _is_product_path "$1"; then
        _product_root "$1"
    else
        echo none
    fi
}

# ---------------------------------------------------------------------------
# Full-context prefilter (lexing, logging, platform-arm + header→cpp body relocation exemptions)
# ---------------------------------------------------------------------------
# _prefilter_diff <diff-file> — read a FULL-CONTEXT unified diff (git diff
# --unified=<huge>, so every hunk carries the whole file and the #if nesting of
# each changed line is knowable) and print, one per line, every changed line of a
# product file that is NOT exempted here, for _classify_diff: trimmed, its comments
# removed and every string / character literal emptied ("" / ''). A line that is not
# trustworthy as text prints as a sentinel _classify_diff never exempts.
#
# Both sides of each hunk are lexed: the post-image (context and '+' lines) for an
# added line, the pre-image (context and '-' lines) for a removed one, each with its
# own state. A small lexer walks each side token by token, as the compilers do
# (identifiers, pp-numbers, header-names, string / char / raw string literals),
# tracking /* */ comments and raw string literals (R"delim( ... )delim") across
# lines. A changed line that is only whitespace/comment is dropped (no surface). A
# changed line that starts inside a raw string literal is string DATA and prints as the
# sentinel, as does one with a carriage return inside it.
#
# A file whose lexing cannot be trusted gets no exemption at all — every changed line
# prints as the sentinel: C, an unknown extension, a '$' / non-ASCII / \u in an
# identifier, a hunk that ends with the #if stack open, closes an arm it never opened,
# ends inside a comment/raw string, splices a line with a trailing backslash outside a
# directive's own continuation, splices a directive where the join could change what the
# lexer sees (a split directive name, or a quote, comment token, '/' or '*' at the splice
# on a spliced directive line), puts a comment between '#' and the directive name, between
# #include / __has_include and the header-name, or a directive after a closing */, uses a
# %: digraph directive, or holds a control byte the compilers read differently (a carriage
# return inside a line, a form feed, a vertical tab, a backslash followed by whitespace).
# A product file git prints as binary ("Binary files ... differ", e.g. one NUL byte) or
# under a quoted path is never exempt (_classify_diff_file). Exemptions, all conservative
# (anything unrecognised is printed, i.e. classified):
#
#   1. Logging. The lexer follows LOG_{DEBUG,INFO,WARN,ERROR,TRACE}( ... ); calls that
#      start a statement (the previous token, outside directives, is ; { or } — or the
#      call opens the file). A changed line whose every token belongs to such a call
#      (its name, its parenthesised arguments, its closing ;) is dropped, so an edit to
#      one line of a wrapped call is exempt and `LOG_X(...); other();` is not. A LOG
#      after `if (c)` or `else`, or at the top of a partial hunk, is not a statement
#      start: it falls through.
#
#   2. Off-target platform arm. Each side keeps an #if/#ifdef/#ifndef/#elif/#else/
#      #endif stack. An arm is OFF-TARGET when its effective condition requires an
#      off-target platform macro: its own condition is an ||/&& combination of
#      ONLY __ANDROID__ atoms (`defined(X)`, `defined X`, bare `X`), or an
#      earlier arm of the same group was the pure negation of such a combination
#      (`#ifndef __ANDROID__ … #else`). Android is the only off-target platform
#      with a CI build (mobile-android-ndk / APK jobs); __APPLE__ / TARGET_OS_*
#      arms stay gated until a macOS/iOS job exists. A changed line inside any
#      off-target arm is dropped. So `#ifdef _WIN32 … #else` stays gated (the
#      #else arm is the Linux/POSIX path CI builds and runs), as does
#      `#elif defined(__ANDROID__) || defined(__linux__)`. A directive line with a
#      backslash continuation or a multi-line comment is classified OTHER (gated).
#      A `#if` line that starts inside a /* */ comment or a raw string literal is
#      not a directive and is not tracked. The stack resets at every hunk header,
#      so a partial-context diff can only under-exempt.
#
#   3. Header→cpp body relocation. Pass 1 collects every complete, brace-balanced
#      function definition inside a run of REMOVED lines of a product header
#      (.h/.hpp), and every run of ADDED lines of a product .cpp/.cc/.cxx. A
#      definition re-added as a contiguous, byte-identical (per-line trimmed;
#      `inline` dropped from the signature line) block in a .cpp is a relocation:
#      both copies are dropped, as are the header's added declaration of the same
#      signature (`<sig>;`) and a bare namespace opener in a .cpp that received a
#      relocated body. The body moved unchanged, so no new runtime surface — the
#      existing callers' tests still exercise it.
_PREFILTER_AWK="$(cat <<'AWK'
function trim(s) { sub(/^[ \t\r]+/, "", s); sub(/[ \t\r]+$/, "", s); return s }
# Mirrors _is_product_path (dataext is _DATA_EXT_RE, cxxtmpl is _CXX_TEMPLATE_RE).
function is_prod(p) {
    if (p !~ /^(Source\/(Core|Plugins|Standalone)|tests)\//) return 0
    if (p ~ /^tests\/fuzz\/corpus\// || p ~ /\/\.git(keep|ignore)$/) return 0
    if (p ~ ("\\.(" cxxtmpl ")\\.in$")) return 1
    return p !~ ("\\.(" dataext ")$")
}
# A UTF-8 byte-order mark opening a file is no token to the compilers. (The prefilter runs under
# LC_ALL=C, so every awk reads bytes.)
function strip_bom(s) { return substr(s, 1, 3) == "\357\273\277" ? substr(s, 4) : s }
# The C++ files this lexer reads; any other product file (C, an unknown extension) is untrusted.
function is_cxx(p) { return p ~ /\.(cpp|cc|cxx|h|hpp|hxx|hh|inl|ipp|tpp|inc)$/ }
function is_hdr(p) { return p ~ /\.(h|hpp)$/ }
function is_cpp(p) { return p ~ /\.(cpp|cc|cxx)$/ }
# The path of a "--- a/<path>" / "+++ b/<path>" header; git ends one with a TAB when the path holds a
# space. A deleted or added file's other side is /dev/null, which is no product path.
function path_of(raw,   p) { p = substr(raw, 5); sub(/\t$/, "", p); sub(/^[ab]\//, "", p); return p }
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
# Update the #if stack for one line; returns 1 when it is a conditional directive
# (#define/#include/#pragma return 0 and are classified as ordinary lines, so one
# inside an off-target arm is still dropped).
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
# Lexical state of one side of a hunk: lx_blk (inside a /* */ comment), lx_raw (inside a raw
# string literal, closed by ")" lx_rdel "\""), lx_macro (the next line continues a directive),
# lx_untrusted, the #if stack (depth, arm[], neg[], uflow), the logging-call tracker (lx_prev, the
# last token outside directives; lg_open, lg_depth, lg_close) and lx_bom (the next line opens the
# file, so a byte-order mark may lead it).
function lex_reset() {
    lx_blk = 0; lx_raw = 0; lx_rdel = ""; lx_untrusted = 0; lx_macro = 0
    depth = 0; uflow = 0
    lx_prev = ""; lg_open = 0; lg_depth = 0; lg_close = 0; lx_bom = 0
}
# Each side of a hunk keeps its own copy of that state; side() swaps the named one in.
function side_save(m,   k) {
    SS[m, "blk"] = lx_blk; SS[m, "raw"] = lx_raw; SS[m, "rdel"] = lx_rdel; SS[m, "unt"] = lx_untrusted
    SS[m, "mac"] = lx_macro; SS[m, "depth"] = depth; SS[m, "uflow"] = uflow; SS[m, "prev"] = lx_prev
    SS[m, "lgo"] = lg_open; SS[m, "lgd"] = lg_depth; SS[m, "lgc"] = lg_close; SS[m, "bom"] = lx_bom
    for (k = 1; k <= depth; k++) { SA[m, k] = arm[k]; SN[m, k] = neg[k] }
}
function side_load(m,   k) {
    lx_blk = SS[m, "blk"]; lx_raw = SS[m, "raw"]; lx_rdel = SS[m, "rdel"]; lx_untrusted = SS[m, "unt"]
    lx_macro = SS[m, "mac"]; depth = SS[m, "depth"]; uflow = SS[m, "uflow"]; lx_prev = SS[m, "prev"]
    lg_open = SS[m, "lgo"]; lg_depth = SS[m, "lgd"]; lg_close = SS[m, "lgc"]; lx_bom = SS[m, "bom"]
    for (k = 1; k <= depth; k++) { arm[k] = SA[m, k]; neg[k] = SN[m, k] }
}
function side(m) { if (cur_side != m) { side_save(cur_side); side_load(m); cur_side = m } }
# A hunk header resets both sides. A side that starts at line 1 starts at the top of the file: a
# byte-order mark may lead its first line, and a logging call there starts a statement.
function hunk_begin(h,   t, pre_start, post_start) {
    t = h; sub(/^@@ -/, "", t); pre_start = t + 0
    t = h; sub(/^@@ -[0-9]+(,[0-9]+)? \+/, "", t); post_start = t + 0
    lex_reset(); lx_bom = (pre_start <= 1); lx_prev = pre_start <= 1 ? "" : "?"; side_save("pre")
    lex_reset(); lx_bom = (post_start <= 1); lx_prev = post_start <= 1 ? "" : "?"; side_save("post")
    cur_side = "post"
    hunk_open = 1
}
# feed(m, raw) — run one diff line's text through side m; returns 1 for a conditional directive.
function feed(m, raw,   b) {
    side(m)
    b = substr(raw, 2)
    if (lx_bom) { b = strip_bom(b); lx_bom = 0 }
    return post_line(b)
}
# lg_tok(kind, val) — one token for the logging-call tracker ("id" / "num" / "lit" / "punct", with the
# identifier or punctuator). A token outside a statement-starting LOG_*( ... ); call sets lx_nonlog.
# A directive's tokens are never part of one.
function lg_tok(kind, val,   inlog) {
    if (lx_dir) { lx_nonlog = 1; return }
    inlog = 0
    if (lg_depth > 0) {
        inlog = 1
        if (val == "(") lg_depth++
        else if (val == ")" && --lg_depth == 0) lg_close = 1
    } else if (lg_open) {
        lg_open = 0
        if (val == "(") { lg_depth = 1; inlog = 1 }
    } else if (lg_close) {
        lg_close = 0
        inlog = (val == ";")
    } else if (kind == "id" && val ~ /^LOG_(DEBUG|INFO|WARN|ERROR|TRACE)$/ &&
               (lx_prev == "" || lx_prev == ";" || lx_prev == "{" || lx_prev == "}")) {
        lg_open = 1
        inlog = 1
    }
    if (!inlog) lx_nonlog = 1
    lx_prev = kind == "punct" ? val : kind
}
# lex_line(s, dir) — advance the lexical state across one line (dir: the line is a directive or
# continues one). Sets lx_code = 1 when any non-whitespace byte lies outside a comment (string-literal
# bytes are code), lx_nonlog (see lg_tok), and lx_text: the line with each comment replaced by a space
# and each string / char literal emptied.
# It scans token by token, as the compilers do, so a quote opens a literal only where
# a token starts: identifiers (a raw-string prefix is a whole identifier R, u8R, uR, UR
# or LR), pp-numbers (1'000, 1.R, 1e+'5 and 0x1e+5 are each one token), header-names
# after #include / __has_include (skipped whole), and ordinary string / char literals,
# which cannot span lines. A '$', non-ASCII byte or \u / \U in an identifier or
# pp-number is accepted differently by different compilers: the file is not trusted, as
# it is for a comment before a header-name (the per-line lexer reads the header-name as
# tokens there).
function lex_line(s, dir,   i, n, c, c2, k, m, d, j, id, rest, q) {
    lx_code = 0
    lx_nonlog = 0
    lx_text = ""
    lx_dir = dir
    n = length(s)
    i = 1
    if (!lx_blk && !lx_raw && match(s, /^[ \t]*#[ \t]*(include_next|include|import)/) &&
        substr(s, RLENGTH + 1, 1) !~ /[A-Za-z0-9_]/) {
        lx_code = 1
        lx_nonlog = 1
        m = RLENGTH
        rest = substr(s, m + 1)
        match(rest, /^[ \t]*/)
        k = RLENGTH
        rest = substr(rest, k + 1)
        if (rest ~ /^\/[*\/]/) lx_untrusted = 1
        if (rest ~ /^[<"]/) {
            q = substr(rest, 1, 1) == "<" ? ">" : "\""
            d = index(substr(rest, 2), q)
            if (d == 0) { lx_untrusted = 1; return }
            lx_text = substr(s, 1, m + k) (q == ">" ? "<>" : "\"\"")
            i = m + k + d + 2
        }
    }
    while (i <= n) {
        if (lx_blk) {
            k = index(substr(s, i), "*/")
            if (k == 0) return
            i += k + 1
            lx_blk = 0
            lx_text = lx_text " "
            continue
        }
        if (lx_raw) {
            lx_code = 1
            k = index(substr(s, i), ")" lx_rdel "\"")
            if (k == 0) return
            i += k + length(lx_rdel) + 1
            lx_raw = 0
            lx_text = lx_text "\"\""
            continue
        }
        c = substr(s, i, 1)
        if (c ~ /[ \t\r\f\v]/) { lx_text = lx_text c; i++; continue }
        c2 = substr(s, i, 2)
        if (c2 == "//") {
            # A trailing backslash splices the next line into this comment, which the
            # per-line lexer cannot follow: the file's tracking is not trusted.
            if (s ~ /\\[ \t\r]*$/) lx_untrusted = 1
            return
        }
        if (c2 == "/*") { lx_blk = 1; i += 2; continue }
        lx_code = 1
        if (c ~ /[A-Za-z_$]/ || c ~ /[^\t -~]/ || c2 ~ /^\\[uU]$/) {
            j = i
            while (j <= n) {
                k = substr(s, j, 1)
                if (k ~ /[A-Za-z0-9_]/) { j++; continue }
                if (k == "$" || k ~ /[^\t -~]/) { lx_untrusted = 1; j++; continue }
                if (k == "\\" && substr(s, j + 1, 1) ~ /[uU]/) { lx_untrusted = 1; j += 2; continue }
                break
            }
            id = substr(s, i, j - i)
            i = j
            if (substr(s, i, 1) == "\"" && id ~ /^(u8|u|U|L)?R$/) {
                m = substr(s, i + 1)
                d = index(m, "(")
                # A delimiter is up to 16 characters, any but space, the parentheses, backslash and the
                # tab, vertical-tab and form-feed controls ('"' is allowed: R""( ... )"").
                if (d > 0 && d <= 17 && substr(m, 1, d - 1) !~ /[ \t\v\f\\)]/) {
                    lx_rdel = substr(m, 1, d - 1)
                    lx_raw = 1
                    lx_text = lx_text id
                    lg_tok("lit", "")
                    i += d + 1
                    continue
                }
                lx_untrusted = 1 # an R prefix with no delimiter the compilers accept
            }
            lx_text = lx_text id
            lg_tok("id", id)
            if (id == "__has_include" || id == "__has_include_next") {
                rest = substr(s, i)
                if (rest ~ /^[ \t]*(\/[*\/]|\([ \t]*\/[*\/])/) lx_untrusted = 1
                if (match(rest, /^[ \t]*\([ \t]*[<"]/)) {
                    q = substr(rest, RLENGTH, 1) == "<" ? ">" : "\""
                    m = index(substr(rest, RLENGTH + 1), q)
                    if (m == 0) { lx_untrusted = 1; return }
                    lx_text = lx_text "(" (q == ">" ? "<>" : "\"\"")
                    lg_tok("punct", "(")
                    lg_tok("lit", "")
                    i += RLENGTH + m
                }
            }
            continue
        }
        if (c ~ /[0-9]/ || (c == "." && substr(s, i + 1, 1) ~ /[0-9]/)) {
            j = i + 1
            while (j <= n) {
                k = substr(s, j, 1)
                if (k ~ /[A-Za-z0-9_.]/) { j++; continue }
                if (k ~ /[+-]/ && substr(s, j - 1, 1) ~ /[eEpP]/) { j++; continue }
                if (k == SQ && substr(s, j + 1, 1) ~ /[A-Za-z0-9_]/) { j += 2; continue }
                if (k == SQ && substr(s, j + 1, 1) ~ /[$\\]|[^\t -~]/) { lx_untrusted = 1; j += 2; continue }
                if (k == "$" || k ~ /[^\t -~]/) { lx_untrusted = 1; j++; continue }
                if (k == "\\" && substr(s, j + 1, 1) ~ /[uU]/) { lx_untrusted = 1; j += 2; continue }
                break
            }
            lx_text = lx_text substr(s, i, j - i)
            lg_tok("num", "")
            i = j
            continue
        }
        if (c == "\"" || c == SQ) {
            i++
            while (i <= n) {
                k = substr(s, i, 1)
                if (k == "\\") { i += 2; continue }
                i++
                if (k == c) break
            }
            lx_text = lx_text c c
            lg_tok("lit", "")
            continue
        }
        lx_text = lx_text c
        lg_tok("punct", c)
        i++
    }
}
# post_line(body) — feed one line of the current side through the #if stack (only when
# it STARTS outside a comment / raw string — a `#if` there is text, not a directive) and
# then the lexer. Sets pl_raw (line starts inside a raw string literal); returns 1 for a
# conditional directive.
function post_line(body,   r, st, tl, splice, cont, isdir) {
    pl_raw = lx_raw
    # Shapes the per-line tracking cannot follow mark the file untrusted:
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
    lex_line(body, isdir || cont)
    return r
}
# Pass-1 trust check, at each hunk end: a side whose tracking leaves the #if stack open, closed an
# arm it never opened, or stops inside a comment / raw string cannot be trusted, nor can its file.
function side_bad() { return depth != 0 || uflow || lx_blk || lx_raw || lx_untrusted }
function end_hunk1() {
    if (hunk_open && (prodo1 || prodn1)) {
        side("pre")
        if (prodo1 && side_bad()) untrusted[fd] = 1
        side("post")
        if (prodn1 && side_bad()) untrusted[fd] = 1
    }
    hunk_open = 0
}
# Pass-1 helpers: close the current removed-header / added-cpp run.
function end_runs() { in_rrun = 0; in_arun = 0 }
# Pass 2: decide one changed line of side m (file f) and print it unless it is exempt here.
function changed(m, raw, f,   r, t, s2) {
    r = feed(m, raw)
    # A carriage return inside the line ends it for the compilers: whatever follows is code the
    # classifier would read as part of the line before (a comment, a directive), so it is never exempt.
    if (!trusted || pl_raw || raw ~ /\r[^\r]/) { print RAWLINE; return }
    t = trim(lx_text)
    if (r) { print t; return }
    if (FNR in reloc) return
    if (in_off_arm()) return
    if (!lx_code || !lx_nonlog) return
    if (m == "post" && relfile[f] && t ~ /^namespace([ \t]+[A-Za-z_][A-Za-z0-9_:]*)?[ \t]*\{$/) return
    if (m == "post" && is_hdr(f) && t ~ /;$/) {
        s2 = t
        sub(/[ \t]*;$/, "", s2)
        if (s2 in relsig) return
    }
    print t
}
BEGIN {
    SQ = sprintf("%c", 39); nr = 0; na = 0; fd = 0
    RAWLINE = "__coverage_gate_untrusted_line__;"
}
# Pass 1: per file diff (fd), whether its lexing can be trusted; the relocation candidates.
NR == FNR {
    if ($0 ~ /^diff --git /) {
        end_runs(); end_hunk1()
        fd++; in_hdr = 1; fo1 = ""; fn1 = ""; prodo1 = 0; prodn1 = 0
        next
    }
    if (in_hdr) {
        if ($0 ~ /^--- /) fo1 = path_of($0)
        else if ($0 ~ /^\+\+\+ /) fn1 = path_of($0)
        else if ($0 ~ /^@@/) {
            in_hdr = 0
            prodo1 = is_prod(fo1)
            prodn1 = is_prod(fn1)
            if ((prodo1 && !is_cxx(fo1)) || (prodn1 && !is_cxx(fn1))) untrusted[fd] = 1
            hunk_begin($0)
        }
        next
    }
    if ($0 ~ /^@@/) { end_runs(); end_hunk1(); hunk_begin($0); next }
    c1 = substr($0, 1, 1)
    if (prodo1 && (c1 == " " || c1 == "-")) feed("pre", $0)
    if (prodn1 && (c1 == " " || c1 == "+")) feed("post", $0)
    if (c1 == "-" && prodo1 && is_hdr(fo1)) {
        if (!in_rrun) { nr++; rn[nr] = 0; in_rrun = 1 }
        rn[nr]++
        rl[nr, rn[nr]] = trim(substr($0, 2))
        rk[nr, rn[nr]] = FNR
        in_arun = 0
        next
    }
    if (c1 == "+" && prodn1 && is_cpp(fn1)) {
        if (!in_arun) { na++; an[na] = 0; af[na] = fn1; in_arun = 1 }
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
    end_runs()
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
                    for (q = 0; q < k; q++) {
                        used[a, st + q] = 1
                        reloc[ak[a, st + q]] = 1
                        reloc[rk[r, i + q]] = 1
                    }
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
    fd = 0
    in_hdr = 0
    hunk_open = 0
}
# Pass 2: print every changed product line that is not exempt.
{
    if ($0 ~ /^diff --git /) { fd++; in_hdr = 1; fo2 = ""; fn2 = ""; prodo2 = 0; prodn2 = 0; next }
    if (in_hdr) {
        if ($0 ~ /^--- /) fo2 = path_of($0)
        else if ($0 ~ /^\+\+\+ /) fn2 = path_of($0)
        else if ($0 ~ /^@@/) {
            in_hdr = 0
            prodo2 = is_prod(fo2)
            prodn2 = is_prod(fn2)
            trusted = !(fd in untrusted)
            hunk_begin($0)
        }
        next
    }
    if ($0 ~ /^@@/) { hunk_begin($0); next }
    c1 = substr($0, 1, 1)
    if (c1 == " ") {
        if (prodo2) feed("pre", $0)
        if (prodn2) feed("post", $0)
    } else if (c1 == "-") {
        if (prodo2) changed("pre", $0, fo2)
    } else if (c1 == "+") {
        if (prodn2) changed("post", $0, fn2)
    }
}
AWK
)"

_prefilter_diff() {
    # LC_ALL=C: byte semantics in every awk (gawk would read UTF-8 characters), so the lexer's
    # non-ASCII checks and offsets mean the same thing on every runner.
    LC_ALL=C awk -v dataext="$_DATA_EXT_RE" -v cxxtmpl="$_CXX_TEMPLATE_RE" "$_PREFILTER_AWK" "$1" "$1"
}

# _binary_names_product <rest> — <rest> is a "Binary files X and Y differ" line without its
# "Binary files " and " differ": X is a/<path> or /dev/null, Y is b/<path> or /dev/null. A path can
# hold " and " itself, so every split that reads that way is tried: 0 when any of them names a
# product path, when none reads, or when git quoted a path (one these patterns cannot read).
_binary_names_product() {
    local rest="$1" off=0 head left right p readable=0
    [[ "$rest" == *\"* ]] && return 0
    while [[ "${rest:off}" == *" and "* ]]; do
        head="${rest:off}"
        head="${head%%" and "*}"
        left="${rest:0:off+${#head}}"
        right="${rest:off+${#head}+5}"
        off=$(( off + ${#head} + 5 ))
        [[ "$left" == a/* || "$left" == /dev/null ]] || continue
        [[ "$right" == b/* || "$right" == /dev/null ]] || continue
        readable=1
        for p in "$left" "$right"; do
            [ "$p" != /dev/null ] && _is_product_path "${p:2}" && return 0
        done
    done
    [ "$readable" -eq 1 ] || return 0
    return 1
}

# _classify_diff_file <diff-file> — prefilter a full-context diff, then classify the
# reduced diff. Both stages read/write temp FILES, never a pipe: _classify_diff
# returns early, and an early-closing pipe reader would SIGPIPE
# the producer (see the GIT_DIFF_TMPFILE note in the normal run below). Echoes
# EXEMPT / FALLTHROUGH; returns non-zero (echoing nothing) if the prefilter fails.
_classify_diff_file() {
    local reduced rc=0
    # git prints a file it takes for binary (one NUL byte, even inside a comment the compilers
    # ignore) as a single "Binary files ... differ" line: a C/C++ file in that form hides its whole
    # change from the classifier, so it is never exempt.
    local bin
    while IFS= read -r bin; do
        bin="${bin#Binary files }"
        if _binary_names_product "${bin% differ}"; then
            echo FALLTHROUGH
            return 0
        fi
    done < <(grep '^Binary files ' "$1" || true)
    # git quotes a path that holds a quote, a backslash or a control byte ("+++ \"b/..."): the
    # patterns here cannot read it, so it is never exempt.
    if grep -qE '^(\+\+\+|---) "' "$1"; then
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
@@ -10,1 +10,3 @@
 void Sync() {
+    LOG_ERROR("sync failed for ticket %s with status %d",
+              ticketKey.c_str(), httpStatus);
EOF

    # 3-line wrapped LOG_ERROR — opener + a middle arg line + the closing `);`. All three
    # accumulate into one exempt logging statement.
    _expect EXEMPT "3-line wrapped LOG_ERROR" <<'EOF'
diff --git a/Source/Core/src/Sync/Wrap3.cpp b/Source/Core/src/Sync/Wrap3.cpp
--- a/Source/Core/src/Sync/Wrap3.cpp
+++ b/Source/Core/src/Sync/Wrap3.cpp
@@ -20,1 +20,4 @@
 void Create() {
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
@@ -30,1 +30,4 @@
 void Retry() {
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
@@ -40,1 +40,2 @@
 void Report() {
+    LOG_ERROR("x (", y);
EOF

    # A LOG_ statement closing mid-line followed by REAL code on the same line must fall through:
    # the trailing statement is classified on its own merits, not blanket-skipped by the close.
    _expect FALLTHROUGH "LOG_ then trailing real code same line (trailing-code fix)" <<'EOF'
diff --git a/Source/Core/src/Sync/WrapTrail.cpp b/Source/Core/src/Sync/WrapTrail.cpp
--- a/Source/Core/src/Sync/WrapTrail.cpp
+++ b/Source/Core/src/Sync/WrapTrail.cpp
@@ -50,1 +50,2 @@
 void Retry() {
+    LOG_ERROR("partial: %s", detail.c_str()); retries = retries + 1;
EOF

    # The string-literal paren AND trailing-code fixes combined: a literal `)` inside the format
    # string must not be mistaken for the statement close, and the real trailing stmt must show.
    _expect FALLTHROUGH "LOG_ literal paren + trailing real code same line" <<'EOF'
diff --git a/Source/Core/src/Sync/WrapLitTrail.cpp b/Source/Core/src/Sync/WrapLitTrail.cpp
--- a/Source/Core/src/Sync/WrapLitTrail.cpp
+++ b/Source/Core/src/Sync/WrapLitTrail.cpp
@@ -60,1 +60,2 @@
 void Count() {
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
    # '1'_a is a char literal with a user-defined-literal suffix, then 'b' and "'/*": no comment opens.
    _expect FALLTHROUGH "a quote after a literal's suffix opens no phantom literal" <<'EOF'
diff --git a/Source/Core/src/Sync/Udl.cpp b/Source/Core/src/Sync/Udl.cpp
--- a/Source/Core/src/Sync/Udl.cpp
+++ b/Source/Core/src/Sync/Udl.cpp
@@ -1,2 +1,3 @@
 #define M '1'_a'b' "'/*"
+int launch = launchMissiles(5);
 /* real */
EOF
    # 1e+'5 is one pp-number (an exponent sign, then a digit separator).
    _expect FALLTHROUGH "a digit separator after an exponent sign stays in the pp-number" <<'EOF'
diff --git a/Source/Core/src/Sync/Exp.cpp b/Source/Core/src/Sync/Exp.cpp
--- a/Source/Core/src/Sync/Exp.cpp
+++ b/Source/Core/src/Sync/Exp.cpp
@@ -1,2 +1,3 @@
 #define M 1e+'5 "'/*"
+int launch = launchMissiles(5);
 /* real */
EOF
    # <none/*> after __has_include is a header-name, not a comment opener.
    _expect FALLTHROUGH "a header-name holds no comment opener" <<'EOF'
diff --git a/Source/Core/src/Sync/HasInc.cpp b/Source/Core/src/Sync/HasInc.cpp
--- a/Source/Core/src/Sync/HasInc.cpp
+++ b/Source/Core/src/Sync/HasInc.cpp
@@ -1,3 +1,4 @@
 #if !__has_include(<none/*>)
+int launch = launchMissiles(5);
 /* real */
 #endif
EOF
    # git ends the header with a TAB when the path holds a space.
    _expect FALLTHROUGH "a product path with a space is classified" < <(printf '%s\n' \
        'diff --git a/Source/Core/src/Sync/a b.cpp b/Source/Core/src/Sync/a b.cpp' \
        "--- a/Source/Core/src/Sync/a b.cpp$(printf '\t')" "+++ b/Source/Core/src/Sync/a b.cpp$(printf '\t')" \
        '@@ -1,1 +1,2 @@' ' // a' '+int launch = launchMissiles(5);')
    _expect FALLTHROUGH "a quoted path is never exempt" <<'EOF'
diff --git "a/Source/Core/src/Sync/De\177l.cpp" "b/Source/Core/src/Sync/De\177l.cpp"
--- "a/Source/Core/src/Sync/De\177l.cpp"
+++ "b/Source/Core/src/Sync/De\177l.cpp"
@@ -1,1 +1,2 @@
 // a
+// only a comment
EOF
    # Any file can be #included: an unknown extension under the product trees is product code.
    _expect FALLTHROUGH "real code in an included .tpp file is not exempt" <<'EOF'
diff --git a/Source/Core/src/Sync/FooImpl.tpp b/Source/Core/src/Sync/FooImpl.tpp
--- a/Source/Core/src/Sync/FooImpl.tpp
+++ b/Source/Core/src/Sync/FooImpl.tpp
@@ -1,1 +1,2 @@
 // implementation
+launchMissiles(x);
EOF
    _expect EXEMPT "a data file under the product trees is not product code" <<'EOF'
diff --git a/Source/Core/src/Sync/README.md b/Source/Core/src/Sync/README.md
--- a/Source/Core/src/Sync/README.md
+++ b/Source/Core/src/Sync/README.md
@@ -1,1 +1,2 @@
 # Sync
+launchMissiles(x);
EOF
    _expect FALLTHROUGH "a binary file with an unknown product extension is never exempt" <<'EOF'
diff --git a/Source/Core/src/Sync/Table.def b/Source/Core/src/Sync/Table.def
index 1111111..2222222 100644
Binary files a/Source/Core/src/Sync/Table.def and b/Source/Core/src/Sync/Table.def differ
EOF
    # A .c file follows C's lexical rules, not C++'s (no raw strings, no digit separators).
    _expect FALLTHROUGH "a .c file gets no off-target drop" <<'EOF'
diff --git a/Source/Core/src/Sync/Plain.c b/Source/Core/src/Sync/Plain.c
--- a/Source/Core/src/Sync/Plain.c
+++ b/Source/Core/src/Sync/Plain.c
@@ -1,3 +1,3 @@
 #ifdef __ANDROID__
-int a = 1;
+int a = launchMissiles(2);
 #endif
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

    # ---- Removed lines are changes too ----

    # Deleting a guard changes behaviour exactly as adding one does.
    _expect FALLTHROUGH "a removed guard statement" <<'EOF'
diff --git a/Source/Core/src/Sync/Guard.cpp b/Source/Core/src/Sync/Guard.cpp
--- a/Source/Core/src/Sync/Guard.cpp
+++ b/Source/Core/src/Sync/Guard.cpp
@@ -1,4 +1,3 @@
 void Commit(bool confirmed) {
-    if (!confirmed) return;
     Apply();
 }
EOF

    # Removing a comment and a logging statement is still exempt.
    _expect EXEMPT "a removed comment and logging statement" <<'EOF'
diff --git a/Source/Core/src/Sync/Quiet.cpp b/Source/Core/src/Sync/Quiet.cpp
--- a/Source/Core/src/Sync/Quiet.cpp
+++ b/Source/Core/src/Sync/Quiet.cpp
@@ -1,6 +1,3 @@
 void Commit() {
-    // stale note
-    LOG_INFO("committing %s",
-             key.c_str());
     Apply();
 }
EOF

    # An added line "++ counter;" prints as "+++ counter;": a body line, not a file header that would
    # hide every line after it.
    _expect FALLTHROUGH "an added line that reads like a +++ header" <<'EOF'
diff --git a/Source/Core/src/Sync/Count.cpp b/Source/Core/src/Sync/Count.cpp
--- a/Source/Core/src/Sync/Count.cpp
+++ b/Source/Core/src/Sync/Count.cpp
@@ -1,2 +1,4 @@
 void Bump() {
+++ counter;
+    DeleteAllTickets();
 }
EOF

    # ---- Logging statements are followed by the lexer ----

    # A char literal paren in the format arguments leaves no call open to swallow the next line.
    _expect FALLTHROUGH "a char literal paren in a LOG call, then real code" <<'EOF'
diff --git a/Source/Core/src/Sync/CharLit.cpp b/Source/Core/src/Sync/CharLit.cpp
--- a/Source/Core/src/Sync/CharLit.cpp
+++ b/Source/Core/src/Sync/CharLit.cpp
@@ -1,2 +1,5 @@
 void Query() {
+    LOG_INFO("expected %c in the query", '(');
+    LOG_INFO("expected %c in the query", '"');
+    DeleteAllTickets(); return Wipe();
 }
EOF

    _expect FALLTHROUGH "two LOG calls then real code on one line" <<'EOF'
diff --git a/Source/Core/src/Sync/TwoLogs.cpp b/Source/Core/src/Sync/TwoLogs.cpp
--- a/Source/Core/src/Sync/TwoLogs.cpp
+++ b/Source/Core/src/Sync/TwoLogs.cpp
@@ -1,2 +1,3 @@
 void Query() {
+    LOG_INFO("a"); LOG_INFO("b"); DeleteAllTickets();
 }
EOF

    # A LOG added as the body of an existing if takes the branch's statement away from it.
    _expect FALLTHROUGH "a LOG call that becomes the body of an if" <<'EOF'
diff --git a/Source/Core/src/Sync/IfBody.cpp b/Source/Core/src/Sync/IfBody.cpp
--- a/Source/Core/src/Sync/IfBody.cpp
+++ b/Source/Core/src/Sync/IfBody.cpp
@@ -1,4 +1,5 @@
 void Run(bool c) {
     if (c)
+        LOG_INFO("taking the branch");
         DoIt();
 }
EOF

    # A changed argument between string literals of an ordinary call is real code.
    _expect FALLTHROUGH "a call between string literals" <<'EOF'
diff --git a/Source/Core/src/Sync/Between.cpp b/Source/Core/src/Sync/Between.cpp
--- a/Source/Core/src/Sync/Between.cpp
+++ b/Source/Core/src/Sync/Between.cpp
@@ -1,4 +1,4 @@
 void Show() {
     Render("title",
-           "label", ComputeSafe(), "other",
+           "label", DeleteAllTickets(), "other",
            0);
EOF

    # A string argument of an ordinary call is runtime data, unlike a LOG message.
    _expect FALLTHROUGH "a string argument of an ordinary call" <<'EOF'
diff --git a/Source/Core/src/Sync/Cmd.cpp b/Source/Core/src/Sync/Cmd.cpp
--- a/Source/Core/src/Sync/Cmd.cpp
+++ b/Source/Core/src/Sync/Cmd.cpp
@@ -1,4 +1,4 @@
 void Clean() {
     RunCommand(
-        "ls /tmp");
+        "rm -rf /tmp/x");
 }
EOF

    _expect EXEMPT "a reworded line inside a wrapped LOG call" <<'EOF'
diff --git a/Source/Core/src/Sync/Reword.cpp b/Source/Core/src/Sync/Reword.cpp
--- a/Source/Core/src/Sync/Reword.cpp
+++ b/Source/Core/src/Sync/Reword.cpp
@@ -1,4 +1,4 @@
 void Report(int n) {
     LOG_INFO("first part "
-             "old tail %d", n);
+             "new tail %d", n);
 }
EOF

    # The call's value feeds an expression the context line finishes: not a logging statement.
    _expect FALLTHROUGH "a LOG call that continues into an expression" <<'EOF'
diff --git a/Source/Core/src/Sync/Comma.cpp b/Source/Core/src/Sync/Comma.cpp
--- a/Source/Core/src/Sync/Comma.cpp
+++ b/Source/Core/src/Sync/Comma.cpp
@@ -1,3 +1,4 @@
 void Run() {
+    LOG_INFO("x"),
         Compute();
 }
EOF

    _expect FALLTHROUGH "static_assert followed by a statement" <<'EOF'
diff --git a/Source/Core/src/Sync/Shapes.cpp b/Source/Core/src/Sync/Shapes.cpp
--- a/Source/Core/src/Sync/Shapes.cpp
+++ b/Source/Core/src/Sync/Shapes.cpp
@@ -1,2 +1,3 @@
 void Shapes() {
+    static_assert(sizeof(int) == 4, "//"); DeleteAllTickets();
 }
EOF

    _expect FALLTHROUGH "a using declaration followed by a statement" <<'EOF'
diff --git a/Source/Core/src/Sync/Using.cpp b/Source/Core/src/Sync/Using.cpp
--- a/Source/Core/src/Sync/Using.cpp
+++ b/Source/Core/src/Sync/Using.cpp
@@ -1,2 +1,3 @@
 void Shapes() {
+    using Id = int; Wipe();
 }
EOF

    # ---- Preprocessor guards ----

    _expect FALLTHROUGH "#if 0 turned into #if 1" <<'EOF'
diff --git a/Source/Core/src/Sync/Purge.cpp b/Source/Core/src/Sync/Purge.cpp
--- a/Source/Core/src/Sync/Purge.cpp
+++ b/Source/Core/src/Sync/Purge.cpp
@@ -1,5 +1,5 @@
 void Purge() {
-#if 0
+#if 1
     PurgeAllLocalTickets();
 #endif
 }
EOF

    _expect FALLTHROUGH "a guard with a constant || operand" <<'EOF'
diff --git a/Source/Core/src/Sync/Purge2.cpp b/Source/Core/src/Sync/Purge2.cpp
--- a/Source/Core/src/Sync/Purge2.cpp
+++ b/Source/Core/src/Sync/Purge2.cpp
@@ -1,3 +1,5 @@
 void Purge() {
+#if defined(SMATCHET_DEBUG) || 1
     PurgeAllLocalTickets();
+#endif
 }
EOF

    _expect EXEMPT "a version guard around existing code" <<'EOF'
diff --git a/Source/Core/src/Sync/Ver.cpp b/Source/Core/src/Sync/Ver.cpp
--- a/Source/Core/src/Sync/Ver.cpp
+++ b/Source/Core/src/Sync/Ver.cpp
@@ -1,3 +1,5 @@
 void Tune() {
+#if defined(_MSC_VER) && _MSC_VER >= 1920
     TuneForMsvc();
+#endif
 }
EOF

    # ---- Lexing the lexer cannot trust gives no exemption ----

    # A byte-order mark leads only the file's first line; one further down is a non-ASCII byte, and
    # "<BOM>#endif" is no directive (the lexer would otherwise end the #if 0 early and drop Hidden as
    # Android-only).
    _expect FALLTHROUGH "a byte-order mark below the first line" < <(printf '%s\n' \
        'diff --git a/Source/Core/src/Bom.cpp b/Source/Core/src/Bom.cpp' \
        '--- a/Source/Core/src/Bom.cpp' '+++ b/Source/Core/src/Bom.cpp' '@@ -1,8 +1,9 @@' \
        ' #if 0' $' \357\273\277#endif' $' \357\273\277#ifdef __ANDROID__' $' \357\273\277#ifdef __ANDROID__' \
        ' #endif' '+int Hidden() { return 9; }' ' #if 0' $' \357\273\277#endif' ' #endif')

    _expect EXEMPT "a byte-order mark on the first line stays trusted" < <(printf '%s\n' \
        'diff --git a/Source/Core/src/Bom1.cpp b/Source/Core/src/Bom1.cpp' \
        '--- a/Source/Core/src/Bom1.cpp' '+++ b/Source/Core/src/Bom1.cpp' '@@ -1,2 +1,2 @@' \
        $'-\357\273\277// old note' $'+\357\273\277// new note' ' int x;')

    _expect FALLTHROUGH "a comment before __has_include's header-name" <<'EOF'
diff --git a/Source/Core/src/HasInc.cpp b/Source/Core/src/HasInc.cpp
--- a/Source/Core/src/HasInc.cpp
+++ b/Source/Core/src/HasInc.cpp
@@ -1,3 +1,4 @@
 #if !__has_include( /**/ <none/*>)
+int Hidden() { return 42; }
 // */
 #endif
EOF

    _expect FALLTHROUGH "a comment between __has_include and its parenthesis" <<'EOF'
diff --git a/Source/Core/src/HasInc2.cpp b/Source/Core/src/HasInc2.cpp
--- a/Source/Core/src/HasInc2.cpp
+++ b/Source/Core/src/HasInc2.cpp
@@ -1,3 +1,4 @@
 #if !__has_include/**/(<none/*>)
+int Hidden() { return 42; }
 // */
 #endif
EOF

    _expect FALLTHROUGH "a comment before #include's header-name" <<'EOF'
diff --git a/Source/Core/src/Inc.cpp b/Source/Core/src/Inc.cpp
--- a/Source/Core/src/Inc.cpp
+++ b/Source/Core/src/Inc.cpp
@@ -1,3 +1,4 @@
 #include /**/ <none/*>
+int Hidden() { return 42; }
 // */
EOF

    # An untrusted file is never classified line by line: a splice inside the string carries the
    # comment opener, which a per-line reading would take for a comment.
    _expect FALLTHROUGH "an untrusted file gets no line-by-line exemption" <<'EOF'
diff --git a/Source/Core/src/Splice.cpp b/Source/Core/src/Splice.cpp
--- a/Source/Core/src/Splice.cpp
+++ b/Source/Core/src/Splice.cpp
@@ -1,2 +1,6 @@
 int x;
+static_assert(sizeof(int) == 4, "int is \
+/* 32-bit");
+int Hidden() { return 7; }
+// */
EOF

    # ---- Paths ----

    _expect FALLTHROUGH "a binary C++ file whose path holds ' and '" <<'EOF'
diff --git a/Source/Core/src/Load and Save.cpp b/Source/Core/src/Load and Save.cpp
index 1111111..2222222 100644
Binary files a/Source/Core/src/Load and Save.cpp and b/Source/Core/src/Load and Save.cpp differ
EOF

    _expect EXEMPT "a binary data file whose path holds ' and '" <<'EOF'
diff --git a/Source/Core/assets/a and b.png b/Source/Core/assets/a and b.png
index 1111111..2222222 100644
Binary files a/Source/Core/assets/a and b.png and b/Source/Core/assets/a and b.png differ
EOF

    _expect FALLTHROUGH "a configure_file template of a C++ file is product code" <<'EOF'
diff --git a/Source/Core/src/Version.cpp.in b/Source/Core/src/Version.cpp.in
--- a/Source/Core/src/Version.cpp.in
+++ b/Source/Core/src/Version.cpp.in
@@ -1,1 +1,2 @@
 const char* kVersion = "@SMATCHET_VERSION@";
+int Hidden() { return 1; }
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
if [ -n "$BASE_REF" ] && ! git rev-parse --verify --quiet "$BASE_REF^{commit}" >/dev/null 2>&1; then
    echo "[coverage-delta-gate] FAIL — SMATCHET_COVERAGE_GATE_BASE '$BASE_REF' names no commit" >&2
    exit 1
fi
if [ -z "$BASE_REF" ]; then
    for candidate in origin/develop develop HEAD~1; do
        if git rev-parse --verify --quiet "$candidate^{commit}" >/dev/null 2>&1; then
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

# Every diff below names its prefixes and turns rename detection on, and runs no external diff or
# textconv driver, whatever the local git configuration: the parsing here depends on all of it.
_gate_diff() {
    git -c core.quotePath=false diff --no-ext-diff --no-textconv --src-prefix=a/ --dst-prefix=b/ -M "$@"
}

# Compute the diff once. --name-only --diff-filter=ACMRT keeps adds, copies, modifies, renames
# and type changes (a file replaced by a symlink: the cases that actually change content). Deletes
# intentionally excluded — removing a production file shouldn't require a new test.
# -z: every path arrives verbatim, whatever bytes it holds (git quotes some even with
# core.quotePath=false). A failing git command fails the gate: an empty list would pass it.
_GATE_LIST_TMP="$(mktemp)"
trap 'rm -f "$_GATE_LIST_TMP"' EXIT
if ! _gate_diff -z --name-only --diff-filter=ACMRT "$MERGE_BASE"...HEAD >"$_GATE_LIST_TMP" 2>/dev/null; then
    echo "[coverage-delta-gate] FAIL — git diff failed (bad MERGE_BASE '$MERGE_BASE' or git error)" >&2
    exit 1
fi
mapfile -d '' -t CHANGED <"$_GATE_LIST_TMP"

# A file renamed between product trees, or into or out of them (notes.txt -> Foo.cpp,
# tests/support/Fake.cpp -> Source/Core/src/Fake.cpp), shows no new lines, yet the compilers
# newly build (or stop building) every line it carries: never exempt. A move inside one tree is.
RENAMED_ACROSS=""
if ! _gate_diff -z --name-status --diff-filter=R "$MERGE_BASE"...HEAD >"$_GATE_LIST_TMP" 2>/dev/null; then
    echo "[coverage-delta-gate] FAIL — git diff failed (bad MERGE_BASE '$MERGE_BASE' or git error)" >&2
    exit 1
fi
mapfile -d '' -t _RENAME_FIELDS <"$_GATE_LIST_TMP"
for ((_ri = 0; _ri + 2 < ${#_RENAME_FIELDS[@]}; _ri += 3)); do
    if [ "$(_rename_class "${_RENAME_FIELDS[_ri + 1]}")" != "$(_rename_class "${_RENAME_FIELDS[_ri + 2]}")" ]; then
        RENAMED_ACROSS="${_RENAME_FIELDS[_ri + 1]} -> ${_RENAME_FIELDS[_ri + 2]}"
        break
    fi
done

if [ "${#CHANGED[@]}" -eq 0 ]; then
    echo "[coverage-delta-gate] no changed files vs $BASE_REF; gate passes"
    exit 0
fi

PROD_CHANGES=()
TEST_CHANGES=()

for f in "${CHANGED[@]}"; do
    case "$f" in
        # Production surface that the gate cares about: every product file under
        # Source/Core/src/ but a header (a .cpp, and the .inl / .tpp / unknown-extension
        # implementation files a .cpp includes). Headers are treated as docs-or-API-shape
        # (different review surface), so they don't require a paired test delta on their own.
        Source/Core/src/*.h|Source/Core/src/*.hpp|Source/Core/src/*.hxx|Source/Core/src/*.hh) ;;
        Source/Core/src/*)
            if _is_product_path "$f"; then
                PROD_CHANGES+=("$f")
            fi ;;
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
    echo "[coverage-delta-gate] PASS — no production Source/Core/src/ changes"
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
trap 'rm -f "$GIT_DIFF_TMPFILE" "$_GATE_LIST_TMP"' EXIT
if ! _gate_diff --unified=100000 --diff-filter=ACMRT "$MERGE_BASE"...HEAD -- \
        Source/Core Source/Plugins Source/Standalone tests >"$GIT_DIFF_TMPFILE" 2>/dev/null; then
    echo "[coverage-delta-gate] FAIL — git diff failed (bad MERGE_BASE '$MERGE_BASE' or git error)" >&2
    exit 1
fi
if ! EXEMPTION="$(_classify_diff_file "$GIT_DIFF_TMPFILE")"; then
    echo "[coverage-delta-gate] FAIL — diff prefilter (awk) failed" >&2
    exit 1
fi
if [ "$EXEMPTION" = "EXEMPT" ] && [ -n "$RENAMED_ACROSS" ]; then
    echo "[coverage-delta-gate] no test-light exemption: $RENAMED_ACROSS moves a file between what the build compiles"
    EXEMPTION=FALLTHROUGH
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

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
# Test-light exemption (no override, no postmortem) — auto-PASS a diff that leaves
# the code the tested builds compile unchanged. Each changed first-party product file
# (every file under Source/Core, Source/Plugins, Source/Standalone, tests/ but known
# data: docs, assets, fixtures, scripts, build files — _DATA_EXT_RE) is lexed before
# and after the change into a stream of C++ tokens, and the two streams must be
# identical once these are left out of both:
#   * comments and whitespace;
#   * a LOG_{DEBUG,INFO,WARN,ERROR,TRACE}( … ); statement that starts a statement
#     (after ; { or }), its arguments included (they are not inspected);
#   * a static_assert( … ); statement (compile-time; the build is the test);
#   * an empty by-reference catch clause right before an empty `catch (…)` clause (the
#     swallow→log pattern of #906, once its LOG statements are left out: both swallow; a
#     by-value one copies the exception, which may do anything);
#   * the code a tested build never compiles: an #if/#elif/#else arm that is false on
#     the desktop builds and either false everywhere (`#if 0`) or Android-only
#     (__ANDROID__, validated by the Android NDK/APK cross-compile jobs, #1021). An arm
#     whose condition the gate cannot decide (any other macro, a comparison, a C++23
#     #elifdef / #elifndef) stays in the stream, its directives with it, so moving code
#     across one changes the stream. A group whose every arm is decided and whose kept
#     arms are the ones the desktop builds compile leaves no directive in the stream
#     (`#ifndef SMATCHET_EMBEDDED_IN_UNREAL` around existing code, #1082). __APPLE__ /
#     TARGET_OS_* arms are NOT left out — no CI job builds macOS/iOS.
# So a new or removed statement, a changed condition or literal, code commented out or
# back in, a moved #else/#endif, a removed catch clause or brace — NOT exempt. Nor is
# anything that can change which declaration a name means or what a macro expands to:
# an #include, #pragma once, a using-declaration or forward declaration, a #define
# (each stays in the stream, so adding, removing or moving one is a change), or code
# moved between a header and a .cpp. Whitespace counts only where the compilers read
# it: between a #define'd name and its '(' (function-like or object-like), across a
# backslash splice included. A file renamed between product trees, into or out of
# them, to another extension (the CMake globs pick up *.cpp) or between a header and a
# translation unit, is never exempt. A file whose lexing the gate cannot trust is
# never exempt (see _prefilter_diff).
# Accepted approximations: a LOG call's arguments are not inspected; whitespace inside a
# stringified macro argument is not compared.
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

# _rename_class <path> — how the build treats a path: "none" outside the product trees (or data
# inside them), else its tree's first directory and its extension (everything after the name's first
# dot: "Source/Core/src:cpp", "Source/Core/include:h", "tests/Core:test.cpp"). A rename that changes it
# changes what the compilers build: the CMake globs pick up translation units by directory and by
# extension (*.cpp: a Reg.cpp renamed Reg.cc leaves the build), and a header moved to a .cpp is newly
# compiled on its own.
_rename_class() {
    local root rest base
    if ! _is_product_path "$1"; then
        echo none
        return
    fi
    root="$(_product_root "$1")"
    rest="${1#"$root"/}"
    base="${1##*/}"
    if [[ "$rest" == */* ]]; then
        echo "$root/${rest%%/*}:${base#*.}"
    else
        echo "$root:${base#*.}"
    fi
}

# ---------------------------------------------------------------------------
# Full-context prefilter: does each changed product file still compile to the same tokens?
# ---------------------------------------------------------------------------
# _prefilter_diff <diff-file> — read a FULL-CONTEXT unified diff (git diff
# --unified=<huge>, so each file's hunk carries the whole file) and print one line,
# "<path>: <reason>", for every product file whose change is not provably exempt.
# Nothing printed: every change is exempt.
#
# Both sides of each hunk are lexed — the pre-image (context and '-' lines) and the
# post-image (context and '+' lines) — each with its own state, token by token as the
# compilers do: identifiers, pp-numbers, punctuators (maximal munch, digraphs read as
# the tokens they spell), header-names after #include / __has_include, and string /
# char / raw string literals, kept verbatim (a raw string may span lines). Each side's
# tokens pass through the same normalisation (the exemptions listed at the top of this
# file) into a queue, and the queues are compared as they fill: the first difference
# fails the file, naming its post-image line.
#
# Preprocessor conditionals are evaluated for three kinds of build — the tested desktop
# builds, Android, and the Unreal plugin — with three-valued logic: __ANDROID__ and
# SMATCHET_EMBEDDED_IN_UNREAL have known values in each, integer literals, true and
# false are constants, `defined`, `!`, `&&`, `||` and parentheses combine them, and
# anything else (another macro, a comparison, a function-like macro, __has_include) is
# unknown. An arm false on the desktop and Unreal builds is left out (its tokens never
# enter the stream, nested groups included). Pass 1 decides, per group, whether every
# arm is decided and every kept arm is one the desktop builds compile; such a group
# leaves no directive in the stream, any other group leaves each of its directives as
# one token. Every other directive is one token too. A logging or static_assert statement
# interrupted by a directive token is not left out.
#
# A file whose lexing cannot be trusted is never exempt: C, an unknown extension, a '$'
# / non-ASCII / \u in an identifier, a hunk that ends with the #if stack open, closes
# an arm it never opened, ends inside a comment/raw string, a directive that ends
# inside a block comment, a backslash splice outside a directive's own continuation, a
# directive splice where the join could change what the lexer sees (a split directive
# name, or a quote, comment token, '/' or '*' at the splice on a spliced directive
# line), a comment between '#' and the directive name, between #include /
# __has_include and the header-name, or a directive after a closing */, a %: digraph
# directive, or a control byte the compilers read differently (a carriage return inside
# a line, a form feed, a vertical tab, a backslash followed by whitespace). A product
# file git prints as binary ("Binary files ... differ", e.g. one NUL byte) or under a
# quoted path is never exempt either (_classify_diff_file).
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
# The path of a "--- a/<path>" / "+++ b/<path>" header; git ends one with a TAB when the path holds a
# space. A deleted or added file's other side is /dev/null, which is no product path.
function path_of(raw,   p) { p = substr(raw, 5); sub(/\t$/, "", p); sub(/^[ab]\//, "", p); return p }

# ---- Three-valued preprocessor conditions: 0 false, 1 true, 2 unknown. Builds: D (the tested
# desktop builds), A (Android), U (the Unreal plugin). ----
function t_not(a) { return a == 2 ? 2 : 1 - a }
function t_and(a, b) { return (a == 0 || b == 0) ? 0 : ((a == 2 || b == 2) ? 2 : 1) }
function t_or(a, b) { return (a == 1 || b == 1) ? 1 : ((a == 2 || b == 2) ? 2 : 0) }
function mdef(name, p) {
    if (name == "__cplusplus") return 1
    if (name == "__ANDROID__") return p == "A" ? 1 : 0
    if (name == "SMATCHET_EMBEDDED_IN_UNREAL") return p == "U" ? 1 : 0
    return 2
}
function mval(name, p) {
    if (name == "__ANDROID__") return p == "A" ? 1 : 0
    if (name == "SMATCHET_EMBEDDED_IN_UNREAL") return p == "U" ? 2 : 0
    return 2
}
function num_truth(t) {
    sub(/[uUlL]+$/, "", t)
    gsub(SQ, "", t)
    if (t ~ /^0[xX][0-9a-fA-F]+$/) return substr(t, 3) ~ /[1-9a-fA-F]/
    if (t ~ /^0[bB][01]+$/) return substr(t, 3) ~ /1/
    if (t ~ /^[0-9]+$/) return t ~ /[1-9]/
    return 2
}
# Recursive descent over DT[ES, EP..EN] for build EPL.
function e_peek() { return EP <= EN ? DT[ES, EP] : "" }
function e_skip_balanced(   d) {
    d = 0
    while (EP <= EN) {
        if (DT[ES, EP] == "(") d++
        else if (DT[ES, EP] == ")") d--
        EP++
        if (d <= 0) return
    }
}
function e_or(   v) { v = e_and(); while (e_peek() == "||") { EP++; v = t_or(v, e_and()) } return v }
function e_and(   v) { v = e_unary(); while (e_peek() == "&&") { EP++; v = t_and(v, e_unary()) } return v }
function e_unary(   v) {
    if (e_peek() == "!") { EP++; return t_not(e_unary()) }
    v = e_primary()
    # Any other operator (a comparison, arithmetic): the operand's value is unknown.
    while (EP <= EN && e_peek() != "||" && e_peek() != "&&" && e_peek() != ")") {
        if (e_peek() == "(") e_skip_balanced()
        else EP++
        v = 2
    }
    return v
}
function e_primary(   t, v, name) {
    t = e_peek()
    if (t == "") return 2
    EP++
    if (t == "(") {
        v = e_or()
        if (e_peek() != ")") return 2
        EP++
        return v
    }
    if (t == "defined") {
        if (e_peek() == "(") {
            EP++
            name = e_peek()
            EP++
            if (e_peek() != ")") return 2
            EP++
        } else {
            name = e_peek()
            EP++
        }
        return mdef(name, EPL)
    }
    if (t ~ /^[0-9]/) return num_truth(t)
    if (t == "true") return 1
    if (t == "false") return 0
    if (t ~ /^[A-Za-z_]/) {
        if (e_peek() == "(") { e_skip_balanced(); return 2 }
        return mval(t, EPL)
    }
    return 2
}
# The value, in build p, of the condition of directive kw (tokens DT[s, 1..n]: '#', kw, ...).
function eval_cond(s, kw, n, p,   i, v) {
    if (kw == "else") return 1
    # #elifdef / #elifndef are C++23: a C++14 compiler may not honour them, so they stay undecided.
    if (kw == "elifdef" || kw == "elifndef") return 2
    if (kw == "ifdef") return n >= 3 ? mdef(DT[s, 3], p) : 2
    if (kw == "ifndef") return n >= 3 ? t_not(mdef(DT[s, 3], p)) : 2
    for (i = 3; i <= n; i++) if (DT[s, i] == "?") return 2
    ES = s; EP = 3; EN = n; EPL = p
    v = e_or()
    return EP <= EN ? 2 : v
}

# ---- Lexical state of one side (swapped in by side()): lx_blk (inside a /* */ comment), lx_raw
# (inside a raw string literal closed by ")" lx_rdel "\"", whose text so far is lx_rtext), lx_macro
# (the next line continues a directive), lx_sgap (whitespace before that splice's backslash),
# lx_untrusted, lx_bom (the next line opens the file). ----
function lex_reset() {
    lx_blk = 0; lx_raw = 0; lx_rdel = ""; lx_rtext = ""; lx_untrusted = 0; lx_macro = 0; lx_sgap = 0; lx_bom = 0
}
function side_save(m) {
    SS[m, "blk"] = lx_blk; SS[m, "raw"] = lx_raw; SS[m, "rdel"] = lx_rdel; SS[m, "rtext"] = lx_rtext
    SS[m, "unt"] = lx_untrusted; SS[m, "mac"] = lx_macro; SS[m, "sgap"] = lx_sgap; SS[m, "bom"] = lx_bom
}
function side_load(m) {
    lx_blk = SS[m, "blk"]; lx_raw = SS[m, "raw"]; lx_rdel = SS[m, "rdel"]; lx_rtext = SS[m, "rtext"]
    lx_untrusted = SS[m, "unt"]; lx_macro = SS[m, "mac"]; lx_sgap = SS[m, "sgap"]; lx_bom = SS[m, "bom"]
}
function side(m) { if (cur_side != m) { side_save(cur_side); side_load(m); cur_side = m } }
# Every other per-side state is indexed by side: DEP (#if depth), UF (an arm closed it never opened),
# NDROP (enclosing left-out arms), per depth d: GT (group leaves no directive), AD (arm left out),
# PV[s, d, build] (an earlier arm's condition held), GO (opening line), GT1 (pass 1: transparent so far);
# the statement normaliser: SP (last token), BD (brace depth), PD[s, BD] (paren depth), SBN / SBS /
# SBD / SB (buffered statement), CL (a case label is open); the catch stage:
# CCS / CCN / CCD / CC (clause), CPN / CP (pending empty clauses); the queue: QH / QT / Q / QL; LN (line).
function side_init(s, start) {
    side(s)
    lex_reset()
    lx_bom = start <= 1
    DEP[s] = 0; UF[s] = 0; NDROP[s] = 0; DTN[s] = 0
    SP[s] = start <= 1 ? "" : "?"; BD[s] = 0; PD[s, 0] = 0; SBN[s] = 0; UNSURE[s] = 0; CL[s] = 0
    CCS[s] = 0; CCN[s] = 0; CPN[s] = 0
    QH[s] = 0; QT[s] = 0; LN[s] = start > 0 ? start - 1 : 0
}
function hunk_begin(h,   t) {
    t = h; sub(/^@@ -/, "", t); pre_start = t + 0
    t = h; sub(/^@@ -[0-9]+(,[0-9]+)? \+/, "", t); post_start = t + 0
    side_init("pre", pre_start)
    side_init("post", post_start)
    hunk_open = 1
}
# feed(m, raw) — run one diff line's text through side m.
function feed(m, raw,   b) {
    side(m)
    LN[m]++
    b = substr(raw, 2)
    if (lx_bom) { b = strip_bom(b); lx_bom = 0 }
    post_line(b)
}

# ---- Tokens. A directive's tokens collect in DT[side, 1..DTN]; a code token goes through the
# statement normaliser and the catch stage into the side's queue (pass 2, outside left-out arms). ----
function tok(kind, t,   s) {
    s = cur_side
    if (lx_dir) { DT[s, ++DTN[s]] = t; DS[s, DTN[s]] = lx_gap; lx_gap = 0; return }
    if (pass == 1 || NDROP[s] > 0) return
    norm_tok(kind, t)
}
function boundary(t) { return t == "" || t == ";" || t == "{" || t == "}" }
function stmt_start(s) { return !UNSURE[s] && boundary(SP[s]) && PD[s, BD[s]] == 0 }
# A logging or static_assert call that starts a statement is buffered until it completes (left out) or
# turns out to be something else (flushed as ordinary tokens).
function norm_tok(kind, t,   s) {
    s = cur_side
    if (SBN[s] > 0) { sb_add(t); return }
    if (kind == "id" && stmt_start(s) && (t ~ /^LOG_(DEBUG|INFO|WARN|ERROR|TRACE)$/ || t == "static_assert")) {
        SBN[s] = 1; SB[s, 1] = t; SBD[s] = 0; SBS[s] = "P0"
        return
    }
    out_tok(t)
}
function sb_add(t,   s, st) {
    s = cur_side
    SB[s, ++SBN[s]] = t
    st = SBS[s]
    if (st == "P0") { if (t == "(") { SBD[s] = 1; SBS[s] = "P1" } else sb_flush(); return }
    if (st == "P1") { if (t == "(") SBD[s]++; else if (t == ")" && --SBD[s] == 0) SBS[s] = "P2"; return }
    if (st == "P2") { if (t == ";") sb_done(); else sb_flush(); return }
    sb_flush()
}
function sb_done(   s) { s = cur_side; SBN[s] = 0; SP[s] = ";" }
function sb_flush(   s, i, n) {
    s = cur_side
    n = SBN[s]
    SBN[s] = 0
    for (i = 1; i <= n; i++) out_tok(SB[s, i])
}
function out_tok(t,   s, label) {
    s = cur_side
    label = 0
    if (t == "{") { BD[s]++; PD[s, BD[s]] = 0 }
    else if (t == "}") { if (BD[s] > 0) BD[s]-- }
    else if (t == "(") PD[s, BD[s]]++
    else if (t == ")") { if (PD[s, BD[s]] > 0) PD[s, BD[s]]-- }
    else if ((t == "case" || t == "default") && stmt_start(s)) CL[s] = 1
    else if (t == ":" && CL[s] && PD[s, BD[s]] == 0) { CL[s] = 0; label = 1 }
    if (t == ";" || t == "{" || t == "}") CL[s] = 0
    # The statement after a case label starts like one after a semicolon.
    SP[s] = label ? ";" : t
    cc_tok(t)
}
# The catch stage: an empty catch clause right before an empty catch (...) clause is left out.
function cc_tok(t,   s, i) {
    s = cur_side
    if (CCS[s] == 0) {
        if (t == "catch") { CCS[s] = 1; CCN[s] = 1; CC[s, 1] = t; return }
        cc_drain()
        q_push(t)
        return
    }
    CC[s, ++CCN[s]] = t
    if (CCS[s] == 1) { if (t == "(") { CCS[s] = 2; CCD[s] = 1 } else cc_flush(); return }
    if (CCS[s] == 2) { if (t == "(") CCD[s]++; else if (t == ")" && --CCD[s] == 0) CCS[s] = 3; return }
    if (CCS[s] == 3) { if (t == "{") CCS[s] = 4; else cc_flush(); return }
    if (t != "}") { cc_flush(); return }
    if (CCN[s] == 6 && CC[s, 3] == "...") {
        CPN[s] = 0
        cc_flush()
        return
    }
    # A handler that catches by value copies the exception (a copy constructor may do anything): only a
    # by-reference one can go.
    for (i = 3; i < CCN[s] - 2 && CC[s, i] != "&" && CC[s, i] != "&&"; i++) ;
    if (i >= CCN[s] - 2) { cc_flush(); return }
    for (i = 1; i <= CCN[s]; i++) CP[s, ++CPN[s]] = CC[s, i]
    CCN[s] = 0
    CCS[s] = 0
}
function cc_drain(   s, i, n) {
    s = cur_side
    n = CPN[s]
    CPN[s] = 0
    for (i = 1; i <= n; i++) q_push(CP[s, i])
}
function cc_flush(   s, i, n) {
    s = cur_side
    cc_drain()
    n = CCN[s]
    CCN[s] = 0
    CCS[s] = 0
    for (i = 1; i <= n; i++) q_push(CC[s, i])
}
function flush_side(m) { side(m); sb_flush(); cc_flush() }
function q_push(t,   s) { s = cur_side; Q[s, ++QT[s]] = t; QL[s, QT[s]] = LN[s] }

# ---- Directives ----
# A directive's last line has been lexed: track the #if stack and, in pass 2, leave a directive token.
function dir_end(   s, n, kw) {
    s = cur_side
    n = DTN[s]
    DTN[s] = 0
    kw = n >= 2 ? DT[s, 2] : ""
    if (kw ~ /^(if|ifdef|ifndef|elif|elifdef|elifndef|else|endif)$/) { cond_dir(s, kw, n); return }
    if (pass == 1 || NDROP[s] > 0 || n < 2) return
    dir_token(s, n)
}
# A directive as one token, its tokens joined with one space: the whitespace between them is no part of
# the code, but for the gap between a #define'd name and a '(' (`#define F(x)` is a function-like macro,
# `#define F (x)` an object-like one).
function dir_token(s, n,   i, t) {
    flush_side(s)
    t = DT[s, 1]
    for (i = 2; i <= n; i++) t = t ((i == 4 && DT[s, 2] == "define" && DT[s, 4] == "(" && !DS[s, 4]) ? "" : " ") DT[s, i]
    q_push(t)
}
# Statement starts around a group that leaves its directives in the stream. Each arm starts after the
# token before the group (GSP); after the group, a statement starts only if one would whichever arm (or,
# with no #else, none) was compiled. Arms that leave the brace or paren depth different from the
# group's start make the depth unknowable: no statement start is recognised for the rest of the hunk.
function grp_open(s, d) {
    GSP[s, d] = SP[s]; GBD[s, d] = BD[s]; GPD[s, d] = PD[s, BD[s]]; GOK[s, d] = 1; GEL[s, d] = 0; GAE[s, d] = 0
}
# The end of an arm a tested build compiles: arms must all end at one brace / paren depth.
function grp_arm_end(s, d, dropped) {
    if (dropped) return
    if (!boundary(SP[s])) GOK[s, d] = 0
    if (!GAE[s, d]) {
        GAE[s, d] = 1
        GEB[s, d] = BD[s]
        GEP[s, d] = PD[s, BD[s]]
    } else if (BD[s] != GEB[s, d] || PD[s, BD[s]] != GEP[s, d]) UNSURE[s] = 1
}
# The next arm starts where the group did.
function grp_next_arm(s, d) { BD[s] = GBD[s, d]; PD[s, BD[s]] = GPD[s, d]; SP[s] = GSP[s, d] }
function grp_close(s, d) {
    if (!GEL[s, d]) {
        if (!boundary(GSP[s, d])) GOK[s, d] = 0
        if (GAE[s, d] && (GEB[s, d] != GBD[s, d] || GEP[s, d] != GPD[s, d])) UNSURE[s] = 1
    }
    if (GAE[s, d]) { BD[s] = GEB[s, d]; PD[s, BD[s]] = GEP[s, d] } else grp_next_arm(s, d)
    SP[s] = GOK[s, d] ? ";" : "#"
}
# An arm is left out when it is false on the desktop and Unreal builds; it keeps its place (no
# directive token needed) when it is true on the desktop builds.
function arm_dropped(s, d) { return VD[s, d] == 0 && VU[s, d] == 0 }
function arm_settled(s, d) { return VD[s, d] == 1 || (VD[s, d] == 0 && VU[s, d] == 0) }
function set_arm(s, d, kw, n,   cD, cA, cU) {
    cD = eval_cond(s, kw, n, "D"); cA = eval_cond(s, kw, n, "A"); cU = eval_cond(s, kw, n, "U")
    VD[s, d] = t_and(t_not(PV[s, d, "D"]), cD)
    VU[s, d] = t_and(t_not(PV[s, d, "U"]), cU)
    PV[s, d, "D"] = t_or(PV[s, d, "D"], cD)
    PV[s, d, "A"] = t_or(PV[s, d, "A"], cA)
    PV[s, d, "U"] = t_or(PV[s, d, "U"], cU)
    if (!arm_settled(s, d)) GT1[s, d] = 0
}
function cond_dir(s, kw, n,   d, live, wasdropped) {
    if (kw == "if" || kw == "ifdef" || kw == "ifndef") {
        d = ++DEP[s]
        GO[s, d] = DFNR[s]
        GT1[s, d] = 1
        PV[s, d, "D"] = 0; PV[s, d, "A"] = 0; PV[s, d, "U"] = 0
        GT[s, d] = pass == 2 && ((s, DFNR[s]) in gtrans) ? gtrans[s, DFNR[s]] : 0
        set_arm(s, d, kw, n)
        if (pass == 2 && !GT[s, d] && NDROP[s] == 0) {
            dir_token(s, n)
            grp_open(s, d)
        }
    } else {
        d = DEP[s]
        if (d == 0) { UF[s] = 1; return }
        wasdropped = AD[s, d]
        if (AD[s, d]) NDROP[s]--
        AD[s, d] = 0
        live = pass == 2 && !GT[s, d] && NDROP[s] == 0
        if (live) grp_arm_end(s, d, wasdropped)
        if (kw == "endif") {
            if (pass == 1) gtrans[s, GO[s, d]] = GT1[s, d]
            DEP[s]--
            if (live) {
                dir_token(s, n)
                grp_close(s, d)
            }
            return
        }
        set_arm(s, d, kw, n)
        if (live) {
            dir_token(s, n)
            grp_next_arm(s, d)
            if (kw == "else") GEL[s, d] = 1
        }
    }
    AD[s, d] = arm_dropped(s, d)
    if (AD[s, d]) NDROP[s]++
}

# lex_line(s, dir) — advance the lexical state across one line, handing every token to tok() (dir: the
# line is a directive or continues one).
# It scans token by token, as the compilers do, so a quote opens a literal only where
# a token starts: identifiers (a raw-string prefix is a whole identifier R, u8R, uR, UR
# or LR), pp-numbers (1'000, 1.R, 1e+'5 and 0x1e+5 are each one token), header-names
# after #include / __has_include (one token), punctuators by maximal munch, and ordinary
# string / char literals, which cannot span lines. A '$', non-ASCII byte or \u / \U in
# an identifier or pp-number is accepted differently by different compilers: the file is
# not trusted, as it is for a comment before a header-name (the per-line lexer would read
# the header-name as tokens there).
function lex_line(s, dir, cont,   i, n, c, c2, k, m, d, j, id, rest, q, p) {
    lx_dir = dir
    # The compiler deletes a splice's backslash-newline: the line it joins starts right after the text
    # before the backslash.
    lx_gap = cont ? (lx_sgap || s ~ /^[ \t]/) : 1
    n = length(s)
    i = 1
    if (!lx_blk && !lx_raw && match(s, /^[ \t]*(#|%:)[ \t]*(include_next|include|import)/) &&
        substr(s, RLENGTH + 1, 1) !~ /[A-Za-z0-9_]/) {
        m = RLENGTH
        id = substr(s, 1, m)
        sub(/^[ \t]*(#|%:)[ \t]*/, "", id)
        tok("punct", "#")
        tok("id", id)
        rest = substr(s, m + 1)
        match(rest, /^[ \t]*/)
        k = RLENGTH
        rest = substr(rest, k + 1)
        if (rest ~ /^\/[*\/]/) lx_untrusted = 1
        if (rest ~ /^[<"]/) {
            q = substr(rest, 1, 1) == "<" ? ">" : "\""
            d = index(substr(rest, 2), q)
            if (d == 0) { lx_untrusted = 1; return }
            lx_gap = k > 0
            tok("hdr", substr(rest, 1, d + 1))
            i = m + k + d + 2
        } else i = m + 1
    }
    while (i <= n) {
        if (lx_blk) {
            k = index(substr(s, i), "*/")
            if (k == 0) return
            i += k + 1
            lx_blk = 0
            lx_gap = 1
            continue
        }
        if (lx_raw) {
            k = index(substr(s, i), ")" lx_rdel "\"")
            if (k == 0) { lx_rtext = lx_rtext substr(s, i) "\n"; return }
            lx_rtext = lx_rtext substr(s, i, k + length(lx_rdel) + 1)
            i += k + length(lx_rdel) + 1
            lx_raw = 0
            tok("lit", lx_rtext)
            lx_rtext = ""
            continue
        }
        c = substr(s, i, 1)
        if (c ~ /[ \t\r\f\v]/) { lx_gap = 1; i++; continue }
        c2 = substr(s, i, 2)
        if (c2 == "//") {
            # A trailing backslash splices the next line into this comment, which the
            # per-line lexer cannot follow: the file's tracking is not trusted.
            if (s ~ /\\[ \t\r]*$/) lx_untrusted = 1
            return
        }
        if (c2 == "/*") { lx_blk = 1; lx_gap = 1; i += 2; continue }
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
                    lx_rtext = id "\"" lx_rdel "("
                    i += d + 1
                    continue
                }
                lx_untrusted = 1 # an R prefix with no delimiter the compilers accept
            }
            tok("id", id)
            if (id == "__has_include" || id == "__has_include_next") {
                rest = substr(s, i)
                if (rest ~ /^[ \t]*(\/[*\/]|\([ \t]*\/[*\/])/) lx_untrusted = 1
                if (match(rest, /^[ \t]*\([ \t]*[<"]/)) {
                    q = substr(rest, RLENGTH, 1) == "<" ? ">" : "\""
                    m = index(substr(rest, RLENGTH + 1), q)
                    if (m == 0) { lx_untrusted = 1; return }
                    tok("punct", "(")
                    tok("hdr", substr(rest, RLENGTH, m + 1))
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
            tok("num", substr(s, i, j - i))
            i = j
            continue
        }
        if (c == "\"" || c == SQ) {
            j = i + 1
            while (j <= n) {
                k = substr(s, j, 1)
                if (k == "\\") { j += 2; continue }
                j++
                if (k == c) break
            }
            tok("lit", substr(s, i, j - i))
            i = j
            continue
        }
        # A backslash ending a directive line is its splice, not a token.
        if (c == "\\" && substr(s, i + 1) ~ /^[ \t\r]*$/) return
        p = punct_at(s, i)
        i += length(p)
        tok("punct", (p in DIGRAPH) ? DIGRAPH[p] : p)
    }
}
# The punctuator at s[i], by maximal munch.
function punct_at(s, i,   p) {
    if (substr(s, i, 4) == "%:%:") return "%:%:"
    p = substr(s, i, 3)
    if (p == "<::" && substr(s, i + 3, 1) !~ /[:>]/) return "<"
    if (p in PUNCT3) return p
    p = substr(s, i, 2)
    if (p in PUNCT2) return p
    return substr(s, i, 1)
}
# post_line(body) — feed one line of the current side through the lexer, and finish a directive whose
# last line it is. A line that starts inside a comment or raw string is no directive.
function post_line(body,   st, tl, splice, cont, isdir, sgap) {
    # Shapes the per-line tracking cannot follow mark the file untrusted:
    #   - a backslash line splice outside a preprocessor directive's continuation (the
    #     compiler joins the lines first, so a string, char literal or comment can run on);
    #   - a directive splice the tracking could misread: one that splits the directive
    #     name (`#el\` + `se`), or a spliced directive line holding a quote, a comment
    #     token, or a '/' or '*' just before the backslash (the join can open or close a
    #     string or comment the per-line lexer never sees);
    #   - a comment between '#' and the directive name (`# /* c */ else`);
    #   - a `%:` digraph directive;
    #   - a directive that ends inside a block comment (the comment carries it onto the next line).
    st = trim(body)
    tl = body
    sub(/[ \t\r]+$/, "", tl)
    splice = (tl ~ /\\$/)
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
    sgap = splice && tl ~ /[ \t]\\$/
    # A directive after a closing */ (`/* x */ #else`, `/* a */ # /* b */ else`, `/* x */ %:else`,
    # or the last line of a multi-line comment) is one the compiler honours but this tracking
    # never sees: the #if stack would be wrong, so the file's tracking is not trusted.
    # (Only a conditional name or a comment after the '#': an ImGui id path "**/##id" is no directive.)
    if (body ~ /\*\/[ \t]*(%:|#[ \t]*(\/\*|(if|ifdef|ifndef|elif|elifdef|elifndef|else|endif)([^A-Za-z0-9_]|$)))/) lx_untrusted = 1
    if (isdir) { DTN[cur_side] = 0; DFNR[cur_side] = FNR }
    lex_line(body, isdir || cont, cont)
    lx_sgap = sgap
    if ((isdir || cont) && lx_blk) lx_untrusted = 1
    if ((isdir || cont) && !lx_macro) dir_end()
}
# Pass-1 trust check, at each hunk end: a side whose tracking leaves the #if stack open, closed an
# arm it never opened, or stops inside a comment / raw string cannot be trusted, nor can its file.
function side_bad(s) { side(s); return DEP[s] != 0 || UF[s] || lx_blk || lx_raw || lx_untrusted || lx_macro }
function end_hunk1() {
    if (hunk_open) {
        if (prodo1 && side_bad("pre")) untrusted[fd] = 1
        if (prodn1 && side_bad("post")) untrusted[fd] = 1
    }
    hunk_open = 0
}

# ---- Pass 2: comparing the queues ----
function short(t) { return length(t) > 60 ? substr(t, 1, 57) "..." : t }
function fail(why) {
    if (!fbad) print fpath ": " why
    fbad = 1
}
function compare(   a, b) {
    while (!fbad && QH["pre"] < QT["pre"] && QH["post"] < QT["post"]) {
        a = Q["pre", QH["pre"] + 1]
        b = Q["post", QH["post"] + 1]
        if (a != b) {
            fail("line " QL["post", QH["post"] + 1] ": the compiled code changes (`" short(a) "` -> `" short(b) "`)")
            return
        }
        QH["pre"]++
        QH["post"]++
        delete Q["pre", QH["pre"]]; delete QL["pre", QH["pre"]]
        delete Q["post", QH["post"]]; delete QL["post", QH["post"]]
    }
}
function end_hunk2() {
    if (hunk_open && !fbad && (prodo2 || prodn2)) {
        flush_side("pre")
        flush_side("post")
        compare()
        if (!fbad && QH["pre"] < QT["pre"])
            fail("line " QL["pre", QH["pre"] + 1] " (before): compiled code removed (`" short(Q["pre", QH["pre"] + 1]) "`)")
        if (!fbad && QH["post"] < QT["post"])
            fail("line " QL["post", QH["post"] + 1] ": compiled code added (`" short(Q["post", QH["post"] + 1]) "`)")
    }
    hunk_open = 0
}
BEGIN {
    SQ = sprintf("%c", 39); fd = 0; cur_side = "post"
    n3 = split("<<= >>= ... ->*", tmp, " ")
    for (i = 1; i <= n3; i++) PUNCT3[tmp[i]] = 1
    n2 = split(":: -> ++ -- << >> <= >= == != && || += -= *= /= %= &= |= ^= .* ## <: :> <% %> %:", tmp, " ")
    for (i = 1; i <= n2; i++) PUNCT2[tmp[i]] = 1
    DIGRAPH["<%"] = "{"; DIGRAPH["%>"] = "}"; DIGRAPH["<:"] = "["; DIGRAPH[":>"] = "]"
    DIGRAPH["%:"] = "#"; DIGRAPH["%:%:"] = "##"
}
# Pass 1: per file diff (fd), whether its lexing can be trusted; the #if groups that leave no
# directive token.
NR == FNR {
    pass = 1
    if ($0 ~ /^diff --git /) {
        end_hunk1()
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
    if ($0 ~ /^@@/) { end_hunk1(); hunk_begin($0); next }
    c1 = substr($0, 1, 1)
    if (prodo1 && (c1 == " " || c1 == "-")) feed("pre", $0)
    if (prodn1 && (c1 == " " || c1 == "+")) feed("post", $0)
    next
}
FNR == 1 && !pass2 {
    pass2 = 1
    end_hunk1()
    fd = 0
    in_hdr = 0
    hunk_open = 0
}
# Pass 2: compare each product file's two token streams.
{
    pass = 2
    if ($0 ~ /^diff --git /) {
        end_hunk2()
        fd++; in_hdr = 1; fo2 = ""; fn2 = ""; prodo2 = 0; prodn2 = 0; fbad = 0
        next
    }
    if (in_hdr) {
        if ($0 ~ /^--- /) fo2 = path_of($0)
        else if ($0 ~ /^\+\+\+ /) fn2 = path_of($0)
        else if ($0 ~ /^@@/) {
            in_hdr = 0
            prodo2 = is_prod(fo2)
            prodn2 = is_prod(fn2)
            fpath = prodn2 ? fn2 : fo2
            if ((prodo2 || prodn2) && (fd in untrusted)) fail("its lexing cannot be trusted (see the gate's header)")
            hunk_begin($0)
        }
        next
    }
    if ($0 ~ /^@@/) { end_hunk2(); hunk_begin($0); next }
    if (fbad) next
    c1 = substr($0, 1, 1)
    if (prodo2 && (c1 == " " || c1 == "-")) feed("pre", $0)
    if (prodn2 && (c1 == " " || c1 == "+")) feed("post", $0)
    compare()
}
END { end_hunk2() }
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

# _classify_diff_file <diff-file> — decide a full-context diff: prints "EXEMPT", or "FALLTHROUGH"
# and, on a second line, why (the first file that fails, "<path>: <reason>"). The prefilter writes
# a temp FILE, never a pipe (see the GIT_DIFF_TMPFILE note in the normal run below). Returns
# non-zero (printing nothing) if the prefilter fails.
_classify_diff_file() {
    local reduced rc=0 bin
    # git prints a file it takes for binary (one NUL byte, even inside a comment the compilers
    # ignore) as a single "Binary files ... differ" line: a C/C++ file in that form hides its whole
    # change from the lexer, so it is never exempt.
    while IFS= read -r bin; do
        bin="${bin#Binary files }"
        if _binary_names_product "${bin% differ}"; then
            printf 'FALLTHROUGH\n%s\n' "git shows a product file as binary: ${bin% differ}"
            return 0
        fi
    done < <(grep '^Binary files ' "$1" || true)
    # git quotes a path that holds a quote, a backslash or a control byte ("+++ \"b/..."): the
    # patterns here cannot read it, so it is never exempt.
    if grep -qE '^(\+\+\+|---) "' "$1"; then
        printf 'FALLTHROUGH\n%s\n' "git had to quote a changed path"
        return 0
    fi
    reduced="$(mktemp)"
    _prefilter_diff "$1" >"$reduced" || rc=$?
    if [ "$rc" -eq 0 ]; then
        if [ -s "$reduced" ]; then
            printf 'FALLTHROUGH\n'
            head -n 1 "$reduced"
        else
            echo EXEMPT
        fi
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
        local want="$1" label="$2" out got
        cat >"$_st_diff"
        out="$(_classify_diff_file "$_st_diff")"
        got="${out%%$'\n'*}"
        if [ "$got" = "$want" ]; then
            echo "  ok   [$want] $label"
        else
            echo "  FAIL [$want != $got] $label"
            [ "$out" = "$got" ] || echo "         ${out#*$'\n'}"
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
@@ -1,0 +1,2 @@
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
     }
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

    # Include/using-only (#3-style include cleanup): an include can change what a later macro or name
    # means (a #define before it, an #ifndef after it, an initializer it sits in), so it is compared.
    _expect FALLTHROUGH "include/using-only" <<'EOF'
diff --git a/Source/Core/include/AppController.h b/Source/Core/include/AppController.h
--- a/Source/Core/include/AppController.h
+++ b/Source/Core/include/AppController.h
@@ -1,1 +1,4 @@
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
    # fan-in: drop a heavy `#include`, add a `class …;` fwd-decl + json_fwd): a forward
    # declaration can hide an outer type, so it is compared.
    _expect FALLTHROUGH "forward-declaration-only header (#1308)" <<'EOF'
diff --git a/Source/Core/include/AppController.h b/Source/Core/include/AppController.h
--- a/Source/Core/include/AppController.h
+++ b/Source/Core/include/AppController.h
@@ -1,1 +1,6 @@
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
    # byte-identical (indentation aside) into a new TU inside a namespace: callers in other TUs
    # can reach another overload, so a move between files is compared like any other change.
    _expect FALLTHROUGH "header inline body relocated byte-identical to a .cpp (#1317)" <<'EOF'
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

    # A guard that holds in every build changes nothing the compilers build.
    _expect EXEMPT "a guard that always holds, around existing code" <<'EOF'
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

    # A guard the gate cannot decide drops the code from some tested build (an undefined macro is
    # an #if 0 there).
    _expect FALLTHROUGH "a guard the gate cannot decide, around existing code" <<'EOF'
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

    # ---- The compiled token streams must match (round-8 counterexamples) ----

    _expect FALLTHROUGH "code wrapped into a block comment" <<'EOF'
diff --git a/Source/Core/src/Sync/Save5.cpp b/Source/Core/src/Sync/Save5.cpp
--- a/Source/Core/src/Sync/Save5.cpp
+++ b/Source/Core/src/Sync/Save5.cpp
@@ -1,6 +1,8 @@
 bool ValidateTicket(int);
 void Persist(int);
 void Save(int t) {
+    /* validation disabled while the backend migrates
     if (!ValidateTicket(t)) return;
+    */
     Persist(t);
 }
EOF

    _expect FALLTHROUGH "code taken back out of a block comment" <<'EOF'
diff --git a/Source/Core/src/Sync/Purge4.cpp b/Source/Core/src/Sync/Purge4.cpp
--- a/Source/Core/src/Sync/Purge4.cpp
+++ b/Source/Core/src/Sync/Purge4.cpp
@@ -1,8 +1,6 @@
 void PurgeAllLocalTickets();
 void ResetSyncCursor();
 void OnStartup() {
-    /*
     PurgeAllLocalTickets();
     ResetSyncCursor();
-    */
 }
EOF

    _expect FALLTHROUGH "an #else added after an #if 0" <<'EOF'
diff --git a/Source/Core/src/Sync/Purge2.cpp b/Source/Core/src/Sync/Purge2.cpp
--- a/Source/Core/src/Sync/Purge2.cpp
+++ b/Source/Core/src/Sync/Purge2.cpp
@@ -1,6 +1,7 @@
 void PurgeAllLocalTickets();
 void Purge() {
 #if 0
+#else
     PurgeAllLocalTickets();
 #endif
 }
EOF

    _expect FALLTHROUGH "an #endif moved up out of an #if 0" <<'EOF'
diff --git a/Source/Core/src/Sync/Purge3.cpp b/Source/Core/src/Sync/Purge3.cpp
--- a/Source/Core/src/Sync/Purge3.cpp
+++ b/Source/Core/src/Sync/Purge3.cpp
@@ -1,8 +1,8 @@
 void PurgeAllLocalTickets();
 void ResetSyncCursor();
 void Purge() {
 #if 0
+#endif
     PurgeAllLocalTickets();
     ResetSyncCursor();
-#endif
 }
EOF

    _expect FALLTHROUGH "an #endif moved up out of an Android-only arm" <<'EOF'
diff --git a/Source/Core/src/Sync/Wipe2.cpp b/Source/Core/src/Sync/Wipe2.cpp
--- a/Source/Core/src/Sync/Wipe2.cpp
+++ b/Source/Core/src/Sync/Wipe2.cpp
@@ -1,8 +1,8 @@
 void WipeLocalCacheOnLowStorage();
 void ResetSyncCursor();
 void OnStartup() {
 #ifdef __ANDROID__
+#endif
     WipeLocalCacheOnLowStorage();
     ResetSyncCursor();
-#endif
 }
EOF

    _expect FALLTHROUGH "an #ifdef __ANDROID__ flipped to #ifndef" <<'EOF'
diff --git a/Source/Core/src/Sync/Wipe.cpp b/Source/Core/src/Sync/Wipe.cpp
--- a/Source/Core/src/Sync/Wipe.cpp
+++ b/Source/Core/src/Sync/Wipe.cpp
@@ -1,7 +1,7 @@
 void WipeLocalCacheOnLowStorage();
 void OnStartup() {
-#ifdef __ANDROID__
+#ifndef __ANDROID__
     WipeLocalCacheOnLowStorage();
 #endif
 }
EOF

    _expect FALLTHROUGH "an #else arm removed from a guard the gate cannot decide" <<'EOF'
diff --git a/Source/Core/src/Sync/Run.cpp b/Source/Core/src/Sync/Run.cpp
--- a/Source/Core/src/Sync/Run.cpp
+++ b/Source/Core/src/Sync/Run.cpp
@@ -1,9 +1,8 @@
 void RunLua();
 void WipeCacheFallback();
 void Run() {
 #ifdef SMATCHET_WITH_LUA
     RunLua();
-#else
     WipeCacheFallback();
 #endif
 }
EOF

    _expect FALLTHROUGH "a catch clause removed (no empty catch (...) follows)" <<'EOF'
diff --git a/Source/Core/src/Sync/Replay.cpp b/Source/Core/src/Sync/Replay.cpp
--- a/Source/Core/src/Sync/Replay.cpp
+++ b/Source/Core/src/Sync/Replay.cpp
@@ -1,15 +1,13 @@
 #include "Logger.h"
 #include <stdexcept>
 struct NetworkError : std::runtime_error {
     using std::runtime_error::runtime_error;
 };
 void ReplayOne();
 void ScheduleRetry();
 void ReplayQueue() {
     try {
         ReplayOne();
     } catch (const NetworkError& e) {
         ScheduleRetry();
-    } catch (const std::exception& e) {
-        LOG_WARN("replay failed: %s", e.what());
     }
 }
EOF

    _expect FALLTHROUGH "a try block unwrapped" <<'EOF'
diff --git a/Source/Core/src/Sync/Save.cpp b/Source/Core/src/Sync/Save.cpp
--- a/Source/Core/src/Sync/Save.cpp
+++ b/Source/Core/src/Sync/Save.cpp
@@ -1,13 +1,10 @@
 #include "Logger.h"
 #include <mutex>
 std::mutex m;
 void SaveToDisk();
 void Flush() {
-    try {
+    {
         std::lock_guard<std::mutex> lk(m);
         SaveToDisk();
-    } catch (const std::exception& ex) {
-        LOG_WARN("flush failed: %s", ex.what());
     }
     LOG_INFO("flushed");
 }
EOF

    _expect FALLTHROUGH "a catch clause widened" <<'EOF'
diff --git a/Source/Core/src/Sync/Replay.cpp b/Source/Core/src/Sync/Replay.cpp
--- a/Source/Core/src/Sync/Replay.cpp
+++ b/Source/Core/src/Sync/Replay.cpp
@@ -1,13 +1,13 @@
 #include "Logger.h"
 #include <stdexcept>
 struct NetworkError : std::runtime_error {
     using std::runtime_error::runtime_error;
 };
 void ReplayOne();
 void ScheduleRetry();
 void ReplayQueue() {
     try {
         ReplayOne();
-    } catch (const NetworkError& e) {
+    } catch (const std::exception& e) {
         ScheduleRetry();
     }
 }
EOF

    _expect FALLTHROUGH "a LOG call spanning an #if 0 arm, then a new statement" <<'EOF'
diff --git a/Source/Core/src/Sync/Count.cpp b/Source/Core/src/Sync/Count.cpp
--- a/Source/Core/src/Sync/Count.cpp
+++ b/Source/Core/src/Sync/Count.cpp
@@ -1,10 +1,11 @@
 #include "Logger.h"
 int Count(int);
 void PurgeAllLocalTickets();
 void Report(int a) {
     LOG_INFO("count %d",
 #if 0
              OldCount(
 #endif
              Count(a));
+    PurgeAllLocalTickets();
 }
EOF

    _expect FALLTHROUGH "a LOG call spanning undecidable arms, then a new statement" <<'EOF'
diff --git a/Source/Core/src/Sync/Count.cpp b/Source/Core/src/Sync/Count.cpp
--- a/Source/Core/src/Sync/Count.cpp
+++ b/Source/Core/src/Sync/Count.cpp
@@ -1,13 +1,14 @@
 #include "Logger.h"
 int Count(int);
 int Count2(int);
 void PurgeAllLocalTickets();
 void Report(int a) {
     LOG_INFO("count %d",
 #ifdef SMATCHET_WITH_LUA
              Count(
 #else
              Count2(
 #endif
                  a));
+    PurgeAllLocalTickets();
 }
EOF

    _expect FALLTHROUGH "a header definition from an #if 0 arm moved to a .cpp" <<'EOF'
diff --git a/Source/Core/include/Sync/Purge.h b/Source/Core/include/Sync/Purge.h
--- a/Source/Core/include/Sync/Purge.h
+++ b/Source/Core/include/Sync/Purge.h
@@ -1,9 +1,6 @@
 #pragma once
 void PurgeAllLocalTickets();
 #if 0
-inline void AutoPurge(int days) {
-    if (days > 0) {
-        PurgeAllLocalTickets();
-    }
-}
 #endif
+void AutoPurge(int days);
diff --git a/Source/Core/src/Sync/Purge.cpp b/Source/Core/src/Sync/Purge.cpp
--- a/Source/Core/src/Sync/Purge.cpp
+++ b/Source/Core/src/Sync/Purge.cpp
@@ -1,2 +1,7 @@
 #include "Sync/Purge.h"
 void PurgeAllLocalTickets() {}
+void AutoPurge(int days) {
+    if (days > 0) {
+        PurgeAllLocalTickets();
+    }
+}
EOF

    _expect FALLTHROUGH "a definition moved from tests/ into a Source/Core .cpp" <<'EOF'
diff --git a/tests/support/PurgeHelpers.h b/tests/support/PurgeHelpers.h
--- a/tests/support/PurgeHelpers.h
+++ b/tests/support/PurgeHelpers.h
@@ -1,7 +1,3 @@
 #pragma once
 void PurgeAllLocalTickets();
-inline void AutoPurge(int days) {
-    if (days > 0) {
-        PurgeAllLocalTickets();
-    }
-}
+void AutoPurge(int days);
diff --git a/Source/Core/src/Sync/Purge.cpp b/Source/Core/src/Sync/Purge.cpp
--- a/Source/Core/src/Sync/Purge.cpp
+++ b/Source/Core/src/Sync/Purge.cpp
@@ -1,2 +1,7 @@
 #include "Sync/Purge.h"
 void PurgeAllLocalTickets() {}
+void AutoPurge(int days) {
+    if (days > 0) {
+        PurgeAllLocalTickets();
+    }
+}
EOF

    _expect FALLTHROUGH "a type alias changed" <<'EOF'
diff --git a/Source/Core/src/Sync/Backoff.cpp b/Source/Core/src/Sync/Backoff.cpp
--- a/Source/Core/src/Sync/Backoff.cpp
+++ b/Source/Core/src/Sync/Backoff.cpp
@@ -1,7 +1,7 @@
 #include <chrono>
 #include <cstdint>
-using Clock = std::chrono::steady_clock;
-using RetryCount = std::int64_t;
+using Clock = std::chrono::system_clock;
+using RetryCount = std::int8_t;
 RetryCount Next(RetryCount n) { return n * 2; }
 bool Expired(Clock::time_point deadline) { return Clock::now() > deadline; }
EOF

    _expect FALLTHROUGH "a scope brace pair removed" <<'EOF'
diff --git a/Source/Core/src/Sync/Queue.cpp b/Source/Core/src/Sync/Queue.cpp
--- a/Source/Core/src/Sync/Queue.cpp
+++ b/Source/Core/src/Sync/Queue.cpp
@@ -1,12 +1,12 @@
 #include <functional>
 #include <mutex>
 #include <vector>
 std::mutex mutex_;
 std::vector<int> pending_;
 std::function<void(int)> callback_;
 void Push(int item) {
-    {
+    // clang-format off
         std::lock_guard<std::mutex> lk(mutex_);
         pending_.push_back(item);
-    }
+    // clang-format on
     callback_(item);
 }
EOF

    # Reformatting changes no token.
    _expect EXEMPT "a reformatted statement" <<'EOF'
diff --git a/Source/Core/src/Sync/Fmt.cpp b/Source/Core/src/Sync/Fmt.cpp
--- a/Source/Core/src/Sync/Fmt.cpp
+++ b/Source/Core/src/Sync/Fmt.cpp
@@ -1,3 +1,5 @@
 int Sum(int a, int b) {
-    return Combine(a,b);
+    return Combine(
+        a,
+        b);
 }
EOF

    # Punctuators are read by maximal munch: `a && b` and `a & &b` are different code.
    _expect FALLTHROUGH "a logical and split into bitwise and address-of" <<'EOF'
diff --git a/Source/Core/src/Sync/Amp.cpp b/Source/Core/src/Sync/Amp.cpp
--- a/Source/Core/src/Sync/Amp.cpp
+++ b/Source/Core/src/Sync/Amp.cpp
@@ -1,3 +1,3 @@
 int Pick(int a, int b) {
-    return a && b;
+    return a & &b;
 }
EOF

    # The empty clause swallows what the catch (...) clause used to clean up after.
    _expect FALLTHROUGH "an empty catch clause before a catch (...) that does something" <<'EOF'
diff --git a/Source/Core/src/Sync/Swallow.cpp b/Source/Core/src/Sync/Swallow.cpp
--- a/Source/Core/src/Sync/Swallow.cpp
+++ b/Source/Core/src/Sync/Swallow.cpp
@@ -1,7 +1,9 @@
 void Run() {
     try {
         Step();
+    } catch (const std::exception& e) {
+        LOG_WARN("step failed: %s", e.what());
     } catch (...) {
         Rollback();
     }
 }
EOF

    # A using-declaration is compared like any declaration.
    _expect FALLTHROUGH "using-declarations reordered after an undecidable guard" <<'EOF'
diff --git a/Source/Core/src/Config/Order.cpp b/Source/Core/src/Config/Order.cpp
--- a/Source/Core/src/Config/Order.cpp
+++ b/Source/Core/src/Config/Order.cpp
@@ -1,6 +1,6 @@
 #if defined(_WIN32)
 using detail::Protect;
 #endif
+using detail::LoadExtras;
 using detail::LoadScalars;
-using detail::LoadExtras;
EOF

    # Behind a guard the gate cannot decide, an opening brace may not be compiled: the LOG after it
    # may be the if's whole body.
    _expect FALLTHROUGH "a LOG after a brace only one arm compiles" <<'EOF'
diff --git a/Source/Core/src/Sync/Arm.cpp b/Source/Core/src/Sync/Arm.cpp
--- a/Source/Core/src/Sync/Arm.cpp
+++ b/Source/Core/src/Sync/Arm.cpp
@@ -1,11 +1,12 @@
 void Run(bool c) {
     if (c)
 #ifdef SMATCHET_WITH_LUA
     {
 #endif
+        LOG_INFO("taking the branch");
         DoIt();
 #ifdef SMATCHET_WITH_LUA
     }
 #endif
 }
EOF

    # One arm ends inside an if: after the group, the LOG may be that if's body.
    _expect FALLTHROUGH "a LOG after a group whose arm ends mid-statement" <<'EOF'
diff --git a/Source/Core/src/Sync/Tail.cpp b/Source/Core/src/Sync/Tail.cpp
--- a/Source/Core/src/Sync/Tail.cpp
+++ b/Source/Core/src/Sync/Tail.cpp
@@ -1,6 +1,7 @@
 void Run(bool c) {
 #ifdef SMATCHET_WITH_LUA
     if (c)
 #endif
+    LOG_INFO("x");
     DoIt();
 }
EOF

    # Each arm follows the token before the group, here an if's condition.
    _expect FALLTHROUGH "a LOG opening an #else arm that follows an if" <<'EOF'
diff --git a/Source/Core/src/Sync/ElseArm.cpp b/Source/Core/src/Sync/ElseArm.cpp
--- a/Source/Core/src/Sync/ElseArm.cpp
+++ b/Source/Core/src/Sync/ElseArm.cpp
@@ -1,8 +1,9 @@
 void Run(bool c) {
     if (c)
 #ifdef SMATCHET_WITH_LUA
         Foo();
 #else
+        LOG_INFO("x");
         Bar();
 #endif
 }
EOF

    # The Unreal plugin's arm is built by no test target: it is not left out.
    _expect FALLTHROUGH "a statement changed in an Unreal-only arm" <<'EOF'
diff --git a/Source/Core/src/Ui/Host.cpp b/Source/Core/src/Ui/Host.cpp
--- a/Source/Core/src/Ui/Host.cpp
+++ b/Source/Core/src/Ui/Host.cpp
@@ -1,5 +1,5 @@
 void Init() {
 #ifdef SMATCHET_EMBEDDED_IN_UNREAL
-    UseEngineFonts();
+    UseBundledFonts();
 #endif
 }
EOF

    # ---- Round-9 counterexamples and false positives ----

    _expect FALLTHROUGH "a using-declaration in a class (it changes which overload a call reaches)" <<'EOF'
diff --git a/Source/Core/src/Over.cpp b/Source/Core/src/Over.cpp
--- a/Source/Core/src/Over.cpp
+++ b/Source/Core/src/Over.cpp
@@ -1,5 +1,6 @@
 struct B { int f(int) { return 1; } };
 struct D : B {
+    using B::f;
     int f(double) { return 2; }
 };
 int Pick() { D d; return d.f(1); }
EOF

    _expect FALLTHROUGH "a using-declaration in a block" <<'EOF'
diff --git a/Source/Core/src/Over.cpp b/Source/Core/src/Over.cpp
--- a/Source/Core/src/Over.cpp
+++ b/Source/Core/src/Over.cpp
@@ -1,5 +1,6 @@
 int g(int) { return 1; }
 namespace n { int g(long) { return 2; } }
 int Pick() {
+    using n::g;
     return g(1);
 }
EOF

    _expect FALLTHROUGH "a relocated body whose raw string is re-indented" <<'EOF'
diff --git a/Source/Core/include/Help.h b/Source/Core/include/Help.h
--- a/Source/Core/include/Help.h
+++ b/Source/Core/include/Help.h
@@ -1,6 +1,2 @@
 #pragma once
-inline const char* HelpText() {
-    return R"(usage:
-    smatchet --sync
-)";
-}
+const char* HelpText();
diff --git a/Source/Core/src/Help.cpp b/Source/Core/src/Help.cpp
--- a/Source/Core/src/Help.cpp
+++ b/Source/Core/src/Help.cpp
@@ -1,2 +1,7 @@
 #include "Help.h"
 int Unused() { return 0; }
+const char* HelpText() {
+    return R"(usage:
+smatchet --sync
+)";
+}
EOF

    _expect FALLTHROUGH "a relocated body whose string loses an 'inline'" <<'EOF'
diff --git a/Source/Core/include/Mode.h b/Source/Core/include/Mode.h
--- a/Source/Core/include/Mode.h
+++ b/Source/Core/include/Mode.h
@@ -1,2 +1,2 @@
 #pragma once
-inline const char* ModeLabel() { return "edit inline mode"; }
+const char* ModeLabel();
diff --git a/Source/Core/src/Mode.cpp b/Source/Core/src/Mode.cpp
--- a/Source/Core/src/Mode.cpp
+++ b/Source/Core/src/Mode.cpp
@@ -1 +1,2 @@
 #include "Mode.h"
+const char* ModeLabel() { return "edit mode"; }
EOF

    _expect FALLTHROUGH "a space between a macro name and its parameter list" <<'EOF'
diff --git a/Source/Core/src/Trace.cpp b/Source/Core/src/Trace.cpp
--- a/Source/Core/src/Trace.cpp
+++ b/Source/Core/src/Trace.cpp
@@ -1,5 +1,5 @@
 static int calls = 0;
 static int bump(int v) { calls++; return v; }
-#define TRACE(bump)
+#define TRACE (bump)
 int Run() { TRACE(1); return calls; }
 int Keep() { return bump(0); }
EOF

    _expect FALLTHROUGH "a relocation without a header declaration (callers change overloads)" <<'EOF'
diff --git a/Source/Core/include/Scale.h b/Source/Core/include/Scale.h
--- a/Source/Core/include/Scale.h
+++ b/Source/Core/include/Scale.h
@@ -1,3 +1,2 @@
 #pragma once
 inline int Scale(long v) { return static_cast<int>(v * 2); }
-inline int Scale(int v) { return v * 3; }
diff --git a/Source/Core/src/Scale.cpp b/Source/Core/src/Scale.cpp
--- a/Source/Core/src/Scale.cpp
+++ b/Source/Core/src/Scale.cpp
@@ -1 +1,2 @@
 #include "Scale.h"
+int Scale(int v) { return v * 3; }
EOF

    _expect FALLTHROUGH "existing code moved into a namespace a relocation added" <<'EOF'
diff --git a/Source/Core/include/Calc.h b/Source/Core/include/Calc.h
--- a/Source/Core/include/Calc.h
+++ b/Source/Core/include/Calc.h
@@ -1,8 +1,6 @@
 #pragma once
 inline int Pick() { return 1; }
 namespace smatchet {
 inline int Pick() { return 2; }
-inline int Twice(int v) {
-    return v * 2;
-}
+int Twice(int v);
 }  // namespace smatchet
diff --git a/Source/Core/src/Calc.cpp b/Source/Core/src/Calc.cpp
--- a/Source/Core/src/Calc.cpp
+++ b/Source/Core/src/Calc.cpp
@@ -1,2 +1,7 @@
 #include "Calc.h"
+namespace smatchet {
+int Twice(int v) {
+    return v * 2;
+}
 int g_mode = Pick();
+}
EOF

    _expect FALLTHROUGH "a commented-out header definition 'relocated' into a .cpp" <<'EOF'
diff --git a/Source/Core/include/Pick.h b/Source/Core/include/Pick.h
--- a/Source/Core/include/Pick.h
+++ b/Source/Core/include/Pick.h
@@ -1,5 +1,4 @@
 #pragma once
 inline int Pick(long) { return 1; }
 /* Disabled:
-inline int Pick(int) { return 2; }
 */
diff --git a/Source/Core/src/Pick.cpp b/Source/Core/src/Pick.cpp
--- a/Source/Core/src/Pick.cpp
+++ b/Source/Core/src/Pick.cpp
@@ -1,2 +1,3 @@
 #include "Pick.h"
+int Pick(int) { return 2; }
 int Use() { return Pick(5); }
EOF

    _expect FALLTHROUGH "an empty by-value catch clause before an empty catch (...)" <<'EOF'
diff --git a/Source/Core/src/Catch.cpp b/Source/Core/src/Catch.cpp
--- a/Source/Core/src/Catch.cpp
+++ b/Source/Core/src/Catch.cpp
@@ -1,13 +1,15 @@
 struct Err {
     static int copies;
     Err() {}
     Err(const Err&) { ++copies; }
 };
 int Err::copies = 0;
 int Run() {
     try {
         throw Err();
+    } catch (Err e) {
+        LOG_WARN("caught");
     } catch (...) {
     }
     return Err::copies;
 }
EOF

    _expect FALLTHROUGH "a relocation whose header declaration has a trailing comment" <<'EOF'
diff --git a/Source/Core/include/Calc.h b/Source/Core/include/Calc.h
--- a/Source/Core/include/Calc.h
+++ b/Source/Core/include/Calc.h
@@ -1,6 +1,4 @@
 #pragma once
 namespace smatchet {
-inline int Twice(int v) {
-    return v * 2;
-}
+int Twice(int v);  // defined in Calc.cpp
 }  // namespace smatchet
diff --git a/Source/Core/src/Calc.cpp b/Source/Core/src/Calc.cpp
--- a/Source/Core/src/Calc.cpp
+++ b/Source/Core/src/Calc.cpp
@@ -1 +1,9 @@
 #include "Calc.h"
+
+namespace smatchet {
+
+int Twice(int v) {
+    return v * 2;
+}
+
+}  // namespace smatchet
EOF

    _expect EXEMPT "a LOG after a case label" <<'EOF'
diff --git a/Source/Core/src/Sw.cpp b/Source/Core/src/Sw.cpp
--- a/Source/Core/src/Sw.cpp
+++ b/Source/Core/src/Sw.cpp
@@ -1,9 +1,9 @@
 void Sw(int k) {
     switch (k) {
     case 1:
-        LOG_INFO("one");
+        LOG_INFO("one (k=1)");
         break;
     default:
         break;
     }
 }
EOF

    _expect EXEMPT "a LOG reworded after a group whose arms each open a brace" <<'EOF'
diff --git a/Source/Core/src/Unsure.cpp b/Source/Core/src/Unsure.cpp
--- a/Source/Core/src/Unsure.cpp
+++ b/Source/Core/src/Unsure.cpp
@@ -1,11 +1,11 @@
 #if defined(SMATCHET_WITH_LUA_AUTOMATION)
 struct Impl : public ILuaHost {
 #else
 struct Impl {
 #endif
     int x = 0;
 };
 
 void Later() {
-    LOG_INFO("later");
+    LOG_INFO("later on");
 }
EOF

    # A header's macro can turn an #if the gate cannot decide: there the includes are compared.
    _expect FALLTHROUGH "an include removed above an #ifndef of a project macro" <<'EOF'
diff --git a/Source/Core/src/About.cpp b/Source/Core/src/About.cpp
--- a/Source/Core/src/About.cpp
+++ b/Source/Core/src/About.cpp
@@ -1,5 +1,4 @@
-#include <SmatchetBuildInfo.h>
 #ifndef SMATCHET_BUILDINFO_AVAILABLE
 #define SMATCHET_GIT_SHA "unknown"
 #endif
 const char* Sha() { return SMATCHET_GIT_SHA; }
EOF

    # An include inside an initializer pastes code into it.
    _expect FALLTHROUGH "an include removed from inside an initializer" <<'EOF'
diff --git a/Source/Core/src/Ui/Icons.cpp b/Source/Core/src/Ui/Icons.cpp
--- a/Source/Core/src/Ui/Icons.cpp
+++ b/Source/Core/src/Ui/Icons.cpp
@@ -1,3 +1,2 @@
 const int kCatalog[] = {
-#include "Icons_Catalog.inl"
 };
EOF

    # An added include is compared, whatever groups surround it (#2242).
    _expect FALLTHROUGH "an include added beside platform-only groups" <<'EOF'
diff --git a/Source/Core/src/Ui/Cells.cpp b/Source/Core/src/Ui/Cells.cpp
--- a/Source/Core/src/Ui/Cells.cpp
+++ b/Source/Core/src/Ui/Cells.cpp
@@ -1,9 +1,10 @@
 #include "Cells.h"
+#include "MarkdownPreviewRender.h"
 #if defined(_WIN32)
 #ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #endif
 int Cell() { return 1; }
EOF

    _expect FALLTHROUGH "an include added under a header's include guard" <<'EOF'
diff --git a/Source/Core/include/Cells.h b/Source/Core/include/Cells.h
--- a/Source/Core/include/Cells.h
+++ b/Source/Core/include/Cells.h
@@ -1,5 +1,6 @@
 #ifndef SMATCHET_CELLS_H
 #define SMATCHET_CELLS_H
 #include <string>
+#include <vector>
 int Cell();
 #endif
EOF

    # Round-10 counterexamples: each changes the compiled code.
    _expect FALLTHROUGH "an include moved past a #define it now sees" <<'EOF'
diff --git a/Source/Core/src/Ui/Panel.cpp b/Source/Core/src/Ui/Panel.cpp
--- a/Source/Core/src/Ui/Panel.cpp
+++ b/Source/Core/src/Ui/Panel.cpp
@@ -1,4 +1,4 @@
-#include "Ui/Secret.h"
 #define ImGui SmatchetLocalizedImGui
+#include "Ui/Secret.h"
 void Draw() { ShowSecret(); }
EOF

    _expect FALLTHROUGH "an include switched inside an initializer with no brace" <<'EOF'
diff --git a/Source/Core/src/Ui/Palette.cpp b/Source/Core/src/Ui/Palette.cpp
--- a/Source/Core/src/Ui/Palette.cpp
+++ b/Source/Core/src/Ui/Palette.cpp
@@ -1,4 +1,4 @@
 const unsigned kPalette[] =
-#include "Palette_Dark.inl"
+#include "Palette_Light.inl"
 ;
 unsigned PaletteAt(int i) { return kPalette[i]; }
EOF

    _expect FALLTHROUGH "an include removed from an extern initializer" <<'EOF'
diff --git a/Source/Core/src/Ui/Icons.cpp b/Source/Core/src/Ui/Icons.cpp
--- a/Source/Core/src/Ui/Icons.cpp
+++ b/Source/Core/src/Ui/Icons.cpp
@@ -1,4 +1,3 @@
 extern const int kCatalog[] {
-#include "Icons_Catalog.inl"
 };
 int IconCount() { return sizeof(kCatalog) / sizeof(kCatalog[0]); }
EOF

    _expect FALLTHROUGH "an include removed above a define-if-undefined that sets a value" <<'EOF'
diff --git a/Source/Core/src/Diagnostics/About.cpp b/Source/Core/src/Diagnostics/About.cpp
--- a/Source/Core/src/Diagnostics/About.cpp
+++ b/Source/Core/src/Diagnostics/About.cpp
@@ -1,5 +1,4 @@
-#include "SmatchetBuildInfo.h"
 #ifndef SMATCHET_GIT_SHA
 #define SMATCHET_GIT_SHA "unknown"
 #endif
 const char* Sha() { return SMATCHET_GIT_SHA; }
EOF

    _expect FALLTHROUGH "code moved into a define-if-undefined group" <<'EOF'
diff --git a/Source/Core/include/Sha.h b/Source/Core/include/Sha.h
--- a/Source/Core/include/Sha.h
+++ b/Source/Core/include/Sha.h
@@ -1,5 +1,5 @@
 #pragma once
 #ifndef SMATCHET_GIT_SHA
 #define SMATCHET_GIT_SHA "unknown"
-#endif
 inline int Fallback() { return 1; }
+#endif
EOF

    _expect FALLTHROUGH "an opaque enum's underlying type changed" <<'EOF'
diff --git a/Source/Core/include/Row.h b/Source/Core/include/Row.h
--- a/Source/Core/include/Row.h
+++ b/Source/Core/include/Row.h
@@ -1,3 +1,3 @@
 #include <cstdint>
-enum class RowId : std::uint64_t;
+enum class RowId : std::uint8_t;
 RowId MakeRow(unsigned v);
EOF

    _expect FALLTHROUGH "a splice turns an object-like macro function-like" <<'EOF'
diff --git a/Source/Core/src/Trace.cpp b/Source/Core/src/Trace.cpp
--- a/Source/Core/src/Trace.cpp
+++ b/Source/Core/src/Trace.cpp
@@ -1,3 +1,4 @@
 int bump(int v) { return v; }
-#define TRACE (bump)
+#define TRACE\
+(bump)
 int Run() { return TRACE(1); }
EOF

    _expect EXEMPT "a function-like macro's body moved onto a spliced line" <<'EOF'
diff --git a/Source/Core/src/Trace.cpp b/Source/Core/src/Trace.cpp
--- a/Source/Core/src/Trace.cpp
+++ b/Source/Core/src/Trace.cpp
@@ -1,3 +1,4 @@
 int bump(int v) { return v; }
-#define TRACE(x) bump(x)
+#define TRACE(x) \
+    bump(x)
 int Run() { return TRACE(1); }
EOF

    # clang-format's directive reflows change no token.
    _expect EXEMPT "clang-format reflows of directives" <<'EOF'
diff --git a/Source/Core/include/Dbg.h b/Source/Core/include/Dbg.h
--- a/Source/Core/include/Dbg.h
+++ b/Source/Core/include/Dbg.h
@@ -1,7 +1,7 @@
-#  pragma once
-#define PAIR(a,b) ((a)+(b))
-#if defined(A)&&defined(B)
-#  define BOTH 1
+#pragma once
+#define PAIR(a, b) ((a) + (b))
+#if defined(A) && defined(B)
+#define BOTH 1
 #endif
 int Pair() { return PAIR(1, 2); }
EOF

    _expect FALLTHROUGH "a template forward declaration's default argument changed" <<'EOF'
diff --git a/Source/Core/src/Buf.cpp b/Source/Core/src/Buf.cpp
--- a/Source/Core/src/Buf.cpp
+++ b/Source/Core/src/Buf.cpp
@@ -1,3 +1,3 @@
-template <int N = 4> struct Buf;
+template <int N = 4096> struct Buf;
 template <int N> struct Buf { char data[N]; };
 int Size() { return sizeof(Buf<>); }
EOF

    # A C++14 compiler need not honour #elifndef: the gate decides nothing about it.
    _expect FALLTHROUGH "a C++23 #elifndef after an #if 0" <<'EOF'
diff --git a/Source/Core/src/Sync/Elif.cpp b/Source/Core/src/Sync/Elif.cpp
--- a/Source/Core/src/Sync/Elif.cpp
+++ b/Source/Core/src/Sync/Elif.cpp
@@ -1,4 +1,6 @@
 void Purge() {
+#if 0
+#elifndef __ANDROID__
     PurgeAllLocalTickets();
+#endif
 }
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

# Every diff below names its prefixes, turns rename detection on and colour off, and runs no external
# diff or textconv driver, whatever the local git configuration: the parsing here depends on all of it.
_gate_diff() {
    git -c core.quotePath=false diff --no-color --no-ext-diff --no-textconv --src-prefix=a/ --dst-prefix=b/ -M "$@"
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
# --unified=100000: full-file context, so _prefilter_diff lexes each changed file whole
# (the #if nesting, comments and raw strings every line sits in).
GIT_DIFF_TMPFILE="$(mktemp)"
trap 'rm -f "$GIT_DIFF_TMPFILE" "$_GATE_LIST_TMP"' EXIT
if ! _gate_diff --unified=100000 --diff-filter=ACMRT "$MERGE_BASE"...HEAD -- \
        Source/Core Source/Plugins Source/Standalone tests >"$GIT_DIFF_TMPFILE" 2>/dev/null; then
    echo "[coverage-delta-gate] FAIL — git diff failed (bad MERGE_BASE '$MERGE_BASE' or git error)" >&2
    exit 1
fi
if ! EXEMPTION_OUT="$(_classify_diff_file "$GIT_DIFF_TMPFILE")"; then
    echo "[coverage-delta-gate] FAIL — diff prefilter (awk) failed" >&2
    exit 1
fi
EXEMPTION="${EXEMPTION_OUT%%$'\n'*}"
if [ "$EXEMPTION" != "$EXEMPTION_OUT" ]; then
    echo "[coverage-delta-gate] no test-light exemption: ${EXEMPTION_OUT#*$'\n'}"
fi
if [ "$EXEMPTION" = "EXEMPT" ] && [ -n "$RENAMED_ACROSS" ]; then
    echo "[coverage-delta-gate] no test-light exemption: $RENAMED_ACROSS moves a file between what the build compiles"
    EXEMPTION=FALLTHROUGH
fi
if [ "$EXEMPTION" = "EXEMPT" ]; then
    echo "[coverage-delta-gate] PASS — test-light exemption: every product-code"
    echo "[coverage-delta-gate]        change is no-new-runtime-surface"
    echo "[coverage-delta-gate]        (comment/whitespace/log/static_assert/catch-scaffold/"
    echo "[coverage-delta-gate]        an #if arm no tested build compiles)."
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
echo "changed units. Changes that leave the compiled code unchanged (comments or"
echo "whitespace, logging or static_assert statements, a swallow->log catch, lines"
echo "inside an #if arm no tested build compiles, such as __ANDROID__) are"
echo "auto-exempted; if yours genuinely cannot be unit-tested, apply the"
echo "'tests-out-of-band' PR label to dismiss this gate."
exit 1

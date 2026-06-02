#!/usr/bin/env python3
"""Function-size analyzer for the decompose-top-20-monoliths gate (Slice 0).

Walks tracked first-party C++ (Source/Core, Source/Plugins, Source/Standalone; excludes
ThirdParty + tests), extracts every function *body* via a comment/string-neutralized brace
scan, and flags functions over the line / branch caps. The line cap is **tiered** by UI
classification (per maintainer decision 2026-06-01):

  function-too-long     body span > 120 lines (non-UI)  OR  > 200 lines (ImGui-draw)
  function-too-branchy  decision count > 30  (if/for/while/case/catch/&&/||/?), all functions

A function is "ImGui-draw" (the 200-line escape hatch — declarative UI is inherently noisier)
if EITHER its path is under a `Ui/` dir OR its unqualified name starts with Draw/Render/draw/
render (draw helpers live in non-Ui files too, e.g. SmatchetDrawAiAssistantPanel / drawMainMenuBar).
See is_ui_function() — the single source of truth, asserted against AGENTS.md by --selftest.

A non-blocking **soft-warning tier** nudges new code toward the 40-80-line ideal without failing
CI: > 100 lines OR > 20 branches emits a warning line (exit code unaffected), mirroring the
advisory comment-ratio warning (comment_audit.py --ratio-warn). Hard caps still block.

Modes mirror comment_audit.py (the comment-regrowth sibling gate):

  function_size_audit.py                  # human report of all current oversized functions
  function_size_audit.py --list           # one `rule<TAB>file:line<TAB>name (NL/MB)` per violation
  function_size_audit.py --baseline-md     # deterministic markdown grandfather snapshot
  function_size_audit.py --diff <ref>      # DELTA gate: emit hard-cap violations that are NEW or
                                           #   crossed a cap vs the merge-base of <ref>, PLUS
                                           #   advisory `[func-size] WARN ...` lines (non-failing)
                                           #   rule<TAB>basename:line<TAB>name (NL/MB)
  function_size_audit.py --selftest        # assert the UI-classification rule matches AGENTS.md

Delta semantics (grandfathering): a function is keyed by (rule, basename, qualified-name). The
existing monoliths live in the base set, so they never fire; a function fires only when it is
brand-new over a cap or has just crossed one. A grandfathered 600-line function growing further
stays grandfathered (same model as the comment-bloat rules). A `// SMATCHET_DEVIATION(rule=<id>;
...)` on the line above the signature suppresses a flagged function.

Exit contract (so test-lint-rules.sh can fail CLOSED): 0 = clean, 1 = violations found (printed
to stdout), >=2 = infra error (git / IO).

See docs/plans/active/decompose-top-20-monoliths.md § Slice 0.
"""

import argparse
import bisect
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import comment_lib as cl

# --- scope (KEEP IN SYNC with comment_audit.py SWEEP_ROOTS) --------------------
SWEEP_ROOTS = ("Source/Core/", "Source/Plugins/", "Source/Standalone/")
EXCLUDE_SUBSTR = ("/ThirdParty/", "ThirdParty/")
CPP_EXT = (".cpp", ".h", ".hpp", ".cc", ".cxx")

# --- caps (tiered; maintainer decision 2026-06-01) ----------------------------
# Hard caps (delta-gated, BLOCK): non-UI line count 120, ImGui-draw line count 200,
# branches 30 for ALL functions. Soft caps (advisory WARN, never block, never affect
# exit code): line count 100, branches 20 — the nudge toward the 40-80-line ideal.
LINE_LIMIT_NONUI = 120
LINE_LIMIT_UI = 200
BRANCH_LIMIT = 30
SOFT_LINE_LIMIT = 100
SOFT_BRANCH_LIMIT = 20

# UI classification (KEEP IN SYNC with AGENTS.md § Tiered enforcement; --selftest guards).
# A function is "ImGui-draw" (200-line cap) if its path is under a Ui/ dir OR its unqualified
# name starts with one of these prefixes (draw/render helpers live in non-Ui files too).
UI_PATH_SUBSTR = ("/Ui/", "Source/Core/src/Ui/", "Source/Core/include/Ui/")
UI_NAME_RE = re.compile(r"^(Draw|Render|draw|render)")

RULE_LONG = "function-too-long"
RULE_BRANCHY = "function-too-branchy"


def is_ui_function(path, name):
    """True if (path, name) is an ImGui-draw function (the 200-line escape hatch). Single source
    of truth for the UI/non-UI classification; --selftest asserts the rule text is in AGENTS.md."""
    p = path.replace("\\", "/")
    if any(s in p for s in UI_PATH_SUBSTR):
        return True
    simple = name.split("::")[-1].lstrip("~")
    return bool(UI_NAME_RE.match(simple))


def line_limit_for(path, name):
    return LINE_LIMIT_UI if is_ui_function(path, name) else LINE_LIMIT_NONUI

# Keywords that disqualify a `{` from being a function body.
CONTROL_KW = {"if", "for", "while", "switch", "catch"}
# Qualifiers that may sit between the param-list `)` and the body `{`.
QUALIFIER_KW = {"const", "noexcept", "override", "final", "volatile", "mutable",
                "throw", "constexpr", "consteval", "requires"}
# Call-like keywords with their own parens that are NOT the param list.
PAREN_QUALIFIER_KW = {"noexcept", "throw", "alignas", "decltype", "sizeof", "static_assert"}

BRANCH_RE = re.compile(r"\b(if|for|while|case|catch)\b")
IDENT_TAIL_RE = re.compile(r"[A-Za-z0-9_:~<>]")


# --- comment/string neutralizer ----------------------------------------------

def skeleton(text):
    """Return `text` with comment + string/char/raw-string spans replaced by spaces, newlines
    preserved, length identical (so char index -> line maps 1:1). After this pass the only
    braces/parens left are real code structure — safe to brace-match."""
    out = []
    i, n = 0, len(text)
    state = "code"
    raw_delim = None
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state == "code":
            if c == "/" and nxt == "/":
                out.append("  "); state = "line"; i += 2; continue
            if c == "/" and nxt == "*":
                out.append("  "); state = "block"; i += 2; continue
            if c == '"' and cl._is_raw_string_start(text, i):
                j = text.index("(", i)
                raw_delim = ")" + text[i + 1:j] + '"'
                out.append(" " * (j + 1 - i)); state = "raw"; i = j + 1; continue
            if c == '"':
                out.append(" "); state = "string"; i += 1; continue
            if c == "'":
                out.append(" "); state = "char"; i += 1; continue
            out.append(c); i += 1; continue
        if state == "line":
            if c == "\n":
                out.append("\n"); state = "code"
            else:
                out.append(" ")
            i += 1; continue
        if state == "block":
            if c == "*" and nxt == "/":
                out.append("  "); state = "code"; i += 2; continue
            out.append("\n" if c == "\n" else " "); i += 1; continue
        if state == "string":
            if c == "\\":
                out.append("  "); i += 2; continue
            if c == '"':
                state = "code"
            out.append("\n" if c == "\n" else " "); i += 1; continue
        if state == "char":
            if c == "\\":
                out.append("  "); i += 2; continue
            if c == "'":
                state = "code"
            out.append("\n" if c == "\n" else " "); i += 1; continue
        if state == "raw":
            if text.startswith(raw_delim, i):
                out.append(" " * len(raw_delim)); i += len(raw_delim); state = "code"; raw_delim = None; continue
            out.append("\n" if c == "\n" else " "); i += 1; continue
    return "".join(out)


def _line_starts(text):
    starts = [0]
    for i, ch in enumerate(text):
        if ch == "\n":
            starts.append(i + 1)
    return starts


def _line_of(starts, idx):
    return bisect.bisect_right(starts, idx)  # 1-based


# --- backward classifier ------------------------------------------------------

def _skip_ws_back(sk, pos):
    while pos >= 0 and sk[pos].isspace():
        pos -= 1
    return pos


def _match_paren_back(sk, close_idx):
    """Given index of a ')', return index of the matching '(' (or -1)."""
    depth = 0
    j = close_idx
    while j >= 0:
        c = sk[j]
        if c == ")":
            depth += 1
        elif c == "(":
            depth -= 1
            if depth == 0:
                return j
        j -= 1
    return -1


OP_SYMBOL_CHARS = set("=<>!+-*/%&|^~[]()")


def _name_before_paren(sk, open_paren):
    """Function name immediately before the param-list '(' at open_paren. Handles plain /
    qualified names (`Foo::Bar`, `~Foo`), symbolic operator overloads (`operator==`,
    `operator[]`, `operator()`), and conversion operators (`operator bool`). Returns "" when the
    token isn't a function name (control keyword resolved by the caller; lambda capture `]`)."""
    pos = _skip_ws_back(sk, open_paren - 1)
    if pos < 0:
        return ""
    # Case A — symbolic operator: the chars right before '(' are operator punctuation.
    if sk[pos] in OP_SYMBOL_CHARS:
        end = pos
        while pos >= 0 and (sk[pos] in OP_SYMBOL_CHARS or sk[pos].isspace()):
            pos -= 1
        word, _ws = _read_word_back(sk, pos)
        if word == "operator":
            return "operator" + re.sub(r"\s+", "", sk[pos + 1:end + 1])
        return ""  # e.g. `](` lambda, `)(` call/cast — not a named function
    # Case B — identifier run (plain / qualified / dtor).
    end = pos
    while pos >= 0 and IDENT_TAIL_RE.match(sk[pos]):
        pos -= 1
    name = sk[pos + 1:end + 1].strip()
    # Conversion operator: `operator bool(` — the word before the type is `operator`.
    prevpos = _skip_ws_back(sk, pos)
    if prevpos >= 0:
        pword, _ps = _read_word_back(sk, prevpos)
        if pword == "operator":
            return "operator " + name
    return name


def _read_word_back(sk, pos):
    """Read a [\\w] run ending at pos. Returns (word, start_index)."""
    end = pos
    while pos >= 0 and (sk[pos].isalnum() or sk[pos] == "_"):
        pos -= 1
    return sk[pos + 1:end + 1], pos + 1


def classify_brace(sk, brace_idx):
    """Decide whether the '{' at brace_idx opens a function body. Returns (name, param_open,
    param_close) on success, else None. Walks backward across trailing qualifiers /
    trailing-return to the param-list ')', then rejects control keywords and lambdas."""
    pos = brace_idx - 1
    guard = 0
    while guard < 4096:
        guard += 1
        pos = _skip_ws_back(sk, pos)
        if pos < 0:
            return None
        c = sk[pos]
        if c == ")":
            open_paren = _match_paren_back(sk, pos)
            if open_paren < 0:
                return None
            name = _name_before_paren(sk, open_paren)
            simple = name.split("::")[-1].lstrip("~")
            if name in CONTROL_KW or simple in CONTROL_KW:
                return None
            if name in PAREN_QUALIFIER_KW or simple in PAREN_QUALIFIER_KW:
                pos = open_paren - 1  # this paren belongs to a qualifier/expr; keep scanning
                continue
            if not name:
                return None  # `](...)`{ lambda / `)(`{ call — not a named function body
            return (name, open_paren, pos)
        if c.isalnum() or c == "_":
            word, ws = _read_word_back(sk, pos)
            if word in QUALIFIER_KW:
                pos = ws - 1
                continue
            return None  # else / try / do / class / namespace / a label / aggregate type
        if c == ">":
            arrow = sk.rfind("->", 0, pos)
            par = sk.rfind(")", 0, pos)
            if arrow != -1 and par != -1 and arrow > par:
                pos = par  # trailing return type -> jump before its ')'
                continue
            return None
        return None  # ';' '{' '}' '=' ':' ',' '(' digit -> not a function body
    return None


def _arity(sk, param_open, param_close):
    """Param count = top-level commas in (param_open, param_close) + 1 if non-empty. Stable
    overload disambiguator for the identity key (param-TYPE edits don't change it; add/remove a
    param does, which is the rare case that legitimately re-grandfathers)."""
    inner = sk[param_open + 1:param_close]
    if not inner.strip():
        return 0
    depth = 0
    commas = 0
    for ch in inner:
        if ch in "([{<":
            depth += 1
        elif ch in ")]}>":
            depth -= 1
        elif ch == "," and depth == 0:
            commas += 1
    return commas + 1


# --- function extraction ------------------------------------------------------

class Func(object):
    __slots__ = ("name", "arity", "start_line", "end_line", "lines", "branches")

    def __init__(self, name, arity, start_line, end_line, lines, branches):
        self.name = name
        self.arity = arity
        self.start_line = start_line
        self.end_line = end_line
        self.lines = lines
        self.branches = branches


def extract_functions(text):
    """Return a list of Func for every top-level / inline-method function body in `text`."""
    sk = skeleton(text)
    starts = _line_starts(text)
    depth = 0
    pending_start = 0           # char index just after the last statement/scope/preprocessor boundary
    active = None               # (open_brace_idx, base_depth, name, arity, start_line)
    funcs = []
    n = len(sk)
    i = 0
    at_line_start = True        # no non-space char seen yet on the current line
    pp_line = False             # current line is a preprocessor directive (#...)
    line_last_ns = ""           # last non-space char on the current physical line (for `\` splice)
    while i < n:
        c = sk[i]
        if c == "\n":
            # A `#...\` directive continues onto the next physical line — keep skipping (its braces
            # must not perturb depth / classify_brace) and don't bound the signature yet.
            continued = pp_line and line_last_ns == "\\"
            if pp_line and not continued:
                pending_start = i + 1   # the directive ends here; it bounds the next signature
            pp_line = continued
            at_line_start = True
            line_last_ns = ""
            i += 1
            continue
        if not c.isspace():
            if at_line_start and c == "#":
                pp_line = True
            at_line_start = False
            line_last_ns = c
        if pp_line:
            i += 1
            continue
        if c == "{":
            if active is None:
                hit = classify_brace(sk, i)
                if hit is not None:
                    name, p_open, p_close = hit
                    # Forward-skip over the skeleton's whitespace (comments + blank lines are
                    # spaces there) to the first real signature char — so a doc-comment / blank
                    # run above the function is not counted into its body.
                    sig_idx = pending_start
                    while sig_idx < i and sk[sig_idx].isspace():
                        sig_idx += 1
                    active = (i, depth, name, _arity(sk, p_open, p_close), _line_of(starts, sig_idx))
            depth += 1
            pending_start = i + 1
        elif c == "}":
            depth -= 1
            if active is not None and depth == active[1]:
                open_idx, _bd, name, arity, start_line = active
                end_line = _line_of(starts, i)
                body = sk[open_idx + 1:i]
                branches = (len(BRANCH_RE.findall(body))
                            + body.count("&&") + body.count("||") + body.count("?"))
                funcs.append(Func(name, arity, start_line, end_line, end_line - start_line + 1, branches))
                active = None
            pending_start = i + 1
        elif c == ";":
            pending_start = i + 1
        i += 1
    return funcs


def violations_for(path, text):
    """Yield (rule, line, name, arity, lines, branches) for each HARD-cap-oversized function in
    `text`. The line cap is tiered by UI classification (120 non-UI / 200 ImGui-draw); the branch
    cap is 30 for all. Soft-warning-tier functions are NOT yielded here — see warnings_for()."""
    for fn in extract_functions(text):
        if fn.lines > line_limit_for(path, fn.name):
            yield (RULE_LONG, fn.start_line, fn.name, fn.arity, fn.lines, fn.branches)
        if fn.branches > BRANCH_LIMIT:
            yield (RULE_BRANCHY, fn.start_line, fn.name, fn.arity, fn.lines, fn.branches)


def warnings_for(path, text):
    """Yield (line, name, arity, lines, branches, why) for each function in the ADVISORY soft tier
    (> 100 lines OR > 20 branches) that is NOT already a hard-cap violation. Never affects exit
    code — the nudge toward the 40-80-line ideal."""
    for fn in extract_functions(text):
        hard_long = fn.lines > line_limit_for(path, fn.name)
        hard_branchy = fn.branches > BRANCH_LIMIT
        if hard_long or hard_branchy:
            continue  # already a blocking violation; don't double-report as a warning
        why = []
        if fn.lines > SOFT_LINE_LIMIT:
            why.append("%d lines > %d" % (fn.lines, SOFT_LINE_LIMIT))
        if fn.branches > SOFT_BRANCH_LIMIT:
            why.append("%d branches > %d" % (fn.branches, SOFT_BRANCH_LIMIT))
        if why:
            yield (fn.start_line, fn.name, fn.arity, fn.lines, fn.branches, "; ".join(why))


# --- git plumbing -------------------------------------------------------------

# Force UTF-8 decoding of git output: source files carry non-ASCII (French locale strings,
# smart quotes) and the Windows default (cp1252) raises UnicodeDecodeError mid-read, which would
# silently empty a base file and false-flag its functions as "new".
def _git(args):
    p = subprocess.run(["git"] + args, capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    if p.returncode != 0:
        raise RuntimeError("git %s failed (%d): %s" % (" ".join(args), p.returncode, p.stderr.strip()))
    return p.stdout


def _git_ok(args):
    """git that tolerates non-zero (returns '' on failure) — for `git show ref:path` of files
    that may not exist at ref (new files)."""
    p = subprocess.run(["git"] + args, capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    return p.stdout if p.returncode == 0 else ""


def _in_scope(f):
    return (f.endswith(CPP_EXT) and f.startswith(SWEEP_ROOTS)
            and not any(s in f for s in EXCLUDE_SUBSTR))


def list_head_files():
    out = _git(["ls-files"] + [r + "**" for r in SWEEP_ROOTS])
    if not out.strip():
        out = _git(["ls-files"])
    return sorted({f for f in out.splitlines() if _in_scope(f)})


def list_ref_files(ref):
    out = _git(["ls-tree", "-r", "--name-only", ref])
    return sorted({f for f in out.splitlines() if _in_scope(f)})


def _read_head(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


def scan_head():
    """{(rule, basename, name, arity): (path, line, lines, branches)} for the working tree.
    arity is in the key so same-named overloads in one file don't collapse / mis-grandfather."""
    out = {}
    for f in list_head_files():
        text = _read_head(f)
        if not text:
            continue
        base = os.path.basename(f)
        for rule, line, name, arity, ln, br in violations_for(f, text):
            out.setdefault((rule, base, name, arity), (f, line, ln, br))
    return out


def scan_head_warnings():
    """{(basename, name, arity): (path, line, lines, branches, why)} of soft-tier functions in the
    working tree (advisory; never gates). Keyed like scan_head so the delta logic grandfathers
    pre-existing soft-tier functions — only new/changed ones warn."""
    out = {}
    for f in list_head_files():
        text = _read_head(f)
        if not text:
            continue
        base = os.path.basename(f)
        for line, name, arity, ln, br, why in warnings_for(f, text):
            out.setdefault((base, name, arity), (f, line, ln, br, why))
    return out


def scan_ref_warnings(ref):
    """Set of (basename, name, arity) keys in the soft-warning tier at <ref>."""
    keys = set()
    for f in list_ref_files(ref):
        text = _git_ok(["show", "%s:%s" % (ref, f)])
        if not text:
            continue
        base = os.path.basename(f)
        for _line, name, arity, _ln, _br, _why in warnings_for(f, text):
            keys.add((base, name, arity))
    return keys


def scan_ref(ref):
    """Set of (rule, basename, name, arity) keys oversized at <ref>."""
    keys = set()
    for f in list_ref_files(ref):
        text = _git_ok(["show", "%s:%s" % (ref, f)])
        if not text:
            continue
        base = os.path.basename(f)
        for rule, line, name, arity, ln, br in violations_for(f, text):
            keys.add((rule, base, name, arity))
    return keys


# --- delta gate ---------------------------------------------------------------

def _merge_base_or_ref(ref):
    p = subprocess.run(["git", "merge-base", ref, "HEAD"], capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    mb = p.stdout.strip()
    if p.returncode == 0 and mb:
        return mb
    # merge-base unresolved — usually a shallow clone missing the fork point. Falling back to
    # <ref>'s tip diffs against its LATEST, which false-flags a branch cut before a sibling change
    # landed. Warn loudly so the misconfiguration is visible, not a silent false-fail (tooling.md P1).
    sys.stderr.write("function_size_audit: WARN: `git merge-base %s HEAD` did not resolve "
                     "(shallow clone?) — falling back to %s tip; delta may false-flag.\n" % (ref, ref))
    return ref


def _suppressed(path, sig_line, rule_id):
    """True if a `// SMATCHET_DEVIATION(rule=<id>[,<id>...]; ...)` sits on the nearest non-blank
    line above the function signature (forward-only deviation grammar; blank lines don't break
    it). The rule= value may list several comma-separated ids, so a function that trips BOTH caps
    can be suppressed by one marker (`rule=function-too-long,function-too-branchy`)."""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            lines = fh.read().split("\n")
    except OSError:
        return False
    idx = sig_line - 2
    while 0 <= idx < len(lines) and lines[idx].strip() == "":
        idx -= 1
    prev = lines[idx] if 0 <= idx < len(lines) else ""
    if "SMATCHET_DEVIATION" not in prev:
        return False
    m = re.search(r"rule=([A-Za-z0-9_,-]+)", prev)
    if not m:
        return False
    return rule_id in [r.strip() for r in m.group(1).split(",")]


def run_diff(ref):
    base = _merge_base_or_ref(ref)
    head = scan_head()
    base_keys = scan_ref(base)
    violations = []
    for key in sorted(head):
        if key in base_keys:
            continue
        rule, bname, name, arity = key
        path, line, ln, br = head[key]
        if _suppressed(path, line, rule):
            continue
        violations.append("%s\t%s:%d\t%s/%d (%dL/%dbr)" % (rule, bname, line, name, arity, ln, br))
    for v in violations:
        print(v)
    # Advisory soft-warning tier (NEVER affects exit code): new/changed functions over the soft
    # caps (100 lines / 20 branches) but under the hard caps. Delta-gated like the hard rules so
    # pre-existing soft-tier functions don't spam every run.
    warn_head = scan_head_warnings()
    warn_base = scan_ref_warnings(base)
    for key in sorted(warn_head):
        if key in warn_base:
            continue
        bname, name, arity = key
        path, line, ln, br, why = warn_head[key]
        print("[func-size] WARN %s:%d %s/%d — %s (soft tier; not blocking, aim for 40-80 lines)"
              % (bname, line, name, arity, why), file=sys.stderr)
    return 1 if violations else 0


# --- reporting ----------------------------------------------------------------

def run_scan_file(path):
    """Git-free single-file scan (bats harness): print `rule<TAB>file:line<TAB>name (NL/MB)`."""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError as e:
        print("function_size_audit: ERROR: %s" % e, file=sys.stderr)
        return 2
    for rule, line, name, arity, ln, br in sorted(violations_for(path, text)):
        print("%s\t%s:%d\t%s/%d (%dL/%dbr)" % (rule, path, line, name, arity, ln, br))
    # Advisory soft-warning tier (to stderr — never affects exit code or the gate's stdout capture).
    for line, name, arity, ln, br, why in sorted(warnings_for(path, text)):
        print("[func-size] WARN %s:%d %s/%d — %s (soft tier; not blocking, aim for 40-80 lines)"
              % (path, line, name, arity, why), file=sys.stderr)
    return 0


def run_list():
    head = scan_head()
    rows = []
    for (rule, bname, name, arity), (path, line, ln, br) in head.items():
        rows.append("%s\t%s:%d\t%s/%d (%dL/%dbr)" % (rule, path, line, name, arity, ln, br))
    for r in sorted(rows):
        print(r)
    return 0


def run_baseline_md():
    head = scan_head()
    by_rule = {RULE_LONG: [], RULE_BRANCHY: []}
    for (rule, bname, name, arity), (path, line, ln, br) in head.items():
        metric = ("%d lines" % ln) if rule == RULE_LONG else ("%d branches" % br)
        by_rule.setdefault(rule, []).append("- `%s` · `%s/%d` · %s" % (bname, name, arity, metric))
    print("# Function-size — grandfathered baseline")
    print()
    print("_Auto-generated. Do not hand-edit; run "
          "`bash agents/scripts/project/test-lint-rules.sh --funcsize-baseline` and commit._")
    print("_The gate is a live merge-base delta vs `origin/develop` (function_size_audit.py "
          "--diff); this file is an informational snapshot, not the gate input._")
    total = 0
    cap_desc = {
        RULE_LONG: "%d lines non-UI / %d lines ImGui-draw" % (LINE_LIMIT_NONUI, LINE_LIMIT_UI),
        RULE_BRANCHY: "%d branches" % BRANCH_LIMIT,
    }
    for rule in (RULE_LONG, RULE_BRANCHY):
        rows = sorted(by_rule.get(rule, []))
        print()
        print("## %s (%d entries, cap %s)" % (rule, len(rows), cap_desc[rule]))
        if rows:
            for r in rows:
                print(r)
        else:
            print("- (none)")
        total += len(rows)
    print()
    print("## Totals")
    print("- oversized functions grandfathered: %d" % total)
    return 0


def run_report():
    head = scan_head()
    longs = sum(1 for k in head if k[0] == RULE_LONG)
    branchy = sum(1 for k in head if k[0] == RULE_BRANCHY)
    print("## Function-size audit — first-party C++ (current tree)\n")
    print("- hard cap: %d lines non-UI / %d lines ImGui-draw / %d branches"
          % (LINE_LIMIT_NONUI, LINE_LIMIT_UI, BRANCH_LIMIT))
    print("- soft warning (advisory): %d lines / %d branches" % (SOFT_LINE_LIMIT, SOFT_BRANCH_LIMIT))
    print("- function-too-long: %d" % longs)
    print("- function-too-branchy: %d" % branchy)
    print("\n### Largest functions\n")
    seen = set()
    rows = sorted(head.items(), key=lambda kv: kv[1][2], reverse=True)
    for (rule, bname, name, arity), (path, line, ln, br) in rows:
        if (bname, name, arity) in seen:
            continue
        seen.add((bname, name, arity))
        print("- `%s` `%s/%d` — %d lines / %d branches (%s:%d)" % (bname, name, arity, ln, br, path, line))
        if len(seen) >= 30:
            break
    return 0


def run_selftest():
    """Assert the tiered caps + UI-classification rule are documented in AGENTS.md, and that the
    classifier behaves on canonical examples. Keeps the script's single-source-of-truth rule from
    drifting out of sync with the human-facing policy. Exit 0 = in sync, 1 = drift."""
    repo_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__)))))
    agents_md = os.path.join(repo_root, "AGENTS.md")
    miss = 0
    try:
        with open(agents_md, "r", encoding="utf-8", errors="replace") as fh:
            doc = fh.read()
    except OSError as e:
        print("SELFTEST FAIL: cannot read AGENTS.md (%s)" % e, file=sys.stderr)
        return 1
    # The tiered caps + UI-classification rule must appear verbatim-ish in AGENTS.md.
    for needle in ("120", "200", "Ui/", "Draw"):
        if needle not in doc:
            print("SELFTEST FAIL: '%s' (tiered-cap / UI-rule token) missing from AGENTS.md" % needle,
                  file=sys.stderr)
            miss = 1
    # Classifier behaviour: path-based, name-based, and negatives.
    checks = [
        ("Source/Core/src/Ui/Foo.cpp", "HelperThing", True),     # Ui/ path
        ("Source/Core/src/Tracker/Foo.cpp", "DrawWidget", True),  # Draw-prefixed name
        ("Source/Core/src/Tracker/Foo.cpp", "renderRow", True),   # render-prefixed name
        ("Source/Core/src/Tracker/Foo.cpp", "ComputeTotals", False),  # non-UI
        ("Source/Core/src/Config/Foo.cpp", "Cls::Save", False),   # qualified non-UI
    ]
    for path, name, expect in checks:
        got = is_ui_function(path, name)
        if got != expect:
            print("SELFTEST FAIL: is_ui_function(%r, %r) = %s, expected %s"
                  % (path, name, got, expect), file=sys.stderr)
            miss = 1
    if miss:
        return 1
    print("selftest: tiered caps + UI-classification rule in sync with AGENTS.md")
    return 0


def _utf8_stdio():
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass


def main():
    _utf8_stdio()
    ap = argparse.ArgumentParser()
    ap.add_argument("--diff", metavar="REF")
    ap.add_argument("--scan-file", metavar="PATH")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--baseline-md", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    try:
        if args.selftest:
            sys.exit(run_selftest())
        if args.diff:
            sys.exit(run_diff(args.diff))
        if args.scan_file:
            sys.exit(run_scan_file(args.scan_file))
        if args.list:
            sys.exit(run_list())
        if args.baseline_md:
            sys.exit(run_baseline_md())
        sys.exit(run_report())
    except Exception as e:  # never crash-as-clean: surface as infra error (>=2)
        print("function_size_audit: ERROR: %s" % e, file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()

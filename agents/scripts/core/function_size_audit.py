#!/usr/bin/env python3
"""Function-size analyzer for the decompose-top-20-monoliths gate (Slice 0).

Walks tracked first-party C++ (Source/Core, Source/Plugins, Source/Standalone; excludes
ThirdParty + tests), extracts every function *body* via a comment/string-neutralized brace
scan, and flags functions over the line / branch caps. Two rule-ids:

  function-too-long     body span > 200 lines
  function-too-branchy  decision count > 30  (if/for/while/case/catch/&&/||/?)

Modes mirror comment_audit.py (the comment-regrowth sibling gate):

  function_size_audit.py                  # human report of all current oversized functions
  function_size_audit.py --list           # one `rule<TAB>file:line<TAB>name (NL/MB)` per violation
  function_size_audit.py --baseline-md     # deterministic markdown grandfather snapshot
  function_size_audit.py --diff <ref>      # DELTA gate: emit only functions that are NEW or
                                           #   crossed a cap vs the merge-base of <ref>
                                           #   rule<TAB>basename:line<TAB>name (NL/MB)

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

# --- caps ---------------------------------------------------------------------
LINE_LIMIT = 200
BRANCH_LIMIT = 30

RULE_LONG = "function-too-long"
RULE_BRANCHY = "function-too-branchy"

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
    """Yield (rule, line, name, arity, lines, branches) for each oversized function in `text`."""
    for fn in extract_functions(text):
        if fn.lines > LINE_LIMIT:
            yield (RULE_LONG, fn.start_line, fn.name, fn.arity, fn.lines, fn.branches)
        if fn.branches > BRANCH_LIMIT:
            yield (RULE_BRANCHY, fn.start_line, fn.name, fn.arity, fn.lines, fn.branches)


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
    return mb if (p.returncode == 0 and mb) else ref


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
    for rule in (RULE_LONG, RULE_BRANCHY):
        rows = sorted(by_rule.get(rule, []))
        print()
        print("## %s (%d entries, cap %s)" % (
            rule, len(rows), "%d lines" % LINE_LIMIT if rule == RULE_LONG else "%d branches" % BRANCH_LIMIT))
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
    print("- cap: %d lines / %d branches" % (LINE_LIMIT, BRANCH_LIMIT))
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
    args = ap.parse_args()
    try:
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

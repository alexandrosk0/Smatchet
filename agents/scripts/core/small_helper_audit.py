#!/usr/bin/env python3
"""Small-helper clone census for the gate-blind-spot-sweep Slice 2 gate.

Sibling of dup_audit.py — and deliberately NOT a replacement for it. dup_audit.py's
`MIN_CLONE_TOKENS = 70` (~8 lines of C++) is what makes the blocking DRY gate viable: dropping the
floor to ~15 would flood it with every `if (!x) return false;` in the tree and destroy the signal.
So small-helper duplication is invisible to Pillar 5 **by construction**, and the grandfathered
449-clone baseline can never surface it.

This audit asks a DIFFERENT question with a DIFFERENT tool, in the band dup_audit.py cannot enter:

    "is the same small NAMED helper re-rolled, body-for-body, in three or more TUs?"

Three narrowings make that answerable without the noise that lowering MIN_CLONE_TOKENS would cause:

  1. **Named free functions only.** The unit is a function definition — its parameter list and body
     — never an arbitrary token run. A repeated `if (!x) return false;` inside three unrelated
     functions is not a finding here; three functions that ARE that body are.
  2. **Exact normalised match.** Parameter + body token streams are normalized with dup_audit's own
     `normalize_token` (identifiers -> ID, literals -> LIT, keywords + punctuation structural), so a
     copy-then-rename still collides while a genuine structural edit diverges. No fuzzy / shingled
     matching — this reports identity, not similarity.
  3. **>= MIN_TU_SPREAD distinct TUs.** Two files sharing a helper is a pair; three or more is a
     pattern, and a pattern is what justifies a shared Core helper. The spread requirement is what
     keeps the SUB-floor band quiet enough to read.

Band: HELPER_MIN_TOKENS <= body tokens < dup_audit.MIN_CLONE_TOKENS. The upper bound is imported,
not copied, so the two audits can never drift into overlapping or leaving a gap between them.

Verdict model — ADVISORY (WARN-first), mirroring the DRY gate's pre-graduation posture
(docs/adr/0015-dry-quality-pillar-duplication-gate.md) and the sibling dead_export_audit.py.
`--check` prints `[small-helper] WARN ...` for non-baselined groups and exits 0.

Accepted blind spots (floor, not ceiling):
  * Out-of-line MEMBER definitions (`Foo::Bar(...) {`) are skipped — the plan scopes this to free
    helpers, and a member's body is bound to its class's state.
  * A helper re-rolled with a genuinely different body (e.g. an explicit `'A'..'Z'` branch instead
    of `std::tolower`) does NOT group with the std::tolower family. That is correct: they are not
    the same function, and merging them would be a behaviour change, not a de-duplication.
  * Template definitions are included, but a body differing only in a dependent type collapses to
    ID under normalization and may group — read the report, do not merge blind.

Modes mirror dup_audit.py so test-gate-selftests.sh + the workflow wiring need no special-casing:

  small_helper_audit.py                 # human report of current sub-floor helper clone groups
  small_helper_audit.py --list          # one `rule<TAB>hash<TAB>file:line<TAB>Name` per member
  small_helper_audit.py --baseline-md   # deterministic markdown grandfather snapshot
  small_helper_audit.py --check         # ADVISORY gate: `[small-helper] WARN ...` for NEW groups
  small_helper_audit.py --selftest      # assert the extractor + banding + spread invariants hold

Exit contract (so the shell wrapper can fail CLOSED on infra): 0 = clean / advisory WARN only,
1 = reserved for the post-graduation blocking verdict, >=2 = infra error (main wraps -> exit 2).

See docs/plans/gate-blind-spot-sweep.md + docs/high-integrity/small-helper-baseline.md.
"""

import argparse
import hashlib
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import comment_lib as cl
# The upper band edge + the normalization are IMPORTED, never copied: if dup_audit.py ever retunes
# MIN_CLONE_TOKENS, this audit's ceiling follows it and the two stay exactly complementary.
import dup_audit

# --- scope (KEEP IN SYNC with dup_audit.py SWEEP_ROOTS) ---------------------------------------
SWEEP_ROOTS = dup_audit.SWEEP_ROOTS
EXCLUDE_SUBSTR = dup_audit.EXCLUDE_SUBSTR
CPP_EXT = dup_audit.CPP_EXT

RULE = "small-helper"
BASELINE_PATH = "docs/high-integrity/small-helper-baseline.md"

# --- detector parameters ----------------------------------------------------------------------
# The band this audit owns: at least HELPER_MIN_TOKENS and strictly below dup_audit's floor (at or
# above it, the DRY gate already sees the clone and owns the verdict). 25 was calibrated against the
# real tree: at 20 the report gained a junk group collapsing three unrelated
# `lock_guard(g_mutex); return <member>;` accessors, because at that size the normalized body of a
# guarded getter carries no distinguishing structure. 25 drops that group and keeps every genuine
# one (the smallest real finding, the rgba-pack helper, is 26 tokens).
HELPER_MIN_TOKENS = 25
HELPER_MAX_TOKENS = dup_audit.MIN_CLONE_TOKENS
# Distinct TUs a body must span before it is a finding. Two files is a pair; three is a pattern.
MIN_TU_SPREAD = 3

# Tokens that may sit between a signature's `)` and its body `{`.
_TRAILING_SPEC = frozenset(("const", "noexcept", "override", "final", "&", "&&"))
# A token immediately before the function name that means "this `(` is a CALL / an expression", not
# a declarator. `::` is here because an out-of-line member definition is out of scope.
_NOT_A_DECLARATOR_PREFIX = frozenset((
    "(", ",", "=", "return", ".", "->", "::", "&&", "||", "!", "?", ":", "[", "{", ";", "+", "-",
    "*", "/", "%", "<", ">", "new", "delete", "throw", "case", "co_return", "sizeof",
))
# Statement keywords that take a parenthesised head and a braced body — never function definitions.
_CONTROL_HEADS = frozenset((
    "if", "for", "while", "switch", "catch", "do", "else", "namespace", "class", "struct", "union",
    "enum", "try",
))


# --- git / file enumeration -------------------------------------------------------------------

def _git(args):
    p = subprocess.run(["git"] + args, capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    if p.returncode != 0:
        raise RuntimeError("git %s failed (%d): %s" % (" ".join(args), p.returncode, p.stderr.strip()))
    return p.stdout


def _in_scope(f):
    return (f.endswith(CPP_EXT) and f.startswith(SWEEP_ROOTS)
            and not any(s in f for s in EXCLUDE_SUBSTR))


def list_files():
    files = sorted(f for f in _git(["ls-files"]).splitlines() if _in_scope(f))
    if not files:
        raise RuntimeError("no first-party C++ found under %s — scan is broken (fail closed)"
                           % ", ".join(SWEEP_ROOTS))
    return files


def _read(root, path):
    try:
        with open(os.path.join(root, path), "r", encoding="utf-8", errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


# --- function extraction ----------------------------------------------------------------------

class Helper(object):
    __slots__ = ("name", "path", "line", "body_hash", "token_count")

    def __init__(self, name, path, line, body_hash, token_count):
        self.name = name
        self.path = path
        self.line = line
        self.body_hash = body_hash
        self.token_count = token_count


def _match_backward(toks, close_idx, open_ch, close_ch):
    """Index of the `open_ch` matching the `close_ch` at close_idx, or None."""
    depth = 0
    i = close_idx
    while i >= 0:
        t = toks[i][0]
        if t == close_ch:
            depth += 1
        elif t == open_ch:
            depth -= 1
            if depth == 0:
                return i
        i -= 1
    return None


def _match_forward(toks, open_idx):
    """Index of the `}` matching the `{` at open_idx, or None."""
    depth = 0
    for i in range(open_idx, len(toks)):
        t = toks[i][0]
        if t == "{":
            depth += 1
        elif t == "}":
            depth -= 1
            if depth == 0:
                return i
    return None


def extract_helpers(text, path):
    """Every named free-function definition in `text`, as Helper records.

    Scans the literal-aware token stream for a `{` whose preceding tokens form a declarator:
    `<type...> Name ( params ) [const|noexcept|...] {`. The name's left neighbour must be a
    type-ish token — that single check is what separates a definition from `if (...) {`,
    `Foo(...)` as a call statement, and an out-of-line member definition (`::` neighbour).
    """
    toks = cl.code_tokens_with_offsets(text)
    # Precompute a 1-based line number per token offset, once.
    line_starts = [0]
    for m in re.finditer("\n", text):
        line_starts.append(m.end())

    def line_of(off):
        lo, hi = 0, len(line_starts) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if line_starts[mid] <= off:
                lo = mid
            else:
                hi = mid - 1
        return lo + 1

    out = []
    i = 0
    n = len(toks)
    while i < n:
        if toks[i][0] != "{":
            i += 1
            continue
        # Walk back over trailing specifiers to find the signature's `)`.
        j = i - 1
        while j >= 0 and toks[j][0] in _TRAILING_SPEC:
            j -= 1
        if j < 0 or toks[j][0] != ")":
            i += 1
            continue
        open_paren = _match_backward(toks, j, "(", ")")
        if open_paren is None or open_paren == 0:
            i += 1
            continue
        name_idx = open_paren - 1
        name = toks[name_idx][0]
        if not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", name) or name in _CONTROL_HEADS:
            i += 1
            continue
        if name_idx == 0:
            i += 1
            continue
        prev = toks[name_idx - 1][0]
        if prev in _NOT_A_DECLARATOR_PREFIX or prev in _CONTROL_HEADS:
            i += 1
            continue
        close_brace = _match_forward(toks, i)
        if close_brace is None:
            i += 1
            continue
        # Normalize the parameter list AND the body together: two helpers are "the same" only when
        # they take the same shape of arguments and do the same thing to them.
        params = [dup_audit.normalize_token(t) for t, _ in toks[open_paren:j + 1]]
        body = [dup_audit.normalize_token(t) for t, _ in toks[i:close_brace + 1]]
        if HELPER_MIN_TOKENS <= len(body) < HELPER_MAX_TOKENS:
            digest = hashlib.blake2b(
                (" ".join(params) + " => " + " ".join(body)).encode("utf-8"), digest_size=8
            ).hexdigest()
            out.append(Helper(name, path, line_of(toks[name_idx][1]), digest, len(body)))
        # Do NOT skip past the body: a lambda or a nested definition inside it is still a candidate
        # for the *enclosing* file, and skipping would hide helpers defined inside a namespace
        # block that this scan already stepped into.
        i += 1
    return out


# --- grouping ---------------------------------------------------------------------------------

class Group(object):
    __slots__ = ("body_hash", "members", "token_count")

    def __init__(self, body_hash, members, token_count):
        self.body_hash = body_hash
        self.members = members          # sorted [Helper]
        self.token_count = token_count

    @property
    def names(self):
        seen = []
        for m in self.members:
            if m.name not in seen:
                seen.append(m.name)
        return seen

    @property
    def tu_count(self):
        return len({m.path for m in self.members})


def find_groups(root="."):
    """Sub-floor helper bodies shared by >= MIN_TU_SPREAD distinct TUs, sorted deterministically."""
    by_hash = {}
    for f in list_files():
        for h in extract_helpers(_read(root, f), f):
            by_hash.setdefault(h.body_hash, []).append(h)

    groups = []
    for digest, members in by_hash.items():
        if len({m.path for m in members}) < MIN_TU_SPREAD:
            continue
        members.sort(key=lambda m: (m.path, m.line, m.name))
        groups.append(Group(digest, members, members[0].token_count))
    # Widest spread first — the biggest de-duplication win reads at the top.
    groups.sort(key=lambda g: (-g.tu_count, -g.token_count, g.members[0].path, g.body_hash))
    return groups


# --- baseline ---------------------------------------------------------------------------------

_BASELINE_ROW_RE = re.compile(r"^##\s+group\s+`([0-9a-f]+)`")


def read_baseline(root="."):
    """Grandfathered group hashes. A missing baseline is an EMPTY set, not an error."""
    keys = set()
    try:
        with open(os.path.join(root, BASELINE_PATH), "r", encoding="utf-8", errors="replace") as fh:
            for raw in fh:
                m = _BASELINE_ROW_RE.match(raw.strip())
                if m:
                    keys.add(m.group(1))
    except OSError:
        return set()
    return keys


def render_baseline(groups):
    out = [
        "# Small-helper clones — grandfathered baseline",
        "",
        "_Auto-generated. Do not hand-edit; run `bash agents/scripts/core/test-small-helper-audit.sh"
        " --baseline` and commit._",
        "_The gate (`small_helper_audit.py --check`) is ADVISORY: it WARNs on groups absent from this"
        " file and never blocks. Graduation to blocking is a separate decision (mirrors ADR-0015)._",
        "",
        "_Band: %d <= body tokens < %d (dup_audit.py's MIN_CLONE_TOKENS, imported). A group is a body"
        " shared by >= %d distinct TUs._"
        % (HELPER_MIN_TOKENS, HELPER_MAX_TOKENS, MIN_TU_SPREAD),
        "",
        "## Totals",
        "- groups grandfathered: %d" % len(groups),
        "- call sites across all groups: %d" % sum(len(g.members) for g in groups),
        "",
    ]
    for g in groups:
        out.append("## group `%s` — %d TUs, %d tokens, name(s): %s"
                   % (g.body_hash, g.tu_count, g.token_count,
                      ", ".join("`%s`" % n for n in g.names)))
        for m in g.members:
            out.append("- `%s:%d` — `%s`" % (m.path, m.line, m.name))
        out.append("")
    if not groups:
        out.append("_(none — no sub-floor helper body is shared by %d or more TUs)_" % MIN_TU_SPREAD)
        out.append("")
    return "\n".join(out)


# --- modes ------------------------------------------------------------------------------------

def run_report():
    groups = find_groups()
    if not groups:
        print("small_helper_audit: no sub-floor helper body shared by %d+ TUs." % MIN_TU_SPREAD)
        return 0
    print("small_helper_audit: %d helper body/bodies re-rolled across %d+ TUs "
          "(band %d..%d tokens — below dup_audit's floor)"
          % (len(groups), MIN_TU_SPREAD, HELPER_MIN_TOKENS, HELPER_MAX_TOKENS - 1))
    for g in groups:
        print("  group %s — %d TUs, %d tokens, name(s): %s"
              % (g.body_hash, g.tu_count, g.token_count, ", ".join(g.names)))
        for m in g.members:
            print("      %s:%d  %s" % (m.path, m.line, m.name))
    return 0


def run_list():
    for g in find_groups():
        for m in g.members:
            print("%s\t%s\t%s:%d\t%s" % (RULE, g.body_hash, m.path, m.line, m.name))
    return 0


def run_baseline_md():
    sys.stdout.write(render_baseline(find_groups()) + "\n")
    return 0


def run_check():
    """ADVISORY verdict: WARN on every non-baselined group, exit 0 regardless."""
    groups = find_groups()
    baseline = read_baseline()
    new = [g for g in groups if g.body_hash not in baseline]
    stale = sorted(baseline - {g.body_hash for g in groups})
    for g in new:
        print("[%s] WARN group %s — the same %d-token body is re-rolled in %d TUs as %s (advisory)"
              % (RULE, g.body_hash, g.token_count, g.tu_count, "/".join(g.names)))
        for m in g.members:
            print("    %s:%d  %s" % (m.path, m.line, m.name))
    if stale:
        print("[%s] NOTE %d baselined group(s) no longer duplicated — regenerate the baseline: %s"
              % (RULE, len(stale), ", ".join(stale)))
    if not new and not stale:
        print("[%s] PASS %d group(s), all grandfathered in %s" % (RULE, len(groups), BASELINE_PATH))
    return 0


# --- selftest ---------------------------------------------------------------------------------

_LOWER_BODY = (
    "std::string %s(std::string s) {\n"
    "    std::transform(s.begin(), s.end(), s.begin(),\n"
    "                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });\n"
    "    return s;\n"
    "}\n"
)


def run_selftest():
    miss = 0

    def fail(msg):
        print("SELFTEST FAIL: %s" % msg, file=sys.stderr)

    # --- the band is anchored to dup_audit's floor, not a copied number ---
    if HELPER_MAX_TOKENS != dup_audit.MIN_CLONE_TOKENS:
        fail("the band ceiling drifted from dup_audit.MIN_CLONE_TOKENS")
        miss = 1
    if HELPER_MIN_TOKENS >= HELPER_MAX_TOKENS:
        fail("the band is empty (min %d >= max %d)" % (HELPER_MIN_TOKENS, HELPER_MAX_TOKENS))
        miss = 1

    # --- a copy-then-RENAME collides (the whole point) ---
    a = extract_helpers(_LOWER_BODY % "ToLowerAscii", "A.cpp")
    b = extract_helpers(_LOWER_BODY % "AsciiLowerCopy", "B.cpp")
    if not a or not b:
        fail("the ASCII-lower helper was not extracted at all (a=%d b=%d)" % (len(a), len(b)))
        miss = 1
    elif a[0].body_hash != b[0].body_hash:
        fail("a copy-then-rename did NOT collide (%s vs %s)" % (a[0].body_hash, b[0].body_hash))
        miss = 1
    elif not (HELPER_MIN_TOKENS <= a[0].token_count < HELPER_MAX_TOKENS):
        fail("the ASCII-lower helper (%d tokens) fell outside the audited band"
             % a[0].token_count)
        miss = 1

    # selftest: asserts-failure — every negative below is a KNOWN-BAD input the extractor must
    # reject; a regression to naive `Name (` matching makes one of these fire.

    # --- a GENUINELY different body must NOT collide with it ---
    branchy = (
        "std::string ToLowerAscii(std::string s) {\n"
        "    std::transform(s.begin(), s.end(), s.begin(),\n"
        "                   [](char c) -> char { return (c >= 'A' && c <= 'Z') ?"
        " static_cast<char>(c + ('a' - 'A')) : c; });\n"
        "    return s;\n"
        "}\n"
    )
    c = extract_helpers(branchy, "C.cpp")
    if c and a and c[0].body_hash == a[0].body_hash:
        fail("two DIFFERENT bodies collided — normalization is over-collapsing")
        miss = 1

    # --- an `if (...) { ... }` is not a function definition ---
    control = (
        "void Outer() {\n"
        "    if (ready) {\n"
        "        DoWork(); DoWork(); DoWork(); DoWork(); DoWork(); DoWork(); DoWork(); DoWork();\n"
        "    }\n"
        "}\n"
    )
    if any(h.name == "if" for h in extract_helpers(control, "D.cpp")):
        fail("an `if (...) {` was extracted as a function definition")
        miss = 1

    # --- an out-of-line MEMBER definition is out of scope ---
    member = "void Widget::Reset(int a, int b) { %s }\n" % ("x = a; y = b; z = a; w = b; " * 4)
    if extract_helpers(member, "E.cpp"):
        fail("an out-of-line member definition was extracted as a free helper")
        miss = 1

    # --- a CALL followed by a braced init is not a definition ---
    call = "void Outer() { std::vector<int> v = Make(1, 2); for (int x : v) { Use(x); Use(x); } }\n"
    if any(h.name in ("Make", "for") for h in extract_helpers(call, "F.cpp")):
        fail("a call / range-for was extracted as a function definition")
        miss = 1

    # --- a body at or ABOVE dup_audit's floor belongs to the DRY gate, not here ---
    big = "void Big() { %s }\n" % ("Step(a); " * 40)
    if extract_helpers(big, "G.cpp"):
        fail("a body at/above MIN_CLONE_TOKENS was claimed by the sub-floor audit "
             "(it overlaps the DRY gate)")
        miss = 1

    # --- a tiny getter is below the band and must not be reported ---
    tiny = "int Get() { return value_; }\n"
    if extract_helpers(tiny, "H.cpp"):
        fail("a sub-band getter was extracted (the band floor is not being applied)")
        miss = 1

    # --- the spread rule: two TUs is a pair, not a finding ---
    pair = {}
    for path in ("A.cpp", "B.cpp"):
        for h in extract_helpers(_LOWER_BODY % "ToLower", path):
            pair.setdefault(h.body_hash, []).append(h)
    if any(len({m.path for m in ms}) >= MIN_TU_SPREAD for ms in pair.values()):
        fail("two TUs were treated as meeting the %d-TU spread requirement" % MIN_TU_SPREAD)
        miss = 1

    if miss:
        return 1
    print("selftest: extractor + banding + spread invariants hold "
          "(band %d..%d tokens, >=%d TUs; ceiling imported from dup_audit)"
          % (HELPER_MIN_TOKENS, HELPER_MAX_TOKENS - 1, MIN_TU_SPREAD))
    return 0


# --- entry point ------------------------------------------------------------------------------

def _utf8_stdio():
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass


def main():
    _utf8_stdio()
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--baseline-md", action="store_true")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    try:
        if args.selftest:
            sys.exit(run_selftest())
        if args.check:
            sys.exit(run_check())
        if args.list:
            sys.exit(run_list())
        if args.baseline_md:
            sys.exit(run_baseline_md())
        sys.exit(run_report())
    except Exception as e:  # never crash-as-clean: surface as infra error (>=2)
        print("small_helper_audit: ERROR: %s" % e, file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()

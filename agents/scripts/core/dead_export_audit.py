#!/usr/bin/env python3
"""Cross-TU dead-export detector for the gate-blind-spot-sweep Slice 1 gate.

Sibling of dup_audit.py / include_cycle_audit.py / function_size_audit.py. Answers a question no
existing gate can see: **a free function DECLARED in a public `Source/Core/include/**` header and
called by nobody.** `-Wunused-function` only sees the static-in-one-TU shape and
`check-test-list.sh` only sees orphan test files; a symbol whose declaration, definition and zero
callers are spread across three files is structurally invisible to both.

Verdict model — ADVISORY (WARN-first), mirroring the DRY gate's pre-graduation posture
(docs/adr/0015-dry-quality-pillar-duplication-gate.md). `--check` prints `[dead-export] WARN ...`
for every non-baselined finding and exits 0. Graduation to blocking is a separate decision with its
own ADR once the baseline is clean and the false-positive rate is known
(docs/plans/gate-blind-spot-sweep.md § Risks / non-goals).

Detection model — file-partition counting, NOT decl-vs-call parsing
-------------------------------------------------------------------
Classifying every textual occurrence as "declaration" vs "call" needs a real C++ parser; a regex
that tries it mis-reads `return Foo(x)` as a definition. So this audit never classifies an
occurrence. It partitions FILES instead, and a symbol is reported dead only when ALL of:

  1. its declaring header H holds exactly as many occurrences as the parser found declaration /
     definition sites for it in H (so an `inline` helper called by a sibling inline in the same
     header is ALIVE — extra occurrences);
  2. no OTHER header in scope mentions it at all;
  3. across every non-header file in scope the total occurrence count equals the number of
     out-of-line definitions the symbol still needs: ZERO when the header already carries an
     `inline` body, otherwise exactly ONE (the single out-of-line definition, in whichever .cpp
     actually carries it — the audit never needs to know which).

Any caller anywhere adds an occurrence and lifts the symbol out of the report. That makes false
POSITIVES (the dangerous direction — deleting live code) require a caller that is textually absent,
which C++ cannot produce. False negatives are accepted freely; see § Accepted blind spots.

Occurrence counting runs over `comment_lib.strip_comments()` output — comments are removed (a
comment never calls anything, and a stale `// TODO: use Foo` must not mask a dead Foo) but STRING
LITERALS ARE KEPT. That asymmetry is deliberate: a symbol reachable only through a Lua binding
table or an MCP command-name string is the detector's worst false-positive shape, so a name that
appears inside any string literal counts as a live reference.

`SMATCHET_WITH_*`-guarded call sites need no special handling — the audit is textual and never
evaluates the preprocessor, so a call inside `#if SMATCHET_WITH_LUA` is an ordinary occurrence and
keeps its callee alive.

Accepted blind spots (floor, not ceiling — a partial detector that runs beats a complete one that
does not; recorded in the plan's § Risks):
  * MEMBER functions are skipped outright — virtual dispatch defeats textual counting.
  * Dead classes / dead types are out of scope.
  * Only CAPITALISED names are considered (the repo's free-function convention); an ALL_CAPS name
    is treated as a macro and skipped.
  * A symbol whose name collides with an unrelated helper elsewhere in the tree reads as alive.

Scope: declarations from `Source/Core/include/**` headers; references counted across `Source/` and
`tests/` (every first-party TU, plugin and standalone included), excluding ThirdParty + generated.

Modes mirror dup_audit.py so test-gate-selftests.sh + the workflow wiring need no special-casing:

  dead_export_audit.py                 # human report of current dead exports
  dead_export_audit.py --list          # one `rule<TAB>header:line<TAB>symbol` per finding
  dead_export_audit.py --baseline-md   # deterministic markdown grandfather snapshot
  dead_export_audit.py --check         # ADVISORY gate: `[dead-export] WARN ...` for NEW findings
  dead_export_audit.py --selftest      # assert the parser + partition invariants hold

Exit contract (so the shell wrapper can fail CLOSED on infra): 0 = clean / advisory WARN only,
1 = reserved for the post-graduation blocking verdict, >=2 = infra error (main wraps -> exit 2).

See docs/plans/gate-blind-spot-sweep.md + docs/high-integrity/dead-export-baseline.md.
"""

import argparse
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import comment_lib as cl

# --- scope ------------------------------------------------------------------------------------
# Declarations are harvested from the PUBLIC Core header surface only — that is the "export" this
# gate is about. A helper declared in a src/-local header is a TU-cluster detail, not an export.
DECL_ROOTS = ("Source/Core/include/",)
# References are counted across every first-party tree: a Core export can legitimately be called
# from a plugin, the standalone shell, the Unreal bridge, or a test.
REF_ROOTS = ("Source/", "tests/")
# KEEP IN SYNC with dup_audit.py / include_cycle_audit.py EXCLUDE_SUBSTR. The Unreal packaging dir
# (Source/UnrealPlugins/**/ThirdParty/Smatchet/include/) is a COPIED build artifact of a handful of
# Core headers, regenerated by SmatchetPackageUnrealLibs_DX12 — counting its stale duplicate of a
# declaration as a live reference would mask every export it mirrors.
EXCLUDE_SUBSTR = ("/ThirdParty/", "ThirdParty/", ".generated.", "/generated/", "/Generated/")
HEADER_EXT = (".h", ".hpp", ".inl")
# Reference-bearing files: C++ TUs + headers, plus the Lua and C# (Unreal Build.cs) surfaces where
# a Core symbol name can legitimately appear.
REF_EXT = HEADER_EXT + (".cpp", ".cc", ".cxx", ".lua", ".cs")

RULE = "dead-export"

BASELINE_PATH = "docs/high-integrity/dead-export-baseline.md"

# A candidate export name: capitalised, and carrying at least one lowercase letter so an ALL_CAPS
# macro invocation (`SMATCHET_DECLARE_FOO(x);`) is never mistaken for a function declaration.
_NAME_RE = re.compile(r"^[A-Z][A-Za-z0-9_]*$")
_HAS_LOWER_RE = re.compile(r"[a-z]")

# The identifier immediately preceding the first `(` of a namespace-scope statement — the function
# name in `std::string Foo(const char* n)`, `void A::B(int)`, `inline bool Bar()`.
_CALLABLE_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\s*\(")

# Statement openers that mean "this is not a function signature" even though a `(` follows.
_NON_DECL_HEADS = frozenset((
    "return", "if", "while", "for", "switch", "catch", "throw", "new", "delete", "sizeof",
    "static_assert", "using", "typedef", "namespace", "else", "do", "case", "decltype",
    "alignas", "noexcept", "extern",
))


# --- git / file enumeration -------------------------------------------------------------------

def _git(args):
    p = subprocess.run(["git"] + args, capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    if p.returncode != 0:
        raise RuntimeError("git %s failed (%d): %s" % (" ".join(args), p.returncode, p.stderr.strip()))
    return p.stdout


def _excluded(path):
    return any(s in path for s in EXCLUDE_SUBSTR)


def _is_decl_header(path):
    return (path.startswith(DECL_ROOTS) and path.endswith(HEADER_EXT)
            and not _excluded(path))


def _is_ref_file(path):
    return (path.startswith(REF_ROOTS) and path.endswith(REF_EXT)
            and not _excluded(path))


def list_tracked():
    """Every git-tracked path, POSIX-separated. Tracked-only so a stale build artifact or an
    untracked scratch copy of a header can never mask a dead export."""
    return [f for f in _git(["ls-files"]).splitlines() if f]


def _read(root, path):
    try:
        with open(os.path.join(root, path), "r", encoding="utf-8", errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


# --- header declaration parser ----------------------------------------------------------------

def _scope_kind(pending):
    """Classify the text preceding a `{` into the scope it opens.

    Only three answers matter: `ns` (a namespace, which does NOT hide a free function), `record`
    (class/struct/union/enum — everything inside is a member, skipped), and `other` (a function
    body, an initialiser list, an extern "C" block, a control block)."""
    if re.search(r"\bnamespace\b", pending):
        return "ns"
    if re.search(r"\b(class|struct|union|enum)\b", pending):
        return "record"
    if re.search(r'\bextern\s*"C(\+\+)?"', pending):
        # `extern "C" {` does not introduce a scope for our purposes — a free function inside it
        # is still a namespace-scope export.
        return "ns"
    return "other"


def parse_declarations(text):
    """Parse free-function declaration/definition sites out of one header.

    Returns {name: [(line, is_definition), ...]} — every namespace-scope site whose callable name
    passes the capitalised-name filter, tagged with whether the site is a `{`-opened INLINE
    DEFINITION (True) or a `;`-terminated pure DECLARATION (False). That tag is what tells the
    partition rule how many out-of-line definitions to expect in the .cpp layer: a header that
    already carries the body expects ZERO, so any non-header occurrence is a caller.

    Walks characters, maintaining a stack of open scopes and the text accumulated since the last
    `{` / `}` / `;`. A namespace-scope statement is recognised when that pending text terminates —
    which is exactly where a declaration ends (`;`) or a definition body begins (`{`). Anything
    inside a `record` or `other` scope is a member / local and never reported.
    """
    text = cl.strip_comments(text)
    decls = {}
    stack = []            # scope kinds, outermost first
    pending = []          # chars since the last scope/statement boundary
    pending_line = 1      # 1-based line where `pending` started
    line = 1
    depth_of_first_non_ns = None  # None while every open scope is a namespace

    def at_namespace_scope():
        return depth_of_first_non_ns is None

    def take_pending():
        return "".join(pending).strip()

    def record(stmt, at_line, is_definition):
        if not at_namespace_scope():
            return
        name = _callable_name(stmt)
        if name is None:
            return
        decls.setdefault(name, []).append((at_line, is_definition))

    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c.isspace():
            if c == "\n":
                line += 1
            if pending:
                pending.append(c)
            else:
                # Keep `pending_line` pinned to the first non-space char of the statement, so a
                # signature spread over several lines reports the line it starts on.
                pending_line = line
            i += 1
            continue
        if c == "{":
            stmt = take_pending()
            kind = _scope_kind(stmt)
            if kind == "other" and at_namespace_scope():
                # A namespace-scope `{` that is not a namespace or a record opens a function body
                # — so `stmt` is that function's signature.
                record(stmt, pending_line, True)
            stack.append(kind)
            if kind != "ns" and depth_of_first_non_ns is None:
                depth_of_first_non_ns = len(stack)
            pending = []
            pending_line = line
            i += 1
            continue
        if c == "}":
            if stack:
                stack.pop()
                if depth_of_first_non_ns is not None and len(stack) < depth_of_first_non_ns:
                    depth_of_first_non_ns = None
            pending = []
            pending_line = line
            i += 1
            continue
        if c == ";":
            record(take_pending(), pending_line, False)
            pending = []
            pending_line = line
            i += 1
            continue
        pending.append(c)
        i += 1

    return decls


def _callable_name(stmt):
    """The declared free-function name in a namespace-scope statement, or None.

    `std::string Foo(const char*)` -> "Foo"; `constexpr int kX = 3` -> None (no `(`);
    `return Bar(x)` -> None (statement head is a keyword); `void (*Cb)(int)` -> None (the name
    before the first `(` is the keyword `void`, and `Cb` is a variable anyway)."""
    if not stmt or "(" not in stmt:
        return None
    # `= something(...)` is an initialiser, never a declaration of the callable it names.
    head = stmt.split("(", 1)[0]
    if "=" in head:
        return None
    m = _CALLABLE_RE.search(stmt)
    if not m:
        return None
    name = m.group(1)
    if not _NAME_RE.match(name) or not _HAS_LOWER_RE.search(name):
        return None
    before = stmt[:m.start(1)].strip()
    if not before:
        # No return type — a macro invocation, a constructor, or a bare call statement.
        return None
    first_word = re.match(r"[A-Za-z_][A-Za-z0-9_]*", before)
    if first_word and first_word.group(0) in _NON_DECL_HEADS:
        return None
    if before.endswith((".", "->", ",")):
        return None
    # `A::Foo(...)` at namespace scope is an out-of-line MEMBER definition — not a free export.
    if re.search(r"[A-Za-z0-9_>]\s*::\s*$", before):
        return None
    return name


# --- occurrence counting ----------------------------------------------------------------------

def _count_occurrences(text_stripped, names):
    """Occurrences of each name in already-comment-stripped text, keyed by name."""
    counts = {}
    for m in re.finditer(r"[A-Za-z_][A-Za-z0-9_]*", text_stripped):
        tok = m.group(0)
        if tok in names:
            counts[tok] = counts.get(tok, 0) + 1
    return counts


class Finding(object):
    __slots__ = ("name", "header", "line")

    def __init__(self, name, header, line):
        self.name = name
        self.header = header
        self.line = line

    @property
    def key(self):
        return "%s::%s" % (self.header, self.name)


def find_dead_exports(root="."):
    """The audit proper. Returns a sorted list of Finding."""
    tracked = list_tracked()
    decl_headers = sorted(f for f in tracked if _is_decl_header(f))
    ref_files = sorted(f for f in tracked if _is_ref_file(f))
    if not decl_headers:
        raise RuntimeError("no headers found under %s — scan is broken (fail closed)"
                           % ", ".join(DECL_ROOTS))

    # name -> {header: [(line, is_definition), ...]}. A name declared in two headers is ambiguous;
    # drop it (a textual audit cannot tell the two apart, and reporting either risks a false
    # positive).
    decl_sites = {}
    for h in decl_headers:
        for name, sites in parse_declarations(_read(root, h)).items():
            decl_sites.setdefault(name, {}).setdefault(h, []).extend(sites)

    candidates = {n: sites for n, sites in decl_sites.items() if len(sites) == 1}
    if not candidates:
        return []
    names = set(candidates)

    # header path -> {name: count}; and the running total across non-header files.
    header_counts = {}
    nonheader_total = {n: 0 for n in names}
    for f in ref_files:
        stripped = cl.strip_comments(_read(root, f))
        counts = _count_occurrences(stripped, names)
        if not counts:
            continue
        if f.endswith(HEADER_EXT):
            header_counts[f] = counts
        else:
            for n, c in counts.items():
                nonheader_total[n] += c

    findings = []
    for name, sites in candidates.items():
        header, entries = next(iter(sites.items()))
        # (1) the declaring header must hold NOTHING but the declaration/definition sites.
        if header_counts.get(header, {}).get(name, 0) != len(entries):
            continue
        # (2) no other header may mention it.
        if any(name in c for h, c in header_counts.items() if h != header):
            continue
        # (3) the .cpp layer must hold exactly the definitions the header still owes and no more.
        # A header that already carries an `inline` body owes ZERO, so a single non-header
        # occurrence there is a CALLER, not the definition.
        # `>` not `!=` deliberately: a count BELOW the expectation is a declaration that was never
        # defined anywhere — dead in the strongest sense (it would not even link if called).
        expected_nonheader = 0 if any(is_def for _, is_def in entries) else 1
        if nonheader_total[name] > expected_nonheader:
            continue
        findings.append(Finding(name, header, min(ln for ln, _ in entries)))

    findings.sort(key=lambda f: (f.header, f.line, f.name))
    return findings


# --- baseline ---------------------------------------------------------------------------------

_BASELINE_ROW_RE = re.compile(r"^-\s+`([^`]+):(\d+)`\s+—\s+`([A-Za-z_][A-Za-z0-9_]*)`")


def read_baseline(root="."):
    """The grandfathered key set (`header::Name`). A missing baseline is an EMPTY set, not an
    error — a fresh checkout of the gate then WARNs about everything, which is the honest
    advisory answer."""
    path = os.path.join(root, BASELINE_PATH)
    keys = set()
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            for raw in fh:
                m = _BASELINE_ROW_RE.match(raw.strip())
                if m:
                    keys.add("%s::%s" % (m.group(1), m.group(3)))
    except OSError:
        return set()
    return keys


def render_baseline(findings):
    out = [
        "# Dead exports — grandfathered baseline",
        "",
        "_Auto-generated. Do not hand-edit; run `bash agents/scripts/core/test-dead-export-audit.sh"
        " --baseline` and commit._",
        "_The gate (`dead_export_audit.py --check`) is ADVISORY: it WARNs on findings absent from"
        " this file and never blocks. Graduation to blocking is a separate decision (mirrors"
        " ADR-0015)._",
        "",
        "## %s (%d symbol%s)" % (RULE, len(findings), "" if len(findings) == 1 else "s"),
    ]
    if findings:
        for f in findings:
            out.append("- `%s:%d` — `%s`" % (f.header, f.line, f.name))
    else:
        out.append("- _(none — the public Core header surface has no textually-dead free export)_")
    out += ["", "## Totals", "- dead exports grandfathered: %d" % len(findings), ""]
    return "\n".join(out)


# --- modes ------------------------------------------------------------------------------------

def run_report():
    findings = find_dead_exports()
    if not findings:
        print("dead_export_audit: no dead exports in the public Source/Core/include surface.")
        return 0
    print("dead_export_audit: %d free export(s) declared in Source/Core/include/** with no caller"
          % len(findings))
    print("  (advisory — see the module docstring for the accepted blind spots)")
    for f in findings:
        print("  %s:%d  %s" % (f.header, f.line, f.name))
    return 0


def run_list():
    for f in find_dead_exports():
        print("%s\t%s:%d\t%s" % (RULE, f.header, f.line, f.name))
    return 0


def run_baseline_md():
    sys.stdout.write(render_baseline(find_dead_exports()) + "\n")
    return 0


def run_check():
    """ADVISORY verdict: WARN on every non-baselined finding, exit 0 regardless."""
    findings = find_dead_exports()
    baseline = read_baseline()
    new = [f for f in findings if f.key not in baseline]
    stale = sorted(baseline - {f.key for f in findings})
    for f in new:
        print("[%s] WARN %s:%d %s — declared in a public Core header, no caller in Source/ or "
              "tests/ (advisory)" % (RULE, f.header, f.line, f.name))
    if stale:
        print("[%s] NOTE %d baselined symbol(s) no longer dead (deleted or now called) — "
              "regenerate the baseline: %s" % (RULE, len(stale), ", ".join(stale)))
    if not new and not stale:
        print("[%s] PASS %d finding(s), all grandfathered in %s"
              % (RULE, len(findings), BASELINE_PATH))
    return 0


# --- selftest ---------------------------------------------------------------------------------

def _selftest_decls(text):
    """parse_declarations() reduced to {name: [line, ...]} for the assertions below."""
    return {k: sorted(ln for ln, _ in v) for k, v in parse_declarations(text).items()}


def run_selftest():
    miss = 0

    def fail(msg):
        print("SELFTEST FAIL: %s" % msg, file=sys.stderr)

    # --- the parser finds a namespace-scope declaration and its inline sibling ---
    hdr = (
        "#pragma once\n"
        "namespace Foo {\n"
        "std::string DeadOne(const char* name);\n"
        "inline int LiveTwo(int x) { return x + 1; }\n"
        "} // namespace Foo\n"
    )
    d = _selftest_decls(hdr)
    if d.get("DeadOne") != [3] or d.get("LiveTwo") != [4]:
        fail("namespace-scope declarations not parsed (got %r)" % d)
        miss = 1

    # --- the inline-vs-declaration tag drives the .cpp-layer expectation ---
    # This is the false-positive class the first cut of the detector shipped with: an `inline`
    # helper defined in the header owes ZERO out-of-line definitions, so its ONE occurrence in a
    # .cpp is a CALLER (ExtensionFromMime / SanitizeForSpreadsheet / BoolArg were all wrongly
    # flagged until the tag existed). A pure `;` declaration still owes one.
    tags = parse_declarations(hdr)
    if [is_def for _, is_def in tags.get("DeadOne", [])] != [False]:
        fail("a `;`-terminated declaration was tagged as an inline definition (got %r)" % tags)
        miss = 1
    if [is_def for _, is_def in tags.get("LiveTwo", [])] != [True]:
        fail("an inline header definition was tagged as a bare declaration (got %r)" % tags)
        miss = 1

    # selftest: asserts-failure — every negative below feeds the parser a KNOWN-BAD shape that a
    # naive regex mis-reads as a declaration; a regression makes one of these fire.

    # --- a MEMBER function is not an export ---
    member = (
        "class Widget {\n"
        "public:\n"
        "    void MemberFn(int x);\n"
        "    static std::string StaticMember();\n"
        "};\n"
    )
    d = _selftest_decls(member)
    if "MemberFn" in d or "StaticMember" in d:
        fail("a class member was reported as a free export (got %r)" % d)
        miss = 1

    # --- an out-of-line member DEFINITION is not a free export ---
    outline = "void Widget::MemberFn(int x) { Helper(x); }\n"
    if "MemberFn" in _selftest_decls(outline):
        fail("an out-of-line member definition was reported as a free export")
        miss = 1

    # --- statements that merely CALL something are not declarations ---
    calls = (
        "namespace N {\n"
        "inline int Caller() {\n"
        "    return Callee(1);\n"
        "}\n"
        "}\n"
    )
    d = _selftest_decls(calls)
    if "Callee" in d:
        fail("a `return Callee(1);` call was parsed as a declaration (got %r)" % d)
        miss = 1
    if d.get("Caller") != [2]:
        fail("the enclosing inline definition was not parsed (got %r)" % d)
        miss = 1

    # --- an ALL_CAPS macro invocation is not a declaration ---
    macro = "SMATCHET_DECLARE_THING(Widget);\n"
    if _selftest_decls(macro):
        fail("an ALL_CAPS macro invocation was parsed as a declaration")
        miss = 1

    # --- an initialiser is not a declaration ---
    init = "const int kValue = ComputeDefault();\n"
    if "ComputeDefault" in _selftest_decls(init):
        fail("an initialiser call was parsed as a declaration")
        miss = 1

    # --- a comment mentioning a symbol does NOT keep it alive ---
    commented = 'std::string Ghost();\n// TODO: call Ghost() one day\n'
    stripped = cl.strip_comments(commented)
    if _count_occurrences(stripped, {"Ghost"}).get("Ghost") != 1:
        fail("a commented-out mention was counted as a live reference")
        miss = 1

    # --- a STRING LITERAL mentioning a symbol DOES keep it alive (Lua/MCP dispatch guard) ---
    dispatched = 'std::string Ghost();\nstatic const char* kName = "Ghost";\n'
    stripped = cl.strip_comments(dispatched)
    if _count_occurrences(stripped, {"Ghost"}).get("Ghost") != 2:
        fail("a string-literal mention was NOT counted as a reference (Lua/MCP dispatch guard)")
        miss = 1

    # --- the partition rule: an inline helper called by a sibling in the SAME header is alive ---
    sibling = (
        "inline std::string Trim(std::string s) { return s; }\n"
        "inline std::string Clean(std::string s) { return Trim(s); }\n"
    )
    d = _selftest_decls(sibling)
    occ = _count_occurrences(cl.strip_comments(sibling), set(d))
    if occ.get("Trim") == len(d.get("Trim", [])):
        fail("a header-internal caller did not lift the callee's occurrence count")
        miss = 1

    if miss:
        return 1
    print("selftest: parser + partition invariants hold "
          "(members skipped · calls not declarations · comments dead / literals live)")
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
        print("dead_export_audit: ERROR: %s" % e, file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()

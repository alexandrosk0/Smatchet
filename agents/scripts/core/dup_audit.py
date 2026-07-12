#!/usr/bin/env python3
"""Duplication (copy-paste clone) analyzer for the DRY Quality Pillar delta-gate.

Sibling of function_size_audit.py / comment_audit.py: walks tracked first-party C++
(Source/Core, Source/Plugins, Source/Standalone; excludes ThirdParty + tests + generated),
token-normalizes each file, and detects **copy-paste clones** — maximal runs of identical
normalized tokens that recur across files. Reuses comment_lib's literal-aware C++ tokenizer so
the clone signal survives whitespace / comment / clang-format churn; an extra normalization pass
maps identifiers -> `ID` and literals -> `LIT` (keywords + punctuation stay structural), so a
copy-then-rename is still caught. This is the DRY gate's detector — NOT a structural-similarity
or semantic-dup tool; it flags literal copy-paste only (see docs/adr/0015 + the plan's
double-edged-DRY guardrail).

Verdict model — BLOCKING (graduated 2026-06-21; calibration complete per ADR-0015). `--diff`
emits `[dup] FAIL ...` lines for NEW cross-file copy-paste clones and exits 1, blocking the
merge. The delta-grandfather machinery ensures only genuinely-NEW clones (duplicated at HEAD but
not at the merge-base) fire; all pre-existing duplication is grandfathered. A clean tree exits 0.

Delta semantics (grandfathering): a clone is keyed by the content-hash of its normalized token
run. All duplication that already exists at the merge-base is grandfathered (its content-hashes
populate the base set); a clone WARNs only when its normalized content is duplicated at HEAD but
was not duplicated at base — i.e. a genuinely new copy-paste. A `// SMATCHET_DEVIATION(rule=
duplication; ...)` on the nearest non-blank line above either clone occurrence suppresses it
(cheap exemption — the plan prefers an exemption over abstracting across unrelated contexts).

Modes mirror function_size_audit.py:

  dup_audit.py                       # human report of current cross-file clones
  dup_audit.py --list                # one `rule<TAB>fileA:lineA<TAB>fileB:lineB (Ntok)` per clone
  dup_audit.py --baseline-md         # deterministic markdown grandfather snapshot
  dup_audit.py --diff <ref>          # DELTA gate: BLOCKING `[dup] FAIL ...` for NEW clones (exit 1)
  dup_audit.py --scan-file <path>    # git-free single-file INTRA-file clone scan (bats harness)
  dup_audit.py --selftest            # assert normalization + threshold invariants hold

Scope note: `--diff` is **cross-file primary** (intra-file clones overlap the function-size gate,
so they are off there). `--scan-file` is a self-contained intra-file diagnostic + the unit-test
seam for the shingle/normalize/extend core.

Exit contract (test-lint-rules.sh fails CLOSED on this): 0 = clean / no NEW clones,
1 = blocking violation (>=1 NEW non-exempt clone), >=2 = infra error.

See docs/plans/shipped/dry-pillar-dup-gate.md + docs/adr/0015-dry-quality-pillar-duplication-gate.md.
"""

import argparse
import hashlib
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import comment_lib as cl

# --- scope (KEEP IN SYNC with comment_audit.py / function_size_audit.py SWEEP_ROOTS) ----------
SWEEP_ROOTS = ("Source/Core/", "Source/Plugins/", "Source/Standalone/")
# Excluded path substrings: vendored (ThirdParty) + generated code. tests/ is already out of scope
# because it is not under SWEEP_ROOTS; intentional test duplication therefore never reaches here.
EXCLUDE_SUBSTR = ("/ThirdParty/", "ThirdParty/", ".generated.", "/generated/", "/Generated/")
CPP_EXT = (".cpp", ".h", ".hpp", ".cc", ".cxx")

RULE_DUP = "duplication"

# --- detector parameters (Moderate sensitivity; tunable during calibration) -------------------
# A clone must be at least MIN_CLONE_TOKENS normalized tokens (~8 lines of C++ ~= 70 tokens).
# SHINGLE_K is the k-gram seed size (< MIN so a clone spans several shingles); WINNOW_W is the
# winnowing window that keeps the fingerprint index sparse. A fingerprint occurring in more than
# MAX_FP_OCCURRENCES distinct spots is treated as ubiquitous boilerplate (a common idiom, not a
# copy-paste) and skipped — a pragmatic noise guard for ImGui-heavy code.
MIN_CLONE_TOKENS = 70
SHINGLE_K = 12
WINNOW_W = 8
MAX_FP_OCCURRENCES = 40

# C++ keywords stay structural under normalization (only identifiers collapse to ID). Numeric /
# string / char / raw-string literals collapse to LIT. Keeping this list small + explicit; an
# unknown alnum word is treated as an identifier (-> ID), which is the safe default for clone
# detection (a renamed type/var must still collapse).
CPP_KEYWORDS = frozenset((
    "alignas", "alignof", "and", "asm", "auto", "bool", "break", "case", "catch", "char",
    "char16_t", "char32_t", "class", "const", "constexpr", "const_cast", "continue", "decltype",
    "default", "delete", "do", "double", "dynamic_cast", "else", "enum", "explicit", "export",
    "extern", "false", "float", "for", "friend", "goto", "if", "inline", "int", "long", "mutable",
    "namespace", "new", "noexcept", "not", "nullptr", "operator", "or", "override", "private",
    "protected", "public", "register", "reinterpret_cast", "return", "short", "signed", "sizeof",
    "static", "static_assert", "static_cast", "struct", "switch", "template", "this", "thread_local",
    "throw", "true", "try", "typedef", "typeid", "typename", "union", "unsigned", "using", "virtual",
    "void", "volatile", "wchar_t", "while", "final",
))

_IDENT_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
_NUM_RE = re.compile(r"^[0-9]")


def normalize_token(tok):
    """Map one code_tokens() token to its normalized form: keywords + punctuation pass through;
    identifiers -> 'ID'; numeric / string / char / raw-string literals -> 'LIT'. This is what makes
    a copy-then-rename collapse to an identical stream while real structural edits diverge."""
    if not tok:
        return tok
    c0 = tok[0]
    if c0 == '"' or c0 == "'":
        return "LIT"  # string / char literal
    # raw string R"delim(...)delim" — code_tokens captures it whole, first char is the prefix letter
    if (c0 in "RLuU") and ('"' in tok):
        return "LIT"
    if _NUM_RE.match(tok):
        return "LIT"  # numeric literal (code_tokens splits floats on '.', each part is still LIT)
    if _IDENT_RE.match(tok):
        return tok if tok in CPP_KEYWORDS else "ID"
    return tok  # punctuation / operator char (code_tokens emits these one char at a time)


def _tokens_with_lines(text):
    """Literal-aware C++ tokens with their 1-based line numbers: [(token, line), ...].

    Uses comment_lib.code_tokens_with_offsets, which returns each token's REAL start offset in the
    source, so line numbers are exact. (The earlier `text.find(tok, cursor)` approach could re-match
    a token's text inside an intervening comment and shift the reported line — fixed here by
    consuming the tokenizer's own offsets instead of re-searching the raw text.)"""
    out = []
    prev_off = 0
    line = 1
    for tok, off in cl.code_tokens_with_offsets(text):
        line += text.count("\n", prev_off, off)
        out.append((tok, line))
        prev_off = off
    return out


def normalized_stream(text):
    """Return (norm_tokens, lines): parallel lists of normalized tokens and their source lines."""
    norm = []
    lines = []
    for tok, ln in _tokens_with_lines(text):
        norm.append(normalize_token(tok))
        lines.append(ln)
    return norm, lines


# --- shingling + winnowing ---------------------------------------------------------------------

def _shingle_hashes(norm):
    """Rolling hashes of every SHINGLE_K-gram in `norm`. Returns a list aligned so element i is the
    hash of norm[i:i+SHINGLE_K]; length = max(0, len(norm)-SHINGLE_K+1)."""
    n = len(norm)
    if n < SHINGLE_K:
        return []
    BASE = 1000003
    MOD = (1 << 61) - 1
    # Per-token hash (stable across runs: hash of the token string's utf-8 bytes via a simple FNV).
    th = [(_fnv(t) % MOD) for t in norm]
    hashes = []
    h = 0
    high = pow(BASE, SHINGLE_K - 1, MOD)
    for i in range(SHINGLE_K):
        h = (h * BASE + th[i]) % MOD
    hashes.append(h)
    for i in range(1, n - SHINGLE_K + 1):
        h = (h - th[i - 1] * high) % MOD
        h = (h * BASE + th[i + SHINGLE_K - 1]) % MOD
        hashes.append(h)
    return hashes


def _fnv(s):
    h = 1469598103934665603
    for b in s.encode("utf-8"):
        h ^= b
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def _winnow(hashes):
    """Winnowing: from the k-gram hash list, select the minimum hash in each WINNOW_W-wide window
    (rightmost on ties). Returns a set of selected positions — the sparse fingerprint anchors.
    Guarantees any shared run longer than WINNOW_W+SHINGLE_K-1 tokens shares >=1 fingerprint."""
    selected = set()
    n = len(hashes)
    if n == 0:
        return selected
    if n <= WINNOW_W:
        selected.add(min(range(n), key=lambda i: (hashes[i], -i)))
        return selected
    prev_min = -1
    for w in range(0, n - WINNOW_W + 1):
        window = range(w, w + WINNOW_W)
        m = min(window, key=lambda i: (hashes[i], -i))
        if m != prev_min:
            selected.add(m)
            prev_min = m
    return selected


# --- clone detection ---------------------------------------------------------------------------

class Clone(object):
    __slots__ = ("content_hash", "ntokens", "locations")

    def __init__(self, content_hash, ntokens, locations):
        self.content_hash = content_hash
        self.ntokens = ntokens
        self.locations = locations  # sorted list of (path, line)


def _content_hash(norm_slice):
    return hashlib.sha1((" ".join(norm_slice)).encode("utf-8")).hexdigest()[:16]


def find_clones(streams, allow_intra=False):
    """Detect copy-paste clones across `streams` (dict path -> (norm_tokens, lines)).

    Builds a winnowed fingerprint index, seeds candidate matches from fingerprints shared by >=2
    distinct positions, extends each seed by REAL normalized-token equality (so hash collisions can
    never fabricate a clone), keeps runs >= MIN_CLONE_TOKENS, and dedups by (content-hash, location
    set). With allow_intra=True a file may match itself at a non-overlapping offset (intra-file)."""
    # fingerprint hash -> list of (path, pos)
    index = {}
    norm_by_path = {}
    for path, (norm, _lines) in streams.items():
        norm_by_path[path] = norm
        hashes = _shingle_hashes(norm)
        for pos in _winnow(hashes):
            index.setdefault(hashes[pos], []).append((path, pos))

    seen_pairs = set()
    clones_by_key = {}
    for fp, occ in index.items():
        if len(occ) < 2 or len(occ) > MAX_FP_OCCURRENCES:
            continue
        for a in range(len(occ)):
            for b in range(a + 1, len(occ)):
                pa, ia = occ[a]
                pb, ib = occ[b]
                if pa == pb and not allow_intra:
                    continue
                key = (pa, ia, pb, ib)
                if key in seen_pairs:
                    continue
                seen_pairs.add(key)
                clone = _extend(norm_by_path, streams, pa, ia, pb, ib)
                if clone is None:
                    continue
                # Dedup by content + the involved location set (overlapping seeds collapse).
                ckey = (clone.content_hash, tuple(clone.locations))
                if ckey not in clones_by_key:
                    clones_by_key[ckey] = clone
    return list(clones_by_key.values())


def _extend(norm_by_path, streams, pa, ia, pb, ib):
    """Extend a seed match (pa@ia, pb@ib) maximally by normalized-token equality; return a Clone or
    None if the resulting run is shorter than MIN_CLONE_TOKENS or the two ranges overlap."""
    na = norm_by_path[pa]
    nb = norm_by_path[pb]
    # Extend forward.
    end = 0
    while ia + end < len(na) and ib + end < len(nb) and na[ia + end] == nb[ib + end]:
        end += 1
    # Extend backward.
    back = 0
    while ia - back - 1 >= 0 and ib - back - 1 >= 0 and na[ia - back - 1] == nb[ib - back - 1]:
        back += 1
    sa, ea = ia - back, ia + end
    sb, eb = ib - back, ib + end
    ntok = ea - sa
    if ntok < MIN_CLONE_TOKENS:
        return None
    if pa == pb and not (eb <= sa or ea <= sb):
        return None  # overlapping intra-file ranges are the same text, not a clone
    ch = _content_hash(na[sa:ea])
    la, la_end = streams[pa][1][sa], streams[pa][1][ea - 1]
    lb, lb_end = streams[pb][1][sb], streams[pb][1][eb - 1]
    # (path, start_line, end_line) per occurrence — the span lets an exemption marker placed
    # anywhere on/above the cloned block suppress it, even when token-run extension drifts the
    # reported start above the human-meaningful boundary.
    locations = sorted({(pa, la, la_end), (pb, lb, lb_end)})
    return Clone(ch, ntok, locations)


# --- git plumbing (UTF-8 forced — French locale strings / smart quotes; the #768 lesson) -------

def _git(args):
    p = subprocess.run(["git"] + args, capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    if p.returncode != 0:
        raise RuntimeError("git %s failed (%d): %s" % (" ".join(args), p.returncode, p.stderr.strip()))
    return p.stdout


def _git_ok(args):
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


def streams_head():
    out = {}
    for f in list_head_files():
        text = _read_head(f)
        if text:
            out[f] = normalized_stream(text)
    return out


def streams_ref(ref):
    out = {}
    for f in list_ref_files(ref):
        text = _git_ok(["show", "%s:%s" % (ref, f)])
        if text:
            out[f] = normalized_stream(text)
    return out


# --- delta gate --------------------------------------------------------------------------------

def _merge_base_or_ref(ref):
    p = subprocess.run(["git", "merge-base", ref, "HEAD"], capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    mb = p.stdout.strip()
    if p.returncode == 0 and mb:
        return mb
    sys.stderr.write("dup_audit: WARN: `git merge-base %s HEAD` did not resolve (shallow clone?) "
                     "— falling back to %s tip; delta may false-flag.\n" % (ref, ref))
    return ref


def _has_dup_deviation(line):
    if "SMATCHET_DEVIATION" not in line:
        return False
    m = re.search(r"rule=([A-Za-z0-9_,-]+)", line)
    return bool(m) and RULE_DUP in [r.strip() for r in m.group(1).split(",")]


def _suppressed(path, start_line, end_line):
    """True if a `// SMATCHET_DEVIATION(rule=duplication; ...)` sits on the nearest non-blank line
    ABOVE the clone, or ANYWHERE WITHIN the cloned span [start_line, end_line]. The span scan is
    needed because the maximal-token-run boundary can drift above the human-meaningful start (a
    stripped comment line doesn't bound the token stream), so an above-the-block marker would
    otherwise be missed. Mirrors function_size_audit._suppressed's grammar (comma-separated ids)."""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            lines = fh.read().split("\n")
    except OSError:
        return False
    # Nearest non-blank line above the clone start (marker placed directly above the block).
    idx = start_line - 2
    while 0 <= idx < len(lines) and lines[idx].strip() == "":
        idx -= 1
    if 0 <= idx < len(lines) and _has_dup_deviation(lines[idx]):
        return True
    # Anywhere inside the cloned span.
    for i in range(start_line - 1, min(end_line, len(lines))):
        if 0 <= i and _has_dup_deviation(lines[i]):
            return True
    return False


def new_clones_vs(base, head_streams, base_streams):
    """Return the list of NEW non-exempt cross-file clones: duplicated at HEAD, not grandfathered at
    `base`, and not SMATCHET_DEVIATION-suppressed. Pure (takes pre-built streams) so --selftest can
    exercise the blocking decision without git."""
    head_clones = find_clones(head_streams, allow_intra=False)
    base_hashes = {c.content_hash for c in find_clones(base_streams, allow_intra=False)}
    # A file is unchanged (for clone purposes) when its NORMALIZED TOKEN stream is identical at base
    # and head — a comment/whitespace-only edit shifts line numbers but not tokens, so it must NOT
    # count as changed (streams are (norm_tokens, lines) pairs; compare [0] only, not the tuple).
    # Winnowing/clustering is corpus-sensitive: adding tokens in an UNRELATED file can shift the
    # maximal-clone boundary selected for a pre-existing clone between two UNCHANGED files, surfacing
    # a "new" content_hash for content that already exists verbatim at base (observed: a Tracker-file
    # diff un-grandfathered a CodeColorView.cpp<->CppSyntaxLex.cpp syntax-highlight clone). A clone
    # whose EVERY occurrence is in an unchanged file cannot be duplication this diff introduced — you
    # cannot duplicate code INTO a file without changing it — so grandfather it regardless of drift.
    def _norm_tokens(streams, f):
        s = streams.get(f)
        return s[0] if s is not None else None

    changed_files = {f for f in set(head_streams) | set(base_streams)
                     if _norm_tokens(head_streams, f) != _norm_tokens(base_streams, f)}
    new = []
    for c in head_clones:
        if c.content_hash in base_hashes:
            continue  # grandfathered: this normalized block was already duplicated at base
        if all(p not in changed_files for p, _s, _e in c.locations):
            continue  # boundary-drift artifact: every occurrence is in a file unchanged vs base
        if any(_suppressed(p, s, e) for p, s, e in c.locations):
            continue
        new.append(c)
    return new


def run_diff(ref):
    base = _merge_base_or_ref(ref)
    new = new_clones_vs(base, streams_head(), streams_ref(base))
    # BLOCKING (graduated 2026-06-21, ADR-0015 calibration complete): each NEW non-exempt clone is a
    # hard FAIL; the gate exits 1 so test-lint-rules.sh / CI fail CLOSED. Exempt with a
    # SMATCHET_DEVIATION(rule=duplication) marker on/above either clone occurrence (cheap exemption).
    for c in sorted(new, key=lambda c: c.locations):
        locs = " <-> ".join("%s:%d" % (os.path.basename(p), ln) for p, ln, _e in c.locations)
        print("[dup] FAIL %s — %d-token copy-paste clone (DRY Engineering Pillar 5; blocking; "
              "exempt with SMATCHET_DEVIATION(rule=duplication))" % (locs, c.ntokens), file=sys.stderr)
    return 1 if new else 0


# --- reporting ---------------------------------------------------------------------------------

def run_scan_file(path):
    """Git-free single-file INTRA-file clone scan (bats harness). Prints
    `rule<TAB>file:lineA<TAB>file:lineB (Ntok)` per intra-file clone; advisory, exit 0."""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError as e:
        print("dup_audit: ERROR: %s" % e, file=sys.stderr)
        return 2
    if not _in_scope_scanfile(path):
        return 0  # excluded path (ThirdParty / generated) — nothing to scan
    streams = {path: normalized_stream(text)}
    for c in sorted(find_clones(streams, allow_intra=True), key=lambda c: c.locations):
        a, b = c.locations[0], c.locations[-1]
        print("%s\t%s:%d\t%s:%d (%dtok)" % (RULE_DUP, a[0], a[1], b[0], b[1], c.ntokens))
    return 0


def _in_scope_scanfile(path):
    """Scope test for --scan-file (no git): only the exclusion substrings apply (the path may be a
    bats tmpdir outside SWEEP_ROOTS, so the root prefix is not required here)."""
    p = path.replace("\\", "/")
    return p.endswith(CPP_EXT) and not any(s in p for s in EXCLUDE_SUBSTR)


def run_list():
    for c in sorted(find_clones(streams_head(), allow_intra=False), key=lambda c: c.locations):
        a, b = c.locations[0], c.locations[-1]
        print("%s\t%s:%d\t%s:%d (%dtok)" % (RULE_DUP, a[0], a[1], b[0], b[1], c.ntokens))
    return 0


def run_baseline_md():
    clones = sorted(find_clones(streams_head(), allow_intra=False), key=lambda c: c.locations)
    print("# Duplication — grandfathered baseline")
    print()
    print("_Auto-generated. Do not hand-edit; run "
          "`bash agents/scripts/project/test-lint-rules.sh --dup-baseline` and commit._")
    print("_The gate is a live merge-base delta vs `origin/develop` (dup_audit.py --diff); this "
          "file is an informational snapshot, not the gate input._")
    print()
    print("## %s (%d cross-file clones, min %d tokens)" % (RULE_DUP, len(clones), MIN_CLONE_TOKENS))
    if clones:
        for c in clones:
            locs = " · ".join("`%s:%d`" % (os.path.basename(p), ln) for p, ln, _e in c.locations)
            print("- %s — %d tokens (`%s`)" % (locs, c.ntokens, c.content_hash))
    else:
        print("- (none)")
    print()
    print("## Totals")
    print("- cross-file clones grandfathered: %d" % len(clones))
    return 0


def run_report():
    clones = find_clones(streams_head(), allow_intra=False)
    print("## Duplication audit — first-party C++ (current tree)\n")
    print("- min clone: %d normalized tokens (~8 lines); identifier + literal normalized" % MIN_CLONE_TOKENS)
    print("- cross-file clones: %d\n" % len(clones))
    print("### Largest clones\n")
    for c in sorted(clones, key=lambda c: c.ntokens, reverse=True)[:30]:
        locs = " <-> ".join("%s:%d" % (os.path.basename(p), ln) for p, ln, _e in c.locations)
        print("- %s — %d tokens" % (locs, c.ntokens))
    return 0


def run_selftest():
    """Assert the normalization + threshold invariants hold (the script's single-source-of-truth).
    NOTE: the AGENTS.md cross-check (rule-id + threshold documented) is added in Slice 2 when the
    'Quality Pillars' / § Tiered-enforcement text lands — it is intentionally absent here so this
    slice's selftest is green before the doc edit."""
    miss = 0
    # Identifier-rename collapses; literal change collapses; keyword preserved; punctuation kept.
    a = normalized_stream("int Foo(int alpha) { return alpha + 1; }")[0]
    b = normalized_stream("int Bar(int beta)  { return beta  + 2; }")[0]
    if a != b:
        print("SELFTEST FAIL: rename/literal normalization did not collapse:\n  %s\n  %s" % (a, b),
              file=sys.stderr)
        miss = 1
    if "ID" not in a or "LIT" not in a or "return" not in a:
        print("SELFTEST FAIL: expected ID + LIT + kept keyword in %s" % a, file=sys.stderr)
        miss = 1
    # A structural edit (different keyword) must NOT collapse.
    c = normalized_stream("int Foo(int a) { while (a) return a; }")[0]
    if a == c:
        print("SELFTEST FAIL: structurally-different code collapsed equal", file=sys.stderr)
        miss = 1
    # Threshold sanity: an identical ~MIN_CLONE_TOKENS run across two files is detected; a
    # sub-threshold run is not.
    big = "void f(){ " + " ".join("int v%d = g(%d) + h(%d);" % (i, i, i) for i in range(40)) + " }"
    streams = {"A.cpp": normalized_stream(big), "B.cpp": normalized_stream(big)}
    # selftest: asserts-failure — a known clone across two files must be detected (the gate's flag path).
    if not find_clones(streams):
        print("SELFTEST FAIL: identical large block across two files not detected", file=sys.stderr)
        miss = 1
    small = "int q(){ return a + b; }"
    streams_small = {"A.cpp": normalized_stream(small), "B.cpp": normalized_stream(small)}
    if find_clones(streams_small):
        print("SELFTEST FAIL: sub-threshold block flagged as a clone", file=sys.stderr)
        miss = 1
    # Graduation invariant (2026-06-21): the delta gate now BLOCKS. A planted NEW clone (present at
    # HEAD, absent from the base streams) must be reported as a new clone — i.e. the verdict path
    # that run_diff turns into exit 1. (WARN-first would have returned exit 0 here.)
    if not new_clones_vs("BASE", streams, {}):
        print("SELFTEST FAIL: planted NEW clone not flagged by the blocking delta gate", file=sys.stderr)
        miss = 1
    # A clone already present at base is grandfathered -> NOT new -> gate stays green (exit 0).
    if new_clones_vs("BASE", streams, streams):
        print("SELFTEST FAIL: grandfathered (base-present) clone flagged as NEW", file=sys.stderr)
        miss = 1
    # Boundary-drift grandfathering: winnowing is corpus-sensitive, so adding tokens in an unrelated
    # CHANGED file can shift the maximal-clone boundary selected for a clone between two UNCHANGED
    # files, giving it a base-absent content_hash for content that already exists verbatim at base.
    # A clone whose EVERY occurrence is in a file unchanged vs base must stay grandfathered. The two
    # unchanged files carry (norm_tokens, lines) with IDENTICAL norm but DIFFERENT lines at base vs
    # head — this pins the norm-only comparison: a full-tuple `!=` would mark them "changed" (line
    # drift) and re-flag the clone. Stub find_clones so BASE reports H1 and HEAD a drifted H2.
    drift_head = {"U1.cpp": (["k", "k"], [1, 2]), "U2.cpp": (["k", "k"], [1, 2]), "CHANGED.cpp": (["z"], [1])}
    drift_base = {"U1.cpp": (["k", "k"], [5, 6]), "U2.cpp": (["k", "k"], [5, 6])}
    saved_find = globals()["find_clones"]

    def _drift_stub(streams_arg, allow_intra=False):
        drifted = "CHANGED.cpp" in streams_arg
        h = "H2drift" if drifted else "H1base"
        ntok = 74 if drifted else 71
        return [Clone(h, ntok, [("U1.cpp", 1, 5), ("U2.cpp", 1, 5)])]

    globals()["find_clones"] = _drift_stub
    try:
        drifted_new = new_clones_vs("BASE", drift_head, drift_base)
    finally:
        globals()["find_clones"] = saved_find
    if drifted_new:
        print("SELFTEST FAIL: boundary-drift clone in files unchanged (by norm tokens) vs base was "
              "not grandfathered", file=sys.stderr)
        miss = 1
    if miss:
        return 1
    print("selftest: normalization + threshold invariants hold (min %d tokens, shingle %d, "
          "winnow %d)" % (MIN_CLONE_TOKENS, SHINGLE_K, WINNOW_W))
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
        print("dup_audit: ERROR: %s" % e, file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()

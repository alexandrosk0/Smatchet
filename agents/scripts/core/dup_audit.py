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
double-edged-DRY guardrail). Preprocessor directives (except function-like macros) and the `using` run
that opens a file are dropped before shingling, so sibling TUs that share only an include block are
not clones.

Verdict model — BLOCKING (graduated 2026-06-21; calibration complete per ADR-0015). `--diff`
emits `[dup] FAIL ...` lines for NEW cross-file copy-paste clones and exits 1, blocking the
merge. The delta-grandfather machinery ensures only genuinely-NEW clones (duplicated at HEAD but
not at the merge-base) fire; all pre-existing duplication is grandfathered. A clean tree exits 0.

Delta semantics (grandfathering): a clone is keyed by the content-hash of its normalized token
run. All duplication that already exists at the merge-base is grandfathered (its content-hashes
populate the base set); a clone FAILs only when its normalized content is duplicated at HEAD but
was not duplicated at base — i.e. a genuinely new copy-paste. A `// SMATCHET_DEVIATION(rule=
duplication; ...)` on the nearest non-blank line above either clone occurrence suppresses it
(cheap exemption — the plan prefers an exemption over abstracting across unrelated contexts).

Modes mirror function_size_audit.py:

  dup_audit.py                       # human report of current cross-file clones
  dup_audit.py --list                # one `rule<TAB>fileA:lineA<TAB>fileB:lineB (Ntok)` per clone
  dup_audit.py --dead-markers        # one `rule<TAB>file:line` per marker that exempts no clone
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
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import comment_lib as cl
import layer_paths

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


# Preprocessor directives are dropped before shingling: sibling TUs split from one file all open with
# the same include block, which on its own clears MIN_CLONE_TOKENS, and no directive is logic. The one
# exception is a function-like macro (the name directly followed by `(`): its body is code and can be
# copy-pasted, so it stays. Object-like defines (include guards, NOMINMAX, `#define ImGui X`) go.
_FUNCTION_MACRO_RE = re.compile(r"^\s*#\s*define\s+[A-Za-z_][A-Za-z0-9_]*\(")


def _drop_directives(toks, text):
    """Remove every token of a preprocessor directive, except a function-like `#define`, from `toks`
    ([(token, line), ...]). A directive is a `#` that is the first token on its line, and it runs to
    the end of its logical line (a trailing backslash continues it). Only the directive's tokens go;
    the code around it stays, so a clone that starts in an include block and continues into real logic
    is still reported for its logic part. Tokens inside a kept macro are never read as a directive
    start, so a stringizing `#` at the start of a macro continuation line stays part of the body."""
    raw_lines = text.split("\n")

    def logical_end(line):
        end = line
        # Whitespace after the backslash still splices, as GCC and Clang accept it.
        while end <= len(raw_lines) and raw_lines[end - 1].rstrip().endswith("\\"):
            end += 1
        return end

    out = []
    drop_through = 0  # last line of the directive being dropped
    keep_through = 0  # last line of the function-like macro being kept
    prev_line = 0
    for tok, ln in toks:
        first_on_line = ln != prev_line
        prev_line = ln
        if ln <= drop_through:
            continue
        if tok == "#" and first_on_line and ln > keep_through:
            if _FUNCTION_MACRO_RE.match(raw_lines[ln - 1]):
                keep_through = logical_end(ln)
            else:
                drop_through = logical_end(ln)
                continue
        out.append((tok, ln))
    return out


def _drop_leading_using_run(toks):
    """Remove the run of `using ...;` declarations and `namespace X = ...;` aliases that opens a file
    once its directives are gone. Sibling TUs repeat the same block of imported names after their
    includes, and it is not logic either. Only the run at the very start of the file goes: the first
    token that begins anything else ends it, so a `using` inside a namespace or function stays."""
    i, n = 0, len(toks)
    while i < n:
        is_using = toks[i][0] == "using"
        is_alias = (toks[i][0] == "namespace" and i + 2 < n and _IDENT_RE.match(toks[i + 1][0])
                    and toks[i + 2][0] == "=")
        if not (is_using or is_alias):
            break
        j = i
        while j < n and toks[j][0] != ";":
            j += 1
        if j == n:
            break  # unterminated: leave the tail in the stream rather than guess
        i = j + 1
    return toks[i:]


def normalized_stream(text):
    """Return (norm_tokens, lines): parallel lists of normalized tokens and their source lines.
    Preprocessor directives other than a function-like `#define` (_drop_directives) and the `using`
    run that opens the file (_drop_leading_using_run) are not part of the stream."""
    norm = []
    lines = []
    for tok, ln in _drop_leading_using_run(_drop_directives(_tokens_with_lines(text), text)):
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


# How far above a clone start to look for a marker that IS a duplication deviation but sits in the
# wrong place. Diagnostic radius only — never widens what _suppressed accepts.
INEFFECTIVE_DEVIATION_WINDOW = 5


def _ineffective_dup_deviation(path, start_line):
    """1-based line of a VALID `rule=duplication` deviation marker within INEFFECTIVE_DEVIATION_WINDOW
    lines above `start_line`, else 0. Callers use this ONLY to explain a FAIL that already happened:
    if _suppressed said no while a well-formed marker sits right there, the marker's PLACEMENT is
    the fault, not its text. The usual cause is a wrapped comment — `_has_dup_deviation` is a
    per-LINE test, so a marker whose reason prose spills onto following comment lines leaves prose
    as the nearest non-blank line above the clone, and prose carries no token. Purely diagnostic:
    this never affects the pass/fail decision."""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            lines = fh.read().split("\n")
    except OSError:
        return 0
    lo = max(0, start_line - 1 - INEFFECTIVE_DEVIATION_WINDOW)
    for i in range(start_line - 2, lo - 1, -1):
        if 0 <= i < len(lines) and _has_dup_deviation(lines[i]):
            return i + 1
    return 0


def _suppressing_lines(path, start_line, end_line):
    """1-based lines of every `// SMATCHET_DEVIATION(rule=duplication; ...)` that exempts the clone
    occurrence [start_line, end_line] of `path`: the nearest non-blank line ABOVE the clone, plus any
    marker ANYWHERE WITHIN the span. The span scan is needed because the maximal-token-run boundary
    can drift above the human-meaningful start (a stripped comment line doesn't bound the token
    stream), so an above-the-block marker would otherwise be missed. Mirrors
    function_size_audit._suppressed's grammar (comma-separated ids)."""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            lines = fh.read().split("\n")
    except OSError:
        return []
    found = []
    # Nearest non-blank line above the clone start (marker placed directly above the block).
    idx = start_line - 2
    while 0 <= idx < len(lines) and lines[idx].strip() == "":
        idx -= 1
    if 0 <= idx < len(lines) and _has_dup_deviation(lines[idx]):
        found.append(idx + 1)
    # Anywhere inside the cloned span.
    for i in range(max(0, start_line - 1), min(end_line, len(lines))):
        if _has_dup_deviation(lines[i]):
            found.append(i + 1)
    return found


def _suppressed(path, start_line, end_line):
    """True if a duplication deviation exempts this clone occurrence (see _suppressing_lines)."""
    return bool(_suppressing_lines(path, start_line, end_line))


def dead_dup_markers(clones, paths):
    """(path, line) of every `rule=duplication` marker in `paths` that exempts none of `clones`. Such
    a marker can be deleted without un-exempting anything the detector reports today. Pure apart from
    reading the files, so --selftest can drive it without git."""
    used = set()
    for c in clones:
        for p, s, e in c.locations:
            for ln in _suppressing_lines(p, s, e):
                used.add((p, ln))
    dead = []
    for p in paths:
        try:
            with open(p, "r", encoding="utf-8", errors="replace") as fh:
                lines = fh.read().split("\n")
        except OSError:
            continue
        for i, line in enumerate(lines):
            if _has_dup_deviation(line) and (p, i + 1) not in used:
                dead.append((p, i + 1))
    return dead


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
        # A marker that IS present but did NOT suppress is a shape error, and the bare FAIL above
        # reads as "your exemption text is wrong" when the text is fine and only its placement is
        # not. Name that explicitly so the reader doesn't spend a round re-wording the reason.
        for p, s, _e in c.locations:
            ln = _ineffective_dup_deviation(p, s)
            if ln:
                print("[dup] hint: %s:%d is a rule=duplication deviation that did NOT suppress this "
                      "clone — the marker must be a SINGLE line on the nearest non-blank line above "
                      "the clone (or inside it). Wrapped reason prose leaves a comment line with no "
                      "token as the nearest line; keep SMATCHET_DEVIATION(...) on one line and put "
                      "extra prose on comment lines ABOVE it."
                      % (os.path.basename(p), ln), file=sys.stderr)
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


def run_dead_markers():
    """One `rule<TAB>file:line` per duplication marker that exempts no cross-file clone at HEAD.
    Advisory, exit 0. `--diff` cannot answer this: removing a comment leaves the token stream
    unchanged, so a clone whose marker was deleted stays grandfathered and the gate stays green
    whether or not the marker was doing anything."""
    for p, ln in dead_dup_markers(find_clones(streams_head(), allow_intra=False), list_head_files()):
        print("%s\t%s:%d" % (RULE_DUP, p, ln))
    return 0


def run_baseline_md():
    clones = sorted(find_clones(streams_head(), allow_intra=False), key=lambda c: c.locations)
    print("# Duplication — grandfathered baseline")
    print()
    print("_Auto-generated. Do not hand-edit; run "
          "`bash %s --dup-baseline` and commit._" % layer_paths.from_project("agents/scripts/project/test-lint-rules.sh"))
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


def _selftest_prologue_filter():
    """Directives and the leading `using` run leave the stream; everything else, line numbers
    included, is untouched. Returns 1 on any failure."""
    miss = 0

    def fail(msg):
        print("SELFTEST FAIL: " + msg, file=sys.stderr)
        return 1

    src = ('#include "a.h"\n'
           '#if defined(X)\n'
           '#include <b>\n'
           '#endif\n'
           'using a::b;\n'
           'namespace fs = c::d;\n'
           'int x = 1;\n'
           'using e::f;\n')
    norm, lines = normalized_stream(src)
    expected = ["int", "ID", "=", "LIT", ";", "using", "ID", ":", ":", "ID", ";"]
    if norm != expected or lines[0] != 7 or lines[-1] != 8:
        miss |= fail("directives / leading using run not dropped as specified: %s %s" % (norm, lines))
    # A backslash continues a directive onto the next line; the whole logical line goes.
    if normalized_stream("#if defined(A) && \\\n    defined(B)\nint y;\n")[0] != ["int", "ID", ";"]:
        miss |= fail("continuation line of a dropped directive leaked into the stream")
    # A function-like macro is code and stays, including a stringizing `#` that starts a
    # continuation line; an object-like define (an include guard) goes.
    norm, lines = normalized_stream("#define GUARD_H\n#define STR(x) \\\n    #x\nint z;\n")
    if (norm[:2] != ["#", "ID"] or norm.count("#") != 2 or norm[-3:] != ["int", "ID", ";"]
            or lines[0] != 2):
        miss |= fail("function-like macro not kept intact, or object-like define kept: %s %s" % (norm, lines))
    # Prologues cycle through several line shapes, as real ones do. A block of one repeated shape
    # would be skipped as ubiquitous (MAX_FP_OCCURRENCES) whatever the filter does, and the
    # "not a clone" checks below would then pass for the wrong reason.
    include_shapes = ('#include "Dir%d/File.h"\n', "#include <lib%d/sub/file.hpp>\n",
                      "#include <cstd%d>\n", '#include "Gen%d.inc" // generated\n', "#include <a%d/b.h>\n")
    using_shapes = ("using a%d::b;\n", "using namespace n%d;\n", "using T%d = std::vector<int>;\n",
                    "namespace fs%d = x::y::z;\n", "using c%d::d::e;\n")
    includes = "".join(include_shapes[i % 5] % i for i in range(40))
    usings = "".join(using_shapes[i % 5] % i for i in range(40))
    logic = "".join("    int v%d = g(%d) + h(%d);\n" % (i, i, i) for i in range(40))
    block = "void f() {\n" + logic + "}\n"

    def unfiltered(text):
        pairs = _tokens_with_lines(text)
        return [normalize_token(t) for t, _ in pairs], [ln for _, ln in pairs]

    # A shared include block or a shared leading using run alone is not a clone, though the same
    # text is one when its directives / using lines stay in the stream.
    for name, shared in (("include block", includes), ("leading using run", usings)):
        texts = {"A.cpp": shared + "int a() { return 1; }\n",
                 "B.cpp": shared + "double b(double x) { return x * 2; }\n"}
        if not find_clones({p: unfiltered(t) for p, t in texts.items()}):
            miss |= fail("the shared %s fixture is not a clone even unfiltered; the check below is "
                         "vacuous" % name)
        if find_clones({p: normalized_stream(t) for p, t in texts.items()}):
            miss |= fail("a shared %s alone was reported as a clone" % name)
    # The guard against over-correction: a clone that starts in a shared include block and continues
    # into logic is still reported, starting at its first line of logic.
    streams = {"A.cpp": normalized_stream(includes + block), "B.cpp": normalized_stream(includes + block)}
    clones = find_clones(streams)
    if not clones or any(s != 41 for c in clones for _p, s, _e in c.locations):
        miss |= fail("prologue-then-logic clone lost or not anchored at the logic: %s"
                     % [c.locations for c in clones])
    return miss


def _selftest_dead_markers():
    """dead_dup_markers lists a marker that exempts nothing and never one that exempts a clone.
    Returns 1 on any failure."""
    logic = "".join("    int v%d = g(%d) + h(%d);\n" % (i, i, i) for i in range(40))
    block = "void f() {\n" + logic + "}\n"
    marker = "// SMATCHET_DEVIATION(rule=duplication; reason=t; owner=t; revisit=2099-01-01)\n"
    with tempfile.TemporaryDirectory() as td:
        a = os.path.join(td, "A.cpp")
        b = os.path.join(td, "B.cpp")
        with open(a, "w", encoding="utf-8") as fh:
            fh.write(marker + block + "\n" + marker + "int lone() { return 7; }\n")
        with open(b, "w", encoding="utf-8") as fh:
            fh.write(block)
        streams = {}
        for p in (a, b):
            with open(p, "r", encoding="utf-8") as fh:
                streams[p] = normalized_stream(fh.read())
        dead = dead_dup_markers(find_clones(streams), [a, b])
        used_line, dead_line = 1, block.count("\n") + 3
        if dead != [(a, dead_line)]:
            print("SELFTEST FAIL: dead_dup_markers expected only %s:%d (and never the used marker on "
                  "line %d), got %s" % (a, dead_line, used_line, dead), file=sys.stderr)
            return 1
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
    # Deviation SHAPE: suppression is a per-LINE test, so a marker whose reason prose wraps onto
    # following comment lines does NOT suppress (the nearest non-blank line above the clone is
    # prose, which carries no token). That silent shape error is what the [dup] hint explains, so
    # pin both halves: the wrapped marker must still FAIL to suppress, and it must be detected as
    # an ineffective-but-present marker. A single-line marker must suppress and produce no hint.
    with tempfile.TemporaryDirectory() as td:
        wrapped = os.path.join(td, "wrapped.cpp")
        with open(wrapped, "w", encoding="utf-8") as fh:
            fh.write("// SMATCHET_DEVIATION(rule=duplication; reason=structural twins across two\n"
                     "// independent subsystems; unifying them would couple unrelated code;\n"
                     "// owner=ui-host; revisit=2026-12-31)\n"
                     "void f() { int a = 1; }\n")
        if _suppressed(wrapped, 4, 4):
            print("SELFTEST FAIL: wrapped multi-line deviation suppressed (per-line rule broken)",
                  file=sys.stderr)
            miss = 1
        if _ineffective_dup_deviation(wrapped, 4) != 1:
            print("SELFTEST FAIL: wrapped deviation not reported as present-but-ineffective",
                  file=sys.stderr)
            miss = 1
        single = os.path.join(td, "single.cpp")
        with open(single, "w", encoding="utf-8") as fh:
            fh.write("// reason prose lives above the marker, where it does no harm\n"
                     "// SMATCHET_DEVIATION(rule=duplication; owner=ui-host; revisit=2026-12-31)\n"
                     "void f() { int a = 1; }\n")
        if not _suppressed(single, 3, 3):
            print("SELFTEST FAIL: single-line deviation directly above the clone did not suppress",
                  file=sys.stderr)
            miss = 1
        # A deviation for a DIFFERENT rule must never draw the duplication hint — otherwise the
        # hint fires on unrelated markers and trains readers to ignore it.
        other = os.path.join(td, "other.cpp")
        with open(other, "w", encoding="utf-8") as fh:
            fh.write("// SMATCHET_DEVIATION(rule=function-too-long; owner=ui-host; revisit=2026-12-31)\n"
                     "void f() { int a = 1; }\n")
        if _ineffective_dup_deviation(other, 2):
            print("SELFTEST FAIL: non-duplication deviation drew the duplication hint",
                  file=sys.stderr)
            miss = 1
    miss |= _selftest_prologue_filter()
    miss |= _selftest_dead_markers()
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
    ap.add_argument("--dead-markers", action="store_true")
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
        if args.dead_markers:
            sys.exit(run_dead_markers())
        if args.baseline_md:
            sys.exit(run_baseline_md())
        sys.exit(run_report())
    except Exception as e:  # never crash-as-clean: surface as infra error (>=2)
        print("dup_audit: ERROR: %s" % e, file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()

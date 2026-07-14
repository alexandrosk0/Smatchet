#!/usr/bin/env python3
"""Read-only comment analyzer for the reduce-source-comment-bloat sweep.

Walks tracked first-party C++ (Source/Core, Source/Plugins, Source/Standalone; excludes
ThirdParty, tests, build), classifies every full-line comment into the plan's taxonomy
buckets, and reports per-file + per-subsystem counts (the § Baseline metrics table) plus a
candidate-removal report.

Usage:
  comment_audit.py                 # human markdown report to stdout
  comment_audit.py --json out.json # machine report
  comment_audit.py --diff <ref>    # Phase-4 regrowth mode: emit noise-bucket violation tuples
                                    #   rule<TAB>basename:line<TAB>snippet  (added lines vs <ref>)
See docs/plans/shipped/reduce-source-comment-bloat.md.
"""

import argparse
import json
import os
import re
import subprocess
import sys

# Intentional sys.path shim: comment_lib.py is a sibling module in this same
# directory, not an installed package, so prepend our own dir to import it when
# comment_audit.py is invoked as a standalone script (no package context).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import comment_lib as cl  # noqa: E402  (import follows the path shim above)

# --- scope ---------------------------------------------------------------------

SWEEP_ROOTS = ("Source/Core/", "Source/Plugins/", "Source/Standalone/")
EXCLUDE_SUBSTR = ("/ThirdParty/", "ThirdParty/")
CPP_EXT = (".cpp", ".h", ".hpp", ".cc", ".cxx")

# Subsystem buckets for the per-subsystem density table (first match wins).
SUBSYSTEMS = [
    ("Source/Core/include/", "Source/Core/include/ (headers/API docs)"),
    ("Source/Core/src/Ui/", "Source/Core/src/Ui/"),
    ("Source/Core/src/Tracker/", "Source/Core/src/Tracker/"),
    ("Source/Core/src/", "Source/Core/src/ (non-Ui)"),
    ("Source/Plugins/", "Source/Plugins/"),
    ("Source/Standalone/", "Source/Standalone/"),
]

# --- taxonomy classification (full-line comments only) -------------------------

# PROTECT: never-touch markers (substring match on the comment text).
PROTECT_SUBSTR = (
    "CLI stdout", "pre-logger-init", "C-ABI", "custom-deleter", "pimpl",
    "SMATCHET_DEVIATION", "PILLAR2_", "est-latency:", "SMATCHET_UI_PERF_SCOPE",
    "clang-format off", "clang-format on", "NOLINT", "IWYU",
    "SMATCHET_WITH_", "SMATCHET_EMBEDDED_IN_UNREAL",
    "docs/plans/", "docs/adr/", "docs/design/", "see docs", "ADR ",
)
PROTECT_RE = [
    re.compile(r"^\s*}\s*//\s*namespace"),     # } // namespace Foo  (nav aid)
    re.compile(r"^\s*//\s*(NOLINT|clang-format)"),
]

DECORATIVE_RE = re.compile(r"^\s*(//|/\*|\*)[\s/*=#~_>-]*$")  # divider/blank: only punctuation
SHOUT_BANNER_RE = re.compile(r"^\s*//\s*\d*\.?\s*[A-Z][A-Z0-9 _]{6,}\s*$")  # // 3. THE REST OF YOUR INCLUDES
BLANK_COMMENT_RE = re.compile(r"^\s*(//|\*)\s*$")
DOC_OPEN_RE = re.compile(r"^\s*(///|/\*\*|//!)")  # doc-comment markers
# commented-out code heuristic: comment whose body looks like code (ends with ; or { or }, or
# matches a call/decl shape). Wave-1 FLAGS only.
CODE_LIKE_RE = re.compile(
    r"^\s*//\s*("
    r".*;\s*$"                       # ends in semicolon
    r"|.*\{\s*$|.*\}\s*$"            # opens/closes a block
    r"|(if|for|while|switch|return|else|case)\b.*"
    r"|[A-Za-z_][\w:<>]*\s+[A-Za-z_]\w*\s*[=(].*"  # decl/assign/call
    r")"
)
KEEP_NOTE_RE = re.compile(r"//.*\b(kept:|reference impl|example:|e\.g\.|usage:)", re.I)

# Prose-vs-code discriminator for the flag-commented-code heuristic. CODE_LIKE_RE's
# decl/assign/call alternative over-matches English prose that merely contains a
# "word word(" or "word = word" fragment — e.g. "...reset the latch (mirroring the
# sibling returns)" or "...drops cpr from the include graph (about a hundred TUs)" —
# the #1 false-positive source for the comment-commented-out-code gate
# (build-quality-velocity-hardening #7; the #915 `).identifier` prose miss). A
# CODE_LIKE_RE match is demoted to prose (NOT flagged) only on a strong prose signal:
# no hard code terminator, no code operator, several English words, alphabetic-dense.
# Otherwise the original flag-commented-code verdict stands — false-negatives are far
# cheaper than false-positives for this WARN-first gate.
_CODE_TERMINATOR_RE = re.compile(r"[;{}]\s*$")  # commented-out code ends in ; { } ; prose ~never does
_CODE_OPERATOR_RE = re.compile(r"->|::|==|!=|<=|>=|&&|\|\||\+\+|--|[-+*/]=|=>")
_WORD_RE = re.compile(r"[A-Za-z]{2,}")
# A bare identifier-call (`foo()` / `foo(bar)`) that is NOT terminated by `;`/`{`/`}` is the #1
# residual false positive: real commented-out code statements close with `;`/brace, whereas prose
# that merely MENTIONS a function ("note foo() does X", "call reset() when done") leaves the call
# mid-sentence. Demote such a line to prose when no code terminator is present.
_BARE_CALL_RE = re.compile(r"[A-Za-z_]\w*\s*\([^)]*\)")

# A `// SMATCHET_DEVIATION( ... )` whose long `reason=` is wrapped across several `//`
# lines: the marker line is PROTECT-listed (contains the token), but its continuation
# lines carry only rationale prose and do NOT contain the token, so they trip
# comment-commented-out-code / decorative / blank-run. clang-format wrapping a long
# single-line deviation (pre-ship whole-file-formats) turns a clean commit into a
# fix -> format -> re-fail loop. This makes the continuations first-class deviation body.
_DEVIATION_OPEN_RE = re.compile(r"SMATCHET_DEVIATION\s*\(")


def _deviation_continuation_lines(lines):
    """1-based line numbers that are CONTINUATION lines of a wrapped
    `// SMATCHET_DEVIATION( ... )` comment — the `//`/`*` lines INSIDE the paren
    span, AFTER the opening marker line (which is protected separately). Paren-
    balanced: a single-line deviation (parens close on the marker line) yields no
    continuations; internal balanced parens in the reason text don't close early."""
    cont = set()
    depth = 0
    in_block = False
    for i, raw in enumerate(lines):
        if not in_block:
            if _DEVIATION_OPEN_RE.search(raw):
                depth = raw.count("(") - raw.count(")")
                in_block = depth > 0   # marker line itself is not a continuation
            continue
        stripped = raw.lstrip()
        if not (stripped.startswith("//") or stripped.startswith("*")):
            # A non-comment line ends the continuation span regardless of paren balance. A wrapped
            # SMATCHET_DEVIATION is contiguous //|* lines; without this bound an unbalanced '(' in
            # free-form reason= prose left depth > 0 forever, so in_block never reset and EVERY later
            # //|* line in the file was silently exempted from the comment-noise gate (#1760).
            in_block = False
            continue
        cont.add(i + 1)
        depth += raw.count("(") - raw.count(")")
        if depth <= 0:
            in_block = False
    return cont


def _comment_body(raw_line):
    """The text of a // comment after the leading slashes, stripped."""
    return re.sub(r"^\s*//+", "", raw_line).strip()


def is_prose_not_code(raw_line):
    """True if a CODE_LIKE_RE-matched // comment is really English prose, not
    commented-out code. Conservative: demotes only on a strong prose signal."""
    body = _comment_body(raw_line)
    if not body or _CODE_TERMINATOR_RE.search(body) or _CODE_OPERATOR_RE.search(body):
        return False
    # An identifier-call anywhere in the line with NO trailing `;`/`{`/`}` is prose mentioning a function,
    # not a commented-out statement (those close with a terminator). Demote it directly — the
    # terminator + operator guards above already rule out genuine code, so a bare unterminated call
    # ("foo() does X", "note bar()") is safe to treat as rationale.
    if _BARE_CALL_RE.search(body):
        return True
    words = _WORD_RE.findall(body)
    if len(words) < 4:
        return False
    nonspace = len(re.sub(r"\s", "", body))
    alpha = sum(len(w) for w in words)
    return nonspace > 0 and (alpha / nonspace) >= 0.6


def classify_comment(stripped, raw_line):
    """Classify a full-line comment into a taxonomy bucket id."""
    for rx in PROTECT_RE:
        if rx.search(raw_line):
            return "protect"
    if any(s in raw_line for s in PROTECT_SUBSTR):
        return "protect"
    # A BARE doc-block opener on its own line (`/**` / `/*!` — standard Javadoc/Doxygen
    # style) is the start of a doc comment, not a decorative divider, but DECORATIVE_RE
    # would otherwise eat it (`/*` + only-punctuation matches the second `*`). Exactly-two
    # stars only: `/***`+ remains a banner. (PR #1112 false positive: ISyncCache.h:10.)
    if stripped in ("/**", "/*!"):
        return "judge-apidoc"
    # Same shape, plain variant: a bare `/*` opener is a legal block-comment start (its body
    # follows on later lines) — never a strippable divider. Zero first-party occurrences
    # today; closed pre-emptively alongside the `/**` fix so the class can't recur.
    if stripped == "/*":
        return "judge-rationale"
    if BLANK_COMMENT_RE.match(raw_line):
        return "cut-blank"
    if DECORATIVE_RE.match(raw_line):
        return "cut-decorative"
    if SHOUT_BANNER_RE.match(raw_line):
        return "cut-decorative"
    if DOC_OPEN_RE.match(stripped):
        return "judge-apidoc"
    if stripped.startswith("//") and CODE_LIKE_RE.match(raw_line) and not KEEP_NOTE_RE.search(raw_line):
        if is_prose_not_code(raw_line):
            return "judge-rationale"
        return "flag-commented-code"
    # plain // or block-continuation prose
    return "judge-rationale"


CUT_BUCKETS = ("cut-blank", "cut-decorative")  # mechanically strippable (Wave 1)


def _is_textual_comment_line(raw_line):
    """True if `raw_line` is a full-line comment carrying actual text (not bare `//`/`*`,
    not a decorative divider, not blank, not code). The neighbor test for an allowed
    single intra-block separator."""
    if raw_line is None:
        return False
    kinds = cl.classify_line_kinds(raw_line + "\n")
    if not kinds or kinds[0] != "full_comment":
        return False
    stripped = raw_line.strip()
    if BLANK_COMMENT_RE.match(raw_line) or DECORATIVE_RE.match(raw_line):
        return False
    return bool(stripped)


def is_allowed_blank_separator(lines, line_no):
    """True when the bare-comment (`cut-blank`) line at 1-based `line_no` is a SINGLE intra-block
    paragraph separator — a lone bare `//` between two textual comment lines of the same block.
    A run of 2+ bare `//` is NOT allowed (each such line has a bare neighbor, so this returns
    False for every line in the run), and a bare `//` not flanked by comment text on BOTH sides
    (e.g. against code or a blank line, or at file edge) is NOT allowed either. `lines` is the
    new-side file content split on '\\n'; `line_no` is 1-based."""
    idx = line_no - 1  # 0-based
    if idx <= 0 or idx >= len(lines) - 1:
        return False  # need a real neighbor on each side (not at file edge)
    prev_line = lines[idx - 1]
    next_line = lines[idx + 1]
    return _is_textual_comment_line(prev_line) and _is_textual_comment_line(next_line)


def _git(args):
    """Run a git command, raising on non-zero exit (silent git failures would corrupt the
    baseline count or the regrowth diff into a false-clean result)."""
    p = subprocess.run(["git"] + args, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if p.returncode != 0:
        raise RuntimeError("git %s failed (%d): %s" % (" ".join(args), p.returncode, p.stderr.strip()))
    return p.stdout


def list_files():
    out = _git(["ls-files"] + [r + "**" for r in SWEEP_ROOTS])
    if not out.strip():
        out = _git(["ls-files"])  # fallback: no glob, filter below
    files = []
    for f in out.splitlines():
        if not f.endswith(CPP_EXT):
            continue
        if not f.startswith(SWEEP_ROOTS):
            continue
        if any(s in f for s in EXCLUDE_SUBSTR):
            continue
        files.append(f)
    return sorted(set(files))


def audit_file(path, text):
    kinds = cl.classify_line_kinds(text)
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines = lines[:-1]
    total = len(kinds)
    code = comment = blank = 0
    buckets = {}
    candidates = []
    for idx, kind in enumerate(kinds):
        if kind == "blank":
            blank += 1
        elif kind == "code":
            code += 1
        elif kind == "trailing":
            code += 1  # trailing comment counts as code
        elif kind == "full_comment":
            comment += 1
            raw = lines[idx] if idx < len(lines) else ""
            b = classify_comment(raw.strip(), raw)
            buckets[b] = buckets.get(b, 0) + 1
            if b in CUT_BUCKETS or b == "flag-commented-code":
                candidates.append((idx + 1, b, raw.strip()[:80]))
    return dict(total=total, code=code, comment=comment, blank=blank, buckets=buckets,
                candidates=candidates)


def subsystem_of(path):
    for prefix, label in SUBSYSTEMS:
        if path.startswith(prefix):
            return label
    return "other"


def run_audit():
    files = list_files()
    per_sub = {}
    grand = dict(files=0, total=0, code=0, comment=0, blank=0, buckets={})
    file_reports = []
    for f in files:
        try:
            with open(f, "r", encoding="utf-8", errors="replace") as fh:
                text = fh.read()
        except OSError:
            continue
        r = audit_file(f, text)
        sub = subsystem_of(f)
        s = per_sub.setdefault(sub, dict(files=0, total=0, comment=0))
        s["files"] += 1
        s["total"] += r["total"]
        s["comment"] += r["comment"]
        grand["files"] += 1
        grand["total"] += r["total"]
        grand["code"] += r["code"]
        grand["comment"] += r["comment"]
        grand["blank"] += r["blank"]
        for b, c in r["buckets"].items():
            grand["buckets"][b] = grand["buckets"].get(b, 0) + c
        file_reports.append((f, r))
    return grand, per_sub, file_reports


def pct(a, b):
    return f"{(100.0 * a / b):.1f}%" if b else "0.0%"


def print_markdown(grand, per_sub):
    g = grand
    nonblank = g["total"] - g["blank"]
    print("## Comment audit — first-party C++ (current develop)\n")
    print("| Scope | Files | Total | Code | Comment | Blank | Comment % total | Comment % non-blank |")
    print("|---|---|---|---|---|---|---|---|")
    print(f"| First-party C++ | {g['files']} | {g['total']} | {g['code']} | {g['comment']} | "
          f"{g['blank']} | {pct(g['comment'], g['total'])} | {pct(g['comment'], nonblank)} |\n")
    print("### Per-subsystem comment density\n")
    print("| Subsystem | Files | Total lines | Comment lines | Comment % |")
    print("|---|---|---|---|---|")
    for _, label in SUBSYSTEMS:
        s = per_sub.get(label)
        if s:
            print(f"| `{label}` | {s['files']} | {s['total']} | {s['comment']} | {pct(s['comment'], s['total'])} |")
    print("\n### Comment taxonomy buckets\n")
    for b in sorted(g["buckets"]):
        print(f"- `{b}`: {g['buckets'][b]}")


def _merge_base_or_ref(ref):
    """Resolve the fork point of <ref> and HEAD so the diff reflects only what THIS branch added —
    not <ref>'s own divergence since the branch point. If <ref> has advanced past our merge-base,
    a raw `git diff <ref> HEAD` misattributes <ref>'s newer lines (and our now-older lines) as our
    additions, false-failing the gate. NOTE: merge-base needs REAL HISTORY on both sides — a shallow
    HEAD *and* a depth-1 <ref> fetch (the CI default) leave it unresolved, so the caller's workflow
    must unshallow / fetch enough depth (tooling.md P1). Falls back to <ref>'s tip (with a stderr
    WARN) only if merge-base can't be found."""
    p = subprocess.run(["git", "merge-base", ref, "HEAD"], capture_output=True, text=True, encoding="utf-8", errors="replace")
    mb = p.stdout.strip()
    if p.returncode == 0 and mb:
        return mb
    sys.stderr.write("comment_audit: WARN: `git merge-base %s HEAD` did not resolve "
                     "(shallow clone?) — falling back to %s tip; delta may false-flag.\n" % (ref, ref))
    return ref


def _scan_added_noise(ref):
    """Scan lines ADDED vs the merge-base of <ref>; return the noise-bucket hits as
    [(path, line_no, bucket, rule_id, body), ...]. `line_no` is the working-tree (new-side)
    line number; `bucket` is the classify_comment id; `rule_id` is the gate rule name.
    Deviation-suppressed lines are excluded. Shared by --diff (report) and --fix (strip) so
    both see an identical set."""
    ref = _merge_base_or_ref(ref)
    rule_for = {
        "cut-blank": "comment-blank-run",
        "cut-decorative": "comment-decorative-banner",
        "flag-commented-code": "comment-commented-out-code",
    }
    diff = _git(["diff", "--unified=0", ref, "--", *[r + "**" for r in SWEEP_ROOTS]])
    cur_file = None
    cur_line = 0
    hits = []
    file_cache = {}

    def _file_lines(path):
        if path not in file_cache:
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                file_cache[path] = fh.read().split("\n")
        return file_cache[path]

    def allowed_separator(path, line_no):
        # A lone bare `//` between two textual comment lines of the same block is an allowed
        # intra-block paragraph separator (NOT a strippable blank-run). Runs of 2+ still flag.
        try:
            return is_allowed_blank_separator(_file_lines(path), line_no)
        except OSError:
            return False

    dev_cont_cache = {}

    def in_deviation_continuation(path, line_no):
        # A continuation line of a wrapped `// SMATCHET_DEVIATION( ... )` block is the
        # deviation's own reason-prose — never real noise, regardless of the noise rule.
        if path not in dev_cont_cache:
            try:
                dev_cont_cache[path] = _deviation_continuation_lines(_file_lines(path))
            except OSError:
                dev_cont_cache[path] = set()
        return line_no in dev_cont_cache[path]

    def suppressed(path, line_no, rule_id):
        # Escape hatch: a `// SMATCHET_DEVIATION(rule=<rule_id>; ...)` on the line ABOVE the
        # flagged line suppresses it (the existing forward-only deviation grammar; no new syntax).
        try:
            lines = _file_lines(path)
            # Walk upward past blank lines to the nearest non-blank line, honoring the forward-only
            # deviation contract (a marker separated from its target by blank lines still suppresses).
            idx = line_no - 2
            while 0 <= idx < len(lines) and lines[idx].strip() == "":
                idx -= 1
            prev = lines[idx] if 0 <= idx < len(lines) else ""
        except OSError:
            return False
        # Prefix-safe: `rule=comment-blank` must NOT match `rule=comment-blank-run` (and vice-versa).
        # The next char after rule_id must be neither a word char NOR a hyphen — `(?![\w-])` —
        # because `\b` matches between a word char and `-`, which would let a longer/typo'd id
        # (e.g. `rule=comment-blank-run-extra`) leak through.
        return "SMATCHET_DEVIATION" in prev and \
            re.search(r"\brule=%s(?![\w-])" % re.escape(rule_id), prev) is not None

    for ln in diff.splitlines():
        if ln.startswith("+++ b/"):
            cur_file = ln[6:]
        elif ln.startswith("@@"):
            m = re.search(r"\+(\d+)", ln)
            cur_line = int(m.group(1)) if m else 0
        elif ln.startswith("+") and not ln.startswith("+++"):
            body = ln[1:]
            if cur_file and cur_file.endswith(CPP_EXT) and cur_file.startswith(SWEEP_ROOTS) \
                    and not any(s in cur_file for s in EXCLUDE_SUBSTR):
                kinds = cl.classify_line_kinds(body + "\n")
                if kinds and kinds[0] == "full_comment":
                    b = classify_comment(body.strip(), body)
                    if b in rule_for and not suppressed(cur_file, cur_line, rule_for[b]) \
                            and not in_deviation_continuation(cur_file, cur_line) \
                            and not (b == "cut-blank" and allowed_separator(cur_file, cur_line)):
                        hits.append((cur_file, cur_line, b, rule_for[b], body))
            cur_line += 1
    return hits


def run_diff_mode(ref):
    """Phase-4 regrowth: emit noise-bucket violations for lines ADDED vs the merge-base of <ref>."""
    hits = _scan_added_noise(ref)
    for path, line_no, _b, rule_id, body in hits:
        print("%s\t%s:%d\t%s" % (rule_id, os.path.basename(path), line_no, body.strip()[:80]))
    return 1 if hits else 0


def _strip_line_numbers(lines, line_nos):
    """Pure helper for --fix: return `lines` with the 1-based indices in `line_nos` removed.
    Deletes high-to-low so earlier indices stay valid; out-of-range numbers are ignored."""
    out = list(lines)
    for ln in sorted({n for n in line_nos if 1 <= n <= len(out)}, reverse=True):
        del out[ln - 1]
    return out


def run_fix(ref):
    """Auto-strip the mechanically-removable NEW comment-noise this change added vs <ref>, in
    place: blank-comment runs (`cut-blank`) and decorative banners/dividers (`cut-decorative`).
    NEVER touches `comment-commented-out-code` — a code-like comment needs a human reword (delete
    the code or reword the prose), not blind deletion — those are reported for manual handling.
    Reads/writes with newline='' so existing line endings are preserved. The delta lint gate run
    AFTER this (pre-ship.sh / CI) stays the authority; --fix only removes what it proves is noise."""
    hits = _scan_added_noise(ref)
    by_file = {}
    manual = []
    for path, line_no, bucket, _rule_id, body in hits:
        if bucket in CUT_BUCKETS:
            by_file.setdefault(path, []).append(line_no)
        else:  # flag-commented-code — human reword, never auto-deleted
            manual.append((path, line_no, body))
    stripped = 0
    for path, line_nos in by_file.items():
        try:
            with open(path, "r", encoding="utf-8", errors="replace", newline="") as fh:
                lines = fh.read().split("\n")
        except OSError:
            continue
        kept = _strip_line_numbers(lines, line_nos)
        removed = len(lines) - len(kept)
        if removed:
            with open(path, "w", encoding="utf-8", errors="replace", newline="") as fh:
                fh.write("\n".join(kept))
            stripped += removed
    if stripped:
        print("comment_audit --fix: stripped %d new blank-run/decorative comment line(s) from "
              "%d file(s)" % (stripped, len(by_file)))
    if manual:
        print("comment_audit --fix: %d new commented-out-code line(s) need MANUAL handling "
              "(reword the prose or delete the code — NOT auto-stripped):" % len(manual),
              file=sys.stderr)
        for path, line_no, body in manual:
            print("  %s:%d  %s" % (os.path.basename(path), line_no, body.strip()[:80]), file=sys.stderr)
    if not stripped and not manual:
        print("comment_audit --fix: no new mechanically-removable comment-noise")
    return 0


def _file_ratio(text):
    """comment / (comment + code) for a file's text; 0.0 if no comment+code lines."""
    code = comment = 0
    for k in cl.classify_line_kinds(text):
        if k == "code" or k == "trailing":
            code += 1
        elif k == "full_comment":
            comment += 1
    denom = code + comment
    return (comment / denom) if denom else 0.0


def run_ratio_warn(ref, threshold=0.50):
    """ADVISORY (always exit 0): warn for each changed first-party C++ file whose comment ratio
    both RISES vs <ref> AND exceeds `threshold`. Well-documented files that don't get worse never
    warn. Never blocks — this is the soft half of the Phase-4 regrowth guard."""
    ref = _merge_base_or_ref(ref)
    changed = _git(["diff", "--name-only", "--diff-filter=ACMR", ref, "--",
                    *[r + "**" for r in SWEEP_ROOTS]]).split()
    warned = 0
    for f in changed:
        if not f.endswith(CPP_EXT) or not f.startswith(SWEEP_ROOTS) or any(s in f for s in EXCLUDE_SUBSTR):
            continue
        try:
            with open(f, "r", encoding="utf-8", errors="replace") as fh:
                head_text = fh.read()
        except OSError:
            continue
        base_p = subprocess.run(["git", "show", "%s:%s" % (ref, f)], capture_output=True, text=True, encoding="utf-8", errors="replace")
        base_text = base_p.stdout if base_p.returncode == 0 else ""  # new file → base ratio 0
        hr, br = _file_ratio(head_text), _file_ratio(base_text)
        if hr > br and hr > threshold:
            print("[comment-ratio] WARN %s: comment ratio %.0f%% (was %.0f%%) > %.0f%% — consider trimming"
                  % (f, hr * 100, br * 100, threshold * 100))
            warned += 1
    return 0  # advisory: never non-zero


def run_selftest():
    """Regression-guard the prose-vs-code discriminator (build-quality-velocity-hardening
    #7). Every `flag` fixture is real commented-out code that MUST stay flagged; every
    `prose` fixture is English rationale that previously false-fired and MUST NOT flag."""
    flag = [
        "// int x = 5;",
        "//   foo(bar);",
        "// if (cond) {",
        "// return result;",
        "// obj->method();",
        "// Foo::Bar baz = qux();",
        "// while (i < n) {",
        "// std::vector<int> v = make();",
        "// foo();",  # bare call WITH terminator — real commented-out code, must still flag
    ]
    prose = [
        "// reset the latch (mirroring the sibling early returns)",
        "// see computeValue() for details on the parser path",
        "// Resolve the fork point of ref and head (so the diff reflects only added lines)",
        "// drops cpr from the public include graph (about a hundred translation units)",
        "// note foo() does X",   # line-leading unterminated ident-call mid-prose — must NOT flag
        "// call reset() before reuse",
    ]
    fails = 0
    # selftest: asserts-failure — known commented-out-code fixtures must classify as flag-commented-code.
    for s in flag:
        b = classify_comment(s.strip(), s)
        if b != "flag-commented-code":
            print("FAIL: expected flag-commented-code, got %s for: %s" % (b, s))
            fails += 1
    for s in prose:
        b = classify_comment(s.strip(), s)
        if b == "flag-commented-code":
            print("FAIL: expected prose (not flagged) for: %s" % s)
            fails += 1
    # Pure strip-helper invariant used by --fix: remove the given 1-based lines, high-to-low,
    # ignoring out-of-range; survivors keep order. Guards the auto-strip's delete arithmetic.
    if _strip_line_numbers(["a", "b", "c", "d"], [2, 4, 99]) != ["a", "c"]:
        print("FAIL: _strip_line_numbers removed the wrong lines")
        fails += 1

    # Single intra-block separator vs blank-run discriminator (PR-4). A lone bare `//` between
    # two textual comment lines of the SAME block is an ALLOWED paragraph separator; a run of
    # 2+ bare `//` still flags every line in the run; a bare `//` not flanked by comment text
    # on both sides still flags. `is_allowed_blank_separator` takes file lines + a 1-based no.
    sep_ok = [
        # (lines, line_no, expected-allowed)
        (["// first paragraph of the block", "//", "// second paragraph of the block"], 2, True),
        (["// doc line one", "//", "// doc line two", "//", "// doc line three"], 2, True),
        # A 2+ bare run: BOTH bare lines must still flag (neither is an allowed separator).
        (["// text above", "//", "//", "// text below"], 2, False),
        (["// text above", "//", "//", "// text below"], 3, False),
        # Not inside a block: bare `//` against code / blank / file edge still flags.
        (["int x = 0;", "//", "int y = 1;"], 2, False),
        (["// only comment above", "//", ""], 2, False),
        (["//", "// text below"], 1, False),  # file-edge: no neighbor above
    ]
    for lines, line_no, expected in sep_ok:
        got = is_allowed_blank_separator(lines, line_no)
        if got != expected:
            print("FAIL: is_allowed_blank_separator expected %s got %s for line %d of %r"
                  % (expected, got, line_no, lines))
            fails += 1

    # Wrapped-SMATCHET_DEVIATION continuation detection: the marker line is protected
    # elsewhere; a long reason= wrapped across // lines yields continuation line numbers
    # that must be exempt (else clang-format wrapping loops the comment-noise gate).
    dev_cases = [
        # (lines, expected-continuation-line-numbers)
        (["    // SMATCHET_DEVIATION(rule=duplication; reason=a long rationale that",
          "    // clang-format wrapped (mirroring the sibling) so it reads as prose",
          "    // and closes here; owner=orch; revisit=never)",
          "    int realCode = 0;"], {2, 3}),
        # Single-line deviation: parens close on the marker line -> no continuations.
        (["    // SMATCHET_DEVIATION(rule=duplication; reason=single line, closes now)",
          "    foo();"], set()),
        # No deviation at all -> empty.
        (["    // ordinary comment", "    bar();"], set()),
        # #1760 fail-open: an unbalanced '(' in the reason prose must NOT leak the exemption past
        # the deviation's own comment lines. The block ends at the first non-comment line, so the
        # later commented-out code stays flaggable (only line 2, the deviation's own wrap, is exempt).
        (["    // SMATCHET_DEVIATION(rule=x; reason=see helper foo(bar for the gory",
          "    // details; owner=orch; revisit=never",
          "    int realCode = 0;",
          "    // int deadCode = 1;  // commented-out code that must stay flaggable"], {2}),
    ]
    for lines, expected in dev_cases:
        got = _deviation_continuation_lines(lines)
        if got != expected:
            print("FAIL: _deviation_continuation_lines expected %r got %r for %r"
                  % (expected, got, lines))
            fails += 1

    if fails:
        print("comment_audit --selftest: FAIL (%d)" % fails)
        return 1
    print("comment_audit --selftest: PASS (%d flag + %d prose fixtures + strip-helper + %d separator + %d deviation-wrap)"
          % (len(flag), len(prose), len(sep_ok), len(dev_cases)))
    return 0


def _utf8_stdio():
    # Source files (and reports/diffs echoing their comments) contain non-ASCII; force UTF-8 on
    # stdout/stderr so the platform default (cp1252 on Windows) can't crash with UnicodeEncodeError.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass


def main():
    _utf8_stdio()
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", metavar="PATH")
    ap.add_argument("--diff", metavar="REF")
    ap.add_argument("--fix", metavar="REF",
                    help="auto-strip NEW blank-run/decorative comment-noise vs REF, in place "
                         "(commented-out-code is reported for manual reword, never deleted)")
    ap.add_argument("--ratio-warn", metavar="REF")
    ap.add_argument("--selftest", action="store_true",
                    help="run the prose-vs-code discriminator fixtures and exit")
    args = ap.parse_args()
    if args.selftest:
        sys.exit(run_selftest())
    # Exit-code contract for the gate seams: 0 = clean, 1 = violations found (printed to stdout),
    # >=2 = infra failure (git/IO error) — so test-lint-rules.sh can fail CLOSED on >=2 without
    # mistaking a legitimate "violations found" (1) for an inability to run.
    if args.ratio_warn:
        try:
            sys.exit(run_ratio_warn(args.ratio_warn))
        except Exception as e:  # advisory mode, but never crash-as-clean
            print("comment_audit: ERROR (ratio-warn): %s" % e, file=sys.stderr)
            sys.exit(2)
    if args.diff:
        try:
            sys.exit(run_diff_mode(args.diff))
        except Exception as e:
            print("comment_audit: ERROR (diff): %s" % e, file=sys.stderr)
            sys.exit(2)
    if args.fix:
        try:
            sys.exit(run_fix(args.fix))
        except Exception as e:
            print("comment_audit: ERROR (fix): %s" % e, file=sys.stderr)
            sys.exit(2)
    grand, per_sub, file_reports = run_audit()
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump({"grand": grand, "per_subsystem": per_sub,
                       "files": {f: {k: v for k, v in r.items() if k != "candidates"}
                                 for f, r in file_reports}}, fh, indent=2)
    print_markdown(grand, per_sub)


if __name__ == "__main__":
    main()

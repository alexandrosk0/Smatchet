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
See docs/plans/active/reduce-source-comment-bloat.md.
"""

import argparse
import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import comment_lib as cl

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


def classify_comment(stripped, raw_line):
    """Classify a full-line comment into a taxonomy bucket id."""
    for rx in PROTECT_RE:
        if rx.search(raw_line):
            return "protect"
    if any(s in raw_line for s in PROTECT_SUBSTR):
        return "protect"
    if BLANK_COMMENT_RE.match(raw_line):
        return "cut-blank"
    if DECORATIVE_RE.match(raw_line):
        return "cut-decorative"
    if SHOUT_BANNER_RE.match(raw_line):
        return "cut-decorative"
    if DOC_OPEN_RE.match(stripped):
        return "judge-apidoc"
    if stripped.startswith("//") and CODE_LIKE_RE.match(raw_line) and not KEEP_NOTE_RE.search(raw_line):
        return "flag-commented-code"
    # plain // or block-continuation prose
    return "judge-rationale"


CUT_BUCKETS = ("cut-blank", "cut-decorative")  # mechanically strippable (Wave 1)


def _git(args):
    """Run a git command, raising on non-zero exit (silent git failures would corrupt the
    baseline count or the regrowth diff into a false-clean result)."""
    p = subprocess.run(["git"] + args, capture_output=True, text=True)
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


def run_diff_mode(ref):
    """Phase-4 regrowth: emit noise-bucket violations for lines ADDED vs <ref>."""
    rule_for = {
        "cut-blank": "comment-blank-run",
        "cut-decorative": "comment-decorative-banner",
        "flag-commented-code": "comment-commented-out-code",
    }
    diff = _git(["diff", "--unified=0", ref, "--", *[r + "**" for r in SWEEP_ROOTS]])
    cur_file = None
    cur_line = 0
    violations = []
    file_cache = {}

    def suppressed(path, line_no, rule_id):
        # Escape hatch: a `// SMATCHET_DEVIATION(rule=<rule_id>; ...)` on the line ABOVE the
        # flagged line suppresses it (the existing forward-only deviation grammar; no new syntax).
        try:
            if path not in file_cache:
                with open(path, "r", encoding="utf-8", errors="replace") as fh:
                    file_cache[path] = fh.read().split("\n")
            lines = file_cache[path]
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
                    if b in rule_for and not suppressed(cur_file, cur_line, rule_for[b]):
                        base = os.path.basename(cur_file)
                        violations.append(f"{rule_for[b]}\t{base}:{cur_line}\t{body.strip()[:80]}")
            cur_line += 1
    for v in violations:
        print(v)
    return 1 if violations else 0


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
        base_p = subprocess.run(["git", "show", "%s:%s" % (ref, f)], capture_output=True, text=True)
        base_text = base_p.stdout if base_p.returncode == 0 else ""  # new file → base ratio 0
        hr, br = _file_ratio(head_text), _file_ratio(base_text)
        if hr > br and hr > threshold:
            print("[comment-ratio] WARN %s: comment ratio %.0f%% (was %.0f%%) > %.0f%% — consider trimming"
                  % (f, hr * 100, br * 100, threshold * 100))
            warned += 1
    return 0  # advisory: never non-zero


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
    ap.add_argument("--ratio-warn", metavar="REF")
    args = ap.parse_args()
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
    grand, per_sub, file_reports = run_audit()
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump({"grand": grand, "per_subsystem": per_sub,
                       "files": {f: {k: v for k, v in r.items() if k != "candidates"}
                                 for f, r in file_reports}}, fh, indent=2)
    print_markdown(grand, per_sub)


if __name__ == "__main__":
    main()

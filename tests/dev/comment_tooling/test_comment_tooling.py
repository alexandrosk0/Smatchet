#!/usr/bin/env python3
"""Fixture tests for the reduce-source-comment-bloat tooling (Phase 0).

Asserts, per the plan's safety contract:
  - the literal-aware tokenizer never mistakes `//`/`/*` inside string/char/raw literals for a
    comment, and its code-token residue preserves all code;
  - the mechanical stripper removes ONLY the unambiguous `//`-line cut buckets and preserves the
    protect-list, doc bodies, block continuations, trailing-on-code comments, and commented-out
    code; and that stripping leaves the code-token residue byte-identical;
  - the analyzer classifies each comment into the expected taxonomy bucket.

Run via agents/scripts/core/test-comment-tooling.sh (auto-enrolled by test-all.sh). Zero deps (stdlib).
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEV = os.path.normpath(os.path.join(HERE, "..", "..", "..", "agents", "scripts", "core"))
sys.path.insert(0, DEV)

import comment_lib as cl       # noqa: E402
import comment_audit as audit  # noqa: E402
import comment_strip as strip  # noqa: E402

failures = []


def check(cond, msg):
    if not cond:
        failures.append(msg)
        print("  FAIL:", msg)
    else:
        print("  ok  :", msg)


# --- 1. tokenizer / residue: literals are never treated as comments -----------
print("[1] tokenizer literal-awareness")
check(cl.classify_line_kinds('auto s = "a//b/*c*/"; // tail\n')[0] == "trailing",
      "string with // and /* is code, trailing // is comment")
check('"a//b/*c*/"' in cl.code_token_residue('auto s = "a//b/*c*/"; // tail\n'),
      "string body with slashes survives residue")
check(cl.code_token_residue('auto r = R"(x // y /* z */)"; int n;\n') ==
      'auto r = R "(x // y /* z */)" ; int n ;',
      "raw-string body (// and /* inside) is captured as one literal token, never split into comments")
# the load-bearing property: a raw string is tokenized identically every time, so base==head
check(cl.code_tokens('R"(a // b)" x') == cl.code_tokens('R"(a // b)"   x'),
      "raw-string tokenization is whitespace-stable (consistent base vs head)")
check(cl.classify_line_kinds("char c = '/'; // x\n")[0] == "trailing",
      "char literal '/' is code")
check(cl.classify_line_kinds("/* a\n * b\n */\nint c;\n") == ["full_comment", "full_comment", "full_comment", "code"],
      "multi-line block comment lines are full_comment; code line is code")
check(cl.code_token_residue("// whole line\nint x;\n") == "int x ;",
      "full-line comment drops from residue; code remains (tokenized)")
check(cl.code_token_residue("int a/**/b;\n") == "int a b ;",
      "block comment between tokens separates them (a/**/b -> ['a','b'], never 'ab')")
# token-equivalence across clang-format reflow: '(' newline 'const'  ==  '(const'
check(cl.code_token_residue("f(\n    const int x);\n") == cl.code_token_residue("f(const int x);\n"),
      "reflow around punctuation is token-equivalent ('(\\n const' == '(const')")
check(cl.code_token_residue("int x;\n") != cl.code_token_residue("int y;\n"),
      "a real token change (x -> y) still differs")
# the regression that broke the first build: a // line must not eat following lines
check(cl.classify_line_kinds("// c1\nint a;\n// c2\nint b;\n") ==
      ["full_comment", "code", "full_comment", "code"],
      "line comment does NOT swallow subsequent code lines (newline resets state)")

# --- 2. mechanical stripper: cut only the safe buckets, preserve the rest ------
print("[2] mechanical stripper scope + safety")
SRC = "\n".join([
    "#include <x>",                              # 0 code
    "//",                                        # 1 cut-blank      -> REMOVE
    "// =====================",                  # 2 cut-decorative -> REMOVE
    "// 3. THE REST OF YOUR INCLUDES",           # 3 cut-decorative -> REMOVE
    "// This explains *why* the next line is subtle.",  # 4 rationale -> KEEP
    "/// Doc: returns the widget count.",        # 5 apidoc -> KEEP
    "/* block start",                            # 6 block -> KEEP
    " * continuation",                           # 7 block body -> KEEP
    " */",                                       # 8 block end -> KEEP
    "int* p = new Foo(); // custom-deleter",     # 9 trailing protect -> KEEP (whole line)
    "// SMATCHET_DEVIATION(rule=no-raw-new; reason=x; owner=y; revisit=never)",  # 10 protect -> KEEP
    "// oldCall(arg);",                          # 11 commented-out code -> KEEP (Wave 1 flags only)
    "} // namespace Foo",                        # 12 nav label (trailing) -> KEEP
    "int y = 1;",                                # 13 code
    "",
])
new_text, removed = strip.strip_file_text(SRC)
kept = new_text.split("\n")
check(removed == [2, 3, 4], "removed exactly the blank + 2 decorative lines (1-based 2,3,4)")
check("// This explains *why*" in new_text, "verbose rationale KEPT (Wave-2/LLM only)")
check("/// Doc: returns the widget count." in new_text, "/// doc comment KEPT")
check(" * continuation" in new_text, "block-body continuation KEPT")
check("// custom-deleter" in new_text, "trailing protect marker on code line KEPT")
check("SMATCHET_DEVIATION" in new_text, "SMATCHET_DEVIATION suppressor KEPT")
check("// oldCall(arg);" in new_text, "commented-out code KEPT in Wave 1 (flag-only)")
check("} // namespace Foo" in new_text, "namespace-close nav label KEPT")
# the load-bearing proof: stripping changed ONLY comments
check(cl.code_token_residue(SRC) == cl.code_token_residue(new_text),
      "code-token residue byte-identical before/after strip")

# --- 3. analyzer taxonomy classification --------------------------------------
print("[3] analyzer classification buckets")
cases = [
    ("//", "cut-blank"),
    ("// --------", "cut-decorative"),
    ("// 3. THE REST OF YOUR INCLUDES", "cut-decorative"),
    ("// custom-deleter", "protect"),
    ("// SMATCHET_DEVIATION(rule=x)", "protect"),
    ("// see docs/adr/0009-foo.md", "protect"),
    ("/* PILLAR2_WORKER_ONLY */", "protect"),
    ("} // namespace Foo", "protect"),
    ("/// returns the count", "judge-apidoc"),
    # PR #1112 regression: a BARE Javadoc/Doxygen doc-block opener on its own line is a doc
    # comment, NOT a decorative divider (DECORATIVE_RE would otherwise eat the second '*').
    ("/**", "judge-apidoc"),
    ("  /**", "judge-apidoc"),
    ("/*!", "judge-apidoc"),
    ("/*", "judge-rationale"),
    # Exactly-two-stars only: three-plus stars / star runs remain decorative banners.
    ("/***", "cut-decorative"),
    ("/****************/", "cut-decorative"),
    ("// foo(bar);", "flag-commented-code"),
    ("// This is a sentence explaining the non-obvious reason.", "judge-rationale"),
]
for line, want in cases:
    got = audit.classify_comment(line.strip(), line)
    check(got == want, f"classify {line!r} -> {want} (got {got})")

# --- 4. Phase-4 regrowth guard (diff + ratio modes, against a throwaway git repo) ---
import contextlib   # noqa: E402
import io           # noqa: E402
import subprocess   # noqa: E402
import tempfile     # noqa: E402

print("[4] Phase-4 regrowth guard (git diff modes)")

REL = "Source/Core/src/Foo/widget.cpp"   # inside SWEEP_ROOTS so the diff filter keeps it
# Enough real code lines that a clean base sits well under the 0.50 ratio threshold.
CLEAN = "\n".join(["int f%d() { return %d; }" % (i, i) for i in range(20)]) + "\n"


def _run_diff(ref):
    """Capture (exit_code, stdout) of run_diff_mode in the current cwd."""
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = audit.run_diff_mode(ref)
    return rc, buf.getvalue()


def _run_ratio(ref):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = audit.run_ratio_warn(ref)
    return rc, buf.getvalue()


def _git_q(repo, *args):
    subprocess.run(["git", "-C", repo] + list(args), check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def _write(repo, rel, text):
    p = os.path.join(repo, rel.replace("/", os.sep))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8") as fh:
        fh.write(text)


def _commit_base(repo, body):
    _git_q(repo, "init", "-q")
    _git_q(repo, "config", "user.email", "t@t")
    _git_q(repo, "config", "user.name", "t")
    _write(repo, REL, body)
    _git_q(repo, "add", "-A")
    _git_q(repo, "commit", "-q", "-m", "base")


def _in_repo(fn):
    """Run fn(repo) inside a fresh temp git repo, cwd switched to it, then restore + cleanup."""
    prev = os.getcwd()
    repo = tempfile.mkdtemp(prefix="cmt-regrowth-")
    try:
        os.chdir(repo)
        return fn(repo)
    finally:
        os.chdir(prev)
        # best-effort recursive cleanup (Windows: drop read-only on .git objects)
        import shutil
        shutil.rmtree(repo, ignore_errors=True)


# 4a — fresh noise FAILS: base clean, head adds all three noise buckets.
def case_fresh(repo):
    _commit_base(repo, CLEAN)
    noisy = CLEAN + "\n".join([
        "//",                       # comment-blank-run
        "// ==================",    # comment-decorative-banner
        "// oldCall(arg);",         # comment-commented-out-code
    ]) + "\n"
    _write(repo, REL, noisy)
    rc, out = _run_diff("HEAD")
    check(rc == 1, "fresh noise: diff-mode exits 1")
    check("comment-blank-run" in out, "fresh noise: flags comment-blank-run")
    check("comment-decorative-banner" in out, "fresh noise: flags comment-decorative-banner")
    check("comment-commented-out-code" in out, "fresh noise: flags comment-commented-out-code")
_in_repo(case_fresh)

# 4b — grandfathered PASSES: identical noise already in base, head changes only code → 0 added comments.
def case_grandfathered(repo):
    base = CLEAN + "//\n// ==================\n// oldCall(arg);\n"
    _commit_base(repo, base)
    _write(repo, REL, base + "int extra() { return 1; }\n")   # add code only
    rc, out = _run_diff("HEAD")
    check(rc == 0, "grandfathered noise (unchanged) passes: diff-mode exits 0")
    check(out.strip() == "", "grandfathered noise emits no violations")
_in_repo(case_grandfathered)

# 4c — SMATCHET_DEVIATION SUPPRESSES: noise added but with a deviation on the line above.
def case_deviation(repo):
    _commit_base(repo, CLEAN)
    suppressed = CLEAN + (
        "// SMATCHET_DEVIATION(rule=comment-commented-out-code; reason=x; owner=y; revisit=never)\n"
        "// oldCall(arg);\n"
    )
    _write(repo, REL, suppressed)
    rc, out = _run_diff("HEAD")
    check(rc == 0, "SMATCHET_DEVIATION above the line suppresses the flag (exit 0)")
    check("comment-commented-out-code" not in out, "deviation: commented-out-code not reported")
_in_repo(case_deviation)

# 4c2 — deviation separated by a BLANK line still suppresses (forward-only contract, blank-skip).
def case_deviation_blank(repo):
    _commit_base(repo, CLEAN)
    sup = CLEAN + (
        "// SMATCHET_DEVIATION(rule=comment-commented-out-code; reason=x; owner=y; revisit=never)\n"
        "\n"
        "// oldCall(arg);\n"
    )
    _write(repo, REL, sup)
    rc, out = _run_diff("HEAD")
    check(rc == 0 and "comment-commented-out-code" not in out,
          "deviation separated by a blank line still suppresses (blank-skip)")
_in_repo(case_deviation_blank)

# 4c3 — prefix-collision: rule=comment-blank must NOT suppress comment-blank-run (and vice-versa).
def case_deviation_prefix(repo):
    _commit_base(repo, CLEAN)
    # A wrong/over-specific rule id on the deviation must NOT suppress a different rule.
    sup = CLEAN + (
        "// SMATCHET_DEVIATION(rule=comment-blank; reason=x; owner=y; revisit=never)\n"
        "//\n"   # comment-blank-run — different rule-id; the comment-blank marker must not cover it
    )
    _write(repo, REL, sup)
    rc, out = _run_diff("HEAD")
    check(rc == 1 and "comment-blank-run" in out,
          "prefix-collision: rule=comment-blank does NOT suppress comment-blank-run")
_in_repo(case_deviation_prefix)

# 4c4 — hyphen-extended collision: rule=comment-blank-run-extra must NOT suppress comment-blank-run
#       (\b matches before '-', so the marker must end on a non-[\w-] char — CodeRabbit #598).
def case_deviation_hyphen_ext(repo):
    _commit_base(repo, CLEAN)
    sup = CLEAN + (
        "// SMATCHET_DEVIATION(rule=comment-blank-run-extra; reason=x; owner=y; revisit=never)\n"
        "//\n"   # comment-blank-run — the longer typo'd id must not cover it
    )
    _write(repo, REL, sup)
    rc, out = _run_diff("HEAD")
    check(rc == 1 and "comment-blank-run" in out,
          "hyphen-extended: rule=comment-blank-run-extra does NOT suppress comment-blank-run")
_in_repo(case_deviation_hyphen_ext)

# 4d — ratio warning is delta-aware AND non-blocking.
def case_ratio_rise(repo):
    _commit_base(repo, CLEAN)   # low ratio base
    # Add far more comment lines than code → ratio rises past 0.50.
    heavy = CLEAN + "\n".join(["// explanatory note number %d about nothing" % i for i in range(60)]) + "\n"
    _write(repo, REL, heavy)
    rc, out = _run_ratio("HEAD")
    check(rc == 0, "ratio warn never blocks (exit 0) even when it fires")
    check("[comment-ratio] WARN" in out and REL.split("/")[-1] in out,
          "ratio warn FIRES when a touched file's comment ratio rises past 0.50")
_in_repo(case_ratio_rise)

def case_ratio_stable(repo):
    _commit_base(repo, CLEAN)
    _write(repo, REL, CLEAN + "int more() { return 2; }\n")   # add code → ratio does not rise
    rc, out = _run_ratio("HEAD")
    check(rc == 0, "ratio warn non-blocking on code-only change (exit 0)")
    check("[comment-ratio] WARN" not in out,
          "ratio warn delta-aware: silent when ratio does not rise past threshold")
_in_repo(case_ratio_stable)


print()
if failures:
    print(f"FAILED: {len(failures)} assertion(s)")
    sys.exit(1)
print("ALL comment-tooling fixtures passed.")

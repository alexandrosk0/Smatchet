#!/usr/bin/env python3
"""Fixture tests for the reduce-source-comment-bloat tooling (Phase 0).

Asserts, per the plan's safety contract:
  - the literal-aware tokenizer never mistakes `//`/`/*` inside string/char/raw literals for a
    comment, and its code-token residue preserves all code;
  - the mechanical stripper removes ONLY the unambiguous `//`-line cut buckets and preserves the
    protect-list, doc bodies, block continuations, trailing-on-code comments, and commented-out
    code; and that stripping leaves the code-token residue byte-identical;
  - the analyzer classifies each comment into the expected taxonomy bucket.

Run via scripts/dev/test-comment-tooling.sh (auto-enrolled by test-all.sh). Zero deps (stdlib).
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEV = os.path.normpath(os.path.join(HERE, "..", "..", "..", "scripts", "dev"))
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
      'auto r = R"(x // y /* z */)"; int n;',
      "raw-string body with // and /* survives residue intact")
check(cl.classify_line_kinds("char c = '/'; // x\n")[0] == "trailing",
      "char literal '/' is code")
check(cl.classify_line_kinds("/* a\n * b\n */\nint c;\n") == ["full_comment", "full_comment", "full_comment", "code"],
      "multi-line block comment lines are full_comment; code line is code")
check(cl.code_token_residue("// whole line\nint x;\n") == "int x;",
      "full-line comment drops from residue; code remains")
check(cl.code_token_residue("int a/**/b;\n") == "int a b;",
      "block comment between tokens becomes a separator (a/**/b -> 'a b', never 'ab')")
check(cl.code_token_residue("foo(); /* c */\n") == "foo();",
      "block comment where space already exists collapses cleanly (no spurious diff)")
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
    ("// foo(bar);", "flag-commented-code"),
    ("// This is a sentence explaining the non-obvious reason.", "judge-rationale"),
]
for line, want in cases:
    got = audit.classify_comment(line.strip(), line)
    check(got == want, f"classify {line!r} -> {want} (got {got})")

print()
if failures:
    print(f"FAILED: {len(failures)} assertion(s)")
    sys.exit(1)
print("ALL comment-tooling fixtures passed.")

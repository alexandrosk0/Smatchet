#!/usr/bin/env python3
"""Literal-aware C++ comment tokenizer + classifier.

Shared by comment_audit.py (analyzer), comment_strip.py (mechanical stripper), and
assert-code-unchanged.sh (the safety gate, via --strip). The whole sweep's safety rests on
this scanner correctly distinguishing comments from `//`-or-`/*` sequences that live inside
string / char / raw-string literals.

Definitions (match docs/plans/active/reduce-source-comment-bloat.md § Baseline metrics):
  - A *comment line* is a full-line comment: the stripped line starts with `//`, `/*`, or `*`
    (the `*` covers continuation lines inside a `/* ... */` block).
  - A *trailing* comment on a code line counts as CODE, never stripped by the mechanical pass
    (removing it could drop a lint-exemption marker that lives on the code line).
"""

import re

# ---- character-level scanner -------------------------------------------------

# Per-character classification of a whole file into spans. We track multi-line state
# (block comments, raw strings) so callers can ask line-by-line questions reliably.

CODE = "code"
LINE_COMMENT = "line_comment"
BLOCK_COMMENT = "block_comment"


def _scan(text):
    """Yield (line_index, kind, had_code_before_comment) per source line.

    kind:
      'blank'        - empty / whitespace-only, not inside a block comment
      'full_comment' - the line's non-blank content is entirely a comment (// line, or a
                       /* */ block line, or a `*`/text continuation inside a block)
      'code'         - has code, no comment
      'trailing'     - has code AND a trailing comment on the same line
    Multi-line block comments: every line the block spans is 'full_comment' if it carries no
    code, else 'trailing' (code then `/* ...` opening) or 'code' handling is folded in.
    """
    i = 0
    n = len(text)
    line = 0
    # Per-line accumulators.
    saw_code = False  # non-comment, non-whitespace char seen on this line
    saw_comment = False  # any comment char seen on this line
    in_block = False  # inside /* */ spanning into this line
    block_only_so_far = True  # line so far is only block-comment continuation/whitespace

    # Whole-file state carried across chars.
    state = CODE
    raw_delim = None  # closing delimiter for a raw string, e.g. )tag"

    results = []

    def end_line():
        nonlocal saw_code, saw_comment, block_only_so_far
        if not saw_code and not saw_comment:
            kind = "blank"
        elif not saw_code:
            kind = "full_comment"
        elif saw_comment:
            kind = "trailing"
        else:
            kind = "code"
        results.append(kind)
        saw_code = False
        saw_comment = False
        block_only_so_far = True

    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""

        if c == "\n":
            # A line comment ends at EOL; an (invalid) unterminated string/char also must not
            # eat following lines. Block comments and raw strings legitimately span newlines.
            if state in (LINE_COMMENT, "string", "char"):
                state = CODE
            end_line()
            line += 1
            i += 1
            continue

        if state == CODE:
            if c == "/" and nxt == "/":
                state = LINE_COMMENT
                saw_comment = True
                i += 2
                continue
            if c == "/" and nxt == "*":
                state = BLOCK_COMMENT
                in_block = True
                saw_comment = True
                i += 2
                continue
            if c == '"':
                # raw string?  R"delim( ... )delim"
                # back up to see if this is preceded by R (and optional prefix u8/L/u/U)
                if _is_raw_string_start(text, i):
                    j = text.index('(', i)
                    raw_delim = ')' + text[i + 1:j] + '"'
                    state = "raw"
                    if not c.isspace():
                        saw_code = True
                    i = j + 1
                    continue
                state = "string"
                saw_code = True
                i += 1
                continue
            if c == "'":
                state = "char"
                saw_code = True
                i += 1
                continue
            if not c.isspace():
                saw_code = True
            i += 1
            continue

        if state == LINE_COMMENT:
            # consumed until newline (handled above); just advance
            i += 1
            continue

        if state == BLOCK_COMMENT:
            saw_comment = True
            if c == "*" and nxt == "/":
                state = CODE
                in_block = False
                i += 2
                continue
            i += 1
            continue

        if state == "string":
            saw_code = True
            if c == "\\":
                i += 2
                continue
            if c == '"':
                state = CODE
            i += 1
            continue

        if state == "char":
            saw_code = True
            if c == "\\":
                i += 2
                continue
            if c == "'":
                state = CODE
            i += 1
            continue

        if state == "raw":
            saw_code = True
            if text.startswith(raw_delim, i):
                i += len(raw_delim)
                state = CODE
                raw_delim = None
                continue
            i += 1
            continue

    # flush last line (no trailing newline)
    if i <= n and (saw_code or saw_comment or text.endswith("\n") is False):
        # only append if there was content OR file had no final newline with content
        if saw_code or saw_comment:
            end_line()
        elif not text.endswith("\n") and text:
            results.append("blank")
    return results


_RAW_PREFIX = re.compile(r'(?:u8|u|U|L)?R$')


def _is_raw_string_start(text, quote_idx):
    """True if the `"` at quote_idx begins a raw string literal (preceded by R/u8R/LR/...)."""
    # need an R immediately before, possibly with an encoding prefix, and a '(' ahead.
    prefix = text[max(0, quote_idx - 2):quote_idx]
    if not (prefix.endswith("R")):
        return False
    if not _RAW_PREFIX.search(text[max(0, quote_idx - 3):quote_idx]):
        # be permissive: a bare R" also counts
        if text[quote_idx - 1:quote_idx] != "R":
            return False
    return "(" in text[quote_idx:quote_idx + 64]


def classify_line_kinds(text):
    """Return a list of per-line kind strings: blank|full_comment|code|trailing."""
    return _scan(text)


# ---- comment stripping (for assert-code-unchanged) ---------------------------

def strip_comments(text):
    """Return `text` with every comment removed (literal-aware), code + literals intact.

    Used by the safety gate: comparing strip_comments(base) vs strip_comments(head) (after a
    further whitespace collapse) proves a diff touched only comments.
    """
    out = []
    i = 0
    n = len(text)
    state = CODE
    raw_delim = None
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state == CODE:
            if c == "/" and nxt == "/":
                state = LINE_COMMENT
                i += 2
                continue
            if c == "/" and nxt == "*":
                # Replace the whole block comment with a single space so adjacent tokens never
                # fuse (`a/**/b` must residue-compare as `a b`, not `ab`). code_token_residue then
                # collapses whitespace runs, so the lone space is harmless where space already existed.
                out.append(" ")
                state = BLOCK_COMMENT
                i += 2
                continue
            if c == '"' and _is_raw_string_start(text, i):
                j = text.index('(', i)
                raw_delim = ')' + text[i + 1:j] + '"'
                out.append(text[i:j + 1])
                state = "raw"
                i = j + 1
                continue
            if c == '"':
                state = "string"
                out.append(c)
                i += 1
                continue
            if c == "'":
                state = "char"
                out.append(c)
                i += 1
                continue
            out.append(c)
            i += 1
            continue
        if state == LINE_COMMENT:
            if c == "\n":
                state = CODE
                out.append(c)
            i += 1
            continue
        if state == BLOCK_COMMENT:
            if c == "*" and nxt == "/":
                state = CODE
                i += 2
                continue
            if c == "\n":
                out.append(c)  # preserve line structure
            i += 1
            continue
        if state == "string":
            out.append(c)
            if c == "\\":
                out.append(nxt)
                i += 2
                continue
            if c == '"':
                state = CODE
            i += 1
            continue
        if state == "char":
            out.append(c)
            if c == "\\":
                out.append(nxt)
                i += 2
                continue
            if c == "'":
                state = CODE
            i += 1
            continue
        if state == "raw":
            if text.startswith(raw_delim, i):
                out.append(raw_delim)
                i += len(raw_delim)
                state = CODE
                raw_delim = None
                continue
            out.append(c)
            i += 1
            continue
    return "".join(out)


def code_tokens(text):
    """Literal-aware C++ token list (whitespace + comments dropped).

    Token-equivalence is what the safety gate needs: removing a comment may let clang-format
    reflow code across lines AND change whitespace around punctuation (`(\\n const` -> `(const`)
    without changing the token stream. Whitespace-collapse can't see that; a token list can.
    Rules: a maximal [A-Za-z0-9_] run is one token (so `int x` stays two tokens, never `intx`);
    string/char/raw-string literals are captured whole; every other non-space char is its own
    token; comments are skipped (and thus separate adjacent identifier runs).
    """
    toks = []
    cur = []
    i, n = 0, len(text)

    def flush():
        if cur:
            toks.append("".join(cur))
            del cur[:]

    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c.isspace():
            flush()
            i += 1
            continue
        if c == "/" and nxt == "/":
            flush()
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "/" and nxt == "*":
            flush()
            i += 2
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                i += 1
            i += 2
            continue
        if c == '"' and _is_raw_string_start(text, i):
            flush()
            j = text.index("(", i)
            delim = ")" + text[i + 1:j] + '"'
            end = text.find(delim, j)
            end = (end + len(delim)) if end != -1 else n
            toks.append(text[i:end])
            i = end
            continue
        if c == '"' or c == "'":
            flush()
            q = c
            j = i + 1
            while j < n and text[j] != q:
                j += 2 if text[j] == "\\" else 1
            toks.append(text[i:min(j + 1, n)])
            i = j + 1
            continue
        if c.isalnum() or c == "_":
            cur.append(c)
            i += 1
            continue
        flush()
        toks.append(c)
        i += 1
    flush()
    return toks


def code_token_residue(text):
    """strip_comments + collapse all whitespace runs to single spaces + drop blank lines.

    The byte-identical comparison key for assert-code-unchanged: only comment/whitespace
    changes leave this residue unchanged.
    """
    # Space-joined token list: independent of all inter-token whitespace and line grouping, so
    # comment removal + clang-format reflow compares equal, while any real token change differs.
    return " ".join(code_tokens(text))


if __name__ == "__main__":
    import sys as _sys
    # `comment_lib.py --residue` : read source on stdin, print its code-token residue.
    # Used by assert-code-unchanged.sh to compare base vs head with comments+whitespace removed.
    if len(_sys.argv) > 1 and _sys.argv[1] == "--residue":
        # Read + write as UTF-8 bytes explicitly: source files (and this residue) can contain
        # non-ASCII, and the platform default stdio encoding (cp1252 on Windows) would crash.
        data = _sys.stdin.buffer.read().decode("utf-8", errors="replace")
        _sys.stdout.buffer.write(code_token_residue(data).encode("utf-8"))
    else:
        _sys.stderr.write("usage: comment_lib.py --residue   (reads stdin)\n")
        _sys.exit(2)

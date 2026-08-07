- 2026-08-07 · claude-code · [tooling] · P2 — `dup_audit.py` suppression is a **per-line** test, so a multi-line `SMATCHET_DEVIATION` comment whose last line is prose silently fails to suppress; neither `cpp-rules.md` nor the gate's own output says so

  Mechanics, from [`dup_audit.py:353-381`](../../../../agents/scripts/core/dup_audit.py):
  `_has_dup_deviation(line)` requires the `SMATCHET_DEVIATION` token **on that one
  line**, with `duplication` among the comma-separated `rule=` ids. `_suppressed`
  then checks (a) the nearest **non-blank line immediately above** the clone start
  and (b) **any line within** `[start_line, end_line]`. `run_diff` wraps it as
  `any(_suppressed(...) for ... in c.locations)`, so a marker on **either**
  occurrence exempts the pair.

  Cost when this bites: a deviation written in the natural way —

  ```cpp
  // SMATCHET_DEVIATION(rule=duplication; reason=the MCP and Lua-console window-layout
  // helpers are long-standing structural twins; unifying would couple independent
  // subsystems; owner=ui-host; revisit=2026-12-31)
  ```

  — does not suppress, because the nearest line above the clone is line 3, which
  carries no token. The gate reports a bare `[dup] FAIL a.cpp:111 <-> b.cpp:74`
  with no hint that a marker was present-but-ineffective, so the reader's first
  instinct is that the exemption text is wrong rather than its *shape*. Cost one
  round of guessing before reading the script.

  Two fixes, independent:

  1. **Doc** — add to [`cpp-rules.md`](../../../agent-rules/cpp-rules.md)
     § `SMATCHET_DEVIATION` grammar: *the whole `SMATCHET_DEVIATION(...)` must fit on
     a single line for the `duplication` rule; put explanatory prose on separate
     comment lines **above** it.* (`.clang-format` `ColumnLimit: 120` is the real
     constraint on how much reason text fits.)
  2. **Gate** — when a clone pair FAILs, scan a small window (say 5 lines) above the
     clone start for the literal string `SMATCHET_DEVIATION` and, if found without a
     matching single-line marker, emit
     `hint: a SMATCHET_DEVIATION comment is nearby but spans multiple lines — the marker must be on one line`.
     Turns a silent shape error into a self-explaining one.

  Related: the same file's `--diff` mode graduated `duplication` from WARN to
  **BLOCKING** on 2026-06-21, which `AGENTS.md` and `agents/core/code-review.md`
  still describe as WARN-first calibration — see the sibling entry.

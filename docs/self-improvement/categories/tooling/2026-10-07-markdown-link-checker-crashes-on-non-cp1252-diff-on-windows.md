# `test-markdown-links.sh` crashes on Windows when a changed doc holds a character cp1252 cannot decode

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-10-07
- **Observed on**: pre-ship for alexandrosk0/Smatchet#2330 on Windows. A new backlog entry quoted the warning-sign emoji, whose UTF-8 encoding contains byte 0x8f. cp1252 has no character for 0x8f.
- **Status**: open

## What happened

`_added_lines()` in `agent-layer/agents/scripts/core/test-markdown-links.sh` runs `git diff` through `subprocess.run(..., text=True)`. That decodes the output with the locale encoding, which is cp1252 on Windows. Byte 0x8f, or any other byte cp1252 leaves undefined, raises `UnicodeDecodeError` in the reader thread. `.stdout` comes back as `None`, and the next line dies with `AttributeError: 'NoneType' object has no attribute 'splitlines'`.

The check therefore fails, and pre-ship refuses the push. This is fail-closed, but it blocks a legitimate Windows push. CI on Linux (UTF-8) is not affected.

## Concrete next action

Decode git output as UTF-8 in every `subprocess.run` in the checker: `encoding="utf-8", errors="replace"` in place of `text=True`. Sweep the layer's other Python-in-bash checkers for the same `text=True` pattern on git output.

Bats coverage: a changed fixture doc containing U+26A0 U+FE0F runs clean with `PYTHONIOENCODING` unset and a cp1252 locale (or with the subprocess encoding forced).

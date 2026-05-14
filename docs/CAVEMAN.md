# Caveman — output-compression companion

[caveman](https://github.com/JuliusBrussee/caveman) is a multi-harness skill (Claude Code, Codex, Gemini, Cursor, Windsurf, Cline, Copilot, 30+ more) that compresses agent output while preserving technical accuracy. README advertises ~75% output-token cuts and a 65% benchmark average. Preserves code, paths, URLs, and structural elements **byte-for-byte** — validation markers, severity-tagged punch lists, `file:line` references, and the `## Self-improvement` section convention used throughout this repo all survive intact. Only surrounding prose gets compressed.

## Install (per-user, system-wide — safe to re-run)

```bash
# bash / WSL / Git Bash / MSYS2
curl -fsSL https://raw.githubusercontent.com/JuliusBrussee/caveman/main/install.sh | bash

# Windows PowerShell
irm https://raw.githubusercontent.com/JuliusBrussee/caveman/main/install.ps1 | iex
```

## Use with Smatchet

Recommended default: **`/caveman full`** at session start. Compresses everything including delegated agents until session end. Switch to `/caveman lite` if you want more nuance in design-doc / security-review outputs from the Opus-tier agents (`architect`, `perf-detective`, `spike-hunter`, `security-review`). Exit with "normal mode".

Other useful skills:

- `/caveman-commit` — Conventional Commit messages ≤ 50 chars
- `/caveman-review` — one-line PR comments
- `/caveman-stats` — session token usage + lifetime savings
- `/caveman-compress <file>` — rewrite memory files in caveman-speak (~46% input-token savings every session)

## Where it pays off

Caveman's value scales with agent complexity tier: Opus-tier agents in this repo emit the longest reports (design docs, perf write-ups, attack-surface findings) and benefit most from compression — that's where the dollar savings concentrate. Subsystem specialists at `low` complexity are already terse; caveman's compression there is marginal but harmless.

## Trade-off worth knowing

Caveman compresses *output* tokens, not thinking tokens. Brain stays full size; only the mouth shrinks. Combined with the read-only Opus agents in this repo (`architect`, `perf-detective`, `spike-hunter`, `security-review`, `code-review`), caveman tightens the most expensive part of each delegated call without altering reasoning quality.

---
name: agent-tokens
description: Report per-agent token usage for the current Claude Code session (default), all sessions (--all), or a time window (--since <N>{h|d|w}). Reads .claude/.agent-tokens.jsonl produced by the SubagentStop hook and prints a fixed-width table with dollar estimates per Anthropic public pricing.
---

# /agent-tokens

Print the per-agent token usage report for the current Smatchet project.

## How to invoke

Run the report script and emit its stdout verbatim into the chat. The user-provided arguments after `/agent-tokens` pass through unchanged.

```bash
python "$CLAUDE_PROJECT_DIR/scripts/agent-tokens-report.py" $ARGUMENTS
```

If `$CLAUDE_PROJECT_DIR` is unset, fall back to the repo root (`$(pwd)`).

## Expected arguments

- *(none)* — current session only (default).
- `--all` — lifetime, every row in the JSONL.
- `--since 24h` / `--since 7d` / `--since 2w` — time window relative to now.

## Output

A fixed-width table:

```
Session <id> (<N> agent calls)

Agent                Model     Calls        In       Out     Cache   Est. USD
------------------------------------------------------------------------------
perf-detective       opus          2     20.0k      3.5k     34.0k  $ 0.6825
command-system       sonnet        1      4.0k       600      8.0k  $ 0.0234
------------------------------------------------------------------------------
Total                              3     24.0k      4.1k     42.0k  $ 0.7059

Pricing cutoff: 2026-05 (update in scripts/agent-tokens-report.py).

Lifetime: <N> calls across <K> sessions, $<Z>.
```

## Notes

- Data source: `$CLAUDE_PROJECT_DIR/.claude/.agent-tokens.jsonl`, written by the `SubagentStop` hook at `.claude/hooks/agent-token-log.py`.
- The JSONL is gitignored; reports are per-machine.
- Pricing table lives at the top of `scripts/agent-tokens-report.py`. Review quarterly; banner prints the cutoff so stale prices are visible.
- If the JSONL is missing or empty, the script prints a friendly hint instead of an error.

See `docs/AGENT_TOKEN_TRACKING.md` for the full design.

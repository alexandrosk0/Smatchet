---
name: scratchpad-recall
description: Recall facts from prior session scratchpads — list archives, show a specific archive, or grep across all archives. Use when the user references "last session", "yesterday's run", "what did <agent> find earlier", or any cross-session continuity question. Archives live at `.session-context.archive/` and are written by `scripts/clear-session-context.sh` on SessionStart. Read-only — never writes.
version: 1
---

# scratchpad-recall

Cross-session recall over the orchestrator's per-session scratchpad archive.

## Layout

- **Live scratchpad**: `.session-context.md` (current session, append-only by SubagentStop hook).
- **Archive dir**: `.session-context.archive/<ts>-<sid8>.md` (one file per prior session, written on SessionStart rotation).
- Both are gitignored. Archives are never auto-pruned.

Filename grammar: `<ISO-UTC-timestamp-with-dashes>-<8char-session-id>.md` — e.g. `2026-05-14T08-22-13Z-a1b2c3d4.md`. Lexical sort = chronological sort.

## When to invoke

User-visible triggers (orchestrator routes here without being asked):

- "last session", "yesterday", "the previous run"
- "what did <agent> say earlier / before / last time"
- "did we already investigate X"
- "pick up where we left off"

Do NOT invoke for in-session recall — `.session-context.md` is already in the orchestrator's view.

## Operations

All shell-driven. Cheap. No state.

**List recent archives (newest first):**

```bash
ls -1t .session-context.archive/ 2>/dev/null | head -n 10
```

**Show one archive:**

```bash
cat .session-context.archive/<filename>.md
```

Prefer `Read` over `cat` when the orchestrator wants the content in its own context window.

**Grep across all archives** (e.g. "find every prior mention of `TicketGridModel`"):

```bash
grep -rn --include='*.md' '<pattern>' .session-context.archive/
```

**Find archives by agent** (every archive `agent-token-log.py` wrote a header for `perf-detective`):

```bash
grep -rln --include='*.md' '^## perf-detective ·' .session-context.archive/
```

## Output contract

When the orchestrator surfaces archived content to the user, prefix the quote with the archive filename so timestamps are visible:

```
From .session-context.archive/2026-05-14T08-22-13Z-a1b2c3d4.md:
  > <relevant line(s)>
```

## Anti-patterns

- **Do not modify archives.** The skill is read-only. Treat archives as immutable history.
- **Do not pull entire archives into context "just in case".** Grep first, then `Read` the matched file. A pop-up grep returns ~1KB; reading a whole archive can be 50KB+.
- **Do not invoke on every turn.** Only when the user signals cross-session intent. The live scratchpad covers the current session.
- **Do not promise persistence across machine moves.** Archives are local + gitignored; they do not sync.

## See also

- AGENTS.md § Session scratchpad protocol — full lifecycle.
- `scripts/clear-session-context.sh` — the rotation/archive script.
- `agents/_shared/token-tracking/agent-token-log.py` — the SubagentStop appender.

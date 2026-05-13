# Agent token tracking

## Context

Smatchet's 17-agent system delegates work across Opus / Sonnet / Haiku tiers. Cost visibility today: zero. Caveman reports compression savings, vexp reports semantic-search hits, but per-agent token spend per session is invisible. Need a way to see which agents burned the most tokens, what the session cost, and whether the delegation routing actually saves money vs running everything in the orchestrator.

Three layers, smallest viable subset first:

1. **B** — `SubagentStop` hook that appends one line of usage to a per-project JSONL.
2. **D** — `/agent-tokens` slash-skill that reads the JSONL and emits a session + lifetime report with dollar estimates.
3. **C** — statusline badge showing per-session running totals next to the existing caveman badge.

Skip **A** (agent self-report) — agents guess ±30%; B's transcript-derived numbers are authoritative.

## Layer B — JSONL log via `SubagentStop` hook

### File

`$CLAUDE_PROJECT_DIR/.claude/.agent-tokens.jsonl` — one JSON object per line, append-only.

Schema (one line per subagent completion):

```json
{"ts":"2026-05-13T22:34:56Z","session":"<id>","agent":"perf-detective","model":"opus","in":12340,"out":2103,"cache_create":4200,"cache_read":18200,"duration_ms":42118}
```

Fields:
- `ts` — ISO-8601 UTC; the moment the subagent stopped.
- `session` — Claude Code session id (groups calls within one user session).
- `agent` — subagent `name` field (e.g. `perf-detective`). Falls back to `unknown` if Claude Code does not pass it.
- `model` — model the subagent ran on (parsed from transcript `usage.model` or first assistant message).
- `in` / `out` — total `input_tokens` / `output_tokens` summed across all assistant messages in the subagent transcript.
- `cache_create` / `cache_read` — `cache_creation_input_tokens` / `cache_read_input_tokens`. Zero if absent.
- `duration_ms` — subagent wall-clock duration if exposed; null otherwise.

### Hook

`.claude/hooks/agent-token-log.sh` (Bash; runs under MSYS2 / Git Bash on Windows).

Wired to `SubagentStop` event with `matcher: ""` (all subagents) in `.claude/settings.json`:

```json
"SubagentStop": [
  {
    "matcher": "",
    "hooks": [
      { "type": "command", "command": "$CLAUDE_PROJECT_DIR/.claude/hooks/agent-token-log.sh", "timeout": 5000 }
    ]
  }
]
```

Hook responsibilities:

1. Read stdin JSON from Claude Code.
2. Extract `session_id`, `transcript_path`, and whatever subagent-identifying field Claude Code provides (`subagent_type`, `agent_name`, or fall back to parsing the transcript's first message).
3. Read the transcript file (JSONL of message objects). Sum `usage.input_tokens`, `usage.output_tokens`, `usage.cache_creation_input_tokens`, `usage.cache_read_input_tokens` across assistant messages.
4. Append one JSONL line to `$CLAUDE_PROJECT_DIR/.claude/.agent-tokens.jsonl`.
5. Silent on success; stderr on parse failure (don't block the user).

Safety: file locked via short-lived `>>` append (Linux/MSYS2 single-write append is atomic for short lines). Skip + warn on stale transcript path. Capped at one log line per call.

### Gitignore

Add `.claude/.agent-tokens.jsonl` — per-machine usage data, not for the repo.

## Layer D — `/agent-tokens` slash-skill

### Skill file

`.claude/skills/agent-tokens/SKILL.md` — Claude Code project-scoped skill.

When the user types `/agent-tokens` (optionally `/agent-tokens --all` or `/agent-tokens --since 7d`), the skill runs `scripts/agent-tokens-report.sh` and emits its stdout into chat.

### Report script

`scripts/agent-tokens-report.sh` — pure Bash + `jq`. Inputs:

- `--all` — lifetime (default: current session only, matched by latest `session` id in the JSONL).
- `--since <N>{h|d|w}` — time-bounded window (e.g. `--since 24h`).

Logic:

1. Detect current session id (read most-recent `session` from JSONL, OR pull from environment if Claude Code exposes it).
2. Filter rows: session-only by default, lifetime with `--all`, time-window with `--since`.
3. Group by `agent` + `model`. Sum `in` / `out` / `cache_*` / call count.
4. Multiply by hardcoded pricing table (per million tokens):

   | Model | input | cache_create | cache_read | output |
   |---|---|---|---|---|
   | opus | $15 | $18.75 | $1.50 | $75 |
   | sonnet | $3 | $3.75 | $0.30 | $15 |
   | haiku | $0.80 | $1.00 | $0.08 | $4 |

   Pricing as of 2026-05; update in one place: top of the script. Document review cadence quarterly.
5. Emit a fixed-width table:

   ```
   Session 2026-05-13 (4h 22m, 11 agent calls)

   Agent              Model    Calls   In       Out     Cache    Est. USD
   perf-detective     opus     2       24.0k    4.2k    36.4k    $0.21
   spike-hunter       opus     1       8.1k     1.4k    12.0k    $0.07
   command-system     sonnet   4       18.0k    3.2k    52.0k    $0.05
   mechanic           haiku    3       9.5k     1.1k    14.0k    $0.01
   ...
   Total                              59.6k    9.9k    114.4k    $0.34

   Lifetime: 247 calls, $12.81 across 12 sessions.
   ```

6. Exit 0 with the table on stdout. Skill captures stdout as the assistant message.

## Layer C — statusline badge

The existing caveman statusline already runs `~/.claude/hooks/caveman-statusline.{sh,ps1}` per refresh. Replace it with a wrapper that emits both badges. New file:

`.claude/hooks/agents-statusline.sh` (and `.ps1`):

1. Read the last N (e.g. 50) lines of `$CLAUDE_PROJECT_DIR/.claude/.agent-tokens.jsonl`.
2. Filter to current session (by id).
3. Sum tokens per agent; pick the top 2-3 by spend.
4. Emit: `[AGENTS] 🤖 perf-detective 14k · spike-hunter 8k · total 28k`.
5. Concat with caveman's existing output (which the wrapper invokes first).

Update `~/.claude/settings.json` (user-scope) to point `statusLine.command` at the new wrapper. The repo's `.claude/settings.json` is project-scope and doesn't carry `statusLine` — keep it that way; statusline is per-user.

Caveats: statusline runs on every refresh — keep parsing under 100ms. Tail the JSONL, don't full-scan. Skip silently if the JSONL is missing.

## Files added

- `docs/AGENT_TOKEN_TRACKING.md` — this doc.
- `agents/_shared/token-tracking/agent-token-log.py` — canonical hook source (Python; mirror at `.claude/hooks/agent-token-log.py`).
- `agents/_shared/token-tracking/agents-statusline.py` — canonical statusline source (mirror at `.claude/hooks/agents-statusline.py`).
- `agents/_shared/token-tracking/SKILL.md` — canonical slash-skill source (mirror at `.claude/skills/agent-tokens/SKILL.md`).
- `agents/_shared/token-tracking/README.md` — README for the canonical dir.
- `scripts/agent-tokens-report.py` — harness-agnostic CLI report. No mirror.

## Files modified

- `.claude/settings.json` — `SubagentStop` hook entry pointing at the mirror at `.claude/hooks/agent-token-log.py`.
- `.gitignore` — ignore `.claude/.agent-tokens.jsonl`.
- `scripts/sync-agents.sh` + `scripts/sync-agents.ps1` — extended to mirror the `agents/_shared/token-tracking/` tree into `.claude/hooks/` + `.claude/skills/agent-tokens/`.
- `scripts/check-agents-mirror.sh` — drift check now covers the token-tracking mirror paths too.
- `AGENTS.md` — § Agent file locations documents the dual-location convention for both `agents/*.md` and `agents/_shared/`.

## Migration / commit order

1. **Commit 1 — Design doc** (this file, `wip:` per Project rules § Plan-doc safety).
2. **Commit 2 — Layer B**: hook + settings.json wiring + gitignore. Test: spawn any subagent, confirm one new JSONL line.
3. **Commit 3 — Layer D**: skill file + report script. Test: `/agent-tokens` shows the line from commit 2.
4. **Commit 4 — Layer C**: statusline wrapper. Test: badge appears after the next status refresh.

## Verification

After each layer:

```bash
# B
cat .claude/.agent-tokens.jsonl | jq .   # well-formed JSON per line
wc -l .claude/.agent-tokens.jsonl        # ≥ 1 after first subagent call

# D
bash scripts/agent-tokens-report.sh                 # session report
bash scripts/agent-tokens-report.sh --all           # lifetime report
bash scripts/agent-tokens-report.sh --since 24h     # time window

# C
# (visual — restart Claude Code or trigger a refresh)
```

End-to-end check: invoke `code-review` once. Confirm a JSONL row appears, `/agent-tokens` shows the row in the session report, statusline updates next refresh.

## Open assumptions

- `SubagentStop` hook receives `transcript_path` in stdin. If absent → fallback to walking `~/.claude/projects/<sanitized-cwd>/transcripts/` for the newest transcript. Add this fallback only if commit 2 testing reveals the field is missing.
- Subagent name is in stdin as `subagent_type`. If not, parse from transcript metadata or stash agent name in the transcript filename. Decide during commit 2.
- Pricing accuracy: prices change. Skill prints a `Pricing: 2026-05` banner so reports note the cutoff. Quarterly review.

## Out of scope

- Per-tool token attribution inside a subagent (e.g. how much did this `Read` cost). Subagent-grain is enough.
- Cross-machine aggregation. Each machine writes its own JSONL.
- Anthropic API direct billing reconciliation. Local estimates are good enough for "is delegation paying off" questions.
- Export to CI / external dashboards. JSONL stays human-readable for now.

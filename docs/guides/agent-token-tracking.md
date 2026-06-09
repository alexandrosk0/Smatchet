<!-- tier: portable -->
# Agent token tracking

## Context

Smatchet's 18-agent system delegates work across Opus / Sonnet / Haiku tiers. Cost visibility today: zero. Caveman reports compression savings, but per-agent token spend per session is invisible. Need a way to see which agents burned the most tokens, what the session cost, and whether the delegation routing actually saves money vs running everything in the orchestrator.

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
{"ts":"2026-05-13T22:34:56Z","session":"<id>","agent":"perf-detective","model":"opus","model_full":"claude-opus-4-7-20260501","in":12340,"out":2103,"cache_create":4200,"cache_read":18200,"duration_ms":42118}
```

Fields:
- `ts` — ISO-8601 UTC; the moment the subagent stopped.
- `session` — Claude Code session id (groups calls within one user session).
- `agent` — subagent `name` field (e.g. `perf-detective`). Falls back to `unknown` if Claude Code does not pass it.
- `model` — model family (`opus`, `sonnet`, `haiku`, or `unknown`) parsed from the full transcript model id.
- `model_full` — full model id from the first assistant message when available; used for version-specific pricing.
- `in` / `out` — total `input_tokens` / `output_tokens` summed across all assistant messages in the subagent transcript.
- `cache_create` / `cache_read` — `cache_creation_input_tokens` / `cache_read_input_tokens`. Zero if absent.
- `duration_ms` — subagent wall-clock duration if exposed; null otherwise.

### Hook

`agents/_shared/token-tracking/agent-token-log.py` is the canonical Python hook source. `agents/scripts/core/setup-harness.sh claude-code` hardlinks it to `.claude/hooks/agent-token-log.py` so canonical edits propagate without a sync step.

Wired to `SubagentStop` event with `matcher: ""` (all subagents) in `.claude/settings.json`:

```json
"SubagentStop": [
  {
    "matcher": "",
    "hooks": [
      { "type": "command", "command": "python \"$CLAUDE_PROJECT_DIR/.claude/hooks/agent-token-log.py\"", "timeout": 5000 }
    ]
  }
]
```

Hook responsibilities:

1. Read stdin JSON from Claude Code.
2. Extract `session_id`, `transcript_path`, and whatever subagent-identifying field Claude Code provides (`subagent_type`, `subagent_name`, `agent`, `agent_name`, or `tool_input.subagent_type`).
3. Read the transcript file (JSONL of message objects). Sum `usage.input_tokens`, `usage.output_tokens`, `usage.cache_creation_input_tokens`, `usage.cache_read_input_tokens` across assistant messages.
4. Append one JSONL line to `$CLAUDE_PROJECT_DIR/.claude/.agent-tokens.jsonl`.
5. Silent on success; stderr on parse failure (don't block the user).

Safety: append one short JSON line per hook invocation; warn but do not fail on stale transcript paths. Missing transcripts produce a zero-token row with `note:"no-transcript"` so hook problems are visible without blocking the user.

### Gitignore

Add `.claude/.agent-tokens.jsonl` — per-machine usage data, not for the repo.

## Layer D — `/agent-tokens` slash-skill

### Skill file

`.claude/skills/agent-tokens/SKILL.md` — Claude Code project-scoped skill.

When the user types `/agent-tokens` (optionally `/agent-tokens --all` or `/agent-tokens --since 7d`), the skill runs `agents/scripts/core/agent-tokens-report.py` and emits its stdout into chat.

### Report script

`agents/scripts/core/agent-tokens-report.py` — pure Python, no `jq` dependency. Inputs:

- `--all` — lifetime (default: current session only, matched by latest `session` id in the JSONL).
- `--since <N>{h|d|w}` — time-bounded window (e.g. `--since 24h`).

Logic:

1. Detect current session id (read most-recent `session` from JSONL, OR pull from environment if Claude Code exposes it).
2. Filter rows: session-only by default, lifetime with `--all`, time-window with `--since`.
3. Group by `agent` + `model`. Sum `in` / `out` / `cache_*` / call count.
4. Resolve pricing from `model_full` when present, falling back to the `model` family, then multiply by the hardcoded pricing table (per million tokens):

   | Pricing key | Applies to | input | cache_create | cache_read | output |
   |---|---|---|---|---|---|
   | `opus` | Opus 4.5+ | $5 | $6.25 | $0.50 | $25 |
   | `opus_legacy` | Opus 4.1 / 4 / 3 | $15 | $18.75 | $1.50 | $75 |
   | `sonnet` | Sonnet 4.x / 3.7 | $3 | $3.75 | $0.30 | $15 |
   | `haiku` | Haiku 4.5+ | $1 | $1.25 | $0.10 | $5 |
   | `haiku_3_5` | Haiku 3.5 | $0.80 | $1.00 | $0.08 | $4 |
   | `haiku_3` | Haiku 3 | $0.25 | $0.30 | $0.03 | $1.25 |

   Pricing as of 2026-05; update both `agents/scripts/core/agent-tokens-report.py` and `agents/_shared/token-tracking/agents-statusline.py`. Document review cadence quarterly.
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

The existing caveman statusline may provide `~/.claude/hooks/caveman-statusline.{sh,ps1}`. The Smatchet wrapper invokes it best-effort first, then appends the agent-token badge. Canonical file:

`agents/_shared/token-tracking/agents-statusline.py` (mirror at `.claude/hooks/agents-statusline.py`):

1. Read the last N lines (`MAX_TAIL_LINES`, currently 200) of `$CLAUDE_PROJECT_DIR/.claude/.agent-tokens.jsonl`.
2. Filter to current session (by id).
3. Sum tokens per agent; pick the top 2-3 by spend.
4. Emit: `[AGENTS] <top agents> · total <tokens> · <cost>`.
5. Concat with caveman's existing output (which the wrapper invokes first).

Update `~/.claude/settings.json` (user-scope) to point `statusLine.command` at `python "<repo>/.claude/hooks/agents-statusline.py"`. The repo's `.claude/settings.json` is project-scope and doesn't carry `statusLine` — keep it that way; statusline is per-user.

Caveats: statusline runs on every refresh — keep parsing under 100ms. Tail the JSONL, don't full-scan. Skip silently if the JSONL is missing.

## Files added

- `docs/guides/agent-token-tracking.md` — this doc.
- `agents/_shared/token-tracking/agent-token-log.py` — canonical hook source (Python; linked into `.claude/hooks/agent-token-log.py` by `setup-harness.sh`).
- `agents/_shared/token-tracking/agents-statusline.py` — canonical statusline source (linked into `.claude/hooks/agents-statusline.py`).
- `agents/_shared/token-tracking/SKILL.md` — canonical slash-skill source (linked into `.claude/skills/agent-tokens/SKILL.md`).
- `agents/_shared/token-tracking/README.md` — README for the canonical dir.
- `agents/scripts/core/agent-tokens-report.py` — harness-agnostic CLI report. Lives in `agents/scripts/core/`, invoked the same way from any harness.

## Files modified

- `.claude/settings.json` — `SubagentStop` hook entry pointing at `.claude/hooks/agent-token-log.py` (hardlinked to canonical).
- `.gitignore` — ignore `.claude/.agent-tokens.jsonl`.
- `agents/scripts/core/setup-harness.sh` — links the canonical `agents/_shared/token-tracking/` tree into `.claude/hooks/` + `.claude/skills/agent-tokens/` so the harness picks up canonical edits with no sync step.
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
python agents/scripts/core/agent-tokens-report.py                 # session report
python agents/scripts/core/agent-tokens-report.py --all           # lifetime report
python agents/scripts/core/agent-tokens-report.py --since 24h     # time window

# C
# (visual — restart Claude Code or trigger a refresh)
```

End-to-end check: invoke `code-review` once. Confirm a JSONL row appears, `/agent-tokens` shows the row in the session report, statusline updates next refresh.

## Open assumptions

- `SubagentStop` usually receives `transcript_path` in stdin. If future runs produce many `note:"no-transcript"` rows, add a fallback that walks `~/.claude/projects/<sanitized-cwd>/transcripts/` for the newest transcript.
- Subagent name usually appears as `subagent_type`, `subagent_name`, `agent`, `agent_name`, or `tool_input.subagent_type`; otherwise the hook records `unknown`.
- Pricing accuracy: prices change. Reports print a `Pricing cutoff: 2026-05` banner so stale prices are visible. Quarterly review.

## Out of scope

- Per-tool token attribution inside a subagent (e.g. how much did this `Read` cost). Subagent-grain is enough.
- Cross-machine aggregation. Each machine writes its own JSONL.
- Anthropic API direct billing reconciliation. Local estimates are good enough for "is delegation paying off" questions.
- Export to CI / external dashboards. JSONL stays human-readable for now.

# `agents/_shared/token-tracking/`

Canonical sources for the agent token-tracking infrastructure (Layers B / D / C from [`docs/AGENT_TOKEN_TRACKING.md`](../../../docs/AGENT_TOKEN_TRACKING.md)).

These files are **harness-agnostic in intent**: the Python logic, JSONL schema, pricing table, and CLI report all work without any Claude-Code-specific assumption. The wiring INTO a specific harness lives in that harness's mirror tree (`.claude/` for Claude Code).

## Files

| Canonical | Mirror (Claude Code) | Purpose |
|---|---|---|
| `agent-token-log.py` | `.claude/hooks/agent-token-log.py` | Reads stdin JSON describing a finished subagent call (`session_id`, `subagent_type`/`subagent_name`/`agent`, `transcript_path`). Parses the transcript JSONL, sums `input_tokens` / `output_tokens` / `cache_*_tokens` across assistant messages, appends one JSONL row to `$CLAUDE_PROJECT_DIR/.claude/.agent-tokens.jsonl`. |
| `agents-statusline.py` | `.claude/hooks/agents-statusline.py` | Tails the JSONL, renders a one-line statusline badge with the top-N agents for the current session and the running cost. Optionally invokes caveman's statusline first to concatenate both badges. |
| `SKILL.md` | `.claude/skills/agent-tokens/SKILL.md` | Slash-skill definition. `/agent-tokens [--all|--since 24h]` shells out to `scripts/agent-tokens-report.py` and emits the report into chat. |

`scripts/agent-tokens-report.py` is **already harness-agnostic** — it stays in `scripts/` (no mirror), and is invoked the same way from any harness.

## Sync

`.claude/` copies are **auto-generated** by `scripts/sync-agents.sh` (bash) or `scripts/sync-agents.ps1` (PowerShell). The sync script injects an `AUTO-GENERATED MIRROR ... DO NOT EDIT` banner at the top of each mirror:

- `.py` files: comment line right after the shebang.
- `SKILL.md`: YAML-comment line right after the opening `---` (so frontmatter parsing still works).

`scripts/check-agents-mirror.sh` verifies the mirror matches canonical; CI / pre-commit friendly.

**Edit canonical only.** Never edit files under `.claude/hooks/agent-token-log.py`, `.claude/hooks/agents-statusline.py`, or `.claude/skills/agent-tokens/SKILL.md` directly — your changes get overwritten on the next sync.

## Wiring for other harnesses

The hook + statusline + skill names + paths are Claude-Code conventions, but the underlying contract is portable:

- **Hook contract**: any harness that fires an event when a delegated agent finishes can pipe `{"session_id":..., "subagent_type":..., "transcript_path":...}` to `agent-token-log.py` and get a JSONL row.
- **Transcript format**: assumes Anthropic-style transcripts where assistant messages carry a `usage` dict with `input_tokens` / `output_tokens` / `cache_creation_input_tokens` / `cache_read_input_tokens`. Harnesses using non-Anthropic transcripts need a translator.
- **Statusline**: anything that runs a shell command for a status badge can invoke `agents-statusline.py` directly.
- **Report**: `scripts/agent-tokens-report.py` is just a CLI; bind it to whatever skill / shortcut your harness uses.

See `AGENTS.md` § Harness adapter for the wider mapping.

## Design

Full design in [`docs/AGENT_TOKEN_TRACKING.md`](../../../docs/AGENT_TOKEN_TRACKING.md).

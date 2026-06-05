# Harness adapter — capability-tag → tool mapping (load on-demand)

Trigger: **mapping an agent's capability tags to a specific harness's tools** (porting to a new harness, or resolving what a tag means here). Each agent declares a closed set of **capability tags**; the orchestrator (and the harness) maps tags to concrete tools. AGENTS.md § Harness adapter keeps the per-agent-hints note + a pointer here.

## Capability tag → tool, per harness

| Capability tag | Claude Code | Codex / OpenAI Agents | Cursor | Aider | Generic CLI |
|---|---|---|---|---|---|
| `semantic-code-search` | `mcp__vexp__run_pipeline` | vexp.run_pipeline (MCP) | (built-in search panel) | (not built-in — fall back to text-search) | `rg` over symbol set |
| `file-skeleton` | `mcp__vexp__get_skeleton` | vexp.get_skeleton (MCP) | — | — | `ctags -x <file>` |
| `file-read` | `Read` | `read_file` | (built-in) | (built-in) | `cat` |
| `file-edit` | `Edit` | `apply_patch` | (built-in) | (built-in) | `sed` / patch |
| `text-search` | `Grep` | `rg` (shell) | (built-in) | (built-in) | `grep` / `rg` |
| `file-glob` | `Glob` | shell `find` | — | — | `find` |
| `shell` | `Bash` | `shell` | terminal | shell | sh |
| `web-fetch` | `WebFetch` | `web.fetch` | — | — | `curl` |
| `git-history` | `Bash(git log)` | `shell(git log)` | (built-in) | (built-in) | `git log` |

## Harness notes

- **Claude Code** discovers agents at `.claude/agents/` — a junction into the canonical `agents/` tree created by `bash agents/scripts/core/setup-harness.sh claude-code`. Edits to `agents/*.md` are visible immediately; no sync step.
- **Codex / OpenAI Agents** reads `AGENTS.md` per the [agents.md spec](https://agents.md/). Discover individual agents at `agents/*.md`.
- **Cursor / Aider / generic** — human-driven. Agent files at `agents/` are reference docs the user pastes or invokes manually.
- Harnesses without `semantic-code-search` should fall back to text-search with the symbol set named in each agent's prose. Output is degraded but workable — expect more round-trips and larger context per query.

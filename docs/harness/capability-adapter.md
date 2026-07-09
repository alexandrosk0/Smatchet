# Harness adapter — capability-tag → tool mapping (load on-demand)

Trigger: **mapping an agent's capability tags to a specific harness's tools** (porting to a new harness, or resolving what a tag means here). Each agent declares a closed set of **capability tags**; the orchestrator (and the harness) maps tags to concrete tools. AGENTS.md § Harness adapter keeps the per-agent-hints note + a pointer here.

## Capability tag → tool, per harness

| Capability tag | Claude Code | Codex / OpenAI Agents | Cursor | Aider | pi | Generic CLI |
|---|---|---|---|---|---|---|
| `semantic-code-search` | vexp `run_pipeline` → `Grep` | `rg` fallback | (built-in search panel) | (not built-in — fall back to text-search) | `grep` (fallback) | `rg` over symbol set |
| `file-skeleton` | vexp `get_skeleton` → targeted `Read` | targeted file read | — | — | `read` (fallback) | `ctags -x <file>` |
| `file-read` | `Read` | `read_file` | (built-in) | (built-in) | `read` | `cat` |
| `file-edit` | `Edit` | `apply_patch` | (built-in) | (built-in) | `edit` | `sed` / patch |
| `file-write` | `Write` | `apply_patch` | (built-in) | (built-in) | `write` | `tee` / redirect |
| `text-search` | `Grep` | `rg` (shell) | (built-in) | (built-in) | `grep` | `grep` / `rg` |
| `file-glob` | `Glob` | shell `find` | — | — | `find` / `ls` | `find` |
| `shell` | `Bash` | `shell` | terminal | shell | `bash` | sh |
| `web-fetch` | `WebFetch` | `web.fetch` | — | — | — (dropped) | `curl` |
| `git-history` | `Bash(git log)` | `shell(git log)` | (built-in) | (built-in) | `bash` (git log) | `git log` |
| `prompt-intent-capture` | `UserPromptSubmit` hook (`capture-intent.sh`) | — (no prompt hook → orchestrator fallback) | — (no executable hooks → fallback) | — (fallback) | — (no prompt hook → fallback) | — (fallback) |

## Harness notes

- **Claude Code** discovers agents at `.claude/agents/` — a junction into the canonical `agents/` tree created by `bash agents/scripts/core/setup-harness.sh claude-code`. Edits to `agents/*.md` are visible immediately; no sync step.
- **Codex / OpenAI Agents** reads `AGENTS.md` per the [agents.md spec](https://agents.md/). Discover individual agents at `agents/{core,project}/*.md`; `setup-harness codex` also generates `.codex/agents/*.toml` custom agents from those canonical prompts.
- **Cursor / Aider / generic** — human-driven. Agent files at `agents/` are reference docs the user pastes or invokes manually.
- **pi** discovers a **flat** `.pi/agents/*.md` (parser reads only `name`/`description`/`tools`/`model`) and auto-loads extensions from `.pi/extensions/`. Both are generated/patched (gitignored) by `bash agents/scripts/core/setup-harness.sh pi` — the `capabilities:`→`tools:` mapping above plus a tier→model map. Project agents are off + confirm-gated in stock pi; the adapter relaxes that for this trusted repo. Bring-up + the security note: [`pi/README.md`](pi/README.md).
- Harnesses without `semantic-code-search` should fall back to text-search with the symbol set named in each agent's prose. Output is degraded but workable — expect more round-trips and larger context per query.
- **`prompt-intent-capture` is Claude-Code-only.** Only Claude Code exposes a `UserPromptSubmit`-equivalent hook, so only it auto-captures the originating prompt into `.session-intent/<branch>.log`. Codex (SessionStart/Stop only), Cursor (no executable hooks), and pi (subagent extension, no prompt hook) cannot — on those, the orchestrator MUST hand-fill the PR `## Intent` from the live prompt (`docs/agent-rules/ship-loops.md` § Intent capture, Fallback). The `Intent section` gate blocks either way; the `intent-out-of-band` label is the escape hatch when neither capture nor hand-fill is possible.

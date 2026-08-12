# Plan: fill 8 gaps from agent-ecosystem comparison
<!-- index-summary: Fill 8 patterns borrowed from Anthropic multi-agent / OpenAI Agents SDK / OpenHands / wshobson — parallel dispatch, session scratchpad, tool-trace, output-shape contract, trigger map, versioning, skeleton-first, telemetry. -->

## Context

A comparison pass against successful public agent ecosystems (Anthropic multi-agent research paper, OpenAI Agents SDK, OpenHands microagents, wshobson/agents, Aider, Continue.dev, mattpocock/skills) surfaced 9 patterns Smatchet could borrow. Gap 5 (skill diversity) is correctly out of scope for a single-domain C++ codebase; the user wants gaps 1–4 + 6–9 implemented.

The 8 gaps:

| # | Pattern | Source | Smatchet impact |
|---|---|---|---|
| 1 | Parallel subagent dispatch | Anthropic | Cuts wall-clock when delegations are independent (perf-detective + debug-detective concurrently for "slow AND wrong output"). |
| 2 | Session scratchpad | Anthropic | Subagent N reads what N-1 already discovered; eliminates re-discovery cost. |
| 3 | Tool-call traces in reports | OpenAI Agents SDK | Debugging delegation gone sideways. |
| 4 | Structured output shape | OpenAI Agents SDK | Standardised section headers per agent class — catches handoff drift. |
| 6 | Trigger auto-activation | OpenHands | Orchestrator-side trigger-keyword map; cuts routing latency. |
| 7 | Agent versioning | OpenAI Agents SDK | `version: N` frontmatter; bump on breaking changes; lets telemetry/backlog reason about which prompt was in play. |
| 8 | Skeleton-first repo-map | Anthropic / Cursor | `get_skeleton` over `Read` for context-only inspection. Already in toolkit, underused. |
| 9 | Agent telemetry beyond tokens | wshobson + Anthropic | Track outcome / halt_reason / agent_version / delegation_chain alongside token counts. |

## Decisions (locked with user)

| # | Question | Choice |
|---|---|---|
| Scratchpad location | `.session-context.md` at repo root | Harness-agnostic. Gitignored. Cleared at SessionStart. |
| Tool-trace inclusion | Always-on one-line summary at report end | Predictable, scannable, ~10 output tokens. |
| Telemetry schema additions | All four: `outcome` + `halt_reason` + `agent_version` + `delegation_chain` | Full visibility into delegation health. |

## Files to modify

| Path | Change |
|---|---|
| **Infra (commit A)** | |
| `agents/_shared/token-tracking/agent-token-log.py` | Extend JSONL schema with `outcome` / `halt_reason` / `agent_version` / `delegation_chain`. Parser pulls from transcript tail. |
| `scripts/agent-tokens-report.py` | New summary table: per-agent outcome breakdown + top halt reasons. |
| `agents/_shared/token-tracking/agents-statusline.py` | Optional `halted: N` count when ≥1 halt this session. |
| `scripts/clear-session-context.sh` *(new)* | Truncates `.session-context.md`, writes starter banner with session id + timestamp. |
| `.claude/settings.json` | New `SessionStart` hook invoking `scripts/clear-session-context.sh`. |
| `.session-context.md` *(new, gitignored)* | Per-session scratchpad. Subagent writes append-only headers; never edits prior entries. |
| `.gitignore` | Add `.session-context.md`. |
| **Policy (commit B)** | |
| `AGENTS.md` | 7 new sub-sections: § Parallel dispatch, § Session scratchpad protocol, § Tool-trace contract, § Agent output contract, § Trigger auto-activation, § Skeleton-first, § Agent versioning. ~110 lines added. |
| **Per-agent (commit B)** | |
| `agents/*.md` (20 files) | + `version: 1` frontmatter, banner gains `· v1`, report-shape ends with `Tools:` line + scratchpad-append marker. ~5 lines per file. |
| `scripts/sync-agents.sh`, `scripts/sync-agents.ps1` | Mirror banner includes `agent-version: <N>` from frontmatter. |
| `.claude/agents/*.md`, `.claude/hooks/*.py` | Auto-regenerated mirrors. |
| **Post-ship** | |
| `docs/backlog/AGENT_SELF_IMPROVEMENT.md` | One applied-row per gap with SHA. |

## Per-gap design

### Gap 1 — Parallel dispatch (doc-only)

`AGENTS.md` § Delegation gains a sub-section:

> **Parallelise independent subagents.** When the orchestrator can identify two or more delegations with no contract between them, dispatch them in a **single tool-use block** (multiple Agent calls in one message). Examples:
> - Symptom "slow AND wrong output" → `perf-detective` + `debug-detective` concurrently.
> - Pre-merge gate → `code-review` + `security-review` concurrently.
> - Multi-subsystem feature → multiple `tracker-backend` / `grid-engine` / `offline-sync` slices when their write sets do not overlap.
>
> Do **not** parallelise when one delegation's output feeds another (e.g. `architect` → subsystem agents). Sequential when contract-coupled; parallel otherwise.

### Gap 2 — Session scratchpad

Mechanism:

1. **Bootstrap.** `scripts/clear-session-context.sh` truncates `.session-context.md` and writes a starter banner:

   ```markdown
   # Session context

   _Session: <session-id> · started: <ISO-8601>_
   _Append-only. Each subagent writes one header block at end of report. Never edit prior entries._
   ```

2. **Hook.** `.claude/settings.json` `SessionStart` event runs the script. Each new session starts with a fresh scratchpad.

3. **Read protocol.** Every subagent's first tool call is `Read(.session-context.md)`. If empty → fresh session, no prior context. If non-empty → bring forward repro state, file:line evidence, decisions already locked.

4. **Write protocol.** Every subagent's report ends with an append-only header block written via `Edit` (append, no replace):

   ```markdown
   ## <agent-name> · <ISO-8601> · <outcome>

   **Key facts** (max 5 bullets):
   - <fact 1 with file:line>

   **Decisions locked**:
   - <decision>

   **Open questions**:
   - <question handed back to orchestrator>
   ```

5. **Gitignore.** `.session-context.md` never committed.

`AGENTS.md` § Session scratchpad protocol codifies steps 3 + 4 as hard rules.

### Gap 3 — Tool-trace contract

`AGENTS.md` § Tool-trace contract:

> Every subagent report ends with a single-line tool summary immediately above `## Self-improvement`:
>
> ```
> Tools: <tool>×<n>, <tool>×<n>, ...
> ```
>
> Sorted by descending count. Counts include only this delegation's own tool calls, not children's. Costs ~10 output tokens; pays back when reviewing halted delegations.

Per-agent report-shape section gains this line in its template. Caveman compression preserves (technical content) so output cost flat regardless of mode.

### Gap 4 — Agent output contract

Agents fall into four classes by output shape:

| Class | Members | Required sections |
|---|---|---|
| **Investigator** (read-only diagnosis) | architect, debug-detective, perf-detective, spike-hunter, code-review, security-review | `## Hypotheses` (or `## Findings` for review agents) → `## Evidence` → `## Cause` (or severity-bucketed list) → `## Handoff` (target agent + write set) |
| **Implementer** (read-edit subsystem) | tracker-backend, grid-engine, offline-sync, command-system, lua-binder, mcp-toolsmith, p4-blame, unreal-bridge, mechanic | `## Files changed` → `## Smoke-test result` → `## Manual residue` (must say "none" if none) |
| **Helper** (terminal helper) | perf-instrument, perf-measure | `## Spec executed` → `## Result` (numbers / inserted-or-stripped count) |
| **Maintenance** (workflow) | build-doctor, test-author, git-janitor | `## Pre-flight` → `## Mutations applied` → `## Regression gate` → `## Residue requiring user action` |

`AGENTS.md` § Agent output contract codifies the four classes + their required headers. Agents already mostly follow these shapes; this codifies + closes drift.

### Gap 6 — Trigger auto-activation

Reality: Claude Code subagent discovery uses the `description:` field for fuzzy match. The `triggers:` frontmatter is informational for the orchestrator, not consumed by the harness directly.

Fix: `AGENTS.md` § Trigger auto-activation table mapping keyword → agent, consulted by the orchestrator **before** falling back to the heuristic block already in § Delegation:

```
| Keyword(s) in user prompt | Agent |
|---|---|
| slow, FPS, lag, profile | perf-detective |
| spike, hitch, freeze, stutter, intermittent | spike-hunter |
| crash, broken, regression, wrong output, doesn't work | debug-detective |
| review, pre-merge, PR review | code-review |
| security, vuln, secret, injection | security-review |
| build, cmake, preset, link, packaging | build-doctor |
| automate testing, manual verification, headless test | test-author |
| end of session, merge open PRs, tidy up | git-janitor |
| stress-test plan, grill, interrogate | grill-with-docs (skill) |
```

Audit pass: ensure every per-agent `triggers:` list covers its row in the table. Currently most do.

### Gap 7 — Agent versioning

New frontmatter field: `version: <N>` (integer, monotonic per-agent).

Bump rules:
- **Bump on**: capability change (added/removed capability tag), workflow contract change (e.g. added cleanup discipline, new mandatory section), breaking output-shape change (renamed report section a downstream agent reads).
- **Don't bump on**: prose tweaks, fixed typos, banner format changes, token-efficiency tightens that preserve semantics.

Banner gains version segment: `🤖 AGENT: name · model/complexity · access · v1`.

`scripts/sync-agents.sh` parses the `version:` field and includes it in the mirror banner: `# AUTO-GENERATED MIRROR of agents/<name>.md@v<N> — DO NOT EDIT.`

Telemetry (gap 9) captures `agent_version` per call so we can correlate behaviour with prompt version.

### Gap 8 — Skeleton-first

`AGENTS.md` § Semantic codebase search already mentions skeleton views. Strengthen:

> **Skeleton-first hard rule.** For files you're **inspecting** (understanding shape, finding the right symbol, scoping a change), use `get_skeleton` (or harness equivalent — see § Harness adapter). For files you're **editing**, use `Read`. Reading a full file for context-only inspection wastes ~70–90% of input tokens and is a self-improvement-loop finding when caught.
>
> The split:
> - "Where is X declared?" → skeleton
> - "What's the shape of this dir?" → skeleton across all files
> - "What does this function actually do?" → Read (but only that function; don't pull the whole file)
> - "I'm about to edit line N" → Read

Audit pass: agents that read full files for context (architect, code-review, security-review) get an explicit reminder in their tooling section.

### Gap 9 — Telemetry extensions

`.claude/.agent-tokens.jsonl` schema gains four fields:

```json
{
  "ts": "...", "session": "...", "agent": "perf-detective",
  "model": "opus", "model_full": "...",
  "in": 12340, "out": 2103, "cache_create": 4200, "cache_read": 18200,
  "duration_ms": 42118,
  "outcome": "applied",
  "halt_reason": null,
  "agent_version": 1,
  "delegation_chain": ["orchestrator"]
}
```

**`outcome`** values: `applied` | `halted` | `failed` | `partial` | `aborted`. Parsed from transcript final assistant message:
- final message contains "## Self-improvement" + no halt language → `applied`
- final message contains "HALT" / "halted" / refusal language → `halted`
- final message has stack trace / exception → `failed`
- final message has TODO / unfinished section → `partial`
- transcript ends without a final assistant message → `aborted`

**`halt_reason`**: when `outcome != applied`, the orchestrator-readable reason, extracted from the line preceding the halt marker. Null otherwise.

**`agent_version`**: read from frontmatter `version:` field of the canonical agent file. Allows correlating outcome trends with prompt version.

**`delegation_chain`**: synthesised by scanning the transcript backward for prior subagent invocations within the same session. Each entry is an agent name; chain ordered root → current. Helps detect deep-stack delegations.

`scripts/agent-tokens-report.py` gains:
- Per-agent outcome counter (rows: agent, applied/halted/failed/partial, halt-reason top-3).
- "Delegation depth distribution" — histogram of chain length.

`agents/_shared/token-tracking/agents-statusline.py`: if any halt this session, append `· halted: N` to the badge.

## Critical files

- `C:\Dev\Smatchet\AGENTS.md` — 7 new sub-sections.
- `C:\Dev\Smatchet\agents\_shared\token-tracking\agent-token-log.py` — schema + parsing extensions. **Read first to understand current schema and transcript-parsing approach.**
- `C:\Dev\Smatchet\scripts\agent-tokens-report.py` — report-table extensions. **Read first to understand current grouping/formatting.**
- `C:\Dev\Smatchet\.claude\settings.json` — add SessionStart hook entry. **Read first to see existing hooks shape.**
- `C:\Dev\Smatchet\agents\*.md` (20 files) — version + banner + report shape changes.
- `C:\Dev\Smatchet\scripts\sync-agents.sh` + `.ps1` — banner injection extension to include version.
- `C:\Dev\Smatchet\scripts\check-agents-mirror.sh` — should already pass once mirrors regenerate; no logic change needed.

## Reused patterns

- **Hook integration** — `SessionStart` mirrors the existing `SubagentStop` hook wiring in `.claude/settings.json`. Same `command` shape.
- **JSONL append-only schema** — exact pattern of `agent-tokens-report.py`. New fields slot in alongside existing.
- **Canonical + mirror** — `.session-context.md` lives only at repo root (no `.claude/` mirror needed; it's repo-state, not agent config).
- **Frontmatter field addition** — pattern of `harness-hints.claude-code.model` already in place. `version:` slots at the same level.
- **Caveman compatibility** — tool-trace one-line summary survives caveman compression (caveman preserves structural / technical content). Verified pattern.

## Migration order (2 commits)

**Commit A — infra + telemetry**

1. Extend `agent-token-log.py` JSONL emit with 4 new fields + parsing logic. Verify against existing `.claude/.agent-tokens.jsonl` entries (parse them as-is; new fields show as null/absent for old rows).
2. Update `agent-tokens-report.py` to consume new fields; show outcome breakdown + halt-reason top-3.
3. Update `agents-statusline.py` to surface halt count.
4. Add `scripts/clear-session-context.sh`.
5. Add SessionStart hook to `.claude/settings.json` invoking the script.
6. Add `.session-context.md` to `.gitignore`.
7. Smoke test: open new session, verify hook fires, scratchpad cleared with banner.

**Commit B — policy + per-agent updates**

1. `AGENTS.md` — 7 new sub-sections (parallel dispatch / scratchpad / tool-trace / output contract / trigger auto-activation / skeleton-first / versioning).
2. Per-agent (20 files): add `version: 1` frontmatter, update banner to include `· v1`, add `Tools:` line to report shape, scratchpad-append protocol mention.
3. `scripts/sync-agents.sh` + `.ps1` — banner includes agent version.
4. Run sync + drift check.
5. Audit per-agent `triggers:` to ensure trigger-keyword table coverage.

**Post-ship**

Append applied entries to `docs/backlog/AGENT_SELF_IMPROVEMENT.md` (one row per gap with SHA).

## Verification

**Static (post-commit A):**

```bash
# JSONL parser accepts old rows without new fields
python -c "import json; [json.loads(l) for l in open('.claude/.agent-tokens.jsonl')]"

# Hook config syntactically valid
python -c "import json; json.load(open('.claude/settings.json'))"

# Scratchpad scaffolding present + ignored
[ -f .session-context.md ] && grep -q "_Session:" .session-context.md
git check-ignore -v .session-context.md

# Report script handles old + new rows
python scripts/agent-tokens-report.py        # session report
python scripts/agent-tokens-report.py --all  # lifetime
```

**Static (post-commit B):**

```bash
# Every canonical agent has version: 1
grep -L "^version: " agents/*.md             # expect: empty output

# Mirror banner includes version
grep "agent-version:" .claude/agents/architect.md

# Drift check
bash scripts/check-agents-mirror.sh

# Trigger table covers every routable agent
python -c "
import re
table = open('AGENTS.md').read()
agents = ['perf-detective','spike-hunter','debug-detective','code-review','security-review','build-doctor','test-author','git-janitor']
missing = [a for a in agents if a not in table.split('Trigger auto-activation')[1].split('##')[0]]
print('missing in trigger table:', missing)
"
```

**Dynamic:**

1. Open a fresh Claude Code session. Confirm `.session-context.md` was truncated + banner written by the SessionStart hook.
2. Invoke `code-review` subagent. Confirm its report ends with `Tools: ...` line + appends a header block to `.session-context.md`.
3. After subagent stops, confirm `.claude/.agent-tokens.jsonl` last row has `outcome` field populated + `agent_version: 1`.
4. Run `python scripts/agent-tokens-report.py` — new outcome breakdown column visible.
5. (Slow) Invoke two independent subagents in one tool-use block (per § Parallel dispatch). Confirm both write scratchpad entries with matching session id.

## Hard rules introduced

- **Parallel dispatch only when contracts are independent.** Sequential when one feeds another.
- **Scratchpad is append-only.** Subagents never edit prior entries; only append a fresh header block.
- **Tool-trace one-liner mandatory** at end of every subagent report.
- **Output-contract sections mandatory** per agent class (Investigator / Implementer / Helper / Maintenance).
- **Skeleton over Read for context-only inspection** (Read only when editing).
- **Version bumps on capability / workflow / output-shape changes only.**
- **Telemetry `outcome` inferred from transcript tail**, never agent self-report (matches existing layer-A skip rationale in `docs/guides/agent-token-tracking.md`).

## Out of scope

- Per-agent JSON output schemas (gap 4 medium-rigour alternative, not selected). Markdown-section contract suffices.
- Migration of `agent-tokens-report.py` to a structured DB. JSONL append-only stays.
- Cross-session scratchpad merge / archive. Per-session, cleared at SessionStart, no carry-over.
- Auto-routing inside Claude Code itself (gap 6). The trigger table is for the orchestrator (main thread), not the harness — Claude Code's `description:` matching stays primary.
- Skill-level changes to `grill-with-docs` or `agent-tokens`. Skills already align with these gaps.

## Implementation log

- `6df6170` · commit A — infra: agent-token-log.py + clear-session-context.sh + SessionStart hook + report + statusline extensions; .session-context.md gitignored
- `d206de5` · commit B — policy: AGENTS.md § Parallel dispatch / Session scratchpad / Tool-trace / Output contract / Trigger auto-activation / Skeleton-first / Agent versioning; per-agent version: 1 + banner update; sync-agents.sh / .ps1 emit @v<N> in mirror banner

## Deviations from plan

- **Scratchpad write protocol** — original plan had subagents Edit the file themselves at end of report. Deviated to **hook-writes-from-section**: subagent emits `## Session context append` in its report; SubagentStop hook reads + appends. Eliminates race when parallel subagents would otherwise contend on the same file, and avoids conflicting with the vexp `run_pipeline`-FIRST rule.
- **Tool-trace generation** — original plan had agents emit a `Tools:` line manually. Deviated to **hook-counts-from-transcript** for the same reason: zero agent burden, fully accurate. The canonical count is the JSONL `tool_trace` field; agents may optionally include their own line for the user's eye but the count source-of-truth is hook-derived.
- **`outcome` inference** — refined from the plan's three-bullet heuristic to a four-tier priority: explicit `## Outcome: <state>` line wins, then halt-keyword scan, then `## Self-improvement` heading presence, then default applied. Catches more cases cleanly.

## Verification

Static:

- `bash scripts/check-agents-mirror.sh` → exits 0 (drift clean post-sync).
- `python -m json.tool .claude/settings.json` → valid.
- `python scripts/agent-tokens-report.py --all` → handles old rows lacking new fields; defaults outcome=applied; no crashes.
- `python scripts/agent-tokens-report.py` → session view works.
- `bash scripts/clear-session-context.sh` → writes banner with session-id + timestamp; `.session-context.md` truncated.
- `git check-ignore -v .session-context.md` → confirms gitignored.
- All 20 agents have `version: 1` in frontmatter (`grep -L "^version: " agents/*.md` returns empty).
- Mirror banner reads `@v1` for every agent (`grep "@v1" .claude/agents/architect.md` matches).

End-to-end (fixture-based):

1. Constructed two fake transcripts (applied path + halted path) under a Windows-friendly temp dir.
2. Invoked `agent-token-log.py` with synthetic SubagentStop stdin JSON pointing at each transcript.
3. Confirmed for applied path: `outcome=applied`, `tools_used={"Read":1,"Edit":2,"Bash":1}`, `tool_trace="Edit×2, Read×1, Bash×1"`, `agent_version=2` (read from fake frontmatter), `.session-context.md` appended with header block.
4. Confirmed for halted path: `outcome=halted`, `halt_reason` captured from the BLOCKED line, `agent_version=1` (default when frontmatter lacks the field).

Dynamic (deferred — needs a real Claude Code session restart):

1. Restart Claude Code (loads the new SessionStart hook + AGENTS.md).
2. Confirm `.session-context.md` shows a fresh banner with the new session id.
3. Invoke any subagent; the hook should append a row to `.claude/.agent-tokens.jsonl` with the new fields populated.
4. Run `python scripts/agent-tokens-report.py` and confirm the outcome breakdown shows the new agent call.
5. Spawn two independent subagents in one tool-use block per § Parallel dispatch; both rows share the session id; scratchpad accumulates two headers.

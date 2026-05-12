# Agent self-improvement backlog

Suggestions emitted by delegated agents (canonical at `agents/`, mirrored to `.claude/agents/` for Claude Code auto-discovery) for improving the agent ecosystem itself — prompt tweaks, missing context, redundant steps, new-subagent candidates, tooling gaps.

The main thread appends new entries here (dedupe against existing). Periodically triage and apply real wins to agent prompts; close out items that landed by deleting them from this file (git history preserves them).

## Format

```
- YYYY-MM-DD · <agent-name> · [shortcut|process|tooling|context|new-agent] — one-line description
  Details: (optional, single paragraph or short bullet list — context that explains why this would help)
  Status: open | applied | rejected (with reason)
```

Categories:

- **shortcut** — a step the agent finds itself doing manually that could be encoded in its prompt as a default
- **process** — workflow friction: redundant steps, wrong order, missing handoff between agents
- **tooling** — missing CLI / static-analyzer / vexp invocation that would speed things up
- **context** — context the agent had to discover during the task that should be pre-loaded in its prompt
- **new-agent** — subsystem / task pattern that recurs and would warrant its own subagent

## Workflow

1. Delegated agents end every report with a `## Self-improvement` section. Empty is the common case and explicitly fine — agents only flag real friction.
2. The orchestrator reads the section, dedupes against this file, appends new entries with date + source agent + category.
3. When an entry has gathered enough evidence (mentioned by ≥ 2 agents, or blocks the same workflow ≥ 3 times), apply it: edit the relevant agent prompt(s) in `agents/`, regenerate the mirror via `scripts/sync-agents.sh`, and close out the entry.

## Triage cadence

Sweep the file when:

- Opening any PR that touches `agents/`
- The list exceeds ~20 open items

## Entries

<!-- Latest first. Append new entries at the top of this section. -->

- 2026-05-12 · command-system · [shortcut] — when the harness lint hook auto-runs on every edit, don't also run a batch `clang-format` at the end
  Details: Doing so produced large reformat diffs on `BuiltinCommands.cpp` / `PlaneClient.cpp` during PR 6 of the project-key removal. The PostToolUse hook in `.claude/settings.json` already covered every edited file.
  Status: open

- 2026-05-12 · grid-engine, command-system · [context] — localization accessor is `SmatchetLocalization::T(key, englishFallback)`, not `Loc(...)` / `Translate(...)`
  Details: Both PR 4b and PR 6 agents guessed wrong names and only converged via grep. Add a one-line note to `agents/grid-engine.md` and `agents/command-system.md`.
  Status: open (≥2 agents mentioned — threshold met for applying)

- 2026-05-12 · grid-engine · [process / new-agent] — design-doc PRs that span ≥3 subsystems have no clear owner
  Details: PR 4 of the project-key removal touched tracker-backend (`ListProjects`) + grid-engine (draft picker, view pill) + bulk-import + i18n. `grid-engine` paused and asked for a split, which was correct but cost a round-trip. Either add an explicit `pr-driver` meta-agent that splits design-doc PRs into subsystem sub-delegations, or add a note to AGENTS.md instructing the orchestrator to pre-split such PRs before delegating.
  Status: open

- 2026-05-12 · tracker-backend · [context] — `RemoteProject` POD uses lowerCamelCase (`id`, `key`, `displayName`) while most other DTOs in `Source_Core/include/` use PascalCase (`Id`, `Name`, …)
  Details: Style drift introduced in PR 1. Worth normalizing before more call sites accumulate. Architect call.
  Status: open

- 2026-05-12 · tracker-backend · [tooling / new-agent] — no test rig in the repo
  Details: pure-C++14 helpers (`JqlProjectScope`, value parser, JQL surgery) had to invent compile-only test patterns that aren't actually executed. A small `test-rig` agent that wires up a CTest target with doctest/GoogleTest against `Source_Core` would unblock real unit tests. High ROI given how much pure-logic code lives there.
  Status: open

- 2026-05-12 · tracker-backend · [context] — design-doc PR sections that list line numbers should mark each as `(cfg-read)` / `(draft-write)` / `(audit-only)`
  Details: Project-key PR 2 §2.3 listed lines 358, 382, 70, 92, 349 alongside `cfg.ProjectKey`-read sites, but they were draft-writes — required a disambiguation pass.
  Status: open

- 2026-05-12 · tracker-backend · [tooling] — `mcp__vexp__run_pipeline` rejects `max_tokens` as float when JSON wire format is double
  Details: Surfaced as "floating point, expected usize" — schema should accept integers-as-floats or improve the error message.
  Status: open

- 2026-05-12 · offline-sync · [shortcut] — `SaveFieldCatalogSnapshot` accumulated 4 extra primitive args; a `FieldCatalogSaveContext` struct would prevent future drift
  Details: callers already had each arg in scope; bundling them into one struct keeps the call site narrow as more per-axis state lands.
  Status: open

- 2026-05-12 · command-system · [process] — when a PR plan names a specific line/symbol, do a 30-second sanity grep before editing
  Details: Project-key PR 6 plan flagged `AppController_LuaBindings.cpp:~L254` as a "Lua config setter to deprecate" — it was actually `LuaApplyIssueCreateKv` (per-operation draft kv, not a config setter). One round-trip cost. Agent correctly flagged back to orchestrator before editing.
  Status: open

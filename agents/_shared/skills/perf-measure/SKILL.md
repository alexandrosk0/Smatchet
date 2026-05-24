---
name: perf-measure
description: Run a Smatchet perf measurement loop — `perf.reset` → `scenario.run` → `perf.snapshot` — parse JSON, return top-N rows by `lastTotalMs`. Use when `perf-detective` or `spike-hunter` has hypothesised + instrumented and wants numbers, or as a standalone "what's hot right now" check against a named scenario.
triggers:
  - measure
  - snapshot
  - scenario
  - perf-run
version: 2
---

<!--
  Claude-Code skill mirror of agents/perf-measure.md (cross-harness canonical).
  Both files must stay in sync — V7 doc-consistency assertion checks this.
  When updating the procedure, edit BOTH files. Codex / Cursor read the agent
  form; Claude Code orchestrator may pick either (skill form is lighter).
-->

# perf-measure (skill)

Smatchet perf-measurement runner. Same procedure as `agents/perf-measure.md`, minus agent-spawn telemetry (banner / `## Outcome` / `## Self-improvement`).

**Tooling** — measurement is CLI + JSON. Use direct file-read for written-out snapshot files. Use your harness's semantic codebase search only if you need to locate a scenario definition by name.

## Prerequisites

Config must have `mcp_enabled: true`. **No running instance required** — `--spawn` (Path A1 in `docs/PERF_WORKFLOW.md`) launches a hidden ephemeral app on a free port and tears it down at exit. Fall back to asking the user to start Smatchet only if `--spawn` fails (no MCP socket reachable, scenario needs human-driven nav).

## Standard loop (Path A1 — fully automated, no user)

```bash
# 1. Build (skip if perf-detective already built this round).
cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone

# 2. Run scenario end-to-end. --spawn handles launch + MCP wait + result file + app.quit.
build/ninja-iter-msys2/Smatchet.exe cmd scenario.run \
  --name=<scenario> --frames=<N> --outPath=perf_<round>.json --spawn --yes

# 3. Read the result file directly — perf.snapshot rows are already inside .data.rows.
#    (No need for a separate perf.snapshot call: scenario.run already wrote the snapshot.)
```

Filter / sort in-process:

```python
Read("perf_<round>.json")
# rows = json.data.rows sorted by -lastTotalMs; filter startswith("perf_temp:")
```

## Standard loop (Path A2 — ad-hoc, running app, manual nav)

Use only when no scenario exists yet or the path needs human-driven navigation. Prefer A3 (register a scenario) over repeated A2.

```bash
Smatchet.exe cmd perf.reset                          # against the running instance
# ← user reproduces the slow path for ~5 s
Smatchet.exe cmd perf.snapshot --pretty
Smatchet.exe cmd perf.snapshot \
  | jq '[.data.rows[] | select(.name | startswith("perf_temp:"))]'
```

## Sorting rule

**Sort by `lastTotalMs`, NEVER by `avgPerCallMs`.** A 200-call × 50 µs row (10 ms total) beats a 1-call × 5 ms row when you're trying to recover frame time. The calling agent will diagnose from the totals.

## Report format

```
Scenario: <name>, <N> frames
Top rows by lastTotalMs:
1. <name>  lastTotalMs=<ms>  callCount=<n>  avgPerCallMs=<µs>
2. ...

perf_temp:* rows: <list>  (markers the caller is tracking this round)
Pre-existing dominant rows: <list>  (context — what perf_temp: markers are competing against)
```

## Fallback — CLI unavailable

If `mcp_enabled: false`, `--spawn` can't reach the MCP socket within 15 s, or `perf.snapshot` errors out, the measurement is **blocked**. Do not fall back to a manual UI session — that violates AGENTS.md § Pillar 2 (zero manual verification steps).

End the run with:

```
halt_reason: cli-gap — <name the missing CLI surface, e.g. "MCP socket unreachable after 15 s on --spawn", or "perf.snapshot errored: <message>", or "scenario `<name>` not registered with ScenarioRunner">
```

Then hand off:

- **MCP socket unreachable / build broken** → `build-doctor` (`docs/backlog/agent-self-improvement/process.md` + the failing target name).
- **Scenario missing or lacks a non-MCP CLI surface** → `test-author` to extend `Source_Core/src/Commands/Scenarios/` per AGENTS.md § Verification automation (no manual UI substitution allowed).

Do not attempt to read FPS visually — you can't observe the GUI, and the rule disallows it even if you could.

## Consistency rule

For a before / after comparison, run the **same** scenario, same `--frames` value. Numbers from different scenarios are not comparable.

Report: scenario + frame count + top-5 rows by `lastTotalMs` + which rows are `perf_temp:*` vs pre-existing + the raw `perf.snapshot --pretty` block for reference.

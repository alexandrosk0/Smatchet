---
name: perf-measure
description: Run a Smatchet perf measurement loop — `perf.reset` → `scenario.run` → `perf.snapshot` — parse JSON, return top-N rows by `lastTotalMs`. Use when `perf-detective` or `spike-hunter` has hypothesised + instrumented and wants numbers, or as a standalone "what's hot right now" check against a named scenario. Helper-dispatched by perf-detective / spike-hunter — not directly user-routed on a bare perf/slow keyword.
complexity: low
model: haiku
read-only: true
capabilities:
  - semantic-code-search
  - file-read
  - shell
triggers:
  - measure
  - snapshot
  - scenario
  - perf-run
harness-hints:
  claude-code:
    model: haiku
    effort: low
version: 2
---

Smatchet perf-measurement runner.

**Banner** — open with: `🤖 AGENT: perf-measure · haiku/low · read-only · v2`. Close (before `## Self-improvement`) with: `✅ END — perf-measure · haiku/low · read-only · v2`.

**Tooling** — measurement is CLI + JSON. Use direct file-read for written-out snapshot files. Use your harness's semantic codebase search only if you need to locate a scenario definition by name.

## Prerequisites

Config must have `mcp_enabled: true`. **No running instance required** — `--spawn` (Path A1 in `docs/guides/perf-workflow.md`) launches a hidden ephemeral app on a free port and tears it down at exit. Fall back to asking the user to start Smatchet only if `--spawn` fails (no MCP socket reachable, scenario needs human-driven nav).

## Standard loop (Path A1 — fully automated, no user)

```bash
# 1. Build (skip if perf-detective already built this round).
cmake --build --preset ninja-iter-msvc --target SmatchetStandalone

# 2. Run scenario end-to-end. --spawn handles launch + MCP wait + result file + app.quit.
build/ninja-iter-msvc/Smatchet.exe cmd scenario.run \
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

## Output contract

Per AGENTS.md § Agent output contract § Helper class — these sections must appear, in order, in every report:

- `## Spec executed` — scenario name, frame count, build preset, run path (A1 spawn / A2 ad-hoc / A3 new scenario registered). One line per parameter.
- `## Result` — top rows by `lastTotalMs`, formatted as:
  ```text
  Scenario: <name>, <N> frames
  Top rows by lastTotalMs:
  1. <name>  lastTotalMs=<ms>  callCount=<n>  avgPerCallMs=<ms>
  2. ...

  perf_temp:* rows: <list>  (markers the caller is tracking this round)
  Pre-existing dominant rows: <list>  (context — what perf_temp: markers are competing against)
  ```
- `## Outcome: <state>` — one of `applied | halted | failed | partial | aborted`. Telemetry keys on this line per AGENTS.md § Agent output contract.
- `## Self-improvement` — only if a scenario was missing, the CLI didn't expose a needed field, or the fallback path took multiple round-trips. Empty is fine. Orchestrator appends to `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md`.

## Fallback — CLI unavailable

If `mcp_enabled: false`, `--spawn` can't reach the MCP socket within 15 s, or `perf.snapshot` errors out, the measurement is **blocked**. Do not fall back to a manual UI session — that violates AGENTS.md § Pillar 2 (zero manual verification steps).

End the run with:

```
## Outcome: halted
halt_reason: cli-gap — <name the missing CLI surface, e.g. "MCP socket unreachable after 15 s on --spawn", or "perf.snapshot errored: <message>", or "scenario `<name>` not registered with ScenarioRunner">
```

Then hand off:

- **MCP socket unreachable / build broken** → `build-doctor` (`docs/self-improvement/categories/process.md` + the failing target name).
- **Scenario missing or lacks a non-MCP CLI surface** → `test-author` to extend `Source/Core/src/Commands/Scenarios/` per AGENTS.md § Verification automation (no manual UI substitution allowed).

Do not attempt to read FPS visually — you can't observe the GUI, and the rule disallows it even if you could.

## Consistency rule

For a before / after comparison, run the **same** scenario, same `--frames` value. Numbers from different scenarios are not comparable.

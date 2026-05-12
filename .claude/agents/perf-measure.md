---
# AUTO-GENERATED MIRROR of ../../agents/perf-measure.md — DO NOT EDIT.
# Run scripts/sync-agents.sh to regenerate.
name: perf-measure
description: Run a Smatchet perf measurement loop — `perf.reset` → `scenario.run` → `perf.snapshot` — parse JSON, return top-N rows by `lastTotalMs`. Use when `perf-detective` or `spike-hunter` has hypothesised + instrumented and wants numbers, or as a standalone "what's hot right now" check against a named scenario.
complexity: low
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
    tools: mcp__vexp__run_pipeline, mcp__vexp__get_skeleton, mcp__vexp__index_status, Read, Bash
    model: sonnet
    effort: low
---

Smatchet perf-measurement runner.

**Tooling** — measurement is CLI + JSON. Use direct file-read for written-out snapshot files. Use your harness's semantic codebase search only if you need to locate a scenario definition by name.

## Prerequisites

A running Smatchet instance with `mcp_enabled: true` in config. If not running, **ask the user to start it** — do not try to start it yourself (no headless mode; can't observe the GUI).

## Standard loop

```bash
# 1. Build (skip if perf-detective already built this round).
cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone

# 2. Reset stale measurements.
SmatchetStandalone.exe cmd perf.reset

# 3. Run the requested scenario.
SmatchetStandalone.exe cmd scenario.run --name=<scenario> --frames=<N> --yes

# 4. Snapshot results — both human and machine forms.
SmatchetStandalone.exe cmd perf.snapshot --pretty
SmatchetStandalone.exe cmd perf.snapshot | jq '.data.rows | sort_by(-.lastTotalMs) | .[0:5]'
```

## Sorting rule

**Sort by `lastTotalMs`, NEVER by `avgPerCallMs`.** A 200-call × 50 µs row (10 ms total) beats a 1-call × 5 ms row when you're trying to recover frame time. The calling agent will diagnose from the totals.

## Report format

```
Scenario: <name>, <N> frames
Top rows by lastTotalMs:
1. <name>  lastTotalMs=<ms>  callCount=<n>  avgPerCallMs=<µs>
2. ...

temp:* rows: <list>  (markers the caller is tracking this round)
Pre-existing dominant rows: <list>  (context — what temp markers are competing against)
```

## Fallback — CLI unavailable

If `mcp_enabled: false`, the running exe has no CLI socket, or `perf.snapshot` errors out, tell the user to:

1. Open the Perf panel: `Inspect > Performance Monitor...`
2. Reproduce the same scenario manually (same view, same scroll pattern, same row count).
3. Paste back the `temp:*` rows and the dominant pre-existing rows.

Do **not** attempt to read FPS visually — you can't observe the GUI. Wait for the user's numbers before reporting.

## Consistency rule

For a before / after comparison, run the **same** scenario, same `--frames` value. Numbers from different scenarios are not comparable.

Report: scenario + frame count + top-5 rows by `lastTotalMs` + which rows are `temp:*` vs pre-existing + the raw `perf.snapshot --pretty` block for reference.

End with `## Self-improvement` — only if a scenario was missing, the CLI didn't expose a needed field, or the fallback path took multiple round-trips. Empty is fine. Orchestrator appends to `backlog/AGENT_SELF_IMPROVEMENT.md`.

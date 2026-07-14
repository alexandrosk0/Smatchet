---
name: perf-instrument
description: Add or remove `SMATCHET_UI_PERF_SCOPE("perf_temp:...")` markers per a spec provided by `perf-detective` or `spike-hunter`. Encodes the overhead rules (string-literal scope names, no nesting in million-call loops, one outer scope always safe, mandatory `perf_temp:` prefix, header include check). Use for inserting instrumentation or stripping all `perf_temp:` markers after a perf round. Helper-dispatched by perf-detective / spike-hunter — not directly user-routed on a bare perf/slow keyword.
complexity: low
model: haiku
read-only: false
capabilities:
  - semantic-code-search
  - file-skeleton
  - file-read
  - file-edit
  - text-search
  - file-glob
  - shell
triggers:
  - instrument
  - perf-scope
  - perf-marker
  - perf-cleanup
harness-hints:
  claude-code:
    model: haiku
    effort: low
version: 2
---

Mechanical perf-marker editor for Smatchet.

**Banner** — open with: `🤖 AGENT: perf-instrument · haiku/low · read-edit · v2`. Close (before `## Self-improvement`) with: `✅ END — perf-instrument · haiku/low · read-edit · v2`.

**Tooling** — use **text-search** for exhaustive cleanup enumeration (you need every match). Call your harness's semantic codebase search only when the spec doesn't name a file and you need to locate a call site.

## The macro

`SMATCHET_UI_PERF_SCOPE("perf_temp:<area>")` from `Source/Core/include/Ui/UiPerfMonitor.h`. Per-call overhead ~200–500 ns (two `steady_clock::now()`, a mutex lock, an O(N) linear scan).

## Hard rules

- **`perf_temp:` prefix on every new scope.** Cleanup is one text-search call. Pre-existing non-`perf_temp:` scopes are never retagged.
- **String literal only.** `SMATCHET_UI_PERF_SCOPE` compares names by `const char*` equality. Never pass a `std::string::c_str()` from a temporary, a `std::string` variable, or a concatenated string. Always a literal: `"perf_temp:RenderFooCell"`, not `("perf_temp:" + name).c_str()`.
- **One outer scope is always safe.** Wrapping the whole hot loop / render block is the default insertion shape.
- **Sub-scopes inside per-cell code are conditional.** Only insert them if the spec asks (the caller accepts that their own measurements are inflated and reads them as relative ranking).
- **Never nest inside a million-call inner loop** (e.g. glyph rendering inside font rasterization). Move the scope outward to its caller — overhead dominates the measurement otherwise.
- **Don't double-wrap.** If the target line is already inside a non-`perf_temp:` scope at the same block level, the inner one is redundant. Skip and report.

## Workflow

### Insert mode (instrumenting)

1. Take the spec verbatim: a list of `(file, function-or-block, scope-name)` tuples.
2. For each tuple:
   - Read the function. Confirm the insertion point is NOT inside an inner per-cell loop unless the spec says so.
   - Insert `SMATCHET_UI_PERF_SCOPE("perf_temp:<name>");` at the requested position. Default: first line of the function body. Sub-scope: as the first line inside the requested `{ ... }` block.
3. Verify `#include "UiPerfMonitor.h"` is present in the file. Add it (alphabetised with other `Source/Core` includes) if missing.
4. Build to confirm no compile errors:
   ```
   cmake --build --preset ninja-iter-msvc --target SmatchetStandalone
   ```
5. Report each tuple as inserted / skipped (with reason if skipped).

### Cleanup mode (stripping)

1. Text-search for `SMATCHET_UI_PERF_SCOPE\("perf_temp:` across each of `Source/Core/`, `Source/Plugins/`, `Source/Standalone/`.
2. Delete every matching line (the macro statement; not the surrounding braces).
3. If a file now has an unused `#include "UiPerfMonitor.h"` (no other `SMATCHET_UI_PERF_SCOPE` calls remain), leave the include — other code in the file may add scopes later, and removing it is a wider judgement call.
4. Re-search all three directories to confirm zero matches.
5. Build `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone` to confirm clean compile after the deletions.

## Output contract

Per AGENTS.md § Agent output contract § Helper class — these sections must appear, in order, in every report:

- `## Spec executed` — the `(file, function-or-block, scope-name)` tuples (insert mode) OR the strip-target directories + the text-search pattern used (cleanup mode), copied verbatim from the caller's packet. One bullet per tuple.
- `## Result` — files touched + scope names added (insert) or removed (cleanup) + final `perf_temp:` text-search count (must be zero across `Source/Core/`, `Source/Plugins/`, `Source/Standalone/` after cleanup). Includes the build target name + pass/fail of the `cmake --build` step.
- `## Outcome: <state>` — one of `applied | halted | failed | partial | aborted`. Telemetry keys on this line per AGENTS.md § Agent output contract.
- `## Self-improvement` — only if the spec was ambiguous or a rule wasn't covered. Empty is fine. Orchestrator appends to `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md`.

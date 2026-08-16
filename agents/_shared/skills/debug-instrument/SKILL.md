---
name: debug-instrument
description: The deterministic mechanics of a Smatchet debug investigation — the NDJSON [temp-debug] instrumentation recipe, build + exe-staleness commands, the unified-CLI run reference, log/evidence reading (jq), sanitizer setup, cleanup, and the two report-shape templates. Invoked by debug-detective (which keeps the judgment: hypotheses, metric choice, the wait-for-feedback loop, hand-off). Use when instrumenting, reading a debug log, cleaning up [temp-debug] markers, or formatting a debug report.
triggers:
  - debug-instrument
  - temp-debug
  - ndjson-debug
  - debug-cleanup
  - debug-report
  - instrument
version: 1
---

<!-- Skill-only helper (no agent twin; registered in SKILL_ONLY_HELPERS). This is the
     EXTRACTED mechanics body of agents/core/debug-detective.md — the deterministic
     copy-paste-run recipes + report scaffolds. The agent keeps the reasoning and points
     here per section. reduce-agent-prompt-bloat Slice 2. Cross-harness: Codex/Cursor read
     the agent's summary + this path; Claude Code loads this skill on demand. -->

# debug-instrument (skill)

The mechanics `debug-detective` runs. The agent owns *what to do and why* (scope, hypotheses, the metric, the §7.5 pause-gate, hand-off); this skill owns *how* — the verbatim shell, the NDJSON helper, the report templates. Every temporary edit carries the literal token `[temp-debug]` so one text-search finds the full delta at cleanup.

## Instrument — the NDJSON debug-log helper

The canonical instrumentation tool. Writes one JSON object per call to a uniquely-named log (`debug-<hex>.log`), independent of `Logger::SetFileSinkPath`, schema `{sessionId, location, hypothesisId, message, data, timestamp}`. Per-investigation: fresh 6-hex ID at session start; helper + log are gitignored and removed at cleanup.

### Roll a session ID and write the helper

Pick one random 6-hex per investigation, e.g. `61b011`. Reuse across every helper + log file this session. Do **not** reuse a previous hex even when revisiting the same bug — that conflates logs from different runs.

**FIRST check whether `tests/_debug/SmatchetAgentDebug.h` is a tracked, committed product file** — it is NOT always a throwaway scratch path. There is a production-resident, tracked helper at that exact path (slice 7 of `docs/plans/shipped/autonomous-debugging-no-creds.md`) with a *different* API (`SMATCHET_AGENT_DEBUG_LOG(category, json_obj)`, closed-set categories, `<userData>/agent-debug/<session>.ndjson` sink). Rolling the per-investigation template over it `sed > file` would CLOBBER a committed file and silently change product behaviour:

```bash
git ls-files tests/_debug/SmatchetAgentDebug.h
```

- **If that prints the path (tracked)** → do NOT overwrite it and do NOT delete it at cleanup. Use the in-repo surface directly: `#include "../../tests/_debug/SmatchetAgentDebug.h" // [temp-debug]` then `SMATCHET_AGENT_DEBUG_LOG("temp-debug", nlohmann::json{{"loc", __FUNCTION__}, {"h", "h1"}, {"i", rowIndex}})` at the suspect site (the `"temp-debug"` category is a closed-set member reserved for exactly this; build with `SMATCHET_AGENT_DEBUG` defined — debug/asan/ui-test presets). The only `[temp-debug]` residue is your include + call lines, stripped at § Cleanup — the tracked file itself is left untouched.
- **Only if `git ls-files` prints nothing (path absent/untracked)** → roll the generic template into a scratch copy and replace every `__SMATCHET_AGENT_DEBUG_ID__` placeholder with the rolled hex:

```bash
# only when tests/_debug/SmatchetAgentDebug.h is ABSENT / untracked
git ls-files tests/_debug/SmatchetAgentDebug.h | grep -q . || {
  mkdir -p tests/_debug
  sed 's/__SMATCHET_AGENT_DEBUG_ID__/<hex>/g' \
      agents/_shared/templates/SmatchetAgentDebug.h.tmpl \
      > tests/_debug/SmatchetAgentDebug.h
}
```

Public API the template exposes (untracked-path branch only):

- `SmatchetAgentNdjsonLog(location, hypothesisId, message, dataInt)` — one NDJSON line per call.
- `SmatchetAgentDebugLogPath()` — repo-root drop (walks ≤ 12 parents for `.git`).
- `SmatchetAgentDebugTempLogPath()` — TEMP/`%TEMP%` fallback.

Schema per line: `{"sessionId":"<hex>","location":"...","hypothesisId":"h1|h2|...","message":"...","data":{"i":<long long>},"timestamp":<ms>}`.

Notes on the helper: `static` functions give per-TU copies (no ODR/link conflicts when `#include`d from multiple TUs in the same investigation); dual-target safe — no GLFW / OpenGL / DX12, compiles into both `SmatchetStandalone` and `SmatchetCore_DX12`; `ghc::filesystem` matches the project FetchContent dep (AGENTS.md § Available libs). The schema carries one `dataInt` per call — if a hypothesis needs string / multi-int data, extend the helper inline (still gitignored, still removed at cleanup) rather than letting the format drift at the call site.

### Add the include + call sites

In each TU you instrument, add the include with a `// [temp-debug]` marker, then call `SmatchetAgentNdjsonLog(...)` at the smallest set of sites that **distinguish the listed hypotheses from each other**.

```cpp
#include "../../tests/_debug/SmatchetAgentDebug.h"  // [temp-debug]

// ... later, at the suspect call site:
SmatchetAgentNdjsonLog(
    __FUNCTION__,                       // location
    "h1",                               // hypothesisId — one of the listed hypotheses
    "selection out of range",           // message — short, fixed text per call site
    static_cast<long long>(rowIndex));  // data.i — the value that distinguishes hypotheses
// [temp-debug]
```

Use a distinct `hypothesisId` (`"h1"`, `"h2"`, …) per listed hypothesis. The same call site can emit multiple breadcrumbs with different `hypothesisId`s when one site distinguishes two hypotheses by different values.

**Cross-boundary instrumentation** — the bug is almost always at the interface between two pieces of code that disagree about a contract: UI thread vs worker, command-dispatch vs handler, parser vs payload-builder, save vs load. **Instrument both sides of the boundary**, not just one. Each side calls the helper with a different `location` (or `hypothesisId`) so the NDJSON log lets you correlate them.

### Fallback to `LOG_DEBUG` / `LOG_TRACE`

Use the project Logger only when the NDJSON helper cannot be used — e.g. deep inside a header where pulling the helper include would be intrusive, or a code path that runs before `ghc::filesystem` is safe (very early bootstrap). Same `[temp-debug]` prefix rules:

- `LOG_TRACE("[temp-debug] %s ...")` inside tight loops, per-frame paths, per-cell paths.
- `LOG_DEBUG("[temp-debug] %s ...")` for occasional events.
- Never use `LOG_INFO`, `LOG_WARN`, or `LOG_ERROR` for temporary breadcrumbs.

### Rules (apply to both helper and fallback)

- Every temporary edit — helper include, helper call, `LOG_*` line, diagnostic toggle, sentinel value, repro scaffolding, anything that must not ship — carries the literal token `[temp-debug]` somewhere on its line. For log messages, prefix the format string; for non-log edits, add a trailing comment `// [temp-debug]`. One cleanup target catches every variant: the regex `\[temp-debug\]` (use the harness's text-search tool, not raw `grep -R`).
- Avoid instrumentation in headers, especially under `Source/Core/include/`.
- Do not add sleeps to "prove" races.
- Do not change behaviour unless explicitly doing a temporary diagnostic toggle, and revert it before completion.
- Keep one instrumentation round small, then build immediately.

Useful values to log (passed via `dataInt` for the helper or interpolated into the format string for the fallback):

- Object identity: stable ID first, pointer only if needed.
- Thread identity hash: `static_cast<long long>(std::hash<std::thread::id>{}(std::this_thread::get_id()))`. No `MainThreadDispatcher::IsMainThread()` helper exists; compare the logged thread-id hash against the UI-thread id captured at startup, or bracket the suspect call with a known-on-UI-thread breadcrumb posted via `MainThreadDispatcher::PostToMainThread`.
- Old and new values; container sizes and indices; ownership/lifetime transitions; return values and error codes; command/scenario names; file paths and normalized keys.

## Build + exe-staleness

Build after each instrumentation round:

```bash
cmake --build --preset ninja-iter-msvc --target SmatchetStandalone
```

If the touched code affects `Source/Core/`, also build:

```bash
cmake --build --preset ninja-iter-msvc --target SmatchetCore_DX12
```

If the build fails because of instrumentation, fix the instrumentation only — do not drift into product fixes. After a successful build, verify freshness and report the absolute exe path, size, and mtime so the user does not test a stale binary:

```bash
ls -la <absolute-path-to-Smatchet.exe>
```

**When the investigation needs the production NDJSON channel**, build the agent-debug preset instead — `SMATCHET_AGENT_DEBUG` is OFF in `ninja-iter-msvc`, so `SMATCHET_AGENT_DEBUG_LOG` compiles to `((void)0)` there and the sink stays empty no matter how the code is instrumented:

```bash
cmake --build --preset ninja-iter-agentdebug-msvc --target SmatchetStandalone
```

It is `ninja-iter-msvc` + `SMATCHET_AGENT_DEBUG=ON` with its **own** `build/ninja-iter-agentdebug-msvc/` tree. Two iter-shaped exes now exist, so name the absolute path you built and the one you ran — an empty NDJSON file is a real signal (§ Read evidence), but only once you have ruled out having run the other exe.

## Run — unified CLI reference

Auto-repro path (preferred): run the deterministic reproducer yourself, capture stderr + the NDJSON log.

```bash
Smatchet.exe cmd scenario.run --name=<repro> --frames=300 --yes 2> debug.log
Smatchet.exe cmd tickets.get --id=<id>             2> debug.log
ctest --preset ninja-test-msvc -R <UnitName>                 # for pure-logic repros
```

Discovery:

```bash
Smatchet.exe cmd commands.list --category=<cat>
Smatchet.exe cmd commands.help --name=<cmd>
Smatchet.exe cmd commands.search --query=<q>
```

Command groups (details via the discovery commands above):

- `debug.*` — `log` (emit a known breadcrumb into the runtime log) · `mcp_status` (MCP reachability / last activity) · `thread_dump` · `dock.dump` (ImGui dock nodes) · `dock.reset` (recovery only, not a diagnosis) · `window.resize` (reproduce layout regressions) · `window.screenshot` (viewport evidence) · `lua_eval` (probe runtime state without rebuilding).
- `scenario.*` — `list` (discover deterministic scenarios) · `run --name=<n> --frames=<N> --yes` · `cancel` (stop active automation).
- `tickets.list_active` / `tickets.get --id=<id>` (ticket state) · `sync.tracker_status` (sync-layer state) · `app.version` (confirm build hash/version).

Prerequisite: a running Smatchet instance with `mcp_enabled: true`.

## Read evidence — NDJSON log

**Primary path — the helper log.** Deterministic path keyed off the rolled hex; try the repo-root drop first, then the TEMP fallback:

```bash
ls -la "$(git rev-parse --show-toplevel)/debug-<hex>.log"   # repo root (helper walks up to .git)
ls -la "$TEMP/Smatchet-debug-<hex>.log"                      # Windows TEMP fallback
ls -la "$(pwd)/debug-<hex>.log"  ||  ls -la "/tmp/Smatchet-debug-<hex>.log"   # POSIX
```

Parse NDJSON (one JSON object per line):

```bash
jq -c 'select(.hypothesisId == "h1")' debug-<hex>.log            # all rows for one hypothesis
jq -c 'select(.location == "ApplySort")' debug-<hex>.log         # all rows from one call site
jq -c '{ts: .timestamp, loc: .location, h: .hypothesisId, i: .data.i, msg: .message}' debug-<hex>.log
```

If `jq` isn't on PATH, fall back to text-search: `grep -n '"hypothesisId":"h1"' debug-<hex>.log`.

**Fallback path — `LOG_DEBUG` / `LOG_TRACE`.** Smatchet file logging is opt-in via `Logger::SetFileSinkPath`. Check the conventional drop dir, then stderr capture:

```bash
ls "$LOCALAPPDATA/Smatchet"/*.log
Smatchet.exe 2> debug.log          # if file sink not active, relaunch with stderr captured
grep -n "\[temp-debug\]" debug.log
```

## Crash — sanitizer setup

For crashes, prioritize stack evidence before logs. Collect: faulting thread; top application frames; exception code / signal; assertion message; faulting address if available; and whether the crashing pointer/value was null, freed, or out of range.

Pick **one** sanitizer per investigation (they cannot coexist at link/runtime). Configure + build:

```bash
cmake --preset ninja-msvc-asan
cmake --build --preset ninja-msvc-asan --target SmatchetStandalone
```

Presets: ASan `ninja-msvc-asan` (MSVC; ASan only, no UBSan) or `ninja-clang-asan` (Clang; ASan+UBSan). TSan / MSan have **no dedicated preset** — set `-DSMATCHET_SANITIZER=tsan|msan` on a Clang preset (MSVC no-ops TSan; MSan needs `clang`/`clang++` on PATH → hand to `build-doctor`). Sanitizer runtime DLLs (`libasan-*.dll`, `libtsan-*.dll`, `libubsan-*.dll`, `libclang_rt.msan*.dll`) must be on `PATH` at launch — "DLL not found" on a sanitized exe is the runtime, not the build. If MSan errors `requires Clang`, `winget install LLVM.LLVM`. Wiring lives in `cmake/Sanitizers.cmake`; preset failures / new sanitizer requests go to `build-doctor`.

## Cleanup — four mandatory steps

Do all four before reporting done.

```bash
# 12a. Strip [temp-debug] markers (expect zero hits) — use the harness text-search tool
rg -n "\[temp-debug\]" Source/Core/ Source/Plugins/ Source/Standalone/

# 12b. Delete the per-investigation helper — ONLY if you rolled the template.
#      NEVER delete a TRACKED tests/_debug/SmatchetAgentDebug.h (the committed
#      production helper); if you used that surface directly there is no scratch
#      file to remove. Guard the rm on untracked-ness:
git ls-files tests/_debug/SmatchetAgentDebug.h | grep -q . || rm -f tests/_debug/SmatchetAgentDebug.h
rmdir tests/_debug 2>/dev/null || true

# 12c. Delete the NDJSON log (use the rolled hex)
rm -f "$(git rev-parse --show-toplevel)/debug-<hex>.log"
rm -f "$TEMP/Smatchet-debug-<hex>.log"
rm -f "/tmp/Smatchet-debug-<hex>.log"

# 12d. Rebuild clean (+ SmatchetCore_DX12 if Source/Core/ was touched)
cmake --build --preset ninja-iter-msvc --target SmatchetStandalone
```

Remove every temporary marker, diagnostic toggle, temporary repro artifact, and temporary behaviour change unless explicitly approved to keep it. Gitignore patterns (`debug-*.log`, `Smatchet-debug-*.log`, `tests/_debug/`) are a safety net, not a substitute — delete explicitly. Report cleanup status (zero `[temp-debug]` hits + helper deleted + log deleted) and final build status.

## Report shapes

Two shapes — pick by gate state. **Mid-loop reports** (at every § 7.5 pause) use the short shape; **final report** (after cleanup, handoff-ready) uses the long shape.

### Mid-loop report (at each § 7.5 pause)

```markdown
## Cycle <N>
Repro path: auto | ask-user
Build / exe: <absolute path + mtime>

## Hypotheses (ranked by distinguishing-evidence cost)
1. <cause #1>  — status: confirmed | rejected | open
2. <cause #2>  — status: ...
3. <cause #3>  — status: ...

## Evidence Delta (this round only)
<new log lines / sanitizer output / stack frames / metric reads>

## Next Step Proposal
propose-fix | next-round | re-frame | blocked
<one-paragraph rationale; for next-round include the call sites + metric to add>

## AWAITING USER FEEDBACK
<exact question or yes/no the agent expects back, e.g. "did the patched exe still freeze on drag-reorder?">

## Outcome: halted
```

### Final report (after cleanup, handoff-ready)

```markdown
## Hypotheses (final)
1. <cause #1>  — confirmed | rejected
2. <cause #2>  — rejected
3. <cause #3>  — rejected
(strike-through rejected lines)

## Reproducer
<exact steps, CLI command, scenario, crash artifact, or evidence source>

## Metric (observable, before / after)
Before fix: <observed value or sequence>
After fix:  <observed value or sequence>

## Evidence Collected
<stack trace, structured NDJSON / `[temp-debug]` log lines, sanitizer output, command output, screenshots, etc.>

## Files changed (temp-debug)
(Instrumentation files touched this round; every `[temp-debug]` marker stripped before this report.)
<files touched and temporary breadcrumbs added; note BOTH sides of any thread / subsystem / save-load boundary>

## Findings
<for each hypothesis: confirmed / rejected / replaced; cite evidence>

## Cause
<concrete explanation with file:line where possible>

## Promoted Logs (kept permanent — handed to subsystem owner)
- <file>:<line> · LOG_DEBUG | LOG_INFO · "<message>" · rationale
(≤ 3 entries; or the literal line "Nothing worth promoting.")

## Handoff (proposed fix)
Target agent: <subsystem-specialist>
Allowed write set: <files>
Decision pre-resolved: <interface deltas, invariant collisions, ownership/threading contract>
Verification: <build + scenario/repro to rerun + the metric to re-check>

## Cleanup
`[temp-debug]` text-search across `Source/Core/`, `Source/Plugins/`, `Source/Standalone/`: 0 hits
Helper deleted: tests/_debug/SmatchetAgentDebug.h → absent
NDJSON log deleted: debug-<hex>.log → absent
Final build: <targets> → <status>
Fresh exe: <absolute path + mtime>

## Outcome: applied
```

`## Outcome:` values — `halted` (mid-loop pause; awaiting feedback), `applied` (closed, cause pinned, cleanup done, handoff ready), `partial` (≥ 1 hypothesis confirmed but more rounds needed and user approved a concurrent subsystem specialist), `failed` (three rounds no progress + user aborted re-frame), `aborted` (user aborted before cause pinned).

## Scenario reuse + add

The agent owns *whether* to reuse / parametrize / fork / add (the reproducer-first contract judgment); this section is the *how* — the bug-class definition, the search recipe, and the scenario-add file mechanics.

**Bug-class** = the smallest grouping that shares:

- an **injection point** — which `ITrackerBackend` (GitHub / Plane / Jira / fake), which `IAiClient` (real / `StubAiClient`), which UI panel / command, which subsystem boundary, AND
- a **render path** — which scenario's `OnFinish` rows[] would have caught the regression (i.e., which `rows[]` emission shape matches the observable failure).

Search recipe (semantic search first, text-search second per AGENTS.md § Semantic codebase search):

```bash
ls Source/Core/src/Commands/Scenarios/
grep -l "<suspect-symbol-or-panel>" Source/Core/src/Commands/Scenarios/*.cpp
```

**If an existing scenario matches**, **parametrize** it (CLI arg / fixture variant / new sub-case in its `OnTick`) rather than fork a near-duplicate. Record the parametrization shape in the § Self-improvement `missing-scenario` entry.

**Forking allowed only** when the existing scenario's render path is *genuinely orthogonal* — e.g. same UI panel but the bug emits to a separate `rows[]` column that the existing scenario does not assert on. Document the orthogonality rationale in the report.

A deterministic reproducer means one of:
- a CLI command (`Smatchet.exe cmd <name> ...`),
- a `scenario.run --name=<x>` invocation,
- a Lua snippet,
- a failing-doctest name (`ctest -R <Unit>`),
- or a registered bucket-E ImGui-Test-Engine action.

**Scenario-add mechanics** (per slice 5 — `SmatchetScenarioRegistry` refactor):

- One new `.cpp` under `Source/Core/src/Commands/Scenarios/<NewScenarioName>Scenario.cpp` implementing the `IScenario` interface, with an `OnFinish` rows[] emission shape matching the observable failure (phase 0 dimension b).
- One new line in `Source/Core/src/Commands/Scenarios/SmatchetScenarioRegistry.cpp`'s registration table — **no `AppController.cpp` edit** (the registry refactor consolidated that).
- The scenario-add lands on the **same branch as the fix**, not a precursor PR.
- Crash logs, minidumps, stack traces, assertion text, and sanitizer reports remain valid *evidence* — they still feed phase 0 dimension (b) — but they are not, by themselves, a reproducer. The agent still wires a scenario that triggers them deterministically.

If the bug is intermittent, the new scenario must define a repeat loop and an expected failure signal (assertion / log line / `rows[]` value) so the loop is deterministic-by-construction. (Run-command shapes → § Run; the crash-collect checklist → § Crash — sanitizer setup.)

## Hypothesis + metric examples

The agent owns the hypothesis/metric *rules* (≥ 2 falsifiable causes ranked by distinguishing-evidence cost; a metric recorded before instrumenting and re-checked after the fix). These are the worked examples.

Good hypothesis list (bad = a single unfalsifiable "It is probably a race."):

> 1. `TicketGridModel::ApplySort` invalidates row indices before `TicketSelection::Restore` reads them.
> 2. `OnFieldEditCommit` runs on a worker thread while UI iterates the same `rows_` vector.
> 3. `kCurrentLayoutSchemaVersion` mismatch silently resets selection during config load.

Metric examples (observable value the bug produces vs what the fix should produce):

- Bug → `selectedRowIndex = 2` after sort; fixed → `selectedRowIndex` follows the moved ticket.
- Bug → second sync replays `pending_creates` count = 3; fixed → count = 0.
- Bug → log shows `Draw` reading `rows_.size() == 0` then `5` in same frame; fixed → size stable across frame.

## Evidence-source catalogue

Prefer existing evidence before adding logs: stack trace, assertions, existing logs, command output, state-dump commands, sanitizer reports, debugger watch/backtrace, existing tests. Only instrument when existing evidence cannot distinguish the hypotheses from each other.

## Race / ordering checklist

A race hypothesis must name the specific read, write, and missing ordering/synchronization edge. For suspected races: identify the shared state, all writers, the expected owning thread, and the synchronization contract; log thread identity and sequence numbers; prefer deterministic scheduling evidence over timing guesses; do not add sleeps as proof; use TSan if supported.

## Handoff

Include in the handoff packet:

- Target agent.
- Concrete cause (file:line where possible).
- Files likely involved.
- Allowed write set.
- Interface decisions already resolved.
- Invariants that must be preserved.
- Exact repro to rerun.
- The metric to re-check on the fixed build.
- Build targets to verify.
- Any temporary instrumentation already removed.

## Promote logs — mechanics

The agent owns the promotion *criteria* (boundary line, state-transition/error-edge, helped-this-and-a-future-investigation, ≤ one cache line, hard cap ≤ 3). These are the mechanics:

1. Pick the level — `LOG_DEBUG` for development-time breadcrumbs, `LOG_INFO` for shipped operational state-transitions, never `LOG_TRACE` (tight loops only — promote only if you have already proven the cost is negligible at 144 Hz).
2. Strip the `[temp-debug]` marker from the line; the line becomes part of the permanent codebase.
3. Replace any NDJSON-helper call with the project `LOG_*` macros — `tests/_debug/SmatchetAgentDebug.h` is deleted at § 12b, so its calls cannot survive.
4. Rewrite the message into the project logger style (`LOG_DEBUG("module: did X with id=%d", id)`); drop the `__FUNCTION__` boilerplate (logger adds source location already).
5. The promoted line is part of the subsystem-specialist handoff, not a free agent edit — list each promoted line in the handoff packet with file:line so the specialist agrees before commit.

If zero lines meet the criteria, say so explicitly in the report. "Nothing worth promoting" is a valid and common outcome.

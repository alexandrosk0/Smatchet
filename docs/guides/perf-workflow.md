<!-- tier: project -->
# Smatchet — perf workflow

**When to read this file**: the app is slow, has low FPS, lags, hitches, stutters, or has a brief or intermittent stall or freeze that recovers on its own ("sometimes hangs for a second"); a grid scroll, autocomplete or startup is slow; memory or CPU use is high; the user asks to optimize or profile; a perf regression needs diagnosing. A hang that never recovers is a behavioural bug — the `debug-instrument` skill (hang capture), not this file. If the task is unrelated to performance, do not load this file.

This is the canonical perf procedure. Three layer skills run its mechanics: `perf-instrument` (add / strip `perf_temp:` markers), `perf-measure` (run a scenario, read the rows, halt on a CLI gap) and `perf-gatekeeper` (PR-time scenario check against baselines). The curated diff→scenario map is [`scripts/dev/perf-scenario-map.json`](../../scripts/dev/perf-scenario-map.json).

---

## Rule 0: never guess

The first FPS-regression PR on this codebase missed the dominant cost twice in a row by reasoning from the code alone. Build the loop on actual numbers from the running exe.

State the hypothesis in one sentence before any tool call. "It's the X" without a measurement is not a hypothesis.

## Steady state or spike?

Classify the symptom first — the two need different captures and different fixes.

- **Steady state** — slow all the time on a path (low FPS while scrolling, slow autocomplete, slow startup). It shows in frame averages: a row whose `calls × avgPerCallMs ≈ lastTotalMs`. Follow § The loop.
- **Spike** — an intermittent hitch, a rare 100 ms+ freeze, an unpredictable pause. It hides in averages: a row whose `lastTotalMs` is one bad call (`calls = 1`, `lastTotalMs ≈ maxMs`), or whose `p99Ms` sits far above its `avgPerCallMs`. Follow § Spikes.
- **High memory or CPU** — `cmd perf.memory` returns RSS plus the icon-cache, plan-cache and dispatcher gauges; take it before and after on the same scenario, under the same never-guess rule. CPU burned at idle is per-frame or worker work: measure it as steady state.

**Known hot paths — measure, don't guess.** Map the symptom to code with semantic search, then check these first:

- Grid per-cell render: `TicketGridModel`, `TrackerGridFieldDisplay`, `SmatchetFieldRender`, `SmatchetFieldIconRender`
- JQL / Plane query suggest: `JqlSuggestEngine`, `PlaneQuerySuggestEngine`
- Lua cell renderers: per-frame Lua dispatch measured ~390 µs per cell against ~5 µs per cell on the cached-replay path (`register_field_display_cached`; measured in `docs/plans/shipped/lua-recorded-cmd-list.md`, summarised in `docs/guides/lua.md` § 5), so an uncached renderer on a visible grid is a prime suspect
- ImGui font / texture cache: `SmatchetImGuiFonts`, `SmatchetImageTextureCache`
- Tracker HTTP: `TrackerHttpClient` (cpr / curl)
- Startup: `AppController`, `Views`, `LocalCacheManager`

## The loop

### 1. Instrument

Wrap suspected hot paths in `SMATCHET_UI_PERF_SCOPE("perf_temp:<area>")` from `Source/Core/include/Ui/UiPerfMonitor.h`. Write the plan as a spec — `[(file, function-or-block, scope-name), …]`, every name prefixed `perf_temp:` — and apply it with the `perf-instrument` skill.

- **Always prefix new markers with `perf_temp:`** so cleanup is mechanical (one text search) and the prefix never collides with production scope names.
- Committed scopes already cover the frame (`SmatchetUI::Draw`), the dispatcher drain (`MainThreadDispatcher::Drain`) and many panels — [`docs/perf/MARKER_INVENTORY.md`](../perf/MARKER_INVENTORY.md) lists them. Read those rows before adding a marker; never retag one.
- Cover the whole hypothesis tree: the call site, every candidate sub-call, and the surrounding render-plan branch — but read the overhead note below before going deep inside per-cell loops.
- Scope names must be **string literals**: the scope keeps the raw `const char*` until it ends, so `c_str()` of a temporary dangles.
- Insert the whole spec, then rebuild once — one build per measurement cycle, not one per marker. The `.claude/.tree-dirty` sentinel records edits since the last build.

**Marker overhead — non-trivial.** Each scope does: 2× `steady_clock::now()`, a mutex lock, and an O(N) linear scan over the working set. Per-call cost is ~200–500 ns with ~20 active names. With 100 cells × 5 nested scopes that's 100–250 µs/frame — visible at 144 Hz. Implications:

- One wrapping scope around the whole hot loop (e.g. the rows-clipper block) is **always safe**.
- Targeted sub-scopes inside per-cell code are fine for **relative** ranking only — their absolute ms values are inflated by instrumentation overhead.
- Never nest a `perf_temp:` scope inside something that runs millions of times per frame. Move the scope outward.

```cpp
void RenderFooCell(...) {
    SMATCHET_UI_PERF_SCOPE("perf_temp:RenderFooCell");
    {
        SMATCHET_UI_PERF_SCOPE("perf_temp:RenderFooCell.resolve");
        // ...
    }
    DrawFoo(...);  // already has its own non-temp scope — don't double-wrap
}
```

---

### 2. Build & measure

```
cmake --build --preset ninja-iter-msvc --target SmatchetStandalone
```

Name the exact exe you measure (`ls -la` the candidate build outputs, compare mtimes — `agent-layer/docs/agent-rules/debug-techniques.md` § Exe staleness check). The `perf-measure` skill runs the paths below. Use the highest path that applies:

---

#### Path A1 — Named scenario (fully automated, no user needed)

**Use when:** a registered scenario exists for the slow path. No running app, no user clicks, no waiting — `--spawn` handles everything.

```bash
# Check available scenarios (also uses --spawn if no instance running)
build/ninja-iter-msvc/Smatchet.exe cmd scenario.list --spawn

# Run the scenario end-to-end. Claude executes this alone.
build/ninja-iter-msvc/Smatchet.exe cmd scenario.run \
  --name=priority-grid-scroll --frames=600 --outPath=perf_before.json --spawn --yes
```

`--spawn` flow (all automatic): detects no running instance → launches a hidden ephemeral app on a free port (`GLFW_VISIBLE=false`) → polls until MCP is reachable (up to 30 s; `SMATCHET_SPAWN_READY_MS` overrides) → sends `scenario.run` → waits for the result file (`frames/60 s` + 30 s buffer) → reads the file → sends `app.quit` → exits.

Read the result directly once the command returns — no paste, no panel:

```python
Read("perf_before.json")
# data.rows[] sorted by lastTotalMs descending — filter perf_temp:* entries
```

**Pre-flight for audits, sweeps and spec docs**: before composing a scenario list, run `cmd scenario.list --spawn` and intersect the requested scenarios with the registered ones. Report missing ones as gaps rather than failing the request; author them via Path A3 or accept the smaller working set.

---

#### Path A2 — Ad-hoc snapshot (app running, no registered scenario)

**Use when:** exploring a path no scenario covers yet; the user drives the app to the slow screen. A2 numbers steer the hypothesis, but they never validate a fix or gate a PR — add the scenario (A3) and validate on A1.

```bash
# Reset so stale data from prior frames doesn't pollute the snapshot
build/ninja-iter-msvc/Smatchet.exe cmd perf.reset

# ← user navigates to the slow screen and reproduces for ~5 s

# Pull rows directly into context — no panel, no paste
build/ninja-iter-msvc/Smatchet.exe cmd perf.snapshot --pretty
```

Filter to `perf_temp:*` rows only:

```bash
build/ninja-iter-msvc/Smatchet.exe cmd perf.snapshot \
  | jq '[.data.rows[] | select(.name | startswith("perf_temp:"))]'
```

Top rows for frame-budget context:

```bash
build/ninja-iter-msvc/Smatchet.exe cmd perf.snapshot \
  | jq '.data.rows[:10]'
```

---

#### Path A3 — Extend the CLI with a new scenario

**Use when:** A1 has no matching scenario. A scenario is permanent and every later investigation reuses it, so add it rather than repeating A2.

1. Create `Source/Core/src/Commands/Scenarios/<Name>Scenario.cpp` implementing `IScenario` (`Source/Core/include/Commands/Scenarios/IScenario.h` — every hook takes an `IAppScenarioHost&`). `IdleScenario.cpp` is the minimal template: `OnStart` calls `UiPerfMonitor::Instance().Reset()`, `OnFrame` drives the slow path, and `OnFinish` returns `{"rows": UiPerfRowsToJson(UiPerfMonitor::Instance().GetLastFrameRows(/*includeP99=*/true))}` (`Ui/UiPerfRowsJson.h`) — the row shape `perf-baseline.sh` and the perf CI lane read. End with a `Make<Name>Scenario()` factory.
2. Register it in `Source/Core/src/Commands/Scenarios/SmatchetScenarioRegistry.cpp` (an `extern` factory declaration plus one `runner.RegisterFactory("<name>", …)` line), and add the name to the registry snapshot test `tests/Core/SmatchetScenarioRegistry.test.cpp` and its stub list `tests/Core/SmatchetScenarioRegistry.stubs.cpp`. The core source glob picks up the new TU; no CMake edit.
3. Add a rule for the files it covers to [`scripts/dev/perf-scenario-map.json`](../../scripts/dev/perf-scenario-map.json).
4. Build, then use Path A1 with `--name=<name>`.

The `test-authoring` skill (§ Pattern B) has the bash-assert side of a scenario test.

---

#### No manual path

There is no paste-the-Performance-Monitor fallback. When the slow path cannot be driven programmatically — no scenario, no CLI surface, `--spawn` cannot reach MCP, or `perf.snapshot` errors — the measurement is **blocked**: the `perf-measure` skill ends with `## Outcome: halted` and `halt_reason: cli-gap — <missing surface>`. Close the gap (a scenario via Path A3, or the missing CLI surface; a build or MCP failure is a build fix) instead of asking the user to eyeball the UI — a manual reading satisfies neither the measurement nor Pillar 2's zero-manual-verification rule. Never read FPS from the GUI yourself; the real-window number comes from § Real wall-clock FPS.

---

### 3. Diagnose from the numbers

- The dominant `perf_temp:*` row by `lastTotalMs` is the target. Sort by that, not `avgPerCallMs` — a 200-call × 50 µs row beats a 1-call × 5 ms row when recovering frame time.
- If no `perf_temp:*` row stands out, the markers are at the wrong granularity — add finer sub-scopes inside the suspected scope and re-measure before editing.
- If the dominant row is a pre-existing (non-temp) scope, still treat it as the target.

### 4. Change, rebuild, re-measure

Design the **smallest** diff that removes the dominant cost — no "while I'm in there". Implement it under the touched subsystem's leaf `AGENTS.md`. Rebuild, then re-measure using the **same path (A1/A2/A3) and the same scenario and frame count as the baseline**:

- **A1/A3** — rerun `cmd scenario.run` with `--outPath=perf_after.json`. Compare both files (see step 5).
- **A2** — `cmd perf.reset`, user reproduces, `cmd perf.snapshot` — to steer only; validate on A1.

Same view, same scroll pattern, same row count — otherwise the comparison is noise.

### 5. Validate

**A1/A3 — diff before/after files directly:**

```bash
jq -s '
  [ .[0].data.rows[] | select(.name | startswith("perf_temp:")) ] as $before |
  [ .[1].data.rows[] | select(.name | startswith("perf_temp:")) ] as $after |
  $before[] as $b | $after[] | select(.name == $b.name) |
  { name,
    before_ms: $b.lastTotalMs,
    after_ms:  .lastTotalMs,
    drop_pct:  ((($b.lastTotalMs - .lastTotalMs) / $b.lastTotalMs * 100) | round) }
' perf_before.json perf_after.json
```

**Ruling:**
- ≥30% drop on the dominant `perf_temp:*` row **and** the reported symptom confirmed gone → fix worked. Confirm the symptom by measurement wherever one exists (the `[fps-measure]` line in § Real wall-clock FPS for an FPS complaint); the user's confirmation counts only for a symptom no measurement reproduces.
- Drop <30% or another row now dominates → **iterate**, don't declare victory.
- FPS recovered but no `perf_temp:*` row shows the win → markers don't cover the changed path; add finer scopes and re-measure.

**Why AND, not OR**: the regression gate this fix must later pass (`perf-compare.py` against `docs/perf/regression-policy.json`, run by the `perf-gatekeeper` skill and the Perf PR-fast CI lane) decides on measured rows only, and each of its relative checks fires only when all of its conditions hold (a relative delta AND an absolute floor). An OR that accepts "the user reports a clear FPS recovery" on its own would accept a fix with no measured drop — exactly the case the third bullet sends back for re-instrumenting.

### 6. Clean up

1. Strip every `perf_temp:` marker from source (`perf-instrument` skill, cleanup mode).
2. Verify with a text search across `Source/` and `tests/` (skip `ThirdParty/`):

```
Grep(pattern: 'SMATCHET_UI_PERF_SCOPE\("perf_temp:', path: 'Source', output_mode: 'files_with_matches')
```

Zero matches before the PR commit. `scripts/dev/test-perf-marker-inventory.sh` fails the test pass on any leftover `perf_temp:` marker.

3. Reset the monitor so stale rows don't appear in the next session:
```bash
build/ninja-iter-msvc/Smatchet.exe cmd perf.reset
```

4. Delete `perf_before.json` / `perf_after.json` scratch files (not committed).

**Report**: the hypothesis, before / after numbers for the dominant row, the diff summary and the cleanup confirmation.

---

### 7. Gate-check vs baseline

After cleanup, gate the change against the checked-in baseline registry. This is the merge-blocking step for feature PRs touching `Source/Core/`. Pick the scenarios from [`scripts/dev/perf-scenario-map.json`](../../scripts/dev/perf-scenario-map.json) (the `perf-gatekeeper` skill classifies a diff with it). See `docs/plans/shipped/pillar-1-2-perf-review-system.md` § C1 + § Slice 1 for the full design.

```bash
# Run a fresh snapshot through the canonical driver (harness-agnostic).
# Capture the result-path on stdout's last line so the diff command can
# reference it without a literal <timestamp> placeholder.
FRESH_JSON="$(bash scripts/dev/perf-run.sh <scenario-id> | tail -n1)"

# Diff against the checked-in baseline.
python scripts/dev/perf-compare.py \
    docs/perf/baselines/<scenario-id>.dev.json \
    "$FRESH_JSON"
# → exits 0 if within docs/perf/regression-policy.json thresholds;
#   1 on regression; 2 on malformed input.
```

For CI: the PR-fast workflow (`.github/workflows/perf-pr-fast.yml`) runs the same comparison automatically over the fixed set in `scripts/dev/perf-pr-fast-set.json` and posts a markdown delta table as a PR comment. For local-only iteration: invoke the two commands above before opening the PR.

Baselines are PER-HOST — local development uses `<scenario>.dev.json`; CI uses `<scenario>.ci-windows-latest.json`; a P4-mode machine uses `<scenario>.$SMATCHET_PERF_HOST.json` ([`docs/perforce/SETUP.md`](../perforce/SETUP.md) § Per-machine perf baseline). Don't share or commit the wrong host's baseline, and a missing baseline is `MISSING_BASELINE`, never a pass.

To update a baseline after an INTENTIONAL change (e.g. a perf improvement that legitimately lowers the numbers):

```bash
bash scripts/dev/perf-baseline.sh bump <scenario-id>
# → re-captures + overwrites docs/perf/baselines/<scenario-id>.dev.json
```

The bump path requires explicit confirmation by default (or `--yes` for CI / scripted use). Always include the new baseline file in the same PR that lowered the numbers.

---

## Judgement rules

- **Never add a cache** without showing its measured miss-rate and per-call cost.
- **A container swap is a hypothesis, not a fix.** For new hot-path code, an ordered `std::map` whose order nothing reads defaults to `std::unordered_map`. As a fix for a measured hotspot, never assume `std::unordered_map` beats `std::map` — hashing string keys and small maps can lose — so profile both and keep the swap only if the dominant row drops.
- **Never disable a feature** ("just don't draw it") as a perf fix unless the user agrees to the trade-off.
- **Never skip the re-measure.** A "should be faster" change that doesn't move the number gets reverted.
- **Extend the CLI / scenarios, never substitute a manual UI session.** If the validating scenario does not exist, add it (Path A3) in the same PR. The measurement is the deliverable.
- **Skip clause** — a fix whose cost is obvious from the code (e.g. removing a confirmed per-frame disk read) may skip the `perf_temp:` instrumentation, never the before/after measurement on the committed scopes. The cheapest mistake is a "fix" that didn't move the number.

---

## Spikes

An intermittent stall is a single bad call hidden among thousands of fast ones. Anything that runs on the UI thread must finish in well under one frame, every time. The blocking-call checklist and its indirect spike sources live in [`Source/Core/src/Ui/AGENTS.md`](../../Source/Core/src/Ui/AGENTS.md) (*UI-thread non-blocking*), the UI-thread model and worker roster under its *Before you edit*; this section is the measurement procedure around them.

1. **Characterise.** Ask how often, how long (ballpark ms) and which action triggers it. With a live repro, take a long capture during normal use (Path A2) — to steer, not to validate.
2. **Scan** the area the symptom implicates for the Ui leaf's blocking-call checklist and its indirect sources (dispatcher bursts, lock contention, Lua over budget, first-touch cold caches, background-apply bursts). Semantic search first, so you see what calls the suspected blocker. `bash scripts/dev/pillar2-scan.sh <files>` flags synchronous I/O in UI-reachable translation units that lacks a `/* PILLAR2_WORKER_ONLY */` + `// est-latency:` marker — a fast first pass, not a reachability proof.
3. **Hypothesis.** Name the specific blocking call you suspect and which thread runs it today.
4. **Instrument** (spec for `perf-instrument`): the frame and the drain are already scoped — read the `SmatchetUI::Draw` and `MainThreadDispatcher::Drain` rows rather than adding outer markers. Add a `perf_temp:` sub-scope to each suspect posted-lambda body and around every suspected blocking call site (`perf_temp:HttpGet`, `perf_temp:SqlExec`, `perf_temp:P4Annotate`, …).
5. **Long capture.** `scenario.run --frames=3000` or more on the same scenario before and after. Snapshot rows describe the **last** frame: `maxMs` is that frame's worst single call, while `p99Ms` covers the scope's most recent 256 calls across frames — so a spike earlier in the capture shows in `p99Ms`, not `maxMs`. `cmd perf.memory` adds the dispatcher gauges (`dispatcherQueueLen`, `dispatcherLastDrainTasks`, `dispatcherLastDrainDeferred`) that expose a drain burst.
6. **Classify.** A row whose `lastTotalMs ≈ maxMs` with `calls = 1`, or whose `p99Ms` towers over `avgPerCallMs`, is a spike. A row with `calls × avgPerCallMs ≈ lastTotalMs` is steady state — go to § The loop. `avgPerCallMs` lies about spikes.
7. **Fix** — move the blocking call off the UI thread with the existing patterns:
   - HTTP → worker thread; return the result via `PostToMainThread`.
   - SQLite write → chunk and post across frames; never block the UI on the write.
   - p4 → the `AnnotateAnalysisUi` pattern (worker thread + future polled per frame).
   - File I/O → `std::async(std::launch::async, …)` + poll with `wait_for(0s)`.
   Add no new threading primitive: mirror the per-subsystem `std::thread` + mutex + atomic shutdown flag + destructor join, and post back through `MainThreadDispatcher`. Never move HTTP / SQLite / p4 onto the UI thread "for simplicity" — that is the cause of most spikes. An `std::async` future that captures by reference must be waited before the captured object dies (`DrainUiDrawSessionFuturesBeforeAppTeardown`, `Source/Core/include/Ui/SmatchetUI.h`).
8. **Validate.** Re-measure the same scenario for the same duration. The suspect row's `maxMs` / `p99Ms` should drop to drained-on-UI cost (under ~1 ms). If the stutter goes away but no scope shows the win, the cause is elsewhere — back to step 2. Never claim a fix from a build pass or intuition.
9. **Clean up** per step 6 of § The loop. Report the spike source (call site), the suspect row's `maxMs` / `p99Ms` before and after, the diff summary and the cleanup confirmation.

---

## Real wall-clock FPS (vsync / GPU present)

The `scenario.run` path above measures **UI-thread CPU time** under headless software GL (Mesa) — it
does NOT capture real framerate (no GPU, no vsync, no present). For the actual perceived FPS, run the
real standalone window with the env-gated measurement built into `RunFrameLoop` (`FpsMeasure` in
`Source/Standalone/main.cpp`):

```bash
# Real perceived FPS (vsync ON → capped to monitor refresh). Warmup excludes startup/sync churn.
SMATCHET_FPS_MEASURE_SECONDS=12 SMATCHET_FPS_WARMUP_SECONDS=5 build/ninja-iter-msvc/Smatchet.exe
# Uncapped max throughput (vsync OFF).
SMATCHET_FPS_MEASURE_SECONDS=12 SMATCHET_FPS_WARMUP_SECONDS=5 SMATCHET_FPS_VSYNC=0 build/ninja-iter-msvc/Smatchet.exe
```

Runs the real GL window for N seconds (after the warmup), prints one `[fps-measure]` line to stderr —
`frames=… vsync=on|off | avg | best | p99 | worst` (frame-ms + fps each) — then auto-quits. Zero cost
when the env var is unset. **Always set a warmup** (≈5 s; the default is 0.5 s): the first few seconds include catalog
fetch + cache hydrate + plugin init, whose hitches otherwise dominate p99/worst and read as a false
steady-state regression. The live in-app readout is `Inspect > Performance Monitor...` (`SmatchetPerfUi`).

---

## CLI reference

The CLI talks to a running Smatchet instance via MCP HTTP. Discovery order: `--mcp-host` / `--mcp-port` flags → `SMATCHET_MCP_HOST` / `SMATCHET_MCP_PORT` env vars → PID-verified `instance.json` in the user-data dir → default.

**Global flags (all `cmd` invocations):**

| Flag | Effect |
|------|--------|
| `--pretty` | Indent stdout JSON (2 spaces) |
| `--quiet` / `-q` | Bare scalars / NDJSON id-per-line for lists; pipe-friendly |
| `--yes` | Confirm destructive command (no prompt) |
| `--dry-run` | Preview mutation without applying; exit 9 if unsupported |
| `--tokens` | Estimate output size to stderr, no stdout produced |
| `--spawn` | Launch a hidden ephemeral app instance if none reachable; quit it when done |
| `--timeout=<ms>` | Cap async wait; 0 = no cap |
| `--mcp-host=<h>` | Override host |
| `--mcp-port=<p>` | Override port |

**Exit codes:**

| Code | Meaning |
|------|---------|
| 0 | success |
| 2 | unknown-command |
| 3 | validation-error / missing-required-arg |
| 4 | handler-error / backend-error / not-found |
| 5 | confirm-required (destructive without --yes) |
| 6 | not-connected |
| 7 | transport-error |
| 8 | timeout |
| 9 | dry-run-unsupported |

**Wire JSON:** `{ok, command, data: <payload>}` on success; `{ok:false, command, error:{code, message, hint?, suggestions?, details?}}` on failure.

**Perf commands:**

| Command | Params | Returns |
|---------|--------|---------|
| `perf.snapshot` | — | `{rows:[{name, lastTotalMs, avgPerCallMs, maxMs, calls, lifetimeHits, emaAvgMs, p99Ms}]}` — rows of the last drawn frame |
| `perf.memory` | — | `{rssBytes, dispatcherQueueLen, dispatcherLastDrainTasks, dispatcherLastDrainDeferred, iconCacheEntries, iconCacheApproxBytes, activeTicketCount, pendingThumbnailUploads, planCacheApproxBytes}` |
| `perf.dump` | `outPath?` | `{file, count}` |
| `perf.reset` | — | `{reset:true}` |
| `perf.frame_count` | — | `{scopeCount, totalCalls}` |
| `perf.toggle_panel` | `open?:bool` | `{showPerformance:bool}` |
| `scenario.list` | `limit?, offset?` | paginated array of names |
| `scenario.run` | `name` (req), `frames?`, `outPath?` | `{running:true, outPath}` — use `--spawn` for fully-automated; CLI waits for file and prints result |
| `scenario.cancel` | — | `{wasCancelled:bool}` |

**Adding a scenario:** § Path A3.

**Instrument macros:**
- `SMATCHET_UI_PERF_SCOPE(name)` — `Source/Core/include/Ui/UiPerfMonitor.h`
- `UiPerfMonitor::Instance().GetLastFrameRows()` — `Source/Core/src/Ui/UiPerfMonitor.cpp`
- UI panel: `Inspect > Performance Monitor...`

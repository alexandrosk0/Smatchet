# Smatchet — perf workflow

**When to read this file**: the user has asked to optimize, profile, or fix FPS / lag / hitch / "slow" / dropped frames. If the task is unrelated to performance, do not load this file.

---

## Rule 0: never guess

The first FPS-regression PR on this codebase missed the dominant cost twice in a row by reasoning from the code alone. Build the loop on actual numbers from the running exe.

## The loop

### 1. Instrument

Wrap suspected hot paths in `SMATCHET_UI_PERF_SCOPE("perf_temp:<area>")` from `Source_Core/include/UiPerfMonitor.h`.

- **Always prefix new markers with `perf_temp:`** so cleanup is mechanical (one Grep call) and the prefix is unique enough to never collide with production scope names.
- Cover the whole hypothesis tree: the call site, every candidate sub-call, and the surrounding render-plan branch — but read the overhead note below before going deep inside per-cell loops.
- Pre-existing non-`perf_temp:` scopes stay; do not retag them.
- Scope names are `const char*`, compared by string equality — always use a **string literal**, not `std::string::c_str()` from a temporary.

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
cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone
```

The CLI is available. Use the highest path that applies:

---

#### Path A1 — Named scenario (fully automated, no user needed)

**Use when:** a registered scenario exists for the slow path. No running app, no user clicks, no waiting — `--spawn` handles everything.

```bash
# Check available scenarios (optional — also uses --spawn if no instance running)
build/ninja-iter-msys2/Smatchet.exe cmd scenario.list --spawn

# Run the scenario end-to-end. Claude executes this alone.
build/ninja-iter-msys2/Smatchet.exe cmd scenario.run \
  --name=priority-grid-scroll --frames=600 --outPath=perf_before.json --spawn --yes
```

`--spawn` flow (all automatic): detects no running instance → launches a hidden ephemeral app on a free port → polls until MCP is reachable (up to 15 s) → sends `scenario.run` → waits for the result file (`frames/60 s` + 30 s buffer) → reads the file → sends `app.quit` → exits. The spawned window is invisible (`GLFW_VISIBLE=false`).

Read the result directly once the command returns — no paste, no panel:

```python
Read("perf_before.json")
# data.rows[] sorted by lastTotalMs descending — filter perf_temp:* entries
```

---

#### Path A2 — Ad-hoc snapshot (app running, no registered scenario)

**Use when:** no named scenario exists yet. Requires the user to navigate to the slow screen — the only manual step. Prefer A3 (add a scenario) to eliminate even this step next time.

```bash
# Reset so stale data from prior frames doesn't pollute the snapshot
build/ninja-iter-msys2/Smatchet.exe cmd perf.reset

# ← user navigates to the slow screen and reproduces for ~5 s (only manual step)

# Pull rows directly into context — no panel, no paste
build/ninja-iter-msys2/Smatchet.exe cmd perf.snapshot --pretty
```

Filter to `perf_temp:*` rows only:

```bash
build/ninja-iter-msys2/Smatchet.exe cmd perf.snapshot \
  | jq '[.data.rows[] | select(.name | startswith("perf_temp:"))]'
```

Top rows for frame-budget context:

```bash
build/ninja-iter-msys2/Smatchet.exe cmd perf.snapshot \
  | jq '.data.rows[:10]'
```

---

#### Path A3 — Extend the CLI with a new scenario

**Use when:** A1 has no matching scenario, and A2 requires too much manual navigation to be repeatable. **Prefer this over manual (Path B)** — a scenario is permanent and usable in future investigations.

**How to add a scenario:**

1. Create `Source_Core/src/Commands/Scenarios/<Name>Scenario.cpp` implementing `IScenario`:

```cpp
#include "Commands/Scenarios/IScenario.h"
#include "AppController.h"
#include "SmatchetUiSession.h"
#include "UiPerfMonitor.h"

class MySlowPathScenario : public IScenario {
public:
    std::string Name() const override { return "my-slow-path"; }

    void OnStart(AppController& app, const nlohmann::json& args, std::string& outErr) override {
        frames_ = args.value("frames", 600);
        // Activate the right view, reset perf monitor
        app.GetUiState().ViewState.Activate(args.value("viewId", std::string("default")));
        UiPerfMonitor::Instance().Reset();
    }

    void OnFrame(AppController& app, int /*frameIndex*/) override {
        // Drive the slow path: scroll, interact, etc.
        app.GetUiState().scenarioScrollTarget += 8;
    }

    bool IsDone(int frameIndex) const override { return frameIndex >= frames_; }

    nlohmann::json OnFinish(AppController& /*app*/) override {
        auto rows = UiPerfMonitor::Instance().GetLastFrameRows();
        nlohmann::json j = nlohmann::json::array();
        for (const auto& r : rows)
            j.push_back({{"name",r.name},{"lastTotalMs",r.lastTotalMs},
                         {"avgPerCallMs",r.avgPerCallMs},{"calls",r.calls}});
        return {{"rows", j}};
    }

private:
    int frames_ = 600;
};
```

2. Register one line in `Source_Core/src/Commands/BuiltinCommands.cpp` in the scenario registration block:

```cpp
app.Scenarios().RegisterFactory("my-slow-path",
    []{ return std::make_unique<MySlowPathScenario>(); });
```

3. Build, then use Path A1 with `--name=my-slow-path`.

---

#### Path B — Manual (last resort)

**Use only when** the slow path genuinely cannot be driven programmatically (e.g. requires real user typing to trigger, or is a race condition that disappears under automation).

Ask the user to:
1. Run the rebuilt exe.
2. Open **`Inspect > Performance Monitor...`**.
3. Reproduce the bad scenario (same view, same scroll, same row count).
4. Paste back all rows whose `name` starts with `perf_temp:` plus the top ~10 rows overall.
5. Optionally report FPS before/after for a sanity check against marker totals.

**Don't attempt to read FPS yourself on Path B** — Claude can't observe the GUI.

---

### 3. Diagnose from the numbers

- The dominant `perf_temp:*` row by `lastTotalMs` is the target. Sort by that, not `avgPerCallMs` — a 200-call × 50 µs row beats a 1-call × 5 ms row when recovering frame time.
- If no `perf_temp:*` row stands out, the markers are at the wrong granularity — add finer sub-scopes inside the suspected scope and re-measure.
- If the dominant row is a pre-existing (non-temp) scope, still treat it as the target.

### 4. Change, rebuild, re-measure

Apply the fix. Rebuild. Re-measure using the **same path (A1/A2/A3/B) as the baseline**:

- **A1/A3** — rerun `cmd scenario.run` with `--outPath=perf_after.json`. Compare both files (see step 5).
- **A2** — `cmd perf.reset`, user reproduces, `cmd perf.snapshot`.
- **B** — ask the user to repeat and paste new rows.

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

**All paths — ruling:**
- ≥30% drop on the dominant `perf_temp:*` row **and** user confirms FPS gap closed → fix worked.
- Drop <30% or another row now dominates → **iterate**, don't declare victory.
- FPS recovered but no `perf_temp:*` row shows the win → markers don't cover the changed path; add finer scopes and re-measure.

### 6. Clean up

1. Strip every `perf_temp:` marker from source.
2. Verify with the Grep tool (project rules forbid bash grep):

```
Grep(pattern: 'SMATCHET_UI_PERF_SCOPE\("perf_temp:',
     path: 'Source_Core', output_mode: 'files_with_matches')
```

Repeat for `Plugins/` and `Target_Standalone/`. All three must return zero matches before the PR commit.

3. Reset the monitor so stale rows don't appear in the next session:
```bash
build/ninja-iter-msys2/Smatchet.exe cmd perf.reset
```

4. Delete `perf_before.json` / `perf_after.json` scratch files (not committed).

---

## Skip clause

Fixes whose cost is obvious from the code (e.g. removing a confirmed per-frame disk read) can skip instrumentation — but **prefer at least one measurement round before declaring victory**. The cheapest mistake is a "fix" that didn't move the number.

---

## CLI reference

The CLI talks to a running Smatchet instance via MCP HTTP. Discovery order: `SMATCHET_MCP_HOST` / `SMATCHET_MCP_PORT` env vars → PID-verified `instance.json` in user-data dir → `--mcp-host` / `--mcp-port` flags.

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
| `perf.snapshot` | — | `{rows:[{name, lastTotalMs, avgPerCallMs, maxMs, calls, lifetimeHits, emaAvgMs}]}` |
| `perf.dump` | `outPath?` | `{file, count}` |
| `perf.reset` | — | `{reset:true}` |
| `perf.frame_count` | — | `{scopeCount, totalCalls}` |
| `perf.toggle_panel` | `open?:bool` | `{showPerformance:bool}` |
| `scenario.list` | `limit?, offset?` | paginated array of names |
| `scenario.run` | `name` (req), `frames?`, `outPath?` | `{running:true, outPath}` — use `--spawn` for fully-automated; CLI waits for file and prints result |
| `scenario.cancel` | — | `{wasCancelled:bool}` |

**Adding a scenario:** create `Source_Core/src/Commands/Scenarios/<Name>Scenario.cpp` implementing `IScenario` + one `RegisterFactory` line in `BuiltinCommands.cpp`. No other files needed.

**Instrument macros:**
- `SMATCHET_UI_PERF_SCOPE(name)` — `Source_Core/include/UiPerfMonitor.h`
- `UiPerfMonitor::Instance().GetLastFrameRows()` — `Source_Core/src/UiPerfMonitor.cpp`
- UI panel: `Inspect > Performance Monitor...`

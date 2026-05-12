# Smatchet — perf workflow

**When to read this file**: the user has asked to optimize, profile, or fix FPS / lag / hitch / "slow" / dropped frames. If the task is unrelated to performance, do not load this file.

---

## Rule 0: never guess

The first FPS-regression PR on this codebase missed the dominant cost twice in a row by reasoning from the code alone. Build the loop on actual numbers from the running exe.

## The loop

### 1. Instrument

Wrap suspected hot paths in `SMATCHET_UI_PERF_SCOPE("temp:<area>")` from `Source_Core/include/UiPerfMonitor.h`.

- **Always prefix new markers with `temp:`** so cleanup is mechanical (one Grep call).
- Cover the whole hypothesis tree: the call site, every candidate sub-call, and the surrounding render-plan branch — but read the overhead note below before going deep inside per-cell loops.
- Pre-existing non-`temp:` scopes stay; do not retag them.
- Scope names are passed as `const char*` and compared by string equality; **always use a string literal**, not a `std::string::c_str()` from a temporary.

**Marker overhead — non-trivial.** Each scope does: 2× `steady_clock::now()`, a mutex lock, and an O(N) linear scan over the working set to find/insert its entry. Per-call cost is ~200-500 ns when ~20 distinct names are active. With 100 cells × 5 nested scopes per cell, that's 100–250 µs/frame just from instrumentation — visible at 144 Hz. Implications:

- One wrapping scope around the whole hot loop (e.g. the rows-clipper block) is **always safe**.
- Targeted sub-scopes inside per-cell code are fine if you accept that their own measurements are inflated, and you read them only as **relative** ranking ("which of these three sub-paths dominates"), not absolute ms.
- Never nest a `temp:` scope inside something that runs millions of times per frame (e.g. a glyph-rendering inner loop). Move the scope outward.

Example pattern when chasing a hot per-cell render — one outer scope, sub-scopes only inside the suspected culprit:

```cpp
void RenderFooCell(...) {
    SMATCHET_UI_PERF_SCOPE("temp:RenderFooCell");
    {
        SMATCHET_UI_PERF_SCOPE("temp:RenderFooCell.resolve");
        // ...
    }
    DrawFoo(...);  // already wrapped in its own non-temp scope, don't double-wrap
}
```

### 2. Build & measure

Build first:

```
cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone
```

Then pick the appropriate measurement path:

---

#### Path A — CLI (preferred)

Requires the command system from `backlog/COMMAND_SYSTEM_PLAN.md` to be implemented. Check availability first:

```
build/ninja-iter-msys2/SmatchetStandalone.exe cmd commands.list
```

Exit 0 → CLI available. Any other exit or unrecognised output → fall back to **Path B**.

**A1 — Named scenario (fully automated, no user needed):**

Use this when the slow scenario has a registered entry in `ScenarioRunner` (e.g. `priority-grid-scroll`):

```bash
build/ninja-iter-msys2/SmatchetStandalone.exe cmd scenario.run \
  --name=priority-grid-scroll --frames=600 --outPath=perf_before.json --spawn --yes
```

Then read the result directly — no user paste required:

```python
Read("perf_before.json")
# data.rows[] sorted by lastTotalMs descending — pick temp:* entries
```

**A2 — App already running, ad-hoc scenario:**

Use this when the user is already navigated to the slow screen and you just need to capture a snapshot:

```bash
# Reset accumulated data from previous frames
build/ninja-iter-msys2/SmatchetStandalone.exe cmd perf.reset

# (user reproduces the bad scenario for ~5 s — the only manual step on this path)

# Read rows directly into context — no panel, no paste
build/ninja-iter-msys2/SmatchetStandalone.exe cmd perf.snapshot --pretty
```

Parse `data.rows` from the JSON output. The rows are pre-sorted by `lastTotalMs` descending.
Filter to `temp:*` entries:

```bash
build/ninja-iter-msys2/SmatchetStandalone.exe cmd perf.snapshot \
  | jq '[.data.rows[] | select(.name | startswith("temp:"))]'
```

Also capture the top non-temp rows for context (they show the overall frame budget):

```bash
build/ninja-iter-msys2/SmatchetStandalone.exe cmd perf.snapshot \
  | jq '.data.rows[:10]'
```

---

#### Path B — Manual (fallback when CLI is not yet built)

Ask the user to:

1. Run the rebuilt exe (`build/ninja-iter-msys2/SmatchetStandalone.exe`).
2. Open the Perf panel: **menu `Inspect > Performance Monitor...`**.
3. Reproduce the bad scenario as concretely as possible (e.g. "open a view that includes the priority column, scroll continuously for 5 s") so the panel's per-frame numbers reflect the loaded state, not the idle desktop.
4. Paste back the rows whose `name` starts with `temp:` — and the top ~10 rows overall so I see what they're competing against.
5. Optionally: also report the user-visible FPS reading before/after, so I can sanity-check the marker totals against the actual frame budget gap.

**Do not attempt to read FPS yourself on Path B.** There is no headless export and Claude can't observe the GUI. Wait for the user's numbers before making code changes.

### 3. Diagnose from the numbers

- The dominant `temp:*` row by `lastTotalMs` is the target. Sort by that, not by `avgPerCallMs` — a 200-call × 50 µs row beats a 1-call × 5 ms row when you're trying to recover frame time.
- Don't change anything that isn't measurably hot. If no `temp:*` row stands out, the markers are at the wrong granularity — add finer ones inside the suspected scope and re-measure before editing.
- If the dominant row is a marker you didn't write (a pre-existing scope), still treat it as the target — it just means the cost lives in already-known infrastructure.

### 4. Change, rebuild, re-measure

Apply the targeted fix. Rebuild (`cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone`). Then re-measure using the **same path (A1/A2/B) you used in step 2**:

- **A1** — rerun `cmd scenario.run` with `--outPath=perf_after.json`. Read both files and compare `rows[].lastTotalMs` on the target scope.
- **A2** — `cmd perf.reset`, user reproduces, `cmd perf.snapshot`.
- **B** — ask the user to repeat the same scenario and paste the new `temp:*` rows.

Same view, same scroll pattern, same row count as the baseline — otherwise the comparison is noise.

### 5. Validate

Compare before/after on the dominant row(s).

**CLI path (A1)** — diff the two JSON files directly:
```bash
jq -s '
  [ .[0].data.rows[] | select(.name | startswith("temp:")) ] as $before |
  [ .[1].data.rows[] | select(.name | startswith("temp:")) ] as $after |
  $before[] as $b | $after[] | select(.name == $b.name) |
  { name, before: $b.lastTotalMs, after: .lastTotalMs,
    drop_pct: ((($b.lastTotalMs - .lastTotalMs) / $b.lastTotalMs * 100) | round) }
' perf_before.json perf_after.json
```

**All paths** — ruling:
- ≥30% drop on the dominant `temp:*` row **and** user confirms the FPS gap closed → fix worked.
- Drop <30% or another row now dominates → **iterate**, don't declare victory.
- FPS recovered but no `temp:*` row shows the win → markers don't cover the path the fix changed; add finer scopes and re-measure.

### 6. Clean up

Once the user confirms the FPS improvement is real:

1. Strip every `temp:` marker from source.
2. Verify with the Grep tool (per project rules — do not use bash `grep`):

```
Grep(pattern: 'SMATCHET_UI_PERF_SCOPE\("temp:',
     path: 'Source_Core', output_mode: 'files_with_matches')
```

Plus the same check against `Plugins/` and `Target_Standalone/`. All three must return zero matches before the PR commit. Pre-existing non-`temp:` scopes stay untouched.

3. **CLI path** — reset accumulated data so stale temp: rows don't persist in the monitor between sessions:
```bash
build/ninja-iter-msys2/SmatchetStandalone.exe cmd perf.reset
```

4. Delete any `perf_before.json` / `perf_after.json` scratch files written during step 2/4 (they live in the working directory or `<userData>/perf/`; not committed).

## Skip clause

Fixes whose cost is obvious from the code alone (e.g. removing a confirmed per-frame disk read, deleting a dead loop) can skip the markers — but **prefer to confirm with one measurement round before declaring victory**. The cheapest mistake is a "fix" that didn't move the number.

## Reference

- Macro: `SMATCHET_UI_PERF_SCOPE(name)` — `Source_Core/include/UiPerfMonitor.h`
- Aggregator: `UiPerfMonitor::Instance().GetLastFrameRows()` — `Source_Core/src/UiPerfMonitor.cpp`
- UI panel: `Source_Core/src/SmatchetPerfUi.cpp` (menu `Inspect > Performance Monitor...`)
- BeginFrame call site: `Source_Core/src/SmatchetUI.cpp` (`UiPerfMonitor::Instance().BeginFrame()`)

**CLI commands (requires `backlog/COMMAND_SYSTEM_PLAN.md` to be implemented):**
- `cmd perf.snapshot [--pretty]` — returns all current frame rows as JSON inline.
- `cmd perf.dump [--outPath=<path>]` — writes rows to disk, returns `{file, count}`.
- `cmd perf.reset` — clears accumulated data.
- `cmd scenario.run --name=<id> --frames=<n> [--outPath=<path>] [--spawn] --yes` — drives a named scenario end-to-end and dumps perf JSON.
- `cmd scenario.list` — lists registered scenario names.

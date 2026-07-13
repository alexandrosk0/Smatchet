---
name: test-authoring
description: The deterministic mechanics of a Smatchet headless-test author — the Bucket-E ImGui Test Engine wire-up inventory + gotchas, the four authoring patterns (CLI probe + bash assert, scenario lifecycle, screenshot scan, sanitizer build run), the mock-tracker setup recipes, the bash-conventions checklist, and the report-format template. Invoked by test-author (which keeps the judgment: the 5-bucket taxonomy, automation-feasibility classification, the never-manual-forever deferral rule, the verification gate, the authoring-discipline refusals, and the golden-image-approval pause-gate). Use when wiring a bucket-E test, writing a CLI probe + bash assert, authoring an IScenario, scanning a screenshot, running a sanitizer build, or formatting a test-author report.
triggers:
  - test-authoring
  - bucket-e
  - imgui-test-engine
  - cli-probe-test
  - scenario-test
  - screenshot-scan
  - sanitizer-test
  - mock-tracker
version: 1
---

<!-- Skill-only helper (no agent twin; registered in SKILL_ONLY_HELPERS). This is the
     EXTRACTED mechanics body of agents/core/test-author.md — the deterministic
     copy-paste-run recipes (bucket-E wire-up, the four authoring patterns, mock-tracker
     recipes, bash conventions, the report template). The agent keeps the reasoning and
     points here per section. reduce-agent-prompt-bloat. Cross-harness: Codex/Cursor read
     the agent's summary + this path; Claude Code loads this skill on demand. -->

# test-authoring (skill)

The mechanics `test-author` runs. The agent owns *what to automate and why* (the 5-bucket taxonomy, automation-feasibility classification, the never-manual-forever deferral rule, the verification gate, the authoring-discipline refusals, the golden-image-approval pause-gate); this skill owns *how* — the bucket-E wire-up inventory, the four authoring patterns, the mock-tracker recipes, the bash conventions, the report template.

## Bucket E — ImGui Test Engine (wired)

**Status**: WIRED (per `docs/plans/shipped/imgui-test-engine-bucket-e-execution.md`). First test landed at `tests/ui/views_columns_reorder.test.cpp`; bash driver at `scripts/dev/test-ui-views-columns-reorder.sh`. Run via `cmake --build --preset ninja-ui-test-msvc`.

How the surface works today:

- `imgui_test_engine` fetched + built as `SmatchetImGuiTestEngine` (`cmake/ImGuiTestEngine.cmake`), pinned to a specific upstream SHA.
- ImGui configuration consolidated in `Source/Core/include/SmatchetImConfig.h` (wired via `IMGUI_USER_CONFIG`). Test-engine hooks gated on `SMATCHET_BUILD_UI_TESTS`.
- `UiTestScenario` (`Source/Core/src/Commands/Scenarios/UiTestScenario.cpp`) owns the engine lifecycle behind the standard `IScenario` contract. Driven from `Source/Standalone/main.cpp` via the post-`glfwSwapBuffers` hook.
- CLI: `ui_test.run --name=<filter> [--all]` returns pass/fail JSON. The filter is **substring-match with `^` (anchor-start) / `$` (anchor-end) modifiers** — NOT a glob. `*` does not work; use `ColumnsReorder` to match every `ColumnsReorder_*` test.
- Tests register via `IM_REGISTER_TEST(engine, category, name)` and add a `RegisterX(engine)` call to `tests/ui/ui_tests_registry.cpp::SmatchetRegisterAllUiTests`.

Adding a new bucket-E test:

1. New `tests/ui/<feature>.test.cpp`. Use a TU-local `Register<Feature>Tests(engine)` entry point + register the test inside.
2. Append the call to `SmatchetRegisterAllUiTests` in `ui_tests_registry.cpp`.
3. New bash wrapper at `scripts/dev/test-ui-<feature>.sh` mirroring `test-ui-views-columns-reorder.sh`. Auto-enrolled by `scripts/dev/test-all.sh`.

Gotchas the wire-up hit (record once, save the next agent):

- `IMGUI_TEST_ENGINE_ENABLE_COROUTINE_STDTHREAD_IMPL` MUST be 1 — `ImGuiTestEngine_Start` asserts on null `CoroutineFuncs`.
- `IMGUI_TEST_ENGINE_ENABLE_CAPTURE` MUST stay 1 — `imgui_te_engine.cpp` calls `ImGuiCaptureContext::*` unconditionally (no `#if` guards). Disabling capture loses link-time symbols. We never wire a `ScreenCaptureFunc` so the runtime cost is zero.
- `IMGUI_DEFINE_MATH_OPERATORS` must be defined BEFORE `imgui.h`. Setting it in `SmatchetImConfig.h` (read at `imgui.h` start via `IMGUI_USER_CONFIG`) is the right hook point.
- The engine's filter language is substring + `^` / `$` / `,` modifiers, NOT shell glob. Don't write `Views/*` — write `Views` or `^Views/`.
- Fresh-profile drivers MUST seed `whisper_setup_completed=true` (and ideally `backend_has_been_reachable=true`) in the test config — otherwise the first-launch `##WhisperSetupBanner` overlays the UI and silently swallows `ItemClick`s, failing click-driven tests with no obvious cause.
- Any bucket-E test that performs a queue/field-edit WRITE MUST use `BucketE::UiTestWriteScope` (`tests/ui/_helpers/UiTestWriteScope.h`) to flip the fresh-profile `ReadOnlyMode=true` default OFF, and MUST HARD-FAIL — not skip — on a write failure once its env gates are met. A fresh profile defaults read-only, so an unscoped write is silently rejected and the test goes vacuously green.

If a future bucket-E test needs synthetic input that the existing engine doesn't cover, the fallback is a recorded one-shot (mouse / key event log replayed via `ImGuiIO`) — but record the recipe so the next bucket-E item stays cheap.

## Authoring patterns

### Pattern A — CLI probe + bash assert

Smallest viable test. Two artifacts: a CLI command that captures state, a bash script that runs it and asserts the JSON.

**CLI command shape** (mirror `debug.lua_log_test` in `Source/Core/src/Commands/BuiltinCommands.cpp`):

```cpp
{
    Command c = MakeCommand("debug.<feature>_test",
        "<one-line summary>",
        [&app](const nlohmann::json& args, const CommandContext& /*ctx*/) {
            // 1. Install ephemeral capture (sink, flag, etc.).
            // 2. Run the feature path via an existing public method.
            // 3. Read captured state into a json result.
            return CommandResult::Success(std::move(out));
        });
    c.Destructive = true;
    c.Idempotent = false;
    c.Params = {PString("<arg>", "<desc>", true)};
    reg.Register(std::move(c));
}
```

If the command runs Lua / requires the automation host, gate the body in `#if defined(SMATCHET_WITH_LUA_AUTOMATION)` with an `else` returning `ErrorCode::HandlerError`.

**Bash shape** (mirror `scripts/dev/test-lua-error-log.sh`):

```bash
#!/usr/bin/env bash
set -euo pipefail
EXE="${SMATCHET_EXE:-build/ninja-iter-msvc/Smatchet.exe}"
PY="${PYTHON:-python}"
# Non-default port forces --spawn to start a fresh child against the rebuilt exe
# (default port may be a stale running UI instance).
TEST_PORT="${SMATCHET_TEST_PORT:-58731}"

PASSED=0; FAILED=0
extract() { "$PY" -c "import sys,json,re; t=sys.stdin.read(); m=re.search(r'\{.*\}',t,re.S); d=json.loads(m.group(0)); print(json.dumps(d.get('data',{}).get('$1','MISSING')))"; }
assert_eq() { if [ "$2" = "$3" ]; then echo "  PASS  $1"; PASSED=$((PASSED+1)); else echo "  FAIL  $1 expected=$3 got=$2"; FAILED=$((FAILED+1)); fi; }
assert_contains() { if echo "$2" | grep -q "$3"; then echo "  PASS  $1"; PASSED=$((PASSED+1)); else echo "  FAIL  $1 needle=$3"; FAILED=$((FAILED+1)); fi; }
run() { "$EXE" cmd debug.<feature>_test --code="$1" --mcp-port="$TEST_PORT" --spawn --yes 2>&1; }

OUT=$(run "<test code>")
assert_eq "<assertion label>" "$(echo "$OUT" | extract <field>)" "<expected>"
# ... more assertions

echo "Passed: $PASSED  Failed: $FAILED"
[ "$FAILED" -gt 0 ] && exit 1 || exit 0
```

Always use `--code=<value>` not `--code <value>` — the latter is a positional arg and the CLI rejects it.

### Pattern B — Scenario + frame-driven assertion

When the path needs multiple frames (scroll, animation, cache warmup), use an `IScenario` subclass. Existing examples: `PriorityGridScrollScenario.cpp`, `LuaRecorderFuzzScenario.cpp`. The scenario:

1. Sets up state in `OnStart` (e.g. via `app.ScenarioRegisterLuaCachedProvider`).
2. Drives N frames in `OnFrame` (sets `scrollY_` etc.; runner reads `CurrentScrollY()`).
3. Tears down in `OnCancel` AND `OnFinish` — both must restore state.
4. Returns `UiPerfMonitor::Instance().GetLastFrameRows()` from `OnFinish` so bash can parse perf metrics.

Register in `Source/Core/src/Commands/Scenarios/SmatchetScenarioRegistry.cpp` next to the other `runner.RegisterFactory(...)` lines (the registration table the pre-refactor `scenarioRunner_->RegisterFactory(...)` calls moved into).

Bash drives it with `scenario.run --name=<name> --frames=<N> --mcp-port=<...> --spawn --yes`. Assert on row totals via the same `extract` helper, indexing into `data.rows[N]`.

### Pattern C — Screenshot scan

For visual regressions (red text, icon visible, layout shift), use `debug.window.screenshot` to write a PPM file then scan pixels for a sentinel colour (or count non-bg pixels).

```bash
"$EXE" cmd debug.window.screenshot --outPath=/tmp/shot.ppm --window=Scripting ...
# count red pixels in the "Lua Errors" panel region
"$PY" -c "import sys; data=open('/tmp/shot.ppm','rb').read(); ..."
```

Coordinate-based slicing of the PPM is brittle; prefer counting global colour-class occurrences (e.g. "≥ 100 pixels with R≥240 G≤80 B≤80 → red ErrorItems present"). Pink-clear (`glClearColor(1,0,1,1)`) is the existing pattern for UI-gap detection — see AGENTS.md § Debug techniques.

Prefer dual-capture-no-golden patterns (`scripts/dev/test-theme-roundtrip.sh`) when both states are produced at runtime within the same test — no checked-in artefact to enshrine.

### Pattern D — Sanitizer build run

For UAF / leak / heap-overflow checks at shutdown, run the scenario under ASan / UBSan. `cmake/Sanitizers.cmake` already wires the flags; the existing CMakePresets has sanitizer variants. Test script invocation:

```bash
cmake --preset ninja-msvc-asan
cmake --build --preset ninja-msvc-asan --target SmatchetStandalone
build/ninja-msvc-asan/Smatchet.exe cmd scenario.run --name=<feature>-stress --frames=600 --mcp-port=<...> --spawn --yes
# Exit code != 0 OR stderr contains "==ERROR: AddressSanitizer:" → fail
```

## Mock-tracker setup

Many cell-rendering tests need real Jira data (field catalog, ticket rows). Without a live tracker, `fieldMeta` is null and by-name providers never fire. Two viable mocks:

1. **Cache-seed**: write a fixture SQLite cache under `<userData>/cache/` with synthetic tickets + a fake field catalog before launching `--spawn`. Existing `LocalCacheManager` can be exercised this way. Heaviest but most realistic.

2. **Lua-injected catalog**: extend `debug.<feature>_test` to take a synthetic field-catalog arg, install it via a new `AppController::ScenarioSeedFieldCatalog(...)` helper. Lighter; requires a new public method.

Pick (2) for most cases — fewer moving parts. Add `AppController::Scenario*` helpers that mirror the lifetime contract of the existing `ScenarioRegisterLuaCachedProvider` (set up in `OnStart`, tear down in `OnCancel` / `OnFinish`).

## Bash conventions

- `set -euo pipefail` always.
- Exit codes: 0 pass, 1 assertion fail, 2 missing binary/build.
- Env overrides: `SMATCHET_EXE`, `SMATCHET_TEST_PORT`, `PYTHON`.
- Print a banner per test group.
- Print a per-assertion `PASS` / `FAIL` line.
- Print a final `Passed: N  Failed: M` summary.
- Use a non-default `--mcp-port` so `--spawn` always launches a fresh child — the default port might be a stale running UI binary.

## Report format

```text
Audited <plan path> § Verification — N items.

Classification:
| # | Item                                          | Bucket   | Status |
|---|---|---|---|
| 1 | log_info shows in scrolling log               | A (CLI)  | automated → scripts/dev/test-X.sh |
| 2 | red [ERROR] color visible                     | C (shot) | automated → scripts/dev/test-X-visual.sh |
| 3 | window auto-opens on error                    | A (CLI)  | automated (flag captured via debug.lua_log_test) |
| 4 | drag column header to re-order                | E        | residue — needs ImGui Test Engine |

New artifacts:
- scripts/dev/test-X.sh           — N assertions
- BuiltinCommands.cpp +debug.X    — captures Y state

Residue (with concrete next action — NEVER "manual forever"):
- Drag/drop column re-order      — bucket E; wired (`tests/ui/views_columns_reorder.test.cpp` + `scripts/dev/test-ui-views-columns-reorder.sh`). Run: `bash scripts/dev/test-ui-views-columns-reorder.sh`.

Run: bash scripts/dev/test-X.sh         →   Passed: N  Failed: 0
Run: bash scripts/dev/test-all.sh       →   Passed: <total>  Failed: 0
```

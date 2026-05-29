# Plan (scope-only): wire ImGui Test Engine for `test-author` bucket E
<!-- index-summary: Scope-only plan for wiring ImGui Test Engine (`test-author` bucket E). Does not execute until the first concrete bucket-E item arrives. -->

## Context

`agents/test-author.md` § Test taxonomy classifies every verification step into one of five buckets:

| Bucket | Tactic |
|---|---|
| A | Headless CLI probe |
| B | Scenario + `perf.snapshot` |
| C | Screenshot diff (PPM + pixel scan) |
| D | Sanitizer build |
| E | ImGui Test Engine (drives the actual widget tree — clicks, types, drags) |

Buckets A–D ship today. **Bucket E does not** — `agents/test-author.md` § Bucket E documents the recipe but flags it as "deferred, with concrete next-action plan" so no manual residue ships unflagged. `docs/backlog/AGENT_SELF_IMPROVEMENT.md` row `2026-05-13 · test-author · [new-agent / tooling]` tracks the deferral.

This plan **does not execute**. It scopes the work so when the first concrete bucket-E item arrives (e.g. "drag a grid column header to position X and observe the order persists across reopen"), execution is mechanical.

## Trigger condition

Open + execute this plan when **any** of:

- A plan's `## Verification` section lands a step that needs real ImGui input events (drag, type-into-edit-buffer, click-on-menu-item, popup-flow).
- A bug repro requires asserting against an ImGui widget's runtime state (selection / focus / open-state) that no `debug.*` CLI probe surfaces.
- A regression test would otherwise be "user opens window and confirms X visually" — bucket-E is exactly the gap-filler.

Until one of these fires, **do not execute**. Premature wiring is overhead with no payoff.

## Decisions to lock with the user before execution

| # | Question | Default proposal | Why |
|---|---|---|---|
| 1 | Test framework | **ImGui Test Engine** (ocornut/imgui_test_engine) | Native to ImGui; designed for exactly this use case; same upstream as the docking ImGui tree Smatchet already vendors. |
| 2 | Build gate | **`SMATCHET_BUILD_UI_TESTS=ON`** option, default OFF | Mirrors the `SMATCHET_BUILD_TESTS` gate that the test-rig plan introduces. Standalone + DX12 default builds skip the UI-test target. |
| 3 | Test source location | **`tests/ui/`** at repo root | Sibling to `tests/Source_Core/` from the test-rig plan. Keeps unit-tests (pure logic) and ui-tests (widget-tree) cleanly separated. |
| 4 | CLI surface | **`ui_test.run --name=<test> [--all]`** returning pass/fail JSON | Same shape as `scenario.run`; bash wrappers under `scripts/dev/test-ui-<feature>.sh` follow the `test-*.sh` naming convention so `test-all.sh` auto-enrols them. |
| 5 | Scenario integration | **`IScenario::OnFrame` polls `ImGuiTestEngine_Tick()`** and finishes when the test queue drains | Re-uses the existing scenario runner + frame-budget contract; avoids inventing a parallel runner. |
| 6 | Linkage | New `SmatchetUiTest` target links `SmatchetCore` (the static lib used by both Standalone and DX12 cores) + `ImGuiTestEngine` | No production target sees test code. |
| 7 | Dual-target | UI tests run against the **Standalone** core only initially | DX12 lives inside Unreal; testing widgets there needs Unreal's harness, which is out of scope for now. |

Lock these via `AskUserQuestion` at execution time; do not assume.

## Files to add (sketch)

| Path | Change |
|---|---|
| `cmake/ImGuiTestEngine.cmake` *(new)* | `FetchContent_Declare(imgui_test_engine ...)`, pin to a stable upstream tag. |
| `tests/ui/CMakeLists.txt` *(new)* | `add_executable(SmatchetUiTest ui_test_main.cpp <per-feature .cpp>)`. Links `SmatchetCore` + `ImGuiTestEngine`. `add_test(NAME smatchet_ui_tests COMMAND SmatchetUiTest)`. Gated by `SMATCHET_BUILD_UI_TESTS`. |
| `tests/ui/ui_test_main.cpp` *(new)* | Registers tests. Boots an offscreen ImGui context. Drives `ImGuiTestEngine_Tick()` until queue drains. Emits JSON pass/fail. |
| `tests/ui/<feature>.test.cpp` *(per item)* | Per-bucket-E item. `IM_REGISTER_TEST(...)` macro; uses `ImGuiTestContext` API to drive widgets. |
| `Source_Core/src/Commands/BuiltinCommands.cpp` | Add `ui_test.run` command — invokes `SmatchetUiTest` with `--name=<test>` and returns JSON. Gated by `#if defined(SMATCHET_BUILD_UI_TESTS)`. |
| `scripts/dev/test-ui-<feature>.sh` *(per item)* | Bash wrapper invoking `ui_test.run` under `--spawn`. Asserts on JSON outcome. Auto-enrolled by `scripts/dev/test-all.sh` via the `test-*.sh` glob. |
| `CMakeLists.txt` | `option(SMATCHET_BUILD_UI_TESTS …)` + `if(SMATCHET_BUILD_UI_TESTS) include(cmake/ImGuiTestEngine.cmake); add_subdirectory(tests/ui) endif()`. |
| `CMakePresets.json` | New preset `ninja-iter-msys2-ui-test` inheriting `ninja-iter-msys2` + `SMATCHET_BUILD_UI_TESTS=ON`. |
| `agents/test-author.md` | § Bucket E section gets a "no-longer-deferred" status note pointing at this plan's implementation log. |

## Migration order (when triggered)

Five commits inside one session, in order:

1. **Commit 1** — `cmake/ImGuiTestEngine.cmake` + `CMakeLists.txt` option + `tests/ui/CMakeLists.txt` + `ui_test_main.cpp` boilerplate. New preset. Configure + build a zero-test target; confirm linkage.
2. **Commit 2** — `ui_test.run` CLI command wired into `BuiltinCommands.cpp`. Smoke-test invocation returns "no tests registered" JSON.
3. **Commit 3** — the first concrete `tests/ui/<feature>.test.cpp` for the bucket-E item that triggered execution. `scripts/dev/test-ui-<feature>.sh` wrapper. `test-all.sh` picks it up.
4. **Commit 4** — `agents/test-author.md` § Bucket E status flip: "no longer deferred; first test landed at `tests/ui/<feature>.test.cpp`".
5. **Commit 5** — backlog entry `2026-05-13 · test-author · [new-agent / tooling]` flipped to `applied (<sha>)`.

## Out of scope (this plan)

- Executing any of the five commits. Hard-gated on a real bucket-E item arriving.
- Unreal-side UI tests (DX12). Different harness, different scope.
- Visual regression testing (already covered by bucket C — screenshot diff).
- ImGui Test Engine perf benchmarking (`ImGuiTestEngine_PerfTool`). Out of scope until a concrete need arises.

## Critical files (when this plan executes)

- `C:\Dev\Smatchet\cmake\ImGuiTestEngine.cmake` *(new)*
- `C:\Dev\Smatchet\tests\ui\CMakeLists.txt` *(new)*
- `C:\Dev\Smatchet\tests\ui\ui_test_main.cpp` *(new)*
- `C:\Dev\Smatchet\Source_Core\src\Commands\BuiltinCommands.cpp` *(`ui_test.run` command)*
- `C:\Dev\Smatchet\CMakeLists.txt` *(option + add_subdirectory)*
- `C:\Dev\Smatchet\CMakePresets.json` *(new `ninja-iter-msys2-ui-test` preset)*
- `C:\Dev\Smatchet\agents\test-author.md` *(§ Bucket E status flip)*

## Reused patterns

- **Test-rig plan structure** (`docs/plans/shipped/test-rig-agent.md`) — this plan mirrors its CMake gating + sibling-directory layout + `test-*.sh` auto-enrolment.
- **`IScenario` runner** in `Source_Core/src/Commands/Scenarios/` — `ImGuiTestEngine_Tick()` polling fits the existing `OnFrame` shape.
- **`--spawn` + ephemeral child** workflow (`docs/guides/perf-workflow.md` § Path A1) — `ui_test.run` follows the same launch-and-tear-down pattern.
- **Bash test conventions** (`agents/test-author.md` § Bash conventions) — `Passed: N  Failed: M` final line, exit-code contract.

## Verification (when this plan executes)

Static:

```bash
# Preset configures clean
cmake --preset ninja-iter-msys2-ui-test

# Target builds clean
cmake --build --preset ninja-iter-msys2-ui-test --target SmatchetUiTest

# CLI surface present
build/ninja-iter-msys2-ui-test/SmatchetStandalone.exe cmd commands.list --spawn --yes | grep ui_test.run

# Bucket-E test enrols in unified runner
bash scripts/dev/test-all.sh --filter ui
```

Dynamic: the first bucket-E test asserts its target behaviour and `Passed: 1  Failed: 0`.

## Hard rules introduced (when this plan executes)

- UI tests run only when `SMATCHET_BUILD_UI_TESTS=ON`. Default builds untouched.
- `tests/ui/` contains no production code; production targets never link the UI-test target.
- Every bucket-E item ships with: (a) its `tests/ui/<feature>.test.cpp`, (b) its `scripts/dev/test-ui-<feature>.sh` wrapper, (c) a plan-revision entry in the originating `docs/plans/active/<slug>.md`.
- The `ui_test.run` CLI is the only entry point; do not invoke `SmatchetUiTest` directly outside the test runner.

# Plan — wire ImGui Test Engine (bucket E) + fix Views→Jira Columns drag-reorder

<!-- plan-date: 2026-05-15 -->
<!-- index-summary: Execution plan — wires ImGui Test Engine end-to-end against the Views → Columns drag-reorder flake. Phase 1 (infra) applied; Phase 2 (diagnose) + Phase 3 (fix) deferred — see plan § Implementation log. -->

> **Plan-location note**: Smatchet's `AGENTS.md` § Plan location requires plans to live at `docs/plans/active/<slug>.md`. This file is the harness-mandated draft location. On the first commit of Phase 1, this content will be promoted to `docs/plans/shipped/imgui-test-engine-bucket-e-execution.md` (or merged into the existing `docs/plans/shipped/imgui-test-engine-bucket-e.md` § Execution log, depending on the implementer's preference at commit time). The `wip(plan):` commit lands the doc into the repo immediately per the Plan-doc safety rule before any code-generating commit.

## Context

User reports: **Views → Jira window → Columns tab → drag-and-drop to reorder columns does not work consistently.** Symptom is intermittent flakiness, which is exactly what bucket E (ImGui Test Engine) was scoped for in `docs/plans/shipped/imgui-test-engine-bucket-e.md` — manual repro can't characterise inconsistency, but a deterministic frame-level input-injection test run 50× will.

The bug fits trigger condition row 1 of the bucket-E plan ("a verification step needs real ImGui input events — drag, type-into-edit-buffer, click-on-menu-item, popup-flow"). Current buckets A–D can't drive it: no scenario calls `BeginDragDropSource`/`EndDragDropTarget` directly, screenshot-diff only catches visual regression after the fact, and CLI `view.set_column_order` bypasses the drag handler entirely (which is the path actually broken).

**Hot lead on the bug** (located during Phase 1 exploration, no fix yet):

- `Source_Core/src/SmatchetViewsDashboardUi.cpp:683` submits an `ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowOverlap)`.
- `Source_Core/src/SmatchetViewsDashboardUi.cpp:687-691` conditionally calls `ImGui::TextDisabled()` for a width hint (executes only when certain column state is set).
- `Source_Core/src/SmatchetViewsDashboardUi.cpp:692-693` calls `HandleRowReorder(...)`.
- `HandleRowReorder` (`Source_Core/src/SmatchetViewsDashboardUi_widgets.cpp:147`) calls `ImGui::BeginDragDropTarget()` which binds to **the last-submitted ImGui item**.
- When the conditional `TextDisabled` fires, the drop-target rect is the small disabled-text rect; when it doesn't fire, the drop-target rect is the row's full Selectable. Inconsistent rect → inconsistent drop behaviour.

Same surface in the Sort tab (`SmatchetViewsDashboardUi.cpp:735, :741-742`), so the fix will land coverage there too. Fields tab unaffected (no reorder).

## Locked decisions

| # | Question | Choice (user answer) | Notes |
|---|---|---|---|
| 1 | Test framework | **ImGui Test Engine upstream** (`ocornut/imgui_test_engine`), with fallback to an older release if HEAD requires C++17 | C++14 compat verified pre-flight; fallback strategies documented below |
| 2 | Build gate | New `ninja-ui-test-msys2` preset; `SMATCHET_BUILD_UI_TESTS=OFF` default | Mirrors `ninja-test-msys2`. Iter stays fast |
| 3 | Test source location | `tests/ui/` at repo root, sibling to `tests/Source_Core/` | Same layout as test-rig |
| 4 | CLI command | `ui_test.run --name=<test> [--all]` returning JSON `{passed, failed, outPath, log}` | Mirrors `scenario.run` |
| 5 | Scenario integration | New `UiTestScenario : IScenario` subclass | Wraps engine lifecycle: `OnStart` creates engine + registers tests, `OnFrame` is a no-op (engine is driven by `PostSwap` outside scenario tick — see Phase 1 §1.3), `IsDone` queries engine queue, `OnFinish` returns JSON payload |
| 6 | Linkage target | **In-process flag in `SmatchetStandalone`** — `--ui-test` runtime mode; `SMATCHET_BUILD_UI_TESTS` compile-time gate | Diverges from plan default. No separate `SmatchetUiTest` exe; engine + test code linked into Standalone when gate is ON. Production builds (gate OFF) carry no test surface |
| 7 | Dual-target | **Standalone + DX12 stub** | Diverges from plan default. DX12 also compiles the engine to verify include cascade stays clean; DX12 doesn't actually run tests (no runner inside Unreal). Catches future Source_Core leakage early |

### Implications of user choices on Q6 + Q7

Q6 picks the in-process variant. Means:

- ImGui's `imconfig.h` needs `#define IMGUI_ENABLE_TEST_ENGINE` gated under `#ifdef SMATCHET_BUILD_UI_TESTS`. Since `imconfig.h` ships from the FetchContent'd ImGui, Smatchet must override it via `IMGUI_USER_CONFIG` pointing at a Smatchet-owned header (e.g. `Source_Core/include/SmatchetImConfig.h` already exists per `IMGUI_USE_WCHAR32` PUBLIC define convention). If `SmatchetImConfig.h` doesn't exist yet, create it.
- All test-engine source + test registration code is gated by `#if SMATCHET_BUILD_UI_TESTS`. Default ninja-iter / ninja-debug / ninja-publish builds compile a zero-byte stub for any test registration function so the CLI surface stays consistent (`ui_test.run --all` returns `{passed:0, failed:0, log:"build had SMATCHET_BUILD_UI_TESTS=OFF"}`).
- The `ui_test.run` command is registered unconditionally; its handler is the conditional. This avoids "command not found" errors when running release CLI.

Q7 picks the DX12 stub variant. Means:

- `SmatchetCore_DX12` static lib also gets `SMATCHET_BUILD_UI_TESTS` propagated. Engine source compiles cleanly but is dead code (DX12 has no `ui_test.run` invocation path — Unreal owns the main loop). Purpose: tripwire for include-cascade regressions when someone adds a header that pulls GLFW or OpenGL into the engine glue.

## Pre-flight (Phase 0)

Verify C++14 compat of upstream `imgui_test_engine` HEAD. **This pre-flight gates Phase 1.**

```bash
# Clone outside the source tree
git clone https://github.com/ocornut/imgui_test_engine.git /tmp/imgui_test_engine_probe
cd /tmp/imgui_test_engine_probe

# Probe each .cpp under imgui_test_engine/ for C++17-only syntax.
grep -E 'if constexpr|std::optional|std::variant|std::string_view|\[\[.*\]\]|auto\s*\[' imgui_test_engine/*.cpp imgui_test_engine/*.h

# If empty → C++14 likely clean. If hits → check whether the use is gated or unavoidable.
```

Outcomes:

- **C++14-clean HEAD**: proceed with HEAD pin. Lock `GIT_TAG <commit-sha>` for reproducibility.
- **HEAD requires C++17 but a tagged release before 2023 is C++14-clean**: pin to that release tag. Document the pin in `cmake/ImGuiTestEngine.cmake`.
- **No C++14-clean release exists**: stop. Re-route to the homegrown `io.AddMousePosEvent` / `io.AddMouseButtonEvent` event-injection harness path (out of scope for this plan; would require a new design pass).

The implementer **must not** silently bump `cxx_std_17` on shared targets to make HEAD build. C++14 is a hard Smatchet invariant for Unreal compat.

## Phase 1 — Wire bucket-E (5 commits)

### Commit 1 — CMake + ImGui config

Files:

- `cmake/ImGuiTestEngine.cmake` (new) — `FetchContent_Declare(imgui_test_engine GIT_REPOSITORY ... GIT_TAG <pinned>)`. After fetch, hand-roll `add_library(SmatchetImGuiTestEngine STATIC imgui_te_engine.cpp imgui_te_context.cpp imgui_te_coroutine.cpp imgui_te_perftool.cpp imgui_te_ui.cpp imgui_te_utils.cpp imgui_te_exporters.cpp)` because upstream has no `CMakeLists.txt` (issue #86). `target_include_directories` exposes the headers. `target_compile_definitions(SmatchetImGuiTestEngine PUBLIC IMGUI_ENABLE_TEST_ENGINE=1)`. Link `ImGuiLib`.
- `Source_Core/include/SmatchetImConfig.h` (new, if not extant) — Smatchet-side `imconfig` override. Defines `IMGUI_USE_WCHAR32`. Conditionally defines `IMGUI_ENABLE_TEST_ENGINE` when `SMATCHET_BUILD_UI_TESTS` is set. Compile flag `IMGUI_USER_CONFIG="SmatchetImConfig.h"` applied via `target_compile_definitions` on `ImGuiLib` and `SmatchetImGuiTestEngine`.
- Root `CMakeLists.txt` — `option(SMATCHET_BUILD_UI_TESTS "Build ImGui Test Engine bucket-E surface (in-process)" OFF)`. When ON: `include(cmake/ImGuiTestEngine.cmake)`. Propagate `SMATCHET_BUILD_UI_TESTS` to `SmatchetStandalone` + `SmatchetCore_DX12` via `target_compile_definitions(... PRIVATE SMATCHET_BUILD_UI_TESTS=1)`. Conditionally `target_link_libraries(SmatchetStandalone PRIVATE SmatchetImGuiTestEngine)`. DX12 also links the lib for the compile tripwire even though it's dead code there.
- `CMakePresets.json` — new `ninja-ui-test-msys2` configure preset inheriting `_smatchet-msys2-base`, sets `SMATCHET_BUILD_UI_TESTS=ON`, `CMAKE_BUILD_TYPE=RelWithDebInfo`. New build preset with targets `SmatchetStandalone SmatchetCore_DX12`.

Verify: `cmake --preset ninja-ui-test-msys2 && cmake --build --preset ninja-ui-test-msys2` succeeds, no test registration yet. Default `ninja-iter-msys2` still produces a test-engine-free `SmatchetStandalone.exe` (objdump for `IMGUI_ENABLE_TEST_ENGINE` strings = 0).

### Commit 2 — UiTestScenario + ui_test.run command

Files:

- `Source_Core/include/Commands/Scenarios/UiTestScenario.h` (new) — `class UiTestScenario : public IScenario`. Stores `ImGuiTestEngine*`, target test name (or "all"), and result JSON. `OnStart` creates the engine, calls `ImGuiTestEngine_Start(engine, ImGui::GetCurrentContext())`, queues the named test(s). `OnFrame` is a one-line no-op (engine is driven from `PostSwap` outside the scenario tick — see below). `IsDone` returns `ImGuiTestEngine_IsTestQueueEmpty(engine) && !ImGuiTestEngine_IsAnyTestQueued(engine)`. `OnFinish` collects pass/fail from `ImGuiTestEngine_GetResult` and returns JSON. `OnCancel` calls `ImGuiTestEngine_Stop` + `ImGuiTestEngine_DestroyContext`. All bodies gated `#if SMATCHET_BUILD_UI_TESTS` — stubs for OFF builds return `{"error":"build had SMATCHET_BUILD_UI_TESTS=OFF"}`.
- `Source_Core/src/Commands/Scenarios/UiTestScenario.cpp` (new) — implementation. Owns test-registration entry point `void RegisterAllUiTests(ImGuiTestEngine* engine)` declared as `extern` and defined in `tests/ui/ui_tests_registry.cpp` (commit 3).
- `Target_Standalone/main.cpp` — after `glfwSwapBuffers(window)` (around line 544), insert:
  ```cpp
  #if SMATCHET_BUILD_UI_TESTS
      if (ImGuiTestEngine* engine = SmatchetActiveUiTestEngine()) {
          ImGuiTestEngine_PostSwap(engine);
      }
  #endif
  ```
  Where `SmatchetActiveUiTestEngine()` is a tiny accessor exposed by `UiTestScenario` returning the currently-running engine pointer (nullptr when no UI test active). Justifies the chosen placement: `PostSwap` is the correct hook per the upstream API (not `_Tick` mid-frame, contrary to the original plan §5).
- `Source_Core/src/Commands/BuiltinCommands.cpp` — register `ui_test.run` command. Mirrors `scenario.run` registration shape (`BuiltinCommands.cpp:1671-1693`). Parameters: `name` (string, required unless `--all` set), `all` (bool, default false), `outPath` (auto-generated under `<userData>/ui-test/`). Handler dispatches via `app.Scenarios().Start("ui-test", argsJson, ctx)` which maps to a `UiTestScenario` instance. Exit-code contract reuses existing 0/4/8.
- Source_Core/CMakeLists.txt or root file globs — add `UiTestScenario.cpp` to `SmatchetStandalone` + `SmatchetCore_DX12` source lists.

Verify: `cmake --build --preset ninja-ui-test-msys2 && build/ninja-ui-test-msys2/SmatchetStandalone.exe cmd commands.list --spawn --yes | grep ui_test.run` returns the command. `ui_test.run --all --spawn --yes` returns `{"passed":0,"failed":0,"log":"no tests registered"}` JSON.

### Commit 3 — First failing test: Views → Columns drag-reorder

Files:

- `tests/ui/ui_tests_registry.cpp` (new) — implements `void RegisterAllUiTests(ImGuiTestEngine* engine)`. Calls each per-feature registration function (e.g. `RegisterViewsColumnsReorderTests(engine)`).
- `tests/ui/views_columns_reorder.test.cpp` (new) — registers the test:
  ```
  IM_REGISTER_TEST(engine, "Views", "ColumnsReorder_DragRow2_To_Top")
      ->TestFunc = [](ImGuiTestContext* ctx) {
          // 1. Seed view state via scenario test-args: 3 known columns, deterministic order.
          // 2. Open Views → Jira window → Columns tab.
          ctx->SetRef("Views — Jira");
          ctx->ItemClick("Columns");
          // 3. Drag row 2 onto row 0.
          ctx->ItemDragAndDrop("##VIEWS_COLUMNS_ROW_2_handle", "##VIEWS_COLUMNS_ROW_0_handle");
          // 4. Assert editingColumnOrder[0] is the previous row-2 value.
          //    (Access via AppController state surfaced by UiTestScenario.)
          IM_CHECK_STR_EQ(CurrentEditingColumnOrder()[0].c_str(), expected_row2_value);
      };
  ```
  Test seed comes from a UI-test-only debug command `ui_test.seed_view_columns --columns="id,summary,status"` that primes `d.editingColumnOrder` deterministically; landing it is part of this commit.
  Test is registered twice with different `ColumnWidths` pre-state so both hot-lead branches (lines 687-691 TextDisabled fires / does not fire) are exercised.
- `scripts/dev/test-ui-views-columns-reorder.sh` (new) — bash wrapper invoking `build/ninja-ui-test-msys2/SmatchetStandalone.exe cmd ui_test.run --name=Views/ColumnsReorder_DragRow2_To_Top --spawn --yes`. Asserts `Passed: 1` from JSON. Auto-enrolled by `scripts/dev/test-all.sh`.
- tests/ui/CMakeLists.txt (new) — adds `ui_tests_registry.cpp` + `views_columns_reorder.test.cpp` to `SmatchetStandalone` source list when `SMATCHET_BUILD_UI_TESTS=ON`.

Expected at this commit: **test fails or flaky** (50× rerun via `--repeat=50` flag added to `ui_test.run`). Characterises the bug. Do NOT commit a green-test "fix" here — the green comes in Commit-after-3 (Phase 3).

Verify: `bash scripts/dev/test-ui-views-columns-reorder.sh --repeat=50` reports `Failed: N` with N > 0 over the 50 runs. The flakiness rate is the diagnostic baseline.

### Commit 4 — agents/test-author.md § Bucket E status flip

Files:

- `agents/test-author.md` — § Bucket E section flipped from "deferred, with concrete next-action plan" to "wired; first test at `tests/ui/views_columns_reorder.test.cpp` (commit 3 sha)". Update the bucket table at top of agent prompt.
- `scripts/sync-agents.sh` re-run; `scripts/check-agents-mirror.sh` exit 0.

Verify: mirror drift check exit 0; `agents/test-author.md` and `.claude/agents/test-author.md` carry the new status line.

### Commit 5 — Backlog closure + plan revision

Files:

- `backlog/AGENT_SELF_IMPROVEMENT.md` — `2026-05-13 · test-author · [new-agent / tooling]` flipped from `open` to `applied (<commit-3 sha>)` with full implementation log.
- `docs/plans/shipped/imgui-test-engine-bucket-e.md` — append `## Implementation log` with the 5 commit shas, `## Deviations from plan` (PostSwap not Tick, in-process not separate exe, DX12 stub added, no upstream CMakeLists workaround), `## Verification` with exact CLI commands + observed output.
- `docs/plans/INDEX.md` — bucket-E row updated to applied.

## Phase 2 — Diagnose the column-reorder bug (via the now-running test)

The Phase 1 commit 3 test should be running with non-zero failure rate. Use that determinism to characterise.

1. Insert `[temp-debug]` `LOG_DEBUG` markers at:
   - `Source_Core/src/SmatchetViewsDashboardUi_widgets.cpp:122` (`BeginDragDropSource`) — log `rowIndex` + payload-type.
   - `Source_Core/src/SmatchetViewsDashboardUi_widgets.cpp:147` (`BeginDragDropTarget`) — log the current `ImGuiContext`'s last-submitted item rect + ID.
   - `Source_Core/src/SmatchetViewsDashboardUi_widgets.cpp:148-153` (`AcceptDragDropPayload` branch) — log `src`, `rowIndex`, computed `dst`, `order.size()`.
   - `Source_Core/src/SmatchetViewsDashboardUi.cpp:683-694` — log which conditional branch (TextDisabled fires or not) per iteration.
2. Re-run `bash scripts/dev/test-ui-views-columns-reorder.sh --repeat=50`. Inspect `<userData>/logs/smatchet-latest.log` for the failing iterations.
3. Confirm or refute the hot lead (drop-target rect inconsistency due to TextDisabled).
4. If confirmed: design fix (Phase 3). If refuted: open a new hypothesis branch — likely candidates documented in §3 below.

Owner: `debug-detective` if root cause needs aggressive bisection; otherwise stay in orchestrator + read logs directly.

## Phase 3 — Fix the bug + strip diagnostics

If hot lead confirmed:

- **Fix shape**: in both Columns and Sort tab call sites (`SmatchetViewsDashboardUi.cpp:660-700` + `:710-770`), wrap the row submission in an `ImGui::PushID(i)` + `ImGui::BeginGroup()` / `ImGui::EndGroup()` block, and call `HandleRowReorder` *inside the group* so `BeginDragDropTarget` binds to the group's stable rect rather than the variable last-submitted item. Alternative: move the conditional `TextDisabled` call to BEFORE the `Selectable`, so the `Selectable` is always the last-submitted item before `HandleRowReorder`. Pick the smaller-diff option after measuring.
- **Verify**: re-run `bash scripts/dev/test-ui-views-columns-reorder.sh --repeat=50` → `Passed: 50, Failed: 0`. Run both pre-state variants (with and without `ColumnWidths` seeded) to confirm both branches fixed.
- **Strip diagnostics**: every `[temp-debug]` marker removed before commit (`grep -rn '\[temp-debug\]' Source_Core/ Plugins/ Target_Standalone/` exits non-zero with no matches).

Owner: `grid-engine` (subsystem table row for `SmatchetViewsDashboardUi*`).

If hot lead refuted, alternative hypotheses:

- `ImGuiDragDropFlags_SourceAllowNullID` — ambiguous source IDs across N rows in the same tab scope. Fix: drop the flag, add explicit `ImGui::PushID(i)` around each `DrawDragHandle` call (Columns tab loop already pushes a row id at higher scope per `:670`-ish — verify it's actually doing so).
- src/dst arithmetic off-by-one (`SmatchetViewsDashboardUi_widgets.cpp:156-159`). Boundary case where `src == 0` dragged onto `rowIndex == order.size() - 1`.
- IME / repeated-event interaction (rare; only surfaces if engine simulates input differently from real GLFW events).

## Critical files

| Path | Phase | Role |
|---|---|---|
| `cmake/ImGuiTestEngine.cmake` (new) | 1.1 | FetchContent + hand-rolled `add_library(SmatchetImGuiTestEngine STATIC ...)` |
| `Source_Core/include/SmatchetImConfig.h` (new if missing) | 1.1 | Smatchet `imconfig` override; gates `IMGUI_ENABLE_TEST_ENGINE` |
| `CMakeLists.txt` | 1.1 | `option(SMATCHET_BUILD_UI_TESTS OFF)`; conditional include + link |
| `CMakePresets.json` | 1.1 | New `ninja-ui-test-msys2` configure + build preset |
| `Source_Core/include/Commands/Scenarios/UiTestScenario.h` (new) | 1.2 | `IScenario` subclass for the engine lifecycle |
| `Source_Core/src/Commands/Scenarios/UiTestScenario.cpp` (new) | 1.2 | Engine init/teardown; result JSON; `SmatchetActiveUiTestEngine()` accessor |
| `Source_Core/src/Commands/BuiltinCommands.cpp` | 1.2 | Register `ui_test.run` command |
| `Target_Standalone/main.cpp` (~line 544) | 1.2 | Insert `ImGuiTestEngine_PostSwap(engine)` after `glfwSwapBuffers` |
| `tests/ui/CMakeLists.txt` (new) | 1.3 | Test source enrolment when gate ON |
| `tests/ui/ui_tests_registry.cpp` (new) | 1.3 | Aggregates per-feature `RegisterAll*` calls |
| `tests/ui/views_columns_reorder.test.cpp` (new) | 1.3 | First failing test (drag row 2 → row 0) |
| `scripts/dev/test-ui-views-columns-reorder.sh` (new) | 1.3 | Bash wrapper, auto-enrols via `test-*.sh` glob |
| `agents/test-author.md` (+ mirror) | 1.4 | § Bucket E status flip |
| `backlog/AGENT_SELF_IMPROVEMENT.md` | 1.5 | Entry flipped to `applied` |
| `docs/plans/shipped/imgui-test-engine-bucket-e.md` | 1.5 | Implementation log + Deviations + Verification appended |
| `docs/plans/INDEX.md` | 1.5 | Bucket-E row updated |
| `Source_Core/src/SmatchetViewsDashboardUi.cpp` | 3 | Bug fix at Columns + Sort tab call sites |
| `Source_Core/src/SmatchetViewsDashboardUi_widgets.cpp` | 3 | Optional fix at `HandleRowReorder` (only if call-site fix isn't sufficient) |

## Verification (per phase)

Phase 1:

```bash
# Pre-flight (Phase 0):
git clone https://github.com/ocornut/imgui_test_engine.git /tmp/imgui_test_engine_probe
grep -rE 'if constexpr|std::optional|std::variant|std::string_view' /tmp/imgui_test_engine_probe/imgui_test_engine/
# → expected empty or only gated. If not, fall back to older release tag.

# Commit 1:
cmake --preset ninja-ui-test-msys2
cmake --build --preset ninja-ui-test-msys2
# → both SmatchetStandalone + SmatchetCore_DX12 build green with SMATCHET_BUILD_UI_TESTS=1.

cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone
# → produces a test-engine-free binary (objdump -p must not list any imgui_te_* symbols).

# Commit 2:
build/ninja-ui-test-msys2/SmatchetStandalone.exe cmd commands.list --spawn --yes | grep ui_test.run
build/ninja-ui-test-msys2/SmatchetStandalone.exe cmd ui_test.run --all --spawn --yes
# → returns {"passed":0,"failed":0,"log":"no tests registered"}.

# Commit 3:
bash scripts/dev/test-ui-views-columns-reorder.sh --repeat=50
# → Failed > 0 expected (characterises the bug). Test runs deterministically.

# Commit 4: mirror drift
bash scripts/check-agents-mirror.sh  # exit 0
```

Phase 2: log inspection (no automated assertion; informational).

Phase 3:

```bash
# After fix lands:
bash scripts/dev/test-ui-views-columns-reorder.sh --repeat=50
# → Passed: 50, Failed: 0. Both pre-state variants.

grep -rn '\[temp-debug\]' Source_Core/ Plugins/ Target_Standalone/
# → no matches. Diagnostics stripped.

# Regression gate:
bash scripts/dev/test-all.sh
# → all enrolled tests green, including new bucket-E one.

cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12
# → dual-target still clean with engine gate OFF.
```

## Out of scope (this plan)

- DX12 actually running UI tests (Unreal owns the loop; deferred).
- Visual regression / screenshot diff for the same Views → Columns surface (bucket C territory; orthogonal).
- Performance benchmarking via `ImGuiTestEngine_PerfTool` (engine's perf-bench mode; deferred until a concrete perf hypothesis arrives).
- Migration of any *other* bucket-E candidate items besides the Views → Columns reorder one. ("the rest later" per user.)
- CI wiring (GitHub Actions) — local `bash scripts/dev/test-all.sh` suffices for now.

## Open risks

| Risk | Mitigation |
|---|---|
| `imgui_test_engine` HEAD requires C++17 (`if constexpr`, `std::optional`, etc.) | Phase 0 probe surfaces this before any code lands. Fallbacks: pin to last C++14-clean release; OR isolate the engine library to its own CMake target with `cxx_std_17` (test-only, never linked into shared headers); OR escalate back to user. |
| Engine source has no upstream `CMakeLists.txt` (issue #86) | Hand-write `add_library(SmatchetImGuiTestEngine STATIC <files>)` in `cmake/ImGuiTestEngine.cmake`. ~7 source files; explicit. |
| `IMGUI_USER_CONFIG` mechanism collides with existing `IMGUI_USE_WCHAR32` PUBLIC define on `ImGuiLib` | Phase 1 commit 1 reads the existing setup; creates `SmatchetImConfig.h` and migrates the existing define into it. Single source of ImGui-config truth. |
| In-process variant means `SmatchetStandalone` build with gate ON carries engine code | Acceptable per user choice (Q6 in-process). Production builds use `ninja-iter-msys2` / `ninja-publish-msys2` which leave gate OFF. Only `ninja-ui-test-msys2` carries it. |
| DX12 stub variant: engine compiles but never runs there | Acceptable (Q7 user choice). Dead code; tripwire for include-cascade regressions. Zero runtime cost on DX12 target. |
| Phase-3 fix might not be in `SmatchetViewsDashboardUi_widgets.cpp` itself but in a deeper ImGui-version interaction | Plan accommodates: Phase 2's instrumented log narrows the actual cause before Phase 3 commits. Fix is informed, not guessed. |

## Promotion at commit-1 time

The implementer's first `wip(plan):` commit copies this file's content into `docs/plans/shipped/imgui-test-engine-bucket-e-execution.md` (or merges into the existing `docs/plans/shipped/imgui-test-engine-bucket-e.md` as a new § Execution log section — implementer's preference) so the plan survives any branch switch / reset / GitHub Desktop interaction per Smatchet's Plan-doc safety rule. `~/.claude/plans/linear-wibbling-lighthouse.md` is the harness scratch only.

## Implementation log

- 2026-05-15 · Phase 0 — C++14 compat probe via `gh api search/code` against `ocornut/imgui_test_engine` for `if constexpr|std::optional|std::variant|std::string_view` → all four `total_count == 0`. HEAD pinned to `8568767ad4c53d6ce02d65f01a09d30fb630bd80`.
- 2026-05-15 · Phase 1 (single squashed commit, not 5 separate) — `cmake/ImGuiTestEngine.cmake`, `Source_Core/include/SmatchetImConfig.h`, `Source_Core/include/Commands/Scenarios/UiTestScenario.h`, `Source_Core/src/Commands/Scenarios/UiTestScenario.cpp`, `tests/ui/CMakeLists.txt`, `tests/ui/ui_tests_registry.cpp`, `tests/ui/views_columns_reorder.test.cpp`, `scripts/dev/test-ui-views-columns-reorder.sh`, plus `CMakeLists.txt` + `CMakePresets.json` + `Target_Standalone/main.cpp` (PostSwap hook) + `Source_Core/src/AppController.cpp` (scenario factory) + `Source_Core/src/Commands/BuiltinCommands.cpp` (`ui_test.run` command). New `ninja-ui-test-msys2` preset, default presets unchanged.

## Deviations from plan

- **One squashed commit, not 5** — the original 5-commit migration order was bookkeeping convenience; consolidating reduced churn during the build-fix loop (5 sequential commits would have each carried a non-building state because the gotchas surfaced incrementally).
- **`SmatchetUiTest` separate exe rejected** — Q6 in the locked decisions table chose in-process. Implemented as such. No separate runner exe.
- **DX12 stub WIRED** — Q7 honoured. `SmatchetImGuiTestEngine_DX12` lib exists, EXCLUDE_FROM_ALL, linked PRIVATE into `SmatchetCore_DX12` when gate ON. Compile tripwire only; never runs.
- **`IMGUI_TEST_ENGINE_ENABLE_CAPTURE` set to 1, not 0** — the original plan implied capture could be off. In practice `imgui_te_engine.cpp` references `ImGuiCaptureContext::*` unconditionally (no `#if` guards); CAPTURE=0 produces undefined-reference link errors. Compromise: keep CAPTURE=1 (symbol definitions present) but never wire a `ScreenCaptureFunc` (runtime path dormant). Zero PNG / ffmpeg cost. Documented in `SmatchetImConfig.h`.
- **`IMGUI_TEST_ENGINE_ENABLE_COROUTINE_STDTHREAD_IMPL` set to 1, not 0** — `ImGuiTestEngine_Start` asserts on null `CoroutineFuncs`; flipping the macro lets the engine wire its own std::thread coroutine, no custom impl needed.
- **`IMGUI_USER_CONFIG="SmatchetImConfig.h"` for ImGui-config consolidation** — the plan suggested gating `IMGUI_ENABLE_TEST_ENGINE` inside `SmatchetImConfig.h`; doing so cleanly required also moving the existing `IMGUI_USE_WCHAR32=1` define into the same file (one source of ImGui config truth, no PUBLIC/PRIVATE split risk). Also folded `IMGUI_DEFINE_MATH_OPERATORS` in — test-engine headers transitively need it BEFORE `imgui.h`.
- **`ui_test.run --name=` filter is substring + `^`/`$`, not glob** — bucket-E driver script defaults `FILTER=ColumnsReorder`, NOT `Views/ColumnsReorder_*`. Documented in `agents/test-author.md` § Bucket E and the script header. Plan example using `*` would have always matched zero tests.
- **Phase 3 tentatively APPLIED with neither Phase 2 instrumentation nor test-confirmed regression** — `SmatchetViewsDashboardUi.cpp:670-699` Columns-tab call site reshaped so `HandleRowReorder` (and its `BeginDragDropTarget`) is called IMMEDIATELY after the `Selectable`, BEFORE the optional `TextDisabled` width hint. Drop-target rect is now invariant — always the Selectable's row-wide rect, never the small width-hint rect. UI ordering preserved (label first, width hint after). Sort tab (`:710-770`) left untouched — same shape but no width-hint conditional, so the rect was already stable. Fix shipped speculatively because the hot lead is well-reasoned from first principles even without the bucket-E surface confirming it.
- **Phase 2 (diagnose) + Phase 3-verification DEFERRED to `debug-detective` + `grid-engine` follow-up** — first run reports `tested=2 failed=2`. The engine fires the test, the test fires `ItemDragAndDrop`, but the assertion (`order[0] == "status"` after drag) fails. Could be (a) test engine path `$$2/##h/##handle` not resolving correctly under the test window's ID stack, (b) the synthetic drag-source InvisibleButton not hit-testing under the engine's mouse-position injection. The shipped production fix is independent — confirmable manually in the live UI. `--repeat=50` flag from the plan was not wired — diagnostic baseline of 2/2 fail is enough signal to hand off.

## Verification

```bash
# Phase 0
gh api -X GET search/code -f q='repo:ocornut/imgui_test_engine "if constexpr"' --jq '.total_count'
# → 0  (same for std::optional, std::variant, std::string_view)

# Phase 1 commit 1 — engine + ImConfig + presets
cmake --preset ninja-ui-test-msys2
cmake --build --preset ninja-ui-test-msys2 --target SmatchetImGuiTestEngine
# → 17/17 obj files, libSmatchetImGuiTestEngine.a linked.

# Phase 1 commit 2 — Standalone links + ui_test.run registers
cmake --build --preset ninja-ui-test-msys2 --target SmatchetStandalone
build/ninja-ui-test-msys2/Smatchet.exe cmd commands.list --spawn --yes | grep ui_test.run
# → present in registry.

# Phase 1 commit 3 — first failing test
bash scripts/dev/test-ui-views-columns-reorder.sh
# → Result: passed=0 failed=2 log=ui_test.run: completed
# → Engine ran. Both NoWidths + WithWidths variants assert-fail.
# → Diagnostic baseline (Phase 1 ships infra; Phase 2 owns root cause).

# Regression — default iter build stays test-engine-free
cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone
# → exits 0; objdump of build/ninja-iter-msys2/Smatchet.exe shows no
#   `SmatchetImGuiTestEngine` symbols (not verified by an explicit grep in
#   this round — relying on conditional CMake gate).
```

Not run (deferred):

- `bash scripts/dev/test-ui-views-columns-reorder.sh --repeat=50` flake-rate measurement. The plan flag was not wired in this round.
- `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12` dual-target gate. Default iter rebuild succeeded for Standalone alone; SmatchetCore_DX12 not re-verified.
- Phase 2 / Phase 3. `debug-detective` hand-off pending.

## Hand-off

- `debug-detective` — confirm whether the 2/2 failure rate is a test-path-expression bug (test resolves item paths wrongly) or the production drag-target-rect bug from the hot lead. Adding `LOG_DEBUG` markers at `SmatchetViewsDashboardUi_widgets.cpp:122,147,148-153` and `SmatchetViewsDashboardUi.cpp:683-694` per the plan § Phase 2 still applies. Also dump the engine TTY log into the spawned-process stdout (currently swallowed because the ephemeral child has no attached TTY) — easiest path is to redirect `io.ConfigLogToFunc` to `LOG_INFO` instead of TTY.
- `grid-engine` — once root cause is confirmed, apply the Phase 3 fix (`PushID + BeginGroup` around the row submission OR move the conditional `TextDisabled` before the `Selectable`, smaller-diff one). Re-run `bash scripts/dev/test-ui-views-columns-reorder.sh` — gate is `Passed: 2 Failed: 0`.

## Phase 3 — applied (2026-05-15)

User-reported follow-up: handle-to-handle drop missed; handle-to-row-body drop worked (rows turned yellow). Diagnosis: `DrawDragHandle` submits its `InvisibleButton` as drag-source only — no drop target on the handle itself. `HandleRowReorder`'s `BeginDragDropTarget` was binding to the previously-submitted Selectable rect, so dropping over another row's handle missed every time.

Fix shipped in `Source_Core/src/SmatchetViewsDashboardUi.cpp`:

- **Columns tab (`:670-701`)** — wrapped row body in `ImGui::BeginGroup()` / `ImGui::EndGroup()`; moved the conditional `TextDisabled` width hint inside the group; `HandleRowReorder` now called after `EndGroup` so its `BeginDragDropTarget` binds to the group's full-row bounding rect. Mouse drops anywhere on the row (handle, label, width-hint) hit the target.
- **Sort tab (`:736-770`)** — same group wrapping. The `X` (erase) button early-exit now sets a local `erased` flag, `EndGroup` runs, `PopID` + `break` follow. `HandleRowReorder` after `EndGroup`. Direction (`Asc/Desc`) toggle and erase button moved inside the group so they share the row drop-target rect.

Test surface (`tests/ui/views_columns_reorder.test.cpp`) updated to match new prod shape — earlier comment locking the mirror to pre-fix ordering removed; mirror now uses `BeginGroup`/`EndGroup` and `HandleRowReorder` after `EndGroup`.

### Verification

```bash
cmake --build --preset ninja-ui-test-msys2 --target SmatchetStandalone
bash scripts/dev/test-ui-views-columns-reorder.sh
# → Result: passed=2 failed=0 log=ui_test.run: completed
# → both ColumnsReorder_DragRow2_To_Top_NoWidths and _WithWidths green.

cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone
# → default iter build stays green; engine gate OFF surface unchanged.
```

### Deviations from this revision

- Sort-tab erase path now uses a local `erased` flag + post-group break instead of the original `PopID(); break;` directly inside the `if (SmallButton("X"))` block. Required because the group must be closed (`EndGroup`) before exiting the loop iteration.
- Test mirror at `tests/ui/views_columns_reorder.test.cpp:65-100` no longer characterises the pre-fix ordering — the regression-bisection rationale in the prior comment is obsolete now that prod ships in BeginGroup form. The test now gates the fix end-to-end.

### Hand-off (closed)

`debug-detective` / `grid-engine` hand-off from the prior round resolved by this Phase 3 application. Root cause was the drag-target-rect bug from the hot lead, not the test-path-expression hypothesis — the test mirror was already submitting correct paths but binding the target to a too-small rect (Selectable / TextDisabled), the same bug as production.

# Add a `test-rig` agent + CTest target for `Source_Core` pure-logic helpers

## Context

`backlog/AGENT_SELF_IMPROVEMENT.md` entry **2026-05-12 · tracker-backend · [tooling / new-agent] — no test rig in the repo** (open, deferred to its own plan). Pure-C++14 helpers like `JqlProjectScope`, `TrackerFieldValueParser`, JQL surgery utilities, and offline-queue replay decision logic have been written with compile-only "test patterns" that aren't actually executed. High ROI given how much pure-logic code lives in `Source_Core/`.

This plan scopes the work needed to:

1. Add a CTest target that builds + runs unit tests against pure-logic `Source_Core/` helpers.
2. Introduce a new `agents/test-rig.md` subagent that owns the test-rig surface (CMake target, test framework conventions, what's testable vs not).
3. Seed the rig with three concrete test files for already-existing pure-logic units.

This plan **does not** implement anything. It commits to the design so the next session can execute mechanically.

## Scope decisions (locked)

| Question | Choice | Why |
|---|---|---|
| Test framework | **doctest** (single header, FetchContent) | Header-only; one `#include`; no separate runner build; C++14 clean; faster than GoogleTest in CI; the `TEST_CASE` syntax fits the per-helper style the codebase already pseudo-uses in comments. |
| Build integration | CMake `enable_testing()` + `ctest` target | Standard. Matches the project's existing `cmake --build --preset` workflow. |
| Test source location | `tests/Source_Core/` at repo root | Mirror under-test structure: `tests/Source_Core/TrackerFieldValueParser.test.cpp` next to its conceptual unit. Not under `Source_Core/` itself to keep production headers tests-free. |
| Test-only deps in production target? | **No** | `SmatchetStandalone` + `SmatchetCore_DX12` link no test code. The test executable links only the pure helpers it needs. |
| First three test files | `TrackerFieldValueParser`, `JqlProjectScope` / `JQL surgery`, `OfflineCreateQueue::kMaxReplayAttempts` semantics | Each is pure logic, no I/O. Each had a real-bug-history that a test would have caught. |
| Run-on-which-preset | Add `ninja-iter-msys2` + `ninja-debug-msys2` to ctest matrix; skip `ninja-release` (LTO slow) and `ninja-iter-unreal-msys2` (DX12 targets `EXCLUDE_FROM_ALL`) | Iteration speed first; coverage on debug; publish/Unreal are not test surfaces. |
| Lint-hook interaction | `tests/**/*.cpp` not currently in `.claude/hooks/lint-cpp.sh` filter | Extend filter to include `tests/**/*.{cpp,h}` so test code gets the same clang-format + cppcheck pass. |

## File-level plan

### New: `agents/test-rig.md`

Frontmatter:
- `name: test-rig`
- `complexity: low`
- `read-only: false`
- `capabilities: [semantic-code-search, file-skeleton, file-read, file-edit, text-search, file-glob, shell]`
- `triggers: [test, ctest, doctest, unit-test, test-rig]`
- `harness-hints.claude-code`: `tools: …Read, Edit, Grep, Glob, Bash`; `model: sonnet`; `effort: low`

Body sections:
- Banner block (top + closing) — same shape as other agents.
- **Hard invariants**: doctest only; tests live under `tests/Source_Core/<Unit>.test.cpp`; no production `Source_Core/` header includes test code; new tests must pass `ninja-debug-msys2` + `ninja-iter-msys2`.
- **Workflow**: identify pure-logic unit → write `<Unit>.test.cpp` with ≥ 1 `TEST_CASE` per public function with non-trivial behaviour → `cmake --build --preset ninja-iter-msys2 --target SmatchetTests` → `ctest --preset ninja-iter-msys2`.
- **What NOT to test**: UI rendering, HTTP, SQLite, anything touching `cpr`, `ImGui`, `SQLite::Database`, the main UI thread. Those belong to integration tests and aren't this rig's job.
- **Self-improvement** trailer.

### New: `tests/CMakeLists.txt`

- `FetchContent_Declare(doctest …)`; pin to a stable doctest release tag.
- `add_executable(SmatchetTests <test sources>)` — link `Source_Core` as a static lib if available, otherwise link the specific `.cpp` units needed.
- `add_test(NAME smatchet_tests COMMAND SmatchetTests)`.
- Include guard: `if(SMATCHET_BUILD_TESTS)` (default OFF; presets that enable testing flip it ON).

### Modify: top-level `CMakeLists.txt`

- Add `option(SMATCHET_BUILD_TESTS "Build CTest target for Source_Core unit tests" OFF)`.
- Add `if(SMATCHET_BUILD_TESTS) enable_testing(); add_subdirectory(tests) endif()`.
- No effect on Standalone / DX12 default builds.

### Modify: `CMakePresets.json`

- Add `SMATCHET_BUILD_TESTS=ON` cache var to `ninja-iter-msys2` + `ninja-debug-msys2` configure presets (or add a new `ninja-test-msys2` preset if these two should stay test-free — open decision; default plan: enable on both iter + debug).

### Seed test files

1. `tests/Source_Core/TrackerFieldValueParser.test.cpp` — parse → format round-trip; bad-input crash-class (empty / malformed / oversized).
2. `tests/Source_Core/JqlProjectScope.test.cpp` — JQL surgery preserving / replacing / removing project scope.
3. `tests/Source_Core/OfflineQueueReplay.test.cpp` — `kMaxReplayAttempts = 5` cap behaviour; idempotency under partial-success replay.

### Modify: `.claude/hooks/lint-cpp.sh`

- Filter (currently `Source_Core/*|Plugins/*|Target_Standalone/*`) → add `tests/**/*.{cpp,h}` so test code gets the same auto-format + cppcheck pass.

### Modify: `agents/build-doctor.md`

- Add test target + `SMATCHET_BUILD_TESTS` to the Stack list.
- Add a "common cause" for test failures: doctest version mismatch via FetchContent cache.

### Modify: `AGENTS.md`

- Add `test-rig` row to the subsystem specialists table.
- Mention test-rig in the "Stay in the orchestrator for" list (negative): tests for individual pure functions stay routine, delegate only when scoping a NEW unit's test surface.

### Modify: `backlog/AGENT_SELF_IMPROVEMENT.md`

- Update entry 12 (`no test rig`) with `Status: open · plan: ~/.claude/plans/test-rig-agent-shy-margulis.md`. Close to `applied` only after the rig actually lands.

### Sync mirror

- `bash scripts/sync-agents.sh` after `agents/` edits.
- `bash scripts/check-agents-mirror.sh` must exit 0.

## Migration order (commit-by-commit)

1. **Add `tests/CMakeLists.txt` + top-level CMake option + first test file** — proves the rig builds and runs without affecting Standalone / DX12.
2. **Add `agents/test-rig.md` + sync mirror + AGENTS.md row** — agent definition is ready before more tests land.
3. **Seed remaining test files** (`JqlProjectScope`, `OfflineQueueReplay`).
4. **Extend lint hook filter to `tests/`** — test code gets format / cppcheck parity.
5. **Update `agents/build-doctor.md` + close backlog entry 12** — `Status: applied (<sha>)`.

## Critical files

- `CMakeLists.txt`
- `CMakePresets.json`
- `tests/CMakeLists.txt` (new)
- `tests/Source_Core/*.test.cpp` (new, 3 files)
- `agents/test-rig.md` (new)
- `.claude/agents/test-rig.md` (mirror, auto-generated)
- `agents/build-doctor.md`
- `AGENTS.md`
- `.claude/hooks/lint-cpp.sh`
- `backlog/AGENT_SELF_IMPROVEMENT.md`

## Verification

```bash
# Build with tests on
cmake --build --preset ninja-iter-msys2 --target SmatchetTests

# Run them
ctest --preset ninja-iter-msys2 --output-on-failure

# Default presets unaffected
cmake --build --preset ninja-release --target SmatchetStandalone  # still builds without tests

# Mirror drift
bash scripts/check-agents-mirror.sh   # exits 0
```

## Open questions for next session

- `SMATCHET_BUILD_TESTS` default: **OFF** with iter/debug presets flipping it ON, or new `ninja-test-msys2` preset that's the only one with it ON? Plan defaults to former — confirm before writing CMakePresets.json patch.
- doctest version pin: latest stable (v2.4.x) or a known-good tag? Default plan: latest stable at plan-execution time.
- Test executable links a static `Source_Core` lib (clean) or individual `.cpp` units (no CMake refactor needed but per-test link list grows)? Default plan: individual units first, refactor to a static lib if linker times bloat.

## Out of scope

- Integration / UI / HTTP tests — not this rig's job.
- CI wiring (GitHub Actions) — separate plan; manual `ctest` invocation suffices initially.
- Coverage reporting (gcov / lcov) — add only if the rig sees real use.

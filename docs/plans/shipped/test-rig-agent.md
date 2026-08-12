# Add a `test-rig` agent + CTest target for `Source_Core` pure-logic helpers

<!-- plan-date: 2026-05-15 -->
<!-- index-summary: Add `test-rig` agent + CTest target for `Source_Core` pure-logic helpers using doctest. -->

## Context

`docs/backlog/AGENT_SELF_IMPROVEMENT.md` entry **2026-05-12 · tracker-backend · [tooling / new-agent] — no test rig in the repo** (open, deferred to its own plan). Pure-C++14 helpers like `JqlProjectScope`, `TrackerFieldValueParser`, JQL surgery utilities, and offline-queue replay decision logic have been written with compile-only "test patterns" that aren't actually executed. High ROI given how much pure-logic code lives in `Source_Core/`.

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

### Modify: `docs/backlog/AGENT_SELF_IMPROVEMENT.md`

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
- `docs/backlog/AGENT_SELF_IMPROVEMENT.md`

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

## Implementation log

- `97ab7f1` · commit 1 — rig bootstrap. `tests/CMakeLists.txt` (doctest v2.4.11 FetchContent + `add_executable(SmatchetTests …)` + per-cpp linkage + `-static-libgcc -static-libstdc++ -static`), `tests/test_main.cpp` shim, `tests/Source_Core/JqlProjectScope.test.cpp` (6 cases / 23 assertions), root `CMakeLists.txt` `option(SMATCHET_BUILD_TESTS OFF)` + `add_subdirectory(tests)` gate, `CMakePresets.json` new `ninja-test-msys2` configure + build preset + flip ON for `ninja-debug-msys2` + `ninja-publish-msys2`.
- `3b47ff0` · commit 2 — agent definition. `agents/test-rig.md` (complexity low · sonnet/low · v1, full hard invariants + workflow + reporting contract), `.claude/agents/test-rig.md` mirror via `scripts/sync-agents.sh`, `AGENTS.md` row in § Subsystem specialists + negative bullet in § Stay in the orchestrator for + § Trigger auto-activation row.
- `7f024fc` · commit 3 — second + third tests. `tests/Source_Core/TextMerge.test.cpp` (6 cases / 16 assertions over `TextMerge::ThreeWayMerge`), `tests/Source_Core/JsonParseUtil.test.cpp` (8 cases / ~30 assertions across header-only loose JSON parsers), `tests/CMakeLists.txt` updated to link `nlohmann_json::nlohmann_json` and pull in `Source_Core/src/TextMerge.cpp`.
- `1f2ad93` · commit 4 — lint hook. `.claude/hooks/lint-cpp.sh` case-glob filter now includes `tests/*.{cpp,h}` + `tests/**/*.{cpp,h}`; clang-tidy compile_commands lookup routes test files to `build/ninja-test-msys2/`; dual-target DX12 syntax probe skipped on `tests/**`.
- commit 5 (this) — `agents/build-doctor.md` adds `ninja-test-msys2` to the preset list + new "doctest FetchContent cache mismatch" common-cause; `docs/backlog/AGENT_SELF_IMPROVEMENT.md` entry 12 (`no test rig`) flipped to `applied`; two new backlog items filed (split `JiraClient.h` for `TrackerFieldValueParser` testability; lift offline-queue replay-cap into a free function); this plan revision.

## Deviations from plan

- **Commit 1 first-test was `JqlProjectScope`, not `TrackerFieldValueParser`** as the plan's commit-1 row suggested. Reason — `JqlProjectScope.cpp` is zero-dep pure stdlib (`<cctype>` / `<string>` / `<vector>`), so the rig boots with a one-file `target_sources` list. `TrackerFieldValueParser.h` pulls `JiraClient.h` → `ITrackerClient.h` + `ConfigManager.h` + HTTP / cpr cascade; linking it would force the entire tracker / HTTP / config stack into `SmatchetTests`, violating the pure-logic invariant the rig is supposed to enforce. Filed as backlog `2026-05-15 · test-rig · [context]` — needs `JiraClient.h` split before the parser becomes testable.
- **Commit 3 seeded `TextMerge` + `JsonParseUtil`, not `TrackerFieldValueParser` + `OfflineQueueReplay`.** Reason — see above for `TrackerFieldValueParser`. `OfflineCreateQueue::kMaxReplayAttempts` lives in `Source_Core/include/LocalCacheManager.h`, which `#include <SQLiteCpp/SQLiteCpp.h>` for the queue POD types that share the file. The doctest rig bans SQLite includes; and the cap value alone (`CHECK(... == 5)`) is barely informative compared to testing the cap-decision logic, which lives inline in `OfflineQueueService.cpp` and can't be reached without constructing the full SQLite-backed queue. Filed as backlog `2026-05-15 · test-rig · [context]` — needs the replay-cap decision lifted into a free function before it becomes testable. `TextMerge` (offline-edit 3-way merge — real bug-history per RICH_TEXT_EDITING_V2_PLAN) and `JsonParseUtil` (loose JSON int parsers — feeds JQL parsing + payload normalisation) are both genuinely zero-banned-include pure logic with real bug-history; they replaced the originally-planned units 1:1.
- **CTest preset (`testPresets` array)** was not added. Reason — `ctest --preset` is a CMake 3.20+ feature gated on a top-level `testPresets` array in `CMakePresets.json`. The plan didn't call for it and `cd build/<preset> && ctest --output-on-failure` works identically. Document this in `agents/test-rig.md` § Workflow step 5 ("CTest preset is wired by build dir, not test preset") so future agents don't try to `ctest --preset ninja-test-msys2` and fail with `No such test preset`.
- **`-static-libgcc -static-libstdc++ -static`** added to `SmatchetTests` link options. Not in the plan. Reason — without static-linking the C++ + GCC runtime, `ctest` invoking `SmatchetTests.exe` from outside the MSYS2 shell fails with exit code `0xc0000139` (STATUS_ENTRYPOINT_NOT_FOUND) because the MSYS2 UCRT64 runtime DLLs aren't on `PATH`. Mirrors the same fix the publish preset applies via `SMATCHET_WINDOWS_FULLY_STATIC_RUNTIME` for `SmatchetStandalone`.

## Verification

```bash
cmake --preset ninja-test-msys2
# → configure passes (doctest v2.4.11 FetchContent'd successfully)

cmake --build --preset ninja-test-msys2 --target SmatchetTests
# → [10/10] Linking CXX executable tests\SmatchetTests.exe

cd build/ninja-test-msys2 && ctest --output-on-failure
# → 1/1 Test #1: smatchet_tests ... Passed (0.01 sec)
#   100% tests passed, 0 tests failed out of 1

tests/SmatchetTests.exe
# → [doctest] test cases: 20 | 20 passed | 0 failed | 0 skipped
#   [doctest] assertions: 69 | 69 passed | 0 failed |
#   [doctest] Status: SUCCESS!

bash scripts/check-agents-mirror.sh
# → agents + token-tracking + shared skills mirrors in sync (exit 0)
```

Iter preset unaffected: `build/ninja-iter-msys2/CMakeCache.txt` has no `SMATCHET_BUILD_TESTS` entry (option remains OFF default), so `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone` still produces a test-free Standalone binary with no link of test code.

Manual residue: none. All five commits ship with deterministic CLI verification.

## Follow-on commits (post plan revision)

The two deferred test units flagged in § Deviations from plan landed as separate follow-on commits closing the surfaced backlog items:

- `03576ff` · `refactor(tracker): split JiraClient.h cascade off TrackerFieldValueParser.h + add value-parser tests`. Removes the `JiraClient.h` cascade from `Source_Core/include/TrackerFieldValueParser.h`, moves `FormatWorkDurationFromSeconds` declaration to pair with `ParseWorkDurationToSeconds` in the value-parser header (was in `JiraClient.h:18`), updates `Source_Core/src/TrackerGridFieldDisplay.cpp` so the call site still resolves, and adds `tests/Source_Core/TrackerFieldValueParser.test.cpp` with 10 cases / 38 assertions (parse + format + round-trip on whole-unit durations). Test exe links pick up `TrackerFieldValueParser.cpp` + `TrackerFieldValueUtils.cpp` + `Logger.cpp` + `ConfigManager.cpp` (the last forces a `target_link_libraries(... crypt32)` on Windows because `ConfigManager` calls `CryptProtectData`). Closes backlog `2026-05-15 · test-rig · JiraClient.h cascade`.
- `86895de` · `refactor(offline-sync): lift replay-cap decision to OfflineQueueReplayPolicy + add tests`. New `Source_Core/include/OfflineQueueReplayPolicy.h` declares `kMaxReplayAttempts = 5` + inline `ShouldArchive(int currentAttempts, int maxAttempts = kMaxReplayAttempts)` — zero banned includes. `LocalCacheManager.h` aliases the existing `OfflineCreateQueue::kMaxReplayAttempts` + `OfflineFieldEditQueue::kMaxReplayAttempts` to the policy constant. `OfflineQueueService.cpp` updated at four decision sites (two pre-attempt + two post-failure gates across both tick loops) to call `OfflineQueueReplayPolicy::ShouldArchive(...)`. `tests/Source_Core/OfflineQueueReplayPolicy.test.cpp` ships 5 cases / 26 assertions. Closes backlog `2026-05-15 · test-rig · OfflineCreateQueue::kMaxReplayAttempts`.

Hook tuning rolled into `03576ff` as a prerequisite: `.claude/hooks/lint-cpp.sh` gained `--language=c++` (cppcheck was guessing C when scanning a bare `.h` and rejecting `std::string`) and `--suppress=unusedStructMember` (same cross-TU false-positive class as the existing `unusedFunction` suppression — verified against `JiraClient::cachedProjects_` which is used in `JiraClient.cpp`).

Aggregate rig state after these two follow-ons: **35 test cases / 133 assertions**; `ctest --output-on-failure` 1/1 green on `ninja-test-msys2`. Dual-target Standalone + DX12 build verified clean.

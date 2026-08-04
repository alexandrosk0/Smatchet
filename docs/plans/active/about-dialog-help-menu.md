# Plan — About dialog under Help

> **Slug**: `about-dialog-help-menu` (matches this file's basename without `.md`).
>
> **Status**: `active` — five slices. Slice 1 (CMake codegen) and slice 2 (host-info extraction) are independent; 3 depends on 1+2; 4 and 5 depend on 3. All five ship as **one PR** per `AGENTS.md` § Autonomous ship-loop default (one PR per logical feature, not per slice).

## Context

Smatchet's Help menu ([`SmatchetUI_MainMenu.cpp:679`](../../../Source/Core/src/Ui/SmatchetUI_MainMenu.cpp:679)) has exactly two items — "Check for Updates..." and "Report a Bug...". There is **no About dialog anywhere in the repo**, and the app version (`SMATCHET_APP_VERSION`, from `project(SmatchetApp VERSION 0.6.7)` at [`CMakeLists.txt:20`](../../../CMakeLists.txt:20)) is visible in exactly one place: inside the update modal ([`SmatchetUI.cpp:162`](../../../Source/Core/src/Ui/SmatchetUI.cpp:162)), which only appears after an update check runs.

So a user who wants to know what build they are running — to file a bug, to confirm an update landed, to check whether their binary has Lua/Whisper/AI compiled in — has no way to find out. There is also **no git SHA anywhere in the build**: no git-describe, no generated version header, no `configure_file` other than the Win32 VERSIONINFO resource. Two identical-version builds from different commits are indistinguishable, which makes a bug report from a dev build unactionable.

Prompted by a direct user request ("add an About where the version is seen and more information under Help") with content scope pinned interactively: build info, git commit + branch, runtime environment, links + actions, and third-party credits **generated from the CMake dependency set** rather than hand-maintained in C++.

**Intended outcome**: after this lands, Help → "About Smatchet..." opens a modal showing version, build info, git commit, runtime environment and third-party credits, with one-click copy-all for bug threads — and the git SHA + dependency list are generated from the build, so neither can drift from the binary the user is actually running.

## Approach

Three layers, so the risky part (build-time codegen) touches exactly one translation unit and the valuable part stays headlessly testable:

1. **Generated data** — `SmatchetBuildInfo.h` emitted into `${CMAKE_BINARY_DIR}/generated/include/`. Never committed, never packaged into the Unreal plugin.
2. **Pure assembly** — new ImGui-free TU `Source/Core/src/Diagnostics/AboutInfo.cpp`, the *only* includer of the generated header. Produces a plain `AboutInfo` struct + `BuildAboutReportText()`. Unit-testable; reused by the command layer.
3. **Render** — new TU `Source/Core/src/Ui/SmatchetAboutUi.cpp`, consuming `AboutInfo`, knowing nothing about codegen.

The non-obvious trade-off: the dependency manifest is **hand-authored then drift-guarded**, not read back from FetchContent. Every `FetchContent_Declare` in this tree pins a raw commit SHA with the human-readable version living only in a trailing comment, so there is no version variable to read — a credits box listing bare 40-char SHAs is useless to a user filing a bug. `smatchet_check_dep_manifest` text-matches each manifest record against the root `CMakeLists.txt` so a pin bump that skips the manifest is caught (WARN local / FATAL under `$ENV{CI}`, per the `cmake-local-gate-ci-scope` rule).

---

## Slice 1 — CMake codegen

**Create [`cmake/SmatchetBuildInfo.h.in`](../../../cmake/SmatchetBuildInfo.h.in)** — template defining `SMATCHET_BUILDINFO_AVAILABLE`, `SMATCHET_GIT_SHA` / `_SHA_SHORT` / `_BRANCH` / `_DIRTY` / `_SHALLOW`, `SMATCHET_CXX_COMPILER_ID` / `_VERSION`, `SMATCHET_CMAKE_VERSION`, and `SMATCHET_DEPS_TEXT`. `SMATCHET_DEPS_TEXT` is **one string literal per dep**, preprocessor-concatenated — MSVC caps a single string literal at 16380 bytes, and per-line literals stay diffable.

**Create [`cmake/SmatchetGenerateBuildInfo.cmake`](../../../cmake/SmatchetGenerateBuildInfo.cmake)** — a `cmake -P` script that probes git and writes the header via `configure_file` → `copy_if_different` (so an unchanged SHA never moves the mtime, so nothing recompiles).

Failure modes, all degrading to `"unknown"` — **the script must never `message(FATAL_ERROR)`** (a new local-configure hard gate violates `cmake-local-gate-ci-scope`):

| Condition | Handling |
|---|---|
| git absent | `find_package(Git QUIET)` → `GIT_EXECUTABLE-NOTFOUND` → all fields `"unknown"` |
| Not a repo | `git rev-parse --git-dir` non-zero → `"unknown"` |
| **`git worktree` tree** | This repo uses worktrees (`scripts/dev/worktree.ps1`), so `.git` is a **file**. Never test `EXISTS <dir>/.git` as a directory — always ask `git rev-parse --git-dir` |
| Dirty | `git status --porcelain=v1 --untracked-files=no`. `--untracked-files=no` is load-bearing: an in-source build dir would otherwise mark every build dirty |
| Detached HEAD | `rev-parse --abbrev-ref HEAD` returns `HEAD` → prefer `$ENV{GITHUB_REF_NAME}`, else `"(detached)"` |
| Shallow CI clone | `rev-parse HEAD` still works; **never `git describe`** (fails on shallow). Surface `SMATCHET_GIT_SHALLOW 1` |
| Unborn HEAD | `"unknown"` |

**Create [`cmake/SmatchetDepManifest.cmake`](../../../cmake/SmatchetDepManifest.cmake)** — `smatchet_collect_dep_manifest` / `smatchet_check_dep_manifest` / `smatchet_build_dep_literals`. Record format `"name|version|license|url|probe"` — **`|` not `;`**, or CMake list semantics shred it; `probe` is internal-only. Conditional deps (`cpr`/`GLFW` under `SMATCHET_BUILD_APP`, `Lua`/`sol2` under `SMATCHET_WITH_LUA_AUTOMATION`, `whisper.cpp` under `SMATCHET_WITH_WHISPER`) are appended only when actually linked, so the credits match the binary the user is running.

**Modify [`CMakeLists.txt`](../../../CMakeLists.txt)**: `find_package(Git QUIET)`; a **configure-time seed** `execute_process(... -P ...)` so the header exists before the first compile (needed for clangd/IDE indexing and for `EXCLUDE_FROM_ALL` targets built without `ALL` having run); a **build-time refresh** `add_custom_target(SmatchetBuildInfoGen ALL ...)` with `BYPRODUCTS`; the include dir added once to the `SmatchetCoreInterface` INTERFACE target; explicit `add_dependencies` edges; and the build type via `target_compile_definitions(... SMATCHET_BUILD_CONFIG="$<CONFIG>")` — **not** via the generated header, because `CMAKE_BUILD_TYPE` is **empty** on the Visual Studio multi-config generator.

**What deliberately does NOT go in the generated header** (compile-time macros beat CMake, matching existing repo idiom at [`BugReportService.cpp:506`](../../../Source/Core/src/Diagnostics/BugReportService.cpp:506)): build date/time (`__DATE__`/`__TIME__`), ImGui version (`IMGUI_VERSION`), target arch (`_M_X64`/`_M_ARM64`/`__aarch64__`/`__x86_64__` — `CMAKE_SYSTEM_PROCESSOR` lies on VS generators).

**Dual-target resolution:** `SmatchetCore_DX12` is built *by this CMake project* over the same `${CORE_SOURCES}` and links `SmatchetCoreInterface`, so it sees the generated include dir like every other consumer. Meanwhile `SmatchetImGuiPlugin.Build.cs` adds exactly one Smatchet include path and the packaging step copies only five headers — **UBT never compiles a `Source/Core` source file**. The whole risk collapses to one invariant: *`SmatchetBuildInfo.h` is included only from `.cpp` under `Source/Core/src/`, never from a packaged header.* **Do not add it to `_SMATCHET_PKG_CMDS`** — a copy in the plugin's flat include dir would go stale against the packaged `.lib`, reintroducing the exact drift this feature exists to kill.

**Exit criterion:** the generated header exists under the build dir and is correct; a new commit + rebuild changes the SHA; rebuilding twice with no commit recompiles nothing.

---

## Slice 2 — Host-info extraction (prerequisite refactor, not optional)

"Runtime env" is already implemented — but as `HostOsName()` / `HostArchName()` / `DetectHostMachine()` inside the **anonymous namespace** of [`BugReportService.cpp:51`](../../../Source/Core/src/Diagnostics/BugReportService.cpp:51). Unreachable from another TU; copy-pasting would trip the `duplication` gate and guarantee drift.

- **Create** `Source/Core/include/Diagnostics/HostMachineInfo.h` + `Source/Core/src/Diagnostics/HostMachineInfo.cpp` — verbatim move.
- **Modify** `BugReportService.cpp` — delete the anon-namespace copies, include the new header; `GatherContext` otherwise untouched.

**Exit criterion:** bug-report `env` payload byte-identical before/after.

---

## Slice 3 — Pure data layer + tests

- **Create `Source/Core/include/Diagnostics/AboutInfo.h`** — `struct AboutDep { std::string Name, Version, License, Url; }`, `struct AboutInfo`, `GatherAboutInfo(...)`, `BuildAboutReportText(...)`, `ParseDepManifest(...)`. **No generated-header include here** — that is what keeps slice 1's invariant structural.
- **Create `Source/Core/src/Diagnostics/AboutInfo.cpp`** — the only includer of `SmatchetBuildInfo.h`, behind a belt-and-braces guard so an out-of-tree build of Core sources cannot break:

```cpp
#if defined(__has_include)
#  if __has_include(<SmatchetBuildInfo.h>)
#    include <SmatchetBuildInfo.h>
#  endif
#endif
#ifndef SMATCHET_BUILDINFO_AVAILABLE
#  define SMATCHET_GIT_SHA "unknown"   /* ...and the rest */
#endif
```

  (`__has_include` is formally C++17; the `defined()` wrapper keeps a bare C++14 compiler happy. MSVC 2015u2+ and Clang both expose it as an extension.) Under `SMATCHET_EMBEDDED_IN_UNREAL`, prefer `SmatchetHost_GetBuildTag()` for the build-tag line, mirroring `BugReportService.cpp` so the two never disagree.

- **Create `tests/Core/AboutInfo.test.cpp`** (doctest). Assert only what is machine-stable: `ParseDepManifest(SMATCHET_DEPS_TEXT)` yields ≥ 9 records, each with non-empty name+version and no stray `|`; `BuildAboutReportText()` contains the app version, a `git:` line and a `compiler:` line, ends in exactly one `\n`, no `\r\n` mixing; a synthetic empty-SHA `AboutInfo` renders `unknown` rather than an empty value or a dangling `()`; dirty/detached formatting exercised with synthetic input. **Never assert the real SHA / branch / build date** — flaky per machine.
- **Modify `tests/CMakeLists.txt`** — **mandatory**: list both the new test and `AboutInfo.cpp` in `add_executable(SmatchetTests ...)`. The configure-time guard there `FATAL_ERROR`s on any `tests/Core/*.test.cpp` present on disk but unreferenced.

**Exit criterion:** `SmatchetTests` green; `--target SmatchetCore_DX12` still compiles.

---

## Slice 4 — UI

- **Create `Source/Core/include/Ui/SmatchetAboutUi.h`** — declares `DrawAboutModal(AppController&, UiDrawSession&)`.
- **Create `Source/Core/src/Ui/SmatchetAboutUi.cpp`** — follows the `SmatchetBugReportUi.cpp` precedent. Picked up automatically: `CORE_SOURCES` is a `GLOB_RECURSE ... CONFIGURE_DEPENDS`, so **no CMake edit for a new `.cpp`**. Structure per `docs/guides/imgui-draw-pattern.md`, keeping every function well under the 200-line `Draw*` cap and the 30-branch cap: an anon-namespace `AboutDrawCtx` + six ~20-40-line section helpers (`DrawAboutIdentity` / `Build` / `Git` / `Runtime` / `Credits` / `Actions`), and a ~50-line `DrawAboutModal` doing latch → `OpenPopup` → `BeginPopupModal(..., ImGuiWindowFlags_AlwaysAutoResize)` → six calls → `EndPopup`. `AboutInfo` is assembled **once on open** and cached in `UiDrawSession`, not per frame. Any config-conditional anon-namespace helper carries the **same `#if` as its call site** or the DX12 build trips `-Wunused-function -Werror`.
- **Modify `Source/Core/include/Ui/SmatchetUiSession.h`** — add `showAbout` / `aboutOpenLatch` + the cached `AboutInfo` beside the bug-report block. Latch pattern (not a bare bool) because the command layer raises it from a non-draw context.
- **Modify `Source/Core/src/Ui/SmatchetUI.cpp`** — `DrawAboutModal(app, d);` in `drawGlobalOverlays`, after `DrawAppUpdateModal`. Global-overlay placement is correct: About must be reachable regardless of docking or panel focus.
- **Modify [`SmatchetUI_MainMenu.cpp`](../../../Source/Core/src/Ui/SmatchetUI_MainMenu.cpp)** — two edits. **(1) Fix the reachability defect**: `:150` wraps the *entire* Help menu in `BeginDisabled` while `trackerLocked` (`= !d.cfg.BackendHasBeenReachable`), so About would be greyed out **exactly** in the first-launch/backend-down state where a user most needs to copy build info for a support thread. Fix using the in-repo precedent — `drawMenuBarToolsMenu` already sits outside the bracket and disables only the item that needs it: move `drawMenuBarHelpMenu(ctx)` out of the bracket, and inside wrap **only the two pre-existing items** so current behaviour is byte-for-byte preserved. **(2)** Add `ImGui::Separator();` + `MenuItem(T("menu.about", "About Smatchet..."), MenuShortcut(ctx, "app.about.open", nullptr).c_str())` setting `d.showAbout` + `d.aboutOpenLatch`.
- **Modify `Source/Core/src/SmatchetLocalization.cpp`** — add `menu.about` to `kEntries[]` in the `menu.*` block. **Never localize the data values** (SHA, compiler, dep names) — they must stay copy-pasteable verbatim.

---

## Slice 5 — Command surface

- **Modify `Source/Core/src/Commands/AppViewCommands.cpp`** — register `app.about.open` right after the `app.bug_report.open` block, same shape (`RunOnUiThreadAsCommandResult` setting `g_ui.showAbout` + `g_ui.aboutOpenLatch`). One registration fans out to palette, CLI, MCP, Lua and scenario tests, and makes the `MenuShortcut` binding live.
- **Modify `Source/Core/src/Commands/Builtin/BuiltinCommands_App.cpp`** — **extend the existing `app.version` command** rather than adding `app.about`: add `out["build"]`, `out["git"]`, `out["runtime"]`, `out["deps"]`. Existing `version` / `releaseRepo` keys stay untouched for back-compat.
- **Add a bats case under `tests/bats/`** — `app.version --json | jq -e '.deps | length > 5'` and `.git.sha != ""`. This is what makes the **CMake half** assertable in CI; the doctest alone cannot catch a broken `add_custom_target`.

---

## Files to modify

1. [`cmake/SmatchetBuildInfo.h.in`](../../../cmake/SmatchetBuildInfo.h.in) — **new**; codegen template.
2. [`cmake/SmatchetGenerateBuildInfo.cmake`](../../../cmake/SmatchetGenerateBuildInfo.cmake) — **new**; `cmake -P` git-probe + emit script.
3. [`cmake/SmatchetDepManifest.cmake`](../../../cmake/SmatchetDepManifest.cmake) — **new**; dep manifest, drift guard, literal builder.
4. [`CMakeLists.txt`](../../../CMakeLists.txt) — wire seed + `ALL` target + include dir + `$<CONFIG>` define + `add_dependencies` edges.
5. `Source/Core/include/Diagnostics/HostMachineInfo.h` — **new**; extracted host-probe declarations.
6. `Source/Core/src/Diagnostics/HostMachineInfo.cpp` — **new**; verbatim move of the anon-namespace bodies.
7. [`Source/Core/src/Diagnostics/BugReportService.cpp:51`](../../../Source/Core/src/Diagnostics/BugReportService.cpp:51) — drop the anon-namespace copies, include the new header.
8. `Source/Core/include/Diagnostics/AboutInfo.h` — **new**; pure data types + assembly entry points.
9. `Source/Core/src/Diagnostics/AboutInfo.cpp` — **new**; sole includer of the generated header.
10. `tests/Core/AboutInfo.test.cpp` — **new**; doctest coverage of the pure layer.
11. [`tests/CMakeLists.txt`](../../../tests/CMakeLists.txt) — register the new test **and** `AboutInfo.cpp` (configure FATALs otherwise).
12. `Source/Core/include/Ui/SmatchetAboutUi.h` — **new**; modal entry point.
13. `Source/Core/src/Ui/SmatchetAboutUi.cpp` — **new**; the six section helpers + `DrawAboutModal`.
14. [`Source/Core/include/Ui/SmatchetUiSession.h:858`](../../../Source/Core/include/Ui/SmatchetUiSession.h:858) — `showAbout` / `aboutOpenLatch` / cached `AboutInfo`.
15. [`Source/Core/src/Ui/SmatchetUI.cpp`](../../../Source/Core/src/Ui/SmatchetUI.cpp) — call `DrawAboutModal` from `drawGlobalOverlays`.
16. [`Source/Core/src/Ui/SmatchetUI_MainMenu.cpp:679`](../../../Source/Core/src/Ui/SmatchetUI_MainMenu.cpp:679) — Help-menu restructure + the About item.
17. [`Source/Core/src/SmatchetLocalization.cpp`](../../../Source/Core/src/SmatchetLocalization.cpp) — `menu.about` entry.
18. [`Source/Core/src/Commands/AppViewCommands.cpp`](../../../Source/Core/src/Commands/AppViewCommands.cpp) — `app.about.open`.
19. [`Source/Core/src/Commands/Builtin/BuiltinCommands_App.cpp`](../../../Source/Core/src/Commands/Builtin/BuiltinCommands_App.cpp) — extend `app.version` JSON.
20. `tests/bats/` — one new case asserting `deps` + `git.sha` over the CLI.

## Existing utilities reused

- `AppController::OpenUrl` — [`AppController.h:307`](../../../Source/Core/include/AppController.h:307); scheme allowlist + host-callback indirection, so the GitHub link is safe under Unreal. In-modal precedent in `SmatchetUI.cpp`.
- `AppController::GetGitHubReleaseRepo()` — repo URL, already used by the update path.
- `smatchet::ui_detail::StartAppUpdateCheck(d, app, /*manual=*/true)` — fwd-declared at [`SmatchetUI_MainMenu.cpp:46`](../../../Source/Core/src/Ui/SmatchetUI_MainMenu.cpp:46); already async (`d.appUpdateFuture` + per-frame drain), so the "Check for Updates" button carries no Pillar 2 concern.
- `ImGui::SetClipboardText` — repo-wide idiom (`SmatchetGridUiSupport.cpp`, `SmatchetAuditUi.cpp`, and ~6 more); resolves through the localization shim unchanged.
- `AppController::GetAppVersion()` — version accessor.
- `TextRedaction` — the same redaction the bug-report egress path applies, reused for the clipboard payload.
- `MenuShortcut(ctx, "app.about.open", nullptr)` — [`SmatchetUI_MainMenu.cpp:72`](../../../Source/Core/src/Ui/SmatchetUI_MainMenu.cpp:72); renders a bound hotkey automatically.
- The configure-time false-green guard in `tests/CMakeLists.txt` — the pattern `smatchet_check_dep_manifest` copies (WARN local / FATAL under CI).

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: no steady-state impact — the modal draws only while open, and `AboutInfo` is assembled **once on open** and cached in `UiDrawSession`, so the `ConfigManager` directory reads and manifest parse never run per frame.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no new sync I/O. The one potentially-slow action, "Check for Updates", reuses the existing async `StartAppUpdateCheck` future + per-frame drain. Manifest parsing is ~15 rows of `std::string::find` on a compile-time literal.
- **Pillar 3 (never crash)**: all `std::string` / `std::vector`, no raw `new`/`delete`, no pointers into the generated literal. Every git field has a `"unknown"` sentinel decided in CMake, so C++ never faces an empty-vs-absent distinction; the `__has_include` fallback means a missing generated header degrades to a compiling binary with `"unknown"` values rather than a build break.
- **Pillar 4 (accessibility)**: aspirational — the modal is standard ImGui widgets, so keyboard nav and font scaling behave as elsewhere; no custom-drawn text or fixed-pixel layout is introduced. No WCAG contrast work in this plan.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

1. **PR-fast CI** — the diff's only steady-state-reachable file is `SmatchetUI_MainMenu.cpp` (menu draw). Nearest scenario in the curated map: the app-startup / main-menu scenario. The About modal itself is not in any scenario (it is closed in steady state).
2. **Pillar 2 static scanner** — **no new sync I/O reachable from `ImGui::*`**. The `ConfigManager` directory reads happen in `GatherAboutInfo`, called once from the open latch, not from a per-frame draw; no `/* PILLAR2_WORKER_ONLY */` annotation needed.
3. **Dispatcher drain** — does not touch `MainThreadDispatcher::Drain()`. Slice 5 *uses* `RunOnUiThreadAsCommandResult`, which posts through the existing dispatcher without changing it.
4. **Visible-cue bucket-E harness** — adds no sync-stall code path > 100 ms.
5. **Marker inventory** — adds no `SMATCHET_UI_PERF_SCOPE` markers; no `docs/perf/MARKER_INVENTORY.md` regen.

**Pre-push local check**: gate-check vs baseline for the named scenario per `docs/guides/perf-workflow.md` § Step 7.

**Override**: none expected; no intentional regression.

## Risks / non-goals

- **`CMAKE_BUILD_TYPE` empty on the VS multi-config generator** → blank build type on the primary Windows path. *High severity, silent.* Mitigated by taking the build type from a `$<CONFIG>` compile definition, never from the generated header.
- **Generated header leaking into a packaged Unreal header** → stale SHA or broken plugin build. *High.* Mitigated structurally: only `AboutInfo.cpp` includes it, it is not in `_SMATCHET_PKG_CMDS`, and the `__has_include` fallback keeps an out-of-tree build compiling.
- **`SmatchetCore_DX12` is `EXCLUDE_FROM_ALL`**, so an `ALL`-only generator target may not run before `--target SmatchetCore_DX12`. *Medium.* Mitigated by the configure-time seed **plus** explicit `add_dependencies` edges.
- **Worktree `.git` is a file** — a naive directory test reports "not a repo" in most dev trees here. *Medium.* Always `git rev-parse --git-dir`.
- **A new `.test.cpp` left unlisted** FATALs configure. *Medium, loud.* Slice 3 lists it explicitly.
- **Dep manifest drifts on the next pin bump.** *Medium.* `smatchet_check_dep_manifest`; WARN local / FATAL under CI.
- **Always-run target causing a rebuild storm.** *Medium.* `copy_if_different` + only one TU depends on the header.
- **About unreachable when the tracker is unreachable.** *Medium.* Help-menu restructure in slice 4.
- **Pasted report leaking a username in a config path.** *Low.* Routed through `TextRedaction`, as the bug-report egress path does.
- **`-dirty` false-positive from build artifacts.** *Low.* `--untracked-files=no`.
- **`HostMachineInfo` extraction perturbing bug-report output.** *Low.* Pure move; slice 2's exit criterion is byte-identical output.

**Non-goals**: no new update mechanism, no telemetry, no crash-reporter integration, no release-notes viewer.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: `tests/Core/AboutInfo.test.cpp` — manifest parse, report-text shape, `unknown` degradation, dirty/detached formatting (all synthetic input; never the real SHA).
- **Bucket E (ImGui Test Engine)**: not added this PR — see § Manual residue.
- **Bash-driver scenario / screenshot / sanitizer**: new `tests/bats/` case over the unified CLI — `app.version --json | jq -e '.deps | length > 5'` and `.git.sha != ""`. This is the only automated assertion that covers the **CMake half** end-to-end.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target).
- **Codegen freshness**: configure + build; the generated header shows the real SHA/branch. Commit, rebuild → SHA changes. Rebuild again with no commit → ninja reports no work (proves `copy_if_different` prevents the rebuild storm).
- **Degradation**: rerun the generator with `-DGIT_EXECUTABLE=GIT_EXECUTABLE-NOTFOUND`; header must produce `"unknown"` and **must not** FATAL.
- **Packaging**: confirm `SmatchetBuildInfo.h` did **not** appear under the Unreal plugin's `ThirdParty/Smatchet/include/`.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: run before finalising; record the outcome in § Verification (actual).
- **Manual residue**: the modal's visual verification (Help → About opens, sections populated, copy round-trips, GitHub link launches, update modal stacks) and the reachability regression (Help enabled + About enabled while the tracker is unreachable) are **manual this PR** — the visual-validation exception in `AGENTS.md` § Autonomous ship-loop default applies. Deferred-automation action plan: both are bucket-E-shaped (menu-item enablement state + modal-open assertion are exactly what the ImGui Test Engine harness asserts elsewhere); file a `docs/self-improvement/categories/test.md` entry to add a bucket-E case for the About modal + the Help-menu enablement matrix.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here.

- **"Report a Bug..." is also disabled when the tracker is unreachable**, arguably wrong for the same reason as About. Flagged, not changed — a separate product decision, and changing it would make slice 4's "byte-for-byte preserved behaviour" claim false.
- **`Locales/fr.json`** is not written; `menu.about` gets its French string in the compile-time `kEntries[]` table only.
- **Reproducible-build purity** — `__DATE__`/`__TIME__` are non-reproducible. Kept, since they are already established practice in two other places in this tree.
- **`THIRD_PARTY_LICENSES.md` gaps** — that file does not document `stb`, `ImGuiColorTextEdit`, or `whisper.cpp`, all of which the new credits list names. No-action here (the About manifest is intentionally a superset, not a replacement for the notice file); worth a follow-up to bring the notice file up to parity.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

- **Slice 1 — dep-manifest functions live in a new `cmake/SmatchetDepManifest.cmake`, not in `cmake/SmatchetThirdParty.cmake`** as the approved plan said. `SmatchetThirdParty.cmake` owns FetchContent acquisition; a credits manifest + drift guard is a separate concern and would have been the only non-acquisition code in that file.
- **Slice 3 — `GatherAboutInfo` takes `(appVersion, githubRepo)` strings, not `const AppController&`.** The doctest rig deliberately does **not** compile `BugReportService.cpp` because it drags in AppController's cpr/SQLite closure (`BugReportBody.cpp` is the pure half split out for exactly that reason). Taking an `AppController&` would have forced `AboutInfo.cpp` into the same exclusion and killed slice 3's whole point — a headless test that proves the CMake codegen produced a real dep manifest. The two facts it needed (`GetAppVersion()`, `GetGitHubReleaseRepo()`) are plain strings, so the call site passes them in.
- **Slice 3 — `tests/CMakeLists.txt` needed the include dir *and* the compile definition duplicated.** `SmatchetTests` does not link `SmatchetCoreInterface` — it re-lists include dirs by hand — so `${SMATCHET_BUILDINFO_DIR}` and `SMATCHET_BUILD_CONFIG="$<CONFIG>"` had to be repeated there. Without them `AboutInfo.cpp` silently takes its all-`"unknown"` `__has_include` fallback and the dep-manifest case fails for the wrong reason.
- **Slice 1 — `add_dependencies` targets corrected.** The approved plan named `add_dependencies(SmatchetCore SmatchetBuildInfoGen)`, but **there is no `SmatchetCore` target** in this tree. The real `${CORE_SOURCES}` consumers are `SmatchetStandalone`, `SmatchetCore_DX12`, `SmatchetMobile`, and `SmatchetCore_PosixCheck`; `SmatchetTests` also compiles `AboutInfo.cpp` from slice 3. All five get an `if(TARGET ...)`-guarded edge, attached at the end of the root `CMakeLists.txt` because `add_dependencies` on an INTERFACE library does not propagate to its consumers.

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*The `git mv` is the step that reliably gets dropped. Bind it to the impl-log write: in the SAME PR that populates the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*(Delete this `## Archive` block as part of step 2.)*

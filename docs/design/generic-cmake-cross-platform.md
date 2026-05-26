# Plan - Generic Cross-Platform CMake

> **Slug**: `generic-cmake-cross-platform`
>
> **Origin**: user request on 2026-05-26: "Make a plan to make a generic cmake that works on all platforms and any compiler."

## Context

Smatchet's current CMake entry point already has several portability guards, but the supported happy path is Windows + MSYS2 UCRT64. `CMakePresets.json:31` pins `gcc.exe` / `g++.exe` for the shared MSYS2 base, `.github/workflows/build-and-test.yml:37` gates CI on Windows/MSYS2, and `README.md:84` lists only MSYS2 presets as supported. The result is good iteration speed on the main developer machine, but not a generic host build that a contributor can configure with their platform's default generator and C++14 compiler.

The intended outcome: after this lands, a contributor can run a neutral CMake configure/build on Windows, Linux, or macOS with a C++14-capable compiler from the MSVC, GNU, Clang, or AppleClang families, and CMake will either build the supported targets or skip platform-specific targets with explicit diagnostics. The existing MSYS2 presets remain the blessed fast path and must not regress.

## Approach

Refactor the build description around capability-gated targets instead of environment-specific assumptions. The generic path should detect host OS, compiler family, generator shape, OpenGL/GLFW availability, sanitizer support, linker support, and feature gates at configure time. It should then expose a small set of host-neutral presets that do not hardcode a compiler, plus retain the existing MSYS2 and MSVC presets for repeatable CI and release workflows.

The phrase "all platforms and any compiler" is bounded here to "platforms where Smatchet's dependencies and source code are meant to build, with a C++14 compiler CMake can identify." Unknown compilers should not receive vendor flags; they should configure with conservative defaults or fail early with an actionable message when a required feature is missing. Windows-only products, especially DX12 / Unreal packaging and WASAPI capture, stay Windows-only targets rather than forcing non-Windows builds to emulate them.

Implement in slices so the first PR improves configure portability without destabilizing release builds: introduce option/platform helpers, add neutral presets, wire cross-platform OpenGL/linking, split Windows-only targets behind explicit build options, then add CI matrix coverage. Each slice should keep `ninja-iter-msys2` green before broadening the matrix.

## Files to modify

1. `CMakeLists.txt:1`: keep the root entry point, but move option defaults, feature detection, target setup helpers, and platform-specific blocks into focused `cmake/` modules as they are touched.
2. `CMakeLists.txt:112`: preserve the existing performance and release options while making defaults host-aware where needed, especially PCH, LTO, fully static Windows runtime, and strict warning flags.
3. `CMakeLists.txt:151`: rework `SMATCHET_WITH_*` defaults so platform-specific features default off when their platform capability is absent, while explicit `-DSMATCHET_WITH_X=ON` still fails loudly if impossible.
4. `CMakeLists.txt:541`: keep `ImGuiLib` as the OpenGL/GLFW variant, but link OpenGL through CMake targets (`OpenGL::GL` where available) so Linux/macOS/Windows do not need separate ad hoc link lists.
5. `CMakeLists.txt:566`: keep `ImGuiLib_DX12` behind `WIN32`, and additionally gate it behind a new `SMATCHET_BUILD_UNREAL_DX12` option whose default is `ON` on Windows and `OFF` elsewhere.
6. `CMakeLists.txt:697`: preserve `SmatchetCoreInterface` and the shim-link contract; do not force optional feature defines through the core interface in a way that violates `docs/adr/0002-plugin-shim-link-discipline.md`.
7. `CMakeLists.txt:810`: update `smatchet_configure_opengl_core_impl_target` to use `find_package(OpenGL)` / `Threads::Threads` / target-based platform libraries rather than raw OS checks wherever CMake already models the dependency.
8. `CMakeLists.txt:829`: keep `smatchet_configure_dx12_core_impl_target` Windows-only and make non-Windows CMake runs skip DX12 targets without leaving dangling build presets.
9. `CMakeLists.txt:1100`: wrap `SmatchetStandalone` creation in a `SMATCHET_BUILD_STANDALONE` option that defaults `ON` only when the OpenGL/GLFW prerequisites are available or fetchable.
10. `Plugins/Whisper/CMakeLists.txt:10`: split platform-neutral Whisper pieces from Win32/WASAPI/global-hotkey pieces so `SMATCHET_WITH_WHISPER=ON` can still build API/client/model helpers on non-Windows if desired, while capture/hotkey sources stay Windows-only.
11. `cmake/SmatchetThirdParty.cmake:1`: keep third-party preparation helpers, but add missing platform/compiler guards around patches that are currently MinGW-specific.
12. `cmake/Sanitizers.cmake:30`: broaden sanitizer support around compiler and OS capability checks, with Linux Clang/GCC support as the first non-Windows target.
13. `CMakePresets.json:1`: add host-neutral configure/build presets that do not set `CMAKE_C_COMPILER`, `CMAKE_CXX_COMPILER`, `MSYSTEM`, or a Windows-only PATH.
14. `.github/workflows/build-and-test.yml:37`: keep the existing Windows/MSYS2 job and add a matrix for host-neutral configure/build/test on Linux GCC, Linux Clang, macOS AppleClang, and Windows MSVC.
15. `README.md:80`: document the new neutral path separately from the existing MSYS2 fast path, including which targets are platform-specific.
16. `docs/dev/offline-builds.md:1`: update FetchContent cache guidance so shared caches are keyed by platform, compiler family, generator, and preset rather than assuming MSYS2 UCRT64.
17. `scripts/dev/test-all.sh:1`: make build-path messages respect a configurable preset/exe path instead of always naming `build/ninja-iter-msys2/Smatchet.exe`.

## Existing utilities reused

- `smatchet_apply_strict_warnings` in `CMakeLists.txt:195`: keep first-party warning policy in one place; extend with compiler-family branches only when CMake identifies the compiler.
- `smatchet_assert_plugin_shim_links` in `CMakeLists.txt:762`: preserve the configure-time guard that prevents optional-feature ABI drift across static plugins.
- `smatchet_configure_core_impl_target` in `CMakeLists.txt:784`: reuse as the shared core target linker/includes hook, with only platform-neutral additions.
- `smatchet_configure_opengl_core_impl_target` in `CMakeLists.txt:810`: reuse for standalone builds; make it the single home for OpenGL/GLFW/Threads platform links.
- `smatchet_configure_dx12_core_impl_target` in `CMakeLists.txt:829`: reuse for Unreal/DX12 packaging and keep it deliberately Windows-only.
- `smatchet_prepare_cpr`, `smatchet_prepare_sqlitecpp`, and `smatchet_prepare_httplib` in `cmake/SmatchetThirdParty.cmake:1`: keep dependency patching centralized.
- `smatchet_apply_sanitizers` in `cmake/Sanitizers.cmake:30`: extend rather than duplicating sanitizer flag logic in presets or workflows.
- `SmatchetTests` target in `tests/CMakeLists.txt:12`: use existing doctest coverage as the generic build smoke test before adding new tests.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: no runtime behavior is intended to change; build-system changes must not enable slower runtime code paths by default.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no UI-thread behavior is intended to change; any new configure-time downloads remain CMake-time only.
- **Pillar 3 (never crash)**: positive impact; broader compiler/sanitizer coverage should catch undefined behavior earlier, but feature-gate drift must be guarded by existing shim-link checks.
- **Pillar 4 (accessibility - keyboard nav / font scaling / WCAG AA)**: no UI or accessibility behavior change.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A - planned CMake/preset/docs/scripts write set only`)

Planned implementation does not touch `Source_Core/` C++ behavior. If portability fixes uncover source changes under `Source_Core/`, revise this section before editing and run the relevant perf gate from `docs/PERF_WORKFLOW.md`.

1. **PR-fast CI** - N/A: no runtime code path is planned.
2. **Pillar 2 static scanner** - N/A: no UI-thread I/O path is planned.
3. **Dispatcher drain** - N/A: does not touch `MainThreadDispatcher::Drain()`.
4. **Visible-cue bucket-E harness** - N/A: no new sync-stall code path.
5. **Marker inventory** - N/A: no `SMATCHET_UI_PERF_SCOPE` markers.

**Pre-push local check**: N/A for a build-system-only slice unless `Source_Core/` changes are added during implementation.

**Override**: N/A.

## Risks / non-goals

- "Any compiler" can overpromise. Mitigation: document support as C++14-capable MSVC/GNU/Clang-family compilers first; unknown compilers receive conservative flags and clear diagnostics.
- Non-Windows source portability may reveal real C++ issues, not just CMake issues. Mitigation: keep those fixes small, update this plan if `Source_Core/` is touched, and verify with dual-target Windows builds plus the new host matrix.
- Existing MSYS2 release flow could regress if neutral presets replace rather than supplement it. Mitigation: leave `ninja-*-msys2` preset names and behavior intact.
- FetchContent dependencies may have OS/compiler-specific failures. Mitigation: record dependency failures separately from first-party CMake issues and prefer upstream options or targeted patches in `cmake/SmatchetThirdParty.cmake`.
- CI runtime may grow. Mitigation: add a lightweight matrix first (`SMATCHET_WITH_WHISPER=OFF`, tests enabled, standalone only), then expand coverage after the base generic path is stable.
- Non-goal: make DX12 / Unreal packaging work on non-Windows.
- Non-goal: replace the publish/release presets with generic presets.
- Non-goal: vendor or mirror all dependencies; existing FetchContent plus offline override docs remain the model.
- Non-goal: rewrite C++ code to support compilers that do not provide C++14 well enough for the current dependency set.

## Verification

Per `AGENTS.md` verification rules, the implementation should be verified with automated commands only.

- **Bucket A (pure-logic ctest, `test-rig`)**: configure/build `host-tests`, then run `ctest --output-on-failure` from the host test build directory.
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msys2`)**: unchanged for Windows/MSYS2; add host-neutral UI-test coverage only after base Linux/macOS OpenGL runners are reliable.
- **Bash-driver scenario / screenshot / sanitizer**: run existing bash drivers through `scripts/dev/test-all.sh` with configurable `SMATCHET_EXE`; run sanitizer smoke on Windows MSVC and Linux Clang/GCC where supported.
- **Build gate**: keep the existing Windows dual-target gate: `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12`.
- **Generic configure/build gates**:
  - `cmake --preset host-debug`
  - `cmake --build --preset host-debug --target SmatchetStandalone`
  - `cmake --preset host-tests`
  - `cmake --build --preset host-tests`
  - `ctest --output-on-failure` from `build/host-tests`
- **CI matrix gates**:
  - Windows + MSYS2 UCRT64 existing job remains required.
  - Windows + MSVC host-neutral configure/build/test.
  - Linux + GCC host-neutral configure/build/test.
  - Linux + Clang host-neutral configure/build/test.
  - macOS + AppleClang host-neutral configure/build/test.
- **Manual residue**: none planned. If a platform can only be manually checked at first, add a tooling backlog entry and keep that platform non-required until automated CI exists.

## Out of scope (flagged, not designed)

- Package manager integration (`find_package` from vcpkg/Conan/Homebrew instead of FetchContent): follow-up plan after generic FetchContent builds are stable.
- Cross-compilation toolchain files for Android/iOS/WebAssembly: follow-up only if the app has platform runtime support.
- Linux/macOS app packaging, codesigning, installers, and desktop entries: separate release-engineering plan.
- Replacing bash/PowerShell scripts with a single cross-platform runner: only fix scripts that block generic CMake verification in this slice.
- Moving the repo to C++17 or newer: explicitly out of scope because project rules require C++14.

## Implementation log

*(populated post-ship per `AGENTS.md` Plan revision after implementation - bullet per shipped commit: `<sha> - <one-line summary>`)* 

## Deviations from plan

*(populated post-ship - what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)

*(populated post-ship - what was actually tested + result, passed / failed / not-run)*

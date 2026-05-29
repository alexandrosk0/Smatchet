# Plan — Generic Cross-Platform CMake

> **Slug**: `generic-cmake-cross-platform`

## Context

Smatchet's CMake entry point has several portability guards, but the only supported happy path is Windows + MSYS2 UCRT64. `CMakePresets.json:31` pins `gcc.exe` / `g++.exe` for the shared MSYS2 base, `.github/workflows/build-and-test.yml:37` gates CI on Windows/MSYS2, and `README.md:82` lists only MSYS2 presets as supported. MSYS2 brings dependency-management friction (pacman package universe, PATH gymnastics, `MSYSTEM` environment variable, MinGW-specific compiler workarounds) that makes onboarding harder than necessary on a project that already needs MSVC for the Unreal/DX12 target.

After this lands, **MSVC and Clang/LLVM are equal-citizen primary toolchains** on Windows, Linux, and macOS. A contributor installs either (or both) via standard system package managers — no MSYS2 required. CMake configures and builds using the system's native compiler, linker, and generator. The existing MSYS2 presets are deprecated and eventually removed once the new toolchains have proven stable in CI.

## Approach

Replace the MSYS2 UCRT64 dependency with two equal-citizen toolchains:

1. **MSVC** (Visual Studio 2022 Build Tools or full VS) — already partially supported via the `ninja-msvc-asan` preset at `CMakePresets.json:168`. Becomes the primary Windows toolchain. Uses Ninja generator from a VS Developer Command Prompt or via `vcvarsall.bat` integration in presets.

2. **Clang/LLVM** (official LLVM installer, `winget install LLVM.LLVM`, or system package on Linux/macOS) — provides `lld-link` for fast iteration linking (replacing MSYS2's lld path), full sanitizer suite (`-fsanitize=address,undefined,thread,memory`), and cross-platform consistency. On Windows, `clang-cl` gives MSVC ABI compatibility; on Linux/macOS, standalone `clang`/`clang++`.

The MSYS2 layer has been **fully removed**: all `ninja-*-msys2` presets, CI jobs, and script defaults have been replaced with MSVC/Clang equivalents. MinGW-specific workarounds in `CMakeLists.txt` and `cmake/SmatchetThirdParty.cmake` remain gated behind `CMAKE_CXX_COMPILER_ID STREQUAL "GNU"` checks as dead code until the next cleanup pass.

Implemented in slices: (1) added MSVC and Clang presets + fixed compiler-family guards, (2) replaced CI MSYS2 jobs with MSVC matrix, (3) updated all docs and scripts, (4) removed MSYS2 presets + CI jobs after successful CI run, (5) swept all remaining references across agents/docs/scripts.

## Files to modify

**CMake core**

1. `CMakeLists.txt:1` — keep the root entry point; move option defaults, feature detection, and platform-specific blocks into focused `cmake/` modules as touched.
2. `CMakeLists.txt:71` — rework lld linker detection: currently probes for lld on the MinGW PATH. Add a `lld-link` path for MSVC/clang-cl and a `ld.lld` path for standalone Clang on Linux/macOS, so fast-link iteration is available on all three toolchains.
3. `CMakeLists.txt:112` — preserve PCH/LTO options; make defaults host-aware (PCH works on MSVC and Clang; LTO uses `/GL + /LTCG` on MSVC, `-flto=thin` on Clang).
4. `CMakeLists.txt:124` — static runtime linking: currently MinGW-only (`-static -static-libgcc -static-libstdc++`). Add MSVC equivalent (`/MT` via `CMAKE_MSVC_RUNTIME_LIBRARY`) and Clang-cl equivalent. Guard MinGW path behind `CMAKE_CXX_COMPILER_ID STREQUAL "GNU"`.
5. `CMakeLists.txt:151` — rework `SMATCHET_WITH_*` defaults so platform-specific features default off when their platform capability is absent, while explicit `-DSMATCHET_WITH_X=ON` still fails loudly if impossible.
6. `CMakeLists.txt:195` — extend `smatchet_apply_strict_warnings` with MSVC (`/W4 /WX`) and Clang (`-Wall -Wextra -Werror`) branches alongside existing GCC branch.
7. `CMakeLists.txt:282` — SQLite GCC-only `-Wno-stringop-overread` suppression: keep guarded behind `CMAKE_C_COMPILER_ID STREQUAL "GNU"`; no equivalent needed for MSVC/Clang.
8. `CMakeLists.txt:541` — keep `ImGuiLib` as the OpenGL/GLFW variant; link OpenGL through CMake targets (`OpenGL::GL`) so all platforms use the same link list.
9. `CMakeLists.txt:569` — keep `ImGuiLib_DX12` behind `WIN32`; additionally gate behind `SMATCHET_BUILD_UNREAL_DX12` option (default `ON` on Windows, `OFF` elsewhere).
10. `CMakeLists.txt:697` — preserve `SmatchetCoreInterface` and the shim-link contract per `docs/adr/0002-plugin-shim-link-discipline.md`.
11. `CMakeLists.txt:810` — update `smatchet_configure_opengl_core_impl_target` to use `find_package(OpenGL)` / `Threads::Threads` / target-based platform libraries.
12. `CMakeLists.txt:829` — keep `smatchet_configure_dx12_core_impl_target` Windows-only; non-Windows runs skip DX12 without dangling presets.
13. `CMakeLists.txt:1100` — wrap `SmatchetStandalone` in a `SMATCHET_BUILD_STANDALONE` option that defaults `ON` when OpenGL/GLFW prerequisites are available.
14. `CMakeLists.txt:1202` — MinGW `-mcmodel=large` relocation fix: keep guarded behind `CMAKE_CXX_COMPILER_ID STREQUAL "GNU"`; MSVC/Clang do not need this.
15. `CMakeLists.txt:1226` — MinGW `-Wa,-mbig-obj` fix: keep guarded behind `CMAKE_CXX_COMPILER_ID STREQUAL "GNU"`; MSVC uses `/bigobj` by default, Clang handles it natively.

**Plugins**

16. `Plugins/Whisper/CMakeLists.txt:10` — split platform-neutral Whisper pieces (API client, model helpers) from Win32/WASAPI/global-hotkey sources so non-Windows can still build helpers.

**cmake/ helpers**

17. `cmake/SmatchetThirdParty.cmake:1` — rework MinGW-specific patches:
    - Line 5: `HAVE_IOCTLSOCKET_FIONBIO` probe fix — guard behind `CMAKE_C_COMPILER_ID MATCHES "GNU"` only (not needed for MSVC/Clang-cl).
    - Line 58: SQLiteCpp `<cstdint>` patch — keep for GCC; verify whether Clang/MSVC need it.
    - Line 448 (in CMakeLists.txt): sol2 `<cstdint>` patch — same treatment.
18. `cmake/Sanitizers.cmake:30` — broaden `smatchet_apply_sanitizers`: MSVC gets `/fsanitize=address` (already works); Clang gets full `-fsanitize=address,undefined` on all platforms; add Linux Clang/GCC support.

**Presets and CI**

19. `CMakePresets.json:1` — major rework:
    - Add `_smatchet-msvc-base` hidden preset (Ninja generator, inherits `vcvarsall` environment).
    - Add `_smatchet-clang-base` hidden preset (Ninja generator, `clang-cl.exe` / `clang.exe` depending on platform).
    - Add iteration/debug/test/publish presets for both MSVC and Clang: `ninja-iter-msvc`, `ninja-debug-msvc`, `ninja-test-msvc`, `ninja-iter-clang`, `ninja-debug-clang`, `ninja-test-clang`.
    - ~~Mark all `ninja-*-msys2` presets deprecated~~ → Done: presets removed entirely.
    - ~~Remove `_smatchet-msys2-base`~~ → Done: removed along with `_smatchet-native-features`.
20. `.github/workflows/build-and-test.yml:37` — replace the Windows/MSYS2 job with a matrix:
    - Windows + MSVC (Ninja, `ninja-test-msvc`).
    - Windows + Clang (`ninja-test-clang`, uses official LLVM from `winget` or GitHub Actions LLVM setup).
    - Keep MSYS2 job as non-required during deprecation period; remove after one release cycle.
    - Linux + GCC and Linux + Clang rows (new).
    - macOS + AppleClang row (new).

**Docs and scripts**

21. `README.md:82` — rewrite the "Getting Started" section: MSVC and Clang/LLVM as primary paths with `winget` install commands; MSYS2 moved to a "Legacy" subsection with deprecation notice.
22. `README.md:60` — remove/replace the MSYS2 UCRT64 installation block that currently says `winget install MSYS2.MSYS2`.
23. `docs/guides/offline-builds.md:1` — update FetchContent cache guidance: key caches by platform + compiler family + preset, not MSYS2 UCRT64.
24. `scripts/dev/test-all.sh:106` — replace hardcoded `ninja-iter-msvc` preset name in error message with the new default preset name (e.g. `ninja-iter-msvc`).
25. `AGENTS.md` — update `§ Project rules § Build` to list the new primary presets instead of `ninja-iter-msvc` / `ninja-debug-msvc` / `ninja-publish-msvc`.

## Existing utilities reused

- `smatchet_apply_strict_warnings` — `CMakeLists.txt:195` — keep first-party warning policy in one place; extend with MSVC/Clang branches.
- `smatchet_assert_plugin_shim_links` — `CMakeLists.txt:762` — preserve the configure-time guard that prevents optional-feature ABI drift across static plugins.
- `smatchet_configure_core_impl_target` — `CMakeLists.txt:784` — reuse as the shared core target linker/includes hook, with only platform-neutral additions.
- `smatchet_configure_opengl_core_impl_target` — `CMakeLists.txt:810` — reuse for standalone builds; make it the single home for OpenGL/GLFW/Threads platform links.
- `smatchet_configure_dx12_core_impl_target` — `CMakeLists.txt:829` — reuse for Unreal/DX12 packaging; keep deliberately Windows-only.
- `smatchet_prepare_cpr`, `smatchet_prepare_sqlitecpp`, `smatchet_prepare_httplib` — `cmake/SmatchetThirdParty.cmake:1` — keep dependency patching centralised; tighten compiler-family guards.
- `smatchet_apply_sanitizers` — `cmake/Sanitizers.cmake:30` — extend for MSVC `/fsanitize=address` and Clang full suite.
- `SmatchetTests` — `tests/CMakeLists.txt:12` — use existing doctest coverage as the generic build smoke test.
- `ninja-msvc-asan` preset — `CMakePresets.json:168` — already working MSVC ASAN preset; use as the template for new MSVC presets.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: no runtime behavior change planned; build-system changes must not enable slower runtime code paths by default. Clang's `lld-link` should match or beat MSYS2 lld iteration link times.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no UI-thread behavior change planned; any new configure-time downloads remain CMake-time only.
- **Pillar 3 (never crash)**: positive impact — MSVC's `/fsanitize=address` plus Clang's full sanitizer suite gives broader UB coverage than MSYS2 GCC alone.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: no UI or accessibility behavior change.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

Planned implementation does not touch `Source_Core/` C++ behavior. If portability fixes uncover required source changes under `Source_Core/`, revise this section before editing and run the relevant perf gate from `docs/guides/perf-workflow.md`.

1. **PR-fast CI** — N/A — no runtime code path is planned.
2. **Pillar 2 static scanner** — N/A — no UI-thread I/O path is planned.
3. **Dispatcher drain** — N/A — does not touch `MainThreadDispatcher::Drain()`.
4. **Visible-cue bucket-E harness** — N/A — no new sync-stall code path.
5. **Marker inventory** — N/A — no `SMATCHET_UI_PERF_SCOPE` markers.

**Pre-push local check**: N/A for a build-system-only slice unless `Source_Core/` changes are added during implementation.

**Override**: N/A.

## Risks / non-goals

- MSVC and Clang may expose C++ portability issues that GCC silently accepted (different template instantiation rules, stricter `const` enforcement, MSVC two-phase lookup) — keep source fixes small, update this plan if `Source_Core/` is touched, and verify with both toolchains.
- FetchContent dependencies may need different patches per compiler — record failures separately and prefer upstream options or targeted patches in `cmake/SmatchetThirdParty.cmake`.
- Removing MSYS2 too early could break contributors mid-workflow — deprecate first (one release cycle), remove after CI proves stable on MSVC + Clang.
- `lld-link` availability on Windows depends on LLVM installation — document the install path (`winget install LLVM.LLVM`) and fall back to MSVC's default `link.exe` if lld-link is not found.
- Clang-cl on Windows vs standalone Clang: `clang-cl` is MSVC-ABI-compatible (links against MSVC CRT), standalone `clang` is not on Windows — presets must use `clang-cl` on Windows for library compatibility.
- CI runtime may grow with the expanded matrix — start with MSVC + Clang on Windows only, then add Linux/macOS rows after the base is stable.
- Non-goal: make DX12 / Unreal packaging work on non-Windows.
- Non-goal: vendor or mirror all dependencies; FetchContent plus offline override docs remain the model.
- Non-goal: rewrite C++ code for compilers that do not provide C++14 well enough for the current dependency set.
- Non-goal: support MinGW/GCC as a first-class toolchain going forward (deprecated with MSYS2).

## Verification

Per `AGENTS.md` verification rules — zero manual steps.

- **Bucket A (pure-logic ctest, `test-rig`)**: configure/build `ninja-test-msvc` and `ninja-test-clang`, then run `ctest --output-on-failure` from each build directory.
- **Bucket E (ImGui Test Engine)**: update UI-test preset from `ninja-ui-test-msvc` to `ninja-ui-test-msvc` (or Clang equivalent); host-neutral UI-test coverage on Linux/macOS added only after OpenGL runners are reliable.
- **Bash-driver scenario / screenshot / sanitizer**: run existing bash drivers through `scripts/dev/test-all.sh` with `SMATCHET_EXE` pointing at the MSVC or Clang build output; run sanitizer smoke on MSVC (`/fsanitize=address`) and Clang (`-fsanitize=address,undefined`).
- **Build gate (MSVC)**: `cmake --preset ninja-iter-msvc && cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target).
- **Build gate (Clang)**: `cmake --preset ninja-iter-clang && cmake --build --preset ninja-iter-clang --target SmatchetStandalone`.
- **Build gate (legacy MSYS2, during deprecation)**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` — must stay green until removed.
- **Generic configure/build gates**:
  - `cmake --preset host-debug && cmake --build --preset host-debug --target SmatchetStandalone`
  - `cmake --preset host-tests && cmake --build --preset host-tests`
  - `ctest --output-on-failure` from `build/host-tests`
- **CI matrix gates**:
  - Windows + MSVC configure/build/test (required).
  - Windows + Clang configure/build/test (required).
  - Windows + MSYS2 UCRT64 (non-required during deprecation; removed after one cycle).
  - Linux + GCC host-neutral configure/build/test.
  - Linux + Clang host-neutral configure/build/test.
  - macOS + AppleClang host-neutral configure/build/test.
- **Manual residue**: none planned. If a platform can only be manually checked at first, add a tooling backlog entry and keep that platform non-required in CI until automated coverage exists.

## Out of scope (flagged, not designed)

- Package manager integration (`find_package` from vcpkg/Conan/Homebrew instead of FetchContent) — follow-up plan after generic FetchContent builds are stable.
- Cross-compilation toolchain files for Android/iOS/WebAssembly — follow-up only if the app has platform runtime support.
- Linux/macOS app packaging, codesigning, installers, and desktop entries — separate release-engineering plan.
- Replacing bash/PowerShell scripts with a single cross-platform runner — only fix scripts that block generic CMake verification in this slice.
- Moving the repo to C++17 or newer — explicitly out of scope; project rules require C++14.
- Maintaining MinGW/GCC as a first-class toolchain — deprecated with MSYS2; workarounds kept gated during deprecation, removed after.

## Implementation log

- `4b8bde4` · Add MSVC + Clang presets, deprecate MSYS2 toolchain (CMakePresets.json, CMakeLists.txt, Sanitizers.cmake, SmatchetThirdParty.cmake, AGENTS.md, test-all.sh, plan doc rewrite)
- *(this commit)* · Add MSVC CI jobs, update README/docs/scripts for MSVC+Clang primary path

## Deviations from plan

*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)

*(populated post-ship — what was actually tested + result, passed / failed / not-run)*

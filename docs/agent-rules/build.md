# Build rules (load on-demand)

Trigger: **building** the project (configuring presets, the light build, the dual-target verify, or clearing the Unreal build). AGENTS.md § Project rules keeps a one-line build pointer here; this doc has the detail.

## Presets + exe path

`cmake --build --preset ninja-iter-msvc` (iter), `ninja-debug-msvc` (debug), `ninja-publish-msvc` (publish). Clang equivalents: `ninja-iter-clang`, `ninja-debug-clang`. Exe at `build/<preset>/Smatchet.exe` (the CMake target is `SmatchetStandalone` but `OUTPUT_NAME` ships as `Smatchet`).

## ctest presets

`CMakePresets.json` carries a `testPresets` section so `ctest --preset <name>` resolves for every test-bearing configure preset (`ninja-test-{msvc,clang}`, `ninja-debug-{msvc,clang}`, `ninja-{msvc,clang}-asan`, `ninja-publish-msvc`, `ninja-publish-msvc-arm64`, `ninja-tsan-linux`, `ninja-test-linux`, `ninja-fuzzer-linux`); each sets `output.outputOnFailure: true` to match the bare `ctest --output-on-failure` the CI workflows run. The two forms are equivalent: `ctest --preset <name>` from the repo root, or `cd build/<preset> && ctest --output-on-failure` (the `working-directory` form the CI YAMLs use). Verify a preset resolves without running anything via `ctest --preset <name> -N` (list-only). `tests/fuzz/README.md` documents both forms for the fuzzer lane.

## TSan on Linux (`ninja-tsan-linux`) — install the Clang TSan runtime first

`SmatchetTsanTests` is the only assertion-based test executable that builds + runs headless on Linux (the primary doctest/UI rigs need MSVC ABI or ImGui/GLFW/X11/GL). On a fresh Linux container `clang-18` ships WITHOUT the compiler-rt sanitizer archives — all TUs compile but the link fails (`ld.lld: cannot open .../libclang_rt.tsan-x86_64.a`). Install the toolchain-matched runtime first:

`sudo apt-get install -y libclang-rt-18-dev` (generally `libclang-rt-$LLVM_VERSION-dev`)

Then `cmake --preset ninja-tsan-linux && cmake --build --preset ninja-tsan-linux && ctest --preset ninja-tsan-linux`. The preset's FetchContent deps are all git-clone based, so it configures through the agent proxy without the release-tarball 403 workaround other presets need (infra `remote-container-fetchcontent-403`). Verified end-to-end in a remote container 2026-07-09: package install → configure → 97-target build → link → suite green.

## Fast Linux unit signal (`ninja-test-linux`) — a linux-container agent's runnable test tier

`ninja-test-linux` builds + runs the **same** curated ImGui-free Core unit subset as `ninja-tsan-linux` but **without the sanitizer** — a fast local unit signal for a `linux-container` environment (finding C3 / Proposal P6), where the MSVC test presets and the ImGui doctest rig are `[n/a]` (`scripts/dev/doctor.sh --tier linux-container`; `project.config.json` § environments). It needs no `libclang-rt` package (no TSan runtime) and, like the TSan preset, resolves `doctest` via git clone (no release-tarball egress). Run it end-to-end:

`cmake --preset ninja-test-linux && cmake --build --preset ninja-test-linux && ctest --preset ninja-test-linux`

One `SMATCHET_CORE_TEST_SUBSET=ON` gate (`tests/CMakeLists.txt`) drives the identical subset for both lanes, so the plain and TSan builds can never silently diverge. CI coverage of this subset already runs via the TSan lane (PR-on-threading-paths + nightly, `.github/workflows/tsan-linux-nightly.yml`); this preset adds the *fast local* signal a container agent runs before it escalates. Verified end-to-end in a remote container 2026-07-14: configure → build → `ctest` green (0 failures).

## Warnings as errors

**First-party warnings are errors** (`/WX` MSVC, `-Werror` clang) — force-ON via `SMATCHET_WARNINGS_AS_ERRORS=ON` in the `_smatchet-msvc-base` / `_smatchet-clang-base` presets, so every dev + CI + light build enforces it; the CMake option itself defaults OFF for external/raw-cmake/Unreal (`vs-unreal-msvc`) consumers. FetchContent deps + vendored object libs are never passed through the strict-warning helper, so third-party warnings can't break the build.

## Light build (default for core-feature work)

Default when the task is NOT an AI/Whisper/MCP feature — faster, fewer moving parts: add `-DSMATCHET_WITH_WHISPER=OFF -DSMATCHET_WITH_AI=OFF -DSMATCHET_WITH_MCP=OFF` to the configure step (the `_smatchet-light-features` preset fragment + `ninja-publish-light-msvc` encode the same OFF triple; CI's "Windows + MSVC (Smatchet light)" job uses exactly this flag set). Still verify the FULL config before merge when the diff could touch those subsystems — flip a full-configured `build/<preset>` by wiping it or reconfiguring with the OFF flags.

## MSYS2 retired

**Never propose MSYS2 for building the project** — the `*-msys2` presets are **retired** (use `ninja-iter-msvc` or `ninja-iter-clang`); the repo-owned PowerShell scripts (`scripts/dev/local/build_and_run.ps1` / `scripts/dev/local/build_standalone.ps1`) auto-bootstrap the MSVC env via `vswhere`→`vcvars64`, so no Developer Prompt or MSYS2 is required, and a `*-msys2` preset is rejected fast with that hint.

## Dual-target verify

`Source/Core/` compiles into both `SmatchetStandalone` (OpenGL+GLFW) and `SmatchetCore_DX12` (Unreal). Full verify: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12`. In a shell without `cl.exe` on PATH, wrap it via `scripts/dev/with-msvc-env.sh` (bash) or `scripts/dev/with-msvc.ps1` (PowerShell) — see § MSVC toolset env. (Macro divergence + the don't-pollute-Core-headers rules live in [`cpp-rules.md`](cpp-rules.md) § Dual-target.)

## MSVC toolset env (bash wrapper + multi-VS pin)

Build from bash through the wrapper — it sources `vcvars64` with the pinned toolset so `cl.exe` gets `INCLUDE`/`LIB` and a newer side-by-side toolset can't shadow it:

`bash scripts/dev/with-msvc-env.sh cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12`

(PowerShell equivalents: `scripts/dev/with-msvc.ps1`, `scripts/dev/local/build_and_run.ps1`.) Without a wrapper, a bare `cmake --build` from bash fails with `Cannot open include file: 'stdio.h'` (no `INCLUDE`).

> **From the Bash tool, default to the `.sh` wrapper** (`with-msvc-env.sh`): the `.ps1` wrapper needs `-ExecutionPolicy Bypass`, which the harness command classifier auto-denies, so it is effectively uninvokable from Bash (tooling self-improvement `process.md:28`). **Shared-tree edit hygiene**: when a concurrent-session shared-tree warning has appeared this session, a sibling's `git reset`/`checkout` can clobber in-flight edits and desync the Read/Edit tool cache from disk — commit immediately after the first successful edit, and prefer an on-disk patch (`python`/`sed`) + `git diff` verification over trusting tool-cache state.

The toolset pin is `build.msvc_toolset_pin` in `project.config.json` (currently `14.38`; override with `$SMATCHET_VCVARS_VER`). It matters on a multi-VS box: an **unpinned** `vcvars64` selects the *newest* installed toolset, whose STL headers reject the cached older `cl.exe` with **`error STL1001`** — which can cascade to `C2801`/`C2333` inside `<memory>`/`<vector>`/`<thread>` (the C++23 `static operator()` STL under `/std:c++14`). The wrappers pass `-vcvars_ver=<pin>`; if you call `vcvars64.bat` by hand, pass the same `-vcvars_ver` (the configured toolset is in `build/<preset>/CMakeCache.txt`). On a VS-18 box the working manual invocation is `"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -vcvars_ver=14.38.33130` (locate via vswhere; the default 14.50 toolset fails in STL headers against the cached configure).

## Fresh-worktree configure pitfalls (CMake 4.x + FetchContent)

All four bit multiple agents during the multi-grid Slice-0/1 + perf-gate-revival sessions (2026-06-06/07):

1. **CMake 4.x drops the default MSVC `/EHsc` + `/DWIN32 /D_WINDOWS` flags on a FRESH configure** — the whole tree builds with exceptions off; doctest TUs fail `C2338: Exceptions are disabled!` and product TUs fail `C4530 + /WX → C2220`. Long-lived build dirs only work because their caches predate the CMake upgrade. **Resolved (PR-15):** the root `CMakeLists.txt` now BAKES these flags right after `project()` — an `if(MSVC)` block append-if-missing-es `/DWIN32 /D_WINDOWS /EHsc` into `CMAKE_CXX_FLAGS` and `/DWIN32 /D_WINDOWS` into `CMAKE_C_FLAGS` and `set(... CACHE STRING ... FORCE)`s them, so a fresh CMake-4 configure can't drop them. The old manual workaround (`cmake --preset … "-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /EHsc"`) is **no longer required** — a bare `cmake --preset ninja-iter-msvc` on a fresh worktree now bakes the right flags (verify: `grep CMAKE_CXX_FLAGS: build/<preset>/CMakeCache.txt` → `…/EHsc`).
2. **A FAILED first configure poisons the cache silently** — `CMakeCache.txt` is left with empty `CMAKE_CXX_FLAGS`, and a later *successful* configure reuses it. The PR-15 bake (#1) also neutralises this: the `if(MSVC)` block detects an empty cached `CMAKE_CXX_FLAGS` (logs a `STATUS` line naming the poisoned-cache cause) and re-injects `/EHsc` on the next configure, and the `CACHE … FORCE` write persists the repair. If doctest TUs still fail "Exceptions are disabled" in a tree that "configured fine", check for `CMAKE_CXX_FLAGS:STRING=` empty in the cache → wipe the build dir and reconfigure (the bake will restore the flags).
3. **Configure presets serially on a fresh worktree — and serialize preset BUILDS too while they share `.fetchcontent-*` dep build dirs** — `FETCHCONTENT_BASE_DIR` is the shared `.fetchcontent-src/`, so two first-time configures/builds in parallel collide (`ninja: failed recompaction: Permission denied`, `LNK1104` on `glfw3.lib`). Even once the dep sources exist, two presets building in parallel still race MSVC PDB writes on the shared dep build trees (e.g. `curl-build`) → **C1041** (cannot open program database); serialize the builds, or give each preset its own `-DFETCHCONTENT_BASE_DIR`. (A `/FS` flag on dep-build CXX flags would remove the race; unshipped — only provable on Windows CI.)
4. **MSVC toolset-mismatch guard (configure-time FATAL)** — the root `CMakeLists.txt` now fails configure with `MSVC toolset mismatch in this build directory` when the cached `cl.exe`'s minor version doesn't match `build.msvc_toolset_pin` in `project.config.json` (e.g. a build dir configured with 14.50's `cl` while the pin is 14.38). This is the cause of the otherwise-cryptic 5000-error `<chrono>`/`<xtimec.h>` cascade (`STL1001`, `'__int64' should be preceded by ';'`, `'_LIKELY' undeclared`) that appears when you build through the wrapper (which sets `INCLUDE` to the pinned headers) but the dir was configured WITHOUT it (so the cache baked a newer `cl`). **Recovery: wipe the build dir and reconfigure THROUGH the wrapper** — `rm -rf build/<preset>` then `bash scripts/dev/with-msvc-env.sh cmake --preset <preset>` (PowerShell: `scripts/dev/with-msvc.ps1 cmake --preset <preset>`), which pins `-vcvars_ver` from `project.config.json`. The guard is MSVC-only (never fires for Clang/GCC/Unreal), **local-only** (skipped when `$CI` is set — CI runners configure fresh with their own consistent toolset, so the pin is a local-dev convention, not a CI requirement), and fails open when the pin is unreadable; pass `-DSMATCHET_ALLOW_TOOLSET_MISMATCH=ON` to downgrade it to a warning if you deliberately want an off-pin toolset. (Authoring a NEW such configure-time `FATAL_ERROR` keyed on a local knob? It MUST be `ENV{CI}`-scoped — the `cmake-local-gate-ci-scope` lint enforces it; see [`cpp-rules.md`](cpp-rules.md) § Tiered enforcement.)
5. **Delegated/subagent build-verification is `cmake --build` ONLY — never `cmake --preset` / reconfigure / wipe a shared build dir.** A subagent dispatched to verify a change builds against the orchestrator-provisioned build dir (`cmake --build --preset <preset> --target SmatchetStandalone SmatchetCore_DX12`) and MUST NOT reconfigure it. On a multi-VS box a stray `cmake --preset` re-picks the wrong side-by-side toolset (14.50 `cl` vs 14.38 STL headers → ~5000 `<chrono>` errors) or drops `/EHsc` on a fresh configure (doctest "Exceptions are disabled"), leaving the dir broken for the next consumer — and the agent then mis-reports a self-inflicted break as a "pre-existing build-infra fault" (infra `subagent-build-reconfigure-hazard`). **If a build dir genuinely looks broken, STOP and report — do not reconfigure.** The orchestrator injects this clause into every build-touching delegation packet (governs `tracker-backend` / `offline-sync` / `grid-engine` / `test-rig` / `ui-host` + any future build-touching agent — single-sourced here, not copied per-agent, per the DRY pillar).
6. **Worktree-isolated build agents: redirect FetchContent at the shared cache to skip the re-fetch (optional).** A fresh worktree's `.fetchcontent-src` is empty, so its first configure cold-clones every dep. As of **#1166** (`GIT_EXEC_PATH` restore in `scripts/dev/with-msvc-env.sh`) a cold configure THROUGH the wrapper now succeeds on its own — submodule-bearing deps (cpr→curl, sol2→Catch) clone cleanly — so the redirect is no longer required for correctness. To avoid re-fetching, pass `-DFETCHCONTENT_BASE_DIR=<main-repo>/.fetchcontent-src` (e.g. `C:/Development/Smatchet/.fetchcontent-src`) on the FIRST configure so the worktree reuses the main checkout's populated dep sources. The orchestrator pre-seeds this flag in worktree-build delegation packets so agents don't rediscover it (infra `worktree-FetchContent-cache`).

> **Resolved (don't re-add):** the old "never configure under bash+vcvars — cpr's FetchContent `git submodule` dies with `git-sh-setup: file not found`" workaround is **obsolete** — fixed at the source by **#1031** (`GIT_SUBMODULES ""` on every git `FetchContent_Declare`; FetchContent runs `git submodule update` unconditionally and that flag skips it). Configuring under bash+vcvars via `scripts/dev/with-msvc-env.sh` is now correct, including fresh clones. The scheduled fresh-clone-configure CI (close-gate-gaps Slice 4) is the standing gate that keeps it that way.

## Remote-container (cloud sandbox) posix-core-check bootstrap

In the Claude Code remote container the network policy allows `git clone` but **403s GitHub release-asset / codeload tarball downloads**, so a bare `cmake --preset posix-core-check` dies at cpr's internal FetchContent of `curl-7.80.0.tar.xz`; glfw's configure also needs `xorg-dev` + `libgl1-mesa-dev` (never built in this preset, but `find_package(X11 REQUIRED)` runs). Run `bash scripts/dev/remote-container-bootstrap.sh` once per container — it shallow-clones curl at the pinned tag into `.fetchcontent-src/curl-manual`, apt-installs the two packages, and configures the preset with `-DFETCHCONTENT_SOURCE_DIR_CURL` pointing at the clone (idempotent; `--no-configure` provisions only). Origin: infra `remote-container-fetchcontent-403` (2026-07-05 session, PRs #1614/#1615).

## ASan over the test rig

The `ninja-msvc-asan` preset does not enable tests; to run the doctest rig under ASan, override at configure: `cmake --preset ninja-msvc-asan -DSMATCHET_BUILD_TESTS=ON`. (The `CallstackParser` ReDoS *timing* sentinel is now ASan-aware — #1215 (`36521f72`) guards it with `#if defined(__SANITIZE_ADDRESS__)` and widens the wall-clock cap 2000 ms→20000 ms, so the full suite runs clean under ASan; the old "exceeds its 2000 ms cap under ASan slowdown" caveat no longer applies.)

## Stale-PCH recovery (C2859)

`SmatchetStandalone` uses a precompiled header (`Source/Core/include/SmatchetPch.h`; `SMATCHET_USE_PCH=ON` by default — `SmatchetCore_DX12` and the publish presets are PCH-less). After a touched PCH-included header or a toolchain change, a stale PCH can surface as **C2859** (or spurious C1xxx). Recovery, cheapest first:
1. **Targeted PCH regen (~4s)** — rebuild the target (`cmake --build --preset <preset> --target SmatchetStandalone`); CMake regenerates the `cmake_pch` artifact when `SmatchetPch.h`'s inputs changed, so no full rebuild is needed.
2. **Escape** — reconfigure with `-DSMATCHET_USE_PCH=OFF` to bypass the PCH entirely (also the right move when bisecting compile errors; see `CMakeLists.txt` § `SMATCHET_USE_PCH` option).

## Clearing the Unreal build — delete `lib/` only

`Source/UnrealPlugins/SmatchetImGuiPlugin/ThirdParty/Smatchet/` is **mixed** — `lib/Win64/{Debug,Development}/*.lib` is git-ignored (~912 MB, regenerated by the `SmatchetPackageUnrealLibs_DX12` target, safe to delete), but `include/*.h` (`imgui.h`, `imconfig.h`, `SmatchetImGuiHost.h`, `SmatchetImGuiHostC.h`, `SmatchetDefaults.h`) is **committed**. To clear the build, `rm` `.../ThirdParty/Smatchet/lib/` only — never the whole `Smatchet/` dir (removes 5 tracked headers). `git ls-files <dir>` is the only reliable tracked-vs-ignored check; scope it to the exact dir before any cleanup `rm`. Path is hardcoded in `SmatchetImGuiPlugin.Build.cs` + `CMakeLists.txt` (`SMATCHET_UNREAL_THIRDPARTY_DIR`).

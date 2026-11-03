# Build

> First build on this machine? [QUICKSTART.md](QUICKSTART.md) is the 5-minute path; this document is the reference.

Smatchet uses CMake presets with **MSVC** and **Clang/LLVM** as equal-citizen primary toolchains.
All supported shared presets live in `CMakePresets.json` so Cursor and CMake
Tools see the same options.

## Prerequisites

### Build toolchain

- CMake 3.24 or newer
- Ninja build system
- Git (Git for Windows on Windows hosts — provides the `bash.exe` the dev scripts need; **must be earlier on PATH than `C:\Windows\System32\bash.exe` which is the WSL launcher**)
- One of:
  - **MSVC** — Visual Studio 2022 (Community or Build Tools). Run builds from a VS Developer Command Prompt.
  - **Clang/LLVM** — `winget install LLVM.LLVM` on Windows. Uses `clang-cl` for MSVC ABI compatibility.

Install the recommended toolchain (MSVC):

```powershell
winget install Git.Git           # Git for Windows
# Visual Studio 2022 Build Tools (or Community/Professional/Enterprise).
# The bare package installs only the core installer — --override passes the
# C++ workload through, which is what brings cl.exe and the Windows SDK.
winget install Microsoft.VisualStudio.2022.BuildTools --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

Or Clang/LLVM:

```powershell
winget install LLVM.LLVM
```

### Dev-script CLI tools

The orchestrator + agent scripts (`scripts/dev/*.sh`, `agent-layer/agents/scripts/core/merge-watcher.py`, etc.) require a small set of CLI tools beyond the build toolchain:

| Tool | Used by | Install (Windows) |
|---|---|---|
| `gh` | merge-gates poller, PR ops, watcher daemon | `winget install GitHub.cli` |
| `jq` | **test harness only** — `merge_gates.bats` mocks `gh` via jq. The merge-gates poller + watcher daemon parse via gh's **bundled** jq (`gh api --jq`), so no standalone jq is needed at runtime. | `winget install jqlang.jq` |
| `python` 3.11+ | dev scripts (perf-compare, watcher CLI, etc.) | `winget install Python.Python.3.13` |
| `clang-format` / `clang-tidy` / `cppcheck` | lint hooks | Install via LLVM or Visual Studio individual components |
| `bats` | `tests/bats/*.bats` regression suite (merge-gates poller, etc.) | `npm i -g bats` |
| `shellcheck` | `agent-layer/agents/scripts/core/test-shell-lint.sh` (pre-push gate) | `npm install -g shellcheck` |

Verify the full set in one shot:

```bash
bash scripts/dev/check-required-tools.sh
```

Exits 0 when every required tool resolves; fails loudly with install hints for any missing tool. The orchestrator's `setup-harness.sh` runs this on every fresh clone.

Install whatever it reports missing:

```bash
bash scripts/dev/setup-env.sh
```

Resolves each missing tool to a package on the host's native manager (winget / MSYS2 `pacman` / `apt` / `brew`, `npm` for `bats` + `shellcheck`), installs it, then re-runs the probe as its verdict. `--dry-run` prints the plan only, `--yes` skips the prompt, `--list` dumps the tool → package map. Idempotent. It covers the dev-script CLI tools in the table above — **not** the MSVC / Clang build toolchain, which stays a manual install per the section above. Full flag reference: [`agent-layer/docs/harness/SETUP.md`](agent-layer/docs/harness/SETUP.md) § Installing the missing ones.

**Scheduled-Task / service environments** (notably `SmatchetMergeWatcher`) get a more minimal PATH than an interactive shell and may not resolve `gh` / `jq` / `bash` via bare-name lookup. The `merge-watcher-install-autostart.ps1` installer probes standard install locations + winget's Links dir + Git's `bin` dir explicitly so daemons keep working. See [`docs/plans/shipped/smatchet-merge-watcher.md`](docs/plans/shipped/smatchet-merge-watcher.md) § Daemon environment prerequisites for the gotchas.

## Supported Presets

**MSVC (primary):**

- `ninja-iter-msvc`: fast standalone iteration (`RelWithDebInfo`)
- `ninja-iter-agentdebug-msvc`: iteration + `SMATCHET_AGENT_DEBUG=ON` (agent NDJSON channel; own build dir)
- `ninja-debug-msvc`: full standalone debug (`Debug`)
- `ninja-test-msvc`: doctest rig (`RelWithDebInfo`)
- `ninja-msvc-asan`: ASAN via MSVC `/fsanitize=address`

**Clang/LLVM (primary):**

- `ninja-iter-clang`: fast standalone iteration (`RelWithDebInfo`)
- `ninja-debug-clang`: full standalone debug (`Debug`)
- `ninja-test-clang`: doctest rig (`RelWithDebInfo`)
- `ninja-clang-asan`: ASAN via clang-cl (ASan only on Windows — see § Troubleshooting)

## Common Workflows

**The one command** — from the repo root, in an ordinary bash (Git Bash on Windows):

```bash
./build.sh                                       # build + run
./build.sh --build-only
./build.sh --preset ninja-debug-msvc --build-only
```

`build.sh` auto-detects a preset (`cl.exe` → `ninja-iter-msvc`; only `clang-cl.exe` →
`ninja-iter-clang`; neither compiler on `PATH` → `ninja-iter-msvc`, because the MSVC environment
is imported for you), prints its choice, and imports the MSVC environment via
`scripts/dev/with-msvc-env.sh` when `cl.exe` isn't already on `PATH` — so no VS Developer Command
Prompt is needed. When no usable Visual Studio install is found it exits `78` and prints the
winget install commands (the diagnosis rides an internal out-of-band status file, so a build
tool's own exit code is never mistaken for a missing toolchain).

Everything below is the **explicit-preset** form: what CI runs, and what to use when the MSVC
environment is already active.

**MSVC** — run from a VS Developer Command Prompt (or inside `scripts/dev/with-msvc-env.sh`):

```bash
cmake --preset ninja-iter-msvc
cmake --build --preset ninja-iter-msvc
```

**Clang/LLVM** — ensure `clang-cl` is on PATH:

```bash
cmake --preset ninja-iter-clang
cmake --build --preset ninja-iter-clang
```

Standalone debug:

```bash
cmake --preset ninja-debug-msvc
cmake --build --preset ninja-debug-msvc
```

Unreal plugin iteration:

```bash
cmake --preset ninja-iter-unreal-msvc
cmake --build --preset ninja-iter-unreal-msvc
```

`build.sh` delegates to `scripts/dev/local/build-and-run.sh`, which can also be called directly
when the MSVC environment is already active (it does **not** import one itself):

```bash
bash scripts/dev/local/build-and-run.sh
bash scripts/dev/local/build-and-run.sh --preset ninja-iter-msvc
bash scripts/dev/local/build-and-run.sh --build-only
bash scripts/dev/local/build-and-run.sh --run-only -- --config foo
```

`scripts/dev/with-msvc-env.sh <command…>` remains the wrapper for running an **ad-hoc** command
inside the MSVC environment (`with-msvc-env.sh cl /?`, `with-msvc-env.sh cmake --preset …`);
`build.sh` is the shortcut for the build case specifically.

### MSVC from bash (Git Bash)

**MSYS2 is not required or recommended for building Smatchet.** The `ninja-iter-msys2`
preset is retired. Use `ninja-iter-msvc` (MSVC) or `ninja-iter-clang` (clang-cl) instead.

`scripts/dev/with-msvc-env.sh` is the single source of truth for the `vcvars64` env (`build.sh`
routes through it, as do the sanitizer and packaging wrappers) — a wrapper that picks
the VS install carrying the pinned MSVC toolset (`build.msvc_toolset_pin` in
`project.config.json`, overridable via `$SMATCHET_VCVARS_VER`) via `vswhere.exe`,
sources `vcvars64.bat -vcvars_ver=<pin>` through a PowerShell-mediated cmd.exe
invocation (works around Git Bash's MSYS path-conversion quirks), and `exec`s
the wrapped command with MSVC `INCLUDE` / `LIB` / `PATH` populated:

```bash
bash scripts/dev/with-msvc-env.sh cmake --build --preset ninja-iter-msvc --target SmatchetStandalone
bash scripts/dev/with-msvc-env.sh cmake --version    # smoke test: VS-bundled CMake
bash scripts/dev/with-msvc-env.sh cl                 # smoke test: MSVC compiler banner
```

Without the wrapper, `cmake --build` from a plain bash session fails with
"Cannot open include file: 'stdio.h'" — `cl.exe` runs but lacks the env
that vcvars64 normally sets. Covers Community / Professional / Enterprise /
BuildTools editions automatically.

## First-time verification

Run on a fresh clone to confirm the toolchain is wired correctly. Expect
**~5 minutes** the very first time (FetchContent downloads + builds
nlohmann/json, cpr, SQLiteCpp, cpp-httplib, md4c, ImGui, GLFW, Lua, sol2, and
ghc::filesystem into `build/<preset>/_deps/`); subsequent configures complete
in seconds.

```bash
# 1. Toolchain pre-flight (instant). Prints [PASS] / [FAIL] / [WARN] per check.
#    Run from Git Bash / WSL; from PowerShell use `bash scripts/dev/doctor.sh`.
bash scripts/dev/doctor.sh

# 2. Configure + build the test rig.
cmake --preset ninja-test-msvc
cmake --build --preset ninja-test-msvc

# 3. Run the test rig.
ctest --test-dir build/ninja-test-msvc --output-on-failure

# 4. Build the standalone exe.
cmake --build --preset ninja-iter-msvc --target SmatchetStandalone

# 5. Optional: static-analysis pass.
python .\scripts\dev\local\run_cppcheck.py
```

If step 1 prints `Doctor: RED`, fix the listed prerequisites before
continuing — CMake errors on a missing compiler install are
much harder to debug than the doctor's install hints.

## Build-system layout

The build is split into per-component `CMakeLists.txt` files with the root as
orchestrator. The component files are pulled in via `include()` — deliberately
**not** `add_subdirectory()` — so every target, variable, and source-file
property lives in the root directory scope and artifact/object paths are
identical to the former monolithic root file (see each component file's header
comment before editing):

| File | Owns |
|---|---|
| `CMakeLists.txt` (root) | `project()`, options, toolchain/global flags, all third-party FetchContent pins (json/cpr/SQLiteCpp/httplib/md4c/ghc/GLFW/Lua+sol2/whisper.cpp/ImGui + the `ImGuiLib*` wrapper libs), component orchestration, `tests/` hookup |
| `Source/Core/CMakeLists.txt` | `CORE_SOURCES`, `SmatchetCoreInterface` + feature shims, core-impl configure helpers, `SmatchetCore_DX12`, `SmatchetCore_PosixCheck` |
| `Source/Standalone/CMakeLists.txt` | `SmatchetStandalone` exe + its config tail (icon, Scripts link, PCH, LTO/IPO, sanitizers, bucket-E wiring) |
| `Source/Mobile/CMakeLists.txt` | `SmatchetMobile` Android `.so` (ANDROID only) |
| `Source/Plugins/Mcp/CMakeLists.txt` | `SmatchetPlugin_Mcp` |
| `Source/Plugins/LuaConsole/CMakeLists.txt` | `SmatchetPlugin_LuaConsole` |
| `Source/Plugins/Whisper/CMakeLists.txt` | Whisper plugin (a true `add_subdirectory`, predates the split) |
| `Source/UnrealPlugins/CMakeLists.txt` | DX12 plugin variants, `SmatchetImGuiHost_DX12`, `SmatchetPackageUnrealLibs_DX12` packaging |

## Configuration Options

Smatchet exposes several CMake options to customize the build:

| Option | Default | Description |
| :--- | :--- | :--- |
| `SMATCHET_WITH_LUA_AUTOMATION` | `ON` | Builds with the Lua console plugin and `sol2` bindings for field automation. |
| `SMATCHET_WITH_MCP` | `ON` | Builds the Model Context Protocol (MCP) server plugin. |
| `SMATCHET_WITH_AI` | `ON` | Builds the AI assistant side panel (provider-pluggable `IAiClient`). |
| `SMATCHET_WITH_WHISPER` | `ON` | Builds the Whisper push-to-talk dictation plugin (Windows). |
| `SMATCHET_ENABLE_STRICT_WARNINGS`| `ON`  | Applies strict compiler warnings (`/W4` or `-Wall -Wextra`) to first-party code. |
| `SMATCHET_WARNINGS_AS_ERRORS` | `OFF` | Treats the strict warnings as errors (the base presets force it `ON`; default `OFF` keeps external/raw-CMake/Unreal consumers building). |

## Local Overrides

If you need local-only presets, create `CMakeUserPresets.json` in your checkout.
Shared presets should stay in `CMakePresets.json`; local machine-specific tweaks
belong in the user preset file.

## Unreal Packaging

`SmatchetPackageUnrealLibs_DX12` packages the DX12-compatible core library,
ImGui, and public headers into
`Source/UnrealPlugins/SmatchetImGuiPlugin/ThirdParty/Smatchet` for Unreal Build Tool
consumption. What it stages must match the libraries `SmatchetImGuiPlugin.Build.cs`
links (a missing required lib fails with `BuildException`, a missing optional one is
skipped silently); the rule and the post-packaging check are in
[`SmatchetImGuiPlugin/README.md` § Packaging the Native Libraries](Source/UnrealPlugins/SmatchetImGuiPlugin/README.md#packaging-the-native-libraries).

## Troubleshooting

How to fix a build break (ask for the preset, reproduce with it, one-sentence root cause, minimum diff, never silence warnings) is the agent layer's [`build.md` § Fix discipline](agent-layer/docs/agent-rules/build.md); this section holds the Smatchet facts and the known causes to check first.

### Stack facts

Verify against `CMakePresets.json` and the root `CMakeLists.txt` if in doubt.

- **Compilers and linkers.** MSVC presets (`_smatchet-msvc-base`) pin `cl.exe` and `link.exe`. Clang presets (`_smatchet-clang-base`) use `clang-cl` for the MSVC ABI and leave the linker to CMake. The native Linux clang presets (`_smatchet-clang-linux-base`) link with `ld.lld` when it sits next to the compiler, else BFD `ld`; `SMATCHET_NO_LLD=1` opts out.
- **Iter vs publish.** `ninja-iter-msvc` is RelWithDebInfo with the PCH on (`SMATCHET_USE_PCH`) and `compile_commands.json` exported. `ninja-publish-msvc` is Release with LTO (`SMATCHET_ENABLE_LTO=ON`), no PCH, the static CRT (`SMATCHET_WINDOWS_FULLY_STATIC_RUNTIME=ON`), the test rig built so ctest can gate publish, and the Unreal ThirdParty output dir.
- **Warnings.** `SMATCHET_ENABLE_STRICT_WARNINGS` (default `ON`: `/W4` or `-Wall -Wextra`) applies to first-party targets only; the base presets force `SMATCHET_WARNINGS_AS_ERRORS=ON`. Never lower either to get a build green.
- **Dependencies.** FetchContent, each pinned to a commit SHA: nlohmann/json, cpr, SQLiteCpp, cpp-httplib, md4c, ghc::filesystem, GLFW, sol2, whisper.cpp and Dear ImGui (root `CMakeLists.txt`), doctest (`tests/CMakeLists.txt`), imgui_test_engine (`cmake/ImGuiTestEngine.cmake`). Lua 5.3.6 is a sha256-pinned tarball download, not FetchContent. Sources live in `.fetchcontent-src/` (the `FETCHCONTENT_BASE_DIR` default), shared by every preset in one checkout; local configures skip the FetchContent update step (`FETCHCONTENT_UPDATES_DISCONNECTED=ON`), CI does not.
- **Test rig.** `SMATCHET_BUILD_TESTS` (default `OFF`) builds the doctest rig under `tests/`; it is `ON` in the `ninja-test-*`, `ninja-debug-*`, `ninja-msvc-asan`, `ninja-publish-msvc` and Linux test presets. Run it with `ctest --preset <name>`.
- **Sanitizers.** `cmake/Sanitizers.cmake` reads the single-valued `SMATCHET_SANITIZER` (`asan` | `tsan` | `msan` | `fuzzer` | `off`) and adds its flags `PRIVATE` to `SmatchetStandalone`, `SmatchetCore_DX12`, the test executables and the fuzz targets, so vendored deps stay uninstrumented. Presets: `ninja-msvc-asan` (MSVC `/fsanitize=address`, Debug, builds and instruments the doctest rig via `SMATCHET_SANITIZE_TESTS`); `ninja-clang-asan` and `ninja-ui-test-asan-{msvc,clang}` (ASan; the UI-test pair adds the bucket-E surface); `ninja-tsan-linux` (TSan over the curated ImGui-free threading subset, `SmatchetTsanTests`); `ninja-fuzzer-linux` (libFuzzer). On Windows every ASan lane is **ASan only**: `clang-cl` takes the MSVC branch of `smatchet_apply_sanitizers()`, so UBSan (`-fsanitize=undefined`) is applied only by the native Linux Clang branch. MSVC warns and ignores `tsan`. MSan has no preset: `-DSMATCHET_SANITIZER=msan` fails configure unless the compiler is Clang, and LLVM ships MSan runtimes for Linux only.

### Common causes — check first

- **Compiles under iter, fails under publish with an undeclared identifier or type.** Publish builds without the PCH, so a header that `Source/Core/include/SmatchetPch.h` supplied under iter is missing from the TU. Add the include to the TU; don't add it to the PCH.
- **Unreal links stale or wrong-configuration libs after a preset switch.** Every preset packages into the same `SMATCHET_UNREAL_THIRDPARTY_DIR` (`lib/Win64/Debug` for Debug, `lib/Win64/Development` otherwise), so the libs Unreal sees come from whichever preset last ran `SmatchetPackageUnrealLibs_DX12`. Re-run that target from the preset you mean to ship. Clearing the output safely: [`build.md` § Clearing the Unreal build](agent-layer/docs/agent-rules/build.md).
- **clang-tidy findings that don't match the code you build.** The lint hook reads `build/ninja-iter-clang/compile_commands.json` when it exists, else `build/ninja-iter-msvc/` (for `tests/` sources, `ninja-test-clang`, else `ninja-test-msvc`). A stale clang tree feeds old defines and include paths; reconfigure it, or delete it so the hook falls back.
- **Breaks only with Lua or MCP off** (the light build, `ninja-publish-light-msvc`, the Lua-off variant). With `SMATCHET_WITH_LUA_AUTOMATION=OFF` the sol2 binding TUs (`AppController_LuaBindings*.cpp`) leave the build, and `AppController_LuaStubs.cpp` must stub every `AppController::` method they define; MCP-only code must sit under `#if SMATCHET_WITH_MCP`. Change bindings and stubs in the same diff. Reproduce the Lua-off variant through the preset: `cmake --preset ninja-iter-msvc -B build/lua-off-check -DSMATCHET_WITH_LUA_AUTOMATION=OFF`, then `cmake --build build/lua-off-check --target SmatchetStandalone` (the git-janitor skill's regression gate runs this feature-OFF variant after end-of-session merges). A bare `cmake -B … -G Ninja` picks the compiler on `PATH`; under plain `clang++` the MSVC-only `/ENTRY:mainCRTStartup` override in `Source/Standalone/CMakeLists.txt` is skipped and the `WIN32_EXECUTABLE` link fails on an undefined `WinMain`.
- **After a FetchContent pin bump: configure fails with `Requested git ref ... is not present locally`, or headers come from the old version** (missing macros, ABI mismatch). Local configures don't update an already-populated `.fetchcontent-src/<dep>-src`. Reconfigure once with `-DFETCHCONTENT_UPDATES_DISCONNECTED=OFF`, or `rm -rf .fetchcontent-src/<dep>-*` and reconfigure. Never `git pull` inside a dep tree; FetchContent owns it.
- **`.fetchcontent-src` subbuild "CMakeCache.txt directory … is different" errors, or `git-sh-setup: file not found` on a cold clone.** See [docs/guides/offline-builds.md § Troubleshooting](docs/guides/offline-builds.md#troubleshooting). The `git-sh-setup` failure is fixed at the source (`GIT_SUBMODULES ""` on every git declare, and `scripts/dev/with-msvc-env.sh` restores Git's exec path after vcvars64); it recurs only for a configure that bypasses the wrapper.
- **MSVC C4003 / C2589 on `std::numeric_limits<T>::max()` or `min()`.** The `windows.h` `max`/`min` macros reached a TU built without `NOMINMAX` (the Core-impl targets define it on Windows; other targets may not). Write `(std::numeric_limits<T>::max)()`.
- **A sanitized exe dies at launch with "DLL not found".** That is the ASan runtime (`clang_rt.asan_dynamic-<arch>.dll`) missing from `PATH`, not a build break. Launch from the MSVC environment (`scripts/dev/with-msvc-env.sh`); for a `clang-cl` build, add LLVM's `lib/clang/<major>/lib/windows` to `PATH`.
- **TSan on Windows.** MSVC has no TSan. Use `ninja-tsan-linux` on Linux, after installing the Clang TSan runtime ([`build.md` § TSan on Linux](agent-layer/docs/agent-rules/build.md)).
- **Lint `gcc` exits 1 with no output at all.** The lint toolchain (gcc, cppcheck, clang-tidy, clang-format) comes from MSYS2 UCRT64 at `C:\msys64\ucrt64\bin` (override: `SMATCHET_TOOLCHAIN_BIN`; lint only, MSYS2 builds are retired), and `cc1plus.exe` cannot load its DLLs unless that dir is on `PATH`. The lint hooks prepend it; a new wrapper that invokes the lint gcc must prepend it too.

## Rename scope

A rename or find-and-replace sweeps every tree below, and the old symbol must then return zero hits across all of them (rules: [`process-rules.md` § Mechanical edits](agent-layer/docs/agent-rules/process-rules.md)):

- **C++**: `Source/Core/{include,src}/`, `Source/Standalone/`, `Source/Plugins/`, `Source/UnrealPlugins/` (including the `*.Build.cs` files), `Source/Mobile/`, `tests/`.
- **Lua**: `scripts/*.lua` (`Automation.lua`, `SmatchetHooks.lua`, `RunLua.lua`, `LuaRecorderFuzz.lua`).
- **Docs**: `README.md`, `BUILD.md`, `AGENTS.md`, the subsystem leaf `Source/**/AGENTS.md`, `docs/guides/cli.md`, `docs/guides/lua.md`, `docs/guides/mcp.md`.
- **Localization**: `Source/Core/src/SmatchetLocalization.cpp` (`kEntries[]`) together with the `T(key, fallback)` call sites — change the English column and the call-site fallback in the same diff.
- **CMake**, only when the symbol is a target or option name: `CMakeLists.txt`, every component `Source/**/CMakeLists.txt` and `tests/**/CMakeLists.txt`, `cmake/*.cmake`, `CMakePresets.json`.

Then run one `git grep -nw '<old-symbol>' -- ':!*/ThirdParty/*'` over the whole tree to catch scripts, CI workflows and test fixtures the list misses.

<!-- ship-loop validation: 2026-05-27 -->

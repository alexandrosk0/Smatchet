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

The orchestrator + agent scripts (`scripts/dev/*.sh`, `agents/scripts/core/merge-watcher.py`, etc.) require a small set of CLI tools beyond the build toolchain:

| Tool | Used by | Install (Windows) |
|---|---|---|
| `gh` | merge-gates poller, PR ops, watcher daemon | `winget install GitHub.cli` |
| `jq` | **test harness only** — `merge_gates.bats` mocks `gh` via jq. The merge-gates poller + watcher daemon parse via gh's **bundled** jq (`gh api --jq`), so no standalone jq is needed at runtime. | `winget install jqlang.jq` |
| `python` 3.11+ | dev scripts (perf-compare, watcher CLI, etc.) | `winget install Python.Python.3.13` |
| `clang-format` / `clang-tidy` / `cppcheck` | lint hooks | Install via LLVM or Visual Studio individual components |
| `bats` | `tests/bats/*.bats` regression suite (merge-gates poller, etc.) | `npm i -g bats` |
| `shellcheck` | `agents/scripts/core/test-shell-lint.sh` (pre-push gate) | `npm install -g shellcheck` |

Verify the full set in one shot:

```bash
bash scripts/dev/check-required-tools.sh
```

Exits 0 when every required tool resolves; fails loudly with install hints for any missing tool. The orchestrator's `setup-harness.sh` runs this on every fresh clone.

Install whatever it reports missing:

```bash
bash scripts/dev/setup-env.sh
```

Resolves each missing tool to a package on the host's native manager (winget / MSYS2 `pacman` / `apt` / `brew`, `npm` for `bats` + `shellcheck`), installs it, then re-runs the probe as its verdict. `--dry-run` prints the plan only, `--yes` skips the prompt, `--list` dumps the tool → package map. Idempotent. It covers the dev-script CLI tools in the table above — **not** the MSVC / Clang build toolchain, which stays a manual install per the section above. Full flag reference: [`docs/harness/SETUP.md`](docs/harness/SETUP.md) § Installing the missing ones.

**Scheduled-Task / service environments** (notably `SmatchetMergeWatcher`) get a more minimal PATH than an interactive shell and may not resolve `gh` / `jq` / `bash` via bare-name lookup. The `merge-watcher-install-autostart.ps1` installer probes standard install locations + winget's Links dir + Git's `bin` dir explicitly so daemons keep working. See [`docs/plans/shipped/smatchet-merge-watcher.md`](docs/plans/shipped/smatchet-merge-watcher.md) § Daemon environment prerequisites for the gotchas.

## Supported Presets

**MSVC (primary):**

- `ninja-iter-msvc`: fast standalone iteration (`RelWithDebInfo`)
- `ninja-debug-msvc`: full standalone debug (`Debug`)
- `ninja-test-msvc`: doctest rig (`RelWithDebInfo`)
- `ninja-msvc-asan`: ASAN via MSVC `/fsanitize=address`

**Clang/LLVM (primary):**

- `ninja-iter-clang`: fast standalone iteration (`RelWithDebInfo`)
- `ninja-debug-clang`: full standalone debug (`Debug`)
- `ninja-test-clang`: doctest rig (`RelWithDebInfo`)
- `ninja-clang-asan`: ASAN + UBSan

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
Prompt is needed. Exit `78` means no usable Visual Studio install; it prints the winget commands.

Everything below is the **explicit-preset** form: what CI runs, and what to use when the MSVC
environment is already active.

**MSVC** — run from a VS Developer Command Prompt (or inside `scripts/dev/with-msvc-env.sh`):

```powershell
cmake --preset ninja-iter-msvc
cmake --build --preset ninja-iter-msvc
```

**Clang/LLVM** — ensure `clang-cl` is on PATH:

```powershell
cmake --preset ninja-iter-clang
cmake --build --preset ninja-iter-clang
```

Standalone debug:

```powershell
cmake --preset ninja-debug-msvc
cmake --build --preset ninja-debug-msvc
```

Unreal plugin iteration:

```powershell
cmake --preset ninja-iter-unreal-msvc
cmake --build --preset ninja-iter-unreal-msvc
```

Unreal plugin debug:

```powershell
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

```powershell
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

## Local Overrides

If you need local-only presets, create `CMakeUserPresets.json` in your checkout.
Shared presets should stay in `CMakePresets.json`; local machine-specific tweaks
belong in the user preset file.

## Unreal Packaging

`SmatchetPackageUnrealLibs_DX12` packages the DX12-compatible core library,
ImGui, and public headers into
`Source/UnrealPlugins/SmatchetImGuiPlugin/ThirdParty/Smatchet` for Unreal Build Tool
consumption.

<!-- ship-loop validation: 2026-05-27 -->

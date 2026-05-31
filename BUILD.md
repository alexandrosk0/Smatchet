# Build

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
# Visual Studio 2022 Build Tools (or Community/Professional/Enterprise)
winget install Microsoft.VisualStudio.2022.BuildTools
```

Or Clang/LLVM:

```powershell
winget install LLVM.LLVM
```

### Dev-script CLI tools

The orchestrator + agent scripts (`scripts/dev/*.sh`, `scripts/dev/merge-watcher.py`, etc.) require a small set of CLI tools beyond the build toolchain:

| Tool | Used by | Install (Windows) |
|---|---|---|
| `gh` | merge-gates poller, PR ops, watcher daemon | `winget install GitHub.cli` |
| `jq` | **test harness only** — `merge_gates.bats` mocks `gh` via jq. The merge-gates poller + watcher daemon parse via gh's **bundled** jq (`gh api --jq`), so no standalone jq is needed at runtime. | `winget install jqlang.jq` |
| `python` 3.11+ | dev scripts (perf-compare, watcher CLI, etc.) | `winget install Python.Python.3.13` |
| `clang-format` / `clang-tidy` / `cppcheck` | lint hooks | Install via LLVM or Visual Studio individual components |
| `bats` | `tests/bats/*.bats` regression suite (merge-gates poller, etc.) | `npm i -g bats` |
| `shellcheck` | `scripts/dev/test-shell-lint.sh` (pre-push gate) | `npm install -g shellcheck` |

Verify the full set in one shot:

```bash
bash scripts/dev/check-required-tools.sh
```

Exits 0 when every required tool resolves; fails loudly with install hints for any missing tool. The orchestrator's `setup-harness.sh` runs this on every fresh clone.

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

**MSVC** — run from a VS Developer Command Prompt:

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

Wrapper shortcuts are still available:

```powershell
.\scripts\dev\local\build_and_run.ps1
.\scripts\dev\local\build_and_run.ps1 -Preset ninja-iter-msvc
.\scripts\dev\local\build_and_run.ps1 -BuildOnly
.\scripts\dev\local\build_and_run.ps1 -RunOnly -StandaloneArgs '--config','foo'
```

### MSVC from bash (Git Bash)

**MSYS2 is not required or recommended for building Smatchet.** The `ninja-iter-msys2`
preset is retired. Use `ninja-iter-msvc` (MSVC) or `ninja-iter-clang` (clang-cl) instead.

`build_and_run.ps1` handles `vcvars64` env for PowerShell sessions. The
bash-side equivalent is `scripts/dev/with-msvc-env.sh` — a wrapper that picks
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

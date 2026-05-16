# Build

Smatchet uses a small shared CMake preset surface centered on MSYS2 UCRT64.
All supported shared presets live in `CMakePresets.json` so Cursor and CMake
Tools see the same options.

## Prerequisites

- CMake 3.24 or newer
- Git
- MSYS2 UCRT64
- Ninja available in the shell you use for builds

Install the recommended toolchain:

```powershell
winget install MSYS2.MSYS2
```

Then, in an MSYS2 terminal:

```bash
pacman -S mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-lld
```

## Supported Presets

- `ninja-iter-msys2`: fast standalone iteration (`RelWithDebInfo`)
- `ninja-debug-msys2`: full standalone debug (`Debug`)
- `ninja-iter-unreal-msys2`: fast Unreal plugin iteration (`RelWithDebInfo`)
- `ninja-debug-unreal-msys2`: full Unreal-specific debug (`Debug`)
- `ninja-publish-msys2`: LTO publish build for standalone plus Unreal packaging (`Release`)
- `ninja-release`: supported legacy standalone release preset using `gcc`/`g++` from the current `PATH`

## Common Workflows

Standalone iteration:

```powershell
cmake --preset ninja-iter-msys2
cmake --build --preset ninja-iter-msys2
```

Standalone debug:

```powershell
cmake --preset ninja-debug-msys2
cmake --build --preset ninja-debug-msys2
```

Unreal plugin iteration:

```powershell
cmake --preset ninja-iter-unreal-msys2
cmake --build --preset ninja-iter-unreal-msys2
```

Unreal plugin debug:

```powershell
cmake --preset ninja-debug-unreal-msys2
cmake --build --preset ninja-debug-unreal-msys2
```

Publish with LTO:

```powershell
cmake --preset ninja-publish-msys2
cmake --build --preset ninja-publish-msys2
```

Wrapper shortcuts are still available:

```powershell
.\scripts\dev\build_and_run.ps1
.\scripts\dev\build_and_run.ps1 -Preset ninja-iter-msys2
.\scripts\dev\build_and_run.ps1 -BuildOnly
.\scripts\dev\build_and_run.ps1 -RunOnly -StandaloneArgs '--config','foo'
```

## First-time verification

Run on a fresh clone to confirm the toolchain is wired correctly. Expect
**~5 minutes** the very first time (FetchContent downloads + builds
nlohmann/json, cpr, SQLiteCpp, cpp-httplib, md4c, ImGui, GLFW, Lua, sol2, and
ghc::filesystem into `build/<preset>/_deps/`); subsequent configures complete
in seconds.

```powershell
# 1. Toolchain pre-flight (instant). Prints [PASS] / [FAIL] / [WARN] per check.
.\scripts\dev\doctor.ps1

# 2. Configure + build the test rig.
cmake --preset ninja-test-msys2
cmake --build --preset ninja-test-msys2 --target SmatchetTests

# 3. Run the test rig.
ctest --test-dir build/ninja-test-msys2 --output-on-failure

# 4. Build the standalone exe.
cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone

# 5. Optional: static-analysis pass.
python .\scripts\dev\run_cppcheck.py
```

If step 1 prints `Doctor: RED`, fix the listed prerequisites before
continuing -- CMake errors on a missing MSYS2 / lld / Python install are
much harder to debug than the doctor's install hints. On MSYS2 / Linux,
`bash scripts/dev/doctor.sh` is the equivalent.

## Local Overrides

If you need local-only presets, create `CMakeUserPresets.json` in your checkout.
Shared presets should stay in `CMakePresets.json`; local machine-specific tweaks
belong in the user preset file.

## Unreal Packaging

`SmatchetPackageUnrealLibs_DX12` packages the DX12-compatible core library,
ImGui, and public headers into
`UnrealPlugins/SmatchetImGuiPlugin/ThirdParty/Smatchet` for Unreal Build Tool
consumption.

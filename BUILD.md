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
.\scripts\build_and_run.ps1
.\scripts\build_and_run.ps1 -Preset ninja-iter-msys2
.\scripts\build_and_run.ps1 -BuildOnly
.\scripts\build_and_run.ps1 -RunOnly -StandaloneArgs '--config','foo'
```

## Local Overrides

If you need local-only presets, create `CMakeUserPresets.json` in your checkout.
Shared presets should stay in `CMakePresets.json`; local machine-specific tweaks
belong in the user preset file.

## Unreal Packaging

`SmatchetPackageUnrealLibs_DX12` packages the DX12-compatible core library,
ImGui, and public headers into
`UnrealPlugins/SmatchetImGuiPlugin/ThirdParty/Smatchet` for Unreal Build Tool
consumption.

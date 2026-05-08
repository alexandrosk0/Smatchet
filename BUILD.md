# Build

Smatchet uses CMake presets. The default presets are portable: they let CMake
discover the compiler from the current shell instead of assuming one developer
machine's IDE install path.

## Prerequisites

- CMake 3.24 or newer
- A C++ compiler supported by CMake
- Ninja for `ninja-*` presets
- Visual Studio 2022 for `vs-*` presets

## Portable Ninja

Use this from a shell where your compiler and Ninja are on `PATH`.

```powershell
cmake --preset ninja-debug
cmake --build --preset ninja-debug --target SmatchetStandalone
```

Or use the wrapper:

```powershell
.\scripts\build_and_run.ps1
.\scripts\build_and_run.ps1 -Preset ninja-release
.\scripts\build_and_run.ps1 -BuildOnly                  # configure + build, do not launch
.\scripts\build_and_run.ps1 -RunOnly -StandaloneArgs '--config','foo'   # launch a previously-built exe
```

`build_and_run.ps1` is a thin shim over `build_standalone.ps1` and
`run_standalone.ps1`; call those directly when you want just one stage in
a pipeline.

Per-preset shorthands are also available for muscle memory and IDE run
configurations:

```powershell
.\scripts\build_and_run_ninja_debug.ps1
.\scripts\build_and_run_ninja_release.ps1
.\scripts\build_and_run_vs_debug.ps1
.\scripts\build_and_run_vs_release.ps1
```

Each is a one-line shim around `build_and_run.ps1 -Preset <name>`.

## Clang

Use these from a shell where `clang`, `clang++`, and Ninja are on `PATH`.

```powershell
cmake --preset ninja-clang-debug
cmake --build --preset ninja-clang-debug --target SmatchetStandalone
```

```powershell
cmake --preset ninja-clang-release
cmake --build --preset ninja-clang-release --target SmatchetStandalone
```

## MSVC

From a Visual Studio Developer PowerShell or Developer Command Prompt:

```powershell
cmake --preset ninja-msvc-debug
cmake --build --preset ninja-msvc-debug --target SmatchetStandalone
```

```powershell
cmake --preset ninja-msvc-release
cmake --build --preset ninja-msvc-release --target SmatchetStandalone
```

The Visual Studio generator is also available:

```powershell
cmake --preset vs-debug
cmake --build --preset vs-debug --target SmatchetStandalone
```

```powershell
cmake --preset vs-release
cmake --build --preset vs-release --target SmatchetStandalone
```

## Local Compiler Paths

Do not add machine-specific compiler paths to `CMakePresets.json`. If you need
to pin a local CLion MinGW or custom toolchain path, put it in
`CMakeUserPresets.json`, which is intentionally local to your checkout:

```json
{
  "version": 3,
  "configurePresets": [
    {
      "name": "ninja-debug-local-mingw",
      "inherits": "ninja-debug",
      "binaryDir": "${sourceDir}/build/ninja-debug-local-mingw",
      "cacheVariables": {
        "CMAKE_C_COMPILER": "C:/Path/To/Your/MinGW/bin/gcc.exe",
        "CMAKE_CXX_COMPILER": "C:/Path/To/Your/MinGW/bin/g++.exe"
      }
    }
  ],
  "buildPresets": [
    {
      "name": "ninja-debug-local-mingw",
      "configurePreset": "ninja-debug-local-mingw"
    }
  ]
}
```

A configure preset on its own only enables `cmake --preset <name>`. Without
a matching `buildPresets` entry, `cmake --build --preset <name>` (and any
wrapper that calls it, such as `build_and_run.ps1 -Preset <name>`) will fail
with `No such build preset`.

## Unreal Packaging

The Ninja Unreal presets package with whatever compiler is active on `PATH`.
Use them only from the intended compiler shell, and treat the output as tied to
that toolchain. For Unreal Build Tool consumption, prefer the Visual Studio
preset below so the packaged libraries are MSVC ABI-compatible.

Toolchain-from-`PATH` Ninja packaging:

```powershell
cmake --preset ninja-unreal-dx12
cmake --build --preset ninja-unreal-dx12
```

```powershell
cmake --preset ninja-unreal-dx12-release
cmake --build --preset ninja-unreal-dx12-release
```

MSVC ABI-compatible release packaging:

```powershell
cmake --preset vs-unreal-dx12-release
cmake --build --preset vs-unreal-dx12-release
```

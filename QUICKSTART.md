# Quickstart

Clone → install prerequisites → one command → the app is on screen. Windows, ordinary PowerShell —
no Visual Studio Developer Command Prompt, no MSYS2.

## 1. Clone

```powershell
git clone https://github.com/alexandrosk0/Smatchet.git
cd Smatchet
```

## 2. Prerequisites

```powershell
winget install Kitware.CMake      # CMake 3.24+
winget install Ninja-build.Ninja  # Ninja

# MSVC (provides cl.exe). The bare Build Tools package installs only the core
# installer — the --override passes the C++ workload through to it, which is
# what actually brings cl.exe and the Windows SDK.
winget install Microsoft.VisualStudio.2022.BuildTools --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

Clang/LLVM (`winget install LLVM.LLVM`) works as an alternative compiler, but the Windows SDK still
comes from a Visual Studio install — keep the Build Tools either way. Open a **new** PowerShell after
installing so `PATH` picks the tools up.

## 3. Build and run

```bash
./build.sh
```

That's it. `build.sh` picks a preset (`cl.exe` on `PATH` → `ninja-iter-msvc`, else `clang-cl.exe` →
`ninja-iter-clang`), prints which one and why, imports the MSVC environment if it isn't already
active, builds, and launches the app. Run it from Git Bash (shipped with Git for Windows).

Useful variants:

```bash
./build.sh --build-only                            # build, don't launch
./build.sh --preset ninja-debug-msvc --build-only   # explicit preset
./build.sh --run-only -- --version                  # run what's already built
```

**Expect ~5 minutes on the first build** — CMake `FetchContent` downloads and builds every
third-party dependency (ImGui, GLFW, SQLiteCpp, cpr, nlohmann/json, Lua, sol2, …). Later builds take
seconds.

## 4. First launch

The app opens on the **Active Project** grid with no tracker configured. Open **Preferences →
Tracker**, pick your backend (Jira, Plane, GitHub, or Linear), fill in the URL and credentials, and
press **Test connection** — it verifies the credentials before saving. Once the test passes, **Save &
Sync** pulls your issues in.

## Trouble?

| Symptom | Fix |
|---|---|
| `build.sh` exits `78`, "no usable MSVC toolchain" | Install the Build Tools (step 2); it prints the exact winget command. |
| `ninja not found on PATH` | `winget install Ninja-build.Ninja`, then reopen the shell. |
| `cl.exe is not on PATH` from `build-standalone.sh` | You called the inner script directly — use `./build.sh` instead, it imports the environment. |
| Anything else | `bash scripts/dev/doctor.sh` prints `[PASS]`/`[FAIL]` per prerequisite with install hints. |

Full build reference — every preset, Clang, Unreal/DX12, tests, sanitizers, bash flows:
[BUILD.md](BUILD.md).

# Smatchet

[![Build and test](https://github.com/alexandrosk0/Smatchet/actions/workflows/build-and-test.yml/badge.svg?branch=develop)](https://github.com/alexandrosk0/Smatchet/actions/workflows/build-and-test.yml)

Smatchet is a high-performance, engine-agnostic productivity tool and issue-tracking client. It provides a unified interface for project management, Perforce source control analysis, and AI assistance — all built using C++14 and Dear ImGui. Smatchet supports **multiple tracker backends** (Jira, Plane.so, GitHub Issues, and Linear) through a clean, backend-agnostic architecture (`ITrackerBackend`), and can run as a standalone desktop application or embedded directly into Unreal Engine.

## Download & Install

Prebuilt binaries are published on the [GitHub Releases page](https://github.com/alexandrosk0/Smatchet/releases). Each release carries these assets (`<tag>` is the version, e.g. `v0.6.7`):

| Asset | What it is |
|---|---|
| `Smatchet-<tag>-windows-setup.exe` | **Recommended.** Windows installer — per-user install (no admin rights needed) into `%LOCALAPPDATA%\Programs\Smatchet`, with uninstall support and in-app update checks. |
| `Smatchet-<tag>-windows-portable.zip` | Portable build — unzip anywhere and run `Smatchet.exe`. No installation, no registry changes. |
| `Smatchet-<tag>-windows-light-portable.zip` | Portable "Light" build with automation-facing features (CLI command surface, MCP server, Lua) disabled. |
| `Smatchet-<tag>-unreal-plugin.zip` | The Unreal Engine editor plugin — see the [plugin install guide](scripts/publish/INSTALL_UNREAL_PLUGIN.md). |
| `Smatchet-<tag>-source.zip` | Source archive of the tagged revision. |

**Quickstart:** install (or unzip) and launch, then open **Settings → Preferences → Tracker** to pick your backend (Jira, Plane.so, GitHub Issues, or Linear) and enter its URL and API credentials. Linear is the exception to "URL and credentials": its **Base URL** is prefilled with the GraphQL endpoint `https://api.linear.app/graphql`, so you supply a personal **API Key** (Linear → Settings → API) plus the **Team Key** that prefixes your issues (e.g. `ENG`); the **Team** UUID and **Workspace URL** fields are optional (the team UUID is resolved from the Team Key when left empty). Your data stays local: issues and field catalogs are cached in a local SQLite database, and on Windows API tokens are stored DPAPI-encrypted in your user profile.

### Platform support

- **Windows 10/11 x64** is the supported, shipped platform for the standalone app (installer + portable ZIP). Windows-on-ARM builds are supported by the release tooling (`-Arch arm64`) but not routinely published.
- **Unreal Engine plugin**: Windows editor (DX12).
- **Linux / macOS**: not shipped. The engine-agnostic core compiles under Linux clang as a portability gate, but there is currently no supported desktop build or packaging for either platform. (Note: on non-Windows builds there is no OS-backed secret store — API tokens would be stored in a plain-text config file.)
- **Android**: an experimental mobile core is built and tested in CI but is not part of any release.

## Features

- **Multi-Backend Tracker Support**: Seamlessly switch between **Jira** (Atlassian Cloud), **Plane.so**, **GitHub Issues**, and **Linear** (GraphQL, personal API key) from the Preferences panel. Each backend is a concrete implementation of the backend-agnostic `ITrackerBackend` interface (`JiraClient`, `PlaneClient`, `GitHubClient`, `LinearClient`); views, field catalogs, and issue data are kept separate per backend. Linear implements five of the six backend role interfaces (read, connectivity, field catalog, mutations, collaboration) but not `ITrackerActivity`, so the user-activity feed is unavailable on Linear; its collaboration surface is posting a plain-text comment only — comment *reading* is not implemented, unlike the other three backends ([plan](docs/plans/linear-tracker-backend.md) § Non-goals).
- **Full Issue Management**: Search, view, create, and edit issues. Supports offline drafting, custom fields, inline field editing, bulk import/export, and per-backend "new issue inherit" field configuration.
- **Perforce Annotate**: Native P4 integration for fast file annotation directly within the UI, complete with syntax highlighting.
- **Fast Local Caching**: Uses SQLite to cache field catalogs, user metadata, and recent issues locally for near-instant load times and offline capabilities.
- **Engine-Agnostic UI**: Built on Dear ImGui. The core library (`Source/Core`) contains no direct graphics API dependencies.
- **UI Localization**: Switch the app-owned Dear ImGui UI between built-in English and French, with optional local JSON overrides for teams that want custom wording.
- **Dual Deployment**:
  - **Standalone App**: A native desktop application leveraging GLFW and OpenGL3.
  - **Unreal Engine Plugin**: Direct DX12 integration allowing Smatchet to run seamlessly inside the Unreal Engine editor.
- **Views System**: Backend-aware views editor for managing saved queries, column layouts, sort orders, and column widths — stored per-tracker so switching backends preserves each backend's view set. A modern two-pane layout (sidebar of saved views on the left, tabbed editor for Filter / Fields / Columns / Sort on the right) supports drag-and-drop reorder, `Alt+↑` / `Alt+↓` keyboard reorder, inline rename, and per-view duplicate. Column reorder, resize, and sort directly in the grid surface an *Unsaved layout changes* strip with `Save` / `Save as new...` / `Discard` so layout edits are gated behind an explicit commit instead of silently autosaving. Shortcuts: `Ctrl+Enter` applies the active view, `Ctrl+N` creates a new one.
- **Optional Lua Automation**: Embeds a Lua 5.3.6 runtime using `sol2`, enabling custom scripts, automation hooks, custom ImGui windows, and a dedicated in-app Lua console.
- **Unified Command System**: A single registry of 56+ named commands feeds four frontends simultaneously — a shell CLI (`Smatchet.exe cmd <name>`), an in-app Command Palette (Ctrl+Shift+P), MCP `tools/call`, and `commands.invoke()` from Lua. Adding a new command means one `RegisterCommand({...})` call and it appears everywhere automatically. See [CLI Guide](CLI_GUIDE.md).
- **Optional AI & MCP Support**: Includes an AI assistant controller and an integrated MCP (Model Context Protocol) server plugin to bridge into modern AI workflows.
- **AI Assistant Side Panel** (`Ctrl+Shift+A`): Right-anchored streaming chat panel with provider-pluggable backends — OpenAI (Chat Completions SSE), Anthropic (Native Messages API), Ollama (OpenAI-compatible SSE *and* native NDJSON), and DeepSeek (OpenAI-protocol-compatible). System prompt is layered from `agents.md` files (repo + user + active project) plus auto-context blocks (active ticket, multi-selection, visible grid rows, active view, audit trail) that the user toggles per-turn. Per-turn cancel, network-usage tracker integration, and DPAPI-protected API key storage. Lua-callable via `ai.add_context`, `ai.clear_context`, `ai.prompt`.
- **Push-to-Talk Dictation** (optional, Windows): A Whisper-based dictation plugin captures audio on a global hotkey and transcribes it via a local Whisper model or a cloud Whisper API, with a consent gate and silence trimming. Built when `SMATCHET_WITH_WHISPER=ON`.

## Documentation

- [Build Guide](BUILD.md): Supported CMake presets, prerequisites, local compiler paths, and wrapper scripts.
- [CLI Guide](CLI_GUIDE.md): Command-line interface for all 56+ registered commands — discovery, output flags, config overrides, environment variables, and composability examples. Suitable for shell scripts and AI agents.
- [Lua Scripting Guide](LUA_GUIDE.md): Complete reference for automating workflows and customizing the UI with Lua.
- [MCP (Model Context Protocol) Guide](MCP_GUIDE.md): How to use Smatchet as an MCP server for AI agents.
- [Privacy Policy](PRIVACY.md): What data Smatchet stores locally, which services it contacts, and what a submitted bug report contains.
- [Windows Signing Guide](scripts/publish/SIGNING.md): How to sign the standalone app and installer during release packaging.
- [Installer Smoke Test Guide](scripts/publish/INSTALLER_SMOKE_TEST.md): Repeatable release validation for installer, portable ZIP, Unreal plugin ZIP, and Fab bundle.

### Contributor / agent docs

- [AGENTS.md](AGENTS.md): The canonical contributor + agentic-harness rulebook — project rules, build invariants (C++14, `LOG_*` logging, RAII), the strict-zone lint contract, ship-loop, and delegation map. Start here before changing code.
- [AI_POLICY.md](AI_POLICY.md): The human-authority charter governing autonomous agent work (loop modes, escalation invariants).
- [docs/STRUCTURE.md](docs/STRUCTURE.md): The normative doc/agentic taxonomy and the portable/project boundary; [CONTEXT-MAP.md](CONTEXT-MAP.md) registers per-subsystem leaf docs.
- [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md): Licenses of bundled third-party dependencies.

## Localization

Smatchet supports lightweight localization for app-owned UI text. The first built-in languages are English (`en-US`) and French (`fr-FR`); aliases `en` and `fr` are normalized to those full locale codes.

Change the UI language from **Settings -> Preferences -> Appearance -> Language**. The selected language is saved as `ui_language` in `smatchet_config.json`, so it persists across restarts.

Teams can override built-in strings without rebuilding by placing JSON files next to the executable/config base under `Locales/<locale>.json`, for example `Locales/fr-FR.json`:

```json
{
    "locale": "fr-FR",
    "strings": {
        "common.save": "Enregistrer"
    }
}
```

Missing override keys fall back to the built-in strings, and missing translations fall back to English. Localization covers app-owned UI labels, buttons, menus, tooltips, and status text; user data and tracker-provided values such as issue fields, statuses, saved view names, file paths, and backend error details are shown as-is.

## Building Smatchet

Smatchet's build system uses CMake and is designed to require **zero manual dependency downloads**. All third-party libraries (ImGui, SQLiteCpp, cpr, nlohmann/json, etc.) are fetched and built automatically via `FetchContent`.

### Prerequisites

- CMake 3.24 or higher
- Ninja build system
- Git
- One of:
  - **MSVC** — Visual Studio 2022 (Community or Build Tools) installed. The `*-msvc` presets need the MSVC toolchain (`cl.exe`) on `PATH`: run from a Visual Studio Developer Command Prompt/PowerShell, or activate the environment first (the bash flows use `scripts/dev/with-msvc-env.sh`).
  - **Clang/LLVM** — `winget install LLVM.LLVM` on Windows. Uses `clang-cl` for MSVC ABI compatibility.

### Supported Presets

**MSVC (primary):**

- `ninja-iter-msvc`: fast standalone iteration (`RelWithDebInfo`)
- `ninja-debug-msvc`: full standalone debug (`Debug`)
- `ninja-test-msvc`: doctest rig (`RelWithDebInfo`)
- `ninja-msvc-asan`: ASAN via `/fsanitize=address`

**Clang/LLVM (primary):**

- `ninja-iter-clang`: fast standalone iteration (`RelWithDebInfo`)
- `ninja-debug-clang`: full standalone debug (`Debug`)
- `ninja-test-clang`: doctest rig (`RelWithDebInfo`)
- `ninja-clang-asan`: ASAN + UBSan


### Build Workflows

**MSVC** — run from a VS Developer Command Prompt:

```powershell
cmake --preset ninja-iter-msvc
cmake --build --preset ninja-iter-msvc
```

**One-command build + run** — `build_and_run.ps1` (under `scripts/dev/local/`) drives `cmake --preset`
+ `cmake --build` (and optionally runs/verifies) in one step. Run it from a VS Developer Command
Prompt/PowerShell — like the raw presets, the `*-msvc` build needs `cl.exe` on `PATH`:

```powershell
scripts/dev/local/build_and_run.ps1 -Preset ninja-iter-msvc
```

> **MSYS2 is not required and not supported for building Smatchet.** The `ninja-iter-msys2` /
> `*-msys2` presets are **retired** — use `ninja-iter-msvc` (MSVC) or `ninja-iter-clang` (clang-cl).
> The repo-owned build scripts fail fast with that hint if a `*-msys2` preset is passed.

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

Tests:

```powershell
cmake --preset ninja-test-msvc
cmake --build --preset ninja-test-msvc
ctest --test-dir build/ninja-test-msvc --output-on-failure
```

Dual-target verification (Standalone + DX12):

```powershell
cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12
```

### Configuration Options

Smatchet exposes several CMake options to customize the build:

| Option | Default | Description |
| :--- | :--- | :--- |
| `SMATCHET_WITH_LUA_AUTOMATION` | `ON` | Builds with the Lua console plugin and `sol2` bindings for field automation. |
| `SMATCHET_WITH_MCP` | `ON` | Builds the Model Context Protocol (MCP) server plugin. |
| `SMATCHET_WITH_AI` | `ON` | Builds the AI assistant side panel (provider-pluggable `IAiClient`). |
| `SMATCHET_WITH_WHISPER` | `ON` | Builds the Whisper push-to-talk dictation plugin (Windows). |
| `SMATCHET_ENABLE_STRICT_WARNINGS`| `ON`  | Applies strict compiler warnings (`/W4` or `-Wall -Wextra`) to first-party code. |

### Unreal Engine Plugin

When built on Windows, Smatchet provides a target (`SmatchetPackageUnrealLibs_DX12`) that automatically packages the DX12-compatible core library, ImGui, and public headers into the `Source/UnrealPlugins/SmatchetImGuiPlugin/ThirdParty/Smatchet` layout for immediate consumption by the Unreal Build Tool.

## Architecture

Smatchet is split into an engine-agnostic core library, a thin standalone host, optional plugins, and an Unreal bridge. The core compiles for **two render targets** from the same sources — `SmatchetStandalone` (GLFW + OpenGL3) and `SmatchetCore_DX12` (DX12, for embedding in Unreal) — so core headers carry no direct graphics-API dependencies.

* **`Source/Core/`**: The heart of the application — the backend-agnostic tracker interface (`ITrackerBackend`), concrete backends (`JiraClient`, `PlaneClient`, `GitHubClient`, `LinearClient`), local cache/sync, the unified command registry, and all ImGui UI definitions. Source is organized by subsystem under `Source/Core/src/<ctx>/`:
  * **`Tracker/`** — the `ITrackerBackend` interface, concrete backends, field catalog, and HTTP plumbing.
  * **`Sync/`** — offline queue replay, ticket sync, and the backend audit trail.
  * **`Persistence/`** — the SQLite `LocalCacheManager` (field catalogs, user metadata, recent issues).
  * **`Config/`** — settings, preferences, and `smatchet_config.json` handling.
  * **`Commands/`** — the unified command registry feeding CLI / Palette / MCP / Lua / scenarios.
  * **`Ui/`** — the ImGui panels, grid, views editor, and theme.
  * **`Vcs/`** — Perforce annotate/describe integration.
  * Plus supporting subsystems (`Diagnostics/`, `Imaging/`, `Privacy/`).
* **`Source/Standalone/`**: The main entry point, window management, and GLFW/OpenGL bootstrapping for the standalone application.
* **`Source/Plugins/`**: Optional, CMake-gated plugin modules — **`LuaConsole/`** (Lua 5.3 + `sol2`), **`Mcp/`** (Model Context Protocol server), and **`Whisper/`** (push-to-talk dictation). The AI assistant lives in the core (`SMATCHET_WITH_AI`).
* **`Source/UnrealPlugins/`**: The Unreal Engine plugin (`SmatchetImGuiPlugin`) and its DX12 render backend.
* **`ThirdParty/`**: Holds custom fixes or scripts for external dependencies.
* **`scripts/`**: Runtime Lua/art assets plus tooling subfolders. `scripts/` root contains the shipped Lua runtime files (`Automation.lua`, `SmatchetHooks.lua`, `RunLua.lua`) and bundled art copied next to the standalone exe as `Scripts/`. Use `scripts/dev/` for local build/dev helpers and `scripts/publish/` for release, signing, installer, and smoke-test tooling.
* **`cmake/`**: Additional CMake helper modules.

For deeper architecture and contribution rules, see [AGENTS.md](AGENTS.md) (the rulebook) and the per-subsystem leaf docs registered in [CONTEXT-MAP.md](CONTEXT-MAP.md).

## License

This project is licensed under the [MIT License](LICENSE).

## Privacy

Smatchet has no analytics and no background telemetry; your data stays on your
machine unless you connect a service yourself. See the [Privacy Policy](PRIVACY.md).

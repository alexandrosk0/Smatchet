# Smatchet

[![Build and test](https://github.com/alexandrosk0/Smatchet/actions/workflows/build-and-test.yml/badge.svg?branch=develop)](https://github.com/alexandrosk0/Smatchet/actions/workflows/build-and-test.yml)

Smatchet is a high-performance, engine-agnostic productivity tool and issue-tracking client. It provides a unified interface for project management, Perforce source control analysis, and AI assistance — all built using C++14 and Dear ImGui. Smatchet supports **multiple tracker backends** (Jira and Plane.so) through a clean, backend-agnostic architecture, and can run as a standalone desktop application or embedded directly into Unreal Engine.

## Features

- **Multi-Backend Tracker Support**: Seamlessly switch between **Jira** (Atlassian Cloud) and **Plane.so** from the Preferences panel. Views, field catalogs, and issue data are kept separate per backend.
- **Full Issue Management**: Search, view, create, and edit issues. Supports offline drafting, custom fields, inline field editing, bulk import/export, and per-backend "new issue inherit" field configuration.
- **Perforce Annotate**: Native P4 integration for fast file annotation directly within the UI, complete with syntax highlighting.
- **Fast Local Caching**: Uses SQLite to cache field catalogs, user metadata, and recent issues locally for near-instant load times and offline capabilities.
- **Engine-Agnostic UI**: Built on Dear ImGui. The core library (`Source_Core`) contains no direct graphics API dependencies.
- **UI Localization**: Switch the app-owned Dear ImGui UI between built-in English and French, with optional local JSON overrides for teams that want custom wording.
- **Dual Deployment**:
  - **Standalone App**: A native desktop application leveraging GLFW and OpenGL3.
  - **Unreal Engine Plugin**: Direct DX12 integration allowing Smatchet to run seamlessly inside the Unreal Engine editor.
- **Views System**: Backend-aware views editor for managing saved queries, column layouts, sort orders, and column widths — stored per-tracker so switching backends preserves each backend's view set. A modern two-pane layout (sidebar of saved views on the left, tabbed editor for Filter / Fields / Columns / Sort on the right) supports drag-and-drop reorder, `Alt+↑` / `Alt+↓` keyboard reorder, inline rename, and per-view duplicate. Column reorder, resize, and sort directly in the grid surface an *Unsaved layout changes* strip with `Save` / `Save as new...` / `Discard` so layout edits are gated behind an explicit commit instead of silently autosaving. Shortcuts: `Ctrl+Enter` applies the active view, `Ctrl+N` creates a new one.
- **Optional Lua Automation**: Embeds a Lua 5.3 runtime using `sol2`, enabling custom scripts, automation hooks, custom ImGui windows, and a dedicated in-app Lua console.
- **Unified Command System**: A single registry of 56+ named commands feeds four frontends simultaneously — a shell CLI (`SmatchetStandalone.exe cmd <name>`), an in-app Command Palette (Ctrl+Shift+P), MCP `tools/call`, and `commands.invoke()` from Lua. Adding a new command means one `RegisterCommand({...})` call and it appears everywhere automatically. See [CLI Guide](CLI_GUIDE.md).
- **Optional AI & MCP Support**: Includes an AI assistant controller and an integrated MCP (Model Context Protocol) server plugin to bridge into modern AI workflows.
- **AI Assistant Side Panel** (`Ctrl+Shift+A`): Right-anchored streaming chat panel with provider-pluggable backends — OpenAI (Chat Completions SSE), Anthropic (Native Messages API), Ollama (OpenAI-compatible SSE *and* native NDJSON). System prompt is layered from `agents.md` files (repo + user + active project) plus auto-context blocks (active ticket, multi-selection, visible grid rows, active view, audit trail) that the user toggles per-turn. Per-turn cancel, network-usage tracker integration, and DPAPI-protected API key storage. Lua-callable via `ai.add_context`, `ai.clear_context`, `ai.prompt`.

## Documentation

- [Build Guide](BUILD.md): Supported CMake presets, prerequisites, local compiler paths, and wrapper scripts.
- [CLI Guide](CLI_GUIDE.md): Command-line interface for all 56+ registered commands — discovery, output flags, config overrides, environment variables, and composability examples. Suitable for shell scripts and AI agents.
- [Lua Scripting Guide](LUA_GUIDE.md): Complete reference for automating workflows and customizing the UI with Lua.
- [MCP (Model Context Protocol) Guide](MCP_GUIDE.md): How to use Smatchet as an MCP server for AI agents.
- [Windows Signing Guide](scripts/publish/SIGNING.md): How to sign the standalone app and installer during release packaging.
- [Installer Smoke Test Guide](scripts/publish/INSTALLER_SMOKE_TEST.md): Repeatable release validation for installer, portable ZIP, Unreal plugin ZIP, and Fab bundle.

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

MSYS2 for iteration (lld), publish explicitly uses BFD. Publish LTO and fast dev link.

### Recommended Developer Path

For developers who want both fast iteration and LTO publish builds, use MSYS2 UCRT64:

```powershell
winget install MSYS2.MSYS2
```

Then, in an MSYS2 terminal:

```bash
pacman -S mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-lld
```

### Prerequisites

- CMake 3.24 or higher
- Git
- MSYS2 UCRT64 with:
  - `mingw-w64-ucrt-x86_64-toolchain`
  - `mingw-w64-ucrt-x86_64-lld`

### Supported Presets

The supported shared presets are:

- `ninja-iter-msys2`: fast standalone iteration (`RelWithDebInfo`)
- `ninja-debug-msys2`: full standalone debug (`Debug`)
- `ninja-iter-unreal-msys2`: fast Unreal plugin iteration (`RelWithDebInfo`)
- `ninja-debug-unreal-msys2`: full Unreal-specific debug (`Debug`)
- `ninja-publish-msys2`: LTO publish build for standalone plus Unreal packaging (`Release`)
- `ninja-release`: supported legacy standalone release preset using `gcc`/`g++` from the current `PATH`

### Build Workflows

Standalone iteration:

```bash
cmake --preset ninja-iter-msys2
cmake --build --preset ninja-iter-msys2
```

Standalone debug:

```bash
cmake --preset ninja-debug-msys2
cmake --build --preset ninja-debug-msys2
```

Unreal plugin iteration:

```bash
cmake --preset ninja-iter-unreal-msys2
cmake --build --preset ninja-iter-unreal-msys2
```

Unreal plugin debug:

```bash
cmake --preset ninja-debug-unreal-msys2
cmake --build --preset ninja-debug-unreal-msys2
```

Publish with LTO:

```bash
cmake --preset ninja-publish-msys2
cmake --build --preset ninja-publish-msys2
```

### Configuration Options

Smatchet exposes several CMake options to customize the build:

| Option | Default | Description |
| :--- | :--- | :--- |
| `SMATCHET_WITH_LUA_AUTOMATION` | `ON` | Builds with the Lua console plugin and `sol2` bindings for field automation. |
| `SMATCHET_WITH_MCP` | `ON` | Builds the Model Context Protocol (MCP) server plugin. |
| `SMATCHET_ENABLE_STRICT_WARNINGS`| `ON`  | Applies strict compiler warnings (`/W4` or `-Wall -Wextra`) to first-party code. |

### Unreal Engine Plugin

When built on Windows, Smatchet provides a target (`SmatchetPackageUnrealLibs_DX12`) that automatically packages the DX12-compatible core library, ImGui, and public headers into the `UnrealPlugins/SmatchetImGuiPlugin/ThirdParty/Smatchet` layout for immediate consumption by the Unreal Build Tool.

## Architecture

* **`Source_Core/`**: The heart of the application. Contains the backend-agnostic tracker interface (`ITrackerClient`), concrete backends (`JiraClient`, `PlaneClient`), Perforce tools, local cache managers, and all ImGui UI definitions.
* **`Target_Standalone/`**: The main entry point, window management, and GLFW/OpenGL bootstrapping for the standalone application.
* **`Plugins/`**: Optional plugin modules (Lua Console, MCP server).
* **`UnrealPlugins/`**: Contains the Unreal Engine plugin scaffolding and DX12 render backend.
* **`ThirdParty/`**: Holds custom fixes or scripts for external dependencies.
* **`scripts/`**: Runtime Lua/art assets plus tooling subfolders. `scripts/` root contains the shipped Lua runtime files (`Automation.lua`, `SmatchetHooks.lua`, `RunLua.lua`) and bundled art copied next to the standalone exe as `Scripts/`. Use `scripts/dev/` for local build/dev helpers and `scripts/publish/` for release, signing, installer, and smoke-test tooling.
* **`cmake/`**: Additional CMake helper modules.

## License

This project is licensed under the [MIT License](LICENSE).

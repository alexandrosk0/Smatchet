# Smatchet

Smatchet is a high-performance, engine-agnostic productivity tool and issue-tracking client. It provides a unified interface for project management, Perforce source control analysis, and AI assistance — all built using C++14 and Dear ImGui. Smatchet supports **multiple tracker backends** (Jira and Plane.so) through a clean, backend-agnostic architecture, and can run as a standalone desktop application or embedded directly into Unreal Engine.

## Features

- **Multi-Backend Tracker Support**: Seamlessly switch between **Jira** (Atlassian Cloud) and **Plane.so** from the Preferences panel. Views, field catalogs, and issue data are kept separate per backend.
- **Full Issue Management**: Search, view, create, and edit issues. Supports offline drafting, custom fields, inline field editing, bulk import/export, and per-backend "new issue inherit" field configuration.
- **Perforce Blame Analysis**: Native P4 integration for fast file blame analysis directly within the UI, complete with syntax highlighting.
- **Fast Local Caching**: Uses SQLite to cache field catalogs, user metadata, and recent issues locally for near-instant load times and offline capabilities.
- **Engine-Agnostic UI**: Built on Dear ImGui. The core library (`Source_Core`) contains no direct graphics API dependencies.
- **UI Localization**: Switch the app-owned Dear ImGui UI between built-in English and French, with optional local JSON overrides for teams that want custom wording.
- **Dual Deployment**:
  - **Standalone App**: A native desktop application leveraging GLFW and OpenGL3.
  - **Unreal Engine Plugin**: Direct DX12 integration allowing Smatchet to run seamlessly inside the Unreal Engine editor.
- **Views System**: Backend-aware views dashboard for managing saved queries, column layouts, sort orders, and column widths — stored per-tracker so switching backends preserves each backend's view set.
- **Optional Lua Automation**: Embeds a Lua 5.3 runtime using `sol2`, enabling custom scripts, automation hooks, custom ImGui windows, and a dedicated in-app Lua console.
- **Optional AI & MCP Support**: Includes an AI assistant controller and an integrated MCP (Model Context Protocol) server plugin to bridge into modern AI workflows.

## Documentation

- [Build Guide](BUILD.md): Supported CMake presets, prerequisites, local compiler paths, and wrapper scripts.
- [Lua Scripting Guide](LUA_GUIDE.md): Complete reference for automating workflows and customizing the UI with Lua.
- [MCP (Model Context Protocol) Guide](MCP_GUIDE.md): How to use Smatchet as an MCP server for AI agents.

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
- A C++14 compliant compiler (MSVC, GCC, or Clang)
- Git (for fetching dependencies)

### Configuration Options

Smatchet exposes several CMake options to customize the build:

| Option | Default | Description |
| :--- | :--- | :--- |
| `SMATCHET_WITH_LUA_AUTOMATION` | `ON` | Builds with the Lua console plugin and `sol2` bindings for field automation. |
| `SMATCHET_WITH_MCP` | `ON` | Builds the Model Context Protocol (MCP) server plugin. |
| `SMATCHET_WITH_AI` | `OFF` | Includes the AI assistance HTTP client. |
| `SMATCHET_ENABLE_STRICT_WARNINGS`| `ON`  | Applies strict compiler warnings (`/W4` or `-Wall -Wextra`) to first-party code. |

### Quick Build

```bash
cmake --preset ninja-debug
cmake --build --preset ninja-debug --target SmatchetStandalone
```

### Unreal Engine Plugin

When built on Windows, Smatchet provides a target (`SmatchetPackageUnrealLibs_DX12`) that automatically packages the DX12-compatible core library, ImGui, and public headers into the `UnrealPlugins/SmatchetImGuiPlugin/ThirdParty/Smatchet` layout for immediate consumption by the Unreal Build Tool.

## Architecture

* **`Source_Core/`**: The heart of the application. Contains the backend-agnostic tracker interface (`ITrackerClient`), concrete backends (`JiraClient`, `PlaneClient`), Perforce tools, local cache managers, and all ImGui UI definitions.
* **`Target_Standalone/`**: The main entry point, window management, and GLFW/OpenGL bootstrapping for the standalone application.
* **`Plugins/`**: Optional plugin modules (Lua Console, MCP server).
* **`UnrealPlugins/`**: Contains the Unreal Engine plugin scaffolding and DX12 render backend.
* **`ThirdParty/`**: Holds custom fixes or scripts for external dependencies.
* **`scripts/`**: Default Lua scripts (`Automation.lua`, `SmatchetHooks.lua`, `RunLua.lua`). Edit them in-app via **Windows → Scripting…** (standalone copies this tree next to the exe as `Scripts/`).
* **`cmake/`**: Additional CMake helper modules.

## License

This project is licensed under the [MIT License](LICENSE).

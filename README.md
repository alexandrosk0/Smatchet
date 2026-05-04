# Smatchet

Smatchet is a high-performance, engine-agnostic productivity tool and ticketing client. It is designed to integrate Jira issue tracking, Perforce source control analysis, and AI assistance into a single, unified interface. Built using C++14 and Dear ImGui, Smatchet is optimized for speed and provides a seamless developer experience, whether running as a standalone application or embedded directly into a game engine like Unreal Engine.

## Features

- **Jira Integration**: Full-featured Jira client for searching, viewing, creating, and mutating tickets. Supports offline issue drafting, custom fields, and bulk exports.
- **Perforce Blame Analysis**: Native P4 integration allowing for fast file blame analysis directly within the UI, complete with syntax highlighting.
- **Fast Local Caching**: Uses SQLite to cache Jira field catalogs, user meta, and recent issues locally for near-instant load times and offline capabilities.
- **Engine-Agnostic UI**: Built on Dear ImGui. The core library (`Source_Core`) contains no direct graphics API dependencies.
- **Dual Deployment**: 
  - **Standalone App**: A native desktop application leveraging GLFW and OpenGL3.
  - **Unreal Engine Plugin**: Direct DX12 integration allowing Smatchet to run seamlessly inside the Unreal Engine editor.
- **Optional Lua Automation**: Embeds a Lua 5.3 runtime using `sol2`, enabling custom scripts, automation, and a dedicated in-app Lua console plugin.
- **Optional AI & MCP Support**: Includes an AI controller and an integrated MCP (Model Context Protocol) server plugin to hook into modern AI workflows.

## Documentation

- [Lua Scripting Guide](LUA_GUIDE.md): Complete reference for automating workflows and customizing the UI with Lua.
- [MCP (Model Context Protocol) Guide](MCP_GUIDE.md): How to use Smatchet as an MCP server for AI agents.

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
| `SMATCHET_WITH_LUA_AUTOMATION` | `ON` | Builds Smatchet with the Lua console plugin and `sol2` bindings for field automation (`option()` default in `CMakeLists.txt`). |
| `SMATCHET_WITH_MCP` | `ON` | Builds the Model Context Protocol (MCP) server plugin (`option()` default in `CMakeLists.txt`). |
| `SMATCHET_WITH_AI` | `OFF` | Includes the AI assistance HTTP client. |
| `SMATCHET_ENABLE_STRICT_WARNINGS`| `ON`  | Applies strict compiler warnings (`/W4` or `-Wall -Wextra`) to first-party code. |

### Build Instructions (Standalone)

```bash
# 1. Clone the repository
git clone https://github.com/alexandrosk0/Smatchet.git
cd Smatchet

# 2. Configure the project
# Enable Lua, MCP, and AI plugins if desired
cmake -B build -S . -DSMATCHET_WITH_LUA_AUTOMATION=ON -DSMATCHET_WITH_MCP=ON -DSMATCHET_WITH_AI=ON

# 3. Build the standalone executable
cmake --build build --config Release
```

Once compiled, the executable can be found in the `build/Target_Standalone` directory.

### Unreal Engine Plugin Generation

When built on Windows, Smatchet provides a target (`SmatchetPackageUnrealLibs_DX12`) that automatically packages the DX12-compatible core library, ImGui, and public headers into the `UnrealPlugins/SmatchetImGuiPlugin/ThirdParty/Smatchet` layout for immediate consumption by the Unreal Build Tool.

## Architecture

* **`Source_Core/`**: The heart of the application. Contains all Jira logic, Perforce tools, local cache managers, and ImGui UI definitions.
* **`Target_Standalone/`**: The main entry point, window management, and GLFW/OpenGL bootstrapping for the standalone application.
* **`Plugins/`**: Optional plugin modules (e.g., Lua Console, MCP).
* **`UnrealPlugins/`**: Contains the Unreal Engine plugin scaffolding.
* **`ThirdParty/`**: Holds custom fixes or scripts for external dependencies.

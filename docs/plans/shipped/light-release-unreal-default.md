# Plan - Lua-Capable Light Release and Unreal Default

> **Slug**: `light-release-unreal-default`

## Context

The release needs a lighter build that ships alongside the existing full standalone, while keeping Lua automation and the in-app command palette available. The same light profile should become the default native payload for the Unreal plugin. After this lands, the full standalone remains feature-rich, while `Smatchet-Light.exe` and the packaged Unreal plugin default to Lua-capable builds without MCP, AI assistant implementation, or Whisper.

## Approach

Define the light profile as Lua ON, full command palette ON, MCP OFF, AI OFF, Whisper OFF, and standalone headless `cmd` runner OFF. Keep command registration intact so palette, Lua actions, automation commands, scenarios, debug commands, and UI-test commands remain discoverable in the GUI.

Split the standalone CLI runner from the in-app command system with a new `SMATCHET_WITH_STANDALONE_CMD_RUNNER` option. The option defaults ON for existing builds and turns OFF for the light standalone only, so `Smatchet-Light.exe cmd ...` exits clearly instead of launching a hidden/headless runner.

Move Unreal release packaging to the same light feature profile by default. The Unreal MSVC helper and release script should package Lua-capable DX12 libs, while omitting MCP, AI, and Whisper components.

## Files to modify

1. `CMakeLists.txt`: add `SMATCHET_WITH_STANDALONE_CMD_RUNNER`, prune inactive AI implementation sources when AI is OFF, and keep Lua/command registry sources in light builds.
2. `CMakePresets.json`: update `ninja-publish-light-msvc`, add a default light Unreal packaging preset, and keep full publish behavior unchanged for full standalone.
3. `Target_Standalone/main.cpp`: gate `CliCommandRunner` include/use and return a clear unsupported error for `Smatchet-Light.exe cmd ...`.
4. `Target_Standalone/CliCommandRunner.cpp` / `.h`: compile only when the standalone command runner option is ON.
5. `scripts/publish/release_github.ps1`: default Unreal packaging to the light Unreal preset while still producing both full and light standalone zips.
6. `scripts/dev/package_unreal_plugin_msvc.ps1` and `scripts/dev/build_and_deploy_unreal_plugin.ps1`: configure MSVC Unreal packaging with Lua ON and MCP/AI/Whisper OFF by default.
7. `Source_Core/src/SmatchetImGuiHost.cpp`: adjust startup logging so missing Lua is not warned for light-disabled cases, while Lua remains registered in the new default Unreal package.
8. `scripts/publish/test_release_smoke.ps1`: update smoke expectations for the light standalone and Unreal package payload.

## Existing utilities reused

- `smatchet_configure_dx12_core_impl_target` in `CMakeLists.txt`: keep using the existing DX12 core target path for Unreal packaging.
- `SmatchetPackageUnrealLibs_DX12` in `CMakeLists.txt`: keep the existing copy/package target and change only its feature inputs.
- `Get-PresetBinaryDir` in `scripts/publish/release_github.ps1`: keep release scripts preset-driven.
- `CommandRegistry::Dispatch` in `Source_Core/src/Commands/CommandRegistry.cpp`: retain the single command system used by palette, Lua, CLI, and future Unreal dispatch.
- `AppController_LuaBindings.cpp`: keep Lua global actions registering `lua.<actionName>` commands for the palette and command system.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: Positive impact for Unreal and light builds by removing inactive MCP/AI/Whisper surfaces; Lua and palette paths remain unchanged.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: No new synchronous UI work; the light change removes standalone headless process work from `Smatchet-Light.exe`.
- **Pillar 3 (never crash)**: Risk is compile-time feature mismatch across Standalone/DX12; mitigate with full standalone, light standalone, and DX12 packaging builds.
- **Pillar 4 (accessibility - keyboard nav / font scaling / WCAG AA)**: No direct visual or keyboard behavior change; command palette keyboard access is preserved.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A - <reason>`)

1. **PR-fast CI** - fires: use `command-palette-fuzzy` or nearest command-palette scenario because command registration and palette availability must remain intact.
2. **Pillar 2 static scanner** - fires: confirm no new sync I/O reachable from `ImGui::*`; this plan should only remove or gate code paths.
3. **Dispatcher drain** - N/A: does not change `MainThreadDispatcher::Drain()`.
4. **Visible-cue bucket-E harness** - N/A: no new sync-stall path over 100 ms.
5. **Marker inventory** - N/A: no new `SMATCHET_UI_PERF_SCOPE` markers.

**Pre-push local check**: run the named command-palette scenario gate if available, plus the build gates below.

**Override**: no perf override expected.

## Risks / non-goals

- Risk: removing AI implementation sources while preserving AI-disabled command stubs can expose missing include guards. Mitigation: build light standalone and DX12 package after source pruning.
- Risk: Unreal MSVC helper may reuse an old CMake cache with full features. Mitigation: document and test `-ForceConfigure`; configure scripts should pass explicit feature flags.
- Risk: packaged Unreal optional libs from a previous full build could remain in `ThirdParty/Smatchet`. Mitigation: keep package target cleaning the lib output directory before copy.
- Non-goal: remove Lua, Lua Console, Scripts & Actions, command palette, or command registry from light builds.
- Non-goal: change full standalone release defaults.
- Non-goal: make the Unreal plugin talk through MCP.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: run if tests are built for the full preset; light preset keeps tests OFF by design.
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: run command-palette scenario coverage if available because palette registration remains a release requirement.
- **Bash-driver scenario / screenshot / sanitizer**: run release smoke and inspect packaged payload; no screenshot expected unless UI files change visually.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` and `cmake --build --preset ninja-publish-light-msvc --target SmatchetStandalone`.
- **Unreal package gate**: build the new light Unreal preset and, when MSVC is available, `scripts/dev/package_unreal_plugin_msvc.ps1 -PackageOnly -ForceConfigure -Configuration Release`.
- **Manual residue**: if an Unreal Editor install cannot be automated in the slice, record the exact deferred automation item in `docs/backlog/agent-self-improvement/tooling.md`.

## Out of scope (flagged, not designed)

- Async Unreal command dispatch is tracked separately in `docs/plans/shipped/unreal-command-bridge.md`.
- A separate light config profile is not included; light and full builds should preserve shared settings.
- A size-first `-Os` build is not included because the selected priority is runtime speed.

## Implementation log

- CMake: `_smatchet-light-features`, `ninja-publish-light-msvc` (`Smatchet-Light.exe`), `ninja-iter-unreal-msvc`, `vs-unreal-msvc` inherits light; AI TUs gated when `SMATCHET_WITH_AI=OFF`.
- Standalone: `StandaloneAppBootstrap` (hidden boot + `InitAppAndPlugins` / `BootEphemeral`); `CliCommandRunner` in-process path when `!SMATCHET_WITH_MCP`; `main.cpp` DRY.
- Core: empty `RegisterAiCommands` when AI OFF (ADR 0010); ImGuiHost command queue + Unreal bridge hooks.
- Release: `release_github.ps1` light zip + default Unreal light preset; `test_release_smoke.ps1` light CLI checks; MSVC package scripts use `vs-unreal-msvc`.
- MSVC/min macro: `(std::min)` / LOG fixes for light builds without Whisper.

## Deviations from plan

- **Item 10 (main.cpp DRY)** — shipped in follow-up slice after initial CLI-only bootstrap: GUI uses `InitAppAndPlugins`; `--ephemeral` uses `BootEphemeral` (same plugin/MCP policy as before, including force-MCP when spawning).
- **In-process light CLI** — plan draft mentioned disabling `cmd` on light; shipped in-process `CommandRegistry::Dispatch` instead (ADR 0010 + command-system-plan § Feature-gated builds).

## Verification (actual)

| Gate | Result |
|------|--------|
| `ninja-iter-msvc` → `SmatchetStandalone` + `SmatchetCore_DX12` | PASS (warm iter build) |
| `ninja-publish-light-msvc` → `Smatchet-Light.exe` | PASS |
| Light CLI: `cmd commands.list --pretty` | exit 0 |
| Light CLI: `cmd ai.send-once` | exit 2 `unknown-command`; no `ai.*` in list |
| `vs-unreal-msvc` package + `build_deploy_and_open_unreal.ps1 -ForceConfigure` | PASS — `TestProjectEditor` + plugin DLL; Editor opened |
| `ninja-test-msvc` + ctest | DEFER — not run this slice |
| Bucket E `command-palette-fuzzy` | DEFER — not run this slice |
| Full `release_github.ps1` + `test_release_smoke.ps1` zip path | DEFER — light CLI smoke only |

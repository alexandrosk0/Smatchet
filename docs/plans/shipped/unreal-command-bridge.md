# Plan - Unreal Async Command Bridge
<!-- plan-date: 2026-05-25 -->

> **Slug**: `unreal-command-bridge`

## Context

The Unreal plugin currently forwards rendering, input, lifecycle, URL opening, and attachment viewing through `SmatchetImGuiHostC.h`, but it does not expose the unified command system to Unreal code. The light Unreal profile keeps Lua and the full command palette, so Unreal should be able to invoke the same registered commands without enabling MCP. After this lands, Unreal Blueprint and C++ code can send Smatchet commands asynchronously and receive canonical JSON command envelopes.

## Approach

Add a native async command bridge below the Unreal module and above `AppController::Commands()`. Unreal enqueues a command name plus JSON args through the C ABI, receives a request id immediately, and later receives or polls a JSON result. Native dispatch runs on the Smatchet host tick/frame path so game thread and render thread callers do not block.

Use the existing command registry and wire result format. Dispatch uses a new `CommandSource::Unreal`, sets `ctx.App`, honors confirmation/dry-run flags, and returns the existing `{ok, command, data}` or `{ok:false, command, error}` JSON envelope. This preserves command behavior across palette, Lua, CLI, MCP, and Unreal.

Expose the Unreal-facing API as Blueprint plus C++. A subsystem or function library should wrap the low-level request id API, validate JSON before enqueueing, and offer a completion delegate for Blueprint users.

## Files to modify

1. `Source_Core/include/SmatchetImGuiHostC.h`: add C ABI functions for enqueueing commands, polling/copying result JSON, and releasing result buffers.
2. `Source_Core/include/SmatchetImGuiHost.h`: add matching C++ host methods.
3. `Source_Core/src/SmatchetImGuiHost.cpp`: own the pending/completed command queues and dispatch through `AppController::Commands()`.
4. `Source_Core/include/Commands/Command.h`: add `CommandSource::Unreal`.
5. `UnrealPlugins/SmatchetImGuiPlugin/Source/SmatchetImGuiPlugin/Private/SmatchetImGuiPluginModule.cpp`: expose safe module-level access to the native host command bridge.
6. `UnrealPlugins/SmatchetImGuiPlugin/Source/SmatchetImGuiPlugin/Public` or equivalent public module folder: add the Blueprint/C++ wrapper surface.
7. `UnrealPlugins/SmatchetImGuiPlugin/Source/SmatchetImGuiPlugin/SmatchetImGuiPlugin.Build.cs`: include any required module dependencies for the wrapper/delegate surface.
8. Tests or smoke scripts under `scripts/dev` / `scripts/publish`: add command bridge smoke coverage where practical.

## Existing utilities reused

- `CommandRegistry::Dispatch` in `Source_Core/src/Commands/CommandRegistry.cpp`: the only command execution path.
- `CommandResult::ToWireJson` in `Source_Core/src/Commands/Command.cpp`: canonical JSON envelope generation.
- `MainThreadDispatcher` in `Source_Core/include/MainThreadDispatcher.h`: existing pattern for UI-thread-sensitive command handlers.
- `TickApplicationWork` in `Source_Core/src/SmatchetImGuiHost.cpp`: existing hidden-overlay tick path to drain native work while UI is hidden.
- `commands.list` / `commands.help` in `Source_Core/src/Commands/Builtin/BuiltinCommands_Meta.cpp`: discovery and smoke-test commands for Unreal.
- Lua global action command mirroring in `Source_Core/src/AppController_LuaBindings.cpp`: lets Unreal invoke `lua.<actionName>` after hooks register actions.

## P4-gated execution protocol

This slice is running with Git edits directly on `develop` in `C:\Dev\Smatchet`, while P4 opens are isolated in task stream `//smatchet/task-unreal-command-bridge` through client `task_unreal-command-bridge` rooted at the same checkout.

- Open only command-bridge files with `P4CLIENT=task_unreal-command-bridge`.
- Keep unrelated dirty `develop` files out of `p4 reconcile` and out of the task-stream CL.
- Shelf the task-stream change for P4V review before integrating back to `//smatchet/main`.
- After approval, run the task-stream promote flow, then mirror the submitted P4 content to Git/PR.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: Queue draining must be bounded per tick or cheap for normal use; large command work remains inside existing command handlers.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: Unreal calls are async and must not block the game or render thread; blocking sync helpers are out of scope for v1.
- **Pillar 3 (never crash)**: C ABI owns result buffer lifetime explicitly and validates handles/request ids; malformed JSON returns an error envelope.
- **Pillar 4 (accessibility - keyboard nav / font scaling / WCAG AA)**: No visual UI change; Blueprint access can be used to bind accessible project-side controls later.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A - <reason>`)

1. **PR-fast CI** - fires: command registry smoke via `commands.list`; if scenario coverage exists, use the command-palette scenario as closest command-system coverage.
2. **Pillar 2 static scanner** - fires: confirm Unreal wrapper and native enqueue path do not perform blocking waits on game/render/UI threads.
3. **Dispatcher drain** - N/A unless implementation changes `MainThreadDispatcher::Drain()`; the preferred design does not.
4. **Visible-cue bucket-E harness** - N/A: no new synchronous UI stall path; commands are async.
5. **Marker inventory** - N/A by default; add/update marker inventory only if queue drain gets a new `SMATCHET_UI_PERF_SCOPE`.

**Pre-push local check**: run command-system and Unreal packaging build gates; run an Unreal smoke if available.

**Override**: no perf override expected.

## Risks / non-goals

- Risk: commands queued before native host initialization could be lost or hang. Mitigation: hold them pending until init succeeds, and return a structured shutdown/error envelope if the host is destroyed.
- Risk: long-running command handlers could still do work after async dispatch. Mitigation: v1 avoids blocking Unreal callers; handler-level responsiveness remains governed by existing command contracts.
- Risk: result buffers crossing C ABI boundaries can leak or dangle. Mitigation: use explicit copy/release API and document ownership in `SmatchetImGuiHostC.h`.
- Risk: Blueprint users may pass invalid JSON. Mitigation: validate before enqueue and return an error envelope without dispatching.
- Non-goal: expose MCP or a network server in the Unreal plugin.
- Non-goal: provide a blocking synchronous Blueprint command API.
- Non-goal: redesign the command registry schema or command names.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: add pure tests for request id lifecycle, invalid JSON, unknown command, and result envelope formatting if the queue logic can be isolated.
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: run command-palette coverage as regression coverage for command registration.
- **Bash-driver scenario / screenshot / sanitizer**: add a smoke that calls `commands.list` through the native host bridge when possible; no screenshot expected.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12`.
- **Unreal package gate**: build the default light Unreal package and the MSVC package when MSVC is available.
- **Manual residue**: if Blueprint delegate behavior cannot be automated in the slice, add a deferred automation entry with the exact Blueprint/C++ smoke needed.

## Out of scope (flagged, not designed)

- Light build feature selection is tracked in `docs/plans/shipped/light-release-unreal-default.md`.
- Remote command transport is out of scope; Unreal talks in-process through the native host.
- Rich typed Blueprint parameter structs are out of scope for v1; JSON keeps parity with existing command surfaces.

## Implementation log

- 2026-05-25: Added native async command queue to `SmatchetImGuiHost`, C ABI enqueue/poll/take/release functions, `CommandSource::Unreal`, bounded completed-result storage, and a Blueprint/C++ wrapper surface with request-id polling plus optional completion callback.
- 2026-05-25: Fixed MSVC Unreal packaging blockers exposed by `rebuild_testproject_plugin.ps1 -Release` and updated the MSVC packaging helpers to invalidate stale caches that keep MCP/AI/Whisper enabled for the Unreal light profile.
- 2026-05-25: Added `UnrealPlugins/SmatchetImGuiPlugin/README.md` as the user-facing manual for overlay, Blueprint, C++, and native C ABI communication paths.

## Deviations from plan

- `SmatchetImGuiPlugin.Build.cs` required no dependency change; existing `CoreUObject`, `Engine`, and `Slate` dependencies cover the wrapper surface.
- The post-review rebuild exposed existing MSVC/light-profile drift in shared core files and scripts; those fixes were folded into this slice so the bridge can be rebuilt and linked by UBT.

## Verification (actual)

- `cmake --build --preset ninja-iter-msvc --target SmatchetImGuiHost_DX12 -j 4` passes after prepending `C:\msys64\ucrt64\bin` to `PATH`; without that, `cc1plus.exe` exits `-1073741515` due missing runtime DLL lookup.
- `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone -j 4` passes.
- `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12 -j 4` passes/no work after the targeted builds.
- `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12 -j 4` passes again after the final polish pass; warnings are pre-existing unused-parameter warnings in scenario files.
- `cppcheck` on the edited `Source_Core` command-bridge files passes.
- `clang-tidy -p build\ninja-iter-msvc --quiet Source_Core\src\SmatchetImGuiHost.cpp` passes.
- `bash scripts/dev/pillar2-scan.sh ...` on the edited bridge files passes.
- `.\scripts\dev\rebuild_testproject_plugin.ps1 -Release` passes end-to-end: stale `build\vs-unreal-msvc` cache was invalidated, MSVC native DX12 libs were packaged with `SMATCHET_WITH_MCP=OFF`, `SMATCHET_WITH_AI=OFF`, and `SMATCHET_WITH_WHISPER=OFF`, the plugin was deployed to the TestProject, UHT ran, and UBT linked `TestProject.exe`.
- `.\scripts\dev\package_unreal_plugin_msvc.ps1 -Configuration Release -PackageOnly` passes against the corrected MSVC cache.
- `llvm-nm -C UnrealPlugins\SmatchetImGuiPlugin\ThirdParty\Smatchet\lib\Win64\Development\SmatchetImGuiHost_DX12.lib` confirms the packaged host exports `SmatchetHost_EnqueueCommand`, `SmatchetHost_IsCommandResultReady`, `SmatchetHost_TakeCommandResultJson`, and `SmatchetHost_ReleaseCommandResultJson`.
- `cppcheck`, `clang-tidy -p build\ninja-iter-msvc --quiet C:\Dev\Smatchet\Source_Core\src\SmatchetImGuiHost.cpp`, and `bash scripts/dev/pillar2-scan.sh Source_Core/src/SmatchetImGuiHost.cpp` pass after the final C ABI wrapper fix.
- Post-review `bash scripts/dev/test-all.sh` was rerun under Git Bash with `/c/msys64/ucrt64/bin` prepended. It finished red (`Passed: 1084  Failed: 38`) on existing harness/visual/lock gate failures including token hook drift, callstack tooltip hover, lint-hook split, lock primitives, merge-gates cases, theme roundtrip, and columns reorder.

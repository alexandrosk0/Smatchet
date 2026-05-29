# Plugin shim-link discipline for `UiDrawSession` and other gated headers

Every Smatchet plugin static library that includes `Source/Core/include/` headers must link the same `*Shim` INTERFACE libraries that the Source/Core impl target it talks to was built against. Mismatched defines silently shift struct layout and cause UB at runtime.

## Background

`Source/Core/include/SmatchetUiSession.h` carries three macro-gated member blocks inside `UiDrawSession`:

- `#if defined(SMATCHET_WITH_LUA_AUTOMATION)` (lines 149-155, 570-572)
- `#if defined(SMATCHET_WITH_MCP)` (lines 157-161, 371-380)
- `#if defined(SMATCHET_WITH_AI)` (lines 163-224)

The `AI` block is the heaviest — about 200 bytes of `assistant*` members in the middle of the struct. Members declared **after** any gated block move offset based on whichever set of macros the TU sees.

The cross-translation-unit contract is: the TU that defines the global (`Source/Core/src/SmatchetUI.cpp` for `g_ui`) and every TU that reads it via `extern UiDrawSession g_ui;` must agree on macro state. If they disagree, the plugin computes wrong offsets, writes garbage, and crashes on the next access — typically inside `std::string::clear()` or `std::set::begin()` because the wrong-offset bytes happen to be a string-control-block or red-black-tree-node-ish pattern.

## Considered options

- **Always-on defines on `SmatchetCoreInterface`.** Simplest — `target_compile_definitions(SmatchetCoreInterface INTERFACE SMATCHET_WITH_AI=1)` and every consumer sees it. Rejected for `WITH_MCP` / `WITH_AI` (kept for `WITH_LUA_AUTOMATION`): the Unreal DX12 packaging path deliberately turns those features off and compiles `Source/Core/src/*.cpp` without the defines so the AI/MCP code paths drop via `#if defined` gates. Forcing the define on the interface library would either bleed AI/MCP code into the Unreal package or trigger `/D` vs `/U` MSVC warnings (D9025).
- **Hash-mangled struct names per macro state.** Linker would refuse to resolve a mismatched extern. Rejected: forces ABI-level type renames and breaks every existing `extern UiDrawSession g_ui;` declaration in plugin TUs.
- **`static_assert(sizeof(UiDrawSession) == N)` in the header.** Catches mismatch at compile time. Rejected: `N` itself depends on the macro state of the TU compiling the header, so the assertion can't distinguish the bad case from the good.
- **Per-feature INTERFACE shim libraries linked by both core impl targets and plugin libs.** Adopted. Each feature (`MCP`, `AI`) gets a tiny `add_library(Smatchet*Shim INTERFACE)` whose only payload is `target_compile_definitions(... INTERFACE SMATCHET_WITH_*=1)`. Standalone core + plugins link the shim; DX12 core + plugins don't. Layout consistency is established by linkage, not by a force-on define.

## Consequences

- **Hard rule**: every CMake target that includes a Source/Core header and is in the standalone (OpenGL) plugin link graph must `target_link_libraries(... PUBLIC SmatchetCoreAiShim)` (and `SmatchetCoreMcpShim` when relevant). DX12 / Unreal variants intentionally skip the shim and stay layout-consistent within the DX12 link graph instead.
- The standalone-side build does the matching at the plugin lib boundary:
  - `SmatchetPlugin_LuaConsole` → `SmatchetCoreInterface` + `SmatchetCoreMcpShim` (when WITH_MCP) + `SmatchetCoreAiShim` (when WITH_AI).
  - `SmatchetPlugin_Mcp` → same + `SmatchetCoreAiShim`.
  - `SmatchetPlugin_Whisper` → `SmatchetCoreInterface` + `SmatchetCoreAiShim`.
- The DX12-side build keeps the variants shim-free; both core and plugin TUs see the same (smaller) layout.
- **Adding a new feature gate (`SMATCHET_WITH_X`) that touches `UiDrawSession` or any other shared struct now requires the matching `SmatchetCoreXShim INTERFACE` library and the same plugin-link discipline.** Whoever adds the gate must also wire the shim into every standalone plugin that includes Source/Core headers.
- Regression risk: a new plugin lib added without the matching shim link will crash silently on first access to a post-block member. The crash signature ("SIGSEGV in `std::string::clear()` reachable from a plugin TU") is the canonical hint. First-line diagnosis: dump `build/<preset>/compile_commands.json` and confirm `-DSMATCHET_WITH_*=1` shows up for both the plugin's TU and the Source/Core TU that defines the affected global.
- Caught in production by `fix(build): link SmatchetCoreAiShim to plugins` after the `Save` button in `LuaConsolePlugin::OnDraw` crashed because the plugin missed the `WITH_AI` shim. Audit pass added the shim to LuaConsole + Mcp + Whisper plugins; DX12 variants remain shim-free by design.

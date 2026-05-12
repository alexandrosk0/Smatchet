## Smatchet — project rules

**Build**: `cmake --build --preset ninja-iter-msys2` (iter), `ninja-debug-msys2` (debug), `ninja-publish-msys2` (publish). Exe at `build/<preset>/SmatchetStandalone.exe`.

**Language**: C++14 hard (Unreal compat). Banned: `string_view`, `optional`, `variant`, structured bindings, `if constexpr`. Must compile on MinGW UCRT + MSVC.

**Layout**: `Source_Core/{src,include}` is the shared core — used by both standalone and Unreal. `Target_Standalone/` builds the OpenGL exe. `Plugins/{Mcp,LuaConsole}` are static plugins. `*_DX12` targets are `EXCLUDE_FROM_ALL` (Unreal only) — don't touch unless asked.

**Available libs** (FetchContent, linked): nlohmann/json, cpr, SQLiteCpp, cpp-httplib, md4c, ImGui (docking), GLFW, Lua + sol2, ghc::filesystem.

**Logging**: `LOG_{DEBUG,INFO,WARN,ERROR,TRACE}` from `Logger.h` — never `printf` / `std::cerr`.

**nlohmann json**: `obj["k"] = v`, not `obj = {...}` (reassignment with brace-list won't compile).

**Optional plugins**: gate with `#if SMATCHET_WITH_LUA_AUTOMATION` / `#if SMATCHET_WITH_MCP`. Lua bindings split: `AppController_LuaBindings.cpp` (on) ↔ `AppController_LuaStubs.cpp` (off) — keep in sync.

**Don't**: add GLFW/OpenGL includes to `Source_Core/` headers (DX12 compiles them too); redefine `IMGUI_USE_WCHAR32` (PUBLIC on `ImGuiLib`).

**Dual-target**: `Source_Core/` compiles into both `SmatchetStandalone` (OpenGL+GLFW) and `SmatchetCore_DX12` (Unreal). Diverging macros: `SMATCHET_EMBEDDED_IN_UNREAL=1` (DX12 only); `SMATCHET_WITH_MCP=1` (Standalone only — `SMATCHET_WITH_MCP_UNREAL` is OFF). Lint hook syntax-checks both targets per `.cpp` edit. Full verify: `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12`.

**Quality**: RAII (no raw `new`/`delete` — use `std::unique_ptr` + `make_unique`); `const&` for non-trivial params; `std::move` on last use; small focused functions; `LOG_TRACE`/`LOG_DEBUG` in non-trivial branches.

**Lint** (PostToolUse hook auto-runs after `.cpp`/`.h` edits in `Source_Core`/`Plugins`/`Target_Standalone`): `clang-format -i` applies in place; `cppcheck` + `clang-tidy` report to stderr — fix all reported issues before responding.

**Perf workflow** (when user asks to optimize, profile, or fix FPS / lag / hitch / "slow"): never guess. Run the measure-change-measure loop:

1. **Instrument.** Wrap suspected hot paths in `SMATCHET_UI_PERF_SCOPE("temp:<area>")` from `UiPerfMonitor.h`. Prefix every *new* marker with `temp:` so cleanup is mechanical (`grep -r 'temp:'`). Cover the whole hypothesis tree, not just one suspect — add markers around the call site, each candidate sub-call, and the surrounding render plan branch. Markers are O(ns) so over-instrumenting is fine.
2. **Build & hand off.** Build with `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone`. Then ask the user to run the exe, reproduce the bad scenario (e.g. "open a view with the priority column visible"), open the Perf panel, and paste the `temp:*` rows back. Do NOT attempt to read FPS yourself — there is no headless export and I can't observe the GUI.
3. **Diagnose from the numbers.** The dominant `temp:*` row (highest `lastTotalMs`) is the target. Don't change anything that isn't measurably hot — if no `temp:*` row stands out, add finer-grained markers and re-measure before editing.
4. **Change, rebuild, re-measure.** Apply the fix. Build. Ask the user to repeat the same scenario and paste the new `temp:*` rows.
5. **Validate.** Compare before/after on the dominant row(s). If `lastTotalMs` didn't drop materially (or another row now dominates), iterate — don't claim success on a build pass alone.
6. **Clean up.** Once the user confirms the FPS improvement is real, remove every `temp:*` marker. `grep -rn 'SMATCHET_UI_PERF_SCOPE("temp:' Source_Core/ Plugins/ Target_Standalone/` should return empty before the PR commit. Pre-existing non-`temp:` scopes stay.

Skip the workflow only for fixes whose cost is obvious from the code alone (e.g. removing a per-frame disk read) — and even then, prefer to confirm with one round of measurement before declaring victory.

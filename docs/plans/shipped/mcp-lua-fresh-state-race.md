# Plan — MCP Lua execution: fresh per-call `sol::state` (cross-thread race fix)
<!-- plan-date: 2026-05-28 -->

> **Slug**: `mcp-lua-fresh-state-race`

## Context

Verified HIGH-severity cross-thread data race violating **UX Pillar 3 (never crash)**. The MCP `run_lua` snippet/script handlers and the registered-Lua-tool handler run inside httplib worker-thread lambdas (`Plugins/Mcp/src/McpPlugin.cpp` REST `/mcp/tools/call` :395 + JSON-RPC :658) and execute against the **shared main `sol::state lua`** member (`Source_Core/include/AppController.h:898`) with **no interpreter mutex and no `MainThreadDispatcher` hop**. The UI thread concurrently drives the same `lua` state (`DrawLuaWindows`, Lua cell-providers, `ExecuteLuaConsoleSnippet`). Two threads through one `lua_State` = undefined behavior (realistically heap corruption / crash). The only locks on these paths (`luaMcpToolsMutex_`, `luaActionsMutex_`) guard metadata vectors, not the interpreter. Reachable locally with no special config: MCP binds `127.0.0.1` by default; only gated by `allow_lua_execution`. `AppController.h:144-146` already documents that MCP/Lua workers must post to `mainThreadDispatcher` — these paths violate that.

Intended outcome: **after this lands, no MCP-worker Lua execution path ever touches the main `lua` state — each runs on a fresh, per-call `sol::state`, so the UI thread and MCP workers never share a `lua_State`.**

## Approach

Adopt the existing, proven isolation pattern from `AutomationWorkerLoop` (`AppController_LuaBindings.cpp:1155`), which already runs each background job on a per-job `sol::state bgState` precisely to avoid sharing. Apply it to all three MCP entry points in `AppController_LuaBindings_Draw.cpp`:

1. **`ExecuteLuaSnippetForMcp` / `ExecuteLuaScriptForMcp`** (`run_lua`): construct a fresh `sol::state`, `InitLuaCore` it, build a sandbox on it, then load + run the code there. No setup-script replay — today these run a sandbox over main `lua` whose `_G` contains *only* `InitLuaCore` builtins (`RunLuaSetupScript` runs setup under a throwaway sandbox, so user helpers never reach main `_G`), so a bare `InitLuaCore` state reproduces today's capability set exactly.

2. **`ExecuteLuaMcpTool`** (registered tool): the tool's `callback` is a `sol::protected_function` **bound to main `lua`**, so it cannot be moved. Instead **rebuild it on a fresh state**: `InitLuaCore` a fresh state, install a **call-local `mcp.register_tool` override** that collects definitions into a stack-local vector (leaving the shared `luaMcpTools_` untouched), replay `activeSetupScripts_` on that state so the tool re-registers as a fresh-state-bound closure, then find + invoke it there. Limitation (accepted): a tool registered ad-hoc by a prior `run_lua` snippet — not by a setup script — won't exist in the rebuilt state and returns "tool not found"; this is a degenerate, already-racy usage.

3. **Host glue marshallers must be state-relative** (found in security review). `InitLuaCore` keeps `smatchet.get_ticket` / `smatchet.create_issue` / `decode_json` live on the fresh state, but their `ILuaBindingHost::Lua*Bind` impls marshalled results via the **member `lua`** (`sol::make_object(lua,…)` / `JsonToLua(lua,…)` / `lua.create_table()`). From a fresh-state caller that both (a) re-touches the shared UI `lua_State` cross-thread and (b) returns a `sol::object` bound to `lua` into a snippet running on `callState` — a cross-state object transfer that is UB even single-threaded. Fix: thread the calling `sol::state_view` (the glue's `sol::this_state`) into `LuaGetTicketBind` / `LuaDecodeJsonBind` / `LuaCreateIssueBind` and marshal against it, never the member. This also closes the same latent bug for the automation worker's `bgState`.

Trade-off: each MCP Lua call now pays `InitLuaCore` (snippet/script) or `InitLuaCore` + setup replay (tool) — bounded, off the UI thread, out-of-band; identical to the per-job cost `AutomationWorkerLoop` already accepts. This is strictly cheaper than the correctness alternatives (a global interpreter mutex would let a worker stall the UI thread; UI-thread marshalling would run I/O-heavy tool callbacks on the UI thread and freeze it).

## Files to modify

1. `Source_Core/src/AppController_LuaBindings_Draw.cpp:740` — rewrite `ExecuteLuaMcpTool` (fresh state + call-local register override + setup replay + invoke).
2. `Source_Core/src/AppController_LuaBindings_Draw.cpp:798` — rewrite `ExecuteLuaSnippetForMcp` (fresh state).
3. `Source_Core/src/AppController_LuaBindings_Draw.cpp:936` — rewrite `ExecuteLuaScriptForMcp` (fresh state).
4. `Source_Core/src/AppController_LuaBindings.cpp:1069` — extract `ParseMcpToolDef(const sol::table&, McpToolDefinition&)` from `LuaMcpRegisterToolBind` (shared by production bind + call-local override).
5. `Source_Core/src/AppController_LuaBindings.cpp:1127` — extract `PrepareFreshLuaState(sol::state&)` (`InitLuaCore` + `__smatchet_app_ui` alias) and `ReplayActiveSetupScripts(sol::state&)` from `AutomationWorkerLoop`; refactor the worker to call them (DRY, single proven pattern). New `RunLuaCodeOnFreshState` helper backing snippet/script.
6. `Source_Core/include/AppController.h:893` — private decls for the new helpers; update the `sol::state lua` member-order comment (:894) + `IsOnUiThread` comment (:144) to record that MCP Lua paths are now fresh-state-isolated.
7. `tests/Source_Core/McpLuaFreshStateRace.test.cpp` — **new** full integration test (real `AppController`, two threads, ASan). See Verification.
8. `tests/CMakeLists.txt` — register the new test.
9. `CMakePresets.json` — add an ASan-with-tests configure/build preset (`ninja-clang-asan` / `ninja-msvc-asan` build `SmatchetStandalone` only; the regression test needs `SMATCHET_BUILD_TESTS=ON` under the sanitizer).

State-relative host-glue marshalling (added per security review, Approach §3):

10. `Source_Core/include/ILuaBindingHost.h:56,60,68` — add leading `sol::state_view sv` to `LuaGetTicketBind` / `LuaDecodeJsonBind` / `LuaCreateIssueBind`.
11. `Source_Core/src/AppController_LuaBindings.cpp:735,743,757` — impls marshal against `sv` (not member `lua`); `Source_Core/src/AppController_LuaBindingsCore.cpp:195,211,224,236` — 4 glue sites pass `sol::state_view(L)`.
12. `tests/support/FakeLuaBindingHost.h:116,126,156` — test fake overrides match the new signatures (use the passed `sv`).

## Existing utilities reused

- `AppController::InitLuaCore(sol::state&)` — `AppController_LuaBindings.cpp:650` → `smatchet::lua::InitLuaCore` — sets per-state `__smatchet_app` host + safe libs + `smatchet/tracker/commands/os` builtins; the basis of fresh-state isolation.
- `CreateSandboxEnvironment(sol::state&)` — `AppController_LuaBindings.cpp:314` — sandbox escape-blocking; reused per fresh state.
- `AutomationWorkerLoop` per-job `bgState` — `AppController_LuaBindings.cpp:1155` — the proven zero-sharing pattern being generalized.
- `LuaHookGuard` (RAII count-hook) — `AppController_LuaBindings.cpp:84` — exception-safe timeout install/clear for the new paths.
- `FieldEditAuditSource::ScopedOverride(kMcp)` — preserved on every path so audit attribution is unchanged.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms)**: no impact on UI-thread steady-state — all changed work runs on httplib worker threads, off the UI thread. Per-call `InitLuaCore`/setup-replay cost is borne by the out-of-band MCP worker.
- **Pillar 2 (UI never blocks > 100 ms)**: **improves** the invariant — the fix removes UI-thread/worker contention on `lua` entirely (no shared state → no lock, no marshalling, no UI stall). Deliberately avoids the mutex/marshal alternatives precisely because they could stall the UI thread.
- **Pillar 3 (never crash)**: the fix's purpose — eliminates the concurrent-`lua_State` UB. Fresh-state member/scope ordering (state declared before the tool-collector vector) preserves the documented `sol::protected_function`-before-`sol::state` teardown invariant.
- **Pillar 4 (accessibility)**: N/A — no UI surface change.

## Perf-review-system gates (diff touches `Source_Core/`)

1. **PR-fast CI** — **N/A**: changed path is the MCP worker `run_lua`/tool execution, not in the curated UI diff→scenario map; no UI-frame scenario exercises it.
2. **Pillar 2 static scanner** — **N/A**: no new sync-I/O reachable from `ImGui::*`. The new code is worker-thread-only; it *removes* a UI-thread shared-state hazard rather than adding one.
3. **Dispatcher drain** — **N/A**: `MainThreadDispatcher::Drain()` untouched.
4. **Visible-cue bucket-E harness** — **N/A**: no new sync-stall > 100 ms code path.
5. **Marker inventory** — **N/A**: no `SMATCHET_UI_PERF_SCOPE` markers added.

## Risks / non-goals

- **Behavior change — `InitLuaUi`-only bindings are unavailable to MCP `run_lua` snippets** (the fresh state runs `InitLuaCore` only, like `AutomationWorkerLoop`). Concretely: `ui.*` becomes a no-op, and `imgui.*` (`AppController_LuaBindings.cpp:697`) + `ai.*` (`:716`, `add_context`/`clear_context`/`prompt`) + the cached-renderer / icon-map `register_*` glues are **absent** (a call gets "attempt to index nil"). Accepted + correct: every one of these mutates UI-thread-affine state, so calling them from the MCP worker thread was already the racy Pillar-2/3 violation this PR removes (pre-fix they ran on the main `lua` from the worker). Migration path for callers that need UI-thread-affine work via MCP: expose it as a Command and call `commands.invoke(...)` (still available on the fresh state; routes through the thread-aware registry). The host glue marshallers (`get_ticket`/`create_issue`/`decode_json`) are now state-relative (see Approach), so they keep working on the fresh state.
- **Behavior change — ad-hoc-registered MCP tools** (registered by a prior `run_lua` snippet, not a setup script) return "tool not found" on the rebuilt state. Accepted: degenerate, already-racy path.
- **Per-call cost** — `InitLuaCore` (+ setup replay for tools) per MCP call. Accepted: out-of-band, off UI thread, equals existing worker cost.
- **Non-goal**: host-binding (`create_issue`, `SubmitFieldEdit`) worker-thread safety — unchanged by this fix and already exercised worker-thread by `AutomationWorkerLoop`; out of scope.
- **Non-goal**: changing the MCP wire protocol, auth, or `allow_lua_execution` gating.

## Verification

- **Full integration test (Bucket E under sanitizer, `tests/ui/mcp_lua_fresh_state_race.test.cpp` — see § Deviations for why bucket-E, not the originally-planned `tests/Source_Core/`)**: a real `AppController` is provided by the live bucket-E run via `SmatchetActiveUiTestAppController()`; a worker thread fires `ExecuteLuaSnippetForMcp` + `ExecuteLuaMcpTool` in a tight bounded loop while the UI thread concurrently drives the real `DrawLuaWindows` against the main `lua` state; assert clean exit under ASan (pre-fix the same body reproduces the race). Two-thread overlap with a spin barrier to maximize interleaving.
- **Build gate (dual-target)**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12`.
- **Sanitizer gate**: configure+build the new ASan-with-UI-tests preset (`ninja-ui-test-asan-clang` / `ninja-ui-test-asan-msvc`), run the new test via `scripts/dev/test-ui-mcp-lua-fresh-state-race.sh`; expect zero sanitizer reports.
- **Existing suites**: `ninja-test-msvc` (`SmatchetTests` + `SmatchetLuaTests`) green; `scripts/dev/test-all.sh` pre-push gate.
- **Manual residue**: if the real-`DrawLuaWindows`-via-ImGui path proves infeasible inside the ctest rig (ImGui context lifetime), the test substitutes a faithful main-`lua` eval proxy on the UI-thread side + a `docs/self-improvement/categories/test.md` entry for a bucket-E follow-up. No silent residue.

## Out of scope (flagged, not designed)

- A general lua-interpreter mutex for the remaining UI-thread `lua` touches — unnecessary once MCP paths are isolated; no other cross-thread `lua` writer exists.
- Caching/pooling fresh `sol::state`s across MCP calls — premature; revisit only if profiling shows per-call init cost matters.

## Implementation log

- **Regression test (bucket-E, not the ctest rig)**: `tests/ui/mcp_lua_fresh_state_race.test.cpp` (ImGui Test Engine) + bash driver `scripts/dev/test-ui-mcp-lua-fresh-state-race.sh`. Registered in `tests/ui/ui_tests_registry.cpp` (guarded `#if defined(SMATCHET_WITH_LUA_AUTOMATION)`) and enrolled in `tests/ui/CMakeLists.txt`. A worker `std::thread` fires the real `ExecuteLuaSnippetForMcp` + `ExecuteLuaMcpTool` (each building a fresh `sol::state` via `PrepareFreshLuaState`) in a 600-iteration bounded loop, gated by a two-party spin barrier, concurrently with the UI thread driving the real `DrawLuaWindows` against the main `lua` state (a Lua window + an MCP tool are registered via `ExecuteLuaConsoleSnippet` on `lua` first). ASan is the primary oracle (heap-corruption / UAF on a shared `lua_State`); the `IM_CHECK`s assert liveness (both bounded loops completed without a crash).
- **ASan-with-UI-tests presets**: added `ninja-ui-test-asan-clang` (Clang ASan+UBSan, RelWithDebInfo) and `ninja-ui-test-asan-msvc` (MSVC `/fsanitize=address`, RelWithDebInfo) to `CMakePresets.json`, each combining `SMATCHET_SANITIZER=asan` + `SMATCHET_BUILD_UI_TESTS=ON` (the existing `ninja-clang-asan` / `ninja-msvc-asan` build `SmatchetStandalone` *without* the bucket-E surface). Clang base preferred per the existing clang-asan note (Debug/MDd incompatible with clang-cl ASan → RelWithDebInfo).

## Deviations from plan

- **Test home: bucket-E (`tests/ui/`), not `tests/Source_Core/` (Files-to-modify item 7 + Verification).** A real `AppController` cannot be constructed in the per-unit `SmatchetTests` ctest rig (it compiles selected `.cpp` + links ImGui/SQLite/cpr but not the full `AppController` graph; `InitLuaCore` is "Class C" per `tests/support/LuaHostFixture.h`). The real-`AppController` home is the bucket-E ImGui Test Engine surface, which compiles into `SmatchetStandalone` under `SMATCHET_BUILD_UI_TESTS=ON` and exposes the live controller via `SmatchetActiveUiTestAppController()`. This is the *more* faithful option, not a fallback: it drives the genuine `DrawLuaWindows` path on the real UI thread (the § Verification "Manual residue" clause anticipated a proxy substitute; **none was needed**). `tests/CMakeLists.txt` (item 8) was therefore not touched; `tests/ui/CMakeLists.txt` + `tests/ui/ui_tests_registry.cpp` carry the enrolment instead.
- **Preset names + scope.** Plan item 9 said "an ASan-with-tests preset"; shipped as two (`ninja-ui-test-asan-clang` / `-msvc`) scoped to `SMATCHET_BUILD_UI_TESTS` (bucket-E) rather than `SMATCHET_BUILD_TESTS` (ctest rig), matching the test-home deviation above.

## Verification (actual)

- **Sanitizer gate — PASS.** Built `SmatchetStandalone` under `ninja-ui-test-asan-clang` (clang-cl 22.1.6, MSVC toolset pinned to 14.38, ASan+UBSan) and ran `Smatchet.exe cmd ui_test.run --name=FreshState --spawn --yes --mcp-port=<p>` with `ASAN_OPTIONS=abort_on_error=1:halt_on_error=1`. Result: `{"passed":1,"failed":0,"tested":1}`, process exit code 0, **zero sanitizer reports**. The 600×{snippet+tool} worker loop ran concurrently with 600 frames of real `DrawLuaWindows` on the main `lua` state with no heap corruption — the fix holds. (Pre-fix, two threads through one `lua_State` would corrupt the Lua heap and ASan would abort.)
- **Default build unaffected**: `tests/ui/CMakeLists.txt` early-returns when `SMATCHET_BUILD_UI_TESTS=OFF`, so the `ninja-iter-msvc` default build is unaffected by the added test source (verified by build, exit 0).
- **Driver**: `scripts/dev/test-ui-mcp-lua-fresh-state-race.sh` (auto-enrolled by `scripts/dev/test-all.sh`; exits 2-skip when the ASan exe is absent, matching the other `test-ui-*.sh` bucket-E drivers).
- **Note on TSan**: ASan detects the *heap-corruption consequence* of the race, not the data race directly (TSan does, but TSan is unavailable on Windows MSVC/clang-cl per `cmake/Sanitizers.cmake`). This matches the plan's stated detection model ("realistically heap corruption / crash").

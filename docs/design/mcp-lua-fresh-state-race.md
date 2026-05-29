# Plan — MCP Lua execution: fresh per-call `sol::state` (cross-thread race fix)

> **Slug**: `mcp-lua-fresh-state-race`

## Context

Verified HIGH-severity cross-thread data race violating **UX Pillar 3 (never crash)**. The MCP `run_lua` snippet/script handlers and the registered-Lua-tool handler run inside httplib worker-thread lambdas (`Plugins/Mcp/src/McpPlugin.cpp` REST `/mcp/tools/call` :395 + JSON-RPC :658) and execute against the **shared main `sol::state lua`** member (`Source_Core/include/AppController.h:898`) with **no interpreter mutex and no `MainThreadDispatcher` hop**. The UI thread concurrently drives the same `lua` state (`DrawLuaWindows`, Lua cell-providers, `ExecuteLuaConsoleSnippet`). Two threads through one `lua_State` = undefined behavior (realistically heap corruption / crash). The only locks on these paths (`luaMcpToolsMutex_`, `luaActionsMutex_`) guard metadata vectors, not the interpreter. Reachable locally with no special config: MCP binds `127.0.0.1` by default; only gated by `allow_lua_execution`. `AppController.h:144-146` already documents that MCP/Lua workers must post to `mainThreadDispatcher` — these paths violate that.

Intended outcome: **after this lands, no MCP-worker Lua execution path ever touches the main `lua` state — each runs on a fresh, per-call `sol::state`, so the UI thread and MCP workers never share a `lua_State`.**

## Approach

Adopt the existing, proven isolation pattern from `AutomationWorkerLoop` (`AppController_LuaBindings.cpp:1155`), which already runs each background job on a per-job `sol::state bgState` precisely to avoid sharing. Apply it to all three MCP entry points in `AppController_LuaBindings_Draw.cpp`:

1. **`ExecuteLuaSnippetForMcp` / `ExecuteLuaScriptForMcp`** (`run_lua`): construct a fresh `sol::state`, `InitLuaCore` it, build a sandbox on it, then load + run the code there. No setup-script replay — today these run a sandbox over main `lua` whose `_G` contains *only* `InitLuaCore` builtins (`RunLuaSetupScript` runs setup under a throwaway sandbox, so user helpers never reach main `_G`), so a bare `InitLuaCore` state reproduces today's capability set exactly.

2. **`ExecuteLuaMcpTool`** (registered tool): the tool's `callback` is a `sol::protected_function` **bound to main `lua`**, so it cannot be moved. Instead **rebuild it on a fresh state**: `InitLuaCore` a fresh state, install a **call-local `mcp.register_tool` override** that collects definitions into a stack-local vector (leaving the shared `luaMcpTools_` untouched), replay `activeSetupScripts_` on that state so the tool re-registers as a fresh-state-bound closure, then find + invoke it there. Limitation (accepted): a tool registered ad-hoc by a prior `run_lua` snippet — not by a setup script — won't exist in the rebuilt state and returns "tool not found"; this is a degenerate, already-racy usage.

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

- **Behavior change — `ui.*` from MCP `run_lua` snippets becomes no-op** (fresh `InitLuaCore` state installs no-op `ui`/`register_*`, matching `AutomationWorkerLoop`). Accepted + correct: worker-thread UI mutation was already an unsafe Pillar-2/3 violation; the no-op is safer. Documented for users.
- **Behavior change — ad-hoc-registered MCP tools** (registered by a prior `run_lua` snippet, not a setup script) return "tool not found" on the rebuilt state. Accepted: degenerate, already-racy path.
- **Per-call cost** — `InitLuaCore` (+ setup replay for tools) per MCP call. Accepted: out-of-band, off UI thread, equals existing worker cost.
- **Non-goal**: host-binding (`create_issue`, `SubmitFieldEdit`) worker-thread safety — unchanged by this fix and already exercised worker-thread by `AutomationWorkerLoop`; out of scope.
- **Non-goal**: changing the MCP wire protocol, auth, or `allow_lua_execution` gating.

## Verification

- **Full integration test (Bucket A under sanitizer, `tests/Source_Core/McpLuaFreshStateRace.test.cpp`)**: construct a real `AppController` (temp SQLite DB, mock backend factory via `SetBackendFactory`), `Initialize` it; spawn a worker thread that fires `ExecuteLuaSnippetForMcp` in a tight loop while the test "UI thread" concurrently drives a main-`lua` operation representative of `DrawLuaWindows`/cell-provider evaluation; assert clean exit under ASan (and pre-fix, the same test reproduces the race). Two-thread overlap with a barrier to maximize interleaving.
- **Build gate (dual-target)**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12`.
- **Sanitizer gate**: configure+build the new ASan-with-tests preset, run the new test under `ninja-clang-asan` (ASan+UBSan) and/or `ninja-msvc-asan`; expect zero sanitizer reports.
- **Existing suites**: `ninja-test-msvc` (`SmatchetTests` + `SmatchetLuaTests`) green; `scripts/dev/test-all.sh` pre-push gate.
- **Manual residue**: if the real-`DrawLuaWindows`-via-ImGui path proves infeasible inside the ctest rig (ImGui context lifetime), the test substitutes a faithful main-`lua` eval proxy on the UI-thread side + a `docs/backlog/agent-self-improvement/test.md` entry for a bucket-E follow-up. No silent residue.

## Out of scope (flagged, not designed)

- A general lua-interpreter mutex for the remaining UI-thread `lua` touches — unnecessary once MCP paths are isolated; no other cross-thread `lua` writer exists.
- Caching/pooling fresh `sol::state`s across MCP calls — premature; revisit only if profiling shows per-call init cost matters.

## Implementation log
*(populated post-ship)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*
